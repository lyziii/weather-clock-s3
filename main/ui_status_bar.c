#include <stdio.h>
#include "ui_status_bar.h"

static lv_obj_t *create_icon(lv_obj_t *parent, const char *symbol, int left, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, symbol);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, left + width - 240, 8);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8CE2BE), 0);
    return label;
}

void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent, bool show_gps)
{
    bar->show_gps = show_gps;
    bar->gps = create_icon(parent, LV_SYMBOL_GPS, 126, 16);
    bar->bluetooth = create_icon(parent, LV_SYMBOL_BLUETOOTH, 144, 16);
    bar->wifi = create_icon(parent, LV_SYMBOL_WIFI, 162, 18);
    bar->battery = create_icon(parent, LV_SYMBOL_BATTERY_EMPTY " ?", 184, 52);
    ui_status_t initial = {0};
    ui_status_bar_update(bar, &initial);
}

void ui_status_bar_update(ui_status_bar_t *bar, const ui_status_t *status)
{
    lv_obj_set_hidden(bar->wifi, !status->wifi_connected);
    lv_obj_set_hidden(bar->bluetooth, !status->bluetooth_connected);
    lv_obj_set_hidden(bar->gps, !bar->show_gps || !status->gps_fixed);
    /* 未接电量来源时明确显示未知，不能将默认零值伪装为真实低电量。 */
    if (!status->battery_valid) {
        lv_label_set_text(bar->battery, LV_SYMBOL_BATTERY_EMPTY " ?");
        lv_obj_set_style_text_color(bar->battery, lv_color_hex(0x8CA49B), 0);
        return;
    }
    unsigned percent = status->battery_percent > 100 ? 100 : status->battery_percent;
    const char *symbol = status->battery_charging ? LV_SYMBOL_CHARGE :
        percent >= 90 ? LV_SYMBOL_BATTERY_FULL : percent >= 65 ? LV_SYMBOL_BATTERY_3 :
        percent >= 40 ? LV_SYMBOL_BATTERY_2 : percent >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char text[24];
    snprintf(text, sizeof(text), "%s %u%%", symbol, percent);
    lv_label_set_text(bar->battery, text);
    lv_obj_set_style_text_color(bar->battery,
        lv_color_hex(percent <= 15 && !status->battery_charging ? 0xFFB86B : 0x8CE2BE), 0);
}