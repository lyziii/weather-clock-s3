#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* 板级显示适配入口；页面回调在 LVGL 锁内执行，失败时输出显示句柄为空。 */
esp_err_t display_port_init(lv_display_t **output, void (*create_pages)(lv_display_t *display));

/* 初始化完成后只读：触摸失败阶段与十六进制错误码，供无串口日志时排查。 */
const char *display_port_touch_diagnostic(void);