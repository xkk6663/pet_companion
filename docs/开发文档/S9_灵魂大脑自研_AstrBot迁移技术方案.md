# S9 灵魂大脑自研 —— 接入云端 AstrBot 大肥鱼 技术移植方案

> ⚠️ **本文件已被替代（2026-10-28）**：架构选型已定稿为 WebSocket 长连接方案，
> 实施请以 `S9_灵魂大脑自研_最终架构选型_桌宠伴侣版.md` + `S9_桌宠伴侣_可执行实施计划.md` 为准。
> 本文件保留仅作历史留痕（HTTPS+SSE 路线的调研结论与百炼额度分析仍可参考）。

> 状态：~~调研完成，待立项~~ **已归档**（原状态 2026-10-24）
> 基线：`端侧人脸识别` 分支 `03f0a57`（ESP-IDF v6.1 / esp_xiaozhi vendor 0.1.1 / av_processor 0.7.0）
> 方法：源码逐行核对本仓库 + `C:\Users\xiao1\Mao-dsh-pet\docs\SKILL-跨设备接入云端AstrBot.md`（下称「接入方案」）。
> 所有结论附文件路径 + 行号；标注 **【需核对】** 的条目不来自本仓库源码，落地前必须在真机/真服务器验证。

---

## 0. 一句话结论

**技术上可行，但"完全自研大脑"是伪需求 —— 真正要自研的只是传输层，MCP 和音频链路一行都不用改重写。**

| 你的设想 | 结论 | 关键依据 |
|---|---|---|
| 灵魂大脑接云端 AstrBot | ✅ 可行，最小改动 | 接入方案 §3.1（零代码接入路径成立） |
| 不阉割原有 MCP 功能 | ✅ **零风险，架构已天然解耦** | `esp_xiaozhi_chat.h:186` + `esp_xiaozhi_mgr.h:220` |
| 图片/视频存 SD 卡 | ✅ 现状已满足 | `ai_camera.c` / `record_video_service` |
| PCM 上云 + 服务器转文字 | ⚠️ **500MB 跑 faster-whisper 现实，但要选对模型档位** | 见 §6 |
| 云端插件功能可用 | ⚠️ 必须补下行图片通道 | 见 §7 |

**最重要的判断修正**：上一轮我建议"云端跑不动，走 ASR 云 API"。在你给出百炼额度后，这个结论要**两分**——

- **语音识别**：**根本不该在你 500MB 服务器上跑**。百炼 `qwen3-asr-flash` 免费 36K 秒（≈10 小时）+ 有效期到 2027-04，服务器只做中转，内存开销 ~10MB。这是明确的最优解。
- **但 faster-whisper 也并非不现实**：只要用 `small` + `int8`，在 500MB 内是**可以跑起来**的（见 §6.3 的实测方案与风险边界）。它适合做**降级兜底**（断网/额度耗尽时），不适合做主路径。

---

## 1. 现状盘点：现在的大脑在哪、由谁构成

当前"小智"并不是一个完整的大脑，它是一套**协议客户端 + 工具注册表**。真正的推理（LLM/人格/记忆）本来就在云端小智服务器。所以"自研灵魂大脑"的实质是：**替换云端，而不是在设备上训一个模型**。

| 层 | 现有实现 | 行数 | 迁移命运 |
|---|---|---|---|
| 传输 | `esp_xiaozhi_websocket.c` / `_mqtt.c`，hello 握手 | 677 | 🔴 **全部重写** → HTTPS + SSE |
| 协议编解码 | `esp_xiaozhi_chat.c` 帧分发 | 1714 | 🔴 重写分发，**保留事件模型** |
| MCP 工具 | `ai_{camera,device,light,music,punch,speaker}.c`（9 工具） | ~2100 | 🟢 **零改动复用** |
| MCP 引擎 | `espressif__mcp-c-sdk`（`esp_mcp_*`） | — | 🟢 **零改动复用** |
| 音频上行 | `av_processor` recorder（AFE + Opus） | — | 🟢 链路保留，改出口 |
| 音频下行 | `av_processor` feeder（Opus 解码） | — | 🟡 需接 TTS（当前无 TTS 客户端） |
| 会话状态机 | `xiaozhi_service.c` | 1116 | 🟡 **约 700 行是协议专属逻辑，需重写** |
| 设备 UI | `overlay_ai.c` + `xiaozhi_service` 事件 | 759 | 🟢 事件模型可保留 |

### 1.1 关键洞察：`xiaozhi_service.c` 那 700 行是"小智服务器 bug 的疤痕"

