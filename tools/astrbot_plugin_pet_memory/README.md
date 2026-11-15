# astrbot_plugin_pet_memory —— 每日转写、记忆查询与成长监督

把 pet_companion 云端已经跑通的记忆链路收成一个 **可启停的 AstrBot 插件**，一个插件内含两个相互独立的模块：

```
设备/VAD 切片 ──> 网关 /slice ──> raw/<day>/events.jsonl
                                      │
                   夜间 nightly.py ──> nightly/<day>.report.json
                                      │
                 ┌────────────────────┴────────────────────┐
                 │ memory_query 模块                        │ growth 模块
                 │ /memory/query、resources/read            │ 21:30 读日报 → growth/<day>.json
                 │ 大肥鱼工具调用 / 指令                     │ on_llm_request 注入最近 3 天
                 └────────────────────┬────────────────────┘ 微信订阅推送大肥鱼评语
                                      │
                              大肥鱼 / 微信会话
```

`memory_query` 只读云端记忆，不改 raw 行，也不重跑夜间抽取。`growth` 只读夜间日报，写自己的 `growth/<day>.json`，不碰原始转写，也不直接写 LivingMemory 数据库。两个模块分别由 `enabled` 和 `growth_enabled` 控制。

## 1. 能力

| 入口 | 类型 | 说明 |
|---|---|---|
| `pet_memory_query` | LLM 工具（唯一一条） | 模型问“昨天发生了什么 / 某人说过什么 / 答应过谁什么”时调用；支持 `day/person/commitments/search` 四种维度，返回带 `built_from` |
| `/记忆` | 指令 | 插件状态、网关连通性、最近日报、订阅状态、成长模块状态 |
| `/记忆日报 [YYYY-MM-DD]` | 指令 | 默认读昨天；没有日报时明确说明，不编造 |
| `/记忆查 关键词` | 指令 | 最近 `default_range_days` 天的事件检索 |
| `/记忆人物 称呼` | 指令 | 按人物过滤事件 |
| `/记忆承诺 [天数]` | 指令 | 读取夜间抽取出的承诺（含 due/status） |
| `/记忆订阅` / `/记忆退订` | 指令 | 把当前会话加入/移出每日日报推送 |
| `/记忆推送 on\|off\|HH:MM` | 指令 | 开关或修改每日推送时间 |
| `/记忆开关 on\|off` | 指令 | 运行时启停记忆能力（等价于插件内开关） |
| `/成长` | 指令 | 成长模块状态、时间、订阅数、最近摘要与最近错误 |
| `/成长摘要 [YYYY-MM-DD]` | 指令 | 查看已生成的成长摘要 |
| `/成长生成 [YYYY-MM-DD]` | 指令 | 手动补生成摘要；空日报只记录 `empty=true`，不推送 |
| `/成长订阅` / `/成长退订` | 指令 | 把当前微信会话加入/移出成长评语推送 |
| `/成长推送 on\|off\|HH:MM` | 指令 | 开关成长推送，或修改生成/推送时间 |
| `/成长开关 on\|off` | 指令 | 独立启停成长模块，不影响记忆查询 |
| `/成长测试` | 指令 | 向已订阅会话发一条链路测试消息，不写摘要、不动日报 |

## 2. 为什么只注册一条工具

AstrBot 的 LLM function calling 只认 tools、不认 resources；如果每个记忆维度都注册一条工具，工具表会随维度膨胀，还会撞上设备 caps 那类工具表约束。

所以沿用 S10 D5 的收敛方案：

- 记忆维度 = 网关 `MEMORY_RESOURCES` 里的一个 resource；
- 模型侧永远只有 `pet_memory_query` 一条工具；
- 加人物库、承诺库、宠物健康等新维度时，只加 resource，工具表不变。

## 3. 启停语义（重要）

AstrBot 动态 `add_func()` 造出的 `FuncTool` 天生 `active=True`、`handler_module_path=None`，会**跳过插件启用检查**。因此：

