# 太空人动画来源

- 上游：https://github.com/kssssxg/esp32s3-cyber-os
- 参考提交：`3fc99dd0ffe34df9bbafc87e38f977478a371823`。
- 授权：上游 README 声明 MIT，文本保留于 [LICENSE](LICENSE)。
- 引入范围：仅太空人旋转动画，不包含上游完整钟面、工具桌面或其他功能。

## 资源

原项目 `i0.h` 至 `i9.h` 的十帧 JPEG 共 25,303 字节，均为 70 x 70。本工程将其解码、去除白底后生成 A8 透明单色蒙版，保留抗锯齿线条；十帧像素共 49,000 字节。蒙版由 LVGL 根据主题绘成白色或黑色，不再保留原始不透明 JPEG 字节；不包含温湿度图标和上下装饰图。

当前工程提交生成后的 [../../main/cyber_assets.c](../../main/cyber_assets.c)，正常构建无需访问上游或安装 Node。需要重新生成时，从上述固定提交下载十个原始头文件到同一目录，使用 Node.js 运行：

```sh
cd tools
npm ci
cd ..
node tools/generate_cyber_assets.mjs <原始头文件目录> main/cyber_assets.c
```

[../../tools/generate_cyber_assets.mjs](../../tools/generate_cyber_assets.mjs) 使用固定版本 `jpeg-js`，检查 JPEG 起止标记、片段边界、70 x 70 尺寸和 baseline 8-bit 编码；对亮度高于 243 的浅色噪点透明化，其余像素按亮度映射到透明度，并检查每帧非空、四角透明。生成资源使用 `LV_COLOR_FORMAT_A8`，运行时不需要 JPEG 解码；图片留在应用 Flash 中，中文字库仍使用独立分区。

可添加第三个参数输出 JPEG 对照预览，从上到下为原图、黑底白线和白底黑线：

```sh
node tools/generate_cyber_assets.mjs <原始头文件目录> build/cyber_assets-check.c build/astronaut-themes.jpg
```

生成器也接受尚未转换的 JPEG C 资源作为输入；不要将已经转换的 A8 C 文件再次作为 JPEG 输入。

## 平台适配

本工程使用 ESP-IDF 6.1 和 LVGL 9 的 `lv_animimg` 播放，十帧一轮 300 ms，与原项目每帧约 30 ms 的节奏一致。在原有天气钟面中，动画代替温度右侧的天气图标，横竖屏都以温度文字中心对齐。天气变化仍通过原天气文字显示，不修改其联网服务。

下拉面板的 `White` 开关仅切换天气钟面和太空人线条，NVS 保存主题；天气区域与屏幕使用相同背景色，切换时不重新创建或重启动画。

没有工具桌面、闹钟、AI 或计算器，也不再依赖 TinyExpr。原系统页、码表、音乐、启动页记忆、睡眠、Flash 字库和手动旋转功能保持原有行为。

## 验证状态

已验证资源再生成一致性、十帧透明角点与非空主体、黑白合成预览、ESP-IDF 编译及分区容量。本轮尚未烧录，四向布局、触摸切换、NVS 重启恢复与播放流畅度仍需真机验收。工程说明见 [../../README.md](../../README.md)。