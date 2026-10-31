/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ws_client.c
 * @brief 云端网关 WebSocket 长连接客户端实现（S9 P1）
 *
 * 结构：
 *   ws_sup_task（supervisor，内部栈 3 KB）
 *     ├─ ws_open_once()：建 client（首次）→ start → 等 CONNECTED（≤15 s）
 *     ├─ 连上后每 200 ms 看一次 s_connected，掉线即进退避
 *     └─ 退避 1s→2s→…→30s（连上即复位 1s）
 *   esp_websocket_client 自己的任务（内部栈 8 KB）：收帧 → 按 '\n' 切行 → rx_cb
 *
 * ★ 为什么自己写 supervisor 而不是用客户端自带的自动重连：
 *   自带重连是**固定间隔**（reconnect_timeout_ms），计划文档要求**指数退避**
 *   （1s→30s）。固定 1s 在服务器挂掉时会变成每秒一次 TLS/TCP 重连风暴。
 *
 * ★ 为什么允许 start-after-stop（同一个 client 句柄复用）：
 *   esp_websocket_client.c:1456/1485 实证 —— stop 把 state 复位成 UNKNOW 并删掉
 *   自己的任务，start 只要 state < INIT 就重新建 transport + 建任务，
 *   ⇒ 句柄可以反复 start/stop，不需要每次 destroy/init（少一次堆抖动）。
 *
 * ★ 下行帧的拼装：payload 超过 buffer_size 时客户端会分多次 WEBSOCKET_EVENT_DATA
 *   交付。**边界由收到内容的括号配对决定**（'{"' … '}'，含字符串转义），不看 '\n'、
 *   也不看 fin：网关一帧一条 JSON 且帧尾无换行，连着 send_text 的多帧还可能被 TCP
 *   合并进同一次 read（F-S9-A，2026-10-31 实机：只按 '\n' 切 ⇒ 设备一条下行都收不到）。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_websocket_client.h"

#include "ws_client.h"

static const char *TAG = "WS";

/* ---- 可调参数 ---- */
#define WS_TASK_STACK        8192        /* 内部 RAM（F35）；只跑 cJSON + 投队列 */
#define WS_TASK_PRIO         8
#define WS_TASK_NAME         "pet_ws"
#define WS_SUP_STACK         3072        /* supervisor：只做延时与状态判断 */
#define WS_SUP_PRIO          4
#define WS_SUP_NAME          "pet_ws_sup"
#define WS_RX_BUF_SIZE       (8 * 1024)  /* 行累加缓冲（PSRAM） */
#define WS_TX_TIMEOUT_MS     8000
#define WS_NET_TIMEOUT_MS    15000
#define WS_CONNECT_WAIT_MS   15000       /* 单次连接尝试的最长等待 */
#define WS_BACKOFF_MIN_MS    1000
#define WS_BACKOFF_MAX_MS    30000
#define WS_URI_MAX           160
#define WS_SESSION_MAX       24

/* ---- NVS ---- */
#define WS_NVS_NS            "pet_cfg"
#define WS_NVS_KEY_BASE      "ws_base"

/* 编译期默认（可被 CMake -DPET_GATEWAY_URL_DEFAULT=... 覆盖，见 tools/idf_env.local.ps1） */
#ifndef PET_GATEWAY_URL_DEFAULT
#define PET_GATEWAY_URL_DEFAULT  "ws://120.76.230.17:8081/ws/"
#endif

/* ---- 连接位 ---- */
#define WS_BIT_CONNECTED     BIT0
#define WS_BIT_CLOSED        BIT1

/* ---- 状态 ---- */
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_mtx;             /* 保护 s_ws 的建/毁/发送 */
static EventGroupHandle_t s_eg;
static TaskHandle_t  s_sup_task;
static volatile bool s_want;                /* 期望保持连接（stop() 置 false） */
static volatile bool s_connected;
static volatile bool s_opening;             /* supervisor 正在 start 中（send 直接拒） */

