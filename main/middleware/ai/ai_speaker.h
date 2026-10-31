/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_speaker.h
 * @brief 小智 MCP 音量工具（§56）：78 官方 self.speaker 命名规范。
 *
 *   self.speaker.set_volume   volume: 0~100（内部按板子上限 90 收口）
 *   self.speaker.get_volume   查询当前音量
 *
 * 三通道同改（按键音量/AI 语音/UI 滑块同一状态源）：
 *   ① button_service_set_volume   —— 全局状态源（UI 弹窗/滑块读它）
 *   ② audio_service_post_cmd      —— 播放态（STREAM/MP3/WAV）生效
 *   ③ xiaozhi_service_set_volume  —— 小智对话态作用于借出的 spk 句柄
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_mcp_s esp_mcp_t;

/** 把音量工具注册进小智 MCP 引擎（chat_setup 里、chat_init 前调用）。 */
esp_err_t ai_speaker_attach(esp_mcp_t *mcp);

/**
 * @brief 设音量（0~100 入参，内部按板子上限 90 收口），三通道同改。
 *
 * ★ S9 P7：MCP 引擎空转后，云端只能通过 WS action relay 调设备，
 *   所以把这条原本在 MCP 回调里的实现提成公开 API —— 一份逻辑两个入口。
 * @return 板上实际生效值（0~90）
 */
int ai_speaker_set_volume(int v);

/** @brief 当前音量（0~90，来自 button_service 这个唯一状态源） */
int ai_speaker_get_volume(void);

#ifdef __cplusplus
}
#endif
