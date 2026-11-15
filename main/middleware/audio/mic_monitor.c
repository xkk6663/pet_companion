/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mic_monitor.c
 * @brief 双麦采集状态可视化后端（见 .h 的线程纪律）
 *
 * 关键取舍：
 *   · ingest 只 memcpy + 入队，FFT 在 mic_mon 任务里做，避免拖慢
 *     rec_task（1536 B 栈）/ AFE feed 任务；
 *   · 队列只存 3 个 slot 的索引，PCM 缓冲在 PSRAM，队列满丢当前帧；
 *   · 频带做快起慢落平滑，UI 20 fps 读快照即可，不用每帧重算。
 */

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mic_monitor.h"

static const char *TAG = "MIC_MON";

#define MIC_MON_SLOTS          3
#define MIC_MON_FRAME_BYTES    (MIC_MON_FFT * 2 * (int)sizeof(int16_t))
#define MIC_MON_TWO_PI         6.283185307179586f
#define MIC_MON_FULL_SCALE     32768.0f
#define MIC_MON_DB_FLOOR       (-70.0f)
#define MIC_MON_DB_CEIL        (-35.0f)   /* 单个 FFT bin 的语音峰值多在 -45~-55，收紧上限让柱子更明显 */
#define MIC_MON_VAD_ON_DB      (-40.0f)   /* 本地代理：高于此算“有话” */
#define MIC_MON_VAD_HOLD_US    (1000 * 1000) /* 低于门限后 1 s 才落 */

static QueueHandle_t s_free_q;
static QueueHandle_t s_data_q;
static uint8_t      *s_slots;
static float        *s_fft;
static float        *s_win;
static uint16_t      s_bin_lo[MIC_MON_BANDS];
static uint16_t      s_bin_hi[MIC_MON_BANDS];
static float         s_smooth_l[MIC_MON_BANDS];
static float         s_smooth_r[MIC_MON_BANDS];
static bool          s_ready;
static volatile bool s_active;
static volatile bool s_reset_smooth;
static int64_t       s_last_voice_us;
static bool          s_local_vad;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static mic_monitor_snapshot_t s_snap;

static void mic_mon_task(void *arg);

/* ============================================================
 * 初始化
 * ============================================================ */

/* 自带 radix-2 FFT：不依赖芯片优化分支，数值行为完全可控。
 * data 为交错复数组 [re, im, ...]，N 必须是 2 的幂。 */
static void fft_radix2(float *data, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = data[2 * i];
            float ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = -MIC_MON_TWO_PI / (float)len;
        float wl_r = cosf(ang);
        float wl_i = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int a = i + j;
                int b = a + len / 2;
                float br = data[2 * b];
                float bi = data[2 * b + 1];
                float tr = wr * br - wi * bi;
                float ti = wr * bi + wi * br;
                data[2 * b] = data[2 * a] - tr;
                data[2 * b + 1] = data[2 * a + 1] - ti;
                data[2 * a] += tr;
                data[2 * a + 1] += ti;
                float next_wr = wr * wl_r - wi * wl_i;
                wi = wr * wl_i + wi * wl_r;
                wr = next_wr;
            }
        }
    }
}

static void bands_init(void)
{
    /* 100 Hz .. 8 kHz 对数分带；每带至少一个 FFT bin。 */
    float ratio = powf(8000.0f / 100.0f, 1.0f / (float)MIC_MON_BANDS);
    for (int b = 0; b < MIC_MON_BANDS; b++) {
        float f0 = 100.0f * powf(ratio, (float)b);
        float f1 = 100.0f * powf(ratio, (float)(b + 1));
        int lo = (int)(f0 * (float)MIC_MON_FFT / (float)MIC_MON_SAMPLE_RATE);
        int hi = (int)(f1 * (float)MIC_MON_FFT / (float)MIC_MON_SAMPLE_RATE);
        if (lo < 1) {
            lo = 1;
        }
        if (lo >= MIC_MON_FFT / 2) {
            lo = MIC_MON_FFT / 2 - 1;
        }
        if (hi > MIC_MON_FFT / 2) {
            hi = MIC_MON_FFT / 2;
        }
        if (hi <= lo) {
            hi = lo + 1;
        }
        s_bin_lo[b] = (uint16_t)lo;
        s_bin_hi[b] = (uint16_t)hi;
    }
}

