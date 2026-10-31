#!/usr/bin/env python3
"""端到端闭环验证：云端 push → 设备 WS → event_bus → 设备日志。

为什么需要这个脚本：这条链路有 4 个断点（云端 SESSIONS / WS 传输 / cJSON 分发 /
UI 队列），断了以后**屏幕上都没反应**，只看现象完全无法定位。所以判据一律取
设备串口日志里的取证行：

    EVT: text <- ...        ← 走到了总线
    EVT: emotion <- ...     ← 表情帧
    EVT: audio <- http://.. ← 下行音频（TTS）

用法：
    python tools/diag/verify_pet_loop.py --ip 192.168.10.111
    python tools/diag/verify_pet_loop.py --ip 192.168.4.1 --gw http://120.76.230.17:8081 --no-tts

前置：设备已开、已连 WiFi、`/api/system/status` 的 link.connected 为 true。
串口默认 COM9，可用 --port 改。
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
import urllib.request

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：用 IDF venv 的 python 跑（C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe）")

# ★ 必须空代理：本机 Clash TUN 会劫持到 127.0.0.1 的 LAN/公网请求
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_json(url: str, data: dict | None = None, timeout: float = 8.0):
    body = None
    headers = {}
    if data is not None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers,
                                 method="POST" if data is not None else "GET")
    with _OPENER.open(req, timeout=timeout) as r:
        return r.status, json.loads(r.read().decode("utf-8", "replace"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.10.111", help="设备 IP")
    ap.add_argument("--port", default="COM9", help="串口")
    ap.add_argument("--gw", default="http://120.76.230.17:8081", help="网关 http 基址")
    ap.add_argument("--secs", type=float, default=25.0, help="抓日志时长")
    ap.add_argument("--text", default="桌宠链路自检：收到这句话说明云端推送通了。")
    ap.add_argument("--no-tts", action="store_true", help="不请求 TTS（只验文字/表情）")
    args = ap.parse_args()

    # ---- 1. 设备侧前置检查 ----
    try:
        _, st = http_json(f"http://{args.ip}/api/system/status")
    except Exception as e:  # noqa: BLE001
        print(f"[FAIL] 设备 {args.ip} 不可达：{e}")
        return 2
    link = st.get("link", {})
    sid = link.get("session", "")
    print(f"[1/4] 设备在线 ip={args.ip} heap_int={st.get('heap_internal')} "
          f"session={sid} link={link.get('connected')} "
          f"state={st.get('session', {}).get('state')}")
    if not link.get("connected"):
        print("[FAIL] WS 未连接：先确认设备已连 WiFi；ws_client 会 1s→30s 指数退避自动重连"
              "（不需要按任何键 —— 长连接自开机起就在）")
        return 2
    print(f"      session.state={st.get('session', {}).get('state')}"
          f"（IDLE 是正常的：会话只在按住录音键时才建）")

    # ---- 2. 开串口收集 ----
    ser = serial.Serial(args.port, 115200, timeout=0.3)
    ser.setDTR(False)
    ser.setRTS(False)
    lines: list[str] = []
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                raw = ser.readline()
            except Exception:  # noqa: BLE001
                break
            if raw:
                lines.append(raw.decode("utf-8", "replace").rstrip())

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    print(f"[2/4] 开始抓串口 {args.port}（{args.secs:.0f}s）")

    # ---- 3. 云端 push ----
    time.sleep(1.0)
    payload = {"session_id": sid, "text": args.text, "emotion": "happy",
               "tts": not args.no_tts}
    try:
        _, r = http_json(f"{args.gw.rstrip('/')}/push", payload, timeout=90)
    except Exception as e:  # noqa: BLE001
        stop.set(); ser.close()
        print(f"[FAIL] push 请求失败：{e}")
        return 2
    print(f"[3/4] 云端 push -> {r}")
    if not r.get("delivered"):
        stop.set(); ser.close()
        print(f"[FAIL] 云端判定未送达：{r.get('reason')}")
        return 2

    # ---- 4. 等日志收敛 ----
    deadline = time.time() + args.secs
    while time.time() < deadline:
        if any("EVT: audio <-" in ln for ln in lines):
            break
        time.sleep(0.3)
    time.sleep(1.5)
    stop.set()
    th.join(timeout=2)
    ser.close()

    def has(pat: str) -> bool:
        return any(re.search(pat, ln) for ln in lines)

    ok_text = has(r"EVT: text <-")
    ok_emo = has(r"EVT: emotion <-\s*happy")
    ok_audio = has(r"EVT: audio <-")
    ok_play = has(r"PET_SVC: play (tts|music):")

    print("[4/4] 设备侧取证")
    print(f"      text    <- {ok_text}")
    print(f"      emotion <- {ok_emo}")
    print(f"      audio   <- {ok_audio}")
    print(f"      playback   {ok_play}")
    if not ok_text:
        print("      --- 最后 15 行日志（排查用）---")
        for ln in lines[-15:]:
            print("      " + ln)

    if ok_text and (args.no_tts or (ok_audio and ok_play)):
        print("[PASS] 云端 → 设备 下行链路闭环")
        return 0
    print("[FAIL] 下行链路未闭环（看上面对证行的 false）")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
