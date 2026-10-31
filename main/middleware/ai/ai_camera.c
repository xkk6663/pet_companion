/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_camera.c
 * @brief 小智 MCP 相机工具实现（S7-A1 / §58；§73.18 同步桥改造，见头文件）。
 *
 * 并发模型（§73.18 改造后）：
 *   - MCP 工具回调（WS 收包任务上下文）：建 explain 任务后**阻塞等待完成**，
 *     把真实识别文本作为 tools/call 结果同步返回。旧版"立即返回占位符"导致
 *     服务端大模型永远拿不到识别文本（占位符就是它收到的全部工具结果），
 *     实际回答只走了本地 UI 通道。官方 xiaozhi-esp32 固件的 explain 工具
 *     本就是回调内同步完成拍照+视觉问答（数秒级阻塞是官方既定契约）。
 *   - explain 任务（每次现建，PSRAM 栈，自删）：相机事务 + 同步 HTTP。
 *     重活仍留在独立任务里：WS 收包任务栈小，TLS/HTTP 不下探（§73.18
 *     评估：整段同步进回调会有栈溢出风险，故采用"信号量桥"折中）。
 *   - 帧回调（cam_task）：只 memcpy + 置事件组（camera_service.h 硬约束）。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_mcp_engine.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"

#include "ai_camera.h"
#include "ws_client.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/capture_service.h"

static const char *TAG = "AI_CAM";

static ai_camera_result_cb_t s_result_cb;
static volatile bool s_busy;            /* 会话互斥（一次只跑一个 explain） */

/* explain 任务私有（任务独占，创建后只有它自己读写） */
static char     s_question[96];
static uint8_t *s_jpg;                  /* PSRAM 帧缓冲（任务内分配释放） */
static size_t   s_jpg_len;
static EventGroupHandle_t s_frame_evt;  /* BIT0 = 抓到一帧 */

#define JPG_BUF_SIZE   (160 * 1024)     /* 1280x720 q8 实测 ~40-90KB，留余量 */
#define FRAME_WAIT_MS  3000
#define EXPLAIN_OUT_SIZE 1024           /* 视觉回答文本缓冲 */
#define EXPLAIN_STACK_SIZE (16 * 1024)  /* PSRAM 栈（纯 HTTP/TLS，F35 允许外置） */
/* §73.18 桥接等待上限：帧等待 3s + vision HTTP 超时 30s + 任务收尾余量。
 * 任务所有路径都保证 give，正常永远先于它到达；超时只兜异常（结果届时
 * 仍会经 report() 进 UI 浮窗，只是服务端模型收不到）。 */
#define EXPLAIN_BRIDGE_TIMEOUT_MS (40 * 1000)

/* §73.18 同步桥：explain 任务 → MCP 工具回调的结果交接。
 * 没有它，tools/call 的 response 就是占位符，服务端大模型永远
 * "拿不到照片识别的结果"（根因修复的核心交接点）。 */
static SemaphoreHandle_t s_done;        /* 二值信号量：explain 任务完成 */
static char s_answer[EXPLAIN_OUT_SIZE]; /* 结果文本（首条 finish_with 获胜） */
static volatile bool s_answer_done;

/* ---- 拍照预览（§59b）：RGB565 缩略图单槽交接 ----
 * 生产者 = 解码所在任务（explain 任务 / capture 预览任务），
 * 消费者 = pet_overlay 50ms lv_timer（LVGL 线程）。
 * 所有权：发布时归槽；UI take 后移交 UI（显示 3s 后 UI free）。
 * 新帧到达而旧帧未被取走 → 直接丢旧（预览是赠品，永远显示最新）。 */
#define PV_W 640
#define PV_H 360                        /* 1/2 scale（两者均 8 的倍数，解码器要求） */

