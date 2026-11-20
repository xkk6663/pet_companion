# -*- coding: utf-8 -*-
"""docs/assets/svg/ 下的 10 张架构图的生成器。

用法：python docs/assets/svg/gen_svgs.py
改图改这里，重跑即可覆盖同名 SVG；不用手改 XML。
所有图形都是「写死坐标 + 统一配色」的 SVG DSL 拼出来的，没有外部依赖（不装 graphviz）。
"""
import math, os

OUT = os.path.dirname(os.path.abspath(__file__))
os.makedirs(OUT, exist_ok=True)

FONT = "'Segoe UI','Microsoft YaHei','PingFang SC','Noto Sans CJK SC',sans-serif"
MONO = "'Cascadia Mono','Consolas','Courier New',monospace"

NAVY, MUT, LINE = '#0f172a', '#64748b', '#cbd5e1'
NEU_BG, NEU_BD = '#f8fafc', '#cbd5e1'
CLOUD = ('#eff6ff', '#93c5fd', '#1e40af')
EDGE = ('#ecfdf5', '#6ee7b7', '#065f46')
TRANS = ('#fffbeb', '#fcd34d', '#92400e')
UI = ('#f5f3ff', '#c4b5fd', '#5b21b6')
EXEC = ('#fef2f2', '#fca5a5', '#991b1b')
NETC = ('#f0f9ff', '#7dd3fc', '#075985')
GREY = ('#f1f5f9', '#cbd5e1', '#334155')
OK = ('#f0fdf4', '#86efac', '#166534')


def esc(t):
    return str(t).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def T(x, y, s, fs=13, fill=NAVY, bold=False, anchor='start', mono=False):
    fam = MONO if mono else FONT
    w = '600' if bold else '400'
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
            f'font-weight="{w}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def R(x, y, w, h, fill, stroke, rx=8, sw=1.2, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def BOX(x, y, w, h, title, lines=(), c=GREY, fs=13, lfs=12, pad=12, rx=10, badge=None):
    fill, stroke, tx = c
    out = [R(x, y, w, h, fill, stroke, rx)]
    cy = y + 26
    if badge:
        out.append(R(x + pad, y + 10, 22, 20, '#ffffff', stroke, rx=6, sw=1.0))
        out.append(T(x + pad + 11, y + 25, badge, 12, tx, bold=True, anchor='middle'))
        out.append(T(x + pad + 30, cy, title, fs, tx, bold=True))
    else:
        out.append(T(x + pad, cy, title, fs, tx, bold=True))
    cy += 20
    for ln in lines:
        out.append(T(x + pad, cy, ln, lfs, '#475569'))
        cy += lfs * 1.5
    return ''.join(out)


def ARROW(pts, color='#94a3b8', w=1.6, dash=None, head=9):
    d = ' M ' + ' L '.join(f'{p[0]:.1f},{p[1]:.1f}' for p in pts)
    da = f' stroke-dasharray="{dash}"' if dash else ''
    body = f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{w}"{da}/>'
    (x1, y1), (x2, y2) = pts[-2], pts[-1]
    ang = math.atan2(y2 - y1, x2 - x1)
    p1 = (x2 - head * math.cos(ang - 0.42), y2 - head * math.sin(ang - 0.42))
    p2 = (x2 - head * math.cos(ang + 0.42), y2 - head * math.sin(ang + 0.42))
    tip = f'<polygon points="{x2:.1f},{y2:.1f} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{color}"/>'
    return body + tip


def A2(x1, y1, x2, y2, **kw):
    return ARROW([(x1, y1), (x2, y2)], **kw)


def BIDIR(x1, y1, x2, y2, color='#94a3b8', w=1.4, head=8):
    a1 = math.atan2(y2 - y1, x2 - x1)
    a2 = math.atan2(y1 - y2, x1 - x2)
    out = f'<path d="M {x1:.1f},{y1:.1f} L {x2:.1f},{y2:.1f}" stroke="{color}" stroke-width="{w}" fill="none"/>'
    for (sx, sy, ang) in ((x2, y2, a1), (x1, y1, a2)):
        p1 = (sx - head * math.cos(ang - 0.42), sy - head * math.sin(ang - 0.42))
        p2 = (sx - head * math.cos(ang + 0.42), sy - head * math.sin(ang + 0.42))
        out += f'<polygon points="{sx:.1f},{sy:.1f} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{color}"/>'
    return out


def LCHIP(cx, cy, text, color='#2563eb', fs=10.5):
    """白底小标签，压在线段上但不被线干扰"""
    w = len(text) * fs * 1.02 + 14
    h = 20
    return (R(cx - w / 2, cy - h / 2, w, h, '#ffffff', color, rx=5, sw=1.0)
            + T(cx, cy + 4, text, fs, color, anchor='middle', bold=True))


