/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_binfont.c
 * @brief 全 CJK bin 字体加载实现（LittleFS → PSRAM → lv_binfont_create_ex 懒加载字形）。
 *
 * 加载链路（★ 故意的两段式，见 F40）：
 *   阶段 1  ui_binfont_load()   ← 不碰 LVGL，必须早于所有 PSRAM 栈任务
 *     /littlefs/fonts/lv_font_cjk_28.bin
 *       --(stat + read, 显式 PSRAM)-->  s_file_buf (~2.7MB)
 *   阶段 2  ui_binfont_attach() ← 需 lv_init()，纯 RAM 无 flash IO
 *     --(lv_binfont_create_ex{ .buffer, .dynamic_glyph_load = true })-->  s_font
 *
 * ★ 为什么必须拆成两段（F40，2026-09-24 上电必崩的修复）：
 *   阶段 1 那次 read 会把 2.7MB 按 16KB 分块读，每块关一次全局 cache；
 *   而 IDF 的 spi_flash_disable_interrupts_caches_and_other_cpu()
 *   （cache_utils.c）**只关 cache、不停另一核** —— 窗口内任何核上的
 *   任何 PSRAM 访问（含他人任务栈的压/弹栈）都会 Cache error。
 *   所以"读"必须发生在 bt_push/bt_sink early_init（PSRAM 栈任务）之前；
 *   而"解析"必须在 lv_init() 之后。两者时机不同，故必须拆分。
 *
 * ★ 为什么必须开 dynamic_glyph_load（字面省 ~1.62MB RAM）：
 *   lv_binfont 的字形其实是"双轴"加载，两件事互不相干：
 *     A 轴 = 字体文件副本是否常驻内存；
 *     B 轴 = 全部字形位图是否在 create 时一次性展开。
 *   `lv_binfont_create_from_buffer()` 只是 `lv_binfont_create_ex()` 的语法糖，
 *   它把 dsc **零初始化** → `dynamic_glyph_load` 默认 false → 于是
 *   `load_glyph()`（lv_binfont_loader.c:650）会 `lv_malloc(glyf 全表展开尺寸)`
 *   再把 21360 个字形位图整块拷进 RAM。
 *   本字体的 glyf 表就有 1,774,476 B，展开后 ≈ 1.71MB —— 而一屏最多用几十个字形，
 *   这 1.7MB 纯属白占（本板 PSRAM 一共 16MB，但 BT 协议栈 + 音频管线 + 双 framebuffer
 *   已经把 PSRAM 压得很紧，见 docs/工程规范/13_内存管理规范）。
 *
 *   改成 `dynamic_glyph_load = true` 后的常驻开销：
 *     · glyph 描述符表  21360 × 12B  ≈ 250KB   （无论哪种模式都省不掉）
 *     · loca 偏移表     21360 × 4B   ≈  85KB   （本字体 index_to_loc_format=1）
 *     · "最大单字形"暂存缓冲         ≈ 200B    （渲染谁读谁，覆盖复用）
 *   净省 ≈ 1.62MB，并且 create 时不再有那一次 1.7MB 的整块 memcpy，
 *   开机 splash 阶段也更短。
 *
 * ★ 为什么 s_file_buf 必须常驻，绝不能 free（旧注释是错的，已作废）：
 *   dynamic_glyph_load=true 时，`lv_binfont_create_ex()` 里的
 *   `file_owned_by_loader` 为真 → LVGL **不会** close 那个 memfs 文件句柄，
 *   而是把它交给 glyph_loader 长期持有；之后每次渲染都按 loca 偏移从
 *   本能缓冲里现读字形位图（见 load_glyph_bitmap_cb，lv_binfont_loader.c:452）。
 *   所以释放 s_file_buf = 悬空指针，必然 HardFault。
 *   旧版本注释里"理论上创建完就能 free 掉源缓冲"的说法只在
 *   dynamic_glyph_load=false 时成立，切勿照做。
 */

#include "ui_binfont.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "middleware/periph/fs_service.h"   /* ★ 本分支适配：挂载点 FS_MNT_PATH="/littlefs" */

#if !LV_USE_FS_MEMFS
#error "ui_binfont 依赖 LV_USE_FS_MEMFS：请在 sdkconfig.defaults 打开 \
CONFIG_LV_USE_FS_MEMFS=y 与 CONFIG_LV_FS_MEMFS_LETTER（见 'btsink Phase 5' 段）"
#endif

static const char *TAG = "UI_BINFONT";

/** bin 字体路径（随 storage 分区镜像一起烧录，见 littlefs_content/fonts/） */
#define BINFONT_PATH   FS_MNT_PATH "/fonts/lv_font_cjk_28.bin"

