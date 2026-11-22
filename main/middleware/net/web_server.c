/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_server.c
 * @brief 网页管理端实现（§74）—— 见 web_server.h 总述
 *
 * 实现纪律：
 *  ★ 只读：所有 SD 访问都是 fopen/opendir 只读路径，绝不创建/删除/改写；
 *  ★ 不占大内存：文件与 JSON 一律 4 KB 块流式发送（resp_send_chunk 循环），
 *    唯一的大缓冲是 AVI 单帧读缓冲（256 KB PSRAM，按请求分配用完即还，
 *    绝不静态占用 —— 给 §73 的 face 模型 1.29 MB 连续 PSRAM 让路）；
 *  ★ ★★ 打卡事件用"代际号轮询"而**不是 SSE**（2026-10-01 实现期修正）：
 *    esp_http_server 是**单任务串行**模型 —— handler 不返回，其它请求全部
 *    饿死。阻塞式 SSE handler 是这条铁律的教科书反例。故 punch 完成回调
 *    只更新"代际号 + 快照"槽，前端每 1 s GET /api/punch/last?gen=N 轮询，
 *    gen 变了才带全量记录 —— 无长连接、无 fd 占用、天然支持多客户端。
 *    显示延迟 ≈ 1 s，打卡场景完全够用；
 *  ★ 联调日志：每个请求打 [WEB] 一行（handler + 客户端 IP + 关键参数 +
 *    结果 + 耗时）—— 手机端不通时照日志逐环定位。
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"                   /* ★ S9 §3.5：/api/system/status 里的 RSSI */
#include "lwip/sockets.h"

#include "middleware/periph/sdcard_service.h"
#include "middleware/periph/companion_stats.h"      /* ★ 陪伴时长：监视面板数据源 */
#include "middleware/net/net_time_service.h"       /* ★ 校时状态（今日桶名字） */
#include "middleware/camera/punch_service.h"
#include "middleware/camera/camera_service.h"       /* /stream.mjpg：720p JPEG 直出 / 占用查询 */
#include "middleware/camera/jpeg_util.h"            /* ★ §76：截图 JPEG 软编 */
#include "middleware/media/record_video_service.h"  /* P1：录像中拒绝 MJPEG */
#include "middleware/periph/boot_guard.h"           /* P0：/api/recovery/clear */
#include "middleware/periph/flash_guard.h"          /* P0：flash 窗口内给 MJPEG 降速 */
#include "middleware/net/screen_shot.h"             /* ★ §76：RGB framebuffer 直读 */
#include "middleware/media/avi_mjpeg_demuxer.h"
#include "middleware/media/wallpaper_service.h"     /* ★ §75 补充 2：壁纸管理 */
#include "middleware/net/web_server.h"
#include "middleware/net/web_action.h"       /* ★ P2：业务动作端点注册表（本文件不再认识业务） */
#include "middleware/net/wifi_manager.h"            /* ★ S9 §3.5：/api/system/status */
#include "middleware/periph/safe_reboot.h"          /* ★ S9 §3.5：/api/system/reboot */
#include "middleware/ai/xiaozhi_service.h"          /* ★ S9 §3.5：/api/media/play、状态 */
#include "middleware/ai/ws_client.h"                /* ★ S9 §3.5：状态里的链路信息 */
#include "ui/tab_video.h"                           /* ★ 远程监控：让出设备端相机（内部持 disp_lock） */

static const char *TAG = "WEB";

/* ---------------------------------------------------------------------------
 *  内嵌单页 HTML（CMake EMBED_FILES "web/index.html"）
 *  ★ 符号命名实证（build/index.html.S）：IDF data_file_embed_asm 只按
 *    **文件名**生成符号（index.html → _binary_index_html_*），不吃
 *    目录前缀 —— 首轮链接 undefined reference 的根因。
 * ------------------------------------------------------------------------- */
extern const unsigned char web_index_start[] asm("_binary_index_html_start");
extern const unsigned char web_index_end[]   asm("_binary_index_html_end");

/* ---------------------------------------------------------------------------
 *  打卡事件槽：代际号 + 快照 + 互斥锁（punch 回调写，轮询 handler 读）
 * ------------------------------------------------------------------------- */
typedef struct {
    SemaphoreHandle_t mtx;
    punch_record_t    rec;
    uint32_t          gen;          /* 每完成一次事务 +1（0 = 还没有事件） */
} evt_slot_t;

static evt_slot_t s_evt;

/* punch 完成回调（punch_task 上下文）：只许快拷 + gen++，绝不阻塞 */
static void on_punch_done(const punch_record_t *rec, void *ctx)
{
    (void)ctx;
    if (rec == NULL || s_evt.mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_evt.mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_evt.rec = *rec;
        s_evt.gen++;
        uint32_t g = s_evt.gen;
        xSemaphoreGive(s_evt.mtx);
        /* ★§74 联调锚点：每次打卡事务上抛必打这行 */
        ESP_LOGI(TAG, "evt gen=%u type=%s err=%s name=%s",
                 (unsigned)g, rec->clear_op ? "clear" : "punch",
                 esp_err_to_name(rec->err), rec->name);
    }
}

/* ---------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------- */

#define IPSTR_LEN 16

/** 请求联调日志：handler 名 + 客户端 IP */
static void log_req(httpd_req_t *req, const char *what)
{
    int fd = httpd_req_to_sockfd(req);
    char ip[IPSTR_LEN] = "?";
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (fd > 0 && getpeername(fd, (struct sockaddr *)&addr, &len) == 0 &&
        addr.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    }
    ESP_LOGI(TAG, "%s <- %s", what, ip);
}

/** 就地 URL 解码（%XX；路径里不该有 '+'，不做加号转换以免毁名） */
static void url_decode(char *s)
{
    char *w = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/** 从 query 里取 key 的值并 URL 解码。返回 ESP_OK / ESP_ERR_NOT_FOUND */
static esp_err_t query_get(httpd_req_t *req, const char *key,
                           char *out, size_t outlen)
{
    static char qbuf[512];              /* httpd 单任务串行，static 无竞争 */
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (httpd_query_key_value(qbuf, key, out, (size_t)outlen) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    url_decode(out);
    return ESP_OK;
}

/**
 * 路径安全闸：必须以 "/sdcard/" 开头、不含 ".."、前缀之后非空。
 * ★ 联调期所有 bad_path 都先看这条日志 —— 十有八九是前端没 encode。
 */
static bool path_safe(const char *p)
{
    if (p == NULL || strncmp(p, SDCARD_MNT_PATH "/", 8) != 0) {
        ESP_LOGW(TAG, "path rejected (prefix): %s", p ? p : "(null)");
        return false;
    }
    if (strstr(p, "..") != NULL) {
        ESP_LOGW(TAG, "path rejected (dotdot): %s", p);
        return false;
    }
    if (strlen(p) <= 8) {
        ESP_LOGW(TAG, "path rejected (empty): %s", p);
        return false;
    }
    return true;
}

/** JSON 字符串转义（引号/反斜杠/控制符）——姓名、文件名都过这里 */
static const char *json_esc(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    if (dstlen == 0) {
        return dst;
    }
    for (; *src && o + 6 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, dstlen - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;         /* UTF-8 原样透传 */
        }
    }
    dst[o] = 0;
    return dst;
}

/** 发送一个小 JSON 响应（错误语义放 body 里，前端统一按 error 字段处理） */
static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/** 按扩展名给 Content-Type */
static const char *mime_by_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".csv"))  return "text/csv";
    if (!strcasecmp(ext, ".txt"))  return "text/plain";
    if (!strcasecmp(ext, ".wav"))  return "audio/wav";
    /* ★ §74 补充 5：浏览器原生解码音频 —— <audio> 直接吃这些 MIME */
    if (!strcasecmp(ext, ".mp3"))  return "audio/mpeg";
    if (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".aac")) return "audio/aac";
    if (!strcasecmp(ext, ".flac")) return "audio/flac";
    if (!strcasecmp(ext, ".ogg"))  return "audio/ogg";
    return "application/octet-stream";
}

/* ---------------------------------------------------------------------------
 *  Handler 1：GET /  内嵌单页
 * ------------------------------------------------------------------------- */
static esp_err_t handler_index(httpd_req_t *req)
{
    log_req(req, "index");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = (size_t)(web_index_end - web_index_start);
    esp_err_t e = httpd_resp_send(req, (const char *)web_index_start, (ssize_t)len);
    ESP_LOGI(TAG, "index sent %u B (%s)", (unsigned)len, esp_err_to_name(e));
    return e;
}

