#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "tf_card.h"
#include "zh_font.h"

/* ch16.bin 的 glyf 表约 2.5 MB，20 位的 bitmap_index 会在 1 MB 处回绕。
 * 使用 32 位字形描述符（sdkconfig.defaults 中的 LV_FONT_FMT_TXT_LARGE）。 */
#if !LV_FONT_FMT_TXT_LARGE
#error "ch16.bin needs CONFIG_LV_FONT_FMT_TXT_LARGE=y (glyph bitmap exceeds 1 MB)"
#endif

#define ZH_FONT_MAX_PATH 64
#define ZH_RETRY_US (3 * 1000000)
#define ZH_FONT_TASK_STACK 8192

/* 按顺序尝试的 VFS 路径，覆盖用户可能采用的几种拷贝位置。 */
static const char *const zh_font_candidates[] = {
    "/tf/fonts/ch16.bin",
    "/tf/font/ch16.bin",
    "/tf/ch16.bin",
};

static const char *TAG = "zh_font";

static lv_fs_drv_t fs_drv;   /* 必须静态存储：register 只保存指针 */
/* 字库在独立低优先级任务里解析，LVGL 任务只读取指针。否则 2.9 MB 的
 * PSRAM 载入会阻塞开机动画和所有页面刷新数秒，看起来像启动卡死。 */
static _Atomic(lv_font_t *) font;
static atomic_bool font_loading;
static TaskHandle_t font_task_handle;
static int64_t last_attempt_us;
static void zh_font_diagnose_once(void);

/* POSIX 直通：TF 卡已由 esp_vfs_fat 挂载为 VFS，LVGL 剥离 S: 后的剩余
 * 路径就是完整 VFS 路径（如 /tf/fonts/ch16.bin），直接用标准 C 文件函数。
 * 每次调用都持有 tf_card 的互斥锁，与后台目录浏览串行化。 */
static void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    const char *flags = (mode & LV_FS_MODE_WR) ? "rb+" : "rb";
    tf_card_lock();
    void *file = fopen(path, flags);
    tf_card_unlock();
    return file;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    tf_card_lock();
    int result = fclose((FILE *)file_p);
    tf_card_unlock();
    return result == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    tf_card_lock();
    size_t read = fread(buf, 1, btr, (FILE *)file_p);
    /* 字库解析要求每次读满；短读通常意味着卡被拔出或文件被截断，
     * 不能把残留缓冲区当成有效字形数据继续解析。 */
    bool failed = read != btr || ferror((FILE *)file_p);
    tf_card_unlock();
    if (br) { *br = (uint32_t)read; }
    return failed ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    int origin = (whence == LV_FS_SEEK_SET) ? SEEK_SET :
                 (whence == LV_FS_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    tf_card_lock();
    int result = fseek((FILE *)file_p, (long)pos, origin);
    tf_card_unlock();
    return result == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    tf_card_lock();
    long position = ftell((FILE *)file_p);
    tf_card_unlock();
    if (position < 0) { return LV_FS_RES_FS_ERR; }
    if (pos_p) { *pos_p = (uint32_t)position; }
    return LV_FS_RES_OK;
}

static bool zh_font_read_all(const char *path, uint8_t **buffer, size_t *size)
{
    *buffer = NULL;
    *size = 0;
    tf_card_reader_begin();
    tf_card_lock();
    FILE *file = fopen(path, "rb");
    if (!file) { tf_card_unlock(); tf_card_reader_end(); return false; }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long length = ok ? ftell(file) : -1;
    ok = ok && length > 0 && length <= 8 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0;
    uint8_t *data = ok ? heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        heap_caps_free(data);
        data = NULL;
    }
    if (data) {
        *buffer = data;
        *size = (size_t)length;
    }
    fclose(file);
    tf_card_unlock();
    tf_card_reader_end();
    return data != NULL;
}


