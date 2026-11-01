/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_audio_service.c
 * @brief 蓝牙 A2DP 中间层实现（封装 esp_bt_audio），本工程只做 Source。
 *
 * 职责：电源开关 + host init/deinit + 事件回调 + 扫描/连接 + 推流控制。
 * 推流的本地文件数据流（local2bt GMF pipeline）在 bt_push_service.c。
 *
 * 相对 display_audio_photo 主线的裁剪：
 *   - 删除 A2DP Sink（bt_sink_service）、元数据/封面、AVRCP 音量、角色切换；
 *   - 删除 session_manager（本工程音频互斥由 audio_service ext 协议负责）；
 *   - button_service 路径改为 middleware/periph/button_service.h；
 *   - 新增：设备列表缓存（UI 侧轮询）、推流结束上层回调。
 */

#include "bt_audio_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"                 /* 连接节流用的单调毫秒时钟 */
#include "sdkconfig.h"
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"               /* esp_coex_preference_set：射频偏向蓝牙 */
#endif

#include "esp_bt_audio.h"
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_vol.h"

#include "bt_push_service.h"

#include "middleware/periph/button_service.h"

static const char *TAG = "BT_SVC";

static bt_audio_evt_cb_t s_evt_cb = NULL;
static void *s_evt_ctx = NULL;
static bool s_on = false;
static bool s_controller_ready = false;

/* ---- 连接状态：音量分发据此决定"归蓝牙管"还是"归本地 codec 管" ---- */
static volatile bool s_connected = false;
/* 已连对端地址：真断开要用 classic_disconnect(role, addr)，BT 栈不缓存地址 */
static uint8_t s_peer_addr[6];
static volatile bool s_peer_valid = false;

/* ---- 扫描发现的设备缓存（BT 线程写，UI 线程只拷贝读取） ---- */
static bt_device_t s_devices[BT_MAX_DEVICES];
static volatile int s_dev_count = 0;

/* ---- 推流自然结束 → 播放层回调 ---- */
static void (*s_push_finished_cb)(void) = NULL;

/* ---- 开蓝牙 host 前的内部 RAM 门槛 ----
 * bluedroid host 起来时要一口气建出 BTU/BTC 任务、各层队列和信号量。
 * 这些分配一旦失败，库内部会直接 assert 崩溃，而不是返回错误码。
 * 所以开之前先体检，不够就**干脆不开**：功能不可用好过整机重启。
 *
 * ★ 2026-11-01 重新标定（原值 48 KB / 24 KB 在本工程永远打不开蓝牙）：
 *   主线没开 BT_ALLOCATION_FROM_SPIRAM_FIRST，host 的大块也跟着要内部 RAM；
 *   本工程 sdkconfig:1986 该开关为 y，且 SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
 *   （A2DP 发送任务 4096 B 栈自己也落在 PSRAM，见 bt_audio_a2dp_src.c:516），
 *   所以 host init 真正只能从内部 RAM 拿的固定开销是：
 *     BTC 任务栈 3072 (CONFIG_BT_BTC_TASK_STACK_SIZE)
 *   + BTU 任务栈 4352 (CONFIG_BT_BTU_TASK_STACK_SIZE)
 *   + osi 队列/信号量                                 ≈ 8.5 KB 连续块
 *   真机实测（2026-11-01）空闲时 free=25471 / largest=19456 就足以起来，
 *   故按「peak × 1.5」= 12 KB 连续 / 20 KB 总量作为闸门。 */
#define BT_HOST_MIN_FREE_INTERNAL     (20 * 1024)
#define BT_HOST_MIN_LARGEST_BLOCK     (12 * 1024)

/* 对外广播的蓝牙设备名（手机/耳机搜索时看到的名字） */
#define BT_DEV_NAME                   "PetCompanion"

/* A2DP 是全局单例，角色位在 esp_bt_audio_init() 里一次性传入。
 * ★ 2026-11-01 只保留 A2DP_SRC：本工程完全不用 AVRCP（不做音量/曲目控制）。
 *   原先 roles 里带 AVRC_CT，esp_bt_audio 会额外注册一个 AVA 控制通道；
 *   真机日志里 "BT_AVCT: Out of ccbs" 出现的频率明显更高（协议栈内部 AVRCP
 *   通道池 AVCT_NUM_CONN=3 被这条多余通道一起占）。日志里
 *   "CT: register volume change notification / CT get rn capabilities" 就是它。
 *   去掉后本侧不再申请 AVRC_CT，只留 bluedroid 为 A2DP 自建的那一条 RC 通道。
 *   注意：AVRCP 在 bluedroid 里被 BT_A2DP_ENABLE select（Kconfig 硬耦合），
 *   协议栈内部那条 RC 通道关不掉，这不是"没配置对"。 */
