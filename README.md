# ESP32-S3 Weather Clock

基于 ESP-IDF 和 LVGL 的触摸天气时钟、骑行码表与 IMU 姿态显示工程。

## 功能

- 时钟、天气、骑行码表和 MPU6050 姿态诊断。
- 右滑依次切换 Clock、Ride、IMU，左滑返回；首尾不循环。
- 翻页圆点短暂显示后淡出，三页采用全屏布局。
- 码表页提供开始、暂停与二次确认清零；无有效传感器数据时显示未知值。
- 仅时钟页自动旋转，码表和 IMU 页固定正向。

## 硬件

目标为 ESP32-S3R8，外部 16 MB Flash，封装内 8 MB Octal PSRAM。

| 外设 | 配置 |
| --- | --- |
| ILI9341V 屏幕 | 240 x 320，SPI，CS10 / MOSI11 / SCLK12 / DC46 / BL45 |
| 屏幕复位 | 接 EN，不使用独立复位 GPIO |
| 共享 I2C | SDA16 / SCL15，100 kHz |
| FT6336G 触摸 | 地址 0x38，RST18；目前使用 10 ms 轮询，INT17 未启用 |
| MPU6050 | 地址 0x68 |
| MMC5983MA | 地址 0x30 |
| GNSS | UART1，RX44 / TX43，9600 baud |
| 电压采样 | 电池 GPIO9，供电来源 GPIO14；分压及阈值需匹配实际电路 |
| 控制台 | 原生 USB Serial/JTAG |

固定安装方向按加速度重力分量判定：X 约 -1 g 为正向竖屏，X 约 +1 g 为倒置；Y 约 +1 / -1 g 分别对应 90 / 270 度。姿态须稳定后才切换方向。

引脚与安装参数见 [main/hardware_config.h](main/hardware_config.h)，显示和天气参数见 [main/weather_clock_config.h](main/weather_clock_config.h)。ES8311 音频接口仅预留，尚未实现播放或录音。

## 构建与烧录

使用已配置环境的 ESP-IDF 6.1，联网获取组件依赖。在本项目目录执行：

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COM12 flash monitor
```

`COM12` 是当前开发板示例端口，其他机器须替换为实际端口。也可通过 VS Code ESP-IDF 扩展选中本项目，并将目标设为 `esp32s3` 后构建、烧录。

组件版本由 [main/idf_component.yml](main/idf_component.yml) 和 [dependencies.lock](dependencies.lock) 管理。Flash、PSRAM、USB 控制台及字体默认值见 [sdkconfig.defaults](sdkconfig.defaults)。下载的组件与构建目录不进入仓库。

## 源码结构

- `main/hello_world_main.c`：应用入口、网络和控制台。
- `main/weather_clock.c`：页面布局、模式与数据刷新。
- `main/touch_controls.c`：滑动翻页、分页点淡出和骑行按钮。
- `main/display_port.c`：LCD、LVGL 和触摸适配。
- `main/board_hardware.c`、`main/board_power.c`：传感器与电压采样。
- `main/imu_view.c`：姿态箭头和 IMU 诊断。
- `main/ride_model.c`、`main/weather_service.c`：骑行状态与天气服务。

## 公开上传注意事项

不提交 Wi-Fi 凭据、访问令牌、本机 SDK 路径或构建产物。首次使用前应核对引脚、电阻分压和传感器安装参数。电压估算不等同于充电状态，GNSS 无有效定位时不生成虚构速度或里程。