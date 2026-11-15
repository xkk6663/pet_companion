/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * mic_frontend.c —— 采集前端（单麦直通 / 双麦 BSS / GSC 波束）
 *
 * ┌ 为什么要有这一层 ────────────────────────────────────────────────
 * │ 2026-11-10 复核代码得出的事实链：
 * │  1) BSP README：ESP32-S31-Korvo-1 V1.1 = "dual-microphone array"；
 * │  2) es8389.c:527/528 同时配 ADCL/ADCR 的 PGA，驱动支持 ch0/ch1 独立增益；
 * │  3) BSP 的 I2S 是 STEREO 槽（bsp_audio.c:34），mic dev 已按 2ch open；
 * │  4) 但本工程原 read_cb 里 `out[i] = scratch[i*2]` —— **只取 L，R 直接丢**。
 * │
 * ├ ★★★ 2026-11-11 结案：双麦成立，右路反相，默认走 BSS ────────────
 * │  P4 五态硬件对照（一版固件 + 长按 SET 循环）实测结果：
 * │    ① all-off      corr = −0.509 / −0.991 / −0.995（基线）
 * │    ② copy21 0x23.b4 corr ≡ +1.000，L/R RMS 相等
 * │       ⇒ 诊断统计与槽位映射（L=ADC1 / R=ADC2）可信；
 * │    ③ hwRinv 0x25.b5 corr = **+0.961 / +0.640**，且 L/R RMS 不相等
 * │       ⇒ 两路是独立麦克风，右路只是极性反（副本绝不可能变成 0.6~0.96）；
 * │    ④ muteR 0x20.b0   L=369/455 不变、R=0
 * │    ⑤ muteL 0x20.b1   L=0、R=679 仍活
 * │       ⇒ 两路输出物理独立、没有对调；回到 all-off 三寄存器精确复位。
 * │
 * │  ⇒ 默认模式改 **BSS**：AFE 的 SE(BSS) 做双麦盲源分离，**不需要麦间距**，
 * │    是当前唯一能直接用的双麦算法（GSC 要实量间距）。右路极性用
 * │    `CONFIG_MIC_FE_INVERT_R=y` 软件固化（硬件 0x25.b5 每次 open 被驱动清掉）。
 * │  ⇒ MONO 保留为回退档：若 BSS 在近场/小房间反而更差，改回 MONO 即可。
 * │
 * ├ 2026-11-11 §28 二次更正：**官方板子没问题，之前把「反相」误判成「副本」**
 * │  官方 user_guide V1.1 明写 Left/Right Microphone 两只麦 + ES8389 双通道 ADC；
 * │  官方原理图 pin24 MIC1P / 23 MIC1N / 22 MIC2P / 21 MIC2N 各挂 1uF 耦合电容；
 * │  本工程 BSP / es8389.c / es8389_reg.h 与官方参考工程**逐字节相同**，
 * │  I2S 也同为 STD STEREO ⇒ 不存在「漏选 sdkconfig / 板子被阉割」。
 * │
 * │  旧判据 corr ≈ −0.885~−0.997 被当成「R = −MIC1P 是同一路的副本」。
 * │  但随后实机出现 corr = −0.419 / −0.691 且 L_rms ≠ R_rms 的窗口
 * │  （L=318/R=387、L=527/R=488）——真副本只可能恒为 −1.00。
 * │  ⇒ 两路是**独立麦克风**，负号来自 ADCR 差分输入的极性：
 * │    枚举名 ES8389_ADC_INSEL_R_DIFF_MIC2N2P / ES8390 注释 RIN2-LIN2
 * │    都指向「麦 2 自己的一对反着接」，而 Espressif 注释写成 MIC2N−MIC1P 有歧义。
 * │
 * │  ★ 结论待实机确认，故**默认仍 MONO**（零风险）；验证开关全部放进
 * │    main/Kconfig.projbuild：menuconfig 选 MIC2P 复测 / INVERT_R 翻极性，
 * │    corr 转正即证明双麦成立，再考虑切 BSS / GSC。详见改动记录 §28。
 * │
 * ├ 另一件必须说清的事 ────────────────────────────────────────────
 * │  esp-sr 的 AFE 里**没有 BF 开关**。
 * │  AFE 集成的数组处理叫 SE(BSS，盲源分离)，由 afe_config->se_init 控制；
 * │  真正的波束成形是 2026-08 才加的独立模块 esp_gsc_*（esp_gsc.h），
 * │  不进 AFE、不在 av_processor 里，要自己接。所以这里才留三条路。
 * └────────────────────────────────────────────────────────────────
 *
 * ★ 内存纪律（本工程铁律）：本层所有缓冲都在 PSRAM，内部 RAM 只放固件常量。
 *   内部 RAM 余量只剩 ~4 KB，任何 heap_caps_malloc(INTERNAL) 都可能把 WiFi 撞死。
 */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "sdkconfig.h"   /* CONFIG_MIC_FE_*：main/Kconfig.projbuild */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "mic_frontend.h"
#include "mic_monitor.h"     /* REC 双麦可视化：复用原始 L/R 采样 */
#include "esp_gsc.h"        /* esp_gsc_create/process/destroy（esp-sr, S31 有导出） */

static const char *TAG = "MIC_FE";

/* ============================================================
 * ★ 工作模式：由 menuconfig 的 MIC_FE_MODE_SEL 决定，默认 MONO。
 *   idf.py menuconfig → "麦克风前端 / MIC Frontend（双麦复测探针）"
 *
 *   BSS/GSC 需要两路独立信号；切之前先按 §28 完成 MIC2P 复测 + INVERT_R 验证。
 *   GSC 另需：1) 实量两麦中心距；2) 阵列沿板边左右排布时正前方 = +y ⇒ phi=90°。
 * ============================================================ */
#if defined(CONFIG_MIC_FE_MODE_SEL_BSS)
#define MIC_FE_MODE             MIC_FE_MODE_BSS
#elif defined(CONFIG_MIC_FE_MODE_SEL_GSC)
#define MIC_FE_MODE             MIC_FE_MODE_GSC
#else
#define MIC_FE_MODE             MIC_FE_MODE_MONO
#endif
#define MIC_FE_MIC_SPACING_M    0.050f      /* 待实测 */
#define MIC_FE_STEER_PHI_DEG    90.0f       /* 90° = 正前方（+y） */
#define MIC_FE_DIAG_ENABLE      true

/* GSC 帧长固定 128 点（esp_gsc.h:69-80），16 kHz ⇒ 8 ms */
#define GSC_FRAME_LEN           128

/* 输出 FIFO：一次回调最多 1920 帧（XZ_REC_READ_SIZE=3840B / 2），留 2 倍余量 */
#define OUT_FIFO_SAMPLES        4096

/* codec 单次读上限（字节）：够大减少 syscall，又不用大缓冲 */
#define READ_CHUNK_MAX          8192

/* ============================================================
 * 状态
 * ============================================================ */
