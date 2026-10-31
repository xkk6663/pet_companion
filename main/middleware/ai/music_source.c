/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file music_source.c
 * @brief 云侧音源客户端（S8 / §73.28）：对接云端 music_proxy /v1 接口。
 *
 * 契约来源：tools/music_proxy/README.md §6（设备侧接口文档）：
 *   GET /v1/search?q=&limit=   → data[]: id/name/artist/duration_ms/cover
 *   GET /v1/url?id=&br=        → data.url + data.size（直链过期服务端自动重取）
 *   GET /v1/lyric?id=          → data.lrc（可能为空串，不是错误）
 *   GET /v1/download?id=&br=   → 音频字节流（支持 Range，错误 502）
 *   鉴权：Header X-Music-Token 或 query token=；/healthz 免鉴权。
 *   错误码：401/429/400/404/502/503。
 *
 * 内存红线（S8 §7）：
 *   - body 缓冲 16KB 一律 PSRAM（heap_caps_malloc MALLOC_CAP_SPIRAM）；
 *   - 纯网络任务，可 PSRAM 栈（调用方负责，本模块不建任务）；
 *   - 回调链上绝不阻塞（本模块函数只在专用任务里调用）。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "music_source.h"

static const char *TAG = "MUSIC_SRC";

/* ---- 编译期默认值（CMakeLists 可用 -D 覆盖） ----
 * ★★ 必须给真值：全仓库**没有**任何地方调 music_source_set_cfg()，
 *   默认留空就等于"云侧点歌永久失效"——`music_source_search()` 一进来
 *   就看到 base 为空直接返回 -1，设备端只回一句"没找到可播的版本"（F59）。
 *   地址＝服务器上的 music_proxy 容器（8080，与网关 8081 同机）。 */
#ifndef MUSIC_BASE_URL_DEFAULT
#define MUSIC_BASE_URL_DEFAULT "http://120.76.230.17:8080"
#endif
#ifndef MUSIC_TOKEN_DEFAULT
#define MUSIC_TOKEN_DEFAULT "04d7978286b0ff50983b33936f466d0d"
#endif

#define MUSIC_SRC_NVS_NS      "music_cfg"
#define MUSIC_SRC_NVS_KEY_BASE "base"
#define MUSIC_SRC_NVS_KEY_TOK "token"

/* 单次 HTTP 请求超时（服务端 UPSTREAM_TIMEOUT=8，端侧给 10 宽裕） */
#define MUSIC_SRC_TIMEOUT_MS  10000
/* 响应体缓冲：搜索响应 5 首 ~8-15KB（F4 估），统一 16KB PSRAM */
#define MUSIC_SRC_BODY_MAX    (16 * 1024)
/* URL 长度上限（与 ai_music.c MUSIC_URL_MAX 对齐） */
#define MUSIC_SRC_URL_MAX     512

/* 静态配置（进程生命周期内不变，NVS 读写走函数） */
static char s_base[MUSIC_SRC_URL_MAX];
static char s_token[80];

/* ============================================================
 * NVS 配置读写
 * ============================================================ */

/* 包装 IDF 的 nvs_get_str/nvs_set_str：避免与标准头重名 → ms_nvs_* */
static esp_err_t ms_nvs_get_str(const char *ns, const char *key,
                                char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = cap;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    out[len < cap ? len : cap - 1] = '\0';
    return ESP_OK;
}

