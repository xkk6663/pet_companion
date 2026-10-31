/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_action.c
 * @brief 动作下发 / 结果上报实现（见 ai_action.h 的来龙去脉）
 *
 * ★★ v2（2026-10-31 改造）：工具表的**真值从手写数组换成 MCP 引擎的 tools/list**。
 *
 *   改造前：本文件手写 5 条，其余 10 个真实存在的 MCP 工具（台灯 4 / 音量 2 /
 *   音乐 4 / 相机 2 / Wi-Fi / 重启）对云端**完全不可见** —— 大肥鱼只知道自己
 *   有 5 只手，"开个灯""关掉音乐"她根本不知道能办。
 *
 *   改造后：
 *     ① 上报：连上 WS 就向本机 MCP 引擎发 `tools/list`，把**引擎里真实注册的
 *        工具**整表转成 caps 上报（名字/说明/参数 schema 全由 ai_*.c 自己声明）
 *        ⇒ 再加一个 MCP 工具只改 ai_*.c，**本文件与云端零改动**；
 *     ② 执行：没有特殊需求的工具直接 `tools/call` 走引擎（参数校验、类型转换、
 *        属性列表构造全由 SDK 负责），不再复制一份 strcmp 分发。
 *
 *   仍保留手写条目的只有 5 个 —— 它们要么有 MCP 里没有的本地能力
 *   （wallpaper），要么回调的返回值不能直接念给用户（详见 TOOLS[] 注释）。
 *   这 5 条 `run != NULL` = 直调，其余一律走引擎。
 *
 * ★ 为什么引擎能这样用：`esp_mcp_handle_message()` 是引擎的 JSON-RPC 入口，
 *   谁调用它就以谁的身份处理一条消息（没有传输层也完全够用）。我们给它喂
 *   initialize → notifications/initialized → tools/list / tools/call，等价于
 *   一个"住在进程内的 MCP 客户端"。15 个工具的 callback 一行都不用改。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dirent.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include "esp_mcp_engine.h"

#include "ai_action.h"
#include "ai_punch.h"
#include "ai_music.h"
#include "ai_speaker.h"
#include "ai_camera.h"
#include "xiaozhi_service.h"
#include "ws_client.h"
#include "middleware/camera/capture_service.h"
#include "middleware/media/wallpaper_service.h"

static const char *TAG = "AI_ACT";

#define ACT_Q_DEPTH     4
#define ACT_NAME_MAX    24
#define ACT_ARGS_MAX    768       /* 单个动作参数 JSON 上限（含网关塞的 __text 原话） */
#define ACT_DATA_MAX    384       /* 结论文本（music.list 这类可能是一串歌名） */
/* ★ 栈必须与"原来跑 MCP 回调的那条任务"同档：tools/call 的回调（尤其
 *   punch_session → 摄像头 + 人脸识别 + littlefs 写流水）是**同步**跑在
 *   本任务上的，4 KB 会溢出。原来它跑在 ws_client 的 8 KB 收包任务里。
 *   这里取 7168（比 8 KB 省 1 KB）+ 每次动作后打印水位，实测不够再抬。 */
#define ACT_STACK       7168
#define ACT_STACK_MIN   4096      /* 建不起来时的退路（少了 MCP 直通也要能打卡） */
#define ACT_PRIO        4

#define ACT_PUNCH_MS    45000     /* 打卡含摄像头启动 + 人脸识别，实测 < 35 s */
#define ACT_CAPTURE_MS  20000
#define ACT_MUSIC_MS    12000     /* 云搜 + 判活 + 起播；不吃满就早失败 */
#define ACT_VOLUME_MS   4000
#define ACT_DEF_MS      20000     /* 非手写工具的默认等待上限 */

#define ACT_CAPS_MAGIC  "__caps"  /* 队列哨兵：让 worker 去发能力表 */
#define ACT_CAPS_BUF    12288     /* 16 条工具含长说明，实测约 8 KB */
#define ACT_ESC_BUF     2048
#define ACT_PARAMS_BUF  640
#define ACT_MODEL_NAME  "ESP32-S31-Korvo-1"

#define ACT_PROTO       "2025-03-26"   /* == 引擎 MCP_PROTO_DEFAULT */
#define ACT_TAB_MAX     24

typedef struct {
    int   id;
    char  name[ACT_NAME_MAX];
    char *args;      /* PSRAM：args 原文可能上百字节，放队列里要占内部 RAM
                      * （xQueueCreate 只从内部 RAM 拿）。NULL = 无参数。 */
} act_req_t;

static QueueHandle_t s_q;

/* ============================================================
 *  小工具
 * ============================================================ */

/* 有界拷贝：比 snprintf("%s") 安全 —— 源串长度编译期未知时 snprintf 会被
 * -Wformat-truncation 判死（本工程 -Werror），而这里截断是明确的。
 * ★ 截断点回退到 UTF-8 字符边界：半个汉字的字节流发给网关，Python 侧
 *   websockets 按严格 UTF-8 解码会直接判连接异常（表现为"莫名其妙断线"）。 */
static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    size_t l = strlen(src);
    if (l >= cap) {
        l = cap - 1;
        while (l > 0 && ((unsigned char)src[l] & 0xC0) == 0x80) {
            l--;
        }
    }
    memcpy(dst, src, l);
    dst[l] = '\0';
}

/* 截到 UTF-8 字符边界（desc 超长时别把汉字劈成两半：云端 json.loads 会炸） */
static size_t utf8_safe_len(const char *s, size_t max)
{
    if (s == NULL) {
        return 0;
    }
    size_t l = strlen(s);
    if (l <= max) {
        return l;
    }
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) {
        max--;
    }
    return max;
}

static void json_esc_n(const char *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (size_t i = 0; i < len && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;              /* UTF-8 原样透传 */
        }
    }
    out[o] = '\0';
}

static void json_esc(const char *in, char *out, size_t cap)
{
    const char *s = (in != NULL) ? in : "";
    json_esc_n(s, strlen(s), out, cap);
}

