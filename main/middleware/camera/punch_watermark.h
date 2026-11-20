/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_watermark.h
 * @brief L2 服务：打卡水印**渲染槽**（纯接口，不含任何 LVGL 类型）
 *
 * 为什么要有这个槽（评估报告 P2 第 4 条）：
 *   打卡业务（middleware/ai/ai_punch.c）原来自己 `lv_canvas_create` + `lv_draw_label`
 *   往相机帧上压水印 —— 一个跑在 WS 收包任务里的业务，硬绑了 LVGL、UI 主题字体
 *   和 `ui/ui_theme.h`。业务向上依赖 UI（G1），中间层吃 LVGL（G4）。
 *
 *   现在责任反过来：**谁能画就谁来注册**。UI 在启动时 install 一组回调，
 *   业务只会说"给我在这帧上压一行名字 + 一行时间"：
 *     · 没注册（UI 未起 / 极简构建）→ apply() 返回 false，业务照常出图，不带水印；
 *     · 注册了 → 由 UI 实现画（字体/配色/版式全归 UI 管，改视觉不动业务）。
 *
 * ★ 线程纪律：本槽只存一份函数表（单次 install，之后只读）；
 *   draw/release/active **必须在调用方已持有 LVGL 锁**的前提下调用
 *   （本层不自己加锁，避免与业务侧的锁序纠缠）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 渲染回调：在 rgb888 缓冲上就地压水印。
 *  @param rgb888 整帧 RGB888（由调用方提供，就地改写）
 *  @param w,h    帧尺寸
 *  @param name   姓名（可空）
 *  @param time   时间串（可空） */
typedef void (*punch_watermark_draw_fn)(uint8_t *rgb888, int w, int h,
                                        const char *name, const char *time);

typedef struct {
    punch_watermark_draw_fn draw;     /* 必填 */
    void (*release)(void);            /* 可空：会话结束释放临时资源（幂等） */
    bool (*active)(void);             /* 可空：是否仍持有临时资源 */
} punch_watermark_ops_t;

/** 注册渲染实现（UI 侧启动时调用；重复注册覆盖旧值）。 */
void punch_watermark_install(const punch_watermark_ops_t *ops);

/** 是否已注册实现。 */
bool punch_watermark_available(void);

/** 请求压水印。@return true = 已画；false = 无实现（业务无感继续）。 */
bool punch_watermark_apply(uint8_t *rgb888, int w, int h,
                           const char *name, const char *time);

/** 会话结束：释放渲染侧持有的临时资源（幂等；无实现时无操作）。 */
void punch_watermark_release(void);

/** 渲染侧是否还持有临时资源（用于"兜底防泄漏"分支）。 */
bool punch_watermark_active(void);
