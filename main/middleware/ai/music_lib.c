/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file music_lib.c
 * @brief SD 曲库实现（S8 / §73.28）：命名清洗、存在性查询、落盘（.part→rename）、
 *        列表、空间预检、lrc。
 *
 * 落盘铁律（S8 §6.2）：
 *   - 路径长度："/sdcard/music/" = 14 + 文件名 + NUL ≤ FM_PATH_MAX(128) ⇒ 文件名 ≤ 110B
 *   - 命名清洗：替换 / \ : * ? " < > | 和控制字符 → '_'；再按 UTF-8 边界截断
 *   - 原子性：先写 "<name>.part"；字节数对上 Content-Length 才 rename 成 .mp3
 *   - 残留下场：失败/超时 → 删 .part（否则 FILES 页出现"能看见但播不了"的鬼文件）
 *   - 空间预检：free < need + 1MB 直接拒绝
 *
 * ★ 调用环境：下载任务（PSRAM 栈 8KB + 内部 DMA 缓冲 ≤4KB，README §8.4）。
 *   本文件所有函数只允许在专用任务里调用；不建任务、不阻塞回调。
 */

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_vfs_fat.h"

#include "music_lib.h"
#include "music_source.h"
#include "sdcard_service.h"

static const char *TAG = "MUSIC_LIB";

/* 文件名上限："/sdcard/music/" = 14 + name + NUL ≤ 128 ⇒ name ≤ 110 字节（S8 §6.2）。
 * 留 2 字节给 '.' + 'm' 之类，实际截断到 96 更稳。 */
#define MUSIC_LIB_NAME_MAX   96
#define MUSIC_LIB_SAVE_BUF   (2 * 1024)   /* 内部 DMA 缓冲：≤4KB 红线（README §8.4） */
#define MUSIC_LIB_TIMEOUT_MS 10000
/* 取链/组链 URL 缓冲上限（与 music_source.h 的契约一致） */
#define MUSIC_SRC_URL_MAX    512

/* ---- 命名清洗：替换 / \ : * ? " < > | 和控制字符 → '_' ---- */
static void ms_sanitize(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20) {
            dst[j++] = '_';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

/* UTF-8 边界截断：cut 后若最后一个字节是 UTF-8 续字节（10xxxxxx），
 * 回退到字符开头，避免切半个汉字。 */
static size_t utf8_safe_cut(const char *s, size_t cut)
{
    if (cut == 0) {
        return 0;
    }
    size_t i = cut;
    /* 回退到不是续字节的位置：UTF-8 首字节要么 ASCII(0xxxxxxx) 要么 11xxxxxx */
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

/* 组文件名 "<artist>-<name>.mp3"，清洗 + 截断 ≤96B */
static void build_filename(const music_cand_t *cand, char *out, size_t cap)
{
    char a[MUSIC_LIB_NAME_MAX];
    char n[MUSIC_LIB_NAME_MAX];
    ms_sanitize(a, sizeof(a), cand->artist);
    ms_sanitize(n, sizeof(n), cand->name);

    char raw[2 * MUSIC_LIB_NAME_MAX + 8];
    snprintf(raw, sizeof(raw), "%s-%s", a, n);
    size_t cut = utf8_safe_cut(raw, MUSIC_LIB_NAME_MAX - 4);  /* 留 ".mp3\0" */
    snprintf(out, cap, "%.*s.mp3", (int)cut, raw);
}

/* ---- 空间预检：esp_vfs_fat_info 拿 free（M0-4 验证过；签名是 uint64_t） ---- */
bool music_lib_has_space(size_t need_bytes)
{
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SDCARD_MNT_PATH, &total, &freeb) != ESP_OK) {
        return false;
    }
    /* S8 §3.2：free < size + 1MB 直接拒绝 */
    bool ok = (freeb >= need_bytes + (1024 * 1024));
    /* ★ uint64_t 必须用 %llu（%zu 是 size_t=32bit）：实测 %zu 导致 varargs
     *   槽错位，free/need 全打垃圾值、%s 读到 NULL 打印 "(null)"（2026-10-31）。
     *   判定本身是纯 uint64 运算不受影响，仅打印错乱。 */
    ESP_LOGI(TAG, "space: free=%lluMB need=%lluMB -> %s",
             (unsigned long long)(freeb / (1024 * 1024)),
             (unsigned long long)(need_bytes / (1024 * 1024)),
             ok ? "ok" : "reject");
    return ok;
}

