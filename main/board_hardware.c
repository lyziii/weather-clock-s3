#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mpu6050.h"
#include "nmea.h"
#include "gprmc.h"
#include "gpgga.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "weather_clock_config.h"

static i2c_master_bus_handle_t i2c_bus;
static mpu6050_handle_t imu;
static i2c_master_dev_handle_t magnetometer;
static SemaphoreHandle_t snapshot_mutex;
static board_hardware_snapshot_t latest;
static bool gnss_uart_installed;
/* 初始化结束后保留诊断信息，即使任务和总线已回收也能在屏幕显示失败原因。 */
static esp_err_t initialization_result = ESP_ERR_INVALID_STATE;
static bool imu_probe_done;
static uint8_t imu_ack_mask;

static esp_err_t magnetic_read(uint8_t reg, void *data, size_t size)
{
    return i2c_master_transmit_receive(magnetometer, &reg, 1, data, size, 30);
}

static esp_err_t magnetic_write(uint8_t reg, uint8_t value)
{
    uint8_t packet[] = {reg, value};
    return i2c_master_transmit(magnetometer, packet, sizeof(packet), 30);
}

/* MMC5983MA 的三轴输出为 18 位偏移二进制，中心值 131072，32768 LSB/G。 */
static void sample_magnetic(board_hardware_snapshot_t *sample)
{
    uint8_t status;
    uint8_t raw[7];
    if (magnetic_read(0x08, &status, 1) != ESP_OK) {
        sample->magnetic_valid = false;
        return;
    }
    if (status & 0x01) {
        if (magnetic_read(0x00, raw, sizeof(raw)) == ESP_OK) {
            uint32_t axes[] = {
                ((uint32_t)raw[0] << 10) | ((uint32_t)raw[1] << 2) | (raw[6] >> 6),
                ((uint32_t)raw[2] << 10) | ((uint32_t)raw[3] << 2) | ((raw[6] >> 4) & 3),
                ((uint32_t)raw[4] << 10) | ((uint32_t)raw[5] << 2) | ((raw[6] >> 2) & 3),
            };
            for (unsigned axis = 0; axis < 3; ++axis) {
                sample->magnetic_gauss[axis] = ((int32_t)axes[axis] - 131072) / 32768.0f;
            }
            sample->magnetic_valid = true;
            sample->magnetic_us = esp_timer_get_time();
        } else { sample->magnetic_valid = false; }
    }
    /* 自动 SET/RESET 后触发单次测量，下个采样周期再检查完成位。 */
    if (magnetic_write(0x09, 0x21) != ESP_OK) { sample->magnetic_valid = false; }
}

