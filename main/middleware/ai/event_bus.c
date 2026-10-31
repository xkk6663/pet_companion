/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file event_bus.c
 * @brief 下行消息总线实现（见 event_bus.h）
 *
 * ★ 相对 URL 的处理：云端下发的 `audio.url` / `image.url` 是**相对路径**
 *   （`/static/tts_xxxx.wav`，网关文档 §5.3 明写）。esp_audio_simple_player 与
 *   esp_http_client 都只认绝对地址 ⇒ 这里统一用 ws_client_get_http_base()
 *   把 `ws://host:port` 换成 `http://host:port` 再拼，设备端零配置。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "xiaozhi_service.h"
#include "ws_client.h"
#include "cloud_media_service.h"
#include "ai_action.h"
#include "ai_camera.h"
#include "ai_light.h"
#include "ai_punch.h"
#include "middleware/camera/capture_service.h"
#include "middleware/periph/safe_reboot.h"
#include "middleware/audio/audio_service.h"

static const char *TAG = "EVT";

static xiaozhi_evt_cb_t s_cb;

static char s_cmd_table[EVENT_BUS_CMD_MAX][32];
static int  s_cmd_count;

static void emit(xiaozhi_evt_id_t id, const char *text, int role, esp_err_t err)
{
    if (s_cb) {
        xiaozhi_evt_t evt = { .id = id, .text = text, .role = role, .err = err };
        s_cb(&evt);
    }
}

/* 相对路径 → 绝对 URL；已是 http(s):// 的原样返回 */
static void abs_url(const char *in, char *out, size_t out_size)
{
    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
        snprintf(out, out_size, "%s", in);
        return;
    }
    char base[96];
    ws_client_get_http_base(base, sizeof(base));
    if (base[0] == '\0') {
        snprintf(out, out_size, "%s", in);
        return;
    }
    snprintf(out, out_size, "%s%s", base, (in[0] == '/') ? "" : "/");
    size_t used = strlen(out);
    snprintf(out + used, out_size - used, "%s", in);
}

static void handle_system(cJSON *root)
{
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        return;
    }
    const char *c = cmd->valuestring;

    if (strcmp(c, "reboot") == 0) {
        emit(XZ_EVT_TEXT, "Rebooting device...", 1, ESP_OK);
        vTaskDelay(pdMS_TO_TICKS(300));      /* 让气泡与日志先落 */
        safe_reboot_perform("gateway system cmd");
        return;
    }
    if (strcmp(c, "set_volume") == 0) {
        cJSON *v = cJSON_GetObjectItem(root, "value");
        int vol = cJSON_IsNumber(v) ? v->valueint : -1;
        if (vol < 0 || vol > 90) {
            ESP_LOGW(TAG, "set_volume: bad value %d", vol);
            return;
        }
        if (xiaozhi_service_set_volume(vol) != ESP_OK) {
            audio_cmd_t ac = { .id = AUDIO_CMD_SET_VOLUME, .value = vol };
            audio_service_post_cmd(&ac);
        }
        ESP_LOGI(TAG, "set_volume(%d)", vol);
        return;
    }
    if (strcmp(c, "explain") == 0) {
        /* 云端要"看看眼前"（S9 裁决 5）：设备拍照 → 上传网关 → WS image_req。
         * 本函数在 WS 收包任务上下文 ⇒ 只能"受理即返回"（红线：收包任务
         * 不得阻塞），真正的相机事务在 ai_camera 的一次性任务里。 */
        cJSON *q = cJSON_GetObjectItem(root, "text");
        esp_err_t e = ai_camera_explain_cloud(cJSON_IsString(q) ? q->valuestring : NULL);
        ESP_LOGI(TAG, "explain -> %s", esp_err_to_name(e));
        return;
    }
    if (strcmp(c, "punch") == 0) {
        /* 云端发起打卡（等价于 HTTP /api/punch；会话结论回 chat 通道） */
        esp_err_t e = ai_punch_trigger();
        ESP_LOGI(TAG, "punch -> %s", esp_err_to_name(e));
        return;
    }
    if (strcmp(c, "light") == 0) {
        /* {"cmd":"light","set":{"power":true,"brightness":40,"rgb":[255,180,90]}} */
        cJSON *set = cJSON_GetObjectItem(root, "set");
        if (!cJSON_IsObject(set)) {
            ESP_LOGW(TAG, "light: missing 'set' object");
            return;
        }
        char *js = cJSON_PrintUnformatted(set);
        if (js) {
            esp_err_t e = ai_light_set_json(js);
            ESP_LOGI(TAG, "light -> %s (%s)", esp_err_to_name(e), js);
            cJSON_free(js);
        }
        return;
    }
    if (strcmp(c, "capture") == 0) {
        /* 拍照存 SD（capture_service 受理制非阻塞） */
        esp_err_t e = capture_service_request();
        ESP_LOGI(TAG, "capture -> %s", esp_err_to_name(e));
        return;
    }
    if (strcmp(c, "command_table") == 0) {
        cJSON *list = cJSON_GetObjectItem(root, "list");
        if (!cJSON_IsArray(list)) {
            return;
        }
        s_cmd_count = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, list) {
            if (s_cmd_count >= EVENT_BUS_CMD_MAX) {
                break;
            }
            if (!cJSON_IsString(it) || it->valuestring == NULL) {
                continue;
            }
            snprintf(s_cmd_table[s_cmd_count], sizeof(s_cmd_table[0]), "%s",
                     it->valuestring);
            s_cmd_count++;
        }
        ESP_LOGI(TAG, "command_table: %d items", s_cmd_count);
        return;
    }
    ESP_LOGI(TAG, "system cmd '%s' ignored (v1)", c);
}

