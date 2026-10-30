# Cyber OS 工具移植来源

- 上游：https://github.com/kssssxg/esp32s3-cyber-os
- 参考提交：`3fc99dd0ffe34df9bbafc87e38f977478a371823`。
- 授权：上游 README 声明 MIT，文本保留于 [LICENSE](LICENSE)。
- 移植范围：太空人钟面、工具桌面、三组闹钟、番茄钟、秒表、倒计时、日历、标准/科学/绘图计算器及 AI 助手。

## 资源

原项目的 `i0.h` 至 `i9.h`、`temperature.h`、`humidity.h`、`watch_top.h`、`watch_bottom.h` 中 JPEG 字节未经重新压缩，转换成当前工程的 LVGL image descriptor。十四张图片共 66,186 字节，十帧太空人均为 70 x 70；温湿度图标为 24 x 24，上下装饰图为 240 x 20。

当前工程提交生成后的 [../../main/cyber_assets.c](../../main/cyber_assets.c)，正常构建无需访问上游或安装 Node。需要重新生成时，从上述固定提交下载十四个原始头文件到同一目录，使用 Node.js 运行：

```sh
node tools/generate_cyber_assets.mjs <原始头文件目录> main/cyber_assets.c
```

[../../tools/generate_cyber_assets.mjs](../../tools/generate_cyber_assets.mjs) 会检查 JPEG 起止标记、片段边界、尺寸和 baseline 8-bit 编码；不接受 TJPGD 不支持的渐进式 JPEG。项目启用 LVGL TJPGD 解码器，图片留在应用 Flash 中，中文字库仍使用独立分区。

## 平台适配

上游使用 Arduino、TFT_eSPI、240 x 240 屏幕和实体按键。本工程用 ESP-IDF 6.1、LVGL 9 和触摸重写界面与事件处理，支持完整 240 x 320 / 320 x 240 布局。保留钟面原图，但桌面图标、控件、科学表达式引擎、日历和 AI 配置交互并非上游逐像素实现；不声称完成真机视觉一致性验收。

没有移植游戏、BLE、原仓库音乐或文件系统模块，也没有复制上游服务密钥。铃声对接当前工程已有的 PCM WAV 播放器；AI 使用用户配置的 OpenAI 兼容 HTTPS 接口。原天气联网、系统页、码表、Flash 字库和手动旋转功能保留。

## 表达式引擎

- TinyExpr 上游：https://github.com/codeplea/tinyexpr
- 固定提交：`ba2e32bbcc2d1e644a7e132d24c2f542821cabe0`。
- 代码及原始授权：[../tinyexpr/tinyexpr.c](../tinyexpr/tinyexpr.c)、[../tinyexpr/tinyexpr.h](../tinyexpr/tinyexpr.h)、[../tinyexpr/LICENSE](../tinyexpr/LICENSE)。
- 三个文件均已与该提交的 Git blob SHA 核对一致，未修改第三方源码。当前组件启用 `TE_POW_FROM_RIGHT`，三角函数按界面 RAD/DEG 选项提供包装。

## 验证状态

已验证资源再生成、第三方来源哈希、ESP-IDF 编译及分区容量。本轮尚未烧录和实测屏幕、触摸、音频、网络或长时间计时。使用限制与真机验收项见 [../../README.md](../../README.md)。