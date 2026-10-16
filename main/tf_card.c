#include <dirent.h>
#include <stdio.h>
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

#define TF_MOUNT "/tf"

typedef struct {
    char path[TF_PATH_SIZE];
    unsigned page;
} browse_request_t;

static SemaphoreHandle_t snapshot_mutex;
static QueueHandle_t requests;
static tf_snapshot_t published;
static tf_snapshot_t working;
static sdmmc_card_t *card;

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

static esp_err_t mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 38;
    slot.cmd = 40;
    slot.d0 = 39;
    slot.d1 = 41;
    slot.d2 = 48;
    slot.d3 = 47;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
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
        if (!card) {
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
        if (xQueueReceive(requests, &request, pdMS_TO_TICKS(3000)) == pdTRUE && card) {
            read_directory(&request);
        }
        /* 卡座没有独立检测引脚，用卡状态命令探测拔出，失败后释放挂载并重试。 */
        if (card && sdmmc_get_status(card) != ESP_OK) {
            esp_vfs_fat_sdcard_unmount(TF_MOUNT, card);
            card = NULL;
            working = (tf_snapshot_t){.error = ESP_ERR_NOT_FOUND, .path = "/"};
            publish();
        }
    }
}

esp_err_t tf_card_init(void)
{
    snapshot_mutex = xSemaphoreCreateMutex();
    requests = xQueueCreate(2, sizeof(browse_request_t));
    if (!snapshot_mutex || !requests ||
        xTaskCreate(card_task, "tf_card", 6144, NULL, 1, NULL) != pdPASS) {
        if (requests) { vQueueDelete(requests); requests = NULL; }
        if (snapshot_mutex) { vSemaphoreDelete(snapshot_mutex); snapshot_mutex = NULL; }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}