# 蓝牙 A2DP 音频链路移植技术可行性（display_audio_photo main → pet_companion）

> 调研日期：2026-10-31
> 参考实现：`display_audio_photo` main 分支，commit `c1a62ad`（"手机蓝牙连接播放"）
> 目标工程：`pet_companion`（同 BSP：esp32_s31_korvo_1，同 IDF v6.1）
> 结论先行：**可行，推荐分两期落地**。第一期 = 蓝牙底座 + A2DP Source 推流（需求主目标）；
> 第二期（可选）= A2DP Sink 接收 + PCM 级路由。

---

## 1. 需求澄清：main 分支里有两条蓝牙音频链路

`c1a62ad` 提交的标题是 Sink（手机连本机播），但**需求描述的"连接外部蓝牙设备、推流在外部设备播放"是另一条 Source 链路**。两条链路在 main 里都已实现并共用同一个底座：

| 链路 | 角色 | 数据流 | 文件 | 对应需求 |
|---|---|---|---|---|
| **Source 推流** | 本机是 A2DP SRC | `io_file(MP3) → aud_dec → aud_asrc → aud_alc → aud_enc(SBC) → io_bt` | `bt_push_service.c`（717 行） | ★ **本需求主目标**：本机音频推到蓝牙音箱/耳机播放 |
| Sink 接收 | 本机是 A2DP SNK | `io_bt → aud_dec(SBC/AAC) → aud_asrc → io_codec_dev(ES8389)` | `bt_sink_service.c`（893 行） | 手机连本机、用本机扬声器播（可选二期） |
| 公共底座 | 电源/角色/事件/扫描连接/内存闸门 | — | `bt_audio_service.c`（640 行） | 两者必依赖 |

底层统一走 Espressif 官方组件 **`espressif/esp_bt_audio` v1.0.0~2**（封装 bluedroid A2DP/AVRCP + GMF `io_bt` 元素），**不直接操作 bluedroid API**。

---

## 2. display_audio_photo 蓝牙实现要点（移植前必读的纪律）

### 2.1 分层与事件模型
```
esp_bt_audio (managed component)
   │ 事件回调（跑在 BT/GMF 线程）
   ▼
bt_audio_service：电源开关 / 角色位 / 扫描连接 / 事件分发 / 内存闸门
   ├─ dir==SINK  → bt_sink_service_on_stream_state()   bt2codec 管线
   └─ dir==SOURCE→ bt_push_service_on_stream_state()   local2bt 管线
   ▼
UI（500ms lv_timer 轮询 get_meta/get_state，BT 线程绝不碰 LVGL）
```

### 2.2 五条必须照搬的工程纪律（main 分支实测踩坑沉淀）
1. **Controller 早期常驻 init**：`esp_bt_controller_init + enable(CLASSIC_BT)` 必须在
   `bsp_display_start()` 之前（内部 RAM 最空的窗口）一次性拿 HCI 内存池，之后常驻不再碰。
   host（bluedroid）**不能**常驻——`deinit` 会真释放 BTU/BTC 任务与全部队列，
   每次 `set_power(true)` 都是一次有风险的重分配（`bt_audio_service.h` 长注释）。
2. **host 内存闸门**：`BT_HOST_MIN_FREE_INTERNAL=48KB / MIN_LARGEST_BLOCK=24KB`，
   不满足就拒绝开蓝牙（返回 `ESP_ERR_NO_MEM`），因为 bluedroid 内部分配失败是
   **直接 assert 崩**（`future_ready / btc_config_init`，实测过）不是返回错误码。
3. **管线「方案 B 延迟常驻」**（P1~P4）：early_init 建 pool/pipeline/task；
   GMF task 栈 `stack_in_ext=true` 进 PSRAM；首次 ALLOCATED 才真正建管线，
   STOPPED/RELEASED 只 stop+reset 不 destroy；销毁走异步 reaper，绝不在 BT 事件回调里做。
4. **会话仲裁**：Sink 播放前查 `audio_stream_is_active()`（16k 会话独占 codec 时不得抢回）；
   Sink 让位走 `bt_sink_service_audio_yield()`（停管线 + 关 codec，同步等 ≤1s），
   否则 `esp_codec_dev` 采样率冲突被静默忽略 → I2S 停在 44.1k → 16k 会话无声。
