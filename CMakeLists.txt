cmake_minimum_required(VERSION 3.22)

# 独立 S3 工程，不读取上一级 ESP32 的 sdkconfig 或构建缓存。
set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "This board requires IDF_TARGET=esp32s3")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(weather_clock_s3)