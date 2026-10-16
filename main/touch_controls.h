#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 顶层触摸控件只投递命令；创建和状态更新均由持有 LVGL 锁的调用方执行。 */
void touch_controls_create(lv_display_t *display, lv_obj_t *ride_page,
						   bool (*submit)(const char *command));
void touch_controls_update(unsigned page, bool running);