static esp_err_t ms_nvs_set_str(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_cfg_from_nvs(void)
{
    char v[MUSIC_SRC_URL_MAX];
    if (ms_nvs_get_str(MUSIC_SRC_NVS_NS, MUSIC_SRC_NVS_KEY_BASE, v, sizeof(v)) == ESP_OK &&
        v[0] != '\0') {
        snprintf(s_base, sizeof(s_base), "%s", v);
    } else {
        snprintf(s_base, sizeof(s_base), "%s", MUSIC_BASE_URL_DEFAULT);
    }
    char t[80];
    if (ms_nvs_get_str(MUSIC_SRC_NVS_NS, MUSIC_SRC_NVS_KEY_TOK, t, sizeof(t)) == ESP_OK &&
        t[0] != '\0') {
        snprintf(s_token, sizeof(s_token), "%s", t);
    } else {
        snprintf(s_token, sizeof(s_token), "%s", MUSIC_TOKEN_DEFAULT);
    }
    ESP_LOGI(TAG, "cfg: base=%s token=%s (len=%d, from %s)",
             s_base, s_token[0] ? "***" : "(empty)",
             (int)strlen(s_token),
             ms_nvs_get_str(MUSIC_SRC_NVS_NS, MUSIC_SRC_NVS_KEY_BASE, v, sizeof(v)) == ESP_OK
             ? "NVS" : "default");
}

esp_err_t music_source_init(void)
{
    load_cfg_from_nvs();
    if (s_base[0] == '\0') {
        ESP_LOGW(TAG, "base not configured - cloud music disabled");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t music_source_set_cfg(const char *base, const char *token)
{
    if (base != NULL && base[0] != '\0') {
        ESP_RETURN_ON_ERROR(ms_nvs_set_str(MUSIC_SRC_NVS_NS, MUSIC_SRC_NVS_KEY_BASE, base),
                            TAG, "nvs base");
        snprintf(s_base, sizeof(s_base), "%s", base);
    }
    if (token != NULL && token[0] != '\0') {
        ESP_RETURN_ON_ERROR(ms_nvs_set_str(MUSIC_SRC_NVS_NS, MUSIC_SRC_NVS_KEY_TOK, token),
                            TAG, "nvs token");
        snprintf(s_token, sizeof(s_token), "%s", token);
    }
    return ESP_OK;
}

const char *music_source_get_base(void)
{
    return s_base;
}

const char *music_source_get_token(void)
{
    return s_token;
}

/* ============================================================
 * HTTP 层
 * ============================================================ */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} ms_http_acc_t;

static esp_err_t ms_http_event(esp_http_client_event_t *evt)
{
    ms_http_acc_t *a = (ms_http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && a) {
        if (a->len + evt->data_len < a->cap) {
            memcpy(a->buf + a->len, evt->data, evt->data_len);
            a->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 歌名百分号编码（UTF-8 字节序），RFC 3986 unreserved 保留 */
static void ms_url_encode(const char *src, char *dst, size_t dst_cap)
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

/* 判断 base 是否 https：决定挂不挂 crt_bundle（S8 §6.1 要点 5/6） */
static bool ms_is_https(const char *base)
{
    return (strncmp(base, "https://", 8) == 0);
}

/* 通用 GET（带 X-Music-Token）：收响应体到 PSRAM body。
 * 成功返回 HTTP 状态码；网络层失败返回 -1。 */
static int ms_http_get(const char *url, const char *token,
                       char *body, size_t body_cap, size_t *out_len)
{
    ms_http_acc_t acc = { .buf = body, .cap = body_cap, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = ms_http_event,
        .user_data = &acc,
        .timeout_ms = MUSIC_SRC_TIMEOUT_MS,
        .buffer_size = 4096,
        /* http(s) 分流：https 挂证书 bundle，http 不挂（S8 §6.1 要点 5/6） */
        .crt_bundle_attach = ms_is_https(s_base) ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -1;
    }
    if (token != NULL && token[0] != '\0') {
        esp_http_client_set_header(client, "X-Music-Token", token);
    }
    esp_err_t r = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "GET %s -> esp_err=%s", url, esp_err_to_name(r));
        return -1;
    }
    if (status >= 200 && status < 300 && acc.len > 0) {
        body[acc.len < body_cap ? acc.len : body_cap - 1] = '\0';
        if (out_len != NULL) {
            *out_len = acc.len;
        }
    }
    return status;
}

/* 构造 /v1/search?q=&limit= 并请求。返回 cJSON root（调用方释放）或 NULL。
 * 只认 code==0；HTTP 200 + code!=0（404 no result）也返回 NULL。 */
static cJSON *ms_search_req(const char *enc_q, int limit)
{
    char url[2 * MUSIC_SRC_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/v1/search?q=%s&limit=%d", s_base, enc_q, limit);

    char *body = heap_caps_malloc(MUSIC_SRC_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return NULL;
    }
    size_t n = 0;
    int status = ms_http_get(url, s_token, body, MUSIC_SRC_BODY_MAX, &n);
    if (status < 0) {
        heap_caps_free(body);
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "search JSON parse fail (status=%d, len=%d)", status, (int)n);
        return NULL;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code == NULL || !cJSON_IsNumber(code) || code->valueint != 0) {
        ESP_LOGW(TAG, "search code!=0 (http=%d)", status);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* ============================================================
 * Public API
 * ============================================================ */

int music_source_search(const char *q, int limit, music_cand_t *out, int cap)
{
    ESP_RETURN_ON_FALSE(q && q[0], -1, TAG, "empty q");
    ESP_RETURN_ON_FALSE(out && cap > 0, -1, TAG, "bad out/cap");
    if (limit <= 0) {
        limit = 5;
    }
    if (limit > cap) {
        limit = cap;
    }
    if (s_base[0] == '\0') {
        ESP_LOGW(TAG, "search skipped: base not configured");
        return -1;
    }

    char enc[MUSIC_SRC_URL_MAX];
    ms_url_encode(q, enc, sizeof(enc));
    cJSON *root = ms_search_req(enc, limit);
    if (root == NULL) {
        return 0;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL || !cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return 0;
    }
    int got = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, data) {
        if (got >= cap) {
            break;
        }
        if (!cJSON_IsObject(e)) {
            continue;
        }
        music_cand_t *c = &out[got];
        memset(c, 0, sizeof(*c));
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(e, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(e, "artist"));
        const char *cover = cJSON_GetStringValue(cJSON_GetObjectItem(e, "cover"));
        cJSON *dur = cJSON_GetObjectItem(e, "duration_ms");
        if (id == NULL || id[0] == '\0' || name == NULL || name[0] == '\0') {
            continue;
        }
        snprintf(c->id, sizeof(c->id), "%s", id);
        snprintf(c->name, sizeof(c->name), "%s", name);
        if (artist != NULL) {
            snprintf(c->artist, sizeof(c->artist), "%s", artist);
        }
        if (cover != NULL) {
            snprintf(c->cover, sizeof(c->cover), "%s", cover);
        }
        if (cJSON_IsNumber(dur)) {
            c->duration_ms = (uint32_t)dur->valuedouble;
        }
        got++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "search '%s' -> %d 条", q, got);
    return got;
}

esp_err_t music_source_resolve(const char *id, int br, char *url, size_t cap,
                               size_t *size)
{
    ESP_RETURN_ON_FALSE(id && id[0], ESP_ERR_INVALID_ARG, TAG, "empty id");
    ESP_RETURN_ON_FALSE(url && cap > 0, ESP_ERR_INVALID_ARG, TAG, "bad url/cap");
    url[0] = '\0';
    if (size != NULL) {
        *size = 0;
    }
    if (s_base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char req[MUSIC_SRC_URL_MAX + 64];
    snprintf(req, sizeof(req), "%s/v1/url?id=%s&br=%d", s_base, id, br);

    char *body = heap_caps_malloc(MUSIC_SRC_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    int status = ms_http_get(req, s_token, body, MUSIC_SRC_BODY_MAX, &n);
    if (status < 0) {
        heap_caps_free(body);
        return ESP_ERR_HTTP_CONNECT;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code == NULL || !cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON_Delete(root);
        return (status == 404) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data != NULL && cJSON_IsObject(data)) {
        const char *dl = cJSON_GetStringValue(cJSON_GetObjectItem(data, "url"));
        cJSON *sz = cJSON_GetObjectItem(data, "size");
        if (dl != NULL && strncmp(dl, "http", 4) == 0 && strlen(dl) < cap) {
            snprintf(url, cap, "%s", dl);
        }
        if (sz != NULL && cJSON_IsNumber(sz) && size != NULL) {
            *size = (size_t)sz->valuedouble;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "resolve %s br=%d -> %s (%.1f MB)", id, br,
             url[0] ? "url" : "(no url)", size ? *size / 1048576.0 : 0.0);
    return ESP_OK;
}

esp_err_t music_source_lyric(const char *id, char *lrc, size_t cap)
{
    ESP_RETURN_ON_FALSE(id && id[0], ESP_ERR_INVALID_ARG, TAG, "empty id");
    ESP_RETURN_ON_FALSE(lrc && cap > 0, ESP_ERR_INVALID_ARG, TAG, "bad lrc/cap");
    lrc[0] = '\0';
    if (s_base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char req[MUSIC_SRC_URL_MAX + 64];
    snprintf(req, sizeof(req), "%s/v1/lyric?id=%s", s_base, id);

    char *body = heap_caps_malloc(MUSIC_SRC_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    int status = ms_http_get(req, s_token, body, MUSIC_SRC_BODY_MAX, &n);
    if (status < 0) {
        heap_caps_free(body);
        return ESP_ERR_HTTP_CONNECT;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code == NULL || !cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    const char *l = (data != NULL) ?
        cJSON_GetStringValue(cJSON_GetObjectItem(data, "lrc")) : NULL;
    if (l != NULL && strlen(l) < cap) {
        snprintf(lrc, cap, "%s", l);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "lyric %s -> %d B", id, (int)strlen(lrc));
    return ESP_OK;
}

esp_err_t music_source_build_download_url(const char *id, int br, char *url,
                                          size_t cap)
{
    ESP_RETURN_ON_FALSE(id && id[0], ESP_ERR_INVALID_ARG, TAG, "empty id");
    ESP_RETURN_ON_FALSE(url && cap > 0, ESP_ERR_INVALID_ARG, TAG, "bad url/cap");
    if (s_base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    /* token 走 query（播放器只吃一个 URL，README §8.2）；base 已是 http/https 全串 */
    int w = snprintf(url, cap, "%s/v1/download?id=%s&br=%d", s_base, id, br);
    if (w < 0 || (size_t)w >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_token[0] != '\0') {
        int t = snprintf(url + w, cap - (size_t)w, "&token=%s", s_token);
        if (t < 0 || (size_t)t >= cap - (size_t)w) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}
