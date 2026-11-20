/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_watermark.c
 * @brief L2 服务：水印渲染槽实现（只做转发，不碰 LVGL）
 *
 * 依赖方向：business(ai_punch) → services(本文件) ← ui(实现注册)。
 * 三方都只认识 punch_watermark.h 里的函数表，谁都不认识对方。
 */

#include "middleware/camera/punch_watermark.h"

#include <string.h>

static punch_watermark_ops_t s_ops;

void punch_watermark_install(const punch_watermark_ops_t *ops)
{
    if (ops == NULL || ops->draw == NULL) {
        memset(&s_ops, 0, sizeof(s_ops));
        return;
    }
    s_ops = *ops;
}

bool punch_watermark_available(void)
{
    return s_ops.draw != NULL;
}

bool punch_watermark_apply(uint8_t *rgb888, int w, int h,
                           const char *name, const char *time)
{
    if (s_ops.draw == NULL) {
        return false;
    }
    s_ops.draw(rgb888, w, h, name, time);
    return true;
}

void punch_watermark_release(void)
{
    if (s_ops.release != NULL) {
        s_ops.release();
    }
}

bool punch_watermark_active(void)
{
    return (s_ops.active != NULL) ? s_ops.active() : false;
}
