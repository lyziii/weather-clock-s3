/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "weather_clock.h"
#include "board_hardware.h"
#include "board_power.h"
#include "power_manager.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_RETRY_LIMIT 5

/* USB 负责交互配置，UART43/44 保留给 GPS，时钟后台独立刷新。 */
static EventGroupHandle_t wifi_events;
static unsigned wifi_retries;
static bool wifi_started;
static bool sntp_initialized;
static EventGroupHandle_t clock_events;
#define TIME_VALID_BIT BIT0
#define CLOCK_DISPLAY_BIT BIT1

/* 支持 CR、LF、CRLF 和退格；超长输入整行丢弃，避免截断密码后误连接。 */
static bool read_line(char *buffer, size_t capacity, bool secret)
{
    static bool previous_cr;
    size_t length = 0;
    bool overflow = false;
    /* 等待串口输入前刷新提示，避免监视器或标准输出缓冲隐藏交互状态。 */
    fflush(stdout);
    for (;;) {
        unsigned char character;
        if (usb_serial_jtag_read_bytes(&character, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (previous_cr && character == '\n') {
            previous_cr = false;
            continue;
        }
        previous_cr = character == '\r';
        if (character == '\r' || character == '\n') {
            buffer[length] = '\0';
            printf("\n");
            return !overflow;
        }
        if (character == 8 || character == 127) {
            if (length > 0 && !overflow) {
                --length;
                printf("\b \b");
            }
        } else if (character >= 32) {
            if (length + 1 < capacity && !overflow) {
                buffer[length++] = (char)character;
                printf("%c", secret ? '*' : character);
            } else {
                overflow = true;
            }
        }
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (wifi_retries++ < WIFI_RETRY_LIMIT) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        wifi_retries = 0;
        xEventGroupClearBits(wifi_events, WIFI_FAILED_BIT);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        printf("\nWiFi connected. IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

static bool load_wifi_config(wifi_config_t *config)
{
    nvs_handle_t storage;
    if (nvs_open("wifi_clock", NVS_READONLY, &storage) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(config->sta);
    esp_err_t result = nvs_get_blob(storage, "station", &config->sta, &size);
    nvs_close(storage);
    return result == ESP_OK && size == sizeof(config->sta) && config->sta.ssid[0] != 0;
}

static void save_wifi_config(const wifi_config_t *config)
{
    nvs_handle_t storage;
    esp_err_t result = nvs_open("wifi_clock", NVS_READWRITE, &storage);
    if (result == ESP_OK) {
        result = nvs_set_blob(storage, "station", &config->sta, sizeof(config->sta));
        if (result == ESP_OK) {
            result = nvs_commit(storage);
        }
        nvs_close(storage);
    }
    printf("WiFi settings %s.\n", result == ESP_OK ? "saved" : "could not be saved");
}

static bool prompt_wifi_config(wifi_config_t *config)
{
    char ssid[40];
    char password[65];
    for (;;) {
        printf("\nEnter skip for offline use, or mode desk / mode ride.\nSSID (1-32 bytes):\n");
        if (!read_line(ssid, sizeof(ssid), false) || ssid[0] == '\0') {
            printf("Invalid SSID. Try again.\n");
            continue;
        }
        /* ssid 前缀用于输入与命令同名的网络；密码阶段始终作为密码处理。 */
        bool literal_ssid = strncmp(ssid, "ssid ", 5) == 0;
        const char *network_name = literal_ssid ? ssid + 5 : ssid;
        if (!literal_ssid && (strcmp(ssid, "skip") == 0 || weather_clock_command(ssid))) {
            return false;
        }
        if (strlen(network_name) == 0 || strlen(network_name) > 32) {
            printf("SSID must contain 1-32 bytes.\n");
            continue;
        }
        printf("Password (empty for open network):\n");
        if (!read_line(password, sizeof(password), true)) {
            printf("Password too long. Try again.\n");
            continue;
        }
        size_t length = strlen(password);
        bool hex_key = length == 64 && strspn(password, "0123456789abcdefABCDEF") == 64;
        if (length != 0 && !hex_key && (length < 8 || length > 63)) {
            printf("Use 8-63 password bytes or a 64-digit hexadecimal key.\n");
            continue;
        }
        memset(config, 0, sizeof(*config));
        memcpy(config->sta.ssid, network_name, strlen(network_name));
        memcpy(config->sta.password, password, length);
        memset(password, 0, sizeof(password));
        return true;
    }
}

static bool connect_wifi(wifi_config_t *config)
{
    wifi_retries = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;
    printf("Connecting to WiFi...\n");
    EventBits_t result = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (result & WIFI_CONNECTED_BIT) != 0;
}

static void stop_wifi(void)
{
    if (wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        wifi_started = false;
    }
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
}

/* 只在联网后启动 SNTP；超时不输出 1970 年假时间，允许串口主动重试。 */
static void synchronize_time(void)
{
    if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
        printf("WiFi offline. Enter config to reconnect.\n");
        return;
    }
    if (sntp_initialized) {
        esp_netif_sntp_deinit();
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    sntp_initialized = true;
    printf("Synchronizing time (up to 30 seconds)...\n");
    esp_err_t result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000));
    if (result == ESP_OK) {
        xEventGroupSetBits(clock_events, TIME_VALID_BIT);
        printf("Time synchronized. Timezone: UTC+8.\n");
    } else {
        printf("Time sync failed: %s. Enter sync to retry.\n", esp_err_to_name(result));
    }
}

static void clock_task(void *argument)
{
    for (;;) {
        EventBits_t state = xEventGroupGetBits(clock_events);
        bool online = (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
        weather_clock_tick(online, (state & TIME_VALID_BIT) != 0);
        if ((state & (TIME_VALID_BIT | CLOCK_DISPLAY_BIT)) == (TIME_VALID_BIT | CLOCK_DISPLAY_BIT)) {
            time_t now = time(NULL);
            struct tm local_time;
            char date_time[32];
            localtime_r(&now, &local_time);
            strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &local_time);
            printf("Date/time: %s UTC+8%s\n", date_time,
                   (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) ? "" : " (WiFi offline)");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    power_manager_handle_wakeup();
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 中断驱动允许控制台阻塞等待，USB 未连接时界面和采集仍继续运行。 */
    usb_serial_jtag_driver_config_t console = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&console));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    printf("\nESP32-S3R8 weather clock ready. Console: native USB Serial/JTAG.\n");
    if (power_manager_wakeup_fault()) {
        printf("Touch wake interrupt did not release; starting normally for recovery.\n");
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    wifi_events = xEventGroupCreate();
    clock_events = xEventGroupCreate();
    if (!wifi_events || !clock_events) {
        abort();
    }
    xEventGroupSetBits(clock_events, CLOCK_DISPLAY_BIT);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) {
        abort();
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    setenv("TZ", "CST-8", 1);
    tzset();
    /* 界面和天气任务先启动，串口等待输入和 NTP 等待均不会阻塞屏幕刷新。 */
    result = board_hardware_init();
    if (result != ESP_OK) { printf("Optional hardware disabled: %s\n", esp_err_to_name(result)); }
    ESP_ERROR_CHECK(weather_clock_init());
    result = board_power_init();
    if (result != ESP_OK) { printf("Power sampling unavailable: %s\n", esp_err_to_name(result)); }
    if (xTaskCreate(clock_task, "clock", 4096, NULL, 3, NULL) != pdPASS) {
        abort();
    }
    printf("\nWiFi initialization complete. Checking saved settings...\n");

    wifi_config_t wifi_config = {0};
    bool stored_config = load_wifi_config(&wifi_config);
    for (;;) {
        if (!stored_config) {
            if (!prompt_wifi_config(&wifi_config)) { break; }
        } else {
            printf("Trying saved WiFi settings...\n");
        }
        if (connect_wifi(&wifi_config)) {
            save_wifi_config(&wifi_config);
            synchronize_time();
            break;
        }
        printf("WiFi connection failed. Check 2.4 GHz SSID/password.\n");
        stop_wifi();
        stored_config = false;
    }
    for (;;) {
        char command[40];
        printf("Commands: mode desk, mode ride, mode imu, ride start, ride pause, ride reset, status, power, config, sync\n");
        if (!read_line(command, sizeof(command), false)) {
            printf("Command too long.\n");
        } else if (weather_clock_command(command)) {
            continue;
        } else if (strcmp(command, "power") == 0) {
            board_power_print_status();
        } else if (strcmp(command, "sync") == 0) {
            synchronize_time();
        } else if (strcmp(command, "config") == 0) {
            /* 暂停刷屏但保留有效时间，重配网络时本地时钟仍继续走时。 */
            xEventGroupClearBits(clock_events, CLOCK_DISPLAY_BIT);
            stop_wifi();
            do {
                if (!prompt_wifi_config(&wifi_config)) { break; }
                if (connect_wifi(&wifi_config)) {
                    save_wifi_config(&wifi_config);
                    synchronize_time();
                    break;
                }
                printf("WiFi connection failed. Try again.\n");
                stop_wifi();
            } while (true);
            xEventGroupSetBits(clock_events, CLOCK_DISPLAY_BIT);
        } else if (command[0] != '\0') {
            printf("Unknown command.\n");
        }
    }
}