- 插件在 `terminate()` 里显式 `remove_func("pet_memory_query")`；
- `/记忆开关 off` 也会立刻摘除工具；
- WebUI“已安装 → 关闭插件”会触发 `terminate()`，工具随插件一起消失；
- 重新打开插件或 `/记忆开关 on` 会重新注册。

这样“关插件”才真的等于“关能力”，而不是只关掉指令、模型还在调用旧工具。

## 4. 安装（云端服务器）

AstrBot 实际运行在 `root@120.76.230.17`，数据目录 `/opt/astrbot/data`，容器名 `astrbot`。

```bash
# 在服务器上，从本仓库拷贝
cd /opt/pet_companion/tools/astrbot_plugin_pet_memory   # 或你放源码的位置
bash install.sh                 # 只拷贝，不重启
bash install.sh --restart       # 拷贝后 docker restart astrbot
```

或从本地直接推：

```powershell
scp -i C:\Users\xiao1\Downloads\_xkk_3.pem -r `
  tools\astrbot_plugin_pet_memory `
  root@120.76.230.17:/opt/astrbot/data/plugins/
ssh -i C:\Users\xiao1\Downloads\_xkk_3.pem root@120.76.230.17 "docker restart astrbot"
```

插件目录最终为：

```
/opt/astrbot/data/plugins/astrbot_plugin_pet_memory/
├── main.py
├── metadata.yaml
├── _conf_schema.json
└── README.md
```

## 5. 配置

WebUI → 插件 → 每日转写与记忆 → 齿轮：

| 配置 | 默认 | 说明 |
|---|---|---|
| `enabled` | `true` | 关闭后不注册工具、查询指令提示已关闭 |
| `gateway_base` | `http://172.18.0.1:8081` | AstrBot 容器访问宿主机网关；**不要填 127.0.0.1** |
| `request_timeout` | `15` | 单次请求超时 |
| `default_range_days` | `7` | `/记忆查`、`/记忆人物` 的默认天数 |
| `max_items` | `20` | 单次最多展示条数（1–50） |
| `daily_push_enabled` | `false` | 每日日报主动推送 |
| `daily_push_time` | `08:30` | 服务器本地时间 |
| `daily_push_targets` | `[]` | 目标会话，发 `/记忆订阅` 自动写入 |
| `growth_enabled` | `true` | 成长模块独立开关；关闭后查询仍可用，已有摘要保留 |
| `growth_time` | `21:30` | 成长摘要生成/推送时间；应晚于晚间 nightly |
| `growth_push_enabled` | `true` | 是否把成长评语推到订阅会话 |
| `growth_push_targets` | `[]` | 成长推送目标，发 `/成长订阅` 自动写入 |
| `growth_persona_id` | `""` | 评语所用 persona；留空取 `personas` 表里 `system_prompt` 最长的一条 |
| `growth_lookback_days` | `7` | 趋势回看天数 |
| `growth_max_chars` | `1200` | 注入人格上下文的成长摘要上限 |
| `growth_graph_write_enabled` | `false` | G4：稳定事实写入人物关系网，默认关 |

配置实体在 `/opt/astrbot/data/config/astrbot_plugin_pet_memory_config.json`。

## 6. 验收（照顺序做）

1. **容器能到网关**：
   ```bash
   docker exec astrbot python -c "import urllib.request;print(urllib.request.urlopen('http://172.18.0.1:8081/health',timeout=3).read().decode())"
   ```
   期望 `{"ok":true,...}`。
