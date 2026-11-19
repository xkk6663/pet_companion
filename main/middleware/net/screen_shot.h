/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_shot.h
 * @brief 抓取设备端 LVGL **真实渲染界面**（直接读 RGB 面板 framebuffer）
 *
 * ===========================================================================
 *  为什么能直接读 framebuffer
 * ---------------------------------------------------------------------------
 *  本工程屏幕是 RGB565 并口屏，LVGL 工作在 `DIRECT` 双缓冲 + `AVOID_TEAR`
 *  （见 sdkconfig 的 CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR）：
 *    · esp_lvgl_port 把面板的两块 framebuffer (`esp_lcd_rgb_panel_get_frame_buffer`)
 *      **直接当作 LVGL 的 draw buffer**；
 *    · 每帧最后一次 flush 是整屏 800x480 的 `draw_bitmap`，并等待 VSYNC。
 *  ⇒ 面板 framebuffer 里的内容就是屏幕正在扫描输出的像素本身。
 *
 *  ★ 取图只在持有 LVGL 锁时做一次 memcpy，网络发送必须在解锁之后
 *    （esp_http_server 是单任务，不能在锁内做慢 IO）。
 *  ★ 相机/视频预览走的是 lv_canvas 零拷贝，也在 LVGL 树上，
 *    所以抓出来的画面是完整的，不丢视频层。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 屏幕原始分辨率（与 BSP_LCD_H_RES / BSP_LCD_V_RES 一致） */
#define SCREEN_SHOT_W          800
#define SCREEN_SHOT_H          480

/** BMP 文件头长度（BITMAPFILEHEADER + BITMAPINFOHEADER） */
#define SCREEN_SHOT_BMP_HDR    54

/** 800x480 RGB565 的 JPEG 输出上限（q80 典型 60~150 KB，留足余量） */
#define SCREEN_SHOT_JPEG_MAX   (256 * 1024)

/**
 * @brief 抓一帧屏幕内容，输出 RGB565（小端、无字节交换）。
 *
 * @param[out] rgb565  输出缓冲（PSRAM，64 字节对齐），调用方用 screen_shot_free 释放
 * @param[out] w,h     实际尺寸（scale=2 时是 400x240）
 * @param[in]  scale   1 = 原始 800x480；2 = 2x2 最近邻抽样 400x240
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_ERR_TIMEOUT /
 *         ESP_ERR_NO_MEM
 *
 * @note 会短暂持有 LVGL 锁（仅 memcpy），不得在 LVGL 线程或 flush 回调里调用。
 */
esp_err_t screen_shot_take(uint8_t **rgb565, int *w, int *h, int scale);

/** 释放 screen_shot_take 返回的缓冲（NULL 安全） */
void screen_shot_free(void *buf);

/** BMP 行跨距（4 字节对齐；800/400 宽时恰好等于 w*3） */
size_t screen_shot_bmp_stride(int w);

/**
 * @brief 生成 24bpp、top-down（biHeight 为负）的 BMP 头。
 * @param[out] hdr 至少 SCREEN_SHOT_BMP_HDR 字节
 */
void screen_shot_bmp_header(uint8_t *hdr, int w, int h);

/** 把 RGB565 的第 y 行转换成 BMP 需要的 BGR888（每像素 3 字节） */
void screen_shot_bmp_row(const uint8_t *rgb565, int w, int y, uint8_t *dst_bgr);

#ifdef __cplusplus
}
#endif