static uint8_t *s_pv_buf;               /* 待取/显示中的 RGB565 帧（NULL=无） */
static int      s_pv_w, s_pv_h;
static volatile bool s_pv_pending;      /* s_pv_buf 有效，等 UI 取 */
static portMUX_TYPE s_pv_mux = portMUX_INITIALIZER_UNLOCKED;

/* §73.23 视频流模式：true 时 UI 连续显示新帧（实时取景），不启动 3s 隐藏
 * 定时器；false（收尾沿）时 UI 立即隐藏并清残留。静态照片卡路径不受影响
 * （explain/capture 预览仍是"一帧 + 3s 自动消失"）。 */
static volatile bool s_pv_video;

void ai_camera_preview_video_set(bool on)
{
    s_pv_video = on;
}

bool ai_camera_preview_video_get(void)
{
    return s_pv_video;
}

static void preview_publish(uint8_t *rgb, int w, int h)
{
    taskENTER_CRITICAL(&s_pv_mux);
    if (s_pv_pending && s_pv_buf) {
        heap_caps_free(s_pv_buf);       /* UI 没来得及取的旧帧：丢 */
    }
    s_pv_buf = rgb;
    s_pv_w = w;
    s_pv_h = h;
    s_pv_pending = true;
    taskEXIT_CRITICAL(&s_pv_mux);
    ESP_LOGI(TAG, "preview published %dx%d", w, h);
}

bool ai_camera_preview_take(uint8_t **buf, int *w, int *h)
{
    bool got = false;
    taskENTER_CRITICAL(&s_pv_mux);
    if (s_pv_pending && s_pv_buf) {
        *buf = s_pv_buf;
        *w = s_pv_w;
        *h = s_pv_h;
        s_pv_buf = NULL;
        s_pv_pending = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_pv_mux);
    return got;
}

void ai_camera_preview_publish_rgb565(uint8_t *buf, int w, int h)
{
    if (buf && w > 0 && h > 0) {
        /* §73.19：打卡照片复用同一预览单槽（AI 浮窗照片卡 3s 自动消失）。
         * 语义与 preview_publish 一致：buf 所有权移交槽（UI take 后 UI free）。 */
        preview_publish(buf, w, h);
    } else if (buf) {
        heap_caps_free(buf);
    }
}

/* JPEG 码流 → 1/2 scale RGB888 解码 → 原地压缩 RGB565 → publish。
 * 失败静默（预览是赠品，不抢 explain/capture 主流程的报错通道）。
 *
 * ★★ §73.23 为什么不再用 RGB565_LE 直解（2026-09-30 实测偏绿事故）：
 *   旧版 cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE，真机上 AI 浮窗
 *   预览整体偏绿；同一份传感器 JPEG 落 SD 卡后走 FILES 页查看颜色正常
 *   （= 传感器码流本身颜色是对的），而本仓**所有 RGB888 解码点**
 *   （overlay_wallpaper / overlay_wp_particles）颜色全部正常。
 *   ⇒ 判定：软解 RGB565 输出转换环节与 LVGL 原生 u16 值序不符（组件是
 *   闭源 .a，实现不可读），绕开它——走已验证的 RGB888 路径，像素压缩
 *   在自己手里（与 ai_punch make_punch_preview 同一套位算式）。 */