`xiaozhi_service.c` 里有大量逻辑，**它们处理的是小智服务器特有的坏行为，换到 AstrBot 后大部分直接消失**：

| 现有机制 | 行号 | 为什么存在 | 换 AstrBot 后 |
|---|---|---|---|
| `XZ_ECHO_GUARD_MS` 500ms 回声静默窗 | `:141` | feeder 无 flush API，尾音被转成垃圾 | 🟡 需保留（本地播放器问题，非服务端） |
| §53 聋化自愈（8s 无回音 → 重开会话） | `:150-154` | 小智服务器第 4 轮起静默失效 | 🟢 **可删**（HTTP 无状态，无此病） |
| barge-in abort_speaking 两段式 | `:134-138` | 小智回合状态机会被打死 | 🟢 **可删**（无长连接回合） |
| `s_ptt_start_pending` 欠账补发 | `:138` | 服务器 side 回合簿记 | 🟢 **可删** |
| 音乐 barge-in `barging` 闸门 | `:174` | 1s 停流延迟吃语音 | 🟡 保留（本地 gmf 管线问题） |
| 上行闸门 `s_chatting` | `:127` | 会话态判定 | 🟡 简化为请求态 |

**这一条决定了总工作量**：不是"重写 1116 行"，而是"新写约 600 行 + 删掉约 400 行疤痕"。

---

## 2. 目标架构

```
┌─ ESP32-S31 设备（身体）─────────────────────────────────────┐
│                                                              │
│  [mic] ─→ av_processor recorder(AFE) ─→ Opus ─→ 上行任务      │
│                                                              │
│  ┌─ astrbot_client.c（新增，约 600 行）──────────────┐        │
│  │  POST /api/v1/chat  ← SSE 流式解析                │        │
│  │  帧: plain / record / file / image / agent_stats  │        │
│  │  附件: GET /api/v1/files/content                  │        │
│  └───────────────────────────────────────────────────┘        │
│                     ↕ 事件模型（保留 esp_xiaozhi 事件枚举）    │
│  [session SM] ─→ overlay_ai（气泡/浮窗）                      │
│               ─→ feeder(Opus) ─→ [spk]                       │
│                                                              │
│  ┌─ MCP 层（零改动）───────────────────────────────┐          │
│  │  esp_mcp_t 引擎 + 9 工具                          │          │
│  │  ↑ esp_mcp_transport_http_server (:80 复用)       │          │
│  │  ↑ esp_mcp_mgr_req_handle() 本地分发              │          │
│  └───────────────────────────────────────────────────┘        │
│  /sdcard ← 拍照/录像/云端下发的图（唯一大容量载体）             │
└──────────────────────────────────────────────────────────────┘
                          ↕ HTTPS
┌─ 云端服务器（500MB）────────────────────────────────────────┐
│  AstrBot (persona 333 大肥鱼 + livingmemory + 17 插件)        │
│  POST /api/v1/chat ──────────────────────────────────────┐   │
│                                                          ↓   │
│  [可选] asr_proxy.py  ~10MB  Qwen→文字 ──────────────→ AstrBot│
│           ↓ 兜底（断网/额度尽）                              │
│  [可选] faster-whisper small-int8  ~300MB ← 有风险，降级用    │
└──────────────────────────────────────────────────────────────┘
```

**核心设计原则（沿用接入方案 §14 的 P1–P7）**：
- **P1 设备端零模型** —— 本方案强化：设备端连 ASR 模型都不跑（esp-sr 的 MultiNet 仍未启用，S7-B2 保持搁置）；
- **P2 命令不走前缀** —— 见 §7.3；
- **P3 会话独立** —— `final_session_id` = `xz-s31-<mac末4位>`，**不**复用微信那条（接入方案 §3.4）；
- **P5 文字先于语音** —— 见 §7.4；
- **P7 失败要可见** —— 见 §7.2。

---

## 3. 传输层迁移（唯一真正的重写工作）

### 3.1 为什么能复用 webchat（接入方案 §2 第 8 条）

`/api/v1/chat` **不是旁路**：`chat_service.py:1356-1375` 显示它是 webchat 适配器的 HTTP 外壳，灌队列后走 `convert_message()`，与小智、微信**同一条 pipeline**。所以人格、记忆、工具、插件全部共享。

### 3.2 请求契约

