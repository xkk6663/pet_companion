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
#include "esp_timer.h"            /* esp_timer_get_time：写 I2S 耗时打点 */
#include "esp_wifi.h"             /* 链路诊断：uplink 水位里带 RSSI/信道 */
#include "middleware/mem_diag.h"

#include "audio_processor.h"        /* av_processor: audio_manager/recorder/playback */
#include "esp_gmf_afe.h"            /* ESP_GMF_AFE_EVT_VAD_START/END（S10 P0 观察回调） */
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
#include "middleware/audio/mic_frontend.h"
#include "middleware/audio/mic_gate.h"   /* S10 P1：会话要麦前，先让监听态收口 */

static const char *TAG = "PET_SVC";

/* ---- 采集/播放格式（下行锚点别再动：I2S 时钟共享的连锁约束）----
 * play_io 16k/2ch 是**下行锚点**（简单播放器统一转成它）。
 * ★ 2026-11-10 双麦前端：rec 的声道数与 AFE 输入布局不再写死，
 *   由 mic_frontend 决定（BSS ⇒ 2ch/"MM"，GSC ⇒ 1ch/"M"）。
 *   原先是 rec 声明 1ch + 布局 "MM" + read_cb 只取 L —— 三处互相矛盾，
 *   等于把双麦阵列在软件层砍成单麦。详见 audio/mic_frontend.h 文件头。 */
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

/* ★★ 2026-11-10 播放任务优先级（"云端对话时 TTS 一帧一帧、本地 MP3 不卡"）
 *
 * 事实：TTS 与本地 MP3 走两条完全不同的链路 ——
 *   TTS  = av_processor/esp_audio_simple_player（本文件的 s_play/s_music_h，
 *          HTTP 拉 /static/tts_*.wav + 解码 + 写 I2S，全在**一个** asp 任务里串行）
 *   MP3  = media_player_service 的 esp_player/GMF 管线（另一套任务，会话外才跑）
 * ⇒ "只有对话时卡"说明问题不在解码器，而在**会话期多出来的任务抢 CPU**。
 *
 * 会话期 core 1 上的现役优先级（实测配置，非推测）：
 *   recorder_task 10（`recorder_task_config`，xiaozhi 覆写 core 1）
 *   WS 收包        8（`WS_TASK_PRIO`，无亲和）
 *   AFE feed/fetch 5 / 5（`afe_*_task_config` 默认；fetch 固定 core 1）
 *   pet_uplink     5（无亲和）
 *   播放任务       4 ← **全场垫底**，却要独自完成 拉流+解码+重采样+写 I2S
 * 一旦播放任务每轮被抢走几十毫秒，I2S TX DMA 就欠载 → 声音断成一片一片。
 * 提到 7：压过 AFE/uplink(5) 与 cmedia(4)，仍让 WS(8)/recorder(10) 先走
 * （收包和取数不能拖，它们的单次占用都在毫秒级）。
 *
 * ★★ 2026-11-10 二轮实测**订正上面那段归因**（prio 7 已生效但仍卡）：
 *   实机 `play tts PLAYING (play task prio 7, core 1)` 且
 *   `tts stat: stalls=16~30 maxgap=824/1200/1441/1842ms`，
 *   段 2 墙钟 18.0 s 只送出 9.27 s 音频 —— 一半时间在空等数据。
 *   CPU 抢占只会造成几十毫秒的空窗，**秒级 gap 只能是上游供给断流**：
 *   TTS 是 esp_audio_simple_player 通过 io_http **边下边播**，
 *   ringbuffer 默认 20 KB ≈ 0.3 s，网络/图片下载/日志一抖，I2S 立刻欠载。
 *   ⇒ 播放任务优先级**不能再往上提**：`HTTP_STREAM_TASK_PRIO = 10`
 *     （esp_gmf_io_http.h）是生产者，把消费者提到 >10 会饿死拉流任务，更卡。
 *   真正的修法是把抖动挡在 I2S 之前：ringbuffer 20 KB → 128 KB（§26）。
 *   其余干扰源：云端「先图片后 TTS」使图片下载/解码与起播重叠（§26）。
 *
 * ★★ 2026-11-11 §35 订正上面「会话期 core1 现役优先级」那张表：
 *   那张表是**改动前快照**，其中 `recorder_task 10` 已被本轮改为
 *   `6 / core0`，AFE feed/fetch/uplink 也一并下沉 core0（见下方
 *   XZ_REC_TASK_* 宏）。另有一条当初漏记的事实：`io_http` 拉流由
 *   `HTTP_STREAM_CFG_DEFAULT()` 固定 **core0 / prio10**，不在 core1；
 *   所以「播放/拉流都在 core1」的说法不成立，隔离靠的是「拉流 10 最高
 *   + 录音侧全部 ≤6 + 播放独占 core1」，不是靠把拉流挪核。 */
#define XZ_PLAY_TASK_PRIO   7
#define XZ_PLAY_TASK_CORE   1
/* 音乐通道让位者：比 TTS 低，但仍要压过 AFE 空转，否则点歌同样会卡 */
#define XZ_MUSIC_TASK_PRIO  5