def chip(x, y, text, c=GREY, fs=12, padx=10, h=26, mono=False):
    w = (len(text) * (fs * 0.62 if mono else fs * 1.02)) + padx * 2
    fill, stroke, tx = c
    return R(x, y, w, h, fill, stroke, rx=h / 2, sw=1.0) + T(x + w / 2, y + h * 0.68, text, fs, tx, anchor='middle', mono=mono), w


def svg(name, w, h, body, title=None, subtitle=None):
    head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}" role="img">')
    head += f'<title>{esc(title or name)}</title>'
    head += f'<rect x="0" y="0" width="{w}" height="{h}" fill="#ffffff"/>'
    head += f'<rect x="6" y="6" width="{w-12}" height="{h-12}" rx="12" fill="none" stroke="#e2e8f0" stroke-width="1"/>'
    pre = ''
    if title:
        pre += T(28, 38, title, 17, NAVY, bold=True)
        if subtitle:
            pre += T(28, 60, subtitle, 12, MUT)
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(head + pre + body + '</svg>\n')
    return name


# ============================================================ 1 系统全景
def d_system_overview():
    W, H = 1040, 700
    b = [R(48, 76, 944, 232, CLOUD[0], CLOUD[1], rx=12),
         T(68, 104, '云  端 · 智力', 14, CLOUD[2], bold=True),
         T(830, 104, '四层能力栈 + 一个网关', 12, MUT)]
    y = 120
    b += [R(68, y, 300, 44, '#ffffff', '#bfdbfe', rx=8), T(78, y + 18, 'L3 智力', 11, CLOUD[2], bold=True),
          T(78, y + 35, 'ASR ─▶ LLM ─▶ TTS', 12, '#334155'),
          R(380, y, 190, 44, '#ffffff', '#bfdbfe', rx=8), T(390, y + 18, '工具回灌', 11, CLOUD[2], bold=True),
          T(390, y + 35, 'function calling 真实结论', 12, '#334155'),
          R(582, y, 390, 44, '#ffffff', '#bfdbfe', rx=8), T(592, y + 18, 'VL / Embedding', 11, CLOUD[2], bold=True),
          T(592, y + 35, '看图 · 记忆检索 · 表情包语义', 12, '#334155')]
    y = 176
    b += [R(68, y, 400, 44, '#ffffff', '#bfdbfe', rx=8), T(78, y + 18, 'L2 编排', 11, CLOUD[2], bold=True),
          T(78, y + 35, 'AstrBot v4.29.1 · 插件体系', 12, '#334155'),
          R(480, y, 492, 44, '#ffffff', '#bfdbfe', rx=8), T(490, y + 18, '插件', 11, CLOUD[2], bold=True),
          T(490, y + 35, 'pet_device · livingmemory · omnidraw · music · proactive_chat', 11.5, '#334155')]
    y = 232
    b += [R(68, y, 904, 58, '#ffffff', '#93c5fd', rx=8, sw=1.4),
          T(78, y + 20, 'L1 接入   pet-gateway :8081（FastAPI + systemd，唯一入口）', 12, CLOUD[2], bold=True),
          T(78, y + 40, '/ws/{sid} 长连接 · /action 动作中继 · /caps 能力查询 · /media 出图 · /push 主动推送 · /upload 原图', 11.5, '#334155')]

    b += [R(48, 322, 944, 56, TRANS[0], TRANS[1], rx=10),
          T(68, 344, '一条 WebSocket 长连接（设备永远主动外连 —— 云端借此穿透 NAT）', 13, TRANS[2], bold=True),
          T(68, 366, '上行 text / pcm / caps / action_result / barge_in       下行 text / emotion / audio / image / action / system / error', 11.5, '#78350f'),
          BIDIR(520, 300, 520, 322, color='#b45309', w=1.6), BIDIR(520, 378, 520, 394, color='#b45309', w=1.6)]

    b += [R(48, 394, 944, 238, EDGE[0], EDGE[1], rx=12),
          T(68, 422, '边 缘 端 · 身体（本仓库）', 14, EDGE[2], bold=True),
          T(748, 422, 'ESP32-S31 · 240 MHz · 16 MB Flash + 8 MB PSRAM', 11.5, MUT)]
    rows = [('L5 表达', 'ui/  LVGL 桌宠浮窗 · 6 个 tab（CAM/VIDEO/FILES/MUSIC/REC/SYS）· bin 字体', UI),
            ('L4 会话', 'xiaozhi_service  会话状态机 · codec 租借 · TTS(pri=2) / 音乐(pri=1) 双通道', UI),
            ('L3 执行', 'ai_action + 6 个业务模块  15 个 MCP 工具 + 1 个纯本地动作（wallpaper）', EXEC),
            ('L2 解码', 'event_bus  7 种下行帧分发（收包线程只入队，绝不阻塞）', NETC),
            ('L1 传输', 'ws_client  单连接 · 1s→30s 指数退避 · 按 JSON 括号切帧', TRANS),
            ('L0 平台', 'platform/  board_map + port_display / port_storage / port_time / port_bus', EDGE)]
    y = 436
    for name, desc, c in rows:
        b += [R(64, y, 912, 26, c[0], c[1], rx=6, sw=1.0), T(76, y + 18, name, 11.5, c[2], bold=True),
              T(142, y + 18, desc, 11.5, '#334155')]
        y += 30
    b.append(T(64, 650, '设备负责「在场」，云端负责「理解」：设备端不跑任何大模型，唯一例外是人脸检测/识别两个端侧小模型。', 11.5, MUT))
    return svg('system-overview.svg', W, H, ''.join(b), 'pet_companion 系统全景',
               '上面是智力的四层栈，下面是身体的六层栈，中间只隔一条设备主动外连的 WebSocket')


