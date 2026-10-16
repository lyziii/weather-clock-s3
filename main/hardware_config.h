#pragma once

/* S3 板载触摸与外接传感器启用；缺失器件只影响自身数据。 */
#define HW_MPU6050_ENABLED 1
#define HW_ATGM336H_ENABLED 1
#define HW_MMC5983_ENABLED 1
#define HW_FT6336G_ENABLED 1
#define HW_AUTO_ROTATE_ENABLED 1

#if HW_AUTO_ROTATE_ENABLED && !HW_MPU6050_ENABLED
#error "Automatic rotation requires MPU6050"
#endif

/* 预留 I2C 与 FT6336G、ES8311 共用，所有设备复用同一总线实例。 */
#define HW_I2C_SDA 16
#define HW_I2C_SCL 15
#define HW_I2C_HZ 100000
/* MPU 地址沿用旧板实测结果；新板接线后仍须确认模块地址跳线。 */
#define HW_MPU6050_ADDRESS 0x68
#define HW_MMC5983_ADDRESS 0x30
#define HW_TOUCH_ADDRESS 0x38
#define HW_AUDIO_ADDRESS 0x18

/* UART1 经矩阵映射到预留串口，控制台独占原生 USB。 */
#define HW_GNSS_UART 1
#define HW_GNSS_RX 44
#define HW_GNSS_TX 43
#define HW_GNSS_BAUD 9600

#define HW_TOUCH_RST 18
#define HW_TOUCH_INT 17
#define HW_TOUCH_SWAP_XY 0
#define HW_TOUCH_MIRROR_X 0
#define HW_TOUCH_MIRROR_Y 0

/* 仅保留音频硬件资源，不初始化尚无播放/录音需求的 ES8311。 */
#define HW_AUDIO_ENABLE_GPIO 1
#define HW_AUDIO_ENABLE_LEVEL 0

/* 两路均为二分之一分压；VBUS 阈值是调试起点，须在负载下实测。 */
#define HW_BATTERY_GPIO 9
#define HW_POWER_GPIO 14
#define HW_BATTERY_DIVIDER 2
#define HW_POWER_DIVIDER 2
#define HW_VBUS_ON_MV 4350
#define HW_VBUS_OFF_MV 4250

/* 固定安装：原始 X=-1g 为正向竖屏、X=+1g 为倒置；Y=+1g/-1g 对应 90/270 度。 */
#define HW_IMU_SCREEN_X_AXIS 1
#define HW_IMU_SCREEN_Y_AXIS 0
#define HW_IMU_SCREEN_X_SIGN 1
#define HW_IMU_SCREEN_Y_SIGN 1
#define HW_ROTATE_STABLE_SAMPLES 20

/* 骑行默认 +Y 朝车头、Z 朝上；水平路面安装俯仰角用于抵消固定安装偏置。 */
#define HW_RIDE_FORWARD_AXIS 1
#define HW_RIDE_FORWARD_SIGN 1
#define HW_RIDE_PITCH_OFFSET_DEG 0.0f

#if HW_RIDE_FORWARD_AXIS < 0 || HW_RIDE_FORWARD_AXIS > 2 || \
	(HW_RIDE_FORWARD_SIGN != 1 && HW_RIDE_FORWARD_SIGN != -1)
#error "Invalid ride forward axis or sign"
#endif