/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_music.c
 * @brief 小智 MCP 在线点歌工具实现（S7-B1 / §73.25 / §73.26 / §73.27 / §73.28）。
 *
 * ★★ §73.27 架构反转：**不再抢占会话**——
 *   旧链路（§73.26）：cb_play 投 xiaozhi_service_stop 拆会话 → kicker 等
 *   IDLE → media_player_service 独占 ext STREAM。代价：点歌必断对话、
 *   播完要重新按 PTT 重开（上下文清零）。
 *   新链路（§73.27）：搜到 URL → xiaozhi_service_music_play(url) 投队列 →
 *   xz_task 调 audio_playback_play(s_play, url)——av_processor 的 URL
 *   播放模块（内部 esp_audio_simple_player，vocat 同款引擎），解码 PCM
 *   汇入 xz_play_write_cb 同一条下行流，与 TTS 共用 s_spk 出声。会话
 *   全程活着，LLM 随时可 stop/换曲，播完接着聊。
 *
 *   media_lib http 适配 + mp3/aac 解码器已由 audio_manager_init 自动注册
 *   （av_processor 内置），点歌通路零额外初始化。
 *
 * ★★ §73.28（S8）：音源从「第三方解析站递归扫 JSON」升级为「自有云代理」。
 *   - 搜索/取链/歌词全部走云端 music_proxy（/v1 契约，见 music_source.h），
 *     拿结构化元数据（id/name/artist/duration_ms）；
 *   - 播放器直接吃 /v1/download URL（README §8.2：代理已处理 302/过期直链）；
 *   - SD 曲库（music_lib）：下载落盘 /sdcard/music/，本地优先播放，断网可听；
 *   - 旧 k_music_apis 递归扫描收进 #if MUSIC_SOURCE_LEGACY 编译开关（默认关，
 *     云端不可用时手动打开作兜底）。
 *
 * 结果回传：搜索/起播失败走 s_result_cb；播完/播失败走 xiaozhi_service
 * 的 xz_music_evt_cb 桥（on_xz_music_evt）→ 统一 XZ_EVT_TEXT 进浮窗。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_mcp_engine.h"

#include "ai_music.h"
#include "xiaozhi_service.h"
#include "music_source.h"
#include "music_lib.h"

static const char *TAG = "AI_MUSIC";

/* URL 最长（https://... 长直链 + 余量；超过这个长度的直链少见，截断保护） */
#define MUSIC_URL_MAX 512
/* 音源搜索超时（服务端 UPSTREAM_TIMEOUT=8，端侧宽裕到 10s） */
#define MUSIC_SEARCH_TIMEOUT_MS 10000
/* 搜索响应体：5 首 ~8-15KB（S8 F4），16KB PSRAM 装得下 */
#define MUSIC_SEARCH_BODY_MAX   (16 * 1024)
/* kicker 任务栈：HTTPS+cJSON 纯网络，F35 允 PSRAM 栈 */
#define MUSIC_KICK_STACK        (12 * 1024)
/* 下载任务栈：纯网络 + SD 写（不碰 flash，F35 允许 PSRAM 栈，README §8.4） */
#define MUSIC_DL_STACK          (10 * 1024)
/* 列表任务栈（扫 SD 目录，很小） */
#define MUSIC_LIST_STACK        (4 * 1024)

#if defined(MUSIC_SOURCE_LEGACY)
/* ============================================================
 * ★ 旧链路（§73.27）：第三方解析站递归扫 JSON 找直链。
 *   默认关闭（S8：云代理为主）。要打开：CMakeLists 注入 -DMUSIC_SOURCE_LEGACY。
 * ============================================================ */

/* 无 key 公开音源端点（歌名拼在 '=' 后）。免费 API 有生有灭，多备几个
 * 轮询回退；下面的递归 JSON 扫描器不挑响应形状，端点活着就能用。 */
static const char *const k_music_apis[] = {
    "https://api.qijieya.cn/meting/?type=search&limit=5&id=",
    "https://api.codelife.cc/api/music?name=",
    NULL,
};

/* GD Studio 两步式（search → url_id → 直链 JSON） */
#define MUSIC_GD_SEARCH_URL \
    "https://music-api.gdstudio.xyz/api.php?types=search&source=netease&name="
#define MUSIC_GD_URL_API \
    "https://music-api.gdstudio.xyz/api.php?types=url&source=netease&br=320&id="

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} music_http_acc_t;

