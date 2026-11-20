/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_action.c
 * @brief L2 服务：HTTP 动作端点注册表实现（纯表，无锁、无堆）
 */

#include "middleware/net/web_action.h"

#include "esp_log.h"

#define TAG "WEB_ACT"

#define WEB_ACTION_MAX 8

typedef struct {
    const char    *uri;
    httpd_method_t method;
    web_action_fn  fn;
} web_action_entry_t;

static web_action_entry_t s_entries[WEB_ACTION_MAX];
static size_t s_count;

esp_err_t web_action_register(const char *uri, httpd_method_t method, web_action_fn fn)
{
    if (uri == NULL || fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= WEB_ACTION_MAX) {
        ESP_LOGE(TAG, "table full (%d), %s ignored", WEB_ACTION_MAX, uri);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (s_entries[i].uri == uri || (s_entries[i].uri && uri &&
            strcmp(s_entries[i].uri, uri) == 0)) {
            ESP_LOGW(TAG, "duplicate uri %s (ignore re-register)", uri);
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_entries[s_count].uri = uri;
    s_entries[s_count].method = method;
    s_entries[s_count].fn = fn;
    s_count++;
    ESP_LOGI(TAG, "registered %s (total %u)", uri, (unsigned)s_count);
    return ESP_OK;
}

size_t web_action_count(void)
{
    return s_count;
}

const char *web_action_uri(size_t i)
{
    return (i < s_count) ? s_entries[i].uri : NULL;
}

httpd_method_t web_action_method(size_t i)
{
    return (i < s_count) ? s_entries[i].method : HTTP_GET;
}

web_action_fn web_action_fn_at(size_t i)
{
    return (i < s_count) ? s_entries[i].fn : NULL;
}

/* ---- 状态端点的忙态提供者（业务侧注册；未注册 = 不忙）---- */
static web_busy_fn s_busy_punch;
static web_busy_fn s_busy_capture;

void web_status_set_busy_providers(web_busy_fn punch, web_busy_fn capture)
{
    s_busy_punch = punch;
    s_busy_capture = capture;
}

bool web_busy_punch(void)
{
    return (s_busy_punch != NULL) ? s_busy_punch() : false;
}

bool web_busy_capture(void)
{
    return (s_busy_capture != NULL) ? s_busy_capture() : false;
}