5. **A2DP 角色是全局单例**：`esp_bt_audio_init()` 一次性传入角色位，切角色 = 整个 host
   推倒重建（断开当前连接），**只能由页内按钮触发**，不得挂在 tab 切换回调里。

### 2.3 sdkconfig 关键项（照抄清单，见 §4.3）
经典蓝牙 only（BR/EDR，不开 BLE/NIMBLE/HFP，省内存）+ A2DP 外部编解码 + AVRCP +
`ESP_BT_AUDIO_GMF_IO_SUPPORT=y`（让 esp_bt_audio 带上 `esp_gmf_io_bt` 元素）+
`BT_ALLOCATION_FROM_SPIRAM_FIRST=y`（bluedroid 内存优先 PSRAM）。

---

## 3. pet_companion 现状盘点（逐项核对）

| 检查项 | 现状 | 判定 |
|---|---|---|
| BSP / 芯片 | 同为 `esp32_s31_korvo_1`（ES8389 codec，I2S） | ✅ 硬件零差异 |
| GMF 栈 | `gmf_core/gmf_audio/gmf_io` 全 1.0.x（esp_player 1.0.6 + av_processor 0.7.0 锁定） | ✅ **与 main 蓝牙管线同代**，`esp_gmf_pool/pipeline/asrc/alc/enc/dec` API 一致 |
| `esp_asrc` | managed_components 已有 | ✅ |
| `esp_audio_codec` | 已有（esp_bt_audio 要求 ^2.5，满足）；**含 SBC 编解码**（`esp_sbc_def.h` + encoder/decoder 树，且带 esp32s31 预编译库） | ✅ |
| `media_lib_sal` | 已有 1.0.0 | ✅（`media_lib_add_default_adapter` 可用） |
| `espressif/esp_bt_audio` | **未引入**（main 里锁 1.0.0~2） | ⚠ 需新增依赖，见 §4.1 |
| sdkconfig BT | **全关**（无任何 `CONFIG_BT*`） | ⚠ 需加整块，见 §4.3 |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | =0（与 main 同款，`malloc` 优先 PSRAM） | ✅ 已是蓝牙友好配置 |
| WiFi/BT 共存 | `CONFIG_ESP_COEX_ENABLED=y` 已默认开 | ✅ |
| 音频所有权 | **audio_service 是 codec 唯一所有者**，外部经 `ext_claim`（流式）/`ext_claim_exclusive`（独占）借用 | ⚠ 蓝牙必须接这套协议，见 §4.4 |
| 内存水位 | 开机早期 `INT free=127,371 / largest=55,296`；WiFi 持有 51,200 连续 DMA 刚性块 | ⚠ 开机窗口过得去闸门，运行中开蓝牙偏紧，见 §5 |
| 编译告警 | `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` **未开**；bluedroid 源码在 GCC 15 下有 strncpy truncation 告警 | ⚠ 需处理，见 §6-R4 |
| main/CMakeLists | 显式 SRCS 登记 | ⚠ 新文件必须登记 |

**结论：依赖代际完全对齐，缺的只是 esp_bt_audio 组件、BT sdkconfig 块、三个移植文件与集成胶水。**

---

## 4. 移植方案（第一期：底座 + Source 推流 + 播放路由）

### 4.1 文件清单（main → pet_companion）

| main 分支文件 | 动作 | 说明 |
|---|---|---|
| `middleware/bluetooth/bt_audio_service.c/.h` | **移植（裁剪）** | 保留：controller_init / set_power / 事件回调 / 扫描连接 / is_connected / 内存闸门。裁掉：`bt_audio_service_set_volume`（推流数字增益逻辑保留在 push 侧）、Source 播完收尾任务里的 `session_manager_*`（换成 pet 的互斥语义，§4.4）。UI 事件回调改投 pet 的 event_bus 或直接回调 |
| `middleware/bluetooth/bt_push_service.c/.h` | **原样移植** | Source 管线自包含，不依赖 display 特有服务（early_init 的 pool 独立建） |
| `middleware/bluetooth/bt_sink_service.c/.h` | **二期** | 需要 codec 时走 audio_service 协议改造（§4.4-b），不建议第一期带 |
| `bt_push_service` 依赖的 `session_manager`（main 的 audio/） | 不移植 | pet_companion 用 audio_service ext 协议做仲裁，见 §4.4 |
| `main/CMakeLists.txt` | 增登记 | SRCS 加两个 .c；`PRIV_REQUIRES` 加 `esp_bt_audio`（其自身会带 gmf_core/gmf_io/esp_audio_codec） |
| `main/app/main.c` | 增两行 | 启动序见 §4.2 |

