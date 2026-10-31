# S9 桌宠伴侣 · pet_companion

**跑在 ESP32-S31-Korvo-1 上的 AI 桌宠**：一直在线的语音伴侣（对话 / 表情包 / 打卡 / 点歌 / 控灯 / 拍照），
云端大模型出脑子，设备端出身体 —— 单条 WebSocket 长连接 + MCP 工具协议打通两端。

> 基线：ESP-IDF v6.1 · LVGL · esp_xiaozhi · esp-dl · OV3660 + 4.3" 800×480 触摸屏

---

## 整体架构

```
麦克风 ─ 语音上行(PCM/opus) ──▶ ┌──────────────┐      ┌─────────────┐
                               │ 云端网关      │ ───▶ │ AstrBot LLM │
喇叭 ◀ TTS/表情包/动作 ──────── │ ws_gateway   │      │  (DeepSeek) │
                               │ :8081        │ ◀─── └─────────────┘
                               └──────────────┘
设备端：ws_client → event_bus → ai_action(16 个 MCP 工具) → 各服务
        xiaozhi_service：TTS 播放(pri=2) / 点歌播放(pri=1) 双通道
```

- **设备端**（`main/`）：语音上行、事件分发、工具执行、TTS/音乐双播放通道、UI
- **云端网关**（`tools/diag/ws_gateway.py`）：设备 WS 接入、意图匹配、调用 AstrBot、
  图片归一化（GIF/PNG/渐进式 JPEG → 设备可解码的 baseline JPEG）、TTS 代理
- **音源代理**（`tools/music_proxy/`）：点歌的云侧音源，端侧只认 4 个 GET，换源不烧固件

## 核心能力

| 能力 | 说明 |
|---|---|
| 🗣 语音对话 | 全双工语音上行 + 云端 LLM 回复（TTS 下播），打断（barge-in）即停 |
| 🧰 16 个 MCP 工具 | 打卡 / 拍照 / 看图讲解 / 点歌 / 音量 / 台灯 / 壁纸 / 表情包 / 天气等；工具表取自设备端 MCP 引擎 `tools/list`，云端 `tools/call` 直调 |
| 😊 表情包 | 云端下发图片 → 网关归一化 → 设备浮窗显示（≤1M 像素 baseline JPEG） |
| 🎵 点歌 | 云端搜歌 → TTS 念结果 → 双通道播放；异步落盘 SD 卡 `/music`（含歌词 LRC），FILES 页可浏览 |
| 🧑‍💼 人脸打卡（S8 遗产） | 录入（三采三验）+ 识别打卡，人脸库 SD 卡持久化，识别引擎按需加载/释放 |
| 📷 拍照 | 720p JPEG 存 SD 卡，拍完浮窗显示成片；与 CAM/VIDEO 页取景流自动抢占/归还 |
| 🖼 动态壁纸 | SD 卡壁纸池定时轮换 + 渐变切换，断电记忆 |
| 🌐 网页管理端 | 设备内置 HTTP :80：打卡记录、媒体浏览、壁纸/音乐管理 |
| 📶 网络 | Wi-Fi APSTA 双模（STA + 常开热点）、SNTP 校时、和风天气 |

## 目录结构

```
main/
  middleware/ai/        WS 长连接、事件总线、MCP 工具动作层、点歌/曲库
  middleware/camera/    相机服务(OV3660/esp_video)、拍照、人脸检测/识别、取景器
  middleware/audio/     播放/录音服务（esp_gmf 链）
  middleware/net/       WiFi、SNTP、网页管理端
  middleware/media/     AVI 封装/解封装、文件管理、录像、壁纸
  middleware/face_db/   人脸特征库（SD 卡持久化）
  ui/                   LVGL 页面 / 浮层 / 桌宠浮窗 / bin 字体
components/             esp_littlefs、esp_xiaozhi、手势检测（vendor）
tools/diag/             云端网关 + 部署/验收脚本
tools/music_proxy/      点歌音源代理（Docker/systemd）
docs/                   改动记录、问题清单（F 系列）、工程规范、验收清单
littlefs_content/       烧进 littlefs 分区的模型与内置资源
```

## 构建与烧录

```powershell
# ESP-IDF v6.1 终端（双击仓库根目录 IDF_v6.1_Powershell.lnk）
idf.py reconfigure
idf.py build
idf.py -p COM9 flash        # 全量烧录（app + storage.bin，分区偏移变更时必须全量）
```

- 分区表 `partitions.csv`（16MB：ota_0/ota_1 各 5M + littlefs 模型区 + storage）
- 模型走 littlefs（`littlefs_content/`），不编进固件
- SD 卡需 FAT32；`/music`、`/photos`、`/wallpaper`、`/face_db` 目录自动创建

## 云端部署（120.76.230.17 参考部署）

```bash
# 网关（systemd 服务 pet-gateway）
scp tools/diag/ws_gateway.py root@<server>:/opt/pet_gateway/
ssh root@<server> systemctl restart pet-gateway

# 音源代理
cd tools/music_proxy && docker compose up -d
```

网关配置见 `tools/diag/config.json`（端口 / AstrBot 地址 / 密钥占位）。

## 诊断与验收

| 工具 | 用途 |
|---|---|
| `tools/diag/serial_watch.py` | 串口日志观察（桌宠闭环联调） |
| `tools/diag/verify_pet_loop.py` | 端到端闭环验证：云端 push → WS → 设备日志 |
| `tools/diag/deploy_gateway.sh` | 网关服务器侧部署 |
| `docs/项目规划/实机验收清单_*.md` | 分段实机验收步骤与期望日志 |

## 开发约定

- 改动按 § 记入 `docs/项目规划/改动记录.md`，问题按 F 编号记入 `docs/工程规范/11_遇到的问题.md`
- 设备端字体：界面 ASCII 用 UI_FONT，任意中文一律走 bin 字体（`ui_binfont`）
- 内部 RAM 紧张：任务栈放 PSRAM 仅限纯 CPU/网络任务，碰 NVS/littlefs/flash IO 的栈必须内部 RAM
- 架构定稿与演化背景见 `tools/diag/feishu_arch_update.md`
