#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_manager.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_RETRY_LIMIT 5
#define WIFI_NVS_NAMESPACE "wifi_clock"
#define WIFI_NVS_KEY "station"

static EventGroupHandle_t wifi_events;
static unsigned wifi_retries;
/* STA 无线是否已启动；与是否有连接目标（attempting_connection）分开。 */
static bool wifi_started;
static bool attempting_connection;
/* 事件驱动扫描：STA 启动与扫描完成各用一次信号量通知。 */
static SemaphoreHandle_t sta_start_sem;
static SemaphoreHandle_t scan_done_sem;

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (sta_start_sem) {
            xSemaphoreGive(sta_start_sem);
        }
        /* 仅在有连接目标时自动连接，扫描临时启动 STA 不会误连。 */
        if (attempting_connection) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (scan_done_sem) {
            xSemaphoreGive(scan_done_sem);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (attempting_connection && wifi_retries++ < WIFI_RETRY_LIMIT) {
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

esp_err_t wifi_manager_init(void)
{
    wifi_events = xEventGroupCreate();
    sta_start_sem = xSemaphoreCreateBinary();
    scan_done_sem = xSemaphoreCreateBinary();
    if (!wifi_events || !sta_start_sem || !scan_done_sem) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (result != ESP_OK) {
        return result;
    }
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

bool wifi_manager_load(wifi_config_t *config)
{
    nvs_handle_t storage;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &storage) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(config->sta);
    esp_err_t result = nvs_get_blob(storage, WIFI_NVS_KEY, &config->sta, &size);
    nvs_close(storage);
    return result == ESP_OK && size == sizeof(config->sta) && config->sta.ssid[0] != 0;
}

static void wifi_manager_save(const wifi_config_t *config)
{
    nvs_handle_t storage;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &storage);
    if (result == ESP_OK) {
        result = nvs_set_blob(storage, WIFI_NVS_KEY, &config->sta, sizeof(config->sta));
        if (result == ESP_OK) {
            result = nvs_commit(storage);
        }
        nvs_close(storage);
    }
    printf("WiFi settings %s.\n", result == ESP_OK ? "saved" : "could not be saved");
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = password ? strlen(password) : 0;
    if (ssid_length > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password_length > sizeof(((wifi_config_t *)0)->sta.password)) {
        return ESP_ERR_INVALID_SIZE;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_length);
    if (password && password_length) {
        memcpy(config.sta.password, password, password_length);
    }
    wifi_manager_stop();
    wifi_retries = 0;
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        return result;
    }
    attempting_connection = true;
    result = esp_wifi_start();
    if (result != ESP_OK) {
        attempting_connection = false;
        return result;
    }
    wifi_started = true;
    wifi_manager_save(&config);
    printf("Connecting to WiFi (SSID: %s)...\n", ssid);
    return ESP_OK;
}

void wifi_manager_stop(void)
{
    attempting_connection = false;
    if (wifi_started) {
        esp_wifi_stop();
        wifi_started = false;
    }
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
}

wifi_manager_state_t wifi_manager_state(void)
{
    EventBits_t bits = xEventGroupGetBits(wifi_events);
    if (bits & WIFI_CONNECTED_BIT) {
        return WIFI_MANAGER_CONNECTED;
    }
    if (bits & WIFI_FAILED_BIT) {
        return WIFI_MANAGER_FAILED;
    }
    return attempting_connection ? WIFI_MANAGER_CONNECTING : WIFI_MANAGER_IDLE;
}

bool wifi_manager_connected(void)
{
    return (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count)
{
    if (!records || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *records = NULL;
    *count = 0;

    /* 扫描前停止任何活跃连接/连接尝试，避免扫描与连接并发导致驱动异常。 */
    if (attempting_connection) {
        printf("WiFi scan: stopping active connection before scan.\n");
        wifi_manager_stop();
    }

    if (!wifi_started) {
        /* 启动 STA 并等待 STA_START 事件，避免驱动尚未就绪就发起扫描。 */
        xSemaphoreTake(sta_start_sem, 0);
        esp_err_t result = esp_wifi_start();
        if (result != ESP_OK) {
            printf("WiFi scan: esp_wifi_start failed: %s\n", esp_err_to_name(result));
            return result;
        }
        wifi_started = true;
        if (xSemaphoreTake(sta_start_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            printf("WiFi scan: STA_START event timeout\n");
            return ESP_ERR_TIMEOUT;
        }
    }
    /* 非阻塞扫描并等待 SCAN_DONE，规避阻塞式扫描与启动流程的竞态。 */
    xSemaphoreTake(scan_done_sem, 0);
    esp_err_t result = esp_wifi_scan_start(NULL, false);
    if (result != ESP_OK) {
        printf("WiFi scan: esp_wifi_scan_start failed: %s\n", esp_err_to_name(result));
        return result;
    }
    if (xSemaphoreTake(scan_done_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        printf("WiFi scan: SCAN_DONE event timeout\n");
        return ESP_ERR_TIMEOUT;
    }
    uint16_t found = 0;
    result = esp_wifi_scan_get_ap_num(&found);
    if (result != ESP_OK) {
        printf("WiFi scan: get_ap_num failed: %s\n", esp_err_to_name(result));
        return result;
    }
    printf("WiFi scan: %u AP(s) found\n", found);
    if (found == 0) {
        return ESP_OK;
    }
    wifi_ap_record_t *aps = calloc(found, sizeof(*aps));
    if (!aps) {
        return ESP_ERR_NO_MEM;
    }
    result = esp_wifi_scan_get_ap_records(&found, aps);
    if (result != ESP_OK) {
        printf("WiFi scan: get_ap_records failed: %s\n", esp_err_to_name(result));
        free(aps);
        return result;
    }
    *records = aps;
    *count = found;
    return ESP_OK;
}
