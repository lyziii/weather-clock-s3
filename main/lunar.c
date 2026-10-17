#include <stdint.h>
#include <stdio.h>
#include "lunar.h"

/* 农历数据 1900-2100（201 年）。每个值：
 * 低 4 位：闰月月份（0 = 无闰月）
 * bit4-15：正月到腊月的大小月（bit4=正月，bit15=腊月），1=大月30天，0=小月29天
 * bit16：闰月是否为30天（1=大月，0=小月）
 * 数据含 17 位，必须用 uint32_t 保存。 */
static const uint32_t lunar_info[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b5a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0,
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0,
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160,
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,
    0x0d520,
};

static const char *const lunar_month_name[] = {
    "", "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"
};

static const char *const lunar_day_name[] = {
    "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
};

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int leap_month_of(int year)
{
    return lunar_info[year - 1900] & 0xF;
}

/* bit16 标记闰月为大月(30 天)，否则为小月(29 天)。 */
static int lunar_leap_days(int year)
{
    return (lunar_info[year - 1900] & 0x10000) ? 30 : 29;
}

static int lunar_month_days(int year, int month)
{
    return (lunar_info[year - 1900] & (0x8000 >> (month - 1))) ? 30 : 29;
}

static int lunar_year_days(int year)
{
    int days = 0;
    for (int month = 1; month <= 12; ++month) {
        days += lunar_month_days(year, month);
    }
    return days + (leap_month_of(year) ? lunar_leap_days(year) : 0);
}

void lunar_format(int year, int month, int day, char *buffer, size_t size)
{
    if (!buffer || size == 0) { return; }
    buffer[0] = '\0';
    if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        return;
    }
    /* 以 1900-01-31（农历 1900 年正月初一）为基准累计天数。 */
    int offset = 0;
    for (int y = 1900; y < year; ++y) {
        offset += 365 + (is_leap_year(y) ? 1 : 0);
    }
    static const int solar_month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; ++m) {
        offset += solar_month_days[m - 1];
        if (m == 2 && is_leap_year(year)) { ++offset; }
    }
    offset += day - 31;
    if (offset < 0) { return; }

    int lunar_year = 1900;
    while (lunar_year < 2100 && offset >= lunar_year_days(lunar_year)) {
        offset -= lunar_year_days(lunar_year);
        ++lunar_year;
    }

    int result_month = 1;
    int result_day;
    int leap = leap_month_of(lunar_year);
    int is_leap_month = 0;
    for (int m = 1; m <= 12; ++m) {
        int days = lunar_month_days(lunar_year, m);
        if (offset < days) {
            result_month = m;
            break;
        }
        offset -= days;
        if (leap == m) {
            int leap_days = lunar_leap_days(lunar_year);
            if (offset < leap_days) {
                is_leap_month = 1;
                result_month = m;
                break;
            }
            offset -= leap_days;
        }
    }
    result_day = offset + 1;

    if (is_leap_month) {
        snprintf(buffer, size, "闰%s月%s", lunar_month_name[result_month], lunar_day_name[result_day]);
    } else {
        snprintf(buffer, size, "%s月%s", lunar_month_name[result_month], lunar_day_name[result_day]);
    }
}