static esp_err_t music_http_event(esp_http_client_event_t *evt)
{
    music_http_acc_t *a = (music_http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && a) {
        if (a->len + evt->data_len < a->cap) {
            memcpy(a->buf + a->len, evt->data, evt->data_len);
            a->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 歌名做百分号编码（UTF-8 字节序）拼进查询串 */
static void music_url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (j + 3 < dst_cap) {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        } else {
            break;
        }
    }
    dst[j] = '\0';
}

/* 启发式：这串像不像音频直链（区别于封面图/歌词）。 */
static bool music_is_audio_url(const char *v)
{
    return (strstr(v, ".mp3")  || strstr(v, ".m4a")  || strstr(v, ".aac") ||
            strstr(v, ".flac") || strstr(v, ".wav")  || strstr(v, ".ogg") ||
            strstr(v, ".opus") || strstr(v, "mp3")   || strstr(v, "audio") ||
            strstr(v, "music") || strstr(v, "media") || strstr(v, "play") ||
            strstr(v, "song")  || strstr(v, "type=url"));
}

static bool music_is_image_url(const char *v)
{
    return (strstr(v, ".jpg") || strstr(v, ".jpeg") || strstr(v, ".png") ||
            strstr(v, ".webp") || strstr(v, ".gif"));
}

/* 递归扫 JSON 树取第一个可用直链。mode 0 优先音频；mode 1 收编任何
 * 非图片/歌词的 http(s) 串 —— 不挑端点响应形状。 */
static const char *music_find_url(cJSON *node, char *out, size_t out_len, int mode)
{
    if (node == NULL || out == NULL) {
        return NULL;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        const char *v = node->valuestring;
        if (strncmp(v, "http", 4) == 0) {
            if (mode == 0) {
                if (music_is_audio_url(v)) {
                    snprintf(out, out_len, "%s", v);
                    return out;
                }
            } else {
                if (!music_is_image_url(v) && !strstr(v, ".lrc")) {
                    snprintf(out, out_len, "%s", v);
                    return out;
                }
            }
        }
        return NULL;
    }
    if (cJSON_IsArray(node)) {
        cJSON *e;
        cJSON_ArrayForEach(e, node) {
            if (music_find_url(e, out, out_len, mode)) {
                return out;
            }
        }
    } else if (cJSON_IsObject(node)) {
        cJSON *c;
        cJSON_ArrayForEach(c, node) {
            if (music_find_url(c, out, out_len, mode)) {
                return out;
            }
        }
    }
    return NULL;
}

/* 通用 HTTPS GET：收响应体到 body（TLS 走证书 bundle 验证）。 */
static bool music_http_get(const char *req, char *body, size_t body_cap, size_t *out_len)
{
    music_http_acc_t acc = { .buf = body, .cap = body_cap, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = req,
        .event_handler = music_http_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = MUSIC_SEARCH_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    esp_err_t r = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (r != ESP_OK || status < 200 || status >= 300 || acc.len == 0) {
        return false;
    }
    body[acc.len < body_cap ? acc.len : body_cap - 1] = '\0';
    *out_len = acc.len;
    return true;
}

/* 查一个端点：HTTPS GET → 收 2KB 响应体 → 两轮递归扫描出直链。 */
static bool music_search_one(const char *api, const char *enc_name,
                             char *out_url, size_t out_url_len)
{
    char req[MUSIC_URL_MAX + 64];
    snprintf(req, sizeof(req), "%s%s", api, enc_name);

    char *body = heap_caps_malloc(MUSIC_SEARCH_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return false;
    }
    bool ok = false;
    size_t n = 0;
    if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
        cJSON *root = cJSON_Parse(body);
        if (root != NULL) {
            if (music_find_url(root, out_url, out_url_len, 0) == NULL) {
                music_find_url(root, out_url, out_url_len, 1);
            }
            cJSON_Delete(root);
            ok = (out_url[0] != '\0');
        }
    }
    heap_caps_free(body);
    ESP_LOGI(TAG, "search %s -> %s", req, ok ? out_url : "(no url)");
    return ok;
}

/* ★ GD Studio 两步式（另一家独立解析站，与 qijieya 互为冗余） */
static bool music_search_gdstudio(const char *enc_name,
                                  char *out_url, size_t out_url_len)
{
    char req[MUSIC_URL_MAX + 128];
    char *body = heap_caps_malloc(MUSIC_SEARCH_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return false;
    }
    bool ok = false;
    size_t n = 0;

    snprintf(req, sizeof(req), "%s%s&count=5", MUSIC_GD_SEARCH_URL, enc_name);
    if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
        cJSON *root = cJSON_Parse(body);
        const char *url_id = NULL;
        if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
            cJSON *first = cJSON_GetArrayItem(root, 0);
            url_id = cJSON_GetObjectItem(first, "url_id") ?
                     cJSON_GetStringValue(cJSON_GetObjectItem(first, "url_id")) : NULL;
        }
        cJSON_Delete(root);
        if (url_id != NULL && url_id[0] != '\0') {
            snprintf(req, sizeof(req), "%s%s", MUSIC_GD_URL_API, url_id);
            if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
                cJSON *r2 = cJSON_Parse(body);
                const char *dl = r2 ? cJSON_GetStringValue(cJSON_GetObjectItem(r2, "url")) : NULL;
                if (dl != NULL && strncmp(dl, "http", 4) == 0 &&
                    strlen(dl) < out_url_len) {
                    snprintf(out_url, out_url_len, "%s", dl);
                    ok = true;
                }
                cJSON_Delete(r2);
            }
        }
    }
    heap_caps_free(body);
    ESP_LOGI(TAG, "gdstudio search -> %s", ok ? out_url : "(no url)");
    return ok;
}

