/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_music.c
 * @brief 本地音乐播放 tab：歌词 / 进度 / 媒体键 / 蓝牙 A2DP 输出。
 *
 * 版式参照 display_audio_photo 主线 tab_pcnow（右手侧竖排三键、歌词三行），
 * 本工程屏幕为 800x480 横屏，tab 内容区 800x432，故坐标按横屏重排
 * （左侧曲名/三行歌词 + 右三键）。封面区按需求整体去掉（本地播放没有封面图源）。
 *
 * 纪律：
 *  - 离开本页必停（doc §10）：tab_music_set_active(false) → stop + lrc_unload；
 *  - 蓝牙事件只由 bt_audio_service 回调置标志，UI 侧 200ms lv_timer 抽取；
 *  - 歌词取行用 lrc_service_find_index()，无同名 lrc 时中间行显示"无"。
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ui_theme.h"
#include "ui_binfont.h"
#include "tab_music.h"

#include "middleware/periph/sdcard_service.h"
#include "middleware/audio/media_player_service.h"
#include "middleware/audio/lrc_service.h"
#include "middleware/bluetooth/bt_audio_service.h"

static const char *TAG = "TAB_MUSIC";

#define MUSIC_DIR        SDCARD_MNT_PATH "/music"
#define MAX_TRACKS       64
#define TRACK_PATH_LEN   300
#define BTN_SIZE         80

/* ---- 控件 ---- */
static lv_obj_t *s_out_lbl;
static lv_obj_t *s_bt_sw;
static lv_obj_t *s_bt_lbl;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_dev_list;
static lv_obj_t *s_title;
static lv_obj_t *s_info;
static lv_obj_t *s_lyr[3];
static lv_obj_t *s_bar;
static lv_obj_t *s_time;
static lv_obj_t *s_play_sym;
static lv_timer_t *s_timer;

/* ---- 状态 ---- */
static bool     s_active;
static bool     s_seeking;
static bool     s_scanning;
static int      s_seen_idx = -1000;      /* 上次歌词行（避免每拍重设文本） */
static int      s_seen_dev = -1;         /* 上次设备数 */
static int s_bt_state = -1;   /* 0=off 1=ready 2=connected；-1 强制首刷 */
/* 程序回填开关时抑制 VALUE_CHANGED 回调：LVGL 里 lv_obj_add/clear_state(CHECKED)
 * 也会发 VALUE_CHANGED，不挡就会把"回填"当成用户点击再去开/关一次电源。
 * SYS 子页（page_bluetooth.c）一直有这个 s_syncing，本页之前漏了，
 * 所以两处开关状态不同步（MUSIC 侧只在自己被点过时才变）。 */
static bool s_syncing;

/* ---- 歌单 ---- */
static char     s_tracks[MAX_TRACKS][TRACK_PATH_LEN];
static int      s_track_count;
static int      s_track_idx = -1;
static char     s_cur_path[TRACK_PATH_LEN];

static const lv_font_t *cn_font(void)
{
    lv_font_t *f = ui_binfont_get();
    return f ? f : UI_FONT;   /* bin 字体未就绪时回退（中文会缺字，但不崩） */
}

/* ============================================================
 * 歌单
 * ============================================================ */

static int scan_tracks(void)
{
    s_track_count = 0;
    DIR *d = opendir(MUSIC_DIR);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed", MUSIC_DIR);
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_track_count < MAX_TRACKS) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcasecmp(n + len - 4, ".mp3") != 0) {
            continue;
        }
        snprintf(s_tracks[s_track_count], TRACK_PATH_LEN, "%s/%s", MUSIC_DIR, n);
        s_track_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "scan tracks: %d", s_track_count);
    return s_track_count;
}

static int find_track_index(const char *path)
{
    for (int i = 0; i < s_track_count; i++) {
        if (strcmp(s_tracks[i], path) == 0) {
            return i;
        }
    }
    return -1;
}

static void update_title(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", base);
    char *dot = strrchr(tmp, '.');
    if (dot) {
        *dot = '\0';
    }
    lv_label_set_text(s_title, tmp);
}

