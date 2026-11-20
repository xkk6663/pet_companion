/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_storage.h
 * @brief L1 平台层：介质容量查询（**UI 不再直接吃 littlefs 头**）
 *
 * UI 的"存储"卡片原来直接调 esp_littlefs_info()（include esp_littlefs.h），
 * 属于 UI 吃文件系统实现细节。改由本层转述：UI 只要"总共多少 / 用了多少"，
 * 换文件系统（littlefs → fatfs → 加密分区）时 UI 一行不改。
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

/** 主文件系统（littlefs，挂载点 /littlefs）容量。
 *  @param total_bytes 总字节（可 NULL）
 *  @param used_bytes  已用字节（可 NULL）
 *  @return ESP_OK / ESP_ERR_NOT_FOUND（未挂载） */
esp_err_t storage_fs_info(size_t *total_bytes, size_t *used_bytes);