/* 逐端点轮询：第一个给出直链的赢。全灭返回 false。 */
static bool music_search_url(const char *name, char *out_url, size_t out_url_len)
{
    char enc[MUSIC_URL_MAX];
    music_url_encode(name, enc, sizeof(enc));
    for (int i = 0; k_music_apis[i] != NULL; i++) {
        if (music_search_one(k_music_apis[i], enc, out_url, out_url_len)) {
            return true;
        }
    }
    return music_search_gdstudio(enc, out_url, out_url_len);
}
#endif /* MUSIC_SOURCE_LEGACY */

/* ============================================================
 * 结果回传
 * ============================================================ */

/* 结果回调（kicker 任务 / simple_player 播放任务上下文调用）→ XZ_EVT_TEXT */
static ai_music_result_cb_t s_result_cb;

void ai_music_set_result_cb(ai_music_result_cb_t cb)
{
    s_result_cb = cb;
}

static void report(const char *text)
{
    ESP_LOGI(TAG, "report: %s", text);
    if (s_result_cb) {
        s_result_cb(text);
    }
}

/* ---- 同步结局（给 ai_action 的 music 动作用） ----
 * ★ ai_music_play() 只保证"kicker 任务起来了"就回 ESP_OK ⇒ 云端把"起播"
 *   当成"放上了"，即使搜不到歌也照样回一句"行，放《xxx》"（F59 假成功）。
 *   action 路径改用 ai_music_play_wait()，等 kicker 给出真结论。
 * ★ 用静态变量而不是堆：只有 ai_action worker 一条调用方（串行），
 *   省一次 malloc、也避开"超时后 free 与回调写同一块内存"的 UAF。 */
static SemaphoreHandle_t s_wait_sem;
static bool s_wait_ok;
static char s_wait_msg[128];

/* kicker 的终结：落日志 + 回传对话流 + 兑现等待者 */
static void kick_finish(bool ok, const char *text)
{
    report(text);
    if (s_wait_sem != NULL) {
        s_wait_ok = ok;
        snprintf(s_wait_msg, sizeof(s_wait_msg), "%s", text);
        xSemaphoreGive(s_wait_sem);
    }
}

/* ★ §73.27：播放结局回传桥——xiaozhi_service 的 playback 事件
 * （simple_player 播放任务上下文）→ 对话记录。STARTED 不报（tools/call
 * 响应已告知）；STOPPED 不报（用户叫停/换曲，工具响应已答）。 */
static void on_xz_music_evt(xz_music_evt_id_t evt, esp_err_t err)
{
    (void)err;
    switch (evt) {
    case XZ_AUDIO_FINISHED:
        report("这首播完了。");
        break;
    case XZ_AUDIO_ERROR:
        report("歌放到一半断了，可能网络不稳，再点一次试试。");
        break;
    default:
        break;
    }
}

