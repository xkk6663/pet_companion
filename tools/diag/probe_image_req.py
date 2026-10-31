# -*- coding: utf-8 -*-
"""S9 P6 真值探针：验证"设备拍照问"全链路（upload → image_req → AstrBot 看图 → 下行）。

用法（在网关上跑，或本机跑但要能连到 8081）：
    python probe_image_req.py <本地jpg> [ws_base]

做三件事，任何一步失败都直接报出来（不猜）：
  1. POST {http}/upload/{sid} 上传原始 JPEG → 断言拿到 attachment_id
  2. WS 连 {ws}/ws/{sid}，连上后应立刻收到 command_table 帧
  3. 发 image_req，收帧直到出现 text（答案）或 error，打印全过程

前提：本机 python 有 aiohttp（网关 venv 已装）。WS 客户端用 aiohttp，不额外依赖 websockets。
"""
import asyncio
import json
import sys
from pathlib import Path

import aiohttp

SID = "xz-s31-probe"


async def main() -> int:
    if len(sys.argv) < 2:
        print("usage: probe_image_req.py <jpg> [host:port]")
        return 2
    jpg = Path(sys.argv[1])
    if not jpg.is_file():
        print(f"[FAIL] 图片不存在: {jpg}")
        return 2
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1:8081"
    http_base = f"http://{host}"
    ws_url = f"ws://{host}/ws/{SID}"

    data = jpg.read_bytes()
    print(f"[1/3] 上传 {jpg.name} ({len(data)} B) -> {http_base}/upload/{SID}")
    async with aiohttp.ClientSession() as s:
        async with s.post(f"{http_base}/upload/{SID}", data=data,
                          headers={"Content-Type": "image/jpeg"}) as r:
            body = await r.text()
            print(f"      HTTP {r.status} {body[:200]}")
            if r.status != 200:
                return 1
            aid = json.loads(body).get("attachment_id")
            if not aid:
                print("[FAIL] 没有 attachment_id")
                return 1

    print(f"[2/3] WS 连接 {ws_url}")
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(ws_url, heartbeat=30) as ws:
            first = await asyncio.wait_for(ws.receive(), timeout=10)
            print(f"      <- {first.data[:160]}")

            req = {"type": "image_req", "attachment_id": aid, "text": "这张图里有什么？一句话"}
            print(f"[3/3] -> {json.dumps(req, ensure_ascii=False)}")
            await ws.send_str(json.dumps(req, ensure_ascii=False))

            got_text = False
            audio = None
            deadline = asyncio.get_event_loop().time() + 90
            while True:
                left = deadline - asyncio.get_event_loop().time()
                if left <= 0:
                    print("[FAIL] 90s 内没有结果")
                    return 1
                msg = await asyncio.wait_for(ws.receive(), timeout=left)
                if msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                    print(f"[FAIL] ws 关闭: {msg.type}")
                    return 1
                if msg.type != aiohttp.WSMsgType.TEXT:
                    continue
                try:
                    evt = json.loads(msg.data)
                except Exception:
                    print(f"      <- (非 JSON) {msg.data[:120]}")
                    continue
                t = evt.get("type")
                print(f"      <- {t}: {str(evt)[:200]}")
                if t == "text":
                    got_text = True
                elif t == "audio":
                    audio = evt.get("url")
                elif t == "error":
                    print("[FAIL] 网关报错")
                    return 1
                elif t == "emotion" and evt.get("name") == "idle" and got_text:
                    break
                if got_text and audio:
                    # 再等一帧 idle 就收工（没有也不强求）
                    pass

    print(f"[OK] 拿到文字回答，音频={'有 ' + str(audio) if audio else '无（tts 关闭或失败）'}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
