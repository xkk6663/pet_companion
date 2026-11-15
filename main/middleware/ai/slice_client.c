/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file slice_client.c
 * @brief S10 P4：事件切片上行（见 slice_client.h 文件头）
 *
 * 线程模型：
 *   · slice_client_feed / gate_start / gate_end —— 闸门采集任务上下文；
 *   · slice_up_task —— 唯一的上传者（队列深度 2，满了就落 SD）；
 *   · slice_retry_task —— 每 60 s 扫一次 pending 目录（在线时补传）。
 *
 * 内存：
 *   ring     1 MB PSRAM（32.7 s @16k/mono/s16）
 *   单切片   最多 30 s = 960 KB；队列满时不分配、直接落 SD。
 *   SD 兜底  64 MB / 64 条 / 最低剩余 8 MB / 7 天 TTL；超限先删最旧，
 *            仍写不下就丢当前切片，绝不把卡写满。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"

#include "slice_client.h"
#include "ws_client.h"
#include "middleware/net/net_time_service.h"
#include "middleware/periph/sdcard_service.h"

static const char *TAG = "SLICE";

#define SLICE_RATE_HZ        16000
#define SLICE_BYTES_PER_S    (SLICE_RATE_HZ * 2)      /* mono s16 */
#define SLICE_RING_BYTES     (1024 * 1024)            /* ≈32.7 s */
#define SLICE_PREROLL_MS     300
#define SLICE_MAX_S          30
#define SLICE_MIN_MS         1000                     /* 太短不值得转写 */
#define SLICE_QUEUE_DEPTH    2
#define SLICE_HTTP_TIMEOUT   8000
#define SLICE_PENDING_DIR    SDCARD_MNT_PATH "/pet_memory/pending"
#define SLICE_RETRY_PERIOD   pdMS_TO_TICKS(60000)
/* SD 兜底上限：断网积压不能无限长，也不能把卡写满。
 * 64 MB / 64 条对 16k mono WAV 约等于 34 分钟语音；超过就删最旧的。 */
#define SLICE_PENDING_MAX_BYTES (64ULL * 1024 * 1024)
#define SLICE_PENDING_MAX_FILES 64
/* 卡上至少留 8 MB，避免写满 FAT / 影响其他用户数据 */
#define SLICE_PENDING_MIN_FREE  (8ULL * 1024 * 1024)
/* 超过 7 天的积压直接丢，避免坏文件/旧文件永久占卡 */
#define SLICE_PENDING_TTL_S     (7 * 24 * 60 * 60)
/* 时间没同步时 mtime 可能是 1970，不能拿来做 TTL，否则会误删全部 pending */
#define SLICE_EPOCH_VALID        1700000000ULL  /* 2023-11 */
/* 一次清理最多删 8 个，避免在音频任务里长时间阻塞 */
#define SLICE_PENDING_PRUNE_MAX 8
/* ISO8601 带 +08:00 = 25 字符 + '\0'；24 字节会触发 -Werror=format-truncation */
#define SLICE_TS_CAP         26

typedef struct {
    uint8_t *wav;          /* 44 B 头 + PCM */
    uint32_t len;
    uint32_t dur_ms;
    char     id[40];
    char     ts[SLICE_TS_CAP];
    char     stamp[20];    /* YYYYMMDDTHHMMSS，用于落盘文件名 */
} slice_item_t;

static uint8_t           *s_ring;
static SemaphoreHandle_t  s_lock;
static SemaphoreHandle_t  s_gate_lock;
static uint64_t           s_wabs;          /* 累计写入字节数（绝对） */
static uint64_t           s_start_abs;
static bool               s_active;
static QueueHandle_t      s_q;
static TaskHandle_t       s_up_task;
static TaskHandle_t       s_retry_task;

static inline size_t ring_off(uint64_t abs) { return (size_t)(abs % SLICE_RING_BYTES); }

static void now_iso(char *out, size_t cap) {
    net_time_t t = {0};
    net_time_service_get(&t);
    if (!t.valid) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
             t.year, t.mon, t.day, t.hour, t.min, t.sec);
}

