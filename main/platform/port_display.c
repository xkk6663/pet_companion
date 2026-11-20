/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file port_display.c
 * @brief L1 平台层：显示端口的**唯一**实现（本文件是整工程里允许 include
 *        `bsp/esp-bsp.h` 的少数几个文件之一）
 *
 * 迁移来源：app/main.c 里那段"照抄 bsp_display_start() 默认 cfg，只覆盖
 * task_stack"的初始化块，逐字搬来，行为不变（初始化顺序对内存最敏感，
 * 这里只搬不改：同样在 bsp_i2c_init 之后、ui_binfont_attach 之前）。
 */

#include "platform/port_display.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "PORT_DISP";

static lv_display_t *s_disp;

lv_display_t *disp_start(void)
{
    if (s_disp != NULL) {
        return s_disp;
    }

    /* 照抄 bsp_display_start() 的默认 cfg（见 bsp_display.c），只覆盖 task_stack。
     * 指定初始化器里不能先整体初始化 .lvgl_port_cfg 再覆盖子成员（-Woverride-init）。 */
    bsp_display_cfg_t dsp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BOARD_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = 1,
#else
        .double_buffer = 0,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    dsp_cfg.lvgl_port_cfg.task_stack = BOARD_LVGL_TASK_STACK;
    s_disp = bsp_display_start_with_config(&dsp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "display start failed");
    }
    return s_disp;
}

bool disp_lock(void)
{
    return bsp_display_lock(0);
}

bool disp_lock_timeout(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void disp_unlock(void)
{
    bsp_display_unlock();
}

esp_err_t disp_brightness_set(int percent)
{
    return bsp_display_brightness_set(percent);
}

lv_indev_t *disp_input_dev(void)
{
    return bsp_display_get_input_dev();
}

esp_err_t disp_fb_get(void **fb0, void **fb1)
{
    if (fb0 == NULL || fb1 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb0 = NULL;
    *fb1 = NULL;

    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    if (panel == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    void *f0 = NULL;
    void *f1 = NULL;
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &f0, &f1);
    if (err != ESP_OK || f0 == NULL) {
        ESP_LOGE(TAG, "get_frame_buffer failed: %s", esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_NOT_SUPPORTED;
    }
    *fb0 = f0;
    *fb1 = f1;
    return ESP_OK;
}
