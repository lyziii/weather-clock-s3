#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "tf_card.h"
#include "touch_sound.h"

#define SOUND_RATE 16000
#define SOUND_FRAMES 480
#define CLICK_FRAMES 192
#define MUSIC_BLOCK_FRAMES 480
/* 常见 WAV 采样率最高按 48 kHz 处理，4x 源帧缓冲足够线性重采样。 */
#define WAV_SOURCE_MAX (MUSIC_BLOCK_FRAMES * 4 + 2)
#define MUSIC_TITLE_SIZE 64

static TaskHandle_t sound_task;
static SemaphoreHandle_t audio_mutex;
static atomic_bool sound_enabled;
static atomic_bool test_requested;
static i2s_chan_handle_t tx_channel;
static esp_codec_dev_handle_t codec;
static const audio_codec_ctrl_if_t *control_if;
static const audio_codec_data_if_t *data_if;
static const audio_codec_if_t *codec_if;
static int16_t click_pcm[SOUND_FRAMES * 2];
static int16_t silence_pcm[SOUND_FRAMES * 2];
static int16_t diagnostic_pcm[320 * 2];
typedef enum {
    MUSIC_SOURCE_NONE,
    MUSIC_SOURCE_DEMO,
    MUSIC_SOURCE_WAV,
} music_source_t;

typedef enum {
    MUSIC_STATE_STOPPED,
    MUSIC_STATE_LOADING,
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_FAILED,
} music_state_t;

typedef struct {
    uint16_t frequency;
    uint16_t duration_ms;
} demo_note_t;

/* 内置旋律使用《欢乐颂》前两句，无需文件即可验证喇叭和编解码器。 */
static const demo_note_t demo_melody[] = {
    {659, 220}, {659, 220}, {698, 220}, {784, 220}, {784, 220}, {698, 220}, {659, 220}, {587, 220},
    {523, 220}, {523, 220}, {587, 220}, {659, 220}, {659, 330}, {587, 110}, {587, 440},
    {659, 220}, {659, 220}, {698, 220}, {784, 220}, {784, 220}, {698, 220}, {659, 220}, {587, 220},
    {523, 220}, {523, 220}, {587, 220}, {659, 220}, {587, 330}, {523, 110}, {523, 440},
};

static atomic_bool music_start_requested;
static atomic_bool music_stop_requested;
static atomic_int music_state = MUSIC_STATE_STOPPED;
static atomic_int requested_volume = HW_AUDIO_CLICK_VOLUME;
static music_source_t music_source;
static FILE *music_file;
static uint32_t wav_rate;
static uint32_t wav_channels;
static uint32_t wav_block_align;
static uint32_t wav_data_offset;
static uint32_t wav_total_frames;
static double wav_cursor;
static int16_t wav_source[WAV_SOURCE_MAX * 2];
static unsigned demo_index;
static uint32_t demo_remaining;
static double demo_phase;
static char music_title[MUSIC_TITLE_SIZE] = "Music stopped";
static char requested_music_path[TF_PATH_SIZE];
static bool requested_builtin;
static portMUX_TYPE music_title_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_music_title(const char *text)
{
    taskENTER_CRITICAL(&music_title_lock);
    size_t length = strlen(text);
    if (length >= MUSIC_TITLE_SIZE) { length = MUSIC_TITLE_SIZE - 1; }
    memcpy(music_title, text, length);
    music_title[length] = '\0';
    taskEXIT_CRITICAL(&music_title_lock);
}



void touch_sound_play(void)
{
    if (atomic_load(&sound_enabled)) { xTaskNotifyGive(sound_task); }
}

bool touch_sound_test(void)
{
    if (!atomic_load(&sound_enabled)) { return false; }
    atomic_store(&test_requested, true);
    touch_sound_play();
    return true;
}
bool touch_sound_music_start(void)
{
    if (!atomic_load(&sound_enabled) || !sound_task) { return false; }
    taskENTER_CRITICAL(&music_title_lock);
    requested_music_path[0] = 0;
    requested_builtin = false;
    taskEXIT_CRITICAL(&music_title_lock);
    atomic_store(&music_stop_requested, false);
    atomic_store(&music_start_requested, true);
    return true;
}

