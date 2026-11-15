/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mic_gate.h
 * @brief S10 P1/P2：VAD 常开闸门（独占麦克风的"监听态"）
 *
 * 语义：
 *   · 空闲时由本模块持有 spk/mic + recorder，VAD 起止驱动 slice_client 切片；
 *   · 任何需要麦克风的既有功能（AI 会话 / 本地录音 / 音乐播放）启动前调
 *     `mic_gate_yield()`，本模块在 300 ms 内收口并**完整归还**独占租约；
 *   · 释放后由监督任务在资源空闲时自动 `resume`（不需要调用方记得恢复）。
 *
 * ★ 当前默认开启：`MIC_GATE_ENABLE 1`。2026-11-15 已在 COM9 实机验收：
 *   启动 `listening`、VAD 触发切片、直推 `/slice`、`task_wdt=0`；
 *   置 0 仍是一行回退点。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** VAD 对外状态（只读，给 UI 用；不改变 gate 行为） */
typedef enum {
    MIC_GATE_VAD_UNKNOWN = 0,   /* 未占麦 / 无 AFE 状态 */
    MIC_GATE_VAD_SILENCE,       /* AFE 认为当前静音 */
    MIC_GATE_VAD_SPEECH,        /* VAD_START 之后 */
    MIC_GATE_VAD_MERGE,         /* VAD_END 后 1.2 s 合并窗口内 */
} mic_gate_vad_state_t;

/** 编译期总开关（1=启用监听态）。当前默认 1：实机验收通过；置 0 回退到旧行为。 */
#ifndef MIC_GATE_ENABLE
#define MIC_GATE_ENABLE 1
#endif

/** 初始化（幂等）：仅建监督任务，不立刻占麦。 */
esp_err_t mic_gate_init(void);

/** 运行期查询/开关（调试接口；关闭会立刻 yield） */
bool mic_gate_enabled(void);
void mic_gate_set_enabled(bool on);

/** 当前是否持有麦克风管线 */
bool mic_gate_is_owner(void);

/** 当前 AFE VAD 状态（只读快照；UNKNOWN = gate 未占麦或未初始化） */
mic_gate_vad_state_t mic_gate_vad_state(void);

/**
 * @brief 让位：停止采集、关闭 recorder、归还 codec 独占租约。
 * @param timeout_ms 等待采集任务退出的上限（建议 300）
 * @return true=已完全归还（或本来就没占用）；false=超时仍在收口
 */
bool mic_gate_yield(uint32_t timeout_ms);

/** 立刻尝试恢复监听（资源忙则忽略；平时由监督任务自动做） */
void mic_gate_resume(void);

/**
 * @brief 让位后的"预约窗口"：ms 内禁止监督任务重新占麦。
 *        给异步启动方（媒体播放/本地录音）留出 claim 的时间，避免 2 s 轮询
 *        恰好插在 yield 与 claim 之间。
 */
void mic_gate_block(uint32_t ms);

#ifdef __cplusplus
}
#endif
