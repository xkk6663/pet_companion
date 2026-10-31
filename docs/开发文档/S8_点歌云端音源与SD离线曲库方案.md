# S8 端侧执行方案：点歌接云端音源 + SD 离线曲库

> 分支：`端侧人脸识别`。本文只覆盖**点歌/曲库**这条线（`ai_music.*` + 新增两个模块），
> 不动 `ai_camera / ai_light / ai_speaker / ai_punch / ai_device`，不动音频仲裁与会话状态机。
>
> 云端文档见 `display_audio_photo/tools/music_proxy/NETEASE_JS_API_直连对接.md`（JS API 部署 + 接口字段）。

---

## 0. 一句话目标

把 `self.music.play` 从「靠第三方解析站瞎猜一个能播的 http 串」升级成：

1. **点歌走自己的云端音源**（自建网易云 JS API），拿结构化元数据（歌名/歌手/时长/歌词/封面）；
2. **能下载到 `/sdcard/music/`**，落盘后**会话内直接播本地文件**（`file:///`），断网也能听；
3. **播过的歌不重复下载**，说「播放晴天」时优先命中本地。

---

## 1. 现状（带证据）

| 环节 | 现状 | 位置 |
|---|---|---|
| 工具 | `self.music.play(name)` / `self.music.stop` | `ai_music.c:438` / `:453` |
| 搜索 | qijieya → codelife → GD Studio，逐个 GET，**递归扫 JSON 找第一个像音频的 http 串** | `ai_music.c:84-95`、`:155-193`、`:306` |
| 响应缓冲 | `MUSIC_SEARCH_BODY_MAX = 2048`（2 KB） | `ai_music.c:61` |
| 播放 | `xiaozhi_service_music_play(url)` → `audio_playback_play(s_play, url)`，会话内并行、不抢会话 | `xiaozhi_service.c:957` |
| URL 上限 | `MUSIC_URL_MAX 512` / `XZ_MUSIC_URL_MAX 512` | `ai_music.c:58`、`xiaozhi_service.c:117` |
| 搜到即播，播完即失 | 无元数据、无落盘、无列表、无缓存 | — |

---

## 2. 已确认的事实（2026-10-21 实测，别再重查）

| # | 事实 | 出处 | 对本方案的影响 |
|---|---|---|---|
| F1 | 公网免费的网易云 API 镜像**全灭**（502 / 403 / DNS / TLS） | 本机实测，见云端文档 §4 | 必须用自建 API，无退路 |
| F2 | qijieya 的 `type=search` 现在返回 `[]`（HTTP 200，0.27s） | 本机实测 | 端侧 `k_music_apis[]` 第一位该删，否则每次点歌白等 |
| F3 | 单曲体积：128k ≈ **4.4~5.0 MB**，320k ≈ 11.2 MB | 实测 `size` 字段：4464684 / 5214920 / 11161644 | SD 空间预算、下载时长、空间预检的判据 |
| F4 | netease 的 `/search` 响应 5 首预计 **8~15 KB**（每首几十个字段） | 估算，**M0-3 要实测确认** | `MUSIC_SEARCH_BODY_MAX` 2KB 装不下，必须抬到 16KB |
| F5 | `esp_audio_simple_player` 支持 `file://` scheme；`esp_player` 文档明写 `file:///sdcard/music/test.mp3` | 官方组件文档 | 「下载后会话内播本地」技术上成立，**但 M0-1 要在 av_processor 上验一遍**（它是否把 URL 写死成 http） |
| F6 | `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_CODEPAGE_437=y` | `sdkconfig` | LFN 支持长文件名；中文名走 VFS 的 UTF-8→UTF-16 转换，理论可行但 **M0-2 要验** |
| F7 | 内部 RAM 上电 `largest_free_block = 55,296`，WiFi 内部 DMA 51,200 搬不走 ⇒ **余量 ~4 KB** | `开发文档/内存管理规划.md` | 下载缓冲只能 4 KB 内部；其余全 PSRAM |

---

## 3. 目标契约

### 3.1 工具表（MCP）

