#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "tf_card.h"
#include "zh_font.h"

#define TF_MOUNT "/tf"

typedef struct {
    char path[TF_PATH_SIZE];
    unsigned page;
} browse_request_t;

static SemaphoreHandle_t snapshot_mutex;
static SemaphoreHandle_t fs_mutex;
static QueueHandle_t requests;
static tf_snapshot_t published;
static tf_snapshot_t working;
static sdmmc_card_t *card;
static sdmmc_card_t *usb_card;
static bool usb_mode;

/* 所有访问卡的操作（VFS 目录、字库文件、挂载/卸载）都通过这把锁串行化。
 * FatFs 在 IDF 中并非完全可重入，且卸载会释放 card 结构。 */
void tf_card_lock(void)
{
    if (fs_mutex) { xSemaphoreTake(fs_mutex, portMAX_DELAY); }
}

void tf_card_unlock(void)
{
    if (fs_mutex) { xSemaphoreGive(fs_mutex); }
}

static void publish(void)
{
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    working.revision = published.revision + 1;
    published = working;
    xSemaphoreGive(snapshot_mutex);
}

/* 只接受挂载点内的路径，不允许通过父目录跳出 TF 卡。 */
bool tf_card_browse(const char *path, unsigned page)
{
    if (!requests || !path || path[0] != '/' || strlen(path) >= TF_PATH_SIZE ||
        strstr(path, "/..") || strstr(path, "//")) { return false; }
    browse_request_t request = {.page = page};
    snprintf(request.path, sizeof(request.path), "%s", path);
    return xQueueSend(requests, &request, 0) == pdTRUE;
}

bool tf_card_snapshot(tf_snapshot_t *snapshot)
{
    if (!snapshot_mutex || !snapshot ||
        xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { return false; }
    *snapshot = published;
    xSemaphoreGive(snapshot_mutex);
    return true;
}

static void read_directory(const browse_request_t *request)
{
    working.busy = true;
    publish();
    working.count = 0;
    working.more = false;
    working.page = request->page;
    snprintf(working.path, sizeof(working.path), "%s", request->path);
    uint64_t total = 0;
    uint64_t free_space = 0;
    working.capacity_valid = esp_vfs_fat_info(TF_MOUNT, &total, &free_space) == ESP_OK;
    working.total_bytes = total;
    working.free_bytes = free_space;
    char full_path[TF_PATH_SIZE + sizeof(TF_MOUNT)];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT, request->path);
    DIR *directory = opendir(full_path);
    working.error = directory ? ESP_OK : ESP_FAIL;
    if (directory) {
        struct dirent *entry;
        unsigned skipped = 0;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { continue; }
            if (skipped++ < request->page * TF_PAGE_SIZE) { continue; }
            if (working.count == TF_PAGE_SIZE) { working.more = true; break; }
            tf_entry_t *item = &working.entries[working.count++];
            snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
            char item_path[TF_PATH_SIZE + sizeof(TF_MOUNT) + 256];
            snprintf(item_path, sizeof(item_path), "%s/%s", full_path, entry->d_name);
            struct stat information;
            bool valid = stat(item_path, &information) == 0;
            item->directory = valid && S_ISDIR(information.st_mode);
            item->size = valid ? information.st_size : 0;
        }
        closedir(directory);
    }
    working.busy = false;
    ESP_LOGI("tf_card", "Directory %s page %u: %u entries, total=%llu, free=%llu, result=%s",
             working.path, working.page + 1, working.count,
             (unsigned long long)working.total_bytes, (unsigned long long)working.free_bytes,
             esp_err_to_name(working.error));
    publish();
}

static void configure_sdmmc(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = SDMMC_FREQ_DEFAULT;
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 4;
    slot->clk = 38;
    slot->cmd = 40;
    slot->d0 = 39;
    slot->d1 = 41;
    slot->d2 = 48;
    slot->d3 = 47;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static esp_err_t mount_card(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    configure_sdmmc(&host, &slot);
    /* 识别失败绝不格式化，保留用户已有文件；实际卡座上拉仍由硬件保证。 */
    esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(TF_MOUNT, &host, &slot, &config, &card);
}

static void card_task(void *argument)
{
    (void)argument;
    browse_request_t request = {.path = "/"};
    for (;;) {
        if (usb_mode) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!card) {
            /* usb_mode 在锁内复查：U 盘切换可能恰好发生在循环顶部的检查之后。 */
            tf_card_lock();
            if (!usb_mode && !card) {
                working.error = mount_card();
                working.mounted = working.error == ESP_OK;
                if (working.mounted) {
                    working.card_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
                    ESP_LOGI("tf_card", "TF mounted: %s, %llu bytes", card->cid.name,
                             (unsigned long long)working.card_bytes);
                    request = (browse_request_t){.path = "/"};
                    read_directory(&request);
                } else { publish(); }
            }
            tf_card_unlock();
        }
        if (xQueueReceive(requests, &request, pdMS_TO_TICKS(3000)) == pdTRUE) {
            tf_card_lock();
            if (!usb_mode && card) { read_directory(&request); }
            tf_card_unlock();
        }
        /* 卡座没有独立检测引脚，用卡状态命令探测拔出，失败后释放挂载并重试。 */
        tf_card_lock();
        if (!usb_mode && card && sdmmc_get_status(card) != ESP_OK) {
            /* 字库仍持有字库文件：先挂释放标记（不能在此等 LVGL 锁，
             * 否则与 fs_open 的 LVGL→tf_card 顺序互锁），再由 LVGL 任务
             * 关闭句柄，重插卡后即可重新加载。 */
            zh_font_invalidate();
            esp_vfs_fat_sdcard_unmount(TF_MOUNT, card);
            card = NULL;
            working = (tf_snapshot_t){.error = ESP_ERR_NOT_FOUND, .path = "/"};
            publish();
        }
        tf_card_unlock();
    }
}

