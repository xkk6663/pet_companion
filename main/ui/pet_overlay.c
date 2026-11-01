/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pet_overlay.c
 * @brief 桌宠对话浮窗（S9 P6：去掉矢量鱼，改全宽流式对话）
 *
 * 布局（内容区 776x456，坐标系相对 s_win 且已含 pad 12）：
 *
 * ┌──────────────────────────────────────────────┐
 * │ 大肥鱼                            在线   [X]  │  y 0..48
 * ├──────────────────────────────────────────────┤
 * │                                              │
 * │   对话流（全宽、可滚、一来一回）                │  y 54..330
 * │   ┌──────────────┐                           │
 * │   │ 大肥鱼：左卡片 │                           │
 * │   └──────────────┘                           │
 * │                  ┌──────────────┐            │
 * │                  │ 我：右青气泡   │            │
 * │                  └──────────────┘            │
 * ├──────────────────────────────────────────────┤
 * │ [打卡][拍照][点歌][换壁纸]                     │  y 340..392
 * ├──────────────────────────────────────────────┤
 * │ [   按住说话   ]  [ 打字 ]  [ 清屏 ]           │  y 400..456
 * └──────────────────────────────────────────────┘
 *
 * ★ 为什么删掉左侧那条矢量鱼：截图里它占了 288px（37% 宽度）却只是装饰，
 *   中文界面本来就窄，聊天区被压到 476px ⇒ 气泡频繁折行。现在聊天区
 *   **整宽 776px**，一屏能放下 4~5 轮对话。
 *
 * ★ chips 的真相源已改为**固件自己的 `TOOLS[]`**（`ai_action_tool_label()`），
 *   不再吃云端下发的 `command_table` —— 那份是第二份真相，固件加动作时它不会跟着变。
 *
 * 线程纪律（F44 同族）：
 *   - xiaozhi 服务回调（WS/播放器任务上下文）：只把事件拷进队列；
 *   - 按键回调（iot_button 任务）：只置 volatile 标志 + 调服务 API；
 *   - LVGL 落笔**全部**在 50 ms lv_timer 里完成。
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "pet_overlay.h"
#include "overlay_manager.h"
#include "ui_theme.h"
#include "ui_binfont.h"
#include "middleware/ai/xiaozhi_service.h"
#include "middleware/ai/ai_action.h"
#include "middleware/ai/cloud_media_service.h"
#include "middleware/ai/ai_camera.h"

LV_FONT_DECLARE(lv_font_cn_20);

static const char *TAG = "UI_PET";

#define PET_W    (BSP_LCD_H_RES - 24)
#define PET_H    (BSP_LCD_V_RES - 24)
#define PET_SLIDE_MS   300

#define PET_EVT_Q_DEPTH 16

/* 单条消息的硬上限：超出部分按 UTF-8 边界截断。
 * ★ 这个值只管"显示"，不再影响队列体积 —— 事件里的文本是**堆上拷贝**
 *   （见 on_xz_evt），队列里只放指针，所以放大它不占内部 RAM。
 *   旧版 160 B ≈ 53 个汉字，长句必被腰斩，这正是"我说话的文本显示不全"。 */
#define PET_TEXT_MAX    600
/* 本地提示 / 待发文字：静态缓冲（bss），保持小 */
#define PET_LINE_MAX    240

/* ---- 布局 ----
 * 纵向：顶栏 56 → 对话 58..332 → chips 340..392 → 按钮 400..456
 * ★ chip 高必须 >= bin 字体 line_height(52)，否则中文字上下被裁
 *   （用户报的"按键里显示不全"）。52 恰好贴边，靠中心对齐兜住。 */
#define TOPBAR_H    56
#define BAR_H       56
#define BAR_Y       (PET_H - BAR_H)            /* 400 */
#define CHIP_H      52
#define CHIP_Y      (BAR_Y - 8 - CHIP_H)       /* 340 */
#define CHAT_Y      (TOPBAR_H + 2)             /* 58  */
#define CHAT_H      (CHIP_Y - 8 - CHAT_Y)      /* 274 */
#define CHAT_W      PET_W                      /* 776（整宽：鱼删了） */

/* 气泡：最大宽 = 聊天区 3/4，最长一行 30 个汉字左右，读起来不费劲 */
#define CHAT_PAD    6
#define MSG_MAXW    (((CHAT_W - 2 * CHAT_PAD) * 3) / 4)
#define MSG_PADX    14
#define MSG_PADY    7
#define MSG_GAP     8
#define MSG_MAX     30

/* chips：槽位上限，实际个数由固件 TOOLS[] 决定（当前 4） */
#define PET_CHIPS   6

/* ---- 事件桥 ----
 * ★ text 是**堆上拷贝**的指针：WS 回调只做 malloc+memcpy，不阻塞；
 *   消费端（LVGL 线程）用完 free。这样消息长度与内部 RAM 无关
 *   （队列项从 520 B/条 降到 24 B/条）。 */
typedef struct {
    int   id;
    int   role;
    char *text;
} pet_evt_msg_t;

/* ---- 控件 ---- */
static lv_obj_t *s_win;
static lv_obj_t *s_status;
static lv_obj_t *s_mood;                     /* 顶栏心情词（emotion 的唯一落点） */

static lv_obj_t *s_chat;                     /* 对话流容器（可滚动） */
static lv_obj_t *s_msg[MSG_MAX];             /* 气泡对象（满了删最旧） */
static int16_t   s_msg_dy[MSG_MAX];          /* 气泡高 + 间距 */
static int       s_msg_n;
static int32_t   s_chat_y;                   /* 下一个气泡的写入 y */

/* 打字机：整段文字逐字吐出（云端一次性下发，观感做成流式） */
static lv_obj_t *s_typing_bub, *s_typing_lb;
static char      s_typing_full[PET_TEXT_MAX];
static int       s_typing_pos;
static bool      s_typing_on;

/* "正在想..." 占位（等 ASR/LLM 的那几秒必须让用户看见东西在动） */
static lv_obj_t *s_think_bub;
static uint32_t  s_think_tick;

static lv_obj_t *s_chip[PET_CHIPS];
static lv_obj_t *s_chip_lb[PET_CHIPS];
static lv_obj_t *s_ptt_btn, *s_ptt_lb;
static lv_obj_t *s_kb_btn, *s_clr_btn;

/* 图片卡 */
static lv_obj_t *s_img_box, *s_img, *s_img_cap;
static lv_image_dsc_t s_img_dsc;
static uint8_t *s_img_buf;
static int s_img_ttl;

/* 相机实时取景卡（打卡/拍照时由 ai_camera 单槽驱动） */
static lv_obj_t *s_pv_box, *s_pv_img;
static lv_image_dsc_t s_pv_dsc;
static uint8_t *s_pv_show;
static int s_pv_ttl;
static bool s_pv_video_prev;

