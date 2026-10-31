/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file music_source.h
 * @brief 云侧音源客户端（S8 / §73.28）：对接云端 music_proxy 的 /v1 接口。
 *
 * 契约来源：tools/music_proxy/README.md §6（设备侧接口文档）。
 *   GET /v1/search?q=&limit=   → data[]: id/name/artist/duration_ms/cover
 *   GET /v1/url?id=&br=        → data.url + data.size（直链过期服务端自动重取）
 *   GET /v1/lyric?id=          → data.lrc（可能为空串，不是错误）
 *   GET /v1/download?id=&br=   → 音频字节流（支持 Range，错误 502）
 *   鉴权：Header X-Music-Token 或 query token=；/healthz 免鉴权。
 *   错误码：401(token)/429(限流)/400(缺参)/404(搜不到或无版权)/502(上游挂)/503(未配token)
 *
 * ★ 配置：NVS namespace `music_cfg`，两个字符串键：base、token。
 *   首次为空时走编译宏默认值（MUSIC_BASE_URL_DEFAULT / MUSIC_TOKEN_DEFAULT，
 *   由 CMakeLists 从环境变量注入——token 绝不写进 git 跟踪的源码）。
 *
 * ★ 所有函数阻塞，只允许在专用任务里调用（PSRAM 栈 12KB）。
 * ★ body 缓冲 16KB 一律 heap_caps_malloc(MALLOC_CAP_SPIRAM)，用完即 free。
 * ★ http(s) 分流：base 以 https:// 开头时挂 crt_bundle_attach，http 不挂。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 搜索结果候选（S8 §6.1，字段对齐 music_proxy data[]） */
typedef struct {
    char id[96];          /* 不透明串（ncm./gd./qj. 前缀），原样回传 resolve/lyric */
    char name[64];
    char artist[64];
    uint32_t duration_ms;
    char cover[192];      /* 可能为空（gd 源），一期不落盘 */
} music_cand_t;

/** 读 NVS 配置（不联网）。base/token 为空时回落到编译宏默认值。 */
esp_err_t music_source_init(void);

/** 写 NVS（替换 base/token，立即生效）。任一个传 NULL 表示保留原值。 */
esp_err_t music_source_set_cfg(const char *base, const char *token);

const char *music_source_get_base(void);
const char *music_source_get_token(void);

/**
 * @brief 搜索：GET /v1/search?q=&limit=
 * @return 实际条数（>0 成功；0 = 搜不到；负 = 网络/解析错误）
 */
int music_source_search(const char *q, int limit, music_cand_t *out, int cap);

/**
 * @brief 取链：GET /v1/url?id=&br=
 * @param url 成功填 http/https 直链；url[0]=='\0' 表示该曲无版权/无链（ESP_OK）
 * @param size 返回 Content-Length（可能为 0）
 */
esp_err_t music_source_resolve(const char *id, int br, char *url, size_t cap,
                               size_t *size);

/**
 * @brief 歌词：GET /v1/lyric?id=
 * @param lrc 成功填 LRC 文本；无歌词时为空串（不算错误）
 */
esp_err_t music_source_lyric(const char *id, char *lrc, size_t cap);

/**
 * @brief 组「播放器直吃」的下载 URL：{base}/v1/download?id=<id>&br=<br>&token=<token>
 *
 * ★ README §8.2 的关键简化：播放/下载走同一入口，端侧不解析 302、不管直链过期，
 *   代理已处理。URL 长度 ≈ base(≤48) + 路径(21) + id(≤96) + br(3) + token(32) < 200B。
 * @param token 传 NULL 时自动追加当前配置 token；base 为 https 也适用。
 */
esp_err_t music_source_build_download_url(const char *id, int br, char *url,
                                          size_t cap);

#ifdef __cplusplus
}
#endif