static mic_fe_cfg_t   s_cfg;
static bool           s_ready;

static int16_t       *s_ilv;        /* 128 帧 × 2ch 交错，PSRAM（仅 GSC） */
static int16_t       *s_planar;     /* 128 帧 × 2ch 分平面，PSRAM（仅 GSC） */
static int16_t       *s_fifo;       /* 单声道输出环，PSRAM（仅 GSC） */
static int            s_fifo_rd;
static int            s_fifo_cnt;

static gsc_handle_t  *s_gsc;

/* 诊断累加器（L/R RMS + 相关系数）
 * ★★ s_acc_lr 必须是**带符号**：l*r 有正有负，早先写成 uint64_t，
 *    负数被补码扩展成 ~1.8e19，累加后 corr 直接变成 69622965047 这种天文数字
 *    （2026-11-10 实机踩到）。相关系数只可能在 [-1, 1]，超出必是类型错。 */
static uint64_t       s_acc_l2, s_acc_r2;
static int64_t        s_acc_lr;
static uint32_t       s_acc_n;
static int64_t        s_stats_last_us;

/* 窗口内峰值（绝对值），用于区分"没人说话"和"在说话但幅度小" */
static int32_t        s_peak_l, s_peak_r;

/* TTS 播放期间挂起诊断：见 mic_fe_set_diag_hold() 的说明。
 * volatile：由播放任务的事件回调置位，AFE feed 任务读。 */
static volatile bool  s_diag_hold;

/* 清空统计窗口并重新起算 5 s。
 * ★ 换探针态时必须调用：窗口是"5 s 音频时间"累加的，若在窗口中途切态，
 *   下一条 diag 会混着两个态的数据（前半段旧态、后半段新态），corr/RMS 两者都不代表，
 *   早期实验里那种"同态两次结果不一样"有一半是这么来的。 */
static void diag_window_reset(void)
{
    s_acc_l2 = s_acc_r2 = 0;
    s_acc_lr = 0;
    s_acc_n  = 0;
    s_peak_l = s_peak_r = 0;
    s_stats_last_us = esp_timer_get_time();
}

/* 本层记住的 mic codec 句柄：仅复测模式用（别处重开 mic 会冲掉输入选择） */
static esp_codec_dev_handle_t s_codec;

/* 静音门限：低于此 RMS 视为底噪，不参与相关统计（底噪的 corr 无物理意义） */
#define DIAG_ACTIVE_RMS   150.0

/* ============================================================
 * ★★ ES8389 ADC 输入选择（2026-11-10 二次复核：原理图到手后修正过一次结论）
 *
 *   引脚（ES8389，I2C 0x20）：
 *     pin24 MIC1P / pin23 MIC1N / pin22 MIC2P / pin21 MIC2N
 *     四根都有 1uF 耦合电容 → 参考设计是**两只差分模拟麦**。
 *
 *   ADC 输入 mux（es8389_reg.h:184-198，0x72=ADCL / 0x73=ADCR，bit[6:4]，bit[3:0]=PGA）：
 *     ADCL: 1 = MIC1P−MIC1N（差分）   5 = 单端 MIC1P   6 = 单端 **MIC2P**
 *     ADCR: 1 = MIC2N−MIC1P（跨话筒） 5 = 单端 MIC2N   6 = 单端 MIC1N
 *
 *   ★★ 上一轮的错误：把两路都设成 5 ⇒ 实际测的是 **MIC1P / MIC2N**，
 *      **MIC2P 从来没被测过**。而第二只麦只可能挂在 MIC2P 上（MIC2N 是它的负端）。
 *      所以"本板单麦"这个结论只证明了 **MIC2N 上没有独立信号**，不能推出 MIC2P 也没有。
 *
 *   已实测（2026-11-10）：
 *     差分（1/1）：L 198~2330，R 244~3585，corr = −0.885 ~ −0.997
 *                  ⇒ R ≈ −MIC1P，说明 ADCR 差分里的 MIC2N 项≈0（若 mic2 存在且说话，
 *                    两麦相距 5 cm 应强正相关，不可能是 −0.99）
 *     单端（5/5）：L 283~3855，R 2~3 / peak 6~15（R 只剩底噪）
 *                  ⇒ MIC2N 确认无信号
 *   待测：单端（6/x）读 MIC2P —— 这是唯一还没排除的第二麦候选点。
 *
 *   ★★ §28 更正：单端确实取不到 MIC2P，但**差分档可能本来就能双麦同采**。
 *     枚举名 `ES8389_ADC_INSEL_R_DIFF_MIC2N2P` 与同一行的 ES8390 注释
 *     `RIN2-LIN2` 都指向「麦 2 自己的一对」，只是**极性相反**（N−P 而非 P−N）；
 *     Espressif 那句 `Diff MIC2N-MIC1P` 与枚举名自相矛盾，不能当定论。
 *     若 R 差分 = −mic2，则 corr ≈ −1 完全正常，软件把 R 取负即可还原双麦。
 *     → 这就是 CONFIG_MIC_FE_INVERT_R 这个探针要验证的假设。
 * ============================================================ */
#define MIC_FE_ADC_SEL_AUTO       0   /* 不碰驱动默认（= 差分） */
#define MIC_FE_ADC_SEL_DIFF       1   /* ADCL: MIC1P−MIC1N | ADCR: MIC2N−MIC1P */
#define MIC_FE_ADC_SEL_SE_1P2N    5   /* ADCL: MIC1P       | ADCR: MIC2N */
#define MIC_FE_ADC_SEL_SE_2P1N    6   /* ADCL: MIC2P       | ADCR: MIC1N */

/* 复测入口：menuconfig 里选，不再手改宏。正常固件 = AUTO / 不翻转。 */
#if defined(CONFIG_MIC_FE_ADC_PROBE_SEL_MIC2P)
#define MIC_FE_ADC_TEST_L   MIC_FE_ADC_SEL_SE_2P1N   /* ADCL → 单端 MIC2P */
#define MIC_FE_ADC_TEST_R   MIC_FE_ADC_SEL_AUTO
#elif defined(CONFIG_MIC_FE_ADC_PROBE_SEL_SE_1P2N)
#define MIC_FE_ADC_TEST_L   MIC_FE_ADC_SEL_SE_1P2N
#define MIC_FE_ADC_TEST_R   MIC_FE_ADC_SEL_SE_1P2N
#else
#define MIC_FE_ADC_TEST_L   MIC_FE_ADC_SEL_AUTO
#define MIC_FE_ADC_TEST_R   MIC_FE_ADC_SEL_AUTO
#endif

#ifdef CONFIG_MIC_FE_INVERT_R
#define MIC_FE_INVERT_R   1
#else
#define MIC_FE_INVERT_R   0
#endif

#define ES8389_REG_ADCL_SEL       0x72
#define ES8389_REG_ADCR_SEL       0x73