/* ============================================================
 * 点歌顺带落盘（§18）：先播后存
 *
 * 与下面的 music_dl_task 的区别：
 *   · music_dl_task = "下载完再播"（用户明确说「下载这首歌」，要等 + 回话报大小）
 *   · 本任务        = "先播后存"（点歌时就顺手攒本地库，不拖慢起播、不打扰对话）
 * 存好后下次说同一个歌名，music_lib_find() 本地优先命中 ⇒ 秒起 + 断网可听。
 * ============================================================ */

typedef struct {
    music_cand_t cand;
} music_cache_t;

static volatile bool s_caching;

static void music_cache_task(void *arg)
{
    music_cache_t *k = (music_cache_t *)arg;
    char path[128];
    size_t bytes = 0;

    /* 错开起播峰值：播放器刚起来时内部 RAM / 带宽都在高位，这里还要再借
     * 2KB 内部缓冲（esp_http_client）+ 一条 HTTP 连接 ⇒ 晚 3s 最省事。 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_err_t err = music_lib_save(k->cand.id, &k->cand, 128,
                                   path, sizeof(path), &bytes);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cached -> %s (%.1f MB)", path, bytes / 1048576.0);
        char lrc[4096];
        if (music_source_lyric(k->cand.id, lrc, sizeof(lrc)) == ESP_OK) {
            music_lib_write_lrc(path, lrc);
        }
    } else {
        /* 无版权 / 空间不足 / 网络抖动：只记日志，绝不影响正在播的那首 */
        ESP_LOGW(TAG, "cache skipped: %s", esp_err_to_name(err));
    }
    s_caching = false;
    heap_caps_free(k);
    vTaskDeleteWithCaps(NULL);
}

/* 同一时刻只跑一个缓存任务：用户连点几首时后来的那首不缓存（它下次点
 * 会重新走云播 + 缓存，不会积压成一条下载队列）。单槽 volatile 足够 ——
 * 写方只有 kick/缓存任务自身，读方只做"跳过"判断。 */