static void now_stamp(char *out, size_t cap) {
    net_time_t t = {0};
    net_time_service_get(&t);
    if (!t.valid) {
        snprintf(out, cap, "19700101T000000");
        return;
    }
    snprintf(out, cap, "%04d%02d%02dT%02d%02d%02d",
             t.year, t.mon, t.day, t.hour, t.min, t.sec);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/** 在 wav[0..43] 写 16k/mono/s16 的标准头；data 长度 = pcm_bytes */
static void wav_header(uint8_t *wav, uint32_t pcm_bytes) {
    memcpy(wav + 0, "RIFF", 4);
    put_le32(wav + 4, 36 + pcm_bytes);
    memcpy(wav + 8, "WAVEfmt ", 8);
    put_le32(wav + 16, 16);              /* fmt chunk size */
    wav[20] = 1; wav[21] = 0;            /* PCM */
    wav[22] = 1; wav[23] = 0;            /* mono */
    put_le32(wav + 24, SLICE_RATE_HZ);
    put_le32(wav + 28, SLICE_RATE_HZ * 2);
    wav[32] = 2; wav[33] = 0;            /* block align */
    wav[34] = 16; wav[35] = 0;           /* bits */
    memcpy(wav + 36, "data", 4);
    put_le32(wav + 40, pcm_bytes);
}

/* ---------------------------------------------------------------- SD 兜底 */
static bool pending_prune(size_t need_bytes) {
    if (!sdcard_service_is_mounted()) {
        return false;
    }
    int removed = 0;
    for (int pass = 0; pass < SLICE_PENDING_PRUNE_MAX; pass++) {
        DIR *dir = opendir(SLICE_PENDING_DIR);
        if (dir == NULL) {
            return false;
        }
        size_t count = 0;
        uint64_t bytes = 0;
        char oldest[128] = "";
        time_t now = time(NULL);
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *name = de->d_name;
            size_t n = strlen(name);
            if (n < 8 || strcmp(name + n - 4, ".wav") != 0) {
                continue;
            }
            char path[160];
            snprintf(path, sizeof(path), SLICE_PENDING_DIR "/%s", name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            if (now > (time_t)SLICE_EPOCH_VALID &&
                st.st_mtime > (time_t)SLICE_EPOCH_VALID &&
                now > st.st_mtime &&
                (uint64_t)(now - st.st_mtime) > SLICE_PENDING_TTL_S) {
                if (unlink(path) == 0) {
                    removed++;
                    ESP_LOGW(TAG, "pending %s expired (>%ds), dropped",
                             name, SLICE_PENDING_TTL_S);
                }
                continue;
            }
            count++;
            bytes += (uint64_t)st.st_size;
            if (oldest[0] == '\0' || strcmp(name, oldest) < 0) {
                snprintf(oldest, sizeof(oldest), "%s", name);
            }
        }
        closedir(dir);

        uint64_t total = 0, freeb = 0;
        bool have_free = esp_vfs_fat_info(SDCARD_MNT_PATH, &total, &freeb) == ESP_OK;
        bool over_files = count >= SLICE_PENDING_MAX_FILES;
        bool over_bytes = (need_bytes > SLICE_PENDING_MAX_BYTES) ||
                          (bytes > SLICE_PENDING_MAX_BYTES - (uint64_t)need_bytes);
        bool over_free = have_free && freeb < (uint64_t)need_bytes + SLICE_PENDING_MIN_FREE;
        if (!over_files && !over_bytes && !over_free) {
            if (removed > 0) {
                ESP_LOGW(TAG, "pending prune: removed=%d, left=%u files/%lluKB free=%lluKB",
                         removed, (unsigned)count,
                         (unsigned long long)(bytes / 1024),
                         (unsigned long long)(freeb / 1024));
            }
            return true;
        }
        if (oldest[0] == '\0') {
            break;
        }
        char path[160];
        snprintf(path, sizeof(path), SLICE_PENDING_DIR "/%s", oldest);
        if (unlink(path) == 0) {
            removed++;
            ESP_LOGW(TAG, "pending %s pruned (full: files=%u bytes=%lluKB free=%lluKB)",
                     oldest, (unsigned)count,
                     (unsigned long long)(bytes / 1024),
                     (unsigned long long)(freeb / 1024));
        }
    }
    if (removed > 0) {
        ESP_LOGW(TAG, "pending prune stopped: removed=%d, still over limit", removed);
    }
    return false;
}

static bool ensure_pending_dir(void) {
    if (!sdcard_service_is_mounted()) {
        return false;
    }
    (void)mkdir(SDCARD_MNT_PATH "/pet_memory", 0777);
    if (mkdir(SLICE_PENDING_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", SLICE_PENDING_DIR, errno);
        return false;
    }
    return true;
}

static void save_pending(const slice_item_t *it) {
    if (!ensure_pending_dir()) {
        ESP_LOGW(TAG, "no SD, slice %s dropped (%u B)", it->id, (unsigned)it->len);
        return;
    }
    if (!pending_prune(it->len)) {
        ESP_LOGW(TAG, "SD pending full, slice %s dropped (%u B)", it->id, (unsigned)it->len);
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), SLICE_PENDING_DIR "/%s~%u~%s.wav",
             it->id, (unsigned)it->dur_ms, it->stamp);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "open %s failed: errno=%d", path, errno);
        return;
    }
    size_t n = fwrite(it->wav, 1, it->len, fp);
    fclose(fp);
    if (n != it->len) {
        ESP_LOGW(TAG, "short write %s (%u/%u)", path, (unsigned)n, (unsigned)it->len);
        (void)unlink(path);
        return;
    }
    ESP_LOGW(TAG, "slice %s spilled to SD (%u B)", it->id, (unsigned)it->len);
}

