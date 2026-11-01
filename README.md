# 桌宠伴侣 · pet_companion

<p>
  <img alt="SoC" src="https://img.shields.io/badge/SoC-ESP32--S31-2563EB?style=flat-square">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v6.1-06B6D4?style=flat-square">
  <img alt="FreeRTOS" src="https://img.shields.io/badge/FreeRTOS-SMP-0F172A?style=flat-square">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.6-F59E0B?style=flat-square">
  <img alt="ESP-DL" src="https://img.shields.io/badge/ESP--DL-端侧推理-10B981?style=flat-square">
  <img alt="License" src="https://img.shields.io/badge/License-Apache--2.0-64748B?style=flat-square">
</p>

> 一台会说话、会看、会动手的桌面机器人。
> ESP32-S31 负责**身体**（感知 / 表达 / 执行），云端负责**智力**（听 / 想 / 说），
> 两端用一条 WebSocket 长连接 + 能力自述协议连接（设备始终主动外连，可穿透 NAT）。

<p>
  <img src="docs/hardware/正面.jpg" width="46%" alt="真机正面：800×480 屏亮屏">
  <img src="docs/hardware/俯视45度.jpg" width="46%" alt="真机俯视 45 度">
</p>

**本文是上手与构建入口。** 端云分层、一次完整回合时序、caps 协议、媒体协同、
详细目录解析与踩坑复盘见 [架构分析](docs/开发文档/架构分析.md)。