/* ★★ 2026-11-11 §35：录音链路整条下沉 core0，core1 只留播放消费端。
 *
 * 起因：切双麦 BSS 后 TTS「播几秒停一下」。§26 的实测结论是
 *   「CPU 抢占只造几十毫秒空窗，秒级 gap 只能是上游供给断流」，
 *   而拉流生产者 `HTTP_STREAM_TASK_PRIO=10`（esp_gmf_io_http.h:18）
 *   与工程覆写的 `recorder_task prio=10` **平级争抢** ⇒ BSS 变重后
 *   拉流被拖，官方 20 KB ringbuffer（≈0.32 s 音频，§29 撤销 128 KB
 *   补丁后的基线）一抖就见底。
 *
 * 布局（★ 拉流 io_http 是托管组件 `HTTP_STREAM_CFG_DEFAULT()` 固定
 *   **core0 / prio10**，不能改核，只能靠优先级隔离）：
 *     core0 : io_http 拉流 10  >  recorder 6  >  AFE feed/fetch 5  >  uplink 5
 *     core1 : 播放 work task 7（独占；GMF 解码/重采样/写 I2S 都串在它里面）
 *   ⇒ 播放与录音物理分核；拉流仍是 core0 最高优先级，不会被 BSS 拖住。
 *
 * ★ recorder 取 6 而不是更低：它要高于 AFE feed/fetch(5)，
 *   否则 GMF 管线内部调度被饿，会重现 `AFE: Ringbuffer of AFE is empty`。
 * ★ 不动 afe_config 的算法/内存，也不动托管组件；改 Kconfig 无关，
 *   纯 C 常量 ⇒ 只需 build，不必 reconfigure。 */
#define XZ_REC_TASK_CORE    0
#define XZ_REC_TASK_PRIO    6

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
    XZ_CMD_TTS_DONE,    /* 内部：一段 TTS 结束，取下一条排队语音 */
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
    char       *text;   /* TTS：这一句的原文（可空）。起播那一刻才抛给 UI，
                         * 让文字气泡和声音同时出现（§40）。 */
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

/* S10 P0：VAD 观察打点（只记录，不参与任何判定）。
 * 用途：给"接线前"留行为基线——每小时 VAD_START 次数、平均句长、误报率。
 * ★ 回调跑在 AFE 任务上下文：只做时间戳与日志，绝不碰 LVGL / 不做 IO。 */
static int64_t s_gate_vad_start_us;

static uint8_t      *s_uplink_buf;   /* PSRAM 读缓冲 */

/* ★★ 2026-11-11 按句流水：云端会把一段回复拆成多句、分别下发一条 audio。
 *   而 `audio_playback_play()` 在 PLAYING 时是"先 stop 再播新的"（换曲语义），
 *   直接连着调会把上一句拦腰截断。这里做一条**有界 FIFO**：正在播时来的
 *   URL 先排队，收到 AUDIO_FINISHED 再由 xz_task 取下一条。
 *   depth=16 覆盖 200 字上限（≈10~16 句）的一整轮回复；溢出时丢新包并
 *   打日志，宁可少念一句也不打断已排好的顺序。
 *   所有权规则：入队即接管 url 指针，出队方负责 heap_caps_free。 */
#define XZ_TTS_QUEUE_DEPTH  16

/* 一条排队中的 TTS：URL + 这一句的原文。
 * ★ 原文必须跟着 URL 一起排队（§40）：网关是"按句"下发 audio 的，文字也按句
 *   跟着来；如果一收到就显示，第 2 句的文字会比它的声音早 4~5 s（因为第 1 句
 *   还在播）。只有等这一句真正起播时才抛 XZ_EVT_TEXT，文字才和声音对齐。 */
typedef struct {
    char *url;
    char *text;
} tts_seg_t;

static tts_seg_t    s_tts_q[XZ_TTS_QUEUE_DEPTH];
static uint8_t      s_tts_q_head;
static uint8_t      s_tts_q_tail;
static uint8_t      s_tts_q_n;
/* 已发起播放、尚未收到结束事件。用它（而不是 s_tts_active）判"忙"：
 * s_tts_active 要等 PLAYING 回调才置位，中间那 100+ms 的窗口会把第二句打进来。 */
static volatile bool s_tts_busy;

static void tts_seg_free(tts_seg_t *s)
{
    if (s->url != NULL) {
        heap_caps_free(s->url);
        s->url = NULL;
    }
    if (s->text != NULL) {
        heap_caps_free(s->text);
        s->text = NULL;
    }
}

static void tts_q_push(char *url, char *text)
{
    if (url == NULL) {
        if (text != NULL) {
            heap_caps_free(text);
        }
        return;
    }
    if (s_tts_q_n >= XZ_TTS_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "tts queue full (%d), drop %.48s", XZ_TTS_QUEUE_DEPTH, url);
        heap_caps_free(url);
        if (text != NULL) {
            heap_caps_free(text);
        }
        return;
    }
    s_tts_q[s_tts_q_tail].url = url;
    s_tts_q[s_tts_q_tail].text = text;
    s_tts_q_tail = (uint8_t)((s_tts_q_tail + 1) % XZ_TTS_QUEUE_DEPTH);
    s_tts_q_n++;
}

static bool tts_q_pop(tts_seg_t *out)
{
    if (s_tts_q_n == 0) {
        return false;
    }
    *out = s_tts_q[s_tts_q_head];
    s_tts_q[s_tts_q_head].url = NULL;
    s_tts_q[s_tts_q_head].text = NULL;
    s_tts_q_head = (uint8_t)((s_tts_q_head + 1) % XZ_TTS_QUEUE_DEPTH);
    s_tts_q_n--;
    return true;
}

static void tts_q_clear(void)
{
    tts_seg_t seg;
    while (tts_q_pop(&seg)) {
        tts_seg_free(&seg);
    }
}

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

