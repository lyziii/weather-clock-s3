#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "board_power.h"
#include "hardware_config.h"
#include "weather_clock.h"

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t calibration;
    adc_channel_t channel;
    int divider;
} power_channel_t;

typedef struct {
    bool battery_valid;
    bool source_valid;
    bool source_known;
    bool vbus_present;
    int battery_mv;
    int source_mv;
    unsigned percent;
} power_status_t;

static const char *TAG = "board_power";
static power_channel_t battery;
static power_channel_t source;
static TaskHandle_t power_task_handle;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static power_status_t latest;

static void release_channel(power_channel_t *channel)
{
    if (channel->calibration) { adc_cali_delete_scheme_curve_fitting(channel->calibration); }
    if (channel->unit) { adc_oneshot_del_unit(channel->unit); }
    *channel = (power_channel_t){0};
}

static esp_err_t initialize_channel(power_channel_t *channel, int gpio, int divider)
{
    adc_unit_t unit_id;
    esp_err_t result = adc_oneshot_io_to_channel(gpio, &unit_id, &channel->channel);
    if (result != ESP_OK) { return result; }
    adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = unit_id};
    result = adc_oneshot_new_unit(&unit_config, &channel->unit);
    if (result != ESP_OK) { return result; }
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    result = adc_oneshot_config_channel(channel->unit, channel->channel, &config);
    if (result == ESP_OK) {
        adc_cali_curve_fitting_config_t calibration = {
            .unit_id = unit_id, .chan = channel->channel,
            .atten = config.atten, .bitwidth = config.bitwidth,
        };
        result = adc_cali_create_scheme_curve_fitting(&calibration, &channel->calibration);
    }
    if (result != ESP_OK) { release_channel(channel); return result; }
    channel->divider = divider;
    return ESP_OK;
}

/* 只接受完整且校准成功的采样组；ADC2 仲裁失败不能作为 0 V 使用。 */
static bool read_voltage(power_channel_t *channel, int *millivolts)
{
    if (!channel->unit || !channel->calibration) { return false; }
    int total = 0;
    for (unsigned sample = 0; sample < 16; ++sample) {
        int raw;
        int voltage;
        if (adc_oneshot_read(channel->unit, channel->channel, &raw) != ESP_OK ||
            raw <= 0 || raw >= 4095 ||
            adc_cali_raw_to_voltage(channel->calibration, raw, &voltage) != ESP_OK) {
            return false;
        }
        total += voltage;
    }
    *millivolts = (total + 8) / 16 * channel->divider;
    return true;
}

/* 单节 4.2 V 锂电的粗略电压曲线，不代替库仑计或电池型号标定。 */
static unsigned estimate_percent(int millivolts)
{
    static const int voltage[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const unsigned percent[] = {0, 5, 10, 25, 50, 70, 85, 95, 100};
    if (millivolts <= voltage[0]) { return 0; }
    for (unsigned index = 1; index < sizeof(voltage) / sizeof(voltage[0]); ++index) {
        if (millivolts <= voltage[index]) {
            return percent[index - 1] + (millivolts - voltage[index - 1]) *
                (percent[index] - percent[index - 1]) / (voltage[index] - voltage[index - 1]);
        }
    }
    return 100;
}

static void power_task(void *argument)
{
    power_status_t status = {0};
    unsigned on_count = 0;
    unsigned off_count = 0;
    vTaskDelay(pdMS_TO_TICKS(50));
    for (;;) {
        status.battery_valid = read_voltage(&battery, &status.battery_mv) &&
                               status.battery_mv >= 2800 && status.battery_mv <= 4500;
        status.percent = status.battery_valid ? estimate_percent(status.battery_mv) : 0;
        status.source_valid = read_voltage(&source, &status.source_mv) &&
                              status.source_mv >= 2500 && status.source_mv <= 5500;
        if (!status.source_valid) {
            status.source_known = false;
            on_count = off_count = 0;
        } else if (status.source_mv >= HW_VBUS_ON_MV) {
            off_count = 0;
            if (on_count < 5) { ++on_count; }
            if (on_count == 5) { status.source_known = true; status.vbus_present = true; }
        } else if (status.source_mv <= HW_VBUS_OFF_MV) {
            on_count = 0;
            if (off_count < 5) { ++off_count; }
            if (off_count == 5) { status.source_known = true; status.vbus_present = false; }
        } else {
            on_count = off_count = 0;
        }
        portENTER_CRITICAL(&status_lock);
        latest = status;
        portEXIT_CRITICAL(&status_lock);
        /* 没有充电状态引脚，不将 VBUS 存在冒充成正在充电。 */
        weather_clock_battery_status(status.battery_valid, status.percent, false);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t board_power_init(void)
{
    if (power_task_handle) { return ESP_ERR_INVALID_STATE; }
    /* 按用户原表使功放保持禁用，ES8311 不在本阶段初始化。 */
    esp_err_t result = gpio_set_level(HW_AUDIO_ENABLE_GPIO, !HW_AUDIO_ENABLE_LEVEL);
    if (result == ESP_OK) { result = gpio_set_direction(HW_AUDIO_ENABLE_GPIO, GPIO_MODE_OUTPUT); }
    if (result != ESP_OK) { return result; }
    esp_err_t battery_result = initialize_channel(&battery, HW_BATTERY_GPIO, HW_BATTERY_DIVIDER);
    esp_err_t source_result = initialize_channel(&source, HW_POWER_GPIO, HW_POWER_DIVIDER);
    ESP_LOGI(TAG, "ADC calibration: battery=%s, source=%s; VBUS thresholds %d/%d mV require bench verification",
             esp_err_to_name(battery_result), esp_err_to_name(source_result), HW_VBUS_ON_MV, HW_VBUS_OFF_MV);
    if (battery_result != ESP_OK && source_result != ESP_OK) { return battery_result; }
    if (xTaskCreate(power_task, "power", 4096, NULL, 2, &power_task_handle) != pdPASS) {
        release_channel(&battery);
        release_channel(&source);
        power_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void board_power_print_status(void)
{
    power_status_t status;
    portENTER_CRITICAL(&status_lock);
    status = latest;
    portEXIT_CRITICAL(&status_lock);
    if (status.battery_valid) {
        printf("Battery: %d mV, estimated %u%%\n", status.battery_mv, status.percent);
    } else { printf("Battery: unknown (ADC/calibration unavailable)\n"); }
    if (status.source_valid) { printf("Power node: %d mV\n", status.source_mv); }
    else { printf("Power node: unknown (ADC/calibration unavailable)\n"); }
    printf("VBUS: %s; charging state: not available\n",
           !status.source_known ? "unknown" : (status.vbus_present ? "present" : "absent"));
}