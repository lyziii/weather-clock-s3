#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ride_model.h"

/* 初始化显示和独立天气任务；缺少接线配置时仍可通过串口读取天气。 */
esp_err_t weather_clock_init(void);
void weather_clock_tick(bool online, bool time_valid);
/* 命令和传感器入口内部加锁，允许串口任务与时钟任务并发调用。 */
bool weather_clock_command(const char *command);
bool weather_clock_ride_sample(const ride_sample_t *sample);

/* 由电量驱动和蓝牙连接/断开回调上报真实状态；未上报时电量未知、蓝牙隐藏。 */
bool weather_clock_battery_status(bool valid, unsigned percent, bool charging);
bool weather_clock_bluetooth_status(bool connected);