```http
POST /api/v1/chat
Authorization: Bearer abk_xxx          # chat scope
Content-Type: application/json

{
  "username":   "xiao1",
  "session_id": "xz-s31-a1b2",        # 设备独立，勿复用微信
  "message":    [{"type":"plain","text":"今天人脸打卡了几次？"}],
  "enable_streaming": true             # ★ 必须是真 bool，字符串会被忽略！
}
```

**UMO** 恒为 `webchat:FriendMessage:webchat!xiao1!xz-s31-a1b2`。

### 3.3 三个必须写进代码的硬约束

| # | 约束 | 违反后果 | 依据 |
|---|---|---|---|
| 1 | `enable_streaming` 必须是 **JSON true** | 传 `"false"` 字符串被忽略回落 `True`；传 `false` 可能**一个正文帧都没有** | 接入方案 §2 第 7 条、§5.6 |
| 2 | 正文取 `plain` 增量拼接，`complete` 兜底 | 只有 `complete` 帧的轮次会失明 | 接入方案 §5.6 附注 |
| 3 | 服务端**不发 `complete` 帧**，以 `message_saved` + 连接关闭收尾 | 等 `complete` 会永久挂起 | `chat_service.py:1194-1222` |

### 3.4 设备端要实现的东西

```c
/* main/middleware/ai/astrbot_client.h —— 接口草案 */
esp_err_t ab_client_init(const ab_config_t *cfg);       /* base_url / api_key / session_id */

/* 文本回合：阻塞到完整回复（内部即 SSE 消费） */
esp_err_t ab_chat_text(const char *text,
                       ab_reply_cb_t on_frame, void *ctx);

/* 语音回合：PCM(16k/mono/s16) → 云端 ASR → 文本回合 */
esp_err_t ab_chat_pcm(const int16_t *pcm, size_t bytes,
                      ab_reply_cb_t on_frame, void *ctx);

/* 附件下载（云端下发的图）→ /sdcard */
esp_err_t ab_fetch_file(const char *filename, char *out_path, size_t n);
```

**SSE 解析**只有三行核心（接入方案 §13.1）：

```c
/* 逐行读，去掉 "data: " 前缀，cJSON_Parse */
if (strncmp(line, "data: ", 6) == 0) { frame = cJSON_Parse(line + 6); }
```

> ⚠️ **TLS 内存**：`ab_client` 若用 `esp_http_client` + mbedTLS 直连 HTTPS，需约 **40–60KB** 堆（握手期峰值）。现有 `ai_camera.c` 的 explain 已用 PSRAM 栈 16KB + TLS（`:50` `EXPLAIN_STACK_SIZE`），说明 TLS 在本工程已验证可跑 —— **照抄这个模式即可**（F35 纪律：TLS/HTTP 栈放 PSRAM）。

---

## 4. MCP 保留方案（本方案技术含金量最高的部分）

### 4.1 证据链：工具层与传输层本来就没耦合

1. **引擎由调用方注入**：`esp_xiaozhi_chat.h:186`
   ```c
   esp_mcp_t *mcp_engine;        /*!< MCP engine instance provided by the caller */
   bool owns_mcp_engine;         /*!< chat 是否接管其生命周期 */
   ```
2. **工具注册在引擎上，不在会话上**：`xiaozhi_service.c:447-483`
   ```c
   esp_mcp_create(&mcp);                 /* 引擎与连接无关 */
   ai_light_attach(mcp);  ai_speaker_attach(mcp);  ai_camera_attach(mcp);
   ai_punch_attach(mcp);  ai_music_attach(mcp);    ai_device_attach(mcp);
   ```
3. **六个 `ai_*.c` 文件均不含任何小智协议符号** —— 只依赖 `esp_mcp_engine.h` + 各业务 middleware。

**结论：保留 MCP = 把 `esp_mcp_create` + 6 个 `attach` 从 `xiaozhi_service.c` 搬到新模块，一次复制粘贴。**

### 4.2 新传输：sdk 自带 HTTP server（省 200 行）

`esp_mcp_mgr_config_t` 接受**可插拔 vtable**（`esp_mcp_mgr.h:220`），且组件**已内置** `esp_mcp_transport_http_server`（`src/transports/esp_mcp_http_server.c:3915`）。

| 方案 | 做法 | 评价 |
|---|---|---|
| **A（推荐）** | 用 `esp_mcp_transport_http_server` 起 MCP HTTP 端点 | 最少代码，sdk 原生路径 |
| B | 复用现有 `web_server.c`（`:80`），直接调 `esp_mcp_mgr_req_handle()` 本地分发 | 最灵活，但要自己写 HTTP 路由（~80 行） |

