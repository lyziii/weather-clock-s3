#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "ui_helpers.h"
#include "zh_font.h"

#define WIFI_VIEW_MAX_APS 20
#define SSID_MAX 32
#define PASSWORD_MAX 63

typedef enum {
    WIFI_OP_SCAN,
    WIFI_OP_CONNECT,
} wifi_op_t;

typedef struct {
    wifi_op_t op;
    char ssid[SSID_MAX + 1];
    char password[PASSWORD_MAX + 1];
} wifi_request_t;

typedef struct {
    char ssid[SSID_MAX + 1];
    int8_t rssi;
} wifi_ap_entry_t;

enum {
    KEY_BACKSPACE = -1,
    KEY_SHIFT = -2,
    KEY_TOGGLE = -3,
    KEY_OK = -4,
};

static lv_obj_t *list_panel;
static lv_obj_t *entry_panel;
static lv_obj_t *status_label;
static lv_obj_t *list_buttons;
static lv_obj_t *ssid_label;
static lv_obj_t *password_label;
static lv_obj_t *ssid_button;
static lv_obj_t *password_button;
static lv_obj_t *keyboard;

static char ssid[SSID_MAX + 1];
static char password[PASSWORD_MAX + 1];
static bool editing_password;
static bool symbols;
static bool shift;
static bool scan_done;
static bool scanning;
static bool scan_ok;
static wifi_ap_entry_t aps[WIFI_VIEW_MAX_APS];
static unsigned ap_count;
static SemaphoreHandle_t ap_mutex;
static QueueHandle_t request_queue;

static void set_status(const char *text);
static void update_fields(void);
static void keyboard_rebuild(void);
static void show_entry(void);
static void show_list(void);
static void connect_current(void);
static void key_pressed(lv_event_t *event);

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                        int width, int height, lv_event_cb_t callback)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_radius(control, 4, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x283B36), 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x39715D), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_style_pad_all(control, 0, 0);
    lv_obj_set_scrollable(control, false);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, NULL);
    return control;
}

static lv_obj_t *field_button(lv_obj_t *parent, int x, int y, int width, int height,
                              lv_event_cb_t callback)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_radius(control, 4, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x14231E), 0);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_style_pad_all(control, 6, 0);
    lv_obj_set_scrollable(control, false);
    lv_obj_add_event_cb(control, callback, LV_EVENT_CLICKED, NULL);
    return control;
}

static lv_obj_t *field_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, 210);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    return label;
}

static void add_key(lv_obj_t *parent, int x, int y, int width, int height,
                    const char *label, intptr_t code)
{
    lv_obj_t *control = lv_button_create(parent);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_radius(control, 4, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x283B36), 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x527C6E), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_style_pad_all(control, 0, 0);
    lv_obj_set_scrollable(control, false);
    lv_obj_t *text = lv_label_create(control);
    lv_label_set_text(text, label);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(text);
    lv_obj_add_event_cb(control, key_pressed, LV_EVENT_CLICKED, (void *)code);
}

static void place_keys(const char *chars, int x, int y, int width, int height, int gap)
{
    for (size_t index = 0; chars[index] != '\0'; ++index) {
        char label[2] = { chars[index], '\0' };
        add_key(keyboard, x, y, width, height, label, (intptr_t)chars[index]);
        x += width + gap;
    }
}

static void set_status(const char *text)
{
    lv_label_set_text(status_label, text);
}

static void update_fields(void)
{
    char line[80];
    snprintf(line, sizeof(line), "SSID: %s%s", ssid, editing_password ? "" : "|");
    lv_label_set_text(ssid_label, line);
    char masked[PASSWORD_MAX + 1];
    size_t length = strlen(password);
    for (size_t index = 0; index < length; ++index) {
        masked[index] = '*';
    }
    masked[length] = '\0';
    snprintf(line, sizeof(line), "PWD : %s%s", masked, editing_password ? "|" : "");
    lv_label_set_text(password_label, line);
    lv_obj_set_style_bg_color(ssid_button, lv_color_hex(editing_password ? 0x14231E : 0x24463A), 0);
    lv_obj_set_style_bg_color(password_button, lv_color_hex(editing_password ? 0x24463A : 0x14231E), 0);
}

static void key_pressed(lv_event_t *event)
{
    intptr_t code = (intptr_t)lv_event_get_user_data(event);
    if (code == KEY_BACKSPACE) {
        char *target = editing_password ? password : ssid;
        size_t length = strlen(target);
        if (length) {
            target[length - 1] = '\0';
        }
        update_fields();
        return;
    }
    if (code == KEY_SHIFT) {
        shift = !shift;
        keyboard_rebuild();
        return;
    }
    if (code == KEY_TOGGLE) {
        symbols = !symbols;
        shift = false;
        keyboard_rebuild();
        return;
    }
    if (code == KEY_OK) {
        if (!editing_password) {
            if (ssid[0] == '\0') {
                set_status("SSID is empty");
                return;
            }
            editing_password = true;
            update_fields();
        } else {
            connect_current();
        }
        return;
    }
    char *target = editing_password ? password : ssid;
    size_t maximum = editing_password ? PASSWORD_MAX : SSID_MAX;
    size_t length = strlen(target);
    if (length < maximum) {
        target[length] = (char)code;
        target[length + 1] = '\0';
    }
    update_fields();
}

