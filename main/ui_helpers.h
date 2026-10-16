#pragma once

#include "lvgl.h"

/* 页面共用的轻量图元，不依赖板级驱动；调用方持有 LVGL 锁。 */
static inline void place_label(lv_obj_t *label, int left, int top, int width)
{
    lv_obj_set_pos(label, left, top);
    lv_obj_set_width(label, width);
}

static inline lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                                   int width, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static inline lv_obj_t *make_shape(lv_obj_t *parent, int x, int y, int width, int height,
                                  uint32_t color, int radius)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    return shape;
}