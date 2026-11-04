/*
 * host 侧单测：人脸框质量门槛（main/middleware/camera/face_quality.c）。
 * 判据出处：docs/项目规划/S8_人脸识别打卡系统方案.md §5.6；
 * 这里把"240 尺度下的 60/80 px"边界逐个钉死。
 */
#include <stdio.h>
#include <string.h>

#include "face_quality.h"

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

static vision_result_t box(int x, int y, int w, int h, float score)
{
    vision_result_t r;
    memset(&r, 0, sizeof(r));
    r.x = x; r.y = y; r.w = w; r.h = h; r.score = score;
    return r;
}

int main(void)
{
    face_quality_t q;

    /* ---- 空输入 ---- */
    q.ok = true;
    face_quality_check(NULL, 240, 240, &q);
    CHECK(q.has_face == false && q.ok == false);
    CHECK(q.score_pct == 0);
    CHECK(strcmp(q.hint, "No face in view") == 0);

    face_quality_check(NULL, 240, 240, NULL);   /* 不崩即可 */

    vision_result_t r = box(80, 80, 80, 80, 0.60f);
    face_quality_check(&r, 0, 240, &q);
    CHECK(q.has_face == false && q.ok == false);

    r = box(0, 0, 0, 80, 0.90f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.has_face == false && q.ok == false);

    /* ---- 正中间、刚好达标 ---- */
    r = box(80, 80, 80, 80, 0.60f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.has_face == true);
    CHECK(q.ok == true);
    CHECK(strcmp(q.hint, "Ready") == 0);
    CHECK(q.score_pct == 60);

    /* ---- 置信度：0.599 不通过，显示仍四舍五入到 60% ---- */
    r = box(80, 80, 80, 80, 0.599f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.ok == false);
    CHECK(strcmp(q.hint, "Hold still (low conf)") == 0);
    CHECK(q.score_pct == 60);

    /* ---- 占比：79 px 不通过，80 px 通过 ---- */
    r = box(80, 80, 79, 80, 0.90f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.ok == false);
    CHECK(strcmp(q.hint, "Move closer") == 0);

    r = box(80, 80, 80, 80, 0.90f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.ok == true);

    /* ---- 居中：偏移 60 px 通过，61 px 不通过 ---- */
    r = box(140, 40, 80, 80, 0.90f);            /* 中心 180，距 120 = 60 */
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.ok == true);

    r = box(141, 40, 80, 80, 0.90f);            /* 中心 181 -> 61 */
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.ok == false);
    CHECK(strcmp(q.hint, "Center the face") == 0);

    /* ---- 显示值四舍五入 ---- */
    r = box(80, 80, 80, 80, 0.605f);
    face_quality_check(&r, 240, 240, &q);
    CHECK(q.score_pct == 61);

    if (fails) {
        printf("face_quality: %d failure(s)\n", fails);
        return 1;
    }
    printf("face_quality: all checks passed\n");
    return 0;
}