static void keyboard_rebuild(void)
{
    lv_obj_clean(keyboard);
    const int key_w = 22;
    const int key_h = 26;
    const int gap = 2;
    const int row_y[4] = { 0, 28, 56, 84 };
    if (symbols) {
        place_keys("1234567890", 1, row_y[0], key_w, key_h, gap);
        place_keys("!@#$%&*()", 1, row_y[1], key_w, key_h, gap);
        place_keys("-_=+.,?/:", 1, row_y[2], key_w, key_h, gap);
        add_key(keyboard, 1, row_y[3], 48, key_h, "ABC", KEY_TOGGLE);
        add_key(keyboard, 54, row_y[3], 132, key_h, "SPACE", ' ');
        add_key(keyboard, 191, row_y[3], 48, key_h, "OK", KEY_OK);
    } else {
        place_keys(shift ? "QWERTYUIOP" : "qwertyuiop", 1, row_y[0], key_w, key_h, gap);
        place_keys(shift ? "ASDFGHJKL" : "asdfghjkl", 12, row_y[1], key_w, key_h, gap);
        add_key(keyboard, 1, row_y[2], 34, key_h, "SHFT", KEY_SHIFT);
        const char *bottom = shift ? "ZXCVBNM" : "zxcvbnm";
        int x = 37;
        for (int index = 0; index < 7; ++index) {
            char label[2] = { bottom[index], '\0' };
            add_key(keyboard, x, row_y[2], key_w, key_h, label, (intptr_t)bottom[index]);
            x += key_w + gap;
        }
        add_key(keyboard, x, row_y[2], 34, key_h, "DEL", KEY_BACKSPACE);
        add_key(keyboard, 1, row_y[3], 48, key_h, "?123", KEY_TOGGLE);
        add_key(keyboard, 54, row_y[3], 132, key_h, "SPACE", ' ');
        add_key(keyboard, 191, row_y[3], 48, key_h, "OK", KEY_OK);
    }
}

static void show_entry(void)
{
    lv_obj_set_hidden(list_panel, true);
    lv_obj_set_hidden(entry_panel, false);
}

static void show_list(void)
{
    lv_obj_set_hidden(entry_panel, true);
    lv_obj_set_hidden(list_panel, false);
    set_status("Tap Scan to find networks");
}

static void ap_clicked(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    xSemaphoreTake(ap_mutex, portMAX_DELAY);
    if (index < ap_count) {
        snprintf(ssid, sizeof(ssid), "%s", aps[index].ssid);
    }
    xSemaphoreGive(ap_mutex);
    password[0] = '\0';
    editing_password = true;
    symbols = false;
    shift = false;
    keyboard_rebuild();
    show_entry();
    update_fields();
    set_status("Type password then OK");
}