static void send_result(int id, const char *name, bool ok, const char *data)
{
    char esc[ACT_DATA_MAX * 2 + 8];
    json_esc(data, esc, sizeof(esc));

    char line[ACT_DATA_MAX * 2 + 160];
    snprintf(line, sizeof(line),
             "{\"type\":\"event\",\"event\":\"action_result\",\"id\":%d,"
             "\"name\":\"%s\",\"ok\":%d,\"data\":\"%s\"}",
             id, name, ok ? 1 : 0, esc);

    esp_err_t e = ws_client_send_text(line);
    ESP_LOGI(TAG, "result %s id=%d ok=%d -> %s (%.80s)",
             name, id, ok ? 1 : 0, esp_err_to_name(e), esc);
}

/* 安全追加：snprintf 返回的是"本应写入的长度"，直接累加 o 会越界 */
static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if (*pos >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) {
        return false;
    }
    *pos += (size_t)n;
    return true;
}

/* ============================================================
 *  MCP 引擎（进程内 JSON-RPC 客户端）
 * ============================================================ */

static bool s_mcp_ready;
static int  s_req_id;                 /* 单调递增：引擎按会话拒重复 id */

/** 发一条 JSON-RPC，返回 result（分离出的 cJSON，调用方 delete）；失败 NULL */
static cJSON *mcp_call(const char *req, char *err, size_t errcap)
{
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    esp_mcp_t *m = (esp_mcp_t *)xiaozhi_service_mcp_engine();
    if (m == NULL) {
        copy_str(err, errcap, "mcp engine not created");
        return NULL;
    }

    uint8_t *out = NULL;
    uint16_t outlen = 0;
    esp_err_t e = esp_mcp_handle_message(m, (const uint8_t *)req,
                                         (uint16_t)strlen(req), &out, &outlen);
    if (out == NULL) {
        copy_str(err, errcap, esp_err_to_name(e));
        return NULL;
    }

    cJSON *doc = cJSON_Parse((const char *)out);
    esp_mcp_free_response(m, out);
    if (doc == NULL) {
        copy_str(err, errcap, "bad response json");
        return NULL;
    }
    cJSON *ej = cJSON_GetObjectItemCaseSensitive(doc, "error");
    if (ej != NULL) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(ej, "message");
        copy_str(err, errcap, cJSON_IsString(msg) ? msg->valuestring : "jsonrpc error");
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON *res = cJSON_DetachItemFromObjectCaseSensitive(doc, "result");
    cJSON_Delete(doc);
    return res;
}

/**
 * 握手（幂等）。
 * ★★ 必须两步：`initialize` 之后还要 `notifications/initialized`。
 *   协议版本比 2024-11-05 新的引擎有生命周期闸门（engine.c:4014），
 *   少了那条通知，后面所有 tools 请求一律回 "client not initialized"。
 */
static bool mcp_ensure_ready(void)
{
    if (s_mcp_ready) {
        return true;
    }
    char req[320];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\",\"params\":{"
             "\"protocolVersion\":\"" ACT_PROTO "\",\"capabilities\":{},"
             "\"clientInfo\":{\"name\":\"esp32-pet-action\",\"version\":\"2.0.0\"}}}",
             ++s_req_id);

    char err[80];
    cJSON *r = mcp_call(req, err, sizeof(err));
    if (r == NULL) {
        ESP_LOGE(TAG, "mcp initialize failed: %s", err);
        return false;
    }
    cJSON_Delete(r);

    (void)mcp_call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
                   NULL, 0);
    s_mcp_ready = true;
    ESP_LOGI(TAG, "mcp engine ready (protocol " ACT_PROTO ")");
    return true;
}

/** 引擎自述的工具清单（result.tools[]）；失败 NULL */
static cJSON *mcp_tools_list(void)
{
    char req[128];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/list\"}", ++s_req_id);
    char err[80];
    cJSON *res = mcp_call(req, err, sizeof(err));
    if (res == NULL) {
        ESP_LOGE(TAG, "tools/list failed: %s", err);
    }
    return res;
}

/* ============================================================
 *  动作表缓存（PSRAM）
 *
 *  caps 每次重连都会重算，这里同时留下"动作名 ↔ MCP 工具名 ↔ 参数模板"。
 *  模板的意义：引擎的 tools/call **要求声明的属性一个都不能少**
 *  （engine.c:1795 缺参直接判失败，不吃 default），所以调用前得先用
 *  schema 里的 default 补齐。整块 malloc 到 PSRAM，不占内部 RAM。
 * ============================================================ */

typedef struct {
    char  act[ACT_NAME_MAX];
    char  mcp[40];
    char  arg_key[16];
    char  arg_mode[8];
    char *tpl;                 /* arguments 模板 JSON（cJSON_Print，PSRAM） */
} act_ent_t;

typedef struct {
    int       n;
    act_ent_t e[ACT_TAB_MAX];
} act_tab_t;

static act_tab_t *s_tab;

static void tab_reset(void)
{
    if (s_tab == NULL) {
        /* ★★ 必须 calloc：下面 for 循环以 s_tab->n 为界去 free 每条 tpl。
         *   用 malloc 的话首次进来 n 是堆里的垃圾值，循环会拿未初始化的
         *   tpl 指针去 free ⇒ 直接撞
         *   `assert failed: heap_caps_free (free() target pointer is outside heap areas)`，
         *   而且必然发生在"连上 WS 就上报 caps"那一刻 —— 见 F62。 */
        s_tab = calloc(1, sizeof(*s_tab));
        if (s_tab == NULL) {
            ESP_LOGE(TAG, "tab alloc failed");
            return;
        }
    }
    for (int i = 0; i < s_tab->n && i < ACT_TAB_MAX; i++) {
        if (s_tab->e[i].tpl != NULL) {
            cJSON_free(s_tab->e[i].tpl);
            s_tab->e[i].tpl = NULL;
        }
    }
    s_tab->n = 0;
}

static act_ent_t *tab_add(const char *act, const char *mcp, const char *arg_key,
                          const char *arg_mode, char *tpl)
{
    if (s_tab == NULL || s_tab->n >= ACT_TAB_MAX) {
        if (tpl != NULL) {
            cJSON_free(tpl);
        }
        return NULL;
    }
    act_ent_t *e = &s_tab->e[s_tab->n++];
    memset(e, 0, sizeof(*e));
    copy_str(e->act, sizeof(e->act), act);
    copy_str(e->mcp, sizeof(e->mcp), mcp != NULL ? mcp : "");
    copy_str(e->arg_key, sizeof(e->arg_key), arg_key != NULL ? arg_key : "");
    copy_str(e->arg_mode, sizeof(e->arg_mode), arg_mode != NULL ? arg_mode : "none");
    e->tpl = tpl;
    return e;
}

