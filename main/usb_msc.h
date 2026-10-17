#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* USB MSC（U盘）模式：把 TF 卡通过 USB OTG 暴露为可移动磁盘。 */
esp_err_t usb_msc_start(void);
esp_err_t usb_msc_stop(void);
bool usb_msc_active(void);
