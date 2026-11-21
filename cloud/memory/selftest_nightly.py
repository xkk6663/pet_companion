# -*- coding: utf-8 -*-
"""P5 自测：夜间轨（补转写 / 统计 / 可复现 / 无 LLM 不编故事）

跑法：
  .venv\\Scripts\\python.exe tools\\memory\\selftest_nightly.py
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
sys.path.insert(0, str(ROOT / "tools" / "memory"))
sys.path.insert(0, str(ROOT / "tools" / "diag"))

TMP = Path(tempfile.mkdtemp(prefix="pet_nightly_"))
os.environ["PET_MEMORY_DIR"] = str(TMP)
DAY = "20261115"


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + ((" | " + detail) if detail else ""))
    if not cond:
        raise SystemExit(1)


def make_wav(path: Path, seconds=1.0):
    buf = bytearray()
    for i in range(int(seconds * 16000)):
        buf += struct.pack("<h", int(900 * math.sin(2 * math.pi * 440 * i / 16000)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(bytes(buf))


# 造数据：一条带文本、一条空文本（模拟断网积压）
raw = TMP / "raw" / DAY
aud = TMP / "audio" / DAY
raw.mkdir(parents=True, exist_ok=True)
aud.mkdir(parents=True, exist_ok=True)
make_wav(aud / "evt_a.wav")
make_wav(aud / "evt_b.wav")
evs = [
    {"id": "evt_a", "ts": "2026-11-15T09:00:03+08:00", "kind": "speech", "source": "mic.gate",
     "dur_ms": 3000, "audio": f"audio/{DAY}/evt_a.wav",
     "asr": {"text": "我答应周五把方案发给张总", "pipeline": "online"}, "schema": 1},
    {"id": "evt_b", "ts": "2026-11-15T09:05:10+08:00", "kind": "speech", "source": "mic.gate",
     "dur_ms": 4000, "audio": f"audio/{DAY}/evt_b.wav",
     "asr": {"text": "", "pipeline": "online"}, "schema": 1},
]
(raw / "events.jsonl").write_text(
    "\n".join(json.dumps(e, ensure_ascii=False) for e in evs) + "\n", encoding="utf-8")

import nightly  # noqa: E402
import ws_gateway as gw  # noqa: E402

# 让"补转写"可控：替换成固定文本（真实环境是调 ASR）
async def fake_stt(pcm, rate=16000):
    return "明天下午三点开会"

gw.stt_transcribe = fake_stt

r1 = nightly.run(DAY, use_llm=False)           # 不补转写，验证统计与确定性
check("无 LLM 时 commitments 来自规则", r1["source"] == "rule")
check("统计事件数=2", r1["stats"]["events"] == 2, str(r1["stats"]))
check("语音时长 7s", r1["stats"]["speech_ms"] == 7000, str(r1["stats"]))
check("规则抽到承诺", len(r1["commitments"]) >= 1, str(r1["commitments"])[:120])
check("承诺带 built_from", r1["commitments"][0]["built_from"] == ["evt_a"], str(r1["commitments"][0]))

r2 = nightly.run(DAY, use_llm=False)
a = dict(r1); b = dict(r2)
a.pop("generated_at"); b.pop("generated_at")
check("两次跑结果一致（可复现）", a == b)

# 补转写路径：use_llm=True。现在优先走云端分组，所以可能 group_asr>0 而不是逐片 annotation。
gw.astrbot_chat = None                    # 让 llm_extract 干净失败
r3 = nightly.run(DAY, use_llm=True)
group_asr = int(r3.get("group_asr") or 0)
fixed = int(r3.get("backlog_fixed") or 0)
check("至少完成一条分组/补转写", group_asr + fixed >= 1,
      f"group_asr={group_asr} backlog_fixed={fixed}")
raw_text = (raw / "events.jsonl").read_text(encoding="utf-8").splitlines()
if group_asr:
    check("分组转写不改 raw", len(raw_text) == 2, f"lines={len(raw_text)}")
    check("组文本非空", any(g.get("text") for g in r3.get("groups") or []))
else:
    check("补转写追加 annotation", len(raw_text) == 3, f"lines={len(raw_text)}")
    check("annotation 是追加的", json.loads(raw_text[-1])["kind"] == "annotation")

after = nightly.read_events(DAY)
b_ev = [e for e in after if e["id"] == "evt_b"][0]
if group_asr:
    check("分组模式 raw evt_b 仍为空（组文本在 report）",
          nightly.event_text(b_ev) == "", nightly.event_text(b_ev))
else:
    check("补的文本读到覆盖值", nightly.event_text(b_ev) == "明天下午三点开会",
          nightly.event_text(b_ev))

check("SQLite 已建库", (TMP / "derived" / "memory.db").is_file())
check("日报已落盘", (TMP / "nightly" / f"{DAY}.report.json").is_file())
print("全部通过；临时目录:", TMP)
