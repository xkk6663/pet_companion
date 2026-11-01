/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.c
 * @brief A2DP Source 推送服务（方案 B「延迟常驻」）实现。
 *
 * 数据流：io_file(MP3) -> aud_dec -> aud_asrc -> aud_alc -> aud_enc(SBC) -> io_bt
 *
 * ── 方案 B 的四条原则（照抄 display_audio_photo 主线实现） ─────────
 *  P1 时机前置：pool/pipeline/task 全部在 bt_push_service_early_init() 里建，
 *     抢在 bsp_display_start() 之前（与 bt_audio_service_controller_init 同窗口）。
 *  P2 显式外置：GMF task 栈 stack_in_ext=true 进 PSRAM；uri/weight 缓冲用
 *     heap_caps_malloc(MALLOC_CAP_SPIRAM)，不在内部 RAM 上再啃一口。
 *  P3 建一次不毁：run/stop/reset 反复用同一个 pipeline+task；
 *     任何路径都不在 BT 事件回调里 destroy。
 *  P4 销毁异步：request_destroy() 只置标志 + 投递命令。
 *
 * ── 关键：所有会阻塞的 pipeline 操作都走命令队列 ─────────────────
 * stream 事件回调（BT/GMF 线程）只做「登记 + reconfig」这类非阻塞动作；
 * set_in_uri / loading_jobs / run / stop / reset 属阻塞语义，一律投递到
 * s_cmd_queue 由 s_proc_task 串行执行（与官方 bt_audio 例程同构）。
 *
 * ★ 本工程（pet_companion）相对主线差异：
 *   esp_bt_audio 本地组件版本（ref: esp32-xiaofeng-main）的
 *   esp_bt_audio_stream_codec_type_t 只有 UNKNOWN/SBC/LC3，**没有 AAC**，
 *   故 codec_to_audio_type() 删除 AAC 分支（否则编译报未定义枚举）。
 */

#include "bt_push_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>   /* 数字音量换算：log10f / lroundf */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_element.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_io_file.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_err.h"
#include "esp_gmf_alc.h"   /* 数字音量：ALC 元素的增益接口 */

#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_defs.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "media_lib_adapter.h"

static const char *TAG = "BT_PUSH";

#define URI_LEN                  256
#define ASRC_MAX_CH              4
#define ASRC_MAX_WEIGHT_LEN      (ASRC_MAX_CH * ASRC_MAX_CH)

#define CMD_QUEUE_SIZE           8
#define PROC_TASK_STACK          4096
#define PROC_TASK_PRIO           20
#define PROC_TASK_CORE           1

/* 命令类型：全部在 s_proc_task 上下文里执行 */
typedef enum {
    CMD_PREPARE = 0,   /* set_in_uri + loading_jobs */
    CMD_RUN,
    CMD_STOP_RESET,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_SEEK,          /* value = 目标毫秒 */
    CMD_SWITCH,        /* 换曲：链路已 STARTED，不能再调 media_start */
    CMD_DESTROY,
} push_cmd_action_t;

typedef struct {
    push_cmd_action_t action;
    int               value;
} push_cmd_t;

/* ---- 常驻对象（本文件唯一的一组全局对象，开机建一次） ---- */
static esp_gmf_pool_handle_t       s_pool  = NULL;
static esp_gmf_pipeline_handle_t   s_pipe  = NULL;
static esp_gmf_task_handle_t       s_task  = NULL;
static esp_bt_audio_stream_handle_t s_stream = NULL;

static QueueHandle_t s_cmd_queue = NULL;

/* uri 与 asrc 权重放 PSRAM，避免占内部 RAM（P2） */
static char  *s_uri = NULL;
static float *s_asrc_weight = NULL;

static volatile bool s_pipe_ready = false;    /* pool+pipe+task 建好 */
static volatile bool s_playing = false;       /* 正在推流 */
static volatile bool s_uri_pending = false;   /* 有待下发的 uri */
static volatile bool s_finished = false;      /* 本曲已自然播完，待上层收尾 */
static void (*s_finished_cb)(void) = NULL;    /* 播完回调（在 GMF 事件线程触发） */
static bool s_media_registered = false;

/* 数字音量：推流路径 SD→解码→ASRC→SBC 编码→蓝牙，不经过本地 codec，
 * 故在 aud_asrc 与 aud_enc 之间插 aud_alc 做编码前 PCM 的数字衰减。 */
static esp_gmf_element_handle_t s_alc_el = NULL;   /* pipeline 内的 aud_alc 实例 */
static int                      s_vol    = 100;    /* 本地档位 0..100，默认满幅 */

/* ------------------------------------------------------------------ */
/* 播放进度 / 曲目时长                                                 */
/* ------------------------------------------------------------------ */
/* ★ 为什么不用 seek/时间轴查询：A2DP Source 侧没有播放器句柄，耳机也不会
 *   回传播放进度。但 io_bt 是按 20 ms 节拍严格实时消费数据的，所以"墙钟"
 *   就是最好的进度来源（与音频时间轴误差 < 100 ms）。
 *   pause/resume 用「累计 + 本段起点」实现，暂停期间停表。 */