static ws_client_rx_cb_t    s_rx_cb;
static void                *s_rx_ctx;
static ws_client_state_cb_t s_state_cb;
static void                *s_state_ctx;

static char s_session_id[WS_SESSION_MAX];
static char s_base[WS_URI_MAX];             /* 用户配置的 base（不含 session） */
/* ★ 完整 URI 要比 base 再宽出 session_id + '/'：
 *   GCC 15 的 -Wformat-truncation 会证明 "base(≤159)+sep+sid(≤23) > 160"
 *   并升级成 -Werror 硬错误（真实构建同样炸，不是本工具的假错）。 */
#define WS_URI_FULL_MAX  (WS_URI_MAX + WS_SESSION_MAX + 8)
static char s_uri[WS_URI_FULL_MAX];         /* 实际连接用的完整 URI */

static char   *s_rx_buf;                    /* PSRAM，当前 JSON 对象累加 */
static size_t  s_rx_len;
static uint32_t s_rx_overflow;

/* ★ JSON 对象切分状态机（跨 WEBSOCKET_EVENT_DATA 事件保持）：
 *   下行是"一帧一条 JSON、帧尾不带 '\n'"，且网关会连着 send_text 多帧
 *   （text/emotion/audio 三连），TCP 侧还可能把两帧合进一次 read。
 *   ⇒ 既不能按 '\n' 切，也不能按 fin 切，只能靠 '{' … '}' 的配对（含字符串转义）。 */
static bool s_js_in_obj;
static bool s_js_in_str;
static bool s_js_esc;
static int  s_js_depth;

/* ============================================================
 *  工具
 * ============================================================ */

static bool uri_scheme_ok(const char *u)
{
    return u && (strncmp(u, "ws://", 5) == 0 || strncmp(u, "wss://", 6) == 0);
}

/** 把 base 与 session_id 拼成完整 URI：
 *  base 以 '#' 结尾 = 调用方给的已是完整地址（不再拼）；
 *  base 以 '/' 结尾 = 直拼；否则补一个 '/'。 */
static void compose_uri(void)
{
    size_t n = strlen(s_base);
    if (n > 0 && s_base[n - 1] == '#') {
        snprintf(s_uri, sizeof(s_uri), "%.*s", (int)(n - 1), s_base);
        return;
    }
    const char *sep = (n > 0 && s_base[n - 1] == '/') ? "" : "/";
    snprintf(s_uri, sizeof(s_uri), "%s%s%s", s_base, sep, s_session_id);
}

static void load_base_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 首次上电/命名空间还没建：用编译期默认值，不算错误 */
        snprintf(s_base, sizeof(s_base), "%s", PET_GATEWAY_URL_DEFAULT);
        return;
    }
    size_t len = sizeof(s_base);
    err = nvs_get_str(h, WS_NVS_KEY_BASE, s_base, &len);
    nvs_close(h);
    if (err != ESP_OK || !uri_scheme_ok(s_base)) {
        snprintf(s_base, sizeof(s_base), "%s", PET_GATEWAY_URL_DEFAULT);
    }
}

static void set_connected(bool on)
{
    if (s_connected == on) {
        return;
    }
    s_connected = on;
    if (on) {
        xEventGroupSetBits(s_eg, WS_BIT_CONNECTED);
    } else {
        xEventGroupClearBits(s_eg, WS_BIT_CONNECTED);
        xEventGroupSetBits(s_eg, WS_BIT_CLOSED);
    }
    ESP_LOGI(TAG, "%s (%s)", on ? "CONNECTED" : "DISCONNECTED", s_uri);
    ws_client_state_cb_t cb = s_state_cb;
    if (cb) {
        cb(on, s_state_ctx);
    }
}

/* ============================================================
 *  收包：按 JSON 对象边界切分（不是按行）
 * ============================================================ */

static void rx_deliver(char *msg)
{
    size_t n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' ||
                     msg[n - 1] == ' '  || msg[n - 1] == '\t')) {
        msg[--n] = '\0';
    }
    if (n == 0) {
        return;
    }
    ws_client_rx_cb_t cb = s_rx_cb;
    if (cb) {
        cb(msg, s_rx_ctx);
    }
}

