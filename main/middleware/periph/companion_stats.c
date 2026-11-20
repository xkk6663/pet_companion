/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "companion_stats.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "middleware/periph/sdcard_service.h"
#include "platform/port_time.h"   /* ★ P2：分桶日期只认平台层可信时间 */

#define TAG "COMPANION_ST"

#define CS_DIR      SDCARD_MNT_PATH "/companion"
/* 路径缓冲：目录(25) + "/" + 桶名(d_2026-11-19 = 13) + ".txt"(4) + ".tmp"(4) + NUL ⇒ 48 够用，
 * 但 GCC 的 -Wformat-truncation 只看 sizeof(tmp) 与来源上限 —— 留足余量到 96 才不报警。 */
#define CS_PATH_LEN 96

/* 落盘节流：攒满 30 s 写一次；写一次 = 读一个小文件 + 写一个更小文件 */
#define CS_FLUSH_EVERY_S 30

static SemaphoreHandle_t s_lock;
static int64_t  s_pending_s;       /* 还没写进 SD 的秒数 */
static bool     s_pending_in_bucket; /* 这一笔已经进了桶（重试时不再加进桶） */
static bool     s_warned_no_sd;    /* "没挂载"只报一次，别每 30 s 刷屏 */

static void reconcile_total(void);   /* 定义在后（要用 list_days），自检时调一次 */

static SemaphoreHandle_t lock(void){
    if (s_lock == NULL) {
        /* 首次调用就在这里建（幂等；两个任务同时进来最多多建一个，先到先得） */
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m == NULL) {
            return NULL;
        }
        if (s_lock == NULL) {
            s_lock = m;
        } else {
            vSemaphoreDelete(m);
        }
    }
    return s_lock;
}

static void ensure_dir(void)
{
    /* EEXIST 是正常情况（第二次以后都是它） */
    (void)mkdir(CS_DIR, 0777);
}

/** 桶名：已校时 → 日期；未校时 → nodate（"所有日子的合计池"）
 *  ★ P2：改读 platform/port_time（唯一可信时间入口），不再反向依赖 net_time_service
 *    —— 陪伴统计是驱动级"落盘分桶"，不该认识 WiFi 校时服务。 */
static void bucket_name(char *out, size_t cap)
{
    int y = 0, mo = 0, d = 0;
    if (port_time_date(&y, &mo, &d)) {
        snprintf(out, cap, "d_%04d-%02d-%02d", y, mo, d);
    } else {
        snprintf(out, cap, "d_nodate");
    }
}

static int64_t read_bucket(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    long long v = 0;
    if (fscanf(f, "%lld", &v) != 1 || v < 0) {
        v = 0;
    }
    fclose(f);
    return (int64_t)v;
}