/* ---------------------------------------------------------------------------
 *  Handler：GET /api/companion/stats  陪伴时长快照（2 s 轮询）
 *
 *  一次性把面板要的东西全给出去：本次会话读数 + 今日/无日期/总池 + 最近 14 天。
 *  · 会话读数是 UI 的**镜像**（companion_stats_report_session），不碰 LVGL；
 *  · 桶数据现读 SD（每个文件就几十字节），一次请求最多 14 次读；
 *  · 手动拼 JSON：字段全是 ASCII + 安全数字，不需要 cJSON 的堆开销。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_companion_stats(httpd_req_t *req)
{
    log_req(req, "companion/stats");

    bool running = false, paused = false;
    int32_t sess = companion_stats_get_session(&running, &paused);

    net_time_t nt;
    memset(&nt, 0, sizeof(nt));
    net_time_service_get(&nt);

    bool sd = sdcard_service_is_mounted();
    int64_t total  = companion_stats_get_total();
    int64_t nodate = companion_stats_get_nodate();

    companion_day_t days[14];
    int nd = companion_stats_list_days(days, 14);

    char today[12] = "";
    int64_t today_s = 0;
    if (nt.valid) {
        snprintf(today, sizeof(today), "%04d-%02d-%02d", nt.year, nt.mon, nt.day);
        for (int i = 0; i < nd; i++) {
            if (strcmp(days[i].date, today) == 0) {
                today_s = days[i].sec;
                break;
            }
        }
    }

    size_t cap = 320 + (size_t)nd * 40;
    char *body = (char *)malloc(cap);
    if (body == NULL) {
        return send_json(req, "{\"ok\":0,\"err\":\"oom\"}");
    }

    int o = snprintf(body, cap,
                     "{\"ok\":1,\"sd\":%d,\"ntp\":%d,\"today\":\"%s\","
                     "\"sess\":%d,\"run\":%d,\"paused\":%d,"
                     "\"today_s\":%lld,\"nodate_s\":%lld,\"total_s\":%lld,\"days\":[",
                     sd ? 1 : 0, nt.valid ? 1 : 0, today,
                     (int)sess, running ? 1 : 0, paused ? 1 : 0,
                     (long long)today_s, (long long)nodate, (long long)total);

    for (int i = 0; i < nd && o > 0 && (size_t)o < cap; i++) {
        int r = snprintf(body + o, cap - (size_t)o, "%s{\"d\":\"%s\",\"s\":%d}",
                         (i > 0) ? "," : "", days[i].date, (int)days[i].sec);
        if (r < 0 || (size_t)r >= cap - (size_t)o) {
            break;
        }
        o += r;
    }
    if ((size_t)o < cap) {
        (void)snprintf(body + o, cap - (size_t)o, "]}");
    }

    esp_err_t e = send_json(req, body);
    free(body);
    return e;
}

/* ---------------------------------------------------------------------------
 *  Handler 2：GET /api/punch/last?gen=N  打卡事件轮询（1 s 一拍）
 *
 *   gen == 当前代 → {"gen":G}              （无新事件，前端继续等）
 *   gen != 当前代 → {"gen":G,"has":1,...}  （带全量快照；has=0 表示槽里
 *                                            还没有任何事务完成过）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_punch_last(httpd_req_t *req)
{
    log_req(req, "punch/last");

    char gen_s[16] = "0";
    query_get(req, "gen", gen_s, sizeof(gen_s));
    uint32_t client_gen = (uint32_t)strtoul(gen_s, NULL, 10);

    char body[768];
    char name_e[FACE_NAME_MAX * 2];
    char path_e[PUNCH_PATH_MAX * 2];

    xSemaphoreTake(s_evt.mtx, portMAX_DELAY);
    uint32_t cur_gen = s_evt.gen;
    int has_new = (cur_gen != client_gen);
    punch_record_t rec = s_evt.rec;
    xSemaphoreGive(s_evt.mtx);

    /* ★ body 直接填好交给 send_json（内部按 strlen 发送），不留"设而不读"
     *   的长度变量 —— GCC15 -Wunused-but-set-variable 在 -Werror 下是硬错误 */
    if (!has_new) {
        snprintf(body, sizeof(body), "{\"gen\":%u}", (unsigned)cur_gen);
    } else if (cur_gen == 0) {
        /* 槽还没发生过任何事务：把代号给出去，快照无意义 */
        snprintf(body, sizeof(body), "{\"gen\":0,\"has\":0}");
    } else {
        json_esc(name_e, sizeof(name_e), rec.name);
        json_esc(path_e, sizeof(path_e), rec.path);
        snprintf(body, sizeof(body),
                     "{\"gen\":%u,\"has\":1,\"type\":\"%s\",\"err\":\"%s\","
                     "\"name\":\"%s\",\"person_id\":%d,\"score\":%.3f,"
                     "\"path\":\"%s\",\"time_valid\":%s,\"elapsed_ms\":%u}",
                     (unsigned)cur_gen,
                     rec.clear_op ? "clear" : "punch",
                     esp_err_to_name(rec.err),
                     name_e, rec.person_id, (double)rec.score, path_e,
                     rec.time_valid ? "true" : "false",
                     (unsigned)rec.elapsed_ms);
        ESP_LOGI(TAG, "punch/last served gen=%u %s", (unsigned)cur_gen,
                 rec.clear_op ? "clear" : rec.name);
    }
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  Handler 3：GET /api/attendance/list  打卡流水（最新在前）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_attendance_list(httpd_req_t *req)
{
    log_req(req, "attendance/list");
    int64_t t0 = esp_timer_get_time();

    punch_log_t *logs = heap_caps_malloc(sizeof(punch_log_t) * PUNCH_LOG_MAX,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!logs) {
        return send_json(req, "{\"error\":\"no_mem\"}");
    }
    int n = punch_service_log_read(logs, PUNCH_LOG_MAX);

    /* 流式拼 JSON：大数组不落地（128 条 × ~120 B，落地也就 16 KB，但流式
     * 是本文件"不占大内存"纪律的统一姿势） */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "{\"rows\":[", -1);
    char line[512], name_e[FACE_NAME_MAX * 2], rel_e[PUNCH_REL_MAX * 2];
    for (int i = n - 1; i >= 0; i--) {          /* 倒序：最新的在数组头 */
        json_esc(name_e, sizeof(name_e), logs[i].name);
        json_esc(rel_e, sizeof(rel_e), logs[i].rel);
        int m = snprintf(line, sizeof(line),
                         "%s{\"seq\":%d,\"person_id\":%d,\"name\":\"%s\","
                         "\"date\":\"%s\",\"time\":\"%s\",\"rel\":\"%s\","
                         "\"score\":%.3f,\"epoch_s\":%lld}",
                         (i == n - 1) ? "" : ",",
                         logs[i].seq, logs[i].person_id, name_e,
                         logs[i].date, logs[i].time, rel_e,
                         (double)logs[i].score, (long long)logs[i].epoch_s);
        if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
            heap_caps_free(logs);
            ESP_LOGW(TAG, "attendance/list send fail");
            return ESP_FAIL;
        }
    }
    heap_caps_free(logs);
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "attendance/list %d rows in %u ms",
             n, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Handler 4：GET /api/media/list?path=/sdcard/xxx  目录 JSON
 * ------------------------------------------------------------------------- */
static esp_err_t handler_media_list(httpd_req_t *req)
{
    log_req(req, "media/list");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (query_get(req, "path", path, sizeof(path)) != ESP_OK ||
        !path_safe(path)) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }

    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed errno=%d", path, errno);
        return send_json(req, "{\"error\":\"opendir\"}");
    }

    char path_e[PUNCH_PATH_MAX * 2];
    json_esc(path_e, sizeof(path_e), path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[600];
    int hn = snprintf(head, sizeof(head), "{\"path\":\"%s\",\"entries\":[", path_e);
    httpd_resp_send_chunk(req, head, hn);

    struct dirent *ent;
    int count = 0;
    char line[560], nm_e[256];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        json_esc(nm_e, sizeof(nm_e), ent->d_name);
        /* FATFS dirent 的 d_type 不可靠 —— 一律 stat 确认 */
        /* ★ GCC15 format-truncation 纪律（同 record_video_service 的
         *   _Static_assert 惯例）：path 最坏 159 + '/' + d_name 最坏 255，
         *   必须 416 才装得下 —— 编译期锁死"最坏输出 < 容量"。 */
        char full[PUNCH_PATH_MAX + 256];    /* 160 + 256 = 416 */
        _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + 255,
                       "full must hold worst path + '/' + worst d_name");
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        int is_dir = 0;
        long long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
        }
        int m = snprintf(line, sizeof(line),
                         "%s{\"name\":\"%s\",\"dir\":%d,\"size\":%lld}",
                         count ? "," : "", nm_e, is_dir, size);
        if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
            closedir(d);
            ESP_LOGW(TAG, "media/list send fail");
            return ESP_FAIL;
        }
        count++;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "media/list %s -> %d entries in %u ms",
             path, count, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Handler 5：GET /api/media/file?path=...  文件内联浏览
 * ------------------------------------------------------------------------- */
