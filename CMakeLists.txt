cmake_minimum_required(VERSION 3.22)

# 独立 S3 工程，不读取上一级 ESP32 的 sdkconfig 或构建缓存。
set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "This board requires IDF_TARGET=esp32s3")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(weather_clock_s3)

# 中文字库：把原始 ch16.bin 原样烧进 partitions.csv 里的 font 数据分区，运行期由
# zh_font.c 用 esp_partition_mmap 直接读 Flash，不再依赖 TF 卡。字库文件缺失时只
# 告警，固件照常构建（界面回退英文）。
set(ZH_FONT_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/main/fonts/ch16.bin")
if(EXISTS "${ZH_FONT_IMAGE}")
    esptool_py_flash_to_partition(flash font "${ZH_FONT_IMAGE}")
else()
    message(WARNING "main/fonts/ch16.bin not found: firmware will fall back to English")
endif()
