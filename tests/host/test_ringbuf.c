/*
 * host 侧单测：main/3rd/ringbuf 的单生产/单消费环形缓冲。
 * 重点盯三件事：满/空边界、跨镜像回绕、put_force 的"丢旧留新"。
 */
#include <stdio.h>
#include <string.h>

#include "ringbuf.h"

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

int main(void)
{
    uint8_t pool[16];
    ringbuf_t rb;

    ringbuffer_init(&rb, pool, sizeof(pool));
    CHECK(ringbuffer_data_len(&rb) == 0);
    CHECK(ringbuffer_space_len(&rb) == 16);

    uint8_t out[32];
    CHECK(ringbuffer_get(&rb, out, 4) == 0);

    /* ---- 基本读写 ---- */
    const uint8_t *abc = (const uint8_t *)"ABC";
    CHECK(ringbuffer_put(&rb, abc, 3) == 3);
    CHECK(ringbuffer_data_len(&rb) == 3);
    CHECK(ringbuffer_get(&rb, out, 3) == 3);
    CHECK(memcmp(out, abc, 3) == 0);
    CHECK(ringbuffer_data_len(&rb) == 0);

    /* ---- 满：再写必须返回 0 ---- */
    const uint8_t *all = (const uint8_t *)"0123456789ABCDEF";
    CHECK(ringbuffer_put(&rb, all, 16) == 16);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_space_len(&rb) == 0);
    CHECK(ringbuffer_put(&rb, abc, 3) == 0);

    /* ---- 回绕：读一半后再写，数据顺序必须仍然正确 ---- */
    CHECK(ringbuffer_get(&rb, out, 8) == 8);
    CHECK(memcmp(out, "01234567", 8) == 0);
    const uint8_t *tail = (const uint8_t *)"IJKLMNOP";
    CHECK(ringbuffer_put(&rb, tail, 8) == 8);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, "89ABCDEFIJKLMNOP", 16) == 0);
    CHECK(ringbuffer_data_len(&rb) == 0);

    uint8_t big[20];
    for (int i = 0; i < 20; i++) big[i] = (uint8_t)('a' + i);

    /* ---- 空间不足时按可用空间截断，而不是整笔丢弃 ---- */
    ringbuffer_init(&rb, pool, sizeof(pool));
    const uint8_t *five = (const uint8_t *)"HELLO";
    CHECK(ringbuffer_put(&rb, five, 5) == 5);
    CHECK(ringbuffer_put(&rb, big, sizeof(big)) == 11);  /* 只剩 11 B */
    CHECK(ringbuffer_data_len(&rb) == 16);

    /* ---- put_force：超长输入只留最新 buffer_size 字节 ---- */
    ringbuffer_init(&rb, pool, sizeof(pool));
    CHECK(ringbuffer_put_force(&rb, big, sizeof(big)) == 16);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, big + 4, 16) == 0);               /* 最新 16 B = big[4..19] */

    /* ---- 单字节接口与满态 ---- */
    ringbuffer_init(&rb, pool, sizeof(pool));
    uint8_t c = 0;
    CHECK(ringbuffer_putbyte(&rb, 'Z') == 1);
    CHECK(ringbuffer_getbyte(&rb, &c) == 1);
    CHECK(c == 'Z');
    CHECK(ringbuffer_getbyte(&rb, &c) == 0);
    for (int i = 0; i < 16; i++) {
        CHECK(ringbuffer_putbyte(&rb, (uint8_t)i) == 1);
    }
    CHECK(ringbuffer_putbyte(&rb, 'x') == 0);

    /* ---- 初始化对齐：非 4 倍数向下取整 ---- */
    ringbuffer_init(&rb, pool, 15);
    CHECK(rb.buffer_size == 12);

    if (fails) {
        printf("ringbuf: %d failure(s)\n", fails);
        return 1;
    }
    printf("ringbuf: all checks passed\n");
    return 0;
}
