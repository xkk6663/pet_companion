/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_display.h
 * @brief L1 平台层：显示端口（**UI / 业务层碰显示的唯一入口**）
 *
 * 为什么要有它（分层结论，见 docs/项目规划/分层解耦评估与优化方案_2026-11-20.md）：
 *   UI 曾经 15 个文件直接 include `bsp/esp-bsp.h`，拿了 71 处 BSP_LCD_H_RES/V_RES
 *   和 bsp_display_lock —— 换一块屏要改 28 处，且 UI 与"厂商板级包"硬绑死。
 *   现在 UI 只认识本头文件里的 6 个函数 + board_map.h 里的 2 个宏：
 *     换屏  → 只改 board_map.h 的两个宏（几何）+ 本层实现（时序）
 *     换板  → 只改本层的 port_display.c
 *
 * 纪律：
 *   · 所有返回 LVGL 对象的接口（disp_start / disp_input_dev）只允许 LVGL 线程
 *     或已持 disp_lock 的调用方使用；
 *   · 任何 lv_* 调用前必须 disp_lock()（LVGL 非线程安全，工程铁律）；
 *   · 本层**不**提供任何业务语义（不画水印、不管页面栈），只有"屏"的物理能力。
 */
#pragma once

#include "platform/board_map.h"

#include "lvgl.h"
#include "esp_err.h"

/** 启动显示 + LVGL（内部 lv_init）。返回 LVGL display；重复调用返回同一句柄。
 *  必须早于任何 ui_* 调用；栈大小由 board_map.h 的 BOARD_LVGL_TASK_STACK 决定。 */
lv_display_t *disp_start(void);

/** 取 LVGL 互斥锁（永久等待）。返回 false = 还没初始化（bsp 尚未起 LVGL）。 */
bool disp_lock(void);

/** 带超时取锁（screen_shot 之类"宁愿失败也不卡死 httpd"的场景用）。 */
bool disp_lock_timeout(uint32_t timeout_ms);

/** 还锁。必须与 disp_lock/disp_lock_timeout 成对。 */
void disp_unlock(void);

/** 背光亮度 0~100（-1 = 不支持的板）。 */
esp_err_t disp_brightness_set(int percent);

/** 输入设备句柄（编码器/触摸），page_main 用它判断是否需要手柄焦点组。 */
lv_indev_t *disp_input_dev(void);

/**
 * 取面板两块 framebuffer（LVGL DIRECT 模式下它们就是 draw buffer，
 * 持 disp_lock 期间内容一致）。供 screen_shot 直读"在屏像素"。
 * @return ESP_OK / ESP_ERR_NOT_SUPPORTED（非 RGB 面板）
 */
esp_err_t disp_fb_get(void **fb0, void **fb1);
