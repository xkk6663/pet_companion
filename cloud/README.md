# 云端侧代码（`cloud/`）

跑在服务器上的那部分。设备端只负责"在场"（录音、执行动作、上屏），智力全在这里：
网关做协议收敛与媒体归一化，AstrBot 做人格与工具编排，音源代理兜底曲库。

```
cloud/
├── gateway/                     FastAPI 网关（唯一入口 / 工具中继 / 媒体归一化）
│   ├── ws_gateway.py            ★ 全部逻辑都在这一个文件里（单文件部署，方便热更）
│   ├── config.example.json      配置模板（密钥是占位符，真实值只放服务器）
│   ├── pet-gateway.service      systemd 单元（MemoryMax=256M）
│   ├── deploy_gateway.sh        服务器侧一键部署（建 venv / 装依赖 / 起服务）
│   └── create_gateway_key.sh    在 AstrBot 面板上换 abk_ 密钥的辅助脚本
├── music_proxy/                 点歌音源代理（Docker，:8080；网易云 + 两个兜底源）
├── memory/                      S10 记忆工具：夜间抽取 / 保留期 / 查询 + systemd timer
└── astrbot_plugin_pet_memory/   AstrBot 插件：长期记忆（设备侧工具注册见 pet_device）
```

## 首次部署顺序

顺序不能反：网关启动时要直读 AstrBot 的 `cmd_config.json` 复用 TTS/STT provider。

1. **AstrBot**（容器 `:6185`）：配好 LLM / TTS / STT / VLM / embedding，装「大肥鱼」人格，
   在面板「设置 → API 密钥」建一把 `abk_` 开头的 key（scopes：`chat` / `im` / `file`）。
2. **网关**：把 `gateway/` 下的 `ws_gateway.py`、`config.example.json`（在服务器上改名为 `config.json`
   并填入真实 key）、`pet-gateway.service` 一起放到服务器 `/tmp/gw/`，然后 `bash deploy_gateway.sh`。
3. **音源代理**：`cd music_proxy && cp .env.example .env && docker compose up -d`（:8080）。
4. **记忆工具**：`bash memory/deploy_memory.sh`（复用网关的 venv，装两个 systemd unit + timer）。
5. **AstrBot 插件**：`bash astrbot_plugin_pet_memory/install.sh --restart`。

## 需要自备、但绝不入库的东西

| 东西 | 放哪 | 仓库里的样子 |
|---|---|---|
| AstrBot `abk_` API key | 服务器 `/opt/pet_gateway/config.json`（`chmod 600`） | `abk_xxx_填入真实值` |
| 服务器 SSH 私钥 | 本机 `~/.ssh/<key>.pem`，用完删副本 | 文档里写 `~/.ssh/<key>.pem` |
| 网易云 / 音源 token | 服务器 `music_proxy/.env` | `.env.example` 里是空串 |
| 服务器地址、实例 ID | 只在服务器与本地终端 | 文档里写 `<server>` / `<instance-id>` |

`deploy_gateway.sh` 有一处刻意设计：**`config.json` 已存在就绝不覆盖**。
否则每次部署都会把线上的真实 key 冲成模板里的占位符，网关静默 401，设备表现为"能连上但一句话不回"。

## 自检

```bash
curl -s http://127.0.0.1:8081/health          # {"ok":true,"tts":true,"stt":true,...}
systemctl --no-pager status pet-gateway        # active (running)
tail -f /opt/pet_gateway/gateway.log           # [GW] 帧级日志
```

仓库内还能脱机跑网关的纯函数单测（不发真实网络请求）：

```bash
python -m pytest tests/unit/test_gateway_protocol.py -q
```

## 与设备端的分界

| 归这里（云端） | 归设备端（`main/`） |
|---|---|
| ASR / LLM / TTS / 生图 / 记忆 | 录音、上屏、执行动作、回执 |
| 把 GIF/PNG 归一化成设备吃得下的 baseline JPEG | 只解 baseline JPEG（≤1M 像素 / ≤1200 KB） |
| 工具中继（`POST /action` 同步等回执） | 动作表（`ai_action.c`）与 6 个业务模块 |
| 断线重试、降级兜底 | 1s→30s 指数退避重连 |

设计动机与完整时序见 [`docs/开发文档/架构分析.md`](../docs/开发文档/架构分析.md)、
[`docs/开发文档/S9_云端网关_开发文档.md`](../docs/开发文档/S9_云端网关_开发文档.md)。