static esp_err_t write_bucket(const char *path, int64_t total)
{
    /* 先写 .tmp 再换名：中途掉电最多少一次增量，不会把累计值写坏 */
    /* tmp 比 path 长 8：snprintf 的上界是 strlen(path)+4，同尺寸会让
     * GCC 的 -Wformat-truncation 认为可能截断（-Werror 下直接失败）。 */
    char tmp[CS_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s failed (errno=%d)", tmp, errno);
        return ESP_FAIL;
    }
    /* ★ 关键：fprintf 的返回值必须查。picolibc 下写失败不会崩，只会返回 -1
     *   ⇒ 旧版"写完就 rename"会把一个 **0 字节的 tmp** 换成正式文件，
     *     卡上就出现 0 KB 的桶文件（用户实测到的现象）。 */
    int n = fprintf(f, "%lld\n", (long long)total);
    int werr = ferror(f);
    fclose(f);
    if (n <= 0 || werr) {
        ESP_LOGW(TAG, "write %s failed (ret=%d ferror=%d errno=%d)",
                 tmp, n, werr, errno);
        (void)remove(tmp);          /* 绝不把 0 字节文件换名为正式桶 */
        return ESP_FAIL;
    }

    /* 二次确认：落盘后大小必须 > 0（有些 VFS 实现只在 close 时才真正写 FAT 表） */
    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "%s size check failed (stat=%d size=%ld)",
                 tmp, (int)stat(tmp, &st), (long)(st.st_size));
        (void)remove(tmp);
        return ESP_FAIL;
    }

    /* FATFS 的 rename 不允许目标已存在 ⇒ 先删旧文件再换名 */
    (void)remove(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno=%d)", tmp, path, errno);
        (void)remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t flush_locked(void)
{
    if (s_pending_s <= 0) {
        return ESP_OK;
    }
    if (!sdcard_service_is_mounted()) {
        if (!s_warned_no_sd) {
            s_warned_no_sd = true;
            ESP_LOGW(TAG, "SD not mounted: %lld s kept in RAM until card is back",
                     (long long)s_pending_s);
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_warned_no_sd = false;

    ensure_dir();

    char key[24];
    bucket_name(key, sizeof(key));

    char path[CS_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.txt", CS_DIR, key);

    char tpath[CS_PATH_LEN];
    snprintf(tpath, sizeof(tpath), "%s/d_total.txt", CS_DIR);

    /* ★★ 两阶段提交（2026-11-20，修"总池自己变多"）：旧写法"桶写失败也照样
     *   写总池、pending 不清"，下一次重试时 pending 又加一遍到总池 ⇒ 总池比
     *   Σ(桶) 多一截（实测多 180 s = 30+60+90，正是三次连续失败的累加）。
     *   现在：先写桶，成功才写总池；两个都成功才清 pending。
     *   `s_pending_in_bucket` 记住"这笔已经进了桶"，重试时不会重复加。 */
    if (!s_pending_in_bucket) {
        esp_err_t e1 = write_bucket(path, read_bucket(path) + s_pending_s);
        if (e1 != ESP_OK) {
            ESP_LOGW(TAG, "bucket %s write failed, %lld s kept for retry",
                     key, (long long)s_pending_s);
            return e1;
        }
        s_pending_in_bucket = true;
    }

    int64_t grand = read_bucket(tpath) + s_pending_s;
    esp_err_t e2 = write_bucket(tpath, grand);
    if (e2 != ESP_OK) {
        ESP_LOGW(TAG, "total pool write failed, %lld s kept for retry",
                 (long long)s_pending_s);
        return e2;                      /* 桶已更新，总池下次再补 */
    }

    ESP_LOGI(TAG, "+%lld s -> %s | total = %lld s (%lld:%02lld:%02lld)",
             (long long)s_pending_s, key, (long long)grand,
             (long long)(grand / 3600), (long long)((grand / 60) % 60),
             (long long)(grand % 60));
    s_pending_s = 0;
    s_pending_in_bucket = false;
    return ESP_OK;
}

esp_err_t companion_stats_add(int64_t sec)
{
    if (sec <= 0) {
        return ESP_OK;
    }
    SemaphoreHandle_t m = lock();
    if (m) {
        xSemaphoreTake(m, portMAX_DELAY);
    }
    s_pending_s += sec;
    bool due = (s_pending_s >= CS_FLUSH_EVERY_S);
    esp_err_t err = due ? flush_locked() : ESP_OK;
    if (m) {
        xSemaphoreGive(m);
    }
    return err;
}

esp_err_t companion_stats_flush(void)
{
    SemaphoreHandle_t m = lock();
    if (m) {
        xSemaphoreTake(m, portMAX_DELAY);
    }
    esp_err_t err = flush_locked();
    if (m) {
        xSemaphoreGive(m);
    }
    return err;
}

int64_t companion_stats_get_total(void)
{
    if (!sdcard_service_is_mounted()) {
        return 0;
    }
    char path[CS_PATH_LEN];
    snprintf(path, sizeof(path), "%s/d_total.txt", CS_DIR);
    return read_bucket(path);
}

int64_t companion_stats_get_nodate(void)
{
    if (!sdcard_service_is_mounted()) {
        return 0;
    }
    char path[CS_PATH_LEN];
    snprintf(path, sizeof(path), "%s/d_nodate.txt", CS_DIR);
    return read_bucket(path);
}

/**
 * 写盘自检（**只在相机页建页时跑一次**，给"文件是 0 KB"这类问题留证据）。
 * 逐步打印 mkdir / fopen / fprintf / fclose / stat / rename 的结果，
 * 失败时带 errno —— 现场一眼就能看出是"没目录""只读卡""写失败"哪种。
 */
void companion_stats_selftest(void)
{
    /* 每开机只跑一次：相机页每次建页都会调它，重复写探针文件没必要。
     * ★ 只有真正挂上卡才算跑过 —— 开机时卡还没就绪的话，下次进页面再试。 */
    static bool s_done;
    if (s_done) {
        return;
    }
    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "[selftest] SD not mounted -> skip");
        return;
    }
    s_done = true;

    ensure_dir();

    char ftmp[CS_PATH_LEN + 8];
    snprintf(ftmp, sizeof(ftmp), "%s/selftest.tmp", CS_DIR);
    char fdst[CS_PATH_LEN];
    snprintf(fdst, sizeof(fdst), "%s/selftest.txt", CS_DIR);

    FILE *f = fopen(ftmp, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "[selftest] fopen %s FAILED errno=%d", ftmp, errno);
        return;
    }
    int n = fprintf(f, "selftest 12345\n");
    int werr = ferror(f);
    fclose(f);

    struct stat st;
    int sr = stat(ftmp, &st);
    ESP_LOGI(TAG, "[selftest] write ret=%d ferror=%d | stat=%d size=%ld",
             n, werr, sr, (sr == 0) ? (long)st.st_size : -1L);

    (void)remove(fdst);
    int rr = rename(ftmp, fdst);
    ESP_LOGI(TAG, "[selftest] rename ret=%d errno=%d", rr, (rr == 0) ? 0 : errno);
    if (rr == 0 && stat(fdst, &st) == 0) {
        ESP_LOGI(TAG, "[selftest] final %s size=%ld", fdst, (long)st.st_size);
    }
    (void)remove(fdst);

    /* 自愈：总池定义上就是 Σ(日期桶) + nodate。历史上有过重复计数就拉回来。 */
    reconcile_total();
}