static void rx_reset(void)
{
    s_rx_len = 0;
    if (s_rx_buf) {
        s_rx_buf[0] = '\0';
    }
    s_js_in_obj = false;
    s_js_in_str = false;
    s_js_esc    = false;
    s_js_depth  = 0;
}

/** 喂入任意分片，按 '{' … '}' 配对整个对象后交给 rx_cb。
 *  ★ 为什么不用 '\n' 也不看 fin：网关一帧一条 JSON、帧尾无 '\n'；
 *    连着发 text/emotion/audio 时 TCP 可能把多帧合进一次 read，
 *    fin/payload_len 只描述第一帧 ⇒ 只有括号配对是可靠边界。 */
static void rx_accum(const char *data, int len)
{
    if (s_rx_buf == NULL || data == NULL || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        char c = data[i];

        if (!s_js_in_obj) {
            /* 对象外只认 '{'，其余（空白/换行/杂字节）全部丢弃 */
            if (c != '{') {
                continue;
            }
            s_js_in_obj = true;
            s_js_in_str = false;
            s_js_esc    = false;
            s_js_depth  = 1;
            s_rx_len    = 0;
            s_rx_buf[s_rx_len++] = c;
            s_rx_buf[s_rx_len]   = '\0';
            continue;
        }

        if (s_rx_len + 1 >= WS_RX_BUF_SIZE) {
            /* 单个对象超缓冲：整条丢弃，状态机复位（只记一次） */
            rx_reset();
            if (s_rx_overflow++ == 0) {
                ESP_LOGW(TAG, "rx frame overflow (>= %d B), dropped", WS_RX_BUF_SIZE);
            }
            continue;
        }
        s_rx_buf[s_rx_len++] = c;
        s_rx_buf[s_rx_len]   = '\0';

        if (s_js_in_str) {
            if (s_js_esc) {
                s_js_esc = false;
            } else if (c == '\\') {
                s_js_esc = true;
            } else if (c == '"') {
                s_js_in_str = false;
            }
            continue;
        }
        if (c == '"') {
            s_js_in_str = true;
        } else if (c == '{') {
            s_js_depth++;
        } else if (c == '}') {
            s_js_depth--;
            if (s_js_depth == 0) {
                rx_deliver(s_rx_buf);
                rx_reset();
            }
        }
    }
}

/* ============================================================
 *  esp_websocket_client 事件
 * ============================================================ */

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        rx_reset();                 /* 上一条连接可能留了半截对象 */
        s_opening = false;
        set_connected(true);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_opening = false;
        set_connected(false);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        /* 服务端主动 close 帧：随后还会来 DISCONNECTED，这里只记日志 */
        ESP_LOGW(TAG, "server closed the link");
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d) {
            ESP_LOGW(TAG, "ws error: type=%d tls=0x%x http=%d errno=%d",
                     (int)d->error_handle.error_type,
                     d->error_handle.esp_tls_last_esp_err,
                     d->error_handle.esp_ws_handshake_status_code,
                     d->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d == NULL) {
            break;
        }
        /* 0x1 = text；0x0 = 续段；0x2 = binary（v1 不用）；0x9/0xA = ping/pong 内部已处理
         * ★★★ F-S9-A（2026-10-31 实机）：网关 `ws.send_text(json.dumps(...))` 一帧一条、
         *   帧尾不带 '\n'，旧实现只按 '\n' 切 ⇒ 所有下行永远留在缓冲里。实机表现：
         *   `WS: CONNECTED`、`pcm up ... ESP_OK`、但零 `EVT:` 日志、界面完全没反应
         *   （连连接后第一条 command_table 都收不到）。现在改由 rx_accum 的
         *   括号配对状态机切对象，帧边界与 '\n' 都不再参与判断。 */
        if ((d->op_code == 0x1 || d->op_code == 0x0) && d->data_ptr && d->data_len > 0) {
            rx_accum(d->data_ptr, d->data_len);
        }
        break;
    default:
        break;
    }
}

