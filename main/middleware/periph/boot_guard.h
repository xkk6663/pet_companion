/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file boot_guard.h
 * @brief 启动失败计数 + 安全模式（P0）
 *
 * 目标：现场出现"开机即 panic / 初始化死循环"时，连续 3 次未能跑到
 * `boot_guard_mark_ready()` 就进入**安全模式**：只挂载文件系统、连 Wi-Fi、
 * 起 Web 管理端，跳过相机/音频/模型/录像等重初始化，让人还能从网页
 * `/api/recovery/clear` 清计数再重启。
 *
 * 计数存 NVS（不在 RTC_NOINIT）：
 *   · 本板实测每次复位都是 POWERON(1)，RTC_NOINIT 计数不保留（2026-11-19 实机）；
 *   · NVS 跨 panic / 看门狗 / 软复位 / 断电都可靠；
 *   · 启动时 +1、正常流程走完归 0；用户拔电重插也不会被锁在安全模式。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化/推进计数（app_main 早期调用一次）。 */
void boot_guard_init(void);

/** 本次启动是否应进入安全模式（连续失败 >= 3）。 */
bool boot_guard_should_enter_safe_mode(void);

/** 正常启动走到 UI/Web 就绪后调用：清计数并置 ready。 */
void boot_guard_mark_ready(void);

/** 人工清除失败计数（Web `/api/recovery/clear` 用）。 */
void boot_guard_clear(void);

/** 当前连续失败次数（诊断日志/Web 状态用）。 */
uint32_t boot_guard_failure_count(void);

#ifdef __cplusplus
}
#endif
