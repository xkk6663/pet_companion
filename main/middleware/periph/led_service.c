/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file led_service.c
 * @brief 中间层：RGB LED 服务（WS2812 on GPIO37, RMT 后端）
 *
 * ★★ 2026-11-15：呼吸灯改为「按绝对 UTC 同步」（规范见 middleware/periph/breath_sync.h）
 *
 * 为什么换掉 led_indicator 的 BSP_LED_BREATHE_SLOW：
 *   - 那个呼吸的相位来自 **本机定时器**（上电时长），两台设备上电时刻不同，
 *     同一面墙上并排看永远不同相，调参数也调不齐；
 *   - 它也只在 2.4 s 左右的包络里按帧率步进，帧率不同的板子还会不同速。
 * 新的规则只有一条：**亮度是绝对时间的纯函数** level = LUT[(utc_us % 4 s)]。
 * 两台设备只要 UTC 对齐，同一微秒算出来的 RGB 逐字节相同——不管芯片、灯型、帧率。
 *
 * 为什么不再用 led_indicator_set_rgb()（而是直接操作 led_strip）：
 *   led_indicator 会先过一层设备 gamma 表（_irgb_convert_to_gamma），
 *   而 breath_sync 的 level 已经是 gamma 2.0 校正后的感知亮度。
 *   再压一次等于把包络变成 gamma^2 曲线，暗部台阶被吃掉，
 *   也违反规范「不许在 breath_frame() 之后再叠二次压亮度」。
 *   ⇒ 这里只把 led_strip 当纯驱动（set_pixel + refresh），亮度全部由 breath_channels 给。
 *
 * 授时通道（当前：SNTP，等级 3）：
 *   - 上电先 breath_clock_init_freerun()：单机呼吸正常，多机只要同时上电也天然同相；
 *   - 一旦 net_time_service 报时间可信，每 1 s 采一口 UTC 喂 observe_src(NETWORK)，
 *     时钟层自己按 ≤25 ms 吸收 / ≤1 s 缓释 / >1 s 跳变处理，**不把相位硬拨**；
 *   - 未授时期间 synced=0 ⇒ breath_gain_guard 把 gain 砍半：现场一眼能看出"这盏灯还没对上表"。
 *   - 以后要接别的授时通道（BLE/ESP-NOW 广播、板载 RTC、GPIO 秒脉冲），
 *     只需把 feed_utc_if_ready() 里的来源换成对应分支，相位规则一个字都不用改。
 *
 * 并发保护（FreeRTOS）：
 *  - 内部建命令队列 + 专用 LED 任务；所有硬件操作串行执行；
 *  - UI(LVGL task) / WIFI(esp event task) 只 post 命令，不直接碰 LED 驱动，
 *    从根本上消除跨任务并发访问 led_strip/RMT 的竞争。
 *  - 主界面 set_status() 反映 Wi-Fi 状态；RGB 页 set_manual() 接管为手动。
 */

#include "led_service.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "led_strip.h"

#include "breath_sync.h"
#include "platform/board_map.h"
#include "platform/port_time.h"

static const char *TAG = "LED_SVC";

/* ---- 同步呼吸参数（规范常数，见 breath_sync.h） ---- */

/* 25 fps：40 ms 一帧，4 s 周期 = 100 帧/周期，256 级包络足够平滑。
 * 帧率不参与相位计算，掉帧只会少一帧画面，不会累积偏移。 */
#define LED_FRAME_PERIOD_MS    40
/* 授时采样间隔。phase 靠 mono 外推，1 s 喂一口足够；
 * 更密不会更准（Slew 吸收会把抖动抹掉），只会多占 CPU。 */
#define LED_TIME_FEED_MS       1000
/* SNTP over WiFi 的 1σ 不确定度（1~30 ms 是常见水平，取保守值 25 ms）。
 * 规范：不确定度宁大勿小；传 0 会退化成该来源的默认值。 */
#define LED_TIME_UNC_US        25000
/* 设备级增益标定：不同型号 LED 的亮度差异固定在这里，不随时间变。
 * 100 = 不标定（满亮）。 */
#define LED_GAIN_PCT           100
/* epoch 下限：早于 2020 的时间视为"校时还没成功"，宁可继续自由运行。
 * 与 net_time_service.c 的合理性判据同源（那里用 1600000000）。 */
#define LED_EPOCH_SANITY_S     1600000000

/* ---- command (decoupled from callers) ---- */
typedef enum {
    LED_CMD_STATUS,
    LED_CMD_RGB,
    LED_CMD_ON,
    LED_CMD_MANUAL,
} led_cmd_id_t;

typedef struct {
    led_cmd_id_t id;
    uint8_t a;
    uint8_t b;
    uint8_t c;
} led_cmd_t;

static QueueHandle_t s_q;
static led_strip_handle_t s_strip;