/* ============================================================
 *  supervisor
 * ============================================================ */

/** 建 client（首次）并发起一次连接尝试；等 CONNECTED 或超时 */
static esp_err_t ws_open_once(void)
{
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_ws == NULL) {
        esp_websocket_client_config_t cfg = {
            .uri = s_uri,
            .buffer_size = 2048,
            .task_stack = WS_TASK_STACK,
            .task_prio = WS_TASK_PRIO,
            .task_name = WS_TASK_NAME,
            /* 退避由本模块 supervisor 负责 ⇒ 关掉客户端自带的固定间隔重连；
             * 但 close 帧之后也不自动重连（enable_close_reconnect 保持默认 false） */
            .disable_auto_reconnect = true,
            .network_timeout_ms = WS_NET_TIMEOUT_MS,
            .reconnect_timeout_ms = WS_BACKOFF_MIN_MS,
            /* 只有 wss 才需要证书包；ws 明文时这个指针不会被用到 */
            .crt_bundle_attach = (strncmp(s_uri, "wss://", 6) == 0)
                                     ? esp_crt_bundle_attach : NULL,
        };
        s_ws = esp_websocket_client_init(&cfg);
        if (s_ws == NULL) {
            xSemaphoreGive(s_mtx);
            ESP_LOGE(TAG, "client init failed (uri=%s)", s_uri);
            return ESP_FAIL;
        }
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    }
    xEventGroupClearBits(s_eg, WS_BIT_CONNECTED | WS_BIT_CLOSED);
    s_opening = true;
    esp_err_t err = esp_websocket_client_start(s_ws);
    xSemaphoreGive(s_mtx);
    if (err != ESP_OK) {
        s_opening = false;
        ESP_LOGW(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_eg, WS_BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WS_CONNECT_WAIT_MS));
    if (bits & WS_BIT_CONNECTED) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "connect timeout (%d ms): %s", WS_CONNECT_WAIT_MS, s_uri);
    return ESP_ERR_TIMEOUT;
}

/** 停掉并销毁 client（下次从零建，避免半死状态残留） */
static void ws_close(void)
{
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "close: lock timeout (task may be stuck in send)");
        return;
    }
    s_opening = false;
    if (s_ws) {
        (void)esp_websocket_client_stop(s_ws);
        (void)esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
    xSemaphoreGive(s_mtx);
}

static void ws_sup_task(void *arg)
{
    (void)arg;
    uint32_t backoff = WS_BACKOFF_MIN_MS;
    uint32_t attempt = 0;

    while (s_want) {
        attempt++;
        esp_err_t err = ws_open_once();
        if (err == ESP_OK) {
            attempt = 0;
            backoff = WS_BACKOFF_MIN_MS;
            /* 连上后就在这里看护，掉线立刻收尾进退避 */
            while (s_want && s_connected) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!s_want) {
                break;
            }
            ESP_LOGW(TAG, "link down, reconnecting in %u ms", (unsigned)backoff);
        }
        ws_close();
        if (!s_want) {
            break;
        }
        /* 分片延时：保证 stop() 能及时打断 30 s 的长等待 */
        for (uint32_t waited = 0; waited < backoff && s_want; waited += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        backoff = (backoff * 2 > WS_BACKOFF_MAX_MS) ? WS_BACKOFF_MAX_MS : backoff * 2;
        (void)attempt;
    }

    ws_close();
    s_sup_task = NULL;
    ESP_LOGI(TAG, "supervisor exit");
    vTaskDelete(NULL);
}

/* ============================================================
 *  Public API
 * ============================================================ */