esp_err_t mic_monitor_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_slots = heap_caps_malloc(MIC_MON_SLOTS * MIC_MON_FRAME_BYTES,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fft = heap_caps_malloc(MIC_MON_FFT * 2 * sizeof(float),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_win = heap_caps_malloc(MIC_MON_FFT * sizeof(float),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots == NULL || s_fft == NULL || s_win == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed (slots=%p fft=%p win=%p)",
                 (void *)s_slots, (void *)s_fft, (void *)s_win);
        goto _fail;
    }

    bands_init();
    for (int i = 0; i < MIC_MON_FFT; i++) {
        /* Hann：避免块边界泄漏把频谱画成满格 */
        s_win[i] = 0.5f - 0.5f * cosf(MIC_MON_TWO_PI * (float)i /
                                      (float)(MIC_MON_FFT - 1));
    }

    s_free_q = xQueueCreate(MIC_MON_SLOTS, sizeof(uint8_t));
    s_data_q = xQueueCreate(MIC_MON_SLOTS, sizeof(uint8_t));
    if (s_free_q == NULL || s_data_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        goto _fail;
    }
    for (uint8_t i = 0; i < MIC_MON_SLOTS; i++) {
        (void)xQueueSend(s_free_q, &i, 0);
    }

    s_snap.seq = 0;
    s_snap.valid = 0;
    s_local_vad = false;
    s_last_voice_us = esp_timer_get_time();

    if (xTaskCreate(mic_mon_task, "mic_mon", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "mic_mon task create failed");
        goto _fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready (fft=%d bands=%d, PSRAM slots=%d)",
             MIC_MON_FFT, MIC_MON_BANDS, MIC_MON_SLOTS);
    return ESP_OK;

_fail:
    if (s_free_q) {
        vQueueDelete(s_free_q);
        s_free_q = NULL;
    }
    if (s_data_q) {
        vQueueDelete(s_data_q);
        s_data_q = NULL;
    }
    if (s_slots) {
        heap_caps_free(s_slots);
        s_slots = NULL;
    }
    if (s_fft) {
        heap_caps_free(s_fft);
        s_fft = NULL;
    }
    if (s_win) {
        heap_caps_free(s_win);
        s_win = NULL;
    }
    return ESP_ERR_NO_MEM;
}

/* ============================================================
 * 对外
 * ============================================================ */

void mic_monitor_set_active(bool active)
{
    if (s_active == active) {
        return;
    }
    s_active = active;
    if (active) {
        s_reset_smooth = true;
        /* 打开时先作废旧快照：切回 REC 页不会先闪一帧上一次的数据。 */
        portENTER_CRITICAL(&s_lock);
        s_snap.valid = 0;
        s_snap.seq++;
        portEXIT_CRITICAL(&s_lock);
    }
}

bool mic_monitor_is_active(void)
{
    return s_active;
}

void mic_monitor_ingest(const int16_t *interleaved, int frames)
{
    if (!s_ready || !s_active || interleaved == NULL || frames <= 0) {
        return;
    }

    uint8_t idx;
    if (xQueueReceive(s_free_q, &idx, 0) != pdPASS) {
        return;                 /* 分析任务没跟上：丢当前帧，绝不等待 */
    }

    uint8_t *slot = s_slots + (size_t)idx * MIC_MON_FRAME_BYTES;
    int use = frames > MIC_MON_FFT ? MIC_MON_FFT : frames;
    int off = (frames - use) * 2;   /* 取最新的一段；不足 FFT 点时补零 */
    memset(slot, 0, MIC_MON_FRAME_BYTES);
    memcpy(slot, interleaved + off,
           (size_t)use * 2 * sizeof(int16_t));

    if (xQueueSend(s_data_q, &idx, 0) != pdPASS) {
        (void)xQueueSend(s_free_q, &idx, 0);
    }
}