void event_bus_on_line(const char *line)
{
    if (line == NULL || line[0] != '{') {
        return;
    }
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json: %.80s", line);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : NULL;

    if (t == NULL) {
        cJSON_Delete(root);
        return;
    }

    /* ★ 每条下行都留一行类型日志：F-S9-A 那次排查里，设备"连上了、也发出去了、
     *   但零下行日志"，靠这条才能一眼分清是"没收到"还是"收到没处理"。
     *   只打 type，不打全文（音频/图片 URL 很长，全文会刷屏）。 */
    ESP_LOGI(TAG, "rx <- %s", t);

    if (strcmp(t, "text") == 0) {
        cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(txt) && txt->valuestring[0]) {
            /* role: 0 = 用户（网关回显的 ASR 结果），1/缺省 = 大肥鱼。
             * ★ 以前写死 role=1 ⇒ 用户说的那句永远不显示，答非所问时
             *   没法判断是 ASR 错了还是模型跑偏（S9 P0 补）。 */
            cJSON *rj = cJSON_GetObjectItem(root, "role");
            int role = cJSON_IsNumber(rj) ? rj->valueint : 1;
            /* ★ 必须打日志：这条链路"云端→WS→总线→UI 队列"任何一环断掉，
             *   现象都一样（屏幕上没反应）。没有日志就只能靠猜，而 UI 在
             *   splash/其它页时队列会被丢弃，日志是唯一的"到过这里"证据。 */
            ESP_LOGI(TAG, "text <- %.96s", txt->valuestring);
            emit(XZ_EVT_TEXT, txt->valuestring, role, ESP_OK);
        }
    } else if (strcmp(t, "emotion") == 0) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(name)) {
            ESP_LOGI(TAG, "emotion <- %s", name->valuestring);
            emit(XZ_EVT_EMOJI, name->valuestring, 0, ESP_OK);
        }
    } else if (strcmp(t, "audio") == 0) {
        cJSON *url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            char abs[256];
            abs_url(url->valuestring, abs, sizeof(abs));
            cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
            ESP_LOGI(TAG, "audio <- %s (%d ms)", abs,
                     cJSON_IsNumber(dur) ? dur->valueint : 0);
            esp_err_t err = xiaozhi_service_cloud_audio_play(abs);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio play rejected: %s", esp_err_to_name(err));
            }
        }
    } else if (strcmp(t, "image") == 0) {
        cJSON *url = cJSON_GetObjectItem(root, "url");
        cJSON *cap = cJSON_GetObjectItem(root, "caption");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            char abs[256];
            abs_url(url->valuestring, abs, sizeof(abs));
            cloud_media_service_fetch(abs, cJSON_IsString(cap) ? cap->valuestring : "");
        }
    } else if (strcmp(t, "action") == 0) {
        /* S9 P0 反向工具 relay：网关决定"做什么"，设备负责"真的做"，
         * 结论异步回 action_result（见 ai_action.h）。
         * ★ 本函数在 WS 收包任务里跑 ⇒ dispatch 只入队，绝不在这里等。 */
        cJSON *name = cJSON_GetObjectItem(root, "name");
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *args = cJSON_GetObjectItem(root, "args");
        char *aj = cJSON_IsObject(args) ? cJSON_PrintUnformatted(args) : NULL;
        esp_err_t ae = ai_action_dispatch(cJSON_IsString(name) ? name->valuestring : NULL,
                                          aj, cJSON_IsNumber(id) ? id->valueint : 0);
        if (aj != NULL) {
            cJSON_free(aj);
        }
        ESP_LOGI(TAG, "action '%s' -> %s",
                 cJSON_IsString(name) ? name->valuestring : "?", esp_err_to_name(ae));
    } else if (strcmp(t, "system") == 0) {
        handle_system(root);
    } else if (strcmp(t, "error") == 0) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        char body[128];
        snprintf(body, sizeof(body), "%s: %s",
                 cJSON_IsString(code) ? code->valuestring : "error",
                 cJSON_IsString(msg) ? msg->valuestring : "failed");
        ESP_LOGW(TAG, "gateway error %s", body);
        emit(XZ_EVT_ERROR, body, 0, ESP_FAIL);
    } else {
        ESP_LOGI(TAG, "type '%s' ignored", t);
    }

    cJSON_Delete(root);
}

void event_bus_init(void)
{
    s_cmd_count = 0;
    memset(s_cmd_table, 0, sizeof(s_cmd_table));
}

void event_bus_set_evt_cb(xiaozhi_evt_cb_t cb)
{
    s_cb = cb;
}

const char *event_bus_command_at(int idx)
{
    if (idx < 0 || idx >= s_cmd_count) {
        return NULL;
    }
    return s_cmd_table[idx];
}

const char *event_bus_command_table(void)
{
    static char joined[EVENT_BUS_CMD_MAX * 34];
    joined[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < s_cmd_count; i++) {
        int n = snprintf(joined + o, sizeof(joined) - o, "%s%s",
                         i ? " · " : "", s_cmd_table[i]);
        if (n <= 0 || (size_t)n >= sizeof(joined) - o) {
            break;
        }
        o += (size_t)n;
    }
    return joined;
}
