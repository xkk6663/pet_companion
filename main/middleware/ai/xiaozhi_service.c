/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file xiaozhi_service.c
 * @brief 桌宠会话服务实现（S9 P1~P3：WebSocket 网关 + PCM 上行 + URL 下行）
 *
 * 与改造前的差异（每一处都能在 `S9_桌宠伴侣_可执行实施计划.md` §2.6 找到依据）：
 *   ✗ 删  §53 会话聋化自愈（8s 无回音重开会话）—— 无状态长连接不存在这种病；
 *   ✗ 删  §52 barge-in 的 abort_speaking 两段式 + 欠票（s_ptt_start_pending）；
 *   ✗ 删  xiaozhi.me 激活检查、wake word 发送；
 *   ✗ 删  Opus feeder（下行语音变成 URL 直连播放，见下）；
 *   ✗ 删  audio_processor_mixer（只剩一路 playback，无需混音/优先级仲裁）；
 *   ✓ 留  状态机、命令队列、ext 独占租借、PTT 期望态、音量、事件枚举。
 *
 * ★ 下行语音为什么走 audio_playback 而不是 feeder：
 *   云端网关把 TTS 落成 `/static/tts_*.wav` 并只下发 URL（云端文档 §5.3）。
 *   `audio_playback_play(handle, url)` 内部是 esp_audio_simple_player ——
 *   HTTP 拉流 + wav/mp3 解码内建，且已在 S8 点歌链路真机跑通
 *   （`xiaozhi_service.c:957` 历史位置）。**不需要 minimp3**。
 *
 * ★ 为什么 TTS 与点歌共用一个 playback 句柄（裁决 4-B）：
 *   计划文档首选 A（再开一个 s_tts 并进 mixer）。但实测前提（第二个 playback
 *   与 mixer/策略共存）未经验证，而 R10 明确写了"失败回退 B"。v1 直接取 B：
 *   零新增句柄、零混音内存。代价 = TTS 会打断正在播的歌（换曲语义），
 *   对"伴侣说话优先"的桌宠定位反而合理。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "middleware/mem_diag.h"

#include "audio_processor.h"        /* av_processor: audio_manager/recorder/playback */
#include "esp_codec_dev.h"

#include "xiaozhi_service.h"
#include "ws_client.h"
#include "voice_service.h"
#include "event_bus.h"
#include "ai_light.h"
#include "ai_speaker.h"
#include "ai_camera.h"
#include "ai_device.h"
#include "ai_punch.h"
#include "ai_action.h"
#include "ai_music.h"
#include "music_source.h"
#include "middleware/audio/media_player_service.h"  /* 反向抢占：起会话前停歌 */
#include "middleware/periph/safe_reboot.h"
#include "audio_service.h"

static const char *TAG = "PET_SVC";

/* ---- 采集/播放格式（与改造前完全一致，别再动：I2S 时钟共享的连锁约束）----
 * play_io 16k/2ch 是**下行锚点**（简单播放器统一转成它），rec 声明 16k/1ch
 * 但物理 RX 是 2ch（同一 I2S 控制器 TX 立体声陪跑）⇒ read_cb 取 L 通道。 */
#define XZ_MIC_LAYOUT       "MM"
#define XZ_BOARD_CHANNELS   1
#define XZ_SAMPLE_RATE      16000
#define XZ_BITS             16
#define XZ_FRAME_MS         60
#define XZ_PLAY_RATE        16000
#define XZ_PLAY_CHANNELS    2

/* 上行读块：60ms @16k/16bit/mono = 1920B，这里给 2 倍余量 */
#define XZ_REC_READ_SIZE    (1920 * 2)

/* 下行播放优先级（只剩一路，保留数值只为日志可读） */
#define XZ_TTS_PRIORITY     2
#define XZ_MUSIC_PRIORITY   1

/* 播放 URL 常驻缓冲（av_processor 内部仍引用字符串，必须收编） */
#define XZ_URL_MAX          512

/* 空闲自动收会话：60s 无 PTT / 无播放 / 无下行事件 ⇒ 归还 codec
 * （否则录音/MP3 等需要 codec 的功能会被永久占死）。
 * ★ S9 token 纪律：会话本身不计费，但它是上行的前提；用户按住说完一句就
 *   松手的场景占多数，60s 足够容纳"连续追问"，又不会让 codec 长期被占。 */
#define XZ_IDLE_CLOSE_S     60

/* ---- 命令 ---- */
typedef enum {
    XZ_CMD_START = 0,
    XZ_CMD_STOP,
    XZ_CMD_PLAY,        /* url = PSRAM 堆串（xz_task 消费后释放） */
    XZ_CMD_STOP_PLAY,
} xz_cmd_id_t;

/* 播放内容类别：决定事件怎么上抛（TTS 要驱动嘴型，音乐不要） */
typedef enum {
    XZ_KIND_NONE = 0,
    XZ_KIND_TTS,
    XZ_KIND_MUSIC,
} xz_kind_t;

typedef struct {
    xz_cmd_id_t id;
    char       *url;
    uint8_t     kind;
} xz_cmd_t;

/* ---- 状态 ---- */
static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_task;
static xiaozhi_evt_cb_t s_evt_cb;
static volatile xiaozhi_state_t s_state = XZ_STATE_IDLE;

