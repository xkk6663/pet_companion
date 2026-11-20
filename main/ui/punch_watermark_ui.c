/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_watermark_ui.c
 * @brief UI 侧：打卡水印渲染实现（代码自 ai_punch.c 整体搬入）
 *
 * 视觉规则（与原实现完全一致，勿在业务侧复刻）：
 *   · 离屏 canvas 挂在 lv_layer_top 上、hidden、ignore_layout、挪到 (-1000,-1000)，
 *     只借用它的 draw buffer 当"立即模式画布"；
 *   · 底部 50% 不透明黑带 + 两行白字（姓名用 CJK bin 字体，回退 cn_20；时间用 UI_FONT）；
 *   · 行高按字体的 line_height 动态算（写死必然压字）。
 *
 * ★ 线程纪律：draw / release 只在**调用方已持 LVGL 锁**时被调用
 *   （业务侧 ai_punch 在自己的 disp_lock 临界区内请求渲染）。
 */

#include "ui/punch_watermark_ui.h"

#include "platform/port_display.h"
#include "middleware/camera/punch_watermark.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_theme.h"
#include "ui_binfont.h"

static const char *TAG = "WM_UI";

LV_FONT_DECLARE(lv_font_cn_20);   /* CJK 兜底字体（bin 字体未就绪时用，同 overlay_ai） */

/* 水印离屏画布（会话期临时；LVGL 对象，锁内使用） */
static lv_obj_t *s_wm_canvas;

static void wm_canvas_ensure(void)
{
    if (s_wm_canvas) {
        return;
    }
    s_wm_canvas = lv_canvas_create(lv_layer_top());
    lv_obj_set_hidden(s_wm_canvas, true);
    lv_obj_set_ignore_layout(s_wm_canvas, true);
    lv_obj_set_pos(s_wm_canvas, -1000, -1000);
}

static void wm_canvas_teardown(void)
{
    if (s_wm_canvas) {
        lv_obj_delete(s_wm_canvas);
        s_wm_canvas = NULL;
    }
}

/* 复刻 tab_camera::watermark_draw（S8-3）：离屏 canvas + 立即模式。 */
static void wm_draw(uint8_t *rgb, int w, int h, const char *name, const char *timestr)
{
    wm_canvas_ensure();
    if (!rgb || w <= 0 || h <= 0 || !s_wm_canvas) {
        ESP_LOGE(TAG, "watermark: bad frame or canvas missing (%p %dx%d)", rgb, w, h);
        return;
    }

    lv_canvas_set_buffer(s_wm_canvas, rgb, w, h, LV_COLOR_FORMAT_RGB888);

    lv_font_t *cjk = ui_binfont_get();
    const lv_font_t *nf = cjk ? cjk : &lv_font_cn_20;
    int lh_name = nf->line_height;
    int lh_time = UI_FONT->line_height;

    int band_h = lh_name + lh_time + 8;
    if (band_h > h / 2) {
        band_h = h / 2;   /* 极端字体下别把整张图糊掉 */
    }
    int band_y = h - band_h;

    lv_layer_t layer;
    lv_canvas_init_layer(s_wm_canvas, &layer);

    /* 半透明黑带：浅色背景上白字可读的前提 */
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_black();
    rdsc.bg_opa   = LV_OPA_50;
    rdsc.radius   = 0;
    lv_area_t band = { 0, band_y, w - 1, h - 1 };
    lv_draw_rect(&layer, &rdsc, &band);

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = lv_color_white();
    ldsc.opa   = LV_OPA_COVER;

    if (name && name[0]) {
        ldsc.font = nf;
        ldsc.text = name;
        lv_area_t a = { 6, band_y + 4, w - 7, band_y + 4 + lh_name - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }
    if (timestr && timestr[0]) {
        ldsc.font = UI_FONT;
        ldsc.text = timestr;
        lv_area_t a = { 6, band_y + 4 + lh_name, w - 7, band_y + 4 + lh_name + lh_time - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }

    lv_canvas_finish_layer(s_wm_canvas, &layer);
}

static bool wm_active(void)
{
    return s_wm_canvas != NULL;
}

static const punch_watermark_ops_t s_ops = {
    .draw    = wm_draw,
    .release = wm_canvas_teardown,
    .active  = wm_active,
};

void punch_watermark_ui_init(void)
{
    punch_watermark_install(&s_ops);
}