/* ---------------------------------------------------------------- HTTP 上行 */
static esp_err_t post_slice(const uint8_t *wav, uint32_t len,
                            const char *id, uint32_t dur_ms, const char *ts) {
    char base[96] = "";
    ws_client_get_http_base(base, sizeof(base));
    if (base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    const char *sid = ws_client_get_session_id();
    char ts_enc[64];
    const char *ts_q = ts;
    if (ts != NULL && ts[0]) {
        /* query 里的 '+' 会被服务端当成空格，必须编码成 %2B，
         * 否则落盘 ts 会变成 "2026-11-15T15:39:53 08:00"。 */
        size_t j = 0;
        for (size_t i = 0; ts[i] && j + 4 < sizeof(ts_enc); i++) {
            if (ts[i] == '+') {
                ts_enc[j++] = '%';
                ts_enc[j++] = '2';
                ts_enc[j++] = 'B';
            } else {
                ts_enc[j++] = ts[i];
            }
        }
        ts_enc[j] = '\0';
        ts_q = ts_enc;
    }
    char url[320];
    if (ts_q != NULL && ts_q[0]) {
        snprintf(url, sizeof(url), "%s/slice/%s?id=%s&dur_ms=%u&ts=%s",
                 base, (sid && sid[0]) ? sid : "dev", id, (unsigned)dur_ms, ts_q);
    } else {
        snprintf(url, sizeof(url), "%s/slice/%s?id=%s&dur_ms=%u",
                 base, (sid && sid[0]) ? sid : "dev", id, (unsigned)dur_ms);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SLICE_HTTP_TIMEOUT,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    esp_err_t e = esp_http_client_set_post_field(c, (const char *)wav, (int)len);
    if (e == ESP_OK) {
        e = esp_http_client_perform(c);
    }
    if (e == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "slice %s HTTP %d", id, status);
            e = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "slice %s upload failed: %s", id, esp_err_to_name(e));
    }
    esp_http_client_cleanup(c);
    return e;
}

static void free_item(slice_item_t *it) {
    if (it->wav != NULL) {
        heap_caps_free(it->wav);
        it->wav = NULL;
    }
}

static void slice_up_task(void *arg) {
    (void)arg;
    slice_item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!ws_client_is_connected() || post_slice(it.wav, it.len, it.id,
                                                    it.dur_ms, it.ts) != ESP_OK) {
            save_pending(&it);
        } else {
            ESP_LOGI(TAG, "slice %s pushed (%u B / %ums)",
                     it.id, (unsigned)it.len, (unsigned)it.dur_ms);
        }
        free_item(&it);
    }
}