# ============================================================ 2 接入拓扑
def d_gateway_topology():
    W, H = 920, 640
    b = [R(40, 84, 840, 152, '#f8fafc', '#cbd5e1', rx=12, dash='5 4'),
         T(58, 106, '云服务器', 12.5, MUT, bold=True)]
    b.append(BOX(70, 118, 330, 100, 'AstrBot 容器 :6185', ['人格「大肥鱼」+ 插件体系', 'Open API /api/v1/chat（SSE）'], CLOUD, fs=12.5))
    b.append(BOX(520, 118, 330, 100, 'AstrBot 插件 pet_device', ['把设备动作注册成 LLM 工具', 'POST /action · POST /push'], CLOUD, fs=12.5))

    b.append(A2(235, 218, 235, 286, color='#2563eb', w=1.6))
    b.append(T(247, 258, 'POST /api/v1/chat（SSE）+ Bearer abk_…', 10.5, '#1e40af'))
    b.append(A2(685, 218, 685, 286, color='#2563eb', w=1.6))
    b.append(T(697, 258, 'POST /action · /push', 10.5, '#1e40af'))

    b.append(BOX(140, 286, 640, 96, 'pet-gateway :8081', ['设备唯一入口 · 动作中继 · 媒体归一化 · 能力查询',
                                                        'FastAPI + systemd 常驻，MemoryMax=256M 防 OOM 拖垮宿主'], NETC, fs=12.5))
    b.append(T(438, 398, '↑ 反向往上：GET /media · /static 取 TTS 音频与图片', 10, '#0e7490', anchor='end'))

    b += [R(140, 408, 640, 52, TRANS[0], TRANS[1], rx=10),
          T(156, 430, 'WS /ws/xz-s31-<mac4>   设备永远主动外连，云端借此穿透 NAT', 12, TRANS[2], bold=True),
          T(156, 450, '↑ text / pcm / caps / action_result / barge_in     ↓ text / emotion / audio / image / action / system / error', 10.5, '#78350f')]
    b.append(BIDIR(460, 382, 460, 408, color='#b45309', w=1.6))
    b.append(BIDIR(460, 460, 460, 486, color='#b45309', w=1.6))

    b.append(BOX(180, 486, 560, 92, 'ESP32-S31 设备', ['ws_client → event_bus → ai_action → 硬件',
                                                   '设备只连网关，不直连 AstrBot：AstrBot 的语言由网关翻译'], EDGE, fs=12.5))
    b.append(T(44, 610, '三段职责：AstrBot = 大脑（人格 / 记忆 / 决定调哪个工具）；网关 = 翻译 + 反向通道；ESP32 = 手脚 + 感官（执行并回执真实结论）。', 10.5, MUT))
    return svg('gateway-topology.svg', W, H, ''.join(b), '接入拓扑：设备 → pet-gateway → AstrBot',
               '一个入口、一条长连接、三段职责')


