/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file event_bus.h
 * @brief 中间层：下行消息总线（WS 一行 JSON → 谁响应，收在一张表里）
 *
 * 依据：`S9_桌宠伴侣_可执行实施计划.md` §3.3；协议契约见
 *      `S9_云端网关_开发文档.md` §5.3（实测字段名，逐字对齐）
 *
 *   {"type":"text"}     → XZ_EVT_TEXT(role=assistant)     → 气泡 + 对话记录
 *   {"type":"emotion"}  → XZ_EVT_EMOJI                    → 桌宠表情状态机
 *   {"type":"audio"}    → xiaozhi_service_cloud_audio_play → 喇叭 + 嘴型
 *   {"type":"image"}    → cloud_media_service_fetch        → /sdcard/cloud_media + 图片卡
 *   {"type":"system"}   → reboot / set_volume / command_table /
 *                         explain / capture / punch / light（S9 §3.5 的 WS 等价物）
 *   {"type":"error"}    → XZ_EVT_ERROR                    → 状态行 + 对话记录
 *
 * ★ 全部在 **esp_websocket_client 任务上下文**执行：只许"解析 + 投队列"，
 *   绝阻塞（下载交给 cloud_media_service、相机交给 ai_camera、打卡交给
 *   ai_punch 的一次性任务）、绝不碰 LVGL。
 * ★ `system` 里带"动作"的命令（explain/capture/punch/light）与 HTTP 动作端点
 *   （web_server §3.5）是**同一批底层入口**：设备在 NAT 后面时云端只能走这里。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "xiaozhi_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 命令表最多缓存多少条（云端下发的插件命令词，供 UI 做快捷按钮，R4） */
#define EVENT_BUS_CMD_MAX  8

void event_bus_init(void);

/** 注册事件出口（xiaozhi_service 的 notify 桥；覆盖式） */
void event_bus_set_evt_cb(xiaozhi_evt_cb_t cb);

/**
 * @brief 处理一条完整的下行 JSON（由 ws_client 的收包回调调用）
 */
void event_bus_on_line(const char *line);

/**
 * @brief 取缓存的命令词（'\n' 分隔的 UTF-8 串，空串 = 云端还没下发）
 *
 * @deprecated 桌宠 UI 的 chips 已改由固件 `ai_action.c` 的 `TOOLS[]` 驱动
 *             （`ai_action_tool_label()`）。本接口**当前无消费者**，仅保留
 *             以兼容旧云端仍在下发的 `type=system/command_table`；
 *             网关那份 `config.json:command_table` 可以整条删掉。
 */
const char *event_bus_command_table(void);

/**
 * @brief 取第 idx 条命令词（0 基）；越界返回 NULL
 */
const char *event_bus_command_at(int idx);

#ifdef __cplusplus
}
#endif