static const act_ent_t *tab_find(const char *act)
{
    if (s_tab == NULL || act == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_tab->n; i++) {
        if (strcmp(s_tab->e[i].act, act) == 0) {
            return &s_tab->e[i];
        }
    }
    return NULL;
}

/** "self.light.set_power" → "light_set_power"（云端函数名不能带点） */
static void act_from_mcp(const char *mcp, char *out, size_t cap)
{
    const char *p = mcp != NULL ? mcp : "";
    if (strncmp(p, "self.", 5) == 0) {
        p += 5;
    }
    size_t o = 0;
    for (; *p != '\0' && o + 1 < cap; p++) {
        out[o++] = (*p == '.') ? '_' : *p;
    }
    out[o] = '\0';
}

/* ============================================================
 *  各动作（手写 5 条：MCP 里没有，或回调结论不适配"念给用户听"）
 * ============================================================ */

/* 从 args JSON 里按候选键取文本/数字 */
static bool args_text(const char *args_json, const char *k1, const char *k2,
                      char *out, size_t cap)
{
    if (cap > 0) {
        out[0] = '\0';
    }
    if (args_json == NULL || args_json[0] != '{') {
        return false;
    }
    cJSON *a = cJSON_Parse(args_json);
    if (a == NULL) {
        return false;
    }
    const cJSON *j = k1 ? cJSON_GetObjectItemCaseSensitive(a, k1) : NULL;
    if (j == NULL && k2 != NULL) {
        j = cJSON_GetObjectItemCaseSensitive(a, k2);
    }
    bool got = false;
    if (cJSON_IsString(j) && j->valuestring != NULL) {
        copy_str(out, cap, j->valuestring);
        got = true;
    } else if (cJSON_IsNumber(j)) {
        snprintf(out, cap, "%d", j->valueint);
        got = true;
    }
    cJSON_Delete(a);
    return got;
}

/* ai_punch 的结论文案是 MCP 时代写给云端模型看的英文（"Punched: xxx (match
 * score 0.91). Attendance photo saved."）。现在这句话要被**中文 TTS 念出来**，
 * 原样回传就是"章盖上了 —— Punched: 张三"，又长又怪。这里按前缀翻成人话；
 * 英文原文仍进串口日志，排查不受影响。 */
static bool punch_to_zh(const char *r, char *data, size_t cap)
{
    if (r == NULL || r[0] == '\0') {
        copy_str(data, cap, "打卡结束了，但没拿到结论");
        return false;
    }
    if (strncmp(r, "Punched:", 8) == 0) {
        const char *who = r + 9;
        const char *sp = strchr(who, ' ');
        int n = sp ? (int)(sp - who) : (int)strlen(who);
        if (n > 24) {
            n = (int)utf8_safe_len(who, 24);   /* 别把汉字劈两半（网关严格 UTF-8） */
        }
        snprintf(data, cap, "「%.*s」打卡成功，照片已存", n, who);
        return true;
    }
    if (strncmp(r, "Already punched", 15) == 0) {
        copy_str(data, cap, "今天已经打过卡了");
    } else if (strstr(r, "Face not in the face database")) {
        copy_str(data, cap, "这张脸不在人脸库里，先去相机页录一张");
    } else if (strstr(r, "No face recognized in time")) {
        copy_str(data, cap, "没看清脸，正对摄像头再来一次");
    } else if (strstr(r, "Face engine failed to load")) {
        copy_str(data, cap, "人脸模型没加载起来，关掉相机页再试");
    } else if (strstr(r, "Face engine load timed out")) {
        copy_str(data, cap, "人脸模型加载超时，再试一次");
    } else if (strstr(r, "Failed to write the punch record")) {
        copy_str(data, cap, "打卡记录写不进去，检查一下 SD 卡");
    } else if (strstr(r, "No SD card")) {
        copy_str(data, cap, "SD 卡没插，打卡存不了");
    } else if (strstr(r, "Face database not ready")) {
        copy_str(data, cap, "人脸库还没就绪，等一会儿");
    } else if (strstr(r, "Punch service busy")) {
        copy_str(data, cap, "上一次打卡还在收尾，等一下");
    } else if (strstr(r, "Recognition engine is busy")) {
        copy_str(data, cap, "识别引擎正忙，等一下");
    } else if (strstr(r, "Out of memory")) {
        copy_str(data, cap, "内存不够，关掉相机页再试");
    } else if (strstr(r, "Failed to start camera")) {
        copy_str(data, cap, "摄像头没起来，关掉相机页再试");
    } else {
        copy_str(data, cap, r);              /* 没见过的文案：原样透传 */
    }
    return false;
}

static bool do_punch(const char *args, char *data, size_t cap)
{
    (void)args;                              /* 打卡无参数 */
    if (ai_punch_trigger() != ESP_OK) {
        copy_str(data, cap, "设备正忙（打卡或相机被占用）");
        return false;
    }
    /* 轮询而不是回调：本任务在等，回调也会跑到别人的上下文里，两条路都留
     * 反而容易重入（report 已经往 UI 推了一条 XZ_EVT_TEXT）。 */
    for (int waited = 0; ai_punch_busy() && waited < ACT_PUNCH_MS; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ai_punch_busy()) {
        copy_str(data, cap, "打卡超时没结束");
        return false;
    }
    const char *r = ai_punch_last_result();
    ESP_LOGI(TAG, "punch raw: %.140s", r ? r : "");
    return punch_to_zh(r, data, cap);
}

