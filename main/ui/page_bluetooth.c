/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_bluetooth.c
 * @brief SYS > Bluetooth 子页面：电源开关 + 搜索设备 + 设备列表 + 断开。
 *
 * 版式照抄主线 main/ui/pages/page_bluetooth.c，按本项目裁剪：
 *   - 去掉 A2DP Sink / 角色切换按钮（本项目固定 A2DP Source，§6）；
 *   - 去掉 AVRCP 音量（Source 方向不可用，§6-R6，音量走 SYS 滑块的 aud_alc 增益）；
 *   - 增加 DISCONNECT 按钮（主线靠关电源断开，体验上少了"只断连接"这一步）。
 *
 * 线程纪律（与 tab_music.c 同一套）：BT/GMF 回调只在 BT 线程上下文里置标志、
 * 缓存设备信息，**一律不碰 LVGL**；所有 lv_* 都在 200ms lv_timer 里做。
 * 蓝牙默认关，打开才 host init；关闭即 host deinit 释放内部 RAM。
 */

#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"

#include "page_bluetooth.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "middleware/bluetooth/bt_audio_service.h"

#define MAX_FOUND   BT_MAX_DEVICES      /* 与 service 侧缓存深度一致（8） */

static const char *TAG = "PAGE_BT";

/* ---- 控件 ---- */
static lv_obj_t *s_state_label;
static lv_obj_t *s_bt_sw;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_disc_btn;
static lv_obj_t *s_dev_list;
static lv_timer_t *s_poll_timer;

/* ---- BT 线程 → LVGL 的交接区（volatile 标志 + 缓存） ---- */
static volatile bool s_need_refresh;
static volatile bool s_state_changed;
static char s_state_txt[48];

static bt_device_t s_devs[MAX_FOUND];
static int s_dev_cnt;

/* 程序化改开关状态会再次触发 VALUE_CHANGED，用这个闩避免回调自噬 */
static bool s_syncing;

static void set_state(const char *txt)
{
    size_t n = strlen(txt);
    if (n >= sizeof(s_state_txt)) n = sizeof(s_state_txt) - 1;
    memcpy(s_state_txt, txt, n);
    s_state_txt[n] = '\0';
    s_state_changed = true;
}

/* 卡片行：label + 右侧控件占位（与 page_rgb 的开关行同一套版式）
 *  ★ 先声明：refresh_dev_list() 在 poll/事件路径里先用到它。 */
static lv_obj_t *make_card_row(lv_obj_t *parent, const char *txt, int32_t h);
static void dev_click_cb(lv_event_t *e);

static void bt_evt_cb(bt_evt_t evt, const void *data, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case BT_EVT_ON:
        set_state("Bluetooth ON");
        break;
    case BT_EVT_OFF:
        /* host 刚 deinit，设备缓存已无意义 */
        s_dev_cnt = 0;
        s_need_refresh = true;
        set_state("Bluetooth OFF");
        break;
    case BT_EVT_DISCOVERY_START:
        set_state("Scanning...");
        break;
    case BT_EVT_DISCOVERY_STOP:
        break;                          /* 状态栏保留上一次结论（已连接/已断开） */
    case BT_EVT_DEVICE_FOUND: {
        if (data && s_dev_cnt < MAX_FOUND) {
            s_devs[s_dev_cnt++] = *(const bt_device_t *)data;
            s_need_refresh = true;
        }
        break;
    }
    case BT_EVT_CONNECTED:
        set_state("Connected");
        break;
    case BT_EVT_DISCONNECTED:
        set_state("Disconnected");
        break;
    case BT_EVT_STREAM_ON:
        set_state("Streaming");
        break;
    case BT_EVT_STREAM_OFF:
        /* 推流自然结束 ≠ 断链，回落到 Connected（主线 2026-09-22 修过这个卡字 */
        set_state("Connected");
        break;
    default:
        break;
    }
}