bool music_lib_ready(void)
{
    if (!sdcard_service_is_mounted()) {
        return false;
    }
    struct stat st;
    if (stat(MUSIC_LIB_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (mkdir(MUSIC_LIB_DIR, 0755) != 0) {
            ESP_LOGW(TAG, "mkdir %s failed", MUSIC_LIB_DIR);
            return false;
        }
        ESP_LOGI(TAG, "created %s", MUSIC_LIB_DIR);
    }
    return true;
}

/* ---- 列表：扫目录，按 mtime 倒序（简单冒泡，条目少无所谓） ---- */
int music_lib_list(music_item_t *out, int cap, int skip, int take)
{
    if (!music_lib_ready()) {
        return 0;
    }
    DIR *dir = opendir(MUSIC_LIB_DIR);
    if (dir == NULL) {
        return 0;
    }

    /* 先收集 *.mp3 文件（上限 64，够用）。
     * ★ 放 PSRAM heap：music_item_t≈208B × 64 ≈ 13KB，绝不能上
     *   4KB 的任务栈（MUSIC_LIST_STACK）——栈溢出必崩（S8 F35 同款）。 */
    music_item_t *tmp = heap_caps_malloc(64 * sizeof(music_item_t),
                                         MALLOC_CAP_SPIRAM);
    if (tmp == NULL) {
        closedir(dir);
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < 64) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".mp3") != 0) {
            continue;
        }
        char p[128];
        snprintf(p, sizeof(p), "%s/%s", MUSIC_LIB_DIR, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) {
            continue;
        }
        snprintf(tmp[n].path, sizeof(tmp[n].path), "%s", p);
        /* title = 去扩展名 */
        size_t tl = l - 4;
        snprintf(tmp[n].title, sizeof(tmp[n].title), "%.*s", (int)tl, de->d_name);
        tmp[n].bytes = (size_t)st.st_size;
        n++;
    }
    closedir(dir);

    /* mtime 倒序（冒泡，n ≤ 64） */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            struct stat a, b;
            stat(tmp[j].path, &a);
            stat(tmp[j + 1].path, &b);
            if (a.st_mtime < b.st_mtime) {
                music_item_t t = tmp[j];
                tmp[j] = tmp[j + 1];
                tmp[j + 1] = t;
            }
        }
    }

    int got = 0;
    for (int i = skip; i < n && got < take && got < cap; i++) {
        out[got++] = tmp[i];
    }
    heap_caps_free(tmp);
    return got;
}

/* ---- 模糊命中：扫 *.mp3，文件名/标题包含关键词（大小写不敏感） ---- */
bool music_lib_find(const char *keyword, char *path, size_t cap)
{
    if (!music_lib_ready() || keyword == NULL || keyword[0] == '\0') {
        return false;
    }
    /* 关键词转小写（ASCII 部分；中文不受影响） */
    char key[128];
    snprintf(key, sizeof(key), "%s", keyword);
    for (int i = 0; key[i]; i++) {
        key[i] = (char)tolower((unsigned char)key[i]);
    }

    DIR *dir = opendir(MUSIC_LIB_DIR);
    if (dir == NULL) {
        return false;
    }
    bool hit = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".mp3") != 0) {
            continue;
        }
        /* 匹配文件名（含扩展名与歌手前缀） */
        char low[160];
        snprintf(low, sizeof(low), "%s", de->d_name);
        for (int i = 0; low[i]; i++) {
            low[i] = (char)tolower((unsigned char)low[i]);
        }
        if (strstr(low, key) != NULL) {
            snprintf(path, cap, "%s/%s", MUSIC_LIB_DIR, de->d_name);
            hit = true;
            break;
        }
    }
    closedir(dir);
    return hit;
}

/* ---- 下载事件累加器 ---- */
typedef struct {
    FILE     *fp;
    size_t    wrote;
    size_t    expected;   /* Content-Length，-1 = 未知 */
    bool      ok;
} dl_acc_t;

