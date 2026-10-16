#include <math.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "weather_clock_config.h"
#include "weather_service.h"

#define HTTP_BODY_LIMIT 4096

const char *weather_service_description(int code)
{
    if (code == 0) { return "Clear sky"; }
    if (code == 1) { return "Mostly clear"; }
    if (code == 2) { return "Partly cloudy"; }
    if (code == 3) { return "Overcast"; }
    if (code == 45 || code == 48) { return "Fog"; }
    if (code >= 51 && code <= 57) { return "Drizzle"; }
    if (code >= 61 && code <= 67) { return "Rain"; }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) { return "Snow"; }
    if (code >= 80 && code <= 82) { return "Rain showers"; }
    if (code >= 95 && code <= 99) { return "Thunderstorm"; }
    return "Unknown weather";
}

/* 有界读取兼容分块响应；检查 HTTPS 证书、HTTP 状态和 JSON 字段后才提交数据。 */
esp_err_t weather_service_fetch(weather_data_t *weather)
{
    if (!weather) { return ESP_ERR_INVALID_ARG; }
    const esp_http_client_config_t config = {
        .url = "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LATITUDE
               "&longitude=" WEATHER_LONGITUDE
               "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=Asia%2FShanghai",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000, .buffer_size = 1024,
        .user_agent = "ESP32-WeatherClock/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { return ESP_ERR_NO_MEM; }
    char *body = calloc(HTTP_BODY_LIMIT + 1, 1);
    if (!body) { esp_http_client_cleanup(client); return ESP_ERR_NO_MEM; }
    esp_err_t result = esp_http_client_open(client, 0);
    cJSON *root = NULL;
    if (result != ESP_OK) { goto cleanup; }
    if (esp_http_client_fetch_headers(client) < 0 || esp_http_client_get_status_code(client) != 200) {
        result = ESP_FAIL;
        goto cleanup;
    }
    size_t used = 0;
    while (used < HTTP_BODY_LIMIT) {
        int count = esp_http_client_read(client, body + used, HTTP_BODY_LIMIT - used);
        if (count < 0) { result = ESP_FAIL; goto cleanup; }
        if (count == 0) { break; }
        used += count;
    }
    if (!esp_http_client_is_complete_data_received(client) || used == HTTP_BODY_LIMIT) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    root = cJSON_Parse(body);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    if (!cJSON_IsNumber(temperature) || !isfinite(temperature->valuedouble) ||
        !cJSON_IsNumber(humidity) || humidity->valuedouble < 0 || humidity->valuedouble > 100 ||
        !cJSON_IsNumber(code) || code->valuedouble < 0 || code->valuedouble > 99) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    weather->temperature = temperature->valuedouble;
    weather->humidity = humidity->valueint;
    weather->code = code->valueint;
    result = ESP_OK;
cleanup:
    cJSON_Delete(root);
    free(body);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}