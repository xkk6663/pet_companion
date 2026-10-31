/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file xiaozhi_service.h
 * @brief 中间层：桌宠会话服务（原"小智对话服务"改造版，S9 P1~P3）
 *
 * 变更摘要（对照 `S9_桌宠伴侣_可执行实施计划.md` §3.4）：
 *   · **保留**：状态机 IDLE→PREPARING→CHATTING→RELEASING、命令队列、
 *     audio_service 独占租借、PTT 期望态 API、音量 API、事件枚举与回调桥；
 *   · **删除**：esp_xiaozhi 会话 / Opus 上下行 / 激活检查 / wake word /
 *     §53 聋化自愈 / §52 abort_speaking 两段式欠票（新 barge-in 只需"停播+上行事件"）；
 *   · **替换**：连接改为 ws_client（云端网关），上行由 Opus 帧改为 PCM 整段
 *     base64（voice_service），下行语音由 Opus feeder 改为 URL 播放（playback）。
 *
 * 音频所有权（不变）：会话期通过 `audio_service_ext_claim_exclusive` 持有 spk/mic，
 * 期间 WAV/录音/通话/MP3 全被拒；退出必经 `ext_release_exclusive` 归还。
 *
 * 线程纪律（F44 同族，不变）：所有回调只置 volatile 快照 / 投队列，绝不碰 LVGL；
 * UI 用 lv_timer 轮询 get_state() 与事件队列。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XZ_STATE_IDLE = 0,
    XZ_STATE_PREPARING,   /* 起音频管线 + 连网关（1~2s，事件通知到位） */
    XZ_STATE_CHATTING,
    XZ_STATE_RELEASING,
} xiaozhi_state_t;

typedef enum {
    XZ_EVT_STATE = 0,     /* 状态机变化 */
    XZ_EVT_TEXT,          /* 对话文本；text/role 有效（指针回调期有效） */
    XZ_EVT_TTS_STATE,     /* TTS start/stop；text 恒 NULL */
    XZ_EVT_ERROR,         /* err + text(=来源/提示) */
    XZ_EVT_EMOJI,         /* 表情名；text = "happy"/"sad"/"sleeping"/... */
    XZ_EVT_LINK,          /* 网关连接变化；role=1 已连上 / 0 已断开 */
} xiaozhi_evt_id_t;

typedef struct {
    xiaozhi_evt_id_t id;
    const char *text;     /* XZ_EVT_TEXT/XZ_EVT_ERROR 时有效 */
    int role;             /* 0=user 1=assistant（XZ_EVT_TEXT / XZ_EVT_LINK） */
    esp_err_t err;        /* XZ_EVT_ERROR */
} xiaozhi_evt_t;

typedef void (*xiaozhi_evt_cb_t)(const xiaozhi_evt_t *evt);

/* ---- 下行 URL 音频（TTS / 点歌共用同一个 playback 句柄，见实现说明）---- */
typedef enum {
    XZ_AUDIO_STARTED = 0,   /* 播放器实际起播（simple_player 任务上下文回调） */
    XZ_AUDIO_FINISHED,      /* 自然播完 */
    XZ_AUDIO_STOPPED_EVT,   /* 被停（换曲/打断/teardown） */
    XZ_AUDIO_ERROR,         /* 链路失效/解码失败 */
} xz_music_evt_id_t;

typedef void (*xz_music_evt_cb_t)(xz_music_evt_id_t evt, esp_err_t err);

/**
 * @brief 初始化：建任务/队列/事件总线（不占音频资源、不连网）
 */
void xiaozhi_service_init(void);

/**
 * @brief 进入会话（异步）：借 codec → 建音频管线 → 连网关。
 * @return ESP_OK=已受理；ESP_ERR_INVALID_STATE=已在会话中或 audio_service 忙
 */
esp_err_t xiaozhi_service_start(void);

/** @brief 退出会话（异步；RELEASING 完成后回 XZ_STATE_IDLE） */
esp_err_t xiaozhi_service_stop(void);

xiaozhi_state_t xiaozhi_service_get_state(void);

/** 网关当前是否已连上（长连接就绪，可收发） */
bool xiaozhi_service_link_up(void);

/**
 * @brief 本机 MCP 引擎句柄（`esp_mcp_t *`，以 void* 返回避免头文件依赖）。
 *
 * 给 `ai_action.c` 做进程内 JSON-RPC 用：caps 取自引擎 `tools/list`，
 * 动作走 `tools/call` —— 工具清单因此只有一份真值（ai_*.c 的注册代码）。
 *
 * @return 引擎句柄；还没 attach（开机早期）返回 NULL
 */
void *xiaozhi_service_mcp_engine(void);

/** TTS 是否正在播（嘴型同步用；也用于上行闸门） */
bool xiaozhi_service_tts_active(void);

/**
 * @brief 上行一段文字（屏幕键盘 / 命令快捷词）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（网关未连上）
 */
esp_err_t xiaozhi_service_send_text(const char *text);

/**
 * @brief 上行一个事件（barge_in / status / button ...）
 */
esp_err_t xiaozhi_service_send_event(const char *event);

/**
 * @brief PTT 按住说话开始。
 *        会话未就绪时返回 INVALID_STATE（UI 负责先 start 并挂 pending）。
 *        ★ 若此刻正在播放下行音频 = **打断**：立刻停播 + 上行 barge_in。
 */
esp_err_t xiaozhi_service_ptt_start(void);

/** @brief PTT 松开（任意时刻可调，幂等）。封帧上行由上行任务完成 */
void xiaozhi_service_ptt_stop(void);

/** @brief 会话态设置扬声器音量（作用于借出的 spk 句柄，0~90） */
esp_err_t xiaozhi_service_set_volume(int vol);

/** @brief 最近一次成功设置的音量（0~90；-1 = 本次开机还没设置过）。供状态查询 */
int xiaozhi_service_get_volume(void);

/** @brief 注册事件回调（覆盖式；服务任务上下文调用，勿阻塞勿碰 LVGL） */
void xiaozhi_service_set_evt_cb(xiaozhi_evt_cb_t cb);

/* ---- 下行 URL 音频 ---- */

/**
 * @brief 播放云端下发的音频 URL（TTS / 点歌共用）。
 *        ★ 会话不在时会**自动起会话并挂起这次播放**（桌宠主动推送场景：
 *          云端推来 TTS 时设备可能正空闲）；起会话失败则放弃并上抛 ERROR。
 * @return ESP_OK=已受理/已挂起；INVALID_ARG=空 url
 */
esp_err_t xiaozhi_service_cloud_audio_play(const char *url);

/** @brief 会话内播放一首 URL（点歌，语义同 cloud_audio_play，保留旧名给 ai_music）
 *
 * ★ 走**独立的音乐播放通道**（priority 1），与 TTS 通道（priority 2）分开：
 *   TTS 一来音乐只会被 policy **暂停**、TTS 说完自动**从原位置续播**，
 *   不再像过去那样被 stop 掉（见 F63）。通道开不起来时自动退化成共用 TTS 通道。
 * @return ESP_OK=已受理；INVALID_STATE=会话不在/两个通道都没就绪
 */
esp_err_t xiaozhi_service_music_play(const char *url);

/** @brief 停止音乐那一路（**不动** TTS，避免把正在说的话砍断；幂等） */
esp_err_t xiaozhi_service_music_stop(void);

void xiaozhi_service_set_music_evt_cb(xz_music_evt_cb_t cb);

bool xiaozhi_service_music_playing(void);

#ifdef __cplusplus
}
#endif
