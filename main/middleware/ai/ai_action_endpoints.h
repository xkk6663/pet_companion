/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_action_endpoints.h
 * @brief L4 业务：网页动作端点的登记入口（实现在 ai_action_endpoints.c）
 *
 * \u2605 P2：这些端点（/api/punch、/api/media/capture、/api/media/play、
 *   /api/light/set、/api/light/get、/api/action/result）的实现归业务层，
 *   由本模块在启动时登记进 web_action 注册表；web_server.c 只负责遍历挂路由，
 *   不再认识任何业务模块。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 登记全部动作端点。必须在 web_server_init() **之前**调用（启动后注册表只读）。 */
esp_err_t ai_action_endpoints_init(void);

#ifdef __cplusplus
}
#endif