两者都**不依赖**新写的 `astrbot_client` —— MCP 通道和聊天通道**完全正交**：云端插件既能通过 `context.send_message()` 推送，也能通过 MCP 端点直接调工具。

### 4.3 现有 9 个 MCP 工具（零改动清单）

| 工具名 | 来源文件 | 说明 |
|---|---|---|
| `self.camera.explain` | `ai_camera.c:477` | 拍照 + 视觉问答（**注意：依赖小智的 vision endpoint，见 §4.4**） |
| `self.camera.capture` | `ai_camera.c:490` | 拍照存 `/sdcard/photos` |
| `self.light.get_state` / `set_*` | `ai_light.c` | RGB 灯控 |
| `self.speaker.get_volume` / `set_volume` | `ai_speaker.c:66,76` | 音量 |
| `self.music.play/download/list/stop` | `ai_music.c:597-647` | 点歌（走 `music_proxy` 云端音源） |
| `self.punch.clock_in` | `ai_punch.c:530` | 人脸打卡 |
| `self.device.reboot` / `self.wifi.get_status` | `ai_device.c:103,108` | 设备控制 |

### 4.4 ⚠️ 唯一的 MCP 迁移风险：`self.camera.explain`

`ai_camera.c` 的 explain 依赖 **`esp_xiaozhi_chat_get_vision_endpoint()`**（URL/token 由小智服务器在 MCP initialize 时下发，见 `chat.c:538-576` 的 §58/§59b 勘误）。**换到 AstrBot 后这个 endpoint 没有了。**

解决方案（三条，按推荐序）：
1. **改用 AstrBot 的图片入口**：`POST /api/v1/file` 换 `attachment_id` → 消息段 `{"type":"image","attachment_id":...}`（接入方案 §7.2）。让云端 Qwen-VL 来"看"，设备不调视觉 API。
2. 自配一个视觉 API（百炼 qwen-vl 有免费额度），`ab_client` 直接调。
3. 暂时从工具表摘除 explain（其余 8 个工具不受影响）。

> **这是必须提前决策的点**，直接影响 `ai_camera.c` 约 100 行的改动范围。

---

## 5. SD 卡存储（现状已满足）

| 内容 | 现状落点 | 是否需要改 |
|---|---|---|
| 拍照 | `/sdcard/photos`（`ai_camera.c`） | ❌ 不用 |
| 录像 | AVI/MJPEG（`record_video_service.c`） | ❌ 不用 |
| 录音 | `/sdcard/music` WAV（`rec_local_service.c`） | ❌ 不用 |
| 云端下发的图 | **当前无此通道** | ✅ 新增 → `/sdcard/cloud_media` |
| 人脸库 | `/sdcard/face_db` | ❌ 不用 |

**新增只需要一个目录 + 复用 `fs_service`**。注意 SD 卡容量约束与"换卡需重启"的既有全局约束（README §内存纪律）。

---

## 6. 语音转文字：百炼 vs faster-whisper（你的核心问题）

### 6.1 先把账算清楚：500MB 到底能干什么

服务器总内存 500MB，扣除 Linux + AstrBot（Python，通常 150–250MB 常驻），**真正可给 ASR 的余量约 150–200MB**。这比我上一轮估计的 300–350MB 更紧 —— 因为 AstrBot 本身是个吃内存的 Python 进程。

**这个数字决定了一切**：不是"能不能装下一个模型"，而是"能否在 AstrBot 已占用之后还能挤出 200MB"。

### 6.2 你的百炼额度清单：推荐结论

你的清单里 11 个模型中，**有效额度只有 3 个是 1M 秒级别**，其余都是 36K 秒：

| 模型 | 额度 | 到期 | 定位 | 推荐度 |
|---|---|---|---|---|
| **`qwen3-asr-flash`** | 36K 秒 | **2027-04-23（181天）** | 短语音、同步返回，**主路径首选** | ⭐⭐⭐⭐⭐ |
| `qwen-audio-3.1-asr-flash-streaming` | **1M 秒** | 2027-01-23 | 流式，额度最大 | ⭐⭐⭐⭐ |
| `qwen-audio-3.1-asr-flash-filetrans` | **1M 秒** | 2027-01-23 | 长音频文件转写 | ⭐⭐⭐⭐ |
| `qwen-audio-3.1-asr-flash-message` | **1M 秒** | 2027-01-23 | 多轮对话式音频 | ⭐⭐⭐ |
| `fun-asr` | 36K 秒 | 2027-01-23 | 通用语音识别 | ⭐⭐⭐（备选） |
| `qwen-audio-3.0-*` 系列 | 36K 秒 | 2027-01-23 | 旧版 | ⭐ |
| `fun-asr-flash-8k-realtime` | 36K 秒 | 2027-01-23 | **8k 采样率** | ⚠️ 见下 |