/* ============================================================
 * ★★ P4 硬件级对照实验（2026-11-11 落地，menuconfig 默认全关）
 *
 *   目的不是"开功能"，而是先用**已知答案的硬件行为**把两件事钉死：
 *     ① 诊断统计与槽位映射（L=ADC1 / R=ADC2）到底可不可信；
 *     ② R 的反相是 codec 通路里的极性，还是软件/槽位偏差。
 *   只有这两件被对照实验确认，P1~P3 的 corr 才有解释力。
 *
 *   寄存器位（ES8389 datasheet 逐条，页 22~23 可查）：
 *     0x20 ADC SP CONTROL（默认 0x00）
 *          bit1 ADC1_P2S_SWMUTE  1 = ADC1 输出静音 → L 槽归零
 *          bit0 ADC2_P2S_SWMUTE  1 = ADC2 输出静音 → R 槽归零
 *     0x23 ADC MODE CONTROL（默认 0x00）
 *          bit4 ADC2_CF_SRCSEL   1 = ADC1 数据复制到 ADC2（R = L 严格副本）
 *          ★ 绝不能碰 bit5（ADC1_CF_SRCSEL），那会把 L 反过来覆盖掉
 *     0x25 ADC HPF CONTROL 2（默认 0x06）
 *          bit5 ADC2_180C        1 = ADC2 输出反相（硬件翻极性）
 *          ★ bit4:0 是 HPF coef2，必须原样保留 ⇒ 只做 RMW
 *     0x64 / 0x6D 只读回（模拟使能 / bias+MODF），用来排除"某一路被关掉"
 *
 *   ★ 全部读-改-写，且**只在目标位真的不同时才写**：attach 时写一次，
 *     diag 每 5 s 补写一次（audio_service / rec_local_service 重开 mic 会把
 *     这些寄存器冲回默认）。开关关掉后不主动清位，靠 codec 重开复位。
 * ============================================================ */
#define ES8389_REG_ADC_SP_CTRL    0x20
#define ES8389_REG_ADC_MODE_CTRL  0x23
#define ES8389_REG_ADC_HPF2       0x25
#define ES8389_REG_ADC_ANA_EN     0x64
#define ES8389_REG_ADC_ANA_CTRL1  0x6D

#define ES8389_ADC1_P2S_MUTE      (1 << 1)
#define ES8389_ADC2_P2S_MUTE      (1 << 0)
#define ES8389_ADC2_FROM_ADC1     (1 << 4)
#define ES8389_ADC2_180C          (1 << 5)

#ifdef CONFIG_MIC_FE_PROBE_ADC2_FROM_ADC1
#define MIC_FE_P4_COPY21   1
#else
#define MIC_FE_P4_COPY21   0
#endif
#ifdef CONFIG_MIC_FE_PROBE_HW_INVERT_R
#define MIC_FE_P4_HWRINV   1
#else
#define MIC_FE_P4_HWRINV   0
#endif
#ifdef CONFIG_MIC_FE_PROBE_MUTE_ADC1
#define MIC_FE_P4_MUTE1    1
#else
#define MIC_FE_P4_MUTE1    0
#endif
#ifdef CONFIG_MIC_FE_PROBE_MUTE_ADC2
#define MIC_FE_P4_MUTE2    1
#else
#define MIC_FE_P4_MUTE2    0
#endif

/* 加法而不是或：这些宏都是 0/1，加法结果可直接给 #if 用 */
#define MIC_FE_P4_ACTIVE   (MIC_FE_P4_COPY21 + MIC_FE_P4_HWRINV + \
                            MIC_FE_P4_MUTE1 + MIC_FE_P4_MUTE2)

/* 运行时切换（长按 SET 循环切探针）：一版固件跑完全部对照实验。
 * ★ 与上面四个编译期开关的分工：编译期只给**开机初值**，一旦长按过就由
 *   运行时状态接管 —— 否则同一份代码里两套真相，排障时必然对不上。 */
#ifdef CONFIG_MIC_FE_PROBE_RUNTIME_SWITCH
#define MIC_FE_P4_RT       1
#else
#define MIC_FE_P4_RT       0
#endif

/* 探针位（运行时状态用位掩码表达，便于"设位 + 清位"一次算完） */
#define P4_BIT_COPY21      (1u << 0)
#define P4_BIT_HWRINV      (1u << 1)
#define P4_BIT_MUTE2       (1u << 2)    /* 静 ADC2 = R 槽 */
#define P4_BIT_MUTE1       (1u << 3)    /* 静 ADC1 = L 槽（上行会没声） */

/* 编译期开关折叠成的开机初值 */
#define MIC_FE_P4_BOOT_MASK \
    ((MIC_FE_P4_COPY21 ? P4_BIT_COPY21 : 0u) | \
     (MIC_FE_P4_HWRINV ? P4_BIT_HWRINV : 0u) | \
     (MIC_FE_P4_MUTE1  ? P4_BIT_MUTE1  : 0u) | \
     (MIC_FE_P4_MUTE2  ? P4_BIT_MUTE2  : 0u))

/* 只要有一项可能改寄存器，就得把执行体编进来 */
#define MIC_FE_P4_NEED_CODE (MIC_FE_P4_ACTIVE || MIC_FE_P4_RT)

/* R 极性翻转：三个采集入口统一在「刚从 codec 读出来」时调用。
 * ★ 放在数据层而不是 diag 层：diag 直接看翻转后的数据，不会重复取负；
 *   MONO 下虽然 R 不进 AFE，但诊断窗口里 corr 会跟着变号 ⇒ 零风险验证。
 * ★ -32768 取负会溢出 int16，钳到 32767（只影响极值单帧）。 */
static void invert_r_inplace(int16_t *ilv, int frames)
{
#if MIC_FE_INVERT_R
    for (int i = 0; i < frames; i++) {
        int32_t r = ilv[2 * i + 1];
        ilv[2 * i + 1] = (r == -32768) ? (int16_t)32767 : (int16_t)(-r);
    }
#else
    (void)ilv;
    (void)frames;
#endif
}

/* L/R 的同一个码字含义不同，名字必须分通道给 */
static const char *adc_sel_name_l(int sel)
{
    switch (sel) {
    case 0: return "off/0";
    case MIC_FE_ADC_SEL_DIFF:   return "diff MIC1P-MIC1N";
    case MIC_FE_ADC_SEL_SE_1P2N: return "single MIC1P";
    case MIC_FE_ADC_SEL_SE_2P1N: return "single MIC2P";
    default: return "?";
    }
}

static const char *adc_sel_name_r(int sel)
{
    switch (sel) {
    case 0: return "off/0";
    case MIC_FE_ADC_SEL_DIFF:   return "diff MIC2N-MIC1P";
    case MIC_FE_ADC_SEL_SE_1P2N: return "single MIC2N";
    case MIC_FE_ADC_SEL_SE_2P1N: return "single MIC1N";
    default: return "?";
    }
}

