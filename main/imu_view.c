#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "board_hardware.h"
#include "hardware_config.h"
#include "imu_view.h"
#include "ui_helpers.h"

static lv_display_t *display;
static lv_timer_t *refresh_timer;
static lv_obj_t *imu_screen;
static lv_obj_t *imu_state_label;
static lv_obj_t *imu_result_label;
static lv_obj_t *imu_ack_label;
static lv_obj_t *imu_acceleration_labels[3];
static lv_obj_t *imu_gyro_labels[3];
static lv_obj_t *imu_pose_label;
static lv_obj_t *imu_gate_label;
static lv_obj_t *imu_sample_label;
static lv_obj_t *imu_arrow;
static lv_obj_t *imu_angle_labels[3];
static lv_obj_t *imu_wiring_label;
static lv_obj_t *imu_separator;
static lv_obj_t *imu_units[2];
static lv_obj_t *imu_axes[3];
static bool imu_arrow_valid;
static float imu_arrow_quaternion[4] = {1, 0, 0, 0};

/* 在小区域内投影带厚度的 +Y 箭头，按深度绘制三角面，不分配画布缓冲。 */
static void draw_imu_arrow(lv_event_t *event)
{
    static const float vertices[][3] = {
        {-0.12f, -0.85f, -0.10f}, {0.12f, -0.85f, -0.10f},
        {0.12f, 0.25f, -0.10f}, {-0.12f, 0.25f, -0.10f},
        {-0.12f, -0.85f, 0.10f}, {0.12f, -0.85f, 0.10f},
        {0.12f, 0.25f, 0.10f}, {-0.12f, 0.25f, 0.10f},
        {-0.42f, 0.15f, -0.25f}, {0.42f, 0.15f, -0.25f},
        {0.42f, 0.15f, 0.25f}, {-0.42f, 0.15f, 0.25f}, {0, 0.95f, 0},
    };
    static const uint8_t faces[][3] = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
        {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
        {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
        {8, 9, 12}, {9, 10, 12}, {10, 11, 12}, {11, 8, 12},
        {8, 11, 10}, {8, 10, 9},
    };
    static const uint32_t colors[] = {0x367C95, 0x367C95, 0xADF3E1, 0xADF3E1,
        0x55B5B1, 0x55B5B1, 0x73D6CB, 0x73D6CB, 0x55B5B1, 0x55B5B1,
        0x409599, 0x409599, 0x459CAD, 0x6CCFC9, 0xC3FFEB, 0x85DED1,
        0x367C95, 0x367C95};
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_layer_t *layer = lv_event_get_layer(event);
    float center_x = (area.x1 + area.x2) * 0.5f;
    float center_y = (area.y1 + area.y2) * 0.5f;
    float scale = fminf((area.x2 - area.x1 + 1) * 0.42f, (area.y2 - area.y1 + 1) * 0.5f);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0x3B504A);
    line.width = 1;
    line.p1 = (lv_point_precise_t){center_x - 47, center_y};
    line.p2 = (lv_point_precise_t){center_x + 47, center_y};
    lv_draw_line(layer, &line);
    line.p1 = (lv_point_precise_t){center_x, center_y - 39};
    line.p2 = (lv_point_precise_t){center_x, center_y + 39};
    lv_draw_line(layer, &line);
    float scalar = imu_arrow_quaternion[0];
    float quat_x = imu_arrow_quaternion[1];
    float quat_y = imu_arrow_quaternion[2];
    float quat_z = imu_arrow_quaternion[3];
    float rotation[3][3] = {
        {1 - 2 * (quat_y * quat_y + quat_z * quat_z), 2 * (quat_x * quat_y - scalar * quat_z),
         2 * (quat_x * quat_z + scalar * quat_y)},
        {2 * (quat_x * quat_y + scalar * quat_z), 1 - 2 * (quat_x * quat_x + quat_z * quat_z),
         2 * (quat_y * quat_z - scalar * quat_x)},
        {2 * (quat_x * quat_z - scalar * quat_y), 2 * (quat_y * quat_z + scalar * quat_x),
         1 - 2 * (quat_x * quat_x + quat_y * quat_y)},
    };
    lv_point_precise_t projected[13];
    float depth[13];
    for (unsigned vertex = 0; vertex < 13; ++vertex) {
        float world[3] = {0};
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                world[row] += rotation[row][axis] * vertices[vertex][axis];
            }
        }
        projected[vertex].x = center_x + scale * (0.8660254f * world[0] - 0.5f * world[1]);
        projected[vertex].y = center_y - scale * (0.25f * world[0] + 0.4330127f * world[1] +
                                            0.8660254f * world[2]);
        depth[vertex] = -0.4330127f * world[0] - 0.75f * world[1] + 0.5f * world[2];
    }
    unsigned order[18];
    float face_depth[18];
    for (unsigned face = 0; face < 18; ++face) {
        order[face] = face;
        face_depth[face] = depth[faces[face][0]] + depth[faces[face][1]] + depth[faces[face][2]];
    }
    for (unsigned face = 1; face < 18; ++face) {
        unsigned selected = order[face];
        unsigned position = face;
        while (position > 0 && face_depth[order[position - 1]] > face_depth[selected]) {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = selected;
    }
    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.opa = LV_OPA_COVER;
    for (unsigned face = 0; face < 18; ++face) {
        unsigned selected = order[face];
        triangle.color = lv_color_hex(imu_arrow_valid ? colors[selected] : 0x62706C);
        for (unsigned corner = 0; corner < 3; ++corner) {
            triangle.p[corner] = projected[faces[selected][corner]];
        }
        lv_draw_triangle(layer, &triangle);
    }
}

