# -*- coding: utf-8 -*-
"""S9 桌宠伴侣 · 云端 WebSocket 网关（P0~P3 落地版）

职责：
  - 设备端唯一长连接入口（wss://<host>:8081/ws/{session_id}）
  - 上行：text / pcm(base64) / event / image_req
  - 下行：text / audio(MP3或WAV URL) / image / emotion / system / error
  - 与 AstrBot Open API (/api/v1/chat) 对接（SSE 流式）
  - 复用 AstrBot 已配好的 TTS（百炼 SpeechSynthesizer）与 STT（OpenAI 兼容转写）
  - POST /push 供插件主动推送（桌宠灵魂）

运行：uvicorn ws_gateway:app --host 0.0.0.0 --port 8081
依赖：fastapi uvicorn aiohttp python-multipart websockets
"""

import asyncio
import base64
import io
import itertools
import json
import logging
import os
import re
import struct
import time
import uuid
import wave
from pathlib import Path

import aiohttp
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

# ---------------------------------------------------------------- 配置
BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"
ASTRBOT_CFG_PATH = "/opt/astrbot/data/cmd_config.json"   # 复用 AstrBot 已配好的 provider

DEFAULT_CONFIG = {
    "port": 8081,
    "astrbot_base": "http://127.0.0.1:6185",
    "astrbot_api_key": "",
    "session_prefix": "xz-s31-",
    "tts_enabled": True,
    "asr_enabled": True,
    "static_dir": str(BASE_DIR / "static"),
    # AstrBot 的图片/语音落盘目录（宿主机路径；容器内是 /AstrBot/data/attachments）。
    # SSE 只给文件名，设备只会按 URL 取 ⇒ 由网关 /media 代理出去。
    "astrbot_attachments": "/opt/astrbot/data/attachments",
    "command_table": ["打卡", "拍照", "点歌", "换壁纸", "自拍"],
    "log_file": str(BASE_DIR / "gateway.log"),
    "max_pcm_seconds": 10,
}

def load_config() -> dict:
    cfg = dict(DEFAULT_CONFIG)
    if CONFIG_PATH.exists():
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8-sig"))
            cfg.update(data)
        except Exception as exc:  # noqa: BLE001
            print(f"[GW] config.json 解析失败，用默认配置: {exc}")
    Path(cfg["static_dir"]).mkdir(parents=True, exist_ok=True)
    return cfg

CONFIG = load_config()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [GW] %(levelname)s %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler(CONFIG["log_file"], encoding="utf-8"),
    ],
)
log = logging.getLogger("gateway")

app = FastAPI(title="S9 Pet Gateway")
app.mount("/static", StaticFiles(directory=CONFIG["static_dir"]), name="static")

# 设备上传的原始图片（POST /upload/{sid}）。**不能**放进 static_dir：
# 那是给"设备要按 URL 去下载"的文件用的，而上行图片只给云端看，不对外暴露。
UPLOAD_DIR = BASE_DIR / "uploads"
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
MAX_UPLOAD_BYTES = 4 * 1024 * 1024      # 设备实测 40~90KB，4MB 是硬闸

# 云端媒体（表情包图片 / 语音）对外下载口。
# ★ 为什么需要它：AstrBot 的 SSE 只发**文件名**（`[IMAGE]2026...png`、
#   `[RECORD]...wav`），文件落在宿主机的 attachments 目录里；而设备端只会
#   "按 URL 取"（cloud_media_service_fetch）。没有这一层，表情包永远到不了屏。
MEDIA_DIR = Path(CONFIG["astrbot_attachments"])
MEDIA_SUFFIX = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp",
                ".wav", ".mp3", ".m4a", ".aac", ".opus"}


@app.get("/media/{name}")
async def media(name: str):
    safe = Path(name).name                      # 挡 ../ 穿越
    if safe != name or Path(safe).suffix.lower() not in MEDIA_SUFFIX:
        raise HTTPException(status_code=400, detail="bad media name")
    for d in (MEDIA_DIR, Path(CONFIG["static_dir"])):
        p = d / safe
        if p.is_file():
            return FileResponse(p)
    raise HTTPException(status_code=404, detail="media not found")


def safe_name(sid: str, suffix: str = "") -> str:
    """session_id / attachment_id 只允许 [A-Za-z0-9_.-]，挡路径穿越。"""
    base = re.sub(r"[^A-Za-z0-9_.-]", "_", sid or "dev")
    return base + suffix

# ---------------------------------------------------------------- 会话表
# session_id -> {ws, sid, connected_at}
SESSIONS: dict[str, dict] = {}
START_TS = time.time()

# ---------------------------------------------------------------- AstrBot provider 复用
def load_astrbot_providers() -> dict:
    """从 AstrBot 主配置读已配好的 TTS/STT provider（同机，直接读文件）。"""
    out = {"tts": None, "stt": None}
    try:
        cfg = json.loads(open(ASTRBOT_CFG_PATH, encoding="utf-8-sig").read())
    except Exception as exc:  # noqa: BLE001
        log.warning("读 AstrBot 配置失败: %s", exc)
        return out
    for p in cfg.get("provider", []):
        ptype = p.get("provider_type", "")
        if ptype == "text_to_speech" and p.get("enable"):
            out["tts"] = {
                "api_key": p.get("api_key", ""),
                "model": p.get("model", "qwen-audio-3.1-tts-flash"),
                "voice": p.get("voice", ""),
                "instruction": p.get("tts_instruction", "") or "",
                "format": p.get("format", "wav"),
                "sample_rate": int(p.get("sample_rate", 24000) or 24000),
                "timeout": int(p.get("timeout", 60) or 60),
            }
        if ptype == "speech_to_text" and p.get("enable"):
            out["stt"] = {
                "api_key": p.get("api_key", ""),
                "api_base": (p.get("api_base") or "").rstrip("/"),
                "model": p.get("model", ""),
                "timeout": int(p.get("timeout", 60) or 60),
            }
    return out

