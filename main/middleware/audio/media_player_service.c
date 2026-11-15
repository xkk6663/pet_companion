/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file media_player_service.c
 * @brief 音乐播放服务实现（本地 esp_player 链路 + 蓝牙 A2DP 推流路由）
 *
 * LOCAL 链路照搬 tab_audio.c 的 LOCAL 模式（同芯片已验证）：
 *   media_lib 默认适配器 + extractor/audio_dec 注册
 *   → gmf pool（ch_cvt/bit_cvt/rate_cvt/alc 四元素）
 *   → esp_audio_render（out_writer 指向本服务）
 *   → esp_player（audio_render stream + 事件回调）
 * 与主线的唯一实质差异：out_writer 不自建 codec 句柄，改走
 * audio_service ext 流式协议（ext_claim/ext_write/ext_release）。
 *
 * BT 链路（A2DP Source）完全交给 bt_audio_service / bt_push_service：
 *   io_file(MP3) → aud_dec → aud_asrc → aud_alc → aud_enc(SBC) → io_bt
 *   不经过本地 codec，也不占用 audio_service ext。
 */

#include <string.h>
#include "sdkconfig.h"                 /* CONFIG_PET_MP_RATE_CVT_ENABLE */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "middleware/mem_diag.h"

#include "esp_player.h"
#include "esp_audio_render.h"
#include "esp_extractor_defaults.h"   /* esp_extractor_register_default */
#include "esp_audio_dec_default.h"    /* esp_audio_dec_register_default */
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "media_lib_adapter.h"

#include "media_player_service.h"
#include "audio_service.h"
#include "mic_gate.h"
#include "middleware/bluetooth/bt_audio_service.h"
#include "middleware/bluetooth/bt_push_service.h"

static const char *TAG = "MP_SVC";

/* ★ 2026-11-11 §29：rate_cvt 的 A/B 开关已搬到 menuconfig
 *   （main/Kconfig.projbuild → CONFIG_PET_MP_RATE_CVT_ENABLE，默认 y）。
 *   托管组件已全部恢复官方原版，本文件不再依赖任何组件补丁。
 *   设 n 只用于定位 task_wdt；非 44100 的歌会建链失败（不是变调），
 *   44100 的歌不受影响——官方 audio_render_optimize_pipeline() 在
 *   src==dest 时根本不创建 rate_cvt 元素。验完改回 y。 */

/* ---- 命令 ---- */
typedef enum {
    MP_CMD_PLAY = 0,     /* path 为待播文件 */
    MP_CMD_STOP,         /* 停止 + release */
    MP_CMD_FINISHED,     /* 本地播放器事件回调投递：自然播完/出错 → release + 上抛 */
    MP_CMD_PAUSE,
    MP_CMD_RESUME,
    MP_CMD_SEEK,         /* value = 目标毫秒 */
    MP_CMD_BT_FINISHED,  /* 蓝牙推流自然播完（bt_audio_service 回调投递） */
    MP_CMD_SET_OUT,      /* value = mp_out_mode_t：切换输出后按需热切换当前曲目 */
} mp_cmd_id_t;

typedef struct {
    mp_cmd_id_t id;
    int         value;
    char        path[256];
} mp_cmd_t;

/* ---- 状态 ---- */
static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_task;
static mp_evt_cb_t   s_evt_cb;

static esp_player_handle_t            s_player;
static esp_audio_render_handle_t      s_render;
static esp_gmf_pool_handle_t          s_pool;
static bool                           s_media_registered;
static volatile bool                  s_playing;
/* ★ pending 段：play() 入队成功到 mp_task 真正把 s_playing 置位之间有 ~50ms
 *   建链窗口（ext_claim + pool/render/player 惰性重建 + run）。这段窗口里
 *   is_playing() 若只看 s_playing，UI 轮询会误判"已结束"而发出误杀 STOP
 *   （2026-09-26 实证）。is_playing() = s_playing || s_play_pending。 */
