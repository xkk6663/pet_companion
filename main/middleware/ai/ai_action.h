/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_action.h
 * @brief 云端「动作下发 / 结果上报」（S9 P0：反向工具 relay）。
 *
 * ★ 为什么需要它：S9 之后 MCP 引擎虽然还 attach 着 15 个工具，但**没有任何
 *   transport**（裁决 D5' 方案 A「引擎空转」），云端 LLM 无法 tools/call。
 *   同时设备在 NAT 后面，云端也不可能 HTTP 直打 `/api/punch`（网关文档
 *   §10.2 的老写法物理上走不通）。
 *
 *   唯一可行的反向通道就是那条**已经常连的 WS**：
 *
 *     网关  --{"type":"action","id":N,"name":"punch","args":{}}-->  设备
 *     网关  <--{"type":"event","event":"action_result","id":N,...}--  设备
 *
 *   网关据此回一句真实结论（不再让 LLM 编"记上了"）。
 *
 * ★ 线程纪律：本模块在 WS **收包任务**里被调用 ⇒ `ai_action_dispatch()`
 *   只做入队，立即返回；真正的等待（打卡几十秒 / 拍照几秒）在独立 worker
 *   里做。收包任务一旦阻塞，TTS/心跳全会停（F-S9 家族红线）。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 队列 + worker 任务（幂等）。xiaozhi_service_init 末尾调用。 */
esp_err_t ai_action_init(void);

/**
 * @brief 受理一条动作请求（非阻塞）。
 *
 * @param name      动作名 = 能力表里上报的名字（punch / capture / music /
 *                  wallpaper / volume / light_set_power / music_stop ...）。
 *                  就是 caps 里那个 `name`，云端原样回传，无需任何映射表。
 * @param args_json 参数对象 JSON（可为 NULL）。走引擎的工具它直接变成
 *                  `tools/call` 的 arguments；手写的 5 个自己从里面取。
 * @param id        网关下发的请求号，原样回传，用于配对
 * @return ESP_OK 已入队；ESP_ERR_INVALID_ARG 名字不认识；ESP_ERR_INVALID_STATE 队列满
 */
esp_err_t ai_action_dispatch(const char *name, const char *args_json, int id);

/**
 * @brief 上报能力表（非阻塞，WS CONNECTED 时调用）。
 *
 * ★ v2（2026-10-31）：能力表**不再来自手写数组**，而是向本机 MCP 引擎发
 *   `tools/list`，把引擎里真实注册的工具（15 个）整表转成
 *   `{"type":"caps","model":..,"tools":[{"name","label","aliases","arg_mode",
 *   "arg_key","desc","params","timeout_ms"}]}` 发上去。
 *
 *   名字/说明/参数 schema 全部由 `ai_*.c` 自己声明 ⇒ **再加一个 MCP 工具
 *   只改 ai_*.c，本文件与云端都不动**。手写表只保留引擎给不了的 5 个
 *   （wallpaper 是纯本地概念；punch/capture/music/volume 的返回值要专门
 *   翻译成"能念给用户听"的中文真结论）。
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 队列满或未初始化（下次重连会重报）
 */
esp_err_t ai_action_report_caps(void);

/**
 * @brief 本地快捷动作（UI chips 用）。
 *
 * ★ 只列 `chip==true` 的条目：屏幕 800 px 排不下 16 个，台灯/Wi-Fi 这类
 *   本来就该用嘴说。
 */
int ai_action_tool_count(void);
/** @return 第 idx 个动作的短标签（如"打卡"）；越界返回 NULL */
const char *ai_action_tool_label(int idx);

#ifdef __cplusplus
}
#endif
