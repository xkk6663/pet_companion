/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file companion_stats.h
 * @brief 陪伴时长累计存档（SD 卡，按"日期分桶"落盘）
 *
 * 需求（用户定案）：
 *   · 有日期（NTP 已校时）→ 记到**当天**这一桶；
 *   · 没有日期（未校时/断网）→ 记到 **nodate 池**（所有"无日期日子"的合计）；
 *   · 计时本身**不依赖网络**：秒数来自 esp_timer，日期只在落盘那一刻才需要。
 *
 * 存储布局（一桶一个文件，避免整文件重写 + 解析错行）：
 *   /sdcard/companion/d_2026-11-19.txt   ← 文件内容就是十进制秒数
 *   /sdcard/companion/d_nodate.txt       ← 无日期池（未校时的那些日子）
 *   /sdcard/companion/d_total.txt        ← ★ 总池：只要计时器在跳，它就一直涨
 *                                          （= 各日期桶之和；未校时的增量也算进去）
 *
 * ★ 并发约定：只在 **LVGL 任务**里调用（tab_camera 的 50 ms 刷新 + 页面销毁）。
 * ★ 落盘节流：`add()` 内部攒够 30 s 才写一次；暂停/离开页面调 `flush()` 强制写。
 * ★ SD 没插/没挂载：只打一次 WARN，秒数留在 RAM 里不丢，等下次 flush 再试。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 累加秒数（内部按 30 s 节流落盘）。sec <= 0 直接忽略。任意线程安全（不做阻塞 IO 之外的加锁） */
esp_err_t companion_stats_add(int64_t sec);

/** 强制落盘（暂停 / 人脸离开 / 页面销毁时调；没攒到 30 s 也会写） */
esp_err_t companion_stats_flush(void);

/** 读总池（d_total.txt 的秒数）。SD 未挂载/文件不存在返回 0。任意线程可调。 */
int64_t companion_stats_get_total(void);

/** 读无日期池（d_nodate.txt 的秒数）。SD 未挂载/文件不存在返回 0。任意线程可调。 */
int64_t companion_stats_get_nodate(void);

/** 写盘自检：建目录 → 写 tmp → stat 看大小 → rename，逐步打日志（只给排查用） */
void companion_stats_selftest(void);

/* ============================================================================
 *  运行时快照 + 明细枚举（给局域网监视面板 /api/companion/stats 用）
 *
 *  ★ 分工：秒数仍在 UI 里算（tab_camera 的 50 ms 刷新），这里只做"镜像"——
 *    UI 每拍调 report_session() 把当前读数丢进来，Web 线程只读这几个标量，
 *    不碰 LVGL 对象、不反向依赖 UI 模块。
 *  ★ 32 位：RISC-V 上 64 位读写不是原子的，所以镜像用 int32（68 年足够）。
 * ========================================================================== */

/** UI 每拍上报：本次会话秒数 / 是否在跑 / 是否暂停。任意线程可调。 */
void companion_stats_report_session(int32_t sec, bool running, bool paused);

/** 读镜像（Web 线程用）：返回当前会话秒数，running/paused 通过出参回填（可为 NULL） */
int32_t companion_stats_get_session(bool *running, bool *paused);

/** 一条"按日桶"明细（Web 面板按日列表用） */
typedef struct {
    char    date[11];   /* "2026-11-19"（文件名里的 YYYY-MM-DD） */
    int32_t sec;
} companion_day_t;

/**
 * 枚举 /sdcard/companion 下的 d_YYYY-MM-DD.txt，按日期**升序**写入 out。
 * @return 实际条数（0 = 没卡 / 没有记录）；超过 max 的截断（保留最近的 max 天）
 */
int companion_stats_list_days(companion_day_t *out, int max);

#ifdef __cplusplus
}
#endif