/* ============================================================
 * 播放控制
 * ============================================================ */

static void start_track(const char *path)
{
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", path);
    s_track_idx = find_track_index(path);
    update_title(path);
    lrc_service_load(path);      /* 无同名 lrc → has_lyrics=false（UI 显示"无"） */
    s_seen_idx = -1000;
    media_player_service_play(path);
}

static void play_relative(int dir)
{
    if (s_track_count <= 0 && scan_tracks() <= 0) {
        lv_label_set_text_static(s_info, "No MP3 in /sdcard/music");
        lv_obj_set_style_text_color(s_info, COL_RED, 0);
        return;
    }
    int idx = s_track_idx;
    if (idx < 0) {
        idx = (dir >= 0) ? 0 : s_track_count - 1;
    } else {
        idx = (idx + dir + s_track_count) % s_track_count;
    }
    start_track(s_tracks[idx]);
}

static void toggle_play(void)
{
    if (!media_player_service_is_playing()) {
        if (s_cur_path[0]) {
            media_player_service_play(s_cur_path);
        } else {
            play_relative(1);
        }
        return;
    }
    if (media_player_service_is_paused()) {
        media_player_service_resume();
    } else {
        media_player_service_pause();
    }
}

/* ============================================================
 * 事件
 * ============================================================ */

static void media_evt_cb(mp_evt_id_t evt)
{
    /* 播放状态一律由 tick_cb 直接读 service（200ms），这里不需要额外标志；
     * 保留回调是为了将来按事件精细化刷新时留钩子。 */
    (void)evt;
}

static void out_btn_cb(lv_event_t *e)
{
    (void)e;
    mp_out_mode_t mode = media_player_service_get_output_mode();
    mode = (mp_out_mode_t)(((int)mode + 1) % 3);
    media_player_service_set_output_mode(mode);   /* 播中途切换 = 当前曲目热切换重播 */
    const char *txt = (mode == MP_OUT_AUTO) ? "OUT: AUTO"
                    : (mode == MP_OUT_LOCAL) ? "OUT: LOCAL" : "OUT: BT";
    lv_label_set_text_static(s_out_lbl, txt);

    /* 选了 BT 但链路不在：播音会直接报 MP_EVT_ERROR，这里先给一句明确提示，
       不然用户只会看到"点了播放没声"。 */
    if (mode == MP_OUT_BT && (!bt_audio_service_is_on() || !bt_audio_service_is_connected())) {
        lv_label_set_text_static(s_info, "BT not connected - will use nothing");
        lv_obj_set_style_text_color(s_info, COL_RED, 0);
    }
}

static void bt_sw_cb(lv_event_t *e)
{
    (void)e;
    if (s_syncing) {
        return;   /* 程序回填触发的事件，不是用户操作 */
    }
    bool want = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
    esp_err_t err = bt_audio_service_set_power(want);
    if (err != ESP_OK) {
        /* 打开失败（多为内部 RAM 不足）：把开关拨回去并提示。
         * 拨回同样会发 VALUE_CHANGED，必须挡住，否则会再调一次 set_power。 */
        s_syncing = true;
        if (want) {
            lv_obj_clear_state(s_bt_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_bt_sw, LV_STATE_CHECKED);
        }
        s_syncing = false;
        lv_label_set_text_static(s_info,
            (err == ESP_ERR_NO_MEM) ? "BT: RAM insufficient" : "BT: unavailable");
        lv_obj_set_style_text_color(s_info, COL_RED, 0);
        return;
    }
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_scanning) {
        bt_audio_service_stop_discovery();
        s_scanning = false;
        lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!bt_audio_service_is_on()) {
        lv_label_set_text_static(s_info, "Turn BT on first");
        lv_obj_set_style_text_color(s_info, COL_RED, 0);
        return;
    }
    if (bt_audio_service_start_discovery() != ESP_OK) {
        lv_label_set_text_static(s_info, "Scan failed");
        lv_obj_set_style_text_color(s_info, COL_RED, 0);
        return;
    }
    s_scanning = true;
    s_seen_dev = -1;
    lv_obj_clear_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
}