static uint32_t classic_roles(void)
{
    return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC;
}

/* ---- 连接节流：A2DP 失败后立刻重连必然再失败，而且会掐掉协议栈自己的重连 ----
 * ★ 2026-11-01 真机日志实证（两条路径对比）：
 *   失败：`connect SRC` → `hcif conn complete`(ACL 好) → ~100 ms 后
 *         `BTA_AV_OPEN_EVT::FAILED status 3`，且每次都打 `reset flags`
 *         （bta_av_save_addr 认为"新一轮 open"，把 recfg_sup/suspend_sup 重置）；
 *   成功：`A2DP connection state: Connected` 之前**没有** `connect SRC` 这行,
 *         是协议栈内部 bta_av_rcfg_discntd 的 num_recfg 重试自己连上的
 *         （栈内部的重连窗口约 BTA_AV_CLOSE_REQ_TIME_VAL = 4000 ms）。
 *   ⇒ 我们每 0.5~1.5 s 一次的 API connect 等于不停把 scb 打回初始态，
 *     把栈内部那次"最终会成功"的重连掐死。
 *   所以这里把窗口拉到 5 s（比栈内部 4 s 略长），并且"正在连"期间直接忽略
 *     重复请求（一次 page+AVDTP 尝试最长可达十几秒，期间再发 connect 纯属添乱）。
 * ★ 只用时间差 + connecting 标志：失败事件在个别场景（ACL 超时）可能不上报，
 *   粘滞的 connecting 标志会挡死后续重连——所以对它加超时兜底。 */
#define BT_CONNECT_MIN_GAP_MS       5000
#define BT_CONNECTING_TIMEOUT_MS    20000   /* 超过这个还没出事件，就认为卡死，放行 */

static uint32_t s_last_connect_ms = 0;   /* 0 = 还没尝试过 */
static volatile bool s_connecting = false;
static uint32_t s_connecting_since_ms = 0;

