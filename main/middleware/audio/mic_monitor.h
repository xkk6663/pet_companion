/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mic_monitor.h
 * @brief 双麦采集状态可视化：L/R RMS(dBFS) + 32 个对数频带 + 本地 VAD 代理
 *
 * 数据来源必须是 **codec 读出的原始 2ch PCM**（不是 AFE/BSS 的输出）：
 *   · 监听态：mic_frontend.c 的 diag_scan_stereo() 已经拿到原始交错 L/R；
 *   · 本地录音：rec_local_service.c 在 esp_codec_dev_read() 之后直接投递。
 *
 * ★ 线程纪律：
 *   - ingest 只做非阻塞入队（队列满丢当前帧），绝不等 FFT；
 *   - FFT/统计跑在独立的低优先级 mic_mon 任务里；
 *   - 结果写一份受临界区保护的快照，LVGL 只读快照，绝不反向碰音频队列；
 *   - 只统计幅度谱，右麦反相不影响结果。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_MON_BANDS       32
#define MIC_MON_FFT         256
#define MIC_MON_SAMPLE_RATE 16000

typedef struct {
    uint32_t seq;                    /*!< 快照序号：每次更新递增，仅供调试 */
    int8_t   db_l;                   /*!< 左麦 RMS dBFS，-80..0 */
    int8_t   db_r;                   /*!< 右麦 RMS dBFS，-80..0 */
    uint8_t  local_vad;              /*!< 0/1，本地能量代理（非 AFE VAD） */
    uint8_t  valid;                  /*!< 是否已有至少一帧分析结果 */
    uint8_t  band_l[MIC_MON_BANDS];  /*!< 左麦频带 0..100 */
    uint8_t  band_r[MIC_MON_BANDS];  /*!< 右麦频带 0..100 */
} mic_monitor_snapshot_t;

/** @brief 初始化（幂等）：分配 PSRAM 缓冲、建队列/任务、初始化软件 FFT */
esp_err_t mic_monitor_init(void);

/**
 * @brief 开/关可视化分析
 *
 * 只在 REC tab 可见时置 true；关闭后 ingest 直接丢弃，mic_mon 任务空转。
 * 打开/关闭都不影响录音/监听数据路径。
 */
void mic_monitor_set_active(bool active);

bool mic_monitor_is_active(void);

/**
 * @brief 投递一块原始 2ch/16bit 交错 PCM
 *
 * @param interleaved 交错样本（L,R,L,R...）
 * @param frames      帧数（每帧 2 个 int16）
 *
 * 非阻塞；队列满或未初始化时静默丢帧，绝不反压录音/AFE。
 */
void mic_monitor_ingest(const int16_t *interleaved, int frames);

/** @brief 复制一份稳定快照；无数据/未初始化返回 false */
bool mic_monitor_read(mic_monitor_snapshot_t *out);

#ifdef __cplusplus
}
#endif