/* 键盘层 */
static lv_obj_t *s_kb_root, *s_kb_ta, *s_kb;
/* 拼音 IME（把 s_kb 的按键转成汉字候选；候选条是它的子对象） */
static lv_obj_t *s_ime;

static lv_timer_t   *s_timer;
static QueueHandle_t s_evt_q;

/* 状态 */
static volatile bool s_visible;
static volatile bool s_set_req;
static volatile bool s_open_req;
static volatile bool s_hw_ptt_pending;
static volatile bool s_hw_ptt_ui;
static volatile bool s_touch_ptt_ui;
/* 屏幕按钮"想按住"但会话还没就绪：与硬件键共用 pending 槽，用这个标记
 * 决定会话就绪后把 PTT 归到哪个 UI 来源。 */
static volatile bool s_ptt_pending_touch;
static volatile bool s_kb_req;
static char          s_local_say[PET_LINE_MAX];
/* 文字（命令 chip / 键盘）在会话未就绪时的暂存：懒会话就绪后由 drain 补发。
 * 只在 LVGL 线程读写（chip/kb 回调与 50ms 定时器同线程）⇒ 不需要锁。 */
static char          s_pending_text[PET_LINE_MAX];

static bool s_prev_ptt_vis;
static bool s_prev_chatting;
static lv_font_t *s_font_cjk;

static uint32_t s_tick;

/* ============================================================
 *  事件桥（服务/播放器任务上下文）：只拷贝 + 入队
 * ============================================================ */
static void on_xz_evt(const xiaozhi_evt_t *evt)
{
    if (s_evt_q == NULL || evt == NULL) {
        return;
    }
    pet_evt_msg_t m = { .id = (int)evt->id, .role = evt->role, .text = NULL };
    if (evt->text && evt->text[0]) {
        size_t len = strlen(evt->text);
        if (len > PET_TEXT_MAX - 1) {
            len = PET_TEXT_MAX - 1;
            /* 回退到 UTF-8 字符首字节：text[len] 是续字节说明这个字被切断，
             * 整个丢掉 —— 否则尾部半个汉字会渲染成乱码/空白。 */
            while (len > 0 && ((unsigned char)evt->text[len] & 0xC0) == 0x80) {
                len--;
            }
        }
        size_t n = len + 1;
        /* 优先 PSRAM：内部 RAM 只剩几 KB（F35 家族），长文本不该去抢 */
        m.text = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (m.text == NULL) {
            m.text = malloc(n);
        }
        if (m.text == NULL) {
            ESP_LOGW(TAG, "evt drop (oom, %u B)", (unsigned)n);
            return;
        }
        memcpy(m.text, evt->text, n - 1);
        m.text[n - 1] = '\0';
    }
    if (xQueueSend(s_evt_q, &m, 0) != pdPASS) {
        free(m.text);
        ESP_LOGW(TAG, "evt queue full (id=%d)", m.id);
    }
}

/* ============================================================
 *  对话流（消息气泡）
 *
 *  ★ 手工布局，不依赖 LVGL 的 flex/align 二次计算：
 *    每条气泡的尺寸用 `lv_txt_get_size()` 两遍算准，再自己摆 x/y。
 *    两遍的意义：第一遍按最大宽量出"实际需要多宽"，第二遍**用这个宽**
 *    重新量高度 —— 只用一遍的话，标签实际宽度与测量宽差几个像素，
 *    换行就多出一行，气泡高度不够 ⇒ 底部文字被裁（旧版"显示不全"之一）。
 * ============================================================ */

typedef enum {
    MSG_AI = 0,     /* 大肥鱼：左侧卡片气泡 */
    MSG_ME,         /* 用户：右侧青色气泡 */
    MSG_SYS,        /* 系统提示：居中灰字，无底 */
} msg_role_t;

static const lv_font_t *pet_font(void)
{
    return s_font_cjk ? s_font_cjk : &lv_font_cn_20;
}