| 工具 | 参数 | 行为 | 备注 |
|---|---|---|---|
| `self.music.play` | `name` | ① 本地有 → 直接播 `file:///`；② 没有 → 云端搜 → 流播 | 保持单参数（MCP 引擎「缺一即拒」） |
| `self.music.download` | `name` | 云端搜 → 落盘 mp3 + lrc → 回话「已存到 SD」 | 单参数 |
| `self.music.list` | 无 | 读 SD 曲库，回「已下载 12 首：…」 | 无参数 |
| `self.music.stop` | 无 | 不变 | — |

★ MCP 引擎对注册过的属性做「缺一即拒」且不回填默认值（`esp_mcp_engine.c: esp_mcp_tool_args_cb`），
所以**每个工具只能有一个参数**，否则 LLM 少填一个就被拒单。想传 `br` 之类，走 NVS 配置，别开第二个参数。

### 3.2 时序

**播放（本地优先）**
```
LLM: play("晴天")
  → 后台任务（PSRAM 栈 12KB）
  → music_lib_find("晴天") 命中?  ── 是 ──> 组 "file:///sdcard/music/xxx.mp3"
  │                                        → xiaozhi_service_music_play(url)
  └─ 否 → music_source_search("晴天", 5) → 取第 1 条
           → music_source_resolve(id, 128) → 直链
           → xiaozhi_service_music_play(直链)
```

**下载**
```
LLM: download("晴天")
  → 后台任务
  → search → resolve(拿 size) → 空间预检 → 已存在则跳过
  → HTTP 流式 GET → /sdcard/music/<file>.part
  → 字节数 == Content-Length ? rename .mp3 : 删 .part
  → 写同名 .lrc（可选，失败不阻塞）
  → report("Saved ... (4.8 MB)")
```

---

## 4. 文件与改动清单

### 新增

| 文件 | 职责 | 估算行数 |
|---|---|---|
| `main/middleware/ai/music_source.h/.c` | 云侧客户端：`search / resolve / lyric`，NVS 存 base+token，http(s) 分流 | ~380 |
| `main/middleware/ai/music_lib.h/.c` | SD 曲库：命名、存在性查询、落盘（.part→rename）、列表、空间预检、lrc | ~300 |

### 改动

| 文件 | 改什么 |
|---|---|
| `main/middleware/ai/ai_music.c` | `cb_play` 改为「本地优先」；新增 `cb_download`、`cb_list`；删掉旧的 `k_music_apis` 递归扫描（或留作 `MUSIC_SOURCE=legacy` 编译开关） |
| `main/middleware/ai/ai_music.h` | 注释更新；如需暴露 `ai_music_download_async()` 给 UI |
| `main/CMakeLists.txt` | **SRCS 显式登记两个新 .c**（不登记就不编译，这工程的老规矩） |
| `main/middleware/ai/xiaozhi_service.c` | 只需允许 `file://` 前缀通过（`xiaozhi_service_music_play` 目前只校验非空，预计零改动，M1 验证） |
| `docs/项目规划/改动记录.md` | 补 §73.28 + 验收方法 |
| `docs/工程规范/11_遇到的问题.md` | 踩坑再写 |

### 配置（不进固件的部分）

NVS namespace `music_cfg`，两个字符串键：`base`（如 `https://music.example.com`）、`token`。
首次开机为空时，走 Kconfig 默认值（`Kconfig.projbuild` 加两项，或在 `sdkconfig.defaults` 写死测试值）。
UI 侧不开放编辑（一期），改配置用 `idf.py menuconfig` 或 NVS 工具。

---

## 5. 分阶段执行

> 每个 Mx 都要能独立验收、独立回退。**M0 不过，后面全部不做。**

### M0：前提验证（半天，只验证不写产品代码）