/* ---- LVGL 线程 ---- */
static void refresh_dev_list(void)
{
    lv_obj_clean(s_dev_list);
    for (int i = 0; i < s_dev_cnt; i++) {
        const char *nm = s_devs[i].name[0] ? s_devs[i].name : "(unknown)";
        lv_obj_t *row = make_card_row(s_dev_list, "", 40);
        lv_obj_set_width(row, BSP_LCD_H_RES - 60);
        lv_obj_set_height(row, 40);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, dev_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lab = lv_obj_get_child(row, 0);
        if (lab) {
            lv_label_set_text_fmt(lab, "%s  (%ld dBm)", nm, (long)s_devs[i].rssi);
        }
    }
    if (s_dev_cnt == 0) {
        lv_obj_t *empty = lv_label_create(s_dev_list);
        lv_label_set_text_static(empty, "No device yet - tap SEARCH");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
        lv_obj_set_style_text_font(empty, UI_FONT, 0);
    }
}

static void dev_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_dev_cnt) return;
    bt_audio_service_stop_discovery();
    /* 节流/已在连：明确提示，别让界面停在 Connecting... 像卡死；
       这段时间反复点会打断协议栈自己的重连（约 4 s 后才会成功）。 */
    if (bt_audio_service_connect(s_devs[idx].addr) == ESP_ERR_INVALID_STATE &&
        !bt_audio_service_is_connected()) {
        set_state("Waiting, don't tap");
        return;
    }
    set_state("Connecting...");
}

static void bt_sw_cb(lv_event_t *e)
{
    (void)e;
    if (s_syncing) return;

    bool on = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
    esp_err_t ret = bt_audio_service_set_power(on);

    if (on && ret != ESP_OK) {
        /* 打开失败（多半是内部 RAM 不足、闸门主动拒绝）：把开关弹回 OFF，
           否则 UI 显示"已开"而实际没起来，用户会一直等扫描结果。 */
        ESP_LOGW(TAG, "BT power on failed: %s", esp_err_to_name(ret));
        s_syncing = true;
        lv_obj_remove_state(s_bt_sw, LV_STATE_CHECKED);
        s_syncing = false;
        set_state(ret == ESP_ERR_NO_MEM ? "Not enough RAM" : "Power on failed");
        return;
    }

    if (!on) {
        s_dev_cnt = 0;
        s_need_refresh = true;
    }
    set_state(on ? "Powering on..." : "Powering off...");
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!bt_audio_service_is_on()) {
        set_state("Turn Bluetooth on first");
        return;
    }
    s_dev_cnt = 0;
    s_need_refresh = true;
    bt_audio_service_start_discovery();
}

static void disc_btn_cb(lv_event_t *e)
{
    (void)e;
    set_state("Disconnecting...");
    bt_audio_service_disconnect();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    bool on   = bt_audio_service_is_on();
    bool conn = bt_audio_service_is_connected();

    /* 开关回填（电源可能被别处（MUSIC tab）改动） */
    bool sw_on = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
    if (sw_on != on) {
        s_syncing = true;
        if (on) lv_obj_add_state(s_bt_sw, LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_bt_sw, LV_STATE_CHECKED);
        s_syncing = false;
    }

    /* 关蓝牙时只留开关；扫描/列表/断开全部隐藏，避免点了没反应 */
    if (on) {
        lv_obj_remove_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
    }
    if (on && conn) lv_obj_remove_flag(s_disc_btn, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_disc_btn, LV_OBJ_FLAG_HIDDEN);

    if (s_need_refresh) {
        s_need_refresh = false;
        refresh_dev_list();
    }
    if (s_state_changed) {
        s_state_changed = false;
        lv_label_set_text(s_state_label, s_state_txt);
    }
}

/* 卡片行：label + 右侧控件占位（与 page_rgb 的开关行同一套版式） */
static lv_obj_t *make_card_row(lv_obj_t *parent, const char *txt, int32_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, h);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text_static(lab, txt);
    lv_obj_set_style_text_color(lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);
    return row;
}

