/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_action_endpoints.c
 * @brief L4 业务：S9 §3.5 动作端点的实现与登记（**从 web_server.c 整体搬出**）
 *
 * 为什么搬：这些 handler 的业务判断（打卡能不能受理、相机是否被占、云播是否
 * 起得来、灯 JSON 合不合法）都属于业务层。它们原来长在 web_server.c 里，
 * 让"网页服务"反向 include ai_punch / ai_light / capture_service / xiaozhi_service。
 * 现在 handler 归业务、路由登记走 web_action 注册表 —— web_server 只看表。
 *
 * 受理语义（一个字都没改，前端契约不变）：
 *   · 长事务（打卡/拍照/播放）一律"投出去 + 202"，结论走 /api/action/result?gen=N；
 *   · 快事务（灯控两发）同步做完（一次队列 post / 一次内存读，< 1 ms）。
 *   · httpd 单任务串行：handler 里绝不阻塞 —— 这条铁律在这里同样成立。
 */

#include "middleware/net/web_action.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "middleware/ai/ai_punch.h"        /* ai_punch_trigger / ai_punch_busy */
#include "middleware/ai/ai_light.h"        /* 灯控端点 */
#include "middleware/ai/xiaozhi_service.h" /* 云播 */
#include "middleware/camera/capture_service.h"

/* ---------------------------------------------------------------------------
 *  POST /api/punch —— 触发人脸打卡（受理即 202，结论轮询 /api/action/result）
 * ------------------------------------------------------------------------- */
static esp_err_t ep_api_punch(httpd_req_t *req)
{
    web_log_req(req, "action/punch");

    esp_err_t e = ai_punch_trigger();
    if (e == ESP_OK) {
        uint32_t g = web_act_accept(WEB_ACT_PUNCH);
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"ok\":1,\"accepted\":1,\"gen\":%u,\"action\":\"punch\"}",
                 (unsigned)g);
        httpd_resp_set_status(req, "202 Accepted");
        return web_send_json(req, body);
    }

    /* 拒绝：会话在跑或相机被页占用 —— 前端据 error 提示，不必重试 */
    const char *why = (e == ESP_ERR_INVALID_STATE) ? "busy" : "no_mem";
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"busy\":%d}",
             why, ai_punch_busy() ? 1 : 0);
    httpd_resp_set_status(req, "409 Conflict");
    return web_send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  POST /api/media/capture —— 拍照存 SD（受理即 202）
 * ------------------------------------------------------------------------- */
static esp_err_t ep_api_capture(httpd_req_t *req)
{
    web_log_req(req, "action/capture");

    esp_err_t e = capture_service_request();
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return web_send_json(req, "{\"error\":\"busy_or_no_sd\"}");
    }
    uint32_t g = web_act_accept(WEB_ACT_CAPTURE);
    char body[96];
    snprintf(body, sizeof(body),
             "{\"ok\":1,\"accepted\":1,\"gen\":%u,\"action\":\"capture\"}",
             (unsigned)g);
    httpd_resp_set_status(req, "202 Accepted");
    return web_send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  POST /api/media/play?url=...  —— 播放一个音频 URL（点歌 / TTS 自测）
 *  ★ 走 xiaozhi_service_cloud_audio_play：会话不在时会自动起会话并挂起播放
 *    （桌宠主动推送场景），所以"受理"不等于"马上出声"。
 * ------------------------------------------------------------------------- */
static esp_err_t ep_api_play(httpd_req_t *req)
{
    web_log_req(req, "action/play");

    char url[512] = "";
    if (web_query_get(req, "url", url, sizeof(url)) != ESP_OK || url[0] == '\0') {
        /* 也接受 JSON body：{"url":"http://..."}（长 URL 走 query 易被截断） */
        char body[512];
        if (web_read_body(req, body, sizeof(body)) > 0) {
            const char *p = strstr(body, "\"url\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        const char *q = strchr(p, '"');
                        if (q && (size_t)(q - p) < sizeof(url)) {
                            memcpy(url, p, (size_t)(q - p));
                            url[q - p] = '\0';
                        }
                    }
                }
            }
        }
    }
    if (url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"error\":\"no_url\"}");
    }

    esp_err_t e = xiaozhi_service_cloud_audio_play(url);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return web_send_json(req, "{\"error\":\"play_failed\"}");
    }
    uint32_t g = web_act_accept(WEB_ACT_PLAY);
    char body[128];
    snprintf(body, sizeof(body),
             "{\"ok\":1,\"accepted\":1,\"gen\":%u,\"action\":\"play\"}", (unsigned)g);
    httpd_resp_set_status(req, "202 Accepted");
    return web_send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  POST /api/light/set  —— {"power":true,"rgb":[r,g,b],"brightness":0..100}
 *  GET  /api/light/get  —— 当前灯状态
 *  快端点：一次 led_service 队列 post，同步可（< 1 ms）
 * ------------------------------------------------------------------------- */