static esp_err_t handler_media_file(httpd_req_t *req)
{
    log_req(req, "media/file");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (query_get(req, "path", path, sizeof(path)) != ESP_OK ||
        !path_safe(path)) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "file open fail %s errno=%d", path, errno);
        return send_json(req, "{\"error\":\"open\"}");
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *mime = mime_by_ext(path);
    ESP_LOGI(TAG, "file serve %s (%ld B, %s)", path, size, mime);

    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=60");

    static char fbuf[4096];                 /* httpd 单任务串行，static 安全 */
    long sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < size) {
        size_t want = (size_t)((size - sent) > 4096 ? 4096 : (size - sent));
        size_t got = fread(fbuf, 1, want, f);
        if (got == 0) {
            ESP_LOGW(TAG, "file short read @ %ld/%ld", sent, size);
            ret = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(req, fbuf, (ssize_t)got) != ESP_OK) {
            ESP_LOGI(TAG, "file client aborted @ %ld/%ld", sent, size);
            ret = ESP_FAIL;                 /* 客户端提前关页也算异常收场 */
            break;
        }
        sent += (long)got;
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    fclose(f);
    ESP_LOGI(TAG, "file done %s %ld/%ld B in %u ms", path, sent, size,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  Handler 6/7：GET /api/media/info 与 /api/media/frame（AVI 逐帧预览）
 * ------------------------------------------------------------------------- */
#define FRAME_BUF_SIZE  (256 * 1024)        /* 单帧 JPEG 上限（720p MJPG 亦足够） */

/** 取 AVI 路径参数（info/frame 共用） */
static esp_err_t get_avi_path(httpd_req_t *req, char *path, size_t len)
{
    if (query_get(req, "path", path, (size_t)len) != ESP_OK ||
        !path_safe(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".avi") != 0) {
        ESP_LOGW(TAG, "not an avi: %s", path);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t handler_media_info(httpd_req_t *req)
{
    log_req(req, "media/info");
    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    avi_demux_t *d = avi_demux_open(path);
    if (!d) {
        ESP_LOGW(TAG, "avi open fail %s", path);
        return send_json(req, "{\"error\":\"format\"}");
    }
    uint32_t fps_x100 = 0, frames = 0, w = 0, h = 0;
    avi_demux_get_info(d, &fps_x100, &frames, &w, &h);
    avi_demux_close(d);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"fps_x100\":%lu,\"total_frames\":%lu,\"width\":%lu,\"height\":%lu}",
             (unsigned long)fps_x100, (unsigned long)frames,
             (unsigned long)w, (unsigned long)h);
    ESP_LOGI(TAG, "avi info %s: fps=%lu.%02lu frames=%lu %lux%lu", path,
             (unsigned long)(fps_x100 / 100), (unsigned long)(fps_x100 % 100),
             (unsigned long)frames, (unsigned long)w, (unsigned long)h);
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  ★ §74 补充 5：AVI 顺序会话（单槽）
 *  httpd 单任务串行 ⇒ 一个静态会话槽天然无竞争。顺序播放（请求帧号 ==
 *  会话下一帧）每帧 O(1) 续读，消除"每请求 reopen+跳帧 → 越播越慢"的
 *  线性劣化；回退/换文件才走重开慢路径。后来者抢占会话（慢路径兜底）。
 * ------------------------------------------------------------------------- */
typedef struct {
    char         path[PUNCH_PATH_MAX];
    avi_demux_t *d;
    long         next;      /* 会话中下一次 avi_demux_next_frame 的帧号 */
    uint32_t     total;     /* 总帧数（打开时缓存，夹取用） */
} avi_sess_t;
static avi_sess_t s_avi;            /* 全零初始化 = 无会话 */

static void avi_sess_release(void)
{
    if (s_avi.d) {
        avi_demux_close(s_avi.d);
        s_avi.d = NULL;
    }
    s_avi.path[0] = 0;
    s_avi.next  = 0;
    s_avi.total = 0;
}

/**
 * 确保会话定位到 want 帧（下一次 next_frame 返回第 want 帧）。
 * @param skip_buf 跳帧时丢弃读的缓冲（借用请求的 256KB 帧缓冲）
 * @return ESP_OK / ESP_ERR_NOT_FOUND(打不开) / ESP_FAIL(跳帧中途 EOF/损坏)
 */
static esp_err_t avi_sess_seek(const char *path, long want,
                               uint8_t *skip_buf, uint32_t skip_len)
{
    if (!s_avi.d || strcmp(s_avi.path, path) != 0) {
        avi_sess_release();
        s_avi.d = avi_demux_open(path);
        if (!s_avi.d) {
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(s_avi.path, sizeof(s_avi.path), "%s", path);
        avi_demux_get_info(s_avi.d, NULL, &s_avi.total, NULL, NULL);
        s_avi.next = 0;
    }
    if (s_avi.total > 0 && want >= (long)s_avi.total) {
        want = (long)s_avi.total - 1;   /* 末帧夹取（录像中途总帧数会涨） */
    }
    if (want < s_avi.next) {            /* 回退：重开 */
        avi_demux_close(s_avi.d);
        s_avi.d = avi_demux_open(path);
        if (!s_avi.d) {
            s_avi.path[0] = 0;
            return ESP_ERR_NOT_FOUND;
        }
        avi_demux_get_info(s_avi.d, NULL, &s_avi.total, NULL, NULL);
        s_avi.next = 0;
    }
    while (s_avi.next < want) {         /* 顺序略过中间帧（SD 顺序读，快） */
        uint32_t dlen = 0;
        if (avi_demux_next_frame(s_avi.d, skip_buf, skip_len, &dlen) != AVID_OK) {
            avi_sess_release();
            return ESP_FAIL;
        }
        s_avi.next++;
    }
    return ESP_OK;
}

static esp_err_t handler_media_frame(httpd_req_t *req)
{
    log_req(req, "media/frame");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    char fnum[16] = "0";
    query_get(req, "frame", fnum, sizeof(fnum));
    long want = strtol(fnum, NULL, 10);
    if (want < 0) {
        want = 0;
    }

    uint8_t *fbuf = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fbuf) {
        ESP_LOGE(TAG, "frame buf alloc fail (PSRAM tight?)");
        return send_json(req, "{\"error\":\"no_mem\"}");
    }

    esp_err_t se = avi_sess_seek(path, want, fbuf, FRAME_BUF_SIZE);
    if (se != ESP_OK) {
        heap_caps_free(fbuf);
        return send_json(req, se == ESP_ERR_NOT_FOUND ? "{\"error\":\"format\"}"
                                                      : "{\"error\":\"frame\"}");
    }

    uint32_t len = 0;
    esp_err_t ret;
    avid_result_t r = avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len);
    if (r != AVID_OK) {
        ESP_LOGW(TAG, "frame %ld fail (%d) in %s", want, (int)r, path);
        avi_sess_release();
        heap_caps_free(fbuf);
        return send_json(req, "{\"error\":\"frame\"}");
    }
    s_avi.next = want + 1;              /* 会话推进：下一请求从 want+1 续读 */

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)fbuf, (ssize_t)len);
    heap_caps_free(fbuf);

    ESP_LOGI(TAG, "frame %ld/%lu %s %u B in %u ms (%s)", want,
             (unsigned long)s_avi.total, path, (unsigned)len,
             (unsigned)((esp_timer_get_time() - t0) / 1000),
             esp_err_to_name(ret));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  ★ §74 补充 5：GET /api/media/frames?path=&from=&count=  批量帧
 *  一次请求连发 count(≤8) 帧：[u32 大端长度][JPEG 字节] 连排，
 *  application/octet-stream。前端双缓冲预取，把网络延迟彻底移出播放回路。
 * ------------------------------------------------------------------------- */
#define FRAMES_MAX_BATCH 8

static esp_err_t handler_media_frames(httpd_req_t *req)
{
    log_req(req, "media/frames");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    char fnum[16] = "0", cnum[16] = "8";
    query_get(req, "from", fnum, sizeof(fnum));
    query_get(req, "count", cnum, sizeof(cnum));
    long from = strtol(fnum, NULL, 10);
    if (from < 0) {
        from = 0;
    }
    int cnt = atoi(cnum);
    if (cnt < 1) cnt = 1;
    if (cnt > FRAMES_MAX_BATCH) cnt = FRAMES_MAX_BATCH;

    uint8_t *fbuf = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fbuf) {
        return send_json(req, "{\"error\":\"no_mem\"}");
    }

    esp_err_t se = avi_sess_seek(path, from, fbuf, FRAME_BUF_SIZE);
    if (se != ESP_OK) {
        heap_caps_free(fbuf);
        return send_json(req, se == ESP_ERR_NOT_FOUND ? "{\"error\":\"format\"}"
                                                      : "{\"error\":\"frame\"}");
    }

    /* 先读第一帧成功再发响应头 —— 失败仍可回 JSON 错误 */
    uint32_t len = 0;
    if (avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len) != AVID_OK) {
        avi_sess_release();
        heap_caps_free(fbuf);
        return send_json(req, "{\"error\":\"frame\"}");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int sent = 0;
    esp_err_t ret = ESP_OK;
    for (;;) {
        /* 4 字节大端长度头 + JPEG 字节（与前端 DataView.getUint32 对齐） */
        uint8_t hdr[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                           (uint8_t)(len >> 8),  (uint8_t)len };
        if (httpd_resp_send_chunk(req, (const char *)hdr, 4) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)fbuf, (ssize_t)len) != ESP_OK) {
            ESP_LOGI(TAG, "frames client aborted @ +%d", sent);
            ret = ESP_FAIL;
            break;
        }
        sent++;
        s_avi.next++;
        if (sent >= cnt) {
            break;
        }
        if (avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len) != AVID_OK) {
            break;                      /* EOF/损坏 → 短批收尾，前端按 4B 头解析自然停 */
        }
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    heap_caps_free(fbuf);

    ESP_LOGI(TAG, "frames %ld..%ld/%lu %s (%d sent) in %u ms", from,
             from + sent - 1, (unsigned long)s_avi.total, path, sent,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  ★ §75 补充 2：壁纸/音乐管理（PC 端上传 / 删除 / 设壁纸）
 *  写路径纪律（覆盖文件头"只读"承诺的扩展——写只允许这两条白名单通道）：
 *   · 目录白名单：wallpaper / music 二选一，其它目录一律拒；
 *   · 文件名字符白名单：禁 / \ : * ? " < > | 与控制符与 ".."；
 *   · 大小上限：壁纸 2MB / 音乐 20MB（请求头预检 + 流中复核双保险）；
 *   · 壁纸上传完成后**立即验图**（800x480 JPEG），不合格当场删除——脏文件绝不进池；
 *   · 全部走 FatFS 文件级 API（fopen/unlink），与 §75 取证结论的写路径纪律一致。
 * ------------------------------------------------------------------------- */
#define WALLPAPER_MAX_BYTES (2LL * 1024 * 1024)
#define MUSIC_MAX_BYTES     (20LL * 1024 * 1024)
#define UPNAME_MAX          80

typedef struct {
    const char *tag;        /* URL 里 dir= 参数的值 */
    const char *dir;        /* 挂载点下的绝对目录 */
    const char *exts;       /* 允许的扩展名（逗号分隔，含点，大小写不敏感） */
    long long   max_bytes;
} upload_dir_t;

static const upload_dir_t s_updirs[] = {
    { "wallpaper", WALLPAPER_POOL_DIR,      ".jpg,.jpeg", WALLPAPER_MAX_BYTES },
    { "music",     SDCARD_MNT_PATH"/music", ".mp3,.wav",  MUSIC_MAX_BYTES },
};

static const upload_dir_t *updir_find(const char *tag)
{
    for (size_t i = 0; i < sizeof(s_updirs) / sizeof(s_updirs[0]); i++) {
        if (strcmp(s_updirs[i].tag, tag) == 0) {
            return &s_updirs[i];
        }
    }
    return NULL;
}

/** 文件名安全闸：非空、< UPNAME_MAX、无 ".."、无路径/通配/控制字符 */
static bool name_safe(const char *n)
{
    if (n == NULL || n[0] == '\0' || strlen(n) >= UPNAME_MAX) {
        return false;
    }
    if (strstr(n, "..") != NULL) {
        return false;
    }
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL) {
            return false;
        }
    }
    return true;
}