# ============================================================ 3 两条切分线
def d_two_axis_layers():
    W, H = 1040, 620
    b = [T(48, 96, '纵向：端 ↔ 云（五层接入模型）', 13.5, NAVY, bold=True),
         T(560, 96, '横向：设备内部（四层 + 组合根）', 13.5, NAVY, bold=True)]
    lv = [('L5', '应用编排', '设备：会话状态机   |   云端：AstrBot 插件体系', UI),
          ('L4', '能力协商', '设备 caps 上行（我会什么） ⟷ 云端 tools 注册（该调什么）', UI),
          ('L3', '指令通道', '云端 action 下行  /  设备 action_result 回执（双向同步）', EXEC),
          ('L2', '内容通道', 'text / emotion / audio(URL) / image(URL) 下行；HTTP 拉大对象', NETC),
          ('L1', '传输', '单条 WebSocket + 指数退避重连 + JSON 帧', TRANS)]
    y = 112
    for tag, name, desc, c in lv:
        b += [R(48, y, 460, 58, c[0], c[1], rx=8), T(62, y + 22, tag, 11.5, c[2], bold=True, mono=True),
              T(100, y + 22, name, 13, c[2], bold=True), T(100, y + 44, desc, 11.5, '#334155')]
        y += 64
    hz = [('ui/', 'LVGL 页面 · 桌宠浮窗 · bin 字体（只能通过队列/回调找下层）', UI),
          ('app/', 'main.c：纯初始化编排（S1~S6），写业务逻辑属违规', GREY),
          ('middleware/', 'ai · camera · audio · media · net · periph · face_db · bluetooth', EDGE),
          ('platform/', 'board_map + port_display / port_storage / port_time / port_bus（L0）', EDGE),
          ('components/', 'esp_xiaozhi（MCP + 音频管线）· esp_littlefs · 蓝牙 · 手势', GREY)]
    y = 112
    for name, desc, c in hz:
        b += [R(560, y, 432, 52, c[0], c[1], rx=8), T(574, y + 21, name, 12.5, c[2], bold=True, mono=True),
              T(574, y + 40, desc, 11, '#334155')]
        y += 58
    b += [A2(830, 402, 830, 424, color='#94a3b8'), T(844, 418, '依赖只许向下（L4 → L0）', 11, MUT)]

    b += [R(48, 448, 944, 96, '#fff7ed', '#fdba74', rx=10),
          T(64, 472, '两条铁律（写进 CI 的 G1~G6，见 01_工程分层架构.md）', 12.5, '#9a3412', bold=True),
          T(64, 496, '① 中间层不许 #include "lvgl.h"；UI 不许直接 include bsp/esp-bsp.h、esp_lcd_*、driver/gpio.h（几何量走 platform/board_map.h）。', 11.5, '#7c2d12'),
          T(64, 518, '② 反向依赖一律改注册表（固定槽、禁 malloc）/ 回调表（xxx_ops_t）/ 队列与事件位 —— 不许直接 include 上层头。', 11.5, '#7c2d12'),
          T(64, 538, '现在：55 处跨层违规 → 27 条存量基线（14 文件），UI 侧 BSP 依赖已清零，新增违规 CI 直接失败。', 11, MUT)]
    b.append(T(48, 580, '横向四层的意义：改 UI 不必动 middleware，改驱动不必动 ai；纵向五层的意义：任何一次对话失败都能定位到「传输/解码/执行/会话/表达」中的一级。', 11, MUT))
    return svg('two-axis-layers.svg', W, H, ''.join(b), '两条切分线：纵向端云五层，横向设备四层',
               '看懂这张图，就看懂了这个工程为什么这么分层')


# ============================================================ 4 设备端五级链路
def d_device_chain():
    W, H = 1020, 700
    b = []
    steps = [('①', '传输层', 'ws_client.c',
              ['唯一长连接：supervisor 任务守护，1s→30s 指数退避重连', '收包按 JSON 对象括号配对切帧（不依赖 \\n 或 fin）'],
              TRANS, '禁止阻塞 IO'),
             ('②', '解码层', 'event_bus.c',
              ['拆成 7 种帧：text / emotion / audio / image / action / system / error', '收包任务里只入队不阻塞，处理放在各自消费者任务'],
              NETC, '收包只入队'),
             ('③', '执行层', 'ai_action.c',
              ['动作队列 worker（栈 7168 B）：参数模板合并 → MCP tools/call 或本地直调',
               '6 个业务模块（punch / camera / music / speaker / light / device）共 15 个 MCP 工具',
               '表里没有的动作当场报错，不空转一圈'],
              EXEC, '栈必须内部 RAM'),
             ('④', '会话层', 'xiaozhi_service.c',
              ['会话状态机 + codec 独占租借 + 双播放通道（TTS pri=2 抢占 / 音乐 pri=1 让位）',
               'audio policy 做「暂停-续播」：TTS 结束音乐自动接回，用户听不出被切断'],
              UI, 'codec 三方互斥'),
             ('⑤', '表达层', 'ui/',
              ['pet_overlay 桌宠浮窗 · tab CAM/VIDEO/FILES/MUSIC/REC/SYS · overlay_manager 浮层调度',
               'ui_binfont 全 CJK bin 字体（字模在 littlefs，不进固件）'],
              UI, '只有 LVGL 线程碰 LVGL')]
    y = 78
    for num, name, fname, lines, c, caveat in steps:
        h = 54 + 18 * len(lines)
        b += [R(48, y, 800, h, c[0], c[1], rx=10),
              R(60, y + 12, 26, 22, '#ffffff', c[1], rx=6, sw=1.0),
              T(73, y + 27, num, 13, c[2], bold=True, anchor='middle'),
              T(98, y + 28, name, 13, c[2], bold=True), T(160, y + 28, fname, 11.5, '#475569', mono=True)]
        for i, ln in enumerate(lines):
            b.append(T(66, y + 50 + i * 18, ln, 11.5, '#334155'))
        b += [R(860, y + 10, 130, 26, '#ffffff', '#e2e8f0', rx=6, sw=1.0),
              T(925, y + 27, caveat, 10, c[2], anchor='middle', bold=True)]
        if num != '⑤':
            b.append(A2(448, y + h, 448, y + h + 14, color='#cbd5e1', w=1.2, head=7))
        y += h + 16
    b.append(T(48, 660, '改动定位法：任意一次「语音没反应 / 动作没执行 / 表情不动」，先按 ①~⑤ 判断断在哪一级，而不是从 UI 往下猜。', 11.5, MUT))
    return svg('device-chain.svg', W, H, ''.join(b), '设备端五级链路：一次对话在设备内走过的五层',
               'main/middleware/ai/ 内部的分层，右侧是每一级的硬约束')


