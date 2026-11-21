# -*- coding: utf-8 -*-
"""记忆查询 CLI（S10 P6 的诊断配套）

与网关 /memory/query 同一份数据、同一种口径：只读 raw + annotation，
每条结果带 built_from（能点回原始事件）。

用法：
  python query.py --date-from 2026-11-09 --date-to 2026-11-15 --person 张总
  python query.py --q 方案 --json
  python query.py --commitments --date-from 2026-11-10
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

MEMORY_DIR = Path(os.environ.get("PET_MEMORY_DIR", "/opt/pet_memory"))


def days_between(a: str, b: str):
    from datetime import datetime, timedelta
    a = datetime.strptime(a.replace("-", ""), "%Y%m%d").date()
    b = datetime.strptime(b.replace("-", ""), "%Y%m%d").date()
    if b < a:
        a, b = b, a
    out = []
    while a <= b:
        out.append(a.strftime("%Y%m%d"))
        a += timedelta(days=1)
    return out


def collect(date_from: str, date_to: str, person: str = "", q: str = ""):
    import nightly
    items, speech_ms, total = [], 0, 0
    for day in days_between(date_from, date_to):
        for ev in nightly.read_events(day):
            total += 1
            text = nightly.event_text(ev).strip()
            if ev.get("kind") == "speech":
                speech_ms += int(ev.get("dur_ms") or 0)
            if person and (person not in text and f"person:{person}" not in (ev.get("tags") or [])):
                continue
            if q and q not in text:
                continue
            items.append({"id": ev.get("id"), "ts": ev.get("ts"), "kind": ev.get("kind"),
                          "text": text, "built_from": [ev.get("id")]})
    return {"items": items, "stats": {"events": total, "matched": len(items),
                                      "speech_ms": speech_ms}}


def commitments(date_from: str, date_to: str):
    out = []
    for day in days_between(date_from, date_to):
        p = MEMORY_DIR / "nightly" / f"{day}.report.json"
        if p.is_file():
            out.extend(json.loads(p.read_text(encoding="utf-8")).get("commitments") or [])
    return out


def main() -> None:
    global MEMORY_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--date-from", default=time.strftime("%Y-%m-%d"))
    ap.add_argument("--date-to", default="")
    ap.add_argument("--person", default="")
    ap.add_argument("--q", default="")
    ap.add_argument("--commitments", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--memory-dir", default="")
    args = ap.parse_args()
    if args.memory_dir:
        # 必须在 import nightly 之前设环境变量：它的 RAW_DIR 是模块级常量
        os.environ["PET_MEMORY_DIR"] = args.memory_dir
        MEMORY_DIR = Path(args.memory_dir)
    date_to = args.date_to or args.date_from

    if args.commitments:
        data = commitments(args.date_from, date_to)
        if args.json:
            print(json.dumps(data, ensure_ascii=False, indent=2))
        else:
            for c in data:
                print(f"- {c.get('person') or '(未定人)'}：{c.get('text')} "
                      f"[{','.join(c.get('built_from') or [])}]")
        return

    data = collect(args.date_from, date_to, args.person, args.q)
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
        return
    print(f"匹配 {data['stats']['matched']} / {data['stats']['events']} 条；"
          f"语音 {data['stats']['speech_ms'] / 60000:.1f} 分钟")
    for it in data["items"]:
        print(f"[{it['id']}] {it['ts']} {it['kind']}: {it['text']}")


if __name__ == "__main__":
    main()
