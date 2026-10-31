/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pet_overlay.h
 * @brief 桌宠浮窗（S9 P5，从 overlay_ai 升级）
 *
 * 结构（800x480，全屏浮窗，lv_layer_top，滑入/滑出）：
 *
 *   ┌───────────────────────────────────────────────────────────┐
 *   │ 大肥鱼 · 桌宠      [idle]                  [ 状态 ] [ X ] │
 *   │ ┌────────┐  ┌──────────────────────────────────────────┐ │
 *   │ │  鱼    │  │  对话气泡（最近一句，大字号，自动换行）   │ │
 *   │ │ (矢量) │  └──────────────────────────────────────────┘ │
 *   │ │        │  ┌──────────────────────────────────────────┐ │
 *   │ │        │  │  对话记录（可滚动，小字号）               │ │
 *   │ └────────┘  └──────────────────────────────────────────┘ │
 *   │            [命令1][命令2][命令3][命令4]        （云端下发）│
 *   │ [ 按住说话 ] [ 键盘 ] [ 清屏 ]                            │
 *   └───────────────────────────────────────────────────────────┘
 *
 * 线程纪律（F44 同族，与 overlay_ai 一致）：
 *   - xiaozhi 服务回调（WS/播放器任务上下文）：只把事件拷进队列；
 *   - 按键回调（iot_button 任务）：只置 volatile 标志 + 调服务 API；
 *   - LVGL 落笔**全部**在 50 ms lv_timer 里完成。
 */

#pragma once

#include <stdbool.h>

#include "middleware/periph/button_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 建 UI（page_main 里调一次；不占音频资源、不连网） */
void pet_overlay_init(void);
void pet_overlay_deinit(void);

void pet_overlay_show(void);
void pet_overlay_hide(void);
bool pet_overlay_is_visible(void);

/** 硬件按键入口（SET=开/关浮窗，MODE=按住说话） */
void pet_overlay_key_feed(btn_id_t id, btn_evt_t evt);

/** 外部注入一句本地提示（网页端 /api/pet/say 自测用） */
void pet_overlay_say_local(const char *text);

#ifdef __cplusplus
}
#endif
