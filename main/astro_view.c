#include <string.h>
#include "astro_frames.h"
#include "astro_view.h"
#include "ui_helpers.h"

/* 参考工程把 40 x 40 的位图直接推到屏幕上，这里放大两倍后放进 A8 画布：
 * 画布只存放透明度，颜色由 image_recolor 提供，因此改主题色不用改帧数据。 */
#define ASTRO_SCALE 2
#define ASTRO_WIDTH (ASTRO_FRAME_WIDTH * ASTRO_SCALE)
#define ASTRO_HEIGHT (ASTRO_FRAME_HEIGHT * ASTRO_SCALE)
#define ASTRO_FRAME_MS 100
/* 挂件配色与时钟页一致：卡片底 0x172321，笔画默认纯白，换主题色只改 ASTRO_INK。 */
#define ASTRO_PANEL_COLOR 0x172321
#define ASTRO_PANEL_RADIUS 12
#define ASTRO_INK 0xFFFFFF

/* 星点用千分比记录位置，面板尺寸变化时按比例重排，横竖屏都能铺满。 */
static const struct {
    uint16_t x;
    uint16_t y;
    uint8_t size;
    uint32_t color;
} astro_star_defs[] = {
    {  40, 170, 2, 0x526B61}, { 110, 640, 3, 0x3B504A}, { 195, 330, 2, 0x8CA49B},
    { 255, 880, 2, 0x526B61}, { 470,  80, 2, 0x3B504A}, { 530, 930, 3, 0x526B61},
    { 715, 110, 2, 0x8CA49B}, { 790, 700, 3, 0x526B61}, { 885, 370, 2, 0x3B504A},
    { 955, 870, 2, 0x8CA49B},
};

#define ASTRO_STAR_COUNT (sizeof(astro_star_defs) / sizeof(astro_star_defs[0]))

static lv_obj_t *astro_panel;
static lv_obj_t *astro_canvas;
static lv_obj_t *astro_stars[ASTRO_STAR_COUNT];
static unsigned astro_frame;
static uint8_t astro_pixels[ASTRO_WIDTH * ASTRO_HEIGHT] __attribute__((aligned(4)));

/* 位图按位展开到 A8 画布：置位写成不透明笔画，清零写成全透明背景，
 * 同时完成整数倍放大，避免运行时再做插值缩放。 */
static void astro_expand(unsigned frame)
{
    const uint8_t *source = astro_frames[frame % ASTRO_FRAME_COUNT];
    for (unsigned y = 0; y < ASTRO_FRAME_HEIGHT; ++y) {
        uint8_t *line = astro_pixels + y * ASTRO_SCALE * ASTRO_WIDTH;
        const uint8_t *row = source + y * ASTRO_ROW_BYTES;
        for (unsigned byte = 0; byte < ASTRO_ROW_BYTES; ++byte) {
            uint8_t bits = row[byte];
            for (unsigned bit = 0; bit < 8; ++bit) {
                uint8_t alpha = (bits & (1u << bit)) ? 0xFF : 0x00;
                unsigned left = (byte * 8 + bit) * ASTRO_SCALE;
                for (unsigned step = 0; step < ASTRO_SCALE; ++step) { line[left + step] = alpha; }
            }
        }
        for (unsigned copy = 1; copy < ASTRO_SCALE; ++copy) {
            memcpy(line + copy * ASTRO_WIDTH, line, ASTRO_WIDTH);
        }
    }
}

static void astro_show_frame(unsigned frame)
{
    astro_frame = frame % ASTRO_FRAME_COUNT;
    astro_expand(astro_frame);
    if (astro_canvas) { lv_obj_invalidate(astro_canvas); }
}

/* 与参考工程一致，每 100 ms 换一帧并循环播放；不在活动屏幕上时只做一次可见性判断。 */
static void astro_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!astro_panel || !lv_obj_is_visible(astro_panel)) { return; }
    astro_show_frame(astro_frame + 1);
}

/* 面板尺寸变化后按千分比重排星点，并把画布放回面板正中。 */
static void astro_place(int width, int height)
{
    for (unsigned index = 0; index < ASTRO_STAR_COUNT; ++index) {
        lv_obj_set_pos(astro_stars[index],
                       (int32_t)width * astro_star_defs[index].x / 1000,
                       (int32_t)height * astro_star_defs[index].y / 1000);
    }
    lv_obj_set_pos(astro_canvas, (width - ASTRO_WIDTH) / 2, (height - ASTRO_HEIGHT) / 2);
}

void astro_view_create_in(lv_obj_t *parent)
{
    astro_panel = make_shape(parent, 0, 0, 212, 84, ASTRO_PANEL_COLOR, ASTRO_PANEL_RADIUS);
    lv_obj_set_clickable(astro_panel, false);
    lv_obj_set_scrollable(astro_panel, false);
    for (unsigned index = 0; index < ASTRO_STAR_COUNT; ++index) {
        astro_stars[index] = make_shape(astro_panel, 0, 0, astro_star_defs[index].size,
                                        astro_star_defs[index].size, astro_star_defs[index].color,
                                        LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(astro_stars[index], false);
    }
    /* 画布挂在面板里，面板的圆角会自然裁掉溢出的边角。 */
    astro_canvas = lv_canvas_create(astro_panel);
    lv_canvas_set_buffer(astro_canvas, astro_pixels, ASTRO_WIDTH, ASTRO_HEIGHT, LV_COLOR_FORMAT_A8);
    lv_obj_set_style_image_recolor(astro_canvas, lv_color_hex(ASTRO_INK), 0);
    lv_obj_set_style_image_recolor_opa(astro_canvas, LV_OPA_COVER, 0);
    astro_place(212, 84);
    astro_show_frame(0);
    lv_timer_create(astro_tick, ASTRO_FRAME_MS, NULL);
}

void astro_view_layout(int x, int y, int width, int height)
{
    if (!astro_panel) { return; }
    lv_obj_set_pos(astro_panel, x, y);
    lv_obj_set_size(astro_panel, width, height);
    astro_place(width, height);
}