static void preview_decode_publish(const uint8_t *jpg, size_t len)
{
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    uint8_t *out = NULL;    /* 解码期 = RGB888（padded_w*PV_H*3），解码后原地压缩成 RGB565 */
    int out_len = 0;

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;   /* §73.23：RGB888 是全仓已验证的正常色路径 */
    cfg.scale.width = PV_W;
    cfg.scale.height = PV_H;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK ||
        jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK) {
        goto out;
    }
    out = heap_caps_malloc((size_t)out_len, MALLOC_CAP_SPIRAM);
    if (!out) {
        goto out;
    }
    io.outbuf = out;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        heap_caps_free(out);
        out = NULL;
        goto out;
    }
    {
        /* 输出尺寸对账：块对齐可能补齐，宽度以 out_len 反推（防画面错乱） */
        int w = out_len / 3 / PV_H;
        if (out_len != PV_W * PV_H * 3) {
            ESP_LOGW(TAG, "preview padded: out_len=%d -> w=%d", out_len, w);
        }
        /* 原地压缩 RGB888 → RGB565（LVGL 原生小端 u16 值序）。
         * 前向迭代安全：第 i 像素写到字节偏移 2i，读自 3i，恒有 2i <= 3i。 */
        uint16_t *d = (uint16_t *)out;
        const uint8_t *p = out;
        for (int i = 0; i < w * PV_H; i++, p += 3) {
            d[i] = (uint16_t)(((uint16_t)(p[0] >> 3) << 11) |
                              ((uint16_t)(p[1] >> 2) << 5)  |
                              (uint16_t)(p[2] >> 3));
        }
        preview_publish(out, w, PV_H);
        out = NULL;     /* 所有权已移交槽 */
    }
out:
    if (out) {
        heap_caps_free(out);
    }
    if (dec) {
        jpeg_dec_close(dec);
    }
}

static void report(const char *text)
{
    ESP_LOGI(TAG, "result: %.96s", text);
    if (s_result_cb) {
        s_result_cb(text);
    }
}

/* cam_task 上下文：只 memcpy + 置事件（camera_service.h 硬约束） */
static void on_jpeg_frame(const uint8_t *data, uint32_t size,
                          uint32_t w, uint32_t h, void *user)
{
    (void)w;
    (void)h;
    (void)user;
    if (!s_jpg || s_jpg_len != 0) {
        return;   /* 已有帧/缓冲未备好：直接丢（流会继续送下一帧） */
    }
    if (size == 0 || size > JPG_BUF_SIZE) {
        ESP_LOGW(TAG, "frame too large: %u > %u", (unsigned)size, (unsigned)JPG_BUF_SIZE);
        return;   /* 等下一帧（不该发生：q8 实测远小于缓冲） */
    }
    memcpy(s_jpg, data, size);
    s_jpg_len = size;
    xEventGroupSetBits(s_frame_evt, BIT0);
}

static void finish_with(const char *msg)
{
    /* §73.18：结果双路分发——s_answer 交给 MCP 工具结果（服务端模型），
     * report() 走 XZ_EVT_TEXT 进 UI 浮窗对话记录（本地可见）。 */
    if (!s_answer_done) {
        strlcpy(s_answer, msg, sizeof(s_answer));
        s_answer_done = true;
    }
    report(msg);
}

/* ---------------------------------------------------------------------------
 *  云端视觉问答（S9 裁决 5）：拍照 → POST {gw}/upload/{sid} → WS image_req
 *
 *  ★ 为什么不再走小智 vision endpoint：那条路要求云端下发 URL + token，
 *    而 S9 的云端是我们自己的网关（8081），设备端不该持有任何视觉 API key。
 *    新路径把"看图"整个交给云端大模型（Qwen-VL），设备只负责把字节送上去。
 *  ★ 回答是**异步**回来的（WS 下行 text/audio → event_bus → 对话记录）：
 *    本函数返回 OK 只代表"照片已送达"，不代表"已经有答案"。
 * ------------------------------------------------------------------------- */

#define AID_MAX          72
#define UPLOAD_URL_MAX   192
#define UPLOAD_TIMEOUT_MS 20000

/** 最小 JSON 字符串转义（问题文本用户可控，引号/反斜杠必须处理） */
static void json_escape_into(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", *p);
        } else {
            dst[o++] = (char)*p;      /* UTF-8 原样透传 */
        }
    }
    dst[o] = '\0';
}

/**
 * 把 JPEG 上传到网关，取回 attachment_id。
 * @return ESP_OK 时 aid 有效；失败时 err 填人话原因
 */
