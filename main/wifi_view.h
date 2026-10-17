#pragma once

#include "lvgl.h"

/* 在系统二级容器内创建 WiFi 配置视图：AP 扫描列表与全屏触摸键盘。 */
void wifi_view_create_in(lv_display_t *display, lv_obj_t *parent);
