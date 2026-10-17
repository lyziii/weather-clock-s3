#pragma once

#include <stddef.h>

/* 公历转农历，输出形如 "九月初七"、"闰二月十五"（不含“农历”前缀）。
 * 覆盖 1900-2100 年；越界或异常时输出空串。 */
void lunar_format(int year, int month, int day, char *buffer, size_t size);
