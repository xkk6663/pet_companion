/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_record.c
 * @brief REC tab：本地录音一键录/停 + 双麦采集状态可视化。
 *
 * 布局（800×480 横屏）：
 *   · 左上标题 / 右上状态；
 *   · 右上 VAD pill；
 *   · 中间偏上 REC/STOP 按钮与时长；
 *   · **左下角 L 麦克风、右下角 R 麦克风各一组竖排频谱柱**；
 *   · 底部保存提示。
 *
 * 数据来自 mic_monitor 的受临界区保护的快照；本文件只在 LVGL timer 里读快照，
 * 不碰音频队列、不读 codec、不做 FFT。
 */

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_record.h"
#include "ui_theme.h"
#include "middleware/audio/mic_monitor.h"
#include "middleware/audio/mic_gate.h"
#include "middleware/audio/rec_local_service.h"

#define SPEC_MAX_H      134
#define SPEC_BAR_W      8
#define SPEC_BAR_GAP    2
#define SPEC_BAR_X0     18

static lv_obj_t   *s_btn;
static lv_obj_t   *s_btn_label;
static lv_obj_t   *s_status;
static lv_obj_t   *s_time_label;
static lv_obj_t   *s_hint;
static lv_obj_t   *s_vad_pill;
static lv_obj_t   *s_vad_label;
static lv_obj_t   *s_l_db;
static lv_obj_t   *s_r_db;
static lv_obj_t   *s_l_bars[MIC_MON_BANDS];
static lv_obj_t   *s_r_bars[MIC_MON_BANDS];
static lv_timer_t *s_timer;
static bool        s_active;
static bool        s_prev_active;
static uint8_t     s_l_h[MIC_MON_BANDS];
static uint8_t     s_r_h[MIC_MON_BANDS];
static uint32_t    s_ui_tick;

/* ============================================================
 * 交互
 * ============================================================ */

