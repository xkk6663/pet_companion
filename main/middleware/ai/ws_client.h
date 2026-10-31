/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ws_client.h
 * @brief 中间层：云端网关 WebSocket 长连接客户端（S9 桌宠伴侣 P1）
 *
 * 依据：`docs/开发文档/S9_桌宠伴侣_可执行实施计划.md` §3.1 / §2.7
 *      `docs/开发文档/S9_云端网关_开发文档.md` §5.1（实测契约）
 *
 * 定位：设备端**唯一**的云端连接。连上 `ws://<host>:8081/ws/{session_id}` 之后
 *      只做三件事：收发一行一条 JSON、断线指数退避重连、把连上/断开事件上抛。
 *      **不解析业务语义**（那是 event_bus 的事），**不碰 LVGL**。
 *
 * 协议（服务端实测，见云端网关文档 §5）：
 *   上行 {"type":"text"|"pcm"|"event"|"image_req", ...}
 *   下行 {"type":"text"|"audio"|"image"|"emotion"|"system"|"error", ...}
 *   帧间以 '\n' 分隔；服务端按行解析，我们下行也按行拆分。
 *
 * ★ 线程纪律（F44 同族）：
 *   - 收包回调 `rx_cb` 跑在 **esp_websocket_client 任务**上下文：只许解析/投队列，
 *     绝阻塞、绝碰 LVGL、绝做 HTTP/文件 IO（要下载交给 cloud_media_service 的任务）；
 *   - `ws_client_send_text` 任意任务可调（内部互斥），但它会阻塞到整帧写完
 *     —— 427 KB 的 PCM 上行大约几百 ms，别在 LVGL 线程里调（由上行任务调）。
 *
 * ★ 栈与内存（本工程铁律 F35 / R3）：
 *   - WS 任务栈必须**内部 RAM**（`xTaskCreate` 只从内部堆拿），8 KB；
 *     vendor 小智那套用的是 12 KB（esp_xiaozhi_websocket.c:260 的注释解释了
 *     为什么不能更小：它要在任务里跑 cJSON + MCP 工具）。我们把工具层搬走，
 *     任务里只剩 cJSON 解析 + 投队列，8 KB 够用且比原来省 4 KB 内部 RAM。
 *   - 收发缓冲（行累加）走 **PSRAM**，与内部 RAM 余量无关。
 *
 * ★ 重连（R2 / D10）：**无状态恢复** —— 断线丢当前回合，不补发未确认消息。
 *   退避 1s→2s→4s→…→30s，**连上即复位为 1s**。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 收包回调：一条完整 JSON（已按 '\n' 切好、已去空白；指针只在回调期有效） */
typedef void (*ws_client_rx_cb_t)(const char *line, void *ctx);

/** 连接事件回调：true=已连上（可发消息），false=断开（正在退避重连） */
typedef void (*ws_client_state_cb_t)(bool connected, void *ctx);

/**
 * @brief 初始化（幂等，只建互斥量/读 NVS 配置，**不连网**）
 */
esp_err_t ws_client_init(void);

/**
 * @brief 开始连接（幂等；已在跑则直接返回 OK）。
 *        内部起一个 supervisor 任务负责"连接 → 断线 → 指数退避重连"。
 *        ★ 需要 WiFi 已联网：未联网时会一直重试到连上为止（不报错）。
 */
esp_err_t ws_client_start(void);

/**
 * @brief 主动断开并停止重连（幂等）。返回后保证不会再有 rx_cb 被调用
 */
void ws_client_stop(void);

/** 当前是否处于 CONNECTED（可发消息） */
bool ws_client_is_connected(void);

/**
 * @brief 发一行 JSON 文本帧（线程安全；内部自动加锁 + 末尾补 '\n' 不必）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（未连上）/ ESP_FAIL（发送失败）
 * @note 会阻塞到整帧交给 lwip（大帧几百 ms），别在 LVGL 线程调
 */
esp_err_t ws_client_send_text(const char *json);

void ws_client_set_rx_cb(ws_client_rx_cb_t cb, void *ctx);
void ws_client_set_state_cb(ws_client_state_cb_t cb, void *ctx);

/** 会话 ID（`xz-s31-<mac末4位>`；init 后恒定，可用于云端 /push 定位设备） */
const char *ws_client_get_session_id(void);

/**
 * @brief 改网关地址（持久化到 NVS，下次 start 生效）。
 * @param uri 完整 URI，如 `ws://1.2.3.4:8081/ws/`（末尾的 session_id 自动拼）
 *            ★ 以 '#' 结尾表示"用户给的是完整地址"，不再拼 session_id
 * @return ESP_OK / ESP_ERR_INVALID_ARG（非 ws:// 或 wss:// 开头）
 */
esp_err_t ws_client_set_uri(const char *uri);

/** 读当前生效的完整 URI（含 session_id，供网页端展示/诊断） */
void ws_client_get_uri(char *out, size_t out_size);

/** 读用户配置的 base（不含 session_id，用于网页端回填） */
void ws_client_get_base_uri(char *out, size_t out_size);

/**
 * @brief 由网关地址推出静态资源基址（`wss://h:p/ws/x` → `https://h:p`）。
 *        云端下行的 `audio.url` / `image.url` 都是**相对路径**（`/static/x.wav`），
 *        设备必须自己拼出可拉流的绝对地址（云端文档 §5.3 / §11）。
 */
void ws_client_get_http_base(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