/* 定时器只读取硬件快照，不获取应用状态锁，保持既有锁顺序。 */
static void update_imu_ui(lv_timer_t *timer)
{
    if (!display || !imu_screen || !lv_obj_is_visible(imu_screen)) { return; }
    board_hardware_snapshot_t hardware;
    bool snapshot_ready = board_hardware_snapshot(&hardware);
    const char *state = !HW_MPU6050_ENABLED ? "DISABLED" :
        hardware.initialization_result != ESP_OK ? "INIT FAIL" :
        !snapshot_ready ? "BUSY" : !hardware.imu_initialized ? "NO DEVICE" :
        !hardware.imu_read_attempted ? "WAITING" : hardware.imu_valid ? "LIVE" :
        hardware.imu_read_result != ESP_OK ? "READ FAIL" : "STALE";
    lv_label_set_text(imu_state_label, state);
    lv_obj_set_style_text_color(imu_state_label,
        lv_color_hex(hardware.imu_valid ? 0x8CE2BE : 0xFFB86B), 0);
    char text[80];
    snprintf(text, sizeof(text), "INIT %s", esp_err_to_name(hardware.initialization_result));
    lv_label_set_text(imu_result_label, text);
    if (hardware.imu_probe_done) {
        snprintf(text, sizeof(text), "ACK 68:%c  69:%c  70:%c",
                 hardware.imu_ack_mask & 1 ? 'Y' : 'N', hardware.imu_ack_mask & 2 ? 'Y' : 'N',
                 hardware.imu_ack_mask & 4 ? 'Y' : 'N');
    } else { snprintf(text, sizeof(text), "ACK --"); }
    lv_label_set_text(imu_ack_label, text);
    imu_arrow_valid = hardware.imu_valid && hardware.attitude_valid;
    if (imu_arrow_valid) {
        memcpy(imu_arrow_quaternion, hardware.attitude_quaternion, sizeof(imu_arrow_quaternion));
    }
    lv_obj_invalidate(imu_arrow);
    float scalar = imu_arrow_quaternion[0];
    float quat_x = imu_arrow_quaternion[1];
    float quat_y = imu_arrow_quaternion[2];
    float quat_z = imu_arrow_quaternion[3];
    float angles[] = {
        atan2f(2 * (scalar * quat_x + quat_y * quat_z), 1 - 2 * (quat_x * quat_x + quat_y * quat_y)),
        asinf(fmaxf(-1, fminf(1, 2 * (scalar * quat_y - quat_z * quat_x)))),
        atan2f(2 * (scalar * quat_z + quat_x * quat_y), 1 - 2 * (quat_y * quat_y + quat_z * quat_z)),
    };
    const char *angle_names[] = {"R", "P", "Y"};
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (imu_arrow_valid) {
            snprintf(text, sizeof(text), "%s%+.1f", angle_names[axis], angles[axis] * (180.0f / 3.14159265f));
        } else { snprintf(text, sizeof(text), "%s --", angle_names[axis]); }
        lv_label_set_text(imu_angle_labels[axis], text);
        if (hardware.imu_valid) { snprintf(text, sizeof(text), "%+.3f", hardware.acceleration_g[axis]); }
        else { snprintf(text, sizeof(text), "--"); }
        lv_label_set_text(imu_acceleration_labels[axis], text);
        if (hardware.imu_valid) { snprintf(text, sizeof(text), "%+.1f", hardware.gyro_dps[axis]); }
        else { snprintf(text, sizeof(text), "--"); }
        lv_label_set_text(imu_gyro_labels[axis], text);
    }
    const char *pose = !hardware.imu_valid ? "UNKNOWN" :
        hardware.placement == BOARD_PLACEMENT_FLAT ? "FLAT" :
        hardware.placement == BOARD_PLACEMENT_UPRIGHT ? "UPRIGHT" : "UNKNOWN";
    if (hardware.rotation_valid) {
        snprintf(text, sizeof(text), "POSE %s  ROT %u", pose, hardware.rotation_degrees);
    } else { snprintf(text, sizeof(text), "POSE %s  ROT --", pose); }
    lv_label_set_text(imu_pose_label, text);
    const char *gate = "NO DATA";
    if (hardware.imu_valid) {
        switch (hardware.orientation_state) {
        case BOARD_ORIENTATION_MOVING: gate = "MOVING"; break;
        case BOARD_ORIENTATION_TILTED: gate = "TILTED"; break;
        case BOARD_ORIENTATION_STABILIZING: gate = "SETTLING"; break;
        case BOARD_ORIENTATION_FLAT: gate = "FLAT HOLD"; break;
        case BOARD_ORIENTATION_READY: gate = "READY"; break;
        default: break;
        }
    }
    snprintf(text, sizeof(text), "%s  %u/%u", gate,
             hardware.imu_valid ? hardware.orientation_samples : 0, HW_ROTATE_STABLE_SAMPLES);
    lv_label_set_text(imu_gate_label, text);
    if (hardware.imu_valid) {
        snprintf(text, sizeof(text), "AGE %lldms  AUTO %s",
                 (long long)((esp_timer_get_time() - hardware.imu_us) / 1000),
                 HW_AUTO_ROTATE_ENABLED ? "ON" : "OFF");
    } else if (hardware.imu_read_attempted && hardware.imu_read_result == ESP_OK && hardware.imu_us) {
        snprintf(text, sizeof(text), "STALE AGE %lldms",
                 (long long)((esp_timer_get_time() - hardware.imu_us) / 1000));
    } else {
        snprintf(text, sizeof(text), "READ %s", esp_err_to_name(hardware.imu_read_result));
    }
    lv_label_set_text(imu_sample_label, text);
}

