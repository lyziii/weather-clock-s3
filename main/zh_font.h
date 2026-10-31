#pragma once

#include "lvgl.h"

/* 中文字库（LVGL 二进制点阵 .bin）常驻 Flash 的 font 数据分区，不依赖 TF 卡。
 * zh_font_init() 在 LVGL 锁内调用一次，创建低优先级解析任务后立即返回；
 * 任务把分区映射进数据地址空间，再交给 LVGL 的 binfont 解析器，字形数据按
 * Flash 地址直接读取。解析完成前 zh_font() 返回 NULL，调用方回退 Montserrat
 * 或英文，下一次界面刷新自动切换到中文。
 * zh_font_tick() 只在首次解析失败时按 3 秒间隔重试，可安全地在 LVGL 任务里
 * 周期调用。 */
void zh_font_init(void);
void zh_font_tick(void);

/* 返回已加载的中文字体；未加载返回 NULL，调用方回退 Montserrat。 */
const lv_font_t *zh_font(void);
