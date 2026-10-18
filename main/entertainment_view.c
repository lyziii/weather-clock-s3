#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "weather_clock_config.h"
#include "imu_view.h"
#include "display_port.h"
#include "touch_sound.h"
#include "entertainment_view.h"
#include "tf_card.h"
#include "wifi_view.h"
#include "zh_font.h"
#include "usb_msc.h"
#include "ui_helpers.h"

static lv_obj_t *screen;
static lv_obj_t *home;
static lv_obj_t *music;
static lv_obj_t *storage;
static lv_obj_t *gyroscope;
static lv_obj_t *magnetometer;
static lv_obj_t *gps;
static lv_obj_t *wifi;
static lv_obj_t *child_title;
static lv_obj_t *magnetic_state;
static lv_obj_t *magnetic_values[3];
static lv_obj_t *magnetic_strength;
static lv_obj_t *magnetic_age;
static lv_obj_t *compass_dial;
static lv_obj_t *compass_heading_label;
static lv_obj_t *magnetic_diagnostic;
static float compass_heading;
static bool compass_valid;
static lv_obj_t *gps_state;
static lv_obj_t *gps_satellites;
static lv_obj_t *gps_latitude;
static lv_obj_t *gps_longitude;
static lv_obj_t *gps_speed;
static lv_obj_t *gps_altitude;
static lv_obj_t *gps_age;
static lv_obj_t *back_button;
static lv_obj_t *capacity_label;
static lv_obj_t *path_label;
static lv_obj_t *file_list;
static lv_obj_t *page_label;
static lv_obj_t *previous_button;
static lv_obj_t *next_button;
static lv_obj_t *up_button;
static lv_obj_t *music_status;
static lv_obj_t *brightness_slider;
static lv_obj_t *brightness_value;
static lv_obj_t *file_detail;
static tf_snapshot_t snapshot;
static tf_snapshot_t incoming;
static unsigned rendered_revision;
static bool child;

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                         int width, int height, lv_event_cb_t callback, void *data)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_radius(control, 4, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x283B36), 0);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_scrollable(control, false);
    lv_obj_t *label = make_label(control, text, 0, 0, width - 12,
                                 &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, data);
    return control;
}

/* 文件列表行：文件名可能含中文，优先用中文字库，未加载时回退 Montserrat。 */
static lv_obj_t *file_button(lv_obj_t *parent, const char *text, int x, int y,
                             int width, int height, lv_event_cb_t callback, void *data)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_radius(control, 4, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x283B36), 0);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_scrollable(control, false);
    const lv_font_t *font = zh_font();
    if (!font) { font = &lv_font_montserrat_12; }
    lv_obj_t *label = make_label(control, text, 0, 0, width - 12, font, 0xFFFFFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, data);
    return control;
}

void entertainment_view_home(void)
{
    child = false;
    lv_obj_set_hidden(home, false);
    lv_obj_set_hidden(music, true);
    lv_obj_set_hidden(storage, true);
    lv_obj_set_hidden(gyroscope, true);
    lv_obj_set_hidden(magnetometer, true);
    lv_obj_set_hidden(gps, true);
    lv_obj_set_hidden(wifi, true);
    lv_obj_set_hidden(child_title, true);
    lv_obj_set_hidden(back_button, true);
}

bool entertainment_view_is_child(void) { return child; }
bool entertainment_view_card_ready(void) { return snapshot.mounted; }

static void show_child(unsigned destination)
{
    if (!destination) { entertainment_view_home(); return; }
    if (destination > 6) { return; }
    const char *titles[] = {"", "MUSIC", "TF CARD", "GYROSCOPE", "MAGNETOMETER", "GPS", "WIFI"};
    child = true;
    lv_obj_set_hidden(home, true);
    lv_obj_set_hidden(back_button, false);
    lv_obj_set_hidden(music, destination != 1);
    lv_obj_set_hidden(storage, destination != 2);
    lv_obj_set_hidden(gyroscope, destination != 3);
    lv_obj_set_hidden(magnetometer, destination != 4);
    lv_obj_set_hidden(gps, destination != 5);
    lv_obj_set_hidden(wifi, destination != 6);
    lv_obj_set_hidden(child_title, false);
    lv_label_set_text(child_title, titles[destination]);
    if (destination == 2) { tf_card_browse("/", 0); }
}

