#pragma once

/* 用户确认的 S3 板载 ILI9341V，竖屏基准方向 0，实机可调整镜像。 */
#define LCD_PANEL_ST7789 0
#define LCD_PANEL_ILI9341 1
#define LCD_PANEL_TYPE LCD_PANEL_ILI9341
#if LCD_PANEL_TYPE != LCD_PANEL_ST7789 && LCD_PANEL_TYPE != LCD_PANEL_ILI9341
#error "Unsupported LCD panel type"
#endif
#define LCD_WIDTH 240
#if LCD_PANEL_TYPE == LCD_PANEL_ILI9341
#define LCD_HEIGHT 320
#else
#define LCD_HEIGHT 240
#endif

/* LCD 复位与 CHIP_PU 共用，只能软件复位，禁止拉低 MCU 使能。 */
#define LCD_PIN_SCLK 12
#define LCD_PIN_MOSI 11
#define LCD_PIN_CS   10
#define LCD_PIN_DC   46
#define LCD_PIN_RST  -1
/* 板载背光高电平开启；45/46 的启动电平由硬件保证。 */
#define LCD_PIN_BL   45
#define LCD_BACKLIGHT_ON_LEVEL 1
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)

/* RGB565 全帧在 40 MHz 下约需 31 ms；须实机核对信号完整性及花屏情况。 */
#define LCD_OFFSET_X 0
#define LCD_OFFSET_Y 0
#define LCD_SWAP_XY false
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y false
#define LCD_INVERT_COLOR true
#define LCD_USE_BGR true

/* 天气坐标独立于时区；默认北京，城市名使用英文以节省中文字库空间。 */
#define WEATHER_CITY "BEIJING"
#define WEATHER_CITY_ZH "北京"
#define WEATHER_LATITUDE "39.9042"
#define WEATHER_LONGITUDE "116.4074"
#define WEATHER_REFRESH_SECONDS (15 * 60)