/* 页面随显示释放时停止刷新，避免初始化失败回收后仍访问旧显示句柄。 */
static void delete_imu_view(lv_event_t *event)
{
    if (refresh_timer) { lv_timer_delete(refresh_timer); refresh_timer = NULL; }
    display = NULL;
    imu_screen = NULL;
}

lv_obj_t *imu_view_create(lv_display_t *created_display)
{
    return imu_view_create_in(created_display, NULL);
}

lv_obj_t *imu_view_create_in(lv_display_t *created_display, lv_obj_t *parent)
{
    display = created_display;
    imu_screen = lv_obj_create(parent);
    lv_obj_set_style_pad_all(imu_screen, 0, 0);
    lv_obj_set_style_border_width(imu_screen, 0, 0);
    if (parent) {
        lv_obj_set_pos(imu_screen, 0, 0);
        lv_obj_set_size(imu_screen, 240, 320);
    }
    lv_obj_set_style_bg_color(imu_screen, lv_color_hex(0x101918), 0);
    lv_obj_set_scrollable(imu_screen, false);
    make_label(imu_screen, "MPU6050", 12, 8, 94, LV_FONT_DEFAULT, 0xFFFFFF);
    imu_state_label = make_label(imu_screen, "WAITING", 12, 79, 108, &lv_font_montserrat_12, 0xFFB86B);
    char wiring[64];
    snprintf(wiring, sizeof(wiring), "ADDR %02X  SDA %d  SCL %d",
             HW_MPU6050_ADDRESS, HW_I2C_SDA, HW_I2C_SCL);
    imu_wiring_label = make_label(imu_screen, wiring, 12, 26, 216, &lv_font_montserrat_12, 0xC1CBC9);
    imu_result_label = make_label(imu_screen, "INIT --", 12, 42, 216, &lv_font_montserrat_12, 0xFFB86B);
    imu_ack_label = make_label(imu_screen, "ACK --", 12, 58, 216, &lv_font_montserrat_12, 0xC1CBC9);
    imu_separator = make_shape(imu_screen, 12, 74, 216, 1, 0x3B504A, 0);
    imu_arrow = lv_obj_create(imu_screen);
    lv_obj_remove_style_all(imu_arrow);
    lv_obj_set_pos(imu_arrow, 8, 93);
    lv_obj_set_size(imu_arrow, 108, 84);
    lv_obj_set_scrollable(imu_arrow, false);
    lv_obj_add_event_cb(imu_arrow, draw_imu_arrow, LV_EVENT_DRAW_MAIN, NULL);
    imu_units[0] = make_label(imu_screen, "g", 138, 80, 44, &lv_font_montserrat_12, 0x8CA49B);
    imu_units[1] = make_label(imu_screen, "dps", 184, 80, 48, &lv_font_montserrat_12, 0x8CA49B);
    const char *axes[] = {"X", "Y", "Z"};
    const char *angle_names[] = {"R --", "P --", "Y --"};
    for (unsigned axis = 0; axis < 3; ++axis) {
        int top = 101 + axis * 24;
        imu_axes[axis] = make_label(imu_screen, axes[axis], 121, top, 14, &lv_font_montserrat_12, 0x8CE2BE);
        imu_acceleration_labels[axis] = make_label(imu_screen, "--", 138, top, 44, &lv_font_montserrat_12, 0xFFFFFF);
        imu_gyro_labels[axis] = make_label(imu_screen, "--", 184, top, 48, &lv_font_montserrat_12, 0xFFFFFF);
        imu_angle_labels[axis] = make_label(imu_screen, angle_names[axis], 12 + axis * 74, 180, 72,
                                           &lv_font_montserrat_12, 0x8CE2BE);
    }
    imu_pose_label = make_label(imu_screen, "POSE UNKNOWN  ROT --", 12, 197, 216, &lv_font_montserrat_12, 0xFFFFFF);
    imu_gate_label = make_label(imu_screen, "NO DATA", 12, 211, 216, &lv_font_montserrat_12, 0xFFB86B);
    imu_sample_label = make_label(imu_screen, "READ --", 12, 225, 216, &lv_font_montserrat_12, 0x8CA49B);
    refresh_timer = lv_timer_create(update_imu_ui, 50, NULL);
    lv_obj_add_event_cb(imu_screen, delete_imu_view, LV_EVENT_DELETE, NULL);
    return imu_screen;
}

