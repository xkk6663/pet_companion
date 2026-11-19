/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_guard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "middleware/audio/media_player_service.h"
#include "middleware/camera/viewfinder.h"
#include "middleware/media/record_video_service.h"

#define TAG "FLASH_GUARD"

/* 等另一个 flash 写结束的最长时间。NVS 一次 commit 通常是几十 ms，
 * 200 ms 足够覆盖一次擦除；等不到就返回 TIMEOUT，让调用方延后。 */
#define FLASH_GUARD_LOCK_MS 200

static SemaphoreHandle_t s_mtx;
static bool s_active;
static bool s_vf_was_on;
static bool s_mp_was_on;

void flash_guard_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            ESP_LOGE(TAG, "mutex create failed; flash writes will run unguarded");
        }
    }
}

esp_err_t flash_guard_begin(const char *reason)
{
    if (s_mtx == NULL) {
        flash_guard_init();
    }
    if (s_mtx == NULL) {
        return ESP_OK;   /* 无锁可用时不要阻断业务；风险由调用方日志暴露 */
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(FLASH_GUARD_LOCK_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "begin(%s): another flash write still busy", reason ? reason : "-");
        return ESP_ERR_TIMEOUT;
    }
    if (s_active) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* 录像的 fwrite 正在往 SD 写，同时 DVP DMA 还在往 PSRAM 搬；此时关 cache
     * 既可能丢帧也可能让 FATFS 事务跨过窗口。直接拒绝，由调用方延后重试。 */
    if (record_video_service_is_recording() ||
        record_video_service_state() != VIDEO_REC_IDLE) {
        ESP_LOGW(TAG, "begin(%s): recording/finalizing, defer flash write",
                 reason ? reason : "-");
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    s_vf_was_on = viewfinder_is_enabled();
    if (s_vf_was_on) {
        viewfinder_disable();
    }

    s_mp_was_on = media_player_service_is_playing() &&
                  !media_player_service_is_paused();
    if (s_mp_was_on) {
        if (media_player_service_pause() != ESP_OK) {
            s_mp_was_on = false;
        }
    }

    s_active = true;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "begin(%s): vf=%d music=%d", reason ? reason : "-",
             (int)s_vf_was_on, (int)s_mp_was_on);
    return ESP_OK;
}

void flash_guard_end(void)
{
    if (s_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(FLASH_GUARD_LOCK_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "end: lock timeout, state left as-is");
        return;
    }
    if (!s_active) {
        xSemaphoreGive(s_mtx);
        return;
    }

    if (s_mp_was_on) {
        media_player_service_resume();
    }
    if (s_vf_was_on) {
        viewfinder_enable();
    }
    ESP_LOGI(TAG, "end: restore vf=%d music=%d",
             (int)s_vf_was_on, (int)s_mp_was_on);
    s_vf_was_on = false;
    s_mp_was_on = false;
    s_active = false;
    xSemaphoreGive(s_mtx);
}

bool flash_guard_is_active(void)
{
    return s_active;
}
