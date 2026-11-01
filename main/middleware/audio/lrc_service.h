/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lrc_service.h
 * @brief 中间层：LRC 歌词解析（与 media_player_service 播放位置联动）
 *
 * 设计约束：
 *  - 只做"加载 → 解析 → 按时间取行"，不依赖 LVGL，任意线程可读（无锁只读）；
 *  - 文件整体读入 PSRAM（上限 LRC_MAX_FILE_SIZE），解析后行文本指向该缓冲，
 *    卸载时一次性释放，避免逐行 malloc 造成碎片；
 *  - 只接受 UTF-8（含 BOM）；GBK/其它编码的行**优雅丢弃**（不崩、不显示乱码）。
 *
 * 路径规则：与音频同名、扩展名换成 .lrc
 *   如 /sdcard/music/a.mp3 → /sdcard/music/a.lrc
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按音频路径加载同名 .lrc（覆盖式；旧歌词自动卸载）
 * @param audio_path 音频文件绝对路径（/sdcard/...）
 * @return ESP_OK 歌词可用；ESP_ERR_NOT_FOUND 无同名 lrc / 无可解析行；
 *         ESP_ERR_NO_MEM 文件过大或内存不足；其它为文件错误
 * @note  任意任务上下文可调（内部持锁，不与读取并发冲突）
 */
esp_err_t lrc_service_load(const char *audio_path);

/** 卸载歌词并释放内存（未加载时无害） */
void lrc_service_unload(void);

/** 是否有可显示的歌词（false 时 UI 显示"无"） */
bool lrc_service_has_lyrics(void);

/** 已解析的歌词行数 */
int lrc_service_line_count(void);

/**
 * @brief 取"当前时间对应行"的索引（含 LRC_LEAD_MS 提前量）
 * @param pos_ms 播放位置（毫秒）
 * @return 行索引；无歌词或早于第一行返回 -1
 */
int lrc_service_find_index(int pos_ms);

/**
 * @brief 取指定行文本
 * @return 以 '\0' 结尾的行文本；索引越界返回 NULL
 */
const char *lrc_service_line_text(int index);

#ifdef __cplusplus
}
#endif