### 4.2 启动序插入点（pet_companion `app_main`）

```c
ui_binfont_load();                       /* 已有（F40：PSRAM 任务起来之前） */
sdcard_service_mount();                  /* 已有 */
/* ★ 新增：蓝牙早期窗口（fs_service/audio_service 均未起，内部 RAM 最空） */
bt_audio_service_controller_init();      /* controller 常驻 */
bt_push_service_early_init();            /* GMF pool 管线延迟常驻 */
...                                      /* 后续照旧：bsp_display_start 等 */
```
- 位置必须在 **`bsp_display_start()` 之前**（纪律 1/3），且在 `ui_binfont_load()` 之后
  （binfont 读 flash 的 cache-error 窗口内不能有 PSRAM 栈任务在跑——F40 同款约束）。
- host 上电（`set_power(true)`）不在开机自动做：pet_companion 内存比 main 更紧张
  （esp-sr/相机/esp-dl 同场），**首次进"音频输出设置"或音乐页时再开**，靠闸门拒绝兜底。
  若实测开机余量充足，可再改开机自动开（"蓝牙已连接自动推流"的体验最顺）。

### 4.3 sdkconfig 增补（同时写 `sdkconfig` 与 `sdkconfig.defaults`，Kconfig 铁律）

```ini
## Bluetooth Classic (BR/EDR) — A2DP Source 推流 + （二期）Sink 接收
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN=1
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y
CONFIG_BT_AVRCP_ENABLED=y
# CONFIG_BT_HFP_ENABLE is not set
# CONFIG_BT_BLE_ENABLED is not set
# CONFIG_BT_NIMBLE_ENABLED is not set
CONFIG_ESP_BT_AUDIO_GMF_IO_SUPPORT=y
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y
```
（main 另有 `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y`，pet 不建议全局开，
处理方式见 §6-R4。）

### 4.4 音频路由设计（需求核心："本地 or 蓝牙，已连蓝牙自动推"）

**方案 A（第一期推荐）：文件级路由，改 `media_player_service` 一处入口**

```
media_player_service_play(path)
    │
    ├─ bt_audio_service_is_on() && is_connected() && 路由开关==AUTO/蓝牙
    │      → bt_audio_service_play_source(path)      # 走 local2bt 管线，SBC 编码后从蓝牙出
    │        控制映射：pause/resume/stop → bt_push 同名 API
    │        音量：bt_push_service_set_volume（aud_alc 数字增益，0..100）
    │        播完收尾：FINISHED 回调 → media_stop + 归还互斥 + MP_EVT_FINISHED
    │
    └─ 否则 → 现有本地链路（ext_claim → ext_write → 本地 ES8389）一字不动
```
- 优点：改动集中在 media_player_service 一个文件 + 一个 UI 路由开关；两条链路的
  控制/音量/播完事件都已由 main 验证过；互斥天然成立（同一时刻只走一条链路）。
- 限制：小智 TTS / 对话音频（PCM 流）**不上蓝牙**，仍走本地扬声器。
- Source 推流**不占用本地 codec**（输出是 io_bt），所以与 xiaozhi 的
  `ext_claim_exclusive`、录音的 mic 租借互不冲突；唯一要防的是"本地 MP3 正在播时又触发推流"——
  路由入口处检查 `media_player_service_is_playing()` 即可。

**方案 B（二期可选）：PCM 级路由**
把 `aud_alc → aud_enc(SBC) → io_bt` 做成 audio_service 的第三种 ext 模式（"BT 输出"），
media_player 与 xiaozhi TTS 的 PCM 都可写进来。统一控制面但改造量大
（io_bt 要等 stream ALLOCATED 才有句柄、pipeline 要从文件驱动改成 PCM 驱动），
且 xiaozhi 走 `ext_claim_exclusive` 独占模型需要重构。**建议只在真需求出现时做。**

**Sink（二期可选）**：`bt_sink_service` 移植时唯一硬改造点是 `io_codec_dev`：
- main 里直接 `bsp_audio_codec_speaker_init()` 自建句柄——在 pet_companion **禁止**
  （audio_service 唯一所有者，二次建句柄 = "同一 I2S 双句柄"P0）；