static void recognize_placement(board_hardware_snapshot_t *sample)
{
    static float filtered[3];
    static bool filter_ready;
    static unsigned stable_samples;
    static unsigned previous_rotation;
    static board_placement_t previous_placement;
    sample->orientation_samples = 0;
    if (!sample->imu_valid) {
        stable_samples = 0;
        filter_ready = false;
        sample->orientation_state = BOARD_ORIENTATION_NO_DATA;
        return;
    }
    float norm = 0;
    float rotation_rate = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        norm += sample->acceleration_g[axis] * sample->acceleration_g[axis];
        rotation_rate += sample->gyro_dps[axis] * sample->gyro_dps[axis];
        filtered[axis] = filter_ready ? filtered[axis] * 0.8f + sample->acceleration_g[axis] * 0.2f :
                                        sample->acceleration_g[axis];
    }
    filter_ready = true;
    /* 加速或快速旋转时不改变朝向；必须静稳约一秒后才提交。 */
    if (norm < 0.8f * 0.8f || norm > 1.2f * 1.2f || rotation_rate > 15.0f * 15.0f) {
        stable_samples = 0;
        sample->orientation_state = BOARD_ORIENTATION_MOVING;
        return;
    }
    float screen_x = filtered[HW_IMU_SCREEN_X_AXIS] * HW_IMU_SCREEN_X_SIGN;
    float screen_y = filtered[HW_IMU_SCREEN_Y_AXIS] * HW_IMU_SCREEN_Y_SIGN;
    unsigned screen_z_axis = 3 - HW_IMU_SCREEN_X_AXIS - HW_IMU_SCREEN_Y_AXIS;
    board_placement_t placement;
    unsigned rotation = sample->rotation_degrees;
    if (fabsf(filtered[screen_z_axis]) > 0.85f) {
        placement = BOARD_PLACEMENT_FLAT;
    } else if (fabsf(screen_y) > 0.75f && fabsf(screen_y) > fabsf(screen_x) + 0.2f) {
        placement = BOARD_PLACEMENT_UPRIGHT;
        rotation = screen_y < 0 ? 0 : 180;
    } else if (fabsf(screen_x) > 0.75f && fabsf(screen_x) > fabsf(screen_y) + 0.2f) {
        placement = BOARD_PLACEMENT_UPRIGHT;
        rotation = screen_x > 0 ? 90 : 270;
    } else {
        stable_samples = 0;
        sample->orientation_state = BOARD_ORIENTATION_TILTED;
        return;
    }
    if (placement != previous_placement || rotation != previous_rotation) {
        previous_placement = placement;
        previous_rotation = rotation;
        stable_samples = 0;
    }
    if (stable_samples < HW_ROTATE_STABLE_SAMPLES) { ++stable_samples; }
    sample->orientation_samples = stable_samples;
    sample->orientation_state = BOARD_ORIENTATION_STABILIZING;
    if (stable_samples >= HW_ROTATE_STABLE_SAMPLES) {
        sample->placement = placement;
        sample->orientation_state = placement == BOARD_PLACEMENT_FLAT ?
            BOARD_ORIENTATION_FLAT : BOARD_ORIENTATION_READY;
        if (placement == BOARD_PLACEMENT_UPRIGHT) {
            sample->rotation_valid = true;
            sample->rotation_degrees = rotation;
        }
    }
}

/* 四元数避免竖立时欧拉角积分奇异；重力仅修正倾斜，不提供绝对航向。 */
static void update_attitude(board_hardware_snapshot_t *sample)
{
    static int64_t previous_us;
    if (!sample->imu_valid) {
        previous_us = 0;
        sample->attitude_valid = false;
        return;
    }
    float gravity[3];
    float magnitude_squared = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!isfinite(sample->acceleration_g[axis]) || !isfinite(sample->gyro_dps[axis])) {
            previous_us = 0;
            sample->attitude_valid = false;
            return;
        }
        gravity[axis] = sample->acceleration_g[axis];
        magnitude_squared += gravity[axis] * gravity[axis];
    }
    bool gravity_valid = magnitude_squared > 0.8f * 0.8f && magnitude_squared < 1.2f * 1.2f;
    float *quaternion = sample->attitude_quaternion;
    if (!previous_us || sample->imu_us - previous_us > 500000 || !sample->attitude_valid) {
        sample->attitude_valid = false;
        previous_us = 0;
        if (!gravity_valid) { return; }
        float roll = atan2f(gravity[1], gravity[2]);
        float pitch = atan2f(-gravity[0], hypotf(gravity[1], gravity[2]));
        quaternion[0] = cosf(roll * 0.5f) * cosf(pitch * 0.5f);
        quaternion[1] = sinf(roll * 0.5f) * cosf(pitch * 0.5f);
        quaternion[2] = cosf(roll * 0.5f) * sinf(pitch * 0.5f);
        quaternion[3] = -sinf(roll * 0.5f) * sinf(pitch * 0.5f);
        previous_us = sample->imu_us;
        sample->attitude_valid = true;
        return;
    }
    float elapsed = (sample->imu_us - previous_us) / 1000000.0f;
    previous_us = sample->imu_us;
    if (elapsed <= 0) { return; }
    float scalar = quaternion[0];
    float quat_x = quaternion[1];
    float quat_y = quaternion[2];
    float quat_z = quaternion[3];
    float rates[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        rates[axis] = sample->gyro_dps[axis] * (3.14159265f / 180.0f);
    }
    if (gravity_valid) {
        float inverse_norm = 1.0f / sqrtf(magnitude_squared);
        for (unsigned axis = 0; axis < 3; ++axis) { gravity[axis] *= inverse_norm; }
        float predicted[3] = {
            2.0f * (quat_x * quat_z - scalar * quat_y),
            2.0f * (scalar * quat_x + quat_y * quat_z),
            1.0f - 2.0f * (quat_x * quat_x + quat_y * quat_y),
        };
        /* 仅在接近 1g 时反馈重力误差，避免明显线性加速度直接拉偏姿态。 */
        rates[0] += 2.0f * (gravity[1] * predicted[2] - gravity[2] * predicted[1]);
        rates[1] += 2.0f * (gravity[2] * predicted[0] - gravity[0] * predicted[2]);
        rates[2] += 2.0f * (gravity[0] * predicted[1] - gravity[1] * predicted[0]);
    }
    float half_step = 0.5f * elapsed;
    quaternion[0] += (-quat_x * rates[0] - quat_y * rates[1] - quat_z * rates[2]) * half_step;
    quaternion[1] += (scalar * rates[0] + quat_y * rates[2] - quat_z * rates[1]) * half_step;
    quaternion[2] += (scalar * rates[1] - quat_x * rates[2] + quat_z * rates[0]) * half_step;
    quaternion[3] += (scalar * rates[2] + quat_x * rates[1] - quat_y * rates[0]) * half_step;
    float inverse_norm = 1.0f / sqrtf(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                                    quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
    for (unsigned index = 0; index < 4; ++index) { quaternion[index] *= inverse_norm; }
}