static esp_err_t upload_photo(const uint8_t *jpg, size_t len,
                              char *aid, size_t aid_cap,
                              char *err, size_t err_cap)
{
    char base[96] = "";
    ws_client_get_http_base(base, sizeof(base));
    if (base[0] == '\0') {
        snprintf(err, err_cap, "Cloud endpoint is not configured.");
        return ESP_ERR_INVALID_STATE;
    }
    const char *sid = ws_client_get_session_id();
    char url[UPLOAD_URL_MAX];
    snprintf(url, sizeof(url), "%s/upload/%s", base, (sid && sid[0]) ? sid : "dev");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = UPLOAD_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        snprintf(err, err_cap, "Failed to create HTTP client.");
        return ESP_FAIL;
    }

    esp_err_t e = ESP_FAIL;
    char rsp[256];
    int rlen = 0;

    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    /* set_post_field 只存指针不拷贝（len 上限由 JPG_BUF_SIZE 保证 < INT_MAX） */
    e = esp_http_client_set_post_field(c, (const char *)jpg, (int)len);
    if (e != ESP_OK) {
        snprintf(err, err_cap, "Failed to set upload body.");
        goto done;
    }
    e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        snprintf(err, err_cap, "Upload failed (network).");
        goto done;
    }

    int status = esp_http_client_get_status_code(c);
    rlen = esp_http_client_read_response(c, rsp, (int)sizeof(rsp) - 1);
    if (rlen < 0) {
        rlen = 0;
    }
    rsp[rlen] = '\0';
    if (status != 200) {
        ESP_LOGW(TAG, "upload HTTP %d: %.120s", status, rsp);
        snprintf(err, err_cap, "Cloud rejected the photo (HTTP %d).", status);
        e = ESP_FAIL;
        goto done;
    }

    /* 只认 attachment_id 字段（响应很小，不做完整 JSON 解析） */
    const char *p = strstr(rsp, "\"attachment_id\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                const char *q = strchr(p, '"');
                if (q && (size_t)(q - p) < aid_cap) {
                    memcpy(aid, p, (size_t)(q - p));
                    aid[q - p] = '\0';
                }
            }
        }
    }
    if (aid[0] == '\0') {
        ESP_LOGW(TAG, "upload response has no attachment_id: %.160s", rsp);
        snprintf(err, err_cap, "Cloud did not return an attachment id.");
        e = ESP_FAIL;
        goto done;
    }

    ESP_LOGI(TAG, "uploaded %u B -> attachment_id=%s", (unsigned)len, aid);
    e = ESP_OK;

done:
    esp_http_client_cleanup(c);
    return e;
}

/** 通知云端：这张图要问什么（回答走下行，不在这里等） */
static esp_err_t send_image_req(const char *aid, const char *question)
{
    char q_esc[192];
    json_escape_into(q_esc, sizeof(q_esc),
                     (question && question[0]) ? question : "请描述这张照片");

    char line[384];
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"image_req\",\"attachment_id\":\"%s\",\"text\":\"%s\"}",
                     aid, q_esc);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ws_client_send_text(line);
}

