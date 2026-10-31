/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file voice_service.c
 * @brief PTT 上行实现（见 voice_service.h 的设计说明）
 *
 * 缓冲布局（一块 PSRAM，一次分配）：
 *
 *   0                PRE_LEN                    PRE_LEN+cap            +64
 *   |-- JSON 前缀 --|------ base64 正文 ------|-- 后缀（动态写） --|NUL|
 *
 *   PRE  = {"type":"pcm","data":"            （编译期定长）
 *   后缀 = ","sample_rate":16000,"duration_ms":%u}   ← ★ 不能补零（F52）
 *
 * base64 每块输入都保证是 **3 的倍数**（不足 3 字节的尾巴留在 s_rem 里跟下一块
 * 拼），这样"分块编码再拼接"与"整体编码"字节级等价 —— 否则中间会出现 '=' 填充，
 * 服务端 base64.b64decode 会直接报错。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "voice_service.h"
#include "ws_client.h"

static const char *TAG = "VOICE";

static const char PRE[] = "{\"type\":\"pcm\",\"data\":\"";
#define PRE_LEN   (sizeof(PRE) - 1)
/* 320000/3 = 106666 组 → 426664 字符（+ 余数最多 +4） */
#define B64_CAP   ((VOICE_MAX_PCM_BYTES / 3 + 1) * 4)
#define BUF_CAP   (PRE_LEN + B64_CAP + 64)

/* 单块编码的原始字节数（3 的倍数；1920 = 60 ms @16k/mono） */
#define ENC_CHUNK 1920

static char    *s_buf;          /* PSRAM：整帧（前缀 + base64 + 后缀） */
static uint8_t *s_stage;        /* PSRAM：单块拼接暂存（ENV_CHUNK+2） */
static size_t   s_b64_len;      /* 已写入的 base64 字符数 */
static size_t   s_pcm_bytes;    /* 已累积的 PCM 字节数（算时长用） */
static uint8_t  s_rem[2];       /* base64 分组余数（0~2 字节） */
static size_t   s_rem_len;
static volatile bool s_active;
static volatile bool s_overflow;

esp_err_t voice_service_init(void)
{
    if (s_buf) {
        return ESP_OK;
    }
    s_buf = heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_stage = heap_caps_malloc(ENC_CHUNK + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL || s_stage == NULL) {
        ESP_LOGE(TAG, "alloc failed (buf=%p stage=%p, need %u B PSRAM)",
                 s_buf, s_stage, (unsigned)(BUF_CAP + ENC_CHUNK + 2));
        if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
        if (s_stage) { heap_caps_free(s_stage); s_stage = NULL; }
        return ESP_ERR_NO_MEM;
    }
    s_b64_len = 0;
    s_pcm_bytes = 0;
    s_rem_len = 0;
    s_active = false;
    s_overflow = false;
    memcpy(s_buf, PRE, PRE_LEN);
    s_buf[PRE_LEN] = '\0';
    ESP_LOGI(TAG, "ready: frame cap %u B PSRAM (max %d s PCM)",
             (unsigned)BUF_CAP, VOICE_MAX_PCM_SECONDS);
    return ESP_OK;
}

void voice_service_deinit(void)
{
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    if (s_stage) {
        heap_caps_free(s_stage);
        s_stage = NULL;
    }
    s_b64_len = 0;
    s_pcm_bytes = 0;
    s_rem_len = 0;
    s_active = false;
    s_overflow = false;
}

bool voice_service_ready(void)
{
    return s_buf != NULL;
}

void voice_service_ptt_begin(void)
{
    if (s_buf == NULL) {
        return;
    }
    s_b64_len = 0;
    s_pcm_bytes = 0;
    s_rem_len = 0;
    s_overflow = false;
    memcpy(s_buf, PRE, PRE_LEN);
    s_buf[PRE_LEN] = '\0';
    s_active = true;
}

static void append_b64(const uint8_t *src, size_t len)
{
    /* 目标空间：留 64 B 给后缀与 NUL */
    size_t room = B64_CAP - s_b64_len;
    if (room < 8) {
        if (!s_overflow) {
            ESP_LOGW(TAG, "pcm buffer full at %u B, rest truncated (R8)",
                     (unsigned)s_pcm_bytes);
        }
        s_overflow = true;
        return;
    }
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)s_buf + PRE_LEN + s_b64_len,
                                   room, &olen, src, len);
    if (rc != 0) {
        s_overflow = true;
        return;
    }
    s_b64_len += olen;
    s_buf[PRE_LEN + s_b64_len] = '\0';
}