PROVIDERS = load_astrbot_providers()
log.info("TTS provider: %s", "OK " + (PROVIDERS["tts"] or {}).get("model", "") if PROVIDERS["tts"] else "未配置(tts_enabled=%s)" % CONFIG["tts_enabled"])
log.info("STT provider: %s", "OK " + (PROVIDERS["stt"] or {}).get("model", "") if PROVIDERS["stt"] else "未配置(asr_enabled=%s)" % CONFIG["asr_enabled"])

# ---------------------------------------------------------------- 工具
def pcm16_to_wav(pcm: bytes, sample_rate: int = 16000) -> bytes:
    """16k/mono/s16 裸 PCM → 标准 WAV。"""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


def normalize_wav(raw: bytes) -> bytes:
    """把流式 WAV（长度占位、FF FF 标记）重写成标准 PCM WAV（照 AstrBot qwen_audio_tts）。"""
    if len(raw) < 44 or raw[:4] != b"RIFF":
        return raw
    pos = 12
    fmt = b""
    data = None
    while pos + 8 <= len(raw):
        cid = raw[pos : pos + 4]
        clen = struct.unpack("<I", raw[pos + 4 : pos + 8])[0]
        body = raw[pos + 8 :]
        if cid == b"data":
            if clen == 0 or clen == 0xFFFFFFFF or clen > len(body) or (clen & 0xFFFF) == 0xFFFE:
                data = body
            else:
                data = body[:clen]
            break
        if cid == b"fmt ":
            fmt = raw[pos : pos + 8 + clen]
        pos += 8 + clen + (clen & 1)
    if data is None or not fmt:
        return raw
    clean = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0xFF and i + 1 < len(data):
            i += 2
            continue
        clean.append(b)
        i += 1
    clean = bytes(clean)
    if len(clean) % 2:
        clean += b"\x00"
    fmt_body = fmt[8:]
    channels, rate, bits = struct.unpack("<H", fmt_body[2:4])[0], struct.unpack("<I", fmt_body[4:8])[0], struct.unpack("<H", fmt_body[14:16])[0]
    if (len(clean) * 8) // max(1, channels * max(1, bits)) < 100:
        return raw
    out = bytearray()
    out += b"RIFF" + struct.pack("<I", 36 + len(clean)) + b"WAVE"
    out += fmt[:8] + fmt[8:] + b"data" + struct.pack("<I", len(clean)) + clean
    return bytes(out)


TTS_MAX_CHARS = 200


def tts_shorten(text: str) -> str:
    """TTS 前的长度闸。

    ★ 为什么必须有：AstrBot 偶尔会返回超长文本（思维链泄漏时一度 7.5 MB 音频，
    设备端表现为"念了四分钟不停"）。这里按句子边界截到 ~200 字：**文字仍是
    全文**（设备气泡可滚动），只是不把整篇念出来。
    """
    t = (text or "").strip()
    if len(t) <= TTS_MAX_CHARS:
        return t
    cut = t[:TTS_MAX_CHARS]
    for sep in ("。", "！", "？", "\n", "…", "；", "，", ","):
        i = cut.rfind(sep)
        if i > TTS_MAX_CHARS // 2:
            return cut[: i + 1]
    return cut


async def tts_synthesize(text: str) -> str:
    """调百炼 SpeechSynthesizer → 存 static → 返回 /static/xxx.wav（照 AstrBot qwen_audio_tts 契约）。

    AstrBot 原实现要点：data(base64) 为空时必须回退到 audio.url 下载模式。
    """
    text = tts_shorten(text)
    p = PROVIDERS["tts"]
    url = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer"
    payload = {
        "model": p["model"],
        "input": {
            "text": text,
            "voice": p["voice"],
            "format": p["format"],
            "sample_rate": p["sample_rate"],
        },
    }
    if p.get("instruction"):
        payload["input"]["instruction"] = p["instruction"]
    headers = {"Authorization": f"Bearer {p['api_key']}", "Content-Type": "application/json"}
    timeout = aiohttp.ClientTimeout(total=p["timeout"])
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.post(url, json=payload, headers=headers) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"TTS HTTP {resp.status}: {body[:200]}")
            data = json.loads(body)
            audio = (data.get("output") or {}).get("audio") or {}
            b64 = audio.get("data")
            if b64:
                raw = base64.b64decode(b64)
            elif audio.get("url"):
                async with session.get(audio["url"]) as r:
                    raw = await r.read()
            else:
                raise RuntimeError(f"TTS 无 data 也无 url: {body[:200]}")
            if not raw or len(raw) < 1000:
                raise RuntimeError("TTS 音频过小")
            if raw[:4] != b"RIFF":
                raise RuntimeError(f"TTS 非 WAV: {raw[:8]!r}")
            raw = normalize_wav(raw)
            fname = f"tts_{uuid.uuid4().hex[:12]}.{p['format']}"
            Path(CONFIG["static_dir"], fname).write_bytes(raw)
            return f"/static/{fname}"


MODE_MARK_RE = re.compile(
    r"(?:[\[【〔⟦]\s*MODE\s*:\s*(?:work|chat)\s*[\]】〕⟧]|MODE\s*:\s*(?:work|chat))", re.I)


