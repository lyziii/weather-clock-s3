#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "zh_font.h"

/* ch16.bin 的 glyf 表约 2.5 MB，20 位的 bitmap_index 会在 1 MB 处回绕。
 * 使用 32 位字形描述符（sdkconfig.defaults 中的 LV_FONT_FMT_TXT_LARGE）。 */
#if !LV_FONT_FMT_TXT_LARGE
#error "ch16.bin needs CONFIG_LV_FONT_FMT_TXT_LARGE=y (glyph bitmap exceeds 1 MB)"
#endif

#define ZH_RETRY_US (3 * 1000000)
#define ZH_FONT_TASK_STACK 8192

static const char *TAG = "zh_font";

/* 字库在独立低优先级任务里解析，LVGL 任务只读取指针。否则 2.9 MB 的
 * PSRAM 载入会阻塞开机动画和所有页面刷新数秒，看起来像启动卡死。 */
static _Atomic(lv_font_t *) font;
static atomic_bool font_loading;
static TaskHandle_t font_task_handle;
static int64_t last_attempt_us;
static void zh_font_load_once(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zh_font");
    if (!partition) {
        ESP_LOGW(TAG, "Chinese font partition missing; flash the updated partition table");
        return;
    }

    int64_t started_us = esp_timer_get_time();
    uint8_t header[8];
    esp_err_t result = esp_partition_read(partition, 0, header, sizeof(header));
    /* LVGL 二进制字体以长度字段和 head 标签开头，空白分区不能交给解析器。 */
    if (result != ESP_OK || memcmp(header + 4, "head", 4) != 0) {
        ESP_LOGW(TAG, "Chinese font partition is empty or invalid; flash fonts/ch16.bin");
        return;
    }

    const void *font_data = NULL;
    esp_partition_mmap_handle_t mapping;
    result = esp_partition_mmap(partition, 0, partition->size,
                               ESP_PARTITION_MMAP_DATA, &font_data, &mapping);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot map Chinese font: %s", esp_err_to_name(result));
        return;
    }
    /* 默认非动态加载会复制全部字形到 PSRAM；解析完成即可释放 Flash 映射。 */
    lv_font_t *loaded = lv_binfont_create_from_buffer((void *)font_data, partition->size);
    esp_partition_munmap(mapping);
    if (!loaded) {
        ESP_LOGW(TAG, "Cannot load Chinese font (bad format or PSRAM exhausted)");
        return;
    }
    loaded->fallback = &lv_font_montserrat_14;
    atomic_store(&font, loaded);
    ESP_LOGI(TAG, "Chinese font loaded from Flash partition %s into PSRAM in %lld ms (%u KiB PSRAM free)",
             partition->label, (long long)((esp_timer_get_time() - started_us) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void zh_font_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        zh_font_load_once();
        /* 失败后释放加载门控，下一次 tick 延迟重试。 */
        atomic_store(&font_loading, false);
    }
}


void zh_font_init(void)
{
    if (xTaskCreate(zh_font_task, "zh_font", ZH_FONT_TASK_STACK, NULL, 1,
                    &font_task_handle) != pdPASS) {
        font_task_handle = NULL;
        ESP_LOGE(TAG, "cannot create font loader task");
    }
}

/* 销毁字体前先把引用它的控件切回内置字体，否则这些控件会持有悬空指针。 */
static lv_obj_tree_walk_res_t retarget_font_cb(lv_obj_t *object, void *user_data)
{
    const lv_font_t *released = (const lv_font_t *)user_data;
    if (lv_obj_get_style_text_font(object, LV_PART_MAIN) == released) {
        lv_obj_set_style_text_font(object, &lv_font_montserrat_14, LV_PART_MAIN);
    }
    return LV_OBJ_TREE_WALK_NEXT;
}

void zh_font_release(void)
{
    lv_font_t *released = atomic_load(&font);
    if (!released) { return; }
    if (atomic_load(&font_loading)) {
        /* 当前没有正常业务会走到这里；保留保护，避免在解析中销毁半成品。 */
        ESP_LOGW(TAG, "release skipped: font loader still active");
        return;
    }
    atomic_store(&font, NULL);
    /* 字库常驻 PSRAM，Flash 映射已释放；此函数仅用于主动重新加载。 */
    lv_obj_tree_walk(NULL, retarget_font_cb, released);
    lv_binfont_destroy(released);
    /* 延后一个重试周期再加载，避免销毁后立即重新申请大块 PSRAM。 */
    last_attempt_us = esp_timer_get_time();
}

/* 只在未加载且没有加载任务运行时投递请求，函数本身立即返回。
 * LVGL 的开机动画和触摸因此不会被字库解析阻塞，也不需要等待 SD 卡。 */
void zh_font_tick(void)
{
    if (!font_task_handle || atomic_load(&font) || atomic_load(&font_loading)) { return; }
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
