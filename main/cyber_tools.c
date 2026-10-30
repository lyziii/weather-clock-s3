#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "cyber_tools.h"
#include "cyber_assets.h"
#include "cyber_services.h"
#include "tinyexpr.h"
#include "touch_sound.h"
#include "touch_controls.h"
#include "zh_font.h"
#include "ui_helpers.h"
#include "weather_clock_config.h"

typedef enum {
    CYBER_CLOCK, CYBER_DESKTOP, CYBER_ALARM, CYBER_POMODORO,
    CYBER_STOPWATCH, CYBER_COUNTDOWN, CYBER_CALENDAR, CYBER_CALCULATOR, CYBER_AI
} cyber_page_t;

static lv_obj_t *root;
static lv_obj_t *content;
static lv_obj_t *clock_time;
static lv_obj_t *clock_date;
static lv_obj_t *clock_weather;
static lv_obj_t *clock_seconds;
static lv_obj_t *clock_temperature;
static lv_obj_t *clock_humidity;
static cyber_page_t page;
static bool connected;
static bool clock_valid;
static weather_data_t weather_cache;
static bool weather_valid;
static const lv_font_t *applied_font;

typedef struct {
    bool running;
    int64_t remaining;
    int64_t deadline;
    int64_t total;
} countdown_t;

static cyber_settings_t settings;
static bool services_ready;
static countdown_t countdown = {.remaining = 60000000, .total = 60000000};
static countdown_t pomodoro = {.remaining = 1500000000, .total = 1500000000};
static bool break_period;
static unsigned sessions;
static bool stopwatch_running;
static int64_t stopwatch_started;
static int64_t stopwatch_elapsed;
static int64_t laps[5];
static unsigned lap_count;
static lv_obj_t *timer_value;
static lv_obj_t *timer_status;
static lv_obj_t *timer_progress;
static lv_obj_t *timer_toggle;
static lv_obj_t *lap_list;
static lv_obj_t *notification;
static int ringing_alarm = -1;
static int64_t notification_started;
static int64_t last_ring;
static int64_t alarm_fired[3];
static unsigned alarm_editor;
static lv_obj_t *alarm_hours;
static lv_obj_t *alarm_minutes;
static lv_obj_t *alarm_enabled;
static lv_obj_t *alarm_days[7];
static lv_obj_t *alarm_ringtone;
static lv_obj_t *alarm_volume;
static cyber_alarm_t alarm_drafts[3];
static unsigned displayed_alarm;
static lv_obj_t *editor_keyboard;
static int calendar_year = 2026;
static int calendar_month = 1;
static lv_obj_t *calendar_object;
static lv_obj_t *calendar_title;
static unsigned calculator_mode;
static bool calculator_degrees;
static char calculator_expression[192];
static lv_obj_t *calculator_input;
static lv_obj_t *calculator_result;
static lv_obj_t *graph_minimum;
static lv_obj_t *graph_maximum;
static char graph_minimum_draft[32] = "-10";
static char graph_maximum_draft[32] = "10";
static lv_obj_t *graph_chart;
static lv_chart_series_t *graph_series;
static bool ai_settings_open;
static lv_obj_t *ai_endpoint;
static lv_obj_t *ai_model;
static lv_obj_t *ai_key;
static lv_obj_t *ai_prompt;
static lv_obj_t *ai_transcript;
static lv_obj_t *ai_status;
static cJSON *chat_messages;
static char chat_text[8192];
static char chat_response[4096];
static bool chat_pending;
static bool calendar_initialized;
static char endpoint_draft[192];
static char model_draft[64];
static char key_draft[192];
static char prompt_draft[1537];

static void create_alarm(int width, int height);
static void create_timer(int width, int height);
static void refresh_tools(void);
static void create_calendar(int width, int height);
static void create_calculator(int width, int height);
static void create_ai(int width, int height);
static void poll_chat(void);

static const char *const page_titles[] = {
    "太空人时钟", "实用工具", "闹钟", "番茄钟", "秒表", "倒计时", "日历", "计算器", "AI 助手"
};

static void show_page(cyber_page_t next);

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, int width, uint32_t color)
{
    return make_label(parent, value, x, y, width, zh_font() ? zh_font() : LV_FONT_DEFAULT, color);
}

static void navigate(lv_event_t *event)
{
    show_page((cyber_page_t)(uintptr_t)lv_event_get_user_data(event));
}

static lv_obj_t *button(lv_obj_t *parent, const char *caption, int x, int y, int width, int height,
                        lv_event_cb_t callback, uintptr_t action)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, 4, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x10212C), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(0x33515E), 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_scrollable(object, false);
    lv_obj_t *label = text(object, caption, 0, 0, width - 4, 0xFFFFFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, (void *)action);
    return object;
}

static void keyboard_done(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target_obj(event);
    lv_obj_set_hidden(keyboard, true);
    lv_obj_set_height(content, lv_obj_get_height(root) - lv_obj_get_y(content));
    if (page == CYBER_AI && !ai_settings_open) { lv_obj_set_scrollable(content, false); }
}

static void edit_focused(lv_event_t *event)
{
    lv_obj_t *area = lv_event_get_target_obj(event);
    if (!editor_keyboard) { return; }
    lv_keyboard_set_textarea(editor_keyboard, area);
    lv_obj_set_hidden(editor_keyboard, false);
    lv_obj_move_foreground(editor_keyboard);
    /* 缩小可滚动内容区，保证输入框不会被软键盘遮挡。 */
    lv_obj_set_scrollable(content, true);
    lv_obj_set_height(content, lv_obj_get_height(root) - lv_obj_get_y(content) - lv_obj_get_height(editor_keyboard));
    lv_obj_scroll_to_view(area, LV_ANIM_ON);
}

static lv_obj_t *input(lv_obj_t *parent, const char *value, const char *placeholder,
                       int x, int y, int width, unsigned max_length, bool secret)
{
    lv_obj_t *area = lv_textarea_create(parent);
    lv_obj_set_pos(area, x, y);
    lv_obj_set_size(area, width, 36);
    lv_textarea_set_one_line(area, true);
    lv_textarea_set_max_length(area, max_length);
    lv_textarea_set_placeholder_text(area, placeholder);
    lv_textarea_set_password_mode(area, secret);
    lv_textarea_set_text(area, value);
    lv_obj_set_style_text_font(area, zh_font() ? zh_font() : LV_FONT_DEFAULT, 0);
    lv_obj_add_event_cb(area, edit_focused, LV_EVENT_FOCUSED, NULL);
    return area;
}