/* ★ 2026-11-10 播放供给打点（"TTS 卡成一帧一帧"的定量判据）
 *   现象只在云端对话时出现、本地 MP3 流畅 ⇒ 差异在"会话期多出来的任务"
 *   与"播放任务优先级"，必须用数据区分下面三种成因：
 *     (a) 供给抖断：播放任务被高优任务抢走 / 解码慢 / HTTP 断流
 *         ⇒ stall 次数多、maxgap 大（远超块时长）
 *     (b) 输出被跳过：audio_policy 判定非 winner，cb 根本没被调
 *         ⇒ writes/bytes 远少于"应播时长 × 64000 B/s"
 *     (c) I2S 硬件层（全双工 TX 欠载）：写入密、间隔正常但声音仍断
 *   块时长按 play_io 16k/2ch/16bit = 64000 B/s 换算。 */
static uint32_t   s_pw_writes;
static uint32_t   s_pw_bytes;
static uint32_t   s_pw_stall;       /* 间隔 > 块时长 + 30ms 的次数 */
static uint32_t   s_pw_maxgap_ms;
static TickType_t s_pw_last;
/* ★ 2026-11-10 二轮：把「两次回调之间的间隔」拆成「间隔」与「写 codec 自身耗时」。
 *   maxgap 大到 0.8~1.8 s 时，必须能区分：
 *     写 codec 慢 ⇒ I2S 全双工 / DMA 层（对话时 mic 与 spk 同时 open）
 *     写 codec 快、间隔大 ⇒ 上游断供（HTTP 边下边播 / 解码 / 任务调度）
 *   否则只能继续猜。tick 分辨率不够（10 ms），用 esp_timer 微秒计时。 */
static uint32_t   s_pw_wr_max_ms;
static uint32_t   s_pw_wr_slow;     /* esp_codec_dev_write 自身 > 30ms 的次数 */

/* ★ 2026-11-11 端到端延迟探针（回答"首响到底花在哪"）：
 *   req  → PLAYING：player 建立 / HTTP open / 解码器 open
 *   PLAYING → first：首字节到达 + 解码预滚（这一段才是 MP3 相对 WAV 的净增量）
 *   first → 结束  ：纯播放时长
 *   三段加起来 = "URL 到达设备 → 出声"的全部可优化空间（不含网关合成）。 */
static int64_t    s_t_req_us;
static int64_t    s_t_playing_us;
static int64_t    s_t_first_us;

static void xz_play_stat_reset(void)
{
    s_pw_writes = 0;
    s_pw_bytes = 0;
    s_pw_stall = 0;
    s_pw_maxgap_ms = 0;
    s_pw_last = 0;
    s_pw_wr_max_ms = 0;
    s_pw_wr_slow = 0;
    s_t_playing_us = esp_timer_get_time();
    s_t_first_us = 0;
}

static esp_err_t xz_play_write_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t now = xTaskGetTickCount();
    uint32_t blk_ms = (uint32_t)size * 1000u /
                      (XZ_PLAY_RATE * XZ_PLAY_CHANNELS * (XZ_BITS / 8));
    if (s_pw_last != 0) {
        uint32_t gap = (uint32_t)(now - s_pw_last) * portTICK_PERIOD_MS;
        if (gap > blk_ms + 30) {
            s_pw_stall++;
            if (gap > s_pw_maxgap_ms) {
                s_pw_maxgap_ms = gap;
            }
        }
    }
    s_pw_last = now;
    s_pw_writes++;
    s_pw_bytes += (uint32_t)size;
    if (s_pw_writes == 1) {
        s_t_first_us = esp_timer_get_time();
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = (esp_codec_dev_write(s_spk, data, size) >= 0) ? ESP_OK : ESP_FAIL;
    uint32_t wr_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (wr_ms > 30) {
        s_pw_wr_slow++;
    }
    if (wr_ms > s_pw_wr_max_ms) {
        s_pw_wr_max_ms = wr_ms;
    }
    return ret;
}

static TickType_t s_rec_last_tick;
static uint32_t   s_rec_xrun_risk;

/* 上行采集回调（AFE feed 任务上下文）
 * ★ 双麦：这里不再自己解交织 / 丢 R，交给 mic_frontend 决定
 *   —— BSS 模式返回 2ch 交错，GSC 模式返回波束后的 1ch。 */
