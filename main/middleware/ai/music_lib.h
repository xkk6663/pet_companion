/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file music_lib.h
 * @brief SD 曲库（S8 / §73.28）：/sdcard/music/ 的命名、查询、落盘、列表、空间预检。
 *
 * 与媒体库（media_player_service / FILES 页）的关系：本模块只管「点歌落盘」
 * 这一条线，产出标准 mp3/lrc 文件；FILES 页扫目录时天然能看到这些文件并可播放。
 *
 * 落盘铁律（S8 §6.2 / README §8.3）：
 *   - 路径："/sdcard/music/" = 14 + 文件名 + NUL ≤ FM_PATH_MAX(128) ⇒ 文件名 ≤ 110 字节
 *   - 命名清洗：替换 / \ : * ? " < > | 和控制字符 → '_'；再按 UTF-8 边界截断
 *   - 原子性：先写 "<file>.part"；字节数对上 Content-Length 才 rename 成 .mp3
 *   - 残留下场：失败/超时 → 删 .part（否则 FILES 页出现"能看见但播不了"的鬼文件）
 *   - 空间预检：free < need + 1MB 直接拒绝
 *
 * ★ 本模块函数跑在下载任务（PSRAM 栈 8KB + 内部 DMA 缓冲 ≤4KB）：
 *   FATFS/SD 写不碰 flash，PSRAM 栈允许（F35 只拦 flash IO 栈）；但下载缓冲
 *   必须内部 RAM（esp_http_client buffer_size=2048，ON_DATA 逐段 fwrite）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "music_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_LIB_DIR   "/sdcard/music"

/** 曲库条目（list 输出） */
typedef struct {
    char path[128];       /* /sdcard/music/xxx.mp3（FM_PATH_MAX=128） */
    char title[64];       /* 无扩展名文件名（清洗后） */
    size_t bytes;
} music_item_t;

/** SD 已挂载 + 目录存在（不存在则自动创建）。没插卡时返回 false，不阻塞。 */
bool music_lib_ready(void);

/** 按 mtime 倒序列出曲库条目。返回实际条数；skip/take 分页。 */
int music_lib_list(music_item_t *out, int cap, int skip, int take);

/** 按关键词模糊命中（文件名/歌手，大小写不敏感）。命中填 path 返回 true。 */
bool music_lib_find(const char *keyword, char *path, size_t cap);

/**
 * @brief 落盘：resolve 拿直链/大小 → 空间预检 → 流式下载 .part → rename .mp3。
 *
 * 流程（README §8.3）：search 已在外面做，这里接收 id + cand；
 *   ① resolve(id, br) 拿 size（无版权 → ESP_ERR_NOT_FOUND）；
 *   ② music_lib_has_space(size + 1MB) 不过 → ESP_ERR_NO_MEM；
 *   ③ 文件名 = "<artist>-<name>.mp3"（清洗 + UTF-8 截断 ≤110B）；
 *   ④ GET /v1/download 流式写 .part（esp_http_client buffer 2048 内部）；
 *   ⑤ bytes_written == Content-Length → rename .mp3，否则删 .part。
 *
 * @param out_path 成功填最终 mp3 绝对路径（cap ≤ 128）
 * @param out_bytes 实际写入字节数
 */
esp_err_t music_lib_save(const char *id, const music_cand_t *cand, int br,
                         char *out_path, size_t cap, size_t *out_bytes);

/** 剩余空间 ≥ need + 1MB 才 true（S8 §3.2 空间预检）。 */
bool music_lib_has_space(size_t need_bytes);

/** 写同名 .lrc（失败不阻塞：歌词缺失不是错误）。 */
esp_err_t music_lib_write_lrc(const char *mp3_path, const char *lrc);

#ifdef __cplusplus
}
#endif
