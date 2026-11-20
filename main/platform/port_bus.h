/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_bus.h
 * @brief L1 平台层：板载总线初始化（**app 不直接调 BSP**）
 *
 * app/main.c 原来直接调 bsp_i2c_init()（板级包 API）。总线归平台层，
 * 启动顺序不变（仍在 sdcard_service_mount 之后、显示启动之前）。
 */
#pragma once

/** 初始化板载 I2C（音频 codec / 触摸 / 传感器共用）。失败只打日志不 abort。 */
void bus_i2c_init(void);