# ============================================================ 5 云端四层栈
def d_cloud_stack():
    W, H = 1000, 600
    b = [R(48, 74, 904, 96, CLOUD[0], CLOUD[1], rx=10),
         T(64, 100, 'L4 内容层', 13, CLOUD[2], bold=True),
         T(64, 122, 'music_proxy :8080 —— 自建网易云 API + 2 个兜底源', 11.5, '#334155'),
         T(64, 142, '契约 GET /v1/search · /v1/url · /v1/lyric · /v1/pic · /v1/download', 11.5, '#334155'),
         T(64, 160, 'alist :5244 —— 表情包 / 素材检索后端（HTTP 直取，不经网关）', 11.5, '#334155'),
         R(48, 180, 904, 136, CLOUD[0], CLOUD[1], rx=10),
         T(64, 206, 'L3 智力层（外部 API，云端只做路由与降级）', 13, CLOUD[2], bold=True)]
    items = [('LLM', 'deepseek-flash', '对话 / 工具决策'), ('ASR', 'SenseVoiceSmall', 'OpenAI 兼容'),
             ('TTS', 'qwen-audio-3.1', '少女音 WAV 24k'), ('VL', '多模态消息段', 'image 段看图'),
             ('Embed', 'openai_embedding', '记忆 / 语义检索')]
    x = 64
    for tag, name, desc in items:
        b += [R(x, 220, 168, 72, '#ffffff', '#bfdbfe', rx=8), T(x + 10, 242, tag, 11.5, CLOUD[2], bold=True, mono=True),
              T(x + 10, 260, name, 11, '#334155', mono=True), T(x + 10, 280, desc, 10.5, MUT)]
        x += 178
    b += [R(48, 332, 904, 92, CLOUD[0], CLOUD[1], rx=10), T(64, 358, 'L2 编排层', 13, CLOUD[2], bold=True),
          T(64, 380, 'AstrBot v4.29.1（容器 :6185，Open API /api/v1/chat SSE）', 11.5, '#334155'),
          T(64, 400, '插件：pet_device(工具注册) · livingmemory(记忆) · omnidraw(生图) · music · proactive_chat(主动关怀) · parser …', 11, '#334155'),
          R(48, 440, 904, 112, NETC[0], NETC[1], rx=10),
          T(64, 466, 'L1 接入层   pet-gateway（FastAPI :8081，systemd 常驻，MemoryMax=256M）', 13, NETC[2], bold=True),
          T(64, 490, '/ws/{sid} 设备长连接 · /action 工具中继 · /caps 能力查询 · /media/{name} 媒体出口', 11.5, '#334155'),
          T(64, 510, '/upload/{sid} 图片入口 · /push 主动推送 · /health 健康检查   —— 共 7 个入口', 11.5, '#334155'),
          T(64, 534, '为什么必须有这一层：协议收敛 + 媒体归一化 + NAT 穿透 + 降级兜底（设备直连 AstrBot 这四件事全落空）', 11, MUT),
          T(48, 580, '四层 + 一个网关：云端不是「一个大模型」，而是「能力栈 + 唯一入口」。设备只认识网关，网关认识 AstrBot。', 11.5, MUT)]
    return svg('cloud-stack.svg', W, H, ''.join(b), '云端四层能力栈 + 一个必须存在的网关', '实测在线：GET /health → version 0.8.1')