#### 为什么主路径是 `qwen3-asr-flash`

1. **额度最低但有 181 天**（其余 1M 级别的都在 91 天后到期）；
2. **同步返回**，与 PTT 单次回合模式天然匹配（`XZ_LISTENING_MODE_MANUAL`，`xiaozhi_service.c:547`）；
3. 设备上行是 **16kHz**（`XZ_SAMPLE_RATE 16000`，`:71`），`fun-asr-flash-8k-realtime` 是 **8k** —— 要降采样，多一步且音质损失，**不推荐**。

#### 额度够用多久？（重要）

按单次语音 **4 秒**（PTT 一句话）、16k 单声道：

```
36,000 秒 / 4 秒 ≈ 9,000 次对话
                   ≈ 每天 30 次 × 10 个月
```

**结论：36K 秒对个人使用完全够，且有 181 天有效期，无需省着用。** 真正的风险不是额度，是**到期日不同步**——建议把模型名做成配置项，到期前可切换。

#### 两条路的分工建议

| 场景 | 用哪个 | 理由 |
|---|---|---|
| 日常 PTT 一句话 | `qwen3-asr-flash` | 同步返回、延迟低 |
| 长录音转写（如 REC 页 30s+） | `qwen-audio-3.1-asr-flash-filetrans` | 1M 秒额度，适合长音频 |
| 流式边说边识别（未来） | `qwen-audio-3.1-asr-flash-streaming` | 1M 秒，但目前 PTT 模式用不上 |

### 6.3 faster-whisper 在 500MB 上现实吗？—— 现实，但只能当兜底

**先纠正我上一轮的判断**：faster-whisper 用的是 CTranslate2，**int8 量化后内存占用远低于 PyTorch 版 Whisper**。在 500MB 机器上并非不可能：

| 档位 | 模型体积 | 常驻内存（int8） | 500MB 可用性 | 中文准确率 |
|---|---|---|---|---|
| `tiny` | ~75MB | ~100MB | ✅ **可以** | ❌ 很差，不懂中文口语 |
| `base` | ~145MB | ~180MB | ⚠️ **临界** | ❌ 勉强 |
| `small` | ~480MB | **~350–450MB** | ❌ **超了** | 🟡 可用 |
| `medium`+ | >1GB | >1GB | ❌ 不可能 | — |

**换算到你的机器**：AstrBot 已占 150–250MB，剩余 250–350MB。所以：

- `small`（需要 350–450MB）→ **大概率 OOM**，即使勉强启动，AstrBot 会被拖死；
- `base`（180MB）→ **可能跑得动**，但中文识别质量对"自然语言调用插件"这种场景**不可接受**（命令词错一个字插件就命中不了，正好踩中接入方案 §5.1 的坑）；
- `tiny`（100MB）→ 能跑，但中文基本不可用。

#### 最终结论

> **faster-whisper 在 500MB 上：`tiny`/`base` 能跑但中文不可用，`small` 中文可用但内存不够。**
> **所以它不能当主路径，但值得作为"断网/额度耗尽"的降级兜底**——因为兜底场景下"能识别个大概"好过"完全没反应"。

**推荐的兜底实现（降级链）**：

```
百炼 qwen3-asr-flash  ── 失败/超时/额度尽 ──→  faster-whisper base-int8
                                                    └─ 再失败 ─→ 云端回一条文字"我没听清，你打字告诉我"
```

**如果一定要在服务器本地做主 ASR**，还有一条被忽略的路——**CPU 型号比内存更关键**。faster-whisper 是 CPU 推理：
- 4 秒音频在 4 核上 `base-int8` 约 **0.5–1.5 秒**，可用；
- 如果是 1–2 核的入门云主机，可能 **3–5 秒**，叠加 AstrBot 的 LLM 延迟，体验会崩。

> **【需核对】请在服务器上跑这三条命令再决策**：
> ```bash
> nproc                       # 核数
> free -m                     # 看 available 而不是 free
> docker stats --no-stream    # 看 AstrBot 实际占用
> ```

### 6.4 为什么"服务器只做中转"是正确架构

