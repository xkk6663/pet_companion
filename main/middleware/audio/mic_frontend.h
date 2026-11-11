/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * mic_frontend.h —— 采集前端（单麦直通 / 双麦 BSS / GSC 波束，编译期选）
 *
 * ★★★ 2026-11-11 结案（上机五态硬件对照，P4）：
 *   本板**确认是两只独立模拟麦**，右路（ADC2）极性反相。证据链：
 *     ① copy21（0x23.b4）：corr ≡ +1.000 且 L/R RMS 相等 ⇒ 槽位映射
 *        （L=ADC1 / R=ADC2）与诊断统计都可信；
 *     ② hwRinv（0x25.b5）：corr −0.99 → **+0.961 / +0.640**，且 L/R RMS
 *        不相等 ⇒ 是"两路麦 + 右路反相"，不是"同一路的副本"；
 *     ③ muteADC2：L=369/455 不变、R 归零；muteADC1：L 归零、R=679 仍活
 *        ⇒ 两路输出物理独立，没有对调。
 *   ⇒ 默认模式 = **BSS**（AFE 内部的 SE/BSS 双麦盲源分离）。
 *     BSS 是**盲分离**，不需要麦间距/阵列几何 —— 这正是选它而不是 GSC 的原因
 *     （GSC 必须实量两麦中心距，本板还没量）。
 *   ⇒ 右路反相用 `CONFIG_MIC_FE_INVERT_R=y` 软件固化：驱动 es8389.c:257
 *     每次 codec open 都写 `0x25 = 0x0A + (0<<5)`，硬件位 0x25.bit5 不持久。
 *
 *   复测入口仍保留在 main/Kconfig.projbuild（P4 那组开关，默认全关）。
 *   详见 docs/项目规划/改动记录.md §28/§33/§34。
 *
 * 三种模式（menuconfig 的 MIC_FE_MODE_SEL 选）：
 *   MONO —— 只取 L，输出 1ch，layout "M"（**回退档**，BSS 效果不佳时用）；
 *   BSS  —— 2ch 直通交给 AFE，用 esp-sr 的 SE(BSS) 做双麦盲源分离（**默认**）；
 *   GSC  —— 本地 esp_gsc_* 波束成形（2 麦 → 1 声道），AFE 只做 VAD
 *           （需实量麦间距；间距错等于波束指歪）。
 *           ★ GSC 是 esp-sr 2026-08 才加进来的**独立模块**，不在 AFE 里，
 *             所以「AFE 自带双麦 BF，开个开关就行」只对了一半：AFE 里那个
 *             开关打开的是 BSS，不是波束成形。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIC_FE_MODE_MONO = 0,  /*!< 单麦直通（只取 L，1ch）—— BSS 效果不佳时的回退档 */
    MIC_FE_MODE_BSS  = 1,  /*!< 双麦直通 → AFE 内部 BSS（**默认**，已实测双麦成立） */
    MIC_FE_MODE_GSC  = 2,  /*!< 本地 GSC 波束成形 → 单声道再进 AFE（需实量麦间距） */
} mic_fe_mode_t;

typedef struct {
    mic_fe_mode_t mode;
    float mic_spacing_m;   /*!< GSC：两麦间距（米），**必须按实板量**，量错等于波束指歪 */
    float steer_phi_deg;   /*!< GSC：波束指向角（度）。0=+x（沿阵列轴线），90=+y（正前方） */
    int   sample_rate;     /*!< 固定 16000（GSC 帧长 128 点 = 8 ms） */
    bool  diag;            /*!< 周期打印 L/R RMS 与相关系数（复测双麦时才有意义） */
} mic_fe_cfg_t;

/* 默认：跟随 menuconfig（MIC_FE_MODE_SEL，当前 = BSS）+ 诊断开 */
#define MIC_FE_CFG_DEFAULT() {          \
    .mode           = MIC_FE_MODE_BSS,  \
    .mic_spacing_m  = 0.050f,           \
    .steer_phi_deg  = 90.0f,            \
    .sample_rate    = 16000,            \
    .diag           = true,             \
}