bool touch_sound_music_start_path(const char *path)
{
    if (!path || strncmp(path, "/tf/", 4) != 0 || strlen(path) >= sizeof(requested_music_path) ||
        !atomic_load(&sound_enabled) || !sound_task) { return false; }
    taskENTER_CRITICAL(&music_title_lock);
    memcpy(requested_music_path, path, strlen(path) + 1);
    requested_builtin = false;
    taskEXIT_CRITICAL(&music_title_lock);
    atomic_store(&music_stop_requested, false);
    atomic_store(&music_start_requested, true);
    return true;
}

bool touch_sound_alarm_start(const char *path)
{
    if (path && path[0]) { return touch_sound_music_start_path(path); }
    if (!atomic_load(&sound_enabled) || !sound_task) { return false; }
    taskENTER_CRITICAL(&music_title_lock);
    requested_builtin = true;
    requested_music_path[0] = 0;
    taskEXIT_CRITICAL(&music_title_lock);
    atomic_store(&music_stop_requested, false);
    atomic_store(&music_start_requested, true);
    return true;
}

void touch_sound_music_stop(void)
{
    if (!sound_task) { return; }
    atomic_store(&music_stop_requested, true);
}

void touch_sound_alarm_volume(int percent)
{
    if (percent < 0) { percent = HW_AUDIO_CLICK_VOLUME; }
    if (percent > 100) { percent = 100; }
    atomic_store(&requested_volume, percent);
}

bool touch_sound_music_busy(void)
{
    int state = atomic_load(&music_state);
    return atomic_load(&music_start_requested) || state == MUSIC_STATE_LOADING || state == MUSIC_STATE_PLAYING;
}

bool touch_sound_music_status(char *out, size_t size)
{
    if (!out || size == 0) { return false; }
    taskENTER_CRITICAL(&music_title_lock);
    size_t length = strlen(music_title);
    if (length >= size) { length = size - 1; }
    memcpy(out, music_title, length);
    out[length] = '\0';
    taskEXIT_CRITICAL(&music_title_lock);
    return atomic_load(&sound_enabled);
}



static bool has_wav_suffix(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && name[length - 4] == '.' &&
           tolower((unsigned char)name[length - 3]) == 'w' &&
           tolower((unsigned char)name[length - 2]) == 'a' &&
           tolower((unsigned char)name[length - 1]) == 'v';
}

static bool read_exact(FILE *file, void *buffer, size_t size)
{
    return fread(buffer, 1, size, file) == size;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* 解析 RIFF/WAVE，只接受 16 位 PCM、单/双声道、8~48 kHz。函数要求
 * 调用方持有 tf_card_lock。 */
static bool wav_parse_locked(FILE *file)
{
    uint8_t riff[12];
    if (!read_exact(file, riff, sizeof(riff)) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool format_ready = false;
    for (;;) {
        uint8_t chunk[8];
        if (!read_exact(file, chunk, sizeof(chunk))) { return false; }
        uint32_t size = read_le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || !read_exact(file, fmt, sizeof(fmt))) { return false; }
            uint16_t format = read_le16(fmt);
            uint16_t channels = read_le16(fmt + 2);
            uint32_t rate = read_le32(fmt + 4);
            uint16_t block_align = read_le16(fmt + 12);
            uint16_t bits = read_le16(fmt + 14);
            if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
                rate < 8000 || rate > 48000 || block_align != channels * 2) {
                return false;
            }
            wav_rate = rate;
            wav_channels = channels;
            wav_block_align = block_align;
            format_ready = true;
            uint32_t remaining = size - sizeof(fmt);
            if (remaining && fseek(file, (long)(remaining + (size & 1)), SEEK_CUR) != 0) {
                return false;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!format_ready) { return false; }
            long position = ftell(file);
            if (position < 0 || size < wav_block_align) { return false; }
            wav_data_offset = (uint32_t)position;
            wav_total_frames = size / wav_block_align;
            return true;
        } else if (fseek(file, (long)(size + (size & 1)), SEEK_CUR) != 0) {
            return false;
        }
    }
}