static esp_err_t ep_api_light_set(httpd_req_t *req)
{
    web_log_req(req, "action/light_set");

    char body[256];
    if (web_read_body(req, body, sizeof(body)) <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"error\":\"no_body\"}");
    }
    esp_err_t e = ai_light_set_json(body);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"error\":\"bad_json\"}");
    }
    return web_send_json(req, ai_light_state_json());
}

static esp_err_t ep_api_light_get(httpd_req_t *req)
{
    web_log_req(req, "action/light_get");
    return web_send_json(req, ai_light_state_json());
}

/* ---------------------------------------------------------------------------
 *  GET /api/action/result?gen=N —— 动作结果轮询（gen 模式，1 s 一拍）
 *
 *   gen == 当前 → {"gen":G,"action":"...","state":"running"|"idle"}
 *   gen != 当前 → punch 带 text、capture 带 err/path/bytes。
 *   ★ 这个端点读的是"业务侧权威状态"（punch 文案 / capture 结果），所以它
 *     本来就属于业务层；搬过来后 web_server 连 capture_service 都不必再认识。
 * ------------------------------------------------------------------------- */
static esp_err_t ep_api_result(httpd_req_t *req)
{
    web_log_req(req, "action/result");

    char gen_s[16] = "0";
    web_query_get(req, "gen", gen_s, sizeof(gen_s));
    uint32_t client_gen = (uint32_t)strtoul(gen_s, NULL, 10);

    web_act_id_t id;
    uint32_t cur_gen, elapsed_ms;
    web_act_snapshot(&id, &cur_gen, &elapsed_ms);

    char body[512];

    if (cur_gen != client_gen && id == WEB_ACT_PUNCH) {
        bool busy = ai_punch_busy();
        const char *txt = ai_punch_last_result();
        char esc[320];
        web_json_esc(esc, sizeof(esc), txt ? txt : "");
        snprintf(body, sizeof(body),
                 "{\"gen\":%u,\"action\":\"punch\",\"state\":\"%s\","
                 "\"ok\":%s,\"elapsed_ms\":%u,\"text\":\"%s\"}",
                 (unsigned)cur_gen, busy ? "running" : "done",
                 (!busy && strncmp(txt ? txt : "", "Punched:", 8) == 0) ? "true" : "false",
                 (unsigned)elapsed_ms, esc);
    } else if (cur_gen != client_gen && id == WEB_ACT_CAPTURE) {
        capture_result_t cr;
        capture_service_get_result(&cr);
        bool busy = (capture_service_state() == CAPTURE_BUSY);
        char esc[CAPTURE_PATH_MAX * 2];
        web_json_esc(esc, sizeof(esc), cr.path);
        snprintf(body, sizeof(body),
                 "{\"gen\":%u,\"action\":\"capture\",\"state\":\"%s\","
                 "\"ok\":%s,\"err\":\"%s\",\"path\":\"%s\",\"bytes\":%u,"
                 "\"elapsed_ms\":%u}",
                 (unsigned)cur_gen, busy ? "running" : "done",
                 (!busy && cr.err == ESP_OK) ? "true" : "false",
                 esp_err_to_name(cr.err), esc, (unsigned)cr.bytes,
                 (unsigned)cr.elapsed_ms);
    } else if (cur_gen != client_gen &&
               (id == WEB_ACT_PLAY || id == WEB_ACT_REBOOT)) {
        snprintf(body, sizeof(body),
                 "{\"gen\":%u,\"action\":\"%s\",\"state\":\"done\",\"ok\":true,"
                 "\"elapsed_ms\":%u}",
                 (unsigned)cur_gen, web_act_name(id), (unsigned)elapsed_ms);
    } else {
        /* 无新动作（或 gen 已对齐）：只回代际号，前端继续轮 */
        snprintf(body, sizeof(body), "{\"gen\":%u,\"action\":\"none\"}",
                 (unsigned)cur_gen);
    }
    return web_send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  登记（app 在 web_server_init 之前调用；web 启动时遍历注册表挂路由）
 * ------------------------------------------------------------------------- */
/* 状态端点的忙态：业务自己回答（★ P2 注入，web_server 不必认识 ai_punch/capture） */
static bool busy_punch_provider(void)
{
    return ai_punch_busy();
}

static bool busy_capture_provider(void)
{
    return (capture_service_state() == CAPTURE_BUSY);
}

esp_err_t ai_action_endpoints_init(void)
{
    esp_err_t err = ESP_OK;
    web_status_set_busy_providers(busy_punch_provider, busy_capture_provider);
    err |= web_action_register("/api/punch",         HTTP_POST, ep_api_punch);
    err |= web_action_register("/api/media/capture", HTTP_POST, ep_api_capture);
    err |= web_action_register("/api/media/play",    HTTP_POST, ep_api_play);
    err |= web_action_register("/api/light/set",     HTTP_POST, ep_api_light_set);
    err |= web_action_register("/api/light/get",     HTTP_GET,  ep_api_light_get);
    err |= web_action_register("/api/action/result", HTTP_GET,  ep_api_result);
    return (err == ESP_OK) ? ESP_OK : ESP_FAIL;
}