/* 诊断文字保留固定字号，竖屏扩大姿态与数据区，横屏使用更宽的数据列。 */
void imu_view_layout(int width, int height)
{
    if (!imu_screen) { return; }
    bool landscape = width > height;
    int acceleration_left = width - 110;
    int gyro_left = width - 60;
    place_label(imu_wiring_label, 12, landscape ? 26 : 36, width - 24);
    place_label(imu_result_label, 12, landscape ? 42 : 54, width - 24);
    place_label(imu_ack_label, 12, landscape ? 58 : 72, width - 24);
    lv_obj_set_pos(imu_separator, 12, landscape ? 74 : 94);
    lv_obj_set_width(imu_separator, width - 24);
    place_label(imu_state_label, 12, landscape ? 79 : 106, acceleration_left - 24);
    lv_obj_set_pos(imu_arrow, 8, landscape ? 94 : 124);
    lv_obj_set_size(imu_arrow, acceleration_left - 30, landscape ? 74 : 100);
    place_label(imu_units[0], acceleration_left, landscape ? 80 : 106, 48);
    place_label(imu_units[1], gyro_left, landscape ? 80 : 106, 52);
    for (unsigned axis = 0; axis < 3; ++axis) {
        int top = (landscape ? 100 : 130) + (int)axis * (landscape ? 24 : 30);
        place_label(imu_axes[axis], acceleration_left - 17, top, 14);
        place_label(imu_acceleration_labels[axis], acceleration_left, top, 48);
        place_label(imu_gyro_labels[axis], gyro_left, top, 52);
        place_label(imu_angle_labels[axis], 12 + (int)axis * (width - 24) / 3,
                    landscape ? 171 : height - 82, (width - 24) / 3);
    }
    place_label(imu_pose_label, 12, height - (landscape ? 54 : 62), width - 24);
    place_label(imu_gate_label, 12, height - (landscape ? 40 : 46), width - 24);
    place_label(imu_sample_label, 12, height - (landscape ? 26 : 30), width - 24);
}