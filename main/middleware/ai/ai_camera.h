/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_camera.h
 * @brief 相机工具：拍照存证 + 拍照问云端（S7-A1 / S9 裁决 5）。
 *
 * 两条能力：
 *   1. capture（拍照存 SD）—— 转 capture_service，受理制非阻塞，零改动；
 *   2. explain（拍照问）—— **S9 改造后的新链路**：
 *        抓一帧 JPEG → `POST {gw}/upload/{sid}`（网关）拿 attachment_id
 *        → WS 上行 `{"type":"image_req","attachment_id":..,"text":..}`
 *        → 云端 Qwen-VL 看图 → 回答经 WS 下行（text/audio/emotion）
 *        → event_bus → 桌宠气泡 + 语音 + 对话记录。
 *
 * ★ 与旧链路的关键差别（为什么必须改）：
 *   旧 explain 依赖小智服务器在 MCP initialize 里下发的 vision URL+token
 *   （esp_xiaozhi_camera_explain）。S9 把编排整个搬到自己的网关后，这类
 *   下发通道不存在了；而设备端更不该持有任何视觉 API key（架构 §5.2 同理）。
 *   ⇒ 设备只负责"把字节送上去"，"看图"由云端大模型做。
 *   ⇒ **回答是异步的**：explain 返回 OK 只代表照片送达，答案稍后从下行来。
 *
 * 线程模型（三条硬约束的出处）：
 *  - explain 任务每次现建（PSRAM 栈，自删）：相机事务 + 一次 HTTP 上传。
 *    ★ F35：本任务只做相机/网络，不碰 NVS/littlefs/flash，外置栈安全；
 *    ★ 若将来网关改 https，TLS 握手也用这个 PSRAM 栈（同 ai_camera.c:60 范式）。
 *  - 相机事务：起流前查 camera_service_owner()，被 PREVIEW/MEDIA 占用时
 *    直接报"相机忙"（不抢断，与 CAM/VIDEO 页和平共处）。
 *  - 帧回调在 cam_task 上下文：只 memcpy + 置事件组（camera_service.h 硬约束）。
 *
 * 保留 MCP 工具壳（D5' 方案 A）：`self.camera.explain / capture` 仍注册进
 * 引擎，未来若恢复 MCP 传输可直接复用；当前引擎无传输层，实际入口是
 * ai_camera_explain_cloud()（云端 command / UI 按钮调用）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 结果回调（explain 任务上下文调用，勿阻塞勿碰 LVGL）；text 为 UTF-8 回答/错误提示。
 *  §73.18 起该回调只服务 UI 浮窗记录；服务端模型的结果走 tools/call response。 */
typedef void (*ai_camera_result_cb_t)(const char *text);

/**
 * @brief 初始化（幂等）：只建完成信号量，不再创建任何视觉客户端
 */
esp_err_t ai_camera_init(void);

/**
 * @brief 发起一次"拍照问云端"（**异步受理**，S9 裁决 5）
 *
 * 设备抓一帧 → 上传网关 → WS image_req。云端回答经下行 text/audio 回到
 * 桌宠气泡与喇叭（不在本函数返回值里）。
 *
 * @param question 想问的话（NULL/空串 = 默认"请用一句话描述这张照片里有什么"）
 * @return ESP_OK               已受理，explain 任务在跑
 *         ESP_ERR_INVALID_STATE 已有一次在跑，或相机被 CAM/VIDEO 页占用
 *         ESP_ERR_NO_MEM        起任务失败
 */
esp_err_t ai_camera_explain_cloud(const char *question);

/**
 * @brief 注册 MCP 相机工具（chat_setup 里与 ai_light/ai_speaker 并列挂载）
 */
esp_err_t ai_camera_attach(esp_mcp_t *mcp);

/**
 * @brief 注册结果回调（xiaozhi_service_init 里注册；未注册时结果只打 LOG）
 */
void ai_camera_set_result_cb(ai_camera_result_cb_t cb);

/**
 * @brief 取走最新一张拍照预览缩略图（§59b，overlay_ai 50ms 轮询调用）。
 *
 * 两个 MCP 摄像头工具（explain / capture）各拍一帧后，本模块解码出
 * RGB565 小图放进单槽；UI 轮询命中即**移交所有权**（返回 true 后 *buf
 * 由 UI free，展示 3s 后释放）。未命中返回 false。
 * 线程安全（临界区保护单槽）；可在 LVGL 任务调用。
 */
bool ai_camera_preview_take(uint8_t **buf, int *w, int *h);

/**
 * @brief 直接发布一帧 RGB565 预览进单槽（§73.19：打卡照片显示用）。
 *
 * 与 take 对偶的"生产者"入口：buf 为 RGB565（LVGL 原生小端）w*h*2 字节，
 * **所有权移交单槽**（UI 取走后由 UI 释放；被新帧顶掉时由本模块释放）。
 * buf 为 NULL 或尺寸非法时释放 buf（容错）。
 * 线程安全（临界区保护单槽）；任意任务可调。
 */
void ai_camera_preview_publish_rgb565(uint8_t *buf, int w, int h);

/**
 * @brief 预览单槽"视频流模式"开关（§73.23：打卡实时取景）。
 *
 * on=true：UI 轮询到新帧就连续换帧显示（实时视频），**不启动** 3s 隐藏
 * 定时器，并清掉残留的静态卡定时器——调用方必须在会话结束时置 false，
 * UI 检测到收尾沿会立即隐藏浮窗并释放显示缓冲。
 * on=false：恢复静态卡语义（一帧 + 3s 自动消失），不影响 explain/capture。
 * 默认 false。任意任务可调；开关本身不带帧，帧照常走 publish/take 单槽。
 */
void ai_camera_preview_video_set(bool on);

/** 读当前视频流模式状态（overlay_ai 轮询用，检测收尾沿）。 */
bool ai_camera_preview_video_get(void);

/**
 * @brief 对一张**已落盘**的照片补发预览（F69，异步）。
 *
 * 拍照的两条触发路都要能出浮窗预览：MCP 工具 cb_capture 自带预览任务；
 * 云端动作快速路 ai_action::do_capture 拿到成功结果后调本 API。
 * 起一次性任务（PSRAM 栈，自删）回读文件 → 解码 → publish 进预览单槽，
 * 立即返回；失败静默（预览是赠品）。任意任务可调。
 */
void ai_camera_preview_file(const char *path);

#ifdef __cplusplus
}
#endif