static void rec_btn_cb(lv_event_t *e)
{
    (void)e;
    if (rec_local_service_is_active()) {
        rec_local_stop();
        lv_label_set_text_static(s_status, "Saving...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        return;
    }
    esp_err_t err = rec_local_start();
    if (err != ESP_OK) {
        lv_label_set_text_static(s_status, "Start failed (busy?)");
        lv_obj_set_style_text_color(s_status, COL_RED, 0);
    }
    /* 成功态由 timer 轮询接管（pending → Recording...） */
}

/* ============================================================
 * 可视化
 * ============================================================ */

static void set_bars(lv_obj_t **bars, const uint8_t *band, uint8_t *last_h)
{
    for (int i = 0; i < MIC_MON_BANDS; i++) {
        uint8_t h = 4;
        if (band != NULL) {
            h = (uint8_t)(2 + ((uint32_t)band[i] * SPEC_MAX_H) / 100U);
        }
        if (h == last_h[i]) {
            continue;
        }
        last_h[i] = h;
        lv_obj_set_height(bars[i], h);
    }
}

static void set_vad_pill(const mic_monitor_snapshot_t *snap)
{
    const char *text;
    lv_color_t color;
    bool rec = rec_local_service_is_active();
    mic_gate_vad_state_t gate = mic_gate_vad_state();

    if (rec) {
        /* 录音态 gate 已 yield，没有 AFE VAD；显示本地能量代理 */
        if (snap != NULL && snap->valid) {
            text = snap->local_vad ? "VAD: LOCAL ON" : "VAD: LOCAL OFF";
            color = snap->local_vad ? COL_GREEN : COL_SUBTEXT;
        } else {
            text = "VAD: LOCAL --";
            color = COL_SUBTEXT;
        }
    } else if (mic_gate_is_owner()) {
        switch (gate) {
        case MIC_GATE_VAD_SPEECH:
            text = "VAD: SPEECH";
            color = COL_GREEN;
            break;
        case MIC_GATE_VAD_MERGE:
            text = "VAD: MERGE";
            color = COL_CYAN;
            break;
        case MIC_GATE_VAD_SILENCE:
            text = "VAD: SILENCE";
            color = COL_SUBTEXT;
            break;
        default:
            text = "VAD: N/A";
            color = COL_SUBTEXT;
            break;
        }
    } else {
        text = "VAD: N/A";
        color = COL_SUBTEXT;
    }

    if (strcmp(lv_label_get_text(s_vad_label), text) != 0) {
        lv_label_set_text_static(s_vad_label, text);
        lv_obj_set_style_bg_color(s_vad_pill, color, 0);
    }
}

static void update_monitor_ui(void)
{
    mic_monitor_snapshot_t snap = {0};
    bool ok = mic_monitor_read(&snap);

    if (ok) {
        set_bars(s_l_bars, snap.band_l, s_l_h);
        set_bars(s_r_bars, snap.band_r, s_r_h);
    } else {
        set_bars(s_l_bars, NULL, s_l_h);
        set_bars(s_r_bars, NULL, s_r_h);
    }

    set_vad_pill(ok ? &snap : NULL);

    /* dB 文本 2 Hz 更新，避免每 50ms 触发 label 重排 */
    if ((s_ui_tick % 10U) == 0U) {
        if (ok) {
            lv_label_set_text_fmt(s_l_db, "L %d dB", (int)snap.db_l);
            lv_label_set_text_fmt(s_r_db, "R %d dB", (int)snap.db_r);
        } else {
            lv_label_set_text_static(s_l_db, "L -- dB");
            lv_label_set_text_static(s_r_db, "R -- dB");
        }
    }
}

/* ============================================================
 * 轮询刷新（50ms）：唯一的状态行/按钮落笔点
 * ============================================================ */

static void rec_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_ui_tick++;
    bool act = rec_local_service_is_active();

    if (act != s_prev_active) {
        if (act) {
            /* pending / recording */
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_STOP "  STOP & SAVE");
            lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
            lv_label_set_text_static(s_status, "Recording...");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
        } else {
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  REC");
            lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
            lv_label_set_text_static(s_time_label, "00:00");
            if (s_prev_active) {
                /* 刚收尾：报结果 */
                if (rec_local_last_ok()) {
                    lv_label_set_text_fmt(s_status, "Saved: %s",
                                          rec_local_last_path());
                    lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
                } else {
                    lv_label_set_text_static(s_status, "Record FAILED");
                    lv_obj_set_style_text_color(s_status, COL_RED, 0);
                }
            }
        }
        s_prev_active = act;
    }

    if (act) {
        uint32_t ms = rec_local_elapsed_ms();
        lv_label_set_text_fmt(s_time_label, "%02u:%02u",
                              (unsigned)(ms / 60000U), (unsigned)((ms / 1000U) % 60U));
    }

    update_monitor_ui();
}

/* ============================================================
 * 建 / 拆
 * ============================================================ */

static lv_obj_t *make_spec_panel(lv_obj_t *parent, bool left, const char *title,
                                 lv_obj_t **bars, lv_obj_t **db_label)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 340, 166);
    lv_obj_align(panel, left ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT,
                 left ? 8 : -8, -28);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(panel);
    lv_label_set_text_static(name, title);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_set_style_text_color(name, left ? COL_CYAN : COL_RED, 0);
    lv_obj_set_style_text_font(name, UI_FONT, 0);

    *db_label = lv_label_create(panel);
    lv_label_set_text_static(*db_label, left ? "L -- dB" : "R -- dB");
    lv_obj_align(*db_label, LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_obj_set_style_text_color(*db_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(*db_label, UI_FONT, 0);

    for (int i = 0; i < MIC_MON_BANDS; i++) {
        lv_obj_t *bar = lv_obj_create(panel);
        lv_obj_set_size(bar, SPEC_BAR_W, 4);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, left ? COL_CYAN : COL_RED, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT,
                     SPEC_BAR_X0 + i * (SPEC_BAR_W + SPEC_BAR_GAP), 0);
        bars[i] = bar;
    }
    return panel;
}