def clean_reply_text(text: str) -> str:
    """清洗 AstrBot 回复：剥掉 ⟦MODE:xxx⟧ 模式标记，并去掉重复段。

    ★ 实测三种形态（2026-10-31 F58）：
      - `⟦MODE:chat⟧<正文>⟧<正文>`（标记夹在两次流式之间）；
      - `<正文><正文>`（整段各发一遍）；
      - `A + B + B`（**只有尾段重复**：工具调用后 AstrBot 会把最后一段
        再流式发一次；本次实测 "…给你翻！喏，给你了……（重复）"）。
    只按"前后半段相同"折叠，第三种完全漏掉 ⇒ 补一个"最长重复后缀"折叠。
    """
    t = MODE_MARK_RE.sub("", text or "").strip()
    n = len(t)
    if n >= 12 and n % 2 == 0 and t[: n // 2] == t[n // 2:]:
        t = t[: n // 2].strip()
        n = len(t)
    # 尾部 B+B：B 至少 10 字，从最长开始试，命中即剪掉后一份
    for k in range(n // 2, 9, -1):
        if t[n - 2 * k: n - k] == t[n - k:]:
            log.info("回复尾段重复 %d 字，已折叠", k)
            t = t[: n - k].strip()
            break
    return t


async def stt_transcribe(pcm: bytes, sample_rate: int = 16000) -> str:
    """OpenAI 兼容 /audio/transcriptions（复用 AstrBot 已配 STT provider）。"""
    p = PROVIDERS["stt"]
    ep = f"{p['api_base']}/audio/transcriptions"
    wav = pcm16_to_wav(pcm, sample_rate)
    data = aiohttp.FormData()
    data.add_field("file", wav, filename="audio.wav", content_type="audio/wav")
    data.add_field("model", p["model"])
    headers = {"Authorization": f"Bearer {p['api_key']}"}
    timeout = aiohttp.ClientTimeout(total=p["timeout"])
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.post(ep, data=data, headers=headers) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"STT HTTP {resp.status}: {body[:200]}")
            return (json.loads(body).get("text") or "").strip()


# ---------------------------------------------------------------- AstrBot chat
async def astrbot_chat(session_id: str, text: str,
                       media: list | None = None) -> str:
    """纯文字聊天（单段 plain）。media 同 astrbot_chat_segments。"""
    return await astrbot_chat_segments(
        session_id, [{"type": "plain", "text": text}], media)


async def astrbot_file_upload(path: Path) -> str:
    """把本地图片 POST 到 AstrBot /api/v1/file（multipart，字段名 file），返回 attachment_id。"""
    url = f"{CONFIG['astrbot_base']}/api/v1/file"
    headers = {"Authorization": f"Bearer {CONFIG['astrbot_api_key']}"}
    form = aiohttp.FormData()
    form.add_field("file", path.read_bytes(), filename=path.name, content_type="image/jpeg")
    timeout = aiohttp.ClientTimeout(total=90)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.post(url, data=form, headers=headers) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"AstrBot file HTTP {resp.status}: {body[:200]}")
            data = json.loads(body)
            # 兼容两种返回形态：{"attachment_id":..} 与 {"data":{"attachment_id":..}}
            aid = data.get("attachment_id") or (data.get("data") or {}).get("attachment_id")
            if not aid:
                raise RuntimeError(f"AstrBot file 无 attachment_id: {body[:200]}")
            log.info("AstrBot file OK %s -> %s", path.name, aid)
            return aid


async def astrbot_chat_segments(session_id: str, segments: list,
                                media: list | None = None) -> str:
    """调 AstrBot /api/v1/chat，SSE 流式拼接 plain 增量（无 complete 帧，以连接关闭收尾）。

    segments 用 AstrBot 的消息段数组：文字 {"type":"plain"}，图片
    {"type":"image","attachment_id":...}（S9 裁决 5 的上行发图路径）。

    media: 传列表进来时，把云端下发的**非文字段**追加进去（("image"/"record", 文件名)）。
    ★ 以前只认 plain，图片与语音段被静默丢弃 —— 表情包"云端发了但屏幕没动静"
      的根因就在这里（F58）。
    """
    url = f"{CONFIG['astrbot_base']}/api/v1/chat"
    headers = {
        "Authorization": f"Bearer {CONFIG['astrbot_api_key']}",
        "Content-Type": "application/json",
    }
    payload = {
        "username": "xiao1",
        "session_id": session_id,
        "message": segments,
        "enable_streaming": True,  # ★ 必须 JSON true（字符串 true 会被拒）
    }
    text = " ".join(x.get("text", "") for x in segments if x.get("type") == "plain")
    log.info("AstrBot << %s | %s%s", session_id, text[:80],
             " [image]" if any(x.get("type") == "image" for x in segments) else "")
    # ★ 150 s：表情包那条路要跑语义检索 + 翻网盘，实测 36~41 s；
    #   首次建索引超过 90 s（旧值），会先超时把好端端的一轮对话掐掉（F58）。
    timeout = aiohttp.ClientTimeout(total=150)
    pieces: list[str] = []
    reasoning_n = 0
    tool_n = 0
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.post(url, json=payload, headers=headers) as resp:
            if resp.status != 200:
                body = await resp.text()
                raise RuntimeError(f"AstrBot HTTP {resp.status}: {body[:200]}")
            async for line in resp.content:
                line = line.decode("utf-8", "ignore").strip()
                if not line.startswith("data:"):
                    continue
                raw = line[5:].strip()
                if not raw:
                    continue
                try:
                    evt = json.loads(raw)
                except Exception:  # noqa: BLE001
                    continue
                if evt.get("type") == "plain":
                    # ★★ chain_type 有三种"非正文"：reasoning（思维链）、
                    #   tool_call / tool_call_result（工具调用 JSON）。
                    #   只滤 reasoning 的话，工具调用的原始 JSON 会被当正文
                    #   拼进回复里（F58 实测 SSE 里 17 条 tool_call）。
                    ct = (evt.get("chain_type") or "").lower()
                    if ct == "reasoning":
                        reasoning_n += 1
                        continue
                    if ct in ("tool_call", "tool_call_result"):
                        tool_n += 1
                        continue
                    piece = evt.get("data") or ""
                    pieces.append(piece)
                    log.debug("AstrBot >> %s", piece[:60])
                elif evt.get("type") in ("image", "record"):
                    # data 形如 "[IMAGE]20261031201107556_33e6.png"（只有文件名）
                    raw = str(evt.get("data") or "")
                    fname = re.sub(r"^\[(IMAGE|RECORD|FILE|VIDEO)\]", "", raw).strip()
                    if fname and media is not None:
                        media.append((evt["type"], fname))
                        log.info("[%s] 云端 %s: %s", session_id, evt["type"], fname)
                elif evt.get("type") == "error":
                    raise RuntimeError(f"AstrBot 流错误: {evt.get('data')}")
    if reasoning_n:
        log.info("[%s] 丢弃思维链事件 %d 条", session_id, reasoning_n)
    if tool_n:
        log.info("[%s] 丢弃工具调用事件 %d 条", session_id, tool_n)
    text_out = clean_reply_text("".join(pieces))
    log.info("AstrBot >> %s | %s", session_id, text_out[:80])
    return text_out


# ---------------------------------------------------------------- 下行分发
async def reply_text(ws: WebSocket, text: str) -> None:
    await ws.send_text(json.dumps({"type": "text", "text": text}, ensure_ascii=False))


async def send_media(ws: WebSocket, media: list | None) -> None:
    """把云端下发的图片段转成设备认的 image 帧（表情包/生图都走这里）。

    ★ 只转 image：record 是 AstrBot 自己合成的 TTS，与网关侧百炼 TTS 重复，
      下发了会"一句话两个声音"，所以这里刻意跳过（F58）。
    """
    if not media:
        return
    host = ws.headers.get("host") or f"120.76.230.17:{CONFIG['port']}"
    for kind, name in media:
        if kind != "image":
            continue
        if not name.startswith("http"):
            # 本地 attachments 文件：先归一化成设备端能解码的 baseline JPEG（v0.8.1）。
            # 云端表情包常见渐进式 JPEG / >1M 像素大图 / PNG —— 直接下发必被
            # 设备丢弃（2026-10-31 实测 "exceeds pixel cap" / "Only baseline-DCT"）。
            norm = normalize_image(name)
            if norm:
                name = norm
        url = name if name.startswith("http") else f"http://{host}/media/{name}"
        await ws.send_text(json.dumps(
            {"type": "image", "url": url, "caption": ""}, ensure_ascii=False))
        log.info("media -> %s", url)


# -------------------------------------------------------- 图片归一化（v0.8.1）
# 设备端硬限制（cloud_media_service.h）：baseline JPEG、≤1M 像素、≤1200KB。
CIMG_PIXEL_CAP = 1024 * 1024
CIMG_BYTES_CAP = 1150 * 1024        # 留 50KB 余量（设备端 1200KB 硬闸）


def normalize_image(name: str) -> str | None:
    """把云端下发的图片文件归一化成设备端 JPEG 解码器能吃的 baseline 文件。

    返回可直接拼 /media/ URL 的文件名；无法处理返回 None（调用方原样下发，
    设备端自会拒绝并留下日志）。结果缓存在 <stem>__n.jpg：源文件没变就复用。
    """
    src = MEDIA_DIR / name
    if not src.is_file():
        return None
    dst = src.with_name(src.stem + "__n.jpg")
    try:
        if dst.is_file() and dst.stat().st_mtime >= src.stat().st_mtime:
            return dst.name            # 已归一化且不旧于源：直接复用
        from PIL import Image
        with Image.open(src) as im:
            im.load()                  # GIF/WebP 动图取首帧
            if (im.format == "JPEG" and not im.info.get("progressive")
                    and im.width * im.height <= CIMG_PIXEL_CAP
                    and src.stat().st_size <= CIMG_BYTES_CAP):
                return src.name        # 本来就合规：不重编码，省 CPU
            w, h = im.size
            if w * h > CIMG_PIXEL_CAP:
                scale = (CIMG_PIXEL_CAP / (w * h)) ** 0.5
                im = im.resize((max(1, int(w * scale)), max(1, int(h * scale))))
            if im.mode != "RGB":
                im = im.convert("RGB")
            im.save(dst, "JPEG", quality=85, progressive=False)
        log.info("image normalized: %s -> %s (%dx%d)", name, dst.name,
                 *Image.open(dst).size)
    except Exception as exc:  # noqa: BLE001
        log.warning("normalize %s failed: %s", name, exc)
        return None
    return dst.name


async def reply_with_tts(ws: WebSocket, text: str, media: list | None = None) -> None:
    """文字先于语音（P4 原则）：先 text，再 speaking → audio URL → idle。

    图片（表情包）排在文字之后、语音之前 —— 先看得见，再听得着。
    """
    await reply_text(ws, text)
    await send_media(ws, media)
    if not CONFIG["tts_enabled"] or not PROVIDERS["tts"]:
        return
    try:
        await ws.send_text(json.dumps({"type": "emotion", "name": "speaking"}, ensure_ascii=False))
        audio_url = await tts_synthesize(text)
        await ws.send_text(json.dumps({"type": "audio", "url": audio_url, "duration_ms": 0}, ensure_ascii=False))
        await ws.send_text(json.dumps({"type": "emotion", "name": "idle"}, ensure_ascii=False))
        log.info("TTS OK %s", audio_url)
    except Exception as exc:  # noqa: BLE001
        log.warning("TTS 失败，降级纯文字: %s", exc)
        await ws.send_text(json.dumps({"type": "error", "code": "tts_failed", "message": str(exc)[:120]}, ensure_ascii=False))


# ---------------------------------------------------------------- 反向工具 relay
#
# ★ 为什么要有这一段（2026-10-31 实机实证）：
#   设备侧的 15 个 MCP 工具虽然还 attach 在 esp_mcp_engine 上，但 S9 已经把
#   "MCP over WS" 传输删掉了（裁决 D5' 方案 A：引擎空转）⇒ 云端 LLM 根本
#   不知道这些工具存在。于是用户说"打卡"时，模型只能**编一句**"记上了，今天
#   这格算你到"，设备端日志里没有任何 AI_PUNCH 调用。
#   而设备在 NAT 后面（192.168.10.x），文档 §10.2 写的"AstrBot 插件直接
#   requests.post(设备 IP)"物理上也不通。
#
#   ⇒ 唯一可行的反向通道就是那条**已经常连的 WS**：
#       网关 --{"type":"action","id":N,"name":"punch"}--> 设备
#       网关 <--{"type":"event","event":"action_result","id":N,...}-- 设备
#   网关拿到真实结论再回话，不再让模型幻觉"成功"。
#
#   P0 用"关键词意图快速路"（不花 token、不进 LLM、延迟 < 1 轮 TTS）；
#   同时保留 POST /action 给将来的 AstrBot 插件做 function-calling。

ACTION_SEQ = itertools.count(1)

# ★ 点歌成功的记账（session_id -> 时间戳）。
#   AstrBot 插件（astrbot_plugin_pet_device）调 /action 让设备起播后，LLM 还会
#   生成一句"行，放《晴天》"，网关照旧 TTS 的话立刻把音乐顶掉（设备端
#   audio_playback 后到先得）⇒ 10 秒内有过成功点播就只回文字。
MUSIC_OK_AT: dict = {}
MUSIC_TTS_GUARD_S = 10.0
PENDING_ACTIONS: dict = {}          # (session_id, req_id) -> asyncio.Future

ACTION_ALIASES = {
    "打卡": "punch",
    "打个卡": "punch",
    "签个到": "punch",
    "签到": "punch",
    "上班打卡": "punch",
    "拍照": "capture",
    "拍个照": "capture",
    "拍张照": "capture",
    "拍张照片": "capture",
    "拍一张": "capture",
    "照相": "capture",
    "自拍": "capture",
    "换壁纸": "wallpaper",
    "换个壁纸": "wallpaper",
    "换张壁纸": "wallpaper",
    "换一张壁纸": "wallpaper",
}

SONG_RE = re.compile(r"^(?:点歌|点一首|点首|点个歌|播放|放一首|放首|放一首歌|来一首|来首|唱一首|唱首|听)\s*(.*)$")

# 语音/口语前缀（"我要打卡""帮我拍张照"）先剥掉再匹配
FILLER_RE = re.compile(r"^(?:我要|我想|帮我|给我|麻烦你|麻烦|请|能不能|可以|现在)")


def _caps_tools(session_id: str) -> list:
    """该会话设备自述的能力表；设备没报过（或已断线）返回空。"""
    return (((SESSIONS.get(session_id) or {}).get("caps")) or {}).get("tools") or []


def match_intent(text: str, session_id: str = ""):
    """把用户话术映射成设备动作。返回 {'name':..,'args':{..}} 或 None。

    ★ 匹配优先级：**设备自述的别名** > 下面的硬编码兜底表。
      前者让"加动作只改固件"成立；后者只服务没升级的旧固件（不发 caps）。
    ★ 只做"确定性短语"，不做语义猜测：宁可漏给 LLM，也不要误触发打卡/拍照。
    """
    t = (text or "").strip().strip("。！？!?.,，、 ")
    if not t:
        return None
    for _ in range(2):                      # "我要帮我打卡" 这种叠词
        t2 = FILLER_RE.sub("", t).strip()
        if t2 == t:
            break
        t = t2
    if not t:
        return None

    # 1) 设备自述（别名按长度降序，避免"放首"抢在"放首歌"前面）
    for tool in _caps_tools(session_id):
        name = tool.get("name")
        aliases = [a for a in (tool.get("aliases") or "").split("|") if a]
        if not name or not aliases:
            continue
        aliases.sort(key=len, reverse=True)
        if tool.get("arg_mode") == "tail":
            for a in aliases:
                if t.startswith(a):
                    tail = t[len(a):].strip().strip("《》<>\"'“”‘’ ")
                    # v2：参数名由设备自述（音乐是 name，音量是 volume）
                    key = tool.get("arg_key") or "name"
                    args = {"__text": t}
                    if tail:
                        args[key] = tail
                    # ⚠ __text 是"整句原话"：像"大点声""小声点"这种别名会把
                    #   整句吃干净（tail 为空），设备端 do_volume 需要靠原话
                    #   判断"相对调大/调小"，光有参数值做不到。引擎只认自述
                    #   的属性，多余键忽略，塞进去是安全的。
                    return {"name": name, "args": args}
        elif t in aliases:
            return {"name": name, "args": {}}

    # 2) 兜底：老固件没报 caps
    if t in ACTION_ALIASES:
        return {"name": ACTION_ALIASES[t], "args": {}}
    m = SONG_RE.match(t)
    if m:
        song = m.group(1).strip().strip("《》<>\"'“”‘’ ")
        return {"name": "music", "args": ({"name": song} if song else {})}
    return None


async def run_action(session_id: str, ws: WebSocket, name: str, args: dict,
                     timeout: float = 45.0) -> dict:
    """下发 action 并等设备回 action_result。超时返回 ok=False。"""
    req_id = next(ACTION_SEQ)
    fut = asyncio.get_running_loop().create_future()
    key = (session_id, req_id)
    PENDING_ACTIONS[key] = fut
    try:
        await ws.send_text(json.dumps(
            {"type": "action", "id": req_id, "name": name, "args": args or {}},
            ensure_ascii=False))
        log.info("[%s] action >> %s #%d %s", session_id, name, req_id,
                 json.dumps(args or {}, ensure_ascii=False))
        res = await asyncio.wait_for(fut, timeout)
        if name == "music" and res.get("ok"):
            MUSIC_OK_AT[session_id] = time.time()
        log.info("[%s] action << %s #%d ok=%s data=%s", session_id, name, req_id,
                 res.get("ok"), (res.get("data") or "")[:80])
        return res
    except asyncio.TimeoutError:
        log.warning("[%s] action %s #%d 超时 %.0fs（设备没回话）",
                    session_id, name, req_id, timeout)
        return {"ok": False, "data": "设备没回话", "timeout": True}
    finally:
        PENDING_ACTIONS.pop(key, None)


def action_reply(name: str, args: dict, result: dict) -> str:
    """把设备结论转成一句人话（保持大肥鱼的嘴硬语气）。

    ★ 设备回的 `data` 已经是**中文整句**（打卡/拍照的失败文案由设备侧翻译，
    见 ai_action.c punch_to_zh）⇒ 失败时直接用它，不要再套"没盖成，"。
    """
    ok = bool(result.get("ok"))
    data = (result.get("data") or "").strip()
    song = (args or {}).get("name") or ""
    if name == "punch":
        return f"章盖上了 —— {data}" if ok else (data or "打卡没成")
    if name == "capture":
        return f"拍了，{data}" if ok else (data or "拍照没成")
    if name == "music":
        if ok:
            return f"行，放《{data}》。"
        if "INVALID_STATE" in data:
            return "先按一下录音键把我叫醒，再说歌名。"
        return f"放不了《{song}》：{data}" if song else (data or "放不了")
    if name == "wallpaper":
        return "换了张新的，抬头看看。" if ok else (data or "换不了")
    if name == "volume":
        return f"调到 {data} 了。" if ok else (data or "音量调不了")
    return data or name


async def handle_user_text(ws: WebSocket, session_id: str, text: str) -> None:
    """一句话的统一入口：先试确定性动作，不命中才进 LLM。"""
    intent = match_intent(text, session_id)
    if intent is not None:
        name = intent["name"]
        args = intent.get("args") or {}
        if name == "music" and not args.get("name"):
            await reply_with_tts(ws, "想听哪首？报个歌名。")
            return
        result = await run_action(session_id, ws, name, args)
        reply = action_reply(name, args, result)
        if name == "music" and result.get("ok"):
            # ★ P1：点歌**只回文字、不出 TTS**。否则"行，放《晴天》"这句 TTS
            #   会立刻把刚起播的音乐顶掉（设备端 audio_playback 后到先得）。
            await reply_text(ws, reply)
            return
        await reply_with_tts(ws, reply)
        return

    media: list = []
    answer = await chat_with_retry(session_id, text, media=media)
    if not answer:
        await ws.send_text(json.dumps(
            {"type": "error", "code": "empty_reply", "message": "大肥鱼没吭声，再说一遍？"},
            ensure_ascii=False))
        return
    # ★ LLM 刚刚通过 pet_play_music 让设备起播 → 这句回复只出文字，
    #   否则 TTS 一响就把音乐顶掉（同 P1 的关键词快速路处理方式）。
    if time.time() - MUSIC_OK_AT.get(session_id, 0) < MUSIC_TTS_GUARD_S:
        log.info("[%s] 刚点过歌，本轮回复只发文字不 TTS", session_id)
        await reply_text(ws, answer)
        await send_media(ws, media)
        return
    await reply_with_tts(ws, answer, media)


async def chat_with_retry(session_id: str, text: str, attempts: int = 2,
                          media: list | None = None) -> str:
    """AstrBot 偶发被 workkit 插件重启（改配置→重启容器，约 20 s），
    表现为 Connection reset / TransferEncodingError。重试一次能救回大半。"""
    last: Exception | None = None
    for i in range(attempts):
        try:
            return await astrbot_chat(session_id, text, media)
        except Exception as exc:  # noqa: BLE001
            last = exc
            log.warning("[%s] AstrBot 第 %d/%d 次失败: %s", session_id, i + 1, attempts, exc)
            if i + 1 < attempts:
                await asyncio.sleep(1.5)
    assert last is not None
    raise last


# ---------------------------------------------------------------- WS 处理
async def handle_image_req(ws: WebSocket, session_id: str, msg: dict) -> None:
    """设备"拍照问"：本地原图 → AstrBot /api/v1/file → 消息段带 image → Qwen-VL 看图。"""
    aid = msg.get("attachment_id") or ""
    question = (msg.get("text") or "").strip() or "请描述这张照片"
    path = UPLOAD_DIR / safe_name(aid)
    if not aid or not path.is_file():
        log.warning("[%s] image_req 找不到附件: %r", session_id, aid)
        await ws.send_text(json.dumps({"type": "error", "code": "image_missing",
                                       "message": "照片没收到，请重拍"}, ensure_ascii=False))
        return
    try:
        remote_aid = await astrbot_file_upload(path)
        segments = [{"type": "plain", "text": question},
                    {"type": "image", "attachment_id": remote_aid}]
        answer = await astrbot_chat_segments(session_id, segments)
        if not answer:
            await ws.send_text(json.dumps({"type": "error", "code": "empty_reply",
                                           "message": "没看懂这张图，再拍一张？"}, ensure_ascii=False))
            return
        await reply_with_tts(ws, answer)
    except Exception as exc:  # noqa: BLE001
        log.error("[%s] image_req 失败: %s", session_id, exc)
        await ws.send_text(json.dumps({"type": "error", "code": "vision_failed",
                                       "message": str(exc)[:120]}, ensure_ascii=False))


async def _safe_handle(ws: WebSocket, session_id: str, msg: dict) -> None:
    """后台任务入口：异常不能冒到 asyncio 的 "Task exception was never retrieved"。"""
    try:
        await handle_message(ws, session_id, msg)
    except WebSocketDisconnect:
        pass
    except Exception as exc:  # noqa: BLE001
        log.error("[%s] handle_message 异常: %s", session_id, exc)


async def handle_message(ws: WebSocket, session_id: str, msg: dict) -> None:
    mtype = msg.get("type")
    log.info("[%s] rx type=%s", session_id, mtype)

    if mtype == "text":
        text = (msg.get("text") or "").strip()
        if not text:
            return
        if msg.get("role") == 0:
            # 设备不会发 role=0（那是网关回显 ASR 用的方向）；收到就丢，防回环。
            log.info("[%s] 忽略 role=0 的 text（回显方向不对）", session_id)
            return
        try:
            await handle_user_text(ws, session_id, text)
        except Exception as exc:  # noqa: BLE001
            log.error("text 链路失败: %s", exc)
            await ws.send_text(json.dumps({"type": "error", "code": "chat_failed", "message": str(exc)[:120]}, ensure_ascii=False))

    elif mtype == "pcm":
        if not CONFIG["asr_enabled"] or not PROVIDERS["stt"]:
            await ws.send_text(json.dumps({"type": "error", "code": "asr_disabled", "message": "ASR 未启用"}, ensure_ascii=False))
            return
        try:
            pcm = base64.b64decode(msg.get("data") or "")
            duration = len(pcm) / 2 / 16000
            max_sec = CONFIG["max_pcm_seconds"]
            if duration > max_sec:
                pcm = pcm[: int(max_sec * 2 * 16000)]
            log.info("[%s] pcm %d bytes (%.2fs)", session_id, len(pcm), duration)
            # ★ 先回一个"思考中"：ASR+LLM+TTS 实测要 9~18 s，这段时间设备侧
            #   如果没有任何下行，用户看到的就是"说完没反应"（2026-10-31 实测踩到）。
            await ws.send_text(json.dumps({"type": "emotion", "name": "thinking"}, ensure_ascii=False))
            text = await stt_transcribe(pcm, int(msg.get("sample_rate", 16000)))
            if not text:
                await ws.send_text(json.dumps({"type": "error", "code": "asr_failed", "message": "没听清，请再说一遍"}, ensure_ascii=False))
                return
            log.info("[%s] ASR >> %s", session_id, text)
            # ★ 回显识别结果（设备 role=0 → "我"的气泡）。没有它，答非所问时
            #   根本分不清是 ASR 听错还是模型跑偏 —— 排查成本极高。
            await ws.send_text(json.dumps({"type": "text", "role": 0, "text": text},
                                          ensure_ascii=False))
            await handle_user_text(ws, session_id, text)
        except Exception as exc:  # noqa: BLE001
            log.error("pcm 链路失败: %s", exc)
            await ws.send_text(json.dumps({"type": "error", "code": "asr_failed", "message": str(exc)[:120]}, ensure_ascii=False))

    elif mtype == "event":
        ev = msg.get("event", "")
        if ev == "action_result":
            # 设备执行完动作的回执：唤醒 run_action 的 future
            req_id = msg.get("id")
            fut = PENDING_ACTIONS.get((session_id, req_id))
            if fut is not None and not fut.done():
                fut.set_result({"ok": bool(msg.get("ok")),
                                "data": msg.get("data") or "",
                                "name": msg.get("name") or ""})
            else:
                log.info("[%s] action_result #%s 无人等待（已超时）", session_id, req_id)
        elif ev == "barge_in":
            log.info("[%s] barge_in（v1：仅记录，TTS 任务继续为并发放置）", session_id)
        elif ev == "status":
            log.info("[%s] status: %s", session_id, json.dumps(msg.get("data", {}), ensure_ascii=False)[:120])
        else:
            log.info("[%s] event=%s", session_id, ev)

    elif mtype == "caps":
        # 设备自述能力表（连上就报一次）。存起来供 AstrBot 插件动态注册工具，
        # 这样"设备会干什么"的唯一真相在固件里，云端不再硬编码动作名。
        tools = msg.get("tools") or []
        SESSIONS[session_id]["caps"] = {
            "model": msg.get("model") or "",
            "tools": tools,
            "at": time.time(),
        }
        names = ",".join(str(t.get("name", "?")) for t in tools)
        log.info("[%s] caps: %d tools [%s] model=%s",
                 session_id, len(tools), names, msg.get("model"))

    elif mtype == "image_req":
        # 设备拍照 → POST /upload/{sid} → 这里带 attachment_id 来（S9 裁决 5）
        await handle_image_req(ws, session_id, msg)

    else:
        log.warning("[%s] 未知消息类型: %s", session_id, mtype)


@app.websocket("/ws/{session_id}")
async def ws_endpoint(ws: WebSocket, session_id: str):
    await ws.accept()
    SESSIONS[session_id] = {"ws": ws, "sid": session_id, "connected_at": time.time()}
    log.info("[%s] connected, total=%d", session_id, len(SESSIONS))
    # 连接即下发命令表（R4：避免插件命令掉进闲聊）
    await ws.send_text(json.dumps({
        "type": "system", "cmd": "command_table", "list": CONFIG["command_table"],
    }, ensure_ascii=False))
    try:
        while True:
            raw = await ws.receive_text()
            # ★ 每帧上行都留一行：排查"设备说发了、网关没反应"时，
            #   这行能立刻区分"帧没到" / "到了但不是 JSON" / "到了且已处理"。
            #   大帧只打头部 90 字符（pcm 的 base64 正文没有诊断价值）。
            if len(raw) <= 200:
                log.info("[%s] rx %d ch: %s", session_id, len(raw), raw)
            else:
                log.info("[%s] rx %d ch: %.90s ...", session_id, len(raw), raw)
            for line in raw.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception:  # noqa: BLE001
                    log.warning("[%s] 非 JSON 行: %.80s", session_id, line)
                    continue
                # ★ 必须并发处理：一条 text 回合里 run_action 会等设备回
                #   action_result，如果这里串行 await，收包循环被占住 ⇒
                #   action_result 永远读不到 ⇒ 每次动作都超时（死锁）。
                asyncio.create_task(_safe_handle(ws, session_id, msg))
    except WebSocketDisconnect:
        pass
    finally:
        SESSIONS.pop(session_id, None)
        log.info("[%s] disconnected, total=%d", session_id, len(SESSIONS))


# ---------------------------------------------------------------- 设备上行图片
@app.post("/upload/{session_id}")
async def upload_image(session_id: str, request: Request):
    """设备拍照后把**原始字节**POST 上来（Content-Type: image/jpeg）。

    返回 {"attachment_id": "<文件名>"}，设备再走 WS 上行
    {"type":"image_req","attachment_id":...} 触发看图（S9 裁决 5）。
    """
    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="空请求体")
    if len(body) > MAX_UPLOAD_BYTES:
        raise HTTPException(status_code=413, detail=f"图片过大 {len(body)} B")
    aid = safe_name(session_id, f"_{uuid.uuid4().hex[:10]}.jpg")
    path = UPLOAD_DIR / aid
    path.write_bytes(body)
    # 只留最近 32 张，避免长期跑满磁盘（设备不会来删）
    try:
        files = sorted(UPLOAD_DIR.glob("*.jpg"), key=lambda f: f.stat().st_mtime, reverse=True)
        for old in files[32:]:
            old.unlink(missing_ok=True)
    except Exception:  # noqa: BLE001
        pass
    log.info("[%s] upload %u B -> %s", session_id, len(body), aid)
    return {"attachment_id": aid, "bytes": len(body)}