/* 复测开关的执行体。AUTO 时什么都不做（返回 false）。
 *
 * ★ 为什么要能被反复调：`audio_service.c:89` / `rec_local_service.c:265` 也会
 *   `set_in_gain`，而驱动 `es8389_apply_mic_pga_gain()` 是用**编译期常量**
 *   `ES8389_ADCL_InputSel/ADCR_InputSel` 拼整个字节写 0x72/0x73
 *   （es8389.c:221/226）⇒ 只要另一个模块重开过 mic，我们的选择就被冲回差分。
 *   所以 attach 时写一次，diag 周期再补写一次（幂等：已是目标值就不写、不刷日志）。 */
static bool adc_apply_test_sel(esp_codec_dev_handle_t mic)
{
    if (mic == NULL) {
        return false;
    }
    if (MIC_FE_ADC_TEST_L == MIC_FE_ADC_SEL_AUTO &&
        MIC_FE_ADC_TEST_R == MIC_FE_ADC_SEL_AUTO) {
        return false;
    }
    int l = -1, r = -1;
    if (esp_codec_dev_read_reg(mic, ES8389_REG_ADCL_SEL, &l) != 0 ||
        esp_codec_dev_read_reg(mic, ES8389_REG_ADCR_SEL, &r) != 0) {
        return false;
    }
    /* 保留 bit[3:0] 的 PGA 增益与 bit7，只替换 bit[6:4] 的输入选择 */
    int nl = (l & ~0x70) | ((MIC_FE_ADC_TEST_L & 0x7) << 4);
    int nr = (r & ~0x70) | ((MIC_FE_ADC_TEST_R & 0x7) << 4);
    if (nl == l && nr == r) {
        return false;                       /* 已是目标值：不写、不打日志 */
    }
    if (esp_codec_dev_write_reg(mic, ES8389_REG_ADCL_SEL, nl) != 0 ||
        esp_codec_dev_write_reg(mic, ES8389_REG_ADCR_SEL, nr) != 0) {
        ESP_LOGE(TAG, "force ADC sel failed, keep driver default");
        return false;
    }
    int cl = -1, cr = -1;
    esp_codec_dev_read_reg(mic, ES8389_REG_ADCL_SEL, &cl);
    esp_codec_dev_read_reg(mic, ES8389_REG_ADCR_SEL, &cr);
    ESP_LOGW(TAG, "ADC sel FORCED: 0x72=0x%02x[%s] 0x73=0x%02x[%s]",
             cl, adc_sel_name_l((cl >> 4) & 0x7),
             cr, adc_sel_name_r((cr >> 4) & 0x7));
    return true;
}

/* ============================================================
 * P4 探针执行体
 * ============================================================ */
#if MIC_FE_P4_NEED_CODE

/* 运行时探针状态。编译期开关只给开机初值（MIC_FE_P4_BOOT_MASK），
 * 长按 SET 之后就完全由这两个变量决定 —— 单一真相源，日志一眼可对。 */
static int  s_p4_mask  = (int)MIC_FE_P4_BOOT_MASK;
static int  s_p4_idx;                   /* 与 s_p4_mask 同步，只用于日志"第几态" */
static bool s_p4_touched;               /* 改过寄存器 ⇒ 归零时必须走一遍清位 */

static const char *const s_p4_names[] = {
    "all-off", "copy21:0x23.b4", "hwRinv:0x25.b5", "muteR:0x20.b0", "muteL:0x20.b1"
};
static const unsigned s_p4_masks[] = {
    0u, P4_BIT_COPY21, P4_BIT_HWRINV, P4_BIT_MUTE2, P4_BIT_MUTE1
};
#define P4_STATE_NUM  ((int)(sizeof(s_p4_masks) / sizeof(s_p4_masks[0])))

/* 把 reg 里 bits 覆盖的那几位整体设为 value 的对应位 —— **既置位也清位**。
 *
 * ★ 这是与旧版最大的区别：旧实现只会 `v | mask`，取消开关根本清不掉，
 *   只能靠 codec 重开复位 ⇒ 同一次上电里没法做 A/B。现在能来回切。
 * ★ 幂等：目标值相同就不写、不打日志（attach 与 diag 周期都会调）。 */
static void p4_write_bits(esp_codec_dev_handle_t mic, int reg,
                          int bits, int value, const char *why)
{
    int v = -1;
    if (esp_codec_dev_read_reg(mic, reg, &v) != 0 || v < 0) {
        return;
    }
    int nv = ((v & ~bits) | (value & bits)) & 0xFF;
    if (nv == v) {
        return;                         /* 已是目标值：不写、不刷日志 */
    }
    if (esp_codec_dev_write_reg(mic, reg, nv) != 0) {
        ESP_LOGE(TAG, "P4: write 0x%02x failed (%s), keep 0x%02x", reg, why, v);
        return;
    }
    int rb = -1;
    esp_codec_dev_read_reg(mic, reg, &rb);
    ESP_LOGW(TAG, "P4: 0x%02x 0x%02x -> 0x%02x (%s) readback=0x%02x",
             reg, v, nv, why, rb);
}

#endif  /* MIC_FE_P4_NEED_CODE */

static void p4_apply_probes(esp_codec_dev_handle_t mic)
{
#if !MIC_FE_P4_NEED_CODE
    (void)mic;                          /* 全关：连 I2C 都不碰 */
#else
    if (mic == NULL) {
        return;
    }
    int mask = s_p4_mask;
    /* 全关且从没改过 ⇒ 完全空转，正常固件的热路径零 I2C 开销 */
    if (mask == 0 && !s_p4_touched) {
        return;
    }
    /* 0x23.bit4：R 变成 L 的严格副本 ⇒ corr 必须 ≡ +1.000 */
    p4_write_bits(mic, ES8389_REG_ADC_MODE_CTRL, ES8389_ADC2_FROM_ADC1,
                  (mask & P4_BIT_COPY21) ? ES8389_ADC2_FROM_ADC1 : 0, "copy21");
    /* 0x25.bit5：硬件翻 R 极性 ⇒ 应与软件 INVERT_R 同结果 */
    p4_write_bits(mic, ES8389_REG_ADC_HPF2, ES8389_ADC2_180C,
                  (mask & P4_BIT_HWRINV) ? ES8389_ADC2_180C : 0, "hwRinv");
    /* 两个静音位在同一寄存器，合成一次写入（分两次写会多打一行日志） */
    const int mute_bits = ES8389_ADC1_P2S_MUTE | ES8389_ADC2_P2S_MUTE;
    const int mute_val  = ((mask & P4_BIT_MUTE1) ? ES8389_ADC1_P2S_MUTE : 0) |
                          ((mask & P4_BIT_MUTE2) ? ES8389_ADC2_P2S_MUTE : 0);
    p4_write_bits(mic, ES8389_REG_ADC_SP_CTRL, mute_bits, mute_val, "mute");
    if (mask == 0) {
        s_p4_touched = false;           /* 清干净了，回到"不碰 I2C" */
    }
#endif
}

/* 运行时切换：长按 SET 时由 button_service 调用。
 * ★ 不依赖会话是否已建立：没 codec 时只记状态，attach 时再生效。 */
