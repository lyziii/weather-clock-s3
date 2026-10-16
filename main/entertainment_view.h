#pragma once

#include "lvgl.h"

/* 系统页维护滚动菜单和二级导航，数据只读取后台快照。 */
lv_obj_t *entertainment_view_create(void);
bool entertainment_view_card_ready(void);
bool entertainment_view_is_child(void);
void entertainment_view_home(void);
void entertainment_view_open_imu(void);