# ---------------------------------------------------------------- 插件主动推送
@app.post("/push")
async def push(payload: dict):
    """插件主动推：{session_id, text, emotion?, tts?}。返回送达状态。"""
    sid = payload.get("session_id")
    text = (payload.get("text") or "").strip()
    if not sid or not text:
        raise HTTPException(status_code=400, detail="session_id 与 text 必填")
    conn = SESSIONS.get(sid)
    if not conn:
        return {"delivered": False, "reason": "device_offline"}
    ws = conn["ws"]
    if payload.get("emotion"):
        await ws.send_text(json.dumps({"type": "emotion", "name": payload["emotion"]}, ensure_ascii=False))
    await reply_text(ws, text)
    if payload.get("tts", False):
        try:
            audio_url = await tts_synthesize(text)
            await ws.send_text(json.dumps({"type": "audio", "url": audio_url, "duration_ms": 0}, ensure_ascii=False))
        except Exception as exc:  # noqa: BLE001
            log.warning("push TTS 失败: %s", exc)
    return {"delivered": True}


# ---------------------------------------------------------------- 反向调用设备工具
@app.post("/action")
async def action_call(payload: dict):
    """给 AstrBot 插件用的 function-calling 入口（S9 §10.2 的正确形态）。

    插件侧（设备在 NAT 后面，所以打的是网关而不是设备）：

        requests.post("http://127.0.0.1:8081/action",
                      json={"session_id": "xz-s31-fc50", "name": "punch"}, timeout=60)

    网关经常连 WS 转发给设备，等 action_result 回来再返回真实结论。
    """
    sid = payload.get("session_id")
    name = (payload.get("name") or "").strip()
    if not sid or not name:
        raise HTTPException(status_code=400, detail="session_id 与 name 必填")
    conn = SESSIONS.get(sid)
    if not conn:
        return {"ok": False, "reason": "device_offline"}
    res = await run_action(sid, conn["ws"], name, payload.get("args") or {},
                           timeout=float(payload.get("timeout", 45)))
    return {"ok": bool(res.get("ok")), "data": res.get("data", ""),
            "timeout": bool(res.get("timeout"))}