void tab_record_build(lv_obj_t *screen, lv_group_t *group)
{
    (void)group;    /* 触摸方案：与 CAM/VIDEO/FILES 一致，不占 encoder 组 */

    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    /* Title（左上） */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO "  REC");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Status（右上） */
    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Idle");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* VAD pill（右上、状态下方） */
    s_vad_pill = lv_obj_create(screen);
    lv_obj_set_size(s_vad_pill, 166, 28);
    lv_obj_align(s_vad_pill, LV_ALIGN_TOP_RIGHT, 0, 32);
    lv_obj_set_style_radius(s_vad_pill, 14, 0);
    lv_obj_set_style_bg_opa(s_vad_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_vad_pill, COL_CARD, 0);
    lv_obj_set_style_border_width(s_vad_pill, 1, 0);
    lv_obj_set_style_border_color(s_vad_pill, COL_BORDER, 0);
    lv_obj_set_style_pad_all(s_vad_pill, 0, 0);
    lv_obj_clear_flag(s_vad_pill, LV_OBJ_FLAG_SCROLLABLE);

    s_vad_label = lv_label_create(s_vad_pill);
    lv_label_set_text_static(s_vad_label, "VAD: N/A");
    lv_obj_center(s_vad_label);
    lv_obj_set_style_text_color(s_vad_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_vad_label, UI_FONT, 0);

    /* 大按钮（上方居中）：待机 REC / 录音中 STOP & SAVE */
    s_btn = lv_btn_create(screen);
    lv_obj_set_size(s_btn, 240, 68);
    lv_obj_align(s_btn, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_radius(s_btn, 20, 0);
    lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_RED, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, rec_btn_cb, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  REC");
    lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0);
    lv_obj_center(s_btn_label);

    /* 时长（按钮下方） */
    s_time_label = lv_label_create(screen);
    lv_label_set_text_static(s_time_label, "00:00");
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_text_color(s_time_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, UI_FONT, 0);

    /* 左下 L / 右下 R：两组竖排频谱柱 */
    (void)make_spec_panel(screen, true, "L MIC", s_l_bars, &s_l_db);
    (void)make_spec_panel(screen, false, "R MIC", s_r_bars, &s_r_db);

    /* 提示（底部） */
    s_hint = lv_label_create(screen);
    lv_label_set_text_static(s_hint,
        "L bottom-left / R bottom-right  -  saved to /sdcard/music");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);

    /* 首拍只处理“正在录音”的恢复；空闲时保持 Idle，避免把上次结果
     * 或未录音的 FAILED 状态重新显示出来。 */
    s_prev_active = false;
    s_active = false;
    s_ui_tick = 0;
    memset(s_l_h, 0, sizeof(s_l_h));
    memset(s_r_h, 0, sizeof(s_r_h));
    mic_monitor_set_active(false);   /* 页面不可见时先关；由 set_active 打开 */
}

void tab_record_set_active(bool active)
{
    if (s_active == active) {
        return;
    }
    s_active = active;
    if (active) {
        mic_monitor_set_active(true);
        s_prev_active = false;   /* 首拍恢复录音态，空闲时不重报上次结果 */
        s_ui_tick = 0;
        if (s_timer == NULL) {
            s_timer = lv_timer_create(rec_timer_cb, 50, NULL);
        }
        rec_timer_cb(NULL);      /* 立刻出一帧，别等 50ms */
    } else {
        if (s_timer) {
            lv_timer_del(s_timer);
            s_timer = NULL;
        }
        mic_monitor_set_active(false);
    }
}

void tab_record_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_active = false;
    mic_monitor_set_active(false);

    s_btn = NULL;
    s_btn_label = NULL;
    s_status = NULL;
    s_time_label = NULL;
    s_hint = NULL;
    s_vad_pill = NULL;
    s_vad_label = NULL;
    s_l_db = NULL;
    s_r_db = NULL;
    for (int i = 0; i < MIC_MON_BANDS; i++) {
        s_l_bars[i] = NULL;
        s_r_bars[i] = NULL;
    }
    /* ★ 故意不 stop：录音是后台服务，切页/回主页不断录（状态在服务侧，
     *   重进 tab 由 timer 恢复显示）。要停请按 STOP & SAVE。 */
}
