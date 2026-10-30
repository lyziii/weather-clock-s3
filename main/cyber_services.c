#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "cyber_services.h"

#define CHAT_RESPONSE_LIMIT 12288
#define CHAT_REPLY_LIMIT 4096

typedef struct {
    bool chat;
    cyber_settings_t settings;
    char *messages;
} service_job_t;

typedef struct {
    char *data;
    size_t size;
    bool overflow;
} http_body_t;

static QueueHandle_t jobs;
static SemaphoreHandle_t reply_mutex;
static bool chat_busy;
static bool reply_ready;
static char chat_reply[CHAT_REPLY_LIMIT];

static esp_err_t receive_body(esp_http_client_event_t *event)
{
    http_body_t *body = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) { return ESP_OK; }
    if ((size_t)event->data_len > CHAT_RESPONSE_LIMIT - 1 - body->size) {
        body->overflow = true;
        return ESP_FAIL;
    }
    memcpy(body->data + body->size, event->data, event->data_len);
    body->size += event->data_len;
    body->data[body->size] = 0;
    return ESP_OK;
}

static void publish_reply(const char *message)
{
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    /* 限长时退回 UTF-8 字符边界，防止中文尾字被截成非法编码。 */
    size_t length = strnlen(message, sizeof(chat_reply) - 1);
    while (length && ((unsigned char)message[length] & 0xC0) == 0x80) { --length; }
    memcpy(chat_reply, message, length);
    chat_reply[length] = 0;
    chat_busy = false;
    reply_ready = true;
    xSemaphoreGive(reply_mutex);
}

static void perform_chat(service_job_t *job)
{
    cJSON *request = cJSON_CreateObject();
    cJSON *messages = cJSON_Parse(job->messages);
    if (!request || !cJSON_IsArray(messages)) {
        cJSON_Delete(request);
        cJSON_Delete(messages);
        publish_reply("请求构建失败");
        return;
    }
    cJSON_AddStringToObject(request, "model", job->settings.model);
    cJSON_AddBoolToObject(request, "stream", false);
    cJSON_AddNumberToObject(request, "max_tokens", 512);
    cJSON_AddItemToObject(request, "messages", messages);
    char *payload = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    http_body_t body = {.data = calloc(1, CHAT_RESPONSE_LIMIT)};
    if (!payload || !body.data) {
        free(body.data);
        cJSON_free(payload);
        publish_reply("内存不足，请稍后重试");
        return;
    }
    esp_http_client_config_t config = {
        .url = job->settings.endpoint,
        .timeout_ms = 25000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = receive_body,
        .user_data = &body,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        /* 禁止跟随重定向，防止 Authorization 被转发到其他服务。 */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_free(payload);
        free(body.data);
        publish_reply("无法创建 HTTPS 客户端");
        return;
    }
    char authorization[208];
    snprintf(authorization, sizeof(authorization), "Bearer %s", job->settings.api_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, payload, strlen(payload));
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(authorization, 0, sizeof(authorization));
    cJSON_free(payload);
    if (result != ESP_OK || status != 200 || body.overflow) {
        char error[96];
        snprintf(error, sizeof(error), "AI 请求失败：HTTP %d，%s%s", status,
                 esp_err_to_name(result), body.overflow ? "，响应过大" : "");
        publish_reply(error);
    } else {
        cJSON *response = cJSON_Parse(body.data);
        cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(response, "choices"), 0);
        cJSON *content = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(choice, "message"), "content");
        publish_reply(cJSON_IsString(content) ? content->valuestring : "服务未返回有效回复");
        cJSON_Delete(response);
    }
    free(body.data);
}

static void service_task(void *argument)
{
    (void)argument;
    service_job_t *job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) { continue; }
        if (job->chat) {
            perform_chat(job);
        } else {
            nvs_handle_t storage;
            esp_err_t result = nvs_open("cyber_tools", NVS_READWRITE, &storage);
            if (result == ESP_OK) {
                result = nvs_set_blob(storage, "settings", &job->settings, sizeof(job->settings));
                if (result == ESP_OK) { result = nvs_commit(storage); }
                nvs_close(storage);
            }
            if (result != ESP_OK) { ESP_LOGW("cyber_tools", "Settings not saved: %s", esp_err_to_name(result)); }
        }
        free(job->messages);
        memset(&job->settings, 0, sizeof(job->settings));
        free(job);
    }
}