static void dev_row_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    const bt_device_t *d = bt_audio_service_get_device(idx);
    if (!d) {
        return;
    }
    uint8_t addr[6];
    memcpy(addr, d->addr, 6);
    bt_audio_service_stop_discovery();
    s_scanning = false;
    lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
    /* 连接被节流/已在连（距上次尝试 < 5 s 或上一轮还没结果）：
       明确提示，否则界面停在 Connecting... 像卡死。
       ★ 这段时间请**不要**再点：协议栈自己的重连要在 ~4 s 后才成功，
         期间反复发 connect 会把它的重试状态打回初始态。 */
    if (bt_audio_service_connect(addr) == ESP_ERR_INVALID_STATE &&
        !bt_audio_service_is_connected()) {
        lv_label_set_text_static(s_info, "Waiting, don't tap");
        lv_obj_set_style_text_color(s_info, COL_SUBTEXT, 0);
        return;
    }
    lv_label_set_text_static(s_info, "Connecting...");
    lv_obj_set_style_text_color(s_info, COL_SUBTEXT, 0);
}

static void bar_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_seeking = false;
        if (code == LV_EVENT_RELEASED) {
            int dur = media_player_service_get_duration_ms();
            int permille = lv_slider_get_value(s_bar);
            if (dur > 0) {
                media_player_service_seek(dur / 1000 * permille);
            }
        }
    }
}

/* 右侧三键：0=上一首 1=播放/暂停 2=下一首 */
static void media_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int id = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (id == 0) {
        play_relative(-1);
    } else if (id == 2) {
        play_relative(1);
    } else {
        toggle_play();
    }
}

/* ============================================================
 * 周期刷新
 * ============================================================ */