static portMUX_TYPE s_clk_mux      = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_clk_acc_ms   = 0;      /* 已累计的播放时长 */
static uint32_t     s_clk_start_ms = 0;      /* 本段起点（毫秒） */
static bool         s_clk_running  = false;
static int          s_duration_ms  = 0;      /* set_uri 时解析出的本曲时长 */

/* 本曲文件大小 / 音频数据起点（mp3_duration_ms 里一并记下）：
 * seek 时按「毫秒 → 字节」线性换算要用。 */
static long         s_file_size    = 0;
static long         s_audio_offset = 0;

/* proc_task 私有状态（只在 s_proc_task 上下文读写，不需要锁） */
static bool         s_pipe_paused     = false;
static int          s_pending_seek_ms = -1;  /* 暂停期间收到的 seek，恢复时补做 */

static inline uint32_t clk_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);   /* 49 天回绕，单曲无影响 */
}

static void clk_pause(void)
{
    portENTER_CRITICAL(&s_clk_mux);
    if (s_clk_running) {
        s_clk_acc_ms += clk_now_ms() - s_clk_start_ms;
        s_clk_running = false;
    }
    portEXIT_CRITICAL(&s_clk_mux);
}

static void clk_resume(void)
{
    portENTER_CRITICAL(&s_clk_mux);
    if (!s_clk_running) {
        s_clk_start_ms = clk_now_ms();
        s_clk_running = true;
    }
    portEXIT_CRITICAL(&s_clk_mux);
}

/* 新曲起播：进度归零并开表 */
static void clk_restart(void)
{
    portENTER_CRITICAL(&s_clk_mux);
    s_clk_acc_ms = 0;
    s_clk_start_ms = clk_now_ms();
    s_clk_running = true;
    portEXIT_CRITICAL(&s_clk_mux);
}

/* 播完/停止：停表（播完时把进度钉到末尾） */
static void clk_stop(bool at_end)
{
    portENTER_CRITICAL(&s_clk_mux);
    if (s_clk_running) {
        s_clk_acc_ms += clk_now_ms() - s_clk_start_ms;
        s_clk_running = false;
    }
    if (at_end && s_duration_ms > 0) {
        s_clk_acc_ms = (uint32_t)s_duration_ms;
    }
    portEXIT_CRITICAL(&s_clk_mux);
}

int bt_push_service_get_position_ms(void)
{
    uint32_t pos;
    portENTER_CRITICAL(&s_clk_mux);
    pos = s_clk_acc_ms;
    if (s_clk_running) {
        pos += clk_now_ms() - s_clk_start_ms;
    }
    portEXIT_CRITICAL(&s_clk_mux);

    if (s_duration_ms > 0 && (int)pos > s_duration_ms) {
        pos = (uint32_t)s_duration_ms;
    }
    return (int)pos;
}

int bt_push_service_get_duration_ms(void)
{
    return s_duration_ms;
}

/* ---- 极简 MP3 头解析：只为拿到本曲总时长 ----
 * 优先用 Xing/Info 的帧数（VBR 也准）；没有就从首帧码率按 CBR 估算。
 * 解析失败返回 0，UI 会退化成"只显示已播时间"。 */
typedef struct {
    int ver;        /* 1 = MPEG1, 2 = MPEG2, 25 = MPEG2.5 */
    int sr;
    int br_kbps;
    int frame_len;
    int channels;
} mp3_hdr_t;

static const int s_mp3_br_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int s_mp3_br_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const int s_mp3_sr_v1[3]  = {44100, 48000, 32000};
static const int s_mp3_sr_v2[3]  = {22050, 24000, 16000};
static const int s_mp3_sr_v25[3] = {11025, 12000, 8000};

static bool mp3_parse_hdr(const uint8_t *h, mp3_hdr_t *o)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return false;
    }
    int ver_bits   = (h[1] >> 3) & 0x3;   /* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
    int layer_bits = (h[1] >> 1) & 0x3;   /* 1 = Layer III */
    int br_idx     = (h[2] >> 4) & 0xF;
    int sr_idx     = (h[2] >> 2) & 0x3;
    int pad        = (h[2] >> 1) & 0x1;
    int ch_mode    = (h[3] >> 6) & 0x3;   /* 3 = mono */
    if (ver_bits == 1 || layer_bits == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return false;
    }
    if ((4 - layer_bits) != 3) {          /* 只认 Layer III（.mp3） */
        return false;
    }
    o->ver      = (ver_bits == 3) ? 1 : ((ver_bits == 2) ? 2 : 25);
    o->sr       = (o->ver == 1) ? s_mp3_sr_v1[sr_idx]
                : (o->ver == 2) ? s_mp3_sr_v2[sr_idx] : s_mp3_sr_v25[sr_idx];
    o->br_kbps  = (o->ver == 1) ? s_mp3_br_v1[br_idx] : s_mp3_br_v2[br_idx];
    o->channels = (ch_mode == 3) ? 1 : 2;
    int spf = (o->ver == 1) ? 1152 : 576;
    o->frame_len = (spf / 8) * o->br_kbps * 1000 / o->sr + pad;
    return (o->br_kbps > 0 && o->frame_len > 8);
}

