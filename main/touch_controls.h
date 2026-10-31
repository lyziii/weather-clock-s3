#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 开机动画期间屏蔽全局手势，避免提前翻页或展开顶部面板。 */
void touch_controls_set_blocked(bool blocked);

/* 顶层触摸控件只投递命令；创建和状态更新均由持有 LVGL 锁的调用方执行。 */
void touch_controls_create(lv_display_t *display, lv_obj_t *ride_page,
						   bool (*submit)(const char *command));
void touch_controls_update(unsigned page, bool running);

/* 下拉面板方向按钮的高亮：0 = 跟随重力，1..4 = 固定 0/90/180/270 度。 */
void touch_controls_set_rotation(unsigned selection);