static void navigate(lv_event_t *event)
{
    show_child((unsigned)(uintptr_t)lv_event_get_user_data(event));
}

static void music_play_clicked(lv_event_t *event)
{
    (void)event;
    if (!touch_sound_music_start()) {
        lv_label_set_text(music_status, "Audio unavailable");
    }
}

static void music_stop_clicked(lv_event_t *event)
{
    (void)event;
    touch_sound_music_stop();
}

static void brightness_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    unsigned value = (unsigned)lv_slider_get_value(slider);
    display_port_brightness_set(value);
    if (brightness_value) { lv_label_set_text_fmt(brightness_value, "%u%%", value); }
}

static void brightness_released(lv_event_t *event)
{
    (void)event;
    display_port_brightness_save();
}

/* USB 磁盘模式：点击进入/退出，让电脑直接读写 TF 卡。 */
static void usb_clicked(lv_event_t *event)
{
    (void)event;
    if (usb_msc_active()) {
        usb_msc_stop();
    } else {
        usb_msc_start();
    }
}

void entertainment_view_open_imu(void) { show_child(3); }

static void browse_clicked(lv_event_t *event)
{
    if (!snapshot.mounted || snapshot.busy) { return; }
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    char path[TF_PATH_SIZE];
    snprintf(path, sizeof(path), "%s", snapshot.path);
    unsigned page = snapshot.page;
    if (action == -1 && page) { --page; }
    else if (action == -2 && snapshot.more) { ++page; }
    else if (action == -3) {
        char *separator = strrchr(path, '/');
        if (separator && separator != path) { *separator = '\0'; }
        else { strcpy(path, "/"); }
        page = 0;
    } else if (action == -4) {
        /* 刷新保留当前目录，便于换卡或由电脑写入后重新查看。 */
    } else if (action >= 0 && action < snapshot.count) {
        const tf_entry_t *entry = &snapshot.entries[action];
        if (!entry->directory) {
            lv_label_set_text_fmt(file_detail, "%s | %.2f MiB", entry->name,
                                  entry->size / 1048576.0);
            return;
        }
        int length = snprintf(path, sizeof(path), "%s%s%s", snapshot.path,
                               !strcmp(snapshot.path, "/") ? "" : "/", entry->name);
        if (length < 0 || length >= sizeof(path)) {
            lv_label_set_text(file_detail, "Path too long");
            return;
        }
        page = 0;
    } else { return; }
    tf_card_browse(path, page);
}

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) { lv_obj_remove_state(object, LV_STATE_DISABLED); }
    else { lv_obj_add_state(object, LV_STATE_DISABLED); }
}