**目录** · [能力](#1-能做什么) · [硬件与分区](#2-硬件与分区) · [代码结构](#3-代码结构) ·
[音频链路](#4-音频链路) · [构建与烧录](#5-构建与烧录) · [关键约束](#6-关键约束违反就炸) ·
[排障与文档](#7-排障与文档)

---

## 1. 能做什么

| 能力 | 用户视角 | 端云分工 |
|---|---|---|
| 语音对话 | 按键说话 → 屏幕出字 + 喇叭出声，可随时打断 | 端：录音 / VAD / 上行；云：ASR + LLM + TTS |
| 设备工具 | "打个卡""拍张照""放首晴天""灯调暖一点""换个壁纸" | 端：执行；云：选择工具 + 组织语言 |
| 看图讲解 | 拍照上传 → 语音回答 | 端：拍照上传；云：视觉模型 |
| 人脸打卡 | 走到设备前打卡，认出是谁后盖章 | **端侧推理**，不联网也认人 |
| 表情包浮窗 | 聊天中收到图片，浮窗显示 | 云：产出；端：解码上屏 |
| 点歌 | 云端搜歌 → 喇叭播放 → 同时存进 SD 曲库 | 云：音源代理；端：播放 + 落盘 + 歌词 |
| 本地音乐 | SD 卡 mp3 列表、同名 lrc 三行歌词、进度拖动、上下曲 | 纯端侧 |
| 蓝牙外放 | 连蓝牙音响/耳机后，音乐改由 A2DP 输出，可后台继续播 | 纯端侧（A2DP Source） |
| 本地功能 | 文件管理 / 录像 / 录音 / 蓝牙音箱 / 网页管理端 / 动态壁纸 | 纯端侧，离线可用 |

主界面 6 个 tab：`CAM · VIDEO · FILES · MUSIC · REC · SYS`。
在 FILES 点 `.mp3` 会自动切到 MUSIC 起播并加载同名 `.lrc`（没有则字幕显示"无"）。

---

## 2. 硬件与分区

| 项 | 规格 |
|---|---|
| SoC | ESP32-S31（RISC-V @ 240 MHz，双核） |
| 内存 | 16 MB Flash + 8 MB PSRAM（OCT @ 200 MHz） |
| 显示 | 800×480 RGB LCD，双帧缓冲 + LVGL，GT1151 电容触摸 |
| 摄像头 | OV3660（DVP 8-bit）：1280×720 JPEG / 240×240 RGB565 |
| 音频 | ES8389 codec；录音 16 kHz mono，播放 16 kHz stereo |
| 存储 | microSD（FATFS）：`/music` `/photos` `/wallpaper` `/face_db` `/attendance` |
| 联网 | Wi-Fi APSTA（STA 联网 + 常开热点，热点供网页管理端） |

分区（`partitions.csv` 是唯一真源）：

| 分区 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `ota_0` | `0x20000` | `0x7C0000`（7.75 MB） | 唯一 app 槽 |
| `storage` | `0x7E0000` | `0x820000`（8.12 MB） | littlefs：模型 / 字体 / 图标 / 素材 |

> 单 app 槽 ⇒ **OTA 实质失效**，回退手段就是 USB 全量烧录。改分区表后必须重烧 `storage.bin`。

---

## 3. 代码结构

```
main/
├── app/main.c                开机初始化编排（服务启动顺序即依赖顺序）
├── middleware/
│   ├── ai/                   端云链路：ws_client / event_bus / ai_action
│   │                          + 灯 / 相机 / 打卡 / 音乐 / 音箱 / 动作 / 云媒体
│   ├── audio/                audio_service · 本地录音 · 音乐播放服务 · 歌词
│   ├── bluetooth/            A2DP Source：bt_audio_service · bt_push_service
│   ├── camera/               相机 / 拍照 / 人脸检测识别 / 取景器 / JPEG / 打卡
│   ├── face_db/              人脸特征库（SD 持久化 + CRC）
│   ├── media/                AVI 封装解封装 · 文件管理 · 录像 · 壁纸
│   ├── mem/                  编译期静态内部内存池（关键短任务栈）
│   ├── net/                  Wi-Fi / SNTP / 网页管理端（:80）
│   └── periph/               按键 / LED / SD / 文件系统 / 安全重启
├── ui/                       LVGL：splash · 6 个 tab · 浮层 · bin 字体
├── 3rd/ringbuf/              环形缓冲
└── web/index.html            管理端单页（编译期 EMBED_FILES 内嵌）

components/                   esp_bt_audio（A2DP Source 底座 + gmf io_bt）
                              esp_xiaozhi（MCP 引擎 + 音频管线）
                              esp_littlefs · hand_detect · hand_gesture_recognition
littlefs_content/             烧进 storage 的模型与素材（不进固件）
docs/                         工程规范 / 开发文档 / 项目规划 / 汇报 / 硬件图
tools/diag/                   云端网关 ws_gateway.py + 联调脚本
tools/music_proxy/            点歌音源代理（Docker）
```

代码量：`main/` 约 5.6 万行（含约 2 万行生成字模），业务逻辑主要集中在
`middleware/` 与 `ui/`。

---

## 4. 音频链路

**本地播放（LOCAL）**

```
SD mp3 → esp_player（extractor + audio_dec）
       → esp_audio_render（ch/bit/rate 转换 + ALC）
       → audio_service ext 流式占用 → ES8389
```

**蓝牙外放（A2DP Source）**

```
SD mp3 → io_file → aud_dec → aud_asrc → aud_alc → aud_enc(SBC) → io_bt → 蓝牙音响
```

- 只申请 **A2DP Source** 角色（不启用 Sink / AVRCP / HFP），SBC 编码。
- 进度与歌词由本机墙钟 + MP3 时长推算（耳机不回传时间轴）。
- 蓝牙开关在 **SYS > Bluetooth** 与 **MUSIC** 页都有，两页状态实时同步；
  连接请求有 5 s 节流，一次没连上请等协议栈内部重连，不要连点。
- 拖动进度条：按「毫秒 → 字节」换算并对齐 MP3 帧头，再 `esp_gmf_io_seek`；
  暂停时拖动会记下目标，恢复时补做。
- 上下曲：链路已 STARTED 时不再调 `media_start`（组件要求 media_state==IDLE），
  改为在常驻 pipeline 上换 uri 重跑。
- **后台播放**：切 tab、进大页面都不会停歌；返回 MUSIC 页会恢复曲名 / 歌词 / 进度。
  小智语音会话与本地录音起播前会反向抢占（同一时刻只有一个音频出口）。
- **懒上电**：蓝牙 controller 默认不开机，首次真正要用蓝牙时才上电
  （省下开机时段的内部 RAM 峰值与 Core0 抢占）。

---

## 5. 构建与烧录

ESP-IDF v6.1（本机 IDF 路径 `C:\esp\v6.1\esp-idf`）：

```powershell
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'
. C:\esp\v6.1\esp-idf\export.ps1

idf.py reconfigure
idf.py build
idf.py -p COM9 flash          # 全量烧录（含分区表 / storage）
idf.py -p COM9 storage-flash  # 只重烧素材镜像（改 littlefs_content 后）
```

**本地密钥**（绝不入库，`tools/idf_env.local.ps1` 已被 `.gitignore` 忽略，
构建时由 `main/CMakeLists.txt` 读环境变量注入宏）：

| 环境变量 | 用途 |
|---|---|
| `QWEATHER_KEY` / `QWEATHER_API_HOST` / `QWEATHER_LOC_ID` | 和风天气 |
| `MUSIC_BASE_URL` / `MUSIC_TOKEN` | 云端点歌音源 |

未注入时对应功能打印 `not configured` 并安静跳过。

---

## 6. 关键约束（违反就炸）

| 约束 | 原因 |
|---|---|
| 内部 RAM 余量很小（约 4 KB 级） | Wi-Fi DMA 独占约 51 KB 挪不走；任务栈 + RTOS 对象只能用内部 RAM |
| 做 NVS / littlefs / flash 写的任务栈**必须内部 RAM** | IDF v6.1 擦写前断言 SP 在内部 DRAM，无豁免 |
| 只读 SD / 纯计算任务可用 PSRAM 栈 | `xTaskCreatePinnedToCoreWithCaps(... MALLOC_CAP_SPIRAM)` |
| 判内存看 `largest_free_block`，不看 `free_size` | TLSF 不解决碎片 |
| 固定尺寸、必须内部 RAM 的短任务栈走静态池 | `main/middleware/mem/static_pool.h`：编译期划好，**只增不减**，建一次常驻、用通知交工单（静态 TCB **不能自删后立刻复用**） |
| WS 收包任务里禁止阻塞 IO | 阻塞一次就是成片丢包 |
| 只有 LVGL 线程能碰 LVGL | 其它任务只置 `volatile` 标志，由 `lv_timer` 刷新 |
| 64 位值打印用 `%llu` / `%lld` | `%zu` 只配 `size_t`，用错 varargs 槽错位 |

更多条目（F1~F72 复盘）见 [11_遇到的问题.md](docs/工程规范/11_遇到的问题.md) 与
[内存管理规划.md](docs/开发文档/内存管理规划.md)。

---

## 7. 排障与文档

| 工具 | 用途 |
|---|---|
| `tools/diag/serial_watch.py` | 串口日志观察，桌宠闭环联调 |
| `tools/diag/verify_pet_loop.py` | 端到端验证：云端 push → WS → 设备日志 |
| `tools/diag/ws_gateway.py` | 云端网关（FastAPI），唯一入口 / 能力中继 / 媒体归一化 |
| `tools/diag/deploy_gateway.sh` | 网关服务器侧一键部署 |
| `tools/music_proxy/` | 点歌音源代理（Docker） |

排障口诀：先看 `[GW]` 日志定位"云端没发 / 设备没收到"，再看设备 `EVT:` 日志定位"收到没处理"。

文档索引：

- [架构分析](docs/开发文档/架构分析.md) —— 端云分层、回合时序、caps 协议、媒体协同
- [工程规范](docs/工程规范/) —— 分层 / 命名 / 注释 / 代码风格 / 问题复盘
- [开发文档](docs/开发文档/) —— 蓝牙 A2DP 移植、内存优化、点歌、人脸、AI 浮窗等方案
- [项目规划](docs/项目规划/) —— 需求文档、todolist、改动记录（§ 编号可追溯）
- [技术汇报](docs/汇报/桌宠伴侣技术汇报/) —— 16 页幻灯片 + 设计说明（DESIGN.md）

---

## 8. 许可与致谢

- vendor 组件版权归 Espressif Systems（Apache-2.0）；`esp_littlefs`、`esp_xiaozhi` 为第三方实现
- 云端大脑基于 [AstrBot](https://github.com/AstrBotDevs/AstrBot)
- 端侧人脸模型：ESP-DL 官方 `espdet_pico_224_face` / MFN(MobileFaceNet) / MSR / MNP
