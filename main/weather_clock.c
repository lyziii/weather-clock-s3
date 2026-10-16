#include <stdio.h>
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
#include "imu_view.h"
#include "ui_helpers.h"
#include "weather_service.h"
#include "touch_controls.h"

#define NETWORK_READY BIT0
#define TIME_READY BIT1

static const char *TAG = "weather_clock";
static EventGroupHandle_t network_events;
static lv_display_t *display;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *temperature_label;
static lv_obj_t *condition_label;
static lv_obj_t *humidity_label;
static lv_obj_t *weather_status_label;
static lv_obj_t *sun_icon;
static lv_obj_t *cloud_icon;
static lv_obj_t *rain_icon;
static lv_obj_t *snow_icon;
static lv_obj_t *storm_icon;
static lv_obj_t *weather_separator;
static lv_obj_t *ride_speed_band;
static lv_obj_t *ride_unit_label;
static lv_obj_t *ride_metric_titles[6];
static lv_obj_t *ride_dividers[3];
static int64_t last_weather_update;
static bool weather_failed;
static SemaphoreHandle_t state_mutex;
static bool ride_mode;
static bool imu_view;
static ride_model_t ride;
static lv_obj_t *desk_screen;
static lv_obj_t *ride_screen;
static lv_obj_t *imu_screen;
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
    return imu_view ? imu_screen : (ride_mode ? ride_screen : desk_screen);
}

/* 适配 X-TRACK Dialplate 的大速度区和四项统计布局，使用 LVGL9 原生接口。 */
static void create_ride_ui(void)
{
    ride_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ride_screen, lv_color_hex(0x111111), 0);
    lv_obj_set_scrollable(ride_screen, false);
    make_label(ride_screen, "RIDE", 12, 8, 46, LV_FONT_DEFAULT, 0xFFB04A);
    ride_clock_label = make_label(ride_screen, "--:--", 65, 8, 53, &lv_font_montserrat_12, 0xCCCCCC);
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

/* 图标由少量 LVGL 图元组成，不需要解码图片或分配整屏画布。 */
static void create_ui(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    desk_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101918), 0);
    lv_obj_set_scrollable(screen, false);
    make_label(screen, WEATHER_CITY, 14, 8, 106, LV_FONT_DEFAULT, 0x8CE2BE);
    date_label = make_label(screen, "---- -- --", 14, 40, 218, LV_FONT_DEFAULT, 0xC1CBC9);
    time_label = make_label(screen, "--:--", 10, 61, 220, &lv_font_montserrat_48, 0xFFFFFF);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    weather_separator = make_shape(screen, 14, 123, 212, 1, 0x3B504A, 0);
    temperature_label = make_label(screen, "-- C", 14, 138, 139, &lv_font_montserrat_28, 0xFFFFFF);
    condition_label = make_label(screen, "Weather pending", 14, 177, 148, LV_FONT_DEFAULT, 0xFFFFFF);
    humidity_label = make_label(screen, "Humidity: --%", 14, 199, 212, LV_FONT_DEFAULT, 0xB2C0BC);
    weather_status_label = make_label(screen, "Open-Meteo | UTC+8", 14, 222, 212, &lv_font_montserrat_12, 0x8CA49B);
    sun_icon = make_shape(screen, 179, 139, 32, 32, 0xFFD166, LV_RADIUS_CIRCLE);
    cloud_icon = lv_obj_create(screen);
    lv_obj_remove_style_all(cloud_icon);
    lv_obj_set_scrollable(cloud_icon, false);
    lv_obj_set_pos(cloud_icon, 163, 146);
    lv_obj_set_size(cloud_icon, 62, 31);
    make_shape(cloud_icon, 1, 9, 27, 22, 0xDDE7E5, LV_RADIUS_CIRCLE);
    make_shape(cloud_icon, 19, 0, 31, 30, 0xDDE7E5, LV_RADIUS_CIRCLE);
    make_shape(cloud_icon, 40, 12, 22, 19, 0xDDE7E5, LV_RADIUS_CIRCLE);
    make_shape(cloud_icon, 14, 17, 40, 14, 0xDDE7E5, 2);
    rain_icon = lv_obj_create(screen);
    lv_obj_remove_style_all(rain_icon);
    lv_obj_set_scrollable(rain_icon, false);
    lv_obj_set_pos(rain_icon, 172, 179);
    lv_obj_set_size(rain_icon, 46, 12);
    for (int index = 0; index < 3; ++index) {
        make_shape(rain_icon, index * 15, 0, 3, 9, 0x73BEFF, 2);
    }
    snow_icon = make_label(screen, "* * *", 171, 174, 51, LV_FONT_DEFAULT, 0xDDE7E5);
    storm_icon = make_label(screen, LV_SYMBOL_CHARGE, 181, 172, 30, LV_FONT_DEFAULT, 0xFFD166);
    lv_obj_set_hidden(sun_icon, true);
    lv_obj_set_hidden(cloud_icon, true);
    lv_obj_set_hidden(rain_icon, true);
    lv_obj_set_hidden(snow_icon, true);
    lv_obj_set_hidden(storm_icon, true);
    ESP_LOGI(TAG, "LCD stage: weather page ready; creating ride page");
    create_ride_ui();
    ESP_LOGI(TAG, "LCD stage: ride page ready; creating IMU page");
    imu_screen = imu_view_create(display);
    ui_status_bar_create(&status_bars[0], desk_screen, false);
    ui_status_bar_create(&status_bars[1], ride_screen, true);
    ui_status_bar_create(&status_bars[2], imu_screen, false);
    board_hardware_snapshot_t hardware;
    board_hardware_snapshot(&hardware);
    if (HW_MPU6050_ENABLED && hardware.initialization_result != ESP_OK) { imu_view = true; }
    ESP_LOGI(TAG, "LCD stage: pages ready; loading initial screen");
    lv_screen_load(selected_screen());
    ESP_LOGI(TAG, "LCD stage: initial screen loaded");
}