/* 从重力投影求车头仰角，不使用会漂移的偏航角；20Hz 采样下约半秒低通。 */
static void update_ride_motion(board_hardware_snapshot_t *sample)
{
    static int64_t previous_us;
    static float filtered_pitch;
    static float filtered_acceleration;
    sample->ride_grade_valid = false;
    sample->ride_acceleration_valid = false;
    if (!sample->imu_valid || !sample->attitude_valid) {
        previous_us = 0;
        return;
    }
    const float *quaternion = sample->attitude_quaternion;
    float gravity[3] = {
        2 * (quaternion[1] * quaternion[3] - quaternion[0] * quaternion[2]),
        2 * (quaternion[0] * quaternion[1] + quaternion[2] * quaternion[3]),
        1 - 2 * (quaternion[1] * quaternion[1] + quaternion[2] * quaternion[2]),
    };
    float forward_gravity = gravity[HW_RIDE_FORWARD_AXIS] * HW_RIDE_FORWARD_SIGN;
    float pitch = asinf(fmaxf(-1, fminf(1, forward_gravity)));
    float acceleration = (sample->acceleration_g[HW_RIDE_FORWARD_AXIS] * HW_RIDE_FORWARD_SIGN -
                          forward_gravity) * 9.80665f;
    if (!isfinite(pitch) || !isfinite(acceleration)) {
        previous_us = 0;
        return;
    }
    float elapsed = previous_us ? (sample->imu_us - previous_us) / 1000000.0f : 0;
    float weight = elapsed > 0 && elapsed <= 0.5f ? elapsed / (0.5f + elapsed) : 1;
    previous_us = sample->imu_us;
    filtered_pitch += weight * (pitch - filtered_pitch);
    filtered_acceleration += weight * (acceleration - filtered_acceleration);
    float road_pitch = filtered_pitch - HW_RIDE_PITCH_OFFSET_DEG * (3.14159265f / 180.0f);
    /* 接近竖直时坡度趋于无穷，不将手持或错误安装显示成有效道路坡度。 */
    sample->ride_grade_valid = isfinite(road_pitch) && fabsf(road_pitch) <= 3.14159265f / 4;
    sample->ride_grade_percent = sample->ride_grade_valid ? tanf(road_pitch) * 100 : 0;
    sample->ride_acceleration_valid = true;
    sample->ride_acceleration_mps2 = filtered_acceleration;
}