2. **插件加载成功**：AstrBot 日志出现 `[pet_memory] 工具已注册：pet_memory_query`，WebUI 插件页无“加载失败”。
3. **指令**：发 `/记忆`，应显示网关在线、工具已注册、最近日报（如果有）。
4. **日报**：发 `/记忆日报 2026-11-14`，有日报则渲染事件/人物/承诺/话题；没有则提示“还没有日报”，不编造。
5. **工具**：问大肥鱼“昨天都记录了什么”，应触发 `pet_memory_query`；返回里每条带 `built_from`。
6. **关闭**：发 `/记忆开关 off`，再问一次，模型不再有该工具；或在 WebUI 关闭插件，日志出现 `[pet_memory] 工具已摘除：pet_memory_query`。
7. **重开**：`/记忆开关 on` 或 WebUI 打开插件，工具恢复。
8. **推送**：`/记忆订阅` → `/记忆推送 08:30`，次日检查是否收到昨日日报（无日报时跳过）。

## 7. 常见问题

- **工具不出现**：当前人格的工具白名单可能没放开；到 人格 → 工具 里确认 `pet_memory_query` 可用。
- **网关不可达**：容器内不要用 `127.0.0.1`，用 `172.18.0.1`；宿主机 `systemctl status pet-gateway`。
- **日报 404**：夜间 timer 没跑或那天没有事件；服务器上先查
  `systemctl list-timers pet-memory-nightly.timer`，再手动补跑
  `/opt/pet_gateway/.venv/bin/python /opt/pet_memory/bin/nightly.py --date 20261114`。
- **关闭插件后模型还调旧工具**：检查日志是否出现 `工具已摘除`；若没有，说明 `terminate()` 没被调用，重载插件再看。
- **推送不生效**：`daily_push_enabled` 必须为 `true`，`daily_push_targets` 非空，且时间匹配服务器 `date +%H:%M`。

## 8. 云端部署实录（2026-11-15）

实测环境：`root@120.76.230.17`，AstrBot v4.29.1，容器 `astrbot`，WebUI `:6185`。

### 8.1 网关：合并，不覆盖

云端 `/opt/pet_gateway/ws_gateway.py` 原版没有 `/slice`、`/memory/*`，但有本地仓库没有的流式 TTS（`tts_stream`、`_sse_pcm_chunks`、`StreamingResponse` 等）。因此**不能整文件 scp 覆盖**，实际做法：

1. 备份 `ws_gateway.py.bak_memory_<YYYYMMDD_HHMMSS>`；
2. `merge_gateway_memory.py` 只把本地 S10 记忆段（285 行）插到云端「插件主动推送」标记前；
3. 合并后 1651 行，`py_compile` 通过；保留 `tts_stream` 10 处、`_sse_pcm_chunks` 2 处、`tts_stream_endpoint` 1 处、`StreamingResponse` 2 处、`CIMG_PIXEL_CAP` 3 处；
4. `systemctl restart pet-gateway`，服务 `active`，`/health` 返回 `ok v0.8.1`。

结果：`GET /memory/resources` 返回四条资源；`POST /memory/query` 空数据返回 200；`POST /slice/<sid>` 能落盘并返回 200。

### 8.2 夜间轨

`pet-memory-nightly.timer` 已 `enable --now`，每天 03:05 跑昨天：

```
NEXT Mon 2026-11-16 03:05:00 CST  pet-memory-nightly.timer
```

手动跑 `20261114` 得到 `events=0` 的日报；`/记忆日报 2026-11-14` 能正常渲染“没有记录到事件”，不再 404。

### 8.3 插件实测

| 项 | 实测结果 |
|---|---|
| 加载 | `/记忆` 显示“已启用 · 工具已注册（管理器可见） · 网关在线 v0.8.1” |
| 关闭 | `/记忆开关 off` 显示“工具未注册”；`/记忆` 显示“已关闭 · 工具未注册” |
| 重开 | `/记忆开关 on` 恢复“已注册（管理器可见）” |
| 空数据 | `/记忆承诺 7` 0 条；`/记忆日报` 渲染 0 事件；`/记忆查`、`/记忆人物` 均 0 条，不编造 |
| 有数据日报 | 样例日报渲染出“人物：张总 / 承诺 1 条 / 话题：方案、周五”，测试文件已删除 |
| LLM 工具 | 自然语言触发 `pet_memory_query` 两次（commitments + day），返回真实空结果，模型如实说明“没有记录” |
| 切片 | 1 s 静音 WAV POST `/slice/selftest` 返回 200，`raw/20260101` 与 `audio/20260101` 落盘；`/memory/query` 读到 1 条事件（测试数据已清理） |
| 每日推送 | 订阅测试会话并把时间设为 `13:25`，AstrBot 日志出现 `13:25:28 [pet_memory] 日报已推送到 webchat:FriendMessage:webchat!xiao1!plugin-push-test` |

