/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_bus.c
 * @brief L1 平台层：板载总线实现（允许 include 板级包的唯一处之一）
 */

#include "platform/port_bus.h"

#include "esp_err.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "PORT_BUS";

void bus_i2c_init(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c init failed: %s", esp_err_to_name(err));
    }
}