static bool coordinate(const nmea_position *position, double limit, double *value)
{
    bool negative = position->cardinal == 'S' || position->cardinal == 'W';
    if ((limit == 90 && position->cardinal != 'N' && position->cardinal != 'S') ||
        (limit == 180 && position->cardinal != 'E' && position->cardinal != 'W')) { return false; }
    if (!isfinite(position->minutes) || position->minutes < 0 || position->minutes >= 60 ||
        position->degrees < 0) { return false; }
    double magnitude = position->degrees + position->minutes / 60.0;
    if (magnitude > limit) { return false; }
    *value = negative ? -magnitude : magnitude;
    return true;
}

static void parse_gnss(char *line, size_t size, board_hardware_snapshot_t *sample)
{
    if (nmea_has_checksum(line, size) != 0 || nmea_validate(line, size, 1) != 0) { return; }
    /* 先验证原文校验和，再统一 GP/GN/BD 等 talker，仅解析 RMC/GGA 的字段。 */
    line[1] = 'G';
    line[2] = 'P';
    nmea_s *message = nmea_parse(line, size, 0);
    if (!message) { return; }
    if (message->errors != 0) {
        if (message->type == NMEA_GPRMC) { sample->gnss_valid = false; }
        if (message->type == NMEA_GPGGA) { sample->altitude_valid = false; }
    } else if (message->type == NMEA_GPRMC) {
        nmea_gprmc_s *rmc = (nmea_gprmc_s *)message;
        double latitude;
        double longitude;
        float speed = rmc->gndspd_knots * 1.852f;
        sample->gnss_valid = rmc->valid && isfinite(speed) && speed >= 0 && speed <= 200 &&
            coordinate(&rmc->latitude, 90, &latitude) && coordinate(&rmc->longitude, 180, &longitude);
        if (sample->gnss_valid) {
            sample->latitude = latitude;
            sample->longitude = longitude;
            sample->speed_kmh = speed;
            sample->gnss_us = esp_timer_get_time();
        }
    } else if (message->type == NMEA_GPGGA) {
        nmea_gpgga_s *gga = (nmea_gpgga_s *)message;
        sample->altitude_valid = gga->position_fix != 0 && gga->altitude_unit == 'M' &&
            isfinite(gga->altitude) && gga->altitude >= -1500 && gga->altitude <= 20000;
        if (sample->altitude_valid) {
            sample->altitude_m = gga->altitude;
            sample->altitude_us = esp_timer_get_time();
        }
        sample->satellites = gga->n_satellites > 0 ? (unsigned)gga->n_satellites : 0;
        if (gga->position_fix == 0) { sample->gnss_valid = false; }
    }
    nmea_free(message);
}

static void sample_gnss(board_hardware_snapshot_t *sample)
{
    static char line[NMEA_MAX_LENGTH + 1];
    static size_t used;
    uint8_t bytes[256];
    int count = uart_read_bytes(HW_GNSS_UART, bytes, sizeof(bytes), 0);
    for (int index = 0; index < count; ++index) {
        char character = (char)bytes[index];
        if (character == '$') { used = 0; }
        if (used == 0 && character != '$') { continue; }
        if (used >= NMEA_MAX_LENGTH) { used = 0; continue; }
        line[used++] = character;
        if (character == '\n') {
            line[used] = '\0';
            parse_gnss(line, used, sample);
            used = 0;
        }
    }
}