/* ---- 首帧扫描缓冲：**必须放 PSRAM，不能放栈上** ----
 * ★ 2026-11-01 真机崩溃（Stack protection fault @ mp3_duration_ms:245）：
 *   本函数由 mp_task 调用（media_player_service.c:488 建的栈只有 3072 B），
 *   而 chunk[4096] + frame[1500] 合计 5.6 KB 的局部数组一进函数就冲爆栈。
 *   改为一次性从 PSRAM 取，之后常驻复用（每首歌只分配一次，不回收）。 */
#define MP3_SCAN_CHUNK     4096          /* 扫描窗口（前 128 KB 共 32 轮） */
#define MP3_FRAME_MAX      1500          /* MPEG1 320 kbps / 32 kHz 上限 1441 */

static uint8_t *s_scan_buf  = NULL;
static uint8_t *s_frame_buf = NULL;
/* seek 找帧头用的探针窗（PSRAM，首次 seek 时分配） */
#define MP3_SEEK_PROBE  8192
static uint8_t *s_seek_buf  = NULL;

static bool mp3_buf_prepare(void)
{
    if (s_scan_buf && s_frame_buf) {
        return true;
    }
    if (!s_scan_buf) {
        s_scan_buf = heap_caps_malloc(MP3_SCAN_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scan_buf) {
            ESP_LOGW(TAG, "no PSRAM for mp3 scan buf; duration unavailable");
            return false;
        }
    }
    if (!s_frame_buf) {
        s_frame_buf = heap_caps_malloc(MP3_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame_buf) {
            ESP_LOGW(TAG, "no PSRAM for mp3 frame buf; duration unavailable");
            return false;
        }
    }
    return true;
}