esp_err_t ws_client_init(void)
{
    if (s_mtx) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_eg = xEventGroupCreate();
    if (s_eg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* 行缓冲走 PSRAM：内部 RAM 只剩 ~4 KB 余量，8 KB 内部缓冲是自杀 */
    s_rx_buf = heap_caps_malloc(WS_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rx_buf == NULL) {
        ESP_LOGE(TAG, "rx buf alloc failed (%d B PSRAM)", WS_RX_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_rx_buf[0] = '\0';
    s_rx_len = 0;

    /* 会话 ID = xz-s31-<mac 末 4 hex>（D6：设备独立，不复用微信会话） */
    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        mac[4] = 0x00;
        mac[5] = 0x01;
    }
    snprintf(s_session_id, sizeof(s_session_id), "xz-s31-%02x%02x", mac[4], mac[5]);

    load_base_from_nvs();
    compose_uri();
    ESP_LOGI(TAG, "ready: session=%s uri=%s", s_session_id, s_uri);
    return ESP_OK;
}

esp_err_t ws_client_start(void)
{
    if (s_mtx == NULL) {
        esp_err_t err = ws_client_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_want) {
        return ESP_OK;
    }
    s_want = true;
    /* 地址可能在 NVS 里被网页端改过：每次 start 重新装一次（幂等，几百 us） */
    load_base_from_nvs();
    compose_uri();
    if (xTaskCreate(ws_sup_task, WS_SUP_NAME, WS_SUP_STACK, NULL, WS_SUP_PRIO,
                    &s_sup_task) != pdPASS) {
        s_want = false;
        s_sup_task = NULL;
        ESP_LOGE(TAG, "supervisor task create failed (%d B)", WS_SUP_STACK);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ws_client_stop(void)
{
    if (s_mtx == NULL) {
        return;
    }
    s_want = false;
    /* 等 supervisor 收尾（它每 200 ms 看一次 s_want，最多 ~1 s 退出） */
    for (int i = 0; i < 40 && s_sup_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ws_close();
    ESP_LOGI(TAG, "stopped");
}

bool ws_client_is_connected(void)
{
    return s_connected;
}

esp_err_t ws_client_send_text(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected || s_ws == NULL || s_opening) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_ws && s_connected) {
        int n = esp_websocket_client_send_text(s_ws, json, (int)strlen(json),
                                               pdMS_TO_TICKS(WS_TX_TIMEOUT_MS));
        err = (n >= 0) ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_mtx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send failed (%d B): %s", (int)strlen(json), esp_err_to_name(err));
    }
    return err;
}

void ws_client_set_rx_cb(ws_client_rx_cb_t cb, void *ctx)
{
    s_rx_ctx = ctx;
    s_rx_cb = cb;
}

void ws_client_set_state_cb(ws_client_state_cb_t cb, void *ctx)
{
    s_state_ctx = ctx;
    s_state_cb = cb;
}

const char *ws_client_get_session_id(void)
{
    return s_session_id;
}

esp_err_t ws_client_set_uri(const char *uri)
{
    if (!uri_scheme_ok(uri)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(uri) >= WS_URI_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, WS_NVS_KEY_BASE, uri);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_base, sizeof(s_base), "%s", uri);
    compose_uri();
    ESP_LOGW(TAG, "gateway uri updated -> %s (take effect on next session)", s_uri);
    return ESP_OK;
}

void ws_client_get_uri(char *out, size_t out_size)
{
    if (out && out_size) {
        snprintf(out, out_size, "%s", s_uri);
    }
}

void ws_client_get_base_uri(char *out, size_t out_size)
{
    if (out && out_size) {
        snprintf(out, out_size, "%s", s_base);
    }
}

void ws_client_get_http_base(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    /* 例：ws://1.2.3.4:8081/ws/xz-s31-ab12 → http://1.2.3.4:8081 */
    const char *p = strstr(s_uri, "://");
    if (p == NULL) {
        out[0] = '\0';
        return;
    }
    const char *host = p + 3;
    const char *slash = strchr(host, '/');
    size_t host_len = slash ? (size_t)(slash - host) : strlen(host);
    const char *scheme = (strncmp(s_uri, "wss://", 6) == 0) ? "https://" : "http://";
    snprintf(out, out_size, "%s%.*s", scheme, (int)host_len, host);
}
