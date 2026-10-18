#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "nvs.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "weather_clock_config.h"
#include "display_port.h"
#include "touch_sound.h"

#define DRAW_LINES 24
#define TOUCH_THRESHOLD 22

static const char *TAG = "display_port";
static esp_lcd_panel_io_handle_t touch_io;
static esp_lcd_touch_handle_t touch_handle;
static lv_indev_t *touch_input;
static esp_lcd_panel_handle_t active_panel;
static bool backlight_pwm;
static unsigned backlight_percent = LCD_BACKLIGHT_DEFAULT_PERCENT;
static const char *touch_stage = "OFF";
static char touch_diagnostic[24] = "NO INPUT";
static uint8_t touch_vendor_id;
static uint8_t touch_chip_id;
static bool touch_ids_valid;

static lv_indev_read_cb_t original_touch_read;
static bool touch_was_pressed;

/* 直接判断采样按下沿，禁用控件、空白区域和滑动也遵循每次落指只响一次。 */
static void read_touch_with_sound(lv_indev_t *input, lv_indev_data_t *data)
{
    original_touch_read(input, data);
    bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    if (pressed && !touch_was_pressed) { touch_sound_play(); }
    touch_was_pressed = pressed;
}

const char *display_port_touch_diagnostic(void)
{
    return touch_diagnostic;
}

/* 用户提供的 ILI9341V IPS 序列，保留供电、Gamma、反色和休眠等待。 */
static const ili9341_lcd_init_cmd_t panel_init_commands[] = {
    {0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},
    {0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20}, 1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t[]){0x13}, 1, 0},
    {0xC1, (uint8_t[]){0x13}, 1, 0},
    {0xC5, (uint8_t[]){0x22, 0x35}, 2, 0},
    {0xC7, (uint8_t[]){0xBD}, 1, 0},
    {0x21, NULL, 0, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0},
    {0xB6, (uint8_t[]){0x0A, 0xA2}, 2, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0xF6, (uint8_t[]){0x01, 0x30}, 2, 0},
    {0xB1, (uint8_t[]){0x00, 0x1B}, 2, 0},
    {0xF2, (uint8_t[]){0x00}, 1, 0},
    {0x26, (uint8_t[]){0x01}, 1, 0},
    {0xE0, (uint8_t[]){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7,
                     0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00}, 15, 0},
    {0xE1, (uint8_t[]){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
                     0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F}, 15, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};
static const ili9341_vendor_config_t panel_vendor = {
    .init_cmds = panel_init_commands,
    .init_cmds_size = sizeof(panel_init_commands) / sizeof(panel_init_commands[0]),
};

