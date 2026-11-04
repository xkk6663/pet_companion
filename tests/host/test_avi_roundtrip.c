/*
 * host 侧单测：AVI(MJPG) 封装 ↔ 解封装往返。
 * 被测文件自带"纯 C99、可脱离 IDF 编译"的约束，这里就按那个约束真编译真跑。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_mjpeg_muxer.h"
#include "avi_mjpeg_demuxer.h"

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i);
    }
}

static int file_has_marker(const char *path, const char *marker)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long n = ftell(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    int found = 0;
    size_t mlen = strlen(marker);
    for (size_t i = 0; i + mlen <= got; i++) {
        if (memcmp(buf + i, marker, mlen) == 0) { found = 1; break; }
    }
    free(buf);
    return found;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "avi_roundtrip.avi";
    const char *abort_path = (argc > 2) ? argv[2] : "avi_abort.avi";
    uint8_t f1[100], f2[121], f3[1000];

    fill(f1, sizeof(f1), 0x10);
    fill(f2, sizeof(f2), 0x20);
    fill(f3, sizeof(f3), 0x30);

    /* ---- 参数闸 ---- */
    CHECK(avi_muxer_open(path, 0, 480, 12) == NULL);
    CHECK(avi_muxer_open(path, 640, 0, 12) == NULL);
    CHECK(avi_muxer_open(path, 640, 480, 0) == NULL);
    CHECK(avi_muxer_open(NULL, 640, 480, 12) == NULL);

    /* ---- 写 ---- */
    avi_muxer_t *m = avi_muxer_open(path, 640, 480, 12);
    CHECK(m != NULL);
    if (!m) return 1;

    avi_stats_t st;
    avi_muxer_get_stats(m, &st);
    CHECK(st.frames == 0);
    CHECK(avi_muxer_add_jpeg(m, NULL, 10) == AVI_ERR_ARG);
    CHECK(avi_muxer_add_jpeg(m, f1, 0) == AVI_ERR_ARG);
    CHECK(avi_muxer_add_jpeg(m, f1, sizeof(f1)) == AVI_OK);
    CHECK(avi_muxer_add_jpeg(m, f2, sizeof(f2)) == AVI_OK);   /* 奇数长度补 pad */
    CHECK(avi_muxer_add_jpeg(m, f3, sizeof(f3)) == AVI_OK);
    avi_muxer_get_stats(m, &st);
    CHECK(st.frames == 3);
    CHECK(st.max_frame_size == 1000);
    CHECK(st.avg_frame_size >= 406 && st.avg_frame_size <= 408);

    avi_muxer_set_real_fps(m, 963);
    CHECK(avi_muxer_close(m) == AVI_OK);
    CHECK(file_has_marker(path, "idx1"));

    /* ---- 读：元信息 ---- */
    avi_demux_t *d = avi_demux_open(path);
    CHECK(d != NULL);
    if (!d) return 1;
    uint32_t fps = 0, total = 0, w = 0, h = 0;
    avi_demux_get_info(d, &fps, &total, &w, &h);
    CHECK(fps == 963);
    CHECK(total == 3);
    CHECK(w == 640);
    CHECK(h == 480);

    /* ---- 读：逐帧字节完全一致 ---- */
    uint8_t buf[1100];
    uint32_t len = 0;
    CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_OK);
    CHECK(len == sizeof(f1) && memcmp(buf, f1, sizeof(f1)) == 0);
    CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_OK);
    CHECK(len == sizeof(f2) && memcmp(buf, f2, sizeof(f2)) == 0);
    CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_OK);
    CHECK(len == sizeof(f3) && memcmp(buf, f3, sizeof(f3)) == 0);
    CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_EOF);
    avi_demux_close(d);

    /* ---- 读：缓冲不够必须报格式错，而不是越界写 ---- */
    d = avi_demux_open(path);
    CHECK(d != NULL);
    if (d) {
        CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_OK);
        CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &len) == AVID_OK);
        CHECK(avi_demux_next_frame(d, buf, 200, &len) == AVID_ERR_FMT);
        avi_demux_close(d);
    }

    /* ---- 非法文件：不是本封装布局 ---- */
    FILE *bad = fopen(abort_path, "wb");
    CHECK(bad != NULL);
    if (bad) {
        fwrite("hello", 1, 5, bad);
        fclose(bad);
    }
    CHECK(avi_demux_open(abort_path) == NULL);

    /* ---- 异常收尾：不生成 idx1，但已写帧仍可顺序读 ---- */
    m = avi_muxer_open(abort_path, 320, 240, 12);
    CHECK(m != NULL);
    if (m) {
        CHECK(avi_muxer_add_jpeg(m, f1, sizeof(f1)) == AVI_OK);
        avi_muxer_abort(m);
        CHECK(!file_has_marker(abort_path, "idx1"));
        d = avi_demux_open(abort_path);
        CHECK(d != NULL);
        if (d) {
            uint32_t got = 0;
            CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &got) == AVID_OK);
            CHECK(got == sizeof(f1) && memcmp(buf, f1, sizeof(f1)) == 0);
            CHECK(avi_demux_next_frame(d, buf, sizeof(buf), &got) == AVID_EOF);
            avi_demux_close(d);
        }
    }

    if (fails) {
        printf("AVI roundtrip: %d failure(s)\n", fails);
        return 1;
    }
    printf("AVI roundtrip: all checks passed\n");
    return 0;
}