static void cache_schedule(const music_cand_t *c)
{
    if (c == NULL || c->id[0] == '\0' || s_caching) {
        return;
    }
    music_cache_t *k = heap_caps_malloc(sizeof(*k), MALLOC_CAP_SPIRAM);
    if (k == NULL) {
        return;
    }
    memset(k, 0, sizeof(*k));
    k->cand = *c;

    s_caching = true;
    TaskHandle_t t = NULL;
    if (xTaskCreateWithCaps(music_cache_task, "music_cache", MUSIC_DL_STACK,
                            k, 3, &t, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_caching = false;
        heap_caps_free(k);
        ESP_LOGW(TAG, "cache task create failed, skip");
    }
}

/* ============================================================
 * 播放（§73.28：本地优先 → 云搜 → 会话内播放）
 * ============================================================ */

/* 一次性 kicker：查 SD 曲库 → 没命中就云搜真链 → 投会话内播放。
 * cb_play 跑在 MCP 引擎任务栈，绝不能做阻塞 HTTP —— 全部留给本任务
 * （PSRAM 栈 12KB，F35 纯网络任务允外置）。 */

typedef struct {
    char name[64];             /* 歌名（本地匹配 + 云搜索用） */
} music_kick_t;

static void music_kick_task(void *arg)
{
    music_kick_t *k = (music_kick_t *)arg;

    /* ① 本地优先（S8 §3.2）：说「播放晴天」先命中 SD 曲库 */
    char local_path[128];
    if (music_lib_find(k->name, local_path, sizeof(local_path))) {
        char uri[160];
        snprintf(uri, sizeof(uri), "file://%s", local_path);
        if (xiaozhi_service_music_play(uri) == ESP_OK) {
            ESP_LOGI(TAG, "play local: %s", local_path);
            kick_finish(true, "从 SD 卡播放。");
            goto out;
        }
        ESP_LOGW(TAG, "local hit but play rejected, fallback to cloud");
    }

    /* ② 云搜（music_proxy /v1/search，最多试 3 条候选，S8 §6.1 要点 7） */
    music_cand_t cands[5];
    int n = music_source_search(k->name, 5, cands, 5);
    if (n <= 0) {
        kick_finish(false, "没搜到这首歌，换个歌名试试。");
        goto out;
    }

    bool played = false;
    int  pick = -1;
    for (int i = 0; i < n && i < 3; i++) {
        char tmp[MUSIC_URL_MAX];
        size_t sz = 0;
        /* resolve 判活（无版权 → url 空） */
        if (music_source_resolve(cands[i].id, 128, tmp, sizeof(tmp), &sz) == ESP_OK &&
            tmp[0] != '\0') {
            /* 播放器直接吃 /v1/download URL（README §8.2：代理已处理 302/过期） */
            char dl[MUSIC_URL_MAX];
            if (music_source_build_download_url(cands[i].id, 128, dl, sizeof(dl)) == ESP_OK &&
                xiaozhi_service_music_play(dl) == ESP_OK) {
                played = true;
                pick = i;
                ESP_LOGI(TAG, "play cloud: %s (%.64s)", cands[i].name, dl);
                break;
            }
        }
    }
    if (!played) {
        kick_finish(false, "这首歌没版权或链接失效，换一首吧。");
    } else {
        kick_finish(true, "开播了。");
        /* ★ 先播后存：不阻塞回话、也不拖慢起播，成功了下次本地直出 */
        cache_schedule(pick >= 0 ? &cands[pick] : NULL);
    }
out:
    heap_caps_free(k);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t ai_music_play(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    music_kick_t *k = heap_caps_malloc(sizeof(music_kick_t), MALLOC_CAP_SPIRAM);
    if (k == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(k, 0, sizeof(*k));
    snprintf(k->name, sizeof(k->name), "%s", name);

    /* 与 ai_camera explain 任务同款：PSRAM 栈一次性网络任务（F35 允外置） */
    TaskHandle_t kick = NULL;
    BaseType_t ok = xTaskCreateWithCaps(music_kick_task, "music_kick",
                                        MUSIC_KICK_STACK, k, 5, &kick,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        heap_caps_free(k);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "play scheduled: %s (local-first, in-conversation)", name);
    return ESP_OK;
}

esp_err_t ai_music_play_wait(const char *name, char *msg, size_t cap,
                            uint32_t wait_ms)
{
    if (s_wait_sem == NULL) {
        s_wait_sem = xSemaphoreCreateBinary();
        if (s_wait_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    (void)xSemaphoreTake(s_wait_sem, 0);     /* 清掉可能残留的一次 give */
    s_wait_ok = false;
    s_wait_msg[0] = '\0';

    esp_err_t err = ai_music_play(name);
    if (err != ESP_OK) {
        if (msg != NULL && cap > 0) {
            snprintf(msg, cap, "起播失败: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (wait_ms == 0) {
        return ESP_OK;                       /* 纯异步：只保证任务起来了 */
    }
    if (xSemaphoreTake(s_wait_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        if (msg != NULL && cap > 0) {
            snprintf(msg, cap, "等 %lu ms 还没出结果", (unsigned long)wait_ms);
        }
        ESP_LOGW(TAG, "play wait timeout: %s", name);
        return ESP_ERR_TIMEOUT;
    }
    if (msg != NULL && cap > 0) {
        snprintf(msg, cap, "%s", s_wait_msg);
    }
    return s_wait_ok ? ESP_OK : ESP_FAIL;
}

static esp_mcp_value_t cb_play(const esp_mcp_property_list_t *props)
{
    /* ★ 单参数工具（§73.27 勘误）：MCP 引擎对注册的全部属性做"缺一即拒"
     * 校验且不回填默认值（esp_mcp_engine.c esp_mcp_tool_args_cb）——
     * 所以只注册 name，LLM 报歌名即可，无 url 参数可填错。 */
    const char *name = esp_mcp_property_list_get_property_string(props, "name");
    if (name == NULL || name[0] == '\0') {
        return esp_mcp_value_create_string("Please tell me the song name to play.");
    }

    esp_err_t e = ai_music_play(name);
    if (e == ESP_ERR_NO_MEM) {
        return esp_mcp_value_create_string("Out of memory - cannot start playback.");
    }
    if (e != ESP_OK) {
        return esp_mcp_value_create_string("Cannot start playback.");
    }

    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg),
             "Okay, looking up '%s' and playing it now. The conversation "
             "stays open - I can switch or stop songs anytime.", name);
    return esp_mcp_value_create_string(ok_msg);
}

/* ============================================================
 * 下载（§73.28：云搜 → 落盘 /sdcard/music/ → 回话）
 * ============================================================ */

typedef struct {
    char name[64];             /* 歌名（云搜索用） */
} music_dl_t;

static void music_dl_task(void *arg)
{
    music_dl_t *k = (music_dl_t *)arg;
    music_cand_t cands[5];
    int n = music_source_search(k->name, 5, cands, 5);
    if (n <= 0) {
        report("No playable link found for this song - try another title.");
        goto out;
    }

    /* 下载取第一条能落盘的（无版权 404 / 空间不足 自动跳下一条） */
    bool saved = false;
    for (int i = 0; i < n && i < 3; i++) {
        char path[128];
        size_t bytes = 0;
        esp_err_t err = music_lib_save(cands[i].id, &cands[i], 128,
                                       path, sizeof(path), &bytes);
        if (err == ESP_OK) {
            /* 歌词可选：失败不阻塞（S8：歌词缺失不是错误） */
            char lrc[4096];
            if (music_source_lyric(cands[i].id, lrc, sizeof(lrc)) == ESP_OK) {
                music_lib_write_lrc(path, lrc);
            }
            char msg[192];
            snprintf(msg, sizeof(msg), "Saved '%s' to SD card (%.1f MB). "
                     "Playing it now.",
                     cands[i].name, bytes / 1048576.0);
            report(msg);
            /* ★ 下载完成即播（S8 §6.2 闭环：落盘 → file:// 播放，验证 M0-1） */
            char uri[160];
            snprintf(uri, sizeof(uri), "file://%s", path);
            esp_err_t perr = xiaozhi_service_music_play(uri);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "play after save rejected (%s), file kept on SD",
                         esp_err_to_name(perr));
            } else {
                ESP_LOGI(TAG, "downloaded + playing: %s", uri);
            }
            saved = true;
            break;
        }
        ESP_LOGW(TAG, "download cand#%d failed: %s", i, esp_err_to_name(err));
    }
    if (!saved) {
        report("Download failed - no playable link or not enough space.");
    }
out:
    heap_caps_free(k);
    vTaskDeleteWithCaps(NULL);
}

static esp_mcp_value_t cb_download(const esp_mcp_property_list_t *props)
{
    const char *name = esp_mcp_property_list_get_property_string(props, "name");
    if (name == NULL || name[0] == '\0') {
        return esp_mcp_value_create_string("Please tell me the song name to download.");
    }

    music_dl_t *k = heap_caps_malloc(sizeof(music_dl_t), MALLOC_CAP_SPIRAM);
    if (k == NULL) {
        return esp_mcp_value_create_string("Out of memory - cannot start download.");
    }
    memset(k, 0, sizeof(*k));
    snprintf(k->name, sizeof(k->name), "%s", name);

    TaskHandle_t t = NULL;
    BaseType_t ok = xTaskCreateWithCaps(music_dl_task, "music_dl",
                                        MUSIC_DL_STACK, k, 5, &t,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        heap_caps_free(k);
        return esp_mcp_value_create_string("Out of memory - cannot start download.");
    }

    ESP_LOGI(TAG, "download scheduled: %s", name);
    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg),
             "Okay, downloading '%s' to the SD card. I'll report the file "
             "size when it's done.", name);
    return esp_mcp_value_create_string(ok_msg);
}

/* ============================================================
 * 列表（§73.28：扫 /sdcard/music，回「已下载 N 首」）
 * ============================================================ */

static void music_list_task(void *arg)
{
    (void)arg;
    music_item_t items[16];
    int n = music_lib_list(items, 16, 0, 16);
    if (n <= 0) {
        report("No songs downloaded yet. Say 'download <song>' to save one.");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    char msg[512];
    int w = snprintf(msg, sizeof(msg), "%d songs on the SD card:", n);
    for (int i = 0; i < n && w < (int)sizeof(msg) - 4; i++) {
        int t = snprintf(msg + w, sizeof(msg) - (size_t)w, " %s,", items[i].title);
        if (t <= 0 || w + t >= (int)sizeof(msg)) {
            break;
        }
        w += t;
    }
    /* 去掉末尾逗号，加句点 */
    if (w > 0 && msg[w - 1] == ',') {
        msg[w - 1] = '.';
    }
    report(msg);
    vTaskDeleteWithCaps(NULL);
}

static esp_mcp_value_t cb_list(const esp_mcp_property_list_t *props)
{
    (void)props;
    TaskHandle_t t = NULL;
    BaseType_t ok = xTaskCreateWithCaps(music_list_task, "music_list",
                                        MUSIC_LIST_STACK, NULL, 5, &t,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        return esp_mcp_value_create_string("Out of memory - cannot list songs.");
    }
    return esp_mcp_value_create_string("Checking the downloaded songs...");
}

/* ============================================================
 * Stop（不变）
 * ============================================================ */

static esp_mcp_value_t cb_stop(const esp_mcp_property_list_t *props)
{
    (void)props;
    esp_err_t err = xiaozhi_service_music_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stop scheduled");
        return esp_mcp_value_create_string("Music stopped.");
    }
    return esp_mcp_value_create_string("No music is playing in this "
                                       "conversation right now.");
}

/* ============================================================
 * 注册（§73.28：play / download / list / stop）
 * ============================================================ */

esp_err_t ai_music_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    /* ---- play(name) ---- */
    esp_mcp_property_t *p_name =
        esp_mcp_property_create_with_string("name", "");
    ESP_RETURN_ON_FALSE(p_name, ESP_FAIL, TAG, "prop name");

    esp_mcp_tool_t *play = esp_mcp_tool_create(
        "self.music.play",
        "在线点歌：按歌名播放音乐，**边播放边保持对话不断开**。"
        "当用户说'放歌/点歌/来首歌/播放xxx'等想听歌的话时调用：把歌名（如"
        "'晴天'）填进 name 参数即可。设备会先查 SD 卡本地曲库（下载过的歌"
        "断网也能播），没有再从云端音源搜索并播放——**不要自己编造任何 URL**。"
        "播放中用户说'下一首/换一首/换成xxx' = 用新歌名再次调用本工具（直接换，"
        "无需先 stop）；'别放了/停止播放'用 self.music.stop。"
        "查不到可播版本会如实回告，请引导用户换歌名。",
        cb_play);
    ESP_RETURN_ON_FALSE(play, ESP_FAIL, TAG, "tool play");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(play, p_name) == ESP_OK,
                        ESP_FAIL, TAG, "add prop name");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, play) == ESP_OK,
                        ESP_FAIL, TAG, "add tool play");

    /* ---- download(name) ---- */
    esp_mcp_property_t *p_dl_name =
        esp_mcp_property_create_with_string("name", "");
    ESP_RETURN_ON_FALSE(p_dl_name, ESP_FAIL, TAG, "prop dl_name");

    esp_mcp_tool_t *download = esp_mcp_tool_create(
        "self.music.download",
        "下载歌曲到设备的 SD 卡（离线曲库）。当用户说'下载xxx/把xxx存下来/"
        "缓存一首歌'时调用：把歌名填进 name 参数。下载完成后会报文件大小，"
        "之后说'播放xxx'会优先从本地播（断网也能听）。单参数。",
        cb_download);
    ESP_RETURN_ON_FALSE(download, ESP_FAIL, TAG, "tool download");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(download, p_dl_name) == ESP_OK,
                        ESP_FAIL, TAG, "add prop dl_name");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, download) == ESP_OK,
                        ESP_FAIL, TAG, "add tool download");

    /* ---- list(无参) ---- */
    esp_mcp_tool_t *list = esp_mcp_tool_create(
        "self.music.list",
        "查看设备 SD 卡上已下载的歌曲列表。当用户问'你都下载了什么歌/曲库里有"
        "什么/有哪些歌'时调用。无参数，直接返回已下载歌曲的清单。",
        cb_list);
    ESP_RETURN_ON_FALSE(list, ESP_FAIL, TAG, "tool list");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, list) == ESP_OK,
                        ESP_FAIL, TAG, "add tool list");

    /* ---- stop(无参) ---- */
    esp_mcp_tool_t *stop = esp_mcp_tool_create(
        "self.music.stop",
        "停止当前播放的音乐。当用户说'别放了/停止播放/关掉音乐'时调用。"
        "换歌不必先 stop——直接用新歌名调 self.music.play 即可。无参数。",
        cb_stop);
    ESP_RETURN_ON_FALSE(stop, ESP_FAIL, TAG, "tool stop");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, stop) == ESP_OK,
                        ESP_FAIL, TAG, "add tool stop");

    /* 播放结局回传进对话记录（播完/播失败用户看得见，不再无声） */
    xiaozhi_service_set_music_evt_cb(on_xz_music_evt);

    ESP_LOGI(TAG, "mcp tools: play(name) / download(name) / list / stop "
                  "(in-conversation, local-first)");
    return ESP_OK;
}
