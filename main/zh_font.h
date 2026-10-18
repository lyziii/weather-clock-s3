#pragma once

#include "lvgl.h"

/* SD 卡中文字库（LVGL 二进制点阵 .bin）整体载入 PSRAM。
 * zh_font_init() 在 LVGL 锁内调用一次，注册 S: 文件系统驱动并创建低优先级
 * 加载任务；zh_font_tick() 在 LVGL 任务内周期调用，只投递加载请求并立即返回，
 * 等 TF 卡挂载后由后台任务读取 /tf/fonts/ch16.bin，失败约每 3 秒重试。
 * 加载成功后字形位图、字形描述符和字表全部常驻 PSRAM，文件句柄立即关闭。 */
void zh_font_init(void);
void zh_font_tick(void);

/* 主动销毁已加载的字库并释放其 PSRAM。
 * 正常运行时不需要调用：字库不持有 TF 文件句柄，拔卡、重新挂载和
 * 切换 U 盘都不会影响已加载的字形。调用者需持有 LVGL 锁。 */
void zh_font_release(void);

/* 返回已加载的中文字体；未加载返回 NULL，调用方回退 Montserrat。 */
const lv_font_t *zh_font(void);