/* 页面用满显示区域；横屏天气改为双栏，码表指标由两列三行切换为三列两行。 */
static void center_ui(void)
{
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    bool landscape = width > height;
    lv_obj_t *screens[] = {desk_screen, ride_screen, imu_screen};
    for (unsigned index = 0; index < 3; ++index) {
        lv_obj_set_style_pad_all(screens[index], 0, 0);
        lv_obj_set_style_border_width(screens[index], 0, 0);
    }
    int weather_left = landscape ? width / 2 + 16 : 14;
    int weather_width = landscape ? width / 2 - 28 : width - 28;
    int temperature_top = landscape ? 52 : height * 55 / 100;
    int icon_top = landscape ? 90 : temperature_top;
    place_label(date_label, 14, landscape ? 50 : height * 15 / 100,
                landscape ? width / 2 - 24 : width - 28);
    place_label(time_label, 10, landscape ? 88 : height / 4,
                landscape ? width / 2 - 20 : width - 20);
    lv_obj_set_pos(weather_separator, landscape ? width / 2 : 14,
                   landscape ? 44 : height * 47 / 100);
    lv_obj_set_size(weather_separator, landscape ? 1 : width - 28,
                    landscape ? height - 74 : 1);
    place_label(temperature_label, weather_left, temperature_top,
                landscape ? weather_width : width - 101);
    place_label(condition_label, weather_left, landscape ? 150 : height * 69 / 100,
                landscape ? weather_width : width - 92);
    place_label(humidity_label, weather_left, landscape ? 178 : height * 80 / 100, weather_width);
    place_label(weather_status_label, 14, height - 30, width - 28);
    lv_obj_set_pos(sun_icon, width - 61, icon_top + 1);
    lv_obj_set_pos(cloud_icon, width - 77, icon_top + 8);
    lv_obj_set_pos(rain_icon, width - 68, icon_top + 41);
    lv_obj_set_pos(snow_icon, width - 69, icon_top + 36);
    lv_obj_set_pos(storm_icon, width - 59, icon_top + 34);

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
    imu_view_layout(width, height);
}

static void create_display_ui(lv_display_t *created_display)
{
    display = created_display;
    create_ui();
    center_ui();
    touch_controls_create(display, ride_screen, submit_touch_command);
    touch_controls_update(imu_view ? 2 : (ride_mode ? 1 : 0), ride.running);
}

static void show_weather(const weather_data_t *weather, bool success)
{
    if (!display || !lvgl_port_lock(1000)) { return; }
    weather_failed = !success;
    if (success) {
        last_weather_update = esp_timer_get_time();
        char temperature_text[24];
        snprintf(temperature_text, sizeof(temperature_text), "%.1f C", weather->temperature);
        lv_label_set_text(temperature_label, temperature_text);
        lv_label_set_text(condition_label, weather_service_description(weather->code));
        lv_label_set_text_fmt(humidity_label, "Humidity: %d%%", weather->humidity);
        lv_obj_set_hidden(sun_icon, weather->code > 2);
        lv_obj_set_hidden(cloud_icon, weather->code == 0);
        lv_obj_set_hidden(rain_icon, true);
        lv_obj_set_hidden(snow_icon, true);
        lv_obj_set_hidden(storm_icon, true);
        if ((weather->code >= 71 && weather->code <= 77) || weather->code == 85 || weather->code == 86) {
            lv_obj_set_hidden(snow_icon, false);
        } else if (weather->code >= 95) {
            lv_obj_set_hidden(storm_icon, false);
        } else if (weather->code >= 51) {
            lv_obj_set_hidden(rain_icon, false);
        }
    }
    lvgl_port_unlock();
}