static void hardware_task(void *argument)
{
    board_hardware_snapshot_t sample = {0};
    for (;;) {
        if (imu) {
            mpu6050_accel_data_axes_t acceleration;
            mpu6050_gyro_data_axes_t gyro;
            float temperature;
            sample.imu_read_attempted = true;
            sample.imu_read_result = mpu6050_get_motion(imu, &gyro, &acceleration, &temperature);
            sample.imu_valid = sample.imu_read_result == ESP_OK;
            if (sample.imu_valid) {
                sample.acceleration_g[0] = acceleration.x_axis;
                sample.acceleration_g[1] = acceleration.y_axis;
                sample.acceleration_g[2] = acceleration.z_axis;
                sample.gyro_dps[0] = gyro.x_axis;
                sample.gyro_dps[1] = gyro.y_axis;
                sample.gyro_dps[2] = gyro.z_axis;
                sample.imu_us = esp_timer_get_time();
            } else {
                sample.rotation_valid = false;
                sample.placement = BOARD_PLACEMENT_UNKNOWN;
            }
            update_attitude(&sample);
            update_ride_motion(&sample);
            recognize_placement(&sample);
        }
        if (magnetometer) { sample_magnetic(&sample); }
        if (gnss_uart_installed) { sample_gnss(&sample); }
        if (xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            latest = sample;
            xSemaphoreGive(snapshot_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* S3 板级固定占用：音频、LCD、ADC、USB、Flash/PSRAM、SD 和 LED。 */
static bool pin_reserved(int pin)
{
    if (pin < 0) { return false; }
    return pin == 0 || pin == 1 || pin == 3 || (pin >= 4 && pin <= 14) ||
        pin == 19 || pin == 20 || (pin >= 26 && pin <= 42) || pin >= 45 ||
        pin == LCD_PIN_SCLK || pin == LCD_PIN_MOSI || pin == LCD_PIN_CS ||
        pin == LCD_PIN_DC || pin == LCD_PIN_RST || pin == LCD_PIN_BL;
}

/* 可选地磁计初始化失败时只释放自身设备，保留触摸共用的 I2C。 */
static esp_err_t initialize_magnetometer(void)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HW_MMC5983_ADDRESS,
        .scl_speed_hz = HW_I2C_HZ,
    };
    esp_err_t result = i2c_master_bus_add_device(i2c_bus, &config, &magnetometer);
    if (result != ESP_OK) { return result; }
    uint8_t product_id = 0;
    result = magnetic_read(0x2F, &product_id, 1);
    if (result == ESP_OK && product_id != 0x30) { result = ESP_ERR_NOT_FOUND; }
    if (result == ESP_OK) { result = magnetic_write(0x0A, 0x00); }
    if (result == ESP_OK) { result = magnetic_write(0x0B, 0x00); }
    if (result == ESP_OK) { result = magnetic_write(0x09, 0x21); }
    if (result != ESP_OK) {
        i2c_master_bus_rm_device(magnetometer);
        magnetometer = NULL;
    }
    return result;
}

static esp_err_t initialize_hardware(void)
{
    if (!HW_MPU6050_ENABLED && !HW_ATGM336H_ENABLED && !HW_MMC5983_ENABLED && !HW_FT6336G_ENABLED) {
        return ESP_OK;
    }
    if (snapshot_mutex) { return ESP_ERR_INVALID_STATE; }
    if (HW_AUTO_ROTATE_ENABLED && !HW_MPU6050_ENABLED) { return ESP_ERR_INVALID_ARG; }
    if (HW_IMU_SCREEN_X_AXIS < 0 || HW_IMU_SCREEN_X_AXIS > 2 || HW_IMU_SCREEN_Y_AXIS < 0 ||
        HW_IMU_SCREEN_Y_AXIS > 2 || HW_IMU_SCREEN_X_AXIS == HW_IMU_SCREEN_Y_AXIS ||
        (HW_IMU_SCREEN_X_SIGN != 1 && HW_IMU_SCREEN_X_SIGN != -1) ||
        (HW_IMU_SCREEN_Y_SIGN != 1 && HW_IMU_SCREEN_Y_SIGN != -1)) { return ESP_ERR_INVALID_ARG; }
    bool needs_i2c = HW_MPU6050_ENABLED || HW_MMC5983_ENABLED || HW_FT6336G_ENABLED;
    if (needs_i2c && (!GPIO_IS_VALID_OUTPUT_GPIO(HW_I2C_SDA) ||
                     !GPIO_IS_VALID_OUTPUT_GPIO(HW_I2C_SCL) || HW_I2C_SDA == HW_I2C_SCL ||
                     pin_reserved(HW_I2C_SDA) || pin_reserved(HW_I2C_SCL))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (HW_ATGM336H_ENABLED && (
        HW_GNSS_UART < 0 || HW_GNSS_UART >= UART_NUM_MAX || !GPIO_IS_VALID_GPIO(HW_GNSS_RX) ||
        pin_reserved(HW_GNSS_RX) || pin_reserved(HW_GNSS_TX) ||
        (HW_GNSS_TX >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(HW_GNSS_TX) || HW_GNSS_TX == HW_GNSS_RX)) ||
        (needs_i2c && (HW_GNSS_RX == HW_I2C_SDA || HW_GNSS_RX == HW_I2C_SCL ||
                      (HW_GNSS_TX >= 0 && (HW_GNSS_TX == HW_I2C_SDA || HW_GNSS_TX == HW_I2C_SCL)))))) {
        return ESP_ERR_INVALID_ARG;
    }
    int touch_pins[] = {HW_TOUCH_RST, HW_TOUCH_INT};
    if (HW_FT6336G_ENABLED) {
        for (unsigned index = 0; index < 2; ++index) {
            int pin = touch_pins[index];
            if (pin < -1 || (pin >= 0 && (!GPIO_IS_VALID_GPIO(pin) || pin_reserved(pin) ||
                pin == HW_I2C_SDA || pin == HW_I2C_SCL ||
                (index == 0 && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) ||
                (HW_ATGM336H_ENABLED && (pin == HW_GNSS_RX || pin == HW_GNSS_TX))))) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (HW_TOUCH_RST >= 0 && HW_TOUCH_RST == HW_TOUCH_INT) { return ESP_ERR_INVALID_ARG; }
    }
    snapshot_mutex = xSemaphoreCreateMutex();
    if (!snapshot_mutex) { return ESP_ERR_NO_MEM; }
    esp_err_t result = ESP_OK;
    esp_err_t imu_result = ESP_OK;
    if (needs_i2c) {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = HW_I2C_SDA,
            .scl_io_num = HW_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        result = i2c_new_master_bus(&config, &i2c_bus);
        if (result != ESP_OK) { goto cleanup; }
        ESP_LOGI("board_hardware", "Shared I2C SDA=%d SCL=%d: touch 0x38=%s, ES8311 0x18=%s",
             HW_I2C_SDA, HW_I2C_SCL,
             i2c_master_probe(i2c_bus, HW_TOUCH_ADDRESS, 30) == ESP_OK ? "ACK" : "missing",
             i2c_master_probe(i2c_bus, HW_AUDIO_ADDRESS, 30) == ESP_OK ? "ACK" : "missing");
    }
    if (HW_MPU6050_ENABLED) {
        const uint8_t addresses[] = {0x68, 0x69, 0x70};
        for (unsigned index = 0; index < sizeof(addresses); ++index) {
            if (i2c_master_probe(i2c_bus, addresses[index], 30) == ESP_OK) {
                imu_ack_mask |= (uint8_t)(1U << index);
            }
        }
        imu_probe_done = true;
        ESP_LOGI("board_hardware", "I2C ACK: 0x68=%s 0x69=%s 0x70=%s; configured=0x%02X",
             imu_ack_mask & 1 ? "YES" : "NO", imu_ack_mask & 2 ? "YES" : "NO",
             imu_ack_mask & 4 ? "YES" : "NO", HW_MPU6050_ADDRESS);
        mpu6050_config_t config = I2C_MPU6050_CONFIG_DEFAULT;
        config.i2c_address = HW_MPU6050_ADDRESS;
        config.i2c_clock_speed = HW_I2C_HZ;
        config.irq_io_num = GPIO_NUM_NC;
        result = mpu6050_init(i2c_bus, &config, &imu);
        if (result != ESP_OK) {
            imu_result = result;
            imu = NULL;
            ESP_LOGW("board_hardware", "MPU6050 unavailable: %s", esp_err_to_name(result));
        }
    }
    if (HW_MMC5983_ENABLED) {
        result = initialize_magnetometer();
        if (result != ESP_OK) {
            ESP_LOGW("board_hardware", "MMC5983MA unavailable: %s", esp_err_to_name(result));
        }
    }
    if (HW_ATGM336H_ENABLED) {
        uart_config_t config = {
            .baud_rate = HW_GNSS_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        result = uart_param_config(HW_GNSS_UART, &config);
        if (result == ESP_OK) {
            result = uart_set_pin(HW_GNSS_UART, HW_GNSS_TX, HW_GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
        if (result == ESP_OK) { result = uart_driver_install(HW_GNSS_UART, 2048, 0, 0, NULL, 0); }
        gnss_uart_installed = result == ESP_OK;
        if (!gnss_uart_installed) {
            ESP_LOGW("board_hardware", "GNSS UART unavailable: %s", esp_err_to_name(result));
        }
    }
    if ((imu || magnetometer || gnss_uart_installed) &&
        xTaskCreate(hardware_task, "hardware", 4096, NULL, 2, NULL) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    return imu_result;

cleanup:
    if (gnss_uart_installed) { uart_driver_delete(HW_GNSS_UART); gnss_uart_installed = false; }
    if (imu) { mpu6050_delete(imu); imu = NULL; }
    if (magnetometer) { i2c_master_bus_rm_device(magnetometer); magnetometer = NULL; }
    if (i2c_bus) { i2c_del_master_bus(i2c_bus); i2c_bus = NULL; }
    vSemaphoreDelete(snapshot_mutex);
    snapshot_mutex = NULL;
    return result;
}

esp_err_t board_hardware_init(void)
{
    initialization_result = initialize_hardware();
    return initialization_result;
}

bool board_hardware_snapshot(board_hardware_snapshot_t *snapshot)
{
    if (!snapshot) { return false; }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialization_result = initialization_result;
    snapshot->imu_initialized = imu != NULL;
    snapshot->imu_read_result = ESP_ERR_INVALID_STATE;
    snapshot->imu_probe_done = imu_probe_done;
    snapshot->imu_ack_mask = imu_ack_mask;
    if (!snapshot_mutex || xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { return false; }
    *snapshot = latest;
    xSemaphoreGive(snapshot_mutex);
    snapshot->initialization_result = initialization_result;
    snapshot->imu_initialized = imu != NULL;
    if (!snapshot->imu_us && snapshot->imu_read_result == ESP_OK) {
        snapshot->imu_read_result = ESP_ERR_INVALID_STATE;
    }
    snapshot->imu_probe_done = imu_probe_done;
    snapshot->imu_ack_mask = imu_ack_mask;
    int64_t now = esp_timer_get_time();
    if (now - snapshot->imu_us > 500000) {
        snapshot->imu_valid = false;
        snapshot->attitude_valid = false;
        snapshot->ride_grade_valid = false;
        snapshot->ride_acceleration_valid = false;
        snapshot->rotation_valid = false;
        snapshot->placement = BOARD_PLACEMENT_UNKNOWN;
    }
    if (now - snapshot->magnetic_us > 500000) { snapshot->magnetic_valid = false; }
    if (now - snapshot->gnss_us > 5000000) { snapshot->gnss_valid = false; }
    if (now - snapshot->altitude_us > 5000000) { snapshot->altitude_valid = false; }
    return true;
}

i2c_master_bus_handle_t board_hardware_i2c_bus(void)
{
    return i2c_bus;
}