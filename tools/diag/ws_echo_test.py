#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""S9 网关自检客户端：连 /ws/{session} 发一条 text，验证 AstrBot+TTS 全链路。

用法：python3 ws_echo_test.py <session_id> ["要问的话"]
依赖：pip install websockets
"""
import asyncio
import json
import sys
import urllib.parse

import websockets

GATEWAY = "ws://127.0.0.1:8081"   # 改成 wss://<公网IP>:8081 即为设备视角


async def main():
    sid = sys.argv[1] if len(sys.argv) > 1 else "xz-s31-test"
    text = sys.argv[2] if len(sys.argv) > 2 else "你好，用一句话介绍你自己"
    url = f"{GATEWAY}/ws/{urllib.parse.quote(sid)}"
    print(f"[TEST] 连接 {url}")
    async with websockets.connect(url, max_size=4 * 1024 * 1024) as ws:
        # 连接后应收到 command_table
        first = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
        print("[TEST] 首条下行:", json.dumps(first, ensure_ascii=False)[:200])
        await ws.send(json.dumps({"type": "text", "text": text}, ensure_ascii=False))
        deadline = asyncio.get_event_loop().time() + 90
        while asyncio.get_event_loop().time() < deadline:
            raw = await asyncio.wait_for(ws.recv(), timeout=30)
            try:
                msg = json.loads(raw)
            except Exception:
                print("[TEST] 非 JSON:", raw[:100])
                continue
            print("[TEST] <<", json.dumps(msg, ensure_ascii=False)[:300])
            if msg.get("type") == "audio":
                print("[TEST] ✅ 收到音频 URL:", msg.get("url"))
                break
            if msg.get("type") == "error":
                print("[TEST] ❌ 错误:", msg.get("code"), msg.get("message"))
                break
        else:
            print("[TEST] ⚠️ 超时未收到 audio/error")


if __name__ == "__main__":
    asyncio.run(main())