/* ---- 同步呼吸时钟（单写单读：只有 LED 任务碰它） ---- */
static struct breath_clock s_clk;
static int64_t s_last_feed_mono_us;     /* 上一次喂授时的单调时刻（节流用） */
static uint8_t s_last_level = 0xFF;     /* 0xFF = 还没打印过一致性等级 */

/* ---- state owned by the LED task ---- */
static bool s_manual;
static led_status_t s_status = LED_STATUS_DISCONNECTED;
static uint8_t s_cur_r, s_cur_g, s_cur_b;
static bool s_cur_breathe;
static bool s_led_on = true;
static uint32_t s_hw_err;

/* 上一次真正写进灯里的颜色：值没变就不重复刷新（手动常亮时省掉 25 次/秒的 RMT） */
static bool s_painted_valid;
static uint8_t s_painted_r, s_painted_g, s_painted_b;

/* ------------------------------------------------------------------ 底层 */

/** 单调时钟（since boot，微秒）。相位外推只认它，绝不直接用 time()。 */
static inline int64_t mono_us_now(void)
{
    return esp_timer_get_time();
}

/** 把一帧 RGB 写进 WS2812。返回是否成功：RMT 传输失败是静默的，忽略就会冻在上一帧。 */
static bool hw_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return false;
    }
    esp_err_t err = led_strip_set_pixel(s_strip, 0, r, g, b);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err != ESP_OK) {
        s_hw_err++;
        /* 不每帧刷屏：首次 + 每 100 次各打一条，够定位又不拖累日志 */
        if ((s_hw_err == 1) || ((s_hw_err % 100) == 0)) {
            ESP_LOGW(TAG, "strip write failed x%u: %s",
                     (unsigned)s_hw_err, esp_err_to_name(err));
        }
        return false;
    }
    return true;
}

/**
 * @brief 带缓存的写灯：颜色与上次写入相同就跳过（WS2812 保持上次值，不会掉）。
 *        呼吸模式下暗部有连续的相同值（如 LUT 前 18 项都是 0），跳过是纯收益。
 *        写失败时不更新缓存，下一帧自动重试。
 */
static void paint_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_painted_valid && s_painted_r == r && s_painted_g == g && s_painted_b == b) {
        return;
    }
    if (hw_write(r, g, b)) {
        s_painted_r = r;
        s_painted_g = g;
        s_painted_b = b;
        s_painted_valid = true;
    } else {
        s_painted_valid = false;
    }
}

/* ------------------------------------------------------------------ 授时 */

/**
 * @brief 采样一口 UTC 喂给呼吸时钟（仅在"时间可信"时调用）。
 *
 * 为什么用 gettimeofday 而不是 time()：timeval 带微秒，丢了亚秒部分
 * 相位误差最高 1 s，一致性等级会从 3 掉到 1（规范明令禁止）。
 *
 * ★ 这里读的是 CLOCK_REALTIME，**只用来喂授时**；相位仍由 breath_clock_now_us()
 *   用单调时钟外推，所以校时跳变不会让灯"闪一下"。
 */
static void feed_utc_if_ready(int64_t mono_us)
{
    if (mono_us - s_last_feed_mono_us < (int64_t)LED_TIME_FEED_MS * 1000) {
        return;
    }
    s_last_feed_mono_us = mono_us;

    if (!port_time_valid()) {
        return;                     /* 还没校过时：保持自由运行（半亮） */
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return;
    }
    if (tv.tv_sec < (time_t)LED_EPOCH_SANITY_S) {
        return;
    }

    int64_t utc_us = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
    breath_clock_observe_src(&s_clk, mono_us, utc_us, BREATH_SRC_NETWORK, LED_TIME_UNC_US);
}

/* ------------------------------------------------------------------ 画面 */

/**
 * @brief 刷一帧。
 *
 * 呼吸（status 模式）：level 由 utc 查 LUT 得到，gain 由一致性等级守卫生成，
 *                      色相来自状态色，只改明暗 ⇒ 多机同相、同色。
 * 手动（RGB 页 / 小智灯控）：用户设的颜色不做包络，保持常亮（原行为）。
 */
static void paint_frame(int64_t mono_us)
{
    if (!s_led_on) {
        paint_rgb(0, 0, 0);
        return;
    }

    if (!s_cur_breathe) {
        paint_rgb(s_cur_r, s_cur_g, s_cur_b);
        return;
    }

    int64_t utc_i = breath_clock_now_us(&s_clk, mono_us);
    uint64_t utc = (utc_i > 0) ? (uint64_t)utc_i : 0ull;    /* 未授时：0 也合法，相位仍有序 */

    uint8_t level = breath_clock_level(&s_clk, mono_us);
    if (level != s_last_level) {
        s_last_level = level;
        if (level == 0) {
            ESP_LOGW(TAG, "breath NOT synced -> half brightness (waiting for SNTP)");
        } else {
            ESP_LOGI(TAG, "breath synced: level=%u budget=%lld us",
                     (unsigned)level,
                     (long long)breath_clock_error_budget_us(&s_clk, mono_us));
        }
    }

    uint8_t gain = breath_gain_guard((uint8_t)LED_GAIN_PCT, level);
    uint8_t base[3] = { s_cur_r, s_cur_g, s_cur_b };
    uint8_t out[3];

    breath_channels(utc, base, 3u, gain, out);
    paint_rgb(out[0], out[1], out[2]);
}