/* 指针表示磁北相对屏幕上沿的位置；无有效航向时不绘制指北针。 */
static void draw_compass(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    float center_x = (area.x1 + area.x2) * 0.5f;
    float center_y = (area.y1 + area.y2) * 0.5f;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;
    for (unsigned tick = 0; tick < 24; ++tick) {
        float angle = tick * (6.2831853f / 24);
        float inner_radius = tick % 6 == 0 ? 63 : 69;
        line.color = lv_color_hex(tick % 6 == 0 ? 0x8CE2BE : 0x526B61);
        line.p1 = (lv_point_precise_t){center_x + sinf(angle) * inner_radius,
                                     center_y - cosf(angle) * inner_radius};
        line.p2 = (lv_point_precise_t){center_x + sinf(angle) * 74,
                                     center_y - cosf(angle) * 74};
        lv_draw_line(layer, &line);
    }
    if (!compass_valid) { return; }
    float angle = -compass_heading * (3.14159265f / 180.0f);
    float north_x = sinf(angle);
    float north_y = -cosf(angle);
    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.opa = LV_OPA_COVER;
    triangle.color = lv_color_hex(0xF37870);
    triangle.p[0] = (lv_point_precise_t){center_x + north_x * 43, center_y + north_y * 43};
    triangle.p[1] = (lv_point_precise_t){center_x - north_y * 7, center_y + north_x * 7};
    triangle.p[2] = (lv_point_precise_t){center_x + north_y * 7, center_y - north_x * 7};
    lv_draw_triangle(layer, &triangle);
    triangle.color = lv_color_hex(0xDDE7E5);
    triangle.p[0] = (lv_point_precise_t){center_x - north_x * 37, center_y - north_y * 37};
    lv_draw_triangle(layer, &triangle);
}

/* 航向只用模块水平两轴，不宣称真北、倾斜补偿或硬铁校准。 */
static void update_compass(const board_hardware_snapshot_t *hardware, bool valid)
{
    float forward = hardware->magnetic_gauss[HW_MAG_FORWARD_AXIS] * HW_MAG_FORWARD_SIGN;
    float right = hardware->magnetic_gauss[HW_MAG_RIGHT_AXIS] * HW_MAG_RIGHT_SIGN;
    float total_squared = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        total_squared += hardware->magnetic_gauss[axis] * hardware->magnetic_gauss[axis];
    }
    valid = valid && isfinite(forward) && isfinite(right) &&
            forward * forward + right * right > 0.000025f && total_squared < 1.44f;
    if (valid) {
        float heading = atan2f(-right, forward) * (180.0f / 3.14159265f);
        if (heading < 0) { heading += 360; }
        if (compass_valid) {
            /* 沿最短圆弧平滑，跨越 359/0 度时不让指针绕行一整圈。 */
            float delta = fmodf(heading - compass_heading + 540, 360) - 180;
            compass_heading = fmodf(compass_heading + delta * 0.4f + 360, 360);
        } else { compass_heading = heading; }
        const char *directions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        unsigned degrees = (unsigned)lroundf(compass_heading) % 360;
        unsigned direction = (unsigned)((compass_heading + 22.5f) / 45) % 8;
        lv_label_set_text_fmt(compass_heading_label, "%03u deg %s", degrees, directions[direction]);
    } else {
        lv_label_set_text(compass_heading_label, "-- deg");
        if (hardware->magnetic_valid && total_squared >= 1.44f) {
            lv_label_set_text(magnetic_state, "MAGNETIC DISTURBANCE");
        }
    }
    compass_valid = valid;
    lv_obj_invalidate(compass_dial);
}

