/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_storage.c
 * @brief L1 平台层：介质容量查询实现
 *
 * 纪律：只做"读容量"这一件事，不 mount / 不 format / 不写文件 —— 挂载归
 * fs_service（L2 服务），本层只转述底层文件系统的统计接口。
 */

#include "platform/port_storage.h"

#include <stdint.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "PORT_STOR";

/** littlefs 分区标签（与 CMake/分区表一致：storage） */
#define PORT_FS_PART_LABEL "storage"

esp_err_t storage_fs_info(size_t *total_bytes, size_t *used_bytes)
{
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (used_bytes != NULL) {
        *used_bytes = 0;
    }

    size_t total = 0;
    size_t used = 0;
    esp_err_t err = esp_littlefs_info(PORT_FS_PART_LABEL, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "littlefs info failed: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = used;
    }
    return ESP_OK;
}
