/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_sys_entry.c
 * @brief SYS tab: volume slider + direct entry rows to the four resource
 *        sub-pages (WIFI / RGB / System).
 */

#include <stdint.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_sys_entry.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "page_manager.h"
#include "page_wifi.h"
#include "page_rgb.h"
#include "page_system.h"
#include "page_face.h"                  /* ★ S8-1：人脸库管理页 */
#include "page_bluetooth.h"            /* ★ A2DP：SYS > Bluetooth 子页面 */
#include "middleware/audio/audio_service.h"
#include "middleware/periph/button_service.h"
#include "middleware/bluetooth/bt_audio_service.h"   /* ★ A2DP：推流数字音量 */

static lv_obj_t  *s_vol_slider;
static lv_timer_t *s_vol_timer;

/* sub-page row ids */
enum {
    ROW_WIFI = 0,
    ROW_BLUETOOTH,                      /* ★ A2DP */
    ROW_RGB,
    ROW_SYSTEM,
    ROW_FACE,                           /* ★ S8-1 */
};

/* 音量统一出口（滑块 0..90 = 本工程量程）：
 *   ① 本地 codec：始终同步（无推流时是唯一出口；推流期间保持状态一致，不刺耳）；
 *   ② BT 推流：同一滑块值映射为 aud_alc 数字增益（0..90 → 0..100）。
 *      非推流状态下 bt_audio_service_set_volume 返回 INVALID_STATE，忽略即可
 *      （AVRCP 音量在 A2DP Source 方向不可用，§6-R6）。 */
static void apply_volume(int vol)
{
    button_service_set_volume(vol);
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = vol,
    };
    audio_service_post_cmd(&cmd);
    (void)bt_audio_service_set_volume(vol);
}

static void volume_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    apply_volume(lv_slider_get_value(slider));
}

static void vol_timer_cb(lv_timer_t *t)
{
    (void)t;
    QueueHandle_t q = button_service_get_vol_queue();
    btn_vol_msg_t msg;
    while (q && xQueueReceive(q, &msg, 0) == pdPASS) {
        if (s_vol_slider) {
            lv_slider_set_value(s_vol_slider, msg.volume, true);
        }
        apply_volume(msg.volume);
    }
}

static void row_click_cb(lv_event_t *e)
{
    uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
    switch (id) {
    case ROW_WIFI:
        PageManager_Load(&Page_Wifi);
        break;
    case ROW_BLUETOOTH:
        PageManager_Load(&Page_Bluetooth);  /* ★ A2DP：电源/搜索/连接/断开 */
        break;
    case ROW_RGB:
        PageManager_Load(&Page_Rgb);
        break;
    case ROW_SYSTEM:
        PageManager_Load(&Page_System);
        break;
    case ROW_FACE:
        PageManager_Load(&Page_Face);       /* ★ S8-1 */
        break;
    default:
        break;
    }
}

void tab_sys_entry_build(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_style_pad_top(screen, 10, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_SETTINGS"  SYSTEM");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Volume card
     * ★ S8-1：高度 80 → 68。本页是**不可滚动**的全屏页（下面 lv_obj_set_scrollable
     *   (screen,false)），内容超高不是"能滚一下"，而是**最后一行被直接裁掉**——
     *   正是那种"按钮在、回调也挂了，但用户看不到也点不到"的隐性故障。
     * ★ A2DP：加第 5 行（Bluetooth）后重排竖直预算（tab 内容区 800x432）：
     *   pad_top 10 + 标题 ~34 + 6×pad_row 8(48) + 本卡 52 + 5 行 × 56(280)
     *   = 424 ⇒ 余 8 px。
     *   ⚠ 再加第 6 行必须先减 56+8：可行的手段只有缩本卡（52→40 以下不可读）
     *     或把 pad_row 压到 4，别只顾着加 ui_common_setting_row。 */
    lv_obj_t *vol = lv_obj_create(screen);
    lv_obj_set_size(vol, BSP_LCD_H_RES - 40, 52);
    lv_obj_set_style_bg_color(vol, COL_CARD, 0);
    lv_obj_set_style_border_color(vol, COL_BORDER, 0);
    lv_obj_set_style_border_width(vol, 1, 0);
    lv_obj_set_style_radius(vol, 16, 0);
    lv_obj_set_style_pad_left(vol, 16, 0);
    lv_obj_set_style_pad_right(vol, 16, 0);
    lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *l = lv_label_create(vol);
    lv_label_set_text_static(l, LV_SYMBOL_VOLUME_MAX"  VOL");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    s_vol_slider = lv_slider_create(vol);
    lv_obj_set_width(s_vol_slider, BSP_LCD_H_RES - 220);
    lv_slider_set_range(s_vol_slider, 0, 90);
    lv_slider_set_value(s_vol_slider, button_service_get_volume(), false);
    lv_obj_set_style_bg_color(s_vol_slider, COL_BORDER, 0);
    lv_obj_set_style_bg_color(s_vol_slider, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_vol_slider, volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (group) {
        lv_group_add_obj(group, s_vol_slider);
    }

    /* Resource sub-page entries（★ S8-1：加 FACE 一行，见上面 vol 卡的竖直预算）
     * ★ A2DP：Bluetooth 紧跟 WIFI（与主线 SYS 页同序） */
    ui_common_setting_row(screen, LV_SYMBOL_WIFI, "WIFI", row_click_cb, (void *)(uintptr_t)ROW_WIFI);
    ui_common_setting_row(screen, LV_SYMBOL_BLUETOOTH, "Bluetooth", row_click_cb, (void *)(uintptr_t)ROW_BLUETOOTH);
    ui_common_setting_row(screen, LV_SYMBOL_IMAGE, "RGB LED", row_click_cb, (void *)(uintptr_t)ROW_RGB);
    ui_common_setting_row(screen, LV_SYMBOL_LIST, "Face DB", row_click_cb, (void *)(uintptr_t)ROW_FACE);
    ui_common_setting_row(screen, LV_SYMBOL_SETTINGS, "System", row_click_cb, (void *)(uintptr_t)ROW_SYSTEM);

    if (s_vol_timer == NULL) {
        s_vol_timer = lv_timer_create(vol_timer_cb, 100, NULL);
    }
}

void tab_sys_entry_destroy(void)
{
    if (s_vol_timer) {
        lv_timer_del(s_vol_timer);
        s_vol_timer = NULL;
    }
    s_vol_slider = NULL;
}