static esp_err_t initialize_touch(lv_display_t *display)
{
    if (!HW_FT6336G_ENABLED) { return ESP_OK; }
    touch_stage = "BUS";
    i2c_master_bus_handle_t bus = board_hardware_i2c_bus();
    if (!bus) { return ESP_ERR_INVALID_STATE; }
    touch_stage = "IO";
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG();
    io_config.dev_addr = HW_TOUCH_ADDRESS;
    io_config.scl_speed_hz = HW_I2C_HZ;
    esp_err_t result = esp_lcd_new_panel_io_i2c(bus, &io_config, &touch_io);
    if (result != ESP_OK) { return result; }
    touch_stage = "RESET";
    /* FT6336G 上电复位后留足启动时间，绕开第三方驱动固定的 10 ms 等待。 */
    if (HW_TOUCH_RST >= 0) {
        result = gpio_set_direction(HW_TOUCH_RST, GPIO_MODE_OUTPUT);
        if (result == ESP_OK) { result = gpio_set_level(HW_TOUCH_RST, 0); }
        if (result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            result = gpio_set_level(HW_TOUCH_RST, 1);
        }
        if (result != ESP_OK) {
            esp_lcd_panel_io_del(touch_io);
            touch_io = NULL;
            return result;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_err_t vendor_result = esp_lcd_panel_io_rx_param(touch_io, 0xA8, &touch_vendor_id, 1);
    esp_err_t chip_result = esp_lcd_panel_io_rx_param(touch_io, 0xA3, &touch_chip_id, 1);
    touch_ids_valid = vendor_result == ESP_OK && chip_result == ESP_OK;
    ESP_LOGI(TAG, "Touch IDs: vendor=0x%02X (%s), chip=0x%02X (%s)",
             touch_vendor_id, esp_err_to_name(vendor_result), touch_chip_id, esp_err_to_name(chip_result));
    esp_lcd_touch_config_t config = {
        .x_max = LCD_WIDTH, .y_max = LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = HW_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy = HW_TOUCH_SWAP_XY,
            .mirror_x = HW_TOUCH_MIRROR_X, .mirror_y = HW_TOUCH_MIRROR_Y,
        },
    };
    touch_stage = "DRIVER";
    result = esp_lcd_touch_new_i2c_ft6x36(touch_io, &config, &touch_handle);
    if (result == ESP_OK) {
        result = gpio_set_pull_mode(HW_TOUCH_INT, GPIO_PULLUP_ONLY);
    }
    if (result == ESP_OK) {
        /* 默认阈值 128 会漏掉轻触；降低检测阈值，但保留控制器自身的噪声过滤。 */
        touch_stage = "SENSE";
        const uint8_t threshold = TOUCH_THRESHOLD;
        result = esp_lcd_panel_io_tx_param(touch_io, 0x80, &threshold, sizeof(threshold));
    }
    if (result == ESP_OK) {
        /* FT6336G 的 G_MODE=2 为中断模式，触摸时 INT 拉低。 */
        const uint8_t interrupt_mode = 0x02;
        result = esp_lcd_panel_io_tx_param(touch_io, 0xA4, &interrupt_mode, sizeof(interrupt_mode));
    }
    if (result == ESP_OK) {
        touch_stage = "LVGL";
        lvgl_port_touch_cfg_t port = { .disp = display, .handle = touch_handle };
        touch_input = lvgl_port_add_touch(&port);
        if (!touch_input) { result = ESP_ERR_NO_MEM; }
        else {
            /* LVGL 需在按下和释放沿都读取状态，避免事件模式卡在 pressed。 */
            result = gpio_set_intr_type(HW_TOUCH_INT, GPIO_INTR_ANYEDGE);
            if (result != ESP_OK) { goto touch_cleanup; }
            ESP_LOGI(TAG, "FT6336G registered: addr=0x%02X SDA=%d SCL=%d RST=%d INT%d active-low",
                     HW_TOUCH_ADDRESS, HW_I2C_SDA, HW_I2C_SCL, HW_TOUCH_RST, HW_TOUCH_INT);
            ESP_LOGI(TAG, "Touch threshold=%d, interrupt mode", TOUCH_THRESHOLD);
        }
    }
touch_cleanup:
    if (result != ESP_OK) {
        if (touch_handle) { esp_lcd_touch_del(touch_handle); touch_handle = NULL; }
        esp_lcd_panel_io_del(touch_io);
        touch_io = NULL;
    }
    return result;
}

/* 此文件独占屏幕总线和外设资源；更换开发板时优先替换本适配层。 */
/* 只写硬件，不改缓存；休眠时写 0 不能覆盖用户保存的亮度。 */
static void backlight_write(unsigned percent)
{
    if (percent > 100) { percent = 100; }
    if (LCD_PIN_BL < 0) { return; }
    uint32_t level = ((uint32_t)percent * 1023u + 50u) / 100u;
    uint32_t duty = LCD_BACKLIGHT_ON_LEVEL ? level : (1023u - level);
    if (backlight_pwm) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        gpio_set_level(LCD_PIN_BL, percent ? LCD_BACKLIGHT_ON_LEVEL : !LCD_BACKLIGHT_ON_LEVEL);
    }
}

void display_port_brightness_set(unsigned percent)
{
    if (percent < LCD_BACKLIGHT_MIN_PERCENT) { percent = LCD_BACKLIGHT_MIN_PERCENT; }
    backlight_percent = percent;
    backlight_write(percent);
}

unsigned display_port_brightness_get(void)
{
    return backlight_percent;
}