static lv_font_t *s_font      = NULL;  /* 加载好的 LVGL 字体 */
static uint8_t   *s_file_buf  = NULL;  /* bin 文件副本，常驻 PSRAM（见文件头） */
static size_t     s_file_len  = 0;     /* 已读入的字节数 */
static bool       s_load_tried = false;  /* 阶段 1（读文件）只尝试一次 */
static bool       s_attach_tried = false;/* 阶段 2（解析成字体）只尝试一次 */

lv_font_t *ui_binfont_get(void)
{
    return s_font;
}

/* ================================================================== *
 *  文本降级：只放行 bin 字表里真有字形的码点（实现说明见 .h）
 * ================================================================== */

/* ★★ 字表覆盖范围是**从 bin 文件里实测解析出来的**，不是照生成参数猜的。
 *
 *   `littlefs_content/fonts/lv_font_cjk_28.bin`：28 px / 1bpp / 2.62 MB /
 *   **32,955 个字形 / 11 段 cmap**（解析方法见 改动记录 §25）：
 *
 *     U+0020-007E  ASCII 95        U+3000-303F  CJK 标点 64
 *     U+00A0-00FF  Latin-1 96      U+3041-30FF  假名 189（含 ・U+30FB）
 *     U+2002-2051  通用标点 34     U+3131-318E  韩文兼容字母 94（含 ㅂU+3142）
 *     U+4E00-9FEF  CJK 汉字 20976  U+AC00-D7A3  韩文音节 11172
 *     U+FF01-FFBE / FFC2-FFEE 全角+半角 235
 *
 *   ⇒ 中文、日文假名、韩文、全角符号、弯引号 “ ” ‘ ’、破折号 — –、
 *     省略号 …、°·×÷´、‼ 全都**能显示**，别再往 ASCII 降级了。
 *   仍然没有的：emoji 本体（U+1F300+ / U+2600-27BF）、几何图形（▽○●■）、
 *     箭头（→←）、数学（≥≤∞∀）、希腊字母（ω）、♪♥。这些才需要替换或丢弃。
 *
 * ★ 改字表（重烧 storage 里的 bin）时**必须重新解析一遍并同步这里**，
 *   否则本函数放行的字在字体里没有字形，等于白放。
 */
/* U+2002..U+2051 里**真实存在字形**的 34 个码点（bin cmap 实测）。 */
static const uint16_t PUNCT_2000[] = {
    0x2002, 0x2003, 0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015,
    0x2016, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020,
    0x2021, 0x2022, 0x2025, 0x2026, 0x2027, 0x2030, 0x2032, 0x2033,
    0x2035, 0x2039, 0x203A, 0x203B, 0x203C, 0x2042, 0x2047, 0x2048,
    0x2049, 0x2051,
};

static bool cp_in_font(uint32_t cp)
{
    if (cp == (uint32_t)'\n' || cp == (uint32_t)'\r' || cp == (uint32_t)'\t') {
        return true;                       /* 控制符在 0x20 之下，单独放行 */
    }
    if (cp >= 0x0020 && cp <= 0x007E) return true;   /* ASCII 可见字符 */
    if (cp >= 0x00A0 && cp <= 0x00FF) return true;   /* Latin-1：° · × ÷ ´ */
    if (cp >= 0x2002 && cp <= 0x2051) {
        /* ★ 这一段是**稀疏**的：80 个码位里只做了 34 个字形（cmap format=SPARSE_TINY）。
         *   只放行真实存在的那些，否则会渲染成空白把句子断开。 */
        for (size_t i = 0; i < sizeof(PUNCT_2000) / sizeof(PUNCT_2000[0]); i++) {
            if (PUNCT_2000[i] == cp) {
                return true;
            }
        }
        return false;
    }
    if (cp >= 0x3000 && cp <= 0x303F) return true;   /* CJK 标点 */
    if (cp >= 0x3041 && cp <= 0x30FF) return true;   /* 平假名 + 片假名 */
    if (cp >= 0x3131 && cp <= 0x318E) return true;   /* 韩文兼容字母 */
    if (cp >= 0x4E00 && cp <= 0x9FEF) return true;   /* CJK 统一汉字 */
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;   /* 韩文音节 */
    if (cp >= 0xFF01 && cp <= 0xFFBE) return true;   /* 全角形式 */
    if (cp >= 0xFFC2 && cp <= 0xFFEE) return true;   /* 半角形式 */
    return false;
}

/* 常见表情 → ASCII 颜文字。
 * ★ 只收"LLM 真的会吐出来"的那批；表里没有的 emoji 一律丢弃（宁可少一个字，
 *   也别在屏上留空白把句子断开）。 */
