#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "weather_clock.h"
#include "weather_clock_config.h"
#include "nvs.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "ui_status_bar.h"
#include "display_port.h"
#include "ui_helpers.h"
#include "weather_service.h"
#include "touch_controls.h"
#include "power_manager.h"
#include "touch_sound.h"
#include "tf_card.h"
#include "entertainment_view.h"
#include "zh_font.h"
#include "lunar.h"
#include "cyber_assets.h"

#define NETWORK_READY BIT0
#define TIME_READY BIT1

static const char *TAG = "weather_clock";
/* 太空人沿用原始 70x70 十帧图像，横竖屏均占用温度右侧的固定槽位。 */
#define WEATHER_ICON_WIDTH 70
#define WEATHER_ICON_HEIGHT 70
/* 温度文字与图标槽位之间的固定间距，横竖屏保持一致。 */
#define WEATHER_ICON_GAP 10
/* 中文星期，用于日期行，避免 strftime %a 输出英文缩写。 */
static const char *const weekday_names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
static const char *const weekday_names_en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static EventGroupHandle_t network_events;
static lv_display_t *display;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *weekday_label;
static lv_obj_t *lunar_label;
static lv_obj_t *city_label;
static int cached_weather_code = -1;
static int cached_humidity = -1;
static lv_obj_t *temperature_label;
static lv_obj_t *condition_label;
static lv_obj_t *humidity_label;
static lv_obj_t *weather_status_label;
static lv_obj_t *weather_icon_slot;
static lv_obj_t *weather_card;
static lv_obj_t *ride_speed_band;
static lv_obj_t *ride_unit_label;
static lv_obj_t *ride_metric_titles[6];
static lv_obj_t *ride_dividers[3];
static int64_t last_weather_update;
static bool weather_failed;
static SemaphoreHandle_t state_mutex;
static bool ride_mode;
static bool media_mode;
static bool light_theme;
/* 传感器不可用时，各页共用持久化的四向旋转状态，避免刷新或重启回正。 */
static lv_display_rotation_t manual_rotation = LV_DISPLAY_ROTATION_0;
static ride_model_t ride;
static lv_obj_t *desk_screen;
static lv_obj_t *ride_screen;
static lv_obj_t *media_screen;
static lv_obj_t *boot_overlay;
static lv_obj_t *ride_speed_label;
static lv_obj_t *ride_avg_label;
static lv_obj_t *ride_trip_label;
static lv_obj_t *ride_elapsed_label;
static lv_obj_t *ride_altitude_label;
static lv_obj_t *ride_state_label;
static lv_obj_t *ride_clock_label;
static lv_obj_t *ride_sensor_label;
static lv_obj_t *ride_grade_label;
static lv_obj_t *ride_acceleration_label;
static ui_status_bar_t status_bars[3];
static ui_status_t device_status;
static int64_t previous_gnss_us;
static float previous_gnss_speed;
static QueueHandle_t touch_commands;
static TaskHandle_t touch_command_task_handle;

/* LVGL 回调持有界面锁，只投递静态命令；工作任务沿用状态锁再界面锁的顺序。 */
static bool submit_touch_command(const char *command)
{
    return touch_commands && xQueueSend(touch_commands, &command, 0) == pdTRUE;
}

static void touch_command_task(void *argument)
{
    (void)argument;
    const char *command;
    for (;;) {
        if (xQueueReceive(touch_commands, &command, portMAX_DELAY) == pdTRUE) {
            if (!weather_clock_command(command)) { ESP_LOGW(TAG, "Touch command rejected: %s", command); }
        }
    }
}

static lv_obj_t *selected_screen(void)
{
    return media_mode ? media_screen : (ride_mode ? ride_screen : desk_screen);
}

static unsigned selected_page(void)
{
    return media_mode ? 1 : (ride_mode ? 2 : 0);
}

static void boot_line_width(void *object, int32_t width)
{
    lv_obj_set_width(object, width);
    lv_obj_align(object, LV_ALIGN_CENTER, 0, 20);
}

static void boot_logo_opacity(void *object, int32_t opacity)
{
    lv_obj_set_style_opa(object, opacity, 0);
}

static void boot_finished(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    lv_obj_delete(boot_overlay);
    boot_overlay = NULL;
    touch_controls_set_blocked(false);
    ESP_LOGI(TAG, "Boot animation finished");
}