bool cyber_services_init(cyber_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->version = 2;
    for (unsigned index = 0; index < 3; ++index) {
        settings->alarms[index].hour = 7;
        settings->alarms[index].volume = 60;
    }
    nvs_handle_t storage;
    if (nvs_open("cyber_tools", NVS_READONLY, &storage) == ESP_OK) {
        cyber_settings_t *saved = calloc(1, sizeof(*saved));
        size_t size = sizeof(*saved);
        if (saved && nvs_get_blob(storage, "settings", saved, &size) == ESP_OK &&
            size == sizeof(*saved) && saved->version == 2) {
            *settings = *saved;
        }
        free(saved);
        nvs_close(storage);
    }
    for (unsigned index = 0; index < 3; ++index) {
        cyber_alarm_t *alarm = &settings->alarms[index];
        if (alarm->hour > 23 || alarm->minute > 59 || alarm->days > 127) {
            memset(alarm, 0, sizeof(*alarm));
            alarm->hour = 7;
            alarm->volume = 60;
        }
        if (alarm->volume > 100) { alarm->volume = 60; }
        alarm->ringtone[sizeof(alarm->ringtone) - 1] = 0;
    }
    settings->endpoint[sizeof(settings->endpoint) - 1] = 0;
    settings->model[sizeof(settings->model) - 1] = 0;
    settings->api_key[sizeof(settings->api_key) - 1] = 0;
    jobs = xQueueCreate(4, sizeof(service_job_t *));
    reply_mutex = xSemaphoreCreateMutex();
    if (jobs && reply_mutex &&
        xTaskCreate(service_task, "cyber_services", 8192, NULL, 1, NULL) == pdPASS) { return true; }
    /* 初始化失败时一并撤销入口，避免设置请求进入无人处理的队列。 */
    if (jobs) { vQueueDelete(jobs); jobs = NULL; }
    if (reply_mutex) { vSemaphoreDelete(reply_mutex); reply_mutex = NULL; }
    return false;
}

static bool enqueue(const cyber_settings_t *settings, const char *messages)
{
    if (!jobs) { return false; }
    service_job_t *job = calloc(1, sizeof(*job));
    if (!job) { return false; }
    job->settings = *settings;
    job->chat = messages != NULL;
    if (messages) { job->messages = strdup(messages); }
    if ((messages && !job->messages) || xQueueSend(jobs, &job, 0) != pdTRUE) {
        free(job->messages);
        memset(&job->settings, 0, sizeof(job->settings));
        free(job);
        return false;
    }
    return true;
}

bool cyber_settings_save(const cyber_settings_t *settings) { return enqueue(settings, NULL); }

bool cyber_chat_start(const cyber_settings_t *settings, const char *messages_json)
{
    if (!reply_mutex || strncmp(settings->endpoint, "https://", 8) != 0 ||
        !settings->api_key[0] || !settings->model[0] ||
        strchr(settings->api_key, '\r') || strchr(settings->api_key, '\n')) { return false; }
    xSemaphoreTake(reply_mutex, portMAX_DELAY);
    bool accepted = !chat_busy && enqueue(settings, messages_json);
    if (accepted) { chat_busy = true; reply_ready = false; }
    xSemaphoreGive(reply_mutex);
    return accepted;
}

bool cyber_chat_poll(char *reply, unsigned capacity, bool *busy)
{
    if (!reply_mutex || xSemaphoreTake(reply_mutex, 0) != pdTRUE) { return false; }
    *busy = chat_busy;
    bool ready = reply_ready;
    if (ready && capacity) {
        snprintf(reply, capacity, "%s", chat_reply);
        reply_ready = false;
    }
    xSemaphoreGive(reply_mutex);
    return ready;
}