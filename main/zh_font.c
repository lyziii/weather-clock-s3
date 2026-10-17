#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "tf_card.h"
#include "zh_font.h"

#define ZH_FONT_MAX_PATH 64
#define ZH_RETRY_US (3 * 1000000)

/* 按顺序尝试的 VFS 路径，覆盖用户可能采用的几种拷贝位置。 */
static const char *const zh_font_candidates[] = {
    "/tf/fonts/ch16.bin",
    "/tf/font/ch16.bin",
    "/tf/ch16.bin",
};

static const char *TAG = "zh_font";

static lv_fs_drv_t fs_drv;   /* 必须静态存储：register 只保存指针 */
static lv_font_t *font;
static volatile bool release_pending;
static int64_t last_attempt_us;

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
    bool failed = read == 0 && btr != 0 && ferror((FILE *)file_p);
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
    release_pending = false;
    if (!font) { return; }
    /* 动态字形模式会一直持有字库文件，只有销毁字体才会关闭句柄。
     * 不关闭就卸载 TF 卡，之后 LVGL 取字形会落到已注销的磁盘驱动上。 */
    lv_obj_tree_walk(NULL, retarget_font_cb, font);
    lv_binfont_destroy(font);
    font = NULL;
    /* 延后一个重试周期再加载：切换 U 盘期间快照可能还停留在“已挂载”，
     * 立刻重试会在卸载前又把字库文件打开。 */
    last_attempt_us = esp_timer_get_time();
}

void zh_font_invalidate(void)
{
    release_pending = true;
}

void zh_font_tick(void)
{
    /* 卡被拔出/切换时由其它任务挂的标记：在这里（LVGL 任务内）安全地
     * 销毁旧字体并关闭文件句柄，下一拍再按挂载状态决定是否重新加载。 */
    if (release_pending) {
        zh_font_release();
        return;
    }
    if (font) { return; }
    tf_snapshot_t snapshot;
    if (!tf_card_snapshot(&snapshot) || !snapshot.mounted) { return; }
    int64_t now = esp_timer_get_time();
    if (now - last_attempt_us < ZH_RETRY_US) { return; }
    last_attempt_us = now;

    /* 先确认文件存在，再交给 LVGL 解析，便于区分“路径不对”和“格式不对”。 */
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

    /* 大字体按需加载字形：仅描述符/字表/kerning 常驻 RAM，字形按需从 SD 读。
     * LVGL 会保存 path 指针，因此必须使用静态缓冲区而非栈变量。 */
    static char lv_path[ZH_FONT_MAX_PATH];
    snprintf(lv_path, sizeof(lv_path), "S:%s", vfs_path);
    const lv_binfont_dsc_t descriptor = {
        .path = lv_path,
        .dynamic_glyph_load = true,
    };
    lv_font_t *loaded = lv_binfont_create_ex(&descriptor);
    if (!loaded) {
        ESP_LOGW(TAG, "%s exists but is not a valid LVGL bin font (regenerate with lv_font_conv --format bin)",
                 vfs_path);
        return;
    }
    /* 中文字库缺字（图标符号等）回退到 Montserrat，保证符号与英文正常显示。 */
    loaded->fallback = &lv_font_montserrat_14;
    font = loaded;
    ESP_LOGI(TAG, "Chinese font loaded from %s with dynamic glyph loading", vfs_path);
}

const lv_font_t *zh_font(void)
{
    return font;
}
