#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* 有效标志和单调时间戳用于区分真实读数、未连接和过期数据。 */
typedef enum {
    BOARD_PLACEMENT_UNKNOWN,
    BOARD_PLACEMENT_UPRIGHT,
    BOARD_PLACEMENT_FLAT,
} board_placement_t;

/* 当前判定阶段独立于上一次稳定朝向，便于定位运动门限和安装方向问题。 */
typedef enum {
    BOARD_ORIENTATION_NO_DATA,
    BOARD_ORIENTATION_MOVING,
    BOARD_ORIENTATION_TILTED,
    BOARD_ORIENTATION_STABILIZING,
    BOARD_ORIENTATION_FLAT,
    BOARD_ORIENTATION_READY,
} board_orientation_state_t;

typedef struct {
    esp_err_t initialization_result;
    esp_err_t imu_read_result;
    bool imu_read_attempted;
    bool imu_initialized;
    bool imu_probe_done;
    uint8_t imu_ack_mask;
    unsigned orientation_samples;
    board_orientation_state_t orientation_state;
    bool imu_valid;
    int64_t imu_us;
    float acceleration_g[3];
    float gyro_dps[3];
    /* 传感器坐标到参考坐标的单位四元数，顺序 w/x/y/z；偏航仅为相对角。 */
    bool attitude_valid;
    float attitude_quaternion[4];
    /* 坡度为百分比；前向线性加速度已扣重力，正值为加速，均为 IMU 估计值。 */
    bool ride_grade_valid;
    bool ride_acceleration_valid;
    float ride_grade_percent;
    float ride_acceleration_mps2;
    board_placement_t placement;
    bool rotation_valid;
    unsigned rotation_degrees;
    bool magnetic_initialized;
    bool magnetic_ack;
    uint8_t magnetic_product_id;
    uint8_t magnetic_status;
    esp_err_t magnetic_init_result;
    esp_err_t magnetic_read_result;
    bool magnetic_valid;
    int64_t magnetic_us;
    float magnetic_gauss[3];
    bool gnss_uart_ready;
    esp_err_t gnss_uart_result;
    uint32_t gnss_rx_bytes;
    uint32_t gnss_tx_bytes;
    int64_t gnss_rx_us;
    uint32_t gnss_valid_sentences;
    uint32_t gnss_checksum_errors;
    uint32_t gnss_format_errors;
    uint32_t gnss_parse_errors;
    bool gnss_stream_active;
    char gnss_last_sentence[6];
    unsigned gnss_fix_quality;
    bool gnss_valid;
    int64_t gnss_us;
    double latitude;
    double longitude;
    float speed_kmh;
    bool altitude_valid;
    int64_t altitude_us;
    float altitude_m;
    unsigned satellites;
} board_hardware_snapshot_t;

/* 初始化失败会回收已分配资源；默认配置下无硬件访问。 */
esp_err_t board_hardware_init(void);
bool board_hardware_snapshot(board_hardware_snapshot_t *snapshot);
i2c_master_bus_handle_t board_hardware_i2c_bus(void);
/* GNSS 发包统一经此接口，保证页面发送计数反映实际 UART 写入量。 */
esp_err_t board_hardware_gnss_write(const void *data, size_t size);