bool mic_monitor_read(mic_monitor_snapshot_t *out)
{
    if (!s_ready || out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    portEXIT_CRITICAL(&s_lock);
    return out->valid != 0;
}

/* ============================================================
 * 分析任务
 * ============================================================ */

static int8_t rms_to_dbfs(double sum_sq, int n)
{
    double rms = sqrtf((float)(sum_sq / (double)n));
    double db = 20.0 * log10(rms / 32768.0 + 1e-12);
    if (db < -80.0) {
        db = -80.0;
    }
    if (db > 0.0) {
        db = 0.0;
    }
    return (int8_t)lrintf((float)db);
}

static void fft_channel_bands(const int16_t *pcm, int ch,
                              float *smooth, uint8_t *out)
{
    for (int i = 0; i < MIC_MON_FFT; i++) {
        s_fft[2 * i] = (float)pcm[2 * i + ch] * s_win[i];
        s_fft[2 * i + 1] = 0.0f;
    }

    /* 软件 radix-2：输入自然序、输出自然序，不再依赖 esp-dsp 的
     * 位反转表/优化分支，数值可复现。 */
    fft_radix2(s_fft, MIC_MON_FFT);

    for (int b = 0; b < MIC_MON_BANDS; b++) {
        float max_mag = 0.0f;
        for (uint16_t k = s_bin_lo[b]; k < s_bin_hi[b]; k++) {
            float re = s_fft[2 * k];
            float im = s_fft[2 * k + 1];
            float mag = sqrtf(re * re + im * im);
            if (mag > max_mag) {
                max_mag = mag;
            }
        }

        /* 幅度归一化后转 dBFS；Hann 的相干增益不再补偿，只用于相对显示。 */
        float db = 20.0f * log10f((2.0f * max_mag /
                                   ((float)MIC_MON_FFT * MIC_MON_FULL_SCALE)) +
                                  1e-12f);
        if (db < MIC_MON_DB_FLOOR) {
            db = MIC_MON_DB_FLOOR;
        }
        if (db > MIC_MON_DB_CEIL) {
            db = MIC_MON_DB_CEIL;
        }
        float v = (db - MIC_MON_DB_FLOOR) * 100.0f /
                  (MIC_MON_DB_CEIL - MIC_MON_DB_FLOOR);

        /* 快起慢落：说话时立起来，停顿后缓慢回落，便于观察。 */
        if (v > smooth[b]) {
            smooth[b] = smooth[b] * 0.35f + v * 0.65f;
        } else {
            smooth[b] = smooth[b] * 0.85f + v * 0.15f;
        }
        out[b] = (uint8_t)(smooth[b] + 0.5f);
    }
}

static void process_frame(const uint8_t *slot)
{
    const int16_t *pcm = (const int16_t *)(const void *)slot;
    double sum_l = 0.0, sum_r = 0.0;

    for (int i = 0; i < MIC_MON_FFT; i++) {
        int32_t l = pcm[2 * i];
        int32_t r = pcm[2 * i + 1];
        sum_l += (double)l * l;
        sum_r += (double)r * r;
    }
    int8_t db_l = rms_to_dbfs(sum_l, MIC_MON_FFT);
    int8_t db_r = rms_to_dbfs(sum_r, MIC_MON_FFT);

    if (s_reset_smooth) {
        memset(s_smooth_l, 0, sizeof(s_smooth_l));
        memset(s_smooth_r, 0, sizeof(s_smooth_r));
        s_reset_smooth = false;
    }

    uint8_t band_l[MIC_MON_BANDS];
    uint8_t band_r[MIC_MON_BANDS];
    fft_channel_bands(pcm, 0, s_smooth_l, band_l);
    fft_channel_bands(pcm, 1, s_smooth_r, band_r);

    int64_t now = esp_timer_get_time();
    int max_db = (db_l > db_r) ? db_l : db_r;
    if ((float)max_db >= MIC_MON_VAD_ON_DB) {
        s_local_vad = true;
        s_last_voice_us = now;
    } else if (s_local_vad && now - s_last_voice_us >= MIC_MON_VAD_HOLD_US) {
        s_local_vad = false;
    }

    portENTER_CRITICAL(&s_lock);
    s_snap.seq++;
    s_snap.db_l = db_l;
    s_snap.db_r = db_r;
    s_snap.local_vad = s_local_vad ? 1 : 0;
    memcpy(s_snap.band_l, band_l, sizeof(band_l));
    memcpy(s_snap.band_r, band_r, sizeof(band_r));
    s_snap.valid = 1;
    portEXIT_CRITICAL(&s_lock);
}

static void mic_mon_task(void *arg)
{
    (void)arg;
    uint8_t idx;
    for (;;) {
        if (xQueueReceive(s_data_q, &idx, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (s_active) {
            process_frame(s_slots + (size_t)idx * MIC_MON_FRAME_BYTES);
        }
        (void)xQueueSend(s_free_q, &idx, 0);
    }
}
