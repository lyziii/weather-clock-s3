#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "zh_font.h"

/* ch16.bin 的 glyf 表约 2.5 MB，20 位的 bitmap_index 会在 1 MB 处回绕。
 * 使用 32 位字形描述符（sdkconfig.defaults 中的 LV_FONT_FMT_TXT_LARGE）。 */
#if !LV_FONT_FMT_TXT_LARGE
#error "ch16.bin needs CONFIG_LV_FONT_FMT_TXT_LARGE=y (glyph bitmap exceeds 1 MB)"
#endif

/* 字库在 Flash 里按地址解析：lv_binfont_create_ex() 用 MEMFS 把一段内存当作
 * 只读文件，因此不需要文件系统，也不需要把 2.6 MB 的文件复制到 PSRAM。 */
#if !LV_USE_FS_MEMFS
#error "flash-resident font needs CONFIG_LV_USE_FS_MEMFS=y"
#endif

/* partitions.csv 中的 font 数据分区，内容就是 main/fonts/ch16.bin 的原始字节。 */
#define ZH_FONT_PARTITION_LABEL "font"
#define ZH_RETRY_US (3 * 1000000)
#define ZH_FONT_TASK_STACK 6144
/* 解析期间的看门狗超时：比实测的十来秒留足余量。 */
#define ZH_FONT_WDT_TIMEOUT_MS 60000

static const char *TAG = "zh_font";

/* 两万多个字形要顺序解析一遍，放在 LVGL 任务里会让开机动画和触摸停顿，
 * 所以由独立低优先级任务完成；加载后 LVGL 任务只读取字体指针。 */
static _Atomic(lv_font_t *) font;
static atomic_bool font_loading;
/* Flash 里没有可用字库（分区缺失、为空或不是 LVGL 二进制格式）时重启前不再
 * 重试：分区内容在运行期不会自己变好，反复解析只会刷日志。 */
static atomic_bool font_unavailable;
static TaskHandle_t font_task_handle;
static int64_t last_attempt_us;

/* 映射必须一直有效：字形位图按地址直接读 Flash，munmap 之后就是野指针。 */
static const uint8_t *font_data;
static size_t font_data_size;

static const uint8_t *zh_font_map_partition(size_t *size)
{
    if (font_data) {
        *size = font_data_size;
        return font_data;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ZH_FONT_PARTITION_LABEL);
    if (!partition) {
        ESP_LOGE(TAG, "no \"%s\" partition in the partition table", ZH_FONT_PARTITION_LABEL);
        return NULL;
    }

    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t result = esp_partition_mmap(partition, 0, partition->size,
                                          ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "cannot map \"%s\" partition at 0x%08x (%s)", ZH_FONT_PARTITION_LABEL,
                 (unsigned)partition->address, esp_err_to_name(result));
        return NULL;
    }
    /* 句柄只用于 munmap；字库一直用到重启，所以不再保存。 */
    font_data = (const uint8_t *)mapped;
    font_data_size = partition->size;
    *size = font_data_size;
    ESP_LOGI(TAG, "mapped \"%s\" partition at 0x%08x, %u KiB",
             ZH_FONT_PARTITION_LABEL, (unsigned)partition->address,
             (unsigned)(font_data_size / 1024));
    return font_data;
}

/* LVGL 逐位解包两万多个字形是一段十来秒的纯 CPU 循环（26 位字形头，位图不
 * 按字节对齐），期间同核的空闲任务拿不到时间片，看门狗会在 5 秒后打印
 * backtrace。解析是一次性的有界操作：只在解析期间把超时放宽，结束后恢复
 * sdkconfig 配置的 5 秒；空闲任务的订阅关系保持不变。 */