| 编号 | 验证内容 | 做法 | Pass 判据 | 不过的 Plan B |
|---|---|---|---|---|
| M0-1 | `audio_playback_play` 能否播本地文件 | 把任意 mp3 拷到 `/sdcard/music/t.mp3`，临时在 `ai_music` 里把 URL 换成 `file:///sdcard/music/t.mp3`（或裸路径 `/sdcard/music/t.mp3`，两种都试） | 会话内出声，日志无 `simple_player` 错误 | 退 `media_player_service`（会话外本地播放）：能听但不能在对话中播；或把 mp3 解成 PCM 喂 `feeder`（成本高，不推荐） |
| M0-2 | 中文长文件名 | `fopen("/sdcard/music/起风了-冯沁苑.mp3","w")` 写 1KB，再 `readdir` 读回 | 文件名一字不差 | 文件名改 ASCII：`ncm-<id>.mp3` + 同名 `.json` 存元数据（方案里预留了这个字段，改 10 行） |
| M0-3 | 云端连通 + 真实响应体大小 | 用 `esp_http_client` 拉 `/search?keywords=起风了&limit=5`，打印 `acc.len` 与首个 `id` | HTTP 200 且能取到 `result.songs[0].id`；记下实际字节数 | 若 >16KB ⇒ body buffer 抬到 24KB；若 TLS 失败 ⇒ 检查 `crt_bundle_attach` 与证书是否公网 CA |
| M0-4 | 空间查询 API | `esp_vfs_fat_info(SDCARD_MNT_PATH,&total,&free)` | 拿到数字 | 退 `f_getfree`（需 `FATFS_USE_LABEL`/卷句柄，麻烦） |

**M0 只加临时打印，不写进产品代码。**

### M1：音源客户端 + 走云（1~1.5 天）

- 写 `music_source.c`：`search / resolve / lyric`；
- `MUSIC_SEARCH_BODY_MAX` 2048 → **16 KB（PSRAM 分配，不占 .bss）**；
- `cb_play` 改为：`search` → `resolve` → `xiaozhi_service_music_play`；
- 保留旧链路作 `#if CONFIG_MUSIC_SOURCE_LEGACY` 兜底（默认关）。

**验收**：PTT 说「放一首起风了」→ 出声；日志能看到 `search -> 1 条 (netease)`、`resolve -> 4.97MB`。
**回退**：`git revert` 单文件 + 打开 legacy 开关。

### M2：落盘（1~1.5 天）

- `music_lib.c`：`build_filename()`（去非法字符 `/ \ : * ? " < > |`，截断到 ~90 字节，UTF-8 边界不切半个汉字）、
  `exists()`、`save_stream()`、`free_bytes()`、`write_lrc()`；
- 新工具 `self.music.download(name)`；
- 下载任务：PSRAM 栈 8 KB + **4 KB 内部 DMA 缓冲**；`esp_http_client` 走 `/v1/download` 或直链；
- 空间预检：`free < size + 1MB` 直接拒绝并回话。

**验收**：说「下载起风了」→ `/sdcard/music/` 出现 `xxx.mp3` + `xxx.lrc`，**没有 `.part` 残留**；
FILES 页能播它；大小与 `Content-Length` 一致。
**回退**：删 `.part` 逻辑不影响 play；也可只关 download 工具注册。

### M3：本地优先 + 列表（1 天）

- `cb_play` 开头先 `music_lib_find(name)`（按文件名/歌手模糊匹配，大小写与空格不敏感）；
- 命中就组 `file:///sdcard/music/<f>` 交给 `xiaozhi_service_music_play`；
- 新工具 `self.music.list`：扫目录 → 按 mtime 倒序 → 回「已下载 12 首，最近：起风了、晴天…」。

**验收**：**拔掉 Wi-Fi**（或关路由器）→ 说「播放起风了」→ 仍然出声。**这一条才算真验收**，在线流播做不到。
**回退**：本地优先是纯加法，关掉 `music_lib_find` 即回到 M1 行为。

### M4：收口（0.5 天）

- 曲库上限（默认 30 首 / 200 MB），超了按 mtime 淘汰最旧；
- `music_source` 的 HTTP 失败分类打点（DNS / TLS / 401 / 429 / 404 / 502），便于排障；
- 更新 `改动记录.md` §73.28、内存文档的「常驻增量」一栏。

---

## 6. 关键代码骨架

### 6.1 `music_source.h`

