/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mic_gate.c
 * @brief S10 P1/P2：VAD 常开闸门（见 mic_gate.h）
 *
 * ★ 所有权只有一个：本模块持有期间，AI 会话 / 本地录音 / 音乐播放都要先
 *   调 `mic_gate_yield()`（已在三处入口接好），失败就按"设备忙"处理，
 *   **绝不并发开第二个 recorder**（audio_service 的独占租约是硬闸）。
 *
 * ★ 初始化序列刻意与 `xiaozhi_service.c:session_setup()` 同构：
 *   claim → mic_fe_init → audio_manager_init → open spk/mic → attach →
 *   recorder_open。顺序错任何一步都会出现"能录但 AFE 不工作"或"左右声道错位"。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "audio_processor.h"
#include "esp_gmf_afe.h"
#include "esp_codec_dev.h"
#include "audio_service.h"
#include "mic_gate.h"
#include "mic_frontend.h"
#include "middleware/ai/xiaozhi_service.h"
#include "middleware/audio/media_player_service.h"
#include "middleware/audio/rec_local_service.h"
#include "middleware/ai/slice_client.h"

static const char *TAG = "MIC_GATE";

#define GATE_SAMPLE_RATE   16000
#define GATE_BITS          16
#define GATE_FRAME_MS      60
#define GATE_READ_BYTES    (1920 * 2)      /* 60ms@16k 的 2 倍余量（同 xiaozhi） */
#define GATE_TASK_CORE     0
#define GATE_TASK_PRIO     6
#define GATE_MAX_SLICE_MS  30000
#define GATE_MERGE_MS      1200      /* VAD_END 后再等 1.2s，下一句进来就并入同一段 */
#define GATE_YIELD_WAIT_MS 300

static volatile bool s_enabled = (MIC_GATE_ENABLE != 0);
static volatile bool s_owner;
static volatile bool s_run;
static TaskHandle_t  s_task;
static audio_recorder_handle_t s_rec;
static esp_codec_dev_handle_t  s_spk;
static esp_codec_dev_handle_t  s_mic;
static uint8_t      *s_buf;
static int64_t       s_vad_start_us;
static int64_t       s_vad_end_us;
static int64_t       s_slice_start_us;
static volatile bool s_merge_pending;
static int64_t       s_block_until_us;
static volatile mic_gate_vad_state_t s_vad_state = MIC_GATE_VAD_UNKNOWN;

static void gate_event_cb(audio_recorder_handle_t recorder, void *event, void *ctx) {
    (void)recorder;
    (void)ctx;
    if (event == NULL) {
        return;
    }
    const esp_gmf_afe_evt_t *evt = (const esp_gmf_afe_evt_t *)event;
    if (evt->type == ESP_GMF_AFE_EVT_VAD_START) {
        int64_t now = esp_timer_get_time();
        s_vad_start_us = now;
        s_vad_state = MIC_GATE_VAD_SPEECH;
        if (s_merge_pending && slice_client_gate_active()) {
            /* VAD_END 后的合并窗口内又说话了：继续同一段，不重新开切片 */
            s_merge_pending = false;
            s_vad_end_us = 0;
            ESP_LOGI(TAG, "vad_start -> merge continue");
        } else {
            s_slice_start_us = now;
            slice_client_gate_start();
            ESP_LOGI(TAG, "vad_start -> slicing");
        }
    } else if (evt->type == ESP_GMF_AFE_EVT_VAD_END) {
        int64_t dur_ms = (s_vad_start_us > 0)
                       ? (esp_timer_get_time() - s_vad_start_us) / 1000 : 0;
        s_vad_start_us = 0;
        if (slice_client_gate_active()) {
            /* 不立即收口：先留 GATE_MERGE_MS 的合并窗口 */
            s_vad_end_us = esp_timer_get_time();
            s_merge_pending = true;
            s_vad_state = MIC_GATE_VAD_MERGE;
            ESP_LOGI(TAG, "vad_end %lldms -> merge window %dms",
                     (long long)dur_ms, GATE_MERGE_MS);
        } else {
            s_vad_state = MIC_GATE_VAD_SILENCE;
        }
    }
}

static esp_err_t gate_play_write_cb(uint8_t *data, int size, void *ctx) {
    (void)data; (void)size; (void)ctx;   /* 监听态不出声：占位回调 */
    return ESP_OK;
}

