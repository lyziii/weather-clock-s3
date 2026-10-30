#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "weather_service.h"

/* 工具界面由 LVGL 任务独占，外部调用须持有界面锁。 */
void cyber_tools_create(lv_obj_t *screen);
void cyber_tools_resize(void);
void cyber_tools_tick(bool online, bool time_valid);
void cyber_tools_weather(const weather_data_t *weather, bool success);
bool cyber_tools_is_child(void);
void cyber_tools_home(void);
/* 不具备 RTC 闹钟唤醒时，不允许深睡丢失正在进行的提醒或网络请求。 */
bool cyber_tools_allow_sleep(void);