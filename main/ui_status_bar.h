#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* 状态栏只消费上层快照，不访问 ADC、无线协议栈或 GNSS 驱动。 */
typedef struct {
    bool wifi_connected;
    bool bluetooth_connected;
    bool gps_fixed;
    bool tf_ready;
    bool battery_valid;
    unsigned battery_percent;
    bool battery_charging;
} ui_status_t;

typedef struct {
    lv_obj_t *wifi;
    lv_obj_t *bluetooth;
    lv_obj_t *gps;
    lv_obj_t *tf;
    lv_obj_t *battery;
    bool show_gps;
} ui_status_bar_t;

/* 创建和更新必须在 LVGL 锁内调用，所有坐标相对页面的 240 像素内容区。 */
void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent, bool show_gps);
void ui_status_bar_update(ui_status_bar_t *bar, const ui_status_t *status);