static bool wav_open_locked(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { return false; }
    if (!wav_parse_locked(file)) {
        fclose(file);
        return false;
    }
    music_file = file;
    wav_cursor = 0;
    /* 文件在播放期间保持打开；计数防止后台任务卸载 /tf。 */
    tf_card_reader_begin();
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char title[MUSIC_TITLE_SIZE];
    snprintf(title, sizeof(title), "WAV: %.52s", name);
    set_music_title(title);
    return true;
}

static bool try_wav_dir_locked(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir) { return false; }
    bool opened = false;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!has_wav_suffix(entry->d_name)) { continue; }
        char path[TF_PATH_SIZE];
        int length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (length > 0 && length < (int)sizeof(path) && wav_open_locked(path)) {
            opened = true;
            break;
        }
    }
    closedir(dir);
    return opened;
}

static bool music_open_first_locked(void)
{
    static const char *const directories[] = {"/tf/music", "/tf"};
    for (size_t index = 0; index < sizeof(directories) / sizeof(directories[0]); ++index) {
        if (try_wav_dir_locked(directories[index])) { return true; }
    }
    return false;
}


static void stop_music_locked(void)
{
    if (music_file) {
        tf_card_lock();
        fclose(music_file);
        tf_card_unlock();
        music_file = NULL;
        tf_card_reader_end();
    }
    music_source = MUSIC_SOURCE_NONE;
    atomic_store(&music_state, MUSIC_STATE_STOPPED);
    set_music_title("Music stopped");
}

static void demo_start_locked(void)
{
    music_source = MUSIC_SOURCE_DEMO;
    demo_index = 0;
    demo_remaining = 0;
    demo_phase = 0;
    atomic_store(&music_state, MUSIC_STATE_PLAYING);
    set_music_title("Demo melody");
}

static void start_music_locked(void)
{
    stop_music_locked();
    atomic_store(&music_state, MUSIC_STATE_LOADING);
    set_music_title("Searching TF card...");
    bool opened = false;
    char path[TF_PATH_SIZE];
    taskENTER_CRITICAL(&music_title_lock);
    memcpy(path, requested_music_path, sizeof(path));
    bool builtin = requested_builtin;
    taskEXIT_CRITICAL(&music_title_lock);
    if (builtin) { demo_start_locked(); return; }
    tf_card_lock();
    opened = path[0] ? wav_open_locked(path) : music_open_first_locked();
    tf_card_unlock();
    if (opened) {
        music_source = MUSIC_SOURCE_WAV;
        atomic_store(&music_state, MUSIC_STATE_PLAYING);
        ESP_LOGI("touch_sound", "%s", music_title);
        return;
    }
    /* 没有可播放的 WAV 时仍用内置旋律验证喇叭，不显示为失败。 */
    demo_start_locked();
    ESP_LOGI("touch_sound", "No WAV on TF card; playing built-in melody");
}

static void demo_generate_locked(int16_t *output)
{
    for (unsigned frame = 0; frame < MUSIC_BLOCK_FRAMES; ++frame) {
        if (demo_remaining == 0) {
            if (demo_index >= sizeof(demo_melody) / sizeof(demo_melody[0])) {
                output[frame * 2] = 0;
                output[frame * 2 + 1] = 0;
                music_source = MUSIC_SOURCE_NONE;
                atomic_store(&music_state, MUSIC_STATE_STOPPED);
                set_music_title("Music finished");
                for (unsigned rest = frame + 1; rest < MUSIC_BLOCK_FRAMES; ++rest) {
                    output[rest * 2] = 0;
                    output[rest * 2 + 1] = 0;
                }
                return;
            }
            const demo_note_t *note = &demo_melody[demo_index];
            demo_remaining = (uint32_t)note->duration_ms * SOUND_RATE / 1000;
            if (!demo_remaining) { demo_remaining = 1; }
            demo_phase = 0;
        }
        const demo_note_t *note = &demo_melody[demo_index];
        uint32_t total = (uint32_t)note->duration_ms * SOUND_RATE / 1000;
        uint32_t position = total > demo_remaining ? total - demo_remaining : 0;
        double envelope = 1.0;
        if (position < 80) { envelope = position / 80.0; }
        if (demo_remaining < 80 && demo_remaining / 80.0 < envelope) {
            envelope = demo_remaining / 80.0;
        }
        int16_t value = (int16_t)(7000.0 * envelope * sin(demo_phase));
        output[frame * 2] = value;
        output[frame * 2 + 1] = value;
        demo_phase += 6.283185307179586 * note->frequency / SOUND_RATE;
        if (demo_phase > 6.283185307179586) { demo_phase -= 6.283185307179586; }
        if (--demo_remaining == 0) { ++demo_index; }
    }
}