# ============================================================ 6 语音点歌时序
def d_voice_sequence():
    W, H = 1020, 800
    lanes = [('用户', 110), ('设备 ESP32', 340), ('网关 :8081', 640), ('云端 AstrBot', 900)]
    top, bot = 96, 700
    b = []
    for name, x in lanes:
        b.append(R(x - 58, top, 116, 26, '#f1f5f9', '#cbd5e1', rx=6, sw=1.0))
        b.append(T(x, top + 17, name, 12, NAVY, bold=True, anchor='middle'))
        b.append(f'<path d="M {x},{top+28} L {x},{bot}" stroke="#cbd5e1" stroke-width="1.1" stroke-dasharray="4 4"/>')

    def msg(y, x1, x2, text, color='#2563eb', num=None, above=16, fs=11.5):
        out = A2(x1, y, x2, y, color=color, w=1.6)
        if num:
            out += R(min(x1, x2) - 34, y - 10, 22, 20, '#ffffff', color, rx=6, sw=1.0)
            out += T(min(x1, x2) - 23, y + 4, num, 11, color, bold=True, anchor='middle')
        cx = (x1 + x2) / 2
        if num:
            cx += 14
        out += T(cx, y - above + 4, text, fs, color, anchor='middle')
        return out

    def note(cx, y, lines, color='#475569', w=300):
        h = 12 + 15 * len(lines)
        out = R(cx - w / 2, y, w, h, '#f8fafc', '#dbeafe', rx=8, sw=1.0, dash='4 3')
        for i, ln in enumerate(lines):
            out += T(cx - w / 2 + 10, y + 18 + i * 15, ln, 10.5, color)
        return out, h

    b.append(msg(140, 130, 320, '按下 PTT 说「放首晴天」', '#475569'))
    b.append(msg(180, 360, 620, 'pcm 16k base64 上行', '#b45309'))
    b.append(msg(228, 620, 360, 'emotion:thinking（ASR+LLM 要 9~18 s，不能静默）', '#1e40af', num='1', fs=10.5))
    b.append(note(640, 252, ['② PCM 封 WAV → SenseVoice（ASR）→ 文本', '③ 回显 text(role=0)：用户看到「放首晴天」'], w=330)[0])
    b.append(msg(320, 660, 880, 'POST /api/v1/chat（SSE 增量拼接）', '#1e40af', num='4', fs=11))
    b.append(note(900, 344, ['LLM(deepseek-flash) 决定', '调用 pet_music'], w=200)[0])
    b.append(msg(420, 880, 660, '插件 POST /action  {music, 晴天}', '#1e40af', num='5'))
    b.append(msg(470, 620, 360, 'action 帧 id=7 name=music args{name:晴天}', '#b45309', fs=11))
    b.append(note(360, 494, ['ai_action 查动作表 → do_music()',
                             '① 本地曲库命中 → 直接播（离线可用）',
                             '② 未命中 → GET music_proxy /v1/search → /v1/url → 起播',
                             '③ 成功 → 异步落盘 /sdcard/music/<歌手>-<歌名>.mp3 + .lrc'], w=380)[0])
    b.append(msg(620, 360, 620, 'action_result ok=1 data=「晴天」', '#059669', fs=11))
    b.append(msg(670, 660, 880, '拿到真实回执才回话：只回文字、不出 TTS', '#1e40af', num='6', fs=10.5))

    b += [R(60, 700, 900, 60, OK[0], OK[1], rx=10),
          T(78, 722, '结果：喇叭出声 + 屏幕显示歌名 + SD 卡多一首歌', 12.5, OK[2], bold=True),
          T(78, 744, '⑦ 点歌成功后 10 s 内网关强制只回文字不出 TTS —— 否则那句「行，放《晴天》」会立刻顶掉音乐', 11, '#166534')]
    return svg('voice-song-sequence.svg', W, H, ''.join(b), '一次语音点歌的完整时序（走 LLM 工具调用 + 反向 relay）',
               '设备只负责上行与执行，云端负责理解与决定，结论以设备回执为准')