- 正确做法：`esp_codec_dev` 句柄经 `audio_service_ext_claim(44100/2/16)` 借用，
  PREPARE/RUN 挂在 claim 之后，STOP/RELEASE 时 `ext_release()`；
- 与 xiaozhi EXCLUSIVE 会话互斥：照搬 main 的 `audio_yield` 同步让位模式。

### 4.5 UI 落点
- 设置页（page_system）或音乐 tab 加**"音频输出"**三态开关：自动（默认）/ 仅本地 / 仅蓝牙；
  语义 = §4.4-A 的路由策略，状态可存 NVS。
- 蓝牙开关按钮触发 `set_power`（遵守纪律 5：角色/开关变化只在页内按钮做）。
- 第一期不需要扫描/配对 UI：Sink 侧连接方式 = 手机主动连本机（Just Works）；
  Source 侧 = 蓝牙音箱主动回连本机（多数音箱支持回连最后设备），或加最简扫描列表
  （`start_discovery`/`connect`，main 的 page_bluetooth 可裁剪复用）。

---

## 5. 内存影响评估

| 消耗项 | 区域 | 时机 | 量级 |
|---|---|---|---|
| Controller HCI 内存池 | 内部 RAM | 开机早期一次性 | 数十 KB（常驻） |
| bluedroid host（BTU/BTC 任务、队列） | 优先 PSRAM（`BT_ALLOCATION_FROM_SPIRAM_FIRST=y`），任务栈仍内部 | set_power(true) | 闸门要求 free≥48KB / largest≥24KB |
| GMF pool/pipeline/task | 缓冲 PSRAM；**GMF task 栈 5120B 显式 PSRAM**；proc_task 栈 PSRAM（WithCaps） | early_init 一次性 | 内部 RAM 占用很小（RTOS 对象级） |
| A2DP 编解码工作缓冲 | esp_audio_codec 内部（SBC） | 推流期 | 数 KB |

- pet_companion 开机窗口（`INT free=127KB / largest=55KB`）能过 48/24 闸门 ✅。
- **运行中开蓝牙是主要风险位**：WiFi 51,200 连续 DMA 块是刚性的，叠加 esp-sr AFE
  （xiaozhi 会话期吃内部 RAM）、esp-dl 模型，运行期 largest 可能掉到 24KB 以下 →
  闸门会拒绝（功能不可用但不崩，符合设计）。
- 缓解：a) 尽量在开机早期开 host；b) 复用工程现成的 `MEM_STAGE` 账单，在
  controller_init / early_init / set_power 三处插桩实测；c) 若运行期largest 常年不足，
  再做 `.bss` 瘦身（同 main 分支的路数）。

---

## 6. 风险清单

| # | 风险 | 缓解 |
|---|---|---|
| R1 | `espressif/esp_bt_audio` 下载：组件注册表中国镜像 403（esp-dl 3.3.11、av_processor 预置 zip 都踩过） | 照 esp-dl 先例：锁 `"1.0.0~2"` + 手动预置 zip 到 managed_components；参考源已在本地 `display_audio_photo/.workbuddy/ref/esp32-xiaofeng-main/components/esp_bt_audio` |
| R2 | host deinit 后重建失败（真·重新 init，可能 NO_MEM） | 闸门已有 + 失败回滚（main 的 switch_role 回滚逻辑一并移植）；UI 把 NO_MEM 提示给用户而非当成功 |
| R3 | WiFi 下载（云点歌）与 BT 推流同抢射频 | COEX 已开；音乐在 SD 本地时无碍；云源边下边推需实测（卡顿降级为"先下完再推"） |
| R4 | bluedroid 源码 GCC 15 `-Werror`（strncpy truncation 等） | 不全局关默认错误：对 bluedroid/esp_bt_audio 组件按 mcp-c-sdk 先例 `target_compile_options(... PRIVATE -Wno-error=stringop-truncation)` |
| R5 | `pvPortMalloc`/任务栈只从内部 RAM 拿（`ALWAYSINTERNAL` 管不住它们） | GMF task 栈/proc 栈显式 PSRAM 的写法已由 main 验证，原样保留 |
| R6 | AVRCP 音量在 SRC 方向不可用（协议层限制，main 已证伪） | 维持 main 结论：Source 音量 = aud_alc 数字增益，不做 AVRCP |
| R7 | 切分支/全清后 sdkconfig 漂移 | BT 块必须同时写进 `sdkconfig.defaults`；`idf.py fullclean` 后核对 |