static void zh_font_wdt_set_timeout(uint32_t timeout_ms)
{
    uint32_t idle_mask = 0;
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    idle_mask |= 1u << 0;
#endif
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    idle_mask |= 1u << 1;
#endif
    const esp_task_wdt_config_t config = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = idle_mask,
#ifdef CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    esp_err_t result = esp_task_wdt_reconfigure(&config);
    if (result != ESP_OK) {
        /* 看门狗未启用（CONFIG_ESP_TASK_WDT_INIT=n）时属正常情况，只记调试日志，
         * 避免解析失败重试时刷屏。 */
        ESP_LOGD(TAG, "cannot set task watchdog timeout to %u ms: %s",
                 (unsigned)timeout_ms, esp_err_to_name(result));
    }
}

static void zh_font_load_once(void)
{
    size_t size = 0;
    const uint8_t *data = zh_font_map_partition(&size);
    if (!data) {
        atomic_store(&font_unavailable, true);
        return;
    }

    /* 分区为空（0xFF）或写入的不是 LVGL 二进制字库时给出可操作的提示，
     * 不要把解析器的失败信息留给用户猜。 */
    if (size < 8 || memcmp(data + 4, "head", 4) != 0) {
        ESP_LOGE(TAG, "\"%s\" partition has no LVGL binary font: run idf.py flash to write main/fonts/ch16.bin",
                 ZH_FONT_PARTITION_LABEL);
        atomic_store(&font_unavailable, true);
        return;
    }

    int64_t started_us = esp_timer_get_time();
    /* path = NULL 时 LVGL 走 MEMFS：只读映射区，不回写任何字节。字形位图与
     * 描述符仍会解析到 PSRAM（约 2.9 MB），但少了整份文件的 PSRAM 副本。 */
    const lv_binfont_dsc_t descriptor = {
        .path = NULL,
        .buffer = data,
        .buffer_size = (uint32_t)size,
        .dynamic_glyph_load = false,
    };
    zh_font_wdt_set_timeout(ZH_FONT_WDT_TIMEOUT_MS);
    lv_font_t *loaded = lv_binfont_create_ex(&descriptor);
    zh_font_wdt_set_timeout((uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000);
    if (!loaded) {
        /* 只在第一次失败时打印，之后静默重试（可能是瞬时的内存不足）。 */
        static bool reported;
        if (!reported) {
            reported = true;
            ESP_LOGW(TAG, "cannot parse the font in flash (bad format, truncated image, or PSRAM exhausted)");
        }
        return;
    }
    loaded->fallback = &lv_font_montserrat_14;
    atomic_store(&font, loaded);
    ESP_LOGI(TAG, "Chinese font loaded from flash in %lld ms (%u KiB PSRAM free)",
             (long long)((esp_timer_get_time() - started_us) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void zh_font_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        zh_font_load_once();
        /* 失败也释放门闩，由 zh_font_tick() 在下一个重试周期再次投递。 */
        atomic_store(&font_loading, false);
    }
}

void zh_font_init(void)
{
    if (font_task_handle) { return; }
    if (xTaskCreate(zh_font_task, "zh_font", ZH_FONT_TASK_STACK, NULL, 1,
                    &font_task_handle) != pdPASS) {
        font_task_handle = NULL;
        ESP_LOGE(TAG, "cannot create font loader task");
        return;
    }
    /* Flash 分区开局即可读，不必像 TF 卡那样等挂载，直接投递第一次解析。 */
    last_attempt_us = esp_timer_get_time();
    atomic_store(&font_loading, true);
    xTaskNotifyGive(font_task_handle);
}

/* 只在未加载且没有解析任务运行时投递请求，函数本身立即返回。 */
void zh_font_tick(void)
{
    if (!font_task_handle || atomic_load(&font) || atomic_load(&font_loading) ||
        atomic_load(&font_unavailable)) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - last_attempt_us < ZH_RETRY_US) { return; }
    last_attempt_us = now;
    atomic_store(&font_loading, true);
    xTaskNotifyGive(font_task_handle);
}

const lv_font_t *zh_font(void)
{
    return atomic_load(&font);
}
