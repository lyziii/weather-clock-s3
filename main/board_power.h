#pragma once

#include "esp_err.h"

/* 独立 ADC 采集任务：校准电池电压、估算电量并诊断 VBUS，失败不伪造读数。 */
esp_err_t board_power_init(void);
void board_power_print_status(void);