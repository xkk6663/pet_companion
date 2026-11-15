# -*- coding: utf-8 -*-
"""P3 自测：/slice 端点（不依赖设备、不依赖真 ASR）

覆盖：
  1. 正常一片 WAV → 200，raw 事件落盘，audio 落盘；
  2. 同 id 重传 → duplicate=true，且 raw 里仍只有一行（幂等）；
  3. >4 MB → 413；
  4. 空 body → 400；
  5. 非 WAV 裸 PCM → 也能落盘（ASR 关闭时 text 为空，夜间补算）。

跑法（项目根）：
  .venv\\Scripts\\python.exe tools\\memory\\selftest_slice.py
"""
import json
import math
import os
import struct
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "diag"))

# 必须先设环境变量再 import：ws_gateway 在模块级建目录
TMP = Path(tempfile.mkdtemp(prefix="pet_slice_"))
os.environ["PET_MEMORY_DIR"] = str(TMP)

import ws_gateway as gw  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

gw.CONFIG["asr_enabled"] = False          # 自测不依赖 AstrBot
client = TestClient(gw.app)


def make_wav(seconds: float = 1.0, rate: int = 16000) -> bytes:
    buf = bytearray()
    for i in range(int(seconds * rate)):
        v = int(1200 * math.sin(2 * math.pi * 440 * i / rate))
        buf += struct.pack("<h", v)
    bio = __import__("io").BytesIO()
    with wave.open(bio, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(buf))
    return bio.getvalue()


def check(name: str, cond: bool, detail: str = "") -> None:
    print(("PASS " if cond else "FAIL ") + name + ((" | " + detail) if detail else ""))
    if not cond:
        raise SystemExit(1)


wav = make_wav(1.0)
r1 = client.post("/slice/sid-test?id=evt_20261115_100000_a1&dur_ms=1000&ts=2026-11-15T10:00:00",
                 content=wav, headers={"Content-Type": "audio/wav"})
check("正常切片 200", r1.status_code == 200, f"status={r1.status_code} body={r1.text[:120]}")
j1 = r1.json()
check("返回 id 与字节数", j1.get("id") == "evt_20261115_100000_a1" and j1.get("bytes") == len(wav), str(j1))

day_dir = TMP / "raw" / "20261115"
raw_lines = (day_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()
check("raw 落一行", len(raw_lines) == 1, f"lines={len(raw_lines)}")
ev = json.loads(raw_lines[0])
check("事件字段齐", ev["kind"] == "speech" and ev["dur_ms"] == 1000 and ev["source"] == "mic.gate", str(ev)[:160])
check("audio 落盘", (TMP / "audio" / "20261115" / "evt_20261115_100000_a1.wav").is_file())

r2 = client.post("/slice/sid-test?id=evt_20261115_100000_a1&dur_ms=1000&ts=2026-11-15T10:00:00",
                 content=wav, headers={"Content-Type": "audio/wav"})
check("重传 200", r2.status_code == 200)
check("重传幂等", r2.json().get("duplicate") is True, str(r2.json()))
raw_lines2 = (day_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()
check("raw 仍只有一行", len(raw_lines2) == 1, f"lines={len(raw_lines2)}")

r3 = client.post("/slice/sid-test?id=evt_big", content=b"0" * (4 * 1024 * 1024 + 1))
check("超 4MB 返回 413", r3.status_code == 413, f"status={r3.status_code}")

r4 = client.post("/slice/sid-test?id=evt_empty", content=b"")
check("空 body 返回 400", r4.status_code == 400, f"status={r4.status_code}")

pcm = struct.pack("<h", 0) * 16000          # 1 s 裸 PCM
r5 = client.post("/slice/sid-test?id=evt_raw&dur_ms=1000", content=pcm,
                 headers={"Content-Type": "application/octet-stream"})
check("裸 PCM 也能落盘", r5.status_code == 200, f"status={r5.status_code}")

r6 = client.post("/memory/query", json={"type": "day", "date_from": "2026-11-15",
                                        "date_to": "2026-11-15"})
check("查询端点 200", r6.status_code == 200, f"status={r6.status_code}")
q = r6.json()
check("查到当天事件", q["stats"]["events"] >= 2 and len(q["items"]) >= 2, str(q["stats"]))
check("每条都带 built_from", all(i.get("built_from") for i in q["items"]))

r7 = client.get("/memory/resources")
check("resources 列表 4 个", r7.status_code == 200 and len(r7.json()["resources"]) == 4,
      str(r7.json())[:120])

r8 = client.post("/memory/resources/read",
                 json={"uri": "memory://events",
                       "params": {"date_from": "2026-11-15", "date_to": "2026-11-15"}})
check("resources/read 200", r8.status_code == 200, str(r8.status_code))
body = json.loads(r8.json()["contents"][0]["text"])
check("read 返回事件", body["stats"]["events"] >= 2, str(body["stats"]))

r9 = client.post("/memory/resources/read",
                 json={"uri": "memory://day", "params": {"date": "2026-11-16"}})
check("不存在的日报 404", r9.status_code == 404, str(r9.status_code))

print("全部通过；临时目录:", TMP)