static void explain_task(void *arg)
{
    (void)arg;
    bool cam_started = false;
    bool preempted = false;
    esp_err_t ret;

    /* 1. 相机事务。
     *   ★ 2026-10-31：只挡"真独占"（CAM_OWNER_MEDIA = 录像/拍照中）。
     *     CAM_OWNER_PREVIEW 是取景流，而且常常是**僵尸**：tabview 的页常驻，
     *     切走后流还开着却没人消费帧 —— 实测它就导致"查一下眼"永远回
     *     "Camera is busy"。取景可抢占：停掉、用完在 out: 里原样还回去。 */
    if (camera_service_owner() == CAM_OWNER_MEDIA) {
        finish_with("Camera is busy - close the CAMERA/VIDEO page first.");
        goto out;
    }
    if (camera_service_owner() == CAM_OWNER_PREVIEW && camera_service_is_running()) {
        esp_err_t se = camera_service_stop();
        ESP_LOGI(TAG, "preempt stale preview: %s", esp_err_to_name(se));
        preempted = (se == ESP_OK);
    }
    s_jpg = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_jpg) {
        finish_with("Out of memory for photo buffer.");
        goto out;
    }
    s_frame_evt = xEventGroupCreate();
    if (!s_frame_evt) {
        finish_with("Out of memory for frame event.");
        goto out;
    }
    ret = camera_service_start_fmt(on_jpeg_frame, NULL, CAM_FMT_JPEG, CAM_MEDIA_W, CAM_MEDIA_H);
    if (ret != ESP_OK) {
        finish_with("Failed to start camera for photo.");
        goto out;
    }
    cam_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_frame_evt, BIT0, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(FRAME_WAIT_MS));
    camera_service_stop();
    cam_started = false;
    if (!(bits & BIT0) || s_jpg_len == 0) {
        finish_with("No frame from camera - please retry.");
        goto out;
    }
    ESP_LOGI(TAG, "captured %u bytes JPEG, uploading...", (unsigned)s_jpg_len);

    /* 先发预览（解码约百 ms 级；用户"所见"先于 AI "所答"出现） */
    preview_decode_publish(s_jpg, s_jpg_len);

    /* 2. 上传 + WS image_req（含重试一次：隧道偶发 502 时不必让用户重拍） */
    char aid[AID_MAX] = "";
    char err[96] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        ret = upload_photo(s_jpg, s_jpg_len, aid, sizeof(aid), err, sizeof(err));
        if (ret == ESP_OK) {
            break;
        }
        if (!ws_client_is_connected()) {
            break;                     /* 链路断了，重试没意义 */
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "explain upload failed: %s", err);
        finish_with(err[0] ? err : "Failed to send the photo to the cloud.");
        goto out;
    }
    if (send_image_req(aid, s_question) != ESP_OK) {
        finish_with("Photo uploaded but the cloud link is down - please retry.");
        goto out;
    }
    finish_with("Photo sent - the answer will show up in a moment.");

out:
    if (cam_started) {
        camera_service_stop();
    }
    /* 抢占过的取景流原样还回去（"还占着"时 restore 会拒绝，所以必须排在 stop 之后） */
    if (preempted) {
        esp_err_t re = camera_service_restore_preview();
        if (re == ESP_OK) {
            ESP_LOGI(TAG, "preview restored after explain");
        } else if (re != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "restore preview failed: %s", esp_err_to_name(re));
        }
    }
    if (s_frame_evt) {
        vEventGroupDelete(s_frame_evt);
        s_frame_evt = NULL;
    }
    if (s_jpg) {
        heap_caps_free(s_jpg);
        s_jpg = NULL;
    }
    s_jpg_len = 0;
    s_busy = false;
    /* 唤醒等待中的调用方（所有路径必经；give 后本任务随即自删） */
    xSemaphoreGive(s_done);
    ESP_LOGI(TAG, "explain task done");
    vTaskDeleteWithCaps(NULL);
}

/** 回读一张已落盘的 JPEG 并解码发布到预览单槽（F35：SD FATFS 不关 flash
 * cache ⇒ PSRAM 栈安全；失败静默——预览是赠品，不抢主流程报错通道）。 */
static void preview_read_publish(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "preview: open %s failed", path);
        return;
    }
    uint8_t *jpg = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (jpg) {
        size_t n = fread(jpg, 1, JPG_BUF_SIZE, f);
        if (n > 0) {
            preview_decode_publish(jpg, n);
        }
        heap_caps_free(jpg);
    }
    fclose(f);
}

/* capture 路径预览：等 capture_service 落盘完成（受理制非阻塞），回读文件解码。
 * SD 卡 FATFS 读写不关 flash cache ⇒ PSRAM 栈安全（F35 只禁 NVS/littlefs/flash IO）。 */