/* ---------------------------------------------------------------- 补传 */
static void retry_once(void) {
    if (!sdcard_service_is_mounted()) {
        return;
    }
    /* 每次补传前先清过期/超限积压；没有新切片要写时 need_bytes=0。 */
    (void)pending_prune(0);
    DIR *dir = opendir(SLICE_PENDING_DIR);
    if (dir == NULL) {
        return;
    }
    struct dirent *de;
    int sent = 0;
    while ((de = readdir(dir)) != NULL && sent < 8) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 8 || strcmp(name + n - 4, ".wav") != 0) {
            continue;
        }
        /* <id>~<dur_ms>~<stamp>.wav */
        char tmp[96];
        snprintf(tmp, sizeof(tmp), "%s", name);
        tmp[n - 4] = '\0';
        char *sep1 = strchr(tmp, '~');
        char *sep2 = sep1 ? strchr(sep1 + 1, '~') : NULL;
        if (sep1 == NULL || sep2 == NULL) {
            continue;
        }
        *sep1 = '\0';
        *sep2 = '\0';
        const char *id = tmp;
        uint32_t dur_ms = (uint32_t)atoi(sep1 + 1);
        const char *stamp = sep2 + 1;

        char path[128];
        snprintf(path, sizeof(path), SLICE_PENDING_DIR "/%s", name);
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
            continue;
        }
        long sz = -1;
        if (fseek(fp, 0, SEEK_END) == 0) {
            sz = ftell(fp);
        }
        if (sz <= 44) {
            fclose(fp);
            continue;
        }
        uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
        if (buf == NULL) {
            fclose(fp);
            break;
        }
        fseek(fp, 0, SEEK_SET);
        size_t rd = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        if (rd != (size_t)sz) {
            heap_caps_free(buf);
            continue;
        }
        char ts[SLICE_TS_CAP] = "";
        if (stamp[0] && strlen(stamp) >= 15) {
            snprintf(ts, sizeof(ts), "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s+08:00",
                     stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11, stamp + 13);
        }
        if (ws_client_is_connected() &&
            post_slice(buf, (uint32_t)sz, id, dur_ms, ts) == ESP_OK) {
            (void)unlink(path);
            sent++;
            ESP_LOGI(TAG, "pending %s retried", id);
        }
        heap_caps_free(buf);
        if (!ws_client_is_connected()) {
            break;
        }
    }
    closedir(dir);
}

static void slice_retry_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(SLICE_RETRY_PERIOD);
        retry_once();
    }
}