static bool do_capture(const char *args, char *data, size_t cap)
{
    (void)args;                              /* 拍照无参数 */
    if (capture_service_request() != ESP_OK) {
        copy_str(data, cap, "拍照被拒（相机被页面占用或 SD 没挂载）");
        return false;
    }
    for (int waited = 0;
         capture_service_state() == CAPTURE_BUSY && waited < ACT_CAPTURE_MS;
         waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (capture_service_state() == CAPTURE_BUSY) {
        copy_str(data, cap, "拍照超时");
        return false;
    }

    capture_result_t r;
    memset(&r, 0, sizeof(r));
    capture_service_get_result(&r);
    if (r.err != ESP_OK) {
        snprintf(data, cap, "拍照失败: %s", esp_err_to_name(r.err));
        return false;
    }

    const char *base = strrchr(r.path, '/');
    ESP_LOGI(TAG, "capture -> %s (%u B, %u ms)",
             base ? base + 1 : r.path, (unsigned)r.bytes, (unsigned)r.elapsed_ms);
    /* F69：动作快速路也要出浮窗预览 —— 预览任务原本只挂在 MCP 工具
     * cb_capture 里，这里（云端 action）拍完从不发预览（实测浮窗没图）。
     * 异步回读已落盘文件解码发布，不影响下面的上报节奏。 */
    ai_camera_preview_file(r.path);
    /* data 会被 TTS 念出来 ⇒ 只给"多大"，文件名进日志就够 */
    snprintf(data, cap, "存好了（%u KB）", (unsigned)((r.bytes + 1023) / 1024));
    capture_service_clear_result();
    return true;
}

/* 点歌要有"声音通道"（s_play / s_spk 是会话 setup 时开的）。
 * 用户点 chip 时可能还没按过录音键（会话 IDLE）⇒ 这里补一次懒建会话。
 * ★ 不花 token：建会话只是 claim codec + 监听，上行一个字都没发。 */
static bool ensure_audio_path(void)
{
    if (xiaozhi_service_get_state() == XZ_STATE_CHATTING) {
        return true;
    }
    if (xiaozhi_service_start() != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 120; i++) {          /* ≤ 12 s */
        if (xiaozhi_service_get_state() == XZ_STATE_CHATTING) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool do_music(const char *args, char *data, size_t cap)
{
    char song[64];
    if (!args_text(args, "name", "song", song, sizeof(song)) || song[0] == '\0') {
        copy_str(data, cap, "没给歌名");
        return false;
    }
    if (!ensure_audio_path()) {
        copy_str(data, cap, "声音通道没起来，按一下录音键再点歌");
        return false;
    }
    /* ★ 等 kicker 的真结论：受理即成功会把"搜不到歌"报成"放上了"（F59） */
    esp_err_t e = ai_music_play_wait(song, data, cap, ACT_MUSIC_MS);
    if (e != ESP_OK) {
        return false;
    }
    copy_str(data, cap, song);
    return true;
}

static bool do_volume(const char *args, char *data, size_t cap)
{
    int cur = ai_speaker_get_volume();
    int v = -1;

    /* ① 先找数字：LLM 走 function calling 时会直接给 value/volume；快速路
     *    把余下文本塞在 "name" 里（"80"）。两条都在同一段 JSON 文本里。 */
    for (const char *p = args; p != NULL && *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            v = atoi(p);
            break;
        }
    }
    /* ② 没数字就看口语：大/高/小/低/静音，按当前值相对调整 */
    if (v < 0 && args != NULL) {
        if (strstr(args, "静音") || strstr(args, "关掉声音") || strstr(args, "闭嘴")) {
            v = 0;
        } else if (strstr(args, "大") || strstr(args, "高")) {
            v = cur + 15;
        } else if (strstr(args, "小") || strstr(args, "低")) {
            v = cur - 15;
        }
    }
    if (v < 0) {
        snprintf(data, cap, "要说清调到多少（0-90），现在是 %d", cur);
        return false;
    }

    int now = ai_speaker_set_volume(v);
    snprintf(data, cap, "%d", now);
    return true;
}

static bool do_wallpaper(const char *args, char *data, size_t cap)
{
    (void)args;                              /* 壁纸无参数（池内随机） */
    char cur[80];
    wallpaper_service_get_current_name(cur, sizeof(cur));

    /* 池内随机挑一张（排除当前）。最多扫 64 个候选就够，池子不会更大。 */
    char cand[64][64];
    int n = 0;
    DIR *d = opendir(WALLPAPER_POOL_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && n < 64) {
            const char *nm = ent->d_name;
            const char *ext = strrchr(nm, '.');
            if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
                continue;
            }
            if (strcmp(nm, cur) == 0) {
                continue;
            }
            /* ★ 用 memcpy 而不是 snprintf：d_name 最长 255，snprintf 会被
             *   -Wformat-truncation 判死（本工程 -Werror），而且截断的名字
             *   交给 wallpaper_service 也必然校验失败。 */
            size_t ln = strlen(nm);
            if (ln >= sizeof(cand[0])) {
                continue;
            }
            memcpy(cand[n], nm, ln + 1);
            n++;
        }
        closedir(d);
    }
    if (n == 0) {
        copy_str(data, cap, "壁纸池是空的");
        return false;
    }

    const char *pick = cand[esp_random() % (uint32_t)n];
    esp_err_t e = wallpaper_service_request_set(pick);
    if (e != ESP_OK) {
        snprintf(data, cap, "换壁纸失败: %s", esp_err_to_name(e));
        return false;
    }
    copy_str(data, cap, pick);
    return true;
}

/* ============================================================
 *  手写表（只留给"引擎给不了"的 5 个）
 *
 *  `mcp`  = 引擎里对应的工具名；NULL = 纯本地动作。
 *  `run`  = 直调实现；NULL = 走引擎 tools/call。
 *  `desc` = 覆盖引擎里的 description（给模型的"什么时候该调"提示，
 *           引擎那版是写给 MCP 客户端看的，缺少"别编造"之类的纪律）。
 *  `params` = 覆盖参数说明（引擎那两条属性没写 description）。
 * ============================================================ */

typedef bool (*act_run_fn)(const char *args_json, char *out, size_t cap);

typedef struct {
    const char *act;        /* 上报/下发用的动作名（云端函数名 = pet_ + 它） */
    const char *mcp;        /* MCP 工具名；NULL = 不进 tools/list */
    const char *label;      /* UI chip 短标签；NULL = 不出 chip */
    bool        chip;
    const char *aliases;    /* '|' 分隔的口语说法（网关快速路）；NULL=不走 */
    const char *arg_mode;   /* "none"=命中即触发；"tail"=余下文本当参数 */
    const char *arg_key;    /* tail 文本塞进哪个参数名 */
    const char *desc;       /* 覆盖 description；NULL = 用引擎的 */
    const char *params;     /* 覆盖参数 schema；NULL = 从 inputSchema 生成 */
    act_run_fn  run;        /* NULL = 走引擎 tools/call */
    uint32_t    timeout_ms;
} ai_tool_t;

