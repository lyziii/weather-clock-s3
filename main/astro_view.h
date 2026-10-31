#pragma once

#include "lvgl.h"

/*
 * 时钟页的太空人挂件：圆角面板 + 静态星点 + 80 x 80 的 A8 画布。
 * 画布只存透明度，颜色由 image_recolor 提供，每 100 ms 换一帧。
 * 位置与尺寸由 center_ui() 通过 astro_view_layout() 给出，横竖屏各一套；
 * 面板不在活动屏幕或已被隐藏时定时器空转，不刷新画布。
 */
void astro_view_create_in(lv_obj_t *parent);
void astro_view_layout(int x, int y, int width, int height);
