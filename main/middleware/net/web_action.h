/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_action.h
 * @brief L2 服务：HTTP 动作端点**注册表** + 公用的 HTTP 小工具
 *
 * 背景（评估报告 P2 第 2 条）：
 *   web_server.c 是"设备网页服务"，却 hard-code 了 /api/punch、/api/media/capture、
 *   /api/media/play、/api/light/set、/api/light/get 五个**业务动作**的完整 handler
 *   —— 于是"服务"反向依赖了 business（ai_punch / ai_light / capture_service /
 *   xiaozhi_service 四条 G1）。业务每加一个动作，都要回来改这张表。
 *
 * 现在反过来：**业务模块在自己的 init 里把端点登记进来**，
 *   · web_server 启动时遍历注册表 httpd_register_uri_handler（一个循环，零业务认知）；
 *   · 加新动作 = 业务侧一行 register，web_server.c 一个字都不用改。
 *
 * 线程/生命周期：注册发生在 web 服务启动之前（单线程 boot 上下文），
 *   启动后注册表只读 —— 因此没有任何锁，也不必动态分配（固定 8 槽静态数组）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 动作槽编号（`/api/action/result?gen=N` 轮询时回给前端的行为标识）。
 *  值必须与 web_server.c 内部 act_id_t 一致 —— 这里就是唯一权威定义。 */
typedef enum {
    WEB_ACT_NONE = 0,
    WEB_ACT_PUNCH,
    WEB_ACT_CAPTURE,
    WEB_ACT_PLAY,
    WEB_ACT_REBOOT,
} web_act_id_t;

typedef esp_err_t (*web_action_fn)(httpd_req_t *req);

/** 注册一个端点（业务模块 init 时调用；uri 必须常驻字符串字面量）。 */
esp_err_t web_action_register(const char *uri, httpd_method_t method, web_action_fn fn);

/** 已注册数量 / 按下标取值（web_server_init 遍历用）。 */
size_t web_action_count(void);
const char *web_action_uri(size_t i);
httpd_method_t web_action_method(size_t i);
web_action_fn web_action_fn_at(size_t i);

/* ---- 公用小工具（实现仍在 web_server.c，业务 handler 复用同一份语义）---- */

/** 请求联调日志：[WEB] handler <- 客户端 IP */
void web_log_req(httpd_req_t *req, const char *what);

/** 发一个小 JSON（Content-Type + no-store + body） */
esp_err_t web_send_json(httpd_req_t *req, const char *body);

/** 读请求体（小 JSON）。@return 字节数；<0 = 失败（空/超长/读错） */
int web_read_body(httpd_req_t *req, char *buf, size_t cap);

/** 从 query 取 key 并 URL 解码。@return ESP_OK / ESP_ERR_NOT_FOUND */
esp_err_t web_query_get(httpd_req_t *req, const char *key, char *out, size_t outlen);

/** 受理一个动作并返回代际号（供 /api/action/result 轮询）。 */
uint32_t web_act_accept(web_act_id_t id);

/** 读动作槽快照（id / 代际号 / 受理已过毫秒）。 */
void web_act_snapshot(web_act_id_t *id, uint32_t *gen, uint32_t *elapsed_ms);

/** 动作名（"punch"/"capture"/"play"/"reboot"/"none"），给 JSON 直接拼。 */
const char *web_act_name(web_act_id_t id);

/** JSON 字符串转义（引号/反斜杠/控制符）—— 姓名、文件名都过这里。 */
const char *web_json_esc(char *dst, size_t dstlen, const char *src);

/* ---- 状态端点的"忙态提供者"（★ P2 注入，避免 web 反向依赖业务）----
 * /api/system/status 里的 busy.punch / busy.capture 是业务事实，
 * 由业务模块在 init 时注册；未注册时按"不忙"回答。 */
typedef bool (*web_busy_fn)(void);
void web_status_set_busy_providers(web_busy_fn punch, web_busy_fn capture);
bool web_busy_punch(void);
bool web_busy_capture(void);

#ifdef __cplusplus
}
#endif
