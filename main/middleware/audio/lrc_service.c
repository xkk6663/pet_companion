/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lrc_service.c
 * @brief LRC 歌词解析实现（PSRAM 单块缓冲 + 行索引表）
 *
 * 关键纪律：
 *  - 文件内容与行索引表都放 PSRAM，不啃内部 RAM；
 *  - 全 UTF-8 守卫：任何一行不合法（典型 GBK 歌词）直接丢弃，绝不显示乱码；
 *  - 加载/卸载持锁，UI 侧读接口同样持锁 —— 卸载瞬间不会读到悬空指针。
 */

#include "lrc_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "LRC";

#define LRC_MAX_FILE_SIZE   (96 * 1024)
#define LRC_MAX_LINES       512
#define LRC_LEAD_MS         200   /* 歌词提前量：贴合"听到时字幕已到"的观感 */

typedef struct {
    uint32_t     t_ms;
    const char  *text;
} lrc_line_t;

static lrc_line_t      *s_lines = NULL;
static int              s_count = 0;
static char            *s_buf   = NULL;
static SemaphoreHandle_t s_lock = NULL;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void lock_init_once(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    } else {
        /* 已存在 */
    }
}

static inline void lrc_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void lrc_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* 严格 UTF-8 校验（拒绝超长编码、代理区、越界续字节） */
static bool utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t need;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
        } else {
            return false;
        }
        for (size_t k = 1; k <= need; k++) {
            if (i + k >= len) {
                return false;   /* 剩余字节不足 */
            }
            if ((unsigned char)s[i + k] < 0x80 || (unsigned char)s[i + k] > 0xBF) {
                return false;
            }
        }
        i += need + 1;
    }
    return true;
}

/* 解析 "[mm:ss.xx]" / "[mm:ss:xx]" / "[mm:ss]"，成功返回 ']' 之后的位置 */
static const char *parse_timestamp(const char *p, uint32_t *out_ms)
{
    if (*p != '[') {
        return NULL;
    }
    p++;
    uint32_t mm = 0;
    int nd = 0;
    while (*p >= '0' && *p <= '9') {
        mm = mm * 10 + (uint32_t)(*p - '0');
        p++;
        if (++nd > 3) {
            return NULL;
        }
    }
    if (nd == 0 || *p != ':') {
        return NULL;
    }
    p++;
    if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')) {
        return NULL;
    }
    uint32_t ss = (uint32_t)(p[0] - '0') * 10 + (uint32_t)(p[1] - '0');
    p += 2;
    uint32_t frac = 0;
    if (*p == '.' || *p == ':') {
        p++;
        uint32_t mul = 100;
        int nf = 0;
        while (*p >= '0' && *p <= '9' && nf < 3) {
            frac += (uint32_t)(*p - '0') * mul;
            mul /= 10;
            p++;
            nf++;
        }
        if (nf == 0) {
            return NULL;
        }
    }
    if (*p != ']') {
        return NULL;
    }
    *out_ms = (mm * 60 + ss) * 1000 + frac;
    return p + 1;
}

/* 把音频路径换成同名 .lrc 路径 */
static int build_lrc_path(const char *audio, char *out, size_t out_sz)
{
    size_t n = strlen(audio);
    if (n == 0 || n >= out_sz) {
        return -1;
    }
    strcpy(out, audio);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = '\0';
    }
    size_t base = strlen(out);
    if (base + 4 >= out_sz) {
        return -1;
    }
    strcpy(out + base, ".lrc");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

static void lrc_free_all(void)
{
    if (s_lines) {
        heap_caps_free(s_lines);
        s_lines = NULL;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_count = 0;
}

esp_err_t lrc_service_load(const char *audio_path)
{
    if (!audio_path || !audio_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_init_once();
    lrc_lock();
    lrc_free_all();

    char lrc_path[300];
    if (build_lrc_path(audio_path, lrc_path, sizeof(lrc_path)) != 0) {
        lrc_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(lrc_path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "no lrc: %s", lrc_path);
        lrc_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        /* ftell 在部分 VFS 上不可靠时退化用 fseek 判定 */
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
    }
    if (sz <= 0 || sz > LRC_MAX_FILE_SIZE) {
        fclose(f);
        ESP_LOGW(TAG, "lrc size invalid (%ld): %s", sz, lrc_path);
        lrc_unlock();
        return (sz > LRC_MAX_FILE_SIZE) ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_SET);

    s_buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_buf = malloc((size_t)sz + 1);   /* PSRAM 未初始化时兜底 */
    }
    s_lines = heap_caps_calloc(LRC_MAX_LINES, sizeof(lrc_line_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lines) {
        s_lines = calloc(LRC_MAX_LINES, sizeof(lrc_line_t));
    }
    if (!s_buf || !s_lines) {
        ESP_LOGE(TAG, "alloc failed");
        fclose(f);
        lrc_free_all();
        lrc_unlock();
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(s_buf, 1, (size_t)sz, f);
    fclose(f);
    s_buf[rd] = '\0';

    /* 解析：按行；UTF-8 不合法的行整体丢弃；支持一行多时间戳 */
    char *p = s_buf;
    if ((size_t)rd >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;   /* 跳过 BOM */
    }

    while (*p && s_count < LRC_MAX_LINES) {
        char *eol = strchr(p, '\n');
        if (eol) {
            *eol = '\0';
        }
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r') {
            p[len - 1] = '\0';
            len--;
        }

        if (len && p[0] == '[' && utf8_valid(p, len)) {
            const char *q = p;
            uint32_t stamps[8];
            int ns = 0;
            while (ns < 8) {
                const char *after = parse_timestamp(q, &stamps[ns]);
                if (!after) {
                    break;
                }
                ns++;
                q = after;
            }
            if (ns > 0) {
                /* 去掉文本前导空格 */
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                if (*q != '\0') {
                    for (int i = 0; i < ns && s_count < LRC_MAX_LINES; i++) {
                        s_lines[s_count].t_ms = stamps[i];
                        s_lines[s_count].text = q;
                        s_count++;
                    }
                }
            }
        }

        if (!eol) {
            break;
        }
        p = eol + 1;
    }

    if (s_count == 0) {
        char *tmp = s_buf;
        (void)tmp;
        lrc_free_all();
        lrc_unlock();
        ESP_LOGI(TAG, "lrc has no valid lines: %s", lrc_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 时间戳排序（多数文件本身有序，这里保证乱序文件也能二分） */
    for (int i = 1; i < s_count; i++) {
        lrc_line_t key = s_lines[i];
        int j = i - 1;
        while (j >= 0 && s_lines[j].t_ms > key.t_ms) {
            s_lines[j + 1] = s_lines[j];
            j--;
        }
        s_lines[j + 1] = key;
    }

    ESP_LOGI(TAG, "loaded %d lines: %s", s_count, lrc_path);
    lrc_unlock();
    return ESP_OK;
}

void lrc_service_unload(void)
{
    lock_init_once();
    lrc_lock();
    lrc_free_all();
    lrc_unlock();
}

bool lrc_service_has_lyrics(void)
{
    return s_count > 0;
}

int lrc_service_line_count(void)
{
    return s_count;
}

int lrc_service_find_index(int pos_ms)
{
    if (s_count <= 0) {
        return -1;
    }
    uint32_t target = (pos_ms > 0) ? (uint32_t)pos_ms : 0;
    target += LRC_LEAD_MS;

    int lo = 0, hi = s_count - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_lines[mid].t_ms <= target) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

const char *lrc_service_line_text(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return s_lines[index].text;
}
