/*
 * breath_sync.h —— 芯片无关的「按绝对时间同步呼吸」核心
 *
 * 要解决的问题：
 *   现有的 breath 模式是 env = triangle(k_uptime_get_32(), 2400)。
 *   k_uptime 是「本机开机到现在的毫秒数」，两台设备上电时刻不可能一样，
 *   于是同一面墙上的两只灯各自呼吸、永不同相——再怎么调参数也对不齐。
 *
 * 这一层的答案只有一句话：
 *   亮度是「绝对时间」的纯函数 B(t)，不是「运行时长」的函数。
 *   只要两台设备的 UTC 对齐，同一个 t 算出来的 rgb 必然逐字节相同。
 *
 * 三条铁律（规范原文见 docs/参考/同步呼吸灯规则.md）：
 *   1. 相位只能由绝对时间推导，不得来自上电时长 / 帧计数 / 定时器计数值；
 *   2. 亮度 = LUT[phase]，LUT 是规范的一部分，跨设备必须逐字节相同；
 *   3. 喂进来的时间必须是「单调的 UTC 估计」，不能是会被校时跳变的 CLOCK_REALTIME。
 *
 * 依赖：只有 <stdint.h>。不认识 Zephyr、不认识 GPIO、不认识 WS2812。
 *       取时间、往灯上刷颜色，都由调用方（各平台的适配层）负责。
 */

#ifndef BREATH_SYNC_H_
#define BREATH_SYNC_H_

#include <stdint.h>

/* ------------------------------------------------------------ 规范 v1.0 常数 */

/* 呼吸周期 4 s。选它的理由：4 s = 15 次/分钟，接近静息呼吸频率；
 * 60 s 能被 4 s 整除（60/4=15），所以「每个整分钟」相位必为 0，
 * 验收时盯着秒表就能肉眼对表——不用示波器。 */
#define BREATH_PERIOD_US      4000000ull

/* 相位锚点：epoch 0。全局常数，不需要任何设备之间协商「从哪一刻开始呼吸」。 */
#define BREATH_ANCHOR_UTC_US  0ll

/* 包络最大值（8 位）。规范里的 level 都按 0..255 定义。 */
#define BREATH_LEVEL_MAX      255u

/* 时间同步门槛：吸收一次校时结果，走哪条路径（见 breath_clock_observe） */
#define BREATH_SYNC_SMALL_US      (25ll * 1000)          /* ≤ 25 ms：直接吃掉 */
#define BREATH_SYNC_BIG_US        (1000ll * 1000)        /* >  1 s ：直接跳变 */
#define BREATH_SLEW_PPM           5000                   /* 缓释速率上限 0.5% */
#define BREATH_SLEW_MIN_SPAN_US   (1ll * 1000 * 1000)    /* 缓释最快 1 s 内走完 */
#define BREATH_SLEW_MAX_SPAN_US   (300ll * 1000 * 1000)  /* 缓释最慢 300 s 内走完 */
#define BREATH_RATE_MIN_SPAN_US   (600ll * 1000 * 1000)  /* 两次校时间隔 ≥10 min 才更新频率 */
#define BREATH_RATE_PPM_LIMIT     100                   /* 频率偏差夹到 ±100 ppm */

/* 没标定过的晶振，按这个上界估漂移（普通无源晶振 ±20~50 ppm，取保守值）。
 * 出厂标定过就把真值写进 rate_ppm，这个常数就只是“最坏情况兜底”。 */
#define BREATH_DRIFT_DEFAULT_PPM  50

/* 一致性等级门槛（按“当前最大相位误差”分档，见 breath_clock_level） */
#define BREATH_LEVEL_STRICT_US    (25ll * 1000)          /* 同相：看不出来 */
#define BREATH_LEVEL_OK_US        (250ll * 1000)         /* 基本同相：并排细看才怪 */
#define BREATH_LEVEL_WEAK_US      (1000ll * 1000)        /* 能看出不同相 */

/* 二值输出（开关灯）的判决阈值。128 = 相位中点，占空比 50%，节拍与 RGB 灯严格同相。 */
#define BREATH_BINARY_THRESHOLD   128u

/* 通道层最多认几个通道（单色 1 / 双色 2 / RGB 3 / RGBW 4 / 小段灯带也够用） */
#define BREATH_MAX_CHANNELS       16u

/* --------------------------------------------------------------- 数据类型 */

