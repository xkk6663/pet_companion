# 记忆轨（S10 P5/P6）使用说明

云端"事件流 → 抽取 → 查询"的三个入口都在这一个目录里。设备侧只负责发切片
（`POST /slice`），不碰这里的任何文件。

## 目录与数据

```
$PET_MEMORY_DIR (默认 /opt/pet_memory)
├── raw/<YYYYMMDD>/events.jsonl     # append-only：speech + annotation
├── audio/<YYYYMMDD>/<id>.wav       # 设备原始切片
├── audio_grouped/<YYYYMMDD>/grp_*.wav  # 云端合并后的 30-60s 语音组
├── derived/memory.db               # people / commitments（可重建）
├── derived/<YYYYMMDD>.json
├── derived/<YYYYMMDD>.groups.json  # 分组 manifest：ids / 时间 / 文本 / 音频
└── nightly/<YYYYMMDD>.report.json  # 日报（数字 + built_from）
```

原始层纪律：**只追加**。补转写写成 `kind=annotation` 的新行（`target` 指向原事件），
不改原行；读取时按 `field` 合并成覆盖值。

## 三个入口

| 入口 | 用途 |
|---|---|
| `nightly.py` | 夜间轨：分组转写 + 抽取 + 统计 + 日报 |
| `group_audio.py` | 把相邻切片合成 30-60s 语音组，并整组做一次 ASR |
| `retention.py` | 按天回收：音频默认 30 天、raw 默认 180 天 |
| `query.py` | 命令行查询（与 `/memory/query` 同口径） |
| 网关 `/memory/query`、`/memory/resources*` | 模型侧读记忆的唯一入口 |

### nightly.py

```bash
python nightly.py --date 2026-11-15            # 有 AstrBot 时走 LLM 抽取
python nightly.py --date 2026-11-15 --no-llm   # 只统计 + 规则抽取（不猜）
```

- LLM 不可用/返回非 JSON → 自动退化为规则抽取，`source="rule"`，**不编造人物**；
- 空数据短路：无 speech、语音 < `PET_MIN_SPEECH_MS`（默认 5000）、或
  `text_count=0 && audio_count=0` 时，只写 `source="empty"` 标记，不分组、不 ASR、不 LLM；
- 同一输入跑两次，除 `generated_at` 外逐字段一致（自测里有断言）。

### group_audio.py

```bash
python group_audio.py --date 20261115
python group_audio.py --date 20261115 --no-transcribe --max-s 45
```

- 默认 `gap_ms=30000`、`max_s=60`：相邻切片间隔 30 秒内、累计语音不超过 60 秒就并成一组；
- 每组写 `audio_grouped/<day>/<group_id>.wav` 和 `derived/<day>.groups.json`；
- 整组只调用一次 ASR；组文本命中后，`nightly.py` 会把组内所有原事件 id 写进 `built_from`；
- 重跑复用 `derived/<day>.groups.json`：组员没变且上次 `asr_done=true`、无 `error` 就不重复 ASR；
  空文本也复用（上次成功但确实没人声），只有新增/失败的组才重转；
- `nightly.py` 在 `use_llm=True` 时自动调用；只有分组完全建不起来才回退逐片补转写。

### query.py

```bash
python query.py --date-from 2026-11-09 --date-to 2026-11-15 --person 张总
python query.py --commitments --date-from 2026-11-10
python query.py --q 方案 --json
```

### retention.py

```bash
python retention.py --dry-run     # 默认，只打印将删除的日期目录
python retention.py --apply       # 真正删除
PET_AUDIO_RETENTION_DAYS=7 python retention.py --apply
```

- 只删 `audio/<YYYYMMDD>/`、`audio_grouped/<YYYYMMDD>/` 与 `raw/<YYYYMMDD>/`；
- 保留 `nightly/*.report.json`、`derived/*.json`、`derived/memory.db` 里的统计/摘要；
- 只处理严格 8 位日期目录，跳过符号链接；`--apply` 前建议先看 dry-run。

### 设备端 SD pending（不是云端脚本）

`main/middleware/ai/slice_client.c` 的断网兜底目录
`/sdcard/pet_memory/pending` 自带上限：

- 64 MB / 64 条 / 剩余空间至少 8 MB；
- 超过 7 天的 pending 自动删；
- 超限先删最旧；仍写不下就丢当前切片，并打
  `SD pending full, slice ... dropped`；
- 每 60 s 补传前先清理一次过期/超限，ACK 200 后立即删除。

### 网关端点

```bash
curl -s http://127.0.0.1:8081/memory/resources
curl -s -X POST http://127.0.0.1:8081/memory/query \
  -H 'Content-Type: application/json' \
  -d '{"type":"day","date_from":"2026-11-15","date_to":"2026-11-15"}'
```

## 自测

```bash
# 项目根目录；Windows 用 venv 的绝对路径
.venv/Scripts/python.exe cloud/memory/selftest_slice.py    # /slice + query + resources
.venv/Scripts/python.exe cloud/memory/selftest_nightly.py  # 夜间轨 + 可复现
```

两个脚本都用临时 `PET_MEMORY_DIR`，不会写真实目录。

## 部署

```bash
# 服务器：先把 nightly.py / query.py / retention.py / systemd unit 放到 /tmp/mem/
bash deploy_memory.sh
```

timer 每天 03:05 跑昨天；日志 `tail -f /opt/pet_memory/nightly/cron.log`。
回收 timer 每天 03:30 跑 `retention.py --apply`，日志 `journalctl -u pet-memory-retention.service`。

## 已知边界

- 断网兜底的补传文件是 WAV（不是 OPUS）——先保证"不丢"，编码迁移见 S10 §13.4；
- 音频保留期默认 30 天、raw 默认 180 天，已由 `pet-memory-retention.timer` 执行；
  设备 SD 的 `pending/` 只在补传成功后删除，没有 TTL/容量上限；
- LLM 抽取走 AstrBot 的 `/api/v1/chat`，会带人设上下文；解析失败即退规则，不会污染原始层。
