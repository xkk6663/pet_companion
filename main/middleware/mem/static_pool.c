/* static_pool.c - 编译期静态内部内存池（设计说明见 static_pool.h） */

#include "static_pool.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SPOOL";

#define SP_ALIGN   16
#define SP_ROUND(n) (((n) + (SP_ALIGN - 1)) & ~((size_t)(SP_ALIGN - 1)))

/* ★ 编译期划定的内部 RAM：进 .bss（DIRAM）。16 B 对齐是 FreeRTOS 栈的最低要求，
 *   顺带也满足 esp-dl 对缓冲的 16 B 对齐判据。 */
static uint8_t s_pool[STATIC_POOL_TOTAL] __attribute__((aligned(SP_ALIGN)));

static size_t s_used;
/** 只在申请瞬间持锁：bump 分配本身不可重入（两个任务同时算 s_used 会重叠）。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void *static_pool_alloc(size_t bytes, const char *who)
{
    if (bytes == 0) {
        return NULL;
    }

    size_t need = SP_ROUND(bytes);

    portENTER_CRITICAL(&s_lock);
    if (s_used + need > STATIC_POOL_TOTAL) {
        size_t used = s_used;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "pool exhausted: want %u B for %s | used=%u total=%d",
                 (unsigned)need, who ? who : "?", (unsigned)used, STATIC_POOL_TOTAL);
        return NULL;
    }
    void *p = &s_pool[s_used];
    s_used += need;
    size_t used = s_used;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "alloc %u B for %s | used=%u/%d (free=%u)",
             (unsigned)need, who ? who : "?", (unsigned)used, STATIC_POOL_TOTAL,
             (unsigned)(STATIC_POOL_TOTAL - used));
    return p;
}

size_t static_pool_used(void)
{
    portENTER_CRITICAL(&s_lock);
    size_t used = s_used;
    portEXIT_CRITICAL(&s_lock);
    return used;
}

size_t static_pool_free(void)
{
    return STATIC_POOL_TOTAL - static_pool_used();
}