static volatile bool s_link;         /* 网关已连上 */
static volatile bool s_tts_active;   /* TTS 正在播（嘴型 + 闸门） */
static volatile bool s_music_playing;
static volatile bool s_ptt_want;     /* UI 期望态 */
static volatile bool s_ptt_on;       /* 上行任务已对齐态 */
static volatile bool s_uplink_sending;   /* 上行任务正在发大帧（teardown 用） */

static uint8_t      *s_uplink_buf;   /* PSRAM 读缓冲 */
static int16_t      *s_rec_scratch;  /* PSRAM 取 L 通道暂存 */

static audio_recorder_handle_t s_recorder;
static audio_play_handle_t     s_play;      /* 通道①：TTS（priority 2） */
static audio_play_handle_t     s_music_h;   /* 通道②：点歌（priority 1） */
static esp_codec_dev_handle_t  s_spk;

/* ★ MCP 引擎实例提到文件级：ai_action.c 要用它做进程内 JSON-RPC
 *   （caps 取自 tools/list、动作走 tools/call）。以前是 tools_attach_once()
 *   里的 static 局部变量，外面拿不到。 */
static esp_mcp_t *s_mcp;
/* 最近一次成功设置的音量（-1 = 没设过）。仅给 /api/system/status 查询用，
 * 写方是 httpd / event_bus，读方是 httpd —— 单任务串行，无锁必要。 */
static int s_volume = -1;
static esp_codec_dev_handle_t  s_mic;

static char s_play_url[XZ_URL_MAX];      /* TTS 通道当前 URL（av_processor 仍引用） */
static char s_music_url[XZ_URL_MAX];     /* 音乐通道当前 URL（两条通道不能共用缓冲） */
static xz_music_evt_cb_t s_music_evt_cb;

/* 会话不在时云端推来的 TTS：挂起一条，连上后自动播 */
static char s_pending_url[XZ_URL_MAX];
static volatile uint8_t s_pending_kind;

/* 空闲看护 */
static volatile TickType_t s_last_act;

static TaskHandle_t s_uplink_task;

/* ============================================================
 * 事件上抛
 * ============================================================ */

static void notify(xiaozhi_evt_id_t id, const char *text, int role, esp_err_t err)
{
    s_last_act = xTaskGetTickCount();
    xiaozhi_evt_cb_t cb = s_evt_cb;
    if (cb) {
        xiaozhi_evt_t evt = { .id = id, .text = text, .role = role, .err = err };
        cb(&evt);
    }
}

/* event_bus 的回调是"结构体指针"形式（总线侧统一一种签名），
 * 本文件内部一律用拆开的 4 参形式 ⇒ 这里做唯一一处适配。 */
static void on_bus_evt(const xiaozhi_evt_t *e)
{
    if (e) {
        notify(e->id, e->text, e->role, e->err);
    }
}

/* §73.19 / §59b：工具（打卡/相机/点歌）的结果也走同一条"助手说话"通道 */
static void on_tool_result(const char *text)
{
    notify(XZ_EVT_TEXT, text, 1, ESP_OK);
}

static void set_state(xiaozhi_state_t st)
{
    s_state = st;
    notify(XZ_EVT_STATE, NULL, 0, ESP_OK);
}

/* ============================================================
 * av_processor 设备 IO（与改造前逐字一致）
 * ============================================================ */

static esp_err_t xz_play_write_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_write(s_spk, data, size) >= 0) ? ESP_OK : ESP_FAIL;
}

#define XZ_REC_CHUNK_SAMPLES  2048
#define XZ_REC_SCRATCH_BYTES  (XZ_REC_CHUNK_SAMPLES * XZ_PLAY_CHANNELS * (XZ_BITS / 8))
static TickType_t s_rec_last_tick;
static uint32_t   s_rec_xrun_risk;

static esp_err_t xz_rec_read_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_mic == NULL || s_rec_scratch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t now = xTaskGetTickCount();
    int block_ms = (size >> 1) * 1000 / XZ_SAMPLE_RATE;
    if (s_rec_last_tick != 0 &&
        (now - s_rec_last_tick) > pdMS_TO_TICKS(block_ms + 15)) {
        s_rec_xrun_risk++;
        ESP_LOGW(TAG, "rec read gap %dms > %dms, xrun risk #%u",
                 (int)(now - s_rec_last_tick) * portTICK_PERIOD_MS,
                 block_ms + 15, (unsigned)s_rec_xrun_risk);
    }
    s_rec_last_tick = now;
    int out_total = size >> 1;
    int done = 0;
    while (done < out_total) {
        int chunk = out_total - done;
        if (chunk > XZ_REC_CHUNK_SAMPLES) {
            chunk = XZ_REC_CHUNK_SAMPLES;
        }
        int rd = esp_codec_dev_read(s_mic, s_rec_scratch,
                                    chunk * XZ_PLAY_CHANNELS * (XZ_BITS / 8));
        if (rd < 0) {
            return ESP_FAIL;
        }
        int16_t *out = (int16_t *)data + done;
        for (int i = 0; i < chunk; i++) {
            out[i] = s_rec_scratch[i * XZ_PLAY_CHANNELS];   /* L = mic0 */
        }
        done += chunk;
    }
    return ESP_OK;
}