static volatile bool                  s_play_pending;
/* 播放器事件回调（播放器任务上下文）只置标志；FINISHED 的收尾
 * （esp_player_stop 后置 + ext_release）统一由服务任务在 MP_CMD_FINISHED 做，
 * 避免在播放器自己的回调里调它的 stop（重入风险）。 */
static volatile bool                  s_player_finished;

/* ---- 路由 / 蓝牙状态 ---- */
static volatile mp_out_mode_t         s_out_mode = MP_OUT_AUTO;
static volatile bool                  s_bt_playing;   /* 当前曲目走蓝牙推流 */
static volatile bool                  s_paused;
/* 最近一次 PLAY 的路径（输出模式热切换时按新路由重播这一首） */
static char                           s_cur_path[256];

static const audio_ext_fmt_t MP_OUT_FMT = {
    .sample_rate = 44100, .channels = 2, .bits = 16,
};

/* ★ 2026-11-10 修"切歌后点播放不出声"：ext STREAM 占用的**真实持有权**。
 *   audio_service 里的 s_ext_mode 是全局单槽、没有 owner 概念 —— 谁 claim 的
 *   就只能在同一个客户端 release。而 do_play() 在"正在播时再次 PLAY"这条路上
 *   先 `esp_player_stop` 把 s_playing 置 false，接着又去 ext_claim ⇒ 撞自己
 *   上次的占用，`ext_claim BUSY (mode=1)` ⇒ `play refused: audio_service busy`。
 *   用一个本地标志记住"这占用是我自己拿的"，重播时直接复用，不再 claim。
 *   任何 release 路径都必须同步清它（统一走 mp_ext_release()）。 */
static volatile bool s_ext_held;

/* ---- 前向 ---- */
static int  mp_link_setup(void);
static void mp_link_teardown(void);
static void mp_ext_release(void);

/* 归还 ext 流式占用（幂等；未持有时无操作） */
static void mp_ext_release(void)
{
    if (s_ext_held) {
        audio_service_ext_release();
        s_ext_held = false;
    }
}

static void notify_evt(mp_evt_id_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt);
    }
}

/* ============================================================
 * esp_player 链路（LOCAL）
 * ============================================================ */

/* render 输出回调（播放器管线任务上下文）：统一走 ext 流式协议。
 * ★ 返回值契约是 **esp_err_t 语义（0=成功，非 0=错误码）**，不是"写入字节数"！
 *   返回正数字节数会被当成错误码 → PREPARING->PLAYING 一瞬间就 ERROR。 */
static int mp_out_write(uint8_t *data, uint32_t size, void *ctx)
{
    (void)ctx;
    if (!data || !size) {
        return -1;
    }
    return audio_service_ext_write(data, size);
}

static void register_media_defaults(void)
{
    if (s_media_registered) {
        return;
    }
    media_lib_add_default_adapter();       /* file/http IO 适配 */
    esp_extractor_register_default();      /* mp3/m4a/flac 容器解析 */
    esp_audio_dec_register_default();      /* mp3/aac/flac 解码器 */
    s_media_registered = true;
}

static int init_gmf_pool(void)
{
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "gmf pool init failed");
        return -1;
    }
    esp_gmf_element_handle_t el = NULL;

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
#ifdef CONFIG_PET_MP_RATE_CVT_ENABLE
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    /* DEFAULT_ESP_GMF_RATE_CVT_CONFIG() 是 44100→48000，与本服务
     * out_sample_info(44100) 不一致，pipeline 建立时会白白触发一次
     * need_reopen。显式对齐；源采样率仍由上游 sound info 事件覆盖，
     * 非 44100 的歌曲 reopen 后照常重采样。
     * complexity 取 1：非等采样率时把重采样 CPU 开销压到最低。 */
    rate_cfg.src_rate   = MP_OUT_FMT.sample_rate;
    rate_cfg.dest_rate  = MP_OUT_FMT.sample_rate;
    rate_cfg.complexity = 1;
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    ESP_LOGI(TAG, "gmf pool: rate_cvt=ON dest=%d complexity=%d",
             MP_OUT_FMT.sample_rate, rate_cfg.complexity);
