#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 开机动画期间屏蔽全局手势，避免提前翻页或展开睡眠面板。 */
void touch_controls_set_blocked(bool blocked);

/* 顶层触摸控件只投递命令；创建和状态更新均由持有 LVGL 锁的调用方执行。 */
void touch_controls_create(lv_display_t *display, lv_obj_t *ride_page,
						   bool (*submit)(const char *command));
void touch_controls_update(unsigned page, bool running);

/* 无有效姿态读数时显示手动翻转入口；调用方须持有 LVGL 锁。 */
void touch_controls_set_manual_rotation(bool available);