---

## 7. 验收方法（插板真机）

1. **编译**：`idf.py fullclean && idf.py build`（esp_bt_audio 组件解析 + bluedroid 链接无 -Werror 炸）。
2. **开机账单**：`MEM_STAGE` 对比移植前后——新增 controller_init / early_init 两行 diff，
   确认内部 RAM 消耗与预期一致、开机不新增失败行。
3. **Source 推流闭环**：连蓝牙音箱 → 音乐 tab 播放 MP3 → 声音从音箱出（本机静音）；
   暂停/恢复/停止/音量键逐项验证；播完自动收尾（不再占用，断开音箱后本地播放正常回落）。
4. **自动路由语义**：蓝牙未连接 → 本地出声；连接中 → 自动切蓝牙；手动切"仅本地" → 蓝牙连着也从本机出。
5. **共存回归**：xiaozhi 对话（AFE 独占期）、本地录音、相机预览/录像各跑一遍，
   确认无内存告警、无 codec 抢占异常；推流期间开相机无崩。
6. **闸门路径**：满载状态下（相机+WiFi+对话）手动开蓝牙 → 期望得到 NO_MEM 提示而非重启。
7. **长时间稳定性**：连续推流 ≥30min，`largest` 无单调下滑（无泄漏）。

---

## 8. 工作量估计

| 项 | 估计 |
|---|---|
| esp_bt_audio 引入 + sdkconfig + CMake | 0.5 天（含组件下载折腾余量） |
| bt_audio_service / bt_push_service 移植裁剪 | 1 天 |
| media_player_service 路由 + 播完收尾 + 音量映射 | 1 天 |
| UI（输出开关 + 蓝牙开关 + 最简扫描页可选） | 1 天 |
| 真机验收 + 内存账单 + 修复 | 1~2 天 |
| **合计（第一期）** | **约 4.5~5.5 天** |
| 二期（Sink + PCM 级路由） | 另计 3~4 天 |

---

# 第二部分：本地 MP3 播放器 UI 移植（进度条 + LRC 歌词滚动 + 上下曲）

> 追加调研（2026-10-31）。需求：只做**本地播放**——本地 MP3 + LRC 歌词滚动，
> 移植 main 分支的播放器 UI，保留进度条、播放/暂停、上一首/下一首。

## 9. main 分支上两个候选 UI 的甄别

| 文件 | 内容 | 判定 |
|---|---|---|
| `ui/tabs/tab_audio.c`（538 行，AUDIO HUB） | LOCAL/BT 模式切换 + 曲目列表 + 播放/暂停/停止 | **不是目标**：没有进度条、没有歌词、没有上下曲 |
| `ui/tabs/tab_pcnow.c`（635 行，PC NOW 副屏播放器） | 圆形旋转封面 + 曲名/歌手 + **lv_slider 进度条（可拖 seek）** + 时间标签 + **三行歌词滚动窗（prev/cur/next，当前行高亮）** + 右侧竖排 上一首/播放/下一首 三键 | ★ **移植这份的 UI 版式与交互** |

注意：tab_pcnow 的**数据侧**不可移植——它的进度/歌词由 PC 上位机算好推过来
（`pcnow_service` 服务端内插进度、自算 `lrc_index/lrc_seq`，设备端只认 seq 变化换文本）。
本地播放必须把这些逻辑搬到设备端自己算（见 §10.2）。

## 10. 移植架构（pet_companion 侧）

### 10.1 数据面盘点

| 能力 | pet_companion 现状 | 需要补 |
|---|---|---|
| MP3 解码播放 | ✅ `media_player_service`（esp_player 1.0.6 + ext 协议，tab_files 已在用） | — |
| 播放/暂停/停止 | ⚠ 只有 play/stop | 服务层加 pause/resume（esp_player 本就支持，只是没暴露） |
| 进度查询 | ❌ 未暴露 | 加 `get_position_ms/get_duration_ms`（透传 `esp_player_get_duration` + 位置接口，组件头文件已确认存在） |
| 拖动 seek | ❌ 未暴露 | 加 `seek(ms)`（透传 `esp_player_seek`，1.0.6 支持，PLAYING/PAUSED 真跳转并回 SEEK_DONE 事件） |
| 上一首/下一首 | ❌ | UI 层维护曲目表（扫 `/sdcard/music/*.mp3`，照抄 main `scan_tracks`），idx±1 |
| LRC 解析/歌词推进 | ❌ 全无 | **新增 lrc 模块**（§10.2） |
| 播放器 UI | ❌（tab_files 是"浏览即播"，无播放界面） | 新增 `ui/tab_music.c`（§10.3） |