#else
    (void)el;
    ESP_LOGW(TAG, "gmf pool: rate_cvt=OFF (A/B diag only; non-44100 songs will FAIL to build pipeline)");
#endif
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    return 0;
}

static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg) {
        return ESP_PLAYER_ERR_OK;
    }
    if (msg->event_type == ESP_PLAYER_EVENT_FINISHED ||
        msg->event_type == ESP_PLAYER_EVENT_ERROR) {
        if (msg->event_type == ESP_PLAYER_EVENT_ERROR) {
            ESP_LOGE(TAG, "player error event");
        }
        s_player_finished = true;
        mp_cmd_t cmd = { .id = MP_CMD_FINISHED };
        if (s_cmd_q) {
            xQueueSend(s_cmd_q, &cmd, 0);
        }
    }
    return ESP_PLAYER_ERR_OK;
}

static int mp_link_setup(void)
{
    register_media_defaults();
    if (init_gmf_pool() != 0) {
        return -1;
    }

    esp_audio_render_cfg_t render_cfg = {
        .max_stream_num = 1,
        .out_writer = mp_out_write,
        .out_ctx = NULL,                    /* 写入目标由 ext 协议决定 */
        .out_sample_info = {
            .sample_rate = MP_OUT_FMT.sample_rate,
            .channel = MP_OUT_FMT.channels,
            .bits_per_sample = MP_OUT_FMT.bits,
        },
        .pool = s_pool,
        .process_period = 20,
        .process_buf_align = 16,
    };
    if (esp_audio_render_create(&render_cfg, &s_render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "audio_render create failed");
        return -1;
    }

    esp_audio_render_stream_handle_t stream = NULL;
    if (esp_audio_render_stream_get(s_render, ESP_AUDIO_RENDER_FIRST_STREAM,
                                    &stream) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "render stream get failed");
        return -1;
    }

    esp_player_config_t player_cfg = ESP_PLAYER_CONFIG_DEFAULT();
    player_cfg.audio_render_hd = stream;
    player_cfg.video_render_hd = NULL;

    if (esp_player_init(&player_cfg, &s_player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player init failed");
        return -1;
    }
    esp_player_set_event_cb(s_player, player_event_cb, NULL);
    ESP_LOGI(TAG, "local player link ready");
    return 0;
}

