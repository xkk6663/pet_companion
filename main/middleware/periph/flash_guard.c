/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file flash_guard.c
 * @brief 运行期 flash 写静默窗口实现（**只认识登记进来的回调，不认识任何业务模块**）
 *
 * ★ P2 解耦（2026-11-20）：
 *   原实现直接 include 并调用 media_player_service / viewfinder / record_video_service
 *   —— 一个"驱动级"的 flash 保护闸，反向依赖了音频服务、相机服务和录像业务（G1 三条）。
 *   现在改成**回调表**：谁需要让出资源，谁在启动时把 busy/calm/restore 三个回调
 *   登记进来（接线在 app/main.c 这个组合根做）。本文件从此是叶子，谁都不依赖。
 *
 * 语义与原实现逐条对齐（不可变）：
 *   ① 任一登记方 busy ⇒ 整个窗口拒绝（INVALID_STATE，调用方延后重试）；
 *   ② 其余登记方 calm() 让出（录像之外的取景/播放），end() 逆序 restore()；
 *   ③ 互斥锁 + 200 ms 超时；begin/end 必须同一任务成对。
 */

#include "flash_guard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "FLASH_GUARD"

/* 等另一个 flash 写结束的最长时间。NVS 一次 commit 通常是几十 ms，
 * 200 ms 足够覆盖一次擦除；等不到就返回 TIMEOUT，让调用方延后。 */
#define FLASH_GUARD_LOCK_MS 200

/* 登记表容量（当前 3 个：录像 / 取景 / 音乐）。静态数组，不占堆。 */
#define FLASH_GUARD_MAX_CLIENTS 4

static SemaphoreHandle_t s_mtx;
static bool s_active;

static const flash_guard_client_t *s_clients[FLASH_GUARD_MAX_CLIENTS];
static int  s_n_clients;
static bool s_quiet[FLASH_GUARD_MAX_CLIENTS];   /* 本次窗口里确实"动过"的登记项 */

void flash_guard_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            ESP_LOGE(TAG, "mutex create failed; flash writes will run unguarded");
        }
    }
}

esp_err_t flash_guard_register(const flash_guard_client_t *client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_n_clients >= FLASH_GUARD_MAX_CLIENTS) {
        ESP_LOGE(TAG, "client table full (%d), '%s' ignored",
                 FLASH_GUARD_MAX_CLIENTS, client->name ? client->name : "-");
        return ESP_ERR_NO_MEM;
    }
    s_clients[s_n_clients++] = client;
    return ESP_OK;
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

    /* ① 硬忙：录像的 fwrite 往 SD 写 + DVP DMA 往 PSRAM 搬，此时关 cache
     *    既可能丢帧也可能让 FATFS 事务跨过窗口。直接拒绝，由调用方延后重试。 */
    for (int i = 0; i < s_n_clients; i++) {
        const flash_guard_client_t *c = s_clients[i];
        if (c != NULL && c->busy != NULL && c->busy()) {
            ESP_LOGW(TAG, "begin(%s): '%s' busy, defer flash write",
                     reason ? reason : "-", c->name ? c->name : "-");
            xSemaphoreGive(s_mtx);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* ② 软让出：停取景 / 暂停播放（失败的让出不算"动过"，end 不会去 restore） */
    for (int i = 0; i < s_n_clients; i++) {
        const flash_guard_client_t *c = s_clients[i];
        s_quiet[i] = false;
        if (c != NULL && c->calm != NULL && c->calm()) {
            s_quiet[i] = true;
        }
    }

    s_active = true;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "begin(%s): vf=%d music=%d", reason ? reason : "-",
             (int)s_quiet[1], (int)s_quiet[2]);   /* 顺序与 app 登记顺序一致：0=录像 1=取景 2=音乐 */
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

    /* ③ 逆序恢复（后让出的先恢复，避免"取景先起、音乐后起"这类时序抖动） */
    for (int i = s_n_clients - 1; i >= 0; i--) {
        const flash_guard_client_t *c = s_clients[i];
        if (s_quiet[i] && c != NULL && c->restore != NULL) {
            c->restore();
        }
        s_quiet[i] = false;
    }
    ESP_LOGI(TAG, "end: restored");
    s_active = false;
    xSemaphoreGive(s_mtx);
}

bool flash_guard_is_active(void)
{
    return s_active;
}
