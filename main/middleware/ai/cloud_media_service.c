/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cloud_media_service.c
 * @brief 云端图片下载/落盘/解码实现（见 cloud_media_service.h）
 *
 * 任务模型：**按需建、跑完自删**（不常驻）。理由与本工程其它一次性任务一致：
 *   下载生图是低频动作，常驻一个 6 KB 内部栈纯属浪费（内部 RAM 只剩 ~4 KB 余量）。
 *
 * 栈必须**内部 RAM**（F35）：任务里要 `fopen/fwrite` SD 卡 —— IDF v6.1 在每次
 *   写/擦 flash 前断言"当前任务 SP 在内部 DRAM"，没有 API 豁免。
 *
 * 解码尺寸：4:2:2 JPEG 的硬件解码输出宽度会被补齐到 16 的倍数
 *   （`driver/jpeg_decode.h` note 2），所以 w/h 来自 SOF 解析、stride 来自
 *   补齐后的宽度 —— UI 侧用 lv_image 的 stride 字段对齐，不会错位。
 */

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "driver/jpeg_decode.h"

#include "cloud_media_service.h"
#include "middleware/periph/sdcard_service.h"

static const char *TAG = "CMEDIA";

#define CMEDIA_STACK      6144
#define CMEDIA_PRIO       4
#define CMEDIA_Q_DEPTH    3
#define CMEDIA_URL_MAX    256

typedef struct {
    char url[CMEDIA_URL_MAX];
    char caption[CLOUD_MEDIA_CAPTION_MAX];
} cmedia_req_t;

static QueueHandle_t s_q;
static TaskHandle_t  s_task;
static volatile bool s_busy;

/* 已解码、等待 UI 取走的一帧（所有权转移给 UI） */
static uint8_t *s_pub;
static int      s_pub_w, s_pub_h, s_pub_stride;
static char     s_pub_cap[CLOUD_MEDIA_CAPTION_MAX];

static char s_last_err[64];

/* ============================================================
 *  目录裁剪（F71）：表情包是"当场看"的临时展示物，落盘只为 FILES 页
 *  能浏览到（P6 判据），没有保留价值。保留最近 CLOUD_MEDIA_KEEP 张，
 *  每次落盘成功后数一遍目录，超了就删最老的一张 —— 数量稳态封顶 KEEP，
 *  不会随下发次数无界增长。跑在 cmedia_run 专用任务里（SD IO 合法）。
 * ============================================================ */
#define CLOUD_MEDIA_KEEP   32       /* ~32 张 × ≤150KB ≈ 3MB 封顶 */

static void cmedia_prune(void)
{
    DIR *dir = opendir(CLOUD_MEDIA_DIR);
    if (dir == NULL) {
        return;
    }
    int n = 0;
    char oldest[96] = "";
    time_t oldest_mt = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".jpg") != 0) {
            continue;
        }
        char p[96];
        if (snprintf(p, sizeof(p), CLOUD_MEDIA_DIR "/%s", de->d_name) >= (int)sizeof(p)) {
            continue;
        }
        struct stat st;
        if (stat(p, &st) != 0) {
            continue;
        }
        n++;
        if (oldest[0] == '\0' || st.st_mtime < oldest_mt) {
            strlcpy(oldest, p, sizeof(oldest));
            oldest_mt = st.st_mtime;
        }
    }
    closedir(dir);
    if (n > CLOUD_MEDIA_KEEP && oldest[0]) {
        if (unlink(oldest) == 0) {
            ESP_LOGI(TAG, "pruned oldest: %s (%d kept)", oldest, n - 1);
        } else {
            ESP_LOGW(TAG, "prune unlink %s failed errno=%d", oldest, errno);
        }
    }
}

/* ============================================================
 *  JPEG 尺寸解析（只认 SOF0/1/2 等，跳过 C4/C8/CC）
 * ============================================================ */
static bool jpeg_dims(const uint8_t *d, size_t n, int *w, int *h)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 9 < n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        if (m == 0xD9 || m == 0xDA) {
            break;
        }
        size_t seg = ((size_t)d[i + 2] << 8) | d[i + 3];
        bool sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof) {
            if (i + 9 >= n) {
                return false;
            }
            *h = (int)(((uint16_t)d[i + 5] << 8) | d[i + 6]);
            *w = (int)(((uint16_t)d[i + 7] << 8) | d[i + 8]);
            return (*w > 0 && *h > 0);
        }
        i += 2 + seg;
    }
    return false;
}