static const ai_tool_t TOOLS[] = {
    {
        .act = "punch", .mcp = "self.punch.clock_in",
        .label = "打卡", .chip = true,
        .aliases = "打卡|打个卡|签到|签个到|上班打卡|考勤|我到了",
        .arg_mode = "none",
        .desc = "在桌宠设备上打卡签到。设备会开摄像头做人脸识别，成功后记一条"
                "考勤并把打卡照片存进 SD 卡。用户说打个卡、签到、上班打卡、我到了时调用。"
                "失败时照实说明原因，不要编造已经打上。",
        .run = do_punch,
        .timeout_ms = ACT_PUNCH_MS,
    },
    {
        .act = "capture", .mcp = "self.camera.capture",
        .label = "拍照", .chip = true,
        .aliases = "拍照|拍个照|拍张照|拍张照片|拍一张|照相|自拍|给我拍一张",
        .arg_mode = "none",
        .desc = "让桌宠设备拍一张照片，存进设备自己的 SD 卡，不上传云端。"
                "用户说拍张照、自拍、给我拍一张、看看现在什么样时调用。"
                "如果用户是想让你\"看看\"眼前有什么、要你描述画面，改用 camera_explain。",
        .run = do_capture,
        .timeout_ms = ACT_CAPTURE_MS,
    },
    {
        /* ★ 保留直调：引擎里的 cb_play 是"受理即回"，搜不到歌也会报成功
         *   （F59 踩过）；这里必须等 kicker 的真结论。 */
        .act = "music", .mcp = "self.music.play",
        .label = "点歌", .chip = true,
        .aliases = "点歌|点一首|点首|点个歌|播放|放首歌|放一首|放首|来一首|来首|唱一首|唱首|我想听|听首歌",
        .arg_mode = "tail", .arg_key = "name",
        .desc = "让桌宠设备播放一首歌，从设备喇叭出声。用户说点歌、放首歌、来一首、"
                "我想听某首歌时调用。起播成功后别再长篇说话，会盖住音乐。",
        .params = "[{\"name\":\"name\",\"type\":\"string\","
                  "\"description\":\"用户原话里的歌名，原样使用，不要改写或翻译\"}]",
        .run = do_music,
        .timeout_ms = ACT_MUSIC_MS,
    },
    {
        /* 引擎里没有对应工具（壁纸池是本机概念），纯本地动作。 */
        .act = "wallpaper", .mcp = NULL,
        .label = "换壁纸", .chip = true,
        .aliases = "换壁纸|换个壁纸|换张壁纸|换一张壁纸|换个背景|换张图",
        .arg_mode = "none",
        .desc = "给桌宠换一张壁纸，从设备本地壁纸池里随机选一张，不联网下载。"
                "用户说换壁纸、换个背景、换张图时调用。",
        .run = do_wallpaper,
        .timeout_ms = 8000,
    },
    {
        /* ★ 保留直调：引擎那条要求整数 volume（0~100），而用户口语
         *   "大点声"没有绝对值，得按当前值相对调整（+15/-15/静音）。 */
        .act = "volume", .mcp = "self.speaker.set_volume",
        .label = "音量", .chip = true,
        .aliases = "音量调到|音量调成|音量设为|调音量|调到|音量|大点声|小点声|大声点|小声点|声音大点|声音小点|静音|调大|调小",
        .arg_mode = "tail", .arg_key = "volume",
        .desc = "设置桌宠设备的扬声器音量（0-90，0 静音）。用户说大声点/小声点/"
                "调音量/静音，或给出具体数值时调用。参数取用户原话里的数字；"
                "只说\"大点\"\"小点\"时按相对调整，不要自己编绝对值。"
                "要查询当前音量用 speaker_get_volume。",
        .params = "[{\"name\":\"value\",\"type\":\"integer\","
                  "\"description\":\"目标音量 0-90；0 是静音\"}]",
        .run = do_volume,
        .timeout_ms = ACT_VOLUME_MS,
    },
    {
        /* 只是改等待上限：引擎里 cb_explain 会**同步等 40 s**（照片上传 +
         * 视觉回答异步回来），用默认 20 s 会先在云端判超时。说明与参数都用
         * 引擎自述的那份（desc/params 留空 = 不覆盖）。 */
        .act = "camera_explain", .mcp = "self.camera.explain",
        .timeout_ms = 45000,
    },
};

#define ACT_NTOOLS  (sizeof(TOOLS) / sizeof(TOOLS[0]))

static const ai_tool_t *overlay_by_act(const char *act)
{
    if (act == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ACT_NTOOLS; i++) {
        if (TOOLS[i].act != NULL && strcmp(TOOLS[i].act, act) == 0) {
            return &TOOLS[i];
        }
    }
    return NULL;
}

static const ai_tool_t *overlay_by_mcp(const char *mcp)
{
    if (mcp == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ACT_NTOOLS; i++) {
        if (TOOLS[i].mcp != NULL && strcmp(TOOLS[i].mcp, mcp) == 0) {
            return &TOOLS[i];
        }
    }
    return NULL;
}

/* ---- 供 UI 取"这台设备有哪些快捷动作"（chips 的唯一真相） ----
 * ★ 只列 chip==true 的：屏幕只有 800 px，16 个 chip 排不下，而且台灯/
 *   Wi-Fi 这类本来就该用嘴说。 */
int ai_action_tool_count(void)
{
    int n = 0;
    for (size_t i = 0; i < ACT_NTOOLS; i++) {
        if (TOOLS[i].chip) {
            n++;
        }
    }
    return n;
}

const char *ai_action_tool_label(int idx)
{
    if (idx < 0) {
        return NULL;
    }
    for (size_t i = 0; i < ACT_NTOOLS; i++) {
        if (!TOOLS[i].chip) {
            continue;
        }
        if (idx-- == 0) {
            return TOOLS[i].label;
        }
    }
    return NULL;
}

/* ============================================================
 *  能力表上报（真值 = 引擎 tools/list）
 * ============================================================ */

/* 从 inputSchema.properties 生成云端要的扁平参数表
 * [{name,type,description}]（= AstrBot add_func 的 func_args 格式） */
