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
/* 地磁模块安装方向：默认 +Y 朝设备上沿、+X 朝右，转向以实机核对为准。 */
#define HW_MAG_FORWARD_AXIS 1
#define HW_MAG_RIGHT_AXIS 0
#define HW_MAG_FORWARD_SIGN 1
#define HW_MAG_RIGHT_SIGN 1
#if HW_MAG_FORWARD_AXIS < 0 || HW_MAG_FORWARD_AXIS > 2 || HW_MAG_RIGHT_AXIS < 0 || HW_MAG_RIGHT_AXIS > 2 || HW_MAG_FORWARD_AXIS == HW_MAG_RIGHT_AXIS
#error "Invalid magnetometer heading axes"
#endif
#if (HW_MAG_FORWARD_SIGN != 1 && HW_MAG_FORWARD_SIGN != -1) || (HW_MAG_RIGHT_SIGN != 1 && HW_MAG_RIGHT_SIGN != -1)
#error "Invalid magnetometer heading signs"
#endif
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

/* ES8311 输出到板载功放；仅用于触摸短音，不启动麦克风采集。 */
#define HW_AUDIO_ENABLE_GPIO 1
#define HW_AUDIO_ENABLE_LEVEL 0
#define HW_AUDIO_MCLK 4
#define HW_AUDIO_BCLK 5
/* 按最新板卡表，ESP32 播放输出使用 IO8；IO6 是录音输入，当前不启用。 */
#define HW_AUDIO_DOUT 8
#define HW_AUDIO_WS 7
/* 触摸音默认音量；范围 0~100，同时作用于触摸音和诊断音。 */
#define HW_AUDIO_CLICK_VOLUME 60

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