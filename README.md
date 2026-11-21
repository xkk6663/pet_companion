# 桌宠伴侣 · pet_companion

<p>
  <img alt="SoC" src="https://img.shields.io/badge/SoC-ESP32--S31-2563EB?style=flat-square">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v6.1-06B6D4?style=flat-square">
  <img alt="FreeRTOS" src="https://img.shields.io/badge/FreeRTOS-SMP-0F172A?style=flat-square">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.6-F59E0B?style=flat-square">
  <img alt="ESP-DL" src="https://img.shields.io/badge/ESP--DL-端侧推理-10B981?style=flat-square">
  <img alt="AstrBot" src="https://img.shields.io/badge/云端大脑-AstrBot%20v4.29-8B5CF6?style=flat-square">
  <img alt="Gateway" src="https://img.shields.io/badge/网关-FastAPI%20%3A8081-EC4899?style=flat-square">
  <img alt="License" src="https://img.shields.io/badge/License-Apache--2.0-64748B?style=flat-square">
</p>

> 一台会说话、会看、会动手的桌面机器人。
> **ESP32-S31 负责身体**（感知 / 表达 / 执行），**云端负责智力**（听 / 想 / 说 / 记）。
> 两端只隔**一条 WebSocket 长连接** —— 设备永远主动外连，所以云端能穿透 NAT 精确指挥它。

<p>
  <img src="docs/功能截屏/状态栏显示.jpg" width="46%" alt="待机主屏：时钟、日期、广州天气、Wi-Fi 与 IP">
  <img src="docs/功能截屏/云端聊天界面展示.jpg" width="46%" alt="聊天页待命：大肥鱼问候 + 5 个快捷 chip + 按住说话">
</p>

<p align="center"><sub>左：待机主屏（时钟 / 天气 / 联网状态）　右：聊天页待命（问候语 + 快捷 chip + 按住说话）—— 全部是 800×480 屏实拍</sub></p>

本文件回答三个问题：**云端到底实现了什么、设备到底实现了什么、云端大肥鱼（AstrBot）是怎么接进这块 ESP32 的**。
更细的分层设计、时序与踩坑复盘见 [架构分析](docs/开发文档/架构分析.md)、[云端网关开发文档](docs/开发文档/S9_云端网关_开发文档.md)。

