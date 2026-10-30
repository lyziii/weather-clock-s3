# Flash 中文字库

`ch16.bin` 是由 Noto Sans SC 生成的 LVGL 二进制字体，使用 SIL Open Font License 1.1，授权原文见本目录的 `OFL.txt`。字库随工程保留，正常构建无需安装 Node 或连接 SD 卡。

## 格式与范围

- 16 px、4 bpp、未压缩；需要 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`。
- 范围：`0x20-0x7E,0xA0-0xFF,0x2000-0x206F,0x3000-0x303F,0x4E00-0x9FA5,0xFF00-0xFFEF`，未包含的字形由 LVGL 回退字体处理。
- 字体源：[Google Fonts / Noto Sans SC](https://github.com/google/fonts/tree/main/ofl/notosanssc)，源文件为 `NotoSansSC[wght].ttf`。
- 转换器：`lv_font_conv@1.5.3`。
- 当前字库：2738992 字节，SHA-256 为 `bda01fa96311411a432f5d18afb5061393c2f3070f94cf7e2896f3af2b1eb85b`。

需要重新生成时，在本目录准备上述 TTF，并在已安装 Node.js 的终端执行：

```sh
npx --yes --package lv_font_conv@1.5.3 lv_font_conv --font "NotoSansSC[wght].ttf" --size 16 --bpp 4 -r "0x20-0x7E,0xA0-0xFF,0x2000-0x206F,0x3000-0x303F,0x4E00-0x9FA5,0xFF00-0xFFEF" --no-compress --format bin --output ch16.bin
```

## 构建与烧录

根 CMake 检查字库头和分区容量，将文件复制为 `build/fonts/ch16.bin`，并自动加入完整烧录清单。`zh_font` 分区位于 `0x200000`，容量 4 MiB。

首次迁移必须使用完整 `idf.py flash`，同时更新分区表、应用和字库；不要仅烧录 app。分区表保留原应用、NVS 和 PHY 地址，无需全片擦除。

后续字库不变时可用 `idf.py app-flash` 只更新应用，保留字库。完整 `flash` 始终包含字库；更换字库后应重新构建并完整烧录。

运行时由低优先级任务映射 Flash 分区，解析完成后释放映射，字形常驻 PSRAM。未烧录字库或加载失败时记录日志并回退英文显示，不会等待 SD 卡。