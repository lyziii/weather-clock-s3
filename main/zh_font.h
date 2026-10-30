#pragma once

#include "lvgl.h"

/* Flash 的 zh_font 分区保存 LVGL 二进制点阵字库，不依赖 SD 卡。
 * zh_font_init() 在 LVGL 锁内调用一次，创建低优先级加载任务；
 * zh_font_tick() 只投递请求并立即返回，失败约每 3 秒重试。
 * 后台映射分区并解析，完成后释放映射，全部字形常驻 PSRAM。 */
void zh_font_init(void);
void zh_font_tick(void);

/* 主动销毁已加载的字库并释放其 PSRAM。
 * 正常运行时不需要调用：字库不持有 TF 文件句柄，拔卡、重新挂载和
 * 切换 U 盘都不会影响已加载的字形。调用者需持有 LVGL 锁。 */
void zh_font_release(void);

/* 返回已加载的中文字体；未加载返回 NULL，调用方回退 Montserrat。 */
const lv_font_t *zh_font(void);