static void params_from_schema(const cJSON *schema, char *out, size_t cap)
{
    char tmp[ACT_PARAMS_BUF];
    copy_str(tmp, sizeof(tmp), "[]");

    const cJSON *props = schema != NULL
                         ? cJSON_GetObjectItemCaseSensitive(schema, "properties") : NULL;
    if (cJSON_IsObject(props)) {
        cJSON *a = cJSON_CreateArray();
        if (a != NULL) {
            const cJSON *p = NULL;
            cJSON_ArrayForEach(p, props) {
                cJSON *it = cJSON_CreateObject();
                if (it == NULL) {
                    break;
                }
                cJSON_AddStringToObject(it, "name", p->string != NULL ? p->string : "");
                const cJSON *ty = cJSON_GetObjectItemCaseSensitive(p, "type");
                cJSON_AddStringToObject(it, "type",
                                        cJSON_IsString(ty) ? ty->valuestring : "string");
                const cJSON *ds = cJSON_GetObjectItemCaseSensitive(p, "description");
                if (cJSON_IsString(ds) && ds->valuestring != NULL) {
                    cJSON_AddStringToObject(it, "description", ds->valuestring);
                }
                cJSON_AddItemToArray(a, it);
            }
            char *s = cJSON_PrintUnformatted(a);
            if (s != NULL) {
                copy_str(tmp, sizeof(tmp), s);
                cJSON_free(s);
            }
            cJSON_Delete(a);
        }
    }
    copy_str(out, cap, tmp);
}

/* 从 inputSchema 造 arguments 模板（每个属性都要出现，缺一个 tools/call
 * 就会被引擎判失败 —— 见 engine.c:1795，它不吃 default）。 */
static char *build_tpl(const cJSON *schema)
{
    cJSON *tpl = cJSON_CreateObject();
    if (tpl == NULL) {
        return NULL;
    }
    const cJSON *props = schema != NULL
                         ? cJSON_GetObjectItemCaseSensitive(schema, "properties") : NULL;
    if (cJSON_IsObject(props)) {
        const cJSON *p = NULL;
        cJSON_ArrayForEach(p, props) {
            if (p->string == NULL) {
                continue;
            }
            cJSON *v = NULL;
            const cJSON *def = cJSON_GetObjectItemCaseSensitive(p, "default");
            if (def != NULL) {
                v = cJSON_Duplicate(def, 1);
            } else {
                const cJSON *ty = cJSON_GetObjectItemCaseSensitive(p, "type");
                const char *t = cJSON_IsString(ty) ? ty->valuestring : "string";
                if (strcmp(t, "string") == 0) {
                    v = cJSON_CreateString("");
                } else if (strcmp(t, "boolean") == 0) {
                    v = cJSON_CreateBool(false);
                } else if (strcmp(t, "array") == 0) {
                    v = cJSON_CreateArray();
                } else if (strcmp(t, "object") == 0) {
                    v = cJSON_CreateObject();
                } else {
                    v = cJSON_CreateNumber(0);
                }
            }
            if (v != NULL) {
                cJSON_AddItemToObject(tpl, p->string, v);
            }
        }
    }
    char *s = cJSON_PrintUnformatted(tpl);
    cJSON_Delete(tpl);
    return s;                     /* malloc ⇒ PSRAM（ALWAYSINTERNAL=0） */
}

/* caps 组装用的两块大缓冲：放 **PSRAM**，不放栈。
 * ★ action_task 只有 7 KB 栈，而 emit_one 一次就要 2048(esc) + 640(params)，
 *   16 条工具串行调用时栈峰值会顶到钳子上（F63）。worker 单线程串行，
 *   模块级指针安全；首次用到才分配，之后常驻复用。 */
typedef struct {
    char esc[ACT_ESC_BUF];
    char params[ACT_PARAMS_BUF];
} caps_wk_t;

static caps_wk_t *s_caps_wk;

static bool caps_wk_ready(void)
{
    if (s_caps_wk == NULL) {
        s_caps_wk = heap_caps_malloc(sizeof(*s_caps_wk),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_caps_wk == NULL) {
            ESP_LOGE(TAG, "caps workspace alloc failed (%u B)",
                     (unsigned)sizeof(*s_caps_wk));
        }
    }
    return s_caps_wk != NULL;
}

/* 上报一条 + 记进动作表。tpl 所有权转移。 */
static bool emit_one(char *buf, size_t cap, size_t *o, int *n,
                     const ai_tool_t *ov, const char *act, const char *mcp,
                     const char *desc, const cJSON *schema, char *tpl)
{
    if (!caps_wk_ready()) {
        if (tpl != NULL) {
            cJSON_free(tpl);
        }
        return false;
    }
    char *esc    = s_caps_wk->esc;
    char *params = s_caps_wk->params;

    const char *label   = (ov != NULL && ov->label) ? ov->label : "";
    const char *aliases = (ov != NULL && ov->aliases) ? ov->aliases : "";
    const char *mode    = (ov != NULL && ov->arg_mode) ? ov->arg_mode : "none";
    const char *key     = (ov != NULL && ov->arg_key) ? ov->arg_key : "";
    uint32_t    ms      = (ov != NULL && ov->timeout_ms) ? ov->timeout_ms : ACT_DEF_MS;
    const char *d       = desc != NULL ? desc : "";

    json_esc_n(d, utf8_safe_len(d, ACT_ESC_BUF - 8), esc, ACT_ESC_BUF);

    if (ov != NULL && ov->params != NULL) {
        copy_str(params, ACT_PARAMS_BUF, ov->params);
    } else {
        params_from_schema(schema, params, ACT_PARAMS_BUF);
    }

    bool ok = append(buf, cap, o,
                     "%s{\"name\":\"%s\",\"label\":\"%s\",\"aliases\":\"%s\","
                     "\"arg_mode\":\"%s\",\"arg_key\":\"%s\",\"desc\":\"%s\","
                     "\"params\":%s,\"timeout_ms\":%lu}",
                     (*n) ? "," : "", act, label, aliases, mode, key, esc,
                     params, (unsigned long)ms);
    if (!ok) {
        ESP_LOGE(TAG, "caps: buffer %d too small at '%s'", ACT_CAPS_BUF, act);
        if (tpl != NULL) {
            cJSON_free(tpl);
        }
        return false;
    }
    tab_add(act, mcp, key, mode, tpl);
    (*n)++;
    return true;
}