### 10.2 新增 `middleware/audio/lrc_service.c/.h`（约 200 行，全新写）

参考 main 的语义（tab_pcnow 只认 `prev/cur/next` 三行 + 递增 `seq`），设备端实现：

```c
typedef struct {
    char     prev[128], cur[128], next[128];  /* 三行窗口 */
    uint32_t seq;                              /* 每次推进 +1，UI 只在 seq 变化时刷新 */
} lrc_window_t;

esp_err_t lrc_service_load(const char *lrc_path);      /* 整文件读入 PSRAM 解析 */
void      lrc_service_tick(uint32_t pos_ms);           /* 按播放位置推进窗口 */
bool      lrc_service_get(lrc_window_t *out);          /* seq==0 表示从无歌词 */
void      lrc_service_unload(void);
```

- 解析 `[mm:ss.xx]`/`[mm:ss.s]` 双精度格式；时间轴存 PSRAM 数组（一首歌 <2KB）。
- `tick()` 由 UI 的 500ms 定时器喂 `media_player_service` 的当前位置，二分/游标找当前行；
  **提前量**（歌词略微提前切换）在 main 里是服务端做的，本地版给 `load()` 加一个
  `lead_ms` 参数（默认 ~200ms）即可。
- LRC 文件约定：**与 MP3 同目录同名 `.lrc` 优先**，其次 `/sdcard/music/lrc/<同名>.lrc`；
  没有歌词文件 → 三行窗显示歌名占位（不报错）。
- ⚠ **编码风险**：国内 `.lrc` GBK/UTF-8 混杂，设备端只认 UTF-8（bin 字体也是 UTF-8 索引）。
  简单判别：含 BOM 或合法 UTF-8 校验通过才加载，否则显示"(歌词编码不支持)"。
  不建议设备端做 GBK 转码。

### 10.3 `ui/tab_music.c` 移植要点（照抄 tab_pcnow 版式 + 魔改数据源）

**版式照抄**（tab_pcnow.c:423-621 的构建序列）：
- 可用区 = 页内容区实际尺寸（读 `lv_obj_get_style_pad_*`，F46 教训：写死尺寸会被页对象 pad 裁掉）；
- 纵向排布算法照搬：「**歌词贴底、封面吃余量**」——先按 `LYR_LINES × line_height` 定歌词带，
  剩余空间给圆形封面（上限 COVER_BOX、下限 120）；
- 三行歌词窗：label 高度 `SIZE_CONTENT`（bin 字体 ascent37/descent-15 ⇒ 行高 52，
  高度写死会裁字）；当前行 `COL_TEXT` 高亮，prev/next 用 `COL_SUBTEXT`；
- 进度条 `lv_slider` + `s_seeking` 标志（手指按住期间 poll 不覆盖滑条值，松手才 seek）；
- 右侧竖排三键（prev/play/next）；
- 封面旋转定时器（`lv_image_set_rotation`，LIVE+playing 才转）——本地版可去掉或保留。

**魔改数据源**（把 pcnow_service 的活儿接过来）：
- poll 定时器（500ms）→ 读 `media_player_service` 的 pos/duration → 滑条/时间标签 +
  `lrc_service_tick(pos_ms)` → seq 变化才刷三行文本；
- 播放键 → pause/resume（图标按 `LV_SYMBOL_PLAY/PAUSE` 翻转，**F47 教训：
  随 state 变的颜色设在按钮上、不设在子 label 上**）；
- prev/next → 曲目表 idx±1 → `media_player_service_play(下一曲路径)` +
  `lrc_service_load(同名 .lrc)`；
