#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    uint8_t enabled;
    uint8_t volume;
    char ringtone[96];
} cyber_alarm_t;

typedef struct {
    uint32_t version;
    cyber_alarm_t alarms[3];
    char endpoint[192];
    char model[64];
    char api_key[192];
} cyber_settings_t;

/* 配置与网络均在后台任务处理；不打印密钥，不访问 LVGL 对象。 */
bool cyber_services_init(cyber_settings_t *settings);
bool cyber_settings_save(const cyber_settings_t *settings);
bool cyber_chat_start(const cyber_settings_t *settings, const char *messages_json);
bool cyber_chat_poll(char *reply, unsigned capacity, bool *busy);