static void create_keyboard(int width, int height)
{
    editor_keyboard = lv_keyboard_create(root);
    lv_obj_set_size(editor_keyboard, width, height > 260 ? 136 : 112);
    lv_obj_align(editor_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(editor_keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(editor_keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(editor_keyboard, true);
}

static void dismiss_notification(lv_event_t *event)
{
    (void)event;
    if (notification) { lv_obj_delete(notification); notification = NULL; }
    if (ringing_alarm >= 0) {
        touch_sound_music_stop();
        touch_sound_alarm_volume(-1);
    }
    ringing_alarm = -1;
    touch_controls_set_blocked(false);
}

static void notify(const char *caption, int alarm)
{
    if (notification) { dismiss_notification(NULL); }
    ringing_alarm = alarm;
    notification_started = esp_timer_get_time();
    last_ring = 0;
    notification = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(root)));
    lv_obj_remove_style_all(notification);
    lv_obj_set_size(notification, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(notification, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(notification, LV_OPA_90, 0);
    lv_obj_set_scrollable(notification, false);
    int width = lv_display_get_horizontal_resolution(lv_obj_get_display(root));
    int height = lv_display_get_vertical_resolution(lv_obj_get_display(root));
    lv_obj_t *title = text(notification, caption, 10, height / 2 - 60, width - 20, 0xFFFF00);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    button(notification, LV_SYMBOL_OK, width / 2 - 44, height / 2, 88, 44, dismiss_notification, 0);
    touch_controls_set_blocked(true);
    if (alarm >= 0) {
        /* 到点立即接管音频，不能等系统页正在播放的长曲目结束。 */
        touch_sound_alarm_volume(settings.alarms[alarm].volume);
        touch_sound_alarm_start(settings.alarms[alarm].ringtone);
        last_ring = notification_started;
    } else if (alarm == -1) { touch_sound_test(); }
}

static int64_t remaining(const countdown_t *timer, int64_t now)
{
    if (!timer->running) { return timer->remaining; }
    return timer->deadline > now ? timer->deadline - now : 0;
}

static int64_t elapsed(int64_t now)
{
    return stopwatch_elapsed + (stopwatch_running ? now - stopwatch_started : 0);
}

static void timer_action(lv_event_t *event)
{
    unsigned action = (uintptr_t)lv_event_get_user_data(event);
    int64_t now = esp_timer_get_time();
    if (page == CYBER_STOPWATCH) {
        if (action == 0) {
            if (stopwatch_running) { stopwatch_elapsed = elapsed(now); stopwatch_running = false; }
            else { stopwatch_started = now; stopwatch_running = true; }
        } else if (action == 1) {
            stopwatch_running = false;
            stopwatch_elapsed = 0;
            lap_count = 0;
        } else if (stopwatch_running) {
            memmove(laps + 1, laps, 4 * sizeof(laps[0]));
            laps[0] = elapsed(now);
            ++lap_count;
        }
    } else {
        countdown_t *timer = page == CYBER_POMODORO ? &pomodoro : &countdown;
        if (action == 0) {
            if (timer->running) { timer->remaining = remaining(timer, now); timer->running = false; }
            else {
                if (!timer->remaining) { timer->remaining = timer->total; }
                timer->deadline = now + timer->remaining;
                timer->running = true;
            }
        } else if (action == 1) {
            timer->running = false;
            if (page == CYBER_POMODORO) {
                break_period = false;
                sessions = 0;
                timer->total = 1500000000;
            }
            timer->remaining = timer->total;
        } else if (!timer->running && page == CYBER_COUNTDOWN) {
            int change = action == 2 ? -10 : action == 3 ? 10 : action == 4 ? -60 : 60;
            int64_t seconds = timer->total / 1000000 + change;
            if (seconds < 10) { seconds = 10; }
            if (seconds > 3600) { seconds = 3600; }
            timer->total = timer->remaining = seconds * 1000000;
        }
    }
    refresh_tools();
}

static void create_timer(int width, int height)
{
    timer_status = text(content, "", 12, 4, width - 24, 0xFFA500);
    timer_value = make_label(content, "00:00", 0, 32, width, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_align(timer_value, LV_TEXT_ALIGN_CENTER, 0);
    if (page == CYBER_STOPWATCH) {
        lv_obj_set_style_text_font(timer_value, &lv_font_montserrat_28, 0);
        lv_obj_t *lap_view = lv_obj_create(content);
        lv_obj_remove_style_all(lap_view);
        lv_obj_set_pos(lap_view, 12, 78);
        lv_obj_set_size(lap_view, width - 24, height - 132);
        lv_obj_set_scroll_dir(lap_view, LV_DIR_VER);
        lap_list = text(lap_view, "", 0, 0, width - 32, 0xC6C6C6);
        button(content, LV_SYMBOL_PLUS, width - 54, height - 48, 44, 38, timer_action, 2);
    } else {
        timer_progress = lv_bar_create(content);
        lv_obj_set_pos(timer_progress, 12, 92);
        lv_obj_set_size(timer_progress, width - 24, 10);
        lv_bar_set_range(timer_progress, 0, 1000);
        if (page == CYBER_COUNTDOWN) {
            int step_width = (width - 42) / 4;
            const char *steps[] = {"-60", "-10", "+10", "+60"};
            const unsigned actions[] = {4, 2, 3, 5};
            for (unsigned index = 0; index < 4; ++index) {
                button(content, steps[index], 12 + (int)index * (step_width + 6),
                       118, step_width, 34, timer_action, actions[index]);
            }
        }
    }
    timer_toggle = button(content, LV_SYMBOL_PLAY, 12, height - 48,
                          64, 38, timer_action, 0);
    button(content, LV_SYMBOL_REFRESH, 88, height - 48, 64, 38, timer_action, 1);
}

static void select_alarm(lv_event_t *event)
{
    alarm_editor = (uintptr_t)lv_event_get_user_data(event);
    show_page(CYBER_ALARM);
}

static void capture_alarm(void)
{
    cyber_alarm_t *draft = &alarm_drafts[displayed_alarm];
    draft->hour = lv_roller_get_selected(alarm_hours);
    draft->minute = lv_roller_get_selected(alarm_minutes);
    draft->enabled = lv_obj_has_state(alarm_enabled, LV_STATE_CHECKED);
    draft->volume = lv_slider_get_value(alarm_volume);
    draft->days = 0;
    for (unsigned index = 0; index < 7; ++index) {
        if (lv_obj_has_state(alarm_days[index], LV_STATE_CHECKED)) { draft->days |= 1U << index; }
    }
    snprintf(draft->ringtone, sizeof(draft->ringtone), "%s", lv_textarea_get_text(alarm_ringtone));
}

static void save_alarm(lv_event_t *event)
{
    (void)event;
    capture_alarm();
    const char *path = lv_textarea_get_text(alarm_ringtone);
    size_t length = strlen(path);
    if (path[0] && (strncmp(path, "/tf/", 4) != 0 || length < 4 || strcasecmp(path + length - 4, ".wav") != 0)) {
        notify("铃声需要 /tf/ 下的 WAV 文件", -2);
        return;
    }
    settings.alarms[alarm_editor] = alarm_drafts[alarm_editor];
    notify(services_ready && cyber_settings_save(&settings) ? "闹钟已提交保存" : "保存队列不可用", -2);
}

static void create_alarm(int width, int height)
{
    lv_obj_set_scrollable(content, true);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    displayed_alarm = alarm_editor;
    cyber_alarm_t *alarm = &alarm_drafts[alarm_editor];
    for (unsigned index = 0; index < 3; ++index) {
        char caption[16];
        snprintf(caption, sizeof(caption), "%u %02u:%02u", index + 1,
                 alarm_drafts[index].hour, alarm_drafts[index].minute);
        lv_obj_t *tab = button(content, caption, 4 + (int)index * (width / 3), 4,
                               width / 3 - 8, 32, select_alarm, index);
        lv_obj_set_style_border_color(tab, lv_color_hex(index == alarm_editor ? 0x00FFFF : 0x424242), 0);
    }
    char hours[80] = {0};
    char minutes[200] = {0};
    for (unsigned index = 0; index < 24; ++index) {
        snprintf(hours + strlen(hours), sizeof(hours) - strlen(hours), "%s%02u", index ? "\n" : "", index);
    }
    for (unsigned index = 0; index < 60; ++index) {
        snprintf(minutes + strlen(minutes), sizeof(minutes) - strlen(minutes), "%s%02u", index ? "\n" : "", index);
    }
    alarm_hours = lv_roller_create(content);
    alarm_minutes = lv_roller_create(content);
    lv_roller_set_options(alarm_hours, hours, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_options(alarm_minutes, minutes, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(alarm_hours, 3);
    lv_roller_set_visible_row_count(alarm_minutes, 3);
    lv_obj_set_pos(alarm_hours, 12, 48);
    lv_obj_set_pos(alarm_minutes, 94, 48);
    lv_obj_set_width(alarm_hours, 70);
    lv_obj_set_width(alarm_minutes, 70);
    lv_roller_set_selected(alarm_hours, alarm->hour, LV_ANIM_OFF);
    lv_roller_set_selected(alarm_minutes, alarm->minute, LV_ANIM_OFF);
    alarm_enabled = lv_switch_create(content);
    lv_obj_set_pos(alarm_enabled, width - 62, 70);
    if (alarm->enabled) { lv_obj_add_state(alarm_enabled, LV_STATE_CHECKED); }
    const char *const days[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (unsigned index = 0; index < 7; ++index) {
        alarm_days[index] = lv_checkbox_create(content);
        lv_obj_set_pos(alarm_days[index], 8 + (int)(index % 4) * (width / 4), 156 + (int)(index / 4) * 32);
        lv_checkbox_set_text(alarm_days[index], days[index]);
        lv_obj_set_style_text_font(alarm_days[index], zh_font() ? zh_font() : LV_FONT_DEFAULT, 0);
        if (alarm->days & (1U << index)) { lv_obj_add_state(alarm_days[index], LV_STATE_CHECKED); }
    }
    alarm_ringtone = input(content, alarm->ringtone, "/tf/music/alarm.wav", 8, 224, width - 16, 95, false);
    text(content, LV_SYMBOL_VOLUME_MAX, 8, 276, 28, 0xFFFFFF);
    alarm_volume = lv_slider_create(content);
    lv_obj_set_pos(alarm_volume, 44, 280);
    lv_obj_set_size(alarm_volume, width - 70, 12);
    lv_slider_set_range(alarm_volume, 0, 100);
    lv_slider_set_value(alarm_volume, alarm->volume, LV_ANIM_OFF);
    button(content, LV_SYMBOL_SAVE, width - 66, 314, 56, 38, save_alarm, 0);
    create_keyboard(width, height + 34);
}

static void calendar_move(lv_event_t *event)
{
    intptr_t direction = (intptr_t)lv_event_get_user_data(event);
    if (direction == 0) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        if (clock_valid) { calendar_year = local.tm_year + 1900; calendar_month = local.tm_mon + 1; }
    } else {
        calendar_month += (int)direction;
        if (calendar_month < 1) { calendar_month = 12; --calendar_year; }
        if (calendar_month > 12) { calendar_month = 1; ++calendar_year; }
        if (calendar_year < 1900) { calendar_year = 1900; calendar_month = 1; }
        if (calendar_year > 2199) { calendar_year = 2199; calendar_month = 12; }
    }
    lv_calendar_set_month_shown(calendar_object, calendar_year, calendar_month);
    lv_label_set_text_fmt(calendar_title, "%04d / %02d", calendar_year, calendar_month);
}

static void calendar_selected(lv_event_t *event)
{
    lv_calendar_date_t date;
    if (lv_calendar_get_pressed_date(lv_event_get_target_obj(event), &date) == LV_RESULT_OK) {
        lv_label_set_text_fmt(calendar_title, "%04u-%02u-%02u", date.year, date.month, date.day);
    }
}

static void create_calendar(int width, int height)
{
    calendar_title = text(content, "", 42, 8, width - 124, 0x00FFFF);
    lv_obj_set_style_text_align(calendar_title, LV_TEXT_ALIGN_CENTER, 0);
    button(content, LV_SYMBOL_LEFT, 4, 2, 32, 30, calendar_move, (uintptr_t)-1);
    button(content, LV_SYMBOL_RIGHT, width - 36, 2, 32, 30, calendar_move, 1);
    button(content, LV_SYMBOL_REFRESH, width - 76, 2, 32, 30, calendar_move, 0);
    calendar_object = lv_calendar_create(content);
    lv_obj_set_pos(calendar_object, 4, 40);
    lv_obj_set_size(calendar_object, width - 8, height - 46);
    lv_obj_set_style_pad_all(calendar_object, 2, 0);
    lv_obj_set_style_text_font(calendar_object, zh_font() ? zh_font() : LV_FONT_DEFAULT, 0);
    lv_obj_t *days_grid = lv_calendar_get_btnmatrix(calendar_object);
    lv_obj_set_style_pad_all(days_grid, 0, 0);
    lv_obj_set_style_pad_row(days_grid, 0, 0);
    lv_obj_set_style_pad_column(days_grid, 0, 0);
    lv_obj_set_style_text_font(days_grid, zh_font() ? zh_font() : LV_FONT_DEFAULT, 0);
    static const char *names[] = {"日", "一", "二", "三", "四", "五", "六"};
    lv_calendar_set_day_names(calendar_object, names);
    if (clock_valid) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        lv_calendar_set_today_date(calendar_object, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    }
    lv_calendar_set_month_shown(calendar_object, calendar_year, calendar_month);
    lv_label_set_text_fmt(calendar_title, "%04d / %02d", calendar_year, calendar_month);
    lv_obj_add_event_cb(calendar_object, calendar_selected, LV_EVENT_VALUE_CHANGED, NULL);
}

static double sine_degrees(double value) { return sin(value * 0.017453292519943295); }
static double cosine_degrees(double value) { return cos(value * 0.017453292519943295); }
static double tangent_degrees(double value) { return tan(value * 0.017453292519943295); }
static double arcsine_degrees(double value) { return asin(value) * 57.29577951308232; }
static double arccosine_degrees(double value) { return acos(value) * 57.29577951308232; }
static double arctangent_degrees(double value) { return atan(value) * 57.29577951308232; }

static te_expr *compile_expression(const char *expression, double *variable, int *error)
{
    te_variable variables[] = {
        {"x", variable, TE_VARIABLE, NULL},
        {"sin", calculator_degrees ? (const void *)sine_degrees : (const void *)sin, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
        {"cos", calculator_degrees ? (const void *)cosine_degrees : (const void *)cos, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
        {"tan", calculator_degrees ? (const void *)tangent_degrees : (const void *)tan, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
        {"asin", calculator_degrees ? (const void *)arcsine_degrees : (const void *)asin, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
        {"acos", calculator_degrees ? (const void *)arccosine_degrees : (const void *)acos, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
        {"atan", calculator_degrees ? (const void *)arctangent_degrees : (const void *)atan, TE_FUNCTION1 | TE_FLAG_PURE, NULL},
    };
    return te_compile(expression, variables, sizeof(variables) / sizeof(variables[0]), error);
}

static void calculate(lv_event_t *event)
{
    (void)event;
    snprintf(calculator_expression, sizeof(calculator_expression), "%s", lv_textarea_get_text(calculator_input));
    double variable = 0;
    int error = 0;
    te_expr *expression = compile_expression(calculator_expression, &variable, &error);
    if (!expression) { lv_label_set_text_fmt(calculator_result, "表达式错误：%d", error); return; }
    if (calculator_mode != 2) {
        double result = te_eval(expression);
        if (isfinite(result)) { lv_label_set_text_fmt(calculator_result, "= %.10g", result); }
        else { lv_label_set_text(calculator_result, "超出定义域或数值溢出"); }
    } else {
        char *end_minimum;
        char *end_maximum;
        const char *minimum_text = lv_textarea_get_text(graph_minimum);
        const char *maximum_text = lv_textarea_get_text(graph_maximum);
        double minimum = strtod(minimum_text, &end_minimum);
        double maximum = strtod(maximum_text, &end_maximum);
        if (end_minimum == minimum_text || end_maximum == maximum_text ||
            *end_minimum || *end_maximum || !isfinite(minimum) || !isfinite(maximum) || minimum >= maximum ||
            maximum - minimum > 1000000) {
            lv_label_set_text(calculator_result, "X 范围无效");
            te_free(expression);
            return;
        }
        double lower = 0, upper = 0;
        unsigned valid = 0;
        int32_t values[101];
        for (unsigned index = 0; index < 101; ++index) {
            variable = minimum + (maximum - minimum) * index / 100;
            double result = te_eval(expression);
            if (!isfinite(result) || fabs(result) > 1000) { values[index] = LV_CHART_POINT_NONE; continue; }
            values[index] = (int32_t)lround(result * 1000);
            if (!valid || result < lower) { lower = result; }
            if (!valid || result > upper) { upper = result; }
            ++valid;
        }
        if (upper - lower < 0.01) { lower -= 1; upper += 1; }
        lv_chart_set_axis_range(graph_chart, LV_CHART_AXIS_PRIMARY_Y,
                               (int32_t)floor(lower * 1000) - 100, (int32_t)ceil(upper * 1000) + 100);
        for (unsigned index = 0; index < 101; ++index) { lv_chart_set_next_value(graph_chart, graph_series, values[index]); }
        lv_label_set_text_fmt(calculator_result, valid ? "Y: %.2g .. %.2g" : "此区间无有效值", lower, upper);
    }
    te_free(expression);
}

static void calculator_key(lv_event_t *event)
{
    lv_obj_t *matrix = lv_event_get_target_obj(event);
    uint32_t selected = lv_buttonmatrix_get_selected_button(matrix);
    const char *key = lv_buttonmatrix_get_button_text(matrix, selected);
    if (!key) { return; }
    if (strcmp(key, "=") == 0) { calculate(NULL); }
    else if (strcmp(key, "C") == 0) { lv_textarea_set_text(calculator_input, ""); }
    else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) { lv_textarea_delete_char(calculator_input); }
    else { lv_textarea_add_text(calculator_input, key); }
    snprintf(calculator_expression, sizeof(calculator_expression), "%s", lv_textarea_get_text(calculator_input));
}

static void calculator_changed(lv_event_t *event)
{
    snprintf(calculator_expression, sizeof(calculator_expression), "%s", lv_textarea_get_text(calculator_input));
    calculator_mode = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    show_page(CYBER_CALCULATOR);
}

static void angle_changed(lv_event_t *event)
{
    calculator_degrees = lv_dropdown_get_selected(lv_event_get_target_obj(event)) == 1;
}

static void create_calculator(int width, int height)
{
    lv_obj_set_scrollable(content, true);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_t *modes = lv_dropdown_create(content);
    lv_obj_set_pos(modes, 4, 2);
    lv_obj_set_size(modes, width - 88, 34);
    lv_obj_set_style_text_font(modes, zh_font() ? zh_font() : LV_FONT_DEFAULT, 0);
    lv_dropdown_set_options(modes, "标准\n科学\n函数绘图");
    lv_dropdown_set_selected(modes, calculator_mode);
    lv_obj_add_event_cb(modes, calculator_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *angle = lv_dropdown_create(content);
    lv_obj_set_pos(angle, width - 80, 2);
    lv_obj_set_size(angle, 76, 34);
    lv_dropdown_set_options(angle, "RAD\nDEG");
    lv_dropdown_set_selected(angle, calculator_degrees);
    lv_obj_add_event_cb(angle, angle_changed, LV_EVENT_VALUE_CHANGED, NULL);
    calculator_input = input(content, calculator_expression, calculator_mode == 2 ? "sin(x)" : "0", 4, 42, width - 8, 191, false);
    calculator_result = text(content, "", 8, 84, width - 16, 0xFFFF00);
    static const char *standard[] = {"C", "(", ")", LV_SYMBOL_BACKSPACE, "\n", "7", "8", "9", "/", "\n",
        "4", "5", "6", "*", "\n", "1", "2", "3", "-", "\n", "0", ".", "=", "+", ""};
    static const char *scientific[] = {"sin(", "cos(", "tan(", "sqrt(", "\n", "ln(", "log(", "exp(", "fac(", "\n",
        "pi", "e", "^", ",", "\n", "7", "8", "9", "/", "\n", "4", "5", "6", "*", "\n",
        "1", "2", "3", "-", "\n", "0", ".", "(", ")", "\n", "C", LV_SYMBOL_BACKSPACE, "=", "+", ""};
    if (calculator_mode == 2) {
        graph_minimum = input(content, graph_minimum_draft, "X min", 4, 110, (width - 72) / 2, 16, false);
        graph_maximum = input(content, graph_maximum_draft, "X max", (width - 72) / 2 + 8, 110, (width - 72) / 2, 16, false);
        button(content, LV_SYMBOL_REFRESH, width - 52, 110, 48, 36, calculate, 0);
        graph_chart = lv_chart_create(content);
        lv_obj_set_pos(graph_chart, 12, 160);
        lv_obj_set_size(graph_chart, width - 24, height > 250 ? 130 : 110);
        lv_chart_set_type(graph_chart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(graph_chart, 101);
        graph_series = lv_chart_add_series(graph_chart, lv_color_hex(0x00FFFF), LV_CHART_AXIS_PRIMARY_Y);
    } else {
        lv_obj_t *matrix = lv_buttonmatrix_create(content);
        lv_buttonmatrix_set_map(matrix, calculator_mode == 0 ? standard : scientific);
        lv_obj_set_pos(matrix, 4, 112);
        lv_obj_set_size(matrix, width - 8, calculator_mode == 0 ? (height > 270 ? height - 120 : 180) : 272);
        lv_obj_add_event_cb(matrix, calculator_key, LV_EVENT_VALUE_CHANGED, NULL);
    }
    create_keyboard(width, height + 34);
}

static void update_chat_text(void)
{
    chat_text[0] = 0;
    if (!chat_messages) { return; }
    /* 优先显示最新消息，按完整消息预留空间，避免尾部中文被截断。 */
    int start = cJSON_GetArraySize(chat_messages);
    size_t budget = 0;
    while (start > 1) {
        cJSON *body = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(chat_messages, start - 1), "content");
        size_t cost = cJSON_IsString(body) ? strlen(body->valuestring) + 16 : 0;
        if (cost >= sizeof(chat_text) - budget) { break; }
        budget += cost;
        --start;
    }
    for (int index = start; index < cJSON_GetArraySize(chat_messages); ++index) {
        cJSON *message = cJSON_GetArrayItem(chat_messages, index);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(body)) { continue; }
        size_t used = strlen(chat_text);
        snprintf(chat_text + used, sizeof(chat_text) - used, "%s\n%s\n\n",
                 strcmp(role->valuestring, "user") == 0 ? "你" : "AI", body->valuestring);
    }
    if (ai_transcript) { lv_label_set_text(ai_transcript, chat_text); }
}

static bool append_message(const char *role, const char *value)
{
    if (!chat_messages) { return false; }
    cJSON *message = cJSON_CreateObject();
    if (!message) { return false; }
    if (!cJSON_AddStringToObject(message, "role", role) ||
        !cJSON_AddStringToObject(message, "content", value) ||
        !cJSON_AddItemToArray(chat_messages, message)) {
        cJSON_Delete(message);
        return false;
    }
    return true;
}

static void ai_configure(lv_event_t *event)
{
    (void)event;
    ai_settings_open = !ai_settings_open;
    show_page(CYBER_AI);
}

static void ai_save(lv_event_t *event)
{
    (void)event;
    const char *endpoint = lv_textarea_get_text(ai_endpoint);
    const char *model = lv_textarea_get_text(ai_model);
    const char *key = lv_textarea_get_text(ai_key);
    if (strncmp(endpoint, "https://", 8) != 0 || !model[0] || !key[0] || strchr(key, '\r') || strchr(key, '\n')) {
        notify("需要有效 HTTPS 接口、模型与密钥", -2);
        return;
    }
    snprintf(settings.endpoint, sizeof(settings.endpoint), "%s", endpoint);
    snprintf(settings.model, sizeof(settings.model), "%s", model);
    snprintf(settings.api_key, sizeof(settings.api_key), "%s", key);
    if (!services_ready || !cyber_settings_save(&settings)) { notify("保存队列不可用", -2); return; }
    ai_settings_open = false;
    show_page(CYBER_AI);
}

static void ai_send(lv_event_t *event)
{
    (void)event;
    if (chat_pending) { lv_label_set_text(ai_status, "请等待当前回复"); return; }
    if (!connected) { lv_label_set_text(ai_status, "WiFi 未连接"); return; }
    const char *prompt = lv_textarea_get_text(ai_prompt);
    if (!prompt[0]) { return; }
    if (!append_message("user", prompt)) { lv_label_set_text(ai_status, "内存不足"); return; }
    char *payload = cJSON_PrintUnformatted(chat_messages);
    bool accepted = payload && services_ready && cyber_chat_start(&settings, payload);
    cJSON_free(payload);
    if (!accepted) {
        cJSON_DeleteItemFromArray(chat_messages, cJSON_GetArraySize(chat_messages) - 1);
        lv_label_set_text(ai_status, "请先配置接口，或等待当前请求完成");
        return;
    }
    chat_pending = true;
    lv_textarea_set_text(ai_prompt, "");
    prompt_draft[0] = 0;
    lv_obj_set_hidden(editor_keyboard, true);
    lv_obj_set_height(content, lv_obj_get_height(root) - lv_obj_get_y(content));
    lv_obj_set_scrollable(content, false);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    lv_label_set_text(ai_status, "等待回复...");
    update_chat_text();
}

static void ai_clear(lv_event_t *event)
{
    (void)event;
    if (chat_pending) { lv_label_set_text(ai_status, "请等待当前回复后清空"); return; }
    while (cJSON_GetArraySize(chat_messages) > 1) { cJSON_DeleteItemFromArray(chat_messages, 1); }
    update_chat_text();
}

static void create_ai(int width, int height)
{
    button(root, LV_SYMBOL_SETTINGS, width - 36, 2, 32, 28, ai_configure, 0);
    if (!settings.endpoint[0] || !settings.model[0]) { ai_settings_open = true; }
    if (ai_settings_open) {
        lv_obj_set_scrollable(content, true);
        lv_obj_set_scroll_dir(content, LV_DIR_VER);
        text(content, "HTTPS 接口", 8, 4, width - 16, 0xC6C6C6);
        ai_endpoint = input(content, endpoint_draft, "https://.../v1/chat/completions", 8, 28, width - 16, 191, false);
        text(content, "模型", 8, 72, width - 16, 0xC6C6C6);
        ai_model = input(content, model_draft, "model", 8, 96, width - 16, 63, false);
        text(content, "API key", 8, 140, width - 16, 0xC6C6C6);
        ai_key = input(content, key_draft, "", 8, 164, width - 16, 191, true);
        button(content, LV_SYMBOL_SAVE, width - 64, 214, 56, 38, ai_save, 0);
    } else {
        lv_obj_t *history = lv_obj_create(content);
        lv_obj_set_pos(history, 4, 4);
        lv_obj_set_size(history, width - 8, height - 100);
        lv_obj_set_style_bg_color(history, lv_color_hex(0x080C10), 0);
        lv_obj_set_style_pad_all(history, 6, 0);
        ai_transcript = text(history, "", 0, 0, width - 36, 0xFFFFFF);
        lv_label_set_long_mode(ai_transcript, LV_LABEL_LONG_WRAP);
        ai_prompt = input(content, prompt_draft, "消息", 4, height - 88, width - 60, 384, false);
        button(content, LV_SYMBOL_RIGHT, width - 52, height - 88, 48, 36, ai_send, 0);
        ai_status = text(content, "", 8, height - 42, width - 64, 0x00FFFF);
        button(content, LV_SYMBOL_TRASH, width - 44, height - 44, 36, 30, ai_clear, 0);
        update_chat_text();
    }
    create_keyboard(width, height + 34);
}

static void poll_chat(void)
{
    bool busy = false;
    if (cyber_chat_poll(chat_response, sizeof(chat_response), &busy)) {
        chat_pending = false;
        bool appended = append_message("assistant", chat_response);
        /* 以完整问答对裁剪上下文，保留系统消息及最近四轮。 */
        while (cJSON_GetArraySize(chat_messages) > 9) {
            cJSON_DeleteItemFromArray(chat_messages, 1);
            cJSON_DeleteItemFromArray(chat_messages, 1);
        }
        update_chat_text();
        if (ai_status) { lv_label_set_text(ai_status, appended ? "" : "回复保存失败：内存不足"); }
    }
}

static void refresh_tools(void)
{
    int64_t now = esp_timer_get_time();
    countdown_t *timers[] = {&countdown, &pomodoro};
    for (unsigned index = 0; index < 2; ++index) {
        countdown_t *timer = timers[index];
        if (!timer->running || now < timer->deadline) { continue; }
        timer->running = false;
        timer->remaining = 0;
        if (index == 1) {
            if (!break_period) { ++sessions; }
            break_period = !break_period;
            timer->remaining = timer->total = (break_period ? 300LL : 1500LL) * 1000000;
            notify(break_period ? "专注结束，休息 5 分钟" : "休息结束，开始下一轮", -1);
        } else { notify("倒计时结束", -1); }
    }
    if (clock_valid && !notification) {
        time_t wall = time(NULL);
        struct tm local;
        localtime_r(&wall, &local);
        int64_t token = ((int64_t)(local.tm_year + 1900) * 366 + local.tm_yday) * 1440 +
                        local.tm_hour * 60 + local.tm_min;
        for (unsigned index = 0; index < 3; ++index) {
            cyber_alarm_t *alarm = &settings.alarms[index];
            unsigned day = (unsigned)(local.tm_wday + 6) % 7;
            if (!alarm->enabled || alarm_fired[index] == token || alarm->hour != local.tm_hour ||
                alarm->minute != local.tm_min || (alarm->days && !(alarm->days & (1U << day)))) { continue; }
            alarm_fired[index] = token;
            if (!alarm->days) {
                alarm->enabled = false;
                alarm_drafts[index].enabled = false;
                cyber_settings_save(&settings);
            }
            char caption[48];
            snprintf(caption, sizeof(caption), "闹钟 %u  %02u:%02u", index + 1, alarm->hour, alarm->minute);
            notify(caption, (int)index);
            break;
        }
    }
    if (notification) {
        int64_t duration = ringing_alarm == -2 ? 3000000 : 90000000;
        if (now - notification_started > duration) { dismiss_notification(NULL); }
        else if (ringing_alarm >= 0 && now - last_ring >= 5000000) {
            const char *path = settings.alarms[ringing_alarm].ringtone;
            if (!touch_sound_music_busy()) {
                touch_sound_alarm_volume(settings.alarms[ringing_alarm].volume);
                touch_sound_alarm_start(path);
            }
            last_ring = now;
        }
    }
    if (!timer_value) { return; }
    char value[192];
    bool running;
    if (page == CYBER_STOPWATCH) {
        int64_t centiseconds = elapsed(now) / 10000;
        snprintf(value, sizeof(value), "%02lld:%02lld.%02lld", (long long)(centiseconds / 6000),
                 (long long)(centiseconds / 100 % 60), (long long)(centiseconds % 100));
        lv_label_set_text(timer_value, value);
        value[0] = 0;
        for (unsigned index = 0; index < 5 && index < lap_count; ++index) {
            int64_t lap = laps[index] / 10000;
            snprintf(value + strlen(value), sizeof(value) - strlen(value), "#%u  %02lld:%02lld.%02lld\n",
                     lap_count - index, (long long)(lap / 6000), (long long)(lap / 100 % 60), (long long)(lap % 100));
        }
        lv_label_set_text(lap_list, value);
        running = stopwatch_running;
        lv_label_set_text(timer_status, running ? "计时中" : "已暂停");
    } else {
        countdown_t *timer = page == CYBER_POMODORO ? &pomodoro : &countdown;
        int64_t left = remaining(timer, now);
        int64_t seconds = (left + 999999) / 1000000;
        snprintf(value, sizeof(value), "%02lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
        lv_label_set_text(timer_value, value);
        lv_bar_set_value(timer_progress, (int32_t)((timer->total - left) * 1000 / timer->total), LV_ANIM_OFF);
        snprintf(value, sizeof(value), "%s  %u", page == CYBER_POMODORO ?
                 (break_period ? "休息" : "专注") : "倒计时", page == CYBER_POMODORO ? sessions : 0);
        lv_label_set_text(timer_status, value);
        running = timer->running;
    }
    lv_label_set_text(lv_obj_get_child(timer_toggle, 0), running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void tool_timer_tick(lv_timer_t *timer)
{
    (void)timer;
    refresh_tools();
    poll_chat();
}

static void create_clock(int width, int height)
{
    /* 横屏将动画移到时间右侧，温湿度并列，避免压住天气和日期。 */
    bool landscape = width > height;
    lv_obj_set_style_bg_color(content, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *top = lv_image_create(content);
    lv_image_set_src(top, &cyber_watch_top);
    lv_obj_set_pos(top, (width - 240) / 2, 0);
    lv_obj_t *bottom = lv_image_create(content);
    lv_image_set_src(bottom, &cyber_watch_bottom);
    lv_obj_set_pos(bottom, (width - 240) / 2, height - 20);
    text(content, WEATHER_CITY_ZH, 8, 28, width - 58, 0x000000);
    button(content, LV_SYMBOL_LIST, width - 42, 24, 36, 28, navigate, CYBER_DESKTOP);
    make_shape(content, 0, 56, width, 1, 0x000000, 0);
    make_shape(content, 0, height - (landscape ? 52 : 64), width, 1, 0x000000, 0);
    clock_time = make_label(content, "--:--", landscape ? 8 : width / 2 - 105, 62, 166,
                            &lv_font_montserrat_48, 0x000000);
    lv_obj_set_style_text_align(clock_time, LV_TEXT_ALIGN_CENTER, 0);
    clock_seconds = make_label(content, "--", landscape ? 178 : width / 2 + 64, 80, 48,
                               &lv_font_montserrat_28, 0x000000);
    lv_obj_t *astronaut = lv_animimg_create(content);
    lv_animimg_set_src(astronaut, cyber_animation, 10);
    lv_animimg_set_duration(astronaut, 300);
    lv_animimg_set_repeat_count(astronaut, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_pos(astronaut, landscape ? width - 78 : width / 2 - 72,
                   landscape ? 62 : height / 2 - 14);
    lv_animimg_start(astronaut);
    lv_obj_t *temperature = lv_image_create(content);
    lv_image_set_src(temperature, &cyber_temperature);
    lv_obj_set_pos(temperature, landscape ? 12 : width - 102, landscape ? 136 : height / 2 - 12);
    lv_obj_t *humidity = lv_image_create(content);
    lv_image_set_src(humidity, &cyber_humidity);
    lv_obj_set_pos(humidity, landscape ? width / 2 : width - 102, landscape ? 136 : height / 2 + 22);
    clock_temperature = text(content, "-- C", landscape ? 40 : width - 74,
                             landscape ? 138 : height / 2 - 10, 70, 0x000000);
    clock_humidity = text(content, "--%", landscape ? width / 2 + 28 : width - 74,
                          landscape ? 138 : height / 2 + 24, 70, 0x000000);
    clock_date = text(content, "", 8, height - (landscape ? 46 : 54), width - 16, 0x000000);
    lv_obj_set_style_text_align(clock_date, LV_TEXT_ALIGN_CENTER, 0);
    clock_weather = text(content, "天气加载中", 8, height - (landscape ? 74 : 86), width - 16, 0x000000);
    lv_obj_set_style_text_align(clock_weather, LV_TEXT_ALIGN_CENTER, 0);
}

static void create_desktop(int width, int height)
{
    const cyber_page_t apps[] = {CYBER_ALARM, CYBER_POMODORO, CYBER_CALCULATOR,
                                 CYBER_AI, CYBER_CLOCK, CYBER_STOPWATCH,
                                 CYBER_COUNTDOWN, CYBER_CALENDAR};
    const char *const symbols[] = {LV_SYMBOL_BELL, LV_SYMBOL_LOOP, LV_SYMBOL_EDIT,
                                   LV_SYMBOL_KEYBOARD, LV_SYMBOL_HOME, LV_SYMBOL_PLAY,
                                   LV_SYMBOL_REFRESH, LV_SYMBOL_LIST};
    const uint32_t colors[] = {0xFFA500, 0xFF4040, 0x4090FF, 0xFF40FF,
                               0xFFFFFF, 0x00FFFF, 0xFFFF00, 0xFFA500};
    int cell_width = width / 3;
    int cell_height = (height - 8) / 3;
    for (unsigned index = 0; index < sizeof(apps) / sizeof(apps[0]); ++index) {
        int left = (int)(index % 3) * cell_width;
        int top = (int)(index / 3) * cell_height;
        lv_obj_t *tile = button(content, "", left + 2, top + 2, cell_width - 4,
                                cell_height - 4, navigate, apps[index]);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_t *icon = make_label(tile, symbols[index], 0, cell_height < 80 ? 2 : 10, cell_width - 8,
                                    &lv_font_montserrat_28, colors[index]);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *name = text(tile, apps[index] == CYBER_CLOCK ? "时钟" : page_titles[apps[index]],
                      0, cell_height - 28, cell_width - 8, 0xFFFFFF);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void show_page(cyber_page_t next)
{
    /* 转屏和字库就绪都会重建页面；先收集草稿，保存按钮才修改生效配置。 */
    if (page == CYBER_ALARM && alarm_ringtone) { capture_alarm(); }
    if (page == CYBER_CALCULATOR && calculator_input) {
        snprintf(calculator_expression, sizeof(calculator_expression), "%s", lv_textarea_get_text(calculator_input));
        if (graph_minimum) {
            snprintf(graph_minimum_draft, sizeof(graph_minimum_draft), "%s", lv_textarea_get_text(graph_minimum));
            snprintf(graph_maximum_draft, sizeof(graph_maximum_draft), "%s", lv_textarea_get_text(graph_maximum));
        }
    }
    if (page == CYBER_AI) {
        if (ai_endpoint) {
            snprintf(endpoint_draft, sizeof(endpoint_draft), "%s", lv_textarea_get_text(ai_endpoint));
            snprintf(model_draft, sizeof(model_draft), "%s", lv_textarea_get_text(ai_model));
            snprintf(key_draft, sizeof(key_draft), "%s", lv_textarea_get_text(ai_key));
        }
        if (ai_prompt) { snprintf(prompt_draft, sizeof(prompt_draft), "%s", lv_textarea_get_text(ai_prompt)); }
    }
    page = next;
    lv_obj_clean(root);
    clock_time = clock_date = clock_weather = NULL;
    clock_seconds = clock_temperature = clock_humidity = NULL;
    timer_value = timer_status = timer_progress = timer_toggle = lap_list = NULL;
    editor_keyboard = NULL;
    ai_transcript = ai_status = NULL;
    ai_endpoint = ai_model = ai_key = ai_prompt = NULL;
    alarm_ringtone = calculator_input = NULL;
    graph_minimum = graph_maximum = NULL;
    int width = lv_display_get_horizontal_resolution(lv_obj_get_display(root));
    int height = lv_display_get_vertical_resolution(lv_obj_get_display(root));
    int header = next == CYBER_CLOCK ? 0 : 34;
    lv_obj_set_size(root, width, height);
    if (header) {
        button(root, LV_SYMBOL_LEFT, 4, 2, 32, 28, navigate,
               next == CYBER_DESKTOP ? CYBER_CLOCK : CYBER_DESKTOP);
        text(root, page_titles[next], 44, 6, width - 52, 0x00FFFF);
        make_shape(root, 0, 32, width, 1, 0x424242, 0);
    }
    content = lv_obj_create(root);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, header);
    lv_obj_set_size(content, width, height - header);
    lv_obj_set_scrollable(content, false);
    lv_obj_set_style_bg_color(content, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    if (next == CYBER_CLOCK) { create_clock(width, height); }
    else if (next == CYBER_DESKTOP) { create_desktop(width, height - header); }
    else if (next == CYBER_ALARM) { create_alarm(width, height - header); }
    else if (next == CYBER_STOPWATCH || next == CYBER_POMODORO || next == CYBER_COUNTDOWN) {
        create_timer(width, height - header);
    }
    else if (next == CYBER_CALENDAR) { create_calendar(width, height - header); }
    else if (next == CYBER_CALCULATOR) { create_calculator(width, height - header); }
    else if (next == CYBER_AI) { create_ai(width, height - header); }
    applied_font = zh_font();
}

void cyber_tools_create(lv_obj_t *screen)
{
    services_ready = cyber_services_init(&settings);
    memcpy(alarm_drafts, settings.alarms, sizeof(alarm_drafts));
    snprintf(endpoint_draft, sizeof(endpoint_draft), "%s", settings.endpoint);
    snprintf(model_draft, sizeof(model_draft), "%s", settings.model);
    snprintf(key_draft, sizeof(key_draft), "%s", settings.api_key);
    chat_messages = cJSON_CreateArray();
    append_message("system", "You are a helpful assistant. Please answer in Chinese.");
    if (time(NULL) > 1704067200) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        calendar_year = local.tm_year + 1900;
        calendar_month = local.tm_mon + 1;
        calendar_initialized = true;
    }
    /* 保留天气后台的内部控件引用，新界面接管可见时钟页。 */
    for (uint32_t index = 0; index < lv_obj_get_child_count(screen); ++index) {
        lv_obj_set_hidden(lv_obj_get_child(screen, index), true);
    }
    root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    show_page(CYBER_CLOCK);
    lv_timer_create(tool_timer_tick, 50, NULL);
}

void cyber_tools_resize(void) { if (root) { show_page(page); } }
bool cyber_tools_is_child(void) { return root && page != CYBER_CLOCK; }
void cyber_tools_home(void) { if (root) { show_page(page == CYBER_DESKTOP ? CYBER_CLOCK : CYBER_DESKTOP); } }

bool cyber_tools_allow_sleep(void)
{
    if (!root) { return true; }
    bool active = countdown.running || pomodoro.running || stopwatch_running || chat_pending || notification;
    for (unsigned index = 0; index < 3; ++index) { active |= settings.alarms[index].enabled != 0; }
    if (active) { notify("请先关闭闹钟并停止计时、等待 AI 完成", -2); }
    return !active;
}

void cyber_tools_weather(const weather_data_t *weather, bool success)
{
    if (success) { weather_cache = *weather; weather_valid = true; }
}

void cyber_tools_tick(bool online, bool time_valid)
{
    connected = online;
    clock_valid = time_valid;
    if (!root) { return; }
    /* 冷启动时可能尚未校时，首次同步后再初始化日历月份。 */
    if (clock_valid && !calendar_initialized) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        calendar_year = local.tm_year + 1900;
        calendar_month = local.tm_mon + 1;
        calendar_initialized = true;
        if (page == CYBER_CALENDAR) { show_page(page); }
    }
    if (applied_font != zh_font()) { show_page(page); }
    if (!clock_time) { return; }
    char value[96];
    if (clock_valid) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(value, sizeof(value), "%H:%M", &local);
        lv_label_set_text(clock_time, value);
        strftime(value, sizeof(value), "%S", &local);
        lv_label_set_text(clock_seconds, value);
        static const char *const weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(value, sizeof(value), "%s  %02d月%02d日", weekdays[local.tm_wday], local.tm_mon + 1, local.tm_mday);
        lv_label_set_text(clock_date, value);
    } else {
        lv_label_set_text(clock_time, "--:--");
        lv_label_set_text(clock_date, connected ? "同步时间中" : "请配置 WiFi");
    }
    if (weather_valid) {
        lv_label_set_text(clock_weather, weather_service_description_zh(weather_cache.code));
        lv_label_set_text_fmt(clock_temperature, "%.1f C", weather_cache.temperature);
        lv_label_set_text_fmt(clock_humidity, "%d%%", weather_cache.humidity);
    }
}