- 拖动松手 → `media_player_service_seek(ms)`；
- **与第一部分蓝牙路由联动**：play 入口走 §4.4-A 的路由。⚠ BT 推流模式下
  **没有进度/位置反馈**（main 的 A2DP Source 不回报播放位置，音量走 aud_alc 数字增益）——
  第一版建议：BT 模式下滑条置灰只显示推流状态，歌词停在第一行；prev/next 仍可用
  （切曲 = 重新 play_source）。

**落点与登记**：`ui/` 新增 `tab_music.c/h`；`main/CMakeLists.txt` SRCS 登记
`ui/tab_music.c`、`middleware/audio/lrc_service.c`（工程铁律：不登记不编译）。
入口按钮放 pet 的主界面（与 tab_files 并列，或 FILES 页加"播放器"入口）。

## 11. UI 移植的工程纪律清单（main 踩坑沉淀，逐条适用于本页）

1. **F46 页内边距**：绝对布局前读页对象真实 pad（main 实机 tabview pad=12px），
   否则顶栏/歌词第三行被静默裁剪。
2. **F47 颜色通道**：播放键按下翻转的 `text_color` 设在**按钮对象**上（可继承 + 随
   state 刷新），固定文字色才设 label。
3. **bin 字体行高**：文本行一律 `SIZE_CONTENT` 或按 `lv_font_get_line_height()` 算；
   bin 字表无 ♪ 等符号 ⇒ 图标用 `UI_FONT`（montserrat）独立 label。
4. **LVGL 线程纪律**：esp_player 回调（FINISHED/SEEK_DONE）在播放器任务上下文，
   只置 volatile 标志，由 lv_timer 抽取（main `s_player_finished` 范式照抄）。
5. **离开页面必停**：`tab_music` destroy 时 stop + lrc_unload（tab_files 已有同款
   收尾语义，别双源同播——进播放器页前若 tab_files 在播，先停它）。
6. **内存**：LRC 全文 + 时间轴进 PSRAM；UI 对象树 ~几十 KB PSRAM，无内部 RAM 大额新增。

## 12. 工作量追加（并入第一部分验收）

| 项 | 估计 |
|---|---|
| media_player_service 扩展（pause/resume/position/seek） | 0.5 天 |
| lrc_service（解析 + tick + 编码守卫） | 1 天 |
| tab_music UI（版式移植 + 数据源接线） | 1.5~2 天 |
| 联调（含与蓝牙路由、tab_files 互斥） | 0.5~1 天 |
| **合计追加** | **约 3.5~4.5 天**（一期总体 8~10 天） |

**验收补充**：
1. `/sdcard/music/` 放 3 首 MP3 + 2 份 UTF-8 LRC + 1 份 GBK LRC（后者应优雅降级）；
2. 进度条随播放推进、拖动 seek 后歌词窗跳到对应行；
3. 上一首/下一首循环切换，播完自动切下一曲（esp_player FINISHED → next）；
4. 蓝牙已连接时播放走推流、滑条置灰；断开后回落本地、滑条恢复；
5. bin 字体中文歌词无裁字（第三行完整可见）。

---

## 附：关键源码索引（display_audio_photo main）

- `main/middleware/bluetooth/bt_audio_service.c:91` 内存闸门阈值
- `main/middleware/bluetooth/bt_audio_service.c:146` classic_roles（A2DP 单例与角色位）
- `main/middleware/bluetooth/bt_audio_service.c:290` controller 早期常驻 init
- `main/middleware/bluetooth/bt_push_service.c:373` Source 管线 early_init（P1~P4 范式）
- `main/middleware/bluetooth/bt_sink_service.c:459` Sink 管线 early_init
- `main/middleware/bluetooth/bt_sink_service.c:442` audio_yield（16k 会话让位）
- `main/ui/tabs/tab_pcnow.c:423` 播放器 UI 构建序列（封面/滑条/三行歌词/三键，移植蓝本）
- `main/ui/tabs/tab_pcnow.c:119` `s_seeking` 拖动防覆盖标志
- `main/ui/tabs/tab_audio.c:204` SD 曲库扫描 scan_tracks（上下曲数据源照抄）
- `main/app/main.c:213-221` 启动序插入点范例
- `sdkconfig.defaults:97-119` BT 配置块
- pet_companion 侧对接点：`main/middleware/audio/media_player_service.c`（play 入口）、
  `main/middleware/audio/audio_service.h:78`（ext 协议注释，蓝牙必须遵守的所有权模型）