static void refresh_sensors(lv_timer_t *timer)
{
    (void)timer;
    if (!lv_obj_is_visible(magnetometer) && !lv_obj_is_visible(gps)) { return; }
    board_hardware_snapshot_t hardware = {0};
    bool ready = board_hardware_snapshot(&hardware);
    int64_t now = esp_timer_get_time();
    if (lv_obj_is_visible(magnetometer)) {
        bool valid = ready && hardware.magnetic_valid;
        lv_label_set_text(magnetic_state, !HW_MMC5983_ENABLED ? "DISABLED" :
            !ready ? "BUSY" : !hardware.magnetic_ack ? "NO ACK AT 0x30" :
            !hardware.magnetic_initialized ? "INITIALIZATION FAILED" :
            valid ? "MAGNETIC | 2D / UNCAL" : "NO DATA / STALE");
        float strength_squared = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (valid) {
                lv_label_set_text_fmt(magnetic_values[axis], "%+.4f G", hardware.magnetic_gauss[axis]);
                strength_squared += hardware.magnetic_gauss[axis] * hardware.magnetic_gauss[axis];
            } else { lv_label_set_text(magnetic_values[axis], "-- G"); }
        }
        if (valid) { lv_label_set_text_fmt(magnetic_strength, "Field %.4f G", sqrtf(strength_squared)); }
        else { lv_label_set_text(magnetic_strength, "Field -- G"); }
        if (ready && hardware.magnetic_us) {
            lv_label_set_text_fmt(magnetic_age, "AGE %lld ms", (long long)((now - hardware.magnetic_us) / 1000));
        } else { lv_label_set_text(magnetic_age, "AGE --"); }
        lv_label_set_text_fmt(magnetic_diagnostic, "0x%02X ID %02X STATUS %02X\n%s | %s",
            HW_MMC5983_ADDRESS, hardware.magnetic_product_id, hardware.magnetic_status,
            esp_err_to_name(hardware.magnetic_init_result), esp_err_to_name(hardware.magnetic_read_result));
        update_compass(&hardware, valid);
    }
    if (lv_obj_is_visible(gps)) {
        bool valid = ready && hardware.gnss_valid;
        lv_label_set_text(gps_state, !HW_ATGM336H_ENABLED ? "DISABLED" :
            !ready ? "BUSY" : valid ? "FIX VALID" : "WAITING / NO FIX");
        if (valid) {
            lv_label_set_text_fmt(gps_satellites, "Satellites %u", hardware.satellites);
            lv_label_set_text_fmt(gps_latitude, "Lat %+.6f", hardware.latitude);
            lv_label_set_text_fmt(gps_longitude, "Lon %+.6f", hardware.longitude);
            lv_label_set_text_fmt(gps_speed, "Speed %.1f km/h", hardware.speed_kmh);
        } else {
            lv_label_set_text(gps_satellites, "Satellites --");
            lv_label_set_text(gps_latitude, "Lat --");
            lv_label_set_text(gps_longitude, "Lon --");
            lv_label_set_text(gps_speed, "Speed -- km/h");
        }
        if (valid && hardware.altitude_valid) {
            lv_label_set_text_fmt(gps_altitude, "Altitude %.1f m", hardware.altitude_m);
        } else { lv_label_set_text(gps_altitude, "Altitude -- m"); }
        if (ready && hardware.gnss_us) {
            lv_label_set_text_fmt(gps_age, "AGE %lld ms", (long long)((now - hardware.gnss_us) / 1000));
        } else { lv_label_set_text(gps_age, "AGE --"); }
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    zh_font_tick();
    const lv_font_t *font = zh_font();
    if (font) { lv_obj_set_style_text_font(file_detail, font, 0); }
    if (!tf_card_snapshot(&incoming)) { return; }
    char audio_status[96];
    bool audio_ready = touch_sound_music_status(audio_status, sizeof(audio_status));
    lv_label_set_text_fmt(music_status, "%s\n%s",
                          incoming.mounted ? "TF card ready" : "TF card unavailable",
                          audio_ready ? audio_status : "Audio unavailable");
    if (incoming.revision == rendered_revision) { return; }
    snapshot = incoming;
    rendered_revision = snapshot.revision;
    if (!snapshot.mounted) {
        lv_label_set_text_fmt(capacity_label, "TF unavailable\n%s", esp_err_to_name(snapshot.error));
    } else if (!snapshot.capacity_valid) {
        lv_label_set_text(capacity_label, "TF detected | capacity unavailable");
    } else {
        lv_label_set_text_fmt(capacity_label, "Total %.2f GiB\nFree %.2f | Used %.2f GiB",
            snapshot.total_bytes / 1073741824.0, snapshot.free_bytes / 1073741824.0,
            (snapshot.total_bytes - snapshot.free_bytes) / 1073741824.0);
    }
    lv_label_set_text(path_label, snapshot.path[0] ? snapshot.path : "/");
    lv_label_set_text_fmt(page_label, "%u", snapshot.page + 1);
    bool ready = snapshot.mounted && !snapshot.busy;
    set_enabled(up_button, ready && strcmp(snapshot.path, "/"));
    set_enabled(previous_button, ready && snapshot.page > 0);
    set_enabled(next_button, ready && snapshot.more);
    if (snapshot.busy) { return; }
    lv_obj_clean(file_list);
    lv_label_set_text(file_detail, snapshot.error != ESP_OK && snapshot.mounted ?
                      "Directory read failed" : "");
    if (!snapshot.mounted || !snapshot.count) {
        make_label(file_list, snapshot.mounted ? "Empty folder" : "No TF card", 4, 4, 184,
                    &lv_font_montserrat_12, 0x8CA49B);
        return;
    }
    for (unsigned index = 0; index < snapshot.count; ++index) {
        const tf_entry_t *entry = &snapshot.entries[index];
        char text[288];
        snprintf(text, sizeof(text), "%s %s", entry->directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                 entry->name);
        lv_obj_t *row = file_button(file_list, text, 0, index * 34, 212, 32,
                                    browse_clicked, (void *)(uintptr_t)index);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1C2B26), 0);
    }
    lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
}

