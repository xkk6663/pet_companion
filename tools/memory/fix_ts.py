#!/usr/bin/env python3
"""一次性修复 raw 事件流里被 URL 解码破坏的时区偏移。

背景：
    设备早期把 ``ts=2026-11-15T15:38:36+08:00`` 直接拼进 query string，
    FastAPI 把 ``+`` 解成空格，落盘成 ``2026-11-15T15:38:36 08:00``。
    设备侧已在 ``slice_client.c`` 把 ``+`` 编码为 ``%2B``，新事件正确；
    本脚本只修历史行，不新增/删除事件，不改音频。

用法：
    python fix_ts.py /opt/pet_memory/raw/20261115/events.jsonl
    python fix_ts.py --dry-run /opt/pet_memory/raw/20261115/events.jsonl

行为：
    - 只匹配 ``YYYY-MM-DDTHH:MM:SS HH:MM`` 这一种已知坏格式；
    - 原地重写前先备份为 ``events.jsonl.bak_tsfix_<UTC时间>``；
    - 幂等：没有需要修的行时不动文件。
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from datetime import datetime, timezone
from pathlib import Path

BAD_TS = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}) (\d{2}:\d{2})$")


def fix_line(line: str) -> tuple[str, bool]:
    line = line.rstrip("\n")
    if not line.strip():
        return line, False
    obj = json.loads(line)
    ts = obj.get("ts")
    if isinstance(ts, str):
        m = BAD_TS.match(ts)
        if m:
            obj["ts"] = f"{m.group(1)}+{m.group(2)}"
            return json.dumps(obj, ensure_ascii=False, separators=(",", ":")), True
    return line, False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    raw = args.path.read_text(encoding="utf-8").splitlines()
    fixed_lines = []
    fixed = 0
    for line in raw:
        new_line, changed = fix_line(line)
        fixed_lines.append(new_line)
        fixed += int(changed)

    print(f"{args.path}: lines={len(raw)} fixable={fixed}")
    if not fixed or args.dry_run:
        return 0

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup = args.path.with_name(f"{args.path.name}.bak_tsfix_{stamp}")
    shutil.copy2(args.path, backup)
    args.path.write_text("\n".join(fixed_lines) + "\n", encoding="utf-8")
    print(f"backup={backup}")
    print(f"rewritten={args.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