@app.get("/caps")
async def device_caps(session_id: str = ""):
    """设备自述的能力表（AstrBot 插件启动 / 轮询时拉）。

    ★ 只认**还连着**的会话：设备断线后这里立刻返回空表，插件据此回落到
      内置默认清单，而不是拿一张过期表去下不可能完成的动作。

    返回形如：
        {"session_id":"xz-s31-fc50","online":true,"model":"ESP32-S31-Korvo-1",
         "tools":[{"name":"punch","label":"打卡","desc":"...","params":[],
                   "timeout_ms":45000}, ...]}
    """
    if session_id:
        caps = (SESSIONS.get(session_id) or {}).get("caps")
        if not caps:
            return {"session_id": session_id, "online": False, "tools": [],
                    "reason": "device_offline" if session_id not in SESSIONS else "no_caps_yet"}
        return {"session_id": session_id, "online": True, **caps}

    # 没指定会话：返回第一台报过 caps 的在线设备（单设备场景下的省事写法）
    for sid, v in SESSIONS.items():
        caps = v.get("caps")
        if caps:
            return {"session_id": sid, "online": True, **caps}
    return {"session_id": "", "online": False, "tools": [],
            "online_devices": list(SESSIONS.keys())}


@app.get("/health")
async def health():
    return {
        "ok": True,
        "uptime_s": round(time.time() - START_TS, 1),
        "sessions": {sid: round(time.time() - v["connected_at"], 1) for sid, v in SESSIONS.items()},
        "caps": {sid: [t.get("name") for t in (v.get("caps") or {}).get("tools", [])]
                 for sid, v in SESSIONS.items()},
        "tts": bool(PROVIDERS["tts"]),
        "stt": bool(PROVIDERS["stt"]),
        "pending_actions": len(PENDING_ACTIONS),
        "version": "0.8.1",
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=CONFIG["port"], log_level="info")