static lv_obj_t *bt_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 12, 0);
    lv_obj_set_style_pad_row(page, 8, 0);

    ui_common_title_bar(page, LV_SYMBOL_BLUETOOTH"  Bluetooth");

    /* 竖直预算（480）：12 + 标题 48 + 6×8 + 状态 18 + 电源 56 + 扫描 48
       + 设备列表 190 + 断开 44 = 464 ⇒ 余 16 px。加控件前先算这一笔。 */

    /* 状态 */
    s_state_label = lv_label_create(page);
    lv_label_set_text_static(s_state_label,
                             bt_audio_service_is_on() ? "Bluetooth ON" : "Bluetooth OFF");
    lv_obj_set_style_text_color(s_state_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_state_label, UI_FONT, 0);

    /* 电源卡 */
    lv_obj_t *pw_row = make_card_row(page, LV_SYMBOL_POWER"  BLUETOOTH POWER", 56);
    s_bt_sw = lv_switch_create(pw_row);
    lv_obj_set_style_bg_color(s_bt_sw, COL_BORDER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bt_sw, COL_CYAN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_bt_sw, bt_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 搜索 */
    s_scan_btn = lv_button_create(page);
    lv_obj_set_size(s_scan_btn, BSP_LCD_H_RES - 40, 48);
    lv_obj_set_style_bg_color(s_scan_btn, COL_CYAN, 0);
    lv_obj_set_style_radius(s_scan_btn, 12, 0);
    lv_obj_t *scan_lab = lv_label_create(s_scan_btn);
    lv_label_set_text_static(scan_lab, LV_SYMBOL_BLUETOOTH"  SEARCH DEVICE");
    lv_obj_set_style_text_color(scan_lab, COL_DARK, 0);
    lv_obj_set_style_text_font(scan_lab, UI_FONT, 0);
    lv_obj_center(scan_lab);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 设备列表（BT 回调写入的 s_devs 在这里镜像） */
    s_dev_list = lv_obj_create(page);
    lv_obj_set_size(s_dev_list, BSP_LCD_H_RES - 40, 190);
    lv_obj_set_style_bg_color(s_dev_list, COL_BG, 0);
    lv_obj_set_style_border_width(s_dev_list, 0, 0);
    lv_obj_set_style_pad_all(s_dev_list, 0, 0);
    lv_obj_set_style_pad_row(s_dev_list, 4, 0);
    lv_obj_set_flex_flow(s_dev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_dev_list, LV_DIR_VER);

    /* 断开 */
    s_disc_btn = lv_button_create(page);
    lv_obj_set_size(s_disc_btn, BSP_LCD_H_RES - 40, 44);
    lv_obj_set_style_bg_color(s_disc_btn, COL_CARD, 0);
    lv_obj_set_style_border_color(s_disc_btn, COL_RED, 0);
    lv_obj_set_style_border_width(s_disc_btn, 1, 0);
    lv_obj_set_style_radius(s_disc_btn, 12, 0);
    lv_obj_t *disc_lab = lv_label_create(s_disc_btn);
    lv_label_set_text_static(disc_lab, "DISCONNECT");
    lv_obj_set_style_text_color(disc_lab, COL_RED, 0);
    lv_obj_set_style_text_font(disc_lab, UI_FONT, 0);
    lv_obj_center(disc_lab);
    lv_obj_add_event_cb(s_disc_btn, disc_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 进页面时把 service 里已有的发现结果搬过来（上一轮扫描过的设备） */
    s_dev_cnt = 0;
    int n = bt_audio_service_get_device_count();
    for (int i = 0; i < n && i < MAX_FOUND; i++) {
        const bt_device_t *d = bt_audio_service_get_device(i);
        if (d) s_devs[s_dev_cnt++] = *d;
    }
    s_need_refresh = true;

    /* 注册事件回调（init 中间层，不打开射频；电源仍由开关触发） */
    bt_audio_service_init(bt_evt_cb, NULL);

    s_poll_timer = lv_timer_create(poll_cb, 200, NULL);
    return page;
}

static void bt_destroy(lv_obj_t *page)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    /* 解绑页面回调，避免离开页面后事件还写页面状态（数据本身是静态区，安全，
       但留着会让下次进入时状态文本错乱）。同时重新挂上推流收尾回调。 */
    bt_audio_service_init(NULL, NULL);

    lv_obj_del(page);
    s_state_label = NULL;
    s_bt_sw = NULL;
    s_scan_btn = NULL;
    s_disc_btn = NULL;
    s_dev_list = NULL;
    s_dev_cnt = 0;
}

const Page_t Page_Bluetooth = {
    .create = bt_create,
    .destroy = bt_destroy,
    .name = "Bluetooth",
};
