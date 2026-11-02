#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 开机动画期间屏蔽全局手势，避免提前翻页或展开睡眠面板。 */
void touch_controls_set_blocked(bool blocked);

/* 顶层触摸控件只投递命令；创建和状态更新均由持有 LVGL 锁的调用方执行。 */
void touch_controls_create(lv_display_t *display, lv_obj_t *ride_page,
						   bool (*submit)(const char *command));
void touch_controls_update(unsigned page, bool running);

/* 无有效姿态读数时显示四档方向选择，并高亮已应用方向；须持有 LVGL 锁。 */
void touch_controls_set_manual_rotation(bool available, lv_display_rotation_t rotation);
/* 开关状态由命令任务确认；设置状态不会再次触发用户事件。 */
void touch_controls_set_light_theme(bool light);