sdmmc_card_t *tf_card_enter_usb(void)
{
    if (usb_mode || !card) { return NULL; }
    usb_mode = true;

    /* 取锁会等待后台任务完成当前目录读取，避免在 FatFs 操作中途卸载。
     * esp_vfs_fat_sdcard_unmount 会释放 card 结构，因此立即清空本地指针。 */
    tf_card_lock();
    /* 先标记为未挂载：切换期间界面与字库都不要再访问 /tf。 */
    working.mounted = false;
    publish();
    esp_err_t umount_err = esp_vfs_fat_sdcard_unmount(TF_MOUNT, card);
    card = NULL;
    tf_card_unlock();
    if (umount_err != ESP_OK) {
        ESP_LOGE("tf_card", "Unmount before USB failed: %s", esp_err_to_name(umount_err));
        goto restore;
    }

    /* 重新初始化主机、槽位和一张卡，供 USB 块设备按扇区读写。
     * 注意顺序必须与 esp_vfs_fat_sdmmc_mount 一致：host → slot → card。 */
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    configure_sdmmc(&host, &slot);
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGE("tf_card", "SDMMC host init failed: %s", esp_err_to_name(err));
        goto restore;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        ESP_LOGE("tf_card", "SDMMC slot init failed: %s", esp_err_to_name(err));
        sdmmc_host_deinit();
        goto restore;
    }
    usb_card = malloc(sizeof(sdmmc_card_t));
    if (!usb_card) {
        sdmmc_host_deinit();
        err = ESP_ERR_NO_MEM;
        goto restore;
    }
    err = sdmmc_card_init(&host, usb_card);
    if (err != ESP_OK) {
        ESP_LOGE("tf_card", "SDMMC card init failed: %s", esp_err_to_name(err));
        free(usb_card);
        usb_card = NULL;
        sdmmc_host_deinit();
        goto restore;
    }

    /* 卡已交给 USB 主机，本地快照标记为未挂载，界面不再访问 /tf。 */
    working.mounted = false;
    working.busy = false;
    publish();
    ESP_LOGI("tf_card", "TF entered USB MSC mode");
    return usb_card;

restore:
    /* 进入 U 盘模式失败时恢复本地挂载，避免 TF 浏览一直不可用。
     * usb_mode 保持为 true 直到挂载完成，防止 card_task 并发重复挂载。 */
    {
        tf_card_lock();
        esp_err_t mount_err = mount_card();
        working.error = mount_err;
        working.mounted = mount_err == ESP_OK;
        publish();
        tf_card_unlock();
        if (mount_err != ESP_OK) {
            ESP_LOGE("tf_card", "Local remount after USB failure failed: %s", esp_err_to_name(mount_err));
        }
    }
    usb_mode = false;
    return NULL;
}

esp_err_t tf_card_exit_usb(void)
{
    if (!usb_mode) { return ESP_OK; }
    if (usb_card) {
        free(usb_card);
        usb_card = NULL;
    }
    sdmmc_host_deinit();
    tf_card_lock();
    esp_err_t err = mount_card();
    if (err != ESP_OK) {
        working = (tf_snapshot_t){.error = err, .path = "/"};
        publish();
    } else {
        /* 恢复本地浏览：更新快照并让后台任务重新读取根目录。 */
        working.error = ESP_OK;
        working.mounted = true;
        working.busy = false;
        working.page = 0;
        working.count = 0;
        working.more = false;
        working.capacity_valid = false;
        working.card_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
        working.path[0] = '/';
        working.path[1] = '\0';
        publish();
    }
    usb_mode = false;
    tf_card_unlock();
    if (err == ESP_OK) { tf_card_browse("/", 0); }
    ESP_LOGI("tf_card", "TF exited USB MSC mode: %s", esp_err_to_name(err));
    return err;
}

esp_err_t tf_card_init(void)
{
    snapshot_mutex = xSemaphoreCreateMutex();
    fs_mutex = xSemaphoreCreateMutex();
    requests = xQueueCreate(2, sizeof(browse_request_t));
    if (!snapshot_mutex || !fs_mutex || !requests ||
        xTaskCreate(card_task, "tf_card", 6144, NULL, 1, NULL) != pdPASS) {
        if (requests) { vQueueDelete(requests); requests = NULL; }
        if (fs_mutex) { vSemaphoreDelete(fs_mutex); fs_mutex = NULL; }
        if (snapshot_mutex) { vSemaphoreDelete(snapshot_mutex); snapshot_mutex = NULL; }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}