static void mp_link_teardown(void)
{
    if (s_player) {
        esp_player_stop(s_player);
        esp_player_deinit(s_player);
        s_player = NULL;
    }
    if (s_render) {
        esp_audio_render_destroy(s_render);
        s_render = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
}

/* ============================================================
 * 服务任务
 * ============================================================ */

/* 本曲是否应走蓝牙（在 PLAY 时刻决策一次） */
static bool route_to_bt(void)
{
    if (s_out_mode == MP_OUT_BT) {
        return true;
    }
    if (s_out_mode == MP_OUT_AUTO) {
        return bt_audio_service_is_on() && bt_audio_service_is_connected();
    }
    return false;
}

static void do_play(const char *path, bool use_bt)
{
    /* 记下本曲路径：OUT 三态在播中途切换时据此重播（热切换，见 MP_CMD_SET_OUT） */
    size_t plen = strlen(path);
    if (plen < sizeof(s_cur_path)) {
        memcpy(s_cur_path, path, plen + 1);
    } else {
        s_cur_path[0] = '\0';
    }

    if (use_bt) {
        if (!bt_audio_service_is_on() || !bt_audio_service_is_connected()) {
            ESP_LOGW(TAG, "BT route unavailable (on=%d connected=%d)",
                     (int)bt_audio_service_is_on(), (int)bt_audio_service_is_connected());
            s_play_pending = false;
            notify_evt(MP_EVT_ERROR);
            return;
        }
        esp_err_t err = bt_audio_service_play_source(path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bt play failed: %s", esp_err_to_name(err));
            s_play_pending = false;
            notify_evt(MP_EVT_ERROR);
            return;
        }
        s_bt_playing = true;
        s_paused = false;
        s_play_pending = false;
        ESP_LOGI(TAG, "playing via BT: %s", path);
        notify_evt(MP_EVT_STARTED);
        return;
    }

    /* ---- LOCAL ---- */
    if (s_playing) {
        /* 重复 PLAY：先停当前，ext 占用**保留**给这一首个继续用 */
        esp_player_stop(s_player);
        s_playing = false;
    }
    /* ext 流式占用：已持有则复用（切歌/重播）；未持有才 claim，
     * 失败 = audio_service 忙（WAV/录音/通话/小智） */
    if (!s_ext_held) {
        if (audio_service_ext_claim(&MP_OUT_FMT) != ESP_OK) {
            ESP_LOGW(TAG, "play refused: audio_service busy");
            s_play_pending = false;
            notify_evt(MP_EVT_ERROR);
            return;
        }
        s_ext_held = true;
    }
    if (!s_player && mp_link_setup() != 0) {
        mp_ext_release();
        s_play_pending = false;
        notify_evt(MP_EVT_ERROR);
        return;
    }
    s_player_finished = false;
    {
        esp_player_data_src_t src =
            ESP_PLAYER_DATA_SRC(path, ESP_PLAYER_MASK_AUDIO);
        esp_player_set_data_src(s_player, &src);
    }
    if (esp_player_run(s_player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player run failed: %s", path);
        esp_player_stop(s_player);
        mp_ext_release();
        s_play_pending = false;
        notify_evt(MP_EVT_ERROR);
        return;
    }
    s_playing = true;
    s_paused = false;
    s_play_pending = false;
    ESP_LOGI(TAG, "playing via LOCAL: %s", path);
    notify_evt(MP_EVT_STARTED);
}

static void do_stop(void)
{
    if (s_bt_playing) {
        /* ★ host 已被关掉（蓝牙电源 OFF）时再调 media_stop 会打到已 deinit 的
         *   host 上，所以只在链路仍在时才停流；状态一律清干净。 */
        if (bt_audio_service_is_connected()) {
            bt_audio_service_stop_source();
        }
        s_bt_playing = false;
        s_paused = false;
        s_play_pending = false;
        ESP_LOGI(TAG, "stopped (BT)");
        notify_evt(MP_EVT_FINISHED);
        return;
    }
    if (s_playing) {
        esp_player_stop(s_player);
        s_playing = false;
        mp_ext_release();
        ESP_LOGI(TAG, "stopped by request");
    } else if (s_ext_held) {
        /* 状态错位兜底：播放器已停但 ext 还占着 ⇒ 这是"切歌后点播放被 BUSY"
         * 的残留态，停一次就把它收干净。 */
        ESP_LOGW(TAG, "stopped: release leaked ext claim");
        mp_ext_release();
    }
    s_paused = false;
    s_play_pending = false;
    /* ★ 全拆 GMF 链路（pool/render/player）：链路在 PLAY 时惰性重建。 */
    mp_link_teardown();
    notify_evt(MP_EVT_FINISHED);
}

static void mp_task(void *arg)
{
    static mp_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(500)) != pdPASS) {
            /* ★ 链路消失兜底：对端断开或蓝牙电源被关时 host 已 deinit，
             *   不会再有 BT_FINISHED；不清状态 UI 会永远显示"播放中"。
             *   只清状态、不调任何 BT API（此时调用会打到已释放的 host）。 */
            if (s_bt_playing && !bt_audio_service_is_connected()) {
                s_bt_playing = false;
                s_paused = false;
                s_play_pending = false;
                ESP_LOGW(TAG, "BT link lost -> playback aborted");
                notify_evt(MP_EVT_FINISHED);
            }
            continue;
        }
        switch (cmd.id) {
        case MP_CMD_PLAY:
            do_play(cmd.path, route_to_bt());
            break;

        case MP_CMD_STOP:
            do_stop();
            break;

        case MP_CMD_FINISHED:
            if (s_bt_playing) {
                /* 不应该发生（BT 走 MP_CMD_BT_FINISHED），防御性清理 */
                s_bt_playing = false;
            }
            if (s_playing) {
                /* 播放器已自行停止（FINISHED/ERROR 事件），收尾归还 */
                esp_player_stop(s_player);
                s_playing = false;
                mp_ext_release();
                ESP_LOGI(TAG, "finished");
            } else if (s_ext_held) {
                ESP_LOGW(TAG, "finished: release leaked ext claim");
                mp_ext_release();
            }
            s_paused = false;
            s_play_pending = false;
            mp_link_teardown();
            notify_evt(MP_EVT_FINISHED);
            break;

        case MP_CMD_BT_FINISHED:
            if (s_bt_playing) {
                s_bt_playing = false;
                s_paused = false;
                s_play_pending = false;
                ESP_LOGI(TAG, "finished (BT)");
                notify_evt(MP_EVT_FINISHED);
            }
            break;

        case MP_CMD_PAUSE:
            if (s_bt_playing) {
                if (bt_audio_service_pause_source() == ESP_OK) {
                    s_paused = true;
                }
            } else if (s_playing) {
                if (esp_player_pause(s_player) == ESP_PLAYER_ERR_OK) {
                    s_paused = true;
                }
            }
            break;

        case MP_CMD_RESUME:
            if (s_bt_playing) {
                if (bt_audio_service_resume_source() == ESP_OK) {
                    s_paused = false;
                }
            } else if (s_playing) {
                if (esp_player_resume(s_player) == ESP_PLAYER_ERR_OK) {
                    s_paused = false;
                }
            }
            break;

        case MP_CMD_SEEK:
            if (cmd.value < 0) {
                break;
            }
            if (s_bt_playing) {
                /* BT 侧没有播放器句柄，进度由 bt_push_service 的墙钟推算；
                 * seek 走它的命令队列（在推送处理任务里换 io 读位置）。 */
                if (bt_push_service_seek_ms(cmd.value) != ESP_OK) {
                    ESP_LOGW(TAG, "bt seek to %d ms failed", cmd.value);
                }
            } else if (s_playing) {
                if (esp_player_seek(s_player, (uint64_t)cmd.value) != ESP_PLAYER_ERR_OK) {
                    ESP_LOGW(TAG, "seek to %d ms failed", cmd.value);
                }
            }
            break;

        case MP_CMD_SET_OUT: {
            /* ★ 手动切换出口：正在播就把当前曲目换到新链路上重播（"热切换"）。
             *   两条链路都没有 seek 能力对齐播放点（BT 侧拿不到对端进度），
             *   所以统一从头开始 —— 这是可预期且不会两个出口同时出声的做法。 */
            s_out_mode = (mp_out_mode_t)cmd.value;
            bool want_bt = route_to_bt();
            bool playing = s_playing || s_bt_playing || s_play_pending;
            if (playing && s_cur_path[0] && want_bt != s_bt_playing) {
                ESP_LOGI(TAG, "hot switch output -> %s", want_bt ? "BT" : "LOCAL");
                do_stop();
                do_play(s_cur_path, want_bt);
            } else {
                ESP_LOGI(TAG, "output mode -> %d (applies on next play)", (int)s_out_mode);
            }
            break;
        }
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

/* 蓝牙推流自然结束（在 bt_audio_service 收尾任务上下文）→ 投递命令 */
static void mp_bt_finished_cb(void)
{
    mp_cmd_t cmd = { .id = MP_CMD_BT_FINISHED };
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

void media_player_service_init(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(mp_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(4, mp_cmd_t)", 4 * sizeof(mp_cmd_t));
    BaseType_t ok = xTaskCreate(mp_task, "mp_task", 3072, NULL, 5, &s_task);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(mp_task, 3072)", 3072);
    bt_audio_service_set_push_finished_cb(mp_bt_finished_cb);
    ESP_LOGI(TAG, "media_player_service ready (link lazy-init on first play)");
}

esp_err_t media_player_service_play(const char *path)
{
    if (!path || !path[0] || !s_cmd_q) {
        return ESP_ERR_INVALID_ARG;
    }
    /* S10 P1：播放要独占 spk，先让监听态收口（默认关闭时是空操作） */
    (void)mic_gate_yield(300);
    mic_gate_block(5000);        /* 给 mp_task 留出 claim 的窗口 */
    mp_cmd_t cmd = { .id = MP_CMD_PLAY };
    if (strlen(path) >= sizeof(cmd.path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(cmd.path, path);
    /* 非阻塞投递；结果通过事件回调（STARTED/ERROR）上抛 */
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_INVALID_STATE;
    }
    s_play_pending = true;   /* ★ 入队即算"播放中"，堵死建链窗口的误杀竞态 */
    return ESP_OK;
}

void media_player_service_stop(void)
{
    mp_cmd_t cmd = { .id = MP_CMD_STOP };
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

esp_err_t media_player_service_pause(void)
{
    mp_cmd_t cmd = { .id = MP_CMD_PAUSE };
    if (!s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_q, &cmd, 0) == pdPASS ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t media_player_service_resume(void)
{
    mp_cmd_t cmd = { .id = MP_CMD_RESUME };
    if (!s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_q, &cmd, 0) == pdPASS ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t media_player_service_seek(int ms)
{
    if (ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_cmd_t cmd = { .id = MP_CMD_SEEK, .value = ms };
    if (!s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_q, &cmd, 0) == pdPASS ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool media_player_service_is_paused(void)
{
    return s_paused;
}

int media_player_service_get_position_ms(void)
{
    /* ★ BT 链路没有 esp_player 句柄，进度由 bt_push_service 按推流墙钟推算；
     *   否则这里会一直返回 0，UI 进度条与歌词就"卡在第 0 秒"。 */
    if (s_bt_playing) {
        return bt_push_service_get_position_ms();
    }
    if (!s_player) {
        return 0;
    }
    uint64_t pos = 0;
    if (esp_player_get_play_time(s_player, &pos) != ESP_PLAYER_ERR_OK) {
        return 0;
    }
    return (int)pos;
}

int media_player_service_get_duration_ms(void)
{
    if (s_bt_playing) {
        return bt_push_service_get_duration_ms();
    }
    if (!s_player) {
        return 0;
    }
    uint64_t dur = 0;
    if (esp_player_get_duration(s_player, &dur) != ESP_PLAYER_ERR_OK) {
        return 0;
    }
    return (int)dur;
}

bool media_player_service_is_playing(void)
{
    /* ★ 含 pending 段：见 s_play_pending 注释（UI 轮询防误杀竞态） */
    return s_playing || s_bt_playing || s_play_pending;
}

void media_player_service_set_output_mode(mp_out_mode_t mode)
{
    /* 立即写入（get_output_mode 要能马上反映按钮状态），再投递命令让服务任务
     * 判断是否需要把当前曲目热切换到新链路。 */
    s_out_mode = mode;
    mp_cmd_t cmd = { .id = MP_CMD_SET_OUT, .value = (int)mode };
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
    ESP_LOGI(TAG, "output mode request -> %d", (int)mode);
}

mp_out_mode_t media_player_service_get_output_mode(void)
{
    return s_out_mode;
}

void media_player_service_set_evt_cb(mp_evt_cb_t cb)
{
    s_evt_cb = cb;
}
