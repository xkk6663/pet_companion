/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_music.h
 * @brief 本地音乐播放 tab（page_main tabview）：歌词 / 进度 / 媒体键 +
 *        蓝牙 A2DP 输出（扫描-连接-推流）与三态输出选择（本页不做封面 UI）。
 *
 * 激活语义（page_main 的 tab_changed_event 里调，**必须在 page_main_refresh_group()
 * 之前**）：
 *   - set_active(false)：离开页面必停（doc §10 纪律）+ lrc_unload；
 *   - set_active(true)：刷新播放态显示。
 *
 * 自动跳转：tab_files 点 .mp3 时由 page_main_play_music() 先切到本 tab 再起播，
 *   同时加载同名 .lrc（找不到则歌词区显示"无"）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_music_build(lv_obj_t *screen, lv_group_t *group);
void tab_music_destroy(void);
void tab_music_set_active(bool active);

/**
 * @brief 播放指定音频（并加载同名 lrc）。必须在 LVGL 线程（已切到本 tab 后）调用。
 * @param path SD 卡绝对路径，如 /sdcard/music/a.mp3
 */
void tab_music_play_path(const char *path);

#ifdef __cplusplus
}
#endif