static esp_err_t dl_event(esp_http_client_event_t *evt)
{
    dl_acc_t *a = (dl_acc_t *)evt->user_data;
    if (a == NULL) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        /* Content-Length 头到达时顺手记下；未到（chunked/分块）保持 0，
         * 校验时靠 wrote>0 + 写盘无错兜底 */
        a->expected = (size_t)esp_http_client_get_content_length(evt->client);
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0 && a->fp != NULL) {
            size_t w = fwrite(evt->data, 1, evt->data_len, a->fp);
            a->wrote += w;
            if (w != (size_t)evt->data_len) {
                a->ok = false;   /* 写盘失败（ENOSPC 等） */
            }
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ---- 落盘核心 ---- */
esp_err_t music_lib_save(const char *id, const music_cand_t *cand, int br,
                         char *out_path, size_t cap, size_t *out_bytes)
{
    ESP_RETURN_ON_FALSE(id && id[0] && cand, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(out_path && cap > 0, ESP_ERR_INVALID_ARG, TAG, "bad path");
    if (!music_lib_ready()) {
        return ESP_ERR_NOT_SUPPORTED;   /* SD 未挂载 */
    }

    /* ① resolve 拿直链 + size（无版权 → 404 提前返回） */
    char url[MUSIC_SRC_URL_MAX];
    size_t size = 0;
    ESP_RETURN_ON_ERROR(music_source_resolve(id, br, url, sizeof(url), &size),
                        TAG, "resolve %s", id);
    if (url[0] == '\0') {
        return ESP_ERR_NOT_FOUND;   /* 该曲无版权/无链 */
    }

    /* ② 空间预检（S8：free < size + 1MB 拒绝） */
    if (size > 0 && !music_lib_has_space(size)) {
        return ESP_ERR_NO_MEM;
    }

    /* ③ 组文件名 "<artist>-<name>.mp3" + ".part" */
    char fname[MUSIC_LIB_NAME_MAX + 8];
    build_filename(cand, fname, sizeof(fname));
    char part[128], final[128];
    snprintf(part, sizeof(part), "%s/%s.part", MUSIC_LIB_DIR, fname);
    snprintf(final, sizeof(final), "%s/%s", MUSIC_LIB_DIR, fname);
    ESP_LOGI(TAG, "download -> %s (need %zuB)", final, size);

    /* ④ 流式下载 .part（esp_http_client buffer 2048 内部 DMA） */
    FILE *fp = fopen(part, "wb");
    if (fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    dl_acc_t acc = { .fp = fp, .wrote = 0, .expected = 0, .ok = true };

    /* 用 download URL（README §8.2：端侧不解析 302、代理已处理过期） */
    char dl_url[MUSIC_SRC_URL_MAX];
    esp_err_t bu = music_source_build_download_url(id, br, dl_url, sizeof(dl_url));
    if (bu != ESP_OK) {
        fclose(fp);
        unlink(part);
        return bu;
    }

    esp_http_client_config_t cfg = {
        .url = dl_url,
        .event_handler = dl_event,
        .user_data = &acc,
        .timeout_ms = MUSIC_LIB_TIMEOUT_MS,
        .buffer_size = MUSIC_LIB_SAVE_BUF,   /* 2KB 内部 DMA */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t r = ESP_FAIL;
    int status = 0;
    if (client != NULL) {
        r = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    fclose(fp);

    /* ⑤ 校验：HTTP 200 + (无 Content-Length 或字节数对上) + 写盘无错 */
    bool ok_len = (acc.expected == 0) || (acc.wrote == acc.expected);
    if (r == ESP_OK && status == 200 && acc.ok && ok_len && acc.wrote > 0) {
        if (rename(part, final) == 0) {
            if (out_bytes != NULL) {
                *out_bytes = acc.wrote;
            }
            snprintf(out_path, cap, "%s", final);
            ESP_LOGI(TAG, "saved %s (%zuB)", final, acc.wrote);
            return ESP_OK;
        }
    }
    /* 失败：删 .part，不留鬼文件 */
    unlink(part);
    ESP_LOGW(TAG, "download fail r=%s status=%d wrote=%zu exp=%zu ok_len=%d",
             esp_err_to_name(r), status, acc.wrote, acc.expected, (int)ok_len);
    return ESP_FAIL;
}

/* ---- 写同名 .lrc（失败不阻塞） ---- */
esp_err_t music_lib_write_lrc(const char *mp3_path, const char *lrc)
{
    if (mp3_path == NULL || lrc == NULL || lrc[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char lrc_path[128];
    size_t l = strlen(mp3_path);
    if (l < 4 || l >= sizeof(lrc_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(lrc_path, sizeof(lrc_path), "%.*s.lrc", (int)(l - 4), mp3_path);

    FILE *fp = fopen(lrc_path, "w");
    if (fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t w = fwrite(lrc, 1, strlen(lrc), fp);
    fclose(fp);
    ESP_LOGI(TAG, "lrc %s (%zuB)", lrc_path, w);
    return (w == strlen(lrc)) ? ESP_OK : ESP_FAIL;
}
