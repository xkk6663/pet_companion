/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file slice_client.h
 * @brief S10 P4：事件切片上行（在线直推 + 断网落 SD 兜底）
 *
 * 数据形态：16 kHz / 16 bit / 单声道（与 `audio_recorder` 的 PCM 输出一致）。
 *
 * 设计（对应 S10 §3.7 的"默认在线直推"）：
 *   1. 1 MB PSRAM 环形缓冲 ≈ 32.7 s，只做**工作缓冲**：保住 VAD 起点前 300 ms；
 *   2. VAD 起 → 打起点标记；VAD 止 → 截取 [起点, 现在]，封 WAV，交给上传任务；
 *   3. 上传成功即释放；失败/队列满 → 写 `/sdcard/pet_memory/pending/`，由重传任务补；
 *   4. 全程不碰 littlefs（那里是模型/字体，写音频会写坏）。
 *
 * ★ 与语音会话的关系：本模块只发 HTTP `/slice`，**不发 WS `pcm`**。
 *   WS `pcm` 是"对话回合"语义，会触发 LLM/TTS；事件切片只做 ASR 入库。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（分配环形缓冲 + 起上传/重传任务）。幂等；失败只降级，不阻塞启动。 */
esp_err_t slice_client_init(void);

/** 是否可用（环形缓冲分配成功） */
bool slice_client_ready(void);

/**
 * @brief 喂入一帧 PCM（16k/mono/s16，任意长度）
 * @note  任意任务可调；内部有锁。未 init 时安全丢弃。
 */
void slice_client_feed(const int16_t *pcm, size_t samples);

/** VAD 起点：立即返回，真正的截取在 gate_end */
void slice_client_gate_start(void);

/** VAD 终点：截取当前切片并投给上传任务（不阻塞录音任务） */
void slice_client_gate_end(void);

/** 丢回预滚状态（TTS/音乐期间、或闸门让位时调） */
void slice_client_gate_reset(void);

/** 当前是否正在积累一个切片 */
bool slice_client_gate_active(void);

#ifdef __cplusplus
}
#endif