static void chat_scroll_bottom(void)
{
    if (s_chat) {
        lv_obj_update_layout(s_chat);
        lv_obj_scroll_to_y(s_chat, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* 从顶部依次重排 y。删过旧消息/占位后必须调用，否则顶部会留空洞。 */
static void chat_relayout(void)
{
    int32_t y = 4;
    for (int i = 0; i < s_msg_n; i++) {
        if (s_msg[i]) {
            lv_obj_set_y(s_msg[i], y);
        }
        y += s_msg_dy[i];
    }
    s_chat_y = y;
}

static void chat_trim(void)
{
    if (s_msg_n < MSG_MAX) {
        return;
    }
    while (s_msg_n >= MSG_MAX) {
        if (s_msg[0]) {
            lv_obj_del(s_msg[0]);
        }
        memmove(&s_msg[0], &s_msg[1], sizeof(s_msg[0]) * (MSG_MAX - 1));
        memmove(&s_msg_dy[0], &s_msg_dy[1], sizeof(s_msg_dy[0]) * (MSG_MAX - 1));
        s_msg_n--;
    }
    s_msg[s_msg_n] = NULL;
    chat_relayout();
}

/* 加一条气泡。stream_out=true 时文字先留空，由 typing_tick() 逐字填。 */
static lv_obj_t *chat_add(msg_role_t role, const char *text, bool stream_out)
{
    if (s_chat == NULL || text == NULL || text[0] == '\0') {
        return NULL;
    }
    chat_trim();

    const lv_font_t *f = pet_font();
    int32_t maxw = (role == MSG_SYS) ? (CHAT_W - 48) : MSG_MAXW;

    lv_point_t sz = { 0, 0 };
    lv_txt_get_size(&sz, text, f, 0, 0, maxw, LV_TEXT_FLAG_NONE);

    int32_t padx = (role == MSG_SYS) ? 0 : MSG_PADX;
    int32_t pady = (role == MSG_SYS) ? 0 : MSG_PADY;

    /* 第二遍：用"真实标签宽"重算高度（+3 是给末字留的舍入余量） */
    int32_t inner = sz.x + 3;
    if (inner > maxw) {
        inner = maxw;
    }
    lv_point_t sz2 = { 0, 0 };
    lv_txt_get_size(&sz2, text, f, 0, 0, inner, LV_TEXT_FLAG_NONE);

    int32_t bw = inner + 2 * padx;
    int32_t bh = sz2.y + 2 * pady;

    lv_obj_t *bub = lv_obj_create(s_chat);
    lv_obj_set_size(bub, bw, bh);
    lv_obj_set_scrollable(bub, false);
    lv_obj_set_style_pad_all(bub, 0, 0);
    lv_obj_set_style_radius(bub, 14, 0);
    lv_obj_set_style_border_width(bub, 0, 0);
    if (role == MSG_ME) {
        lv_obj_set_style_bg_color(bub, COL_CYAN, 0);
        lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    } else if (role == MSG_AI) {
        lv_obj_set_style_bg_color(bub, COL_CARD, 0);
        lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bub, COL_BORDER, 0);
        lv_obj_set_style_border_width(bub, 1, 0);
    } else {
        lv_obj_set_style_bg_opa(bub, LV_OPA_TRANSP, 0);
    }

    int32_t x = (role == MSG_ME) ? (CHAT_W - CHAT_PAD - bw)
              : (role == MSG_SYS) ? ((CHAT_W - bw) / 2)
              : CHAT_PAD;
    lv_obj_set_pos(bub, x, s_chat_y);

    lv_obj_t *lb = lv_label_create(bub);
    lv_obj_set_size(lb, inner, sz2.y);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lb, f, 0);
    lv_obj_set_style_text_color(lb, (role == MSG_ME) ? COL_DARK
                                 : (role == MSG_SYS) ? COL_SUBTEXT : COL_TEXT, 0);
    lv_obj_set_pos(lb, padx, pady);
    if (!stream_out) {
        lv_label_set_text(lb, text);
    }

    s_msg[s_msg_n] = bub;
    s_msg_dy[s_msg_n] = (int16_t)(bh + MSG_GAP);
    s_msg_n++;
    s_chat_y += bh + MSG_GAP;

    chat_scroll_bottom();
    return bub;
}

/* 从消息表里摘掉一个气泡（占位用），不留空洞 */
static void chat_remove(lv_obj_t *bub)
{
    if (bub == NULL) {
        return;
    }
    for (int i = 0; i < s_msg_n; i++) {
        if (s_msg[i] == bub) {
            lv_obj_del(bub);
            memmove(&s_msg[i], &s_msg[i + 1], sizeof(s_msg[0]) * (s_msg_n - i - 1));
            memmove(&s_msg_dy[i], &s_msg_dy[i + 1], sizeof(s_msg_dy[0]) * (s_msg_n - i - 1));
            s_msg_n--;
            break;
        }
    }
    chat_relayout();
    chat_scroll_bottom();
}

static void chat_think_begin(void)
{
    if (s_think_bub != NULL || s_chat == NULL) {
        return;
    }
    s_think_bub = chat_add(MSG_AI, "正在想...", false);
    s_think_tick = s_tick;
}

static void chat_think_end(void)
{
    if (s_think_bub == NULL) {
        return;
    }
    lv_obj_t *b = s_think_bub;
    s_think_bub = NULL;
    chat_remove(b);
}

/* 点数动画：正在想 → 正在想. → 正在想.. → 正在想... */
static void chat_think_tick(void)
{
    if (s_think_bub == NULL) {
        return;
    }
    static const char *k_text[4] = { "正在想", "正在想.", "正在想..", "正在想..." };
    lv_obj_t *lb = lv_obj_get_child(s_think_bub, 0);
    if (lb) {
        lv_label_set_text(lb, k_text[(s_tick / 8) % 4]);
    }
}

static void chat_clear(void)
{
    for (int i = 0; i < s_msg_n; i++) {
        if (s_msg[i]) {
            lv_obj_del(s_msg[i]);
        }
        s_msg[i] = NULL;
    }
    s_msg_n = 0;
    s_chat_y = 4;
    s_think_bub = NULL;
    s_typing_bub = NULL;
    s_typing_lb = NULL;
    s_typing_on = false;
}

/* ============================================================
 *  打字机：整段文字逐字吐出（云端一次性下发，观感做成流式）
 * ============================================================ */
static int utf8_next(const char *s, int pos)
{
    unsigned char c = (unsigned char)s[pos];
    if (c == 0) {
        return pos;
    }
    int len = 1;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
    }
    int i = 1;
    while (i < len && s[pos + i] != '\0') {
        i++;
    }
    return pos + i;
}

/* 立刻把上一条补完整（新消息到达时，别让它半句卡着） */
static void typing_flush(void)
{
    if (!s_typing_on) {
        return;
    }
    s_typing_on = false;
    if (s_typing_lb) {
        lv_label_set_text(s_typing_lb, s_typing_full);
    }
    s_typing_bub = NULL;
    s_typing_lb = NULL;
}

/* 大肥鱼说话：开一条气泡，之后按节奏吐字 */
static void chat_say_stream(const char *text)
{
    typing_flush();
    chat_think_end();

    lv_obj_t *bub = chat_add(MSG_AI, text, true);
    if (bub == NULL) {
        return;
    }
    snprintf(s_typing_full, sizeof(s_typing_full), "%s", text);
    s_typing_pos = 0;
    s_typing_bub = bub;
    s_typing_lb = lv_obj_get_child(bub, 0);
    s_typing_on = (s_typing_lb != NULL);
    if (!s_typing_on) {
        return;
    }
    lv_label_set_text(s_typing_lb, "");
}

static void typing_tick(void)
{
    if (!s_typing_on || s_typing_lb == NULL) {
        return;
    }
    int len = (int)strlen(s_typing_full);
    if (s_typing_pos >= len) {
        s_typing_on = false;
        s_typing_bub = NULL;
        s_typing_lb = NULL;
        return;
    }

    /* 长文本一次多吐几个字：50 ms/字 对 200 字要 10 s，太磨人 */
    int remain = len - s_typing_pos;
    int step = (remain > 300) ? 6 : (remain > 150) ? 4 : (remain > 60) ? 2 : 1;
    while (step-- > 0 && s_typing_pos < len) {
        s_typing_pos = utf8_next(s_typing_full, s_typing_pos);
    }

    char save = s_typing_full[s_typing_pos];
    s_typing_full[s_typing_pos] = '\0';
    lv_label_set_text(s_typing_lb, s_typing_full);
    s_typing_full[s_typing_pos] = save;
}

/* 系统提示（灰字居中小条） */
static void chat_sys(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    typing_flush();
    chat_add(MSG_SYS, text, false);
}

/* ============================================================
 *  图片卡 / 相机取景卡
 * ============================================================ */
/* 云端 emotion 名 → 中文心情词。
 * ★ 删掉矢量鱼之后表情没有载体了，但云端仍在发 speaking/idle/thinking
 *   （网关每个回复都发）。放一个顶栏小标记，至少让"她在说话/在想"看得见，
 *   也不用把鱼请回来。未知名字**不显示**，避免英文原样丢屏上。 */