| 维度 | 百炼 API 中转 | 本地 faster-whisper |
|---|---|---|
| 新增内存 | **~10MB**（一个 Python 转发进程） | 100–450MB |
| 新增 CPU | 几乎为零 | 每次识别占满核心 1–5s |
| 中文准确率 | 高（Qwen 系专门优化中文） | base 档不可用 |
| 成本 | 免费额度内为 0 | 0 |
| 依赖 | 需要外网（你的服务器本来就在跑 AstrBot，必然有外网） | 完全离线 |
| 失败模式 | 网络抖动 → 重试即可 | OOM → 拖死 AstrBot |

**结论：中转方案的每一项都更好，唯一的优势是离线。所以：主路径走百炼，faster-whisper 只做离线兜底。**

### 6.5 音频格式决策：PCM 还是 Opus？

这是一个**必须提前定的技术选型**，直接影响带宽和 CPU：

| 格式 | 4秒音频大小 | 设备 CPU | 服务器 CPU | 备注 |
|---|---|---|---|---|
| **PCM 16k/mono/s16** | **128 KB** | 0 | 0 | 直观，但要 128KB/s 上行 |
| **Opus 24kbps** | **12 KB** | 已有编码器 | 需解码 | 省 10 倍带宽 |

- 设备现有 Opus 编码器是 `av_processor` 的 recorder 内置（`XZ_OPUS_BITRATE 24000`，`:74`）；
- **但百炼 API 接受的是常见音频格式（wav/mp3 等），Opus 需确认是否支持【需核对】**；
- **推荐：PCM 直传 + 服务器侧封 WAV 头**（44 字节），最简单、零依赖、无格式兼容风险。128KB/4秒在家庭宽带上无压力（需约 256kbps 上行）。

> **【需核对】** `qwen3-asr-flash` 的输入格式支持列表与单次时长上限（我无法联网核实）。若只接受 base64 内嵌，4 秒 PCM 的 base64 约 170KB，需确认请求体上限。

---

## 7. 必须补的四个功能点

### 7.1 云端下发的图片：当前完全没有通道（P7 最高优先级）

**证据**：`esp_xiaozhi_chat.c:729-734` 的帧分发表**只有 6 种**：
```c
{ "hello", ... }, { "goodbye", ... }, { "mcp", ... },
{ "tts", ... }, { "stt", ... }, { "llm", ... }
```
**没有 `image` 处理**。而云端插件（如 `/setu_file`）用 `File` 组件发图，SSE 里是 `"type":"file"`：

```json
{"type":"file","data":"[FILE]2026..._960c.jpg|白发斗篷少女_20261023.jpg"}
```

**新方案必须做三件事**：
1. **认 `file` 帧**：`file`/`video` + 图片扩展名 → 归一成 `image`（接入方案 §5.4 的修法）；
2. **去重**：`image_mode` 图集模式会同时发同名 `file` + `image` 帧，按云端存储名去重，否则落两次盘；
3. **下载落盘**：`[IMAGE]xxx.png` 是**文件名不是 URL**，需再 `GET /api/v1/files/content?filename=` 取字节（接入方案 §7.1）。

### 7.2 设备 → 云端发图（拍照问）

**不能**把 data URL 塞进 `message`，必须两段式（接入方案 §7.2）：
```http
POST /api/v1/file          # multipart，字段名 file
→ {"attachment_id": "..."}
```
然后消息体用段数组：
```json
{"message":[{"type":"plain","text":"看图"},{"type":"image","attachment_id":"..."}]}
```
**这条路径正好可以替代 §4.4 里失效的 `self.camera.explain`。**

### 7.3 插件命令不能加设备前缀（会让 17 个插件全部失效）

**证据**：AstrBot 命令匹配是**纯字符串前缀判断**（`star/filter/command.py:202`）：
```python
if message_str.startswith(f"{full_cmd} ") or message_str == full_cmd:
```

所以一旦给每条消息加 `[ESP32-S31] [设备上下文] 用户说：…` 之类的前缀：
- `/setu_file`、`/p`、`/余额` 全部不在开头 → **全部交给 LLM 闲聊**；
- 更糟：LLM 会**编造执行结果**（"好，就这张"），用户以为插件坏了。

**必须实现三层判定**（接入方案 §5.2）：
```c
bool is_plugin_command(const char *text) {
    if (strchr("/!！", text[0])) return true;          /* ① 标准前缀 */
    for (命令表) if (lower_starts_with(text, name))   /* ② 登记命令（含中文） */
        return true;
    return 关键词兜底;                                  /* ③ 角色名/作品名 */
}
```

