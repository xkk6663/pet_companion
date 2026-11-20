/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_time.c
 * @brief L1 平台层：可信时间实现
 *
 * 状态只有一个 volatile bool —— 校时由单个网络任务写、多方读，不加锁
 * （写的是单字节 bool，读到的只可能是"旧值 false"或"新值 true"，不会撕裂；
 * 最坏情况是某次读晚一个 tick 看到 false，业务侧只是少记一次时间戳）。
 */

#include "platform/port_time.h"

#include <time.h>

#include "esp_timer.h"

static volatile bool s_synced;

void port_time_mark_synced(void)
{
    s_synced = true;
}

bool port_time_valid(void)
{
    return s_synced;
}

int64_t port_time_epoch(void)
{
    if (!s_synced) {
        return 0;
    }
    time_t now = 0;
    time(&now);
    return (int64_t)now;
}

int64_t port_time_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t port_time_uptime_us(void)
{
    return esp_timer_get_time();
}

bool port_time_date(int *year, int *mon, int *day)
{
    if (!s_synced) {
        return false;
    }
    time_t now = 0;
    struct tm tmv;
    time(&now);
    /* 与 net_time_service 的合理性判据同源：标志为真但时间戳明显不合理
     * （时钟被改 / TZ 没生效）⇒ 宁可报"不可信"。 */
    if (now <= (time_t)1600000000 || localtime_r(&now, &tmv) == NULL) {
        return false;
    }
    if (year != NULL) {
        *year = tmv.tm_year + 1900;
    }
    if (mon != NULL) {
        *mon = tmv.tm_mon + 1;
    }
    if (day != NULL) {
        *day = tmv.tm_mday;
    }
    return true;
}
