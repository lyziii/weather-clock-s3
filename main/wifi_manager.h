#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"

/* WiFi 状态机集中管理 STA 初始化、扫描、连接与 NVS 凭据，供串口和触摸界面共用。 */
typedef enum {
    WIFI_MANAGER_IDLE,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

/* 初始化事件组、WiFi 栈与 STA 模式；netif 与事件循环须已创建。 */
esp_err_t wifi_manager_init(void);

/* 保存凭据并启动连接，立即返回；结果通过状态查询获取。ssid 1-32 字节。 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/* 停止 STA 并清空连接/失败状态，回到空闲态。 */
void wifi_manager_stop(void);

wifi_manager_state_t wifi_manager_state(void);
bool wifi_manager_connected(void);

/* 读取 NVS 中保存的凭据；无有效记录返回 false。 */
bool wifi_manager_load(wifi_config_t *config);

/* 阻塞扫描附近 AP；成功时把申请内存的记录数组放入 *records，调用方 free。 */
esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count);

/* 等待连接或失败，连接成功返回 true，超时或失败返回 false。 */
bool wifi_manager_wait_connected(uint32_t timeout_ms);
