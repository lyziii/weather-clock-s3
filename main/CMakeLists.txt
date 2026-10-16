idf_component_register(SRCS "hello_world_main.c" "weather_clock.c" "ride_model.c" "board_hardware.c"
                            "ui_status_bar.c" "display_port.c" "imu_view.c" "weather_service.c" "board_power.c"
                            "touch_controls.c"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_driver_uart esp_driver_usb_serial_jtag
                                     esp_driver_spi esp_driver_gpio esp_lcd esp_lcd_ili9341 esp_lvgl_port lvgl
                                     esp_http_client mbedtls cjson esp_timer esp_driver_i2c esp_adc
                                     esp_mpu6050 libnmea esp_lcd_touch_ft6x36
                       INCLUDE_DIRS "")