static inline uint32_t bt_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- esp_bt_audio 事件回调（跑在 BT/GMF 线程，只转发事件） ---- */
static void on_bt_audio_event(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    (void)user_data;
    switch (event) {
    case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
        esp_bt_audio_event_discovery_st_t *d = (esp_bt_audio_event_discovery_st_t *)event_data;
        ESP_LOGI(TAG, "discovery %s", d->discovering ? "start" : "stop");
        if (s_evt_cb) s_evt_cb(d->discovering ? BT_EVT_DISCOVERY_START : BT_EVT_DISCOVERY_STOP, NULL, s_evt_ctx);
        break;
    }
    case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
        esp_bt_audio_event_device_discovered_t *dev = (esp_bt_audio_event_device_discovered_t *)event_data;
        if (dev->tech != ESP_BT_AUDIO_TECH_CLASSIC) break;

        /* 去重：同一 MAC 只入表一次（RSSI 抖动不管） */
        int i;
        for (i = 0; i < s_dev_count; i++) {
            if (memcmp(s_devices[i].addr, dev->addr, 6) == 0) {
                break;
            }
        }
        if (i == s_dev_count && s_dev_count < BT_MAX_DEVICES) {
            bt_device_t *slot = &s_devices[s_dev_count];
            memset(slot, 0, sizeof(*slot));
            memcpy(slot->name, dev->name, sizeof(slot->name) - 1);
            memcpy(slot->addr, dev->addr, 6);
            slot->rssi = dev->rssi;
            s_dev_count++;   /* volatile 写在后，读侧先读 count */
            ESP_LOGI(TAG, "found[%d]: %s [%02x:%02x:%02x:%02x:%02x:%02x] rssi=%d",
                     s_dev_count - 1, slot->name, dev->addr[0], dev->addr[1], dev->addr[2],
                     dev->addr[3], dev->addr[4], dev->addr[5], dev->rssi);
            if (s_evt_cb) s_evt_cb(BT_EVT_DEVICE_FOUND, slot, s_evt_ctx);
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        esp_bt_audio_event_connection_st_t *c = (esp_bt_audio_event_connection_st_t *)event_data;
        ESP_LOGI(TAG, "connection %s addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 c->connected ? "CONNECTED" : "DISCONNECTED",
                 c->addr[0], c->addr[1], c->addr[2], c->addr[3], c->addr[4], c->addr[5]);
        s_connected = c->connected;
        /* 无论连上还是失败，本轮尝试都算结束（含协议栈内部重连的结果） */
        s_connecting = false;
        if (c->connected) {
            memcpy(s_peer_addr, c->addr, 6);
            s_peer_valid = true;
        } else {
            s_peer_valid = false;
        }
        if (s_evt_cb) s_evt_cb(c->connected ? BT_EVT_CONNECTED : BT_EVT_DISCONNECTED, c->addr, s_evt_ctx);
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
        ESP_LOGI(TAG, "stream %p state=%d", st->stream_handle, st->state);

        /* 只做 Source：io_file→…→io_bt 推流管线 */
        bt_push_service_on_stream_state(st->stream_handle, st->state);

        /* 推流开始时把当前档位同步成数字增益：
           未推流期间调音量走的是本地 codec 路径，ALC 里存的可能是旧档位。 */
        if (st->state == ESP_BT_AUDIO_STREAM_STATE_ALLOCATED ||
            st->state == ESP_BT_AUDIO_STREAM_STATE_STARTED) {
            bt_push_service_set_volume(button_service_get_volume());
        }

        /* UI 层按 BOOL 语义收：只有真正 STARTED 才算"正在推流" */
        if (s_evt_cb) {
            bool on = (st->state == ESP_BT_AUDIO_STREAM_STATE_STARTED);
            s_evt_cb(on ? BT_EVT_STREAM_ON : BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "unhandled event %d", event);
        break;
    }
}

static void on_push_finished(void);

void bt_audio_service_init(bt_audio_evt_cb_t cb, void *ctx)
{
    s_evt_cb = cb;
    s_evt_ctx = ctx;
    bt_push_service_set_finished_cb(on_push_finished);
    ESP_LOGI(TAG, "service ready (power off)");
}

/* 开机早期调用：此时 LCD/WiFi/摄像头/GMF 都还没起，内部 RAM 最空最连续。 */
void bt_audio_service_controller_init(void)
{
    ESP_LOGI(TAG, "early controller init: free internal=%d largest=%d free PSRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s (BT disabled)", esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s (BT disabled)", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        return;
    }
    s_controller_ready = true;
    ESP_LOGI(TAG, "controller ready (host off)");
}

/* 开 host 前的内部 RAM 体检（每次打开都必须走，见头文件说明） */
static bool host_mem_sufficient(void)
{
    const size_t need_free    = BT_HOST_MIN_FREE_INTERNAL;
    const size_t need_largest = BT_HOST_MIN_LARGEST_BLOCK;

    size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (free_sz < need_free || largest < need_largest) {
        ESP_LOGE(TAG, "internal RAM too low for BT host: free=%u (need %u) "
                      "largest=%u (need %u) -- refuse to power on",
                 (unsigned)free_sz, (unsigned)need_free,
                 (unsigned)largest, (unsigned)need_largest);
        ESP_LOGE(TAG, "[diag] internal free=%u largest=%u | DMA-capable free=%u largest=%u | "
                      "PSRAM free=%u largest=%u",
                 (unsigned)free_sz, (unsigned)largest,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return false;
    }
    ESP_LOGI(TAG, "mem check ok: internal free=%u largest=%u (need %u/%u)",
             (unsigned)free_sz, (unsigned)largest,
             (unsigned)need_free, (unsigned)need_largest);
    return true;
}

esp_err_t bt_audio_service_set_power(bool on)
{
    if (on && s_on) return ESP_OK;
    if (!on && !s_on) return ESP_OK;

    if (on) {
        if (!s_controller_ready) {
            ESP_LOGE(TAG, "controller not ready (early init failed)");
            return ESP_ERR_INVALID_STATE;
        }
        /* 体检必须放在 esp_bt_audio_init() 之前（进去后再失败无法优雅退出） */
        if (!host_mem_sufficient()) {
            return ESP_ERR_NO_MEM;
        }
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_ESP_COEX_SW_COEXIST_ENABLE
        /* ★ 2026-11-01 真机实证：WiFi 开着时 A2DP 几乎连不上，关掉 WiFi 立刻就成功。
         *   WiFi 在反复连 ws://... 时会把 2.4G 时隙占满，AVDTP 握手（需要几轮
         *   L2CAP 往返）在争夺中被拖到超时，表现为 BTA_AV_OPEN_EVT FAILED。
         *   蓝牙打开期间把射频调度偏向 BT，保证 A2DP 建链与推流优先拿到时隙。 */
        esp_err_t coex_ret = esp_coex_preference_set(ESP_COEX_PREFER_BT);
        ESP_LOGI(TAG, "coex preference -> BT: %s", esp_err_to_name(coex_ret));
#endif
        /* Bluedroid host 配置：Just Works（IO_CAP_NONE），无需耳机端确认 */
        esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
        snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "%s", BT_DEV_NAME);
        host_cfg.iocap = ESP_BT_IO_CAP_NONE;

        esp_bt_audio_config_t cfg = {0};
        cfg.host_config = &host_cfg;
        cfg.event_cb = on_bt_audio_event;
        cfg.event_user_ctx = NULL;
        cfg.classic.roles = classic_roles();
        cfg.classic.a2dp_src_send_task_core_id = 1;
        cfg.classic.a2dp_src_send_task_prio = 10;
        cfg.classic.a2dp_src_send_task_stack_size = 4096;

        esp_err_t ret = esp_bt_audio_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_audio_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_on = true;
        ESP_LOGI(TAG, "ON, device name=%s, role=SOURCE", host_cfg.dev_name);
        if (s_evt_cb) s_evt_cb(BT_EVT_ON, NULL, s_evt_ctx);
    } else {
        esp_bt_audio_deinit();
        s_on = false;
        s_connected = false;
        s_connecting = false;
#if CONFIG_SW_COEXIST_ENABLE || CONFIG_ESP_COEX_SW_COEXIST_ENABLE
        /* 蓝牙关掉后把射频还给 WiFi（否则点歌/联网还会继续被 BT 优先压着） */
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
#endif
        ESP_LOGI(TAG, "OFF (controller stays resident)");
        if (s_evt_cb) s_evt_cb(BT_EVT_OFF, NULL, s_evt_ctx);
    }
    return ESP_OK;
}

bool bt_audio_service_is_on(void) { return s_on; }

esp_err_t bt_audio_service_start_discovery(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    s_dev_count = 0;   /* 新一轮扫描清表（读侧可能短暂看到 0 条） */
    ESP_LOGI(TAG, "start discovery");
    return esp_bt_audio_classic_discovery_start();
}

esp_err_t bt_audio_service_stop_discovery(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    return esp_bt_audio_classic_discovery_stop();
}

esp_err_t bt_audio_service_connect(const uint8_t *addr)
{
    if (!s_on || !addr) return ESP_ERR_INVALID_STATE;
    if (s_connected) {
        ESP_LOGW(TAG, "already connected, skip");
        return ESP_OK;
    }
    uint32_t now = bt_now_ms();
    if (s_connecting && (now - s_connecting_since_ms) < BT_CONNECTING_TIMEOUT_MS) {
        ESP_LOGW(TAG, "connect ignored: attempt already in flight (%u ms)",
                 (unsigned)(now - s_connecting_since_ms));
        return ESP_ERR_INVALID_STATE;
    }
    if ((now - s_last_connect_ms) < BT_CONNECT_MIN_GAP_MS) {
        ESP_LOGW(TAG, "connect throttled (%u ms since last attempt)",
                 (unsigned)(now - s_last_connect_ms));
        return ESP_ERR_INVALID_STATE;
    }
    s_last_connect_ms = now;
    s_connecting = true;
    s_connecting_since_ms = now;

    /* connect 的 role 参数要传单个 profile（SRC=0x1），不是 bitmask */
    ESP_LOGI(TAG, "connect SRC addr=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, (uint8_t *)addr);
    ESP_LOGI(TAG, "classic_connect ret=%s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        s_connecting = false;   /* 请求都没发出去，别占着"正在连" */
    }
    return ret;
}

esp_err_t bt_audio_service_disconnect(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;

    /* Source：先停 A2DP 流（media_stop 只支持 SRC）。
       ★ 2026-11-01：主线到这里就结束了（"断开"实际只是停流，ACL 链路还在）；
         本工程补上真正的 classic_disconnect —— 否则 SYS 页的 DISCONNECT 名不副实，
         而且下一条歌还会沿用同一条仍在的链路，用户感知不到"断开"。 */
    if (s_connected && bt_push_service_is_playing()) {
        esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    }

    esp_err_t ret = ESP_OK;
    if (s_connected && s_peer_valid) {
        ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, s_peer_addr);
        ESP_LOGI(TAG, "classic_disconnect ret=%s", esp_err_to_name(ret));
    }
    return ret;
}

int bt_audio_service_get_device_count(void)
{
    int n = s_dev_count;
    return (n > BT_MAX_DEVICES) ? BT_MAX_DEVICES : n;   /* 防并发读到中间值 */
}

const bt_device_t *bt_audio_service_get_device(int index)
{
    if (index < 0 || index >= bt_audio_service_get_device_count()) {
        return NULL;
    }
    return &s_devices[index];
}

/* ============================================================================
 * 推流自然结束的收尾
 * ============================================================================
 * 现象（主线原案）：整首播完后 GMF 打 "playback finished"，但 A2DP 流仍保持
 *   STARTED，设备端持续以为在播，UI 停在"暂停/继续"态。
 * 根因：bt_push_service 收到 FINISHED 只置标志，没停 A2DP 流。
 *
 * 修复：push 层 FINISHED/ERROR 时回调 here，由本函数做完整收尾。
 * ★ 回调运行在 GMF 事件线程，esp_bt_audio_media_stop() 是阻塞调用，
 *   直接调用会重入死锁，故起一个短命任务（栈外置 PSRAM）执行收尾。
 */
static void push_finish_cleanup_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));   /* 让 GMF 事件线程先退出当前事件 */

    ESP_LOGI(TAG, "auto-stop: stopping A2DP stream");
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    bt_push_service_clear_finished();

    if (s_evt_cb) {
        s_evt_cb(BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
    }
    if (s_push_finished_cb) {
        s_push_finished_cb();   /* 播放层负责把 BT 播完翻译成 MP_EVT_FINISHED */
    }

    vTaskDelete(NULL);
}

