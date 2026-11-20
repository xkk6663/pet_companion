/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_watermark_ui.h
 * @brief UI 侧：打卡水印渲染实现（离屏 canvas + 主题字体）
 *
 * 这是从 middleware/ai/ai_punch.c 里**整体搬出**的那段代码（原 wm_canvas_ensure /
 * wm_draw / wm_canvas_teardown），逻辑逐字保留；变的只是归属：
 *   LVGL 对象、字体、"半透明黑带 + 两行白字"的视觉，全都回到 ui/。
 *   业务侧只经 middleware/camera/punch_watermark.h 的槽请求渲染。
 */
#pragma once

/** 启动时注册渲染实现（在 disp_start() 之后、LVGL 线程可跑之后调用一次）。 */
void punch_watermark_ui_init(void);
