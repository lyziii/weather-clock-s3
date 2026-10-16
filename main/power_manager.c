#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware_config.h"
#include "power_manager.h"
#include "touch_sound.h"

#define WAKE_TAP_LIMIT 3
#define WAKE_TAP_WINDOW_US 5000000ULL
#define TOUCH_RELEASE_TIMEOUT_MS 1500

static RTC_DATA_ATTR uint8_t wake_tap_count;
static RTC_DATA_ATTR bool sleep_sequence_armed;
static bool wake_gate_fault;

static void configure_touch_wakeup(bool tap_window)
{
    rtc_gpio_init(HW_TOUCH_INT);
    rtc_gpio_set_direction(HW_TOUCH_INT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(HW_TOUCH_INT);
    rtc_gpio_pulldown_dis(HW_TOUCH_INT);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup_io(1ULL << HW_TOUCH_INT, ESP_EXT1_WAKEUP_ANY_LOW);
    if (tap_window) { esp_sleep_enable_timer_wakeup(WAKE_TAP_WINDOW_US); }
}

static void enter_deep_sleep(bool tap_window)
{
    configure_touch_wakeup(tap_window);
    esp_deep_sleep_start();
}

static bool read_touch_status(i2c_master_dev_handle_t device)
{
    uint8_t reg = 0x02;
    uint8_t count = 0;
    uint8_t data[16] = {0};
    if (i2c_master_transmit_receive(device, &reg, 1, &count, 1, 50) != ESP_OK) { return false; }
    reg = 0x00;
    return i2c_master_transmit_receive(device, &reg, 1, data, sizeof(data), 50) == ESP_OK;
}

/* 深睡唤醒时没有常规驱动对象；临时建总线读 FT6336G，清除低有效 INT 锁存。 */
static bool wait_for_touch_release(void)
{
    rtc_gpio_init(HW_TOUCH_INT);
    rtc_gpio_set_direction(HW_TOUCH_INT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(HW_TOUCH_INT);
    rtc_gpio_pulldown_dis(HW_TOUCH_INT);
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t device = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = HW_I2C_SDA,
        .scl_io_num = HW_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HW_TOUCH_ADDRESS,
        .scl_speed_hz = HW_I2C_HZ,
    };
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) { return false; }
    esp_err_t result = i2c_master_bus_add_device(bus, &device_config, &device);
    bool released = false;
    if (result == ESP_OK) {
        for (unsigned attempt = 0; attempt < TOUCH_RELEASE_TIMEOUT_MS / 10; ++attempt) {
            if (rtc_gpio_get_level(HW_TOUCH_INT) != 0 && attempt > 0) {
                released = true;
                break;
            }
            read_touch_status(device);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rtc_gpio_get_level(HW_TOUCH_INT) != 0) { released = true; }
        i2c_master_bus_rm_device(device);
    }
    i2c_del_master_bus(bus);
    return released;
}

void power_manager_enter_sleep(void)
{
    touch_sound_prepare_sleep();
    sleep_sequence_armed = true;
    wake_tap_count = 0;
    (void)esp_wifi_stop();
    enter_deep_sleep(false);
}

void power_manager_handle_wakeup(void)
{
    wake_gate_fault = false;
    if (!sleep_sequence_armed) { return; }

    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & (1U << ESP_SLEEP_WAKEUP_EXT1)) {
        if (wake_tap_count < WAKE_TAP_LIMIT) { ++wake_tap_count; }
    } else if (causes & (1U << ESP_SLEEP_WAKEUP_TIMER)) {
        wake_tap_count = 0;
        enter_deep_sleep(false);
    } else {
        sleep_sequence_armed = false;
        wake_tap_count = 0;
        return;
    }

    bool released = wait_for_touch_release();
    if (!released) {
        wake_gate_fault = true;
        sleep_sequence_armed = false;
        wake_tap_count = 0;
        rtc_gpio_deinit(HW_TOUCH_INT);
        return;
    }

    if (wake_tap_count >= WAKE_TAP_LIMIT) {
        sleep_sequence_armed = false;
        wake_tap_count = 0;
        rtc_gpio_deinit(HW_TOUCH_INT);
        return;
    }

    enter_deep_sleep(true);
}

bool power_manager_wakeup_fault(void)
{
    return wake_gate_fault;
}