static const struct { uint32_t cp; const char *rep; } SMILE_MAP[] = {
    /* ---- 笑脸 / 表情 ---- */
    { 0x1F600, "^_^" }, { 0x1F601, "^_^" }, { 0x1F602, "^^"  }, { 0x1F603, "^_^" },
    { 0x1F604, "^_^" }, { 0x1F605, "^_^" }, { 0x1F606, "^_^" }, { 0x1F609, ";)"  },
    { 0x1F60A, "^_^" }, { 0x1F60B, ";P"  }, { 0x1F60D, "<3"  }, { 0x1F60E, "B)"  },
    { 0x1F60F, ";)"  }, { 0x1F612, "-_-" }, { 0x1F613, "T_T" }, { 0x1F614, "-_-" },
    { 0x1F618, "<3"  }, { 0x1F61C, ";P"  }, { 0x1F61D, ";P"  }, { 0x1F620, ">_<" },
    { 0x1F621, ">_<" }, { 0x1F622, "T_T" }, { 0x1F623, ">_<" }, { 0x1F624, ">_<" },
    { 0x1F625, "T_T" }, { 0x1F62D, "T_T" }, { 0x1F630, ">_<" }, { 0x1F631, "O_O" },
    { 0x1F632, "O_O" }, { 0x1F633, "O_O" }, { 0x1F634, "zzz" }, { 0x1F642, ":)"  },
    { 0x1F643, ":)"  }, { 0x1F644, "O_O" }, { 0x1F914, "-_-" }, { 0x1F970, "<3"  },
    /* ---- 手势 / 符号 ---- */
    { 0x1F44D, "(y)" }, { 0x1F44E, "(n)" }, { 0x1F44C, "(y)" }, { 0x1F44F, "(*)" },
    { 0x1F64F, "(_)" }, { 0x1F4AA, "(y)" }, { 0x1F4AF, "100" }, { 0x1F525, "!!"  },
    { 0x1F389, "*"   }, { 0x1F38A, "*"   }, { 0x1F4A1, "*"   }, { 0x2B50,  "*"   },
    { 0x2728,  "*"   }, { 0x2605,  "*"   }, { 0x2606,  "*"   }, { 0x2705,  "[OK]"},
    { 0x274C,  "[X]" }, { 0x2757,  "!"   }, { 0x2753,  "?"   },
    /* ---- 爱心（字体里没有 U+2665/2661，日式颜文字的 ♥♡ 得换掉）---- */
    { 0x2764,  "<3"  }, { 0x1F494, "</3" }, { 0x1F495, "<3"  }, { 0x1F496, "<3"  },
    { 0x1F497, "<3"  }, { 0x1F49D, "<3"  }, { 0x1F49E, "<3"  },
    { 0x2665,  "<3"  }, { 0x2661, "<3"  },
    /* ---- 字体**真的没有**的符号（这一段才是"必须替换"的）----
     *   箭头 U+2190-21FF、数学 U+2265/2264/221E、几何图形 U+25A0-25FF、
     *   希腊字母 U+03C9、音符 U+266A/266B 全不在 11 段 cmap 里。 */
    { 0x2192, "->"  }, { 0x2190, "<-"  }, { 0x2265, ">="  }, { 0x2264, "<="  },
    { 0x221E, "inf" }, { 0x2200, "V"   }, { 0x03C9, "w"   },
    { 0x25BD, "v"   },   /* ▽ */  { 0x25CB, "o"  },   /* ○ */
    { 0x25CF, "*"   },   /* ● */  { 0x25A0, "#"  },   /* ■ */
    { 0x266A, "~"   },   /* ♪ */  { 0x266B, "~"  },   /* ♫ */
    { 0x203F, "_"   },   /* ‿（U+203F 不在那 34 个通用标点里）*/
    /* ★ 下面这个**不是缺字形**（U+3142 韩文 jamo 字体里有），
     *   只是韩文字母当"omega 嘴"看着怪，故意换成 w。纯观感取舍，可删。 */
    { 0x3142, "w"   },   /* ㅂ */
};

static const char *smile_rep(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(SMILE_MAP) / sizeof(SMILE_MAP[0]); i++) {
        if (SMILE_MAP[i].cp == cp) {
            return SMILE_MAP[i].rep;
        }
    }
    return NULL;
}

/**
 * 解一个 UTF-8 码点。
 * @return 消耗的字节数（>=1）；遇到 NUL 返回 0；非法序列按 1 字节丢弃并给 U+FFFD。
 */
static int utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];
    if (c == 0) {
        return 0;
    }
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int len;
    uint32_t v;
    if ((c & 0xE0) == 0xC0)      { len = 2; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07u; }
    else                         { *cp = 0xFFFD; return 1; }   /* 孤立续字节 */
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i;                          /* 截断的多字节：只丢掉已有的 */
        }
        v = (v << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cp = v;
    return len;
}