/** 扩展名是否在该目录的白名单里 */
static bool ext_allowed(const char *name, const upload_dir_t *u)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    /* exts 形如 ".jpg,.jpeg"：逐段比较 */
    const char *p = u->exts;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if ((int)seg == strlen(ext) && strncasecmp(ext, p, seg) == 0) {
            return true;
        }
        p += seg + (comma ? 1 : 0);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 *  GET /api/wallpaper/list —— 池列表 + 当前壁纸名
 * ------------------------------------------------------------------------- */
static esp_err_t handler_wallpaper_list(httpd_req_t *req)
{
    log_req(req, "wallpaper/list");

    char cur[80] = "";
    wallpaper_service_get_current_name(cur, sizeof(cur));
    char cur_e[160];
    json_esc(cur_e, sizeof(cur_e), cur);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[224];
    int hn = snprintf(head, sizeof(head), "{\"current\":\"%s\",\"files\":[", cur_e);
    httpd_resp_send_chunk(req, head, hn);

    char line[384], nm_e[160];
    int count = 0;
    DIR *d = opendir(WALLPAPER_POOL_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *nm = ent->d_name;
            const char *ext = strrchr(nm, '.');
            if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
                continue;
            }
            char full[PUNCH_PATH_MAX + 160];
            _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + 79,
                           "full must hold worst path + '/' + worst name");
            snprintf(full, sizeof(full), "%s/%s", WALLPAPER_POOL_DIR, nm);
            struct stat st;
            long long size = 0;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                size = (long long)st.st_size;
            }
            json_esc(nm_e, sizeof(nm_e), nm);
            int m = snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"size\":%lld}",
                             count ? "," : "", nm_e, size);
            if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
                closedir(d);
                return ESP_FAIL;
            }
            count++;
        }
        closedir(d);
    }
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  POST /api/wallpaper/set?name=xxx.jpg —— 设为当前壁纸（异步切换）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_wallpaper_set(httpd_req_t *req)
{
    log_req(req, "wallpaper/set");

    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    const char *ext = strrchr(name, '.');
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
        return send_json(req, "{\"error\":\"bad_ext\"}");
    }
    /* 提前给明确错误：名字必须在池内（request_set 侧还会再校验一次） */
    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + (UPNAME_MAX - 1),
                   "full must hold worst path + '/' + worst name");
    snprintf(full, sizeof(full), "%s/%s", WALLPAPER_POOL_DIR, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json(req, "{\"error\":\"not_in_pool\"}");
    }

    esp_err_t e = wallpaper_service_request_set(name);
    if (e != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"busy\",\"rc\":\"%s\"}",
                 esp_err_to_name(e));
        return send_json(req, body);
    }
    ESP_LOGI(TAG, "wallpaper set -> '%s'", name);
    return send_json(req, "{\"ok\":1}");
}

/* ---------------------------------------------------------------------------
 *  POST /api/upload?dir=wallpaper|music&name=xxx —— 原始 body 流式落盘
 *  前端用 XHR 直接发文件二进制（不搞 multipart，文件名走 query）。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_upload(httpd_req_t *req)
{
    log_req(req, "upload");

    char dir[16] = "";
    if (query_get(req, "dir", dir, sizeof(dir)) != ESP_OK) {
        return send_json(req, "{\"error\":\"no_dir\"}");
    }
    const upload_dir_t *u = updir_find(dir);
    if (!u) {
        return send_json(req, "{\"error\":\"bad_dir\"}");
    }
    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    if (!ext_allowed(name, u)) {
        return send_json(req, "{\"error\":\"bad_ext\"}");
    }
    if (req->content_len <= 0 || (long long)req->content_len > u->max_bytes) {
        return send_json(req, "{\"error\":\"too_large\"}");
    }

    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    snprintf(full, sizeof(full), "%s/%s", u->dir, name);
    /* ★ §75 补充 2 修订：空卡上目录不存在时先补建（EEXIST 无害）——
     *   music 由"设备录一次音"顺带创建、wallpaper 此前没人建，纯格式化空卡
     *   直接网页上传会 fopen 失败。写入目标已受目录白名单约束，这里 mkdir 无风险。 */
    mkdir(u->dir, 0775);
    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGW(TAG, "upload open fail %s errno=%d", full, errno);
        return send_json(req, "{\"error\":\"open\"}");
    }

    static char rbuf[4096];                 /* httpd 单任务串行，static 安全 */
    long long total = 0;
    int remaining = req->content_len;
    esp_err_t fail = ESP_OK;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(rbuf) ? (int)sizeof(rbuf) : remaining;
        int got = httpd_req_recv(req, rbuf, (size_t)want);
        if (got <= 0) {
            fail = ESP_FAIL;                /* 断连 / 超时 */
            break;
        }
        if (fwrite(rbuf, 1, (size_t)got, f) != (size_t)got) {
            fail = ESP_FAIL;                /* 卡满 / IO 错 */
            break;
        }
        total += got;
        remaining -= got;
        if (total > u->max_bytes) {         /* 流中复核（防谎报 Content-Length） */
            fail = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    fclose(f);
    if (fail != ESP_OK) {
        unlink(full);
        ESP_LOGW(TAG, "upload aborted (%s), removed %s",
                 esp_err_to_name(fail), full);
        return send_json(req, fail == ESP_ERR_INVALID_SIZE
                              ? "{\"error\":\"too_large\"}" : "{\"error\":\"recv\"}");
    }

    /* 壁纸特检：800x480 JPEG，不合格当场删——脏文件绝不进池 */
    if (strcmp(u->tag, "wallpaper") == 0) {
        esp_err_t v = wallpaper_service_validate_file(full);
        if (v != ESP_OK) {
            unlink(full);
            ESP_LOGW(TAG, "upload rejected: '%s' not a 800x480 JPEG (%s), removed",
                     name, esp_err_to_name(v));
            return send_json(req, "{\"error\":\"not_800x480_jpeg\"}");
        }
    }
    ESP_LOGI(TAG, "upload ok: %s (%lld B)", full, total);
    return send_json(req, "{\"ok\":1}");
}

/* ---------------------------------------------------------------------------
 *  DELETE /api/file?dir=wallpaper|music&name=xxx —— 白名单目录内删除
 * ------------------------------------------------------------------------- */
static esp_err_t handler_file_delete(httpd_req_t *req)
{
    log_req(req, "file/delete");

    char dir[16] = "";
    if (query_get(req, "dir", dir, sizeof(dir)) != ESP_OK) {
        return send_json(req, "{\"error\":\"no_dir\"}");
    }
    const upload_dir_t *u = updir_find(dir);
    if (!u) {
        return send_json(req, "{\"error\":\"bad_dir\"}");
    }
    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    snprintf(full, sizeof(full), "%s/%s", u->dir, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json(req, "{\"error\":\"not_found\"}");
    }
    if (unlink(full) != 0) {
        return send_json(req, "{\"error\":\"unlink\"}");
    }
    ESP_LOGI(TAG, "deleted %s", full);
    return send_json(req, "{\"ok\":1}");
}

/* ===========================================================================
 *  S9 桌宠改造 · 动作端点（S9 §3.5）
 *
 *  红线的来源：esp_http_server 是**单任务串行**（cfg.stack_size 8 KB，
 *  一次只跑一个 handler）。任何 handler 里阻塞 1 秒，所有其他请求
 *  （包括手机在轮询的 /api/punch/last）一起卡住 —— 所以：
 *
 *    · 长事务端点（打卡 30s、拍照 2s、播放、重启）一律"**投出去 + 202 立即
 *      返回**"，结果由 /api/action/result?gen=N 轮询（照 on_punch_done 的
 *      gen 槽模式，同一套前端习惯，不多造一种交互）。
 *    · 快端点（灯控、状态查询）同步做完 —— 它们只有一次队列 post / 一次
 *      内存读，耗时 < 1 ms，异步反而增加延迟与状态复杂度。
 *
 *  ★ 为什么动作槽只存"动作名 + gen"、结果去**权威服务**现取：
 *    把结论再抄一份进槽会**静默过期**（服务侧状态变了槽不知道）。
 *    槽只回答"最近受理了哪个动作"，结论永远问 ai_punch / capture_service。
 * =========================================================================== */

typedef enum {
    ACT_NONE = 0,
    ACT_PUNCH,
    ACT_CAPTURE,
    ACT_PLAY,
    ACT_REBOOT,
} act_id_t;

static const char *const s_act_name[] = { "none", "punch", "capture", "play", "reboot" };

typedef struct {
    SemaphoreHandle_t mtx;
    act_id_t          id;
    uint32_t          gen;      /* 每受理一次 +1（0 = 还没受理过任何动作） */
    int64_t           t_us;     /* 受理时刻（算 elapsed） */
} act_slot_t;

static act_slot_t s_act;

static uint32_t act_accept(act_id_t id)
{
    uint32_t g = 0;
    if (xSemaphoreTake(s_act.mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_act.id = id;
        s_act.t_us = esp_timer_get_time();
        s_act.gen++;
        g = s_act.gen;
        xSemaphoreGive(s_act.mtx);
        ESP_LOGI(TAG, "act accepted: %s gen=%u", s_act_name[id], (unsigned)g);
    }
    return g;
}

/** 读动作槽快照（id/gen/受理毫秒数） */
static void act_snapshot(act_id_t *id, uint32_t *gen, uint32_t *elapsed_ms)
{
    act_id_t i = ACT_NONE;
    uint32_t g = 0;
    int64_t t = 0;
    if (xSemaphoreTake(s_act.mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        i = s_act.id;
        g = s_act.gen;
        t = s_act.t_us;
        xSemaphoreGive(s_act.mtx);
    }
    *id = i;
    *gen = g;
    *elapsed_ms = (t > 0) ? (uint32_t)((esp_timer_get_time() - t) / 1000) : 0;
}

/** 读请求体（小 JSON）。返回字节数；<0 = 失败（超长/空/读错） */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0) {
        return -1;
    }
    if ((size_t)req->content_len >= cap) {
        ESP_LOGW(TAG, "body too large: %d >= %u", req->content_len, (unsigned)cap);
        return -1;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)(req->content_len - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;               /* 对端慢：继续等（recv_wait_timeout 兜底） */
        }
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* ---------------------------------------------------------------------------
 *  POST /api/system/reboot —— 有序软重启（延迟 200ms，先把 202 发出去）
 * ------------------------------------------------------------------------- */
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));     /* 让 HTTP 响应发完 */
    safe_reboot_perform("http /api/system/reboot");
    vTaskDelete(NULL);                  /* 理论上到不了（perform 不返回） */
}

