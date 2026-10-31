/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file voice_service.h
 * @brief 中间层：PTT 语音回合的**上行**（PCM 累积 → base64 → WebSocket 上行）
 *
 * 依据：`S9_桌宠伴侣_可执行实施计划.md` §3.2 / 修正 3
 *
 * 为什么不需要"另写采集"：av_processor 的 recorder 原生支持
 * `AV_PROCESSOR_FORMAT_ID_PCM`（`av_processor_type.h:49`），把编码格式从 OPUS
 * 切成 PCM 即整条管线复用（含 AFE 的 NS/VAD、16 kHz、单声道）。
 * ⇒ 本模块只做三件事：**累积、base64、封一帧 JSON**。
 *
 * ★ 内存（关键）：10 s 语音 = 320,000 B PCM → base64 426,672 字符。
 *   两段式（先存 PCM 再编码）要同时占 320 KB + 427 KB = 747 KB PSRAM；
 *   本实现**边收边编码**，只占一块 ~427 KB 的 PSRAM 缓冲（R8 的"单段 ≤10 s"）。
 *   缓冲在会话建立时一次分配、teardown 时释放 —— 与其它大块同生共死，
 *   避免运行中在 PSRAM 里打洞（本工程 §61 纪律）。
 *
 * ★ 为什么 buf 里要预留 prefix：JSON 是 `{"type":"pcm","data":"<427KB>"...}`，
 *   如果先拼 base64 再 memmove 出 21 B 前缀，等于白白搬 427 KB（PSRAM 上 ~100 ms）。
 *   这里把 21 B 前缀长度做成**编译期常量**、把 duration 固定成 6 位十进制，
 *   整帧长度就固定可算 ⇒ 发送时只写头尾，零搬运。
 *
 * ★ 线程：只有 xiaozhi_service 的上行任务会调用 `_feed`；`_finish` 同样由它调
 *   （发送会阻塞，绝不能在 LVGL 线程做）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单段上限（R8）：10 s @ 16 kHz/mono/s16 */
#define VOICE_MAX_PCM_SECONDS   10
#define VOICE_MAX_PCM_BYTES     (16000 * 2 * VOICE_MAX_PCM_SECONDS)

/** 最短有效语音：0.2 s（低于此判为误触，不发，省一次 ASR 调用） */
#define VOICE_MIN_PCM_BYTES     (16000 * 2 / 5)

/**
 * @brief 建缓冲（幂等）。会话建立时调，失败即整条语音链路不可用
 */
esp_err_t voice_service_init(void);

/** 释放缓冲（teardown 时调，幂等） */
void voice_service_deinit(void);

/** 缓冲是否就绪 */
bool voice_service_ready(void);

/** PTT 按下：清累积、置采集态 */
void voice_service_ptt_begin(void);

/**
 * @brief 累积一块 PCM（PTT 按住期间由上行任务调）
 * @note 只在 begin 之后有效；缓冲写满则**静默截断**并置溢出标志（R8）
 */
void voice_service_ptt_feed(const uint8_t *pcm, size_t len);

/**
 * @brief PTT 松开：封帧上行
 * @return ESP_OK                已发出
 *         ESP_ERR_INVALID_STATE 未在采集态 / WS 未连上
 *         ESP_ERR_INVALID_SIZE  有效语音过短（不发）
 * @note 发送阻塞（最大 ~427 KB），只能在上行任务/普通任务上下文调用
 */
esp_err_t voice_service_ptt_finish(void);

/** PTT 取消（收窗/断线）：丢弃累积，不发 */
void voice_service_ptt_cancel(void);

/** 当前累积的 PCM 字节数（给 UI 显示录音时长用） */
size_t voice_service_ptt_bytes(void);

/** 是否处于采集态 */
bool voice_service_ptt_active(void);

#ifdef __cplusplus
}
#endif