static void finish_music_locked(void)
{
    if (music_file) {
        tf_card_lock();
        fclose(music_file);
        tf_card_unlock();
        music_file = NULL;
        tf_card_reader_end();
    }
    music_source = MUSIC_SOURCE_NONE;
    atomic_store(&music_state, MUSIC_STATE_STOPPED);
    set_music_title("Music finished");
}

/* 从当前光标读取一小段 WAV 并线性重采样到 16 kHz 立体声。
 * 返回 true 表示文件还有后续数据，false 表示本块已到结尾。 */
static bool wav_generate_locked(int16_t *output)
{
    const double ratio = (double)wav_rate / SOUND_RATE;
    uint32_t start = (uint32_t)wav_cursor;
    double last_position = wav_cursor + (MUSIC_BLOCK_FRAMES - 1) * ratio;
    uint32_t needed = (uint32_t)(last_position - start) + 2;
    if (needed > WAV_SOURCE_MAX) { needed = WAV_SOURCE_MAX; }
    uint32_t available = start < wav_total_frames ? wav_total_frames - start : 0;
    uint32_t wanted = available < needed ? available : needed;
    size_t frames_read = 0;
    if (wanted) {
        tf_card_lock();
        if (fseek(music_file, (long)(wav_data_offset + start * wav_block_align), SEEK_SET) == 0) {
            frames_read = fread(wav_source, wav_block_align, wanted, music_file);
        }
        tf_card_unlock();
        if (!frames_read) { return false; }
    }

    for (unsigned frame = 0; frame < MUSIC_BLOCK_FRAMES; ++frame) {
        double position = wav_cursor + frame * ratio - start;
        if (position < 0) { position = 0; }
        uint32_t index = (uint32_t)position;
        if (index >= frames_read) {
            output[frame * 2] = 0;
            output[frame * 2 + 1] = 0;
            continue;
        }
        const int16_t *sample = &wav_source[index * wav_channels];
        int16_t left = sample[0];
        int16_t right = wav_channels == 2 ? sample[1] : sample[0];
        if (index + 1 < frames_read) {
            const int16_t *next = &wav_source[(index + 1) * wav_channels];
            double fraction = position - index;
            left = (int16_t)(left + (next[0] - left) * fraction);
            right = (int16_t)(right + ((wav_channels == 2 ? next[1] : next[0]) - right) * fraction);
        }
        output[frame * 2] = left;
        output[frame * 2 + 1] = right;
    }
    wav_cursor += MUSIC_BLOCK_FRAMES * ratio;
    return wav_cursor < wav_total_frames;
}

/* 生成一块音乐 PCM。返回 -1 表示编码器写入失败，0 表示正常。 */
static int music_write_block_locked(void)
{
    int16_t block[MUSIC_BLOCK_FRAMES * 2];
    bool playing = false;
    if (music_source == MUSIC_SOURCE_WAV) {
        playing = wav_generate_locked(block);
    } else if (music_source == MUSIC_SOURCE_DEMO) {
        demo_generate_locked(block);
        playing = music_source == MUSIC_SOURCE_DEMO;
    } else {
        return 0;
    }
    int result = esp_codec_dev_write(codec, block, sizeof(block));
    if (result != ESP_CODEC_DEV_OK) { return -1; }
    if (!playing) { finish_music_locked(); }
    return 0;
}


