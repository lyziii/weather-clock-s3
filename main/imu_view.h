#pragma once

#include "lvgl.h"

/* 单实例诊断页：拥有箭头、标签和刷新定时器，不管理应用模式或 NVS。 */
lv_obj_t *imu_view_create(lv_display_t *display);

/* 屏幕方向改变后重排诊断区、姿态图及数据列；调用方持有 LVGL 锁。 */
void imu_view_layout(int width, int height);