/* ---------------------------------------------------------------- 对外 API */
esp_err_t slice_client_init(void) {
    if (s_ring != NULL) {
        return ESP_OK;
    }
    s_ring = heap_caps_malloc(SLICE_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "ring alloc failed (%u B)", (unsigned)SLICE_RING_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    s_gate_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(SLICE_QUEUE_DEPTH, sizeof(slice_item_t));
    if (s_lock == NULL || s_gate_lock == NULL || s_q == NULL) {
        ESP_LOGE(TAG, "sync primitive alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(slice_up_task, "slice_up", 4096, NULL, 4,
                                &s_up_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "upload task create failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(slice_retry_task, "slice_retry", 3072, NULL, 2,
                                &s_retry_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "retry task create failed (online push still works)");
    }
    ESP_LOGI(TAG, "ready: ring=%uKB, max=%ds, queue=%d",
             (unsigned)(SLICE_RING_BYTES / 1024), SLICE_MAX_S, SLICE_QUEUE_DEPTH);
    return ESP_OK;
}

bool slice_client_ready(void) { return s_ring != NULL; }

void slice_client_feed(const int16_t *pcm, size_t samples) {
    if (s_ring == NULL || pcm == NULL || samples == 0) {
        return;
    }
    size_t bytes = samples * 2;
    if (bytes > SLICE_RING_BYTES) {
        pcm += (samples - SLICE_RING_BYTES / 2);
        bytes = SLICE_RING_BYTES;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdPASS) {
        return;
    }
    size_t off = ring_off(s_wabs);
    size_t head = SLICE_RING_BYTES - off;
    if (bytes <= head) {
        memcpy(s_ring + off, pcm, bytes);
    } else {
        memcpy(s_ring + off, pcm, head);
        memcpy(s_ring, (const uint8_t *)pcm + head, bytes - head);
    }
    s_wabs += bytes;
    xSemaphoreGive(s_lock);
}

void slice_client_gate_start(void) {
    if (s_ring == NULL) {
        return;
    }
    if (xSemaphoreTake(s_gate_lock, portMAX_DELAY) != pdPASS) {
        return;
    }
    uint64_t pre = (uint64_t)((SLICE_PREROLL_MS * SLICE_BYTES_PER_S) / 1000);
    uint64_t avail = (s_wabs > SLICE_RING_BYTES) ? SLICE_RING_BYTES : s_wabs;
    uint64_t back = (pre < avail) ? pre : avail;
    s_start_abs = s_wabs - back;
    s_active = true;
    xSemaphoreGive(s_gate_lock);
    ESP_LOGI(TAG, "gate_start (preroll=%ums)", (unsigned)SLICE_PREROLL_MS);
}

void slice_client_gate_reset(void) {
    if (s_gate_lock != NULL) {
        xSemaphoreTake(s_gate_lock, portMAX_DELAY);
        s_active = false;
        xSemaphoreGive(s_gate_lock);
    } else {
        s_active = false;
    }
}

bool slice_client_gate_active(void) { return s_active; }

void slice_client_gate_end(void) {
    if (s_ring == NULL) {
        return;
    }
    if (xSemaphoreTake(s_gate_lock, portMAX_DELAY) != pdPASS) {
        return;
    }
    if (!s_active) {
        xSemaphoreGive(s_gate_lock);
        return;
    }
    s_active = false;
    uint64_t avail = (s_wabs > SLICE_RING_BYTES) ? SLICE_RING_BYTES : s_wabs;
    uint64_t take = s_wabs - s_start_abs;
    if (take > avail) {
        take = avail;
    }
    uint32_t max_bytes = (uint32_t)SLICE_MAX_S * SLICE_BYTES_PER_S;
    if (take > max_bytes) {
        take = max_bytes;
    }
    uint32_t dur_ms = (uint32_t)((take * 1000) / SLICE_BYTES_PER_S);
    if (take < (uint32_t)(SLICE_MIN_MS * SLICE_BYTES_PER_S / 1000)) {
        xSemaphoreGive(s_gate_lock);
        ESP_LOGI(TAG, "gate_end too short (%ums), dropped", (unsigned)dur_ms);
        return;
    }

    slice_item_t it = {0};
    it.len = (uint32_t)(44 + take);
    it.wav = heap_caps_malloc(it.len, MALLOC_CAP_SPIRAM);
    if (it.wav == NULL) {
        xSemaphoreGive(s_gate_lock);
        ESP_LOGW(TAG, "slice alloc failed (%u B), dropped", (unsigned)it.len);
        return;
    }
    wav_header(it.wav, (uint32_t)take);

    uint64_t begin = s_wabs - take;
    size_t off = ring_off(begin);
    size_t head = SLICE_RING_BYTES - off;
    if (take <= head) {
        memcpy(it.wav + 44, s_ring + off, take);
    } else {
        memcpy(it.wav + 44, s_ring + off, head);
        memcpy(it.wav + 44 + head, s_ring, take - head);
    }
    xSemaphoreGive(s_gate_lock);

    it.dur_ms = dur_ms;
    now_stamp(it.stamp, sizeof(it.stamp));
    snprintf(it.id, sizeof(it.id), "evt_%s_%02u", it.stamp,
             (unsigned)(esp_random() % 100));
    now_iso(it.ts, sizeof(it.ts));

    if (xQueueSend(s_q, &it, 0) != pdPASS) {
        ESP_LOGW(TAG, "upload queue full -> SD");
        save_pending(&it);
        free_item(&it);
        return;
    }
    ESP_LOGI(TAG, "slice queued: %s %ums %uB", it.id, (unsigned)dur_ms, (unsigned)it.len);
}