static esp_err_t handler_api_reboot(httpd_req_t *req)
{
    log_req(req, "action/reboot");

    /* 栈必须内部 RAM：safe_reboot 里要 unmount SD（flash IO，F35 铁律） */
    if (xTaskCreate(reboot_task, "http_reboot", 4096, NULL, 5, NULL) != pdPASS) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"no_mem\"}");
    }
    act_accept(ACT_REBOOT);
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, "{\"ok\":1,\"accepted\":1,\"action\":\"reboot\"}");
}

/* ---------------------------------------------------------------------------
 *  GET /api/system/status —— 一句话看全设备（联调/云端探活都用它）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_api_status(httpd_req_t *req)
{
    log_req(req, "action/status");

    char ip[IPSTR_LEN] = "";
    wifi_manager_get_ip(ip, sizeof(ip));

    int8_t rssi = 0;
    wifi_ap_record_t ap;
    bool have_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    if (have_rssi) {
        rssi = ap.rssi;
    }

    char sid[40] = "";
    snprintf(sid, sizeof(sid), "%s", ws_client_get_session_id() ? ws_client_get_session_id() : "");

    char uri[96] = "";
    ws_client_get_base_uri(uri, sizeof(uri));

    const char *xz[] = { "IDLE", "PREPARING", "CHATTING", "RELEASING" };
    int st = (int)xiaozhi_service_get_state();
    const char *st_s = (st >= 0 && st <= 3) ? xz[st] : "?";

    char body[640];
    snprintf(body, sizeof(body),
             "{\"uptime_s\":%u,\"heap_internal\":%u,\"heap_psram\":%u,"
             "\"wifi\":{\"role\":\"%s\",\"connected\":%s,\"ip\":\"%s\","
             "\"rssi\":%d,\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\",\"clients\":%d},"
             "\"link\":{\"connected\":%s,\"session\":\"%s\",\"uri\":\"%s\"},"
             "\"session\":{\"state\":\"%s\",\"tts\":%s,\"music\":%s,\"volume\":%d},"
             "\"busy\":{\"punch\":%s,\"capture\":%d}}",
             (unsigned)(esp_timer_get_time() / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             wifi_manager_get_role() == WIFI_ROLE_AP ? "AP" : "STA",
             wifi_manager_is_connected() ? "true" : "false",
             ip, have_rssi ? (int)rssi : 0,
             wifi_manager_get_ap_ssid() ? wifi_manager_get_ap_ssid() : "",
             wifi_manager_get_sta_ssid() ? wifi_manager_get_sta_ssid() : "",
             wifi_manager_get_client_count(),
             ws_client_is_connected() ? "true" : "false",
             sid, uri, st_s,
             xiaozhi_service_tts_active() ? "true" : "false",
             xiaozhi_service_music_playing() ? "true" : "false",
             xiaozhi_service_get_volume(),
             /* ★ P2：业务忙态由业务侧注册的 provider 回答（本文件不认识业务模块） */
             web_busy_punch() ? "true" : "false",
             web_busy_capture() ? 1 : 0);
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  ★ §76：设备端真实界面抓取
 *
 *  数据源不是 lv_snapshot 的"重新渲染"，而是 RGB 面板的 framebuffer：
 *  LVGL 在 DIRECT+AVOID_TEAR 下把两块面板 FB 直接当 draw buffer，
 *  所以读出来的就是屏幕正在扫描输出的像素（含视频/相机画布）。
 *  抓图与网络发送严格分离：持 LVGL 锁只做 memcpy，发 chunk 时已解锁。
 * ------------------------------------------------------------------------- */
#define SHOT_CHUNK_BYTES   (64 * 1024)
#define SHOT_BMP_BLOCK_ROWS 16
/* httpd 是单任务：同一时刻只允许一个抓图/抓帧请求占用它。
 * 浏览器下载失败后会自动重试，若不加闸门，重试请求会排满 socket，
 * 把整个 Web UI 一起堵死（现场表现为页面和下载全部“网络连接失败”）。 */
static volatile bool s_frame_busy;
static volatile bool s_shot_busy;

/** 读取整型 query 参数（缺省/非法时用 def） */
static int shot_query_int(httpd_req_t *req, const char *key, int def)
{
    char s[16];
    if (query_get(req, key, s, sizeof(s)) != ESP_OK) {
        return def;
    }
    return atoi(s);
}

/** GET /api/display/frame?scale=1|2
 *  8 字节头：'R','G','1','6' + u16 LE 宽 + u16 LE 高，随后是 RGB565 小端像素。 */