void display_port_brightness_save(void)
{
    nvs_handle_t storage;
    if (nvs_open("device_ui", NVS_READWRITE, &storage) != ESP_OK) { return; }
    nvs_set_u8(storage, "brightness", (uint8_t)backlight_percent);
    nvs_commit(storage);
    nvs_close(storage);
}

static unsigned brightness_load(void)
{
    nvs_handle_t storage;
    uint8_t value = LCD_BACKLIGHT_DEFAULT_PERCENT;
    if (nvs_open("device_ui", NVS_READONLY, &storage) == ESP_OK) {
        nvs_get_u8(storage, "brightness", &value);
        nvs_close(storage);
    }
    if (value < LCD_BACKLIGHT_MIN_PERCENT || value > 100) {
        value = LCD_BACKLIGHT_DEFAULT_PERCENT;
    }
    return value;
}


esp_err_t display_port_init(lv_display_t **output, void (*create_pages)(lv_display_t *display))
{
    if (!output || !create_pages) { return ESP_ERR_INVALID_ARG; }
    *output = NULL;
    if (LCD_PIN_SCLK < 0 || LCD_PIN_MOSI < 0 || LCD_PIN_DC < 0) {
        ESP_LOGW(TAG, "LCD disabled: configure pins in weather_clock_config.h");
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_SCLK) || !GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_MOSI) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_DC) ||
        (LCD_PIN_CS >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_CS)) ||
        (LCD_PIN_RST >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_RST)) ||
        (LCD_PIN_BL >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(LCD_PIN_BL))) { return ESP_ERR_INVALID_ARG; }
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lv_display_t *display = NULL;
    bool bus_ready = false;
    bool port_ready = false;
    esp_err_t result;
    if (LCD_PIN_BL >= 0) {
        result = gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT);
        if (result != ESP_OK) { return result; }
        gpio_set_level(LCD_PIN_BL, !LCD_BACKLIGHT_ON_LEVEL);
        const ledc_timer_config_t timer_config = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LCD_BACKLIGHT_PWM_RESOLUTION,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = LCD_BACKLIGHT_PWM_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t channel_config = {
            .gpio_num = LCD_PIN_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        if (ledc_timer_config(&timer_config) == ESP_OK &&
            ledc_channel_config(&channel_config) == ESP_OK) {
            backlight_pwm = true;
        } else {
            ESP_LOGW(TAG, "Backlight PWM unavailable; using GPIO on/off");
        }
        backlight_percent = brightness_load();
        backlight_write(0);
    }
    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK, .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * DRAW_LINES * 2,
    };
    result = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) { goto cleanup; }
    bus_ready = true;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC, .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_SPI_CLOCK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 4,
    };
    result = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io);
    if (result != ESP_OK) { goto cleanup; }
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_USE_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&panel_vendor,
    };
    if (LCD_PANEL_TYPE == LCD_PANEL_ILI9341) {
        result = esp_lcd_new_panel_ili9341(io, &panel_config, &panel);
    } else {
        result = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    }
    if (result != ESP_OK) { goto cleanup; }
    active_panel = panel;
    result = esp_lcd_panel_reset(panel);
    if (result != ESP_OK) { goto cleanup; }
    result = esp_lcd_panel_init(panel);
    if (result != ESP_OK) { goto cleanup; }
    result = esp_lcd_panel_invert_color(panel, LCD_INVERT_COLOR);
    if (result != ESP_OK) { goto cleanup; }
    result = esp_lcd_panel_set_gap(panel, LCD_OFFSET_X, LCD_OFFSET_Y);
    if (result != ESP_OK) { goto cleanup; }
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_priority = 3;
    /* LVGL 任务栈需容纳全屏渲染与各二级页（含 WiFi 扫描结果列表）的对象创建；
     * 6144 字节会在渲染 AP 列表时栈溢出，加大到 10 KB 留足余量。 */
    port_config.task_stack = 10240;
    result = lvgl_port_init(&port_config);
    if (result != ESP_OK) { goto cleanup; }
    port_ready = true;
    ESP_LOGI(TAG, "LCD stage: LVGL port ready; adding display (hardware rotation)");
    /* 双份 DMA 缓冲交替绘制与发送，减少整屏切页时等待 SPI 传输的空档。 */
    lvgl_port_display_cfg_t display_config = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_WIDTH * DRAW_LINES, .double_buffer = true,
        .hres = LCD_WIDTH, .vres = LCD_HEIGHT, .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = LCD_SWAP_XY, .mirror_x = LCD_MIRROR_X, .mirror_y = LCD_MIRROR_Y,
        },
        .flags = { .buff_dma = true, .swap_bytes = true, .sw_rotate = false },
    };
    if (!lvgl_port_lock(1000)) { result = ESP_ERR_TIMEOUT; goto cleanup; }
    ESP_LOGI(TAG, "LCD stage: registration lock acquired; creating LVGL display");
    display = lvgl_port_add_disp(&display_config);
    lvgl_port_unlock();
    if (!display) { result = ESP_ERR_NO_MEM; goto cleanup; }
    /* 先完成输入注册并冻结诊断文字，再启动页面定时器，避免跨任务读写诊断缓冲区。 */
    result = initialize_touch(display);
    if (result != ESP_OK) {
        snprintf(touch_diagnostic, sizeof(touch_diagnostic), "TP %s\n%X", touch_stage, (unsigned)result);
        if (touch_ids_valid && touch_stage[0] == 'D') {
            snprintf(touch_diagnostic, sizeof(touch_diagnostic), "V%02X C%02X\n%X",
                     touch_vendor_id, touch_chip_id, (unsigned)result);
        }
        ESP_LOGW(TAG, "Optional touch disabled at %s: %s", touch_stage, esp_err_to_name(result));
    }
    ESP_LOGI(TAG, "LCD stage: display added; acquiring UI lock");
    if (!lvgl_port_lock(1000)) { result = ESP_ERR_TIMEOUT; goto cleanup; }
    *output = display;
    if (touch_input) {
        original_touch_read = lv_indev_get_read_cb(touch_input);
        lv_indev_set_read_cb(touch_input, read_touch_with_sound);
    }
    create_pages(display);
    lvgl_port_unlock();
    result = esp_lcd_panel_disp_on_off(panel, true);
    if (result != ESP_OK) { goto cleanup; }
    if (LCD_PIN_BL >= 0) {
        backlight_write(backlight_percent);
        ESP_LOGI(TAG, "LCD backlight on at %u%% (%s)", backlight_percent,
                 backlight_pwm ? "PWM" : "GPIO");
    }
    ESP_LOGI(TAG, "LCD stage: display initialization complete");
    return ESP_OK;