static void render_list(void)
{
    lv_obj_clean(list_buttons);
    xSemaphoreTake(ap_mutex, portMAX_DELAY);
    const lv_font_t *font = zh_font();
    for (unsigned index = 0; index < ap_count; ++index) {
        lv_obj_t *row = lv_button_create(list_buttons);
        lv_obj_set_size(row, 232, 34);
        lv_obj_set_pos(row, 0, index * 38);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x283B36), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x39715D), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, aps[index].ssid);
        lv_obj_set_style_text_font(name, font ? font : &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(name, 160);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_t *signal = lv_label_create(row);
        lv_label_set_text_fmt(signal, "%d dBm", aps[index].rssi);
        lv_obj_set_style_text_font(signal, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(signal, lv_color_hex(0x8CA49B), 0);
        lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_add_event_cb(row, ap_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    }
    xSemaphoreGive(ap_mutex);
}

static void focus_ssid(lv_event_t *event)
{
    (void)event;
    editing_password = false;
    update_fields();
}

static void focus_password(lv_event_t *event)
{
    (void)event;
    editing_password = true;
    update_fields();
}

static void start_scan(void)
{
    if (wifi_manager_state() == WIFI_MANAGER_CONNECTING) {
        set_status("Busy connecting...");
        return;
    }
    wifi_request_t request = { .op = WIFI_OP_SCAN };
    xQueueSend(request_queue, &request, 0);
    scanning = true;
    set_status("Scanning...");
}

static void connect_current(void)
{
    if (ssid[0] == '\0') {
        set_status("SSID is empty");
        return;
    }
    size_t length = strlen(password);
    bool hex_key = length == 64 && strspn(password, "0123456789abcdefABCDEF") == 64;
    if (length != 0 && !hex_key && length < 8) {
        set_status("Password needs 8-63 chars");
        return;
    }
    wifi_request_t request = { .op = WIFI_OP_CONNECT };
    snprintf(request.ssid, sizeof(request.ssid), "%s", ssid);
    snprintf(request.password, sizeof(request.password), "%s", password);
    xQueueSend(request_queue, &request, 0);
    set_status("Connecting...");
}

static void scan_clicked(lv_event_t *event)
{
    (void)event;
    start_scan();
}

static void manual_clicked(lv_event_t *event)
{
    (void)event;
    ssid[0] = '\0';
    password[0] = '\0';
    editing_password = false;
    symbols = false;
    shift = false;
    keyboard_rebuild();
    show_entry();
    update_fields();
    set_status("Type SSID then OK");
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    show_list();
}

static void connect_clicked(lv_event_t *event)
{
    (void)event;
    connect_current();
}

static void wifi_task(void *argument)
{
    (void)argument;
    wifi_request_t request;
    for (;;) {
        if (xQueueReceive(request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (request.op == WIFI_OP_SCAN) {
            wifi_ap_record_t *records = NULL;
            uint16_t count = 0;
            bool ok = wifi_manager_scan(&records, &count) == ESP_OK;
            xSemaphoreTake(ap_mutex, portMAX_DELAY);
            scan_ok = ok;
            ap_count = ok ? (count < WIFI_VIEW_MAX_APS ? count : WIFI_VIEW_MAX_APS) : 0;
            for (unsigned index = 0; index < ap_count; ++index) {
                snprintf(aps[index].ssid, sizeof(aps[index].ssid), "%.*s",
                         32, (const char *)records[index].ssid);
                if (aps[index].ssid[0] == '\0') {
                    snprintf(aps[index].ssid, sizeof(aps[index].ssid), "<hidden>");
                }
                aps[index].rssi = records[index].rssi;
            }
            scan_done = true;
            xSemaphoreGive(ap_mutex);
            free(records);
        } else {
            wifi_manager_connect(request.ssid, request.password);
        }
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (scan_done) {
        scan_done = false;
        scanning = false;
        render_list();
        xSemaphoreTake(ap_mutex, portMAX_DELAY);
        unsigned total = ap_count;
        bool ok = scan_ok;
        xSemaphoreGive(ap_mutex);
        if (!ok) {
            set_status("Scan failed");
        } else if (total == 0) {
            set_status("No networks found");
        } else {
            set_status("Select a network");
        }
    }
    if (scanning) {
        return;
    }
    wifi_manager_state_t state = wifi_manager_state();
    switch (state) {
        case WIFI_MANAGER_CONNECTED:
            set_status("Connected");
            break;
        case WIFI_MANAGER_CONNECTING:
            set_status("Connecting...");
            break;
        case WIFI_MANAGER_FAILED:
            set_status("Connection failed");
            break;
        default:
            break;
    }
}

void wifi_view_create_in(lv_display_t *display, lv_obj_t *parent)
{
    (void)display;
    ap_mutex = xSemaphoreCreateMutex();
    request_queue = xQueueCreate(4, sizeof(wifi_request_t));
    xTaskCreate(wifi_task, "wifi_view", 6144, NULL, 5, NULL);

    make_label(parent, "WIFI SETUP", 14, 2, 212, &lv_font_montserrat_12, 0x8CE2BE);
    status_label = make_label(parent, "Tap Scan to find networks", 14, 22, 212,
                               &lv_font_montserrat_12, 0xC1CBC9);

    list_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(list_panel);
    lv_obj_set_pos(list_panel, 0, 44);
    lv_obj_set_size(list_panel, 240, 206);
    button(list_panel, "SCAN", 10, 0, 104, 34, scan_clicked);
    button(list_panel, "MANUAL", 126, 0, 104, 34, manual_clicked);
    list_buttons = lv_obj_create(list_panel);
    lv_obj_remove_style_all(list_buttons);
    lv_obj_set_pos(list_buttons, 4, 40);
    lv_obj_set_size(list_buttons, 232, 162);
    lv_obj_set_scrollable(list_buttons, true);
    lv_obj_set_scroll_dir(list_buttons, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_buttons, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list_buttons, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_buttons, lv_color_hex(0x8CE2BE), LV_PART_SCROLLBAR);
    lv_obj_set_scroll_elastic(list_buttons, false);

    entry_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(entry_panel);
    lv_obj_set_pos(entry_panel, 0, 44);
    lv_obj_set_size(entry_panel, 240, 206);
    ssid_button = field_button(entry_panel, 6, 0, 228, 28, focus_ssid);
    ssid_label = field_label(ssid_button);
    password_button = field_button(entry_panel, 6, 30, 228, 28, focus_password);
    password_label = field_label(password_button);
    button(entry_panel, "BACK", 6, 62, 108, 26, back_clicked);
    button(entry_panel, "CONNECT", 126, 62, 108, 26, connect_clicked);
    keyboard = lv_obj_create(entry_panel);
    lv_obj_remove_style_all(keyboard);
    lv_obj_set_pos(keyboard, 0, 92);
    lv_obj_set_size(keyboard, 240, 114);
    lv_obj_set_scrollable(keyboard, false);
    keyboard_rebuild();
    show_list();
    update_fields();
    lv_timer_create(refresh, 200, NULL);
}
