#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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