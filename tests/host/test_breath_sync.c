/*
 * test_breath_sync.c —— 在 PC 上验证 breath_sync 的一致性
 *
 * 这个文件不碰硬件，跑的是「规范对得上吗」这一层：
 *   ① LUT 校验和 + 全部验收向量
 *   ② 两台“设备”上电时刻不同，同一面墙的时刻是否算出同一亮度
 *   ③ 相位差函数的环形性
 *   ④ 校时三路径：吸收 / 缓释 / 跳变
 *   ⑤ 石英漂移学习（50 ppm 的假时钟能不能学回来）
 *
 * 编译（Windows / MinGW）：
 *   gcc -std=c99 -Wall -Wextra -I apps/common/include ^
 *       tools/test_breath_sync.c apps/common/src/breath_sync.c -o %TEMP%\test_breath.exe
 */

#include <stdio.h>
#include <inttypes.h>

#include "breath_sync.h"

static int g_fail;

static void chk(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		g_fail++;
	}
}

static int64_t iabs64(int64_t v)
{
	return v < 0 ? -v : v;
}

int main(void)
{
	uint16_t i;
	int rc = breath_selftest();

	printf("== 1. spec self-test ==\n");
	printf("   breath_selftest()  = %d (0 = ok)\n", rc);
	printf("   lut checksum       = 0x%08" PRIX32 " (spec 0x51521FF6)\n",
	       breath_lut_checksum());
	chk(rc == 0, "breath_selftest");

	printf("\n== 2. acceptance vectors ==\n");
	printf("   %18s %6s %6s\n", "utc_us", "phase", "level");
	for (i = 0; i < breath_vector_count; i++) {
		const struct breath_vector *v = &breath_vectors[i];

		printf("   %18" PRIu64 " %6u %6u\n", v->utc_us, v->phase, v->level);
		chk(breath_phase(v->utc_us) == v->phase, "vector phase");
		chk(breath_envelope(v->utc_us) == v->level, "vector level");
	}

	printf("\n== 3. two devices, same wall clock, different boot time ==\n");
	{
		struct breath_clock a, b;
		const int64_t utc0 = 1735689600000000ll;  /* 2025-01-01T00:00:00Z */
		const int64_t b_late = 899000ll;          /* B 比 A 晚 0.899 s 上电 */

		breath_clock_init(&a, 1000ll, utc0);
		breath_clock_init(&b, 1000ll + b_late, utc0);

		int64_t ua = breath_clock_now_us(&a, 5000000ll);
		int64_t ub = breath_clock_now_us(&b, 5000000ll + b_late);

		printf("   A utc = %" PRId64 "\n", ua);
		printf("   B utc = %" PRId64 "\n", ub);
		printf("   diff  = %" PRId64 " us\n", ub - ua);
		chk(iabs64(ua - ub) <= 1, "two devices agree on UTC");
		chk(breath_envelope((uint64_t)ua) == breath_envelope((uint64_t)ub),
		    "two devices agree on level");
		chk(breath_phase_error_us((uint64_t)ua, (uint64_t)ub) == 0, "phase error is 0");
	}

	printf("\n== 4. phase error wraps around the 4 s period ==\n");
	{
		/* 3.000 s vs 1.000 s：真实差 +2 s，环形上等价于 -2 s */
		int64_t e1 = breath_phase_error_us(3000000ull, 1000000ull);
		/* 1.000 s vs 3.999 s：真实差 -2.999 s，环形上等价于 +1.001 s */
		int64_t e2 = breath_phase_error_us(1000000ull, 3999000ull);

		printf("   err(3.000s, 1.000s) = %" PRId64 " us\n", e1);
		printf("   err(1.000s, 3.999s) = %" PRId64 " us\n", e2);
		chk(e1 == 2000000ll || e1 == -2000000ll, "wrap to half period");
		chk(e2 == 1001000ll || e2 == -1001000ll, "wrap across boundary");
	}

	printf("\n== 5. sync paths ==\n");
	{
		struct breath_clock c;
		int r;

		/* 5a. 5 ms 偏差：直接吃掉 */
		breath_clock_init(&c, 0, 0);
		r = breath_clock_observe(&c, 1000000ll, 1005000ll);
		printf("   5 ms   -> %d (1=absorbed), now=%" PRId64 "\n",
		       r, breath_clock_now_us(&c, 1000000ll));
		chk(r == BREATH_SYNC_ABSORBED, "5ms absorbed");
		chk(breath_clock_now_us(&c, 1000000ll) == 1005000ll, "5ms applied");

		/* 5b. 100 ms 偏差：缓释 20 s，一半时间走一半 */
		breath_clock_init(&c, 0, 0);
		r = breath_clock_observe(&c, 2000000ll, 2100000ll);
		printf("   100 ms -> %d (2=slewing), span=%" PRId64 " us\n",
		       r, c.slew_span_us);
		chk(r == BREATH_SYNC_SLEWING, "100ms slewing");
		chk(c.slew_span_us == 20000000ll, "slew span = 20 s");

		int64_t mid = breath_clock_now_us(&c, 12000000ll);   /* 走过一半 */
		int64_t end = breath_clock_now_us(&c, 22000000ll);   /* 窗口结束 */

		printf("   at +10 s / +20 s : err = %" PRId64 " / %" PRId64 " us\n",
		       12100000ll - mid, 22100000ll - end);
		chk(iabs64(12100000ll - mid) <= 51000ll, "half of error absorbed at half span");
		chk(22100000ll - end == 0, "all error absorbed at end of span");
		/* 缓释期间相位是连续的：不能出现“一步跳完” */
		chk(iabs64((breath_clock_now_us(&c, 6000000ll) - breath_clock_now_us(&c, 5999000ll))
			   - 1000ll) <= 60ll, "slew is smooth (no phase jump)");

		/* 5c. 5 s 偏差：直接跳 */
		breath_clock_init(&c, 0, 0);
		r = breath_clock_observe(&c, 3000000ll, 8000000ll);
		printf("   5 s    -> %d (3=stepped), now=%" PRId64 "\n",
		       r, breath_clock_now_us(&c, 3000000ll));
		chk(r == BREATH_SYNC_STEPPED, "5s stepped");
		chk(breath_clock_now_us(&c, 3000000ll) == 8000000ll, "5s applied at once");
	}

	printf("\n== 6. quartz drift learning (fake clock +50 ppm) ==\n");
	{
		struct breath_clock c;
		const int64_t span_us = 3600ll * 1000 * 1000;   /* 1 h */
		const int64_t drift_us = 180000ll;              /* 50 ppm × 1 h = 180 ms */
		const int64_t u0 = 1000000ll + span_us + drift_us;

		breath_clock_init(&c, 0, 1000000ll);
		/* 1 小时后校时：UTC 比单调时钟多走了 180 ms */
		int r = breath_clock_observe(&c, span_us, u0);

		printf("   learned rate = %" PRId32 " ppm (expect 50)\n", breath_clock_rate_ppm(&c));
		printf("   residual err = %" PRId64 " us (180 ms 漂移，走缓释)\n",
		       u0 - breath_clock_now_us(&c, span_us));
		printf("   slew span    = %" PRId64 " us\n", c.slew_span_us);
		chk(breath_clock_rate_ppm(&c) == 50, "rate learned");
		chk(r == BREATH_SYNC_SLEWING, "180ms residual is slewing");
		chk(c.slew_span_us == 36000000ll, "slew span = 36 s");

		/* 缓释窗口结束后，估计应贴住“真值继续按 50 ppm 走”的外延 */
		int64_t u_later = u0 + 36000000ll + 1800ll;     /* +36 s，含 50 ppm */
		int64_t est = breath_clock_now_us(&c, span_us + 36000000ll);

		printf("   after 36 s   : err = %" PRId64 " us\n", u_later - est);
		chk(iabs64(u_later - est) <= 2000ll, "slew converged onto the true clock");
	}

	printf("\n== 7. channel layer: 1ch LED / 3ch RGB / binary / strip ==\n");
	{
		uint64_t t = 1000000ull;
		uint8_t one[1], three[3];
		const uint8_t base1[1] = {255};
		const uint8_t base3[3] = {255, 255, 255};

		breath_channels(t, base1, 1u, 100u, one);
		breath_channels(t, base3, 3u, 100u, three);
		printf("   env=%u  mono=%u  rgb=(%u,%u,%u)\n",
		       breath_envelope(t), one[0], three[0], three[1], three[2]);
		chk(one[0] == three[0] && one[0] == three[1] && one[0] == three[2],
		    "1ch and 3ch share the same level (same phase)");

		printf("   duty(lv=128): 8bit=%u 12bit=%u 16bit=%u\n",
		       breath_duty(128u, 8u), breath_duty(128u, 12u), breath_duty(128u, 16u));
		chk(breath_duty(255u, 8u) == 255u, "duty full 8bit");
		chk(breath_duty(255u, 12u) == 4095u, "duty full 12bit");
		chk(breath_duty(255u, 16u) == 65535u, "duty full 16bit");

		printf("   binary: t=0 -> %u, t=2000ms -> %u\n",
		       breath_binary(0), breath_binary(2000000ull));
		chk(breath_binary(0) == 0u, "binary dark at phase 0");
		chk(breath_binary(2000000ull) == 1u, "binary lit at peak");

		printf("   strip: p0=%u  p3(step=0)=%u  p3(step=16)=%u\n",
		       breath_pixel_phase(t, 0u, 0u), breath_pixel_phase(t, 3u, 0u),
		       breath_pixel_phase(t, 3u, 16u));
		chk(breath_pixel_phase(t, 3u, 0u) == breath_pixel_phase(t, 0u, 0u),
		    "strip is phase-locked with step=0");
		chk(breath_pixel_phase(t, 3u, 16u) ==
		    (uint8_t)(breath_pixel_phase(t, 0u, 0u) + 48u), "strip wave offset");
	}

	printf("\n== 8. offline degradation: freerun is honest about it ==\n");
	{
		struct breath_clock c, a, b;

		breath_clock_init_freerun(&c, 0);
		printf("   synced=%u  level=%u  budget=%" PRId64 "\n",
		       c.synced, breath_clock_level(&c, 1000000ll),
		       breath_clock_error_budget_us(&c, 1000000ll));
		chk(c.synced == 0u, "freerun reports unsynced");
		chk(breath_clock_level(&c, 1000000ll) == 0u, "freerun level 0");
		chk(breath_gain_guard(80u, 0u) == 40u, "unsynced gain halved");
		chk(breath_gain_guard(80u, 3u) == 80u, "synced gain untouched");

		/* 同一条电源开关同时上电的两台设备：mono 相同 -> 天然同相 */
		breath_clock_init_freerun(&a, 0);
		breath_clock_init_freerun(&b, 0);
		chk(breath_clock_now_us(&a, 3000000ll) == breath_clock_now_us(&b, 3000000ll),
		    "same power-on event -> same phase");
		printf("   both power on together -> utc %" PRId64 " / %" PRId64 "\n",
		       breath_clock_now_us(&a, 3000000ll), breath_clock_now_us(&b, 3000000ll));
	}

	printf("\n== 9. RTC-only device: one shot + calibration ==\n");
	{
		struct breath_clock c;
		const int64_t utc0 = 1735689600000000ll;
		const int64_t h1 = 3600ll * 1000 * 1000;
		const int64_t h24 = 24ll * h1;
		int r;

		breath_clock_init(&c, 0, utc0);
		breath_clock_set_calib(&c, 0, 2);     /* 出厂标定：+2 ppm */
		r = breath_clock_observe_src(&c, 0, utc0, BREATH_SRC_RTC, 0);

		printf("   observe_src=%d  uncertainty=%d us\n", r, c.uncertainty_us);
		printf("   budget @1h = %" PRId64 " us, @24h = %" PRId64 " us\n",
		       breath_clock_error_budget_us(&c, h1),
		       breath_clock_error_budget_us(&c, h24));
		printf("   level  @1h = %u, @24h = %u\n",
		       breath_clock_level(&c, h1), breath_clock_level(&c, h24));
		chk(breath_clock_error_budget_us(&c, h1) < BREATH_LEVEL_STRICT_US,
		    "RTC + 2ppm: still strict at 1 h");
		chk(breath_clock_level(&c, h24) < 3u, "24 h without resync drops below strict");
	}

	printf("\n== 10. save/restore across a reboot ==\n");
	{
		struct breath_clock c, c2;
		struct breath_clock_save sv;

		breath_clock_init(&c, 0, 1735689600000000ll);
		breath_clock_set_calib(&c, 0, 2);
		breath_clock_save_state(&c, 5000000ll, &sv);

		printf("   saved utc=%" PRId64 " rate=%" PRId32 "\n", sv.utc_us, sv.rate_ppm);
		chk(sv.magic == 0x42524854u, "magic ok");

		chk(breath_clock_restore(&c2, &sv, 5000000ll) == 0, "restore ok");
		printf("   restored now=%" PRId64 " (expect %" PRId64 ")\n",
		       breath_clock_now_us(&c2, 5000000ll), sv.utc_us);
		chk(breath_clock_now_us(&c2, 5000000ll) == sv.utc_us, "restored clock continues");
	}

	printf("\n== 11. one 4 s period, sampled every 250 ms ==\n");
	{
		uint32_t t;

		for (t = 0; t <= BREATH_PERIOD_US; t += 250000u) {
			printf("   t=%" PRIu64 " ms  phase=%3u  level=%3u\n",
			       (uint64_t)(t / 1000u), breath_phase(t), breath_envelope(t));
		}
	}

	printf("\n%s (%d failure(s))\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS", g_fail);
	return g_fail ? 1 : 0;
}