```c
/* 云侧音源客户端。所有函数阻塞，只允许在专用任务里调用（PSRAM 栈）。 */
typedef struct {
    char id[96];          /* 不透明，原样回传给 resolve/lyric */
    char name[64];
    char artist[64];
    uint32_t duration_ms;
    char cover[192];
} music_cand_t;

esp_err_t music_source_init(void);                     /* 读 NVS，不联网 */
esp_err_t music_source_set_cfg(const char *base, const char *token);  /* 写 NVS */

/* 返回实际条数；0 = 搜不到；负 = 错误 */
int music_source_search(const char *q, int limit, music_cand_t *out, int cap);

/* 成功填 url（http/https）与 size（可能为 0）。url==NULL 表示该曲无版权 */
esp_err_t music_source_resolve(const char *id, int br, char *url, size_t cap, size_t *size);
esp_err_t music_source_lyric(const char *id, char *lrc, size_t cap);
```

实现要点（每条都对应一个已知坑）：

1. **body 缓冲 16 KB，`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`**，用完即 free；
2. `/search` 走 **GET**（`?keywords=&limit=`），少写 POST 分支；带 `X-Music-Token`；
3. 取字段只认 `result.songs[].id / name / artists[0].name / duration`（**不要**再用递归扫 http 串）；
4. `/song/url/v1` 的 `data[0].url` **可能为 null**（VIP/无版权）→ 返回 `ESP_ERR_NOT_FOUND`，**绝不能把 `album.picUrl` 当音频**；
5. 直链是 `http://` ⇒ `resolve` 这一次请求**不要**挂 `crt_bundle_attach`（http 用不到），避免混淆；
6. `base` 是 https 时，搜索请求必须挂 `crt_bundle_attach`（否则 §73.27 那个 `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` 会复现）；
7. 超时 8s，失败**换下一首候选**（最多试 3 条），不要同一首反复重试——CDN 拿不到就是拿不到。

### 6.2 `music_lib.h`

```c
#define MUSIC_LIB_DIR   "/sdcard/music"

typedef struct {
    char path[128];       /* /sdcard/music/xxx.mp3（FM_PATH_MAX=128，注意别超） */
    char title[64];
    char artist[64];
    size_t bytes;
} music_item_t;

bool     music_lib_ready(void);                                  /* SD 已挂载 + 目录存在 */
int      music_lib_list(music_item_t *out, int cap, int skip, int take);
bool     music_lib_find(const char *keyword, char *path, size_t cap);   /* 模糊命中 */
esp_err_t music_lib_save(const char *id, const music_cand_t *c, int br,
                         char *out_path, size_t cap, size_t *out_bytes); /* 含 .part→rename */
bool     music_lib_has_space(size_t need_bytes);
esp_err_t music_lib_write_lrc(const char *mp3_path, const char *lrc);
```

落盘铁律：

```
路径长度："/sdcard/music/" = 14 + 文件名 + NUL  ≤ FM_PATH_MAX(128)  ⇒ 文件名 ≤ 110 字节
命名清洗：替换 / \ : * ? " < > | 和控制字符 → '_'；再按 UTF-8 边界截断
原子性：  先写 "<name>.part"；字节数对上 Content-Length 才 rename(2) 成 .mp3
残留下场：失败/超时 → 删 .part（否则 FILES 页里会出现一个"能看见但播不了"的鬼文件）
```

### 6.3 `ai_music.c` 的三个回调

```c
/* play：本地优先，一次搜索最多试 3 条候选 */
static void music_kick_task(void *arg)   /* 已有，扩成两段：local → remote */
{
    if (music_lib_find(k->name, path, sizeof(path))) {
        char uri[160];
        snprintf(uri, sizeof(uri), "file://%s", path);      /* M0-1 决定用 file:// 还是裸路径 */
        xiaozhi_service_music_play(uri);
        report("Playing from SD card.");
        return;
    }
    ... search → resolve → xiaozhi_service_music_play(直链) ...
}

static esp_mcp_value_t cb_download(const esp_mcp_property_list_t *props) { ... }
static esp_mcp_value_t cb_list(const esp_mcp_property_list_t *props)     { ... }
```

---

## 7. 红线（违反必炸）

