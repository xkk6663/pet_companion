# S10 桌面总结监听学习助手：闸门 · 日复盘 · MCP resources

> **当前项目定位（2026-11-15 调整）**：放在桌面，白天按 VAD 切片记录桌面前发生的话语，
> 晚上自动汇总成“今天做了什么、围绕哪些主题、有什么结论、学到什么、还有什么没闭环”，
> 再由大肥鱼人格通过微信推送一张“今日桌面复盘卡”。
> 人物关系网不再是主线，G4 保留开关但默认关闭；P7 从“主体画像”改为“桌面活动画像”。
>
> 状态：**P0–P6 已实施；P1–P4 实机通过；P5/P6 云端真实数据跑通；存储回收已落地；A/B 合并已落地；P7 未开始**
> （2026-11-15，v7 合并版）
> v2 修订：上行策略改为"在线直推、断网兜底"（§3.7）；v3：补 §13 实施状态与验收清单；
> v4：补 §13.7 成长模块；v5：定位改为桌面复盘，夜间报告新增主题/时间线/学习点/未闭环；
> v6：补 §13.10 存储与回收（音频 30 天 / raw 180 天，03:30 定时清理）；
> v7：补 §13.12 A/B 合并（设备端 1.2s 合并窗口 + 云端 30-60s 分组转写）
> v8：补 §13.13 兜底（SD 容量/TTL、云端空数据短路、ASR 结果缓存）
> 基线：当前工作区（ESP32-S31 双麦 BSS + AFE VAD / 60 ms 帧 / 15 个 MCP 工具 + 1 个纯本地动作 /
> 网关 `v0.8.1` :8081 / AstrBot v4.29.1）
> 上游文档：[架构分析](架构分析.md)（端云五层与回合时序）、[S9 云端网关](S9_云端网关_开发文档.md)（接口契约）、
> [S9 最终架构选型](S9_灵魂大脑自研_最终架构选型_桌宠伴侣版.md)（连接模型）
> 本文讨论 S9 之后的**闸门、记忆与表达**：什么时候进入系统、进来之后记住什么、
> 记下来怎么被读到和用出去。原“陪伴体三层扩展”是历史名称，能力骨架不变。

---

## 0. 一句话结论

**这套骨架已经具备"陪伴体"的 ①~③ 与 ⑤~⑦ 层，缺的只有 ②（闸门）和 ④（记忆）。
这两块的改动面确实都在软件里，但代价不是"加两个文件"，而是三件事：**

1. **闸门不是加一个 VAD 判断，是改"音频所有权模型"** —— 现在录音管线是**会话的附属物**：
   `audio_recorder_open()` 在 `session_prepare()` 里建（`xiaozhi_service.c:959`），会话 60 s 空闲就拆
   （`XZ_IDLE_CLOSE_S`，`:157`）。常开监听必须先把"监听面"从"对话面"里剥出来，否则 60 s 后自己把耳朵关了。
2. **VAD 已经算好了，只是没人接** —— `AV_PROCESSOR_AFE_FLAG_VAD_ENABLE` 一直开着
   （`xiaozhi_service.c:919-924`），但 `rcfg.recorder_event_cb = NULL`（`:933`）。
   AFE 的 `ESP_GMF_AFE_EVT_VAD_START / VAD_END`（`esp_gmf_afe.h:51-52`）本来就会回调，只是被丢弃。
   **接这根线是 1 个回调 + 1 个状态机，不是重新做 VAD。**
3. **记忆的地基设备上已经有一块了** —— `/sdcard/attendance/index.csv` 就是一条 **append-only、
   带时间、带人、带照片路径、带置信度** 的事件流（`punch_service.h:69-88,128-140`）。
   云端记忆不是从零建库，是**把这条流水扩成"所有事件"，再在其上重建人物/承诺/统计**。
4. **上行默认是"在线直推"，不是"设备缓存 + 夜间批量"** —— 对照录音豆的公开方案：它是**离线优先**
   （靠手机中继，连不上就存本地，连上批量传、传完删）；桌宠自连 Wi-Fi 且 USB 常供，**在线就该拿到即推**。
   SD 缓存只为"断网不丢事件"，不是主路径。v1 把前者当成默认，是错的（§3.7 已重写）。

一句话取舍：**闸门换 VAD，记忆换事件流，读记忆走 resources —— 三个动作，都不动硬件。**

---

## 1. 七层模型：把"感知→上传→云端处理"补成"陪伴体"

普通端云架构讲完"感知→上传→云端处理"就结束了，因为它默认终点是**回答**。
陪伴体的终点不是回答，是**在场**：它要决定什么值得记、记得住、并且能反过来做事。

| 层 | 职责 | 本项目现状 | 源码证据 | 缺口 |
|---|---|---|---|---|
| ① 感知 | 拾音 / 图像 / 传感器 | 双麦 BSS + AFE、OV3660、ES8389 | `mic_frontend.h:8-34`、`xiaozhi_service.c:914-932` | 无（够用） |
| ② 注意 | 决定"什么进入系统" | **PTT 按键**（人替系统做决定） | `xiaozhi_service.c:661-666`（注释原文就叫"闸门"） | **要新建** |
| ③ 语义 | 转写 / 识别 / 事件抽取 | SenseVoice 转写、端侧人脸识别、caps 意图快路 | `ws_gateway.py:304`、`face_recog_service.cpp`、`match_intent():580` | 事件抽取（夜间轨） |
| ④ 记忆 | 分层存储 / 衰减 / 检索 | 只有对话流水（AstrBot `livingmemory`） | 云端插件，不在本仓库 | **要新建** |
| ⑤ 人格 | 主体画像 + 表达风格 | 「大肥鱼」人设已定稿 | AstrBot persona | 主体画像（对谁是什么关系） |
| ⑥ 表达 | 复盘 / 吐槽 / 主动说话 | TTS + `punch_to_zh()` + `POST /push` | `ai_action.c:426`、`ws_gateway.py:928` | 复盘模板 |
| ⑦ 行动 | 反过来作用于现实 | **16 个动作**（15 MCP + wallpaper） | `ai_action.c:693-764`、caps 上报 | 由记忆触发（而非用户口令） |

### 1.1 为什么第 ⑦ 层是这个项目独有的

设备已经有"手"了：打卡、拍照、点歌、台灯、音量、重启、换壁纸，且**全部能被云端同步调用并拿到真实回执**
（`action` 帧下发 → `action_result` 回执，`ws_gateway.py:632-656`）。
录音笔/录音豆只能"记录"，它没有执行器；桌宠能提醒、能开口、能开灯。**所以"陪伴"在这里可以闭环，
在那类产品里只能停在"回放"。** 这不是能力差异，是结构差异。

---

## 2. 四条原则：哪些成立、哪些要改口径

### 2.1 原则 1「闸门决定全栈」——成立，但结论要往前推一步

成立的部分（可直接对源码）：

| 现状选择 | 因为闸门是 PTT，所以才能这么设计 | 证据 |
|---|---|---|
| 上行 **PCM 裸流** base64 | 一段只有几秒（≤10 s），320 KB 原始 / 427 KB base64，PSRAM 一块缓冲够 | `voice_service.h:18-27,45-47` |
| **不装 AEC** | 不会边说边听，喇叭出声时麦克风不采 | `xiaozhi_service.c:904-909` |
| 成本可控 | 只转写"按下去的那几下" | `ws_gateway.py:304` 只在收到 `pcm` 时调 |
| 一次 `pcm` 帧 = 一个回合 | 上下行时序简单，无 VAD 端点判定 | `xiaozhi_service.c:630-666` |

**要改的口径**："换闸门不用换架构"只对**代码落点**成立，对**所有权**不成立。换成常开 VAD 之后，
下面四条约束会同时被触发，必须一起解，否则一开机就撞：

| # | 约束 | 为什么常开会撞 | 现在的位置 |
|---|---|---|---|
| G1 | codec 是**独占租借**（`audio_service_ext_claim_exclusive`） | 监听要长期持 mic，而本地录音/MP3/蓝牙也要 codec | `audio_service.h:132-138`、`xiaozhi_service.h:19-20` |
| G2 | 会话 **60 s 空闲自动拆** | 拆了就 `audio_recorder_close`，耳朵没了 | `xiaozhi_service.c:157,688-697` |
| G3 | **没有 AEC**（无参考通道） | 自己播 TTS/音乐时，麦克风收到的是自己的声音，VAD 会一直触发 | `xiaozhi_service.c:904-909` |
| G4 | 内部 RAM 最大连续块常年 **4~7 KB** | 新任务栈、新队列都只能从内部 RAM 拿 | README §8、`xiaozhi_service.c:952` |

⇒ 结论：**闸门改造的正确命名是"把录音管线从会话里提出来，变成常驻的监听面"**，
不是"在 PTT 旁边并联一个 if"。后面 §3.4 给出的双平面模型就是为了解这四条。

### 2.2 原则 2「工具与记忆共用契约」——方向对，"零改动"要打折

- **协议层真的支持**：MCP SDK 两侧都有 resources。
  服务端 `esp_mcp_add_resource()`（`esp_mcp_engine.h:97-106`、`esp_mcp_resource.h`），
  客户端 `esp_mcp_mgr_post_resources_list/read/subscribe`（`esp_mcp_mgr.h:445-482`）。
- **但当前端云之间跑的不是 MCP**：设备↔云端是自定义 WS JSON（11 种帧），设备侧 MCP 只被
  `ai_action.c` 当**进程内 JSON-RPC 客户端**用（`tools/list` / `tools/call`，见 `ai_action.c:9-29`）。
- **所以"加一个 resource 云端零改动"只在一种前提下成立**：把 resources 也纳入 caps 自述，
  并且云端用**同一条中继通道**去读（§5.4 的 `resource_read` 帧）。
  否则要么网关加一条 HTTP 读口，要么 AstrBot 侧接 MCP client —— 两种都不是"零改动"。

**结论：契约同构的思路值得保留，"零改动"的说法要收一档：加维度只改"资源注册方"一侧 +
一次 caps/资源表上报；中继与读取口是共用基础设施，只建一次。**

### 2.3 原则 3「append-only 事件流 + 派生视图」——成立，而且设备侧已有原型

`/sdcard/attendance/index.csv` 的字段（`punch_service.h:128-140`）本身就是一条事件：

```
seq | person_id | name | date | time | rel(<NN_姓名>/<YYYYMMDD_HHMMSS>.jpg) | score | (未校时=--)
```

特点与"原始层"要求完全一致：**只追加、不修改；一次事件带人、带时刻、带证据（照片）、带置信度**。
缺的只是"事件类型"和"非打卡事件"。⇒ §4 直接把它当 schema 的样板。

### 2.4 原则 4「表达与事实解耦」——已有半成品，要继续沿着做

- 设备侧：`punch_to_zh()` 把 `"Punched: 张三 (match score 0.91)"` 翻成"「张三」打卡成功"
  （`ai_action.c:423-490`），**英文事实仍进串口日志**，中文只用于念给用户。
- 云端侧：`action_reply()` 拿设备回执组一句"章盖上了 —— …"，并明确要求"设备回的是中文整句就直接用"
  （`ws_gateway.py:659-682`）。
- 这已经是 **事实层（结构化、可追溯）→ 表达层（语气自由）** 的两段式。
  **复盘和吐槽必须沿用同一条路**：先出数字，再出人格判断，最后出话。绝不让 LLM 直接读当天转写"总结一下"
  —— 那样三个月后它会开始编故事（§6.3 给了四条硬约束）。

### 2.5 三处必须先纠正的事实（不纠正会照着错的前提做设计）

| 说法 | 源码事实 | 影响 |
|---|---|---|
| "AFE 有 NS 降噪" | **没有 NSNet**。`AV_PROCESSOR_AFE_FLAG_NS_ENABLE` 被 av_processor 接到 `se_init`，打开的是 **SE(BSS) 双麦盲源分离**，`ns_init` 根本没设（`xiaozhi_service.c:906-909`，注释已结案） | 24 h 监听别指望"降噪后更好转写"；要提高信噪比只能靠 BSS 或将来量麦距上 GSC |
| "8 MB PSRAM + littlefs 能存几个小时" | PSRAM 不是存储。**PCM 16 k/mono/s16 = 32 KB/s**，1 h = **115 MB**；`storage`(littlefs) 只有 8.12 MiB ⇒ **≈4.4 分钟** | 若真要离线落盘，只能落 **microSD**；littlefs 只放模型/字体/素材，**绝不写音频**。但注意：**在线直推时根本不落盘**（§3.7），这里的 115 MB 是上行流量，不是存储需求 |
| "SenseVoice 跑在那台 2C/1.7G 机器上，常开后是瓶颈" | **不在本机**。网关只是把 WAV POST 到 AstrBot 已配的 OpenAI 兼容端点（`ws_gateway.py:304-319`，provider 类型 `openai_whisper_api`，模型 `FunAudioLLM/SenseVoiceSmall`，见 S9 网关文档 §provider） | 瓶颈不是本机 CPU/内存，是**调用次数与往返延迟**。按切片直推（一片一次、秒级往返）可行；夜间轨只做抽取与断网积压补算 |

---

## 3. 闸门：从 PTT 到事件闸门

### 3.1 现状链路（带数字）

```
ES8389 双麦(右路反相, 软件固化 0x25.b5)
   └─ mic_frontend：BSS 模式 2ch "MM"（盲分离，不需要麦距）
        └─ AFE：SE(BSS) + VAD（vad_mode=4, min_speech=64 ms, min_noise=1000 ms）
             └─ recorder：PCM 16k/mono/s16，帧 60 ms（1920 B/帧，16.7 帧/s）
                  └─ xz_uplink_task：`if (s_ptt_on && s_link && ptt_active)` 才 feed   ← 闸门
                       └─ voice_service：累积 → base64 → 单帧 {"type":"pcm",...}（≤10 s / ≤427 KB）
                            └─ ws → 网关 → WAV → SenseVoice → 文本 → AstrBot
```

现有关键常量（改动前必须知道，改错就炸）：

| 常量 | 值 | 位置 |
|---|---|---|
| 采样 / 位宽 / 帧长 | 16000 / 16 / **60 ms** | `xiaozhi_service.c:73-76` |
| 上行读块 | `1920 * 2` = 3840 B（2 帧余量） | `:79-80` |
| PTT 单段上限 | 10 s = 320,000 B → base64 426,672 字符 | `voice_service.h:45-47` |
| 最短有效语音 | 0.2 s（低于此判误触，不发） | `voice_service.h:50` |
| 会话空闲自动拆 | 60 s | `xiaozhi_service.c:157` |