struct breath_rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/*
 * 授时来源。规则层只关心「这一口时间有多准」，不关心它是从哪条链路来的：
 * 网线、BLE、ESP-NOW、CAN、GPS 的 PPS 脚、DS3231、工频过零、甚至人工按键，
 * 只要能产出一对 (mono_us, utc_us)，就都能喂进 breath_clock_observe_src。
 *
 * 这就是「无 WiFi 设备怎么同步」的答案：授时通道可插拔，相位规则一字不改。
 */
enum breath_source {
	BREATH_SRC_NONE = 0,    /* 从没授过时：只能自由运行 */
	BREATH_SRC_MANUAL,      /* 人工对表（按键/串口命令/App 点一下） */
	BREATH_SRC_LOCAL_LINK,  /* 同一簇内的 master 广播（BLE/ESP-NOW/UART/CAN/一根线） */
	BREATH_SRC_RTC,         /* 板载 RTC（DS3231 之类，一次设定长期走） */
	BREATH_SRC_LONGWAVE,    /* 长波授时（中国 BPC 68.5 kHz / 日本 JJY / 欧洲 DCF77） */
	BREATH_SRC_GNSS,        /* GPS/北斗，含 PPS */
	BREATH_SRC_NETWORK,     /* SNTP/NTP */
};

/* 各来源的“一口时间”典型不确定度（us）。用来算误差预算，不用来改相位。 */
int32_t breath_source_uncertainty_us(uint8_t source);

/* 验收向量：给定 utc_us，规范要求的相位下标与包络值。
 * 定义在 breath_sync.c 的生成区段里，由 tools/gen_breath_table.py 产出。 */
struct breath_vector {
	uint64_t utc_us;
	uint8_t  phase;
	uint8_t  level;
};

/* 生成区段导出的两张“规范实体”：向量表 + 表长。shell/自检都从这里遍历。 */
extern const struct breath_vector breath_vectors[];
extern const uint16_t breath_vector_count;

/* UTC 估计器：把「单调时钟」换算成「UTC」，
 * 校时只改锚点和斜率，绝不直接把时间拨跳（除了首次同步那种大偏差）。 */
struct breath_clock {
	int64_t utc_anchor_us;    /* 锚点：某次测量对应的 UTC（us, epoch） */
	int64_t mono_anchor_us;   /* 同一时刻的单调时钟读数（us） */
	int32_t rate_ppm;         /* 学到的频率偏差（石英漂移），±100 ppm 以内 */
	int64_t slew_err_us;      /* 正在缓释的剩余误差（us），0 表示没在缓释 */
	int64_t slew_start_mono_us;
	int64_t slew_span_us;     /* 缓释窗口长度（us） */
	int64_t prev_mono_us;     /* 上一次校时样本，用来学频率 */
	int64_t prev_utc_us;
	int64_t last_obs_mono_us; /* 最近一次授时的时刻，用来算“这口时间放多久了” */
	int32_t uncertainty_us;   /* 最近一次授时的不确定度（us） */
	uint8_t source;           /* enum breath_source：这口时间从哪条通道来的 */
	uint8_t has_prev;
	uint8_t rate_valid;       /* 1 = rate_ppm 已经学到过，可以走 EMA 了 */
	uint8_t synced;           /* 1 = 至少授过一次时，时间可信 */
};

/* breath_clock_observe 的返回值 */
enum breath_sync_result {
	BREATH_SYNC_NONE = 0,
	BREATH_SYNC_ABSORBED,   /* 误差很小，直接改锚点 */
	BREATH_SYNC_SLEWING,    /* 中等误差，按 ≤0.5% 速率缓释 */
	BREATH_SYNC_STEPPED,    /* 大误差（首次同步/长期失联），跳一次 */
};

/* --------------------------------------------------------------- 纯函数层 */

/**
 * @brief 规范相位：LUT 下标 0..255。相位 0 = 最暗，128 = 最亮。
 * @param utc_us 绝对时间（自 1970-01-01T00:00:00Z 起的微秒数，UTC，不含闰秒修正）
 */
uint8_t breath_phase(uint64_t utc_us);

/**
 * @brief 亮度包络 0..255。整机一致性就靠这一个函数的输出相同。
 *        已包含 gamma=2.0 的感知校正，调用方不需要再压一次亮度（除了设备级 gain）。
 */
uint8_t breath_envelope(uint64_t utc_us);

/**
 * @brief 按规范算出一帧颜色：包络 × 设备 gain × 基色。
 * @param base      基色（线性发光权重，不做 sRGB 转换）
 * @param gain_pct  设备级增益 0..100（不同型号 LED 的亮度标定，必须固定，不随时间变）
 */
