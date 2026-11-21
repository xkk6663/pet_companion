# -*- coding: utf-8 -*-
"""按天回收桌面监听音频与原始事件。

策略与 S10 §4.6 / D6 对齐：
  · 音频默认保留 30 天；
  · 原始转写默认保留 180 天；
  · 只删 audio/<YYYYMMDD>/、audio_grouped/<YYYYMMDD>/ 与 raw/<YYYYMMDD>/，
    保留 nightly/*.report.json、derived/*.json 与 memory.db 里的统计/摘要；
  · 默认 dry-run，加 --apply 才真正删除；
  · 只处理严格 8 位日期目录，跳过符号链接与非法目录名。

用法：
  python retention.py --dry-run
  python retention.py --apply
  PET_AUDIO_RETENTION_DAYS=7 python retention.py --apply
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

DAY_RE = re.compile(r"^\d{8}$")


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, str(default)))
    except ValueError:
        return default


def _parse_day(name: str) -> date | None:
    try:
        return datetime.strptime(name, "%Y%m%d").date()
    except ValueError:
        return None


def _dir_stats(path: Path) -> tuple[int, int]:
    files = 0
    total = 0
    for root, _dirs, names in os.walk(path):
        for name in names:
            p = Path(root) / name
            try:
                if p.is_file() and not p.is_symlink():
                    files += 1
                    total += p.stat().st_size
            except OSError:
                pass
    return files, total


def _human_bytes(n: int) -> str:
    value = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} GB"


def _old_dirs(base: Path, cutoff: date):
    if not base.is_dir():
        return
    base_resolved = base.resolve()
    for child in sorted(base.iterdir()):
        if child.is_symlink() or not child.is_dir():
            continue
        if not DAY_RE.match(child.name):
            continue
        day = _parse_day(child.name)
        if day is None or day >= cutoff:
            continue
        resolved = child.resolve()
        if base_resolved not in resolved.parents:
            print(f"[retention] skip unsafe path: {resolved}", file=sys.stderr)
            continue
        yield child, day


def main() -> int:
    parser = argparse.ArgumentParser(description="Pet memory retention cleanup")
    parser.add_argument("--memory-dir", default=os.environ.get("PET_MEMORY_DIR", "/opt/pet_memory"))
    parser.add_argument("--audio-days", type=int,
                        default=_env_int("PET_AUDIO_RETENTION_DAYS", 30))
    parser.add_argument("--raw-days", type=int,
                        default=_env_int("PET_RAW_RETENTION_DAYS", 180))
    parser.add_argument("--apply", action="store_true",
                        help="真正删除；不加时只打印将要删除的内容")
    parser.add_argument("--dry-run", action="store_true",
                        help="显式 dry-run（默认行为；与 --apply 互斥）")
    args = parser.parse_args()

    if args.apply and args.dry_run:
        print("[retention] --apply and --dry-run are mutually exclusive", file=sys.stderr)
        return 2

    if args.audio_days < 1 or args.raw_days < 1:
        print("[retention] days must be >= 1", file=sys.stderr)
        return 2

    root = Path(args.memory_dir).expanduser().resolve()
    if root == Path("/") or not root.is_dir():
        print(f"[retention] unsafe or missing memory dir: {root}", file=sys.stderr)
        return 2

    today = date.today()
    audio_cutoff = today - timedelta(days=args.audio_days)
    raw_cutoff = today - timedelta(days=args.raw_days)
    mode = "APPLY" if args.apply else "DRY-RUN"
    print(f"[retention] mode={mode} root={root} "
          f"audio_days={args.audio_days} raw_days={args.raw_days}")

    targets = (
        ("audio", root / "audio", audio_cutoff),
        ("audio_grouped", root / "audio_grouped", audio_cutoff),
        ("raw", root / "raw", raw_cutoff),
    )
    deleted_dirs = 0
    deleted_files = 0
    deleted_bytes = 0
    errors = 0

    for label, base, cutoff in targets:
        for path, day in _old_dirs(base, cutoff):
            files, total = _dir_stats(path)
            print(f"[retention] {'DELETE' if args.apply else 'WOULD DELETE'} "
                  f"{label}/{path.name} day={day.isoformat()} "
                  f"files={files} bytes={total} ({_human_bytes(total)})")
            if not args.apply:
                continue
            try:
                shutil.rmtree(path)
                deleted_dirs += 1
                deleted_files += files
                deleted_bytes += total
            except OSError as e:
                errors += 1
                print(f"[retention] delete failed {path}: {e}", file=sys.stderr)

    print(f"[retention] done: dirs={deleted_dirs} files={deleted_files} "
          f"bytes={deleted_bytes} ({_human_bytes(deleted_bytes)}) errors={errors}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