### 3.2 VAD 其实已经在跑，只是没人接

- AFE 的 VAD 开关一直是开的：BSS 模式 `NS_ENABLE | VAD_ENABLE`，MONO/GSC 模式 `VAD_ENABLE`
  （`xiaozhi_service.c:919-924`）。
- AFE 会通过 `recorder_event_cb` 抛 `ESP_GMF_AFE_EVT_VAD_START` / `ESP_GMF_AFE_EVT_VAD_END`
  （`esp_gmf_afe.h:48-56`；回调类型见 `audio_processor.h:228-236`）。
- **但 `rcfg.recorder_event_cb = NULL`（`xiaozhi_service.c:933`）**，事件被丢弃，
  `s_ptt_on` 是唯一决定"这块 PCM 要不要"的条件（`:663`）。

⇒ 改造的第一刀不是"引入 VAD"，而是**接上这个回调**，让 `VAD_START/END` 驱动闸门状态机。

> AFE 的 `delay_samples` 注释（`esp_gmf_afe.h:77-80`）提醒：若希望"从 VAD_START 起才开始用输出"，
> `delay_samples` 要不小于 `vad_min_speech_ms`，否则会丢开头一小段。本方案用**环形缓冲回捞**
> （§3.5），不依赖 `delay_samples`，因此不动 AFE 配置。

### 3.3 三种闸门方案对比

| 方案 | 做法 | 优点 | 代价 | 判断 |
|---|---|---|---|---|
| A. 保持 PTT | 现状 | 零成本、零误触发、隐私最好解释 | 永远需要"人手按"；不能主动注意 | 保留，作为**兜底与手动入口** |
| B. 纯 VAD 常开 | 会话常驻 + VAD 直接决定上传 | 改动最小（禁 idle close + 接回调） | codec 被占死（本地录音/MP3 全废）；自己放 TTS 时 VAD 自触发；一直传流 | **不推荐** |
| C. **双平面：监听面常驻 + 对话面按需**（推荐） | 监听面常驻录音、只做 VAD + 切片 + 直推；对话面沿用现有会话 | 互不打架；误触发只占内存、不上传；断网也不丢（SD 兜底） | 要改所有权（把 recorder 从 session 里提出来） | **推荐**，见 §3.4 |

### 3.4 推荐形态：把"耳朵"和"嘴"分成两个平面

```
┌ 监听面（常驻，不依赖会话）───────────────────────────────────────────┐
│  gate_task  ── 持有 recorder（或用独立 lease）                        │
│    VAD_START → 开一段 event slice（预滚 300 ms 从环形缓冲回捞）        │
│    VAD_END   → 收口（尾部保留 500 ms），交给三级闸门判定               │
│    判定通过  → 在线：立刻 POST /slice 直推云端（见 §3.7）              │
│                离线：溢写到 /sdcard/events/<YYYYMMDD>/ + .jsonl        │
│    判定拒绝  → 丢弃（只记计数，不落盘）                                │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ 事件切片就绪（元数据 + 音频字节/路径）
┌ 对话面（按需，沿用现有状态机）──▼──────────────────────────────────────┐
│  PTT 仍是最高优先：触发即 barge_in、停播、走 voice_service 上行         │
│  会话启动 = 从 gate_task 借 recorder / codec（一次交接，100~300 ms）    │
│  会话结束 = 60 s 空闲归还给 gate_task（idle close 保留，语义改为"还耳朵"） │
└────────────────────────────────────────────────────────────────────────┘
```

**为什么不是"会话常驻"**：`audio_service_ext_claim_exclusive` 的语义是**独占**
（`audio_service.h:132-143`，`xiaozhi_service.h:19-20`），会话常驻 = 本地录音、MP3、蓝牙外放全部被拒。
监听面必须是一个**可让位**的持有者，而不是新的独占者。

**交接协议（最小实现，建议 v1 就按这个）**：

| 事件 | 动作 | 超时/失败处理 |
|---|---|---|
| 用户按 PTT | gate 停切片、`audio_recorder_close`（或 pause）→ 通知 session 可起 | 300 ms 内没交出去：本次 PTT 走"老路径失败提示"，不硬等 |
| 会话空闲 60 s | session `session_teardown` → gate 重新 open recorder | open 失败 → 上报 `error:gate_recover_failed`，退化为 PTT-only |
| 本地录音/MP3 请求 mic | 先问 gate（新增 `gate_yield()`），gate 让位 | 正在切片：先收口再让 |

> 取舍：**"VAD 常开"与"codec 独占"天然冲突，选双平面就是把冲突显式化**。
> 不选"直接禁 idle close"是因为那条路会让 `audio_service_mic_available()` 长期为 false，
> 本地录音/通话入口全部静默失效 —— 那是"改好了一处、弄坏了四处"。

### 3.5 三级闸门：VAD → 结构判定 → 值不值得记

```
① 声学闸门（AFE 已给）
   VAD_START / VAD_END（min_speech 64 ms，min_noise 1000 ms → 自然按"1 s 静音"切句）
② 结构闸门（gate_task，纯本地，0 成本）
   时长   : 有效语音 ≥ 1.0 s 且整段 ≤ 30 s（连续独白按 30 s 强切，保证 PCM 单片 ≤4 MB）
   占比   : 段内 VAD 活跃帧 / 总帧 ≥ 15%（过滤"碰一下麦克风"）
   抑制   : TTS/音乐播放期间与结束后 300 ms 内，一律不开启切片（无 AEC 的必然选择）
   去重   : 与上一段间隔 < 800 ms 的，合并为同一段（避免"一句话被切成三段"）
③ 价值闸门（本地只做便宜判定，不拍板语义）
   白名单时段内 / 无人在场（见下）/ 长度达标 → 进入上行（§3.7）
   是否"值得记"交给云端：端侧只负责"这一段可能有用"，抽取与入不入库在云端决定
```

**"无人在场"怎么判（重要，避免过度承诺）**：

- v1 **只靠"听见"**：近 10 分钟有语音活动 / 有人在设备前完成过打卡（`/attendance` 刚写过）。
  这是零成本、零隐私风险的判据。
- **不要**为了"看见你在不在家"让摄像头常开：OV3660 常开 + ESP-DL 常跑人脸检测，
  功耗、发热、隐私三条都不划算，且 8 MB PSRAM 下与 AFE 抢内存。
  "有视线"是长期优势，但不是这一阶段的必要项。

### 3.6 无 AEC 的硬约束（这一条必须写进代码注释）

`AEC` 在本板**开不起来**：需要播放参考通道，而采集链路没有参考
（`xiaozhi_service.c:904-905`）。后果很直接：

```
喇叭出声 → 麦克风收到自己的声音 → FVAD 判定"有人说话" → 切片 → 转写"你自己刚说过的话"
```

所以闸门必须有**自回声屏蔽**，且三处都要：

| 位置 | 规则 |
|---|---|
| 切片开启时 | `s_tts_active || s_music_playing` → 拒绝开新切片（不是暂停，是直接不采） |
| 播放结束时 | 保留 300 ms 冷却，避免尾音被当成新语音 |
| PTT 打断时 | 沿用现有 `barge_in`：先停播、再上行事件、再录音（`xiaozhi_service.c:634-647`），顺序不能反 |

> 如果将来要"边说边听"（全双工），必须同时具备 AEC + 参考通道；那是硬件/驱动级改动，
> 不在本文范围。**v1 明确是半双工 + 事件切片。**

### 3.7 上行模型：默认在线直推，SD 只做断网兜底

> 本节是 v2 修订。v1 把"落盘 + 夜间批量"写成默认路径，等于把录音豆的**离线假设**错搬到了
> 一台常联网、常供电的桌宠上。对照录音豆的公开方案后，默认策略改为**在线直推、断网兜底**。

**（1）录音豆是怎么做的（对照基线）**

| 事实 | 原文 / 数值 | 来源 |
|---|---|---|
| 本地存储 | **8 GB 本地存储**（另有 64 GB 版本） | 36氪横评、京东商品页 |
| 传输方式 | "蓝牙传输（默认）"；另有 Wi-Fi 快传 | 飞书帮助中心 |
| 联网后行为 | "设备与手机正常连接时，设备中的文件会自动传输至飞书。**传输完成后，文件会从设备中删除**，以释放设备空间" | 飞书帮助中心 |
| 断网时行为 | "如果手机不在附近，它会把录音存在本地的 8 GB 存储中，等下次连接手机时自动传输" | 腾讯评测 / 飞书帮助中心 |
| 隐私口径 | "默认录音与转写内容存本地；只有选择转写时才临时上传云端，转写完成即从云端删除" | soundcore 官方英文页 |
| 实时转写 | 连接状态下的独立通道；"边录边转和文件传输冲突时，文件传输会暂停" | 飞书帮助中心 |

