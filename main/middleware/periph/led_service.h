/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file led_service.h
 * @brief 中间层：RGB LED 服务（WS2812 on GPIO37, RMT 后端）
 *
 * ★ 呼吸灯按**绝对 UTC**同步（规范实现见同目录 breath_sync.c/h，2026-11-15）：
 *   亮度 = LUT[(utc_us % 4 s)]，不是本机定时器的函数 ⇒ 多台设备并排同相。
 *   - 授时通道：SNTP（net_time_service_time_valid() 为真后每 1 s 喂一口）；
 *   - 未授时：自由运行 + 半亮守卫（现场可区分"同步/未同步"）；
 *   - 手动颜色（RGB 页 / 小智灯控）不呼吸，保持常亮，与旧行为一致。
 *
 * 并发保护（FreeRTOS）：
 *  - 内部建命令队列 + 专用 LED 任务；所有硬件操作串行执行（25 fps 刷帧）；
 *  - UI(LVGL task) / WIFI(esp event task) / MCP 只 post 命令，不直接碰 LED 驱动，
 *    从根本上消除跨任务并发访问 led_strip/RMT 的竞争。
 *  - 主界面 set_status() 反映 Wi-Fi 状态；RGB 页 set_manual() 接管为手动。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATUS_DISCONNECTED = 0,
    LED_STATUS_CONNECTING,
    LED_STATUS_CONNECTED,
} led_status_t;

void led_service_init(void);

void led_service_set_status(led_status_t status);
void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void led_service_set_on(bool on);
void led_service_set_manual(bool manual);

#ifdef __cplusplus
}
#endif
