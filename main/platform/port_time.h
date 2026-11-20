/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_time.h
 * @brief L1 平台层：**唯一可信时间入口**（横切能力，向下沉淀）
 *
 * 背景（评估报告"反向依赖"第 1 条）：`net_time_service_get()` / 校时有效位
 * 被 14 处跨层使用（UI 的相机页/壁纸层、业务的打卡/切片、以及 led_service
 * 这种驱动），于是 LED 驱动不得不像上反向引用网络服务 —— 一个纯驱动居然
 * 依赖"WiFi 校时"。
 *
 * 解法：把"现在几点了、时间可信吗"沉到 L1，变成一个没有任何依赖的端口：
 *   · 网络服务（L2/L3）在校时成功后**向下**推送 port_time_mark_synced()；
 *   · 任何层（含驱动）只读 port_time_valid() / port_time_epoch()。
 * 于是依赖方向永远向下，驱动再也不会因为"想知道时间"而依赖云。
 *
 * ★ 语义边界：本层只回答"有没有可信墙钟 + 单调计时"，**不**解析天气、
 *   不**做时区换算（TZ 由 net_time_service 在校时前 setenv 设好，time()
 *   会自己吃 TZ）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 校时成功（由 net_time_service 在 SNTP 同步完成后调用）。幂等。 */
void port_time_mark_synced(void);

/** 墙钟是否可信（false = 还没校过时，任何时间戳都不许写进落盘记录）。 */
bool port_time_valid(void);

/** 墙钟 epoch 秒；未校时返回 0（调用方据 port_time_valid 判空，别用 0 当 1970）。 */
int64_t port_time_epoch(void);

/** 单调开机计时（毫秒 / 微秒），等价 esp_timer_get_time()，但**不暴露 esp_timer 头**。 */
int64_t port_time_uptime_ms(void);
int64_t port_time_uptime_us(void);

/**
 * 本地日期（已按 TZ 换算）。用于"按日期分桶"这类落盘命名。
 * @return true = 时间可信且日期合法；false = 未校时/时间戳不合理（调用方落"无日期"桶）
 *
 * ★ 合理性判据与 net_time_service 同源（epoch > 1600000000），避免时钟被改后
 *   写出一批错的日期桶。
 */
bool port_time_date(int *year, int *mon, int *day);