void breath_frame(uint64_t utc_us, const struct breath_rgb *base, uint8_t gain_pct,
		  struct breath_rgb *out);

/**
 * @brief 通用通道展开：把同一个亮度包络铺到 n 个通道上。
 *
 * 这是「RGB 灯 / 单色 LED / 双色灯 / 灯带」共用一套规则的关键：
 *   同步的是**亮度标量**，不是灯。n 只改变“翻译”，不改变时间。
 *
 *   n = 1  单色 LED      base = {255}                → 1 路 PWM 占空比
 *   n = 2  双色 LED      base = {255, 0}             → 只呼吸第一色
 *   n = 3  RGB / 3颗独立LED base = {r, g, b}          → 色相不变，只改明暗
 *   n = 4  RGBW          base = {r, g, b, w}
 *   灯带：对每一颗像素各调一次（base 相同 = 整条同相；配合 breath_pixel_phase 做波浪）
 *
 * @param base  满亮时各通道的值 0..255
 * @param n     通道数，1..BREATH_MAX_CHANNELS
 * @param out   输出数组，调用方保证有 n 个元素
 */
void breath_channels(uint64_t utc_us, const uint8_t *base, uint8_t n,
		     uint8_t gain_pct, uint8_t *out);

/**
 * @brief 二值输出（只有亮/灭两态的灯、GPIO 直驱的灯）：level ≥ 128 → 1。
 *        阈值是规范常数，所以「开关灯」与「PWM 呼吸灯」的节拍严格同相。
 */
uint8_t breath_binary(uint64_t utc_us);

/**
 * @brief 把包络换成 PWM 占空比（0..2^bits-1），供 pwm_set_pulse_dt 直接乘周期。
 * @param bits PWM 分辨率 8..16。8 位够用；12/16 位能让暗部台阶更细。
 */
uint32_t breath_duty(uint8_t level, uint8_t bits);

/**
 * @brief 灯带第 index 颗灯的相位下标：base + index*step（mod 256）。
 *        step = 0（默认）时整条灯带同相，两台同规格灯带逐颗对齐；
 *        step ≠ 0 时形成相位波，波峰位置只由 UTC 决定，所以两台设备波浪位置也一致。
 */
uint8_t breath_pixel_phase(uint64_t utc_us, uint16_t index, uint8_t step);

/**
 * @brief 两台设备（或本机与基准时间）之间的相位差，单位 us，范围 (-P/2, P/2]。
 *        用来验收：|返回值| ≤ 25 ms 就算同相。
 */
int64_t breath_phase_error_us(uint64_t a_utc_us, uint64_t b_utc_us);

/** @brief 直接读表（调试用，等价于呼吸灯当前该给的 level） */
uint8_t breath_lut_lookup(uint8_t phase);

/** @brief 表的 FNV-1a 校验和，用来确认「两台设备拿的是同一张表」 */
uint32_t breath_lut_checksum(void);

/**
 * @brief 自检：表校验和 + 全部验收向量。
 * @return 0 通过；-1 表被改过；-2 相位不符；-3 亮度不符
 */
int breath_selftest(void);

/* --------------------------------------------------------------- 时钟层 */

/** @brief 用「当前单调时钟 + 当前 UTC」初始化。调用方保证两者是同一瞬间采样。 */
void breath_clock_init(struct breath_clock *c, int64_t mono_us, int64_t utc_us);

/**
 * @brief 「啥都不知道」的初始化：上电时刻当作 epoch 0，从此自由运行。
 *
 * 这是无网络、无 RTC、无任何授时通道时的兜底：
 *   - 单机看：呼吸完全正常；
 *   - 多机看：只要**同时上电**（同一条电源开关），就天然同相（误差 = 上电时刻差）；
 *   - 一旦之后拿到一次 UTC，调 breath_clock_observe_src 就立刻并到全局时间上。
 * 在直到第一次授时之前，设备处于“未同步”状态（synced = 0），亮度会被半亮守卫压一档。
 */
void breath_clock_init_freerun(struct breath_clock *c, int64_t mono_us);

/** @brief 把单调时钟换算成 UTC。纯计算，不读硬件，可在任意线程调用。 */
int64_t breath_clock_now_us(const struct breath_clock *c, int64_t mono_us);

