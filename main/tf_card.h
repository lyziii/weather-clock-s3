#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#define TF_PATH_SIZE 512
#define TF_PAGE_SIZE 12

/* 后台任务独占文件系统；界面获取固定大小快照，不持有卡或目录句柄。 */
typedef struct {
    char name[256];
    bool directory;
    uint64_t size;
} tf_entry_t;

typedef struct {
    bool mounted;
    bool busy;
    bool more;
    bool capacity_valid;
    esp_err_t error;
    uint64_t card_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
    unsigned page;
    unsigned count;
    unsigned revision;
    char path[TF_PATH_SIZE];
    tf_entry_t entries[TF_PAGE_SIZE];
} tf_snapshot_t;

esp_err_t tf_card_init(void);
bool tf_card_snapshot(tf_snapshot_t *snapshot);
bool tf_card_browse(const char *path, unsigned page);

/* 串行化所有 TF 卡访问（目录浏览、字库读取、挂载/卸载）。
 * 字库等外部 VFS 使用者在每次文件操作前后调用；USB 切换会先取此锁，
 * 保证卸载时没有正在进行的目录或字库访问。 */
void tf_card_lock(void);
void tf_card_unlock(void);

/* USB MSC（U盘）支持：进入 USB 模式时暂停本地文件系统、卸载 VFS 并重新
 * 初始化一张卡给 USB 块设备；退出时释放 USB 卡并重新挂载 VFS。 */
sdmmc_card_t *tf_card_enter_usb(void);
esp_err_t tf_card_exit_usb(void);