推送验收后已复位：`daily_push_enabled=false`、`daily_push_time=08:30`、`daily_push_targets=[]`。

### 8.4 当前边界

- `/memory/*` 无鉴权，只应开在同机/容器内网；公网暴露前先加鉴权。
- `/记忆开关` 未加管理员校验，任何能发指令的会话都能开关；多人场景需收紧。
- 设备侧 `mic_gate` / `slice_client` 尚未编译烧录，`/opt/pet_memory` 目前只有 20261114、20261115 两份空日报，没有真实日常数据。

## 9. 不做的事

- 不触发夜间补转写/LLM 抽取：那是 `pet-memory-nightly.timer` 的职责，避免插件和 timer 双重跑。
- 不写 raw、不改 annotation：所有查询都是只读。
- 不新增网关鉴权：当前 `/memory/*` 无鉴权，仅同机/容器内网可达；如要暴露公网，先在网关前加鉴权。

## 10. 成长模块：每晚总结 / 人格注入 / 微信监督

### 10.1 为什么独立成模块

成长模块不是“把聊天记录再总结一遍”，而是把夜间日报里的 **结构化事实** 蒸馏成三样东西：

1. **事实**：今天几件事、说了多久、出现谁、哪些承诺还挂着；
2. **判断**：大肥鱼对“承诺没兑现 / 今天太安静 / 关系有变化”的态度；
3. **表达**：一两句微信话，能直接推给用户，不新增事实。

它与 `memory_query` 共用同一个插件、同一份配置，但用 `growth_enabled` 独立启停，避免拆成两个插件后状态不一致。

### 10.2 两层记忆，不混库

| 层 | 位置 | 内容 | 是否长期 |
|---|---|---|---|
| 插件成长记忆 | `plugin_data/astrbot_plugin_pet_memory/growth/<YYYYMMDD>.json` | 蒸馏事实、判断、建议、微信话、`built_from` | 按天保留，最近 3 天注入人格 |
| 人格知识库 | LivingMemory `livingmemory.db` / 人物关系网 | 稳定的人物、关系、长期偏好 | 默认不写入；G4 开关打开后才接 |

边界：

- 只读 `nightly/<day>.report.json`，**不把 raw 转写喂给 LLM 自由总结**；
- `growth/*.json` 只存蒸馏结果，不存原始句子；
- 不直接 `INSERT livingmemory.db`，也不把每日流水灌进人物关系网；
- 长期稳定事实若要进图库，走 LivingMemory 官方 memorize / `memory_engine.add_memory`，并带 `source=pet_growth`、`built_from`。G4 默认关闭。

### 10.3 每晚链路

```
21:15  pet-memory-evening.timer
       └─ nightly.py --date 今天 → nightly/20261115.report.json
21:30  growth 检查任务（每 30 秒轮询）
       ├─ 读 report.json（stats / people / commitments / topics）
       ├─ 读最近 growth_lookback_days 天趋势
       ├─ 读 memory://commitments 过滤未兑现
       ├─ persona（默认 personas 表最长 system_prompt）+ 纪律
       ├─ 输出 JSON：judgment / suggestion / message / facts_used
       ├─ 数字校验：message 里的数字必须来自事实，否则退化规则兜底
       └─ 原子写 growth/<day>.json
            └─ 有订阅且非空 → context.send_message 微信文字
```