static void audio_task(void *argument)
{
    (void)argument;
    int applied_volume = HW_AUDIO_CLICK_VOLUME;
    /* 首次通知仅释放启动门控，不能被解释为一次触摸。 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
        if (!atomic_load(&sound_enabled)) {
            stop_music_locked();
            xSemaphoreGive(audio_mutex);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        int volume = atomic_load(&requested_volume);
        if (volume != applied_volume && esp_codec_dev_set_out_vol(codec, volume) == ESP_CODEC_DEV_OK) {
            applied_volume = volume;
        }
        /* 音乐请求只改标志，不额外发通知，避免被误当成一次触摸音。 */
        if (atomic_exchange(&music_stop_requested, false)) { stop_music_locked(); }
        if (atomic_exchange(&music_start_requested, false)) { start_music_locked(); }

        bool requested = ulTaskNotifyTake(pdFALSE, 0) != 0;
        int result = ESP_CODEC_DEV_OK;
        if (requested && atomic_exchange(&test_requested, false)) {
            /* 显式诊断命令才播放长音，正常开机与触摸仍保持短促提示。 */
            stop_music_locked();
            for (unsigned block = 0; block < 20 && result == ESP_CODEC_DEV_OK; ++block) {
                result = esp_codec_dev_write(codec, diagnostic_pcm, sizeof(diagnostic_pcm));
            }
            int volume_register = 0;
            int mute_register = 0;
            esp_err_t volume_read = esp_codec_dev_read_reg(codec, 0x32, &volume_register);
            esp_err_t mute_read = esp_codec_dev_read_reg(codec, 0x31, &mute_register);
            ESP_LOGI("touch_sound", "TEST result=%s DAC volume=0x%02X (%s), mute=0x%02X (%s), PA=%d",
                     esp_err_to_name(result), volume_register, esp_err_to_name(volume_read),
                     mute_register, esp_err_to_name(mute_read), gpio_get_level(HW_AUDIO_ENABLE_GPIO));
        } else if (requested) {
            result = esp_codec_dev_write(codec, click_pcm, sizeof(click_pcm));
        } else if (music_source != MUSIC_SOURCE_NONE) {
            result = music_write_block_locked();
        } else {
            result = esp_codec_dev_write(codec, silence_pcm, sizeof(silence_pcm));
        }
        if (result != ESP_CODEC_DEV_OK) {
            atomic_store(&sound_enabled, false);
            stop_music_locked();
            gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
            ESP_LOGW("touch_sound", "Audio output disabled: %s", esp_err_to_name(result));
        }
        xSemaphoreGive(audio_mutex);
        if (result != ESP_CODEC_DEV_OK) { vTaskDelay(pdMS_TO_TICKS(20)); }
    }
}

static void release_audio(void)
{
    if (codec) { esp_codec_dev_close(codec); esp_codec_dev_delete(codec); codec = NULL; }
    if (codec_if) { audio_codec_delete_codec_if(codec_if); codec_if = NULL; }
    if (control_if) { audio_codec_delete_ctrl_if(control_if); control_if = NULL; }
    if (data_if) { audio_codec_delete_data_if(data_if); data_if = NULL; }
    if (tx_channel) {
        i2s_channel_disable(tx_channel);
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
    }
    if (audio_mutex) { vSemaphoreDelete(audio_mutex); audio_mutex = NULL; }
    gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
}

