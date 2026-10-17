#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tf_card.h"
#include "zh_font.h"
#include "usb_msc.h"

static const char *TAG = "usb_msc";
static bool active;
static tinyusb_msc_storage_handle_t storage;

/* 进入 U 盘模式前释放字库：LVGL 的动态字形模式会一直持有字库文件句柄，
 * 句柄没关闭就卸载 /tf，后续取字形会落到已注销的磁盘驱动上。
 * LVGL 锁可递归获取，因此按钮回调（LVGL 任务内）调用也安全。 */
static void release_fonts(void)
{
    if (lvgl_port_lock(1000)) {
        zh_font_release();
        lvgl_port_unlock();
    }
}

/* 用与 USB 完全相同的方式读一个扇区，确认交出去的块设备真的可读。
 * 失败时立即把卡还给本地挂载，避免电脑只能看到一个打不开的盘。 */
static esp_err_t probe_medium(sdmmc_card_t *card)
{
    /* 按卡的扇区大小分配，避免个别 1 KB 扇区卡写越界。 */
    size_t sector_size = card->csd.sector_size ? card->csd.sector_size : 512;
    uint8_t *sector = heap_caps_malloc(sector_size, MALLOC_CAP_DMA);
    if (!sector) {
        ESP_LOGW(TAG, "No DMA buffer for medium probe; skipping check");
        return ESP_OK;
    }
    esp_err_t err = sdmmc_read_sectors(card, sector, 0, 1);
    if (err == ESP_OK && sector_size >= 512) {
        ESP_LOGI(TAG, "USB medium ready: %u-byte sectors, sector0 sig=%02X%02X, partition type=0x%02X",
                 (unsigned)sector_size, sector[510], sector[511], sector[446 + 4]);
    }
    heap_caps_free(sector);
    return err;
}

esp_err_t usb_msc_start(void)
{
    if (active) { return ESP_OK; }
    release_fonts();
    sdmmc_card_t *card = tf_card_enter_usb();
    if (!card) {
        ESP_LOGE(TAG, "TF card unavailable for USB (no card or volume still busy)");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t probe_err = probe_medium(card);
    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "USB medium probe failed: %s; restoring local mount",
                 esp_err_to_name(probe_err));
        tf_card_exit_usb();
        return probe_err;
    }

    /* 关闭自动重挂载：U 盘模式下存储始终归 USB 主机所有，
     * 拔插时不再由 MSC 组件把同一张卡挂到本地 /data。 */
    const tinyusb_msc_driver_config_t driver_config = {
        .user_flags = {.auto_mount_off = 1},
        .callback = NULL,
        .callback_arg = NULL,
    };
    esp_err_t err = tinyusb_msc_install_driver(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC driver install failed: %s", esp_err_to_name(err));
        tf_card_exit_usb();
        return err;
    }

    /* 只暴露裸扇区给主机，不在设备侧挂载 FAT；绝不自动格式化。 */
    const tinyusb_msc_storage_config_t storage_config = {
        .medium.card = card,
        .fat_fs = {
            .base_path = NULL,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 3,
                .allocation_unit_size = 16 * 1024,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    err = tinyusb_msc_new_storage_sdmmc(&storage_config, &storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage init failed: %s", esp_err_to_name(err));
        tinyusb_msc_uninstall_driver();
        tf_card_exit_usb();
        return err;
    }

    tinyusb_config_t tusb_config = TINYUSB_DEFAULT_CONFIG();
    /* 默认优先级 5 高于 LVGL(3)：主机反复重试时会挤掉界面刷新，
     * 这里降为与界面同级，保证触摸和屏幕始终有响应。 */
    tusb_config.task.priority = 3;
    err = tinyusb_driver_install(&tusb_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        tinyusb_msc_delete_storage(storage);
        storage = NULL;
        tinyusb_msc_uninstall_driver();
        tf_card_exit_usb();
        return err;
    }
    active = true;
    ESP_LOGI(TAG, "USB MSC enabled: USB port now carries the disk, serial log stops here");
    return ESP_OK;
}

esp_err_t usb_msc_stop(void)
{
    if (!active) { return ESP_OK; }
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB uninstall failed: %s", esp_err_to_name(err));
    }
    if (storage) {
        /* 主机可能还有缓冲写未落盘，delete 会返回 INVALID_STATE；等它完成后重试，
         * 否则 medium 内部的卡指针不会清空，之后释放 usb_card 会留下悬空引用。 */
        esp_err_t del = ESP_FAIL;
        for (int attempt = 0; attempt < 10; ++attempt) {
            del = tinyusb_msc_delete_storage(storage);
            if (del == ESP_OK) { break; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (del != ESP_OK) {
            ESP_LOGW(TAG, "MSC storage delete failed: %s", esp_err_to_name(del));
        }
        storage = NULL;
    }
    err = tinyusb_msc_uninstall_driver();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MSC driver uninstall failed: %s", esp_err_to_name(err));
    }
    active = false;
    /* 归还 SDMMC 主机并重新挂载 /tf，恢复本地文件浏览；字库会在挂载后自动重载。 */
    tf_card_exit_usb();
    ESP_LOGI(TAG, "USB MSC disabled: local TF mount restored, remove the disk from the PC first");
    return ESP_OK;
}

bool usb_msc_active(void) { return active; }