static esp_err_t gate_rec_read_cb(uint8_t *data, int size, void *ctx) {
    (void)ctx;
    /* AFE feed 任务靠这个回调把 codec PCM 喂进管线；必须真正读，
     * 不能返回空数据，否则 feed 任务会在空转里把 CPU 占满并触发 task_wdt。 */
    if (s_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return mic_fe_read(data, size, s_mic);
}

static void gate_task(void *arg) {
    (void)arg;
    int err = 0;
    while (s_run) {
        int ret = audio_recorder_read_data(s_rec, s_buf, GATE_READ_BYTES);
        if (ret <= 0) {
            if (++err == 1 || (err % 50) == 0) {
                ESP_LOGW(TAG, "read failed: %d (consec=%d)", ret, err);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        err = 0;
        slice_client_feed((const int16_t *)s_buf, (size_t)(ret / 2));
        int64_t now = esp_timer_get_time();
        /* 合并窗口到点：这一句后面 1.2s 没人再说话，才真正收口 */
        if (s_merge_pending && s_vad_end_us > 0 &&
            (now - s_vad_end_us) / 1000 >= GATE_MERGE_MS) {
            s_merge_pending = false;
            s_vad_end_us = 0;
            slice_client_gate_end();
            s_slice_start_us = 0;
            s_vad_state = MIC_GATE_VAD_SILENCE;
            ESP_LOGI(TAG, "merge window closed -> slice");
        }
        /* 长段强制收口：单切片上限 30 s（云端 4 MB 口也扛得住，但语义上够了） */
        if (slice_client_gate_active() && s_slice_start_us > 0 &&
            (now - s_slice_start_us) / 1000 > GATE_MAX_SLICE_MS) {
            ESP_LOGW(TAG, "slice > %ds, force end", GATE_MAX_SLICE_MS / 1000);
            slice_client_gate_end();
            s_merge_pending = false;
            s_vad_end_us = 0;
            s_slice_start_us = 0;
            s_vad_state = MIC_GATE_VAD_SILENCE;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void gate_cleanup(bool release_lease) {
    if (s_rec) {
        audio_recorder_close(s_rec);
        s_rec = NULL;
    }
    audio_manager_deinit();
    mic_fe_deinit();
    if (s_spk) {
        esp_codec_dev_close(s_spk);
        s_spk = NULL;
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    if (release_lease) {
        audio_service_ext_release_exclusive();
    }
    s_vad_state = MIC_GATE_VAD_UNKNOWN;
    s_owner = false;
}

/** 按 session_setup 的顺序起一条"只录音、不播放、不连 WS"的管线 */
static bool gate_start_pipeline(void) {
    esp_err_t ret = audio_service_ext_claim_exclusive(&s_spk, &s_mic);
    if (ret != ESP_OK) {
        return false;   /* 忙：下次监督周期再试 */
    }

    if (mic_fe_init(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "mic_fe_init failed");
        gate_cleanup(true);
        return false;
    }

    audio_manager_config_t acfg = DEFAULT_AUDIO_MANAGER_CONFIG();
    acfg.play_io.write_cb = gate_play_write_cb;
    acfg.rec_io.read_cb = gate_rec_read_cb;
    audio_manager_config_set_play_io_format(&acfg, GATE_SAMPLE_RATE, GATE_BITS, 2);
    audio_manager_config_set_rec_io_format(&acfg, GATE_SAMPLE_RATE, GATE_BITS,
                                           mic_fe_out_channels());
    snprintf(acfg.mic_layout, sizeof(acfg.mic_layout), "%s", mic_fe_layout());
    if (audio_manager_init(&acfg) != ESP_OK) {
        ESP_LOGW(TAG, "audio_manager_init failed");
        gate_cleanup(true);
        return false;
    }

    esp_codec_dev_sample_info_t fs_play = {
        .sample_rate = GATE_SAMPLE_RATE, .channel = 2, .bits_per_sample = GATE_BITS,
    };
    esp_codec_dev_sample_info_t fs_mic = {
        .sample_rate = GATE_SAMPLE_RATE, .channel = 2, .bits_per_sample = GATE_BITS,
    };
    if (esp_codec_dev_open(s_spk, &fs_play) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "open spk failed");
        gate_cleanup(true);
        return false;
    }
    if (esp_codec_dev_open(s_mic, &fs_mic) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "open mic failed");
        gate_cleanup(true);
        return false;
    }
    esp_codec_dev_set_in_gain(s_mic, 24.0);
    mic_fe_attach_codec(s_mic);       /* 必须在 open + set_in_gain 之后 */

    audio_recorder_config_t rcfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_afe_config_t afe = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    if (mic_fe_out_channels() == 2) {
        afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_NS_ENABLE
                               | AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    } else {
        afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    }
    rcfg.afe_config = afe;
    rcfg.encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    rcfg.encoder_cfg.params.pcm.audio_info.sample_rate = GATE_SAMPLE_RATE;
    rcfg.encoder_cfg.params.pcm.audio_info.sample_bits = GATE_BITS;
    rcfg.encoder_cfg.params.pcm.audio_info.channels = 1;
    rcfg.encoder_cfg.params.pcm.audio_info.frame_duration = GATE_FRAME_MS;
    rcfg.recorder_event_cb = gate_event_cb;
    rcfg.recorder_ctx = NULL;
    rcfg.afe_feed_task_config.task_core = GATE_TASK_CORE;
    rcfg.afe_fetch_task_config.task_core = GATE_TASK_CORE;
    rcfg.recorder_task_config.task_core = GATE_TASK_CORE;
    rcfg.recorder_task_config.task_prio = GATE_TASK_PRIO;

    if (audio_recorder_open(&rcfg, &s_rec) != ESP_OK) {
        ESP_LOGW(TAG, "recorder_open failed");
        gate_cleanup(true);
        return false;
    }

    s_buf = heap_caps_malloc(GATE_READ_BYTES, MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        ESP_LOGW(TAG, "read buf alloc failed");
        gate_cleanup(true);
        return false;
    }

    s_run = true;
    if (xTaskCreatePinnedToCore(gate_task, "mic_gate", 3584, NULL, 5,
                                &s_task, GATE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "gate task create failed");
        s_run = false;
        gate_cleanup(true);
        return false;
    }

    s_owner = true;
    s_vad_state = MIC_GATE_VAD_SILENCE;
    ESP_LOGI(TAG, "listening: %s layout=%s", mic_fe_mode_name(), mic_fe_layout());
    return true;
}

static void gate_supervisor(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_enabled || s_owner) {
            continue;
        }
        if (xiaozhi_service_get_state() != XZ_STATE_IDLE) {
            continue;
        }
        if (media_player_service_is_playing()) {
            continue;
        }
        if (rec_local_service_is_active()) {
            continue;
        }
        if (!audio_service_mic_available() || !slice_client_ready()) {
            continue;
        }
        if (s_block_until_us > esp_timer_get_time()) {
            continue;
        }
        (void)gate_start_pipeline();
    }
}

esp_err_t mic_gate_init(void) {
    if (!s_enabled) {
        ESP_LOGI(TAG, "disabled (MIC_GATE_ENABLE=0), zero behavior change");
        return ESP_OK;
    }
    if (slice_client_ready() || slice_client_init() == ESP_OK) {
        if (xTaskCreatePinnedToCore(gate_supervisor, "mic_gate_sup", 3072, NULL, 3,
                                    NULL, GATE_TASK_CORE) != pdPASS) {
            ESP_LOGE(TAG, "supervisor create failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "supervisor started");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "slice_client init failed, gate stays off");
    return ESP_FAIL;
}

bool mic_gate_enabled(void) { return s_enabled; }

void mic_gate_set_enabled(bool on) {
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    if (!on) {
        (void)mic_gate_yield(GATE_YIELD_WAIT_MS);
    }
    ESP_LOGI(TAG, "enabled=%d", (int)on);
}

bool mic_gate_is_owner(void) { return s_owner; }

mic_gate_vad_state_t mic_gate_vad_state(void) { return s_vad_state; }

bool mic_gate_yield(uint32_t timeout_ms) {
    if (!s_owner && s_task == NULL) {
        return true;
    }
    s_enabled = false;           /* 防止收口期间被监督任务重新拉起 */
    s_run = false;

    uint32_t waited = 0;
    while (s_task != NULL && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "yield timeout (%ums), task still running", (unsigned)timeout_ms);
        s_enabled = true;
        return false;
    }

    slice_client_gate_reset();
    s_merge_pending = false;
    s_vad_end_us = 0;
    s_slice_start_us = 0;
    s_vad_start_us = 0;
    gate_cleanup(true);
    s_enabled = true;            /* 让监督任务在资源空闲后自动恢复 */
    ESP_LOGI(TAG, "yielded in %ums", (unsigned)waited);
    return true;
}

void mic_gate_resume(void) {
    if (!s_enabled || s_owner || s_task != NULL) {
        return;
    }
    if (xiaozhi_service_get_state() != XZ_STATE_IDLE) {
        return;
    }
    if (!audio_service_mic_available() || !slice_client_ready()) {
        return;
    }
    if (s_block_until_us > esp_timer_get_time()) {
        return;
    }
    (void)gate_start_pipeline();
}

void mic_gate_block(uint32_t ms) {
    s_block_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
    ESP_LOGI(TAG, "blocked for %ums", (unsigned)ms);
}
