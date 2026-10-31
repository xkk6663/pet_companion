/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cloud_media_service.h
 * @brief 中间层：云端下发的图片（生图 / 插件图）下载 + 落盘 + 解码（S9 P6）
 *
 * 依据：`S9_桌宠伴侣_可执行实施计划.md` §3.6 / §5-P6
 *
 * 链路：WS 收到 `{"type":"image","url":"/static/img_x.jpg","caption":"..."}`
 *       → 本模块（独立任务，**内部 RAM 栈**，因为要写 SD 卡，F35）
 *       → HTTP 拉流到 PSRAM → 落盘 `/sdcard/cloud_media/img_NNN.jpg`
 *       → 硬件 JPEG 解码成 RGB565（PSRAM）
 *       → 单槽发布，UI 的 50 ms 定时器 take 走后上屏
 *
 * ★ 为什么落盘：`/sdcard/cloud_media/` 是设备的"唯一大容量载体"，
 *   图片留在卡上才能被 FILES 页浏览（验收判据）与后续复用。
 * ★ 为什么用硬件解码器（esp_driver_jpeg）而不是 esp_new_jpeg 软解：
 *   与 viewfinder 同一条已验证路径（PSRAM 无加密、cache 对齐由
 *   `jpeg_alloc_decoder_mem` 保证），且不引入新组件。
 * ★ 为什么 take 是**所有权转移**：与 `ai_camera_preview_take` 同一约定 ——
 *   UI 拿到指针后自负释放，服务端只保留"新帧"这一份。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 落盘目录（SD 卡根下） */
#define CLOUD_MEDIA_DIR       "/sdcard/cloud_media"

/** 下载体积上限（PSRAM 一块，超了直接拒，不落盘） */
#define CLOUD_MEDIA_MAX_BYTES (1200 * 1024)

/** 解码像素上限（RGB565 2 B/px ⇒ ≤ 2 MB PSRAM） */
#define CLOUD_MEDIA_MAX_PIXELS (1024 * 1024)

/** caption 长度上限 */
#define CLOUD_MEDIA_CAPTION_MAX 96

/** 初始化（幂等，只建队列） */
void cloud_media_service_init(void);

/**
 * @brief 请求下载一张图（**非阻塞**，WS 收包任务上下文安全）
 * @return ESP_OK 已受理 / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM（队列满）
 */
esp_err_t cloud_media_service_fetch(const char *url, const char *caption);

/**
 * @brief 取走最新解码好的图（所有权转移：调用方负责 heap_caps_free）
 * @param rgb565  输出 RGB565 缓冲（PSRAM）
 * @param w,h     逻辑宽高
 * @param stride  行跨距（字节；4:2:2 解码会把宽度补齐到 16 的倍数）
 * @param caption 输出 caption（可为空串）
 * @return true = 有一帧可取
 */
bool cloud_media_service_take(uint8_t **rgb565, int *w, int *h, int *stride,
                              char *caption, size_t cap_size);

/** 最近一次失败原因（UI 显示用；空串 = 没失败过） */
const char *cloud_media_service_last_error(void);

#ifdef __cplusplus
}
#endif