/* ===========================================================================
 *  运行时镜像 + 明细枚举（局域网监视面板用）
 * ========================================================================== */

/* ★ 32 位标量：RISC-V 上没有原子的 64 位读写，镜像用 int32 就够（68 年）。
 *   写方是 LVGL 任务（50 ms 一拍），读方是 httpd 任务；单写单读的标量
 *   即使读到"上一拍"的值也只是一帧的误差，不需要锁。 */
static volatile int32_t s_sess_sec;
static volatile bool    s_sess_run;
static volatile bool    s_sess_paused;

void companion_stats_report_session(int32_t sec, bool running, bool paused)
{
    s_sess_sec    = (sec > 0) ? sec : 0;
    s_sess_run    = running;
    s_sess_paused = paused;
}

int32_t companion_stats_get_session(bool *running, bool *paused)
{
    if (running) { *running = s_sess_run; }
    if (paused)  { *paused  = s_sess_paused; }
    return s_sess_sec;
}

static int day_cmp(const void *a, const void *b)
{
    const companion_day_t *x = (const companion_day_t *)a;
    const companion_day_t *y = (const companion_day_t *)b;
    return strcmp(x->date, y->date);
}

/* 桶文件名必须是 d_YYYY-MM-DD.txt（长度 **16**，数字位都得是数字） */
static bool is_day_name(const char *nm)
{
    if (strlen(nm) != 16 || strncmp(nm, "d_", 2) != 0 || strcmp(nm + 12, ".txt") != 0) {
        return false;
    }
    if (nm[6] != '-' || nm[9] != '-') {
        return false;
    }
    for (int i = 2; i <= 10; i++) {
        if (i == 6 || i == 9) {
            continue;
        }
        if (nm[i] < '0' || nm[i] > '9') {
            return false;
        }
    }
    return true;
}

#define CS_LIST_HARD_MAX 120   /* sanity：一天一个文件，120 天足够 */

int companion_stats_list_days(companion_day_t *out, int max)
{
    if (out == NULL || max <= 0 || !sdcard_service_is_mounted()) {
        return 0;
    }
    DIR *d = opendir(CS_DIR);
    if (d == NULL) {
        ESP_LOGW(TAG, "list_days: opendir %s failed errno=%d", CS_DIR, errno);
        return 0;
    }

    companion_day_t *all = (companion_day_t *)malloc(sizeof(companion_day_t) * CS_LIST_HARD_MAX);
    if (all == NULL) {
        closedir(d);
        return 0;
    }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < CS_LIST_HARD_MAX) {
        if (!is_day_name(e->d_name)) {
            continue;
        }
        char path[CS_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", CS_DIR, e->d_name);
        all[n].sec = (int32_t)read_bucket(path);
        memcpy(all[n].date, e->d_name + 2, 10);
        all[n].date[10] = '\0';
        n++;
    }
    closedir(d);

    qsort(all, (size_t)n, sizeof(companion_day_t), day_cmp);

    /* 只保留**最近**的 max 天（排序后末尾最新） */
    int skip = (n > max) ? (n - max) : 0;
    int k = n - skip;
    memcpy(out, all + skip, sizeof(companion_day_t) * (size_t)k);
    free(all);
    return k;
}

/**
 * 总池自愈：**定义上** d_total == Σ(d_YYYY-MM-DD) + d_nodate。
 * 历史版本曾有过"桶写失败也加总池"的重复计数，这里按定义重算一次；
 * 只有在明显不一致（差值 ≥ 1 s）时才回写文件，避免每进一次页面就写 SD。
 * 正常情况（两边相等）什么都不做，只留一条 INFO 之外的静默。
 */
static void reconcile_total(void)
{
    if (!sdcard_service_is_mounted()) {
        return;
    }
    ensure_dir();

    companion_day_t *ds = (companion_day_t *)malloc(sizeof(companion_day_t) * 64);
    if (ds == NULL) {
        return;
    }
    int n = companion_stats_list_days(ds, 64);

    int64_t sum = companion_stats_get_nodate();
    for (int i = 0; i < n; i++) {
        sum += ds[i].sec;
    }
    free(ds);

    char tpath[CS_PATH_LEN];
    snprintf(tpath, sizeof(tpath), "%s/d_total.txt", CS_DIR);
    int64_t cur = read_bucket(tpath);

    if (cur == sum) {
        return;
    }
    ESP_LOGW(TAG, "total pool drift: file=%lld s, sum(buckets)=%lld s -> rewrite",
             (long long)cur, (long long)sum);
    if (write_bucket(tpath, sum) == ESP_OK) {
        ESP_LOGI(TAG, "total pool reconciled to %lld s (%lld:%02lld:%02lld)",
                 (long long)sum, (long long)(sum / 3600),
                 (long long)((sum / 60) % 60), (long long)(sum % 60));
    }
}
