#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网关补充自检：①TTS 文件有效性 ②ASR 链路（TTS 语音反喂转写）③/push 主动推送。"""
import asyncio
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, "/opt/pet_gateway")
import aiohttp
import websockets

import ws_gateway as gw

TTS_DIR = Path("/opt/pet_gateway/static")


def wav_to_pcm16(path: Path, target_rate: int = 16000) -> bytes:
    """读 WAV，抽稀到 16k 裸 PCM（冒烟测试够用）。"""
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF"
    pos = 12
    fmt_rate = 24000
    data = b""
    while pos + 8 <= len(raw):
        cid = raw[pos : pos + 4]
        clen = struct.unpack("<I", raw[pos + 4 : pos + 8])[0]
        body = raw[pos + 8 :]
        if cid == b"fmt ":
            fmt_rate = struct.unpack("<I", body[4:8])[0]
        if cid == b"data":
            data = body[:clen]
            break
        pos += 8 + clen + (clen & 1)
    assert data
    pcm = data
    if fmt_rate != target_rate:
        # 简单线性抽稀
        ratio = fmt_rate / target_rate
        idx = [int(i * ratio) for i in range(int(len(pcm) / 2 / ratio))]
        pcm = b"".join(pcm[j * 2 : j * 2 + 2] for j in idx)
    return pcm


async def main():
    # 1) TTS 文件
    wavs = sorted(TTS_DIR.glob("tts_*.wav"))
    if not wavs:
        print("[T1] ❌ 无 TTS 文件")
        return
    f = wavs[-1]
    print(f"[T1] ✅ TTS 文件 {f.name} size={f.stat().st_size} bytes")

    # 2) ASR：TTS 语音反喂
    try:
        pcm = wav_to_pcm16(f)
        text = await gw.stt_transcribe(pcm, 16000)
        print("[T2] ✅ ASR 转写:", repr(text))
    except Exception as exc:  # noqa: BLE001
        print("[T2] ❌ ASR 失败:", exc)

    # 3) /push 主动推送
    sid = "xz-s31-pushtest"
    results = {}

    async def ws_watcher():
        async with websockets.connect(f"ws://127.0.0.1:8081/ws/{sid}") as ws:
            results["first"] = json.loads(await asyncio.wait_for(ws.recv(), 10))
            await asyncio.sleep(1)
            async with aiohttp.ClientSession() as s:
                async with s.post(
                    "http://127.0.0.1:8081/push",
                    json={"session_id": sid, "text": "该睡觉了，桌宠测试推送", "emotion": "sleepy", "tts": False},
                ) as r:
                    results["push"] = await r.json()
            results["msgs"] = []
            for _ in range(3):
                try:
                    results["msgs"].append(json.loads(await asyncio.wait_for(ws.recv(), 15)))
                except Exception:  # noqa: BLE001
                    break

    await ws_watcher()
    print("[T3] push 返回:", results.get("push"))
    print("[T3] 推送后下行:", [m.get("type") for m in results.get("msgs", [])])
    for m in results.get("msgs", []):
        print("      ", json.dumps(m, ensure_ascii=False)[:160])


if __name__ == "__main__":
    asyncio.run(main())