> 来源链接：飞书帮助中心《[安克 AI 录音豆产品使用说明](https://www.feishu.cn/hc/zh-CN/articles/445044627145-%E5%AE%89%E5%85%8B-ai-%E5%BD%95%E9%9F%B3%E8%B1%86%E4%BA%A7%E5%93%81%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8E)》
> ｜[腾讯新闻评测（2026-01-28）](https://news.qq.com/rain/a/20260128A03FVD00)
> ｜[36氪：飞书与钉钉 AI 硬件对比](https://eu.36kr.com/zh/p/3650392831418501)
> ｜[soundcore 官方产品页](https://www.store.anker.com/ae/products/d3200-soundcore-work-ai-voice-recorder)

⇒ 录音豆的真实模型是**离线优先**：边录边写本地 → 连上手机后批量传 → 传完即删。
这不是"技术落后"，而是它的拓扑决定的：它靠手机中继，蓝牙会断、人会走开、电量低时还禁 Wi-Fi 传输
（<30%），所以**本地存储是它的可靠性来源**，不是它的存储需求。另一条口径也印证这点：
官方英文页写"音频只在转写时临时上云，转写完成立即删除"——**云端是为了算，不是为了存**。

**（2）桌宠的拓扑不同，默认要反过来**

| 维度 | 录音豆 | pet_companion |
|---|---|---|
| 联网 | **本身不直接上互联网**：蓝牙连手机；"Wi-Fi 快速传输"是设备开热点给手机连（官方原文：加入设备自带的 Wi-Fi 热点） | 设备 STA 模式直连路由器，出站 WS 长连到网关（`ws_gateway.py:861` `/ws/{sid}`） |
| 供电 | 电池；"麦克风电量低于 30% 时……无法使用 Wi-Fi 传输" | USB 常供，无电量闸门 |
| 服务端 | 手机 App + 飞书云 | 局域网网关常开（`pet-gateway.service`） |
| 音频体量 | 8 GB 本地离线容量 | 只需覆盖"断网那几小时" |
| 默认策略 | 离线优先 → 连上批量 → 传完删 | **在线直推 → 断网兜底 → ACK 即删** |

**这就是"为什么它必须缓存、而桌宠不必"的根因**：录音豆**没有独立的上行链路**，
手机不在身边它就无处可传，本地 8 GB 是它唯一的落点；桌宠自己就是联网设备，
"拿到就推"在链路上成立。换句话说，缓存不是录音豆的设计偏好，是它的拓扑约束。

**（3）"直推"不等于"连续流推"（关键区分）**

- ASR 是**文件式端点**：网关把 WAV POST 给 OpenAI 兼容 provider（`ws_gateway.py:304-319`），
  一次调用必须是一段完整音频；没有可接的流式 ASR 会话。
- 现有 `pcm` 帧语义是"一个对话回合"：转写完直接进 `astrbot_chat_segments` → TTS。
  事件切片**不能**复用这个语义，否则每片都触发一次对话。
- ⇒ 直推的最小单位是**切片**（1~30 s 一句/一段），不是采样点级流。
  新增 `POST /slice/{sid}?id=&dur_ms=`：raw body（≤4 MB，复用 `ws_gateway.py:901` 的 raw-body
  写法与 `:86` 的硬闸），只做 `stt_transcribe` + 写 raw 事件，**不触发对话**。
- 为什么走 HTTP 不走 WS：WS 那条路是 text + base64，30 s PCM 编码后约 1.28 MB 字符，
  超过发送缓冲；HTTP raw body 没有 33% 膨胀，也直接复用现有 4 MB 通道。
- 设备侧**已有现成写法**：拍照上传就是 raw body POST（`ai_camera.c:295-321`，`esp_http_client`
  + `set_post_field` + `Content-Type`）。`slice_client` 照搬这段即可，960 KB 放 PSRAM 一次发完。

**（4）三条路径（按网络状态分流）**

| 状态 | 动作 | 数据落点 |
|---|---|---|
| 在线（WS 在连且 HTTP 可达） | 切片收口 → 立即 `POST /slice` → 返回 text → 写 raw 事件 | **不落 SD**；PSRAM 只留 1 MB 环形缓冲（≈32 s）做预滚与收口 |
| 上传失败 / 断网 > 60 s | 切片编码 OPUS 24 kbps 溢写 SD，`.part` → rename，追加 `index.jsonl` | `/sdcard/events/<YYYYMMDD>/`；恢复后按 `id` 幂等补传 |
| 云端 ACK | 删除本地文件与 index 行 | 设备端"传完即删"，与录音豆同策略 |

**（5）数字重算（按直推算，不按"存一天"算）**

| 项 | 数值 | 说明 |
|---|---|---|
| 单切片 PCM | 30 s = 960 KB | ≤4 MB，一片一 POST；PSRAM 峰值 < 2 MB |
| 单切片 OPUS 24 kbps | 30 s ≈ 90 KB | 仅断网溢写用 |
| 日均上行（1~2 h 语音） | PCM 115~230 MB / OPUS 11~22 MB | 局域网/家用宽带无压力；OPUS 是省流选项 |
| SD 兜底容量 | 32 GB ≈ 291 h PCM / ≈3000 h OPUS | 断网按小时计，8 GB 卡都富余 |
| "每小时 115 MB" | 只在"连续落盘"假设下成立 | 直推后它是**上行流量**，不再是设备存储需求 |

**（6）编码定位**
- 在线直推默认 **PCM**：复用现有 `pcm16_to_wav`，零转码损耗，ASR 质量最好；单段 ≤30 s，永远在 4 MB 内。
- 断网溢写用 **OPUS 24 kbps**（编码器现成，`av_processor_type.h:52,74-78`），把 SD 占用压 10 倍。
- ⇒ OPUS 从"主路径"降为"兜底路径的编码"，不是每段都要转码。

**（7）断电安全**
- 在线路径不落盘，断电只丢"正在收口的那一段"（≤30 s），可接受。
- 离线路径一片一段，写完 `fsync` 再追加 index；`.part` → rename，崩溃不产生半截文件被当成完整事件。

> 一句话：**设备端缓存的理由不是"存音频"，而是"断网不丢事件"**。在线就该直推，
> 只有网络不在时 SD 才出场。这也是录音豆"传完即删"能成立、而"先攒一天"不成立的原因。

### 3.8 设备端改动清单（文件级）

| 文件 | 改什么 | 规模 | 风险 |
|---|---|---|---|
| **新增** `main/middleware/audio/mic_gate.c/.h` | 常驻 recorder 持有、VAD 回调、切片状态机、PSRAM 环形缓冲、在线直推 / 离线溢写分流 | ~400 行 | 中（并发/内存） |
| `xiaozhi_service.c` | ① 把 recorder 创建/销毁从 `session_prepare/teardown` 抽出为 gate 的 open/close；② `rcfg.recorder_event_cb` 接到 gate；③ idle close 改为"还耳朵"；④ 保留 PTT 优先级 | ~120 行改动 | **高**（这段是全工程最敏感的生命周期） |
| `voice_service.c` | 名称不动，语义不变（仍是对话回合的 PCM 累积）；增加"gate 让位中"的等待 | ~20 行 | 低 |
| `audio_service.c/.h` | 增加 `gate_yield()/gate_resume()`；`mic_available` 把 gate 计为"可用但需让位" | ~40 行 | 中 |
| **新增** `main/middleware/ai/slice_client.c` | 在线 `POST /slice`（raw body，≤4 MB）、失败转 SD、恢复后幂等补传、ACK 即删 | ~200 行 | 低 |
| `main/app/main.c` | 初始化顺序：gate 在 audio_service 之后、xiaozhi_service 之前 | ~5 行 | 低 |

**内存纪律（必须照现有做法）**：

- 环形缓冲放 **PSRAM**（8 MB 里划 **1 MB ≈ 32 s** PCM，够 30 s 切片 + 收口），不占内部 RAM；
- gate_task 栈从**静态池**取（`main/middleware/mem/static_pool.h`），或 4 KB 内部 RAM，
  不能默认 7 KB（会与打卡会话抢最大连续块）；
- 队列深度 ≤ 4，元素带 `char*` 指向 PSRAM（照 `ai_action.c:88-93` 的做法：句柄在内部 RAM，正文在 PSRAM）。

### 3.9 验收判据（可观测、可对日志）

| # | 场景 | 期望日志/现象 |
|---|---|---|
| V1 | 开机不按任何键 | `GATE: listening (vad on)`，之后每段 `GATE: slice 4.2s -> POST /slice 200 (940 KB)`（在线直推） |
| V2 | 对着设备说 3 句话，中间停顿 | **3 条切片**，不是 1 条也不是 9 条（验证 1 s 静音切句 + 800 ms 合并） |
| V3 | 放 TTS 时说话 | 播放期间**不产生切片**；结束后 300 ms 内也不产生 |
| V4 | 按 PTT 说话 | 切片立即收口；会话正常起；说完正常上行；60 s 后 `GATE: resumed listening` |
| V5 | 拔网线 | 切片转 SD 溢写；恢复后按幂等补传，ACK 后本地删除 |
| V6 | 本地录音页 | 能正常拿到 mic（gate 让位），不报 `INVALID_STATE` |
| V7 | 24 h 运行 | 内部 RAM `largest_free_block` 不低于改动前基线的 80%；无 xrun 累积；SD 上无"已 ACK 未删"残留 |

---

## 4. 记忆：append-only 事件流 + 派生视图

### 4.1 地基：设备侧的打卡流水已经是"事件流"了

```
/sdcard/attendance/index.csv
seq, person_id, name, date, time, rel(照片相对路径), score
```

它满足原始层的全部要求：**只追加、不可变、带主体、带时刻、带证据、带置信度**
（`punch_service.h:69-88,128-140`）。所以记忆迁移的第一步不是"建表"，而是**把这条流水接进云端事件流**，
然后按同一 schema 扩出"非打卡事件"（语音切片、动作结果、设备状态）。

### 4.2 三层模型

```
① 原始层 raw      只追加的 JSONL，永不修改（纠错=新增一条 annotation 事件）
② 实体层 entity   人 / 关系 / 承诺 / 话题 / 地点 —— 全部可由 ① 重建
③ 统计层 stats    日/周聚合、承诺兑现率、活动节律 —— 全部可由 ② 重算
```

**原始事件（一条一行，JSONL）**：

```json
{"id":"evt_20261115_091203_a1","ts":"2026-11-15T09:12:03+08:00",
 "kind":"speech","source":"mic.gate","site":"desk",
 "dur_ms":4200,"audio":"audio/20261115/evt_20261115_091203_a1.wav",
 "asr":{"text":"我答应周五之前把方案发给张总","model":"SenseVoiceSmall","pipeline":"online"},
 "tags":["person:zhang_zong"],"prev":null,"schema":1}
```

**幂等与不可变**：`id` 由设备生成（时间戳 + 短随机），云端"同名即存在"直接跳过；
**任何修正都是新事件**：

```json
{"id":"ann_…","kind":"annotation","target":"evt_…","field":"asr.text","value":"…","reason":"…"}
```

**实体视图（可重建，带水位）**：

```json
{"person_id":"p_zhang","display":"张总","first_seen":"2026-09-12","last_seen":"2026-11-15",
 "relations":["work.counterpart"],"open_commitments":2,"built_from":"evt_…_a1..evt_…_z9",
 "generated_at":"2026-11-16T03:10:00+08:00","schema":1}
```

关键字段是 `built_from`（水位）：**任何上层视图都必须能回答"我这条结论是从哪几条原始事件推出来的"**。
这一条是防幻觉的地基，不是可选装饰（§6.3 依赖它）。

### 4.3 云端落地选型（2C / 1.7G，可用 ~746 MB）

| 方案 | 结论 | 理由 |
|---|---|---|
| 直接上 Postgres | 不推荐 | 单机 1.7G 里再养一个 DB 进程，swap 一开就抖 |
| **SQLite（WAL）+ JSONL 原始文件** | **推荐** | 零常驻进程；原始层用文件（可 grep、可备份、可整段丢弃）；实体/统计用 SQLite 表（可重建） |
| 直接塞进 AstrBot 的 `livingmemory` | 不推荐作为**原始层** | 那是"对话摘要"型记忆，改写/压缩不可控；原始层必须不可变 |

目录建议（与现有 `/opt/pet_gateway/` 同级，不动网关代码结构）：

```
/opt/pet_memory/
├── raw/20261115/events.jsonl        # 原始层（按天分文件，append-only）
├── audio/20261115/*.wav|*.opus      # 切片：在线直推存 WAV，断网补传存 OPUS；与 jsonl 同生命周期
├── derived/memory.db               # 实体/统计（可整库删除后重建）
└── nightly/20261115.report.json    # 夜间产物（复盘用的事实包）
```

### 4.4 写入时序：实时轨"边推边转"，夜间轨只做抽取与补算

```
实时轨（秒~十秒级，端侧 + 网关）
  切片收口 → POST /slice → SenseVoice 返回 text → 云端 raw 立刻写一条完整事件
             （kind=speech, asr.text 已填, pipeline="online"）
  对话回合 → 已有的 text/pcm 路径，顺手记 kind=dialogue（含 ASR 文本）
  动作回执 → kind=action（name/ok/data），这是**最可靠的事实源**（设备回执，不经模型）

夜间轨（03:00，批量；职责从"转写"收窄为"抽取 + 补算"）
  离线积压的切片（asr=null，正常在线时不该存在）→ 补转写 → 回写 kind=asr（target=原 id）
  → LLM 抽取（人/承诺/话题）→ 写实体层
  → 统计重算 → 产出 report.json（只含数字与出处，不含形容词）
```

**这样改的理由**：直推后 90% 以上的切片在白天就已拿到文本，"夜间批量转写"只剩两种用途——
断网积压的补算，以及抽取（抽取本来就适合离线批跑，可重跑、可换 prompt）。
把转写也压到夜间，只是把"当天可查"变成"隔天可查"，并不省钱（ASR 按次计费，切片数不变）。

**为什么 ASR 结果要写成新事件而不是改原事件**：白天转写失败/晚上重跑，原事件仍然成立
（"这里有一段 4.2 s 的语音"是事实），转写是**派生结论**，可重跑、可换模型。
这正是原则 3 的用法：**原始数据只追加，派生数据永远可重建。**

> 夜间任务用 systemd timer（`pet-memory-nightly.timer`），与现有 `pet-gateway.service` 同一套部署方式。
> 不要用"网关进程内 cron"，否则网关重启会吞掉当晚任务。

### 4.5 检索：结构化优先，embedding 兜底

| 问题类型 | 走哪条 | 例子 |
|---|---|---|
| 精确/聚合 | SQLite 查询（零成本、可复现） | "这周我打了几次卡""张总相关的承诺有几条未兑现" |
| 语义模糊 | embedding（AstrBot 已配 embedding provider） | "上次聊到搬家是什么时候" |
| 时间范围 | 直接扫 `raw/YYYYMMDD/` | "昨天下午我说了什么" |

**默认先结构化**：能算出来的不交给相似度。理由：数字错一次，人格信任就崩一次。

### 4.6 隐私与删除权（必须做，不做就是负债）

| 能力 | 实现 | 为什么必须 |
|---|---|---|
| 按天删除 | 删 `raw/YYYYMMDD/` + `audio/YYYYMMDD/` + 重建 derived | 用户说"把昨天删了"要能真删 |
| 按人删除 | 标记 `person:x` 的事件加 `redact` 注解，派生层重建时剔除 | 家里来客人，不该留下他的画像 |
| 保留期 | 默认音频 30 天、原始文本 180 天，超期只留统计 | 已由 `tools/memory/retention.py` + `pet-memory-retention.timer` 每天 03:30 执行；可改 env |
| 暂停监听 | 物理按键 / 网页管理端一个开关 → `gate_pause()` | 隐私的第一原则是"能关" |
| 指示灯 | 监听面活跃时 LED 常亮（`led_service` 已有） | 用户必须"看得见它在听" |

---

## 5. MCP resources：把"向内读"做成契约

### 5.1 对称结构

```
                 ┌─────────────── 大肥鱼（LLM）───────────────┐
                 │                                            │
      tools/call │                                            │ resources/read
    （向外的手）  ▼                                            ▼ （向内的记忆）
        ┌────────────────┐                          ┌────────────────────┐
        │ 设备动作（16 个）│                          │ 记忆资源（N 个）    │
        │ caps 自述       │                          │ 资源表自述          │
        └────────────────┘                          └────────────────────┘
```

两侧共用同一套纪律：**注册方自述、消费方动态发现、加一个维度只改注册方**。

### 5.2 resource server 放哪：云端为主，设备为辅

| 资源 | 归属 | 为什么 |
|---|---|---|
| `pet://events/…`、`pet://people/…`、`pet://commitments/…`、`pet://stats/…` | **云端**（pet_memory） | 数据在云端；设备不需要知道历史 |
| `pet://device/attendance`、`pet://device/media/recent` | **设备**（MCP engine resource） | 数据在 SD 卡上，云端拿不到；且天然受 NAT 保护 |
| 将来的 `pet://pet/health`、`pet://home/expense` | 云端 | 纯新增 resource，不动中继 |

**设备资源怎么被读到（NAT 的正确解法）**：不要指望云端直接访问设备 `:80`
（`/api/attendance/list` 确实存在，见 `web_server.c:1348`，但设备在路由器后面）。
两条可用路径，**推荐第二条**：

1. 设备主动推送：新帧 `{"type":"event","event":"attendance_new", ...}` → 云端记 raw；
2. **按需拉取（推荐）**：云端下发 `{"type":"resource_read","id":N,"uri":"pet://device/attendance"}`，
   设备侧走 `handler_attendance_list` 同一份数据，回 `resource_result` 帧 ——
   完全复用现有 `action`/`action_result` 的**请求-回执**语义，只是换了名字，网关零新增通道。

### 5.3 URI 与返回体（带 provenance 是硬要求）

```
pet://events/2026-11-15                 当日事件（分页：?limit=&before=）
pet://people/{person_id}                主体画像（可见事实 + 出处水位）
pet://commitments/open                  未兑现承诺（含 due、对方、出处）
pet://stats/weekly                      周统计（打卡/对话/承诺兑现率）
pet://device/attendance?since=…          设备打卡流水（直连设备侧资源）
```

返回体按 MCP resource 习惯：

```json
{"uri":"pet://commitments/open","mimeType":"application/json",
 "text":"{\"items\":[{\"id\":\"cm_…\",\"who\":\"p_zhang\",\"what\":\"发方案\",\"due\":\"2026-11-20\",\"from\":[\"evt_…_a1\"]}],\"count\":1}",
 "meta":{"built_from":"evt_…_a1","generated_at":"2026-11-16T03:10:00+08:00"}}
```

★ `text` 必须是**可直接被模型引用的事实**，不含形容词、不含推测。人格化的说法由表达层加（§6）。

### 5.4 caps 扩展：tools + resources 双自述

现在的 caps 帧只报工具（`ai_action.c:824-990`，缓冲 12 KB，实测 ~8 KB）。
扩展方式（**向后兼容**：老网关忽略 `resources` 字段即可）：

```json
{"type":"caps","model":"ESP32-S31-Korvo-1",
 "tools":[ … 现状不动 … ],
 "resources":[{"uri":"pet://device/attendance","name":"打卡流水",
               "mimeType":"application/json","desc":"最近 128 条打卡记录"}],
 "limits":{"audio":{"slice_max_s":30,"push":"online","offline_codec":"opus","offline_bitrate":24000}}}
```

设备侧实现最省事的版本：把资源注册进 MCP 引擎（`esp_mcp_add_resource()`），
caps 里用**进程内 `resources/list`** 枚举 —— 与工具用 `tools/list` 完全同构
（`ai_action.c:9-29` 已经在做这件事，加一个 `resources/list` 分支即可）。

### 5.5 AstrBot 侧的现实：resources 不一定直接进模型，需要一层适配

必须说清楚的前提：**LLM 的 function calling 只认"工具"，不认"资源"**。
所以 resources 是**契约层**，落到模型要经过适配，两种做法：

| 做法 | 说明 | 代价 | 适用 |
|---|---|---|---|
| ① AstrBot 作为 MCP client 连网关的 `/mcp` | 若 AstrBot 已支持 MCP server，直接 resources/read | 需核实 v4.29 的 MCP resources 支持程度；网关要加一个 MCP server 端点 | 长期最干净 |
| ② **一条工具 + 提示词注入**（推荐 v1） | 注册 `pet_memory_query(uri)`，内部转发 `resources/read`；会话开始把 `commitments/open` 摘要注入系统提示 | 多一个工具；注入占 token | 一周内可落地，不改 AstrBot |

★ 结论要写实：**"resources 零改动"只在契约层成立，模型侧永远需要一个入口。**
把入口收敛成**一个**工具（而不是每个记忆维度一个工具），就是这套设计真正省下来的地方：
加"宠物健康"维度 → 加一个 resource，**工具表不变**，模型不用重学。

### 5.6 验收"可插拔"

新增一个记忆维度时，允许改动的文件应当**只有**：

1. `pet_memory` 的资源注册表（一行）；
2. 抽取规则（夜间轨一个 prompt / 一份 schema）。

**不允许**改：设备固件、caps 帧格式、AstrBot 工具注册、网关转发逻辑。
这条与现有的"加工具只改固件"是同一条纪律的另一半。

---

## 6. 表达：复盘与吐槽

### 6.1 三段式（事实 → 判断 → 表达）

```
事实层（可追溯）   report.json：打卡 2 次 / 语音 41 段 / 承诺 3 条未兑现（出处 evt…）
   ↓ 只喂数字与出处，不喂原始转写
判断层（受人格约束） 大肥鱼对"3 条承诺未兑现"的态度（调侃/催办/装没看见）—— 由人格决定
   ↓
表达层（语气自由）  一两句话；可带一个动作建议；绝不新增事实
```

### 6.2 三个模板（都要有"没数据就不说"的分支）

| 模板 | 触发 | 事实输入 | 表达要点 |
|---|---|---|---|
| 晚间小结 | 21:30 定时 | 今日事件计数、打卡、未兑现承诺 | 一句事实 + 一句态度；**不逐条念流水** |
| 周复盘 | 周日 20:00 | 周统计、承诺兑现率、活动节律 | 一个趋势 + 一个具体建议 |
| 承诺催办 | 到期前 1 天 | `commitments/open` | 只提**对方与事由**，不评价人格 |

### 6.3 防编故事的四条硬约束

1. **表达层不得访问原始转写**，只能读 `report.json` / `resources/read` 的结果；
2. **每个数字必须带 `built_from`**，拼不出出处的数字一律不许说；
3. **无数据不说话**：当天 0 事件 → 输出"今天没什么可说的"，不许脑补；
4. **失败照实说**：沿用 `punch_to_zh()` 的纪律（`ai_action.c:426-480`）——"没拿到结论"就说没拿到。

> 这四条不是"风格建议"，是**防人格崩坏**。LLM 一旦被允许"读原始转写自由总结"，
> 它就会在信息不足时补细节；补三次之后，用户对整台设备的信任就没了。

### 6.4 渠道：复用 `/push`，不要新造一条

`POST /push {session_id, text, emotion, audio?}`（`ws_gateway.py:928`）已经把
`emotion → text → audio` 的下行顺序做完了。复盘/催办直接调它，**零新增协议**。

新增一个可选字段即可：`{"speak": false}` —— 免打扰时段（23:00~07:30）只落文字气泡、
不出 TTS（避免半夜把人吵醒，也避免吵醒后 VAD 把"翻身声"记成事件）。

---

## 7. 行动：让记忆驱动已有的 16 只手

### 7.1 一个工具都不用新写

| 记忆事件 | 可用的现成动作 | 调用通道 |
|---|---|---|
| 承诺明天到期 | `POST /push` 说话 + 屏幕提醒 | 网关 |
| 久坐（连续 2 h 有语音活动且无打卡移动） | push + `self.light.set_brightness` 调暗提醒休息 | 网关 action |
| 检测到"该睡了"（23:30 仍有语音） | push + `self.speaker.set_volume` 降音量 | 网关 action |
| 有人来访（陌生人脸 + 打卡 cooldown 内） | push 告知 + `self.camera.capture` 留证 | 网关 action |

全部走 `POST /action` → WS `action` 帧 → 设备执行 → `action_result` 回执
（`ws_gateway.py:632-656,952`）。**回执是唯一结论来源**，与打卡不让模型决定的纪律一致
（`README.md` §4.3）。

### 7.2 三种触发源（都不需要"主动推理"）

1. **时间**：到期、定时（systemd timer）；
2. **计数**：统计阈值（承诺数、久坐时长、事件量突变）；
3. **事件**：动作回执、打卡、`attendance_new`。

★ 明确不做"让 LLM 每隔 5 分钟自己想想要不要说话"—— 那是烧 token 且必然变成骚扰。

### 7.3 刹车（没有这个，陪伴体三天就变成烦人精）

| 规则 | 值 | 说明 |
|---|---|---|
| 主动开口频率 | ≤ 3 次/天，间隔 ≥ 90 min | 超了就并到晚间小结 |
| 免打扰 | 23:00–07:30 只文字不发声 | `speak:false` |
| 同类重复抑制 | 同一承诺 24 h 只催一次 | 按 `commitment_id` 去重 |
| 可撤销 | 每条主动消息带"别提醒这个"入口 | 用户一句话就能关掉一个维度 |

---

## 8. 四个取舍的结论

| # | 取舍 | 选项 | 推荐 | 理由（带本项目数字） |
|---|---|---|---|---|
| 1 | 常开 VAD 的误触发与隐私 | 全开 / **三级闸门 + 只传切片** / 不开 | **三级闸门** | 误触发只占内存、不上传；上传的只是"判定通过的事件切片"。家里有别人时，原始层可按人 redact；隐私上"事件录音"比"连续流"好解释得多 |
| 2 | 拿到音频就推，还是设备端缓存 | **在线直推** / 落盘 + 夜间批量 | **在线直推 + 断网兜底** | 录音豆是"离线优先"因为它靠手机中继（连不上就存本地，连上批量传、传完删）；桌宠自连 Wi-Fi + USB 常供，直推才是默认。30 s PCM 切片 960 KB ≤ 4 MB，一片一 POST；SD 只在断网时出场。OPUS 退为兜底编码 |
| 3 | 实时理解还是夜间批处理 | 只实时 / 只夜间 / **在线转写 + 夜间抽取** | **两轨分工** | 实时轨：切片直推 → ASR 拿文本，当天可查；夜间轨：抽取人/承诺、统计重算、补算断网积压。把转写也压到夜间不省钱（切片数不变），只损失在场感 |
| 4 | SenseVoice 跑在哪 | 本机部署 / 换云 API / **保持现状托管** | **保持现状** | 它本来就不在本机（`ws_gateway.py:304` 是 HTTP 转发到 AstrBot provider）。真正的瓶颈是**调用次数**；按切片直推后每次只处理 1~30 s，单次往返可控，不需要动部署 |

---

## 9. 实施顺序（每步都有独立价值，可随时停）

| 阶段 | 做什么 | 改哪些文件 | 工作量 | 风险 | 验收判据 |
|---|---|---|---|---|---|
| **P0 观察（只接回调）** | `recorder_event_cb` 接一个只打日志的函数；不改所有权、不上传、不落盘 | `xiaozhi_service.c`（+1 回调函数 ~10 行，`:933` 赋值） | 0.5 天 | 低 | 串口成对出现 `GATE: vad_start 09:12:03.120` / `GATE: vad_end 4.2s`；PTT、会话、TTS 日志与改动前逐条一致 |
| **P1 所有权拆分** | recorder 从 session 提到 gate；60 s idle close 改为"还耳朵"；本地录音/MP3 可 `gate_yield()` | 新增 `audio/mic_gate.c/.h`；`xiaozhi_service.c`（`session_prepare/teardown`）；`audio_service.c/.h`；`main.c` | 2~3 天 | **高** | V2/V4/V6 全过；24 h 无 `largest_free_block` 跌破基线 80% |
| **P2 切片状态机（本地）** | 1 MB PSRAM 环形缓冲、预滚 300 ms、尾部 500 ms、1~30 s 切片、TTS 期抑制；**先不联网** | `mic_gate.c/.h` | 1~2 天 | 中 | V1/V2/V3/V7；日志能对齐"3 句话 = 3 条切片" |
| **P3 网关 `/slice` + raw 存储** | 新增 raw-body 端点，ASR-only，写 `/opt/pet_memory/raw`，返回 text 与 ACK；幂等 | `tools/diag/ws_gateway.py`；`/opt/pet_memory/` | 1~2 天 | 低 | `curl --data-binary @x.wav` 返回 text；同 id 重传不新增；>4 MB 返回 413 |
| **P4 直推接线 + 断网兜底** | 在线 `POST /slice`；失败/断网 >60 s 转 OPUS 溢写 SD；恢复补传、ACK 即删 | 新增 `ai/slice_client.c/.h`；`mic_gate.c` | 1 天 | 中 | V1/V5：在线时 SD 无新增文件；拔网线后出现 `.opus`，恢复后补传并删除 |
| **P5 夜间抽取** | 断网积压补转写；LLM 抽取人/承诺/话题；统计；`report.json`（只含数字与出处） | 新增 `tools/memory/nightly.py` + systemd timer；SQLite | 2~3 天 | 中 | 同一批数据跑两次，report 数字逐字段一致 |
| **P6 resources + 查询 + 表达** | 资源注册、caps 扩 `resources`、一条 `pet_memory_query` 工具、`/push` 三模板 | `ai_action.c`（`resources/list` 分支 + caps）、`esp_mcp_add_resource`；网关适配层；`tools/memory/query.py` | 2 天 | 低 | 问"这周欠了谁什么"能给出 `built_from`；无数据时明确说"没有" |
| **P7 主体画像** | 关系分类、承诺兑现率、活动节律 | 派生层（可重建） | 3 天+ | 中 | 每个结论都能点到原始事件；改规则可重跑，不清数据 |

### 9.1 实施进度（2026-11-15）

| 阶段 | 状态 | 落地位置 | 说明 |
|---|---|---|---|
| P0 | ✅ 已完成 | `xiaozhi_service.c:826-843`（回调）、`:978`（赋值） | 行为零变化，删掉赋值即回退 |
| P1 | ✅ 实机通过（2026-11-15） | `mic_gate.c/.h` 新建；`xiaozhi_service.c:853`、`media_player_service.c:562`、`rec_local_service.c:114` 三处让位；`main.c:915` 初始化；`CMakeLists.txt` 登记 | `MIC_GATE_ENABLE=1`；启动 `listening: BSS layout=MM`，`task_wdt=0`、`afe_feed=0` |
| P2 | ✅ 实机通过（2026-11-15） | `slice_client.c` 环形缓冲/预滚/1~30 s 切片；`mic_gate.c` VAD 驱动 | 串口可见 `vad_start -> slicing` / `vad_end Nms -> slice`；当天落盘 141 条切片 |
| P3 | ✅ 已完成 + 自测 | `ws_gateway.py:972` `/slice/{session_id}`；`:86` 4 MB 闸 | 幂等、413/400、裸 PCM 全过；设备实机上传继续通过 |
| P4 | ✅ 实机通过（2026-11-15） | `slice_client.c:313` 初始化、`:397` 截取、`:209` 上传、`:228` 补传 | 在线直推 + SD 补传；`:170` 把 `ts` 的 `+` 编码为 `%2B`，新事件时区正确 |
| P5 | ✅ 已完成 + 真实数据跑通 | `tools/memory/nightly.py`、`query.py`、两个 systemd unit、`deploy_memory.sh` | `nightly --date 20261115`：141 事件 / 4.47 min，LLM 抽出板卡配置、设备树、编译输出等 topics |
| P6 | ✅ 云端完成 / 设备侧不做 | `ws_gateway.py:1089` `/memory/query`、`:1172` `resources/list`、`:1178` `resources/read` | 模型侧一条工具指向 `/memory/query` |
| P7 | ⬜ 未开始（改为“桌面活动画像”） | — | 不再做人物关系推断；只看时间块、主题分布、重复出现的项目/学习内容，依赖至少一周复盘数据 |

**依赖关系**：P2 与 P3 可并行（一个在设备、一个在服务端），P4 必须等两者都完成；
P0 → P1 是串行，且 P1 之前必须先定 D1 / D2 / D6。

**回滚点**（每步都要能一键退回）：
- P0：删掉 `recorder_event_cb` 赋值，回到 `NULL`；
- P1：`mic_gate` 用 `GATE_ENABLE` 宏包起来，置 0 即回到"会话内建 recorder"的老路径；
- P2/P4：同一宏关掉切片与上传，PTT 路径完全不受影响；
- P3/P5/P6：服务端新增端点/定时器独立部署，停 timer、摘路由即回退，不改设备。

**总计约 2~3 周**（与 S9 同量级）。关键路径是 **P1**：它动的是全工程最敏感的生命周期
（codec 租借 + 会话状态机 + 内部 RAM），必须先在 P0 用"只观察不改所有权"把 VAD 行为摸清。

---

## 10. 不做什么（本阶段明确不做）

| 不做 | 为什么 |
|---|---|
| 唤醒词 | 需要 KWS/WakeNet 模型与训练；S9 已经删掉唤醒词路径（`xiaozhi_service.h:14`），现在加回来是倒退 |
| 全双工 / AEC | 无参考通道，硬件级前提不满足（`xiaozhi_service.c:904-905`） |
| 摄像头常开做在场检测 | 功耗、发热、隐私、PSRAM 四条都不划算；v1 只靠"听见"和打卡 |
| 实时流式 ASR | 托管 ASR 不是流式端点；但"按切片直推"（§3.7）已足够，不必为流式改造 |
| 让 LLM 自由读原始转写做总结 | 会编故事（§6.3） |
| 把音频写 littlefs | 8.12 MiB = 4.4 分钟，且写坏模型直接变砖 |
| 设备直连云端 DB / 云端直连设备 :80 | 前者越权，后者过不了 NAT（`README.md` §4.5） |

---

## 11. 源码索引（本文引用的全部落点）

| 要找什么 | 路径:行 |
|---|---|
| 上行闸门（PTT）唯一判定 | `main/middleware/ai/xiaozhi_service.c:661-666` |
| AFE VAD 开关 | `xiaozhi_service.c:919-924` |
| VAD 回调（P0 已接出；原 `NULL`） | `xiaozhi_service.c:978`（`recorder_event_cb = xz_recorder_event_cb`） |
| VAD 事件枚举 | `managed_components/espressif__gmf_ai_audio/include/esp_gmf_afe.h:48-56` |
| AFE 事件回调契约 | `managed_components/jason-mao__av_processor/include/audio_processor.h:228-236` |
| 60 ms 帧 / 读块 | `xiaozhi_service.c:73-80` |
| AEC 关 / NS 标志实为 BSS | `xiaozhi_service.c:903-932` |
| 会话 60 s 空闲拆 | `xiaozhi_service.c:157,688-697` |
| 双麦 BSS / GSC 需量麦距 | `main/middleware/audio/mic_frontend.h:8-34,54-60` |
| PCM 回合内存账（10 s / 427 KB） | `main/middleware/ai/voice_service.h:18-27,45-50` |
| OPUS 编码器配置项 | `managed_components/jason-mao__av_processor/include/av_processor_type.h:52,74-78` |
| raw body HTTP POST 现成写法 | `main/middleware/ai/ai_camera.c:295-321`（`esp_http_client` + `set_post_field`） |
| 网关 raw body 接收 / 4 MB 硬闸 | `tools/diag/ws_gateway.py:901-926`、`:86` |
| 网关 ASR 转发（文件式端点） | `tools/diag/ws_gateway.py:304-319` |
| 动作队列深度 / 栈 | `main/middleware/ai/ai_action.c:61-71` |
| tools/list 作为唯一真值 | `ai_action.c:9-29` |
| 16 个动作表 | `ai_action.c:693-764` |
| 中文表达层雏形 | `ai_action.c:423-490` |
| caps 上报（12 KB 缓冲） | `ai_action.c:79-81,824-990` |
| MCP resources 服务端 API | `managed_components/espressif__mcp-c-sdk/include/esp_mcp_engine.h:97-106` |
| MCP resources 客户端 API | `…/esp_mcp_mgr.h:445-482` |
| 打卡流水 schema（append-only 原型） | `main/middleware/camera/punch_service.h:69-88,128-140` |
| 设备打卡只读口 | `main/middleware/net/web_server.c:1348` |
| codec 独占租借 | `main/middleware/audio/audio_service.h:132-143` |
| ASR 是 HTTP 转发（不在本机） | `tools/diag/ws_gateway.py:304-319` |
| 意图快路（设备别名优先） | `ws_gateway.py:580-629` |
| action 下发 + 回执 | `ws_gateway.py:632-656` |
| 主动推送 /push | `ws_gateway.py:928-950` |
| 上传 4 MB 天花板 | `ws_gateway.py:86` |

---

## 12. 待拍板（做 P1 之前必须先定）

| # | 决策点 | 选项 | 建议 |
|---|---|---|---|
| D1 | 监听面是否常驻录音 | 全天常开 / 白名单时段（08:00–23:00） / 仅家中 Wi-Fi | **白名单时段起步**，跑稳再放宽 |
| D2 | 切片编码 | **在线 PCM / 断网 OPUS** / 统一 OPUS | **在线 PCM、断网 OPUS**：在线每段 ≤30 s（960 KB）直推质量最好；断网才转码省 SD |
| D3 | 原始层落哪 | 云端 JSONL / SQLite / 直接进 AstrBot 记忆 | **云端 JSONL + SQLite 派生**，AstrBot 只读视图 |
| D4 | 设备资源的读取方式 | 设备主动推 / **云端下发 resource_read** | **按需拉取**（复用 action 回执语义） |
| D5 | 模型侧入口 | MCP client / **一条 `pet_memory_query` 工具** | **一条工具**，后续再评估 MCP client |
| D6 | 语音切片保留期 | 7 / 30 / 90 天 | **默认音频 30 天、raw 180 天**（已落地，可 env 调整；比 7 天更适合回看，比 90 天省空间） |
| D7 | 验证方式 | 只跑日志 / 加 `tools/diag` 脚本 | 先加一个 `verify_gate_loop.py`（照 `verify_pet_loop.py` 的写法） |

---

## 13. 实施状态与验收清单（v3）

> 这一节是"可交付验收"的口径：每一条都写了**改在哪、怎么证明它成立、出问题怎么退**。
> 设备两件事（编译、烧录）按既有约定由用户执行，本仓库只落代码与验收步骤。

### 13.1 设备端改动（全部可一行回退）

| # | 文件:行 | 改了什么 | 默认行为 |
|---|---|---|---|
| 1 | `main/middleware/ai/xiaozhi_service.c:826-843` | 新增 `xz_recorder_event_cb()`，只打 `GATE: vad_start/vad_end Nms` | 与改动前一致（只是多两行日志） |
| 2 | `xiaozhi_service.c:978` | `rcfg.recorder_event_cb = xz_recorder_event_cb`（原 `NULL`） | 同上 |
| 3 | `main/middleware/audio/mic_gate.c/.h` | 新建：监听态所有权、VAD→切片、让位/恢复、监督任务 | 当前 `MIC_GATE_ENABLE=1`（实机验收用）；置 0 回退到零变化 |
| 4 | `main/middleware/ai/slice_client.c/.h` | 新建：1 MB PSRAM 环形、300 ms 预滚、30 s 上限、HTTP 直推、SD 兜底与补传 | 未初始化时所有 API 安全空转 |
| 5 | `xiaozhi_service.c:853` | 会话 setup 前 `mic_gate_yield(300)` | 未启用时立即返回 true |
| 6 | `media_player_service.c:562` | 播放前 `mic_gate_yield(300)` + `mic_gate_block(5000)` | 未启用时无操作 |
| 7 | `rec_local_service.c:114` | 本地录音前 `mic_gate_yield(300)` + `mic_gate_block(10000)` | 未启用时无操作 |
| 8 | `main/app/main.c:915` | `mic_gate_init()`（在 `xiaozhi_service_init()` 之后） | 未启用时只打日志 |
| 9 | `main/CMakeLists.txt:7-8` | 登记 `mic_gate.c`、`slice_client.c` | — |

**为什么保留这个宏**：P1 动的是全工程最敏感的生命周期（codec 独占租约 + 会话状态机 + 内部 RAM）。
2026-11-15 已在实机上把 P0 的 VAD 日志对齐（"说一句 = 一对 vad_start/end"），再把宏置 1，
所以当前默认是"开机即监听"。宏仍是唯一回退开关，置 0 即回到旧路径。

### 13.2 云端改动（独立部署，停服务即回退）

| # | 位置 | 能力 | 自测证据 |
|---|---|---|---|
| 1 | `tools/diag/ws_gateway.py:972` | `POST /slice/<session>`：raw WAV/裸 PCM → ASR → `raw/<day>/events.jsonl` 追加；同 id 幂等；>4 MB 413 | `tools/memory/selftest_slice.py` 全 PASS |
| 2 | `ws_gateway.py:1046-1084` | `load_day_events()`：annotation 覆盖值合并（原始行不改） | 同上 |
| 3 | `ws_gateway.py:1089` | `POST /memory/query`：`day/person/search/commitments`，每条带 `built_from` | 同上 |
| 4 | `ws_gateway.py:1172` | `GET /memory/resources`：4 个资源契约 | 同上 |
| 5 | `ws_gateway.py:1178` | `POST /memory/resources/read`：`uri + params → contents[]` | 同上 |
| 6 | `tools/memory/nightly.py` | 补转写（annotation 追加）、LLM 抽取（失败退化规则）、统计、`derived/memory.db`、日报 | `selftest_nightly.py` 全 PASS |
| 7 | `tools/memory/query.py` | 与 `/memory/query` 同口径的 CLI 诊断 | 手工跑通（见 §13.3-C） |
| 8 | `tools/memory/pet-memory-nightly.{service,timer}`、`deploy_memory.sh` | 每天 03:05 跑昨天 | 部署脚本内含一次手动验收 |

**目录结构**（`PET_MEMORY_DIR` 可覆盖，默认 `/opt/pet_memory`）：

```
/opt/pet_memory/
├── raw/<YYYYMMDD>/events.jsonl     # append-only 原始层（speech + annotation）
├── audio/<YYYYMMDD>/<id>.wav       # 原始音频（保留期见 D6）
├── derived/memory.db               # 派生：people / commitments（可重建）
├── derived/<YYYYMMDD>.json         # 当天抽取结果
└── nightly/<YYYYMMDD>.report.json  # 日报（只含数字与出处）
```

### 13.3 验收步骤（照顺序执行）

**A. 设备基线（P0，先不开闸门）**

1. 编译烧录当前固件（`MIC_GATE_ENABLE` 保持 0）。
2. 对着设备说 3 句话，每次停顿 > 1 s；串口应出现 3 对
   `GATE: vad_start` / `GATE: vad_end <N>ms`；纯静音 1 分钟应接近 0 对。
3. 按 PTT 正常对话一轮：上行 `fed=...KB`、TTS 正常、`session released` 与改动前逐条一致。
4. 判据：**VAD 对数 ≈ 语句数**，且 PTT/TTS/内存日志无新增 WARN。

**B. 开启闸门（P1/P2/P4，第一轮只验"切片有没有"）**

> 2026-11-15 已按本节在 `COM9` 实机跑通：开机监听、VAD 触发、切片直推、SD 补传、
> 云端 raw/audio 落盘与 `/memory/query` 全部有证据；具体见 §13.8。

1. 把 `mic_gate.h` 的 `MIC_GATE_ENABLE` 改为 1，重新编译烧录（已完成）。
2. 空闲时说一句 2~3 s 的话，串口应出现：
   `MIC_GATE: vad_start -> slicing` → `SLICE: gate_end ...` → `SLICE: slice queued: evt_...`
3. 网关侧应出现 `POST /slice/<sid>` 200 与 `[sid] slice evt_... -> <文本>`；
   `/opt/pet_memory/raw/<今天>/events.jsonl` 多一行，`audio/<今天>/evt_*.wav` 能播放。
4. 冲突验收：
   - 闸门占用时按 PTT → `mic_gate: yielded in ...ms`，会话正常建立（首次多 100~300 ms）；
   - 闸门占用时进本地录音 → 录音正常，日志出现 `MIC_GATE: blocked for ...`；
   - 闸门占用时点歌 → 播放正常。
5. 断网兜底：拔掉路由器 / 停网关，再说两句 → `/sdcard/pet_memory/pending/` 出现 `evt_*~<ms>~<stamp>.wav`；
   恢复网络后 60 s 内应看到 `SLICE: pending evt_... retried`，文件被删除。
6. 判据：24 h 内 `INT largest` 不低于基线 80%；无 `audio busy` / `recorder_open failed`。

**C. 夜间轨（P5）**

```bash
# 服务器上（先部署网关，再部署记忆）
bash deploy_memory.sh
# 手动跑指定一天（验收用）
/opt/pet_gateway/.venv/bin/python /opt/pet_memory/bin/nightly.py --date 20261115
# 查承诺 / 查某天
/opt/pet_gateway/.venv/bin/python /opt/pet_memory/bin/query.py --commitments --date-from 2026-11-15
```

判据：同一批数据跑两次，`nightly/<day>.report.json` 除 `generated_at` 外**逐字段一致**；
`derived/memory.db` 的 `commitments.built_from` 非空。

**D. 查询与 resources（P6）**

```bash
curl -s http://127.0.0.1:8081/memory/resources
curl -s -X POST http://127.0.0.1:8081/memory/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"person","person":"张总","date_from":"2026-11-09","date_to":"2026-11-15"}'
curl -s -X POST http://127.0.0.1:8081/memory/resources/read \
  -H 'Content-Type: application/json' \
  -d '{"uri":"memory://commitments","params":{"date_from":"2026-11-09","date_to":"2026-11-15"}}'
```

判据：每条结果都有 `built_from`；无数据时返回空数组而不是编造；`memory://day` 不存在时 404。

### 13.4 已知偏差与未做（必须如实记录）

1. **收口没有额外 300~500 ms 挂尾**：切片在 AFE 的 `VAD_END` 处立即截断（预滚 300 ms 有），
   靠 VAD 自身的 hangover 兜住句尾；若实机发现尾字被切，再在 `gate_event_cb` 里加一个延时收口定时器。
2. **断网兜底存的是 WAV，不是 OPUS**：`slice_client.c` 的溢写与补传都走 WAV（30 s ≈ 960 KB）。
   文档 §8 取舍 2 的"断网 OPUS 24 kbps"是目标态；v1 先保证"不丢"，编码迁移列 P4.1。
3. **P6 的设备侧 caps `resources` 没加**：LLM 在 AstrBot，读记忆走云端 HTTP 契约即可；
   设备 caps 仍只报 tools（`ai_action.c` 未改），因此没有动 12 KB caps 缓冲的风险。
4. **D1/D2 仍未拍板，D6 已落地**：监听时段（当前全天可监听，开关控制）、断网编码
   （当前 WAV 兜底，OPUS 未迁）；音频/原始转写保留期已按默认 30/180 天落地，
   `pet-memory-retention.timer` 每天 03:30 执行，可通过环境变量调整。
5. **已编译、已烧录，并已本地提交**：2026-11-15 用 ESP-IDF v6.1 构建
   `build/display_audio_photo.bin`（最新 `0x785c50`，7,887,952 B），
   `idf.py -p COM9 -b 921600 app-flash` 整包写入 7,887,952 B，
   `Hash of data verified` / `Done`（`build/idf_app_flash_pttfix_full.log`）。
   本次整理后已做本地提交 `fe0e4c3`（未推送远端）。
6. **历史事件时区已修，raw 被一次性重写**：`slice_client.c` 修好 `%2B` 之前落盘的 106 行
   `ts` 是 `" ... 08:00"`；已用 `tools/memory/fix_ts.py` 原地修复，并留
   `/opt/pet_memory/raw/20261115/events.jsonl.bak_tsfix_20261115T080251Z` 备份。
   修复后 141 条事件全部是合法 `+08:00`，`/memory/query` 不再返回坏时间戳。
7. **ASR 质量仍是真实短板**：141 条切片中有一部分 `text` 为空或语义偏差（例如把术语听成
   近似词）。这取决于播放音量、切片边界和环境声，不能据此判定 SenseVoiceSmall 本身失败。
8. **PTT“麦克风被占用”已修复并实机跑通**：根因是 `xiaozhi_service_start()` 在入队前
   只检查 `audio_service_mic_available()`；gate 持有独占租约时该值为 false，UI 直接报
   “麦克风被占用”并返回，根本没走到 `session_setup()` 的 `mic_gate_yield()`。现已放行
   gate owner：`if (!audio_service_mic_available() && !mic_gate_is_owner())`，由
   `session_setup()` 完成交接。新固件串口已抓到
   `PET_SVC: ptt UP -> ESP_OK (130560 B / 4080 ms)`，说明 start 不再被 gate 提前拒绝。
   TTS 让位也已复测：`/push` 后 `MIC_GATE: yielded in 20ms`、会话建立并播放；
   但播放收尾出现 `play tts ERROR` 与负的 `req->play` 延迟，仍需单独排查。
   本地录音与 gate 的 `yielded/blocked` 仍未专项触发。
9. **受控断网没有做**：没有主动拔网/停网关；只观察到网关恢复后
   `SLICE: pending evt_... retried` 多条，说明"溢写 SD → 恢复补传"的兜底路径被真实触发过，
   但"拔网瞬间立即落盘"的时序仍未受控验证。

### 13.5 回滚

| 想退回 | 动作 |
|---|---|
| 只退监听态 | `MIC_GATE_ENABLE` 改回 0（一行） |
| 退 VAD 观察 | `xiaozhi_service.c:978` 改回 `NULL` |
| 退网关能力 | 停 `pet-memory-nightly.timer`；`/slice`、`/memory/*` 是新路由，删掉即回退（不影响 WS 主链路） |
| 退设备直推 | 闸门关闭后 `slice_client` 不会被调用；如需彻底移除，从 `CMakeLists.txt` 删两行 |

### 13.6 云端实机部署与 AstrBot 插件验收（2026-11-15）

> 这一节记录**已经发生在服务器上**的事实，不是设计目标。
> 环境：`root@120.76.230.17`，AstrBot v4.29.1（容器 `astrbot`），网关 `pet-gateway:8081`。

**A. 网关：合并而非覆盖**

云端 `/opt/pet_gateway/ws_gateway.py` 是旧版，没有 `/slice`、`/memory/*`，但有本地仓库没有的流式 TTS
（`tts_stream`、`_sse_pcm_chunks`、`StreamingResponse`、`CIMG_PIXEL_CAP` 等）。整文件覆盖会把这些改动回退，
因此实际走合并：

1. 备份 `ws_gateway.py.bak_memory_<YYYYMMDD_HHMMSS>`；
2. `tools/astrbot_plugin_pet_memory/merge_gateway_memory.py` 只把本地 S10 记忆段（285 行）插到云端
   「插件主动推送」标记前，不改其它函数；
3. 合并后 1651 行，`py_compile` 通过；保留 `tts_stream` 10 处、`_sse_pcm_chunks` 2 处、
   `tts_stream_endpoint` 1 处、`StreamingResponse` 2 处、`CIMG_PIXEL_CAP` 3 处；
4. `systemctl restart pet-gateway`，服务 `active`；`/health` 返回 `{"ok":true,...,"version":"0.8.1"}`；
   `GET /memory/resources` 返回四条资源；`POST /memory/query` 空数据返回 200。

> 注意：合并后云端行号与 §13.2 的本地行号不再一一对应，定位端点请按函数名/路由字符串。

**B. 夜间轨**

`pet-memory-nightly.timer` 已 `enable --now`，每天 03:05 跑昨天：

```
NEXT Mon 2026-11-16 03:05:00 CST  pet-memory-nightly.timer
```

手动跑 `20261114`：`[nightly] 20261114: events=0 speech=0.0min people=0 commitments=0 source=rule fixed=0`。
`/记忆日报 2026-11-14` 能正常渲染“事件 0 条 / 这天没有记录到事件”，不再 404。

**C. AstrBot 插件**

插件目录 `/opt/astrbot/data/plugins/astrbot_plugin_pet_memory/`，配置实体
`/opt/astrbot/data/config/astrbot_plugin_pet_memory_config.json`。实测：

| 项 | 结果 |
|---|---|
| 加载 | `/记忆` 返回“已启用 · 工具已注册（管理器可见） · 网关在线 v0.8.1” |
| 关闭 | `/记忆开关 off` → “工具未注册”；`/记忆` 显示“已关闭 · 工具未注册” |
| 重开 | `/记忆开关 on` → 恢复“已注册（管理器可见）” |
| 空数据 | 承诺 0 条、日报 0 事件、搜索/人物 0 条，均不报错不编造 |
| LLM 工具 | 自然语言触发 `pet_memory_query` 两次（`commitments` + `day`），返回 `items: []`，模型如实回答 |
| 每日推送 | 订阅测试会话并把时间设为 `13:25`，日志出现 `13:25:28 [pet_memory] 日报已推送到 webchat:FriendMessage:webchat!xiao1!plugin-push-test` |

推送验收后已复位：`daily_push_enabled=false`、`daily_push_time=08:30`、`daily_push_targets=[]`。

**D. 切片与测试数据**

用 1 s 静音 WAV 模拟设备上行：

```
POST /slice/selftest?id=evt_gw_selftest&dur_ms=1000&ts=2026-01-01T12:00:00+08:00
→ 200，raw/20260101/events.jsonl 与 audio/20260101/evt_gw_selftest.wav 落盘
→ /memory/query type=events 返回 1 条（text 为空，静音 ASR 无文本）
```

测试数据（`raw/20260101`、`audio/20260101`）已在验收后删除，未污染真实记忆目录。
另用样例 `nightly/20260102.report.json` 验证有数据渲染（人物：张总 / 承诺 1 条 / 话题：方案、周五），
验收后也已删除；当前 `nightly/` 只保留 `20261114.report.json`、`20261115.report.json` 两份空日报。

**E. 当前未闭环**

- `/memory/*` 无鉴权，当前只应开在同机/容器内网；公网暴露前需加鉴权。
- `/记忆开关` 未加管理员校验，任何能发指令的会话都能开关；多人场景需收紧。
- 设备侧 `mic_gate` / `slice_client` 已编译烧录，`/opt/pet_memory` 已有真实事件（16:02 141 条，16:06 174 条）与对应 WAV。
- 每日推送只做了“到点触发”实测，未做连续多日稳定性观察。
- 成长模块 G1–G3 已完成并验收，G4 默认关闭，详见 §13.7。

### 13.7 成长模块 G1–G3：每晚总结、人格注入、微信监督（2026-11-15）

> 这一节只记录已经落地并实测的部分。G4（写入人物关系网）只留开关，默认关闭。

**A. 定位：一个插件、两个模块**

`astrbot_plugin_pet_memory` 从 1.0 的“只读查询 + 日报渲染”扩成 1.1 的“查询 + 成长”：

| 模块 | 开关 | 职责 |
|---|---|---|
| `memory_query` | `enabled` | `/记忆*`、`pet_memory_query`；只读 `/memory/*`，不改 raw |
| `growth` | `growth_enabled` | 每晚读日报、写插件成长记忆、注入人格、微信推送 |

没有拆成两个插件：拆开后会出现“查询开着、成长关了”或配置各存一份的状态不一致。用一个插件、两个独立开关，WebUI 里仍是一张插件卡。

**B. 两层记忆：插件库与人格知识库分开**

| 层 | 载体 | 写什么 | 不写什么 |
|---|---|---|---|
| 插件成长记忆 | `plugin_data/astrbot_plugin_pet_memory/growth/<YYYYMMDD>.json` | 蒸馏事实、趋势、判断、建议、微信话、`built_from` | 原始转写、原始音频 |
| 人格知识库 | LivingMemory `livingmemory.db` / 人物关系网 | 稳定人物、关系、长期偏好 | 每日流水、未验证的推断 |

硬边界：

- 只读 `nightly/<day>.report.json`，不把 `raw/<day>/events.jsonl` 直接交给 LLM“总结一下”；
- 不直接 `INSERT livingmemory.db`；
- G4 若要写图库，走 LivingMemory 官方 memorize / `memory_engine.add_memory`，带 `source=pet_growth`、`built_from`；
- `growth_graph_write_enabled=false`，摘要里 `graph_write` 只写 `{enabled:false,status:"reserved"}`。

**C. 每晚链路**

```
21:15  pet-memory-evening.timer
       └─ nightly.py --date 当天 → nightly/<day>.report.json
21:30  growth 检查任务（30 s 轮询）
       ├─ _collect_facts：stats / people / commitments / topics + built_from
       ├─ _collect_trend：回看 growth_lookback_days 天的 reports/events/speech/active_days
       ├─ _collect_open_commitments：memory://commitments，过滤 done/closed/完成，最多 8 条
       ├─ _llm_reflection：persona + 纪律 → JSON（judgment/suggestion/message/facts_used）
       ├─ 数字校验：message 里的数字必须来自事实，否则退化规则兜底
       └─ 原子写 growth/<day>.json → 有订阅且非空 → context.send_message 微信文字
```

空日报写 `empty=true` 但不推送；已推送写 `push.sent_at`，不重复；失败 5 分钟后重试。

**D. 人格注入**

`@filter.on_llm_request()` 读取最近 3 天、非空摘要，压成只读事实块追加到 `req.system_prompt`，长度受 `growth_max_chars` 限制。注入的是事实和判断，不是原始句子。成功率日志：`[pet_memory] 已注入成长上下文（N 字）`。

**E. 指令与配置**

新增指令：`/成长`、`/成长摘要 [day]`、`/成长生成 [day]`、`/成长订阅`、`/成长退订`、`/成长推送 on|off|HH:MM`、`/成长开关 on|off`、`/成长测试`。

新增配置：`growth_enabled`、`growth_time`、`growth_push_enabled`、`growth_push_targets`、`growth_persona_id`、`growth_lookback_days`、`growth_max_chars`、`growth_graph_write_enabled`。

persona 来源：`/AstrBot/data/data_v4.db` 的 `personas` 表；`growth_persona_id` 留空时取 `system_prompt` 最长的一条。云端实测选中 `persona=333`，提示词约 3156 字；兜底文案为 `GROWTH_SYSTEM_FALLBACK`。

**F. 实机验收**

| # | 项 | 结果 |
|---|---|---|
| 1 | 插件加载 | 1.1.0；`pet_memory_query` 已注册；日志 `成长摘要检查任务已启动（时间 21:30，开关 开）` |
| 2 | 空日报 | `/成长生成 20261114` → “日报为空，已记录摘要但不推送” |
| 3 | 样例摘要 | `/成长生成 20261115` → LLM 输出判断/建议/43 字微信话，`reflection_source=llm`、`persona_id=333` |
| 4 | 数字校验 | 首次把 `12.5` 说成 `12` 被拒；允许整数部分后通过；事实外数字仍拒 |
| 5 | 人格注入 | 聊天“我最近怎么样”，模型 reasoning 引用“11-15成长记忆：3条事件，人物张总、小李，2条未兑现承诺” |
| 6 | 工具回归 | 修 `_tool_query` 兼容位置参数后，一次调用返回 2 条 open 承诺；不再报 `takes 2 positional arguments but 3 were given` |
| 7 | 微信推送 | `/成长测试` → 日志 `成长测试消息已推送到 weixin_personal_mgwh:...`；实际微信会话收到测试话 |
| 8 | 定时器 | `pet-memory-evening.timer` 每天 21:15 跑当天 nightly；原 03:05 昨天 nightly 保留 |
| 9 | 数据隔离 | `livingmemory.db` 未变；成长数据只在 `plugin_data/astrbot_plugin_pet_memory/growth/` |
| 10 | 清理 | 样例 `nightly/20261115.report.json` 与 `growth/20261115.json` 已删除，避免注入人格 |

**G. 已知边界**

1. G4 没有实际图写入，只有配置开关和 `reserved` 占位；摘要质量观察一段时间后再决定是否开。
2. `/成长订阅`、`/成长开关` 未加管理员校验；单人使用够，多人场景要收紧。
3. 成长摘要质量受夜间日报质量约束；日报抽不到人物/承诺时，评语退化为“今天没有记录到事件”，不编造。
4. 微信推送只验证了单次链路，未做连续多日稳定性观察；夜间 23:00–07:30 只发文字，不发语音。
5. 验收样例已清理；当前 `growth/` 只保留两份真实空日报摘要：`20261114.json`、`20261115.json`。

### 13.8 硬件闭环实机验收：P1–P4（2026-11-15）

> 这一节只记录**串口和服务器上真实发生的事实**。设备 `session=xz-s31-fc50`，
> 串口 `COM9` / 115200 8N1，STA IP `10.168.1.198`，网关 `120.76.230.17:8081`。

**A. 构建与烧录**

| 项 | 证据 |
|---|---|
| 构建 | ESP-IDF v6.1，最新 `build/idf_build_pttfix.log` 末尾 `Project build complete`；`build/display_audio_photo.bin` = `0x785c50`，7,887,952 B |
| 烧录 | `idf.py -p COM9 -b 921600 app-flash`；移走被半写污染的差分基线后强制整包，写入 7,887,952 B，`Hash of data verified`，`Done` |
| 日志 | `build/idf_app_flash_pttfix_full.log`；上一版时区修复为 `build/idf_app_flash_tsfix.log` |

**B. 启动：监听态稳定，无 watchdog**

```
I (4581) MIC_GATE: supervisor started
I (9013) MIC_GATE: listening: BSS layout=MM
I (9711) esp_netif_handlers: sta ip: 10.168.1.198
I (20661) WS: CONNECTED (ws://120.76.230.17:8081/ws/xz-s31-fc50)
```

同一份 `build/serial_com9_tsfix_boot.txt` 中 `task_wdt` 出现 **0** 次、`afe_feed` **0** 次。
对比修复前的 `build/serial_com9_gate1_boot.txt`：`task_wdt` 在 14777 ms / 24522 ms 连续触发，
CPU1 停在 `afe_feed`；根因是 `gate_rec_read_cb` 只 `return ESP_OK` 不读 PCM，
修复为 `s_mic` 保护 + `return mic_fe_read(data, size, s_mic)`。

**C. VAD → 切片 → 在线直推**

```
I (51741) SLICE: gate_start (preroll=300ms)
I (51741) MIC_GATE: vad_start -> slicing
I (52761) SLICE: slice queued: evt_20261115T155715_22 1320ms 42284B
I (52762) MIC_GATE: vad_end 1018ms -> slice
I (54136) SLICE: slice evt_20261115T155715_22 pushed (42284 B / 1320ms)
```

`build/serial_com9_vad_test.txt` 另有三段连续证据：`evt_20261115T153953_09`（1320 ms / 42284 B）、
`evt_20261115T153955_18`（1560 ms / 49964 B）、`evt_20261115T153958_09`（2040 ms / 65324 B），
均为 `queued` → `pushed`，无 watchdog。

**D. 云端落盘与 `/memory/query`**

| 项 | 修复后实测 |
|---|---|
| raw 事件 | `/opt/pet_memory/raw/20261115/events.jsonl` 141 行 |
| 音频 | `/opt/pet_memory/audio/20261115/*.wav` 141 个；`file` 抽查 `evt_20261115T160628_87.wav` = `RIFF ... WAVE audio, Microsoft PCM, 16 bit, mono 16000 Hz` |
| 时间戳 | 141 条全部合法 `+08:00`；first `2026-11-15T15:38:36+08:00`，last `2026-11-15T16:02:19+08:00` |
| 查询 | `POST /memory/query` `{"type":"day","date_from":"2026-11-15","date_to":"2026-11-15","limit":200}` → HTTP 200，`items=141`、`stats.events=141`、`speech_ms=268380` |
| ASR 样例 | `evt_20261115T155640_46`：`好，我们回到内函数里面。`；`evt_20261115T155620_49`：`特找到它的边名，然后你就可以。` |

**ASR 已知文本自测**（`POST /slice/selftest?id=evt_asr_verify&dur_ms=10000&ts=2026-01-01T12:00:00%2B08:00`，
HTTP 200）：预期 `这是一段用于验证语音转文字的测试音频，今天下午我们检查麦克风闸门。`，
实际 `这是一段用于验证语音转文字的测试音频。今天下午我们检查麦克风闸本。`。
主体正确，最后一个词“闸门”同音误成“闸本”；STT 链路可用，短词/同音仍是准确率边界。
测试目录 `raw/20260101`、`audio/20260101` 已删除，未污染真实记忆。

> 上表是 16:02 的快照。设备保持监听后数量继续增长，16:06 复查为 174 条事件 / 174 个 WAV；
> 新固件（PTT 修复版）复测后 16:30 `limit=200` 查询返回 `items=200`、`stats.events=200`，
> 最新事件 `evt_20261115T163044_60` 的 `ts` 为合法 `2026-11-15T16:30:44+08:00`。
> 验收看的是“每条事件都有 raw + audio + 可查询”，不是固定条数。

网关日志同步可见：

```
15:57:16 [xz-s31-fc50] slice evt_20261115T155715_22 42284 B 1320ms -> 在官方是里面的。
15:57:28 [xz-s31-fc50] slice evt_20261115T155640_46 42284 B 1320ms -> 好，我们回到内函数里面。
```

**E. SD 兜底补传**

串口实际出现多条：

```
I (65176) SLICE: pending evt_20261115T155620_49 retried
I (65843) SLICE: pending evt_20261115T155640_46 retried
I (66293) SLICE: pending evt_20261115T154558_80 retried
```

说明上传失败/短暂断连时切片确实溢写到 SD，网络恢复后自动补传。该路径是被真实触发的，
但不是"主动拔网"的受控测试，时序见 §13.4-9。

**F. 三个实机缺陷（都已修完并复测）**

1. **`gate_rec_read_cb` 空读 → `afe_feed` 占满 CPU1 → `task_wdt`**：
   修复 `mic_gate.c:89-97`，加 `s_mic == NULL` 保护并真正 `mic_fe_read`。
2. **`ts` 里的 `+` 被服务端解成空格**：`slice_client.c:167-183` 新增 `ts_enc[64]`，
   把 `+` 转成 `%2B`；修复前 raw 是 `"2026-11-15T15:39:53 08:00"`，
   修复后新事件是 `"2026-11-15T15:56:40+08:00"`；历史 106 行另用
   `tools/memory/fix_ts.py` 修复并备份。
3. **PTT 报“麦克风被占用”**：`xiaozhi_service_start()` 在 gate 持麦时提前拒绝，
   UI 没机会走到 `mic_gate_yield()`。修复为 gate owner 也允许入队，由 `session_setup()`
   交接；新固件串口 `PET_SVC: ptt UP -> ESP_OK (130560 B / 4080 ms)` 证明 PTT
   会话与上行已恢复。

**G. PTT 与 TTS 让位复测**

PTT：新固件下串口抓到 `PET_SVC: ptt UP -> ESP_OK (130560 B / 4080 ms)`，
`VOICE: pcm up: 130560 B pcm / 174080 B b64 / 4080 ms -> ESP_OK`，会话进入 `CHATTING`
并正常收到云端 TTS。说明 `xiaozhi_service_start()` 不再因 gate 持麦而提前拒绝。

TTS：会话空闲后 `POST /push` 返回 `{"delivered":true}`，串口同一次抓取到：

```
I (178853) PET_SVC: cloud audio queued, session starting: http://.../tts/9c75b468f61f.mp3
I (179140) MIC_GATE: yielded in 20ms
I (179141) AUDIO_SVC: ext EXCLUSIVE claimed (handles lent out)
I (179722) PET_SVC: flush pending audio: http://.../tts/9c75b468f61f.mp3
```

即 gate 在 20 ms 内交出 recorder，会话建立并播放推送音频。证据文件
`build/serial_com9_pttfix_active.txt`、`build/serial_com9_push_yield_retest.txt`。
TTS 播放结束日志出现 `play tts ERROR` 与负的 `req->play` 延迟，疑似播放器收尾统计
问题；音频已写入 4.70 s，但这一条仍需单独排查，不能写成“TTS 完全正常”。

**H. 本轮遗留**

- PTT 已修复并实机上行；TTS 与 gate 的交接也已验证（`yielded in 20ms`），
  但 TTS 播放收尾报 `play tts ERROR`，需单独排查。本地录音与 gate 的
  `yielded/blocked` 仍未专项触发，不能写成已验收。
- 没有做受控断网（拔网/停网关），只有真实补传日志。
- 部分切片 ASR 为空或偏差；已知文本自测主体正确，但“闸门”被听成“闸本”。
  未定位是音量、切片边界还是 STT 本身。
- 至少 200 条事件（16:30 `limit=200` 查询）是当天连续环境声/说话触发的结果，
  不代表“每天只会记这么多”；D1 监听时段、D6 保留期仍未拍板。

---

## 13.9 定位调整：桌面总结监听学习助手（2026-11-15）

### A. 为什么改

人物关系网需要稳定的多人身份、关系和长期一致性，数据门槛高、误判成本也高。
本轮把主线收窄为**单人桌面场景**：设备放在桌上，白天按 VAD 切片记录桌面前发生的话语，
晚上不再问“你今天成长了吗”，而是回答：

1. 今天做了什么（上午/下午/晚上时间线）；
2. 围绕哪 3–5 个主题；
3. 形成了哪些结论/决定；
4. 学到什么（新术语、新知识、待查问题）；
5. 还有什么没闭环；
6. 一句大肥鱼复盘 + 一个明天动作。

产物叫**今日桌面复盘卡**，由大肥鱼人格在 21:30 通过微信推送。人物关系网降为可选增强：
G4 保留开关，默认关闭；P7 从“主体画像”改为“桌面活动画像”，只看时间块、主题分布、
重复项目/学习内容。

### B. 边界（不因为定位收窄而放松）

- 原始音频不进 LLM；LLM 只读转写文本；
- 每条主题/时间线/结论/学习点/未闭环保留 `built_from` 事件 id，可回溯到原切片；
- 每日复盘仍独立放在插件 `growth/` 目录，不写入 AstrBot LivingMemory；
- 人格知识库仍以既有知识文档为准，关系网只作为可选增强，不参与默认生成；
- 不做“监督训话”，不做人物关系推断，不把环境声硬编成事件。

### C. 代码改动

`tools/memory/nightly.py`：

- `LLM_PROMPT` 改为“桌面复盘整理器”；
- 新增 `headline`、`themes`、`timeline`、`decisions`、`learning`、`open_loops`、
  `next_actions`，保留 `people`、`commitments`、`topics`；
- 新增 `_clean_items()` 收敛 LLM 列表字段；`run()` 对新增字段统一 `attach_built_from`；
- 报告写 `schema_version: 2`；无 LLM 时新增字段为空数组，标 `source="rule"`。

`tools/astrbot_plugin_pet_memory/main.py`：

- `_collect_facts()` 把新字段带进 facts；
- `_refresh_growth_cache()` 注入最近三天的 headline、主题、未闭环、明天动作；
- `_fallback_reflection()` / `_llm_reflection()` 输出 `headline`、`review`、`next_action`，
  人格提示改为“睡前桌面复盘，不是监督训话”；
- `_generate_digest()` 写 `headline`/`review`/`next_action`；
- `_digest_text()` 输出桌面复盘全文；新增 `_push_text()`，微信只发短复盘卡；
- `_growth_push_once()` 改用 `_push_text()`；
- 命令入口暂保留 `/成长...`（未改用户已有订阅），用户可见文案改为“桌面复盘”。

### D. 验收结果

用今天真实事件做非破坏性试跑（复制 raw 到 `/tmp`，不碰生产）：

```text
[nightly] 20261115: events=236 speech=7.2min themes=4 timeline=4 learning=5 open_loops=1 source=llm fixed=0
```

生产重新生成（先备份旧 report/derived）：

```text
[nightly] 20261115: events=245 speech=7.39min themes=5 timeline=4 learning=5 open_loops=1 source=llm fixed=0
```

生产报告字段已验证：`schema_version=2`，`headline/themes/timeline/decisions/learning/open_loops/next_actions`
均存在；`headline` 为“下午在桌前听/讲嵌入式工程课，从板卡与设备树配置、构建输出文件，
一路讲到可视化配置、依赖管理和红定义”。

部署：

- 云端 `/opt/pet_memory/bin/nightly.py` 已覆盖，旧版备份 `nightly.py.bak_review_20261115T164858`；
- 云端插件 `main.py` 已覆盖，旧版备份 `main.py.bak_review_20261115T164858`；
- AstrBot 重启后插件 1.1.0 加载成功，日志出现
  `桌面复盘检查任务已启动（时间 21:30，开关 开）`；
- 生产报告旧版备份 `20261115.report.json.bak_review_<ts>`、`derived/20261115.json.bak_review_<ts>`；
- 发现旧 `growth/20261115.json` 是 14:50 生成的 `empty=true` 版本（当时 raw 还是 0 事件），
  已移到 `growth/20261115.json.bak_review_<ts>`；`_growth_push_once()` 改为
  `_generate_digest(day, force=True)`，避免 21:30 直接复用旧空摘要而不推送。

### E. 设备当前状态（2026-11-15 16:52）

- `/api/system/status`：`session=IDLE`、`link.connected=true`、`uptime_s=1561`；
- 最新事件 `evt_20261115T164838_20`，`ts=2026-11-15T16:48:38+08:00`；
- 说明白天 gate 正在监听并持续上传切片；**不是持续推送**，而是白天积累、
  21:30 生成并推送一张复盘卡；
- PTT/TTS 会话占用麦克风时 gate 临时让位，会话结束回 IDLE 后恢复监听。

### F. 本轮未完成

- 21:30 的真实微信推送尚未到点触发，需看真实微信长度与格式；
- P7 桌面活动画像未开始，依赖至少一周复盘数据；
- `/成长...` 是否改名 `/复盘...` 未定；
- D1 监听时段、D2 断网编码（当前 WAV 不是 OPUS）未拍板；D6 保留期已按 30/180 天落地；
- 24 h 长稳观察、受控断网、本地录音让位仍未专项验证；
- TTS 收尾 `play tts ERROR` 与负延迟仍未排查。

---

## 13.10 存储与回收：在线直推、离线兜底、云端按天清理

### A. 音频到底在哪

| 阶段 | 位置 | 说明 |
|---|---|---|
| 在线 | PSRAM 环形缓冲（`SLICE_RING_BYTES`，1 MB） | 只做 300 ms 预滚 + 30 s 切片；**不落盘** |
| 上传成功 | 云端 `/opt/pet_memory/audio/<YYYYMMDD>/<evt_id>.wav` | 与 `raw/<YYYYMMDD>/events.jsonl` 同事件 id |
| 上传失败 / 断网 | 设备 SD `/sdcard/pet_memory/pending/<evt_id>~<dur_ms>~<stamp>.wav` | 队列深度只有 2；满了或失败就溢写 SD |
| 网络恢复 | 同上 pending 目录 | `slice_retry_task` 每 60 s 扫一次，最多补传 8 条 |
| 补传成功 | 删除 SD 文件 | `post_slice()` HTTP 200 → `unlink(path)` |

当前实现是**在线直推优先**，不是“先攒一天再传”。`slice_up_task` 的判定：
`ws_client_is_connected()` 为真且 `POST /slice` 返回 200 → 直接推；任何一步失败 →
`save_pending()` 落 SD。离线文件补传成功后立即删除，不做二次保留。

### B. 回收机制

云端新增 `tools/memory/retention.py`，只处理严格 8 位日期目录：

- 音频默认保留 **30 天**：删 `audio/<YYYYMMDD>/`；
- 云端分组音频同属音频产物，默认也保留 **30 天**：删 `audio_grouped/<YYYYMMDD>/`；
- 原始转写默认保留 **180 天**：删 `raw/<YYYYMMDD>/`；
- 保留 `nightly/*.report.json`、`derived/*.json`、`derived/memory.db` 里的统计/摘要；
- 默认 dry-run，`--apply` 才删；跳过符号链接和非法目录名；
- 保留期可通过 `PET_AUDIO_RETENTION_DAYS` / `PET_RAW_RETENTION_DAYS` 调整。

systemd：

- `pet-memory-retention.service`：`/opt/pet_gateway/.venv/bin/python /opt/pet_memory/bin/retention.py --apply`；
- `pet-memory-retention.timer`：每天 **03:30**，在 03:05 nightly 之后跑；
- 首次执行已清掉 `audio/19700101`、`raw/19700101` 这一对零时间戳残留（共 2 文件 / 41.6 KB）；
- 当前云端 `audio/` 约 16 MB、`raw/` 约 156 KB，只有 `20261115` 一天真实数据。

### C. 还没做好的部分

- 设备 SD 的 `pending/` **没有 TTL / 容量上限**：只在补传成功后删除；如果长期断网，
  SD 会一直涨，卡满时 `save_pending()` 会失败并丢切片。需要后续加“最多保留 N 天/最多 N GB”
  的端侧策略，或至少把 pending 计数/剩余空间暴露到 `/api/system/status`。
- 断网兜底仍是 WAV（30 s ≈ 960 KB），不是 OPUS；迁 OPUS 后才能把 SD 占用压约 10 倍。
- 没有做受控断网测试，只观察到真实 `pending ... retried`。
- 云端回收只删 raw/audio/audio_grouped，不删 `derived/memory.db`；这是有意保留统计，不是漏删。

---

## 13.11 ASR 现状：外部免费模型，不是本地推理

### A. 实际走的是哪条链路

网关 `stt_transcribe()` 不加载本地模型，而是把 WAV POST 到 AstrBot 已配置的
OpenAI 兼容 `/audio/transcriptions`：

```text
设备切片 -> /slice -> stt_transcribe() -> https://api.siliconflow.cn/v1/audio/transcriptions
model = FunAudioLLM/SenseVoiceSmall
pipeline = online
```

云端 `cmd_config.json` 里的 STT provider 就是：

```text
provider_type = speech_to_text
api_base      = https://api.siliconflow.cn/v1
model         = FunAudioLLM/SenseVoiceSmall
```

所以“云端转文本”目前= **外部 API**，不是服务器本地推理。

### B. 额度与调用量

- 硅基流动官方价格页（2026-11-15）把 `FunAudioLLM/SenseVoiceSmall` 标为**免费**：
  输入按 UTF-8 字节/千字符计，价格 0 元；不是按音频分钟数收费。
  <https://siliconflow.cn/pricing>
- 免费不等于零成本：仍然是外部请求，受网络、并发/限流、隐私边界影响。
- 当前实测（2026-11-15 17:12 左右）：**307 条事件 / 8.87 分钟语音 / 47 条空文本**；
  其中 **254 条切片短于 2 秒**，平均 1.73 秒。
- 一次切片一次 POST：今天 307 次请求只为 8.87 分钟语音。按这个碎片率，真实 1–2 小时
  讲话会变成 2000+ 次请求/天；钱不是问题，**请求数和延迟才是**。

### C. 本地转写可选方案

| 方案 | 是否本地 | 主要代价 | 当前服务器可行性 |
|---|---|---|---|
| 继续用 SiliconFlow SenseVoiceSmall | 否 | 外部依赖/隐私/限流 | 现成，免费，最省事 |
| FunASR + SenseVoiceSmall | 是 | 需装 PyTorch/ONNX，常驻内存 | 服务器 2 vCPU / 1.7 GB / 无 GPU，当前只剩约 710 MB，在线并发风险高 |
| sherpa-onnx SenseVoice int8 | 是 | 模型小、CPU 推理，精度略降 | 可做**夜间批处理**，不建议跟 AstrBot/gateway 同时常驻 |
| whisper.cpp tiny/base | 是 | 中文精度明显低于 SenseVoice | 内存低，但错字更多，不推荐做主链路 |
| 本地 PC 跑 ASR | 是 | 需要内网穿透/中继，PC 要常开 | 资源够，但架构复杂；适合作为隐私版备选 |

结论：**现在不建议在这台 1.7 GB 云服务器上把 SenseVoice 常驻成在线 ASR**，
会和 AstrBot、网关抢内存；如果要做本地化，优先“夜间批处理”或升级到 4 GB+ 内存，
或者把 ASR 放到本地 PC 侧。

### D. 更省请求的做法（不用换模型）

1. 把 `SLICE_MIN_MS` 从 1000 ms 提到 1500–1800 ms，减少 1.1–1.4 s 的碎片；
2. 相邻切片在 300–500 ms 内合并成 3–8 s 的一段再送 ASR；
3. 只对空文本/低置信切片做夜间补转写，在线不重复转；
4. 保留 `built_from` 和原始 WAV，随时可以用新模型重跑。

> 注意：用 PowerShell `Invoke-RestMethod` 直接看 `/memory/query` 会显示乱码，
> 这是客户端按 Latin-1 解码造成的；`curl`/raw JSONL 里是正常 UTF-8 中文。

---

## 13.12 A/B 合并：设备端 1.2s 合并窗口 + 云端 30-60s 分组转写

### A. 为什么不做“1 小时一条”

硅基流动转录接口允许“时长 ≤1 小时、文件 ≤50 MB”，但当前设备是
16 kHz / 16 bit / 单声道 PCM：

- 1 小时 WAV = `16000 × 2 × 3600 ≈ 115 MB`，**超过 50 MB**；
- 50 MB 上限只够约 26 分钟 WAV；
- 设备 PSRAM 环形缓冲 1 MB（约 32 s），单切片上限 30 s，**缓不住 1 小时**；
- 1 小时一条请求失败会丢整小时，且一条文本对应几百个事件，`built_from`/时间线粒度会变粗。

所以没有采用 1 小时单请求。实际采用两级合并：
**设备端先合并成 1-30 s 的完整语句 → 云端再合成 30-60 s 的语音组做一次 ASR。**

### B. 设备端 B：VAD_END 延迟 1200 ms 收口

`main/middleware/audio/mic_gate.c`：

- `GATE_MERGE_MS = 1200`；
- `VAD_END` 不立即 `slice_client_gate_end()`，只记 `s_vad_end_us`、置 `s_merge_pending`；
- 合并窗口内再次 `VAD_START` → `vad_start -> merge continue`，继续同一段；
- 窗口到点仍无新语音 → `merge window closed -> slice`，才真正收口；
- 单切片上限保持 `GATE_MAX_SLICE_MS = 30000`，不把 1 小时塞进 RAM；
- `mic_gate_yield()` 同时清掉合并状态，避免 PTT/本地录音交接时留下悬挂切片。

实机验证（2026-11-15 18:06）：

```text
MIC_GATE: vad_start -> slicing
MIC_GATE: vad_end 1293ms -> merge window 1200ms
SLICE: slice queued: evt_20261115T180621_40 2820ms 90284B
MIC_GATE: merge window closed -> slice
```

同一句话从旧策略的 1.3 s 变成 2.82 s 的完整切片；当天 18:04 的切片也是 2.46 s。
串口日志在 `build/serial_com9_merge_verify2.txt`。

**双句合并也已实机验证**（2026-11-15 18:19，PC 扬声器播放两段 TTS，中间精确 500 ms 静音）：

```text
MIC_GATE: vad_start -> slicing
MIC_GATE: vad_end 3071ms -> merge window 1200ms
MIC_GATE: vad_start -> merge continue
MIC_GATE: vad_end 1037ms -> merge window 1200ms
SLICE: slice queued: evt_20261115T181949_70 6660ms 213164B
MIC_GATE: merge window closed -> slice
```

第二句在 1.2 s 窗口内到达，`vad_start -> merge continue` 成立；两段并成 **6660 ms** 的切片。
日志在 `build/serial_com9_merge_loud.txt`，测试音频为 `build/merge_test_exact_loud.wav`。

### C. 云端 A：相邻切片合并成 30-60 s 语音组

新增 `tools/memory/group_audio.py`，已部署到 `/opt/pet_memory/bin/`：

```text
raw/<day>/events.jsonl + audio/<day>/*.wav
  -> 按 gap_ms=30000、max_s=60 分组
  -> audio_grouped/<day>/grp_*.wav
  -> derived/<day>.groups.json（ids / start_ts / end_ts / dur_ms / text / audio）
```

`nightly.py` 的 `run()` 在 `use_llm=True` 时先调用 `build_groups()`：

- 组文本作为 LLM 的输入行，替代逐片碎片文本；
- `attach_built_from(..., extra_events=groups)`：LLM 引用命中组文本时，
  把组内**所有原事件 id**写进 `built_from`；
- 组 ASR 失败时回退逐片 `transcribe_backlog()`，不阻塞夜间报告。

2026-11-15 真实数据验证：

| 指标 | 逐片 | A 分组后 |
|---|---|---|
| 原 speech 事件 | 374 | 374（raw 不改） |
| ASR 请求 | 374 | 27（有文本的组） |
| 语音时长 | 10.51 min | 9.96 min（组覆盖） |
| 主题 built_from | 单 event | 48 / 62 / 12 条原事件 |

生产报告 `/opt/pet_memory/nightly/20261115.report.json` 已在 17:50 用分组版重新生成：
`stats.group_count=27`、`group_asr=27`、`source=llm`、5 主题 / 5 时间线 / 5 学习点 / 2 未闭环。

### D. 当前参数与边界

- 设备端合并窗口：1200 ms；单切片 30 s；`SLICE_MIN_MS` 仍为 1000 ms（不丢短句）。
- 云端分组：`gap_ms=30000`、`max_s=60`；30 s 间隔把桌面连续活动并成一组，
  60 s 上限保证单文件小、失败半径可控。
- 组音频会保留 30 s 间隔内的静音尾巴；这是为了让 ASR 听到完整上下文，不是数据冗余。
- 组 ASR 是**夜间批处理**，不是在线低延迟文本；在线 `/memory/query` 仍是逐片文本。
- 组 ASR 默认 **3 路并发**（`group_audio._transcribe_groups` 的 semaphore），避免真实讲话
  到 1 小时时串行转写拖过 21:30。
- 1 小时单请求需要 OPUS + SD/流式上传，当前未做，也不建议作为在线主链路。

## 13.13 兜底与成本保护（2026-11-15）

> 目标：设备断网不把 SD 写爆；云端没有有效语音时不做 ASR/LLM；重复跑不重复烧 token。

### A. 设备端 SD 兜底

`main/middleware/ai/slice_client.c` 的 pending 目录：

| 保护 | 参数 | 行为 |
|---|---|---|
| 总量上限 | `PENDING_MAX_BYTES=64 MB` | 超过就删最旧的 pending WAV |
| 文件数上限 | `PENDING_MAX_FILES=64` | 超过就删最旧的 |
| 最低剩余空间 | `PENDING_MIN_FREE=8 MB` | 剩余不足 8 MB + 新文件大小就删最旧；仍不够则丢当前切片 |
| 过期 | `PENDING_TTL=7 天` | 超过 7 天的积压直接删，避免坏文件永久占卡 |
| 单次清理 | `PENDING_PRUNE_MAX=8` | 每次最多删 8 个，避免长时间阻塞音频任务 |

- `save_pending()` 先 `pending_prune(it->len)`，通过后才写；不通过就
  `SD pending full, slice ... dropped`，**不会把卡写满**。
- `slice_retry_task` 每 60 s 先清一次过期/超限，再补传；ACK 200 后仍立即 `unlink`。
- 没插卡 / FAT 信息拿不到时按“没空间”处理，宁丢当前切片也不写坏文件系统。

### B. 云端空数据短路

`tools/memory/nightly.py` 在进入分组/ASR/LLM 之前先看统计：

| 条件 | 结果 |
|---|---|
| `speech == 0` | `source=empty, reason=no_speech`，不分组、不 ASR、不 LLM |
| `speech_ms < PET_MIN_SPEECH_MS`（默认 5000） | `reason=speech_too_short`，同上 |
| `text_count == 0 && audio_count == 0` | `reason=no_text_no_audio`，同上 |
| 组已建好但所有组文本为空 | `reason=no_usable_text`，不再逐片重试 ASR |

空报告只写 `derived/<day>.json` 与 `nightly/<day>.report.json` 的标记，
`source=empty`、`empty=true`；插件 `_generate_digest()` 看到 `empty` 直接跳过
LLM 复盘和微信推送。

### C. 重复跑不重复烧 ASR

`group_audio.build_groups()` 会读 `derived/<day>.groups.json`：

- 组 id 或组员 `ids` 与上次一致、且上次 `asr_done=true`、没有 `error` → 直接复用文本；
- **空文本也复用**：如果上次 ASR 成功返回空（确实没人声/只有噪声），不再每晚重烧；
- `error` 的组才重试；只有新增/变化的组才进 `todo` 调 ASR；
- `nightly.py` 只有在 `built_groups` 完全为空（分组没建起来）时才回退
  `transcribe_backlog()`，组已建好但全空不会逐片重烧。

### D. 已做过的验证

- `nightly.py` 无 raw：`empty, skip ASR/LLM (no_speech)`；
- 有 speech、无文本无音频：`empty, skip ASR/LLM (no_text_no_audio)`；
- 已有 52 组全部 `asr_done` 后重跑：`reused=52, cached=52`，只合并不调 ASR；
- 插件 `_growth_push_once()` 本身已有 `empty` / 无文本 / 已推送 / 无目标四道跳过；
  空报告不会发微信。
- **设备端 SD 落盘/补传已实机验证**（2026-11-15 19:16，云端临时 DROP 设备到 8081，
  网关不重启）：
  - 测试音频触发两段 VAD：
    `evt_20261115T191637_88 3660ms 117164B` 与
    `evt_20261115T191642_75 3600ms 115244B`；
  - 两段都在断连期间 `SLICE: ... spilled to SD`；
  - 网关恢复后分别 `SLICE: pending ... retried`；
  - 云端 raw 收到这两条，设备 WS 重新在线。
  - 日志：`build/serial_com9_pending_verify.txt`。
- **64 MB / 64 条 / 8 MB / 7 天 TTL 的极限分支仍未触发**：要触发需要人为塞满
  SD 或临时改小阈值再烧一版；当前只验证了“失败落盘 + 恢复补传 + ACK 后删除”。
- 本次验证写入 2 条测试事件（`19:16:37/42`，文本“测试第二句合并测试。”）；
  尚未清理，正式报告前需要过滤或清掉。

## 13.14 REC 双麦采集状态可视化（2026-11-15）

- 新增 `main/middleware/audio/mic_monitor.c/.h`：256 点 FFT + Hann，
  32 个对数频带（100 Hz~8 kHz），L/R RMS→dBFS，快照供 LVGL 读取。
  FFT 为代码内软件 radix-2，频带 dB 归一化到 PCM 满量程 32768；
  频谱柱显示范围 -70~-35 dBFS，dB 文本仍为真实全量程。
- 采集点：监听态走 `mic_frontend.c` 的 `diag_scan_stereo()`；
  REC 本地录音走 `rec_local_service.c` 的 `esp_codec_dev_read()` 旁路。
- 线程：`mic_mon` 独立任务（4096 B 栈、优先级 3）；ingest 非阻塞，
  PSRAM 3 slot，满则丢帧，不反压录音/AFE。
- VAD：`mic_gate_vad_state()` 提供 `UNKNOWN/SILENCE/SPEECH/MERGE`；
  REC 录音态 gate 已 yield，UI 显示本地 RMS 代理 `VAD: LOCAL`，
  不冒充 AFE 内部阈值。
- UI：REC 页左下 L、右下 R 两组竖排频谱柱；中间 REC/STOP + 时长；
  右上 VAD pill。仅 REC tab 可见时启动 50ms timer 与 monitor。
- 边界：不改变 REC WAV 数据路径；AFE 内部 VAD 阈值不可见，UI 只显示状态，
  不伪造数值。

---

> **一句话收尾**：这套骨架不缺"能力"，缺的是"决定什么进来"和"进来之后记住什么"。
> 而这两块恰好全在软件里 —— **闸门接一根已经存在的 VAD 回调，记忆扩一条已经存在的打卡流水。**
> 硬件一分钱不用花，但所有权模型一定要先想清楚（P1 是全部风险所在）。
