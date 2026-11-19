/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_shot.c
 * @brief RGB 面板 framebuffer 直读实现（设计与约束见 screen_shot.h）
 */

#include "screen_shot.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_rgb.h"
#include "lvgl.h"

#define TAG "SCREEN_SHOT"

/* 直读 framebuffer 时，如何确保拿到的是"正在显示"的那一块？
 * 在 flush 回调（整屏、last）里记下 px_map —— 此刻还没换 buffer，
 * 它就是要扫描输出的那一块。之后持 LVGL 锁再拷，LVGL 不会写它。 */
static lv_display_flush_cb_t s_port_flush_cb;
static void                 *s_on_screen_fb;

static void screen_shot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (lv_display_flush_is_last(disp)) {
        s_on_screen_fb = px_map;
    }
    if (s_port_flush_cb) {
        s_port_flush_cb(disp, area, px_map);
    }
}

/** 调用方必须持有 LVGL 锁 */
static void screen_shot_ensure_hook(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL || lv_display_get_flush_cb(disp) == screen_shot_flush_cb) {
        return;
    }
    s_port_flush_cb = lv_display_get_flush_cb(disp);
    if (s_port_flush_cb != NULL) {
        lv_display_set_flush_cb(disp, screen_shot_flush_cb);
        ESP_LOGI(TAG, "flush hook installed");
    }
}

esp_err_t screen_shot_take(uint8_t **rgb565, int *w, int *h, int scale)
{
    if (rgb565 == NULL || w == NULL || h == NULL ||
        (scale != 1 && scale != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    *rgb565 = NULL;

    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    if (panel == NULL) {
        ESP_LOGE(TAG, "no RGB panel handle");
        return ESP_ERR_INVALID_STATE;
    }

    /* LVGL direct 模式把这两块就是面板 framebuffer；两块内容经脏区同步保持一致，
     * 持锁期间不会有一帧只刷新了一半。 */
    void *fb[2] = { NULL, NULL };
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fb[0], &fb[1]);
    if (err != ESP_OK || fb[0] == NULL) {
        ESP_LOGE(TAG, "get_frame_buffer failed: %s", esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    const int dw = SCREEN_SHOT_W / scale;
    const int dh = SCREEN_SHOT_H / scale;
    const size_t bytes = (size_t)dw * (size_t)dh * sizeof(uint16_t);

    /* 64 字节对齐：后续若直接交给 esp_jpeg/其它硬件模块也满足其 16 字节要求 */
    uint8_t *dst = heap_caps_aligned_alloc(64, bytes,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == NULL) {
        ESP_LOGE(TAG, "snapshot alloc %u B failed (PSRAM tight?)", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }

    /* 只在这一小段持锁：先把指针处的完整一帧拷走，再去发网络 */
    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "lvgl lock timeout");
        heap_caps_free(dst);
        return ESP_ERR_TIMEOUT;
    }

    screen_shot_ensure_hook();
    /* 优先用 flush 时记下的"在屏"缓冲；首次抓图/软件旋转等异常时退回 fb[0] */
    const void *on_screen = s_on_screen_fb;
    if (on_screen != fb[0] && on_screen != fb[1]) {
        on_screen = fb[0];
    }
    const uint16_t *src = (const uint16_t *)on_screen;
    if (scale == 1) {
        memcpy(dst, src, bytes);
    } else {
        uint16_t *d = (uint16_t *)dst;
        for (int y = 0; y < dh; y++) {
            const uint16_t *row = src + (size_t)y * scale * SCREEN_SHOT_W;
            for (int x = 0; x < dw; x++) {
                d[(size_t)y * dw + x] = row[x * scale];
            }
        }
    }
    bsp_display_unlock();

    *rgb565 = dst;
    *w = dw;
    *h = dh;
    return ESP_OK;
}

void screen_shot_free(void *buf)
{
    if (buf != NULL) {
        heap_caps_free(buf);
    }
}

size_t screen_shot_bmp_stride(int w)
{
    return ((size_t)w * 3 + 3) & ~(size_t)3;
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void screen_shot_bmp_header(uint8_t *hdr, int w, int h)
{
    const uint32_t stride = (uint32_t)screen_shot_bmp_stride(w);
    const uint32_t image = stride * (uint32_t)h;

    memset(hdr, 0, SCREEN_SHOT_BMP_HDR);
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_u32(hdr + 2, SCREEN_SHOT_BMP_HDR + image);   /* bfSize */
    put_u32(hdr + 10, SCREEN_SHOT_BMP_HDR);          /* bfOffBits */
    put_u32(hdr + 14, 40);                           /* biSize */
    put_u32(hdr + 18, (uint32_t)w);                  /* biWidth */
    put_u32(hdr + 22, (uint32_t)(-h));               /* 负高度 = top-down */
    put_u16(hdr + 26, 1);                            /* biPlanes */
    put_u16(hdr + 28, 24);                           /* biBitCount */
    put_u32(hdr + 34, image);                        /* biSizeImage */
    put_u32(hdr + 38, 2835);                         /* 72 DPI */
    put_u32(hdr + 42, 2835);
}

void screen_shot_bmp_row(const uint8_t *rgb565, int w, int y, uint8_t *dst_bgr)
{
    const uint16_t *src = (const uint16_t *)rgb565 + (size_t)y * (size_t)w;
    for (int x = 0; x < w; x++) {
        const uint16_t v = src[x];
        const uint8_t r = (uint8_t)((v >> 11) & 0x1F);
        const uint8_t g = (uint8_t)((v >> 5) & 0x3F);
        const uint8_t b = (uint8_t)(v & 0x1F);
        /* 5/6 bit 扩展到 8 bit（复制高位，保证 0x1F -> 0xFF） */
        dst_bgr[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
        dst_bgr[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        dst_bgr[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
    }
}