void voice_service_ptt_feed(const uint8_t *pcm, size_t len)
{
    if (!s_active || s_buf == NULL || pcm == NULL || len == 0) {
        return;
    }
    if (s_pcm_bytes >= VOICE_MAX_PCM_BYTES) {
        return;                     /* 已到 10 s 上限：直接丢，不发超长帧 */
    }
    if (s_pcm_bytes + len > VOICE_MAX_PCM_BYTES) {
        len = VOICE_MAX_PCM_BYTES - s_pcm_bytes;
    }
    s_pcm_bytes += len;

    while (len > 0) {
        size_t take = (len > ENC_CHUNK) ? ENC_CHUNK : len;
        /* 拼上一块留下的 0~2 B 余数，凑成 3 的倍数再编码 */
        memcpy(s_stage, s_rem, s_rem_len);
        memcpy(s_stage + s_rem_len, pcm, take);
        size_t total = s_rem_len + take;
        size_t usable = total - (total % 3);
        if (usable > 0) {
            append_b64(s_stage, usable);
        }
        s_rem_len = total - usable;
        if (s_rem_len > 0) {
            memcpy(s_rem, s_stage + usable, s_rem_len);
        }
        pcm += take;
        len -= take;
    }
}

esp_err_t voice_service_ptt_finish(void)
{
    if (!s_active || s_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false;

    /* 尾巴不足 3 字节：补零凑一组（16 bit 采样，单个字节本身就是数据损坏，
     * 补 0 只会影响最后 1 个样本的低/高字节，ASR 无感） */
    if (s_rem_len > 0) {
        memset(s_stage, 0, 3);
        memcpy(s_stage, s_rem, s_rem_len);
        append_b64(s_stage, 3);
        s_rem_len = 0;
    }

    if (s_pcm_bytes < VOICE_MIN_PCM_BYTES) {
        ESP_LOGW(TAG, "too short (%u B < %d), dropped",
                 (unsigned)s_pcm_bytes, VOICE_MIN_PCM_BYTES);
        s_b64_len = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t dur_ms = (uint32_t)(s_pcm_bytes / 32u);   /* /2(B) /16(kHz) *1000 */
    if (dur_ms > VOICE_MAX_PCM_SECONDS * 1000u) {
        dur_ms = VOICE_MAX_PCM_SECONDS * 1000u;
    }
    /* ★★★ 必须是 %u，**绝不能**是 %06u（F52，2026-10-31 实机踩到）：
     *   JSON 数字不允许前导零，`"duration_ms":000840` 会让 python json.loads
     *   抛 "Expecting ',' delimiter"，网关把它当"非 JSON 行"直接丢弃**且不回任何东西**
     *   ⇒ 现象是"按住 MODE 说话，云端毫无反应"，而日志里 pcm up 还是 ESP_OK。
     *   固定宽度对"零搬运"没有作用（b64 长度用的是 s_b64_len，不是反推）。 */
    size_t at = PRE_LEN + s_b64_len;
    int n = snprintf(s_buf + at, BUF_CAP - at,
                     "\",\"sample_rate\":16000,\"duration_ms\":%u}", (unsigned)dur_ms);
    if (n <= 0 || (size_t)n >= BUF_CAP - at) {
        ESP_LOGE(TAG, "suffix overflow (b64=%u)", (unsigned)s_b64_len);
        s_b64_len = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ws_client_send_text(s_buf);
    ESP_LOGI(TAG, "pcm up: %u B pcm / %u B b64 / %u ms -> %s%s",
             (unsigned)s_pcm_bytes, (unsigned)s_b64_len, (unsigned)dur_ms,
             esp_err_to_name(err), s_overflow ? " (truncated)" : "");
    s_b64_len = 0;
    return err;
}

void voice_service_ptt_cancel(void)
{
    s_active = false;
    s_b64_len = 0;
    s_pcm_bytes = 0;
    s_rem_len = 0;
    s_overflow = false;
}

size_t voice_service_ptt_bytes(void)
{
    return s_pcm_bytes;
}

bool voice_service_ptt_active(void)
{
    return s_active;
}
