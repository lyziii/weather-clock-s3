#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 触摸回调只发通知；编解码器与 PCM 输出由音频任务独占。 */
esp_err_t touch_sound_init(void);
void touch_sound_play(void);
bool touch_sound_test(void);
void touch_sound_prepare_sleep(void);

/* 音乐播放：优先播放 TF 卡 /tf/music 或 /tf 根目录下第一个 16 位 PCM WAV，
 * 没有 WAV 时播放内置旋律。启动/停止都是异步通知，不阻塞 LVGL 回调。 */
bool touch_sound_music_start(void);
/* 指定 TF 卡上的 PCM WAV 作为铃声，失败时沿用内置旋律兜底。 */
bool touch_sound_music_start_path(const char *path);
/* 空路径直接播放内置旋律，不扫描 TF 卡。 */
bool touch_sound_alarm_start(const char *path);
void touch_sound_music_stop(void);
/* 闹钟临时覆盖音量；负值恢复硬件默认值，实际寄存器操作仍由音频任务执行。 */
void touch_sound_alarm_volume(int percent);
bool touch_sound_music_busy(void);

/* 把当前播放状态复制到 out；无音频设备时返回 false。 */
bool touch_sound_music_status(char *out, size_t size);