# ============================================================ 7 双入口调度
def d_dual_dispatch():
    W, H = 920, 500
    b = [R(300, 86, 320, 44, GREY[0], GREY[1], rx=8),
         T(460, 113, '设备文本（语音转写 / 屏幕键盘）', 12, '#334155', anchor='middle')]
    b.append(A2(460, 130, 460, 158, color='#94a3b8'))
    b.append(R(350, 158, 220, 46, '#fefce8', '#fde047', rx=8))
    b.append(T(460, 186, 'match_intent()  遍历 caps aliases', 11.5, '#854d0e', bold=True, anchor='middle'))

    b.append(A2(390, 204, 250, 268, color='#059669', w=1.7))
    b.append(LCHIP(300, 236, '命中 aliases', '#059669'))
    b.append(A2(530, 204, 670, 268, color='#2563eb', w=1.7))
    b.append(LCHIP(650, 236, '不命中', '#2563eb'))

    b.append(BOX(80, 268, 340, 116, '关键词快速路（默认）', ['直接下发 action 帧，不进 LLM', '延迟 = 一次动作 RTT，0 token',
                                                   '打卡 / 拍照 / 点歌 / 换壁纸等有副作用动作'], OK, fs=12.5))
    b.append(BOX(520, 268, 330, 116, 'LLM 工具调用', ['文本进 AstrBot → 模型自己决定调 pet_<name>', '插件 POST /action → 网关 relay → 设备',
                                                   '任意自然语言（「我今天到公司了，记一下考勤」）'], CLOUD, fs=12.5))
    b.append(A2(250, 384, 380, 424, color='#94a3b8', w=1.4))
    b.append(A2(685, 384, 540, 424, color='#94a3b8', w=1.4))
    b.append(R(300, 424, 300, 42, NEU_BG, NEU_BD, rx=8))
    b.append(T(450, 450, '设备执行 → action_result 回执', 11.5, '#334155', anchor='middle', bold=True))
    b.append(T(48, 484, '为什么打卡不让模型决定：模型在工具失败时会「编一句成功」，用户以为打了卡、实际没打 —— 结论只认设备回执。', 11, MUT))
    return svg('dual-dispatch.svg', W, H, ''.join(b), '双入口调度：先试确定性匹配，不命中才交 LLM',
               '有副作用的动作走高确定性通道，绝不交给模型幻觉')


# ============================================================ 8 caps 协议
def d_caps_flow():
    W, H = 980, 420
    b = [BOX(48, 92, 300, 176, '① 固件（真相在这里）', ['ai_action_report_caps()', '· 遍历 MCP tools/list：15 个工具的真实 schema',
                                                  '· 用 ai_action.c 的 TOOLS[] 覆盖文案', '   （desc / label / aliases / arg_key）',
                                                  '· 补一个引擎里没有的纯本地动作 wallpaper', '· 拼成 12 KB JSON'], EDGE, fs=12, lfs=11.5)]
    b.append(BOX(392, 92, 250, 130, '② 网关', ['存进 SESSIONS[sid].caps', 'GET /caps 供插件查询', '断线重连后设备自动重报'], NETC, fs=12))
    b.append(BOX(686, 92, 250, 190, '③ AstrBot 插件', ['每 45 s GET /caps', '· 注册成 pet_<name> 的 LLM 工具', '· 参数结构与固件同构，零转换',
                                                  '· 按签名判断是否重注册', '· 能力缩水就 remove_func() 下线'], CLOUD, fs=12, lfs=11.5))
    b.append(A2(348, 148, 390, 148, color='#0e7490', w=1.6))
    b.append(A2(642, 148, 684, 148, color='#2563eb', w=1.6))
    b.append(A2(811, 282, 811, 316, color='#2563eb', w=1.6))
    b.append(R(646, 316, 330, 62, NEU_BG, NEU_BD, rx=8))
    b.append(T(660, 338, 'LLM 看到「这台设备有 16 个工具」', 11, '#334155'))
    b.append(T(660, 358, '加一个设备动作 = 只改固件，网关与插件零改动', 11, '#334155'))
    b.append(T(48, 356, '链路：设备连上 / 重连即上报 caps（WS 上行）→ 网关缓存 → 插件每 45 s GET /caps 同步成 LLM 工具。', 11, MUT))
    b.append(T(48, 396, '旧固件不上报 caps 时，插件回落到内置 _DEFAULT_TOOLS，不会失能。', 11, MUT))
    return svg('caps-flow.svg', W, H, ''.join(b), '能力自述协议 caps：设备会什么，唯一真相在固件里',
               '从固件自述到 LLM 工具清单的三步')