static esp_err_t handler_display_frame(httpd_req_t *req)
{
    log_req(req, "display/frame");
    if (s_frame_busy) {
        return send_json(req, "{\"error\":\"busy\"}");
    }
    s_frame_busy = true;
    int64_t t0 = esp_timer_get_time();
    const int scale = (shot_query_int(req, "scale", 1) == 2) ? 2 : 1;

    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    esp_err_t e = screen_shot_take(&rgb, &w, &h, scale);
    if (e != ESP_OK) {
        s_frame_busy = false;
        ESP_LOGW(TAG, "display/frame capture failed: %s", esp_err_to_name(e));
        return send_json(req, e == ESP_ERR_NO_MEM ? "{\"error\":\"no_mem\"}"
                                                  : "{\"error\":\"capture\"}");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const uint8_t hdr[8] = { 'R', 'G', '1', '6',
                             (uint8_t)w, (uint8_t)(w >> 8),
                             (uint8_t)h, (uint8_t)(h >> 8) };
    const size_t bytes = (size_t)w * (size_t)h * 2;
    esp_err_t ret = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    for (size_t off = 0; ret == ESP_OK && off < bytes; off += SHOT_CHUNK_BYTES) {
        size_t n = bytes - off;
        if (n > SHOT_CHUNK_BYTES) {
            n = SHOT_CHUNK_BYTES;
        }
        ret = httpd_resp_send_chunk(req, (const char *)rgb + off, (ssize_t)n);
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    screen_shot_free(rgb);

    ESP_LOGI(TAG, "display/frame %dx%d %u B in %u ms (%s)", w, h,
             (unsigned)bytes, (unsigned)((esp_timer_get_time() - t0) / 1000),
             esp_err_to_name(ret));
    s_frame_busy = false;
    return ret;
}

/** GET /api/display/shot?fmt=bmp|jpg&scale=1|2&q=80（浏览器可直接 <img>/下载） */
static esp_err_t handler_display_shot(httpd_req_t *req)
{
    log_req(req, "display/shot");
    int64_t t0 = esp_timer_get_time();

    char fmt[8] = "bmp";
    query_get(req, "fmt", fmt, sizeof(fmt));
    const bool as_jpg = (strcasecmp(fmt, "jpg") == 0 || strcasecmp(fmt, "jpeg") == 0);
    if (!as_jpg && strcasecmp(fmt, "bmp") != 0) {
        return send_json(req, "{\"error\":\"bad_fmt\"}");
    }
    const int scale = (shot_query_int(req, "scale", 1) == 2) ? 2 : 1;
    int quality = shot_query_int(req, "q", 80);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    if (s_shot_busy) {
        return send_json(req, "{\"error\":\"busy\"}");
    }
    s_shot_busy = true;

    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    esp_err_t e = screen_shot_take(&rgb, &w, &h, scale);
    if (e != ESP_OK) {
        s_shot_busy = false;
        ESP_LOGW(TAG, "display/shot capture failed: %s", esp_err_to_name(e));
        return send_json(req, e == ESP_ERR_NO_MEM ? "{\"error\":\"no_mem\"}"
                                                  : "{\"error\":\"capture\"}");
    }

    esp_err_t ret = ESP_OK;

    if (as_jpg) {
        uint8_t *jpg = heap_caps_malloc(SCREEN_SHOT_JPEG_MAX,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (jpg == NULL) {
            screen_shot_free(rgb);
            s_shot_busy = false;
            return send_json(req, "{\"error\":\"no_mem\"}");
        }
        size_t len = 0;
        e = jpeg_util_encode_rgb565_le(rgb, w, h, quality, jpg,
                                       SCREEN_SHOT_JPEG_MAX, &len);
        screen_shot_free(rgb);
        if (e != ESP_OK) {
            heap_caps_free(jpg);
            s_shot_busy = false;
            return send_json(req, "{\"error\":\"encode\"}");
        }
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"screen.jpg\"");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        ret = httpd_resp_send(req, (const char *)jpg, (ssize_t)len);
        ESP_LOGI(TAG, "display/shot jpg %dx%d q=%d %u B in %u ms (%s)", w, h,
                 quality, (unsigned)len,
                 (unsigned)((esp_timer_get_time() - t0) / 1000),
                 esp_err_to_name(ret));
        heap_caps_free(jpg);
        s_shot_busy = false;
        return ret;
    }

    /* BMP：头 + 每 16 行一块转换后发送 —— 不再为 1.15 MB 的 24bpp 整图
     * 额外申请一次大缓冲（PSRAM 要留给检测模型）。 */
    const size_t stride = screen_shot_bmp_stride(w);
    uint8_t *blk = heap_caps_malloc(stride * SHOT_BMP_BLOCK_ROWS,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (blk == NULL) {
        screen_shot_free(rgb);
        s_shot_busy = false;
        return send_json(req, "{\"error\":\"no_mem\"}");
    }

    uint8_t hdr[SCREEN_SHOT_BMP_HDR];
    screen_shot_bmp_header(hdr, w, h);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"screen.bmp\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));

    int rows = 0;
    for (int y = 0; y < h && ret == ESP_OK; y += SHOT_BMP_BLOCK_ROWS) {
        int n = h - y;
        if (n > SHOT_BMP_BLOCK_ROWS) {
            n = SHOT_BMP_BLOCK_ROWS;
        }
        for (int r = 0; r < n; r++) {
            uint8_t *row = blk + (size_t)r * stride;
            screen_shot_bmp_row(rgb, w, y + r, row);
            if (stride > (size_t)w * 3) {
                memset(row + (size_t)w * 3, 0, stride - (size_t)w * 3);
            }
        }
        ret = httpd_resp_send_chunk(req, (const char *)blk, (ssize_t)((size_t)n * stride));
        rows += n;
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    heap_caps_free(blk);
    screen_shot_free(rgb);

    ESP_LOGI(TAG, "display/shot bmp %dx%d rows=%d in %u ms (%s)", w, h, rows,
             (unsigned)((esp_timer_get_time() - t0) / 1000), esp_err_to_name(ret));
    s_shot_busy = false;
    return ret;
}

/* ---------------------------------------------------------------------------
 *  A：720p 传感器 JPEG 直出（/stream.mjpg）
 *
 *  ★ 与上一版（240x240 RGB565 → CPU 软编 q40 ≈1.25 fps）的根本区别：
 *    · 本版**不做任何编码**：直接起 OV3660 的 JPEG 档（1280x720@12fps，传感器
 *      内部硬件编码），回调只把**变长码流** memcpy 到另一块 PSRAM，HTTP 侧
 *      原样按 multipart 帧发出去；
 *    · 因此完全不碰 esp_driver_jpeg 的硬件编解码器（codec 已被 viewfinder 的
 *      取景/文件预览占着，两边共用一把 portMAX_DELAY 的 codec_mutex）——
 *      零 CPU 编码、零互斥；
 *    · 帧缓冲只做双缓冲交换：httpd 读 front 时回调写 back；in_use 期间回调
 *      丢帧（网络慢只会降帧率，绝不阻塞 cam_task，DVP 不会连续丢帧）。
 *
 *  ★ 分辨率为什么固定 720p：ov3660_fmt.c 只收录了这一条 JPEG 档。供应商对
 *    640x480 只给了 RGB565/YUV422 寄存器表，要 640x480 JPEG 得自己拼表 + 实测。
 *  ★ 质量参数 q：OV3660 语义是**值越小质量越高、单帧越大**（CAM_JPEG_Q_BEST=1）。
 *    这里限制在 [MJPG_Q_MIN, MJPG_Q_MAX]，并给单帧设硬上限：超限帧丢弃并打日志
 *    （宁可明确告警，也不要出现"连上了但没画面"的黑屏假象）。
 *  ★ 单客户端：两个客户端就是 2 份 320 KB 帧缓冲 + 两个 httpd 连接，而 httpd
 *    是**单任务串行**模型，第二个长连接会把所有短请求一起饿死。
 * ------------------------------------------------------------------------- */
#define MJPG_Q_DEFAULT       12      /* 720p 单帧约 25~40 KB，WiFi 下比 q8 更稳 */
#define MJPG_Q_MIN            6
#define MJPG_Q_MAX           40
#define MJPG_FRAME_CAP_MAX  (320u * 1024u)     /* 上限；实际按下方阶梯探测 */
/* ★ 帧缓冲容量必须"按可用内存降级"：设备端 VIDEO 720p 取景在跑时（搭车场景），
 *   传感器 + 取景器已经吃掉大部分 PSRAM，再要 2×320 KB 会直接 alloc 失败
 *   ⇒ 网页 500 黑屏。实测搭车时只能拿到 192 KB 档，q12 单帧 ~50 KB 完全够用。 */
static volatile uint32_t s_mj_cap = MJPG_FRAME_CAP_MAX;
#define MJPG_IDLE_TIMEOUT_US (10LL * 1000 * 1000)

/* ★ 2026-11-22：推流走独立 httpd 实例（默认 :81），长连接不再占住主服务的
 *   唯一任务 ⇒ 监控期间管理页/统计轮询照常响应。起不来则回退 :80（预检里告诉前端）。 */
static int s_stream_port = 80;

static uint8_t *s_mj_buf[2];
static volatile int  s_mj_front;
static volatile bool s_mj_new;
static volatile bool s_mj_in_use;
static volatile bool s_mj_client;
static volatile uint32_t s_mj_size;          /* 当前 front 帧的字节数 */
static portMUX_TYPE  s_mj_mux = portMUX_INITIALIZER_UNLOCKED;

/* 诊断：尺寸不符 / 编码忙（httpd 还在发上一帧）各计一个数，每 30 次打一条 WARN */
static volatile uint32_t s_mj_drop_size;
static volatile uint32_t s_mj_drop_busy;
static uint32_t          s_mj_frames_tx;

static void mjpg_frame_cb(const uint8_t *data, uint32_t size,
                          uint32_t w, uint32_t h, void *user)
{
    (void)user;
    if (data == NULL || size == 0) {
        return;
    }
    /* 只收本档的帧：设备端 CAM 页是 240x240、VIDEO 页也是 720p，
     * 尺寸不符说明相机已被别的页面换档，明确计数而不是静默丢。 */
    if (w != CAM_MEDIA_W || h != CAM_MEDIA_H) {
        if ((++s_mj_drop_size % 30) == 1) {
            ESP_LOGW(TAG, "mjpeg: frame %ux%u ignored (need %ux%u, sz=%u)",
                     (unsigned)w, (unsigned)h, (unsigned)CAM_MEDIA_W,
                     (unsigned)CAM_MEDIA_H, (unsigned)size);
        }
        return;
    }
    if (size > s_mj_cap) {
        if ((++s_mj_drop_size % 30) == 1) {
            ESP_LOGW(TAG, "mjpeg: frame %u B > cap %u B dropped (lower q to shrink)",
                     (unsigned)size, (unsigned)s_mj_cap);
        }
        return;
    }
    if (s_mj_in_use || s_mj_buf[0] == NULL || s_mj_buf[1] == NULL) {
        if ((++s_mj_drop_busy % 30) == 1) {
            ESP_LOGW(TAG, "mjpeg: frame dropped, sender busy (in_use=%d)",
                     (int)s_mj_in_use);
        }
        return;
    }

    const int back = s_mj_front ^ 1;
    memcpy(s_mj_buf[back], data, size);

    taskENTER_CRITICAL(&s_mj_mux);
    s_mj_front = back;
    s_mj_size  = size;
    s_mj_new   = true;
    taskEXIT_CRITICAL(&s_mj_mux);
}

static esp_err_t handler_stream_mjpg(httpd_req_t *req)
{
    log_req(req, "stream.mjpg");

    ESP_LOGI(TAG, "mjpeg: req owner=%d running=%d has_frame_cb=%d",
             (int)camera_service_owner(), (int)camera_service_is_running(),
             (int)camera_service_has_frame_cb());

    if (s_mj_client) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mjpeg stream busy");
    }
    /* ★ 远程监控优先，但要按"事务"分级：
     *   ① 录像/拍照/打卡是跨帧事务，绝不抢；录像可由 /api/camera/release
     *      显式结束（用户确认后）；
     *   ② 设备端"闲置预览"（CAM 页 240x240、VIDEO 取景器）直接让出 ——
     *      它是随时可重建的观察者，而且让出必须在 LVGL 锁内做（ui_camera_yield），
     *      否则 tab_camera 的刷新会读到已 unmap 的帧指针；
     *   ③ 云端识图等 MEDIA 会话要拒绝（几秒级，等它自己结束）。 */
    if (record_video_service_state() != VIDEO_REC_IDLE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req,
                               "recording in progress; POST /api/camera/release first",
                               HTTPD_RESP_USE_STRLEN);
    }
    if (web_busy_punch() || web_busy_capture()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "punch/photo transaction in progress",
                               HTTPD_RESP_USE_STRLEN);
    }
    /* ★ 搭车优先：设备端 VIDEO 取景（720P JPEG、cb=NULL）已经在跑时，
     *   网页直接挂第二观察者发同一路码流 —— **不停流、不打断设备预览**。
     *   这是"不影响设备端正常使用"的最优路径（零额外起流、零内存搬迁）。 */
    bool tap = false;
    if (camera_service_is_running() && camera_service_owner() == CAM_OWNER_PREVIEW) {
        uint32_t tw = 0, th = 0;
        cam_fmt_t tf = CAM_FMT_RGB565;
        camera_service_get_stream(&tw, &th, &tf);
        if (tf == CAM_FMT_JPEG && tw == CAM_MEDIA_W && th == CAM_MEDIA_H &&
            !camera_service_has_frame_cb()) {
            tap = true;
            ESP_LOGI(TAG, "mjpeg: tapping device viewfinder stream (no restart)");
        }
    }

    if (!tap) {
        if (camera_service_is_running() &&
            camera_service_owner() == CAM_OWNER_MEDIA) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_send(req, "media session in progress",
                                   HTTPD_RESP_USE_STRLEN);
        }
        if (!ui_camera_yield()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_send(req, "device UI busy; retry", HTTPD_RESP_USE_STRLEN);
        }
        /* 让出 PREVIEW 后仍在跑的只能是刚抢进来的 MEDIA 会话（云端识图等）——不能碰。 */
        if (camera_service_is_running()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_send(req, "media session in progress",
                                   HTTPD_RESP_USE_STRLEN);
        }
    }

    int quality = MJPG_Q_DEFAULT;
    {
        char qbuf[8];
        if (query_get(req, "q", qbuf, sizeof(qbuf)) == ESP_OK) {
            int v = atoi(qbuf);
            if (v >= MJPG_Q_MIN && v <= MJPG_Q_MAX) {
                quality = v;
            }
        }
    }

    s_mj_client    = true;
    s_mj_new       = false;
    s_mj_in_use    = false;
    s_mj_size      = 0;
    s_mj_front     = 0;
    s_mj_drop_size = 0;
    s_mj_drop_busy = 0;
    s_mj_frames_tx = 0;

    /* ★ 16 字节对齐：不是给 esp_jpeg（本版不编码）用的，而是 PSRAM 上 memcpy
     * 与 cache 行对齐的常规要求，也方便将来若接硬编直接当输入。
     * ★ 分配顺序：先让驱动拿到 720p 的三块 921,600 B 连续缓冲，再分配我们的
     *   2×320 KB——反过来的话，600 KB 小块的碎片有可能让驱动凑不出连续块
     *   （`no mem for DVP backup buffer`，§73.9 踩过的坑）。 */

    bool      started      = false;
    bool      headers_sent = false;
    esp_err_t err          = ESP_OK;
    int64_t   t_start      = 0;
    uint64_t  bytes_tx     = 0;

    /* 质量必须在 start 之前设：camera_service 会记住它，起流时写传感器。 */
    err = camera_service_set_jpeg_quality(quality);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mjpeg: set q=%d failed: %s", quality, esp_err_to_name(err));
        goto cleanup;
    }

    /* ★ owner 显式给 MEDIA：设备端 CAM 页据此置灰按钮并提示"被占用"，
     *   不会在远程监控期间被本机用户按 START 抢走。
     *   ★ 搭车（tap）时流本来就在跑、归 PREVIEW，绝不能再 start 一次。 */
    if (!tap) {
        err = camera_service_start_fmt_owner(NULL, NULL, CAM_FMT_JPEG,
                                             CAM_MEDIA_W, CAM_MEDIA_H, CAM_OWNER_MEDIA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mjpeg: camera start failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        started = true;
    }

    /* ★ 容量阶梯：搭车模式下 PSRAM 被设备取景器占着，2×320 KB 拿不到。
     *   从大到小试，取第一个"两块都拿到"的尺寸；全失败才报 500。
     *   小容量只会让 q 高时的大帧被丢（drop_size 计数），不会黑屏。 */
    {
        static const uint32_t ladder[] = {
            MJPG_FRAME_CAP_MAX, 192u * 1024u, 128u * 1024u, 96u * 1024u, 64u * 1024u
        };
        size_t i;
        for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); ++i) {
            s_mj_buf[0] = heap_caps_aligned_alloc(16, ladder[i], MALLOC_CAP_SPIRAM);
            s_mj_buf[1] = heap_caps_aligned_alloc(16, ladder[i], MALLOC_CAP_SPIRAM);
            if (s_mj_buf[0] && s_mj_buf[1]) {
                s_mj_cap = ladder[i];
                break;
            }
            if (s_mj_buf[0]) { heap_caps_free(s_mj_buf[0]); s_mj_buf[0] = NULL; }
            if (s_mj_buf[1]) { heap_caps_free(s_mj_buf[1]); s_mj_buf[1] = NULL; }
        }
        if (s_mj_buf[0] == NULL || s_mj_buf[1] == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "mjpeg: frame buffers alloc failed (all sizes), psram free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            goto cleanup;
        }
        ESP_LOGI(TAG, "mjpeg: frame buffers %u B x2 (tap=%d), psram free=%u",
                 (unsigned)s_mj_cap, (int)tap,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    if (tap) {
        camera_service_set_observer2(mjpg_frame_cb, NULL);   /* 不顶替设备取景器的第一观察者 */
    } else {
        camera_service_set_observer(mjpg_frame_cb, NULL);
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    headers_sent = true;

    t_start = esp_timer_get_time();
    int64_t last_tx = t_start;

    /* ★ 不再设 60 s 硬上限：监控场景是"随时看"，由客户端断开或 10 s 无帧收尾。
     *   httpd 单任务被本 handler 占着，这正是"离开页面必须主动 src=''"的原因。 */
    while ((esp_timer_get_time() - last_tx) < MJPG_IDLE_TIMEOUT_US) {
        if (flash_guard_is_active()) {      /* flash 窗口：让出 PSRAM 带宽 */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const int64_t wait_end = esp_timer_get_time() + 500LL * 1000;
        while (!s_mj_new && esp_timer_get_time() < wait_end) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_mj_new) {
            continue;
        }

        taskENTER_CRITICAL(&s_mj_mux);
        const uint8_t *front = s_mj_buf[s_mj_front];
        const size_t   len   = s_mj_size;
        s_mj_new   = false;
        s_mj_in_use = true;
        taskEXIT_CRITICAL(&s_mj_mux);

        char part[128];
        int hlen = snprintf(part, sizeof(part),
                            "--frame\r\nContent-Type: image/jpeg\r\n"
                            "Content-Length: %u\r\n\r\n", (unsigned)len);
        esp_err_t se = httpd_resp_send_chunk(req, part, hlen);
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, (const char *)front, (ssize_t)len);
        }
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        s_mj_in_use = false;

        if (se != ESP_OK) {
            err = se;                       /* 客户端断开 / 发送超时 */
            break;
        }
        bytes_tx += len;
        s_mj_frames_tx++;
        last_tx = esp_timer_get_time();
    }

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);   /* 正常收尾：结束 multipart */
    }

    {
        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        if (elapsed_ms < 1) elapsed_ms = 1;
        ESP_LOGI(TAG, "mjpeg stream ended: sent=%u q=%d %lldms (%.2f fps) "
                      "%llu KB drop_size=%u drop_busy=%u err=%s",
                 (unsigned)s_mj_frames_tx, quality, (long long)elapsed_ms,
                 (double)s_mj_frames_tx * 1000.0 / (double)elapsed_ms,
                 (unsigned long long)(bytes_tx / 1024u),
                 (unsigned)s_mj_drop_size, (unsigned)s_mj_drop_busy,
                 esp_err_to_name(err));
    }

