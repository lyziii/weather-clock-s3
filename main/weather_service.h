#pragma once

#include "esp_err.h"

/* 天气数据不含 LVGL 对象，网络服务可独立于具体页面移植。 */
typedef struct {
    float temperature;
    int humidity;
    int code;
} weather_data_t;

esp_err_t weather_service_fetch(weather_data_t *weather);
const char *weather_service_description(int code);
const char *weather_service_description_zh(int code);