size_t ui_binfont_text_fit(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }

    size_t o = 0;
    const char *p = in;
    while (*p != '\0') {
        uint32_t cp = 0;
        int n = utf8_decode(p, &cp);
        if (n <= 0) {
            break;
        }
        const char *rep = smile_rep(cp);
        if (rep != NULL) {
            size_t rl = strlen(rep);
            if (o + rl + 1 > cap) {
                break;                         /* 放不下就整段截断，不留半个字 */
            }
            memcpy(out + o, rep, rl);
            o += rl;
        } else if (cp_in_font(cp)) {
            if (o + (size_t)n + 1 > cap) {
                break;
            }
            memcpy(out + o, p, (size_t)n);
            o += (size_t)n;
        }
        /* 其余码点（emoji / 韩文 / 泰文 / 变体选择符 / ZWJ …）静默丢弃 */
        p += n;
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ *
 * 阶段 1／2：LittleFS → PSRAM（**不碰 LVGL**，必须在 PSRAM 栈任务之前）
 * ------------------------------------------------------------------ */
void ui_binfont_load(void)
{
    if (s_load_tried) {
        return;
    }
    s_load_tried = true;

    struct stat st;
    if (stat(BINFONT_PATH, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "font missing: %s -- CJK will fall back to built-in font", BINFONT_PATH);
        return;
    }

    /* ★ 显式 PSRAM：本字体 ~2.7MB，而此刻内部 RAM 只剩几十 KB
       （见 docs/工程规范/13_内存管理规范）。绝不能落内部。 */
    s_file_buf = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file_buf) {
        ESP_LOGE(TAG, "PSRAM alloc %d failed (PSRAM free=%u)",
                 (int)st.st_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }

    int fd = open(BINFONT_PATH, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open failed: %s", BINFONT_PATH);
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        return;
    }
    /* ★★ F40 现场：这一句就是上电崩溃的爆点。
       esp_flash_read() 对 PSRAM 目标走 16KB 分块 + 每块关/开全局 cache
       （esp_flash_api.c:929 的 read_chunk_size 与 cache_utils.c 的 disable/enable），
       2.7MB ⇒ ~168 个"cache 已关"的窗口。窗口内若有任何 PSRAM 访问即 Cache error。
       所以本函数必须早于所有 PSRAM 栈任务（调用方 main.c 保证）。 */
    ssize_t got = read(fd, s_file_buf, (size_t)st.st_size);
    close(fd);
    if (got != (ssize_t)st.st_size) {
        ESP_LOGE(TAG, "short read: %d/%d", (int)got, (int)st.st_size);
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        return;
    }
    s_file_len = (size_t)st.st_size;
    ESP_LOGI(TAG, "bin loaded: %d bytes -> PSRAM (phase 1/2, LVGL untouched)", (int)s_file_len);
}

/* ------------------------------------------------------------------ *
 * 阶段 2／2：buffer → LVGL 字体（纯 RAM 解析，必须在 lv_init() 之后）
 * ------------------------------------------------------------------ */
void ui_binfont_attach(void)
{
    if (s_attach_tried) {
        return;
    }
    s_attach_tried = true;

    if (!s_file_buf || s_file_len == 0) {
        ESP_LOGW(TAG, "attach skipped: bin not loaded (phase 1 failed)");
        return;
    }

    /* ★ 走 lv_binfont_create_ex + dynamic_glyph_load=true：
       只展开 glyph 描述符 + loca，位图按需现读，省下整块 glyf 展开（≈1.62MB）。
       注意 s_file_buf 因此必须常驻（LVGL 会长期持有 memfs 句柄），见文件头。 */
    const lv_binfont_dsc_t dsc = {
        .path               = NULL,      /* 用 buffer 源，需 LV_USE_FS_MEMFS */
        .buffer             = s_file_buf,
        .buffer_size        = (uint32_t)s_file_len,
        .dynamic_glyph_load = true,
    };
    s_font = lv_binfont_create_ex(&dsc);
    if (!s_font) {
        /* 常见原因：bin 与当前 LVGL 版本的字表格式不匹配，或字表生成参数被改过 */
        ESP_LOGE(TAG, "lv_binfont_create_ex failed (bad bin or version mismatch)");
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        s_file_len = 0;
        return;
    }

    ESP_LOGI(TAG, "CJK font ready: %d bytes in PSRAM, dynamic_glyph_load=ON, "
                  "line_h=%d base_line=%d | PSRAM free=%u",
             (int)s_file_len, s_font->line_height, s_font->base_line,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void ui_binfont_init(void)
{
    ui_binfont_load();
    ui_binfont_attach();
}

void ui_binfont_deinit(void)
{
    if (s_font) {
        lv_binfont_destroy(s_font);
        s_font = NULL;
    }
    if (s_file_buf) {
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
    }
    s_file_len = 0;
    s_load_tried = false;
    s_attach_tried = false;
}