/* ============================================================
 *  HTTP 下载到 PSRAM
 * ============================================================ */
static uint8_t *http_download(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* http 时不会被调用 */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return NULL;
    }
    uint8_t *buf = NULL;
    size_t total = 0;
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "http open failed");
        ESP_LOGW(TAG, "open failed: %s", url);
        esp_http_client_cleanup(cli);
        return NULL;
    }
    int clen = esp_http_client_fetch_headers(cli);
    if (clen > 0) {
        total = (size_t)clen;
        if (total > CLOUD_MEDIA_MAX_BYTES) {
            snprintf(s_last_err, sizeof(s_last_err), "too large (%u B)", (unsigned)total);
            ESP_LOGW(TAG, "%s: content-length %u > cap %u", url, (unsigned)total,
                     CLOUD_MEDIA_MAX_BYTES);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return NULL;
        }
        buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            snprintf(s_last_err, sizeof(s_last_err), "no mem (%u B)", (unsigned)total);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return NULL;
        }
    } else {
        /* 无 Content-Length：按 64 KB 增长（上限同样受 CLOUD_MEDIA_MAX_BYTES 约束） */
        total = 64 * 1024;
        buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            snprintf(s_last_err, sizeof(s_last_err), "no mem");
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return NULL;
        }
    }

    size_t got = 0;
    for (;;) {
        if (got == total) {                    /* 需要扩容 */
            if (total >= CLOUD_MEDIA_MAX_BYTES) {
                snprintf(s_last_err, sizeof(s_last_err), "too large");
                got = 0;
                break;
            }
            size_t want = total * 2;
            if (want > CLOUD_MEDIA_MAX_BYTES) {
                want = CLOUD_MEDIA_MAX_BYTES;
            }
            uint8_t *nb = heap_caps_realloc(buf, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == NULL) {
                snprintf(s_last_err, sizeof(s_last_err), "no mem (grow)");
                got = 0;
                break;
            }
            buf = nb;
            total = want;
        }
        int rd = esp_http_client_read(cli, (char *)buf + got, (int)(total - got));
        if (rd < 0) {
            snprintf(s_last_err, sizeof(s_last_err), "read failed");
            got = 0;
            break;
        }
        if (rd == 0) {
            break;                             /* EOF */
        }
        got += (size_t)rd;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (got == 0) {
        heap_caps_free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

/* ============================================================
 *  一次完整的取图事务
 * ============================================================ */
static void cmedia_run(const cmedia_req_t *req)
{
    size_t jlen = 0;
    uint8_t *jpg = http_download(req->url, &jlen);
    if (jpg == NULL) {
        ESP_LOGW(TAG, "download failed: %s", req->url);
        return;
    }

    /* ① 落盘（FILES 页要能浏览到 —— P6 验收判据） */
    if (sdcard_service_is_mounted()) {
        mkdir(CLOUD_MEDIA_DIR, 0775);
        static uint32_t seq;
        char path[96];
        snprintf(path, sizeof(path), CLOUD_MEDIA_DIR "/img_%03u.jpg", (unsigned)(++seq % 1000));
        FILE *f = fopen(path, "wb");
        if (f) {
            size_t wr = fwrite(jpg, 1, jlen, f);
            fclose(f);
            ESP_LOGI(TAG, "saved %s (%u/%u B)", path, (unsigned)wr, (unsigned)jlen);
            cmedia_prune();     /* F71：数量封顶，删最老 */
        } else {
            ESP_LOGW(TAG, "fopen %s failed errno=%d", path, errno);
        }
    } else {
        ESP_LOGW(TAG, "SD not mounted, skip save");
    }

    /* ② 解码 */
    int w = 0, h = 0;
    if (!jpeg_dims(jpg, jlen, &w, &h)) {
        snprintf(s_last_err, sizeof(s_last_err), "bad jpeg");
        ESP_LOGW(TAG, "not a jpeg (%u B)", (unsigned)jlen);
        heap_caps_free(jpg);
        return;
    }
    if ((int64_t)w * h > CLOUD_MEDIA_MAX_PIXELS) {
        snprintf(s_last_err, sizeof(s_last_err), "too big %dx%d", w, h);
        ESP_LOGW(TAG, "image %dx%d exceeds pixel cap, skipped", w, h);
        heap_caps_free(jpg);
        return;
    }
    int pw = (w + 15) & ~15;                   /* 硬件解码补齐到 16 */
    int ph = (h + 15) & ~15;
    size_t out_bytes = (size_t)pw * (size_t)ph * 2u;

    jpeg_decode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t out_cap = 0;
    uint8_t *out = jpeg_alloc_decoder_mem(out_bytes, &mcfg, &out_cap);
    if (out == NULL) {
        snprintf(s_last_err, sizeof(s_last_err), "decode mem %u B", (unsigned)out_bytes);
        ESP_LOGE(TAG, "decoder mem alloc failed (%u B)", (unsigned)out_bytes);
        heap_caps_free(jpg);
        return;
    }

    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 400 };
    esp_err_t err = jpeg_new_decoder_engine(&ecfg, &dec);
    if (err != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "decoder engine");
        ESP_LOGE(TAG, "jpeg_new_decoder_engine: %s", esp_err_to_name(err));
        heap_caps_free(out);
        heap_caps_free(jpg);
        return;
    }
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* 与 viewfinder 同款，勿改 */
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(dec, &dcfg, jpg, (uint32_t)jlen, out, out_cap, &out_size);
    (void)jpeg_del_decoder_engine(dec);
    heap_caps_free(jpg);
    if (err != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "decode failed");
        ESP_LOGW(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
        heap_caps_free(out);
        return;
    }

    /* ③ 发布（顶掉旧帧） */
    if (s_pub) {
        heap_caps_free(s_pub);
    }
    s_pub = out;
    s_pub_w = w;
    s_pub_h = h;
    s_pub_stride = pw * 2;
    snprintf(s_pub_cap, sizeof(s_pub_cap), "%s", req->caption);
    s_last_err[0] = '\0';
    ESP_LOGI(TAG, "published %dx%d (stride %d, dec %u B)", w, h, s_pub_stride,
             (unsigned)out_size);
}

