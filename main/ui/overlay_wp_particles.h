/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lvgl.h"

lv_obj_t *overlay_wp_particles_build(lv_obj_t *parent);

/**
 * @brief 预分配粒子层 canvas 缓冲（800×480 RGB888 ≈ 1.15 MB PSRAM，幂等）。
 *
 * splash 现在是"长按 SET 按需拉起的屏保页"，会反复开关；缓冲做成进程内唯一、
 * 每次打开复用。在开机内存宽裕时先拿稳，避免第一次长按屏保时临时抢大块失败。
 */
void overlay_wp_particles_prealloc(void);

void overlay_wp_particles_stop(void);