# ============================================================ 9 媒体链路
def d_media_pipeline():
    W, H = 1000, 440
    b = [T(48, 92, '图片下行（表情包 / 配图）：难点不在「传」，在两端的解码能力不一致', 12.5, NAVY, bold=True)]
    xs = [(48, 176, '云插件产出', ['GIF 500×500', 'PNG 1024×1024']),
          (240, 176, '网关 normalize_image()', ['合规 → 原名直发', '不合规 → Pillow 转 RGB', '等比缩 → JPEG q85']),
          (436, 176, '下行帧', ['{"type":"image",', ' "url":"/media/x__n.jpg"}']),
          (632, 176, '设备硬解上屏', ['下载 → 硬件 JPEG 解码', '→ 浮窗；落盘只留 32 张'])]
    for x, w, title, lines in xs:
        b.append(BOX(x, 104, w, 104, title, lines, NETC, fs=11.5, lfs=11, pad=10))
    for x in (224, 416, 612):
        b.append(A2(x, 156, x + 14, 156, color='#0e7490', w=1.4, head=7))
    b.append(T(48, 228, '设备侧硬约束：≤ 1M 像素 · ≤ 1200 KB · 只支持 baseline-DCT —— 云端产出什么格式都得先过网关', 11, MUT))

    b.append(T(48, 268, '音频下行（TTS 与音乐共存）：靠 audio policy 的优先级，不靠代码轮询', 12.5, NAVY, bold=True))
    b.append(R(48, 282, 904, 42, UI[0], UI[1], rx=8))
    b.append(T(64, 309, 'pri=2  TTS 通道     网关 {"type":"audio"} → xiaozhi_service', 12, UI[2], bold=True))
    b.append(R(48, 330, 904, 42, EDGE[0], EDGE[1], rx=8))
    b.append(T(64, 357, 'pri=1  音乐通道     本地曲库 / 云端音源 → media_player', 12, EDGE[2], bold=True))
    b.append(T(930, 309, '大者胜出出声', 9.5, MUT, anchor='end'))
    b.append(T(930, 357, '低者静音、进度照走', 9.5, MUT, anchor='end'))
    b.append(R(48, 384, 904, 46, '#f8fafc', '#e2e8f0', rx=8))
    b.append(T(64, 406, 'TTS 起播 → 自动暂停音乐（保留播放位置）；TTS 结束 → 自动续播音乐，用户听不出「被切断」', 11.5, '#334155'))
    b.append(T(64, 424, '这就是「说话压歌」零代码轮询的全部实现：交给框架的 av_processor 优先级机制。', 11, MUT))
    return svg('media-pipeline.svg', W, H, ''.join(b), '媒体链路的端云协同：图片归一化 + 音频双通道',
               '两条链路的难点都是两端能力边界不一致，而不是带宽')


# ============================================================ 10 分层模型
def d_layering_model():
    W, H = 940, 610
    b = [R(140, 78, 680, 40, '#ffffff', '#94a3b8', rx=8, dash='5 4'),
         T(158, 103, 'app（组合根） main.c —— 只做「按顺序 init + 接线」，不写业务判断', 11.5, '#334155')]
    b.append(ARROW([(480, 118), (480, 136)], color='#94a3b8', w=1.2, dash='4 3', head=6))

    layers = [('L4', 'ui', '唯一调用 LVGL 的层；几何量必须来自 board_map.h', UI, 'ui/*.c · port_display_lock / unlock'),
              ('L3', 'business', 'ai_action + 6 个业务模块；camera_service / wallpaper / mic_monitor', EXEC, 'HTTP 动作经 web_action 注册表进来'),
              ('L2', 'services', 'ws_client / event_bus / xiaozhi_service / web_server / fs_service', NETC, '跨模块让路走 flash_guard 回调表'),
              ('L1', 'drivers', 'periph / audio / media / camera(ov3660) —— 直接拨硬件', EDGE, 'esp_video · codec · SD · LED'),
              ('L0', 'platform', 'board_map.h + port_display / port_storage / port_time / port_bus', TRANS, '全工程唯一碰 bsp/esp-bsp.h 的层')]
    y = 138
    for tag, name, desc, c, right in layers:
        b += [R(140, y, 680, 66, c[0], c[1], rx=10),
              T(156, y + 25, tag, 12, c[2], bold=True, mono=True),
              T(196, y + 25, name, 13.5, c[2], bold=True),
              T(196, y + 46, desc, 11, '#334155'),
              T(196, y + 60, right, 9.5, MUT)]
        y += 78

    b.append(ARROW([(86, 150), (86, 500)], color='#2563eb', w=2.0, head=10))
    b.append(T(86, 528, '只许向下', 11.5, '#1e40af', bold=True, anchor='middle'))

    b.append(ARROW([(852, 500), (852, 150)], color='#b45309', w=1.6, dash='6 4'))
    b.append(T(852, 132, '反向通道', 10.5, '#92400e', bold=True, anchor='middle'))

    b.append(R(48, 536, 892, 56, OK[0], OK[1], rx=8))
    b.append(T(64, 558, '橙色虚线 = 反向通道：注册表 / 回调 / 事件（不许 include 上层头）；蓝色 = 唯一合法的向下依赖。', 11, '#166534'))
    b.append(T(64, 578, '落地证据：python tools/layering/layer_guard.py --baseline tools/layering/layering_baseline.json → 存量 27 条 / 新增 0，CI 强制。', 11, '#166534'))
    return svg('layering-model.svg', W, H, ''.join(b), 'L0~L4 五层模型与唯一合法依赖方向',
               '文档写什么，tools/layering/layer_guard.py 就查什么')


def main():
    fns = [d_system_overview, d_gateway_topology, d_two_axis_layers, d_device_chain, d_cloud_stack,
           d_voice_sequence, d_dual_dispatch, d_caps_flow, d_media_pipeline, d_layering_model]
    for fn in fns:
        p = fn()
        print(p, os.path.getsize(os.path.join(OUT, p)))


if __name__ == '__main__':
    main()