static const char *mood_zh(const char *name)
{
    static const struct { const char *k, *v; uint32_t c; } M[] = {
        { "idle",      "待命", 0x94A3B8 },
        { "thinking",  "在想", 0x22D3EE },
        { "speaking",  "说话", 0x34D399 },
        { "happy",     "开心", 0x34D399 },
        { "excited",   "兴奋", 0x34D399 },
        { "love",      "喜欢", 0xF472B6 },
        { "shy",       "害羞", 0xF472B6 },
        { "surprised", "惊讶", 0x22D3EE },
        { "confused",  "疑惑", 0x22D3EE },
        { "sad",       "难过", 0x94A3B8 },
        { "cry",       "委屈", 0x94A3B8 },
        { "sleepy",    "困了", 0x94A3B8 },
        { "angry",     "生气", 0xF87171 },
        { "neutral",   "平静", 0x94A3B8 },
    };
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        if (strcmp(M[i].k, name) == 0) {
            lv_obj_set_style_text_color(s_mood, lv_color_hex(M[i].c), 0);
            return M[i].v;
        }
    }
    return NULL;
}

static void mood_set(const char *name)
{
    if (s_mood == NULL) {
        return;
    }
    const char *zh = mood_zh(name);
    if (zh == NULL) {
        return;
    }
    lv_label_set_text(s_mood, zh);
}

static void mood_init(lv_obj_t *parent)
{
    s_mood = lv_label_create(parent);
    lv_label_set_text_static(s_mood, "");
    lv_obj_set_style_text_color(s_mood, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_mood, pet_font(), 0);
    lv_obj_set_pos(s_mood, 96, 0);
}

static void image_poll(void)
{
    if (s_img_ttl > 0 && --s_img_ttl == 0) {
        lv_obj_set_hidden(s_img_box, true);
        if (s_img_buf) {
            heap_caps_free(s_img_buf);
            s_img_buf = NULL;
        }
        return;
    }
    uint8_t *buf = NULL;
    int w = 0, h = 0, stride = 0;
    char cap[CLOUD_MEDIA_CAPTION_MAX];
    if (!cloud_media_service_take(&buf, &w, &h, &stride, cap, sizeof(cap))) {
        return;
    }
    if (s_img_buf) {
        heap_caps_free(s_img_buf);          /* 新图顶掉旧图 */
    }
    s_img_buf = buf;
    lv_memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.w = (uint32_t)w;
    s_img_dsc.header.h = (uint32_t)h;
    s_img_dsc.header.stride = (uint32_t)stride;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.data = buf;
    s_img_dsc.data_size = (uint32_t)stride * (uint32_t)h;
    lv_image_set_src(s_img, &s_img_dsc);
    lv_label_set_text(s_img_cap, cap[0] ? cap : "");
    lv_obj_set_hidden(s_img_box, false);
    lv_obj_move_foreground(s_img_box);
    s_img_ttl = 160;                        /* 8 s */
    ESP_LOGI(TAG, "image shown %dx%d (stride %d)", w, h, stride);
}

/* 相机实时取景：ai_camera 单槽 → 50 ms 轮询取走（连续换帧，
 * 静态照片卡 3 s 后消失）。 */
static void preview_poll(void)
{
    bool video = ai_camera_preview_video_get();
    if (s_pv_video_prev && !video) {
        s_pv_video_prev = false;
        s_pv_ttl = 0;
        lv_obj_set_hidden(s_pv_box, true);
        if (s_pv_show) {
            heap_caps_free(s_pv_show);
            s_pv_show = NULL;
        }
        return;
    }
    s_pv_video_prev = video;

    if (s_pv_ttl > 0) {
        if (--s_pv_ttl == 0) {
            lv_obj_set_hidden(s_pv_box, true);
            if (s_pv_show) {
                heap_caps_free(s_pv_show);
                s_pv_show = NULL;
            }
        }
    }

    uint8_t *buf = NULL;
    int w = 0, h = 0;
    if (!ai_camera_preview_take(&buf, &w, &h)) {
        return;
    }
    if (s_pv_show) {
        heap_caps_free(s_pv_show);
    }
    s_pv_show = buf;
    lv_memset(&s_pv_dsc, 0, sizeof(s_pv_dsc));
    s_pv_dsc.header.w = (uint32_t)w;
    s_pv_dsc.header.h = (uint32_t)h;
    s_pv_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_pv_dsc.data = buf;
    s_pv_dsc.data_size = (uint32_t)w * (uint32_t)h * 2u;
    lv_image_set_src(s_pv_img, &s_pv_dsc);
    lv_obj_set_hidden(s_pv_box, false);
    lv_obj_move_foreground(s_pv_box);
    s_pv_ttl = video ? 0 : 60;      /* 视频流：常显；静态帧：3 s */
    ESP_LOGI(TAG, "camera preview %dx%d (%s)", w, h, video ? "live" : "still");
}

/* ============================================================
 *  50 ms 定时器：唯一落笔点
 * ============================================================ */
static void evt_drain(void)
{
    pet_evt_msg_t m;
    while (s_evt_q && xQueueReceive(s_evt_q, &m, 0) == pdPASS) {
        switch ((xiaozhi_evt_id_t)m.id) {
        case XZ_EVT_TEXT:
            if (m.role == 0) {
                /* 语音识别结果（云端回吐的"你说的"）——右侧我的气泡 */
                chat_think_end();
                chat_add(MSG_ME, m.text, false);
            } else {
                /* 大肥鱼的回答：逐字吐，观感是流式 */
                chat_say_stream(m.text);
            }
            break;
        case XZ_EVT_EMOJI:
            /* 鱼删了，表情本体没有落点；改在顶栏显示中文心情词（说话/在想/开心…） */
            ESP_LOGD(TAG, "emotion: %s", m.text ? m.text : "");
            mood_set(m.text);
            break;
        case XZ_EVT_TTS_STATE:
            /* role=1 start / role=0 stop；已有文字就撤掉"正在想" */
            if (m.role == 1) {
                chat_think_end();
            }
            break;
        case XZ_EVT_ERROR:
            if (m.text && m.text[0]) {
                /* ★ 错误必须进对话流：只写后台日志的话，用户看到的就是"按了没反应" */
                chat_think_end();
                chat_sys(m.text);
            }
            break;
        case XZ_EVT_LINK:
            /* 链路事件不占对话流（顶栏状态已经表达），只留串口 */
            ESP_LOGI(TAG, "link: %s", m.text ? m.text : "");
            break;
        default:
            break;
        }
        if (m.text) {
            free(m.text);
        }
    }
}