void mic_fe_p4_next(void)
{
#if !MIC_FE_P4_RT
    ESP_LOGW(TAG, "P4 runtime: 本固件未开启（menuconfig → 麦克风前端 → P4.4 运行时切换）");
#else
    s_p4_idx  = (s_p4_idx + 1) % P4_STATE_NUM;
    s_p4_mask = (int)s_p4_masks[s_p4_idx];
    s_p4_touched = true;                /* 哪怕回到 all-off 也要走清位 */

    if (s_codec == NULL) {
        ESP_LOGW(TAG, "P4 runtime -> [%d/%d] %s mask=0x%02x（codec 未就绪，attach 时生效）",
                 s_p4_idx, P4_STATE_NUM - 1, s_p4_names[s_p4_idx], s_p4_mask);
        return;
    }
    p4_apply_probes(s_codec);

    /* 换态后必须清窗口：否则下一条 diag 混着旧态数据，两态都不代表 */
    diag_window_reset();

    /* 切换后立刻读回三个寄存器，省得用户自己算 —— 这是判断"到底生效没有"的唯一依据 */
    int r20 = -1, r23 = -1, r25 = -1;
    esp_codec_dev_read_reg(s_codec, ES8389_REG_ADC_SP_CTRL,   &r20);
    esp_codec_dev_read_reg(s_codec, ES8389_REG_ADC_MODE_CTRL, &r23);
    esp_codec_dev_read_reg(s_codec, ES8389_REG_ADC_HPF2,      &r25);
    ESP_LOGW(TAG, "P4 runtime -> [%d/%d] %s mask=0x%02x | 0x20=0x%02x 0x23=0x%02x 0x25=0x%02x",
             s_p4_idx, P4_STATE_NUM - 1, s_p4_names[s_p4_idx], s_p4_mask,
             (unsigned)(r20 & 0xFF), (unsigned)(r23 & 0xFF), (unsigned)(r25 & 0xFF));
#endif
}

/* P4.0 基线读回：5 个 ADC 相关寄存器各打一次真值。
 * ★ 只在 attach（会话建立、codec open 之后）打一次 —— 这些值平时不变，
 *   每 5 s 刷一遍只会把 diag 行淹掉。 */
static void p4_dump_regs(esp_codec_dev_handle_t mic)
{
    static const int  regs[]  = { ES8389_REG_ADC_SP_CTRL, ES8389_REG_ADC_MODE_CTRL,
                                  ES8389_REG_ADC_HPF2, ES8389_REG_ADC_ANA_EN,
                                  ES8389_REG_ADC_ANA_CTRL1 };
    static const char *names[] = { "0x20", "0x23", "0x25", "0x64", "0x6d" };
    /* ★ 期望值是**本工程实测**，不是 datasheet 复位默认（reset default 是 00/00/06）：
     *   0x20=0x60  ← 驱动 es8389.c:544 写 ES8389_S16_LE(3<<5)，即 16 bit 字长
     *   0x23=0x80  ← 驱动 es8389.c:609 置 bit7（no_dac_ref==false 的"内部参考"分支）
     *   0x25=0x0a  ← 驱动 es8389.c:257 硬写 HPF coef2=0x0A（复位默认 0x06）
     * 0x64/0x6D 是只读回参考值（模拟使能 / bias），不参与判定。 */
    static const int  expect[] = { 0x60, 0x80, 0x0a, -1, -1 };
    char buf[5][8];
    int  val[5];
    for (int i = 0; i < 5; i++) {
        int v = -1;
        if (esp_codec_dev_read_reg(mic, regs[i], &v) != 0 || v < 0) {
            val[i] = -1;
            strcpy(buf[i], "ERR");
        } else {
            val[i] = v & 0xFF;
            /* ★ 必须 &0xFF：直接传 int 时 GCC 无法排除负值，
             *   %02x 最坏按 8 位算 ⇒ -Werror=format-truncation 直接拦下 */
            snprintf(buf[i], sizeof(buf[i]), "0x%02x", (unsigned)val[i]);
        }
    }
    ESP_LOGI(TAG, "es8389 regs: %s=%s %s=%s %s=%s %s=%s %s=%s "
                  "(expect 60/80/0a/-/- : 16bit / TDM-or-ref / HPF2)",
             names[0], buf[0], names[1], buf[1], names[2], buf[2],
             names[3], buf[3], names[4], buf[4]);
    /* 0x25 bit5 = ADC2_180C：还在 hwRinv 态说明上一轮运行时切换没切回 all-off */
    if (val[2] >= 0 && (val[2] & 0x20)) {
        ESP_LOGW(TAG, "0x25 bit5(ADC2_180C)=1：运行时还停在 hwRinv 态，长按 SET 切回 all-off");
    }
    for (int i = 0; i < 3; i++) {
        if (expect[i] >= 0 && val[i] >= 0 && val[i] != expect[i]) {
            ESP_LOGW(TAG, "reg %s=0x%02x != expect 0x%02x（有别的模块改过，先把这条查清）",
                     names[i], (unsigned)val[i], (unsigned)expect[i]);
        }
    }
}

/* ============================================================
 * FIFO
 * ============================================================ */
static inline void fifo_reset(void)
{
    s_fifo_rd = 0;
    s_fifo_cnt = 0;
}

static inline void fifo_push(int16_t v)
{
    if (s_fifo_cnt >= OUT_FIFO_SAMPLES) {
        /* 满 = 上游没在消费，丢掉最旧的一帧，保住延迟不漂 */
        s_fifo_rd = (s_fifo_rd + 1) % OUT_FIFO_SAMPLES;
        s_fifo_cnt--;
    }
    s_fifo[(s_fifo_rd + s_fifo_cnt) % OUT_FIFO_SAMPLES] = v;
    s_fifo_cnt++;
}

static inline int16_t fifo_pop(void)
{
    int16_t v = s_fifo[s_fifo_rd];
    s_fifo_rd = (s_fifo_rd + 1) % OUT_FIFO_SAMPLES;
    s_fifo_cnt--;
    return v;
}

/* ============================================================
 * 诊断：判断"两个麦到底是不是两路"
 *   corr ≈ 1.000 且两路 RMS 基本相同 ⇒ 第二路是复制品（没单独接麦）
 *   corr 明显 < 1（0.3~0.9）      ⇒ 真两麦，有空间差
 * ============================================================ */