/**
 * 整表上报。malloc 走 PSRAM（ALWAYSINTERNAL=0），不占内部 RAM 余量。
 * ★ 两条来源合并：① 引擎 tools/list（真值）；② 手写表里没有 MCP 对应物的
 *   纯本地动作（wallpaper）。引擎挂掉时退化成"只报手写表"，不至于零工具。
 */
static void send_caps(void)
{
    char *buf = malloc(ACT_CAPS_BUF);
    if (buf == NULL) {
        ESP_LOGE(TAG, "caps: no mem");
        return;
    }

    size_t o = 0;
    int n = 0;
    bool ok = append(buf, ACT_CAPS_BUF, &o,
                     "{\"type\":\"caps\",\"model\":\"" ACT_MODEL_NAME "\",\"tools\":[");
    if (!ok) {
        ESP_LOGE(TAG, "caps: header overflow");
        free(buf);
        return;
    }

    tab_reset();

    /* ---- ① 引擎自述 ---- */
    int from_mcp = 0;
    bool used[ACT_NTOOLS];        /* 手写条目里哪些已经在 ① 出过 */
    memset(used, 0, sizeof(used));
    cJSON *res = mcp_ensure_ready() ? mcp_tools_list() : NULL;
    const cJSON *arr = res != NULL
                       ? cJSON_GetObjectItemCaseSensitive(res, "tools") : NULL;
    if (cJSON_IsArray(arr)) {
        const cJSON *t = NULL;
        cJSON_ArrayForEach(t, arr) {
            const cJSON *jn = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (!cJSON_IsString(jn) || jn->valuestring == NULL) {
                continue;
            }
            const char *mcp = jn->valuestring;
            const cJSON *jd = cJSON_GetObjectItemCaseSensitive(t, "description");
            const cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
            const ai_tool_t *ov = overlay_by_mcp(mcp);
            if (ov != NULL) {
                used[ov - TOOLS] = true;
            }

            char act[ACT_NAME_MAX];
            if (ov != NULL) {
                copy_str(act, sizeof(act), ov->act);
            } else {
                act_from_mcp(mcp, act, sizeof(act));
            }

            ok = emit_one(buf, ACT_CAPS_BUF, &o, &n, ov, act, mcp,
                          (ov != NULL && ov->desc != NULL) ? ov->desc
                          : (cJSON_IsString(jd) ? jd->valuestring : NULL),
                          schema, build_tpl(schema));
            from_mcp++;
            if (!ok) {
                break;
            }
        }
    }
    cJSON_Delete(res);

    /* ---- ② 剩下没出过的手写条目 ----
     * ① 引擎里没有对应工具（wallpaper 是纯本地概念）；
     * ② 引擎 attach 失败/漏挂时的兜底（否则 caps 会缩水成只剩 wallpaper）。 */
    if (ok) {
        for (size_t i = 0; i < ACT_NTOOLS; i++) {
            if (used[i]) {
                continue;
            }
            char act[ACT_NAME_MAX];
            copy_str(act, sizeof(act), TOOLS[i].act);
            if (!emit_one(buf, ACT_CAPS_BUF, &o, &n, &TOOLS[i], act,
                          TOOLS[i].mcp, TOOLS[i].desc, NULL, NULL)) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        ok = append(buf, ACT_CAPS_BUF, &o, "]}");
    }
    if (ok) {
        esp_err_t e = ws_client_send_text(buf);
        ESP_LOGI(TAG, "caps -> %s (%u B, %d tools, %d from mcp engine)",
                 esp_err_to_name(e), (unsigned)o, n, from_mcp);
    }
    free(buf);
}

/* ============================================================
 *  引擎直通（tools/call）
 * ============================================================ */

/**
 * 走引擎调一个工具。
 * args 的合并顺序：schema 模板 → 网关/模型给的实参（后者覆盖）。
 * 返回值：content[0].text 进 data；isError 或空文本算失败。
 */
static bool mcp_tools_call(const act_ent_t *e, const char *args_json,
                           char *data, size_t cap)
{
    cJSON *tpl = (e->tpl != NULL) ? cJSON_Parse(e->tpl) : NULL;
    if (tpl == NULL || !cJSON_IsObject(tpl)) {
        cJSON_Delete(tpl);
        tpl = cJSON_CreateObject();
    }
    if (tpl == NULL) {
        copy_str(data, cap, "内存不够");
        return false;
    }

    if (args_json != NULL && args_json[0] == '{') {
        cJSON *in = cJSON_Parse(args_json);
        if (cJSON_IsObject(in)) {
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, in) {
                if (it->string == NULL) {
                    continue;
                }
                cJSON *dup = cJSON_Duplicate(it, 1);
                if (dup == NULL) {
                    continue;
                }
                /* 不用 ReplaceItemInObject：新版本 cJSON 找不到键时会把新值
                 * 丢掉，行为随版本变；先删后加是所有版本都成立的写法。 */
                cJSON_DeleteItemFromObjectCaseSensitive(tpl, it->string);
                cJSON_AddItemToObject(tpl, it->string, dup);
            }
        }
        cJSON_Delete(in);
    }

    char *args_str = cJSON_PrintUnformatted(tpl);
    cJSON_Delete(tpl);
    if (args_str == NULL) {
        copy_str(data, cap, "内存不够");
        return false;
    }

    char req[ACT_ARGS_MAX + 320];
    int len = snprintf(req, sizeof(req),
                       "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                       ++s_req_id, e->mcp, args_str);
    cJSON_free(args_str);
    if (len <= 0 || (size_t)len >= sizeof(req)) {
        copy_str(data, cap, "参数太长");
        return false;
    }

    char err[96];
    cJSON *res = mcp_call(req, err, sizeof(err));
    if (res == NULL) {
        ESP_LOGW(TAG, "tools/call %s failed: %s", e->mcp, err);
        snprintf(data, cap, "设备执行失败（%s）", err);
        return false;
    }

    bool ok = false;
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(res, "content");
    const cJSON *ie = cJSON_GetObjectItemCaseSensitive(res, "isError");
    if (cJSON_IsArray(content)) {
        const cJSON *first = cJSON_GetArrayItem(content, 0);
        const cJSON *txt = first != NULL
                           ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;
        if (cJSON_IsString(txt) && txt->valuestring != NULL) {
            copy_str(data, cap, txt->valuestring);
            ok = (data[0] != '\0');
        }
    }
    if (cJSON_IsTrue(ie)) {
        ok = false;
    }
    if (!ok && data[0] == '\0') {
        copy_str(data, cap, "设备没回结论");
    }
    cJSON_Delete(res);
    ESP_LOGI(TAG, "tools/call %s -> ok=%d (%.100s)", e->mcp, ok ? 1 : 0, data);
    return ok;
}

/* ============================================================
 *  worker
 * ============================================================ */

static void action_task(void *arg)
{
    (void)arg;
    act_req_t req;

    for (;;) {
        if (xQueueReceive(s_q, &req, portMAX_DELAY) != pdPASS) {
            continue;
        }

        /* 哨兵：连接建立后上报能力表。复用本任务是为了**零新增栈/任务**
         * （内部 RAM 已经很紧，起一个一次性任务会踩到边上）。 */
        if (strcmp(req.name, ACT_CAPS_MAGIC) == 0) {
            send_caps();
            continue;
        }

        char data[ACT_DATA_MAX];
        data[0] = '\0';
        bool ok = false;

        const ai_tool_t *ov = overlay_by_act(req.name);
        const act_ent_t *e = tab_find(req.name);

        if (ov != NULL && ov->run != NULL) {
            ok = ov->run(req.args, data, sizeof(data));
        } else if (e != NULL && e->mcp[0] != '\0') {
            ok = mcp_tools_call(e, req.args, data, sizeof(data));
        } else if (ov != NULL && ov->mcp != NULL && mcp_ensure_ready()) {
            /* 能力表还没上报过（极少数：刚连上就来了动作）⇒ 现建个临时条目 */
            act_ent_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            copy_str(tmp.act, sizeof(tmp.act), ov->act);
            copy_str(tmp.mcp, sizeof(tmp.mcp), ov->mcp);
            ok = mcp_tools_call(&tmp, req.args, data, sizeof(data));
        } else {
            snprintf(data, sizeof(data), "未知动作: %s", req.name);
        }

        if (req.args != NULL) {
            heap_caps_free(req.args);
            req.args = NULL;
        }

        send_result(req.id, req.name, ok, data);
        ESP_LOGI(TAG, "stack left %u B after '%s'",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), req.name);
    }
}

