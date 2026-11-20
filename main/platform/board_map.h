/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file board_map.h
 * @brief L1 板级事实表（**换板唯一入口**，只放纯宏 + 纯数字，零依赖）
 *
 * 定位：本层是"板上有什么、在哪、多大"的唯一事实来源。
 *   · 只允许纯宏/枚举，**不许** include 任何驱动或 SDK 头 —— 这样它能被
 *     app / ui / services / drivers 任意层安全引用而不构成层间耦合；
 *   · 换屏 = 改 BOARD_LCD_H_RES / BOARD_LCD_V_RES 两个宏（编译期常量，
 *     fs_service 用它算整帧缓冲尺寸，必须保持宏而**不能**改成函数）；
 *   · 换板 = 改本文件 + platform/port_display.c 里的驱动实现，其它层不动。
 *
 * ★ 与原 BSP 宏的对应（值必须一致，改一处漏一处会当场花屏/黑屏）：
 *   BOARD_LCD_H_RES/V_RES  == BSP_LCD_H_RES / BSP_LCD_V_RES   (800x480)
 *   BOARD_LCD_DISP_GPIO    == BSP_LCD_RGB_DISP                (GPIO38)
 *   BOARD_LED_RGB_GPIO     == BSP_LED_RGB_IO                  (GPIO37)
 */
#pragma once

/* ---- 显示：GC9xxx/ST7701 类 800x480 RGB565 面板 ---- */
#define BOARD_LCD_H_RES          800
#define BOARD_LCD_V_RES          480
/** 屏 standby 控制脚（BSP_LCD_RGB_DISP）：低 = standby，高 = 正常显示。
 *  safe_reboot 软重启前必须拉低它，否则重启瞬间残影/撕裂（见 safe_reboot.c 注释）。 */
#define BOARD_LCD_DISP_GPIO      38

/* ---- LVGL 任务栈（P2-4 按实测水位收敛到 9728，判据见 app/main.c 的栈表一节）---- */
#define BOARD_LVGL_TASK_STACK    9728

/* ---- 灯：单线 WS2812 类 RGB 灯珠 ---- */
#define BOARD_LED_RGB_GPIO       37

/* ---- 音频：本板有扬声器 + 麦克风（对应 BSP_CAPS_AUDIO / BSP_CAPS_AUDIO_MIC）---- */
#define BOARD_HAS_AUDIO_SPEAKER  1
#define BOARD_HAS_AUDIO_MIC      1