static void diag_scan_stereo(const int16_t *ilv, int frames)
{
    /* ★ TTS 期间整段跳过：不读样本、不算浮点、不打日志、不碰 5 s 定时器。
     *   此时 mic 收到的是喇叭回声，corr 恒为负，本来也没有诊断价值。 */
    if (frames <= 0 || s_diag_hold) {
        return;
    }

    /* REC 页可视化：消费原始双路（BSS/MONO/GSC 都在这里过一遍）。
     * 必须在 s_cfg.diag 判断之前 —— 关掉 5 s 日志诊断不应连带关掉 UI。 */
    mic_monitor_ingest(ilv, frames);

    if (!s_cfg.diag) {
        return;
    }
    int n = frames > 1024 ? 1024 : frames;

    /* ① 先算这一块的能量与峰值：判「有没有人说话」 */
    uint64_t bl2 = 0, br2 = 0;
    int32_t  pk_l = 0, pk_r = 0;
    for (int i = 0; i < n; i++) {
        int32_t l = ilv[2 * i];
        int32_t r = ilv[2 * i + 1];
        bl2 += (uint64_t)((int64_t)l * l);
        br2 += (uint64_t)((int64_t)r * r);
        int32_t al = (l < 0) ? -l : l;
        int32_t ar = (r < 0) ? -r : r;
        if (al > pk_l) { pk_l = al; }
        if (ar > pk_r) { pk_r = ar; }
    }
    if (pk_l > s_peak_l) { s_peak_l = pk_l; }
    if (pk_r > s_peak_r) { s_peak_r = pk_r; }

    /* ② 只有"像人声"的块才进相关统计
     *    静音窗口里两路各自只有 ADC 底噪，corr 会乱跳（甚至接近 0），
     *    拿它去判"是不是复制品"必然误判 —— 早期三个窗口 corr 全是天文数字/垃圾值，
     *    一半原因也是它们全是 RMS 21~65 的静音段。 */
    double blk_rms = sqrt((double)(bl2 + br2) / (2.0 * (double)n));
    if (blk_rms >= DIAG_ACTIVE_RMS) {
        for (int i = 0; i < n; i++) {
            int32_t l = ilv[2 * i];
            int32_t r = ilv[2 * i + 1];
            s_acc_l2 += (uint64_t)((int64_t)l * l);
            s_acc_r2 += (uint64_t)((int64_t)r * r);
            s_acc_lr += (int64_t)l * r;      /* ★ 带符号，别改回 uint64 */
        }
        s_acc_n += (uint32_t)n;
    }

    int64_t now = esp_timer_get_time();
    if (s_stats_last_us == 0) {
        s_stats_last_us = now;
    } else if (now - s_stats_last_us >= 5 * 1000 * 1000) {
        mic_fe_dump_stats();
        s_stats_last_us = now;
    }
}

void mic_fe_set_diag_hold(bool hold)
{
    s_diag_hold = hold;
    if (!hold) {
        /* 恢复时把窗口清干净：挂起期间没有累加，但 peak 可能在挂起前残留，
         * 且 5 s 定时器应从恢复时刻重新起算，免得恢复后立刻打一行空统计。 */
        diag_window_reset();
    }
}

void mic_fe_dump_stats(void)
{
    if (s_diag_hold) {
        return;
    }
    /* 复测模式下每次 dump 补写一次（别的模块重开 mic 会把 0x72/0x73 冲回差分）。
     * AUTO 模式下是空操作，连 I2C 都不碰。 */
    adc_apply_test_sel(s_codec);
    /* P4 探针同理：全关时是空函数，连 I2C 都不碰 */
    p4_apply_probes(s_codec);

    /* 5 秒窗口内一句有效语音都没有：也要说话，否则看不出"前端在跑但没人说话"
     * 与"前端根本没跑"的区别（前者 peak 有值，后者全 0）。 */
    if (s_acc_n == 0) {
        ESP_LOGI(TAG, "diag[%s] idle 5s: peak L=%d R=%d (< %.0f)，静音不判 corr",
                 mic_fe_mode_name(), (int)s_peak_l, (int)s_peak_r, DIAG_ACTIVE_RMS);
        s_peak_l = 0;
        s_peak_r = 0;
        return;
    }

    double ml  = sqrt((double)s_acc_l2 / s_acc_n);
    double mr  = sqrt((double)s_acc_r2 / s_acc_n);
    double den = sqrt((double)s_acc_l2 * (double)s_acc_r2);
    double corr = (den > 1e-9) ? ((double)s_acc_lr / den) : 0.0;
    if (corr > 1.0)  { corr = 1.0; }        /* 数值兜底，防浮点误差越界 */
    if (corr < -1.0) { corr = -1.0; }

    const char *hint;
    double a = (corr < 0) ? -corr : corr;
    if (a > 0.99) {
        /* ★ 这里必须看绝对值：只判正相关的话，「两路反相」会被误报成
         *   "相关性低：远场/相位差大"，把最好的一条线索颠倒过来。 */
        hint = (corr > 0)
             ? (MIC_FE_INVERT_R
                ? "  <== 同相且强相关：双麦正常（BSS 已启用）"
                : "  <== 两路几乎同相：第二路疑似复制/悬空")
             : (MIC_FE_INVERT_R
                ? "  <== 翻转后仍反相：假设不成立，查 ADCR 定义 / 硬件"
                : "  <== 两路反相：可能是右麦差分极性反了，不是副本；开 MIC_FE_INVERT_R 复测");
    } else if (a > 0.30) {
        hint = (corr > 0) ? "  <== 有空间差，像真双麦"
                          : "  <== 有空间差但极性相反";
    } else {
        hint = "  <== 相关性低：远场/相位差大，或某一路是噪声";
    }
    ESP_LOGI(TAG, "diag[%s Rinv=%d] L_rms=%.0f R_rms=%.0f corr=%.3f n=%u peak=%d/%d%s",
             mic_fe_mode_name(), (int)MIC_FE_INVERT_R, ml, mr, corr, (unsigned)s_acc_n,
             (int)s_peak_l, (int)s_peak_r, hint);

    s_acc_l2 = s_acc_r2 = 0;
    s_acc_lr = 0;
    s_acc_n = 0;
    s_peak_l = 0;
    s_peak_r = 0;
}

/* ============================================================
 * 采集
 * ============================================================ */