cleanup:
    ESP_LOGE(TAG, "LCD initialization failed: %s", esp_err_to_name(result));
    if (touch_input) { lvgl_port_remove_touch(touch_input); touch_input = NULL; }
    if (touch_handle) { esp_lcd_touch_del(touch_handle); touch_handle = NULL; }
    if (touch_io) { esp_lcd_panel_io_del(touch_io); touch_io = NULL; }
    if (display) { lvgl_port_remove_disp(display); }
    if (port_ready) { lvgl_port_deinit(); }
    if (panel) { esp_lcd_panel_del(panel); }
    active_panel = NULL;
    if (io) { esp_lcd_panel_io_del(io); }
    if (bus_ready) { spi_bus_free(SPI2_HOST); }
    *output = NULL;
    return result;
}

void display_port_prepare_sleep(void)
{
    if (!active_panel) { return; }
    if (lvgl_port_lock(1000)) {
        esp_lcd_panel_disp_on_off(active_panel, false);
        esp_lcd_panel_disp_sleep(active_panel, true);
        if (LCD_PIN_BL >= 0) { backlight_write(0); }
        lvgl_port_unlock();
    }
    /* 释放触摸中断锁存，避免刚进入深睡就被仍为低电平的 INT 再次唤醒。 */
    for (unsigned attempt = 0; attempt < 150 && gpio_get_level(HW_TOUCH_INT) == 0; ++attempt) {
        esp_lcd_touch_read_data(touch_handle);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}