static esp_err_t xz_rec_read_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t now = xTaskGetTickCount();
    int ch = mic_fe_out_channels();
    int frames = size / (ch * (XZ_BITS / 8));
    int block_ms = frames * 1000 / XZ_SAMPLE_RATE;
    if (s_rec_last_tick != 0 &&
        (now - s_rec_last_tick) > pdMS_TO_TICKS(block_ms + 15)) {
        s_rec_xrun_risk++;
        ESP_LOGW(TAG, "rec read gap %dms > %dms, xrun risk #%u",
                 (int)(now - s_rec_last_tick) * portTICK_PERIOD_MS,
                 block_ms + 15, (unsigned)s_rec_xrun_risk);
    }
    s_rec_last_tick = now;
    return mic_fe_read(data, size, s_mic);
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
        xz_play_stat_reset();
        /* TTS 期间麦克风收到的是喇叭回声：停掉 MIC 诊断（统计 + 5s 日志），
         * 不与下行播放争 PSRAM / UART 日志锁。只影响诊断，不影响采集。 */
        mic_fe_set_diag_hold(true);
        ESP_LOGI(TAG, "play tts PLAYING (play task prio %d, core %d)",
                 XZ_PLAY_TASK_PRIO, XZ_PLAY_TASK_CORE);
        notify(XZ_EVT_TTS_STATE, NULL, 1, ESP_OK);          /* role=1 start */
        break;
    case AUDIO_PLAY_STATE_FINISHED:
    case AUDIO_PLAY_STATE_STOPPED:
    case AUDIO_PLAY_STATE_ERROR: {
        bool err = (state == AUDIO_PLAY_STATE_ERROR);
        bool fin = (state == AUDIO_PLAY_STATE_FINISHED);
        s_tts_active = false;
        s_tts_busy = false;
        s_last_act = xTaskGetTickCount();
        mic_fe_set_diag_hold(false);
        /* 供给质量：bytes/64000 = 实际送入 codec 的音频秒数。
         * stalls 多 ⇒ 抖断（调度/网络）；bytes 明显小于实际播放秒数 ⇒ 被 policy 跳过。 */
        ESP_LOGI(TAG, "tts stat: writes=%u audio=%.2fs stalls=%u maxgap=%ums "
                      "wr_max=%ums wr_slow=%u",
                 (unsigned)s_pw_writes,
                 (double)s_pw_bytes / (double)(XZ_PLAY_RATE * XZ_PLAY_CHANNELS *
                                              (XZ_BITS / 8)),
                 (unsigned)s_pw_stall, (unsigned)s_pw_maxgap_ms,
                 (unsigned)s_pw_wr_max_ms, (unsigned)s_pw_wr_slow);
        /* 端到端延迟：req(收到URL)→PLAYING→首帧。首帧以后的都是纯播放，不参与延迟。 */
        if (s_t_req_us && s_t_first_us) {
            ESP_LOGI(TAG, "tts lat: req->play=%lldms play->first=%lldms total=%lldms",
                     (long long)((s_t_playing_us - s_t_req_us) / 1000),
                     (long long)((s_t_first_us - s_t_playing_us) / 1000),
                     (long long)((s_t_first_us - s_t_req_us) / 1000));
        }
        ESP_LOGI(TAG, "play tts %s (queued=%u)", err ? "ERROR" : (fin ? "FINISHED" : "STOPPED"),
                 (unsigned)s_tts_q_n);
        /* ★ 按句流水：STOP 事件这里**不**上抛 idle —— 队列里还有下一句时，
         *   中间那一瞬间的 idle 会让嘴型/UI 闪一下。统一由 xz_task 在
         *   "队列真的空了"时发 stop（XZ_CMD_TTS_DONE 分支）。 */
        xz_cmd_t m = { .id = XZ_CMD_TTS_DONE, .url = NULL, .kind = XZ_KIND_TTS };
        xQueueSend(s_cmd_q, &m, 0);
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

static void queue_play(const char *url, xz_kind_t kind, const char *text);

/* 云端推来的 TTS 在"codec 还没借出来"时只能挂起，等会话可用再补播。
 * 两个入口都要调：① 刚连上（state_cb）② 连接**早就**在（WS 常连），
 * 此时 state_cb 不会再回调 —— 必须在 session_setup 尾部自己查一次。 */
static void flush_pending_audio(void)
{
    if (s_pending_url[0]) {
        ESP_LOGI(TAG, "flush pending audio: %.48s", s_pending_url);
        queue_play(s_pending_url, (xz_kind_t)s_pending_kind, NULL);
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

        /* 水位（每 50 帧 ≈ 3s 一条）：可追溯"按了多久/送了多少"
         * ★ 2026-11-11 追加 wifi rssi/ch：下行 TTS 卡顿时用来判断是链路弱
         *   还是拉流慢（拉流只有 8~24 KB/s，PC 同源 130+ KB/s）。 */
        if ((frames % 50) == 0) {
            wifi_ap_record_t ap;
            int rssi = 0;
            unsigned ch = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
                ch = ap.primary;
            }
            ESP_LOGI(TAG, "uplink: frames=%u fed=%uKB ptt=%d link=%d tts=%d "
                          "music=%d xrun=%u | wifi rssi=%d ch=%u",
                     (unsigned)frames, (unsigned)(fed_b / 1024),
                     (int)s_ptt_on, (int)s_link, (int)s_tts_active,
                     (int)s_music_playing, (unsigned)s_rec_xrun_risk,
                     rssi, ch);
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
    s_tts_busy = false;
    tts_q_clear();          /* 会话结束：排队中的后续句子没有播放的意义了 */
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

    /* 双麦前端：GSC 模式还持有 gsc 句柄 + PSRAM 缓冲，必须显式放 */
    mic_fe_deinit();

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

/* ============================================================
 * S10 P0：VAD 观察回调
 *
 * 现状：AFE 的 VAD 一直开着（`algo_mask` 里带 VAD_ENABLE），事件也一直抛，
 *      但 `recorder_event_cb` 被置 NULL，所以全被丢弃。这里只把它接出来打日志。
 * 纪律：不改 `s_ptt_on` 判定、不改任何所有权、不上报云端。
 * 判据：说 3 句出现 3 对 vad_start/vad_end；纯静音应接近 0 对。
 * ============================================================ */
static void xz_recorder_event_cb(audio_recorder_handle_t recorder, void *event, void *ctx)
{
    (void)recorder;
    (void)ctx;
    if (event == NULL) {
        return;
    }
    const esp_gmf_afe_evt_t *evt = (const esp_gmf_afe_evt_t *)event;
    if (evt->type == ESP_GMF_AFE_EVT_VAD_START) {
        s_gate_vad_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "GATE: vad_start");
    } else if (evt->type == ESP_GMF_AFE_EVT_VAD_END) {
        int64_t dur_ms = (s_gate_vad_start_us > 0)
                       ? (esp_timer_get_time() - s_gate_vad_start_us) / 1000
                       : 0;
        ESP_LOGI(TAG, "GATE: vad_end %lldms", (long long)dur_ms);
        s_gate_vad_start_us = 0;
    }
}

static esp_err_t session_setup(void)
{
    esp_err_t ret = ESP_OK;
    set_state(XZ_STATE_PREPARING);

    /* 0. S10 P1：监听态（若启用）必须先完整归还 codec + recorder。
     *    ★ 不是"两个 recorder 并存"，是交接：gate 关干净，会话再 claim。 */
    if (!mic_gate_yield(300)) {
        ESP_LOGW(TAG, "mic gate busy, session refused");
        notify(XZ_EVT_ERROR, "audio busy", 0, ESP_ERR_INVALID_STATE);
        set_state(XZ_STATE_IDLE);
        return ESP_ERR_INVALID_STATE;
    }

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

    /* 1.5 双麦前端（★ 必须在 audio_manager_init 之前：
     *     rec_io 声道数与 AFE 输入布局都由它决定，写死会打架） */
    ESP_GOTO_ON_ERROR(mic_fe_init(NULL), fail, TAG, "mic_fe_init");

    /* 2. audio_manager（play 16k/2ch 锚点；rec 声道/布局跟随双麦前端） */
    audio_manager_config_t acfg = DEFAULT_AUDIO_MANAGER_CONFIG();
    acfg.play_io.write_cb = xz_play_write_cb;
    acfg.rec_io.read_cb   = xz_rec_read_cb;
    audio_manager_config_set_play_io_format(&acfg, XZ_PLAY_RATE, XZ_BITS,
                                            XZ_PLAY_CHANNELS);
    audio_manager_config_set_rec_io_format(&acfg, XZ_SAMPLE_RATE, XZ_BITS,
                                           mic_fe_out_channels());
    snprintf(acfg.mic_layout, sizeof(acfg.mic_layout), "%s", mic_fe_layout());
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

    /* ★★ 必须在 open 之后，且必须在 set_in_gain 之后（顺序两处都关键）：
     *   1) ADC 输入选择是 open 时才写进 0x72/0x73（es8389.c:527-528）；
     *   2) set_in_gain → es8389_apply_mic_pga_gain → es8389_pga_gain_byte(gain,
     *      ADCL_InputSel, 0) 用**编译期常量**重新生成整字节并整体写入 0x72/0x73
     *      ⇒ 放在它前面，单端 sel 会被覆盖回差分（2026-11-10 实测踩过：开关=1
     *      但 corr 仍是 -0.9，就是这里被吃掉了）。 */
    mic_fe_attach_codec(mic);

    /* 3.5 上行采集就绪标记（缓冲已收编进 mic_frontend，这里不再分配） */
    s_rec_last_tick = 0;
    s_rec_xrun_risk = 0;

    /* 4. 下行播放①：TTS 通道（priority 2，抢占者） */
    audio_play_config_t pcfg = DEFAULT_AUDIO_PLAY_CONFIG();
    pcfg.priority = XZ_TTS_PRIORITY;
    pcfg.event_cb = xz_tts_event_cb;
    pcfg.event_cb_ctx = NULL;
    pcfg.block_until_done = false;
    pcfg.task_config.task_core = XZ_PLAY_TASK_CORE;
    pcfg.task_config.task_prio = XZ_PLAY_TASK_PRIO;
    ESP_GOTO_ON_ERROR(audio_playback_open(&pcfg, &s_play), fail, TAG, "playback_open(tts)");

    /* 4b. 下行播放②：音乐通道（priority 1，让位者）
     * ★ 两条通道 = audio policy 的"暂停-续播"（见文件头注释）。栈沿用默认
     *   task_stack_in_ext=true（5KB 在 PSRAM），内部 RAM 只多一个 handle。
     * ★ 开不起来**不致命**：退化成共用 s_play（老行为，TTS 仍会顶掉音乐），
     *   宁可少一层体验，也要保证说话/点歌不残废。 */
    pcfg.priority = XZ_MUSIC_PRIORITY;
    pcfg.event_cb = xz_music_event_cb;
    pcfg.task_config.task_prio = XZ_MUSIC_TASK_PRIO;   /* 解码别抢 TTS 的 CPU */
    if (audio_playback_open(&pcfg, &s_music_h) != ESP_OK) {
        s_music_h = NULL;
        ESP_LOGW(TAG, "music channel open failed -> fallback to shared handle");
    } else {
        ESP_LOGI(TAG, "music channel ready (priority %d < %d)",
                 XZ_MUSIC_PRIORITY, XZ_TTS_PRIORITY);
    }

    /* 5. 上行 recorder：**PCM** 编码（整条管线复用，含 AFE 前端处理）。
     *    AEC 必须关：AEC 要播放参考通道（布局里的 'R'），本板采集链路没有参考，
     *    硬开会把回声路径当噪声学（改造前 §53 的实测结论，仍然成立）。
     *  ★ 一个必须知道的坑：av_processor 把 NS 标志接到 afe_cfg->se_init
     *    （audio_processor.c:1212 `se_init = ns_enable`），而 `ns_init` 它根本没设。
     *    也就是说 AV_PROCESSOR_AFE_FLAG_NS_ENABLE 打开的是 **SE(BSS 数组处理)**，
     *    不是 NSNet 降噪 —— 它是双麦的开关，不是"降噪开关"。
     *  ★ 2026-11-11 结案：本板**是两只独立麦**，右路极性反（P4 五态对照，§33）。
     *    默认 MIC_FE_MODE=BSS ⇒ 这里走 if 分支，把 SE(BSS) 打开；
     *    若 menuconfig 改回 MONO（1ch），自动走 else 只开 VAD —— 两档都由
     *    `mic_fe_out_channels()` 决定，不需要在业务层加开关。 */
    audio_recorder_config_t rcfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_afe_config_t afe = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    if (mic_fe_out_channels() == 2) {
        /* BSS：2 麦直通 → AFE 按 "MM" 解析成两麦，SE 做盲源分离 + VAD。
         * ★ 盲分离不需要麦间距，这是选它而非 GSC 的原因（§34.1）。 */
        afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_NS_ENABLE
                               | AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    } else {
        /* MONO / GSC：前端只给一路，AFE 再做 BSS 等于把单声道当两麦解交织
         *（或对波束输出做二次空间处理），纯浪费且会伤 ASR ⇒ 只留 VAD */
        afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    }
    rcfg.afe_config = afe;
    av_processor_encoder_config_t *enc = &rcfg.encoder_cfg;
    enc->format = AV_PROCESSOR_FORMAT_ID_PCM;
    enc->params.pcm.audio_info.sample_rate = XZ_SAMPLE_RATE;
    enc->params.pcm.audio_info.sample_bits = XZ_BITS;
    enc->params.pcm.audio_info.channels = 1;
    enc->params.pcm.audio_info.frame_duration = XZ_FRAME_MS;
    /* S10 P0：把已经被丢弃的 VAD 事件接出来（只打日志；行为零变化） */
    rcfg.recorder_event_cb = xz_recorder_event_cb;
    rcfg.recorder_ctx = NULL;
    /* ★★ §35 核隔离 + 降优先级（见 XZ_REC_TASK_* 宏处的完整推导）。
     *   组件默认：feed=core0/prio5、fetch=core1/prio5、recorder=core0/prio10；
     *   工程此前把 recorder 覆写成 core1/prio10，于是 core1 上 prio10 压着
     *   播放 prio7，core0 上拉流 prio10 又与它平级 ⇒ BSS 一变重就互相拖。
     *   现在三条录音任务统一 core0，recorder 降到 6：
     *     · 播放(7) 独占 core1；
     *     · 拉流(10) 在 core0 仍是绝对最高，BSS(5) 抢不过它；
     *     · recorder(6) 仍压得住 AFE feed/fetch(5)，管线不会饿。
     *   ★ feed 的 core 本来就是 0，显式写出来是为了让这张表在代码里可读，
     *     防止以后有人改组件默认值时静默破坏隔离。 */
    rcfg.afe_feed_task_config.task_core  = XZ_REC_TASK_CORE;
    rcfg.afe_fetch_task_config.task_core = XZ_REC_TASK_CORE;
    rcfg.recorder_task_config.task_core  = XZ_REC_TASK_CORE;
    rcfg.recorder_task_config.task_prio  = XZ_REC_TASK_PRIO;

    /* ★★ 双麦 BSS 的失败兜底（2026-11-11 落地）。
     *   BSS 是本板唯一的双麦算法（不需要麦间距），但它比单麦 VAD 多吃
     *   一份数组处理内存 + CPU；本工程内部 RAM 余量只有约 4 KB，
     *   任何一次 AFE 创建失败都会让整条语音链路开不起来。
     *   ⇒ 先按双麦开；失败就退回"同一份 2ch 采集、AFE 只用第一路"
     *     （afe_config_check 的原话：SE 关掉时只用第一个麦克风通道）。
     *     这样最坏情况等价于原来的单麦，而不是彻底没声。
     *   ★ 不在这里把 mic_fe 切回 MONO：rec_io 已按 2ch 声明，
     *     中途改 out_channels 会让 read_cb 的字节数/帧数对不上。 */
    esp_err_t rec_err = audio_recorder_open(&rcfg, &s_recorder);
    if (rec_err != ESP_OK && mic_fe_out_channels() == 2) {
        ESP_LOGW(TAG, "recorder_open with BSS failed (%s) -> retry: AFE 只用第一路（单麦降级）",
                 esp_err_to_name(rec_err));
        rcfg.afe_config.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
        rec_err = audio_recorder_open(&rcfg, &s_recorder);
    }
    ESP_GOTO_ON_FALSE(rec_err == ESP_OK, rec_err, fail, TAG, "recorder_open");
    /* ★ 这一行是 BSS 的"代价台账"：双麦比单麦多一份数组处理，
     *   本工程内部 RAM 只有约 4 KB 余量 ⇒ 必须让 largest 在这里留痕，
     *   否则后面 WiFi/播放撞内存时无法判断是不是 BSS 吃掉的。
     *   口径：只看 largest_free_block，不看 free_size（碎片时 free 仍很大）。 */
    ESP_LOGI(TAG, "uplink frontend: %s (mic=%dch layout=%s) -> 上行 1ch | "
                  "INT free=%u largest=%u | PSRAM free=%uKB",
             (mic_fe_out_channels() == 2) ? "SE(BSS)+VAD" : "VAD",
             mic_fe_out_channels(), mic_fe_layout(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 6. 上行任务 + PSRAM 读缓冲 */
    s_uplink_buf = heap_caps_calloc(1, XZ_REC_READ_SIZE, MALLOC_CAP_SPIRAM);
    ESP_GOTO_ON_FALSE(s_uplink_buf, ESP_ERR_NO_MEM, fail, TAG, "uplink buf");
    /* §35：上行任务也钉 core0，避免它在两核间漂到播放核去。
     * ★ 栈留在内部 RAM（xTaskCreatePinnedToCore 默认内部），符合 F35 铁律；
     *   本任务会走 flash/NVS 之外的纯缓冲路径，但保守起见不启用外置栈。 */
    if (xTaskCreatePinnedToCore(xz_uplink_task, "pet_uplink", 3584, NULL, 5,
                                &s_uplink_task, XZ_REC_TASK_CORE) != pdPASS) {
        s_uplink_task = NULL;
        ESP_GOTO_ON_ERROR(ESP_ERR_NO_MEM, fail, TAG, "uplink task");
    }
    /* 验收台账：一行看清两核的优先级排序（拉流 10 是组件固定，不在此配置）。 */
    ESP_LOGI(TAG, "cpu affinity: core0 = io_http(10) > recorder(%d) > "
                  "AFE feed,fetch(5) / uplink(5) | core1 = play(%d) only",
             XZ_REC_TASK_PRIO, XZ_PLAY_TASK_PRIO);

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

static void queue_play(const char *url, xz_kind_t kind, const char *text)
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
    char *t = NULL;
    if (text != NULL && text[0] != '\0') {
        size_t n = strlen(text) + 1;
        t = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (t != NULL) {
            memcpy(t, text, n);
        }
    }
    xz_cmd_t cmd = { .id = XZ_CMD_PLAY, .url = u, .text = t, .kind = (uint8_t)kind };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        heap_caps_free(u);
        if (t != NULL) {
            heap_caps_free(t);
        }
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
            /* ★ 2026-11-11 §39：本条 URL 是否"已入队"。入队后 cmd.url 被置 NULL，
             *   若不加这个标记就会掉进下面的 else 分支 —— 那里会执行
             *   `s_tts_busy = false`，把正在播的那句的"忙"标志误清掉：
             *   第 3 句一到就被当成"空闲"直接起播，用换曲语义把第 1 句拦腰砍断。
             *   日志里那条 `play dropped (handle=1 url=0)` 也是这么来的。 */
            bool queued = false;
            /* 音乐通道没开起来 ⇒ 退化到共用 TTS 通道（老行为，会顶掉 TTS） */
            if (h == NULL && is_music) {
                h = s_play;
                dst = s_play_url;
                ESP_LOGW(TAG, "music channel absent, sharing tts handle");
            }
            /* ★ 按句流水：TTS 正在播时来的下一句先排队，等 FINISHED 再播
             *   （否则 audio_playback_play 会先 stop，把上一句拦腰砍断）。
             *   音乐不走队列：点歌本来就是"换曲语义"，新歌顶掉旧的才对。 */
            if (!is_music && s_tts_busy) {
                tts_q_push(cmd.url, cmd.text);
                cmd.url = NULL;
                cmd.text = NULL;
                queued = true;
                ESP_LOGI(TAG, "tts queued (%u/%d)", (unsigned)s_tts_q_n, XZ_TTS_QUEUE_DEPTH);
            }
            if (h != NULL && cmd.url != NULL && cmd.url[0] != '\0') {
                snprintf(dst, XZ_URL_MAX, "%s", cmd.url);
                /* ★ 2026-11-01 现场诊断：GMF 任务/信号量只吃**内部 RAM**，
                 *   真机曾在 `ESP_GMF_TASK: Memory exhausted`（xSemaphoreCreateBinary
                 *   返回 NULL）→ `play tts ERROR` → TTS 整段静音。
                 *   这一行把失败瞬间的"还剩多少、最大块多少"钉死，下次复现直接对照：
                 *     INT free 若还有几十 KB 而 largest 很小 → 碎片；
                 *     INT free 本身就接近 0 → 被 AFE/相机/WiFi 占满。 */
                ESP_LOGI(TAG, "tts mem pre: INT free=%u largest=%u | PSRAM free=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                s_t_req_us = esp_timer_get_time();
                if (!is_music) {
                    s_tts_busy = true;      /* 先置忙：堵住 PLAYING 之前的那 100ms 窗口 */
                    /* ★ 文字和声音同一刻出现（§40）：整段 text 不再等 LLM 写完，
                     *   改成每句起播时抛一条，UI 一侧就是"一句一个气泡"。 */
                    if (cmd.text != NULL) {
                        notify(XZ_EVT_TEXT, cmd.text, 1, ESP_OK);
                    }
                }
                esp_err_t err = audio_playback_play(h, dst);
                ESP_LOGI(TAG, "play %s: %s (%.64s)",
                         is_music ? "music" : "tts", esp_err_to_name(err), dst);
                if (err != ESP_OK) {
                    if (!is_music) {
                        s_tts_busy = false;     /* 起播失败没有结束事件，别把标志挂死 */
                        tts_q_clear();
                    }
                    notify(XZ_EVT_ERROR, "audio play failed", 0, err);
                }
            } else if (!queued) {
                if (!is_music) {
                    s_tts_busy = false;     /* 没有可播句柄：别把"忙"标志挂死 */
                }
                ESP_LOGW(TAG, "play dropped (handle=%d url=%p)", (int)(h != NULL),
                         (void *)cmd.url);
            }
            if (cmd.url != NULL) {
                heap_caps_free(cmd.url);
            }
            if (cmd.text != NULL) {
                heap_caps_free(cmd.text);
            }
        } else if (cmd.id == XZ_CMD_STOP_PLAY) {
            /* kind：NONE = 全停（barge-in）；TTS / MUSIC = 只停那一路 */
            uint8_t k = cmd.kind;
            if (k == XZ_KIND_NONE || k == XZ_KIND_TTS) {
                /* ★ 排队中的后续句子必须一起丢：用户已经在打断/离开了，
                 *   再念下去就是"打不断"的坏体验。 */
                tts_q_clear();
            }
            if (s_play != NULL && (k == XZ_KIND_NONE || k == XZ_KIND_TTS)) {
                esp_err_t err = audio_playback_stop(s_play);
                ESP_LOGI(TAG, "play stop tts: %s", esp_err_to_name(err));
                /* 标志由 STOPPED 事件复位（stop 异步生效） */
            }
            if (s_music_h != NULL && (k == XZ_KIND_NONE || k == XZ_KIND_MUSIC)) {
                esp_err_t err = audio_playback_stop(s_music_h);
                ESP_LOGI(TAG, "play stop music: %s", esp_err_to_name(err));
            }
        } else if (cmd.id == XZ_CMD_TTS_DONE) {
            /* 一段语音播完（或被停/出错）：队列里还有就接着播，真没了才收嘴型。 */
            tts_seg_t next = { 0 };
            if (tts_q_pop(&next) && s_play != NULL) {
                snprintf(s_play_url, XZ_URL_MAX, "%s", next.url);
                s_t_req_us = esp_timer_get_time();
                s_tts_busy = true;
                /* 这一句的文字就在此刻冒出来 —— 和它的声音对齐（§40） */
                if (next.text != NULL) {
                    notify(XZ_EVT_TEXT, next.text, 1, ESP_OK);
                }
                esp_err_t err = audio_playback_play(s_play, s_play_url);
                ESP_LOGI(TAG, "play tts (next): %s (left=%u)",
                         esp_err_to_name(err), (unsigned)s_tts_q_n);
                tts_seg_free(&next);
                if (err != ESP_OK) {
                    s_tts_busy = false;
                    tts_q_clear();
                    notify(XZ_EVT_TTS_STATE, NULL, 0, err);
                }
            } else {
                tts_seg_free(&next);    /* s_play 已在 teardown 里关了 */
                notify(XZ_EVT_TTS_STATE, NULL, 0, ESP_OK);      /* role=0 stop */
            }
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void xiaozhi_service_init(void)
{
    /* ★★ 双麦前端必须在这里初始化，不能只留在 session_setup 里。
     *   原因（2026-11-10 实机日志暴露）：session_setup 只在**首次会话**才跑，
     *   而 WiFi/网关不通时永远走不到那一步 ⇒ 串口里既看不到 "MIC_FE: ready"，
     *   紧接着下面那行 "pet session service ready (mic=...)" 又会去读**还没初始化**
     *   的 s_cfg（全零正好落在 BSS/MM/2 默认分支），回显得像"双麦已生效"。
     *   两个假象叠起来会让人以为改动没编进去。
     *   BSS 模式本函数零分配（只填 s_cfg），GSC 才 malloc PSRAM，提前调无成本。 */
    if (mic_fe_init(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "mic_frontend init failed (mic array falls back to raw 2ch)");
    }

    /* 12：按句流水时云端会在几百毫秒内连推 5~10 条 audio，队列浅了会在
     * WS 收包任务（prio 8）与 xz_task（prio 5）之间丢命令。 */
    s_cmd_q = xQueueCreate(12, sizeof(xz_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(12, xz_cmd_t)", 12 * sizeof(xz_cmd_t));
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
    /* ★ 网关地址是**可选项**：未配置时 ws_client_start() 按设计返回
     *   ESP_ERR_INVALID_STATE（"别让 supervisor 空转"）。
     *   以前这里用 ESP_ERROR_CHECK，未配网关的设备会在开机 init 阶段直接
     *   abort → 连续 3 次启动失败进 SAFE MODE（相机/音频/AI 全跳过）——
     *   这是把"没配网关"误判成"启动故障"。改为只告警，Web/相机照常可用。 */
    {
        esp_err_t wsr = ws_client_start();
        if (wsr != ESP_OK) {
            ESP_LOGW(TAG, "ws_client_start: %s (gateway disabled until configured)",
                     esp_err_to_name(wsr));
        }
    }

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

    ESP_LOGI(TAG, "pet session service ready (mic=%s layout=%s ch=%d, gateway=%s)",
             mic_fe_mode_name(), mic_fe_layout(), mic_fe_out_channels(),
             ws_client_get_session_id());
}

esp_err_t xiaozhi_service_start(void)
{
    if (s_state != XZ_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_service_mic_available() && !mic_gate_is_owner()) {
        /* 闸门持有租约时 audio_service_mic_available() 必然是 false；
         * 这不是"被别的功能占用"，而是本服务自己的监听态。
         * 真正的交接在 session_setup() 的 mic_gate_yield()，不能在这里提前拒绝。 */
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

esp_err_t xiaozhi_service_cloud_audio_play_ex(const char *url, const char *text)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= XZ_URL_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_state == XZ_STATE_CHATTING && s_play != NULL) {
        queue_play(url, XZ_KIND_TTS, text);
        return ESP_OK;
    }
    /* 会话不在：挂起一条，起会话；连上后由 on_ws_state 补播。
     * ★ 文字不排队（§40）：挂起路径没有 FIFO 可放 text，只能立刻抛出去。
     *   代价是"空闲时被云端主动推送"这一路文字会略早于声音（会话建立约 1 s）；
     *   会话内的正常对话走 queue_play，是严格对齐的。 */
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
    if (text != NULL && text[0] != '\0') {
        notify(XZ_EVT_TEXT, text, 1, ESP_OK);
    }
    return ESP_OK;
}

esp_err_t xiaozhi_service_cloud_audio_play(const char *url)
{
    return xiaozhi_service_cloud_audio_play_ex(url, NULL);
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
    queue_play(url, XZ_KIND_MUSIC, NULL);
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
