# DESIGN — 桌宠伴侣技术汇报

> 项目：`桌宠伴侣技术汇报/桌宠伴侣技术汇报.pptx` · 画布 1280×720 · 16 页
> 受众：技术 / 产品决策者。目标：讲清「云端大脑怎么分层、边缘设备怎么分工、各自贡献什么」。
> 素材原则：**所有架构、数字、文件名均可回溯到仓库代码或服务器实况**（README / 改动记录 / 源码）。

---

## 一、全局视觉约定

### ① 画布与安全区

- 1280×720，padding 上下 60 / 左右 60，内容区 1160×600
- 页脚带：y = 672（顶边距底 48px），字号 14，左端 x=60，右端 ≤1220
- 封面 / 目录 / 章节页 / 结尾页 **不加页脚**

### ② 配色（科技蓝）

| 槽位 | Hex | 用途 |
|---|---|---|
| 主色 | `#2563EB` | 标题强调、编号块、主图形、章节页底色 |
| 辅色 | `#06B6D4` | 云端侧图元、分隔线、第二序列 |
| 强调色 | `#F59E0B` | 关键数字、当前步骤、"设备端"高亮 |
| 深色 | `#0F172A` | 章节页底、深色强调卡、封面文字区 |
| 浅底 | `#F1F5F9` / `#EFF6FF` | 卡片底、表格斑马纹 |
| 正文 | `#1E293B` | 主文字 |
| muted | `#64748B` | 注释、次级说明 |

纪律：主色为主，强调色每页 ≤3 处；深色大块每页 ≤1；禁阴影。

### ③ 字体

- 中文：`Microsoft YaHei`；西文/代码：`Consolas`（仅代码与帧名）
- 阶梯：封面主标 64 / 章节大字 60 + 序号 150 / 巨型数据 84 / 页面主标 34 / 卡片小标 22 / 正文 18-20 / 注释 15 / 页脚 14
- 同页样式组合 ≤4 种

### ④ 视觉母题

**页面主标题下 1px 贯通细线（#E2E8F0，贯通 1160）**，全篇内容页复用；标题固定 y=60，细线紧随其下。右侧统一放章节眉标（`01 / 产品与形态` 形式，14px muted）。

### ⑤ 配图清单

| 文件 | 等级 | 来源 | 真实内容 | 使用页 |
|---|---|---|---|---|
| `assets/device_front.jpg` | L1 | 实拍（docs/hardware/正面.jpg，1109×832） | ESP32-S31-Korvo-1 真机正面，800×480 屏亮屏 | P1 |
| `docs/hardware/俯视45度.jpg` | L1 | 实拍（1031×773） | 俯视 45° 整机：主板、摄像头、供电口一览 | P4 |
| `docs/hardware/主板图.jpg` | L2 | 实拍（1569×1057） | 主板俯视，可见模组与接口 | P7（深色底纹，80% 遮罩） |
| `docs/hardware/tex_screen.jpg` | L2 | 实拍（800×480） | 屏幕内容特写（壁纸 + FPS / CPU HUD） | P16（深色底纹，84% 遮罩） |

其余全部为 **SVG 结构化架构图**（分层栈、时序链、对比矩阵）——技术汇报中架构图就是主视觉，不用 AI 生图替代，避免"编造的架构"。

---

## 二、分章与节奏

| 章 | 标题 | 页码 |
|---|---|---|
| 01 | 产品形态与系统全景 | P3–P6 |
| 02 | 边缘端：身体如何落地 | P7–P9 |
| 03 | 云端大脑与端云协同 | P10–P15 |

节奏：P1 peak（封面）→ P2 valley → P3 transition → P4 valley → P5 valley → **P6 peak** →
P7 transition → P8 valley → P9 valley → P10 transition → **P11 peak** → P12–P15 valley → P16 peak（结尾）。
Hero 3 个（P1 / P6 / P11 + 结尾），占比 25%。

---

## 三、逐页字段表

