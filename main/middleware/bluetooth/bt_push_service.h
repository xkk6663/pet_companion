/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.h
 * @brief A2DP Source 推送服务（方案 B「延迟常驻」）：把 SD 卡 MP3 推到蓝牙耳机/音箱。
 *
 * 数据流：io_file(MP3) -> aud_dec -> aud_asrc -> aud_alc -> aud_enc(SBC) -> io_bt
 *
 * 方案 B 设计要点：
 *  - P1 时机前置：pool 在 bt_push_service_early_init() 建，抢开机内部 RAM 最空的窗口
 *    （与 bt_audio_service_controller_init 同一个窗口，在 LCD/WiFi/摄像头之前）；
 *  - P2 显式外置：GMF task 栈 stack_in_ext=true 进 PSRAM；uri/weight 缓冲在 PSRAM；
 *  - P3 建一次不毁：pipeline 常驻，STOPPED/RELEASED 只停不毁；
 *  - P4 销毁异步：真正的 destroy 只在 reaper 任务上下文执行，绝不在 BT 事件回调里做。
 *
 * 事件驱动：由 bt_audio_service 在 STREAM_STATE_CHG 里调用
 * bt_push_service_on_stream_state(stream, state)。
 */

#ifndef BT_PUSH_SERVICE_H
#define BT_PUSH_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机早期预建 GMF pool / pipeline / task（抢内部 RAM 最空窗口）。
 * @note  必须在 bsp_display_start() 之前调用；失败只打日志不 abort（蓝牙非必需功能）。
 */
void bt_push_service_early_init(void);

/**
 * @brief 登记待推送的本地文件路径。
 * @note  uri 在 STREAM ALLOCATED 事件到来时才真正下发给 pipeline。
 * @param path SD 卡上的 MP3 绝对路径，如 /sdcard/music/a.mp3
 */
esp_err_t bt_push_service_set_uri(const char *path);

/** 暂停推流（已连接且正在推流时有效） */
esp_err_t bt_push_service_pause(void);

/** 从暂停恢复推流 */
esp_err_t bt_push_service_resume(void);

/**
 * @brief 跳到本曲指定播放位置（毫秒）。
 * @note  在 io_file 上按「毫秒→字节」换算并做 MP3 帧头对齐；进度墙钟同步到目标点。
 *        暂停时只记下目标，恢复时补做（暂停期间 IO 任务不保证会跑）。
 */
esp_err_t bt_push_service_seek_ms(int ms);

/** 停止推流并复位 pipeline（不销毁 pipeline/task） */
esp_err_t bt_push_service_stop(void);

/** 当前是否在推流 */
bool bt_push_service_is_playing(void);

/**
 * @brief 当前推流进度（毫秒）。
 * @note  A2DP Source 侧拿不到对端时间轴，本服务按「推流墙钟」推算：
 *        io_bt 严格按实时节奏消费数据，故墙钟与音频时间轴误差 < 100 ms；
 *        pause/resume 期间停表，不会出现暂停还往前走的情况。
 *        未推流 / 未设 uri 时返回 0。
 */
int bt_push_service_get_position_ms(void);

/**
 * @brief 当前曲目总时长（毫秒）；解析不出（非 MP3/异常头）时返回 0。
 * @note  在 set_uri() 时解析一次 MP3 帧头（支持 Xing/Info 帧数与 CBR），
 *        供 UI 进度条与歌词时间轴使用。
 */
int bt_push_service_get_duration_ms(void);

/** 请求异步销毁 pipeline（蓝牙关闭时调用；实际销毁在 reaper 任务里做） */
void bt_push_service_request_destroy(void);

/**
 * @brief 由 bt_audio_service 在 STREAM_STATE_CHG 事件里调用。
 * @param stream  stream handle（透传自 esp_bt_audio_event_stream_st_t）
 * @param state   ESP_BT_AUDIO_STREAM_STATE_* 四态
 */
void bt_push_service_on_stream_state(void *stream, int state);

/**
 * @brief 注册"播放自然结束"回调。
 * @note 回调在 GMF 事件线程上下文执行，**不得**直接调用阻塞的
 *       esp_bt_audio_media_stop()，应只投递命令/置标志。
 */
void bt_push_service_set_finished_cb(void (*cb)(void));

/** 该曲是否已自然播完（供 UI 轮询/收尾判断）。 */
bool bt_push_service_is_finished(void);

/** 清除"播完"标志（上层收尾完成后调用） */
void bt_push_service_clear_finished(void);

/**
 * @brief 设置推流数字音量（aud_asrc 与 aud_enc 之间的 aud_alc 数字衰减）。
 * @note  未推流时调用同样有效：增益保存在常驻的 ALC 元素里。
 * @param vol 音量档位 0..100（0 = 静音，100 = 不衰减）
 */
esp_err_t bt_push_service_set_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif /* BT_PUSH_SERVICE_H */
