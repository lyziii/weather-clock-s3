# ESP32-S3 Weather Clock

基于 ESP-IDF 和 LVGL 的触摸天气时钟、骑行码表与 IMU 姿态显示工程。

## 功能

- 时钟、天气、骑行码表和 MPU6050 姿态诊断。
- 右滑依次切换 Clock、System、Ride，左滑返回；首尾不循环，IMU 不再独占一级标签。
- 翻页圆点短暂显示后淡出，三页采用全屏布局。
- System（系统）一级页提供 Music、TF 容量与文件、Gyroscope、Magnetometer 和 GPS 二级入口；菜单可上下滑动并显示滚动条，视频仍不可用。
- 陀螺仪二级页保留三轴加速度、角速度、姿态图和诊断状态，可纵向滚动查看；地磁计页显示三轴磁场、总强度与数据年龄，GPS 页显示定位、卫星数、经纬度、速度与海拔。未接入、无定位或过期数据明确显示未知，不生成虚假读数。
- 二级页点击返回键或左滑回系统菜单，系统内容区上下滑动不会展开电源面板；从标题栏下拉仍可展开睡眠面板。
- TF 挂载成功后所有标题栏显示卡图标；后台检测拔出和重插，失败不自动格式化。
- TF 文件页显示文件系统总量、剩余量和已用量，支持目录进入、返回上级、列表滚动、分页和查看文件大小，不写入或删除文件。
- 冷启动及三击触摸唤醒后先显示横线展开、L-Vectra 渐显的约 1.8 秒动画，再显示保存的一级标签页。
- 码表页提供开始、暂停与二次确认清零；无有效传感器数据时显示未知值。
- 仅时钟页自动旋转，系统及其子页、码表固定正向。
- 正常运行时每次手指落屏播放一次短促触摸音，空白区域与禁用控件同样有效；长按或不离屏滑动不会连响。

触摸音使用 ES8311 DAC 和 FM8002E 功放，PCM 由独立任务输出，不读取 TF 文件。初始音量参数 `HW_AUDIO_CLICK_VOLUME=60`，可在 [main/hardware_config.h](main/hardware_config.h) 调整（0～100），同时作用于触摸音和诊断音；修正接线后应从低音量逐步试听。音频初始化失败只关闭提示音，不阻断界面。进入深睡前静音并保持 GPIO1 为关闭电平；深睡三击门控期间不启动音频，完整唤醒后恢复触摸音。

TF 卡使用已有 FAT16/FAT32 文件系统（32 GB 卡通常为 FAT32）；不支持的格式会报告挂载错误，不改写卡。容量单位为 GiB，32 GB 标称卡显示约 29.8 GiB 属正常现象。文件系统 API 使用 UTF-8 长文件名；当前 Montserrat 字体不含中文字形，中文名称可能显示缺字。二级页可点击返回键或左滑回系统首页；查看文件不等于打开、解码或播放内容，音乐播放仍未实现。

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
| TF 卡 | SDMMC 4 位，CLK38 / CMD40 / D0=39 / D1=41 / D2=48 / D3=47；无独立插卡检测引脚 |
| ES8311 / FM8002E | I2C 地址 0x18，共享 SDA16 / SCL15；I2S MCLK4 / BCLK5 / DOUT8 / WS7；GPIO6 录音输入暂不启用；功放 GPIO1 低有效 |
| 电压采样 | 电池 GPIO9，供电来源 GPIO14；分压及阈值需匹配实际电路 |
| 控制台 | 原生 USB Serial/JTAG |

固定安装方向按加速度重力分量判定：X 约 -1 g 为正向竖屏，X 约 +1 g 为倒置；Y 约 +1 / -1 g 分别对应 90 / 270 度。姿态须稳定后才切换方向。

引脚与安装参数见 [main/hardware_config.h](main/hardware_config.h)，显示和天气参数见 [main/weather_clock_config.h](main/weather_clock_config.h)。ES8311 已接入触摸音，音乐文件播放与录音仍未实现。

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
- `main/tf_card.c`：SDMMC 挂载、状态探测和只读容量、目录快照。
- `main/entertainment_view.c`：系统滚动菜单、音乐/TF 文件/陀螺仪/地磁计/GPS 二级导航及快照显示。
- `main/touch_sound.c`：ES8311 初始化、异步触摸短音和休眠静音。
- `main/ride_model.c`、`main/weather_service.c`：骑行状态与天气服务。

## 公开上传注意事项

不提交 Wi-Fi 凭据、访问令牌、本机 SDK 路径或构建产物。首次使用前应核对引脚、电阻分压和传感器安装参数。电压估算不等同于充电状态，GNSS 无有效定位时不生成虚构速度或里程。