static void key_req_drain(void)
{
    if (s_set_req) {
        s_set_req = false;
        if (!s_visible) {
            /* ★★ SET 只负责"打开桌宠面板"，**不建会话**。
             *
             * 会话 = 借 codec + 起 recorder + 占内存，本身不计费，但它是一切
             * 上行的前提：一旦会话在，任何自动逻辑都可能把语音送上去（ASR /
             * LLM / TTS 三项都按量计费）。所以把会话推迟到**按住录音键**那一刻
             * 才建 —— 用户没开口，云端一个字节都收不到（S9 token 纪律）。 */
            overlay_open(OVERLAY_PET);
            ESP_LOGI(TAG, "SET: panel open (session lazy - waiting for PTT)");
        } else {
            /* 再按 = 收起面板；会话由 pet_overlay_hide() 统一收（codec 归还） */
            overlay_close(OVERLAY_PET);
        }
    }
    if (s_open_req) {
        s_open_req = false;
        if (!s_visible) {
            overlay_open(OVERLAY_PET);
        }
    }
    if (s_kb_req) {
        s_kb_req = false;
        lv_obj_move_foreground(s_kb_root);
        lv_obj_set_hidden(s_kb_root, false);
        lv_textarea_set_text(s_kb_ta, "");
        lv_keyboard_set_textarea(s_kb, s_kb_ta);
        /* 上一次可能停在"拼音打了一半"的状态：候选条残留旧字，先收起。
           （IME 内部的输入缓存只有入口 Enter 才会清，残留最多是选字时多删几个字符，无副作用） */
        if (s_ime) {
            lv_obj_set_hidden(lv_ime_pinyin_get_cand_panel(s_ime), true);
        }
    }
    if (s_local_say[0]) {
        char tmp[PET_LINE_MAX];
        snprintf(tmp, sizeof(tmp), "%s", s_local_say);
        s_local_say[0] = '\0';
        chat_add(MSG_AI, tmp, false);
        if (!s_visible) {
            overlay_open(OVERLAY_PET);
        }
    }
    /* 懒会话就绪后补发暂存的文字（命令 chip / 键盘输入，见 submit_text） */
    if (s_pending_text[0] && xiaozhi_service_get_state() == XZ_STATE_CHATTING) {
        if (xiaozhi_service_send_text(s_pending_text) != ESP_OK) {
            chat_think_end();
            chat_sys("发送失败，请再说一次。");
        }
        ESP_LOGI(TAG, "pending text sent: %.48s", s_pending_text);
        s_pending_text[0] = '\0';
    }
    /* 按住录音键（硬件 MODE 或屏幕按钮）但会话还没就绪：CHATTING 后补一票。
     * ★ 这一支同时覆盖 PREPARING 窗口（start 后约 0.5 s）：用户在窗口内
     *   按下时两个入口都只置 pending，由这里补发 —— 否则"第一次按没反应"。 */
    if (s_hw_ptt_pending && xiaozhi_service_get_state() == XZ_STATE_CHATTING) {
        s_hw_ptt_pending = false;
        if (xiaozhi_service_ptt_start() == ESP_OK) {
            if (s_ptt_pending_touch) {
                s_ptt_pending_touch = false;
                s_touch_ptt_ui = true;
            } else {
                s_hw_ptt_ui = true;
            }
        } else {
            s_ptt_pending_touch = false;
        }
    }
}

static void pet_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_tick++;
    evt_drain();
    key_req_drain();
    image_poll();
    preview_poll();
    typing_tick();
    chat_think_tick();

    /* 思考占位超时兜底：云端 30 s 没有任何回包（断链 / ASR 空 / 队列丢）时，
     * 别让"正在想"永远挂着 —— 那看起来就是"死机"。 */
    if (s_think_bub != NULL && (s_tick - s_think_tick) > 600) {
        chat_think_end();
        chat_sys("没收到回复，请再说一次。");
    }

    /* PTT 视觉：屏幕按钮 OR 硬件 MODE 键。
     * ★ 这里**不能**再拼 LV_SYMBOL_AUDIO：bin 字体没有 FontAwesome 私用区字形，
     *   图标会和中文同框 ⇒ 按钮里出现"方框"。 */
    bool ptt_vis = (s_hw_ptt_ui || s_touch_ptt_ui);
    if (ptt_vis != s_prev_ptt_vis) {
        if (ptt_vis) {
            lv_label_set_text_static(s_ptt_lb, "正在听，松手发送");
            lv_obj_set_style_bg_color(s_ptt_btn, COL_RED, 0);
        } else {
            lv_label_set_text_static(s_ptt_lb, "按住说话");
            lv_obj_set_style_bg_color(s_ptt_btn, COL_CYAN, 0);
            /* 松手 = 已上送，等 ASR/LLM（8~18 s）。挂个占位，别让用户干瞪眼。
             * ★ 放在边沿统一处理，硬件 MODE 与屏幕按钮两条路都覆盖。 */
            chat_think_begin();
        }
        s_prev_ptt_vis = ptt_vis;
    }

    /* 状态行（全中文；省略号统一用 ASCII "..."，中文省略号 U+2026 不在字表里） */
    xiaozhi_state_t st = xiaozhi_service_get_state();
    bool chatting = (st == XZ_STATE_CHATTING);
    if (chatting != s_prev_chatting || (s_tick % 20) == 0) {
        s_prev_chatting = chatting;
        if (chatting) {
            lv_label_set_text(s_status, xiaozhi_service_link_up() ? "在线" : "连接中");
            lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
        } else if (st == XZ_STATE_PREPARING) {
            lv_label_set_text(s_status, "启动中");
            lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        } else if (st == XZ_STATE_RELEASING) {
            lv_label_set_text(s_status, "准备休眠");
            lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        } else {
            lv_label_set_text(s_status, xiaozhi_service_link_up() ? "待命" : "离线");
            lv_obj_set_style_text_color(s_status,
                xiaozhi_service_link_up() ? COL_CYAN : COL_RED, 0);
        }
    }
}

/* ============================================================
 *  交互回调
 * ============================================================ */

/* 文字发送统一入口（命令 chip 与键盘共用）。
 *
 * ★ 懒会话：SET 不再建会话，所以用户直接点命令词/打字时会话可能是 IDLE。
 *   这里是**用户主动发起**（不是后台自动），符合"用户动作才花钱"的纪律：
 *   就地建会话，把文字挂 pending，CHATTING 后由 drain 补发。
 * ★ 只在 LVGL 线程调用（两个回调与 50ms 定时器同线程）⇒ s_pending_text 无需锁。 */
static void submit_text(const char *txt)
{
    if (txt == NULL || txt[0] == '\0') {
        return;
    }
    if (xiaozhi_service_get_state() == XZ_STATE_CHATTING &&
        xiaozhi_service_send_text(txt) == ESP_OK) {
        chat_add(MSG_ME, txt, false);
        chat_think_begin();
        return;
    }
    snprintf(s_pending_text, sizeof(s_pending_text), "%s", txt);
    chat_add(MSG_ME, txt, false);
    chat_think_begin();
    if (xiaozhi_service_get_state() == XZ_STATE_IDLE &&
        xiaozhi_service_start() != ESP_OK) {
        s_pending_text[0] = '\0';
        chat_think_end();
        chat_sys("麦克风被占用，稍后再试。");
    }
}