### P1 封面
- type: cover / role: hero / rhythm: peak / layout: 杂志对页式（左图右文，右 36% 深色文字区）
- visual: L1 `assets/device_front.jpg` 占左 64% 出血（objectFit cover）
- visual_role: anchor
- density: 字数约 40 / 图片 1 张 / 留白约 30%
- description: 左侧真机实拍铺满左栏；右侧 #0F172A 深色区自上而下：眉标「S9 · 边缘计算 × 云端大脑」→ 主标题「边缘 AI 桌宠的技术架构」→ 副标「ESP32-S31 身体 × 云端大脑：分层、契约与分工」→ 底部信息「技术汇报 · 2026.11」。无数据、无页脚。
- anti_pattern: 禁止在封面放任何指标数字；禁止上下对称居中式封面；禁止把真机图缩成小贴图

### P2 目录
- type: catalog / role: supporting / rhythm: valley / layout: 经典纵排（左「目录 / CONTENTS」+ 贯通细线 + 右竖排三章）
- visual: L3 无（纯排版）
- visual_role: evidence
- density: 字数约 60 / 图片 0 张 / 留白约 45%
- description: 三章清单：01 产品形态与系统全景（P03）、02 边缘端：身体如何落地（P07）、03 云端大脑与端云协同（P10）。只列编号+章名+页码。
- anti_pattern: 禁止加"全篇共 16 页 / 3 个章节"统计块；禁止复述阅读顺序

### P3 章节页 01
- type: section / role: transition / rhythm: transition / layout: 章节大字（左侧超大 01，右侧章名 + 一行说明）
- visual: 深色底 #0F172A + 超大编号作装饰
- visual_role: atmosphere
- density: 字数约 30 / 留白约 60%
- description: 编号「01」150px 半透明；章名「产品形态与系统全景」；一行说明「这台设备长什么样、能干什么、系统由哪几层构成」。
- anti_pattern: 禁止放三条看点清单；禁止加"本章结论"条

### P4 产品形态与硬件
- type: content / role: supporting / rhythm: valley / layout: 左图右文（图 52% / 文 44%，中间 4% 间距）
- visual: L1 `docs/hardware/俯视45度.jpg`（左侧，560×420）
- visual_role: evidence
- density: 字数约 200 / 图片 1 张 / 留白约 15%
- description: 标题「一台会说话、会看、会动手的终端」。左侧真机照。右侧一张 6 行硬件规格表（SoC ESP32-S31 RISC-V 240MHz / 16MB Flash + 8MB PSRAM / 800×480 RGB 触摸屏 / OV3660 摄像头 / ES8389 音频 / microSD）。底部结论条：「4 英寸屏 + 摄像头 + 麦克风，形态决定它必须"在场"—— 感知与执行都在设备上，云端只负责判断。」
- anti_pattern: 禁止 50:50 等分；禁止把规格写成无容器的裸露表格

### P5 能力全景（16 个工具）
- type: content / role: supporting / rhythm: valley / layout: 左标题色块 + 右内容（左 30% 深色块，右 66% 两列模块清单）
- visual: 左侧色块 + 右侧 6 组模块条目
- visual_role: evidence
- density: 字数约 260 / 留白约 12%
- description: 标题「6 个业务模块，16 个可被云端调用的工具」。左侧色块写「16 / 工具总数」+ 「6 个业务模块」+ 一句「工具表由固件自述，云端动态注册」。右侧按模块分组列：camera 拍照·看图讲解 / punch 人脸打卡 / music 点歌·停播·曲库·下载 / speaker 音量读写 / light 开关·亮度·色彩·状态 / device 重启·Wi-Fi / local 换壁纸。底部一行小字标注来源 `main/middleware/ai/ai_action.c :: TOOLS[]`；左下深色块内再补一行「另有 8 项纯端侧能力：文件 / 录像 / 录音 / 蓝牙 / 壁纸 / 网页端」。
- anti_pattern: 禁止用等大卡片网格罗列 16 个工具；禁止把工具名写成营销话术

