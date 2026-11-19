/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_guard.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "BOOT_GUARD"

#define BOOT_GUARD_NS      "bootg"
#define BOOT_GUARD_KEY_CNT "cnt"
#define BOOT_GUARD_LIMIT   3u
#define BOOT_GUARD_MAX     99u

/* ★ 为什么用 NVS 而不是 RTC_NOINIT：
 *   本板实测每次复位 esp_reset_reason() 都是 POWERON(1)，RTC_NOINIT 里的
 *   计数在复位后读到的是初值（prev_ready 永远 0），计数根本累不起来 ——
 *   安全模式永远进不去（2026-11-19 实机验证）。NVS 跨 panic/WDT/软复位/
 *   断电都保留，代价是每次启动 2 次小 commit（启动不频繁，可接受）。
 *   计数语义：启动即 +1，走完正常流程归 0；连续 ≥3 次未归零 = 安全模式。 */

static bool s_safe_mode;
static uint32_t s_count;

static esp_err_t nvs_open_retry(nvs_handle_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区被写坏/版本变了：擦一次重建。仅重启期做，不会打断业务。 */
        ESP_LOGW(TAG, "nvs init failed (%s), erase + retry", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_open(BOOT_GUARD_NS, NVS_READWRITE, out);
}

static void store_count(uint32_t cnt)
{
    nvs_handle_t h;
    if (nvs_open_retry(&h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, failure counter not persisted");
        return;
    }
    nvs_set_u32(h, BOOT_GUARD_KEY_CNT, cnt);
    nvs_commit(h);
    nvs_close(h);
}

void boot_guard_init(void)
{
    uint32_t cnt = 0;
    nvs_handle_t h;

    if (nvs_open_retry(&h) == ESP_OK) {
        nvs_get_u32(h, BOOT_GUARD_KEY_CNT, &cnt);
        if (cnt < BOOT_GUARD_MAX) {
            cnt++;
        }
        nvs_set_u32(h, BOOT_GUARD_KEY_CNT, cnt);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs unavailable: safe mode disabled this boot");
    }

    s_count = cnt;
    s_safe_mode = (cnt >= BOOT_GUARD_LIMIT);

    ESP_LOGW(TAG, "reset_reason=%d fail_count=%u safe_mode=%d",
             (int)esp_reset_reason(), (unsigned)s_count, (int)s_safe_mode);
}

bool boot_guard_should_enter_safe_mode(void)
{
    return s_safe_mode;
}

void boot_guard_mark_ready(void)
{
    s_count = 0;
    s_safe_mode = false;
    store_count(0);
    ESP_LOGI(TAG, "marked ready (failure counter cleared)");
}

void boot_guard_clear(void)
{
    s_count = 0;
    s_safe_mode = false;
    store_count(0);
    ESP_LOGW(TAG, "failure counter cleared by user");
}

uint32_t boot_guard_failure_count(void)
{
    return s_count;
}