static void chip_cb(lv_event_t *e)
{
    /* ★ user_data 存的是**索引**（事件注册时固定），文字现取固件表 ——
     *   与 F41-A 同族纪律：不要把指针塞进 user_data。 */
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    submit_text(ai_action_tool_label(idx));
}

static void ptt_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        xiaozhi_state_t st = xiaozhi_service_get_state();
        if (st != XZ_STATE_CHATTING) {
            /* 会话没就绪（IDLE / PREPARING）：挂 pending，就绪后由 drain 补发。
             * ★ token 纪律：IDLE 时**按录音键**才是第一次建会话（SET 只开面板）。 */
            if (st == XZ_STATE_IDLE) {
                if (xiaozhi_service_start() != ESP_OK) {
                    chat_sys("麦克风被占用，稍后再试。");
                    return;
                }
            }
            s_hw_ptt_pending = true;
            s_ptt_pending_touch = true;
            return;
        }
        if (xiaozhi_service_ptt_start() != ESP_OK) {
            s_touch_ptt_ui = false;
            return;
        }
        s_touch_ptt_ui = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* 松手必须清 pending：否则会话刚就绪就凭空开录（白占麦） */
        s_hw_ptt_pending = false;
        s_ptt_pending_touch = false;
        if (s_touch_ptt_ui) {
            xiaozhi_service_ptt_stop();
            s_touch_ptt_ui = false;
        }
    }
}

static void kb_ready_cb(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_kb_ta);
    if (txt && txt[0]) {
        submit_text(txt);
    }
    lv_obj_set_hidden(s_kb_root, true);
}

static void kb_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_set_hidden(s_kb_root, true);
}

static void kb_btn_cb(lv_event_t *e)
{
    (void)e;
    s_kb_req = true;
}

static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    chat_clear();
    chat_sys("已清屏。按住说话，松手发送。");
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    overlay_close(OVERLAY_PET);
}