| 类别 | 规则 | 后果 |
|---|---|---|
| 内部 RAM | 下载缓冲 ≤ 4 KB 内部（`MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`）；search body 16 KB 一律 PSRAM | 余量只有 ~4 KB，多要一点就撞 WiFi 初始化 |
| 任务栈 | 纯网络任务（search/resolve）可 PSRAM 栈；**做 FATFS/littlefs/flash IO 的任务栈必须内部 RAM**（F35） | 实机 panic（IDF 在写 flash 前断言 SP 在内部 DRAM） |
| 回调 | `cb_*` 跑在 MCP 引擎任务栈上，**只允许建任务/投队列，绝不阻塞** | 搜索 8s 会拖死 MCP 分发，会话卡住 |
| LVGL | 任何回调不碰 LVGL；UI 只读 volatile 快照 | 随机死机 |
| MCP 参数 | 每个工具只能一个参数 | LLM 少填一个 → 整次 tools/call 被拒 |
| 存储 | 落盘前必须 `music_lib_has_space()`；不写 `.part` 残留 | ENOSPC 中途失败留下半个文件，播放器解码报错 |

---

## 8. 风险与回退

| 风险 | 概率 | 影响 | 回退 |
|---|---|---|---|
| M0-1 失败（av_processor 不认 file://） | 中 | 会话内播不了本地文件 | 退 M2 只下载不播；播放走 FILES 页 `media_player_service`（会话外） |
| M0-2 中文名乱码 | 低 | 文件名不可读 | 换 `ncm-<id>.mp3` + `.json` sidecar |
| 云端 API 又挂 | 中 | 点歌全灭 | 留着旧 `k_music_apis` 作 legacy 开关；或加 music_proxy 兜底 |
| 下载半途断网 | 高 | 残留 .part | 已设计：校验 + 删 |
| 内存不够导致 WiFi 重建失败 | 中 | 设备掉线 | M2 前先做「栈静态化/缩 .bss」，把余量从 4 KB 抬到 8 KB |
| VIP 曲拿不到链 | 高 | 特定歌永远点不到 | 已设计：`url==null` → 换下一候选 → 明确回话「这首歌没有可播版本」 |

---

## 9. 验收清单（真机，按顺序）

1. [ ] 开机，`/sdcard/music/` 目录自动创建（日志一行）
2. [ ] PTT「放一首起风了」→ 出声，日志 `search -> 1 条 (netease)` + `resolve -> 4.97MB`
3. [ ] PTT「下载起风了」→ 回话报大小；`/sdcard/music/` 里有 mp3 + lrc，**无 .part**
4. [ ] 再问「你都下载了什么」→ `self.music.list` 能报出歌名
5. [ ] PTT「播放起风了」→ 日志走 `file://`，**不再是 https**
6. [ ] **拔 Wi-Fi** → 重复第 5 步 → 仍然出声（真验收）
7. [ ] 说「下一首/换个歌」→ 立刻切换，不掉会话
8. [ ] 说「别放了」→ 停
9. [ ] 连续下载 3 首，看 `esp_get_free_heap_size()` 与最大块无持续下跌
10. [ ] 插拔 SD（不重启不支持热插拔，重启后）曲库仍在

---

## 10. 顺序与工作量

| 阶段 | 内容 | 估时 | 依赖 |
|---|---|---|---|
| M0 | 4 个前提验证 | 半天 | 云服务已部署 |
| M1 | 音源客户端 + play 走云 | 1~1.5 天 | M0-3 过 |
| M2 | 落盘 | 1~1.5 天 | M0-2 / M0-4 过 |
| M3 | 本地优先 + list | 1 天 | M2 过 |
| M4 | 收口 | 0.5 天 | — |

**合计约 4~5 天**（不含编译排错与内存手术）。

---

## 11. 本文不做什么

- 不做点歌排行榜/推荐/歌单管理（`self.music.list` 只读本地目录）；
- 不做封面显示到 UI（`cover` 字段先存不用，等 UI 排期）；
- 不碰 `media_player_service`（FILES 页那套独立通路保持原样）；
- 不改小智会话状态机、不改音频仲裁；
- 不做断点续传（`Range` 服务端支持了，但端侧一期不做，失败就整个重下）。