static void zh_font_load_once(void)
{
    tf_snapshot_t snapshot;
    if (!tf_card_snapshot(&snapshot) || !snapshot.mounted) { return; }

    const char *vfs_path = NULL;
    tf_card_lock();
    for (size_t i = 0; i < sizeof(zh_font_candidates) / sizeof(zh_font_candidates[0]); ++i) {
        FILE *probe = fopen(zh_font_candidates[i], "rb");
        if (probe) {
            fclose(probe);
            vfs_path = zh_font_candidates[i];
            break;
        }
    }
    tf_card_unlock();
    if (!vfs_path) {
        ESP_LOGW(TAG, "Chinese font missing (tried /tf/fonts/ch16.bin, /tf/font/ch16.bin, /tf/ch16.bin)");
        zh_font_diagnose_once();
        return;
    }

    /* 先把整个文件一次性读入 PSRAM，再通过 MEMFS 解析。LVGL 的 binfont
     * 解析器会为每个字形做多次小读；直接走 SD 会产生 2 万多次 fread，
     * 整字库加载会拖到几十秒并让开机动画看起来卡死。 */
    int64_t started_us = esp_timer_get_time();
    uint8_t *font_data = NULL;
    size_t font_size = 0;
    lv_font_t *loaded = NULL;
    if (zh_font_read_all(vfs_path, &font_data, &font_size)) {
        loaded = lv_binfont_create_from_buffer(font_data, (uint32_t)font_size);
        heap_caps_free(font_data);
    }
    if (!loaded) {
        /* 内存缓冲分配失败时保留旧路径作为兜底；速度较慢但能显示中文。 */
        static char lv_path[ZH_FONT_MAX_PATH];
        snprintf(lv_path, sizeof(lv_path), "S:%s", vfs_path);
        const lv_binfont_dsc_t descriptor = {
            .path = lv_path,
            .dynamic_glyph_load = false,
        };
        loaded = lv_binfont_create_ex(&descriptor);
    }
    if (!loaded) {
        ESP_LOGW(TAG, "cannot load %s (bad format, truncated file, or PSRAM exhausted)", vfs_path);
        return;
    }
    loaded->fallback = &lv_font_montserrat_14;
    atomic_store(&font, loaded);
    ESP_LOGI(TAG, "Chinese font loaded from %s into PSRAM in %lld ms (%u KiB PSRAM free)",
             vfs_path, (long long)((esp_timer_get_time() - started_us) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void zh_font_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        zh_font_load_once();
        /* Failed loads also release the gate; the next tick retries later. */
        atomic_store(&font_loading, false);
    }
}


void zh_font_init(void)
{
    lv_fs_drv_init(&fs_drv);
    fs_drv.letter = 'S';
    fs_drv.open_cb = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb = fs_read;
    fs_drv.seek_cb = fs_seek;
    fs_drv.tell_cb = fs_tell;
    lv_fs_drv_register(&fs_drv);
    if (xTaskCreate(zh_font_task, "zh_font", ZH_FONT_TASK_STACK, NULL, 1,
                    &font_task_handle) != pdPASS) {
        font_task_handle = NULL;
        ESP_LOGE(TAG, "cannot create font loader task");
    }
}

/* 列出目录内容一次，帮助定位字库文件路径。 */
static void zh_font_list_dir(const char *path)
{
    tf_card_lock();
    DIR *dir = opendir(path);
    if (!dir) {
        tf_card_unlock();
        ESP_LOGW(TAG, "cannot list %s", path);
        return;
    }
    ESP_LOGW(TAG, "%s contains:", path);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
        ESP_LOGW(TAG, "  %s", entry->d_name);
    }
    closedir(dir);
    tf_card_unlock();
}

static void zh_font_diagnose_once(void)
{
    static bool done;
    if (done) { return; }
    done = true;

    zh_font_list_dir("/tf");
    zh_font_list_dir("/tf/fonts");
    zh_font_list_dir("/tf/font");
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
    /* 字库已经整体驻留 PSRAM，加载结束时文件句柄就已关闭。此函数只用于
     * 需要主动丢弃并重新读取字库的场合；拔卡和 U 盘切换不再调用它。 */
    lv_obj_tree_walk(NULL, retarget_font_cb, released);
    lv_binfont_destroy(released);
    /* 延后一个重试周期再加载，避免在调用者尚未完成卡切换时立刻重开文件。 */
    last_attempt_us = esp_timer_get_time();
}

/* 只在未加载且没有加载任务运行时投递请求，函数本身立即返回。
 * LVGL 的开机动画和触摸因此不会被 SD 卡读取阻塞。 */
void zh_font_tick(void)
{
    if (!font_task_handle || atomic_load(&font) || atomic_load(&font_loading)) { return; }
    tf_snapshot_t snapshot;
    if (!tf_card_snapshot(&snapshot) || !snapshot.mounted) { return; }
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
