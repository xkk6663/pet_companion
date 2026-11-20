/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_splash.h
 * @brief 屏保页（原开机 splash）。上电不再自动装载，由 SET 长按 ≥3 s 按需
 *        压栈拉起；上滑触发 enter 回调（page_main 里绑成 PageManager_Back）。
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*page_splash_enter_cb_t)(void);

/* Set the callback fired once when swipe-up is detected. */
void page_splash_set_enter_cb(page_splash_enter_cb_t cb);

extern const Page_t Page_Splash;

#ifdef __cplusplus
}
#endif