cleanup:
    if (tap) {
        camera_service_set_observer2(NULL, NULL);    /* 只摘自己的观察者，设备取景流照旧 */
    } else {
        camera_service_set_observer(NULL, NULL);
    }
    if (started) {
        camera_service_stop();
    }
    if (s_mj_buf[0]) heap_caps_free(s_mj_buf[0]);
    if (s_mj_buf[1]) heap_caps_free(s_mj_buf[1]);
    s_mj_buf[0] = s_mj_buf[1] = NULL;
    s_mj_size = 0;
    s_mj_client = false;

    if (!headers_sent && err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  GET /api/camera/stream —— 开流前预检（轻量 JSON）
 *
 *  ★ 为什么需要它：<img src="/stream.mjpg"> 拿不到 HTTP 状态码，失败时只会
 *    触发 onerror，用户看到的是"黑屏 + 无原因"。先打这个短请求，把 409 的
 *    具体原因（录像 / 拍照 / 打卡 / 已有流）原样显示到网页上。
 *  ★ 设备端"预览占用"不算 busy：开流时会自动让出（见 /stream.mjpg）。
 *    can_release=1 表示录像可以经 /api/camera/release 结束并保存。
 *  ★ 只做只读判断，不抢相机。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_api_camera_stream(httpd_req_t *req)
{
    log_req(req, "camera/stream");

    bool         busy       = false;
    bool         can_release = false;
    const char  *reason     = "";

    video_rec_state_t rec = record_video_service_state();
    if (s_mj_client) {
        busy = true;  reason = "stream already running";
    } else if (rec == VIDEO_REC_RECORDING) {
        busy = true;  can_release = true;  reason = "recording in progress";
    } else if (rec != VIDEO_REC_IDLE) {
        busy = true;  reason = "recording finalizing (wait a few seconds)";
    } else if (web_busy_punch()) {
        busy = true;  reason = "punch session in progress";
    } else if (web_busy_capture()) {
        busy = true;  reason = "photo capture in progress";
    } else if (camera_service_is_running() &&
               camera_service_owner() == CAM_OWNER_MEDIA) {
        busy = true;  reason = "media session in progress (cloud vision?)";
    }

    char body[224];
    snprintf(body, sizeof(body),
             "{\"ok\":%d,\"busy\":%d,\"reason\":\"%s\",\"owner\":%d,\"running\":%d,"
             "\"can_release\":%d,\"retry\":%d,\"port\":%d}",
             busy ? 0 : 1, busy ? 1 : 0, reason,
             (int)camera_service_owner(), camera_service_is_running() ? 1 : 0,
             can_release ? 1 : 0,
             (rec != VIDEO_REC_IDLE) ? 1 : 0,
             s_stream_port);
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  POST /api/camera/release —— 让设备端把相机交出来
 *
 *  ★ 能做的：
 *    - owner=PREVIEW（CAM 页预览 / VIDEO 取景）→ ui_camera_yield() 在 LVGL
 *      锁内停流并归还取景器 2.93 MB（网页开流时也会自动做这一步）；
 *    - 正在录像 → record_video_service_stop()（非阻塞；服务任务收尾、重扫
 *      idx1 要几秒）→ retry=1，前端轮询预检直到 IDLE 再开流。
 *  ★ 不做的（避免破坏事务）：打卡会话、拍照、云端识图 → 明确拒绝，等它结束。
 *  ★ 单任务 httpd：本端点在"监控流已建立"时无法被服务（s_mj_client 时也拒绝），
 *    它只用于开流之前。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_api_camera_release(httpd_req_t *req)
{
    log_req(req, "camera/release");

    const char *action = "none";
    const char *reason = "";
    bool        ok     = true;
    bool        retry  = false;

    video_rec_state_t rec = record_video_service_state();
    bool punch = web_busy_punch();
    bool photo = web_busy_capture();

    if (s_mj_client) {
        ok = false;  reason = "stream running; close it on the web page first";
    } else if (rec == VIDEO_REC_RECORDING) {
        esp_err_t e = record_video_service_stop();
        action = "recording_stopping";
        if (e == ESP_OK) {
            retry = true;                       /* 等 FINALIZING → IDLE */
        } else {
            ok = false;
            reason = esp_err_to_name(e);
        }
    } else if (rec == VIDEO_REC_FINALIZING) {
        action = "wait_finalize";
        retry  = true;
    } else if (punch) {
        ok = false;  reason = "punch session in progress";
    } else if (photo) {
        ok = false;  reason = "photo capture in progress";
    } else if (camera_service_is_running() &&
               camera_service_owner() == CAM_OWNER_MEDIA) {
        ok = false;  reason = "media session in progress";
    } else if (!ui_camera_yield()) {
        ok = false;  reason = "device UI busy; retry";
    } else {
        action = "released";
    }

    char body[192];
    snprintf(body, sizeof(body),
             "{\"ok\":%d,\"action\":\"%s\",\"reason\":\"%s\",\"retry\":%d,"
             "\"owner\":%d,\"running\":%d}",
             ok ? 1 : 0, action, reason, retry ? 1 : 0,
             (int)camera_service_owner(), camera_service_is_running() ? 1 : 0);
    ESP_LOGI(TAG, "camera/release: ok=%d action=%s retry=%d reason=%s",
             (int)ok, action, (int)retry, reason);
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  P0：安全模式出口。清掉启动失败计数后，用户再点 /api/system/reboot。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_api_recovery_clear(httpd_req_t *req)
{
    log_req(req, "recovery/clear");
    boot_guard_clear();
    const char *body = "{\"ok\":1,\"msg\":\"boot counter cleared; reboot to leave safe mode\"}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------------
 *  启动
 * ------------------------------------------------------------------------- */
/* ===========================================================================
 *  ★ P2：把内部小工具导出给业务 handler 复用（web_action.h 声明）
 *  只加薄包装，原有 static 实现与语义一字不动。
 * ------------------------------------------------------------------------- */
void web_log_req(httpd_req_t *req, const char *what)
{
    log_req(req, what);
}

esp_err_t web_send_json(httpd_req_t *req, const char *body)
{
    return send_json(req, body);
}

int web_read_body(httpd_req_t *req, char *buf, size_t cap)
{
    return read_body(req, buf, cap);
}

esp_err_t web_query_get(httpd_req_t *req, const char *key, char *out, size_t outlen)
{
    return query_get(req, key, out, outlen);
}

const char *web_json_esc(char *dst, size_t dstlen, const char *src)
{
    return json_esc(dst, dstlen, src);
}

uint32_t web_act_accept(web_act_id_t id)
{
    return act_accept((act_id_t)id);
}

void web_act_snapshot(web_act_id_t *id, uint32_t *gen, uint32_t *elapsed_ms)
{
    act_id_t i = ACT_NONE;
    act_snapshot(&i, gen, elapsed_ms);
    *id = (web_act_id_t)i;
}

const char *web_act_name(web_act_id_t id)
{
    int i = (int)id;
    if (i < 0 || i >= (int)(sizeof(s_act_name) / sizeof(s_act_name[0]))) {
        return "none";
    }
    return s_act_name[i];
}

static const httpd_uri_t s_uris[] = {
    { .uri = "/",                    .method = HTTP_GET,    .handler = handler_index,             .user_ctx = NULL },
    { .uri = "/api/companion/stats", .method = HTTP_GET,    .handler = handler_companion_stats,   .user_ctx = NULL },
    { .uri = "/api/punch/last",      .method = HTTP_GET,    .handler = handler_punch_last,        .user_ctx = NULL },
    { .uri = "/api/attendance/list", .method = HTTP_GET,    .handler = handler_attendance_list,   .user_ctx = NULL },
    { .uri = "/api/media/list",      .method = HTTP_GET,    .handler = handler_media_list,        .user_ctx = NULL },
    { .uri = "/api/media/file",      .method = HTTP_GET,    .handler = handler_media_file,        .user_ctx = NULL },
    { .uri = "/api/media/info",      .method = HTTP_GET,    .handler = handler_media_info,        .user_ctx = NULL },
    { .uri = "/api/media/frame",     .method = HTTP_GET,    .handler = handler_media_frame,       .user_ctx = NULL },
    { .uri = "/api/media/frames",    .method = HTTP_GET,    .handler = handler_media_frames,      .user_ctx = NULL },
    { .uri = "/api/wallpaper/list",  .method = HTTP_GET,    .handler = handler_wallpaper_list,    .user_ctx = NULL },
    { .uri = "/api/wallpaper/set",   .method = HTTP_POST,   .handler = handler_wallpaper_set,     .user_ctx = NULL },
    { .uri = "/api/upload",          .method = HTTP_POST,   .handler = handler_upload,            .user_ctx = NULL },
    { .uri = "/api/file",            .method = HTTP_DELETE, .handler = handler_file_delete,       .user_ctx = NULL },
    /* ---- S9 §3.5 动作端点（受理即返回；长事务走 gen 轮询）---- */
    { .uri = "/api/system/reboot",   .method = HTTP_POST,   .handler = handler_api_reboot,        .user_ctx = NULL },
    { .uri = "/api/system/status",   .method = HTTP_GET,    .handler = handler_api_status,        .user_ctx = NULL },
    /* ---- P0/P1：安全模式出口 + 低分辨率实时预览 ---- */
    { .uri = "/api/recovery/clear",  .method = HTTP_POST,   .handler = handler_api_recovery_clear, .user_ctx = NULL },
    { .uri = "/stream.mjpg",         .method = HTTP_GET,    .handler = handler_stream_mjpg,       .user_ctx = NULL },
    { .uri = "/api/camera/stream",   .method = HTTP_GET,    .handler = handler_api_camera_stream, .user_ctx = NULL },
    { .uri = "/api/camera/release",  .method = HTTP_POST,   .handler = handler_api_camera_release, .user_ctx = NULL },
    /* ---- §76 设备端真实界面抓取（RGB framebuffer）---- */
    { .uri = "/api/display/frame",   .method = HTTP_GET,    .handler = handler_display_frame,     .user_ctx = NULL },
    { .uri = "/api/display/shot",    .method = HTTP_GET,    .handler = handler_display_shot,      .user_ctx = NULL },
};

/* ★ 独立推流实例只挂这一条：长连接任务与管理任务彻底分离。 */
static const httpd_uri_t s_stream_uris[] = {
    { .uri = "/stream.mjpg", .method = HTTP_GET, .handler = handler_stream_mjpg, .user_ctx = NULL },
};

esp_err_t web_server_init(void)
{
    static bool s_started = false;
    if (s_started) {
        return ESP_OK;
    }

    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.mtx = xSemaphoreCreateMutex();
    if (!s_evt.mtx) {
        return ESP_ERR_NO_MEM;
    }
    punch_service_set_done_cb(on_punch_done, NULL);

    memset(&s_act, 0, sizeof(s_act));
    s_act.mtx = xSemaphoreCreateMutex();
    if (!s_act.mtx) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;                  /* 流式 handler + snprintf 需要 */
    cfg.max_uri_handlers = 30;              /* 原 26 + recovery/clear + stream.mjpg 等余量 */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;             /* 客户端停读时尽快释放单任务 httpd */
    cfg.keep_alive_enable = false;          /* 每个请求独立连接，避免半死 keep-alive 卡住服务 */
    cfg.lru_purge_enable = true;            /* fd 紧张时挤掉最久未用的连接 */
    /* ★ 实测软编 240x240 q70 单帧墙钟 770 ms（模块文档预期 20~60 ms），
     *   说明不是算法慢，而是 httpd 任务被同级/更高优先级的音频 AFE 抢得没 CPU。
     *   把 httpd 提到 prio 7 并钉到 core1（AFE 在 core0），让编码真正跑满；
     *   只影响"有人在看流"的这一小段时间，音频不挪核。 */
    cfg.task_priority = 7;
    cfg.core_id       = 1;

    httpd_handle_t server = NULL;
    esp_err_t e = httpd_start(&server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        return e;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &s_uris[i]));
    }

    /* ★ 2026-11-22：独立推流服务（:81）
     *   esp_http_server 每个实例一条任务，handler 不返回就饿死该实例的所有请求。
     *   推流单独一个实例后，监控中主服务（:80）仍能正常响应管理页/统计轮询。
     *   ★ 主实例上保留 /stream.mjpg 作为回退；两边共享 s_mj_client，互斥。 */
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = 81;
    scfg.ctrl_port = 32769;              /* 不能与主实例的 32768 冲突 */
    scfg.max_uri_handlers = 2;
    scfg.max_open_sockets = 2;           /* 1 listen + 1 客户端；LWIP_MAX_SOCKETS=10，主实例 7 不越界 */
    scfg.stack_size = 6144;
    scfg.recv_wait_timeout = 10;
    scfg.send_wait_timeout = 10;
    scfg.keep_alive_enable = false;
    scfg.lru_purge_enable = true;
    scfg.task_priority = 7;
    scfg.core_id = 1;

    httpd_handle_t stream_srv = NULL;
    esp_err_t se = httpd_start(&stream_srv, &scfg);
    if (se == ESP_OK) {
        for (size_t i = 0; i < sizeof(s_stream_uris) / sizeof(s_stream_uris[0]); i++) {
            ESP_ERROR_CHECK(httpd_register_uri_handler(stream_srv, &s_stream_uris[i]));
        }
        s_stream_port = 81;
        ESP_LOGI(TAG, "stream server on :81 (management stays responsive on :%d)",
                 cfg.server_port);
    } else {
        ESP_LOGW(TAG, "stream server :81 start failed (%s) -> fallback :%d "
                      "(long stream will block management requests)",
                 esp_err_to_name(se), cfg.server_port);
    }

    /* ★ P2：业务动作端点（谁登记谁负责实现；本文件零业务认知） */
    for (size_t i = 0; i < web_action_count(); i++) {
        httpd_uri_t u = {
            .uri     = web_action_uri(i),
            .method  = web_action_method(i),
            .handler = web_action_fn_at(i),
            .user_ctx = NULL,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
    }
    s_started = true;
    ESP_LOGI(TAG, "web server ready: %u endpoints, port %d "
             "(AP: http://192.168.4.1/ / STA: http://<device-ip>/)",
             (unsigned)(sizeof(s_uris) / sizeof(s_uris[0])), cfg.server_port);
    return ESP_OK;
}