/* ============================================================
 * 两条播放通道（★ 双 handle，靠 SDK 的 audio policy 做"暂停-续播"）
 *
 * 为什么不能共用一个 handle：`audio_play_internal()` 里对
 * `state == PLAYING` 的处理是**先 stop 再 play**（`audio_processor.c:1899`），
 * 所以 TTS 一来音乐必被砍掉，且回不去 —— 与音频来自 http 还是 SD 无关。
 *
 * 现在：TTS handle priority=2、音乐 handle priority=1。高优先级 play() 会
 * `audio_policy_pause_lower_priority_players()`（`:1906`）把音乐**暂停**并保留
 * 播放位置；TTS 的 STOPPED/FINISHED/ERROR 事件触发
 * `audio_policy_resume_paused_players()`（`:1022`）自动从原位置续播。
 * 输出端还有 `audio_policy_can_player_output()` 兜底，保证同一时刻只有一路
 * 真正写 codec。
 *
 * 事件回调按 handle 分开：TTS 只驱动嘴型/闸门，音乐只驱动点歌结论 ——
 * 绝不共用一套标志（原来靠 s_play_kind 猜，双通道后会互相踩）。
 * ============================================================ */

/* TTS 通道事件（simple_player 任务上下文：只置标志 + 转投回调） */
static void xz_tts_event_cb(audio_play_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case AUDIO_PLAY_STATE_PLAYING:
        s_tts_active = true;
        s_last_act = xTaskGetTickCount();
        ESP_LOGI(TAG, "play tts PLAYING");
        notify(XZ_EVT_TTS_STATE, NULL, 1, ESP_OK);          /* role=1 start */
        break;
    case AUDIO_PLAY_STATE_FINISHED:
    case AUDIO_PLAY_STATE_STOPPED:
    case AUDIO_PLAY_STATE_ERROR: {
        bool err = (state == AUDIO_PLAY_STATE_ERROR);
        bool fin = (state == AUDIO_PLAY_STATE_FINISHED);
        s_tts_active = false;
        s_last_act = xTaskGetTickCount();
        ESP_LOGI(TAG, "play tts %s", err ? "ERROR" : (fin ? "FINISHED" : "STOPPED"));
        notify(XZ_EVT_TTS_STATE, NULL, 0, err ? ESP_FAIL : ESP_OK);  /* role=0 stop */
        break;
    }
    default:
        break;
    }
}

/* 音乐通道事件
 * ★ PAUSED 是 policy 抢焦点造成的"暂时让位"，**不能**当成音乐结束：
 *   不清 s_music_playing、不发 XZ_AUDIO_STOPPED_EVT，否则上层会以为歌播完了。 */
static void xz_music_event_cb(audio_play_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case AUDIO_PLAY_STATE_PLAYING:
        s_music_playing = true;
        s_last_act = xTaskGetTickCount();
        ESP_LOGI(TAG, "play music PLAYING");
        if (s_music_evt_cb) {
            s_music_evt_cb(XZ_AUDIO_STARTED, ESP_OK);
        }
        break;
    case AUDIO_PLAY_STATE_PAUSED:
        /* TTS 插话：让位但保留位置，等 TTS 结束由 SDK 自动 resume */
        ESP_LOGI(TAG, "play music PAUSED (ducked by tts)");
        break;
    case AUDIO_PLAY_STATE_FINISHED:
    case AUDIO_PLAY_STATE_STOPPED:
    case AUDIO_PLAY_STATE_ERROR: {
        bool err = (state == AUDIO_PLAY_STATE_ERROR);
        bool fin = (state == AUDIO_PLAY_STATE_FINISHED);
        s_music_playing = false;
        s_last_act = xTaskGetTickCount();
        ESP_LOGI(TAG, "play music %s", err ? "ERROR" : (fin ? "FINISHED" : "STOPPED"));
        if (s_music_evt_cb) {
            s_music_evt_cb(err ? XZ_AUDIO_ERROR
                               : (fin ? XZ_AUDIO_FINISHED : XZ_AUDIO_STOPPED_EVT),
                           err ? ESP_FAIL : ESP_OK);
        }
        break;
    }
    default:
        break;
    }
}

/* ============================================================
 * 网关连接/收包
 * ============================================================ */

static void queue_play(const char *url, xz_kind_t kind);

/* 云端推来的 TTS 在"codec 还没借出来"时只能挂起，等会话可用再补播。
 * 两个入口都要调：① 刚连上（state_cb）② 连接**早就**在（WS 常连），
 * 此时 state_cb 不会再回调 —— 必须在 session_setup 尾部自己查一次。 */
static void flush_pending_audio(void)
{
    if (s_pending_url[0]) {
        ESP_LOGI(TAG, "flush pending audio: %.48s", s_pending_url);
        queue_play(s_pending_url, (xz_kind_t)s_pending_kind);
        s_pending_url[0] = '\0';
    }
}

static void on_ws_state(bool connected, void *ctx)
{
    (void)ctx;
    s_link = connected;
    notify(XZ_EVT_LINK, connected ? "online" : "offline", connected ? 1 : 0, ESP_OK);
    if (connected) {
        /* 每条新连接都重报能力表：网关重启、固件升级后两边自动对齐，
         * 不必人工同步工具清单。★ 只入队，不在事件任务里等。 */
        ai_action_report_caps();
    }
    if (connected && s_state == XZ_STATE_PREPARING) {
        set_state(XZ_STATE_CHATTING);   /* 管线已就绪 + 连接就绪 = 会话可用 */
        flush_pending_audio();
    }
}

/* 收包在 WS 任务上下文：event_bus 只做 cJSON 解析 + 投队列/转发 */
static void on_ws_line(const char *line, void *ctx)
{
    (void)ctx;
    event_bus_on_line(line);
}