static void cmedia_task(void *arg)
{
    (void)arg;
    cmedia_req_t req;
    s_busy = true;
    while (xQueueReceive(s_q, &req, 0) == pdPASS) {
        cmedia_run(&req);
    }
    s_busy = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ============================================================
 *  Public API
 * ============================================================ */

void cloud_media_service_init(void)
{
    if (s_q) {
        return;
    }
    s_q = xQueueCreate(CMEDIA_Q_DEPTH, sizeof(cmedia_req_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    s_last_err[0] = '\0';
    ESP_LOGI(TAG, "ready (dir %s, cap %u B / %u px)", CLOUD_MEDIA_DIR,
             CLOUD_MEDIA_MAX_BYTES, CLOUD_MEDIA_MAX_PIXELS);
}

esp_err_t cloud_media_service_fetch(const char *url, const char *caption)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(url) >= CMEDIA_URL_MAX) {
        snprintf(s_last_err, sizeof(s_last_err), "url too long");
        return ESP_ERR_INVALID_SIZE;
    }
    cmedia_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.url, sizeof(req.url), "%s", url);
    if (caption) {
        snprintf(req.caption, sizeof(req.caption), "%s", caption);
    }
    if (xQueueSend(s_q, &req, 0) != pdPASS) {
        snprintf(s_last_err, sizeof(s_last_err), "busy (queue full)");
        ESP_LOGW(TAG, "queue full, dropped %s", url);
        return ESP_ERR_NO_MEM;
    }
    if (!s_busy && s_task == NULL) {
        if (xTaskCreate(cmedia_task, "cmedia", CMEDIA_STACK, NULL, CMEDIA_PRIO,
                        &s_task) != pdPASS) {
            s_task = NULL;
            snprintf(s_last_err, sizeof(s_last_err), "task create");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "queued %s", url);
    return ESP_OK;
}

bool cloud_media_service_take(uint8_t **rgb565, int *w, int *h, int *stride,
                              char *caption, size_t cap_size)
{
    if (s_pub == NULL) {
        return false;
    }
    *rgb565 = s_pub;
    *w = s_pub_w;
    *h = s_pub_h;
    *stride = s_pub_stride;
    if (caption && cap_size) {
        snprintf(caption, cap_size, "%s", s_pub_cap);
    }
    s_pub = NULL;                 /* 所有权转移给 UI */
    return true;
}

const char *cloud_media_service_last_error(void)
{
    return s_last_err;
}