> ⚠️ **命令表必须跟着插件走**：装了新插件就要补登记，否则新命令又掉进闲聊。建议把命令表做成 **NVS 可配 + 云端下发**，别硬编码在固件里。

### 7.4 文字先于语音（气泡时序）

```c
/* ❌ 错误顺序 */
tts_synth(reply);            /* 几秒~十几秒 */
publish_bubble(text);        /* 气泡 3~8 秒就收起 → 永远看不到 */

/* ✅ 正确顺序 */
publish_bubble(text);        /* ① 文字立刻，无条件 */
tts_synth(reply);            /* ② 语音是增强 */
play_audio(blob);            /* ③ 成功再补 */
```
**判据**：日志里 `agent_bubble` 必须早于 `agent_audio`；TTS 失败**不能**影响文字显示。

---

## 8. 工作量与分阶段

| 阶段 | 内容 | 工作量 | 风险 | 交付判据 |
|---|---|---|---|---|
| **P0** | 服务器实测 + 决策 | 半天 | 低 | `free -m` / `nproc` / `docker stats` 出数；确认百炼 API 格式上限 |
| **P1** | `astrbot_client.c`（纯文字 + SSE 流式） | 3–5 天 | 中 | 屏幕输入一行字 → 收到 `plain` 帧完整正文；`model_id` 是 `cloud_feiyu` |
| **P2** | ASR 中转（`asr_proxy.py`）+ PCM 上行 | 2 天 | 低 | 说一句话 → 云端日志出现正确文字 |
| **P3** | MCP transport 迁移（§4） | 2–3 天 | **低**（架构已验证） | 9 个工具在自研链路上全部可用 |
| **P4** | 下行图片 + 上行发图（§7.1/§7.2） | 2–3 天 | 中 | `/setu_file 1` → 图落到 `/sdcard/cloud_media` 并显示 |
| **P5** | 插件命令识别表 + TTS 下行 + 气泡时序 | 2–3 天 | 低 | 云端日志 `preview='setu_file 1'` **干净** |
| **P6** | faster-whisper 兜底（可选） | 1 天 | 中 | 断网测试能识别 |

**总计约 3 周**。关键路径是 P1（TLS + SSE，工程细节多），最大风险反而在 P0（服务器资源是否够）。

**P3 之所以风险低**：§4.1 的证据链是**读源码得出的**，不是推测 —— 工具层与传输层的解耦是 sdk 的设计事实。

---

## 9. 联调方法论（照接入方案 §13 执行，能省几十轮试错）

| 手段 | 做法 | 能定位什么 |
|---|---|---|
| **① 抓 SSE 原始帧** | 逐帧打印 `type` + `data[:120]` | 是"云端没发"还是"本地丢了" |
| **② 看云端收到什么** | `docker logs astrbot --since 10m \| grep -iE "preview="` | `preview='setu_file 1'` 干净 vs 带前缀污染 |
| **③ 看走没走云端** | 响应里 `model_id` / `route` | 误落回本地直连模型（接入方案 §5.5 最隐蔽的坑） |

**排查顺序图**（照抄接入方案 §13.4）：
```
插件用不了
  ↓ ① 云端收到了什么？ → preview 干净吗
  ↓ ② 请求走云端了吗？ → model_id = cloud_feiyu?
  ↓ ③ 云端回什么帧？   → 有 file/image 帧吗
  ↓ ④ 本地收下了吗？   → kind 判成 image 了吗
  ↓ ⑤ 设备显示了吗？   → 气泡/图片真出现了吗
```

> **必须新增一条**：本仓库 `docs/_serial_capture.py` 只能抓串口日志。**建议同时写一个 `tools/diag/sse_frame_dump.py`**（接入方案 §13.1 已有现成实现，直接搬），端到端验证不经过 UI。

---

## 10. 风险登记表

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | 服务器 500MB 被 AstrBot 吃满 | ASR 兜底/中转进程 OOM | **P0 先实测**；ASR 进程加 `memory limit`；优先走 API 不跑本地模型 |
| R2 | 百炼额度到期日不同步（36K@181天 vs 1M@91天） | 某天突然失效 | 模型名做成配置；到期前切换；faster-whisper 兜底 |
| R3 | `self.camera.explain` 依赖小智 vision endpoint | 换自研后该工具失效 | §4.4 三选一，**提前决策** |
| R4 | 下行 `file` 帧被静默丢弃 | 插件图"凭空消失"，无日志 | §7.1 归一 + **丢弃必记原因**（P7） |
| R5 | 命令前缀污染 | 17 个插件全部失效 + LLM 编造结果 | §7.3 三层判定 + 云端日志验证 |
| R6 | TLS 握手堆峰值 | 设备内存紧张（本工程内部 RAM 余量本就 ~4KB） | 照抄 `ai_camera.c` 的 PSRAM 栈模式 |
| R7 | 非流式请求拿不到正文 | 对话"没反应" | §3.3 强制流式 |
| R8 | 百炼输入格式/时长上限未知 | P2 返工 | **【需核对】**，P0 一并验证 |

