#pragma once

#include "lvgl.h"

/* SD 卡中文字库（LVGL 二进制点阵 .bin）按需加载。
 * zh_font_init() 在 LVGL 锁内调用一次，注册 S: 文件系统驱动；
 * zh_font_tick() 在 LVGL 任务内周期调用，等 TF 卡挂载后尝试加载
 * /tf/fonts/ch16.bin，失败会约每 3 秒重试一次。 */
void zh_font_init(void);
void zh_font_tick(void);

/* 销毁已加载的字库并关闭 binfont 持有的文件句柄。
 * 进入 U 盘模式前必须先调用：卸载 /tf 时只要还有文件打开，后续取字形
 * 就会访问已注销的磁盘驱动。调用者需持有 LVGL 锁；重新挂载后
 * zh_font_tick() 会自动再次加载。 */
void zh_font_release(void);

/* 返回已加载的中文字体；未加载返回 NULL，调用方回退 Montserrat。 */
const lv_font_t *zh_font(void);

/* 卡被拔出等无法获取 LVGL 锁的场合调用：只挂“待释放”标记，
 * 由 LVGL 任务在下一次 zh_font_tick() 里真正销毁字体并关闭句柄，
 * 避免在 tf_card 任务里反向等待 LVGL 锁造成死锁。 */
void zh_font_invalidate(void);