/* ------------------------------------------------------------------ 命令 */

static void set_color(uint8_t r, uint8_t g, uint8_t b, bool breathe)
{
    s_cur_r = r;
    s_cur_g = g;
    s_cur_b = b;
    s_cur_breathe = breathe;
}

static void show_status(void)
{
    if (s_status == LED_STATUS_DISCONNECTED) {
        set_color(255, 0, 0, true);
    } else if (s_status == LED_STATUS_CONNECTING) {
        set_color(0, 80, 255, true);
    } else {
        set_color(0, 255, 0, true);
    }
}

static void handle_cmd(const led_cmd_t *cmd)
{
    switch (cmd->id) {
    case LED_CMD_STATUS:
        if (s_manual) {
            break;
        }
        s_status = (led_status_t)cmd->a;
        show_status();
        break;
    case LED_CMD_RGB:
        set_color(cmd->a, cmd->b, cmd->c, false);
        break;
    case LED_CMD_ON:
        s_led_on = cmd->a ? true : false;
        break;
    case LED_CMD_MANUAL:
        s_manual = cmd->a ? true : false;
        if (!s_manual) {
            show_status();
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ 任务 */

static void led_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LED task ready: synced breath 4 s / 25 fps");

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        led_cmd_t cmd;
        /* 阻塞等命令，最长一帧：没命令时这就是帧节拍 */
        if (xQueueReceive(s_q, &cmd, pdMS_TO_TICKS(LED_FRAME_PERIOD_MS)) == pdPASS) {
            handle_cmd(&cmd);
        }

        int64_t mono_us = mono_us_now();
        feed_utc_if_ready(mono_us);
        paint_frame(mono_us);

        /* 命令来得密集时也别超频：帧率锁定在 25 fps */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_FRAME_PERIOD_MS));
    }
}

static void post(led_cmd_id_t id, uint8_t a, uint8_t b, uint8_t c)
{
    led_cmd_t cmd = { .id = id, .a = a, .b = b, .c = c };
    if (s_q) {
        xQueueSend(s_q, &cmd, 0);
    }
}

/* ------------------------------------------------------------------ 对外 */

void led_service_init(void)
{
    /* 规范要求：表必须对得上（校验和 0x51521FF6 + 验收向量）。
     * 表错了接硬件只会看到"灯在动但不同相"，所以先自检再点灯。 */
    int rc = breath_selftest();
    ESP_LOGI(TAG, "breath_selftest=%d (%s), LUT checksum=0x%08X",
             rc, (rc == 0) ? "PASS" : "FAIL",
             (unsigned)breath_lut_checksum());

    /* 必须在第一个刷灯线程启动之前初始化（breath_boot 先于刷灯） */
    breath_clock_init_freerun(&s_clk, mono_us_now());

    /* 纯驱动：WS2812 / GPIO37 / RMT 10 MHz，参数与 BSP 原配置一致 */
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_LED_RGB_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        /* 显式写明 GRB：WS2812 的物理顺序是 G-R-B，不写会落到默认值，
         * 一旦驱动版本改动默认就变成 R/G 互换（红蓝反色很难查）。 */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* BSP 原配置，WS2812 位时序按 10 MHz 分 */
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led strip create failed: %s", esp_err_to_name(err));
        s_strip = NULL;
    }

    /* boot: red breathing = not connected（未授时期间是半亮，一眼能看出"还没对上表"）
     * ★ 必须在建任务之前设好：任务第一帧就该是红色，否则会先黑一帧再变红 */
    set_color(255, 0, 0, true);

    s_q = xQueueCreate(8, sizeof(led_cmd_t));
    if (!s_q) {
        ESP_LOGE(TAG, "led cmd queue create failed, service NOT started");
        return;
    }
    /* 栈 2048：整型呼吸计算 + gettimeofday + RMT 调用，无大缓冲；原实现峰值 656 B */
    if (xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "led task create failed");
    }

    ESP_LOGI(TAG, "RGB LED init: GPIO37, red synced-breathe=disconnected");
}

void led_service_set_status(led_status_t status)   { post(LED_CMD_STATUS, status, 0, 0); }
void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b) { post(LED_CMD_RGB, r, g, b); }
void led_service_set_on(bool on)                  { post(LED_CMD_ON, on ? 1 : 0, 0, 0); }
void led_service_set_manual(bool manual)          { post(LED_CMD_MANUAL, manual ? 1 : 0, 0, 0); }
