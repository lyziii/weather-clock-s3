#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
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
#include "touch_sound.h"

#define SOUND_RATE 16000
#define SOUND_FRAMES 480
#define CLICK_FRAMES 192

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

static void audio_task(void *argument)
{
    (void)argument;
    /* 首次通知仅释放启动门控，不能被解释为一次触摸。 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
        if (!atomic_load(&sound_enabled)) {
            xSemaphoreGive(audio_mutex);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        bool requested = ulTaskNotifyTake(pdFALSE, 0) != 0;
        int result = ESP_CODEC_DEV_OK;
        if (requested && atomic_exchange(&test_requested, false)) {
            /* 显式诊断命令才播放长音，正常开机与触摸仍保持短促提示。 */
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
        } else {
            result = esp_codec_dev_write(codec, requested ? click_pcm : silence_pcm, sizeof(click_pcm));
        }
        if (result != ESP_CODEC_DEV_OK) {
            atomic_store(&sound_enabled, false);
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
    if (xTaskCreate(audio_task, "touch_sound", 4096, NULL, 2, &sound_task) != pdPASS) {
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
    gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    if (codec) { esp_codec_dev_set_out_mute(codec, true); esp_codec_dev_close(codec); }
    if (audio_mutex) { xSemaphoreGive(audio_mutex); }
    /* 三击唤醒前两次不启动音频；RTC 保持功放关闭，避免深睡期间引脚浮空。 */
    rtc_gpio_init(HW_AUDIO_ENABLE_GPIO);
    rtc_gpio_set_direction(HW_AUDIO_ENABLE_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    rtc_gpio_hold_en(HW_AUDIO_ENABLE_GPIO);
}