static int mp3_duration_ms(const char *path)
{
    s_file_size    = 0;
    s_audio_offset = 0;
    if (!mp3_buf_prepare()) {
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    int dur_ms = 0;
    long fsize = 0;
    mp3_hdr_t hdr = {0};
    long hdr_pos = -1;
    uint8_t *chunk = s_scan_buf;
    uint8_t *frame = s_frame_buf;

    do {
        if (fseek(f, 0, SEEK_END) != 0) break;
        fsize = ftell(f);
        if (fsize <= 0) break;
        s_file_size = fsize;
        if (fseek(f, 0, SEEK_SET) != 0) break;

        /* 跳过 ID3v2（大小是 syncsafe 编码） */
        uint8_t id3[10];
        if (fread(id3, 1, sizeof(id3), f) == sizeof(id3) && memcmp(id3, "ID3", 3) == 0) {
            long tag = (long)(((id3[6] & 0x7F) << 21) | ((id3[7] & 0x7F) << 14) |
                              ((id3[8] & 0x7F) << 7)  |  (id3[9] & 0x7F));
            if (fseek(f, 10 + tag, SEEK_SET) != 0) break;
        } else {
            if (fseek(f, 0, SEEK_SET) != 0) break;
        }

        /* 前 128 KB 内找第一个合法帧头 */
        for (int round = 0; round < 32 && hdr_pos < 0; round++) {
            size_t n = fread(chunk, 1, MP3_SCAN_CHUNK, f);
            if (n < 4) break;
            for (size_t i = 0; i + 4 <= n; i++) {
                if (mp3_parse_hdr(&chunk[i], &hdr)) {
                    hdr_pos = ftell(f) - (long)n + (long)i;
                    break;
                }
            }
        }
        if (hdr_pos < 0) break;
        s_audio_offset = hdr_pos;

        /* 首帧内找 Xing/Info（帧数 → 时长，VBR 也准） */
        int spf = (hdr.ver == 1) ? 1152 : 576;
        size_t flen = (hdr.frame_len < MP3_FRAME_MAX) ? (size_t)hdr.frame_len : MP3_FRAME_MAX;
        if (fseek(f, hdr_pos, SEEK_SET) != 0) break;
        size_t got = fread(frame, 1, flen, f);
        if (got < 12) break;

        int side = (hdr.ver == 1) ? (hdr.channels == 1 ? 17 : 32) : (hdr.channels == 1 ? 9 : 17);
        for (size_t off = (size_t)(4 + side); off + 12 <= got && off <= (size_t)(4 + side + 16); off++) {
            if (memcmp(frame + off, "Xing", 4) == 0 || memcmp(frame + off, "Info", 4) == 0) {
                uint32_t flags = ((uint32_t)frame[off + 4] << 24) | ((uint32_t)frame[off + 5] << 16) |
                                 ((uint32_t)frame[off + 6] << 8)  |  (uint32_t)frame[off + 7];
                if ((flags & 0x1) && off + 12 <= got) {
                    uint32_t nframes = ((uint32_t)frame[off + 8] << 24) | ((uint32_t)frame[off + 9] << 16) |
                                       ((uint32_t)frame[off + 10] << 8) |  (uint32_t)frame[off + 11];
                    if (nframes > 0 && nframes < 10000000U) {
                        dur_ms = (int)(((int64_t)nframes * spf * 1000) / hdr.sr);
                    }
                }
                break;
            }
        }
        if (dur_ms <= 0) {
            /* 无 Xing：按首帧码率当 CBR 估算（ms = bytes*8/kbps） */
            long bytes = fsize - hdr_pos;
            if (bytes > 0) {
                dur_ms = (int)((int64_t)bytes * 8 / hdr.br_kbps);
            }
        }
    } while (0);

    fclose(f);
    if (dur_ms < 0 || dur_ms > 6 * 3600 * 1000) {
        dur_ms = 0;
    }
    ESP_LOGI(TAG, "mp3 duration: %d ms (%s)", dur_ms, path);
    return dur_ms;
}

/* 目标字节若落在帧中间，向后找最近的合法帧头（并要求下一帧也合法）。
 * ★ 不做对齐的话，aud_dec 会从半个 MP3 帧开始解，可能直接报错让 pipeline
 *   进 ERROR（我们的 ERROR 分支等同"播完"，表现为"拖一下就断"）。 */
static uint64_t mp3_align_frame(const char *path, uint64_t pos)
{
    if (!s_seek_buf) {
        s_seek_buf = heap_caps_malloc(MP3_SEEK_PROBE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_seek_buf) {
            return pos;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return pos;
    }
    size_t n = 0;
    if (fseek(f, (long)pos, SEEK_SET) == 0) {
        n = fread(s_seek_buf, 1, MP3_SEEK_PROBE, f);
    }
    fclose(f);

    for (size_t i = 0; i + 4 <= n; i++) {
        mp3_hdr_t hdr;
        if (!mp3_parse_hdr(&s_seek_buf[i], &hdr)) {
            continue;
        }
        size_t nxt = i + (size_t)hdr.frame_len;
        if (nxt + 4 > n) {
            break;      /* 探针窗尾部，无法校验下一帧 */
        }
        mp3_hdr_t h2;
        if (!mp3_parse_hdr(&s_seek_buf[nxt], &h2)) {
            continue;
        }
        return pos + i;
    }
    return pos;         /* 找不到就按原位置交给解码器 */
}

/* 毫秒 → 字节并做帧对齐，然后在 IO 任务里跳过数据（esp_gmf_io_seek 会自动
 * abort 当前读并重置数据总线）。仅在 s_proc_task 上下文调用。 */
static void bt_push_do_seek_ms(int ms)
{
    if (!s_pipe || s_duration_ms <= 0 || s_file_size <= 0 || s_audio_offset < 0) {
        ESP_LOGW(TAG, "seek ignored (dur=%d size=%ld)", s_duration_ms, s_file_size);
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    if (ms > s_duration_ms) {
        ms = s_duration_ms;
    }

    uint64_t span = (uint64_t)(s_file_size - s_audio_offset);
    uint64_t pos  = (uint64_t)s_audio_offset +
                    span * (uint64_t)ms / (uint64_t)s_duration_ms;
    if (pos >= (uint64_t)s_file_size) {
        pos = (uint64_t)s_file_size - 1;
    }
    pos = mp3_align_frame(s_uri + 7 /* "file://" */, pos);

    esp_gmf_io_handle_t in = ESP_GMF_PIPELINE_GET_IN_INSTANCE(s_pipe);
    if (!in) {
        return;
    }
    if (esp_gmf_io_seek(in, pos) != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "seek to %d ms (byte %llu) failed", ms, (unsigned long long)pos);
        return;
    }

    /* 墙钟同步到目标点：seek 之后 IO 仍严格实时出数据 */
    portENTER_CRITICAL(&s_clk_mux);
    s_clk_acc_ms = (uint32_t)ms;
    s_clk_start_ms = clk_now_ms();
    s_clk_running = !s_pipe_paused;
    portEXIT_CRITICAL(&s_clk_mux);
    ESP_LOGI(TAG, "seek -> %d ms (byte %llu)", ms, (unsigned long long)pos);
}

/* 本地档位(0..100) → dB 增益。对数律贴合听感：
 *   100 -> 0 dB（不衰减）· 50 -> -6 dB · 25 -> -12 dB · 0 -> -64 dB（听不见） */
static int8_t vol_to_gain_db(int vol)
{
    if (vol <= 0) {
        return -64;
    }
    if (vol >= 100) {
        return 0;
    }
    float db = 20.0f * log10f((float)vol / 100.0f);
    if (db < -64.0f) {
        db = -64.0f;
    }
    return (int8_t)lroundf(db);
}

/* 把档位下发到 ALC 元素。
 * ★ 必须逐声道下发：ALC 是 per-channel 增益，只设 ch0 会造成左右不平衡。 */
static void alc_apply_gain(int vol)
{
    if (!s_alc_el) {
        return;
    }
    esp_ae_alc_cfg_t *cfg = (esp_ae_alc_cfg_t *)OBJ_GET_CFG(s_alc_el);
    uint8_t nch = (cfg && cfg->channel >= 1) ? cfg->channel : 2;
    if (nch > 2) {
        nch = 2;   /* 本链路最多立体声（ASRC dest_ch ≤ 2） */
    }
    int8_t db = vol_to_gain_db(vol);
    for (uint8_t i = 0; i < nch; i++) {
        esp_gmf_alc_set_gain(s_alc_el, i, db);
    }
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void register_media_defaults(void)
{
    if (s_media_registered) {
        return;
    }
    media_lib_add_default_adapter();
    esp_audio_dec_register_default();
    esp_audio_enc_register_default();
    s_media_registered = true;
}

/* ★ 本地组件版本无 AAC 枚举，只保留 SBC / LC3 */
static esp_audio_type_t codec_to_audio_type(esp_bt_audio_stream_codec_type_t t)
{
    switch (t) {
    case ESP_BT_AUDIO_STREAM_CODEC_SBC: return ESP_AUDIO_TYPE_SBC;
    case ESP_BT_AUDIO_STREAM_CODEC_LC3: return ESP_AUDIO_TYPE_LC3;
    default:                            return ESP_AUDIO_TYPE_UNSUPPORT;
    }
}

/* 按 src_ch -> dest_ch 生成 asrc 混音权重矩阵（与官方实现同构） */
static void fill_asrc_weight(float *weight, uint8_t src_ch, uint8_t dest_ch)
{
    if (!weight || src_ch == 0 || dest_ch == 0 ||
        src_ch > ASRC_MAX_CH || dest_ch > ASRC_MAX_CH) {
        return;
    }
    uint32_t len = (uint32_t)src_ch * dest_ch;
    memset(weight, 0, len * sizeof(weight[0]));

    for (uint8_t dest = 0; dest < dest_ch; dest++) {
        if (src_ch == dest_ch) {
            weight[dest * src_ch + dest] = 1.0f;
        } else if (src_ch == 1) {
            weight[dest] = 1.0f;                       /* 单声道 -> 多声道：复制 */
        } else if (dest_ch == 1) {
            for (uint8_t src = 0; src < src_ch; src++) {
                weight[src] = 1.0f / src_ch;           /* 多声道 -> 单声道：平均 */
            }
        } else {
            uint8_t pick = (dest < src_ch) ? dest : (src_ch - 1);
            weight[dest * src_ch + pick] = 1.0f;       /* 取前 N 路 */
        }
    }
}

/* 在 pipeline 里找第 index 个 aud_asrc 元素 */
static esp_gmf_element_handle_t find_asrc(uint8_t index)
{
    if (!s_pipe) {
        return NULL;
    }
    const void *iter = NULL;
    esp_gmf_element_handle_t el = NULL;
    uint8_t cnt = 0;
    while (esp_gmf_pipeline_iterate_element(s_pipe, &iter, &el) == ESP_GMF_ERR_OK) {
        char *tag = NULL;
        esp_gmf_obj_get_tag((esp_gmf_obj_handle_t)el, &tag);
        if (tag && strcasecmp(tag, "aud_asrc") == 0) {
            if (cnt == index) {
                return el;
            }
            cnt++;
        }
    }
    return NULL;
}

/* 配置 asrc 的目标采样率/声道及混音矩阵 */
static void set_asrc_dest(uint8_t index, uint32_t sample_rate, uint8_t src_ch, uint8_t dest_ch)
{
    esp_gmf_element_handle_t asrc = find_asrc(index);
    if (!asrc) {
        ESP_LOGW(TAG, "aud_asrc[%u] not found", index);
        return;
    }
    esp_gmf_asrc_set_dest_rate(asrc, sample_rate);
    esp_gmf_asrc_set_dest_ch(asrc, dest_ch);

    fill_asrc_weight(s_asrc_weight, src_ch, dest_ch);
    esp_asrc_cfg_t *cfg = (esp_asrc_cfg_t *)OBJ_GET_CFG(asrc);
    if (cfg) {
        cfg->weight = s_asrc_weight;
        cfg->weight_len = (uint32_t)src_ch * dest_ch;
    }
}

/* ------------------------------------------------------------------ */
/* 命令队列                                                            */
/* ------------------------------------------------------------------ */

static bool post_cmd_value(push_cmd_action_t action, int value)
{
    if (!s_cmd_queue) {
        return false;
    }
    push_cmd_t cmd = { .action = action, .value = value };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop action=%d", action);
        return false;
    }
    return true;
}

static bool post_cmd(push_cmd_action_t action)
{
    return post_cmd_value(action, 0);
}

/* ------------------------------------------------------------------ */
/* pipeline 事件回调                                                   */
/* ------------------------------------------------------------------ */

static esp_gmf_err_t pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (!pkt || pkt->type != ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->sub == ESP_GMF_EVENT_STATE_FINISHED) {
        ESP_LOGI(TAG, "playback finished");
        s_playing = false;
        clk_stop(true);    /* 停表并把进度钉到末尾 */
        /* 通知上层收尾（media_stop + UI 复位）。
           ★ 本函数跑在 GMF 事件线程，绝不能直接调阻塞的 media_stop，
             只置标志 + 回调；上层负责投递到安全上下文执行。 */
        s_finished = true;
        if (s_finished_cb) {
            s_finished_cb();
        }
    } else if (pkt->sub == ESP_GMF_EVENT_STATE_ERROR) {
        ESP_LOGE(TAG, "pipeline error");
        s_playing = false;
        clk_stop(false);
        s_finished = true;   /* 出错同样需要收尾，避免卡在"以为在播" */
        if (s_finished_cb) {
            s_finished_cb();
        }
    }
    return ESP_GMF_ERR_OK;
}

/* ------------------------------------------------------------------ */
/* 处理任务：串行执行所有阻塞的 pipeline 操作                          */
/* ------------------------------------------------------------------ */

static void proc_task(void *arg)
{
    (void)arg;
    push_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_pipe) {
            continue;
        }

        switch (cmd.action) {
        case CMD_PREPARE:
            if (s_uri_pending) {
                s_uri_pending = false;
                ESP_LOGI(TAG, "set uri: %s", s_uri);
                esp_gmf_pipeline_set_in_uri(s_pipe, s_uri);
                esp_gmf_pipeline_loading_jobs(s_pipe);
            }
            break;

        case CMD_RUN:
            ESP_LOGI(TAG, "pipeline run");
            s_pipe_paused = false;
            clk_restart();     /* 新曲从 0 起算，UI 进度条/歌词跟着走 */
            esp_gmf_pipeline_run(s_pipe);
            /* run 之后元素才拿到真实音频参数并可能 reopen 内部句柄，
               这里再下发一次音量，确保新开的 ALC 实例带上正确增益。 */
            alc_apply_gain(s_vol);
            break;

        case CMD_SWITCH:
            /* 换曲：A2DP media 已在 STARTED，组件拒绝再次 media_start
               （a2dp_src_media_start 要求 media_state==IDLE）。
               所以在常驻 pipeline 上换 uri 重跑。 */
            ESP_LOGI(TAG, "switch track: %s", s_uri);
            s_pipe_paused = false;
            s_pending_seek_ms = -1;
            esp_gmf_pipeline_stop(s_pipe);
            esp_gmf_pipeline_reset(s_pipe);
            clk_restart();
            esp_gmf_pipeline_set_in_uri(s_pipe, s_uri);
            esp_gmf_pipeline_loading_jobs(s_pipe);
            esp_gmf_pipeline_run(s_pipe);
            alc_apply_gain(s_vol);
            break;

        case CMD_STOP_RESET:
            ESP_LOGI(TAG, "pipeline stop+reset");
            s_pipe_paused = false;
            s_pending_seek_ms = -1;
            clk_stop(false);
            esp_gmf_pipeline_stop(s_pipe);
            esp_gmf_pipeline_reset(s_pipe);
            break;

        case CMD_PAUSE:
            s_pipe_paused = true;
            clk_pause();
            esp_gmf_pipeline_pause(s_pipe);
            break;

        case CMD_RESUME:
            s_pipe_paused = false;
            /* 暂停期间拖动的目标点在这里补做（暂停时 IO 任务可能不跑，
               直接 seek 会卡在等待 seek-done 位上）。 */
            if (s_pending_seek_ms >= 0) {
                bt_push_do_seek_ms(s_pending_seek_ms);
                s_pending_seek_ms = -1;
            }
            clk_resume();
            esp_gmf_pipeline_resume(s_pipe);
            break;

        case CMD_SEEK:
            if (s_pipe_paused) {
                s_pending_seek_ms = cmd.value;
            } else {
                bt_push_do_seek_ms(cmd.value);
            }
            break;

        case CMD_DESTROY:
            /* pipeline 常驻（P3/P4），此处不做销毁 */
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 开机早期：一次性建好 pool / pipeline / task                         */
/* ------------------------------------------------------------------ */

void bt_push_service_early_init(void)
{
    if (s_pipe_ready) {
        return;
    }

    ESP_LOGI(TAG, "early init: free internal=%d largest=%d free PSRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    register_media_defaults();

    /* uri / weight 放 PSRAM（P2） */
    s_uri = heap_caps_malloc(URI_LEN, MALLOC_CAP_SPIRAM);
    s_asrc_weight = heap_caps_calloc(ASRC_MAX_WEIGHT_LEN, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_uri || !s_asrc_weight) {
        ESP_LOGE(TAG, "PSRAM alloc failed (uri=%p weight=%p)", s_uri, s_asrc_weight);
        goto fail;
    }

    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool init failed");
        goto fail;
    }

    /* 注册元素到 pool：aud_dec / aud_asrc / aud_alc / aud_enc */
    esp_gmf_element_handle_t el = NULL;
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
    };
    if (esp_gmf_audio_dec_init(&dec_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_dec init failed");
        goto fail;
    }

    esp_asrc_cfg_t asrc_cfg = DEFAULT_ESP_GMF_ASRC_CONFIG();
    if (esp_gmf_asrc_init(&asrc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_asrc init failed");
        goto fail;
    }

    /* 数字音量元素：aud_alc 插在 asrc 与 enc 之间，
       作用于"已重采样到耳机协商参数"的 PCM，最省算力且不受编码器影响。 */
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_alc init failed");
        goto fail;
    }

    esp_audio_enc_config_t enc_cfg = {
        .type = ESP_AUDIO_TYPE_SBC,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (esp_gmf_audio_enc_init(&enc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_enc init failed");
        goto fail;
    }

    /* 注册 IO：io_file（输入） */
    esp_gmf_io_handle_t io = NULL;
    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    if (esp_gmf_io_file_init(&file_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_file init failed");
        goto fail;
    }

    /* 注册 IO：io_bt（输出） */
    bt_io_cfg_t bt_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    if (esp_gmf_io_bt_init(&bt_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_bt init failed");
        goto fail;
    }

    /* 建 pipeline：io_file -> aud_dec -> aud_asrc -> aud_alc -> aud_enc -> io_bt */
    const char *el_names[] = {"aud_dec", "aud_asrc", "aud_alc", "aud_enc"};
    if (esp_gmf_pool_new_pipeline(s_pool, "io_file", el_names, 4, "io_bt", &s_pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "new_pipeline failed");
        goto fail;
    }
    esp_gmf_pipeline_set_event(s_pipe, pipe_event_cb, NULL);

    /* 取管内 aud_alc 实例句柄（音量控制要用），并把已存档位下发一次。 */
    if (esp_gmf_pipeline_get_el_by_name(s_pipe, "aud_alc", &s_alc_el) != ESP_GMF_ERR_OK
        || s_alc_el == NULL) {
        s_alc_el = NULL;
        ESP_LOGW(TAG, "aud_alc not in pipeline; digital volume unavailable");
    } else {
        alc_apply_gain(s_vol);
    }

    /* 建 GMF task：栈进 PSRAM（P2），核 1，prio 15 */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.core = 1;
    task_cfg.thread.stack = 5120;
    task_cfg.thread.prio = 15;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "local2bt_task";
    if (esp_gmf_task_init(&task_cfg, &s_task) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task init failed");
        goto fail;
    }
    esp_gmf_pipeline_bind_task(s_pipe, s_task);

    /* 处理任务：所有阻塞 pipeline 操作都在这里串行跑 */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(push_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "cmd queue create failed");
        goto fail;
    }
    if (xTaskCreatePinnedToCoreWithCaps(proc_task, "bt_push_proc", PROC_TASK_STACK, NULL,
                                        PROC_TASK_PRIO, NULL, PROC_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "proc task create failed");
        goto fail;
    }

    s_pipe_ready = true;
    ESP_LOGI(TAG, "ready (early, resident): internal free=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return;

fail:
    /* 蓝牙非必需功能：失败只打日志，不 abort */
    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    if (s_task) {
        esp_gmf_task_deinit(s_task);
        s_task = NULL;
    }
    if (s_pipe) {
        esp_gmf_pipeline_destroy(s_pipe);
        s_pipe = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    if (s_uri) {
        heap_caps_free(s_uri);
        s_uri = NULL;
    }
    if (s_asrc_weight) {
        heap_caps_free(s_asrc_weight);
        s_asrc_weight = NULL;
    }
    ESP_LOGE(TAG, "early init FAILED, BT push disabled");
}

/* ------------------------------------------------------------------ */
/* 对外控制接口                                                        */
/* ------------------------------------------------------------------ */

esp_err_t bt_push_service_set_uri(const char *path)
{
    if (!path || !s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* io_file 需要 file:// 前缀 */
    snprintf(s_uri, URI_LEN, "file://%s", path);
    s_finished = false;   /* 新一曲开始：清掉上一曲的播完标志 */

    /* 本曲时长只在这里解析一次（pipeline 起播后再读文件会被 io_file 占用） */
    s_duration_ms = mp3_duration_ms(path);
    clk_stop(false);      /* 先把上一曲的表停掉，RUN 时再从 0 重新开表 */
    portENTER_CRITICAL(&s_clk_mux);
    s_clk_acc_ms = 0;
    portEXIT_CRITICAL(&s_clk_mux);

    ESP_LOGI(TAG, "uri queued: %s", s_uri);

    /* ★ 链路已在推流（含"上一曲自然播完但 A2DP 仍停在 STARTED"）：
     *   不能再调 media_start —— a2dp_src_media_start 要求 media_state==IDLE，
     *   否则返回 ESP_ERR_INVALID_STATE，表现就是"上/下一曲点了没反应"。
     *   改为在常驻 pipeline 上换 uri 重跑（CMD_SWITCH）。 */
    if (s_playing || s_stream != NULL) {
        s_uri_pending = false;
        return post_cmd(CMD_SWITCH) ? ESP_OK : ESP_FAIL;
    }

    s_uri_pending = true;

    /* 触发 A2DP 流；真正的 pipeline 准备在 stream ALLOCATED 事件里做 */
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    ESP_LOGI(TAG, "media_start ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bt_push_service_pause(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_cmd(CMD_PAUSE) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_push_service_resume(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_cmd(CMD_RESUME) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_push_service_seek_ms(int ms)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 阻塞的 pipeline/IO 操作一律走命令队列（见文件头约束） */
    return post_cmd_value(CMD_SEEK, ms) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_push_service_stop(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_playing = false;
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    return post_cmd(CMD_STOP_RESET) ? ESP_OK : ESP_FAIL;
}

bool bt_push_service_is_playing(void)
{
    return s_playing;
}

/* 播完（或出错）收尾支持 */
void bt_push_service_set_finished_cb(void (*cb)(void))
{
    s_finished_cb = cb;
}

bool bt_push_service_is_finished(void)
{
    return s_finished;
}

void bt_push_service_clear_finished(void)
{
    s_finished = false;
}

/* 数字音量入口：把本地档位(0..100)转成编码前 PCM 的 dB 增益。 */
esp_err_t bt_push_service_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_vol = vol;
    if (!s_alc_el) {
        /* pipeline 尚未建好（early_init 失败或还没跑）——
           档位已记下，元素就绪时由 early_init 末尾统一下发。 */
        return ESP_ERR_INVALID_STATE;
    }
    alc_apply_gain(vol);
    ESP_LOGI(TAG, "digital vol %d -> %d dB", vol, (int)vol_to_gain_db(vol));
    return ESP_OK;
}

void bt_push_service_request_destroy(void)
{
    /* P4：这里只投递命令，销毁动作在 proc_task 上下文里做 */
    if (s_pipe_ready) {
        post_cmd(CMD_STOP_RESET);
    }
    ESP_LOGI(TAG, "destroy requested (pipeline stays resident by design)");
}

/* ------------------------------------------------------------------ */
/* stream 状态事件（跑在 BT/GMF 线程）                                 */
/* ------------------------------------------------------------------ */

void bt_push_service_on_stream_state(void *stream, int state)
{
    if (!s_pipe_ready || !s_pipe || !stream) {
        return;
    }
    esp_bt_audio_stream_handle_t st = (esp_bt_audio_stream_handle_t)stream;

    switch (state) {
    case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED: {
        s_stream = st;
        ESP_LOGI(TAG, "stream ALLOCATED, prepare pipeline");

        /* 绑 BT stream 到 io_bt（非阻塞，可在此做） */
        esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(s_pipe), st);

        /* 取耳机协商的 codec 参数 */
        esp_bt_audio_stream_codec_info_t ci = {0};
        if (esp_bt_audio_stream_get_codec_info(st, &ci) != ESP_OK) {
            ESP_LOGE(TAG, "get codec info failed");
            return;
        }
        uint8_t ch = (uint8_t)__builtin_popcount(ci.channels);
        if (ch == 0) {
            ch = 2;
        }
        ESP_LOGI(TAG, "codec type=%d sr=%d ch=%d bits=%d",
                 ci.codec_type, ci.sample_rate, ch, ci.bits);

        /* asrc：把源（MP3 解码输出，按立体声 44.1k 交底）转到耳机协商参数 */
        set_asrc_dest(0, ci.sample_rate, 2, ch);

        /* 声道数可能随本曲的 codec 协商结果变化，按新声道数补一次增益 */
        alc_apply_gain(s_vol);

        /* encoder：耳机协商的 codec（SBC） */
        esp_audio_enc_config_t enc_cfg = {
            .type = codec_to_audio_type(ci.codec_type),
            .cfg = ci.codec_cfg,
            .cfg_sz = ci.cfg_size,
        };
        esp_gmf_audio_enc_reconfig(ESP_GMF_PIPELINE_GET_LAST_ELEMENT(s_pipe), &enc_cfg);

        /* set_in_uri + loading_jobs 走命令队列（可能阻塞） */
        post_cmd(CMD_PREPARE);
        break;
    }

    case ESP_BT_AUDIO_STREAM_STATE_STARTED:
        ESP_LOGI(TAG, "stream STARTED");
        s_playing = true;
        post_cmd(CMD_RUN);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        ESP_LOGI(TAG, "stream STOPPED");
        s_playing = false;
        post_cmd(CMD_STOP_RESET);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
        ESP_LOGI(TAG, "stream RELEASED");
        s_playing = false;
        if (s_stream == st) {
            s_stream = NULL;
        }
        /* 不 destroy pipeline（P3/P4） */
        break;

    default:
        break;
    }
}