static void weather_task(void *argument)
{
    for (;;) {
        xEventGroupWaitBits(network_events, NETWORK_READY | TIME_READY,
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
    esp_err_t result = display_port_init(&display, create_display_ui);
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
    if (!state_mutex || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    bool handled = true;
    bool save_mode = false;
    lv_screen_load_anim_t transition = LV_SCREEN_LOAD_ANIM_NONE;
    if (strcmp(command, "page next") == 0 || strcmp(command, "page prev") == 0) {
        int page = imu_view ? 2 : (ride_mode ? 1 : 0);
        bool forward = strcmp(command, "page next") == 0;
        int next_page = page + (forward ? 1 : -1);
        /* 页序固定为 Clock/Ride/IMU，首尾不循环，方向反转即可回到原页。 */
        if (next_page < 0 || next_page > 2) {
            xSemaphoreGive(state_mutex);
            return true;
        }
        const char *commands[] = {"mode desk", "mode ride", "mode imu"};
        command = commands[next_page];
        transition = forward ? LV_SCREEN_LOAD_ANIM_MOVE_RIGHT : LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
    }
    if (strcmp(command, "mode desk") == 0 || strcmp(command, "mode ride") == 0) {
        imu_view = false;
        bool next_ride = strcmp(command, "mode ride") == 0;
        if (!next_ride) { ride_model_pause(&ride, now); previous_gnss_us = 0; }
        ride_mode = next_ride;
        save_mode = true;
        printf("Mode: %s\n", ride_mode ? "ride" : "desk");
    } else if (strcmp(command, "mode imu") == 0) {
        imu_view = true;
        ride_model_pause(&ride, now);
        previous_gnss_us = 0;
        printf("MPU6050 diagnostics; ride timer paused.\n");
    } else if (strcmp(command, "ride start") == 0) {
        if (ride_mode && !imu_view) {
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
        printf("Mode: %s | ride: %s | elapsed: %lld s | sensor: %s\n", imu_view ? "imu" : (ride_mode ? "ride" : "desk"),
               ride.running ? "running" : "paused", seconds,
               ride_model_sample_fresh(&ride, now) ? "valid" : "unavailable");
    } else {
        handled = false;
    }
    if (save_mode) {
        nvs_handle_t storage;
        esp_err_t result = nvs_open("device_ui", NVS_READWRITE, &storage);
        if (result == ESP_OK) {
            result = nvs_set_u8(storage, "mode", ride_mode ? 1 : 0);
            if (result == ESP_OK) { result = nvs_commit(storage); }
            nvs_close(storage);
        }
        if (result != ESP_OK) { printf("Mode not saved: %s\n", esp_err_to_name(result)); }
    }
    if (handled && display && lvgl_port_lock(1000)) {
        /* 只有时钟页自动旋转；切出后立即回正，避免码表和诊断页短暂沿用横屏方向。 */
        if ((ride_mode || imu_view) && lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_0) {
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
            center_ui();
        }
        if (transition != LV_SCREEN_LOAD_ANIM_NONE) {
            /* 保留三页对象供反向翻页复用，不让动画自动销毁旧页。 */
            lv_screen_load_anim(selected_screen(), transition, 220, 0, false);
        } else if (lv_display_get_screen_active(display) != selected_screen()) {
            lv_screen_load(selected_screen());
        }
        touch_controls_update(imu_view ? 2 : (ride_mode ? 1 : 0), ride.running);
        lvgl_port_unlock();
    }
    xSemaphoreGive(state_mutex);
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
    /* 姿态只改变摆件的显示方向，不自动改变 desk/ride 模式；码表保持安装方向。 */
    lv_display_rotation_t rotation = LV_DISPLAY_ROTATION_0;
    if (HW_AUTO_ROTATE_ENABLED && !ride_mode && !imu_view && hardware.imu_valid && hardware.rotation_valid) {
        if (hardware.rotation_degrees == 90) { rotation = LV_DISPLAY_ROTATION_90; }
        else if (hardware.rotation_degrees == 180) { rotation = LV_DISPLAY_ROTATION_180; }
        else if (hardware.rotation_degrees == 270) { rotation = LV_DISPLAY_ROTATION_270; }
    } else if (!ride_mode && !imu_view) {
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
    touch_controls_update(imu_view ? 2 : (ride_mode ? 1 : 0), ride.running);
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
    for (unsigned index = 0; index < 3; ++index) {
        ui_status_bar_update(&status_bars[index], &device_status);
    }
    if (time_valid) {
        time_t now = time(NULL);
        struct tm local_time;
        char text[40];
        localtime_r(&now, &local_time);
        strftime(text, sizeof(text), "%H:%M:%S", &local_time);
        /* 主时钟保留 HH:MM，大字号在 240 像素内不裁切；秒显示在日期行。 */
        text[5] = '\0';
        lv_label_set_text(time_label, text);
        strftime(text, sizeof(text), "%Y-%m-%d %a  :%S", &local_time);
        lv_label_set_text(date_label, text);
    } else {
        lv_label_set_text(date_label, online ? "Synchronizing time..." : "Configure WiFi via serial");
    }
    bool stale = last_weather_update && esp_timer_get_time() - last_weather_update > 1800LL * 1000000;
    lv_label_set_text(weather_status_label,
        !online ? "Offline | cached weather" :
        weather_failed ? "Weather failed | retrying" :
        stale ? "Weather stale | Open-Meteo" :
        !last_weather_update ? "Waiting for weather | UTC+8" : "Open-Meteo | UTC+8");
    lvgl_port_unlock();
    xSemaphoreGive(state_mutex);
}