#define CAPV_WAIT_MS  6000
#define CAPV_STACK    (12 * 1024)

static void cap_preview_task(void *arg)
{
    (void)arg;
    int waited = 0;

    while (capture_service_state() == CAPTURE_BUSY && waited < CAPV_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    capture_result_t res;
    capture_service_get_result(&res);
    if (res.err == ESP_OK && res.path[0]) {
        preview_read_publish(res.path);
    }
    vTaskDeleteWithCaps(NULL);
}

/* ---------------------------------------------------------------------------
 *  公共入口：对一张已落盘的照片补发预览（F69）。
 *  拍照有两条触发路：MCP 工具 cb_capture（挂了 cap_preview_task）与
 *  云端动作快速路 ai_action::do_capture（自己等结果，从不建预览任务）
 *  —— 后者实测就是"拍照成功但浮窗没图"的原因。把回读+解码+发布封装成
 *  本 API，两条路共用。异步：起一次性任务（PSRAM 栈，自删），立即返回。
 * ------------------------------------------------------------------------- */
typedef struct {
    char path[96];
} pv_file_arg_t;

static void pv_file_task(void *arg)
{
    pv_file_arg_t *a = (pv_file_arg_t *)arg;
    preview_read_publish(a->path);
    heap_caps_free(a);
    vTaskDeleteWithCaps(NULL);
}

void ai_camera_preview_file(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    pv_file_arg_t *a = heap_caps_malloc(sizeof(*a), MALLOC_CAP_SPIRAM);
    if (a == NULL) {
        ESP_LOGW(TAG, "preview file: no mem for arg");
        return;
    }
    snprintf(a->path, sizeof(a->path), "%s", path);
    if (xTaskCreateWithCaps(pv_file_task, "ai_pvf", CAPV_STACK,
                            a, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        heap_caps_free(a);
        ESP_LOGW(TAG, "preview file task create failed");
        return;
    }
    ESP_LOGI(TAG, "preview from file: %s", path);
}

/* ---------------------------------------------------------------------------
 *  公共入口：发起一次"拍照问云端"（S9 裁决 5）
 *  异步受理；answer 不是返回值，而是云端经 WS 下行回来的 text/audio。
 * ------------------------------------------------------------------------- */
esp_err_t ai_camera_explain_cloud(const char *question)
{
    if (!s_done) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *q = (question && question[0]) ? question : "请用一句话描述这张照片里有什么";
    if (q != s_question) {          /* 防自拷贝（snprintf 重叠是 UB） */
        snprintf(s_question, sizeof(s_question), "%s", q);
    }

    s_busy = true;
    s_jpg_len = 0;
    s_answer[0] = '\0';
    s_answer_done = false;

    if (xTaskCreateWithCaps(explain_task, "ai_cam", EXPLAIN_STACK_SIZE,
                            NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_busy = false;
        ESP_LOGE(TAG, "create explain task failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "explain requested: %.64s", s_question);
    return ESP_OK;
}

/* ---- MCP 工具回调（引擎无传输层也保留：未来可换回 MCP 通道） ---- */

static esp_mcp_value_t cb_explain(const esp_mcp_property_list_t *props)
{
    if (s_busy) {
        return esp_mcp_value_create_string("A photo is already being analyzed, please wait.");
    }
    if (!s_done) {
        return esp_mcp_value_create_string("Camera service is not initialized.");
    }
    const char *q = esp_mcp_property_list_get_property_string(props, "question");
    esp_err_t e = ai_camera_explain_cloud((q && q[0]) ? q : NULL);
    if (e != ESP_OK) {
        return esp_mcp_value_create_string("Failed to start the camera task (busy or out of memory).");
    }

    /* 等 explain 任务落到"已上传/失败"（上传超时 20s × 2 次 + 相机 3s）。
     * ★ 注意：这里等到的是**照片送达结论**，不是答案本身——答案由云端
     *   经 WS 下行异步回来（event_bus → 对话记录 + 语音），因为看图的是
     *   云端大模型，设备端没有同步通道（S9 裁决 5）。 */
    xSemaphoreTake(s_done, pdMS_TO_TICKS(EXPLAIN_BRIDGE_TIMEOUT_MS));

    if (s_answer[0] == '\0') {
        ESP_LOGW(TAG, "explain bridge timeout");
        return esp_mcp_value_create_string("Photo upload timed out - please retry.");
    }
    return esp_mcp_value_create_string(s_answer);
}

/* ---- 注册 ---- */

/* §58 C 级：拍照存 /sdcard/photos（FILES 页可看）。capture_service 受理制
 * 非阻塞（切档→抓帧→落盘→归还，约 1~3s 自行完成），回调直接受理即回。
 * 与 explain 的互斥天然成立：capture 在相机被占（含 explain 事务中）时
 * 返回 INVALID_STATE。 */
static esp_mcp_value_t cb_capture(const esp_mcp_property_list_t *props)
{
    (void)props;
    esp_err_t r = capture_service_request();
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "capture requested (-> /sdcard/photos)");
        /* 预览任务（后置：等落盘完成回读解码；失败不影响照片本身） */
        if (xTaskCreateWithCaps(cap_preview_task, "ai_cappv", CAPV_STACK,
                                NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGW(TAG, "preview task create failed (photo still saved)");
        }
        return esp_mcp_value_create_string(
            "Photo taken and saved to the SD card (photos folder). "
            "It can be viewed in the FILES page.");
    }
    if (r == ESP_ERR_INVALID_STATE) {
        return esp_mcp_value_create_string(
            "Cannot take a photo now - camera is busy or SD card is not mounted.");
    }
    return esp_mcp_value_create_string("Failed to request a photo.");
}

esp_err_t ai_camera_init(void)
{
    if (s_done) {
        return ESP_OK;   /* 幂等 */
    }
    /* 完成信号量（先于任务创建，失败即整体失败）。
     * 相机硬件本身由 camera_service 管理，这里不再创建任何客户端。 */
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "bridge semaphore");
    return ESP_OK;
}

void ai_camera_set_result_cb(ai_camera_result_cb_t cb)
{
    s_result_cb = cb;
}

esp_err_t ai_camera_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_INVALID_STATE, TAG, "ai_camera_init first");

    esp_mcp_property_t *p_q = esp_mcp_property_create_with_string(
        "question", "请用一句话描述这张照片里有什么");
    ESP_RETURN_ON_FALSE(p_q, ESP_FAIL, TAG, "prop question");

    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "self.camera.explain",
        "拍一张照片并用视觉模型回答。当用户想让你\"看看\"眼前的东西、描述看到的场景时调用。"
        "question: 想针对照片问的问题（可选）",
        cb_explain);
    ESP_RETURN_ON_FALSE(tool, ESP_FAIL, TAG, "tool create");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(tool, p_q) == ESP_OK,
                        ESP_FAIL, TAG, "add prop");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool");
    ESP_LOGI(TAG, "mcp tool: self.camera.explain");

    /* self.camera.capture：无参数 */
    esp_mcp_tool_t *t2 = esp_mcp_tool_create(
        "self.camera.capture",
        "拍一张照片保存到 SD 卡（photos 目录），供用户在 FILES 页查看。"
        "不需要视觉回答、只想留存照片时调用",
        cb_capture);
    ESP_RETURN_ON_FALSE(t2, ESP_FAIL, TAG, "tool create capture");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, t2) == ESP_OK,
                        ESP_FAIL, TAG, "add tool capture");
    ESP_LOGI(TAG, "mcp tool: self.camera.capture");
    return ESP_OK;
}