esp_err_t touch_sound_init(void)
{
    rtc_gpio_hold_dis(HW_AUDIO_ENABLE_GPIO);
    rtc_gpio_deinit(HW_AUDIO_ENABLE_GPIO);
    gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    esp_err_t result = gpio_set_direction(HW_AUDIO_ENABLE_GPIO, GPIO_MODE_OUTPUT);
    if (result != ESP_OK) { return result; }
    i2c_master_bus_handle_t bus = board_hardware_i2c_bus();
    if (!bus) { return ESP_ERR_INVALID_STATE; }
    result = i2c_master_probe(bus, HW_AUDIO_ADDRESS, 100);
    if (result != ESP_OK) { return result; }
    audio_mutex = xSemaphoreCreateMutex();
    if (!audio_mutex) { return ESP_ERR_NO_MEM; }
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = 80;
    channel_config.auto_clear_after_cb = false;
    result = i2s_new_channel(&channel_config, &tx_channel, NULL);
    if (result != ESP_OK) { goto failure; }
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOUND_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = HW_AUDIO_MCLK,
            .bclk = HW_AUDIO_BCLK,
            .ws = HW_AUDIO_WS,
            .dout = HW_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    result = i2s_channel_init_std_mode(tx_channel, &standard);
    if (result != ESP_OK) { goto failure; }
    result = i2s_channel_enable(tx_channel);
    if (result != ESP_OK) { goto failure; }
    /* codec_dev 的地址约定为含 R/W 位的地址，底层适配器会右移还原 0x18。 */
    audio_codec_i2c_cfg_t control_config = {
        .addr = HW_AUDIO_ADDRESS << 1,
        .bus_handle = bus,
        .clock_speed_hz = HW_I2C_HZ,
    };
    control_if = audio_codec_new_i2c_ctrl(&control_config);
    audio_codec_i2s_cfg_t data_config = {.port = I2S_NUM_0, .tx_handle = tx_channel};
    data_if = audio_codec_new_i2s_data(&data_config);
    if (!control_if || !data_if) { result = ESP_ERR_NO_MEM; goto failure; }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .mclk_div = 256,
    };
    codec_if = es8311_codec_new(&codec_config);
    if (!codec_if) { result = ESP_FAIL; goto failure; }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    codec = esp_codec_dev_new(&device_config);
    if (!codec) { result = ESP_ERR_NO_MEM; goto failure; }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = SOUND_RATE, .bits_per_sample = 16, .channel = 2,
    };
    result = esp_codec_dev_open(codec, &sample);
    if (result != ESP_CODEC_DEV_OK) { goto failure; }
    result = esp_codec_dev_set_out_vol(codec, HW_AUDIO_CLICK_VOLUME);
    if (result != ESP_CODEC_DEV_OK) { goto failure; }
    /* 12 ms 衰减短脉冲，前沿平滑；双声道相同避免 ES8311 槽位选择影响发声。 */
    for (unsigned frame = 0; frame < CLICK_FRAMES; ++frame) {
        float attack = frame < 8 ? frame / 8.0f : 1.0f;
        float tail = (CLICK_FRAMES - frame) / (float)CLICK_FRAMES;
        int16_t value = (int16_t)(24000.0f * attack * tail * expf(-(float)frame / 36.0f) *
                                  sinf(6.2831853f * 1700.0f * frame / SOUND_RATE));
        click_pcm[frame * 2] = value;
        click_pcm[frame * 2 + 1] = value;
    }
    result = esp_codec_dev_write(codec, silence_pcm, sizeof(silence_pcm));
    if (result != ESP_CODEC_DEV_OK) { goto failure; }
    /* 先发送静音并稳定 DAC，再开功放，防止把初始化瞬态当作提示音。 */
    vTaskDelay(pdMS_TO_TICKS(30));
    if (xTaskCreate(audio_task, "touch_sound", 6144, NULL, 2, &sound_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto failure;
    }
    gpio_set_level(HW_AUDIO_ENABLE_GPIO, HW_AUDIO_ENABLE_LEVEL);
    /* 诊断波形由显式串口命令触发，不在开机时自动播放。 */
    for (unsigned frame = 0; frame < 320; ++frame) {
        int16_t value = (int16_t)(16000.0f * sinf(6.2831853f * 1000.0f * frame / SOUND_RATE));
        diagnostic_pcm[frame * 2] = value;
        diagnostic_pcm[frame * 2 + 1] = value;
    }
    atomic_store(&sound_enabled, true);
    /* 任务可能在启用前已等待通知；只唤醒它，不播放开机提示音。 */
    xTaskNotifyGive(sound_task);
    ESP_LOGI("touch_sound", "ES8311 ready: 16 kHz, volume %d, touch click enabled", HW_AUDIO_CLICK_VOLUME);
    return ESP_OK;

failure:
    release_audio();
    return result;
}

void touch_sound_prepare_sleep(void)
{
    atomic_store(&sound_enabled, false);
    if (audio_mutex) { xSemaphoreTake(audio_mutex, portMAX_DELAY); }
    stop_music_locked();
    gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    if (codec) { esp_codec_dev_set_out_mute(codec, true); esp_codec_dev_close(codec); }
    if (audio_mutex) { xSemaphoreGive(audio_mutex); }
    /* 三击唤醒前两次不启动音频；RTC 保持功放关闭，避免深睡期间引脚浮空。 */
    rtc_gpio_init(HW_AUDIO_ENABLE_GPIO);
    rtc_gpio_set_direction(HW_AUDIO_ENABLE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    rtc_gpio_hold_en(HW_AUDIO_ENABLE_GPIO);
}