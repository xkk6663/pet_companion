/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_server.h
 * @brief 中间层：网页管理端（§74）—— APSTA 热点/局域网内浏览器直接管理设备
 *
 * 查询类（原 §74）：
 *   1. GET /                    内嵌单页 HTML（手机/PC 通用，响应式）
 *   2. GET /api/punch/last      打卡事件轮询（代际号；esp_http_server 单任务
 *                               串行模型不支持阻塞式 SSE，故前端 1s 一拍）
 *   3. GET /api/attendance/list 打卡流水 JSON（读 punch_service 现役 CSV 解析）
 *   4. GET /api/media/list      SD 卡目录 JSON（白名单 /sdcard/ 之下）
 *   5. GET /api/media/file      照片/文件内联浏览（浏览器直接显示）
 *   6. GET /api/media/info      AVI(MJPG) 元信息 JSON（现役 avi_demux）
 *   7. GET /api/media/frame     AVI 单帧 JPEG（JS 逐帧轮询播放，全平台通用）
 *   （另有 /api/media/frames、/api/wallpaper/{list,set}、/api/upload、/api/file
 *    —— 见 §75 补充 2）
 *
 * 动作类（S9 §3.5，长事务一律"受理即返回 + gen 槽轮询"，红线 R9）：
 *   8. POST /api/punch             触发人脸打卡    → 202 + /api/action/result
 *   9. POST /api/media/capture     拍照存 SD       → 202 + 同上
 *  10. POST /api/media/play?url=   播放 URL（TTS/点歌自测）→ 202
 *  11. POST /api/light/set         灯控 {"power","rgb","brightness"} → 200 状态
 *  12. GET  /api/light/get         灯状态
 *  13. POST /api/system/reboot     有序软重启（延迟 200ms 起任务）
 *  14. GET  /api/system/status     wifi/link/session/busy/heap 一屏全看
 *  15. GET  /api/action/result?gen=N  动作结果（gen 对齐回代际号，否则给结论）
 *
 *  ★ 为什么需要动作端点：S9 把"编排"搬到云端后，云端插件与联调脚本需要
 *    能**主动**驱动设备（打卡/拍照/灯/重启），而原来 12 条路由全是只读。
 *  ★ 设备在 NAT 后面时云端打不进来 ⇒ 同一批动作另有 WS `system` 命令等价物
 *    （见 event_bus.c：explain/capture/punch/light/set_volume/reboot）。
 *
 * 安全边界：
 *   - 仅局域网/AP 热点（无公网暴露），AP 侧已有 WPA2 密码；
 *   - 文件路径强校验：必须以 "/sdcard/" 开头且不含 ".."（防路径穿越）；
 *   - 写 SD 的只有白名单三条：`/api/upload`（wallpaper|music）、
 *     `/api/wallpaper/set`、`/api/media/capture`（capture_service 自己的领地）；
 *     其余端点一律只读 —— 仍然遵守"只动自己领地"红线。
 *
 * 线程模型：
 *   - httpd 内部任务串行处理请求（stack 8 KB，handler 必须有界返回）；
 *   - 打卡事件：punch 完成回调只更新"代际号 + 快照"槽（互斥锁保护），
 *     前端 1 s 轮询比对 gen —— 多客户端（手机 + PC）天然支持，无连接占用；
 *   - 动作端点：动作槽（s_act）只记"动作名 + gen + 受理时刻"，**结论不抄**，
 *     一律去 ai_punch / capture_service 现取（抄一份会静默过期）。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动网页管理端（幂等）：注册 punch 完成回调 + 起 httpd:80。
 *
 * ★ 不挑时机：httpd 绑 0.0.0.0，AP/STA 任一接口拿到 IP 后即可访问；
 *   建议在各 service_init 之后调用（订阅 punch 回调需 punch_service_init 已跑）。
 * @return ESP_OK / 启动失败的 esp_err_t（不打断系统启动，只打 ERROR 日志）
 */
esp_err_t web_server_init(void);

#ifdef __cplusplus
}
#endif
