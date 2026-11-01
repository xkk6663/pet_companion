/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file media_player_service.h
 * @brief 中间层：音乐播放服务（本地 esp_player 链路 / 蓝牙 A2DP 推流路由）
 *
 * 设计（与 audio_service 的命令队列模型一致）：
 *  - media_player_service_init() 创建常驻任务与命令队列；本地 esp_player 链路
 *    （media_lib 注册 + gmf pool + render + player）在**首次本地 PLAY 时惰性建立**。
 *  - LOCAL 输出走 audio_service ext 流式协议（ext_claim → ext_write → ext_release），
 *    从设计上消灭"同一 I2S 双句柄" P0。
 *  - BT 输出走 bt_push_service（io_file→解码→ASRC→ALC→SBC→io_bt），
 *    与 audio_service 的本地 codec 通路完全不相干，不占用 ext。
 *  - 事件回调在服务任务上下文执行（回调内操作 LVGL 前须自行 bsp_display_lock）。
 *
 * 占用语义：LOCAL PLAY 成功 ⇒ audio_service 处于 ext STREAM 占用；
 * FINISHED/STOP/ERROR 均会 release。BT PLAY 不占用 audio_service。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_EVT_STARTED = 0,   /* 已受理并起播 */
    MP_EVT_FINISHED,      /* 自然播完或被 STOP */
    MP_EVT_ERROR,         /* 链路建立失败 / 播放器错误 / 蓝牙不可用 */
} mp_evt_id_t;

typedef void (*mp_evt_cb_t)(mp_evt_id_t evt);

/** 音频输出路由：AUTO 优先蓝牙（已连接时），否则本地扬声器 */
typedef enum {
    MP_OUT_AUTO = 0,      /* 已连蓝牙→BT，否则 LOCAL */
    MP_OUT_LOCAL,         /* 强制本地扬声器（ext 流式协议） */
    MP_OUT_BT,            /* 强制蓝牙 A2DP 推流（未连接则报错） */
} mp_out_mode_t;

/**
 * @brief 初始化服务：创建任务与命令队列（本地 esp_player 链路首次 PLAY 才建）
 * @note  同时注册蓝牙推流结束回调（BT 播完 → MP_EVT_FINISHED）。
 */
void media_player_service_init(void);

/**
 * @brief 播放一个音频文件（.mp3 等解码器树内支持的格式）
 * @param path SD 卡绝对路径（如 /sdcard/music/a.mp3）
 * @return ESP_OK = 已受理（Started 事件稍后上抛）；
 *         ESP_ERR_INVALID_STATE = 服务忙 / 蓝牙未连接(BT 路由)；
 *         其它 = 链路建立失败
 * @note  任意任务上下文可调；重复 PLAY = 先 STOP 再起新曲。
 */
esp_err_t media_player_service_play(const char *path);

/**
 * @brief 停止播放并归还 codec / 停蓝牙推流（未播放时无害）
 */
void media_player_service_stop(void);

/** 暂停当前曲目（本地 esp_player_pause / 蓝牙 pipeline_pause） */
esp_err_t media_player_service_pause(void);

/** 从暂停恢复 */
esp_err_t media_player_service_resume(void);

/**
 * @brief 跳转到指定播放位置（毫秒）
 * @note  仅 LOCAL 路由支持；BT 路由返回 ESP_ERR_NOT_SUPPORTED（A2DP Source 无时间轴）。
 */
esp_err_t media_player_service_seek(int ms);

/** 是否处于暂停态 */
bool media_player_service_is_paused(void);

/** 当前播放位置（毫秒）；BT 路由恒为 0（无进度反馈） */
int media_player_service_get_position_ms(void);

/** 当前曲目总时长（毫秒）；未起播/BT 路由为 0 */
int media_player_service_get_duration_ms(void);

/**
 * @brief 是否正在播放（或已请求播放、尚未真正起管线）
 * @note  含 pending 段：play() 入队成功即算"播放中"，直到 STOP/FINISHED/
 *        ERROR 收尾。UI 轮询（tab_files refresh 回浏览态）必须用它。
 */
bool media_player_service_is_playing(void);

/**
 * @brief 设置输出路由（下一次 PLAY 生效；播放中调用不会中途迁移）
 */
void media_player_service_set_output_mode(mp_out_mode_t mode);
mp_out_mode_t media_player_service_get_output_mode(void);

/**
 * @brief 注册事件回调（覆盖式；在服务任务上下文调用）
 */
void media_player_service_set_evt_cb(mp_evt_cb_t cb);

#ifdef __cplusplus
}
#endif