/* 顶层遮罩不改变已保存的目标页；冷启动和触摸深睡唤醒共用同一动画。 */
static void show_boot_animation(void)
{
    touch_controls_set_blocked(true);
    boot_overlay = make_shape(lv_display_get_layer_top(display), 0, 0,
                              LV_PCT(100), LV_PCT(100), 0x080F0D, 0);
    lv_obj_set_scrollable(boot_overlay, false);
    lv_obj_t *line = make_shape(boot_overlay, 0, 0, 2, 2, 0x8CE2BE, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t *logo = make_label(boot_overlay, "L-Vectra", 0, 0, 220,
                                &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_style_text_align(logo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, line);
    lv_anim_set_exec_cb(&animation, boot_line_width);
    lv_anim_set_values(&animation, 2, 168);
    lv_anim_set_duration(&animation, 450);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
    lv_anim_set_var(&animation, logo);
    lv_anim_set_exec_cb(&animation, boot_logo_opacity);
    lv_anim_set_values(&animation, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&animation, 450);
    lv_anim_set_duration(&animation, 500);
    lv_anim_start(&animation);
    lv_timer_t *timer = lv_timer_create(boot_finished, 1800, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

/* 适配 X-TRACK Dialplate 的大速度区和四项统计布局，使用 LVGL9 原生接口。 */
static void create_ride_ui(void)
{
    ride_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ride_screen, lv_color_hex(0x111111), 0);
    lv_obj_set_scrollable(ride_screen, false);
    make_label(ride_screen, "RIDE", 12, 8, 46, LV_FONT_DEFAULT, 0xFFB04A);
    ride_clock_label = make_label(ride_screen, "--:--", 63, 8, 43, &lv_font_montserrat_12, 0xCCCCCC);
    ride_speed_band = make_shape(ride_screen, 0, 31, 240, 74, 0x333333, 0);
    ride_speed_label = make_label(ride_screen, "--", 10, 29, 220, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_align(ride_speed_label, LV_TEXT_ALIGN_CENTER, 0);
    ride_unit_label = make_label(ride_screen, "km/h", 10, 83, 220, &lv_font_montserrat_12, 0xCCCCCC);
    lv_obj_set_style_text_align(ride_unit_label, LV_TEXT_ALIGN_CENTER, 0);
    ride_dividers[0] = make_shape(ride_screen, 119, 110, 1, 108, 0x444444, 0);
    ride_dividers[1] = make_shape(ride_screen, 12, 146, 216, 1, 0x444444, 0);
    ride_dividers[2] = make_shape(ride_screen, 12, 183, 216, 1, 0x444444, 0);
    ride_metric_titles[0] = make_label(ride_screen, "AVG km/h", 12, 109, 105, &lv_font_montserrat_12, 0xAAAAAA);
    ride_avg_label = make_label(ride_screen, "--", 12, 124, 105, LV_FONT_DEFAULT, 0xFFFFFF);
    ride_metric_titles[1] = make_label(ride_screen, "TIME", 132, 109, 102, &lv_font_montserrat_12, 0xAAAAAA);
    ride_elapsed_label = make_label(ride_screen, "00:00:00", 132, 124, 102, LV_FONT_DEFAULT, 0xFFFFFF);
    ride_metric_titles[2] = make_label(ride_screen, "TRIP km", 12, 149, 105, &lv_font_montserrat_12, 0xAAAAAA);
    ride_trip_label = make_label(ride_screen, "--", 12, 164, 105, LV_FONT_DEFAULT, 0xFFFFFF);
    ride_metric_titles[3] = make_label(ride_screen, "ALT m", 132, 149, 102, &lv_font_montserrat_12, 0xAAAAAA);
    ride_altitude_label = make_label(ride_screen, "--", 132, 164, 102, LV_FONT_DEFAULT, 0xFFFFFF);
    ride_metric_titles[4] = make_label(ride_screen, "GRADE %", 12, 186, 105, &lv_font_montserrat_12, 0xAAAAAA);
    ride_grade_label = make_label(ride_screen, "--", 12, 201, 105, LV_FONT_DEFAULT, 0x8CE2BE);
    ride_metric_titles[5] = make_label(ride_screen, "ACC m/s2", 132, 186, 102, &lv_font_montserrat_12, 0xAAAAAA);
    ride_acceleration_label = make_label(ride_screen, "--", 132, 201, 102, LV_FONT_DEFAULT, 0x8CE2BE);
    ride_state_label = make_label(ride_screen, LV_SYMBOL_PAUSE " PAUSED", 12, 224, 112,
                                  &lv_font_montserrat_12, 0xFFB04A);
    ride_sensor_label = make_label(ride_screen, "NO SENSOR", 134, 224, 100,
                                   &lv_font_montserrat_12, 0xAAAAAA);
}

/* A8 动画只存线条透明度，切换配色不重启动画，也不产生独立背景块。 */
static void apply_clock_theme(void)
{
    static bool applied;
    static bool applied_light;
    if (applied && applied_light == light_theme) { return; }
    lv_color_t background = lv_color_hex(light_theme ? 0xFFFFFF : 0x000000);
    lv_color_t foreground = lv_color_hex(light_theme ? 0x000000 : 0xFFFFFF);
    lv_color_t secondary = lv_color_hex(light_theme ? 0x555555 : 0xBBBBBB);
    lv_obj_set_style_bg_color(desk_screen, background, 0);
    lv_obj_set_style_bg_color(weather_card, background, 0);
    lv_obj_t *primary_labels[] = {city_label, time_label, temperature_label, condition_label};
    lv_obj_t *secondary_labels[] = {date_label, weekday_label, lunar_label, humidity_label, weather_status_label};
    for (unsigned index = 0; index < sizeof(primary_labels) / sizeof(primary_labels[0]); ++index) {
        lv_obj_set_style_text_color(primary_labels[index], foreground, 0);
    }
    for (unsigned index = 0; index < sizeof(secondary_labels) / sizeof(secondary_labels[0]); ++index) {
        lv_obj_set_style_text_color(secondary_labels[index], secondary, 0);
    }
    lv_obj_set_style_bg_opa(weather_icon_slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_image_recolor(weather_icon_slot, foreground, 0);
    lv_obj_set_style_image_recolor_opa(weather_icon_slot, LV_OPA_COVER, 0);
    ui_status_bar_set_monochrome(&status_bars[0], foreground);
    touch_controls_set_light_theme(light_theme);
    applied_light = light_theme;
    applied = true;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    desk_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101918), 0);
    lv_obj_set_scrollable(screen, false);
    /* 页头：左侧城市，右上角状态图标；日期与星期同一行，农历单独一行。 */
    city_label = make_label(screen, WEATHER_CITY, 14, 8, 150, LV_FONT_DEFAULT, 0x8CE2BE);
    date_label = make_label(screen, "---- -- --", 14, 34, 120, LV_FONT_DEFAULT, 0xC1CBC9);
    weekday_label = make_label(screen, "--", 122, 34, 104, LV_FONT_DEFAULT, 0xC1CBC9);
    lv_obj_set_style_text_align(weekday_label, LV_TEXT_ALIGN_RIGHT, 0);
    lunar_label = make_label(screen, "", 14, 58, 212, LV_FONT_DEFAULT, 0xC1CBC9);
    lv_obj_set_style_text_align(lunar_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 48 px 时间："12:34:56" 约 184 px，横竖屏都能完整显示。 */
    time_label = make_label(screen, "--:--:--", 0, 84, 240, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 天气卡片把图标、温度、描述和湿度收成一组，与日期/时间分开。 */
    weather_card = make_shape(screen, 14, 150, 212, 126, 0x172321, 8);
    lv_obj_set_clickable(weather_card, false);
    lv_obj_set_scrollable(weather_card, false);
    temperature_label = make_label(screen, "-- C", 14, 171, 100, &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_width(temperature_label, LV_SIZE_CONTENT);
    condition_label = make_label(screen, "Weather pending", 14, 230, 212, LV_FONT_DEFAULT, 0xFFFFFF);
    lv_obj_set_style_text_align(condition_label, LV_TEXT_ALIGN_CENTER, 0);
    humidity_label = make_label(screen, "Humidity: --%", 14, 252, 212, LV_FONT_DEFAULT, 0xB2C0BC);
    lv_obj_set_style_text_align(humidity_label, LV_TEXT_ALIGN_CENTER, 0);
    weather_status_label = make_label(screen, "Open-Meteo | UTC+8", 14, 286, 212, &lv_font_montserrat_12, 0x8CA49B);
    lv_obj_set_style_text_align(weather_status_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 仅替换图标区域，时间、农历、天气数据及状态栏仍使用原天气钟面。 */
    weather_icon_slot = lv_animimg_create(screen);
    lv_obj_set_size(weather_icon_slot, WEATHER_ICON_WIDTH, WEATHER_ICON_HEIGHT);
    lv_obj_set_clickable(weather_icon_slot, false);
    lv_obj_set_scrollable(weather_icon_slot, false);
    lv_animimg_set_src(weather_icon_slot, cyber_animation, 10);
    lv_animimg_set_duration(weather_icon_slot, 300);
    lv_animimg_set_repeat_count(weather_icon_slot, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(weather_icon_slot);
    ESP_LOGI(TAG, "LCD stage: weather page ready; creating ride page");
    create_ride_ui();
    ESP_LOGI(TAG, "LCD stage: ride page ready; creating system page");
    media_screen = entertainment_view_create();
    ui_status_bar_create(&status_bars[0], desk_screen, false);
    ui_status_bar_create(&status_bars[1], ride_screen, true);
    ui_status_bar_create(&status_bars[2], media_screen, true);
    apply_clock_theme();
    /* 传感器异常留在诊断页显示，不覆盖用户保存的启动标签页。 */
    ESP_LOGI(TAG, "LCD stage: pages ready; loading initial screen");
    lv_screen_load(selected_screen());
    ESP_LOGI(TAG, "LCD stage: initial screen loaded");
}

/* 页面用满显示区域；时钟页按“页头/日期行/大号时间/天气卡片/来源”分区，
 * 横屏时间占满整行，天气卡片横向铺开；码表指标由两列三行切换为三列两行。 */
/* 温度与图标槽位同一行：槽位左边缘固定为“温度文字右边缘 + WEATHER_ICON_GAP”，
 * 竖直方向以温度文字的实际中心为准；槽位只在布局时定位，不随帧切换移动。 */
static void align_weather_icons(void)
{
    if (!display || !temperature_label || !weather_icon_slot) { return; }
    /* 温度宽度随数字位数变化，先按内容定宽再测量，图标才能贴住文字。 */
    lv_obj_set_width(temperature_label, LV_SIZE_CONTENT);
    lv_obj_update_layout(temperature_label);
    int32_t temperature_width = lv_obj_get_width(temperature_label);
    int32_t temperature_height = lv_obj_get_height(temperature_label);
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    bool landscape = width > height;

    /* 温度行中心：横屏在天气卡片内偏上，竖屏在卡片内居中。 */
    int32_t row_center = landscape ? 162 : 186;
    /* 横屏温度靠卡片左侧，右侧留给天气描述；竖屏“温度 + 间距 + 图标槽位”整组居中。 */
    int32_t group_width = temperature_width + WEATHER_ICON_GAP + WEATHER_ICON_WIDTH;
    int32_t temperature_x = landscape ? 24 : (width - group_width + 1) / 2;
    int32_t temperature_y = row_center - temperature_height / 2;
    int32_t icon_x = temperature_x + temperature_width + WEATHER_ICON_GAP;
    /* 由温度文字实际中心反推槽位 y，而不是再写死一个行中心。 */
    int32_t icon_y = temperature_y + temperature_height / 2 - WEATHER_ICON_HEIGHT / 2;
    lv_obj_set_pos(temperature_label, temperature_x, temperature_y);
    lv_obj_set_pos(weather_icon_slot, icon_x, icon_y);
    /* 横屏为较宽的负温度留出空间，长天气文字单行滚动，不压住动画或湿度。 */
    if (landscape) {
        int32_t details_x = icon_x + WEATHER_ICON_WIDTH + WEATHER_ICON_GAP;
        if (details_x < 202) { details_x = 202; }
        place_label(condition_label, details_x, 144, width - details_x - 14);
        place_label(humidity_label, details_x, 167, width - details_x - 14);
    }
    lv_label_set_long_mode(condition_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_long_mode(humidity_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

static void center_ui(void)
{
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    bool landscape = width > height;
    lv_obj_t *screens[] = {desk_screen, ride_screen, media_screen};
    for (unsigned index = 0; index < 3; ++index) {
        lv_obj_set_style_pad_all(screens[index], 0, 0);
        lv_obj_set_style_border_width(screens[index], 0, 0);
    }
    /* 时钟页：页头 → 日期行 → 大号时间 → 天气卡片 → 数据来源。 */
    place_label(city_label, 14, 8, landscape ? 140 : 150);
    place_label(date_label, 14, 34, 120);
    place_label(weekday_label, 122, 34, landscape ? 44 : 104);
    lv_obj_set_style_text_align(weekday_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (landscape) {
        /* 横屏把农历移到日期行右侧，给大号时间腾出一整行。 */
        place_label(lunar_label, 172, 34, width - 186);
        lv_obj_set_style_text_align(lunar_label, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        place_label(lunar_label, 14, 58, width - 28);
        lv_obj_set_style_text_align(lunar_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    place_label(time_label, 0, landscape ? 60 : 84, width);

    if (landscape) {
        /* 横屏：天气卡片横向铺开，图标+温度在左，描述与湿度在右。 */
        lv_obj_set_pos(weather_card, 14, 126);
        lv_obj_set_size(weather_card, width - 28, 76);
        place_label(condition_label, 202, 144, width - 216);
        place_label(humidity_label, 202, 167, width - 216);
        lv_obj_set_style_text_align(condition_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(humidity_label, LV_TEXT_ALIGN_LEFT, 0);
    } else {
        /* 竖屏：天气卡片居中，图标+温度成组居中，描述与湿度各占一行。 */
        lv_obj_set_pos(weather_card, 14, 150);
        lv_obj_set_size(weather_card, width - 28, 126);
        place_label(condition_label, 14, 230, width - 28);
        place_label(humidity_label, 14, 252, width - 28);
        lv_obj_set_style_text_align(condition_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(humidity_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    /* 底部留出翻页圆点位置：来源行上移，圆点淡出时不压住文字。 */
    place_label(weather_status_label, 14, height - 34, width - 28);
    align_weather_icons();

    lv_obj_set_pos(ride_speed_band, 0, landscape ? 30 : 36);
    lv_obj_set_size(ride_speed_band, width, landscape ? 66 : 86);
    place_label(ride_speed_label, 10, landscape ? 28 : 38, width - 20);
    place_label(ride_unit_label, 10, landscape ? 80 : 96, width - 20);
    unsigned columns = landscape ? 3 : 2;
    unsigned rows = landscape ? 2 : 3;
    int grid_top = landscape ? 102 : 134;
    int row_height = (height - 82 - grid_top) / (int)rows;
    int column_width = width / (int)columns;
    lv_obj_t *values[] = {ride_avg_label, ride_elapsed_label, ride_trip_label,
                         ride_altitude_label, ride_grade_label, ride_acceleration_label};
    for (unsigned index = 0; index < 6; ++index) {
        int left = (int)(index % columns) * column_width + 12;
        int top = grid_top + (int)(index / columns) * row_height;
        place_label(ride_metric_titles[index], left, top, column_width - 20);
        place_label(values[index], left, top + 14, column_width - 20);
    }
    lv_obj_set_pos(ride_dividers[0], column_width - 1, grid_top);
    lv_obj_set_size(ride_dividers[0], 1, row_height * (int)rows);
    for (unsigned index = 1; index < 3; ++index) {
        lv_obj_set_hidden(ride_dividers[index], landscape && index == 2);
        lv_obj_set_pos(ride_dividers[index], landscape ? 2 * column_width - 1 : 12,
                       landscape ? grid_top : grid_top + (int)index * row_height - 2);
        lv_obj_set_size(ride_dividers[index], landscape ? 1 : width - 24,
                        landscape ? row_height * (int)rows : 1);
    }
    place_label(ride_state_label, 12, height - 76, width / 2 - 12);
    place_label(ride_sensor_label, width / 2 + 8, height - 76, width / 2 - 20);
}

static void create_display_ui(lv_display_t *created_display)
{
    display = created_display;
    board_hardware_snapshot_t hardware = {0};
    board_hardware_snapshot(&hardware);
    /* 创建页面前恢复方向，开机动画也沿用用户保存的横竖屏。 */
    if (!hardware.imu_valid) { lv_display_set_rotation(display, manual_rotation); }
    zh_font_init();
    create_ui();
    center_ui();
    touch_controls_create(display, ride_screen, submit_touch_command);
    touch_controls_set_light_theme(light_theme);
    touch_controls_update(selected_page(), ride.running);
    show_boot_animation();
}

/* 中文字库已加载时用中文，否则回退英文，避免中文显示为方框。 */
static const char *localize(const char *zh, const char *en)
{
    return zh_font() ? zh : en;
}

static void render_condition(void)
{
    if (cached_weather_code < 0) {
        lv_label_set_text(condition_label, localize("天气加载中", "Weather pending"));
        return;
    }
    lv_label_set_text(condition_label,
        localize(weather_service_description_zh(cached_weather_code),
                 weather_service_description(cached_weather_code)));
}

static void render_humidity(void)
{
    if (cached_humidity < 0) {
        lv_label_set_text(humidity_label, localize("湿度：--%", "Humidity: --%"));
        return;
    }
    lv_label_set_text_fmt(humidity_label,
        localize("湿度：%d%%", "Humidity: %d%%"), cached_humidity);
}

/* 中文字库加载或释放后，把时钟页标签切换到对应字体并刷新文案。
 * 以字体指针作为“已应用”标记：U 盘模式会释放字体，退出后重新加载，
 * 指针变化即触发再次应用。 */
static void apply_zh_font(void)
{
    static const lv_font_t *applied_font;
    const lv_font_t *font = zh_font();
    if (font == applied_font) { return; }
    const lv_font_t *label_font = font ? font : LV_FONT_DEFAULT;
    lv_obj_set_style_text_font(city_label, label_font, 0);
    lv_obj_set_style_text_font(date_label, label_font, 0);
    lv_obj_set_style_text_font(weekday_label, label_font, 0);
    lv_obj_set_style_text_font(lunar_label, label_font, 0);
    lv_obj_set_style_text_font(condition_label, label_font, 0);
    lv_obj_set_style_text_font(humidity_label, label_font, 0);
    lv_obj_set_style_text_font(weather_status_label, label_font, 0);
    lv_label_set_text(city_label, localize(WEATHER_CITY_ZH, WEATHER_CITY));
    render_condition();
    render_humidity();
    applied_font = font;
}

static void show_weather(const weather_data_t *weather, bool success)
{
    if (!display || !lvgl_port_lock(1000)) { return; }
    weather_failed = !success;
    if (success) {
        last_weather_update = esp_timer_get_time();
        cached_weather_code = weather->code;
        cached_humidity = weather->humidity;
        char temperature_text[24];
        snprintf(temperature_text, sizeof(temperature_text), "%.1f C", weather->temperature);
        lv_label_set_text(temperature_label, temperature_text);
        render_condition();
        render_humidity();
        /* 只调整槽位位置；天气刷新不能重新启动或打断太空人动画。 */
        align_weather_icons();
    }
    lvgl_port_unlock();
}

static void weather_task(void *argument)
{
    for (;;) {
        /* 天气只需要网络；时间同步失败时仍应尝试 HTTP 回退，而不是
         * 让界面永远停在 “Weather pending”。 */
        xEventGroupWaitBits(network_events, NETWORK_READY,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        weather_data_t weather = {0};
        esp_err_t result = weather_service_fetch(&weather);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "%s: %.1f C, %s, humidity %d%% (Open-Meteo)", WEATHER_CITY,
                     weather.temperature, weather_service_description(weather.code), weather.humidity);
        } else {
            ESP_LOGW(TAG, "Weather request failed: %s; retry in 60 s", esp_err_to_name(result));
        }
        show_weather(&weather, result == ESP_OK);
        int wait_seconds = result == ESP_OK ? WEATHER_REFRESH_SECONDS : 60;
        for (int second = 0; second < wait_seconds; ++second) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!(xEventGroupGetBits(network_events) & NETWORK_READY)) { break; }
        }
    }
}

esp_err_t weather_clock_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) { return ESP_ERR_NO_MEM; }
    nvs_handle_t storage;
    if (nvs_open("device_ui", NVS_READONLY, &storage) == ESP_OK) {
        uint8_t saved_mode = 0;
        if (nvs_get_u8(storage, "mode", &saved_mode) == ESP_OK) {
            ride_mode = saved_mode == 1;
            /* 兼容旧版 IMU 标签，恢复原来的启动页记忆。 */
            media_mode = saved_mode == 2 || saved_mode == 3;
        }
        uint8_t saved_rotation = 0;
        if (nvs_get_u8(storage, "rotation", &saved_rotation) == ESP_OK &&
            saved_rotation <= LV_DISPLAY_ROTATION_270) {
            manual_rotation = (lv_display_rotation_t)saved_rotation;
        }
        uint8_t saved_theme = 0;
        if (nvs_get_u8(storage, "light", &saved_theme) == ESP_OK && saved_theme <= 1) {
            light_theme = saved_theme == 1;
        }
        nvs_close(storage);
    }
    network_events = xEventGroupCreate();
    if (!network_events) { return ESP_ERR_NO_MEM; }
    touch_commands = xQueueCreate(8, sizeof(const char *));
    if (!touch_commands) { return ESP_ERR_NO_MEM; }
    if (xTaskCreate(touch_command_task, "touch_commands", 4096, NULL, 2,
                    &touch_command_task_handle) != pdPASS) {
        vQueueDelete(touch_commands);
        touch_commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = tf_card_init();
    if (result != ESP_OK) { ESP_LOGW(TAG, "TF task unavailable: %s", esp_err_to_name(result)); }
    result = display_port_init(&display, create_display_ui);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed: %s; serial clock continues", esp_err_to_name(result));
    }
    if (xTaskCreate(weather_task, "weather", 6144, NULL, 2, NULL) != pdPASS) {
        vTaskDelete(touch_command_task_handle);
        touch_command_task_handle = NULL;
        vQueueDelete(touch_commands);
        touch_commands = NULL;
        vEventGroupDelete(network_events);
        network_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 锁顺序固定为状态锁再 LVGL 锁；NVS 只保存模式，不保存虚构的骑行数据。 */
bool weather_clock_command(const char *command)
{
    if (strcmp(command, "sound test") == 0) { return touch_sound_test(); }
    if (strcmp(command, "music start") == 0) { return touch_sound_music_start(); }
    if (strcmp(command, "music stop") == 0) { touch_sound_music_stop(); return true; }
    if (strncmp(command, "brightness ", 11) == 0) {
        char *end = NULL;
        unsigned long value = strtoul(command + 11, &end, 10);
        if (end == command + 11 || *end != '\0' || value > 100) { return false; }
        display_port_brightness_set((unsigned)value);
        display_port_brightness_save();
        printf("Brightness: %u%%\n", (unsigned)value);
        return true;
    }
    if (strcmp(command, "mag status") == 0) {
        board_hardware_snapshot_t hardware = {0};
        bool ready = board_hardware_snapshot(&hardware);
        printf("MAG snapshot=%d addr=0x%02X ACK=%d ID=0x%02X init=%s ready=%d read=%s status=0x%02X valid=%d XYZ=%+.4f,%+.4f,%+.4f age_ms=%lld\n",
               ready, HW_MMC5983_ADDRESS, hardware.magnetic_ack, hardware.magnetic_product_id,
               esp_err_to_name(hardware.magnetic_init_result), hardware.magnetic_initialized,
               esp_err_to_name(hardware.magnetic_read_result), hardware.magnetic_status,
               hardware.magnetic_valid, hardware.magnetic_gauss[0], hardware.magnetic_gauss[1],
               hardware.magnetic_gauss[2], hardware.magnetic_us ?
               (long long)((esp_timer_get_time() - hardware.magnetic_us) / 1000) : -1LL);
        return true;
    }
    if (!state_mutex || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    bool handled = true;
    bool save_mode = false;
    bool open_imu = false;
    bool rotate_display = false;
    lv_display_rotation_t requested_rotation = (lv_display_rotation_t)((manual_rotation + 1) % 4);
    bool save_rotation = false;
    bool save_theme = false;
    bool enter_sleep = strcmp(command, "power sleep") == 0;
    lv_screen_load_anim_t transition = LV_SCREEN_LOAD_ANIM_NONE;
    if (enter_sleep) {
        printf("Entering touch-wake sleep. Tap the screen three times to wake.\n");
    } else if (strcmp(command, "page next") == 0 || strcmp(command, "page prev") == 0) {
        int page = selected_page();
        bool forward = strcmp(command, "page next") == 0;
        int next_page = page + (forward ? 1 : -1);
        /* 一级页固定为 Clock/System/Ride，传感器只从系统二级菜单进入。 */
        if (next_page < 0 || next_page > 2) {
            xSemaphoreGive(state_mutex);
            return true;
        }
        const char *commands[] = {"mode desk", "mode system", "mode ride"};
        command = commands[next_page];
        transition = forward ? LV_SCREEN_LOAD_ANIM_MOVE_RIGHT : LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
    }
    if (enter_sleep) {
        /* 睡眠动作在状态锁释放后执行，不能让 LVGL 回调路径持锁进入深睡。 */
    } else if (strcmp(command, "mode desk") == 0 || strcmp(command, "mode ride") == 0 ||
             strcmp(command, "mode media") == 0 || strcmp(command, "mode system") == 0) {
         media_mode = strcmp(command, "mode media") == 0 || strcmp(command, "mode system") == 0;
        bool next_ride = strcmp(command, "mode ride") == 0;
        if (!next_ride) { ride_model_pause(&ride, now); previous_gnss_us = 0; }
        ride_mode = next_ride;
        save_mode = true;
        printf("Mode: %s\n", media_mode ? "system" : (ride_mode ? "ride" : "desk"));
    } else if (strcmp(command, "mode imu") == 0) {
        open_imu = true;
        media_mode = true;
        ride_mode = false;
        save_mode = true;
        ride_model_pause(&ride, now);
        previous_gnss_us = 0;
        printf("MPU6050 diagnostics; ride timer paused.\n");
    } else if (strcmp(command, "display rotate") == 0) {
        rotate_display = true;
    } else if (strncmp(command, "display rotate ", 15) == 0) {
        char *end = NULL;
        unsigned long degrees = strtoul(command + 15, &end, 10);
        /* 只接受四个正交方向，非法角度不能写入 NVS 或改变显示。 */
        if (end == command + 15 || *end != '\0' || degrees > 270 || degrees % 90 != 0) {
            handled = false;
        } else {
            requested_rotation = (lv_display_rotation_t)(degrees / 90);
            rotate_display = true;
        }
    } else if (strcmp(command, "clock theme light") == 0 || strcmp(command, "clock theme dark") == 0) {
        light_theme = strcmp(command, "clock theme light") == 0;
        save_theme = true;
    } else if (strcmp(command, "ride start") == 0) {
        if (ride_mode) {
            if (!ride.running) { previous_gnss_us = 0; }
            ride_model_start(&ride, now);
            printf("Ride timer started. Readings require a valid sensor fix.\n");
        } else { printf("Select mode ride first.\n"); }
    } else if (strcmp(command, "ride pause") == 0) {
        ride_model_pause(&ride, now);
        previous_gnss_us = 0;
        printf("Ride timer paused.\n");
    } else if (strcmp(command, "ride reset") == 0) {
        ride_model_reset(&ride);
        previous_gnss_us = 0;
        printf("Ride reset; timer stopped.\n");
    } else if (strcmp(command, "status") == 0) {
        long long seconds = ride_model_elapsed(&ride, now) / 1000000;
        printf("Mode: %s | ride: %s | elapsed: %lld s | sensor: %s\n", media_mode ? "system" : (ride_mode ? "ride" : "desk"),
               ride.running ? "running" : "paused", seconds,
               ride_model_sample_fresh(&ride, now) ? "valid" : "unavailable");
    } else {
        handled = false;
    }
    if (save_mode) {
        nvs_handle_t storage;
        esp_err_t result = nvs_open("device_ui", NVS_READWRITE, &storage);
        if (result == ESP_OK) {
            result = nvs_set_u8(storage, "mode", media_mode ? 2 : (ride_mode ? 1 : 0));
            if (result == ESP_OK) { result = nvs_commit(storage); }
            nvs_close(storage);
        }
        if (result != ESP_OK) { printf("Mode not saved: %s\n", esp_err_to_name(result)); }
    }
    if (handled && !enter_sleep && display && lvgl_port_lock(1000)) {
        apply_clock_theme();
        board_hardware_snapshot_t hardware = {0};
        board_hardware_snapshot(&hardware);
        /* 再次核对读数，避免按钮点击排队期间传感器恢复后仍覆盖自动朝向。 */
        if (rotate_display && !hardware.imu_valid) {
            save_rotation = manual_rotation != requested_rotation;
            manual_rotation = requested_rotation;
        }
        touch_controls_set_manual_rotation(!hardware.imu_valid, manual_rotation);
        lv_display_rotation_t rotation = !hardware.imu_valid ? manual_rotation :
            ((ride_mode || media_mode) ? LV_DISPLAY_ROTATION_0 : lv_display_get_rotation(display));
        if (lv_display_get_rotation(display) != rotation) {
            lv_display_set_rotation(display, rotation);
            center_ui();
        }
        if (transition != LV_SCREEN_LOAD_ANIM_NONE) {
            /* 保留三页对象供反向翻页复用，不让动画自动销毁旧页。 */
            lv_screen_load_anim(selected_screen(), transition, 220, 0, false);
        } else if (lv_display_get_screen_active(display) != selected_screen()) {
            lv_screen_load(selected_screen());
        }
        if (save_mode) {
            if (open_imu) { entertainment_view_open_imu(); }
            else { entertainment_view_home(); }
        }
        touch_controls_update(selected_page(), ride.running);
        lvgl_port_unlock();
    }
    /* Flash 提交不占用界面锁；状态锁保证连续点击按顺序保存。 */
    if (save_theme) {
        nvs_handle_t storage;
        esp_err_t result = nvs_open("device_ui", NVS_READWRITE, &storage);
        if (result == ESP_OK) {
            result = nvs_set_u8(storage, "light", light_theme ? 1 : 0);
            if (result == ESP_OK) { result = nvs_commit(storage); }
            nvs_close(storage);
        }
        if (result != ESP_OK) { ESP_LOGW(TAG, "Theme not saved: %s", esp_err_to_name(result)); }
    }
    if (save_rotation) {
        nvs_handle_t storage;
        esp_err_t result = nvs_open("device_ui", NVS_READWRITE, &storage);
        if (result == ESP_OK) {
            result = nvs_set_u8(storage, "rotation", (uint8_t)manual_rotation);
            if (result == ESP_OK) { result = nvs_commit(storage); }
            nvs_close(storage);
        }
        if (result != ESP_OK) { ESP_LOGW(TAG, "Rotation not saved: %s", esp_err_to_name(result)); }
    }
    xSemaphoreGive(state_mutex);
    if (enter_sleep) {
        display_port_prepare_sleep();
        power_manager_enter_sleep();
    }
    return handled;
}

bool weather_clock_battery_status(bool valid, unsigned percent, bool charging)
{
    if (valid && percent > 100) { return false; }
    if (!state_mutex || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }
    device_status.battery_valid = valid;
    device_status.battery_percent = percent;
    device_status.battery_charging = charging;
    xSemaphoreGive(state_mutex);
    return true;
}

bool weather_clock_bluetooth_status(bool connected)
{
    if (!state_mutex || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }
    device_status.bluetooth_connected = connected;
    xSemaphoreGive(state_mutex);
    return true;
}

bool weather_clock_ride_sample(const ride_sample_t *sample)
{
    if (!state_mutex || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }
    bool valid = ride_model_sample(&ride, sample, esp_timer_get_time());
    xSemaphoreGive(state_mutex);
    return valid;
}

/* GNSS 速度积分仅累计运行区间；定位中断或暂停后重新建立基准，不补算缺口。 */
static void consume_gnss(const board_hardware_snapshot_t *hardware)
{
    if (!HW_ATGM336H_ENABLED) { return; }
    if (!hardware->gnss_valid) {
        previous_gnss_us = 0;
        ride_model_sample(&ride, NULL, esp_timer_get_time());
        return;
    }
    float trip = ride.sample.trip_km;
    float speed = hardware->speed_kmh >= 1.5f ? hardware->speed_kmh : 0;
    if (ride.running && previous_gnss_us && hardware->gnss_us > previous_gnss_us) {
        int64_t interval = hardware->gnss_us - previous_gnss_us;
        if (interval <= 2500000) {
            trip += (previous_gnss_speed + speed) * 0.5f * interval / 3600000000.0f;
        }
    }
    previous_gnss_us = ride.running && hardware->gnss_us >= ride.started_us ? hardware->gnss_us : 0;
    previous_gnss_speed = speed;
    ride_sample_t sample = {
        .valid = true,
        .speed_kmh = hardware->speed_kmh,
        .trip_km = trip,
        .altitude_valid = hardware->altitude_valid,
        .altitude_m = hardware->altitude_m,
    };
    ride_model_sample(&ride, &sample, hardware->gnss_us);
}

void weather_clock_tick(bool online, bool time_valid)
{
    if (!network_events) { return; }
    if (online) { xEventGroupSetBits(network_events, NETWORK_READY); }
    else { xEventGroupClearBits(network_events, NETWORK_READY); }
    if (time_valid) { xEventGroupSetBits(network_events, TIME_READY); }
    else { xEventGroupClearBits(network_events, TIME_READY); }
    board_hardware_snapshot_t hardware = {0};
    board_hardware_snapshot(&hardware);
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) { return; }
    consume_gnss(&hardware);
    if (!display) { xSemaphoreGive(state_mutex); return; }
    if (!lvgl_port_lock(100)) { xSemaphoreGive(state_mutex); return; }
    apply_zh_font();
    touch_controls_set_manual_rotation(!hardware.imu_valid, manual_rotation);
    /* 无传感器时各页沿用手动方向；恢复读数后仅时钟页自动旋转。 */
    lv_display_rotation_t rotation = boot_overlay ? lv_display_get_rotation(display) : LV_DISPLAY_ROTATION_0;
    if (!boot_overlay && !hardware.imu_valid) {
        rotation = manual_rotation;
    } else if (HW_AUTO_ROTATE_ENABLED && !ride_mode && !media_mode && !boot_overlay && hardware.imu_valid && hardware.rotation_valid) {
        if (hardware.rotation_degrees == 90) { rotation = LV_DISPLAY_ROTATION_90; }
        else if (hardware.rotation_degrees == 180) { rotation = LV_DISPLAY_ROTATION_180; }
        else if (hardware.rotation_degrees == 270) { rotation = LV_DISPLAY_ROTATION_270; }
    } else if (!ride_mode && !media_mode && !boot_overlay) {
        rotation = lv_display_get_rotation(display);
    }
    if (lv_display_get_rotation(display) != rotation) {
        lv_display_set_rotation(display, rotation);
        center_ui();
    }
    if (lv_display_get_screen_active(display) != selected_screen() &&
        lv_display_get_screen_loading(display) != selected_screen()) {
        lv_screen_load(selected_screen());
    }
    touch_controls_update(selected_page(), ride.running);
    if (ride_mode) {
        int64_t now = esp_timer_get_time();
        long long seconds = ride_model_elapsed(&ride, now) / 1000000;
        char elapsed[32];
        snprintf(elapsed, sizeof(elapsed), "%02lld:%02lld:%02lld",
             seconds / 3600, (seconds / 60) % 60, seconds % 60);
        lv_label_set_text(ride_elapsed_label, elapsed);
        lv_label_set_text(ride_state_label, ride.running ? LV_SYMBOL_PLAY " RUNNING" : LV_SYMBOL_PAUSE " PAUSED");
        bool fresh = ride_model_sample_fresh(&ride, now);
        lv_label_set_text(ride_sensor_label, fresh ? (HW_ATGM336H_ENABLED ? "GNSS OK" : "SENSOR OK") :
                  (HW_ATGM336H_ENABLED ? "NO FIX" : "NO SENSOR"));
        char text[32];
        if (hardware.imu_valid && hardware.ride_grade_valid) {
            snprintf(text, sizeof(text), "%+.1f", hardware.ride_grade_percent);
            lv_label_set_text(ride_grade_label, text);
        } else { lv_label_set_text(ride_grade_label, "--"); }
        if (hardware.imu_valid && hardware.ride_acceleration_valid) {
            snprintf(text, sizeof(text), "%+.2f", hardware.ride_acceleration_mps2);
            lv_label_set_text(ride_acceleration_label, text);
        } else { lv_label_set_text(ride_acceleration_label, "--"); }
        if (fresh) {
            snprintf(text, sizeof(text), "%.1f", ride.sample.speed_kmh);
            lv_label_set_text(ride_speed_label, text);
            snprintf(text, sizeof(text), "%.2f", ride.sample.trip_km);
            lv_label_set_text(ride_trip_label, text);
            if (seconds > 0) {
                snprintf(text, sizeof(text), "%.1f", ride.sample.trip_km * 3600.0 / seconds);
                lv_label_set_text(ride_avg_label, text);
            } else { lv_label_set_text(ride_avg_label, "--"); }
            if (ride.sample.altitude_valid) {
                snprintf(text, sizeof(text), "%.0f", ride.sample.altitude_m);
                lv_label_set_text(ride_altitude_label, text);
            } else { lv_label_set_text(ride_altitude_label, "--"); }
        } else {
            lv_label_set_text(ride_speed_label, "--");
            lv_label_set_text(ride_trip_label, "--");
            lv_label_set_text(ride_avg_label, "--");
            lv_label_set_text(ride_altitude_label, "--");
        }
        if (time_valid) {
            time_t wall_time = time(NULL);
            struct tm local_time;
            localtime_r(&wall_time, &local_time);
            strftime(text, sizeof(text), "%H:%M", &local_time);
            lv_label_set_text(ride_clock_label, text);
        } else { lv_label_set_text(ride_clock_label, "--:--"); }
    }
    device_status.wifi_connected = online;
    device_status.gps_fixed = hardware.gnss_valid;
    device_status.tf_ready = entertainment_view_card_ready();
    for (unsigned index = 0; index < 3; ++index) {
        if (index == 0) { apply_clock_theme(); }
        ui_status_bar_update(&status_bars[index], &device_status);
    }
    if (time_valid) {
        time_t now = time(NULL);
        struct tm local_time;
        char text[40];
        localtime_r(&now, &local_time);
        strftime(text, sizeof(text), "%H:%M:%S", &local_time);
        /* 时分秒同一排、同大小。 */
        lv_label_set_text(time_label, text);
        /* 日期与周几分行显示。 */
        snprintf(text, sizeof(text), "%04d-%02d-%02d",
                 local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday);
        lv_label_set_text(date_label, text);
        lv_label_set_text(weekday_label,
            localize(weekday_names[local_time.tm_wday], weekday_names_en[local_time.tm_wday]));
        /* 农历：中文字库加载后显示，否则留空。 */
        char lunar_text[32];
        lunar_format(local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
                     lunar_text, sizeof(lunar_text));
        lv_label_set_text(lunar_label, zh_font() ? lunar_text : "");
    } else {
        /* 时间未同步时把提示放在农历行，日期与星期仍保持两列布局。 */
        lv_label_set_text(date_label, "---- -- --");
        lv_label_set_text(weekday_label, "--");
        lv_label_set_text(lunar_label, online
            ? localize("同步时间中...", "Syncing time...")
            : localize("请配置 WiFi", "Configure WiFi"));
        lv_label_set_text(time_label, "--:--:--");
    }
    bool stale = last_weather_update && esp_timer_get_time() - last_weather_update > 1800LL * 1000000;
    lv_label_set_text(weather_status_label,
        !online ? localize("离线 | 缓存天气", "Offline | cached weather") :
        weather_failed ? localize("天气获取失败 | 重试中", "Weather failed | retrying") :
        stale ? localize("天气数据过期 | Open-Meteo", "Weather stale | Open-Meteo") :
        !last_weather_update ? localize("等待天气数据 | UTC+8", "Waiting for weather | UTC+8")
                             : "Open-Meteo | UTC+8");
    lvgl_port_unlock();
    xSemaphoreGive(state_mutex);
}