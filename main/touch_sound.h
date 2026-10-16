#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 触摸回调只发通知；编解码器与 PCM 输出由音频任务独占。 */
esp_err_t touch_sound_init(void);
void touch_sound_play(void);
bool touch_sound_test(void);
void touch_sound_prepare_sleep(void);