### P6 系统全景（★ 核心架构图）
- type: content / role: hero / rhythm: peak / layout: 三分层横向架构图（设备 | 网关 | 云端），SVG + 容器
- visual: SVG 架构图占内容区 100%（1160×470）
- visual_role: anchor
- density: 字数约 180 / 图 1 张 / 留白约 10%
- description: 从上到下画三段：**云端大脑**（AstrBot 编排 / LLM·ASR·TTS·VL / 音源与媒体）↔ **网关 pet-gateway :8081**（长连接、动作中继、媒体归一化）↔ **边缘设备 ESP32-S31**（五级链路）。三段之间用标注两条通道：上行 PCM/text/caps/action_result，下行 text/emotion/audio/image/action。底部结论条：「设备永远主动向外连 —— 这一条连接同时承担实时对话、能力协商和反向控制。」
- anti_pattern: 禁止画成"云→设备单向箭头"；禁止在架构图里堆未实现的模块

### P7 章节页 02
- type: section / role: transition / rhythm: transition / layout: 同 P3（编号 02）
- visual: 深色底 #0F172A + 主板实拍（`docs/hardware/主板图.jpg`）整页铺底，上盖 80% #0F172A 遮罩
- description: 「02 边缘端：身体如何落地」+ 「从一条 WebSocket 到一个动作：五级链路，以及哪些智能必须留在本地」。

### P8 边缘端五级链路
- type: content / role: supporting / rhythm: valley / layout: 纵向层级栈（左编号 01-05，右侧每层：文件名 + 职责 + 约束）
- visual: SVG 纵向管道 + 右侧卡片
- visual_role: evidence
- density: 字数约 280 / 留白约 10%
- description: 五层自上而下：① 传输 `ws_client.c` 单连接、指数退避、按 JSON 括号切帧；② 解码 `event_bus.c` 7 种下行帧分发、收包任务不阻塞；③ 执行 `ai_action.c` + 6 模块，动作队列 7168B 栈，回 action_result；④ 会话 `xiaozhi_service.c` 双播放通道（TTS pri=2 抢占 / 音乐 pri=1 让位）；⑤ 表达 `ui/` 桌宠浮窗 + 6 个 tab。右侧标注各层代码行数（ai 8,570 / ui 11,000+）。
- anti_pattern: 禁止五层等高等宽地平铺；禁止只写模块名不写职责

### P9 端侧智能：为什么推理留在本地
- type: content / role: supporting / rhythm: valley / layout: 对比式（左「传脸到云」vs 右「端侧识别」，中间竖线 + VS）
- visual: 左右两组数据点 + SVG 小图标
- visual_role: evidence
- density: 字数约 200 / 留白约 18%
- description: 标题「人脸识别不上云：三个理由与一个代价」。左栏"上云方案"：延迟受网络支配、人脸特征出设备、断网即失效；右栏"端侧方案"：MFN 1.30MB + MSR/MNP，单次推理 324ms，特征存 SD 卡（face_db，双世代 + CRC），断网可打卡。中间 VS。底部结论条：「代价是 1.5MB 模型与一块 PSRAM —— 换来隐私与离线可用，这笔交易在人脸场景成立，在对话场景不成立。」
- anti_pattern: 禁止两栏文字量不等导致高低错位；禁止用"行业领先"这类形容词

### P10 章节页 03
- type: section / role: transition / rhythm: transition / layout: 同 P3（编号 03）
- description: 「03 云端大脑与端云协同」+ 「四层能力栈、11 种帧的契约，以及一次回合里两端各做什么」。

### P11 云端四层能力栈（★）
- type: content / role: hero / rhythm: peak / layout: 分层堆叠（L1-L4 自下而上，每层左标号 + 名称 + 组件，右侧一列"失败时的降级"）
- visual: SVG 四层栈，本层高 96px，层间 8px
- visual_role: anchor
- density: 字数约 220 / 留白约 12%
- description: L1 接入 pet-gateway（/ws · /action · /caps · /media · /upload · /push · /health）；L2 编排 AstrBot v4.29.1 + 19 插件；L3 智力 deepseek-flash / SenseVoiceSmall / qwen-audio-3.1-tts-flash / 多模态 VL / embedding；L4 内容 music_proxy（自建网易云 + 2 兜底源）+ alist。右侧降级列：TTS 挂 → 只掉语音；AstrBot 重启 → 重试一次；回复超长 → TTS 截 200 字、文字全文。
- anti_pattern: 禁止画成"一个模型包打天下"；禁止把 L3 写成自研模型

