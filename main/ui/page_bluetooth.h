/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_bluetooth.h
 * @brief SYS > Bluetooth 子页面（电源开关 + 搜索 + 设备列表 + 断开）。
 *
 * 与主线 display_audio_photo 的同名页面对齐，但按本项目裁剪：
 * A2DP 角色固定 Source，**没有** Sink/角色切换按钮（§6 裁剪 Sink），
 * 也**没有** AVRCP 音量（Source 方向不可用，§6-R6）。
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const Page_t Page_Bluetooth;

#ifdef __cplusplus
}
#endif