/* ============================================================
 *  对外
 * ============================================================ */

esp_err_t ai_action_init(void)
{
    static bool s_started = false;
    if (s_started) {
        return ESP_OK;
    }

    s_q = xQueueCreate(ACT_Q_DEPTH, sizeof(act_req_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }
    /* ★ 内部 RAM 栈（不能用 xTaskCreateWithCaps/PSRAM）：本任务会做
     *   opendir/读目录、tools/call 里还可能是 punch 的 littlefs 写流水，
     *   F35 家族铁律。栈吃紧时降级建 4 KB —— 少了 MCP 直通也要能打卡。 */
    if (xTaskCreate(action_task, "ai_action", ACT_STACK, NULL, ACT_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "task %d B failed, retry %d B", ACT_STACK, ACT_STACK_MIN);
        if (xTaskCreate(action_task, "ai_action", ACT_STACK_MIN, NULL, ACT_PRIO, NULL) != pdPASS) {
            vQueueDelete(s_q);
            s_q = NULL;
            ESP_LOGE(TAG, "task create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "ready (queue %d, stack %d internal)", ACT_Q_DEPTH, ACT_STACK);

    /* ★ 时序：ws_client_start() 在本模块初始化**之前**就跑了，CONNECTED
     *   回调早于 ai_action_init ⇒ 那一次 ai_action_report_caps() 会因队列
     *   未建而丢掉，caps 要等到下次重连才发。这里补一枪。 */
    if (xiaozhi_service_link_up()) {
        ESP_LOGI(TAG, "link already up, report caps now");
        ai_action_report_caps();
    }
    return ESP_OK;
}

esp_err_t ai_action_dispatch(const char *name, const char *args_json, int id)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (name == NULL || name[0] == '\0' || strlen(name) >= ACT_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 早失败：两层表里都没有就当场拒，别等 worker 跑一圈再回"未知动作"
     * （云端看到的是明确失败，而不是一个措辞含糊的成功回执）。 */
    if (overlay_by_act(name) == NULL && tab_find(name) == NULL) {
        ESP_LOGW(TAG, "unknown action '%s'（固件能力表里没有）", name);
        return ESP_ERR_INVALID_ARG;
    }

    act_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = id;
    copy_str(req.name, sizeof(req.name), name);

    /* ★ 参数原样存 JSON 原文，不再拍平成字符串：
     *   走引擎时它是 tools/call 的 arguments（结构化的 r/g/b、power 都要保留），
     *   走手写实现时由 args_text()/do_volume() 自己从里面取。
     * ★ 放 PSRAM 而不是塞进队列项：队列是内部 RAM，4 项 × 320 B 会一直占着
     *   ~1.3 KB（内部 RAM 余量本来就只有个位数 KB）。 */
    if (args_json != NULL && args_json[0] == '{') {
        size_t l = strlen(args_json);
        if (l > ACT_ARGS_MAX) {
            /* 整句原话可能很长，但超过 768 B 一定不是正常话术 ⇒ 丢掉比截半好 */
            ESP_LOGW(TAG, "args too long (%u B), dropped", (unsigned)l);
        } else {
            req.args = heap_caps_malloc(l + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (req.args != NULL) {
                memcpy(req.args, args_json, l + 1);
            }
        }
    }

    if (xQueueSend(s_q, &req, 0) != pdPASS) {
        ESP_LOGW(TAG, "queue full, drop action '%s'", req.name);
        if (req.args != NULL) {
            heap_caps_free(req.args);
        }
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "accept '%s' id=%d args=%s", req.name, req.id,
             req.args != NULL ? req.args : "{}");
    return ESP_OK;
}

esp_err_t ai_action_report_caps(void)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    act_req_t req;
    memset(&req, 0, sizeof(req));
    req.id = -1;
    copy_str(req.name, sizeof(req.name), ACT_CAPS_MAGIC);

    /* ★ 非阻塞：本函数在 WS 事件任务（CONNECTED 回调）里被调用，
     *   事件任务里绝不能等。队列满就丢，下次重连会再报。 */
    if (xQueueSend(s_q, &req, 0) != pdPASS) {
        ESP_LOGW(TAG, "queue full, caps dropped（下次重连会重报）");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