/**
 * @brief 初始化采集前端（PSRAM 缓冲，幂等）
 * @note  只做软件侧准备，不碰 codec 句柄；须在 audio_manager_init() 之前调用，
 *        因为 audio_manager 要用 mic_fe_layout()/mic_fe_out_channels() 决定
 *        rec_io 声道数与 AFE 输入布局。
 */
esp_err_t mic_fe_init(const mic_fe_cfg_t *cfg);

/** @brief 释放（关会话时调，可重复调） */
void mic_fe_deinit(void);

/** @brief AFE 输入布局串："M"（MONO/GSC，单声道）/ "MM"（BSS，双麦） */
const char *mic_fe_layout(void);

/** @brief 上游应声明的声道数：1（MONO/GSC）/ 2（BSS） */
int mic_fe_out_channels(void);

/** @brief 当前模式名（打日志用） */
const char *mic_fe_mode_name(void);

/**
 * @brief 取一块上行 PCM（**按 mic_fe_out_channels() 声道交错**）
 * @param out   目标缓冲
 * @param size  字节数（须为「帧数 × 声道数 × 2」的整数倍）
 * @param mic   已 open 成 2ch/16bit 的 codec dev
 */
esp_err_t mic_fe_read(uint8_t *out, int size, esp_codec_dev_handle_t mic);

/** @brief 主动打一次统计（不看周期也行） */
void mic_fe_dump_stats(void);

/**
 * @brief TTS 播放期间挂起 MIC 诊断（逐块统计 + 5 s 周期打印全停）
 *
 * 2026-11-10 实机：用户报「下行音频卡顿与 `MIC_FE: diag` 打出同时出现」。
 * `diag_scan_stereo()` 跑在 AFE feed 任务上下文，每 5 s 要做一次浮点
 * RMS/corr 与 UART 日志；而 TTS 期间麦克风收到的是**喇叭自己的回声**
 * （实测 L_rms 200~900），这段统计没有诊断价值，却与下行播放争
 * PSRAM / UART 全局日志锁。挂起后零统计、零日志；恢复时清空累加窗口，
 * 避免把 TTS 期间的样本混进下一个统计窗。
 *
 * ★ 只影响诊断，**不改变送给 AFE 的任何数据**（MONO 仍照常读 L 丢 R）。
 */
void mic_fe_set_diag_hold(bool hold);

/**
 * @brief 绑定 codec 并诊断 ES8389 的 ADC 输入选择
 *
 * ★ 为什么需要它：驱动默认把两个 ADC 都配成**差分**（0x72/0x73 sel=1）。
 *   `es8389_reg.h:185` 的枚举名 `DIFF_MIC2N2P` 与 ES8390 注释 `RIN2-LIN2`
 *   指向「麦 2 自己的一对、极性反接」，但同一行中文注释写 `Diff MIC2N-MIC1P`，
 *   两者矛盾 ⇒ corr≈−1 既可能是「副本」也可能是「反相的右麦」。
 *   用 menuconfig 的 MIC2P / INVERT_R 两个探针实机区分（§28）。
 *
 * @param mic   已 open 的 codec 句柄（nullptr 时安全返回）
 */
void mic_fe_attach_codec(esp_codec_dev_handle_t mic);

/**
 * @brief 运行时切到下一个 P4 探针态（长按 SET 键触发）
 *
 * 循环顺序（共 5 态）：
 *   all-off → copy21(0x23.b4) → hwRinv(0x25.b5) → muteR(0x20.b0) → muteL(0x20.b1) → 回到 all-off
 *
 * ★ 只有 `CONFIG_MIC_FE_PROBE_RUNTIME_SWITCH=y` 时才真正生效，否则只打一行提示。
 * ★ 切换是**可逆**的（会主动清位），所以能在同一次上电内反复 A/B —— 这正是它
 *   相比"每项重烧一版"最大的价值：排除了"每次重启环境不同"这个干扰变量。
 * ★ 会话未建立（无 codec 句柄）时只记状态，attach 时统一应用。
 */
void mic_fe_p4_next(void);

#ifdef __cplusplus
}
#endif