/**
 * @brief 喂一次校时结果（不带来源信息，按“一般授时 25 ms”记，只用于调试/单机）。
 *
 * 三种处理：
 *   误差 ≤ 25 ms  → 直接改锚点（小于一帧，肉眼看不见）
 *   误差 ≤ 1 s    → 记成缓释，按 ≤0.5% 的速率在 1..300 s 内走完（不跳相）
 *   误差 > 1 s    → 直接跳（首次同步、或设备断电很久后重连）
 *
 * 量产代码请用 breath_clock_observe_src，把来源说清楚——否则误差预算只能瞎猜。
 *
 * @return enum breath_sync_result
 */
int breath_clock_observe(struct breath_clock *c, int64_t mono_us, int64_t utc_us);

/**
 * @brief 同上，但写明这口时间是哪条通道给的、有多准。
 *
 * 授时通道随便挑：SNTP、BLE/ESP-NOW 收到 master 广播、DS3231、GPS 的 PPS 脚、
 * BPC 长波模块、串口命令、按键对表……都能喂进来。规则层不关心介质，只关心精度。
 *
 * @param source         enum breath_source
 * @param uncertainty_us 这次授时的 1σ 不确定度；传 0 表示“按该来源的默认值算”
 */
int breath_clock_observe_src(struct breath_clock *c, int64_t mono_us, int64_t utc_us,
			     uint8_t source, int32_t uncertainty_us);

/**
 * @brief 写晶振标定（ppm）。出厂用频率计/对 PC 时间跑 24 h 测出来的值。
 *
 * 作用：把「没标定 = 按 50 ppm 估漂移」降到「标定后 = 按残余 2 ppm 估」，
 * 于是**一台只能用一次授时的离线设备，也能撑一天**：
 *   50 ppm × 24 h = 4.3 s（相位彻底跑飞）
 *    2 ppm × 24 h = 173 ms（相位 4%，肉眼仍算同相）
 *
 * 标定写进 rate_ppm，但后续真实测到的漂移会通过 EMA 覆盖它（实测优先）。
 */
void breath_clock_set_calib(struct breath_clock *c, int64_t mono_us, int32_t ppm);

/**
 * @brief 当前最大相位误差预算（us）。这是「一致性」的定量答案：
 *        E = 授时不确度 + |漂移| × 距上次授时的时长 + 正在缓释的剩余量。
 *        未同步时返回 INT64_MAX。
 */
int64_t breath_clock_error_budget_us(const struct breath_clock *c, int64_t mono_us);

/**
 * @brief 一致性等级 0..3（见 BREATH_LEVEL_*_US）：
 *        3 = 严格同相（≤25 ms）；2 = 基本同相（≤250 ms）；1 = 能看出不齐（≤1 s）；0 = 未同步。
 */
uint8_t breath_clock_level(const struct breath_clock *c, int64_t mono_us);

/**
 * @brief 亮度守卫：未同步（等级 0）时把 gain 砍半，让“这盏灯没对上表”在视觉上一眼可辨。
 *        这不是同步手段，是给运维/用户的诚实提示。
 */
uint8_t breath_gain_guard(uint8_t gain_pct, uint8_t level);

/** @brief 当前估计的频率偏差（ppm），用于日志/诊断 */
int32_t breath_clock_rate_ppm(const struct breath_clock *c);

/**
 * @brief 把时钟状态压成一个可存 NVS 的小结构（离线重启后能接着走）。
 *
 * 只对「单调时钟跨重启连续」的设备有意义（有 RTC 备份域 / 电池供电不中断）。
 * 普通设备重启后 monotonic 从 0 开始，恢复出来的时间会把“关机时长”当 0，
 * 所以只适合“一直插电、偶尔软重启”的场景。
 */
struct breath_clock_save {
	uint32_t magic;         /* 0x42524854 "BRHT"，防脏数据 */
	int64_t  utc_us;        /* 保存时刻的 UTC */
	int64_t  mono_us;       /* 保存时刻的单调时钟 */
	int32_t  rate_ppm;
	uint8_t  source;
	uint8_t  reserved[3];
};

void breath_clock_save_state(const struct breath_clock *c, int64_t mono_us,
			     struct breath_clock_save *out);

/**
 * @brief 从 NVS 恢复。mono_us 传「本次上电后、与保存时同一个单调时钟体系」的读数。
 * @return 0 成功；-1 magic 不对（当没存过）
 */
int breath_clock_restore(struct breath_clock *c, const struct breath_clock_save *in,
			 int64_t mono_us);

#endif /* BREATH_SYNC_H_ */