/* ============================================================
 *  动画骨架
 * ============================================================ */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void pet_animate_to(int32_t target_y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_win);
    lv_anim_set_values(&a, lv_obj_get_y(s_win), target_y);
    lv_anim_set_time(&a, PET_SLIDE_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ============================================================
 *  构建
 * ============================================================ */
static lv_obj_t *mk_btn(lv_obj_t *parent, int w, int h, lv_color_t bg, bool filled)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    if (filled) {
        lv_obj_set_style_border_width(b, 0, 0);
    } else {
        lv_obj_set_style_border_color(b, COL_BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
    }
    return b;
}

/* chips 的文字来自固件自己的动作表（唯一真相） */
static void chips_build(lv_obj_t *parent)
{
    int n = ai_action_tool_count();
    if (n > PET_CHIPS) {
        n = PET_CHIPS;
    }
    int32_t cw = (n > 0) ? (CHAT_W - (n - 1) * 8) / n : CHAT_W;

    for (int i = 0; i < PET_CHIPS; i++) {
        const char *lb = (i < n) ? ai_action_tool_label(i) : NULL;

        s_chip[i] = mk_btn(parent, cw, CHIP_H, COL_CARD, false);
        lv_obj_set_pos(s_chip[i], i * (cw + 8), CHIP_Y);
        lv_obj_set_style_pad_all(s_chip[i], 0, 0);
        lv_obj_add_event_cb(s_chip[i], chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_chip_lb[i] = lv_label_create(s_chip[i]);
        lv_label_set_text(s_chip_lb[i], lb ? lb : "");
        lv_obj_set_width(s_chip_lb[i], LV_PCT(100));
        /* 超宽循环滚动：宁可滚一下，也不要把 4 个中文字裁掉半个 */
        lv_label_set_long_mode(s_chip_lb[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(s_chip_lb[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_chip_lb[i], COL_TEXT, 0);
        lv_obj_set_style_text_font(s_chip_lb[i], pet_font(), 0);
        lv_obj_center(s_chip_lb[i]);
        if (!lb) {
            lv_obj_set_hidden(s_chip[i], true);
        }
    }
    ESP_LOGI(TAG, "chips: %d from firmware TOOLS[]", n);
}

void pet_overlay_init(void)
{
    s_font_cjk = ui_binfont_get();

    lv_obj_t *top = lv_layer_top();
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(top, false);

    s_win = lv_obj_create(top);
    lv_obj_set_size(s_win, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(s_win, 0, BSP_LCD_V_RES);
    lv_obj_set_scrollable(s_win, false);
    lv_obj_set_style_bg_color(s_win, COL_BG, 0);
    lv_obj_set_style_border_width(s_win, 0, 0);
    lv_obj_set_style_radius(s_win, 0, 0);
    lv_obj_set_style_pad_all(s_win, 12, 0);

    /* ---- 顶栏：标题 + 状态 + 关闭（全中文，关闭键单独用 UI_FONT 画 ✕）---- */
    lv_obj_t *title = lv_label_create(s_win);
    lv_label_set_text_static(title, "大肥鱼");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, pet_font(), 0);
    lv_obj_set_pos(title, 0, 0);

    mood_init(s_win);            /* 标题右侧的心情词（emotion 落点） */

    s_status = lv_label_create(s_win);
    lv_label_set_text_static(s_status, "离线");
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_status, pet_font(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -72, 0);

    lv_obj_t *xbtn = mk_btn(s_win, 58, 34, COL_CARD, false);
    lv_obj_align(xbtn, LV_ALIGN_TOP_RIGHT, 0, 11);
    lv_obj_set_style_radius(xbtn, 10, 0);
    lv_obj_add_event_cb(xbtn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xlb = lv_label_create(xbtn);
    lv_label_set_text_static(xlb, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xlb, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(xlb, UI_FONT, 0);
    lv_obj_center(xlb);

    /* 顶栏分隔线：视觉上把"对话"和"标题"分开，比描边整屏更轻 */
    lv_obj_t *sep = lv_obj_create(s_win);
    lv_obj_set_size(sep, CHAT_W, 1);
    lv_obj_set_pos(sep, 0, TOPBAR_H);
    lv_obj_set_style_bg_color(sep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_scrollable(sep, false);

    /* ---- 对话流（整宽、透明底：气泡自己带底，容器别再套一层）---- */
    s_chat = lv_obj_create(s_win);
    lv_obj_set_pos(s_chat, 0, CHAT_Y);
    lv_obj_set_size(s_chat, CHAT_W, CHAT_H);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat, 0, 0);
    lv_obj_set_style_pad_all(s_chat, 0, 0);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);
    /* ★ 不设 flex/align：气泡的 x/y 全由 chat_add() 算好 */

    /* 命令词 chips：文字来自固件 TOOLS[]（加动作只改固件） */
    chips_build(s_win);

    /* ---- 底部按钮行：按住说话（宽） + 打字 + 清屏 ---- */
    s_ptt_btn = mk_btn(s_win, 490, BAR_H, COL_CYAN, true);
    lv_obj_set_pos(s_ptt_btn, 0, BAR_Y);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_PRESS_LOST, NULL);

    s_ptt_lb = lv_label_create(s_ptt_btn);
    lv_label_set_text_static(s_ptt_lb, "按住说话");
    lv_obj_set_style_text_color(s_ptt_lb, COL_DARK, 0);
    lv_obj_set_style_text_font(s_ptt_lb, pet_font(), 0);
    lv_obj_center(s_ptt_lb);

    s_kb_btn = mk_btn(s_win, 150, BAR_H, COL_CARD, false);
    lv_obj_set_pos(s_kb_btn, 498, BAR_Y);
    lv_obj_add_event_cb(s_kb_btn, kb_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *klb = lv_label_create(s_kb_btn);
    lv_label_set_text_static(klb, "打字");
    lv_obj_set_style_text_color(klb, COL_TEXT, 0);
    lv_obj_set_style_text_font(klb, pet_font(), 0);
    lv_obj_center(klb);

    s_clr_btn = mk_btn(s_win, 120, BAR_H, COL_CARD, false);
    lv_obj_set_pos(s_clr_btn, 656, BAR_Y);
    lv_obj_add_event_cb(s_clr_btn, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clb = lv_label_create(s_clr_btn);
    lv_label_set_text_static(clb, "清屏");
    lv_obj_set_style_text_color(clb, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(clb, pet_font(), 0);
    lv_obj_center(clb);

    /* 图片卡（最后创建 = 最上层；浮在对话流之上，8 s 自动消失） */
    s_img_box = lv_obj_create(s_win);
    lv_obj_set_size(s_img_box, 300, 236);
    lv_obj_set_pos(s_img_box, CHAT_W - 304, CHAT_Y + 4);
    lv_obj_set_style_bg_color(s_img_box, COL_DARK, 0);
    lv_obj_set_style_border_color(s_img_box, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_img_box, 1, 0);
    lv_obj_set_style_radius(s_img_box, 10, 0);
    lv_obj_set_style_pad_all(s_img_box, 4, 0);
    lv_obj_set_scrollable(s_img_box, false);
    lv_obj_set_hidden(s_img_box, true);

    s_img = lv_image_create(s_img_box);
    lv_obj_set_size(s_img, 288, 200);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 0);

    s_img_cap = lv_label_create(s_img_box);
    lv_label_set_text_static(s_img_cap, "");
    lv_obj_set_width(s_img_cap, 284);
    lv_label_set_long_mode(s_img_cap, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_img_cap, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_img_cap, pet_font(), 0);
    lv_obj_align(s_img_cap, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 相机取景卡（打卡 / 拍照期间显示；与图片卡同区域，后创建者在上） */
    s_pv_box = lv_obj_create(s_win);
    lv_obj_set_size(s_pv_box, 216, 126);
    lv_obj_set_pos(s_pv_box, CHAT_W - 220, CHAT_Y + 4);
    lv_obj_set_style_bg_color(s_pv_box, COL_DARK, 0);
    lv_obj_set_style_border_color(s_pv_box, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_pv_box, 1, 0);
    lv_obj_set_style_radius(s_pv_box, 8, 0);
    lv_obj_set_style_pad_all(s_pv_box, 3, 0);
    lv_obj_set_scrollable(s_pv_box, false);
    lv_obj_set_hidden(s_pv_box, true);

    s_pv_img = lv_image_create(s_pv_box);
    lv_obj_set_size(s_pv_img, 208, 118);
    lv_image_set_inner_align(s_pv_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(s_pv_img);

    /* ---- 键盘层（默认隐藏；覆盖全屏避免和气泡抢触摸）---- */
    s_kb_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_kb_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(s_kb_root, 0, 0);
    lv_obj_set_style_bg_color(s_kb_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_kb_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb_root, 0, 0);
    lv_obj_set_style_pad_all(s_kb_root, 12, 0);
    lv_obj_set_hidden(s_kb_root, true);

    lv_obj_t *kbtitle = lv_label_create(s_kb_root);
    lv_label_set_text_static(kbtitle, "文字输入（英文 / 拼音，选候选字即输入中文）");
    lv_obj_set_style_text_color(kbtitle, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(kbtitle, pet_font(), 0);
    lv_obj_align(kbtitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_kb_ta = lv_textarea_create(s_kb_root);
    lv_obj_set_size(s_kb_ta, 700, 56);
    lv_obj_align(s_kb_ta, LV_ALIGN_TOP_MID, 0, 34);
    lv_textarea_set_one_line(s_kb_ta, true);
    lv_textarea_set_max_length(s_kb_ta, 120);
    /* ★ 占位符必须全 ASCII：字体在 binfont 缺失时会回退 cn_20（小字表），
     *   中文占位符可能出方框；ASCII 在任何一档都有。 */
    lv_textarea_set_placeholder_text(s_kb_ta, "Type here ...");
    /* 输入框要能显示中文 ⇒ 用全 CJK 的 pet_font()，不能用 UI_FONT_BIG（montserrat）。 */
    lv_obj_set_style_text_font(s_kb_ta, pet_font(), 0);

    lv_obj_t *ok = mk_btn(s_kb_root, 110, 48, COL_GREEN, true);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -130, 96);
    lv_obj_add_event_cb(ok, kb_ready_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text_static(okl, "发送");
    lv_obj_set_style_text_color(okl, COL_DARK, 0);
    lv_obj_set_style_text_font(okl, pet_font(), 0);
    lv_obj_center(okl);

    lv_obj_t *cancel = mk_btn(s_kb_root, 110, 48, COL_CARD, false);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, 0, 96);
    lv_obj_add_event_cb(cancel, kb_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel);
    lv_label_set_text_static(clbl, "取消");
    lv_obj_set_style_text_color(clbl, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(clbl, pet_font(), 0);
    lv_obj_center(clbl);

    s_kb = lv_keyboard_create(s_kb_root);
    lv_obj_set_size(s_kb, 760, 240);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_kb_ta);
    lv_obj_add_event_cb(s_kb, kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cancel_cb, LV_EVENT_CANCEL, NULL);

    /* ★ 中文输入：LVGL 自带拼音 IME（26 键 + 候选条），挂在键盘上。
     *   要点：
     *   1) 候选条是 IME 的子对象，由 lv_ime_pinyin_set_keyboard() 挂到键盘
     *      的父对象并对齐到键盘上方 —— 所以必须**先**有 s_kb 再 binding。
     *   2) 字体必须用 pet_font()（ui_binfont 全 CJK 字表）。默认 montserrat /
     *      cn_20 都不含候选汉字，用它们候选条全是方框。给 IME 设字体会触发
     *      LV_EVENT_STYLE_CHANGED，LVGL 内部据此把字体同步到候选条。
     *   3) 候选条默认高 5%（480 屏只有 24 px），装不下 28 px 的汉字，需显式改尺寸。 */
    s_ime = lv_ime_pinyin_create(s_kb_root);
    lv_ime_pinyin_set_keyboard(s_ime, s_kb);
    lv_obj_set_style_text_font(s_ime, pet_font(), 0);
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
    lv_obj_set_size(cand, 760, 48);
    lv_obj_align_to(cand, s_kb, LV_ALIGN_OUT_TOP_MID, 0, -6);
    lv_obj_set_style_text_color(cand, COL_TEXT, 0);
    lv_obj_set_style_text_font(cand, pet_font(), 0);

    /* ---- 事件队列 + 轮询 + 按键注册 ---- */
    s_evt_q = xQueueCreate(PET_EVT_Q_DEPTH, sizeof(pet_evt_msg_t));
    s_timer = lv_timer_create(pet_timer_cb, 50, NULL);
    xiaozhi_service_set_evt_cb(on_xz_evt);
    button_service_set_evt_cb(pet_overlay_key_feed);

    s_visible = false;
    s_prev_ptt_vis = false;
    s_prev_chatting = false;
    s_tick = 0;
    s_chat_y = 4;

    /* 开场白走消息流（中文；不带 LV_SYMBOL，bin 字体会把它画成方框） */
    chat_add(MSG_AI, "嗨，我是大肥鱼。想聊天、点歌、拍照，直接说就行。", false);

    ESP_LOGI(TAG, "pet overlay ready (%dx%d, chat %dx%d, chips=%d)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, CHAT_W, CHAT_H, ai_action_tool_count());
}

void pet_overlay_deinit(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_img_buf) {
        heap_caps_free(s_img_buf);
        s_img_buf = NULL;
    }
    if (s_pv_show) {
        heap_caps_free(s_pv_show);
        s_pv_show = NULL;
    }
    if (s_evt_q) {
        /* 队列里可能还有没消费的文本指针，先释放再删队列 */
        pet_evt_msg_t m;
        while (xQueueReceive(s_evt_q, &m, 0) == pdPASS) {
            free(m.text);
        }
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }
    xiaozhi_service_set_evt_cb(NULL);
    button_service_set_evt_cb(NULL);
    if (s_kb_root) {
        lv_obj_del(s_kb_root);   /* 连带 s_kb / s_ime / 候选条一起删 */
        s_kb_root = NULL;
        s_kb = NULL;
        s_kb_ta = NULL;
        s_ime = NULL;
    }
    if (s_win) {
        lv_obj_del(s_win);
        s_win = NULL;
    }
    s_visible = false;
    s_chat = NULL;
    s_msg_n = 0;
    s_chat_y = 0;
    s_typing_on = false;
    s_typing_bub = NULL;
    s_typing_lb = NULL;
    s_think_bub = NULL;
}

/* ============================================================
 *  overlay_manager 钩子
 * ============================================================ */
void pet_overlay_show(void)
{
    if (s_win) {
        pet_animate_to(0);
        s_visible = true;
    }
}

void pet_overlay_hide(void)
{
    if (s_win) {
        pet_animate_to(BSP_LCD_V_RES);
        s_visible = false;
        s_hw_ptt_pending = false;
        s_ptt_pending_touch = false;
        s_pending_text[0] = '\0';   /* 关窗即作废：别让下次会话凭空冒出一句话 */
        if (s_hw_ptt_ui) {
            xiaozhi_service_ptt_stop();
            s_hw_ptt_ui = false;
        }
        if (s_touch_ptt_ui) {
            xiaozhi_service_ptt_stop();
            s_touch_ptt_ui = false;
        }
        if (s_kb_root) {
            lv_obj_set_hidden(s_kb_root, true);
        }
        /* ★ 关面板 = 结束这轮对话：把会话收掉，codec 立刻还给系统
         *   （否则用户关窗后 recorder 还在跑，录音/视频页会抢不到麦）。
         *   **WS 长连接不动** —— 它不属于会话，断了云端就推不到设备。
         *   IDLE 时 stop 返回 INVALID_STATE，无害。 */
        (void)xiaozhi_service_stop();
    }
}

bool pet_overlay_is_visible(void)
{
    return s_visible;
}

void pet_overlay_say_local(const char *text)
{
    if (text == NULL) {
        return;
    }
    snprintf(s_local_say, sizeof(s_local_say), "%s", text);
}

/* ============================================================
 *  硬件按键入口（iot_button 任务上下文：无 LVGL）
 * ============================================================ */
void pet_overlay_key_feed(btn_id_t id, btn_evt_t evt)
{
    if (id == BTN_ID_SET) {
        if (evt == BTN_EVT_SINGLE_CLICK) {
            s_set_req = true;
        }
        return;
    }
    if (id == BTN_ID_MODE) {
        if (evt == BTN_EVT_PRESS_DOWN) {
            xiaozhi_state_t st = xiaozhi_service_get_state();
            if (st == XZ_STATE_CHATTING) {
                s_hw_ptt_ui = (xiaozhi_service_ptt_start() == ESP_OK);
            } else if (st == XZ_STATE_IDLE) {
                /* ★ 语音链路的唯一入口：**按下录音键**才建会话（SET 不建）。
                 *   start 是异步投队列，会话就绪由 drain 里 pending 补发 PTT。 */
                if (!s_visible) {
                    s_open_req = true;      /* 没开面板就直接说话：顺手打开 */
                }
                if (xiaozhi_service_start() != ESP_OK) {
                    ESP_LOGW(TAG, "mode: start failed (mic busy?)");
                    return;
                }
                s_hw_ptt_pending = true;
                s_ptt_pending_touch = false;
            } else if (st == XZ_STATE_PREPARING) {
                /* 建链窗口里又按了一次：记下来，就绪后立刻开录 */
                s_hw_ptt_pending = true;
                s_ptt_pending_touch = false;
            }
            /* RELEASING：正在拆会话（几十 ms），忽略；松手重按即可 */
        } else if (evt == BTN_EVT_PRESS_UP || evt == BTN_EVT_PRESS_END) {
            s_hw_ptt_pending = false;
            s_ptt_pending_touch = false;
            if (s_hw_ptt_ui) {
                xiaozhi_service_ptt_stop();
                s_hw_ptt_ui = false;
            }
        }
    }
}
