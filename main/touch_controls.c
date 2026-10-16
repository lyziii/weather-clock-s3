#include "touch_controls.h"
#include "esp_log.h"
#include "ui_helpers.h"

static lv_display_t *control_display;
static bool (*submit_command)(const char *command);
static lv_obj_t *page_indicator;
static lv_obj_t *page_dots[3];
static lv_obj_t *ride_buttons[3];
static lv_obj_t *reset_overlay;
static unsigned active_page = 3;

/* 分页点只在翻页时显示，220 ms 页面动画结束后再淡出，不占布局空间。 */
static void fade_indicator(void *object, int32_t opacity)
{
    lv_obj_set_style_opa(object, opacity, 0);
}

static void show_indicator(void)
{
    lv_anim_delete(page_indicator, fade_indicator);
    lv_obj_set_style_opa(page_indicator, LV_OPA_COVER, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, page_indicator);
    lv_anim_set_exec_cb(&animation, fade_indicator);
    lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&animation, 300);
    lv_anim_set_duration(&animation, 240);
    lv_anim_start(&animation);
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, const char *command)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x283B36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x39715D), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x527C6E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_scrollable(button, false);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)command);
    return button;
}

static void command_clicked(lv_event_t *event)
{
    const char *command = lv_event_get_user_data(event);
    if (!submit_command(command)) { ESP_LOGW("touch_ui", "Command queue busy"); }
}

/* 原生手势按显示旋转后的坐标识别；一次滑动只提交一次，弹窗期间禁止翻页。 */
static void page_gesture(lv_event_t *event)
{
    if (!control_display || reset_overlay || lv_display_get_screen_loading(control_display)) { return; }
    lv_indev_t *input = lv_event_get_user_data(event);
    lv_dir_t direction = lv_indev_get_gesture_dir(input);
    const char *command;
    if (direction == LV_DIR_RIGHT) { command = "page next"; }
    else if (direction == LV_DIR_LEFT) { command = "page prev"; }
    else { return; }
    lv_indev_wait_release(input);
    if (!submit_command(command)) { ESP_LOGW("touch_ui", "Page queue busy"); }
}

static void close_reset(lv_event_t *event)
{
    const char *command = lv_event_get_user_data(event);
    if (command && !submit_command(command)) {
        ESP_LOGW("touch_ui", "Reset queue busy");
        return;
    }
    lv_obj_delete(reset_overlay);
    reset_overlay = NULL;
}

/* 清零必须二次确认，测试触摸时不因单次误触丢失骑行数据。 */
static void reset_clicked(lv_event_t *event)
{
    (void)event;
    if (reset_overlay) { return; }
    reset_overlay = lv_obj_create(lv_display_get_layer_top(control_display));
    lv_obj_remove_style_all(reset_overlay);
    lv_obj_set_size(reset_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(reset_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(reset_overlay, LV_OPA_70, 0);
    lv_obj_set_scrollable(reset_overlay, false);
    lv_obj_t *dialog = make_shape(reset_overlay, 0, 0, 220, 110, 0x172C25, 4);
    lv_obj_set_scrollable(dialog, false);
    lv_obj_center(dialog);
    lv_obj_t *title = make_label(dialog, "Reset trip?", 12, 14, 196,
                                 &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *cancel = create_button(dialog, "Cancel", close_reset, NULL);
    lv_obj_set_pos(cancel, 12, 52);
    lv_obj_set_size(cancel, 92, 42);
    lv_obj_t *confirm = create_button(dialog, "Reset", close_reset, "ride reset");
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0x8A433C), 0);
    lv_obj_set_pos(confirm, 116, 52);
    lv_obj_set_size(confirm, 92, 42);
}

static void controls_deleted(lv_event_t *event)
{
    (void)event;
    lv_anim_delete(page_indicator, fade_indicator);
    page_indicator = NULL;
    control_display = NULL;
    active_page = 3;
}

void touch_controls_create(lv_display_t *display, lv_obj_t *ride_page,
                           bool (*submit)(const char *command))
{
    control_display = display;
    submit_command = submit;
    page_indicator = make_shape(lv_display_get_layer_top(display), 0, 0, 52, 12, 0x10221C, 4);
    lv_obj_set_clickable(page_indicator, false);
    lv_obj_set_scrollable(page_indicator, false);
    lv_obj_align(page_indicator, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_opa(page_indicator, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(page_indicator, controls_deleted, LV_EVENT_DELETE, NULL);
    for (unsigned index = 0; index < 3; ++index) {
        page_dots[index] = make_shape(page_indicator, 7 + index * 16, 3, 6, 6,
                                      0x527C6E, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(page_dots[index], false);
        lv_obj_set_scrollable(page_dots[index], false);
    }
    /* 图标属于码表页面，随页面移动和隐藏，不再覆盖其他模式。 */
    ride_buttons[0] = create_button(ride_page, LV_SYMBOL_PLAY, command_clicked, "ride start");
    ride_buttons[1] = create_button(ride_page, LV_SYMBOL_PAUSE, command_clicked, "ride pause");
    ride_buttons[2] = create_button(ride_page, LV_SYMBOL_TRASH, reset_clicked, NULL);
    for (unsigned index = 0; index < 3; ++index) {
        lv_obj_set_size(ride_buttons[index], 48, 36);
        lv_obj_align(ride_buttons[index], LV_ALIGN_BOTTOM_MID, ((int)index - 1) * 64, -16);
    }
    lv_indev_t *input = NULL;
    while ((input = lv_indev_get_next(input))) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_display(input) == display) {
            lv_indev_add_event_cb(input, page_gesture, LV_EVENT_GESTURE, input);
            break;
        }
    }
}

void touch_controls_update(unsigned page, bool running)
{
    if (!page_indicator) { return; }
    if (page >= 3) { return; }
    if (page != active_page) {
        if (active_page < 3) { show_indicator(); }
        active_page = page;
    }
    for (unsigned index = 0; index < 3; ++index) {
        lv_obj_set_style_bg_color(page_dots[index],
                                 lv_color_hex(index == page ? 0x8CE2BE : 0x527C6E), 0);
        bool disabled = page != 1 || (index == 0 && running) || (index == 1 && !running);
        if (disabled) { lv_obj_add_state(ride_buttons[index], LV_STATE_DISABLED); }
        else { lv_obj_remove_state(ride_buttons[index], LV_STATE_DISABLED); }
    }
}