- 空日报：写 `empty=true`，**不推送**；
- 已推送：`push.sent_at` 非空，不重复推；
- 生成失败：5 分钟后重试；
- 23:00–07:30 只发文字，不发声；微信通道本身只发文字。

### 10.4 摘要文件字段

```json
{
  "ok": true,
  "empty": false,
  "date": "20261115",
  "source": "pet_growth",
  "built_from": "nightly/20261115.report.json",
  "facts": {"stats": {}, "people": [], "commitments": [], "topics": []},
  "trend": {"reports": 3, "events": 8, "speech_min": 21.5, "active_days": 2},
  "open_commitments": [],
  "judgment": "今天只记到 3 件事……",
  "suggestion": "今晚先把周报发给小李……",
  "message": "哼，周报还没发给小李呢……",
  "facts_used": ["events=3", "speech_min=12.5"],
  "reflection_source": "llm",
  "persona_id": "333",
  "push": {"enabled": true, "sent_at": null, "targets": []},
  "graph_write": {"enabled": false, "status": "reserved"}
}
```

### 10.5 人格注入

`@filter.on_llm_request()` 把最近 3 天、非空、已生成的摘要压成只读事实块，追加到 `req.system_prompt`：

```
[成长记忆 · 只读事实]
11-15：3 条事件，语音 12.5 分钟；人物 张总、小李；未兑现 2 条：……
11-14：今天没有记录到事件。
```

- 只注入事实与判断，不注入原始转写；
- 总长度受 `growth_max_chars` 限制；
- 模型可以引用，但不能把摘要当成新事实再编；
- 注入成功日志：`[pet_memory] 已注入成长上下文（N 字）`。

### 10.6 推送与订阅

- 目标会话由 `/成长订阅` 写入 `growth_push_targets`，存的是 AstrBot `unified_msg_origin`；
- 推送走 `context.send_message`，不走设备 `/push`；
- `/成长测试` 只发一条固定测试话，用来验收微信通道，不生成摘要；
- 真实微信通道：`weixin_personal_mgwh` 适配器；当前已订阅用户自己的微信会话，README 不记录具体 UMO。

### 10.7 已验收（2026-11-15）

| 项 | 结果 |
|---|---|
| 插件加载 | 1.1.0，`pet_memory_query` 已注册，成长检查任务已启动（21:30） |
| 空日报 | `/成长生成 20261114` → “日报为空，已记录摘要但不推送” |
| 样例日报 | `/成长生成 20261115` → LLM 输出 43 字微信话，`来源 llm`、`persona=333` |
| 数字校验 | 首次把 12.5 说成 12 被拒；允许整数部分后通过，事实外数字仍拒 |
| 人格注入 | 聊天“我最近怎么样”时，模型 reasoning 明确引用“11-15成长记忆：3条事件……” |
| 工具回归 | `_tool_query` 兼容位置参数后，`pet_memory_query` 返回 2 条 open 承诺，不再报 `takes 2 positional arguments` |
| 微信推送 | `/成长测试` → 日志 `成长测试消息已推送到 weixin_personal_mgwh:...`；实际微信会话收到测试话 |
| 定时器 | `pet-memory-evening.timer` 每天 21:15 跑当天 nightly；原 03:05 昨天 nightly 保留 |
| 清理 | 验收用样例 `20261115.report.json` 与 `growth/20261115.json` 已删除，避免污染人格 |

### 10.8 边界

- G4 只留 `growth_graph_write_enabled` 配置与 `graph_write.status="reserved"`，**没有实际图库写入**；
- 推送失败只记日志，不阻塞摘要生成；
- `/成长开关`、`/成长订阅` 未加管理员校验，单人使用足够，多人场景需收紧；
- 成长摘要质量依赖夜间日报质量；日报抽取不到人物/承诺时，评语会退化为“今天没有记录到事件”，不会编造。
