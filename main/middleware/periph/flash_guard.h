/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file flash_guard.h
 * @brief 运行期 flash 写静默窗口（P0）
 *
 * 背景：spi_flash 擦写会走 `spi_flash_disable_interrupts_caches_and_other_cpu()`，
 * 关中断 + 关 cache（PSRAM 与 flash 映射共用同一 cache），单次窗口可达
 * 94~157 ms（见 wifi_manager.c 的 F48 注释）。窗口内任何核对 PSRAM 的访问
 * 都可能 Cache error，RGB 屏 GDMA 持续读帧缓冲也会断流。
 *
 * 本模块只做**低风险收尾**，不试图冻结整个系统：
 *   ① 录像中/收尾中 ⇒ 直接拒绝本次 flash 写（由调用方延后，绝不打断录像）；
 *   ② 取景器开着 ⇒ `viewfinder_disable()`，写完再 `enable()`；
 *   ③ 本地音乐正在播 ⇒ `media_player_service_pause()`，写完 `resume()`。
 *
 * 线程纪律：begin/end 必须在**同一个任务**里成对调用。内部互斥锁只保证
 * 不并发，不解决跨任务配对问题。
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 静默窗口的"让出者"回调表（定义见 .c 顶部注释；接线在 app/main.c）。 */
typedef struct {
    const char *name;      /* 日志用，如 "recording" / "viewfinder" / "music" */
    bool (*busy)(void);    /* 可空：true = 现在绝不能写 flash */
    bool (*calm)(void);    /* 可空：让出资源，true = 本次动过（end 要 restore） */
    void (*restore)(void); /* 可空：与 calm 成对 */
} flash_guard_client_t;

/** 初始化互斥锁（幂等；main 在 wifi 启动前调一次）。 */
void flash_guard_init(void);

/**
 * @brief 登记一个"让出者"（★ P2：本模块不再认识任何具体业务模块）
 *
 * 语义由回调表达，接线在 app/main.c（组合根）做：
 *   · busy()    true = 此刻绝对不能写 flash（如录像中/收尾中）⇒ begin 直接拒绝；
 *   · calm()    true = 本项这次确实让出了资源（停取景/暂停播放）⇒ end 会 restore；
 *   · restore() 反向恢复，与 calm 成对。
 *
 * @param client 静态生命周期（调用方保证常驻），最多 4 个。
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM（表满）
 */
esp_err_t flash_guard_register(const flash_guard_client_t *client);

/**
 * @brief 打开静默窗口。
 *
 * @return ESP_OK            已暂停取景/音乐，可以安全写 flash；
 *         ESP_ERR_INVALID_STATE 正在录像/录像收尾（调用方应延后，不要强写）；
 *         ESP_ERR_TIMEOUT       另一个 flash 写还没结束。
 */
esp_err_t flash_guard_begin(const char *reason);

/** 关闭静默窗口：恢复音乐与取景。未 begin 时无害。 */
void flash_guard_end(void);

/** 当前是否处于静默窗口（MJPEG 等长流可据此降速，避免抢 PSRAM 带宽）。 */
bool flash_guard_is_active(void);

#ifdef __cplusplus
}
#endif