**目录** · [功能速览](#功能速览实拍) · [系统全景](#1-系统全景) · [云端做了什么](#2-云端服务器实现了什么) · [设备做了什么](#3-边缘设备实现了什么) ·
[大肥鱼怎么接进来的](#4-大肥鱼astrbot-是怎么接进-esp32-的) · [端云契约](#5-端云契约11-种帧) ·
[快速开始](#6-快速开始) · [仓库地图](#7-仓库地图) · [关键约束](#8-关键约束违反就炸) · [排障与文档](#9-排障与文档)

---

## 功能速览（实拍）

> 下面 10 张全是设备实拍（抓屏直出，非渲染图）。每张右下角常驻的 `FPS / CPU / 帧耗时` 是运行期 HUD ——
> 它顺带证明这些画面不是 PPT 画的：粒子屏保 6 FPS、人脸检测 11 FPS、纯 UI 页 26 FPS，都能对上。

**会说** — 云端 LLM 负责"想"，设备只负责"说"与"听"，一轮对话带全套工具说明书

| <img src="docs/功能截屏/聊天界面展示.jpg" width="420"> | <img src="docs/功能截屏/中文输入法展示.jpg" width="420"> |
|:--:|:--:|
| **多轮对话**：`你好` → 「哼，又"你好"。」→「今天想聊点什么？」。底部 5 个 chip 是 `chip=true` 的快捷工具，说不清就点 | **中文输入**：打拼音「你hao」，候选条即时出 好 / 号 / 灏 / 豪；英文直接发，不用切输入法 |

**会看** — 人脸检测 + 识别 + 手势三个模型都在 ESP32 上跑，画面与特征不出设备

| <img src="docs/功能截屏/人脸追踪识别模型端侧部署.jpg" width="420"> | <img src="docs/功能截屏/端侧手势识别模型部署.jpg" width="420"> |
|:--:|:--:|
| **人脸追踪 / 识别**：取景框里绿框跟脸走，`DET ON` 亮着，右侧 `PUNCH` 一键打卡（识别 324 ms/次，绿框照样跟手） | **手势识别**：竖拇指当场给出 `likes 99%`，`HAND` 模式实时推理 —— 云端往返一趟 200 ms 起，这类交互只能放端侧 |

**会听** — 双麦 + VAD：只有真说话了才切段上行，静默不上传

| <img src="docs/功能截屏/静默录音检测.jpg" width="420"> | <img src="docs/功能截屏/双阵列麦克风录音.jpg" width="420"> |
|:--:|:--:|
| **VAD 判定"在说话"**：`VAD: SPEECH` 亮起，左/右声道实时电平 −49 / −50 dB，双色柱状图就是门限依据 | **双麦录音**：`Idle` 时 `VAD: N/A`，两路电平条独立走；`L bottom-left / R bottom-right` 标的是两颗麦的物理方位 |

**会放** — 本地媒体全在 SD 卡上，断网也能用（云端只兜底曲库没命中的歌）

| <img src="docs/功能截屏/本地音乐播放.jpg" width="280"> | <img src="docs/功能截屏/本地录像和拍摄.jpg" width="280"> | <img src="docs/功能截屏/本地sd卡管理.jpg" width="280"> |
|:--:|:--:|:--:|
| **本地音乐**：三行滚动歌词（当前行高亮）、进度可拖、`OUT:AUTO` 与蓝牙 `BT:OFF` 一键切输出 | **录像 / 拍摄**：VIDEO 页全屏预览，右侧两个圆钮 = 拍照与开始录像，文件落 SD | **SD 卡管理**：FILES 页列 8 个目录（音乐 / 照片 / 壁纸 / 人脸库 / 考勤…），右侧 UP / REFRESH / OPEN / DELETE |

**会待机** — 闲下来是桌宠：主屏报时天气，久不碰进粒子屏保，上滑即回

| <img src="docs/功能截屏/屏幕保护.jpg" width="640"> |
|:--:|
| **粒子屏保**：800×480 全屏粒子（1.15 MB PSRAM，幂等预分配）+ `SWIPE UP TO ENTER` 上滑退出；限帧跑 6 FPS，是为了不跟主界面抢 CPU |

---

## 1. 系统全景

![系统全景：云端四层能力栈 + 设备六层栈，中间只隔一条设备主动外连的 WebSocket](docs/assets/svg/system-overview.svg)

> 上面是**智力**：云端四层能力栈 + 一个网关；下面是**身体**：设备 L0~L5 六层栈。
> 中间那条 WS 就是全部的连接 —— 矢量图源在 `docs/assets/svg/`，改图不必改文档正文。

一句话概括分工：**设备负责"在场"，云端负责"理解"**。设备端**不跑任何大模型**（连 ASR 都不跑），
唯一例外是人脸检测/识别两个小模型 —— 那是隐私、时延、离线可用性三者共同逼出来的。

---

## 2. 云端服务器实现了什么

云端不是"一个大模型"，而是**四层能力栈 + 一个必须存在的网关**。全部接口已实测在线
（`GET /health` 返回 `version 0.8.1`）。

### 2.1 四层能力栈

| 层 | 组件 | 实际职责 |
|---|---|---|
| **L4 内容** | `music_proxy`（Docker :8080） | 点歌音源代理：自建网易云 API + gdstudio / qijieya 两个兜底源；契约 `GET /v1/search · /v1/url · /v1/lyric · /v1/pic · /v1/download`。设备本地曲库未命中时由它兜底 |
| | `alist`（:5244） | 表情包、素材的检索后端 |
| **L3 智力** | LLM `deepseek-flash` | 对话、人格、决定"该调哪个工具" |
| | ASR `SenseVoiceSmall` | OpenAI 兼容 `/audio/transcriptions`，16k mono WAV |
| | TTS `qwen-audio-3.1-tts-flash` | 少女音，WAV 24 kHz；流式 WAV 需重写头（否则播放器读成 4 万秒） |
| | VL（多模态消息段） | 把设备上行的 JPEG 作为 image 段塞进 LLM = 看图回答 |
| | Embedding | 长期记忆、表情包语义检索 |
| **L2 编排** | AstrBot v4.29.1（容器 :6185） | 人格「大肥鱼」+ 插件体系：`pet_device`（工具注册）· `livingmemory`（记忆）· `omnidraw`（生图）· `music` · `proactive_chat`（主动关怀）… |
| **L1 接入** | `pet-gateway`（FastAPI :8081，systemd） | 设备唯一长连接、工具中继、媒体归一化、主动推送。`MemoryMax=256M` 防 OOM 拖垮宿主 |

### 2.2 网关对外只有 7 个入口

| 入口 | 方向 | 作用 |
|---|---|---|
| `WS /ws/{session_id}` | 设备 ↔ 云 | 唯一长连接，收发全部 JSON 帧 |
| `POST /upload/{session_id}` | 设备 → 云 | 上传原始 JPEG（≤4 MB）换 `attachment_id`，再走看图；**不进 `/static`**，只给云端看 |
| `POST /action` | 插件 → 设备 | 工具中继：把动作转成 WS `action` 帧并**同步等回执** |
| `GET /caps` | 插件 ← 设备 | 查设备自述的能力表（工具名 / 描述 / 参数 / 别名） |
| `POST /push` | 插件 → 设备 | 主动推送（"该睡觉了"）：`emotion → text →（可选）audio` |
| `GET /media/{name}` | 设备 ← 云 | AstrBot 产出的图片/语音出口（AstrBot 的 SSE 只给文件名，设备只会按 URL 取） |
| `GET /health` | 运维 | `{"ok":true,"tts":true,"stt":true,"sessions":…,"caps":…}` |

### 2.3 云端替设备扛下的三件事

1. **媒体归一化**：云端插件产出 GIF(500×500) / PNG(1024×1024)，设备的硬件 JPEG 解码器只吃
   baseline-DCT、≤1M 像素、≤1200KB ⇒ 网关 `normalize_image()` 用 Pillow 转 RGB、等比缩、重编码，
   设备端**零改动**。
2. **降级兜底**：TTS 挂了只掉语音、文字照发；AstrBot 重启时网关重试一次；模型回复超长时
   `tts_shorten()` 截到 200 字，**屏幕上仍是全文**；图片不合规就转码而不是丢弃。
3. **零新密钥复用语音能力**：网关启动时直读 `/opt/astrbot/data/cmd_config.json`，自动拿到
   AstrBot 已配好的 TTS/STT provider。换音色、换 ASR 模型只在 AstrBot 面板改一次，
   **网关与设备零感知**。

---

## 3. 边缘设备实现了什么

### 3.1 16 个可被云端调用的能力

设备把工具表**自己报给云端**（caps），因此"设备会什么"的唯一真相在固件里。

| 模块 | 工具（MCP 名） | 用户说法 |
|---|---|---|
| `camera` | `self.camera.capture` · `self.camera.explain` | "拍张照" / "看看这是什么" |
| `punch` | `self.punch.clock_in` | "打个卡"（人脸识别 + 存照 + 记考勤） |
| `music` | `self.music.play` · `stop` · `list` · `download` | "放首晴天"（本地曲库优先，未命中走云端音源） |
| `speaker` | `self.speaker.set_volume` · `get_volume` | "大点声"（相对调整，不编绝对值） |
| `light` | `self.light.set_power` · `set_brightness` · `set_rgb` · `get_state` | "灯调暖一点" |
| `device` | `self.device.reboot` · `self.wifi.get_status` | "重启一下" / "Wi-Fi 怎么样" |
| 本地 | `wallpaper`（MCP 引擎里没有，纯本机动作） | "换个壁纸" |

> 15 个来自 MCP 引擎 `tools/list`，`wallpaper` 由 `ai_action.c` 补报，合计 **16 个**。
> 屏幕上的快捷 chip 只取 `chip=true` 的 6 个 —— 800 px 排不下 16 个，台灯/Wi-Fi 本来就该用嘴说。

### 3.2 边缘端五级链路

`main/middleware/ai/` 是整条端云链路的落点，任何一次改动都能定位到其中一级：

| 级 | 文件 | 职责 | 关键约束 |
|---|---|---|---|
| ① 传输 | `ws_client.c` | 唯一长连接、1s→30s 指数退避、按 JSON 大括号配对切帧 | 不按 `\n` 也不按 fin 切（网关会连续多帧、TCP 还会合并） |
| ② 解码 | `event_bus.c` | 7 种下行帧分发：text / emotion / audio / image / action / system / error | 收包任务里**只入队不阻塞** |
| ③ 执行 | `ai_action.c` + 6 模块 | 动作队列 worker（栈 7168 B）：参数模板合并 → 直调或 `tools/call` → 回 `action_result` | 表里没有的动作**当场报错**，不空转一圈 |
| ④ 会话 | `xiaozhi_service.c` | 会话状态机 + codec 独占租借 + 双播放通道：TTS `pri=2` 抢占、音乐 `pri=1` 让位 | TTS 结束自动续播音乐，用户听不出被切断 |
| ⑤ 表达 | `ui/` | `pet_overlay` 桌宠浮窗 + 6 个 tab + 全 CJK bin 字体 | **只有 LVGL 线程能碰 LVGL**，其它任务只置 `volatile` 标志；显示/存储一律走 `platform/port_*`，不许 include `bsp/esp-bsp.h` |

### 3.3 端侧智能：为什么人脸必须留在本地

| 能力 | 模型 | 体量 | 耗时 | 为什么不上云 |
|---|---|---|---|---|
| 人脸检测 | `espdet_pico_224_face` | ~300 KB | < 100 ms | 取景框要实时画框，往返云端不可能 |
| 人脸识别 | MFN + MSR + MNP | 1.30 MB + 60 KB + 127 KB | **324 ms/次** | 人脸特征不出设备 = 隐私；断网也能打卡 |
| 手势识别 | hand_detect / hand_gesture | ~200 KB | 实时 | 同上 |

人脸库 `face_db` 落在 SD 卡：**双世代号 + CRC**，掉电不丢、写坏可回退上一代。

### 3.4 纯端侧、断网也能用的功能

| 功能 | 实现 |
|---|---|
| 本地音乐 | SD 卡 mp3 列表、同名 `.lrc` 三行歌词、进度拖动、上下曲、后台播放 |
| 蓝牙外放 | A2DP Source（SBC），连上音响后音乐改由蓝牙出声，可后台继续；controller **懒上电** |
| 录像 / 录音 | AVI 封装、ES8389 录音，全部落 SD |
| 文件管理 | FILES 页浏览 / 删除 / 播放；点 `.mp3` 自动切 MUSIC 起播 |
| 动态壁纸 | 本地壁纸池定时轮换，断电记忆；**屏保模式**（闲时压暗 + 轻点唤醒 + 上滑退出）叠一层 800×480 粒子特效，canvas 内存 1.15 MB PSRAM，幂等预分配不重复申请 |
| 网页管理端 | Wi-Fi APSTA 常开热点 + `:80` 单页，**25 个 API**（19 条静态路由 + 6 条运行时注册），手机直连改配置 / 看内存状态 / 抓屏幕 / 传素材 |
| 开机即用 | 上电直进主界面（屏保待机）；CAM 页建页后 100 ms 自动起预览，不用先点 START |

> 上表里能拍到的画面都在 [功能速览](#功能速览实拍)：本地音乐、录像 / 拍摄、FILES、粒子屏保。
> 蓝牙外放与网页管理端是"后台能力"，屏幕上看不出差别，所以没有对应图；真机外观（正面 / 侧面 / 主板）的照片在 `docs/hardware/`。

### 3.5 硬件与分区

| 项 | 规格 |
|---|---|
| SoC | ESP32-S31（RISC-V @ 240 MHz，双核 SMP） |
| 内存 | 16 MB Flash + 8 MB PSRAM（OCT @ 200 MHz） |
| 显示 | 800×480 RGB LCD，双帧缓冲 + LVGL，GT1151 电容触摸 |
| 摄像头 | OV3660（DVP 8-bit）：1280×720 JPEG / 240×240 RGB565 |
| 音频 | ES8389 codec；录音 16 kHz mono，播放 16 kHz stereo |
| 存储 | microSD（FATFS）：`/music` `/photos` `/wallpaper` `/face_db` `/attendance` |
| 联网 | Wi-Fi APSTA（STA 联网 + 常开热点） |

| 分区 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `ota_0` | `0x20000` | `0x7C0000`（7.75 MB） | 唯一 app 槽 |
| `storage` | `0x7E0000` | `0x820000`（8.12 MB） | littlefs：模型 / 字体 / 图标 / 素材 |

> 单 app 槽 ⇒ **OTA 实质失效**，回退手段就是 USB 全量烧录。改分区表后必须重烧 `storage.bin`。

### 3.6 工程结构：L0~L4 五层与分层护栏

`main/` 不是"按功能分的一排文件夹"，依赖是有方向的：**只许向内，不许反向**。

| 层 | 目录 | 允许依赖 | 现状 |
|---|---|---|---|
| L0 平台 | `platform/` | ESP-IDF / BSP / 驱动 | 唯一碰 `bsp/esp-bsp.h` 的地方：`board_map.h` 收屏几何与引脚，`port_display / port_storage / port_time / port_bus` 只做转发 |
| L1 驱动 | `periph/ · camera/ · audio/ · media/ · net/ · bluetooth/` | L0 | 相机起停、按键、SD 卡、codec 这些"直接拨硬件"的部分 |
| L2 服务 | 同目录里的 `*_service` | L0~L1 | 持有句柄，对外只暴露语义 API（是谁在调用一概不问） |
| L3 业务 | `ai/` | L0~L2 | 端云链路 + 6 个工具模块；`net/web_server` 不再直调业务，改走 `web_action` 注册表 |
| L4 表达 | `ui/` | 全栈（只读） | 唯一调用 LVGL 的层；BSP 依赖已清零（15 个文件 → 0） |

护栏脚本按"谁 include 谁"判层，6 条规则：**G1** 反向依赖、**G2** UI 直接 include BSP / `esp_lcd_*` / `driver/gpio.h`、**G3** 公共头暴露板级硬件类型、**G4** 中间层 include `lvgl.h`、**G5** 公共头里的 `extern` 可变全局、**G6** 模块环。

```powershell
python tools/layering/layer_guard.py                                        # 盘点：27 处存量全部列出
python tools/layering/layer_guard.py --baseline tools/layering/layering_baseline.json  # 只报新增（CI 用）
```

> 2026-11-20 解耦之前实测 **55 处跨层违规**（52 high / 3 medium）。本轮把 UI 的 15 处 BSP 依赖、`web_server` 的 5 处业务直调、打卡水印的 `lv_canvas` 上屏全部切开，
> 降到 **27 处存量**并登记成基线，**新增 0**，CI 已挂这条 step。剩下的按设备类抽驱动（合并 `drv_jpeg` 5→1、`drv_http` 7→1）再收口。
> 单片可回滚的例外写法：`// layer-guard:ignore <理由>`。

---

## 4. 大肥鱼（AstrBot）是怎么接进 ESP32 的

这是整套系统最容易被问"到底怎么连的"部分。结论先给：**设备不从 AstrBot 拿任何东西，
设备只连 pet-gateway；由网关去和 AstrBot 说 AstrBot 的语言。**

### 4.1 接入拓扑

![接入拓扑：设备 → pet-gateway → AstrBot](docs/assets/svg/gateway-topology.svg)

三段职责各一句：

- **AstrBot = 大脑**：人格、记忆、模型调用、决定"该调哪个工具"；
- **pet-gateway = 翻译 + 反向通道**：把设备的 JSON 变成 AstrBot 的 SSE 请求，把 AstrBot 的
  文件名变成设备能下载的 URL，把云端的"想动手"变成一条能穿过 NAT 的 `action` 帧；
- **ESP32 = 手脚 + 感官**：录音上行、执行动作、把真实结果回执上去。

### 4.2 五步接入

**① AstrBot 侧：先把"脑子"配好**

Docker 跑 AstrBot（`:6185`），在面板里配 LLM / TTS / STT / VLM / embedding provider，
装好「大肥鱼」人格。然后在 **设置 → API 密钥** 建一个 `abk_` 开头的密钥
（scopes：`chat` / `im` / `file`）。网关只认这把钥匙，不碰面板密码。

**② 网关侧：填三个字段就能起来**

```bash
# 本地仓库 → 服务器 /tmp/gw/（deploy 脚本会建 venv、装依赖、装 systemd、起服务）
scp cloud/gateway/ws_gateway.py cloud/gateway/pet-gateway.service \
    cloud/gateway/config.example.json root@<server>:/tmp/gw/
ssh root@<server> 'cd /tmp/gw && mv config.example.json config.json && bash deploy_gateway.sh'
```

`config.json` 里只有三样必须改（模板里的 key 是占位符，改完 `chmod 600`）：

```json
{
  "port": 8081,
  "astrbot_base": "http://127.0.0.1:6185",
  "astrbot_api_key": "abk_你的密钥",
  "session_prefix": "xz-s31-"
}
```

> 端口用 8081 而不是 8080：8080 已被 `music_proxy` 容器占用。
> 启动时网关会**自动直读** AstrBot 的 `cmd_config.json`，拿到已配好的 TTS/STT —— 不需要再申请语音密钥。

**③ 设备侧：填网关地址，其余自动**

- 地址：网页管理端（`:80`）填写，存 NVS `pet_cfg/ws_base`；也可编译期用
  `-DPET_GATEWAY_URL_DEFAULT=ws://<host>:8081/ws/` 覆盖默认值（见 `main/middleware/ai/ws_client.c`）；
- 会话 ID：固件读 Wi-Fi STA MAC 末 4 位，自动拼成 `xz-s31-<mac4>`（**与微信等其它渠道完全隔离**）；
- 连上后**第一件事是上行 `caps`**：遍历 MCP 引擎 `tools/list`，用 `ai_action.c` 的 `TOOLS[]`
  补齐文案（描述 / 标签 / 别名 / 参数名），再补一个纯本地动作 `wallpaper`，拼成一条 JSON 发上去。

**④ 插件侧：让模型"知道有手"**

AstrBot 插件 `astrbot_plugin_pet_device` 每 **45 s** 拉一次 `GET /caps`：

- 把每个动作注册成 LLM 工具 `pet_<name>`（参数结构与固件上报**同构，零转换**）；
- 按签名（name/desc/params/timeout 的 JSON 摘要）判断是否需要重注册，能力缩水就 `remove_func()` 下线；
- 设备还没上报 / 离线时**保留**现有注册并回落到 `_DEFAULT_TOOLS`，避免"打个卡"退化成空口答应。

于是 **固件加一个动作，只改固件** —— 网关和插件一行都不用动。

**⑤ 验证三处日志**

```
设备串口  AI_ACT: caps -> ESP_OK (xxxx B, N tools)
网关      [xz-s31-xxxx] caps: N tools [punch,capture,music,...] model=ESP32-S31-Korvo-1
插件      [pet_device] 能力表已同步：[...]
```

### 4.3 一句话进来之后：三条通道

| 通道 | 走法 | 成本 / 延迟 | 用途 |
|---|---|---|---|
| **关键词快速路**（默认） | 网关 `match_intent()` 遍历 caps 上报的 `aliases`（按长度降序，避免"放首"抢在"放首歌"前）→ 命中**直接下发 `action`**，不进 LLM | 0 token，一次动作 RTT | 打卡 / 拍照 / 点歌 / 换壁纸这类**有副作用**的动作 |
| **LLM 工具调用** | 未命中 → 文本进 AstrBot → 模型自己决定调 `pet_<name>` → 插件 `POST /action` → 网关 relay → 设备执行 → 回执 | 每轮带工具说明书 | 任意自然语言（"我今天到公司了，帮我记一下考勤"） |
| **主动推送** | AstrBot 插件（如 `proactive_chat`）`POST /push` → 设备 `emotion → text → audio` | 无需用户开口 | 桌宠"先开口"：该睡觉了、久坐提醒 |

**为什么打卡不让模型决定**：模型在工具失败时会"编一句成功"，用户以为打了卡、实际没打。
所以有副作用的动作走高确定性通道，**结论只认设备回执**。

### 4.4 一次语音点歌的完整时序

![语音点歌时序：PTT 上行 → 网关 ASR → AstrBot → 插件发 action → 设备执行 → 回执](docs/assets/svg/voice-song-sequence.svg)

两个细节容易被忽略，但都是线上踩出来的：

- 网关的 WS 收包必须 `asyncio.create_task()` 并发处理 —— 否则一边 `await` 回执、一边堵着收包循环，
  **每个动作都会超时**；
- 点歌成功后 10 s 内网关强制"只回文字不出 TTS"，否则那句"行，放《晴天》"会立刻顶掉音乐。

### 4.5 为什么中间必须夹一层网关

设备直连 AstrBot 看起来更短，但四件事没有网关就全部落空：

1. **NAT 穿透**：设备在路由器后面（`192.168.x.x`），云端插件**打不到**它的 `:80`。
   只有设备主动建立的那条 WS 是稳定反向通道，网关就是这条通道的持有者；
2. **协议收敛**：设备只说一种 JSON 方言；AstrBot 的 SSE 帧序、附件机制、provider 差异
   全在网关里消化（SSE 没有 `complete` 帧，只能以流关闭为准拼 `plain` 增量）；
3. **媒体归一化**：设备解码器只吃 baseline JPEG，云端产出什么格式都得先转；
4. **降级兜底**：TTS / ASR / AstrBot 任一环节挂掉，网关都能"掉一部分、留一部分"。

### 4.6 会话、鉴权与重连

| 项 | 约定 |
|---|---|
| 鉴权 | 网关 → AstrBot：`Authorization: Bearer abk_…`（chat scope）。设备侧不鉴权，靠不可猜的 session_id |
| 会话 | `session_id = xz-s31-<mac4>`，原样透传给 AstrBot；`enable_streaming` 必须传 **JSON true**（字符串会被拒） |
| 隔离 | 设备会话在 AstrBot 里**豁免模式切换钩子**（`WORKKIT_MODE_SWITCH` 不再为它重启），不影响其它渠道 |
| 重连 | 1 s→30 s 指数退避；断线丢当前回合，重连后网关补发 `command_table`（不做状态恢复） |
| 传输 | 当前 `ws://`；网关已支持 `wss://`，配好证书后设备改地址即可 |

---

## 5. 端云契约：11 种帧

设备与网关之间只有这些帧，全部单行 JSON。**这张表就是"边缘接入云端大脑"的完整协议。**

### 上行（设备 → 云）

| 帧 | 触发 | 作用 |
|---|---|---|
| `{"type":"caps","model":"…","tools":[…]}` | 连上 / 重连 | **能力自述**：我会干什么 |
| `{"type":"pcm","data":"<b64>","sample_rate":16000}` | 按键说完一句 | 语音上行（16k/mono/s16 裸 PCM） |
| `{"type":"text","text":"…"}` | 屏幕键盘 | 文字回合 |
| `{"type":"event","event":"action_result","id":N,"ok":1,"data":"…"}` | 动作执行完 | **回执**：让云端同步调用拿到真实结论 |
| `{"type":"event","event":"barge_in"}` | 用户打断 | 停播 + 取消排队 |
| `{"type":"image_req","attachment_id":"…"}` | 拍完照要讲解 | 触发视觉问答 |
| `POST /upload/{sid}`（HTTP） | 拍照后 | 原图字节送云端（≤4 MB） |

### 下行（云 → 设备）

| 帧 | 作用 | 设备落点 |
|---|---|---|
| `{"type":"text","text":"…","role":1}` | 回复文本（`role=0` 是 ASR 回显） | `event_bus` → 气泡 |
| `{"type":"emotion","name":"speaking"}` | 表情/嘴型：thinking / speaking / idle … | `pet_overlay` |
| `{"type":"audio","url":"/static/tts_xxx.wav"}` | TTS 音频 URL | `xiaozhi_service`（pri=2 通道） |
| `{"type":"image","url":"/media/xxx__n.jpg"}` | 表情包 / 配图 | `cloud_media_service`：下载 → 硬解 → 浮窗 |
| `{"type":"action","id":N,"name":"punch","args":{}}` | **云端指挥设备做事** | `ai_action_dispatch()` |
| `{"type":"system","cmd":"capture\|punch\|light\|set_volume\|reboot\|explain"}` | 系统级指令 | `event_bus::handle_system()` |
| `{"type":"error","code":"tts_failed\|asr_failed\|device_offline\|…"}` | 失败可见 | 气泡 + 日志 |

> **相对 URL**：云端只发 `/static/…`，设备用 `ws_client_get_http_base()` 把 `ws://host:port`
> 换成 `http://host:port` 再拼 —— 换服务器地址只改一处。
> 设备端**真的实现了 7 种下行帧**（`event_bus.c`）；11 种是上表上下行合计的口径。

---

## 6. 快速开始

### 6.1 设备端（ESP-IDF v6.1）

```powershell
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'
. C:\esp\v6.1\esp-idf\export.ps1

idf.py reconfigure
idf.py build
idf.py -p COM9 flash          # 全量烧录（含分区表 / storage）
idf.py -p COM9 storage-flash  # 只重烧素材镜像（改 littlefs_content/ 后）
```

提交前跑一次分层护栏（新增跨层 include 会直接失败）：

```powershell
python tools/layering/layer_guard.py --baseline tools/layering/layering_baseline.json
```

**本地密钥**（绝不入库，`tools/idf_env.local.ps1` 已被 `.gitignore` 忽略，构建时由
`main/CMakeLists.txt` 读环境变量注入宏）：

| 环境变量 | 用途 |
|---|---|
| `QWEATHER_KEY` / `QWEATHER_API_HOST` / `QWEATHER_LOC_ID` | 和风天气 |
| `MUSIC_BASE_URL` / `MUSIC_TOKEN` | 云端点歌音源 |

未注入时对应功能打印 `not configured` 并安静跳过。

### 6.2 云端（可复用现有 AstrBot）

```bash
# 网关：FastAPI + systemd
bash cloud/gateway/deploy_gateway.sh
curl -s http://127.0.0.1:8081/health          # {"ok":true,...,"version":"0.8.1"}

# 音源代理：Docker
cd cloud/music_proxy && docker compose up -d music-proxy
```

部署完把 `cloud/gateway/ws_gateway.py`、`config.example.json`（在服务器上改名为 `config.json`，
密钥是占位符）、`pet-gateway.service` 同步到 `/opt/pet_gateway/`。别忘了在云厂商安全组放行 `8081`。

> 五个云端组件的**完整部署顺序**（AstrBot → 网关 → 音源代理 → 记忆 → 插件）与"必须自备、绝不入库"的清单见
> [`cloud/README.md`](cloud/README.md)。

---

## 7. 仓库地图

```
main/
├── app/main.c                开机初始化编排（S1~S6 六个 stage，顺序 = 依赖顺序）
├── platform/                 L0 端口层：board_map.h（屏几何/引脚）+ port_display / port_storage / port_time / port_bus
├── middleware/
│   ├── ai/                   端云链路：ws_client / event_bus / ai_action / ai_action_endpoints
│   │                         + 6 个工具模块 + 点歌 / 曲库 / 云媒体
│   ├── audio/                audio_service · 本地录音 · 音乐播放 · 歌词 · 麦克风前端/闸门/监测
│   ├── bluetooth/            A2DP Source：bt_audio_service · bt_push_service
│   ├── camera/               相机 / 拍照 / 人脸检测识别 / 取景器 / JPEG / 打卡 / 打卡水印
│   ├── face_db/              人脸特征库（SD 持久化 + CRC）
│   ├── media/                AVI 封装解封装 · 文件管理 · 录像 · 壁纸
│   ├── mem/                  编译期静态内部内存池（关键短任务栈）
│   ├── net/                  Wi-Fi / SNTP / 网页管理端（web_server + web_action 注册表）
│   └── periph/               按键 / LED / SD / 文件系统 / 安全重启 / flash 占用闸 / 开机守护
├── ui/                       LVGL：splash · 6 个 tab · 浮层 · bin 字体 · 打卡水印上屏
├── 3rd/ringbuf/              环形缓冲
└── web/index.html            管理端单页（编译期 EMBED_FILES 内嵌）

components/                   esp_bt_audio（A2DP Source + gmf io_bt）
                              esp_xiaozhi（MCP 引擎 + 音频管线）
                              esp_littlefs · hand_detect · hand_gesture
littlefs_content/             烧进 storage 的模型与素材（不进固件）
docs/                         工程规范 / 开发文档 / 项目规划 / 汇报 / 硬件图
docs/assets/svg/              10 张架构图（系统全景 / 接入拓扑 / 分层模型 / 时序 / 媒体链路…）

cloud/                        云端侧全部代码（init/部署说明见 cloud/README.md）
├── gateway/                  网关 ws_gateway.py + config.example.json + systemd + 部署脚本
├── music_proxy/              点歌音源代理（Docker，:8080）
├── memory/                   S10 记忆工具：夜间抽取 / 保留期 / 查询 + timer
└── astrbot_plugin_pet_memory/ AstrBot 长期记忆插件

tools/diag/                   设备侧联调：serial_watch / verify_pet_loop / screen_shot
tools/layering/               分层护栏 layer_guard.py + 存量基线（CI 直接调用）
```

| 规模 | 数值 |
|---|---|
| 设备端代码 | **70,939 行 / 173 个 `.c`·`.h`·`.cpp`**（`main/` 全量，含 `ui/fonts` 生成的字体 C **20,228 行**；业务代码 50,251 行） |
| 云端代码 | 1,708 行 Python（网关 + 音源代理） |
| 分层护栏 | 6 条规则（G1~G6），存量 27 条已登记基线，新增违规 CI 直接失败 |

---

## 8. 关键约束（违反就炸）

| 约束 | 原因 |
|---|---|
| 内部 RAM 最大连续块只剩 **7 KB 级**（打卡会话实测 `free=7259 largest=7168`） | Wi-Fi DMA 独占约 51 KB 挪不走；任务栈 + RTOS 对象只能用内部 RAM |
| 判内存看 `largest_free_block`，不看 `free_size` | TLSF 不解决碎片，"总空闲够" ≠ "能分配" |
| 做 NVS / littlefs / flash 写的任务栈**必须内部 RAM** | IDF v6.1 擦写前断言 SP 在内部 DRAM，无 API 豁免 |
| 固定尺寸、必须内部 RAM 的短任务栈走**静态池** | `main/middleware/mem/static_pool.h`：编译期划好 12 KB `.bss`，**只增不减**；静态 TCB 不能自删后立刻复用 |
| WS 收包任务里禁止任何阻塞 IO | 阻塞一次就是成片丢包（心跳正常但响应时好时坏） |
| 只有 LVGL 线程能碰 LVGL | 其它任务只置 `volatile` 标志，由 `lv_timer` 刷新 |
| 层间依赖只能向内（L0→L4） | 反向依赖 = 改一处崩一片；UI / 业务不许 include `bsp/esp-bsp.h`，屏几何与引脚走 `main/platform/board_map.h`（护栏 G2 会报） |
| 注册表 / 槽表一律固定容量静态数组 | `web_action`（8 槽）、`flash_guard`（4 槽）都是启动前注册，运行期零 malloc、零加锁；满了就是设计上限，不做动态扩容 |
| 初始化顺序 S1~S6 冻结，不许重排 | 内存峰值对顺序极敏感（先腾地再加载），重排一次就可能 panic；顺序表在 `main/app/main.c` 顶部 |
| `lv_event_get_user_data(e)` ≠ `lv_obj_get_user_data(obj)` | 两个存储，混用会静默失效（行为不对但日志全对） |
| 云端下发的图片必须归一化 | 设备 JPEG 解码器只吃 baseline、≤1M 像素 |
| 64 位值打印用 `%llu` / `%lld` | `%zu` 只配 `size_t`，用错 varargs 槽错位 |

完整清单见 [11_遇到的问题.md](docs/工程规范/11_遇到的问题.md)（F1~F72）与
[内存管理规划.md](docs/开发文档/内存管理规划.md)。

---

## 9. 排障与文档

| 工具 | 用途 |
|---|---|
| `tools/diag/serial_watch.py` | 串口日志观察，桌宠闭环联调 |
| `tools/diag/verify_pet_loop.py` | 端到端验证：云端 push → WS → 设备日志 |
| `cloud/gateway/ws_gateway.py` | 云端网关（FastAPI），唯一入口 / 能力中继 / 媒体归一化 |
| `cloud/gateway/deploy_gateway.sh` | 网关服务器侧一键部署（`config.json` 已存在则不覆盖，防冲掉线上 key） |
| `cloud/music_proxy/` | 点歌音源代理（Docker） |
| `tools/layering/layer_guard.py` | 分层护栏：G1~G6 六条规则，`--baseline` 只报新增，`--update-baseline <path>` 刷新基线 |
| [`cloud/README.md`](cloud/README.md) | 云端四个子目录的部署顺序 + **需要自备但绝不入库**的密钥清单 |

**排障口诀**：先看 `[GW]` 日志定位"云端没发 / 设备没收到"，再看设备 `EVT:` 日志定位
"收到没处理"，最后看 `[pet_device]` 判断"模型有没有调工具"。三段都用逐帧日志，
任何一环断了都能一眼分清。

**文档索引**

- [架构分析](docs/开发文档/架构分析.md) —— 端云分层、回合时序、caps 协议、媒体协同
- [工程分层架构](docs/工程规范/01_工程分层架构.md) —— L0~L4 五层模型、六条护栏规则、允许的例外
- [分层解耦评估与优化方案（2026-11-20）](docs/项目规划/分层解耦评估与优化方案_2026-11-20.md) —— 审计结论、P0~P4 实施记录、上机崩溃复盘
- [云端网关开发文档](docs/开发文档/S9_云端网关_开发文档.md) —— 接口契约、AstrBot 对接硬约束、插件开发
- [通信接口规范 ICD](docs/开发文档/通信接口规范_ICD.md) —— 帧格式与字段级定义
- [S10 陪伴体三层扩展](docs/开发文档/S10_陪伴体三层扩展_闸门_记忆与MCP资源.md) —— 闸门（VAD 事件切片）、记忆（append-only 事件流）与 MCP resources：P0/P3/P5/P6 已实施，P1/P2/P4 默认关闭待实机验收（工具见 [cloud/memory](cloud/memory/README.md)）
- [工程规范](docs/工程规范/) —— 分层 / 命名 / 注释 / 代码风格 / 问题复盘
- [开发文档](docs/开发文档/) —— 蓝牙 A2DP 移植、内存优化、点歌、人脸、AI 浮窗
- [项目规划](docs/项目规划/) —— 需求文档、todolist、改动记录（§ 编号可追溯）
- [技术汇报](docs/汇报/桌宠伴侣技术汇报/) —— 16 页幻灯片 + 设计说明（DESIGN.md）

---

## 10. 许可与致谢

- vendor 组件版权归 Espressif Systems（Apache-2.0）；`esp_littlefs`、`esp_xiaozhi` 为第三方实现
- 云端大脑基于 [AstrBot](https://github.com/AstrBotDevs/AstrBot)（开源 LLM 机器人框架）
- 端侧人脸模型：ESP-DL 官方 `espdet_pico_224_face` / MFN(MobileFaceNet) / MSR / MNP