/* 由 bt_push_service 在 GMF 事件线程回调（不可阻塞！只看标志、起任务） */
static void on_push_finished(void)
{
    ESP_LOGI(TAG, "push finished -> schedule cleanup");
    BaseType_t ok = xTaskCreateWithCaps(push_finish_cleanup_task, "bt_push_stop",
                                        2048, NULL, 5, NULL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "cleanup task create failed; notify only");
        if (s_push_finished_cb) {
            s_push_finished_cb();
        }
    }
}

void bt_audio_service_set_push_finished_cb(void (*cb)(void))
{
    s_push_finished_cb = cb;
}

esp_err_t bt_audio_service_play_source(const char *path)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!path) return ESP_ERR_INVALID_ARG;

    /* uri 交给 push 服务暂存（等 ALLOCATED 事件拿到 stream 才真正下发） */
    esp_err_t err = bt_push_service_set_uri(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_uri failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t bt_audio_service_pause_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    /* 暂停只停 pipeline（保留 A2DP 链路），以便 resume 继续推同一首 */
    return bt_push_service_pause();
}

esp_err_t bt_audio_service_resume_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    return bt_push_service_resume();
}

esp_err_t bt_audio_service_stop_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    return bt_push_service_stop();
}

bool bt_audio_service_is_connected(void) { return s_connected; }

/* button_service 的档位上限（其 VOL_MAX 是 .c 里的私有宏，这里显式对齐）。
 * ★ 必须折算：数字增益按 0..100 满幅映射，若直接传 0..90，
 *   90 档算出来只有约 -0.9 dB，用户会感觉"音量没推满"。 */
#define BT_VOL_LOCAL_MAX   90

esp_err_t bt_audio_service_set_volume(int volume)
{
    if (!s_on) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只在真正推流时接管：否则返回 ESP_ERR_INVALID_STATE，
       让上层继续走本地 codec（扬声器播放 / 开机提示音的正确出口）。 */
    if (!s_connected || !bt_push_service_is_playing()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 本地 0..90 → 数字增益量纲 0..100（四舍五入） */
    int v100 = (volume * 100 + BT_VOL_LOCAL_MAX / 2) / BT_VOL_LOCAL_MAX;
    return bt_push_service_set_volume(v100);
}