/* ============================================================
 * 上行任务：recorder(PCM) → PTT 累积 → base64 上行
 * ★ 生命周期：**不自退**（read_data 在 fifo 上 portMAX_DELAY 阻塞），
 *   统一由 session_teardown 强制删除（改造前 vocat 已验证的顺序）。
 * ============================================================ */

static void xz_uplink_task(void *arg)
{
    (void)arg;
    int consec_err = 0;
    uint32_t frames = 0, fed_b = 0;

    for (;;) {
        int ret = audio_recorder_read_data(s_recorder, s_uplink_buf, XZ_REC_READ_SIZE);
        if (ret <= 0) {
            if (++consec_err == 1 || (consec_err % 50) == 0) {
                ESP_LOGW(TAG, "recorder read failed: %d (consec=%d)", ret, consec_err);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        consec_err = 0;
        frames++;

        /* PTT 边沿对齐（UI 只置期望态；真正的动作在这里做） */
        bool want = s_ptt_want;
        if (want != s_ptt_on) {
            if (want) {
                /* 打断：停播 + 上行 barge_in（§6.3，比小智那套两段式简单得多）
                 * ★ 双通道后两路都要停（kind=NONE）：说话就是"别放了"，不是让音乐
                 *   在后台悄悄续着 —— 续播只在 TTS 自己结束时由 SDK 触发。 */
                if ((s_tts_active && s_play != NULL) ||
                    (s_music_playing && (s_music_h != NULL || s_play != NULL))) {
                    xz_cmd_t m = { .id = XZ_CMD_STOP_PLAY, .url = NULL,
                                   .kind = XZ_KIND_NONE };
                    xQueueSend(s_cmd_q, &m, 0);
                    ESP_LOGI(TAG, "barge-in: stop playback");
                }
                (void)xiaozhi_service_send_event("barge_in");
                voice_service_ptt_begin();
                s_ptt_on = true;
                ESP_LOGI(TAG, "ptt DOWN (buffered pcm start)");
            } else {
                s_uplink_sending = true;
                esp_err_t e = voice_service_ptt_finish();
                s_uplink_sending = false;
                s_ptt_on = false;
                ESP_LOGI(TAG, "ptt UP -> %s (%u B / %u ms)",
                         esp_err_to_name(e),
                         (unsigned)voice_service_ptt_bytes(),
                         (unsigned)(voice_service_ptt_bytes() / 32u));
            }
            s_last_act = xTaskGetTickCount();
        }

        /* 闸门：按住 + 连接在 + 缓冲就绪。云端语音回合没有"回声自激"问题
         * （我们不边说边听），所以只需要"按住才收"。 */
        if (s_ptt_on && s_link && voice_service_ptt_active()) {
            voice_service_ptt_feed(s_uplink_buf, (size_t)ret);
            fed_b += (uint32_t)ret;
        }

        /* 水位（每 50 帧 ≈ 3s 一条）：可追溯"按了多久/送了多少" */
        if ((frames % 50) == 0) {
            ESP_LOGI(TAG, "uplink: frames=%u fed=%uKB ptt=%d link=%d tts=%d "
                          "music=%d xrun=%u",
                     (unsigned)frames, (unsigned)(fed_b / 1024),
                     (int)s_ptt_on, (int)s_link, (int)s_tts_active,
                     (int)s_music_playing, (unsigned)s_rec_xrun_risk);
        }

        /* 空闲自动收会话（防 codec 被永久占死） */
        if (!s_ptt_on && !s_tts_active && !s_music_playing) {
            TickType_t idle = xTaskGetTickCount() - s_last_act;
            if (idle > pdMS_TO_TICKS(XZ_IDLE_CLOSE_S * 1000)) {
                ESP_LOGW(TAG, "idle %ds -> auto close session", XZ_IDLE_CLOSE_S);
                xz_cmd_t m = { .id = XZ_CMD_STOP, .url = NULL, .kind = 0 };
                if (xQueueSend(s_cmd_q, &m, 0) == pdPASS) {
                    s_last_act = xTaskGetTickCount();   /* 防重复投 */
                }
            }
        }
    }
}

/* ============================================================
 * PREPARING / RELEASING
 * ============================================================ */

static void session_teardown(void)
{
    set_state(XZ_STATE_RELEASING);
    s_ptt_want = false;
    s_ptt_on = false;
    /* ★ s_link / 连接本身**不动**：WS 是开机常驻的长连接（架构 §"必须长连接"
     *   —— 云端要能主动找设备）。会话只借 codec/录音管线，收会话不等于断网。
     *   连接态一律以 ws_client_is_connected() 为准，不在这里缓存镜像。 */
    s_tts_active = false;
    s_music_playing = false;
    s_play_url[0] = '\0';
    s_music_url[0] = '\0';

    /* ★ 顺序：先关"喂料口"，再删任务，最后断链。
     *   反过来（先删任务）会撞上"任务正卡在 ws send 里、攥着 WS 互斥量"，
     *   导致 ws_client_stop() 的 close 超时、句柄泄漏。 */
    if (s_uplink_task) {
        for (int i = 0; i < 30 && s_uplink_sending; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelete(s_uplink_task);
        s_uplink_task = NULL;
    }
    voice_service_ptt_cancel();
    /* ★ 这里**不**调 ws_client_stop()：见上文。停会话只还 codec，长连接留着。
     *   真需要断（换网关地址/OTA 前）由 ws_client_stop() 显式调用。 */

    if (s_play) {
        (void)audio_playback_stop(s_play);
    }
    if (s_music_h) {
        (void)audio_playback_stop(s_music_h);
    }
    if (s_recorder) {
        audio_recorder_close(s_recorder);
        s_recorder = NULL;
    }
    if (s_play) {
        audio_playback_close(s_play);
        s_play = NULL;
    }
    if (s_music_h) {
        audio_playback_close(s_music_h);
        s_music_h = NULL;
    }
    audio_manager_deinit();

    if (s_rec_scratch) {
        heap_caps_free(s_rec_scratch);
        s_rec_scratch = NULL;
    }
    if (s_uplink_buf) {
        heap_caps_free(s_uplink_buf);
        s_uplink_buf = NULL;
    }
    voice_service_deinit();

    /* 归还 codec：必须先 close 两个 dev（见 audio_service.h 约定） */
    if (s_spk) {
        esp_codec_dev_close(s_spk);
        s_spk = NULL;
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
    audio_service_ext_release_exclusive();

    s_pending_url[0] = '\0';
    ESP_LOGI(TAG, "session released");
    set_state(XZ_STATE_IDLE);
}

/* MCP 工具壳层：D5' 取 A（保留 attach，引擎空转，未来可回 MCP）。
 * 工具本体（6 个 ai_*.c，15 个工具）零改动保留；HTTP 反向调用走 web_server。 */
static esp_err_t tools_attach_once(void)
{
    if (s_mcp) {
        return ESP_OK;
    }
    esp_err_t ret = esp_mcp_create(&s_mcp);
    if (ret != ESP_OK || s_mcp == NULL) {
        ESP_LOGE(TAG, "mcp engine create failed: %s", esp_err_to_name(ret));
        s_mcp = NULL;
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }
    struct { const char *name; esp_err_t (*fn)(esp_mcp_t *); } tab[] = {
        { "light",   ai_light_attach   },
        { "speaker", ai_speaker_attach },
        { "camera",  ai_camera_attach  },
        { "punch",   ai_punch_attach   },
        { "music",   ai_music_attach   },
        { "device",  ai_device_attach  },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        ret = tab[i].fn(s_mcp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ai_%s_attach failed: %s", tab[i].name, esp_err_to_name(ret));
            return ret;
        }
    }
    ESP_LOGI(TAG, "mcp tool shells attached (15 tools, idle engine)");
    return ESP_OK;
}

static esp_err_t session_setup(void)
{
    esp_err_t ret = ESP_OK;
    set_state(XZ_STATE_PREPARING);

    /* 1. ext 独占租借 */
    esp_codec_dev_handle_t spk = NULL, mic = NULL;
    ESP_RETURN_ON_ERROR(audio_service_ext_claim_exclusive(&spk, &mic),
                        TAG, "codec busy");
    if (mic == NULL) {
        ESP_LOGE(TAG, "BSP has no mic, voice chat unsupported");
        audio_service_ext_release_exclusive();
        notify(XZ_EVT_ERROR, "no mic", 0, ESP_ERR_NOT_SUPPORTED);
        set_state(XZ_STATE_IDLE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_spk = spk;
    s_mic = mic;

    /* 2. audio_manager（play 16k/2ch 锚点，rec 声明 16k/1ch） */
    audio_manager_config_t acfg = DEFAULT_AUDIO_MANAGER_CONFIG();
    acfg.play_io.write_cb = xz_play_write_cb;
    acfg.rec_io.read_cb   = xz_rec_read_cb;
    audio_manager_config_set_play_io_format(&acfg, XZ_PLAY_RATE, XZ_BITS,
                                            XZ_PLAY_CHANNELS);
    audio_manager_config_set_rec_io_format(&acfg, XZ_SAMPLE_RATE, XZ_BITS,
                                           XZ_BOARD_CHANNELS);
    snprintf(acfg.mic_layout, sizeof(acfg.mic_layout), "%s", XZ_MIC_LAYOUT);
    ESP_GOTO_ON_ERROR(audio_manager_init(&acfg), fail, TAG, "audio_manager_init");

    /* 3. open 两个 dev */
    esp_codec_dev_sample_info_t fs_play = {
        .sample_rate = XZ_PLAY_RATE,
        .channel = XZ_PLAY_CHANNELS,
        .bits_per_sample = XZ_BITS,
    };
    esp_codec_dev_sample_info_t fs_mic = {
        .sample_rate = XZ_PLAY_RATE,        /* 物理时钟对齐 TX */
        .channel = XZ_PLAY_CHANNELS,
        .bits_per_sample = XZ_BITS,
    };
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(spk, &fs_play) == ESP_CODEC_DEV_OK
                        ? ESP_OK : ESP_FAIL, fail, TAG, "open spk");
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(mic, &fs_mic) == ESP_CODEC_DEV_OK
                        ? ESP_OK : ESP_FAIL, fail, TAG, "open mic");
    esp_codec_dev_set_out_vol(spk, 70);
    s_volume = 70;                  /* 与硬件同口径：/api/system/status 查到的就是它 */
    esp_codec_dev_set_in_gain(mic, 24.0);

    /* 3.5 上行取 L 通道暂存 */
    s_rec_scratch = heap_caps_malloc(XZ_REC_SCRATCH_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(s_rec_scratch, ESP_ERR_NO_MEM, fail, TAG, "rec scratch alloc");
    s_rec_last_tick = 0;
    s_rec_xrun_risk = 0;

    /* 4. 下行播放①：TTS 通道（priority 2，抢占者） */
    audio_play_config_t pcfg = DEFAULT_AUDIO_PLAY_CONFIG();
    pcfg.priority = XZ_TTS_PRIORITY;
    pcfg.event_cb = xz_tts_event_cb;
    pcfg.event_cb_ctx = NULL;
    pcfg.block_until_done = false;
    pcfg.task_config.task_core = 1;
    pcfg.task_config.task_prio = 4;
    ESP_GOTO_ON_ERROR(audio_playback_open(&pcfg, &s_play), fail, TAG, "playback_open(tts)");

    /* 4b. 下行播放②：音乐通道（priority 1，让位者）
     * ★ 两条通道 = audio policy 的"暂停-续播"（见文件头注释）。栈沿用默认
     *   task_stack_in_ext=true（5KB 在 PSRAM），内部 RAM 只多一个 handle。
     * ★ 开不起来**不致命**：退化成共用 s_play（老行为，TTS 仍会顶掉音乐），
     *   宁可少一层体验，也要保证说话/点歌不残废。 */
    pcfg.priority = XZ_MUSIC_PRIORITY;
    pcfg.event_cb = xz_music_event_cb;
    pcfg.task_config.task_prio = 3;      /* 解码别抢 TTS 的 CPU */
    if (audio_playback_open(&pcfg, &s_music_h) != ESP_OK) {
        s_music_h = NULL;
        ESP_LOGW(TAG, "music channel open failed -> fallback to shared handle");
    } else {
        ESP_LOGI(TAG, "music channel ready (priority %d < %d)",
                 XZ_MUSIC_PRIORITY, XZ_TTS_PRIORITY);
    }

    /* 5. 上行 recorder：**PCM** 编码（修正 3：整条管线复用，含 AFE NS/VAD）。
     *    AEC 必须关（MM 布局无参考通道，见改造前 §53 的实测结论）。 */
    audio_recorder_config_t rcfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_afe_config_t afe = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_NS_ENABLE
                           | AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    rcfg.afe_config = afe;
    av_processor_encoder_config_t *enc = &rcfg.encoder_cfg;
    enc->format = AV_PROCESSOR_FORMAT_ID_PCM;
    enc->params.pcm.audio_info.sample_rate = XZ_SAMPLE_RATE;
    enc->params.pcm.audio_info.sample_bits = XZ_BITS;
    enc->params.pcm.audio_info.channels = 1;
    enc->params.pcm.audio_info.frame_duration = XZ_FRAME_MS;
    rcfg.recorder_event_cb = NULL;
    rcfg.recorder_ctx = NULL;
    rcfg.recorder_task_config.task_core = 1;
    ESP_GOTO_ON_ERROR(audio_recorder_open(&rcfg, &s_recorder), fail, TAG, "recorder_open");

    /* 6. 上行任务 + PSRAM 读缓冲 */
    s_uplink_buf = heap_caps_calloc(1, XZ_REC_READ_SIZE, MALLOC_CAP_SPIRAM);
    ESP_GOTO_ON_FALSE(s_uplink_buf, ESP_ERR_NO_MEM, fail, TAG, "uplink buf");
    if (xTaskCreate(xz_uplink_task, "pet_uplink", 3584, NULL, 5, &s_uplink_task)
        != pdPASS) {
        s_uplink_task = NULL;
        ESP_GOTO_ON_ERROR(ESP_ERR_NO_MEM, fail, TAG, "uplink task");
    }

    /* 7. 上行帧缓冲（427 KB PSRAM，一次分配） */
    ESP_GOTO_ON_ERROR(voice_service_init(), fail, TAG, "voice buf");

    /* 8. 网关：注册回调 → 起连接（异步；连上才置 CHATTING） */
    ws_client_set_rx_cb(on_ws_line, NULL);
    ws_client_set_state_cb(on_ws_state, NULL);
    ESP_GOTO_ON_ERROR(ws_client_start(), fail, TAG, "ws start");

    s_last_act = xTaskGetTickCount();

    /* ★ WS 是常连的：绝大多数情况下 start() 是幂等空操作，state_cb **不会**
     *   再回调一次 ⇒ 必须自己判一次，否则永远停在 PREPARING（表现为
     *   "按了 SET 但一直不说话"，因为 PTT 只在 CHATTING 才发得出去）。 */
    if (ws_client_is_connected()) {
        set_state(XZ_STATE_CHATTING);
        flush_pending_audio();
        ESP_LOGI(TAG, "session setup done, link already up (chatting)");
    } else {
        ESP_LOGI(TAG, "session setup done, waiting gateway link");
    }
    return ESP_OK;

fail:
    notify(XZ_EVT_ERROR, "session setup failed", 0, ESP_FAIL);
    session_teardown();
    return (ret == ESP_OK) ? ESP_FAIL : ret;
}

/* ============================================================
 * 服务任务
 * ============================================================ */

static void queue_play(const char *url, xz_kind_t kind)
{
    if (url == NULL || url[0] == '\0') {
        return;
    }
    char *u = heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM);
    if (u == NULL) {
        ESP_LOGE(TAG, "play url alloc failed (%u B)", (unsigned)strlen(url) + 1);
        return;
    }
    strcpy(u, url);
    xz_cmd_t cmd = { .id = XZ_CMD_PLAY, .url = u, .kind = (uint8_t)kind };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        heap_caps_free(u);
        ESP_LOGW(TAG, "play dropped: cmd queue full");
    }
}

static void xz_task(void *arg)
{
    (void)arg;
    static xz_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (cmd.id == XZ_CMD_START && s_state == XZ_STATE_IDLE) {
            /* 反向抢占：听歌时按 PTT 说话 ⇒ 先停本地 MP3 播放器，租约才能借出 */
            media_player_service_stop();
            session_setup();
        } else if (cmd.id == XZ_CMD_STOP &&
                   (s_state == XZ_STATE_PREPARING || s_state == XZ_STATE_CHATTING)) {
            session_teardown();
        } else if (cmd.id == XZ_CMD_PLAY) {
            bool is_music = (cmd.kind == XZ_KIND_MUSIC);
            audio_play_handle_t h = is_music ? s_music_h : s_play;
            char *dst = is_music ? s_music_url : s_play_url;
            /* 音乐通道没开起来 ⇒ 退化到共用 TTS 通道（老行为，会顶掉 TTS） */
            if (h == NULL && is_music) {
                h = s_play;
                dst = s_play_url;
                ESP_LOGW(TAG, "music channel absent, sharing tts handle");
            }
            if (h != NULL && cmd.url != NULL && cmd.url[0] != '\0') {
                snprintf(dst, XZ_URL_MAX, "%s", cmd.url);
                esp_err_t err = audio_playback_play(h, dst);
                ESP_LOGI(TAG, "play %s: %s (%.64s)",
                         is_music ? "music" : "tts", esp_err_to_name(err), dst);
                if (err != ESP_OK) {
                    notify(XZ_EVT_ERROR, "audio play failed", 0, err);
                }
            } else {
                ESP_LOGW(TAG, "play dropped (handle=%d url=%p)", (int)(h != NULL),
                         (void *)cmd.url);
            }
            if (cmd.url != NULL) {
                heap_caps_free(cmd.url);
            }
        } else if (cmd.id == XZ_CMD_STOP_PLAY) {
            /* kind：NONE = 全停（barge-in）；TTS / MUSIC = 只停那一路 */
            uint8_t k = cmd.kind;
            if (s_play != NULL && (k == XZ_KIND_NONE || k == XZ_KIND_TTS)) {
                esp_err_t err = audio_playback_stop(s_play);
                ESP_LOGI(TAG, "play stop tts: %s", esp_err_to_name(err));
                /* 标志由 STOPPED 事件复位（stop 异步生效） */
            }
            if (s_music_h != NULL && (k == XZ_KIND_NONE || k == XZ_KIND_MUSIC)) {
                esp_err_t err = audio_playback_stop(s_music_h);
                ESP_LOGI(TAG, "play stop music: %s", esp_err_to_name(err));
            }
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void xiaozhi_service_init(void)
{
    s_cmd_q = xQueueCreate(6, sizeof(xz_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(6, xz_cmd_t)", 6 * sizeof(xz_cmd_t));
    BaseType_t ok = xTaskCreate(xz_task, "pet_svc", 4096, NULL, 5, &s_task);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(pet_svc, 4096)", 4096);

    /* ★★ 顺序铁律：**先把"收到帧该谁处理"接好，再起连接**。
     *   反过来的话，开机常连那几百毫秒里来的下行帧会撞上 rx_cb == NULL
     *   被整条丢弃（云端 command_table / 定时问候正好在这个窗口）。
     *   1) 下行 JSON 分发总线；
     *   2) 把 rx/state 回调挂上；
     *   3) 最后才 ws_client_start()。 */
    event_bus_init();
    event_bus_set_evt_cb(on_bus_evt);

    /* 网关客户端：建句柄 + 读 NVS + **立刻起长连接**。
     * ★ 为什么不在"会话建立时"才连：桌宠要能被云端主动推送（插件 push、
     *   定时问候、生图、TTS），而 NAT 后面的设备只有一条出站长连接可被复用。
     *   会话期只借 codec/录音管线，与连接生命周期解耦。 */
    ESP_ERROR_CHECK(ws_client_init());
    ws_client_set_rx_cb(on_ws_line, NULL);
    ws_client_set_state_cb(on_ws_state, NULL);
    ESP_ERROR_CHECK(ws_client_start());

    /* 工具层：相机 explain 客户端 + 结果上桥 */
    if (ai_camera_init() == ESP_OK) {
        ai_camera_set_result_cb(on_tool_result);
    } else {
        ESP_LOGW(TAG, "ai_camera_init failed (vision tools disabled)");
    }
    ai_punch_set_result_cb(on_tool_result);
    ai_music_set_result_cb(on_tool_result);

    /* ★ 云侧点歌的配置源。以前**从来没被调用过**（F59）⇒ s_base 一直为空，
     *   music_source_search() 直接返回 -1，点歌永远"没找到可播的版本"。
     *   必须在 NVS 初始化之后（main.c 早期已 init）。 */
    if (music_source_init() != ESP_OK) {
        ESP_LOGW(TAG, "music_source_init failed (cloud music disabled)");
    }

    /* D5' 方案 A：工具壳层挂进 MCP 引擎（无传输层，引擎空转）。
     * 失败不致命：所有能力都有 web_server / 云端 command 的等价入口。 */
    if (tools_attach_once() != ESP_OK) {
        ESP_LOGW(TAG, "mcp tool shells not attached (http/ws entries still work)");
    }

    /* ★ S9 P0：MCP 空转 ⇒ 反向调用的真实通道是 WS action relay。
     *   没有它，云端说"打卡"只能编一句话（实机已踩）。 */
    if (ai_action_init() != ESP_OK) {
        ESP_LOGW(TAG, "ai_action init failed (cloud actions disabled)");
    }

    ESP_LOGI(TAG, "pet session service ready (mic_layout=%s ch=%d, gateway=%s)",
             XZ_MIC_LAYOUT, XZ_BOARD_CHANNELS, ws_client_get_session_id());
}

esp_err_t xiaozhi_service_start(void)
{
    if (s_state != XZ_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_service_mic_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    xz_cmd_t cmd = { .id = XZ_CMD_START, .url = NULL, .kind = 0 };
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t xiaozhi_service_stop(void)
{
    if (s_state != XZ_STATE_PREPARING && s_state != XZ_STATE_CHATTING) {
        return ESP_ERR_INVALID_STATE;
    }
    xz_cmd_t cmd = { .id = XZ_CMD_STOP, .url = NULL, .kind = 0 };
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

xiaozhi_state_t xiaozhi_service_get_state(void)
{
    return s_state;
}

bool xiaozhi_service_link_up(void)
{
    /* 唯一真源 = 客户端真实状态。s_link 只是 state_cb 的镜像，会话收尾时
     * 故意不清它，所以不能拿它当判据（会与实际相反）。 */
    return ws_client_is_connected();
}

/* ★ 给 ai_action.c 用：把 MCP 引擎当"进程内 JSON-RPC 服务"直接投消息。
 *   返回 void* 是为了不让所有 include 本头文件的模块都拖进 mcp-c-sdk 的头；
 *   ai_action.c 自己 include "esp_mcp_engine.h" 后强转成 esp_mcp_t*。
 *   引擎还没 attach（开机早期）时返回 NULL，调用方按"没有工具"处理。 */
void *xiaozhi_service_mcp_engine(void)
{
    return (void *)s_mcp;
}

bool xiaozhi_service_tts_active(void)
{
    return s_tts_active;
}

/* 上行文字：JSON 转义后整帧发送（转义缓冲走 PSRAM，最长 6× 膨胀） */
esp_err_t xiaozhi_service_send_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(text);
    size_t cap = n * 6 + 32;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "{\"type\":\"text\",\"text\":\"");
    for (size_t i = 0; i < n && o + 8 < cap; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            buf[o++] = '\\';
            buf[o++] = (char)c;
        } else if (c == '\n') {
            buf[o++] = '\\';
            buf[o++] = 'n';
        } else if (c < 0x20) {
            o += (size_t)snprintf(buf + o, cap - o, "\\u%04x", c);
        } else {
            buf[o++] = (char)c;          /* UTF-8 原样透传 */
        }
    }
    o += (size_t)snprintf(buf + o, cap - o, "\"}");
    esp_err_t err = ws_client_send_text(buf);
    ESP_LOGI(TAG, "text up %u B -> %s", (unsigned)n, esp_err_to_name(err));
    heap_caps_free(buf);
    return err;
}

esp_err_t xiaozhi_service_send_event(const char *event)
{
    if (event == NULL || event[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"event\",\"event\":\"%s\"}", event);
    return ws_client_send_text(buf);
}

esp_err_t xiaozhi_service_ptt_start(void)
{
    if (s_state != XZ_STATE_CHATTING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ptt_want = true;
    s_last_act = xTaskGetTickCount();
    return ESP_OK;
}

void xiaozhi_service_ptt_stop(void)
{
    s_ptt_want = false;
    s_last_act = xTaskGetTickCount();
}

esp_err_t xiaozhi_service_set_volume(int vol)
{
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_codec_dev_set_out_vol(s_spk, vol) == ESP_CODEC_DEV_OK) {
        s_volume = vol;      /* 只记成功值：失败时查询口径不撒谎 */
        return ESP_OK;
    }
    return ESP_FAIL;
}

int xiaozhi_service_get_volume(void)
{
    return s_volume;
}

void xiaozhi_service_set_evt_cb(xiaozhi_evt_cb_t cb)
{
    s_evt_cb = cb;
}

esp_err_t xiaozhi_service_cloud_audio_play(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= XZ_URL_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_state == XZ_STATE_CHATTING && s_play != NULL) {
        queue_play(url, XZ_KIND_TTS);
        return ESP_OK;
    }
    /* 会话不在：挂起一条，起会话；连上后由 on_ws_state 补播 */
    snprintf(s_pending_url, sizeof(s_pending_url), "%s", url);
    s_pending_kind = XZ_KIND_TTS;
    if (s_state == XZ_STATE_IDLE) {
        esp_err_t err = xiaozhi_service_start();
        if (err != ESP_OK) {
            s_pending_url[0] = '\0';
            notify(XZ_EVT_ERROR, "cloud audio: session busy", 0, err);
            return err;
        }
        ESP_LOGI(TAG, "cloud audio queued, session starting: %.48s", url);
    }
    return ESP_OK;
}

esp_err_t xiaozhi_service_music_play(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= XZ_URL_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_state != XZ_STATE_CHATTING || (s_music_h == NULL && s_play == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    queue_play(url, XZ_KIND_MUSIC);
    return ESP_OK;
}

esp_err_t xiaozhi_service_music_stop(void)
{
    if (s_music_h == NULL && s_play == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 只停音乐那一路：TTS 可能正在说话，别把它的句子砍断 */
    xz_cmd_t cmd = { .id = XZ_CMD_STOP_PLAY, .url = NULL, .kind = XZ_KIND_MUSIC };
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void xiaozhi_service_set_music_evt_cb(xz_music_evt_cb_t cb)
{
    s_music_evt_cb = cb;
}

bool xiaozhi_service_music_playing(void)
{
    return s_music_playing;
}
