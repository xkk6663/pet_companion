/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_main.h
 * @brief Main page: CALL / REC / SYS tab-view + pull-down wallpaper overlay.
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const Page_t Page_Main;

/* Rebind the encoder group to the active tab (formerly set_tab_group). */
void page_main_refresh_group(void);

/**
 * @brief 播放 SD 卡音频并**自动切到 MUSIC tab**（tab_files 点 .mp3 时调用）。
 * @param path SD 卡绝对路径（如 /sdcard/music/a.mp3）
 * @note  必须在 LVGL 线程调用。内部会先切页（触发 FILES 页收尾停播）再起播，
 *        并加载同名 .lrc；找不到则歌词区显示"无"。
 */
void page_main_play_music(const char *path);

#ifdef __cplusplus
}
#endif