static void refresh_devices(void)
{
    int n = bt_audio_service_get_device_count();
    if (n == s_seen_dev) {
        return;
    }
    s_seen_dev = n;
    lv_obj_clean(s_dev_list);
    if (n == 0) {
        lv_obj_t *row = lv_label_create(s_dev_list);
        lv_label_set_text_static(row, "Searching...");
        lv_obj_set_style_text_color(row, COL_SUBTEXT, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        const bt_device_t *d = bt_audio_service_get_device(i);
        if (!d) {
            continue;
        }
        lv_obj_t *row = lv_button_create(s_dev_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, d->name[0] ? d->name : "(unknown)");
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(lbl, UI_FONT, 0);
        lv_obj_center(lbl);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, dev_row_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void refresh_lyrics(int pos_ms)
{
    int idx = -1;
    if (lrc_service_has_lyrics()) {
        idx = lrc_service_find_index(pos_ms);
    }
    if (idx == s_seen_idx) {
        return;
    }
    s_seen_idx = idx;

    if (!lrc_service_has_lyrics()) {
        /* 没有同名 .lrc（或没有可解析行）：中间行显示"无" */
        lv_label_set_text_static(s_lyr[0], "");
        lv_label_set_text_static(s_lyr[1], "无");
        lv_label_set_text_static(s_lyr[2], "");
        lv_obj_set_style_text_font(s_lyr[1], cn_font(), 0);
        lv_obj_set_style_text_color(s_lyr[1], COL_SUBTEXT, 0);
        return;
    }

    int base = (idx < 0) ? 0 : idx;
    for (int row = 0; row < 3; row++) {
        int line = base - 1 + row;   /* 上一行 / 当前行 / 下一行 */
        const char *txt = lrc_service_line_text(line);
        lv_label_set_text(s_lyr[row], txt ? txt : "");
        lv_obj_set_style_text_font(s_lyr[row], cn_font(), 0);
        bool current = (line == idx);
        lv_obj_set_style_text_color(s_lyr[row], current ? COL_CYAN : COL_SUBTEXT, 0);
        /* 当前行满亮度，上下行走 50% 透明：三行里"现在唱到哪句"一眼可辨 */
        lv_obj_set_style_text_opa(s_lyr[row], current ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

static void refresh_status(void)
{
    if (!s_bt_lbl) {
        return;
    }
    /* ★ 状态只能靠"每拍读一眼"来跟上：连接是异步完成的（connect() 返回后
     *   才出 CONNECTION_STATE_CHG），本页又没注册 BT 事件回调，所以早先
     *   "只在脏标志时刷新"会让标签永远停在 "BT: ready"。改成派生状态缓存比较，
     *   状态没变就不动 label（避免每 200ms 重排）。 */
    /* ★ 开关回填：电源可能在 SYS 子页被改动。每拍（及进页 refresh_status）
     *  把开关拉回真实状态；回填期间用 s_syncing 挡住回调，否则会被当成
     *  用户点击又调用一次 set_power。 */
    if (s_bt_sw) {
        bool on = bt_audio_service_is_on();
        bool sw_on = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
        if (sw_on != on) {
            s_syncing = true;
            if (on) lv_obj_add_state(s_bt_sw, LV_STATE_CHECKED);
            else    lv_obj_clear_state(s_bt_sw, LV_STATE_CHECKED);
            s_syncing = false;
        }
    }

    int st = !bt_audio_service_is_on() ? 0
           : (bt_audio_service_is_connected() ? 2 : 1);
    if (st == s_bt_state) {
        return;
    }
    s_bt_state = st;
    if (st == 0) {
        lv_label_set_text_static(s_bt_lbl, "BT: OFF");
        lv_obj_set_style_text_color(s_bt_lbl, COL_SUBTEXT, 0);
    } else if (st == 2) {
        lv_label_set_text_static(s_bt_lbl, "BT: connected");
        lv_obj_set_style_text_color(s_bt_lbl, COL_GREEN, 0);
    } else {
        lv_label_set_text_static(s_bt_lbl, "BT: ready");
        lv_obj_set_style_text_color(s_bt_lbl, COL_TEXT, 0);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) {
        return;
    }
    refresh_status();

    bool playing = media_player_service_is_playing();
    bool paused = media_player_service_is_paused();
    lv_label_set_text_static(s_play_sym,
                             (playing && !paused) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    int dur = media_player_service_get_duration_ms();
    int pos = media_player_service_get_position_ms();

    if (dur > 0) {
        if (!s_seeking) {
            lv_slider_set_value(s_bar, (int32_t)((int64_t)pos * 1000 / dur), LV_ANIM_OFF);
        }
        lv_label_set_text_fmt(s_time, "%02d:%02d / %02d:%02d",
                              pos / 60000, (pos / 1000) % 60,
                              dur / 60000, (dur / 1000) % 60);
    } else {
        lv_label_set_text_static(s_time,
                                 playing ? "BT playback (no position)" : "00:00 / --:--");
    }

    refresh_lyrics(pos);
    if (s_scanning) {
        refresh_devices();
    }
}

/* ============================================================
 * 构建
 * ============================================================ */

static lv_obj_t *mk_media_btn(lv_obj_t *parent, const char *sym, int x, int y, int id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, BTN_SIZE / 2, 0);
    lv_obj_set_style_bg_color(btn, COL_CARD, 0);
    lv_obj_set_style_bg_color(btn, COL_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_text_color(btn, COL_CYAN, 0);   /* 图标色（text_color 可继承） */
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    /* ★ bin 字体无 FontAwesome，图标必须用 UI_FONT 系字体（F47） */
    lv_obj_set_style_text_font(lbl, UI_FONT_BIG, 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void *)(intptr_t)id);
    lv_obj_add_event_cb(btn, media_btn_cb, LV_EVENT_CLICKED, NULL);
    return lbl;   /* 返回图标 label（仅播放键需要动态换图标） */
}

void tab_music_build(lv_obj_t *screen, lv_group_t *group)
{
    (void)group;
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    /* ---- 头部：输出三态 / 蓝牙开关 / 扫描 ---- */
    lv_obj_t *out_btn = lv_button_create(screen);
    lv_obj_set_size(out_btn, 120, 34);
    lv_obj_set_pos(out_btn, 8, 6);
    lv_obj_set_style_bg_color(out_btn, COL_CARD, 0);
    lv_obj_set_style_radius(out_btn, 8, 0);
    s_out_lbl = lv_label_create(out_btn);
    lv_label_set_text_static(s_out_lbl, "OUT: AUTO");
    lv_obj_set_style_text_font(s_out_lbl, UI_FONT, 0);
    lv_obj_center(s_out_lbl);
    lv_obj_add_event_cb(out_btn, out_btn_cb, LV_EVENT_CLICKED, NULL);

    s_bt_lbl = lv_label_create(screen);
    lv_label_set_text_static(s_bt_lbl, "BT: OFF");
    lv_obj_set_style_text_font(s_bt_lbl, UI_FONT, 0);
    lv_obj_set_style_text_color(s_bt_lbl, COL_SUBTEXT, 0);
    lv_obj_set_pos(s_bt_lbl, 145, 12);

    s_bt_sw = lv_switch_create(screen);
    lv_obj_set_size(s_bt_sw, 64, 32);
    lv_obj_set_pos(s_bt_sw, 250, 7);
    if (bt_audio_service_is_on()) {
        lv_obj_add_state(s_bt_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_bt_sw, bt_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_scan_btn = lv_button_create(screen);
    lv_obj_set_size(s_scan_btn, 130, 34);
    lv_obj_set_pos(s_scan_btn, 660, 6);
    lv_obj_set_style_bg_color(s_scan_btn, COL_CARD, 0);
    lv_obj_set_style_radius(s_scan_btn, 8, 0);
    lv_obj_t *scan_lbl = lv_label_create(s_scan_btn);
    lv_label_set_text_static(scan_lbl, LV_SYMBOL_WIFI "  SCAN");
    lv_obj_set_style_text_font(scan_lbl, UI_FONT, 0);
    lv_obj_center(scan_lbl);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 曲名 / 状态（封面区已删，整行左起） ---- */
    s_title = lv_label_create(screen);
    lv_label_set_text_static(s_title, "Music");
    lv_obj_set_size(s_title, 660, 52);
    lv_obj_set_pos(s_title, 16, 62);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_title, cn_font(), 0);
    lv_obj_set_style_text_color(s_title, COL_TEXT, 0);

    s_info = lv_label_create(screen);
    lv_label_set_text_static(s_info, "Pick a song in FILES");
    lv_obj_set_size(s_info, 660, 28);
    lv_obj_set_pos(s_info, 16, 118);
    lv_obj_set_style_text_font(s_info, UI_FONT, 0);
    lv_obj_set_style_text_color(s_info, COL_SUBTEXT, 0);

    /* ---- 歌词三行窗（prev / cur / next）----
     * ★ 必须是「静态三行、整行换文本」：不能用 LV_LABEL_LONG_SCROLL_CIRCULAR，
     *   否则每行都变成跑马灯，三行一起滑动，看不出"一句句递进"。
     *   行高用 SIZE_CONTENT（bin 字体 ascent37/descent-15 ⇒ 行高 52，写死会裁字）。 */
    for (int i = 0; i < 3; i++) {
        s_lyr[i] = lv_label_create(screen);
        lv_label_set_text_static(s_lyr[i], "");
        lv_obj_set_size(s_lyr[i], 660, LV_SIZE_CONTENT);
        lv_obj_set_pos(s_lyr[i], 16, 190 + i * 52);
        lv_obj_set_style_text_font(s_lyr[i], cn_font(), 0);
        lv_obj_set_style_text_color(s_lyr[i], COL_SUBTEXT, 0);
        lv_label_set_long_mode(s_lyr[i], LV_LABEL_LONG_DOT);
    }

    /* ---- 设备列表（扫描时覆盖歌词区） ---- */
    s_dev_list = lv_list_create(screen);
    lv_obj_set_size(s_dev_list, 560, 160);
    lv_obj_set_pos(s_dev_list, 16, 210);
    lv_obj_set_style_bg_color(s_dev_list, COL_DARK, 0);
    lv_obj_set_style_border_width(s_dev_list, 1, 0);
    lv_obj_set_style_border_color(s_dev_list, COL_BORDER, 0);
    lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);

    /* ---- 进度条 / 时间 ---- */
    s_bar = lv_slider_create(screen);
    lv_obj_set_size(s_bar, 560, 16);
    lv_obj_set_pos(s_bar, 16, 382);
    lv_slider_set_range(s_bar, 0, 1000);
    lv_slider_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bar, bar_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(s_bar, 14);

    s_time = lv_label_create(screen);
    lv_label_set_text_static(s_time, "00:00 / --:--");
    lv_obj_set_pos(s_time, 16, 402);
    lv_obj_set_style_text_font(s_time, UI_FONT, 0);
    lv_obj_set_style_text_color(s_time, COL_SUBTEXT, 0);

    /* ---- 右手三键：上一首 / 播放暂停 / 下一首 ---- */
    (void)mk_media_btn(screen, LV_SYMBOL_PREV, 700, 80, 0);
    s_play_sym = mk_media_btn(screen, LV_SYMBOL_PLAY, 700, 170, 1);
    (void)mk_media_btn(screen, LV_SYMBOL_NEXT, 700, 260, 2);

    media_player_service_set_evt_cb(media_evt_cb);

    /* ★ 2026-11-01 后台播放：本页可能因切大页面被销毁后又重建，
     *   按当前曲目恢复曲名/歌词/状态（播放本身一直在 media_player_service 里跑）。 */
    if (s_cur_path[0]) {
        if (s_track_count == 0) {
            scan_tracks();
        }
        s_track_idx = find_track_index(s_cur_path);
        update_title(s_cur_path);
        lrc_service_load(s_cur_path);
        if (media_player_service_is_playing()) {
            lv_label_set_text_static(s_info, "Playing");
            lv_obj_set_style_text_color(s_info, COL_TEXT, 0);
        }
    }

    s_timer = lv_timer_create(tick_cb, 200, NULL);
    s_active = false;
    s_bt_state = -1;
}

void tab_music_destroy(void)
{
    /* ★ 2026-11-01 后台播放：离开主页面（切到大页面）**不再停播**。
     *   播放由 media_player_service 持有，本页只负责展示；重建时按
     *   s_cur_path 恢复曲名/歌词/进度（见 tab_music_build）。 */
    media_player_service_set_evt_cb(NULL);

    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_out_lbl = NULL; s_bt_sw = NULL; s_bt_lbl = NULL; s_scan_btn = NULL;
    s_dev_list = NULL; s_title = NULL; s_info = NULL;
    s_lyr[0] = s_lyr[1] = s_lyr[2] = NULL;
    s_bar = NULL; s_time = NULL; s_play_sym = NULL;
    /* 保留 s_cur_path / s_track_idx 供重建时恢复；歌单下次进页重扫 */
    s_track_count = 0;
    s_active = false; s_seeking = false; s_scanning = false;
    s_seen_idx = -1000; s_seen_dev = -1;
}

void tab_music_set_active(bool active)
{
    if (active == s_active) {
        return;
    }
    s_active = active;
    if (active) {
        s_seen_idx = -1000;
        /* 进页时刷新一次字幕（避免停留在上次的行）；
           歌词若被卸掉（本页曾销毁）就按当前曲目重新读一次。 */
        if (s_cur_path[0] && !lrc_service_has_lyrics()) {
            lrc_service_load(s_cur_path);
        }
        if (s_bar) {
            refresh_lyrics(media_player_service_get_position_ms());
            refresh_status();
        }
    } else {
        /* ★ 2026-11-01 后台播放：离开本页**不停播、不卸歌词**。
         *   tick_cb 在 s_active=false 时直接返回，隐藏期间不做任何 UI 刷新；
         *   音频所有权由 media_player_service 管（小智/录音起播前会反向抢占）。 */
        s_seeking = false;
        if (s_dev_list) {
            lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
        }
        s_scanning = false;
    }
}

void tab_music_play_path(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    if (s_track_count == 0) {
        scan_tracks();
    }
    start_track(path);
    if (s_info) {
        lv_label_set_text_static(s_info, "Playing");
        lv_obj_set_style_text_color(s_info, COL_TEXT, 0);
    }
    if (s_bar) {
        refresh_lyrics(0);
    }
}
