#pragma once

#include <stdbool.h>

/* 负责屏幕休眠和 FT6336G 三击唤醒；唤醒门控须在 app_main 最早阶段调用。 */
void power_manager_enter_sleep(void);
void power_manager_handle_wakeup(void);
bool power_manager_wakeup_fault(void);