### P12 端云契约：11 种帧
- type: content / role: supporting / rhythm: valley / layout: 上标题 + 左侧说明窄栏（26%）+ 右侧帧表（70%）
- visual: Table
- visual_role: evidence
- density: 字数约 240 / 表格 1 张 / 留白约 12%
- description: 标题「11 种帧，就是两端之间的全部协议」。表格四列：帧 / 方向 / 作用 / 设备落点。11 行：caps↑、pcm↑、text↑↓、event.action_result↑、image_req↑、action↓、system↓、emotion↓、audio↓、image↓、error↓。左栏点出三条设计纪律：单行 JSON、大对象走 URL 不传二进制、相对 URL 由设备补 base。
- anti_pattern: 禁止裸表格无容器；禁止把 11 种帧拆成多页

### P13 一次点歌的完整时序
- type: content / role: supporting / rhythm: valley / layout: 横向流程链（6 步，箭头连接）+ 底部两条关键设计
- visual: SVG 流程链
- visual_role: anchor
- density: 字数约 200 / 留白约 15%
- description: 六步：① PTT 录音上行 → ② 网关先回 thinking（ASR+LLM 要 9–18s）→ ③ SenseVoice 转写 + 回显 → ④ deepseek-flash 决定调 pet_music → ⑤ 网关 /action 下发，设备本地曲库优先、否则 music_proxy 搜索起播 → ⑥ 回执真实结论，且只回文字不出 TTS（否则顶掉音乐）。底部两条：「先回 thinking 再慢慢答」「拿到回执才说话，不让模型编成功」。
- anti_pattern: 禁止把六步写成无连接的卡片横排；禁止省略回执这一环

### P14 双入口：确定性 vs 大模型
- type: content / role: supporting / rhythm: valley / layout: 对比式（左右两栏 + 顶部判定分支图）
- visual: 顶部分支 SVG + 下方四行维度对比
- visual_role: evidence
- density: 字数约 190 / 留白约 20%
- description: 标题「一句话进来，先走确定性，再走模型」。顶部一条分支：「设备文本 → 命中设备自述别名？→ 是：直接下发 action（0 token）／否：交 LLM 决策」。下方四行对比：覆盖范围、成本、延迟、风险。结论句：「打卡、拍照这类有副作用的动作只走确定性通道 —— 模型在工具失败时会编一句成功。」
- anti_pattern: 禁止把两条通道画成对等；禁止两边用同色系难分

### P15 工程边界：4 KB 内部 RAM 下的取舍
- type: content / role: supporting / rhythm: valley / layout: 数据看板（左一个大数字，右侧三个小数据 + 三条洞察）
- visual: 巨型数字 SVG + 三个 KPI
- visual_role: anchor
- density: 字数约 180 / 留白约 20%
- description: 主数字「7 KB」= 内部 RAM 最大连续块（打卡会话实测 `free=7259 largest=7168`，WiFi DMA 独占 51KB 挪不走）。右侧三个：8 MB PSRAM / 7,168 B 动作任务栈 / 61,953 行设备端 C 代码（137 文件）。三条洞察：判内存只看 largest 不看 free（关键短任务栈改走编译期 12 KB 静态池）；碰 flash 的任务栈必须内部 RAM；云端把图片归一化，省下设备端一条解码分支。结论：边缘设备的边界是内存与带宽，不是 CPU。
- anti_pattern: 禁止把四个数字做成等大卡片；禁止只给数字不给判断

### P16 结尾
- type: ending / role: hero / rhythm: peak / layout: 收束金句 + 落款
- visual: 深色底 #0F172A + 屏幕实拍（`docs/hardware/tex_screen.jpg`）整页铺底，上盖 84% 遮罩 + 一句大字
- visual_role: atmosphere
- density: 字数约 50 / 留白约 55%
- description: 金句「身体负责在场，大脑负责判断 —— 中间只隔一条连接。」下方落款「S9 桌宠伴侣 · pet_companion · ESP32-S31 + AstrBot」。无页脚。
- anti_pattern: 禁止只写"谢谢"；禁止放联系方式空占位
