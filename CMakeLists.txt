cmake_minimum_required(VERSION 3.22)

# 独立 S3 工程，不读取上一级 ESP32 的 sdkconfig 或构建缓存。
set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "This board requires IDF_TARGET=esp32s3")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(weather_clock_s3)

# 字库单独烧入数据分区，避免占用接近容量上限的应用分区。
set(ZH_FONT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fonts/ch16.bin")
if(NOT EXISTS "${ZH_FONT_SOURCE}")
    message(FATAL_ERROR "Missing fonts/ch16.bin; see fonts/README.md to generate it")
endif()
file(SIZE "${ZH_FONT_SOURCE}" ZH_FONT_SIZE)
file(READ "${ZH_FONT_SOURCE}" ZH_FONT_MAGIC OFFSET 4 LIMIT 4 HEX)
partition_table_get_partition_info(ZH_FONT_PARTITION_SIZE "--partition-name zh_font" "size")
if(NOT ZH_FONT_PARTITION_SIZE OR ZH_FONT_SIZE LESS 8 OR
   ZH_FONT_SIZE GREATER ZH_FONT_PARTITION_SIZE OR NOT ZH_FONT_MAGIC STREQUAL "68656164")
    message(FATAL_ERROR "fonts/ch16.bin must be a valid LVGL binfont fitting the zh_font partition")
endif()
configure_file("${ZH_FONT_SOURCE}" "${CMAKE_BINARY_DIR}/fonts/ch16.bin" COPYONLY)
esptool_py_flash_to_partition(flash "zh_font" "${CMAKE_BINARY_DIR}/fonts/ch16.bin")