static lv_obj_t *content(void)
{
    lv_obj_t *container = lv_obj_create(screen);
    lv_obj_remove_style_all(container);
    lv_obj_set_pos(container, 0, 40);
    lv_obj_set_size(container, 240, 280);
    lv_obj_set_scrollable(container, false);
    return container;
}

static void enable_vertical_scroll(lv_obj_t *container)
{
    lv_obj_set_scrollable(container, true);
    lv_obj_set_scroll_dir(container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(container, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x8CE2BE), LV_PART_SCROLLBAR);
    lv_obj_set_scroll_elastic(container, false);
}

lv_obj_t *entertainment_view_create(void)
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101918), 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    make_label(screen, "SYSTEM", 12, 8, 88, &lv_font_montserrat_12, 0x8CE2BE);
    home = content();
    lv_obj_set_height(home, 272);
    enable_vertical_scroll(home);
    button(home, LV_SYMBOL_AUDIO "  Music", 14, 8, 212, 52, navigate, (void *)1);
    button(home, LV_SYMBOL_SD_CARD "  TF card / Files", 14, 68, 212, 52, navigate, (void *)2);
    button(home, LV_SYMBOL_REFRESH "  Gyroscope", 14, 128, 212, 52, navigate, (void *)3);
    button(home, LV_SYMBOL_LOOP "  Magnetometer", 14, 188, 212, 52, navigate, (void *)4);
    button(home, LV_SYMBOL_GPS "  GPS", 14, 248, 212, 52, navigate, (void *)5);
    button(home, LV_SYMBOL_WIFI "  WiFi", 14, 308, 212, 52, navigate, (void *)6);
    button(home, LV_SYMBOL_SD_CARD "  USB Disk", 14, 368, 212, 52, usb_clicked, NULL);
    make_label(home, LV_SYMBOL_SETTINGS "  Brightness", 20, 430, 202,
                &lv_font_montserrat_12, 0x8CE2BE);
    brightness_slider = lv_slider_create(home);
    lv_obj_set_pos(brightness_slider, 20, 454);
    lv_obj_set_size(brightness_slider, 160, 18);
    lv_slider_set_range(brightness_slider, LCD_BACKLIGHT_MIN_PERCENT, 100);
    lv_slider_set_value(brightness_slider, display_port_brightness_get(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0x283B36), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0x8CE2BE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider, 2, LV_PART_KNOB);
    brightness_value = make_label(home, "", 190, 450, 42, &lv_font_montserrat_12, 0xFFFFFF);
    lv_label_set_text_fmt(brightness_value, "%u%%", display_port_brightness_get());
    lv_obj_add_event_cb(brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_slider, brightness_released, LV_EVENT_RELEASED, NULL);
    /* 视频暂不提供可点击入口，避免让未实现的功能看起来可用。 */
    make_label(home, LV_SYMBOL_VIDEO "  Video - unavailable", 20, 492, 202,
                &lv_font_montserrat_12, 0x8CA49B);
    music = content();
    make_label(music, LV_SYMBOL_AUDIO, 92, 8, 56, &lv_font_montserrat_28, 0x8CE2BE);
    make_label(music, "MUSIC", 20, 48, 200, &lv_font_montserrat_28, 0xFFFFFF);
    make_label(music, "Plays /tf/music/*.wav or built-in melody", 20, 92, 200,
                &lv_font_montserrat_12, 0x8CA49B);
    music_status = make_label(music, "Checking TF card...", 20, 118, 200,
                               &lv_font_montserrat_12, 0x8CE2BE);
    lv_obj_set_height(music_status, 36);
    button(music, LV_SYMBOL_PLAY "  Play music", 20, 162, 200, 42, music_play_clicked, NULL);
    button(music, LV_SYMBOL_STOP "  Stop", 20, 210, 200, 36, music_stop_clicked, NULL);
    storage = content();
    capacity_label = make_label(storage, "Checking TF card...", 14, 6, 212,
                                 &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_set_height(capacity_label, 34);
    path_label = make_label(storage, "/", 14, 46, 168, &lv_font_montserrat_12, 0x8CE2BE);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    button(storage, LV_SYMBOL_REFRESH, 190, 40, 36, 28, browse_clicked, (void *)(intptr_t)-4);
    file_list = lv_obj_create(storage);
    lv_obj_remove_style_all(file_list);
    lv_obj_set_pos(file_list, 14, 76);
    lv_obj_set_size(file_list, 212, 130);
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
    file_detail = make_label(storage, "", 14, 210, 212, &lv_font_montserrat_12, 0x8CA49B);
    lv_label_set_long_mode(file_detail, LV_LABEL_LONG_SCROLL_CIRCULAR);
    up_button = button(storage, LV_SYMBOL_UP, 14, 234, 40, 36, browse_clicked, (void *)(intptr_t)-3);
    previous_button = button(storage, LV_SYMBOL_LEFT, 68, 234, 40, 36, browse_clicked, (void *)(intptr_t)-1);
    page_label = make_label(storage, "1", 116, 244, 36, &lv_font_montserrat_12, 0xFFFFFF);
    next_button = button(storage, LV_SYMBOL_RIGHT, 158, 234, 40, 36, browse_clicked, (void *)(intptr_t)-2);
    gyroscope = content();
    enable_vertical_scroll(gyroscope);
    imu_view_create_in(lv_obj_get_display(screen), gyroscope);
    imu_view_layout(240, 320);
    magnetometer = content();
    enable_vertical_scroll(magnetometer);
    magnetic_state = make_label(magnetometer, "WAITING", 14, 4, 212, &lv_font_montserrat_12, 0x8CE2BE);
    compass_dial = make_shape(magnetometer, 42, 24, 156, 156, 0x14231E, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(compass_dial, 1, 0);
    lv_obj_set_style_border_color(compass_dial, lv_color_hex(0x3B504A), 0);
    lv_obj_set_scrollable(compass_dial, false);
    lv_obj_set_clickable(compass_dial, false);
    lv_obj_add_event_cb(compass_dial, draw_compass, LV_EVENT_DRAW_MAIN, NULL);
    const char *cardinals[] = {"N", "E", "S", "W"};
    const int cardinal_x[] = {66, 122, 66, 10};
    const int cardinal_y[] = {10, 70, 130, 70};
    for (unsigned index = 0; index < 4; ++index) {
        lv_obj_t *label = make_label(compass_dial, cardinals[index], cardinal_x[index], cardinal_y[index],
                                     24, &lv_font_montserrat_12, index ? 0xC1CBC9 : 0xF37870);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *hub = make_shape(compass_dial, 74, 74, 8, 8, 0x8CE2BE, LV_RADIUS_CIRCLE);
    lv_obj_set_clickable(hub, false);
    compass_heading_label = make_label(magnetometer, "-- deg", 14, 184, 212,
                                        &lv_font_montserrat_28, 0xFFFFFF);
    lv_obj_set_style_text_align(compass_heading_label, LV_TEXT_ALIGN_CENTER, 0);
    const char *axes[] = {"X", "Y", "Z"};
    for (unsigned axis = 0; axis < 3; ++axis) {
        make_label(magnetometer, axes[axis], 14, 254 + axis * 24, 24, &lv_font_montserrat_12, 0x8CE2BE);
        magnetic_values[axis] = make_label(magnetometer, "-- G", 50, 254 + axis * 24, 176,
                                           &lv_font_montserrat_12, 0xFFFFFF);
    }
    magnetic_strength = make_label(magnetometer, "Field -- G", 14, 222, 212, &lv_font_montserrat_12, 0xFFFFFF);
    magnetic_age = make_label(magnetometer, "AGE --", 14, 330, 212, &lv_font_montserrat_12, 0x8CA49B);
    magnetic_diagnostic = make_label(magnetometer, "MMC5983MA", 14, 354, 212, &lv_font_montserrat_12, 0x8CA49B);
    gps = content();
    gps_state = make_label(gps, "WAITING / NO FIX", 14, 8, 212, &lv_font_montserrat_12, 0x8CE2BE);
    gps_satellites = make_label(gps, "Satellites --", 14, 38, 212, &lv_font_montserrat_12, 0xFFFFFF);
    gps_latitude = make_label(gps, "Lat --", 14, 68, 212, &lv_font_montserrat_12, 0xFFFFFF);
    gps_longitude = make_label(gps, "Lon --", 14, 98, 212, &lv_font_montserrat_12, 0xFFFFFF);
    gps_speed = make_label(gps, "Speed -- km/h", 14, 128, 212, &lv_font_montserrat_12, 0xFFFFFF);
    gps_altitude = make_label(gps, "Altitude -- m", 14, 158, 212, &lv_font_montserrat_12, 0xFFFFFF);
    gps_age = make_label(gps, "AGE --", 14, 208, 212, &lv_font_montserrat_12, 0x8CA49B);
    wifi = content();
    wifi_view_create_in(lv_obj_get_display(screen), wifi);
    back_button = button(screen, LV_SYMBOL_LEFT, 10, 36, 34, 30, navigate, NULL);
    child_title = make_label(screen, "", 54, 44, 174, &lv_font_montserrat_12, 0xC1CBC9);
    /* 二级页向下让出返回键，文件区仍保持可滚动且不会越过底部。 */
    lv_obj_set_y(music, 68);
    lv_obj_set_height(music, 250);
    lv_obj_set_y(storage, 68);
    lv_obj_set_height(storage, 250);
    lv_obj_set_height(file_list, 100);
    lv_obj_set_y(file_detail, 180);
    lv_obj_set_y(up_button, 204);
    lv_obj_set_y(previous_button, 204);
    lv_obj_set_y(page_label, 214);
    lv_obj_set_y(next_button, 204);
    lv_obj_t *sensor_pages[] = {gyroscope, magnetometer, gps};
    for (unsigned index = 0; index < 3; ++index) {
        lv_obj_set_pos(sensor_pages[index], 0, 68);
        lv_obj_set_height(sensor_pages[index], 244);
    }
    lv_obj_set_pos(wifi, 0, 68);
    lv_obj_set_height(wifi, 250);
    entertainment_view_home();
    lv_timer_create(refresh_sensors, 100, NULL);
    lv_timer_create(refresh, 500, NULL);
    return screen;
}