---

## 11. 待决策清单（需要你拍板）

| # | 决策点 | 选项 | 我的建议 |
|---|---|---|---|
| D1 | 主 ASR | 百炼 `qwen3-asr-flash` / 本地 faster-whisper | **百炼**（36K秒够用 10 个月，零内存压力） |
| D2 | 兜底 ASR | faster-whisper base-int8 / 无兜底 | **base-int8**（仅断网时启用） |
| D3 | `self.camera.explain` | AstrBot 图片入口 / 自配视觉 API / 摘除 | **AstrBot 图片入口**（顺带把 §7.2 一起做了） |
| D4 | 音频格式 | PCM 直传 / Opus | **PCM**（最简单，128KB/4秒可接受） |
| D5 | MCP 传输 | sdk 内置 http_server / 复用 web_server | **sdk 内置**（省 200 行） |
| D6 | 会话 ID | 设备独立 / 复用微信 | **设备独立**（接入方案 §3.4 的血泪教训） |
| D7 | TTS | 本地 TTS 引擎 / 云端 TTS / 暂不做 | 需你确认 AtrBot 侧是否已配 TTS 插件 |

---

## 12. 附：源码索引（本仓库，本方案涉及）

| 要找什么 | 路径:行号 |
|---|---|
| MCP 引擎由调用方注入 | `components/esp_xiaozhi/include/esp_xiaozhi_chat.h:186` |
| 6 个 `ai_*_attach` 注册点 | `main/middleware/ai/xiaozhi_service.c:447-483` |
| **协议帧分发表（只有 6 种，无 image）** | `components/esp_xiaozhi/src/esp_xiaozhi_chat.c:729-734` |
| vision endpoint 截获（§58/§59b） | `esp_xiaozhi_chat.c:538-576` |
| MCP HTTP server vtable | `managed_components/espressif__mcp-c-sdk/include/esp_mcp_mgr.h:225` |
| MCP 本地分发入口 | `esp_mcp_mgr.h:361` `esp_mcp_mgr_req_handle()` |
| 音频上行参数（16k/mono/60ms） | `xiaozhi_service.c:69-74` |
| 回声静默窗 / 聋化自愈 / barge-in | `xiaozhi_service.c:134-154` |
| PSRAM 栈 + TLS 模式范例 | `main/middleware/ai/ai_camera.c:50`（16KB PSRAM 栈） |
| 音频租约互斥 API | `main/middleware/audio/audio_service.h:116-140` |
| 9 个 MCP 工具定义 | `ai_camera.c:477,490` / `ai_music.c:597-647` / `ai_punch.c:530` / `ai_speaker.c:66,76` / `ai_device.c:103,108` |

## 13. 与 S7 文档的关系

S7（`S7_小智待发掘能力考古.md`）列出的 A/B/C 级能力，**大半在本方案中焕发新生命**：

| S7 条目 | 原状态 | 本方案后 |
|---|---|---|
| A1 Vision 拍照问答 | 需改 vendor（§58 已完成） | 🟡 改走 AstrBot 图片入口（§4.4） |
| A2 EMOJI 情绪灯效 | 已 emit，app 未接 | 🟢 **AstrBot 无此协议帧 → 搁置** |
| A3 SYSTEM_CMD reboot | 已 emit，app 未接 | 🟢 改为 MCP `self.device.reboot` 已有，**无需此通道** |
| B1 MCP 点歌 | 挂起等 I2S 收口 | 🟢 **已落地**（`ai_music.c`，9 工具之一） |
| B2/B3 唤醒词 + MultiNet | 挂起等内存手术 | 🟢 **本方案更不需要**（ASR 走云端） |
| C 级 MCP 工具位 | 待加 | 🟢 本方案可直接补 |

> **一句话**：换掉小智服务器后，S7 里两个"待发掘"项（A2 情绪、A3 系统命令）直接失去意义——**这说明自研的正确收益不是"多出功能"，而是"少维护一堆协议补丁"**。