static esp_err_t read_stereo(esp_codec_dev_handle_t mic, uint8_t *dst, int bytes)
{
    int done = 0;
    while (done < bytes) {
        int chunk = bytes - done;
        if (chunk > READ_CHUNK_MAX) {
            chunk = READ_CHUNK_MAX;
        }
        chunk &= ~(int)(2 * sizeof(int16_t) - 1);   /* 对齐到「一帧 2ch/16bit」= 4B */
        if (chunk <= 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* esp_codec_dev_read 返回实际字节数，-1 失败；不足一帧的尾部不合法 */
        if (esp_codec_dev_read(mic, dst + done, chunk) < 0) {
            return ESP_FAIL;
        }
        done += chunk;
    }
    return ESP_OK;
}

/* MONO（回退档）：I2S 槽是 stereo（codec 按 2ch open），只取 L。
 * 读 2ch 交错到**栈上小缓冲**再抽 L —— 不额外占 PSRAM，也不把一次读放大成
 * 2 倍内存峰值。顺手把交错块喂给 diag（双麦复测时这是唯一观察入口）。
 * ★ 默认走 BSS，本函数只在 menuconfig 改回 MONO 后使用。 */
#define MONO_CHUNK_BYTES   1024      /* 512 帧 × 2ch × 2B */

static esp_err_t read_left_only(esp_codec_dev_handle_t mic, uint8_t *dst, int bytes)
{
    int16_t chunk[MONO_CHUNK_BYTES / (int)sizeof(int16_t)];
    int done = 0;                    /* 已产出字节（单声道） */
    while (done < bytes) {
        int out_part = bytes - done;
        if (out_part > MONO_CHUNK_BYTES / 2) {
            out_part = MONO_CHUNK_BYTES / 2;
        }
        if (esp_codec_dev_read(mic, chunk, out_part * 2) < 0) {
            return ESP_FAIL;
        }
        int frames = out_part / (int)sizeof(int16_t);
        invert_r_inplace(chunk, frames);   /* 只影响 diag 看到的 R；MONO 上行仍只取 L */
        int16_t *d = (int16_t *)(void *)(dst + done);
        for (int i = 0; i < frames; i++) {
            d[i] = chunk[2 * i];     /* 只取 L；R 是否可用由 §28 探针判定 */
        }
        diag_scan_stereo(chunk, frames);
        done += out_part;
    }
    return ESP_OK;
}

/* GSC：读一帧 128 点 → 波束 → 128 个单声道入 FIFO */
static esp_err_t gsc_fill_once(esp_codec_dev_handle_t mic)
{
    if (esp_codec_dev_read(mic, s_ilv, GSC_FRAME_LEN * 2 * (int)sizeof(int16_t)) < 0) {
        return ESP_FAIL;
    }
    invert_r_inplace(s_ilv, GSC_FRAME_LEN);
    diag_scan_stereo(s_ilv, GSC_FRAME_LEN);

    for (int i = 0; i < GSC_FRAME_LEN; i++) {
        s_planar[i]                  = s_ilv[2 * i];        /* ch0 = mic L */
        s_planar[GSC_FRAME_LEN + i]  = s_ilv[2 * i + 1];    /* ch1 = mic R */
    }
    int16_t out[GSC_FRAME_LEN];     /* 256 B，栈上足够 */
    esp_gsc_process(s_gsc, s_planar, s_cfg.steer_phi_deg, out);
    for (int i = 0; i < GSC_FRAME_LEN; i++) {
        fifo_push(out[i]);
    }
    return ESP_OK;
}

esp_err_t mic_fe_read(uint8_t *out, int size, esp_codec_dev_handle_t mic)
{
    if (!s_ready || out == NULL || mic == NULL || size <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cfg.mode == MIC_FE_MODE_MONO) {
        /* 回退档：只取 L，输出 1ch（默认走 BSS，本分支仅在 menuconfig 改回 MONO 时用） */
        return read_left_only(mic, out, size);
    }

    if (s_cfg.mode == MIC_FE_MODE_BSS) {
        /* 2ch 直通：原样交给 AFE（AFE 按 "MM" 解析成两麦做 BSS）
         * ★ 默认路径。R 在这里先按 INVERT_R 取负，保证 AFE 拿到同相双麦。 */
        esp_err_t err = read_stereo(mic, out, size);
        if (err == ESP_OK) {
            int frames = size / (2 * (int)sizeof(int16_t));
            invert_r_inplace((int16_t *)out, frames);
            diag_scan_stereo((const int16_t *)out, frames);
        }
        return err;
    }

    /* GSC：输出单声道，补满 size/2 帧 */
    int frames = size / (int)sizeof(int16_t);
    int16_t *dst = (int16_t *)out;
    for (int i = 0; i < frames; i++) {
        if (s_fifo_cnt == 0) {
            esp_err_t err = gsc_fill_once(mic);
            if (err != ESP_OK) {
                return err;
            }
            if (s_fifo_cnt == 0) {          /* 理论上不会：一帧必产 128 点 */
                dst[i] = 0;
                continue;
            }
        }
        dst[i] = fifo_pop();
    }
    return ESP_OK;
}

/* ============================================================
 * 生命周期
 * ============================================================ */
const char *mic_fe_layout(void)
{
    /* 只有 BSS 是真双麦输入；MONO 与 GSC 都只给 AFE 一路 */
    return (s_cfg.mode == MIC_FE_MODE_BSS) ? "MM" : "M";
}

int mic_fe_out_channels(void)
{
    return (s_cfg.mode == MIC_FE_MODE_BSS) ? 2 : 1;
}

const char *mic_fe_mode_name(void)
{
    switch (s_cfg.mode) {
    case MIC_FE_MODE_BSS: return "BSS";
    case MIC_FE_MODE_GSC: return "GSC";
    case MIC_FE_MODE_MONO:
    default:              return "MONO";
    }
}

esp_err_t mic_fe_init(const mic_fe_cfg_t *cfg)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (cfg) {
        s_cfg = *cfg;
    } else {
        mic_fe_cfg_t def = MIC_FE_CFG_DEFAULT();
        def.mode          = MIC_FE_MODE;
        def.mic_spacing_m = MIC_FE_MIC_SPACING_M;
        def.steer_phi_deg = MIC_FE_STEER_PHI_DEG;
        def.diag          = MIC_FE_DIAG_ENABLE;
        s_cfg = def;
    }
    s_stats_last_us = 0;
    /* 分开赋值：s_acc_lr 是有符号的，链式赋值会触发 sign-conversion */
    s_acc_l2 = 0;
    s_acc_r2 = 0;
    s_acc_lr = 0;
    s_acc_n  = 0;
    s_peak_l = 0;
    s_peak_r = 0;

    if (s_cfg.mode == MIC_FE_MODE_GSC) {
        /* ★ 坐标：x 左→右，y 近→远（gsc_core_types.h:17-21），单位米 */
        static PlaneCoord coord[2];
        coord[0].x = 0.0f;
        coord[0].y = 0.0f;
        coord[0].z = 0.0f;
        coord[1].x = s_cfg.mic_spacing_m;
        coord[1].y = 0.0f;
        coord[1].z = 0.0f;

        s_gsc = esp_gsc_create(coord, 2);
        if (s_gsc == NULL) {
            /* 回退 BSS 而不是 MONO：选 GSC 的前提就是"有真双麦"，
             * 双麦直通至少还能把两路都交出去（本板默认 MONO，走不到这里）。 */
            ESP_LOGE(TAG, "gsc create failed (spacing=%.3fm) -> fallback BSS",
                     (double)s_cfg.mic_spacing_m);
            s_cfg.mode = MIC_FE_MODE_BSS;
        } else {
            const size_t ilv_bytes   = GSC_FRAME_LEN * 2 * sizeof(int16_t);
            const size_t fifo_bytes  = OUT_FIFO_SAMPLES * sizeof(int16_t);
            s_ilv    = heap_caps_malloc(ilv_bytes,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_planar = heap_caps_malloc(ilv_bytes,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_fifo   = heap_caps_malloc(fifo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_ilv || !s_planar || !s_fifo) {
                ESP_LOGE(TAG, "psram alloc failed (ilv=%u fifo=%u)",
                         (unsigned)ilv_bytes, (unsigned)fifo_bytes);
                esp_gsc_destroy(s_gsc);
                s_gsc = NULL;
                heap_caps_free(s_ilv);    s_ilv = NULL;
                heap_caps_free(s_planar); s_planar = NULL;
                heap_caps_free(s_fifo);   s_fifo = NULL;
                s_cfg.mode = MIC_FE_MODE_BSS;
            } else {
                fifo_reset();
            }
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready: mode=%s layout=%s out_ch=%d spacing=%.3fm phi=%.0f",
             mic_fe_mode_name(), mic_fe_layout(), mic_fe_out_channels(),
             (double)s_cfg.mic_spacing_m, (double)s_cfg.steer_phi_deg);
    /* 编译期配置回显：一眼判断"烧进去的是不是带最新改动的固件"。
     * ADC 输入选择只能在会话建立时（codec 打开后）读回，开机看不到，
     * 所以这里先把本固件的意图打出来，免得误判成"没编进去"。 */
    ESP_LOGI(TAG, "build cfg: mode=%s ADC_TEST L=%d R=%d Rinv=%d "
                  "(0=AUTO/driver diff, 5=MIC1P|MIC2N, 6=MIC2P|MIC1N) "
                  "-> 生效读数要等首次会话的 'es8389 ADC sel' 行",
             mic_fe_mode_name(), (int)MIC_FE_ADC_TEST_L, (int)MIC_FE_ADC_TEST_R,
             (int)MIC_FE_INVERT_R);
    ESP_LOGI(TAG, "build cfg P4: copy21=%d hwRinv=%d muteADC1=%d muteADC2=%d%s%s",
             (int)MIC_FE_P4_COPY21, (int)MIC_FE_P4_HWRINV,
             (int)MIC_FE_P4_MUTE1, (int)MIC_FE_P4_MUTE2,
             MIC_FE_P4_ACTIVE ? "" : " (all off)",
             MIC_FE_P4_RT ? " runtime=ON(long-press SET)" : "");
#if defined(CONFIG_MIC_FE_MODE_SEL_BSS)
    /* 双麦的生效条件写在一行里：layout=MM + AFE 的 se_init 由
     * xiaozhi_service 的 NS_ENABLE 标志打开（av_processor 里 `se_init = ns_enable`）。
     * 任一条不满足，AFE 都会退化成"只用第一路"，听起来就是单麦。
     * ★ 这里必须用 `#if defined(CONFIG_...)`：MIC_FE_MODE 是**枚举常量**，
     *   在预处理器里两边都算 0，写成 `#if MIC_FE_MODE == MIC_FE_MODE_BSS`
     *   会恒为真（2026-11-11 差点埋进去）。 */
    ESP_LOGI(TAG, "BSS 路径: 2ch feed -> AFE SE(BSS) -> 1ch 上行 | layout=%s Rinv=%d"
                  " | 若上行仍像单麦，先查 'uplink frontend' 那行",
             mic_fe_layout(), (int)MIC_FE_INVERT_R);
#if !MIC_FE_INVERT_R
    ESP_LOGW(TAG, "BSS 但 INVERT_R=0：右麦反相未纠正，分离质量会明显下降");
#endif
#endif
#if MIC_FE_P4_RT
    /* 让开机初值落在正确的"第几态"上，否则第一次长按会跳到重复的态 */
    s_p4_idx = 0;
    for (int i = 0; i < P4_STATE_NUM; i++) {
        if ((int)s_p4_masks[i] == (int)MIC_FE_P4_BOOT_MASK) {
            s_p4_idx = i;
            break;
        }
    }
    ESP_LOGW(TAG, "P4 runtime boot: state=%d/%d %s mask=0x%02x "
                  "（长按 SET 循环切换：all-off -> copy21 -> hwRinv -> muteR -> muteL）",
             s_p4_idx, P4_STATE_NUM - 1, s_p4_names[s_p4_idx], s_p4_mask);
#endif
    return ESP_OK;
}

void mic_fe_deinit(void)
{
    s_codec = NULL;     /* 句柄由上层销毁，这里只丢引用，绝不解引用 */
    if (!s_ready) {
        return;
    }
    if (s_gsc) {
        esp_gsc_destroy(s_gsc);
        s_gsc = NULL;
    }
    if (s_ilv)    { heap_caps_free(s_ilv);    s_ilv = NULL; }
    if (s_planar) { heap_caps_free(s_planar); s_planar = NULL; }
    if (s_fifo)   { heap_caps_free(s_fifo);   s_fifo = NULL; }
    fifo_reset();
    s_ready = false;
    ESP_LOGI(TAG, "released");
}

void mic_fe_attach_codec(esp_codec_dev_handle_t mic)
{
    if (mic == NULL) {
        return;
    }
    int l = -1;
    int r = -1;
    if (esp_codec_dev_read_reg(mic, ES8389_REG_ADCL_SEL, &l) != 0 ||
        esp_codec_dev_read_reg(mic, ES8389_REG_ADCR_SEL, &r) != 0) {
        /* 不是 ES8389（或 codec 未 open）：静默跳过，别刷屏 */
        ESP_LOGW(TAG, "es8389 ADC sel read failed (codec not open / not ES8389)");
        return;
    }
    int sl = (l >> 4) & 0x7;
    int sr = (r >> 4) & 0x7;
    ESP_LOGI(TAG, "es8389 ADC sel: ADCL(0x72)=0x%02x sel=%d[%s] gain=%d | "
                  "ADCR(0x73)=0x%02x sel=%d[%s] gain=%d",
             l, sl, adc_sel_name_l(sl), l & 0xF,
             r, sr, adc_sel_name_r(sr), r & 0xF);

    /* 复测开关：只在显式指定时才改写，正常固件两个都是 AUTO → 这段不执行 */
    if (!adc_apply_test_sel(mic) &&
        sl == MIC_FE_ADC_SEL_DIFF && sr == MIC_FE_ADC_SEL_DIFF) {
        /* 驱动默认差分。已知 MIC2N 项≈0（见文件顶部实测表），R 只喂 diag 不进 AFE。 */
        ESP_LOGI(TAG, "both ADC differential; mode=%s Rinv=%d "
                      "（MIC2P 复测：menuconfig 选 MIC_FE_ADC_PROBE_SEL_MIC2P；"
                      "极性验证：选 MIC_FE_INVERT_R）",
                 mic_fe_mode_name(), (int)MIC_FE_INVERT_R);
    }

    /* P4.0：先读回基线（此刻还没写任何 P4 位），再应用 P4 探针。
     * ★ 顺序不能反：反了就把"改写后的值"当基线，对照实验失去意义。 */
    p4_dump_regs(mic);
    p4_apply_probes(mic);

    s_codec = mic;      /* diag 周期要拿它补写（别的模块重开 mic 会冲掉输入选择） */
}
