# -*- coding: utf-8 -*-
"""把相邻的短切片合并成 30-60 秒的“完整语音组”。

背景：
  设备端 VAD 在每句话结束时立即收口，单个切片常见 1-3 秒；逐片送 ASR 会得到
  很多碎片文本。这里不改 raw，只在派生层把相邻切片按时间间隔合并，再对整组做一次
  ASR，得到更完整的文本和更适合回听的 WAV。

输入：
  raw/<YYYYMMDD>/events.jsonl + audio/<YYYYMMDD>/*.wav
输出：
  audio_grouped/<YYYYMMDD>/<group_id>.wav
  derived/<YYYYMMDD>.groups.json

用法：
  python group_audio.py --date 20261115
  python group_audio.py --date 20261115 --no-transcribe --max-s 45
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import wave
from datetime import datetime, timedelta
from pathlib import Path

DEFAULT_GAP_MS = 30000
DEFAULT_MAX_S = 60


def _parse_ts(ts: str) -> datetime | None:
    try:
        return datetime.fromisoformat((ts or "").replace("Z", "+00:00"))
    except ValueError:
        return None


def _read_events(day: str, memory_dir: Path) -> list[dict]:
    path = memory_dir / "raw" / day / "events.jsonl"
    if not path.is_file():
        return []
    rows: list[dict] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    base = {r.get("id"): r for r in rows if r.get("kind") != "annotation"}
    for r in rows:
        if r.get("kind") != "annotation":
            continue
        tgt = base.get(r.get("target"))
        if tgt is not None and r.get("field"):
            tgt.setdefault("_ann", {})[r["field"]] = r.get("value")
    return [r for r in rows if r.get("kind") != "annotation"]


def _merge_wavs(paths: list[Path], out_path: Path) -> tuple[bytes, int, int]:
    if not paths:
        raise ValueError("empty group")
    with wave.open(str(paths[0]), "rb") as first:
        params = first.getparams()
        nchannels, sampwidth, framerate = params.nchannels, params.sampwidth, params.framerate
        comptype, compname = params.comptype, params.compname
        frames = bytearray(first.readframes(first.getnframes()))
    total_ms = int(round(len(frames) * 1000 / (framerate * nchannels * sampwidth)))
    for p in paths[1:]:
        with wave.open(str(p), "rb") as w:
            if (w.getnchannels(), w.getsampwidth(), w.getframerate(),
                    w.getcomptype()) != (nchannels, sampwidth, framerate, comptype):
                raise ValueError(f"wav format mismatch: {p}")
            chunk = w.readframes(w.getnframes())
            frames.extend(chunk)
            total_ms += int(round(len(chunk) * 1000 / (framerate * nchannels * sampwidth)))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out_path), "wb") as out:
        out.setnchannels(nchannels)
        out.setsampwidth(sampwidth)
        out.setframerate(framerate)
        out.setcomptype(comptype, compname)
        out.writeframes(bytes(frames))
    return bytes(frames), framerate, total_ms


def _group_events(events: list[dict], memory_dir: Path,
                  gap_ms: int, max_s: int) -> list[list[dict]]:
    candidates: list[tuple[dict, datetime, datetime, int]] = []
    for ev in events:
        if ev.get("kind") != "speech":
            continue
        rel = str(ev.get("audio") or "")
        if not rel:
            continue
        p = memory_dir / rel
        if not p.is_file():
            continue
        start = _parse_ts(str(ev.get("ts") or ""))
        dur = int(ev.get("dur_ms") or 0)
        if start is None or dur <= 0:
            continue
        end = start + timedelta(milliseconds=dur)
        candidates.append((ev, start, end, dur))
    candidates.sort(key=lambda x: x[1])

    groups: list[list[dict]] = []
    current: list[dict] = []
    current_end: datetime | None = None
    current_ms = 0
    for ev, start, end, dur in candidates:
        if not current:
            current = [ev]
            current_end = end
            current_ms = dur
            continue
        gap = (start - current_end).total_seconds() * 1000 if current_end else 999999
        if gap <= gap_ms and current_ms + dur <= max_s * 1000:
            current.append(ev)
            current_end = end
            current_ms += dur
        else:
            groups.append(current)
            current = [ev]
            current_end = end
            current_ms = dur
    if current:
        groups.append(current)
    return groups


async def _transcribe_one(g: dict, sem: asyncio.Semaphore, gw) -> None:
    async with sem:
        try:
            g["text"] = ((await gw.stt_transcribe(g.pop("_pcm"), g["rate"])) or "").strip()
            g["asr_done"] = True
            g.pop("error", None)
        except Exception as exc:  # noqa: BLE001
            g["text"] = ""
            g["error"] = str(exc)[:200]


async def _transcribe_groups(groups: list[dict], gw, concurrency: int = 3) -> None:
    sem = asyncio.Semaphore(max(1, int(concurrency)))
    await asyncio.gather(*(_transcribe_one(g, sem, gw) for g in groups))


def build_groups(day: str, events: list[dict], memory_dir: Path,
                 gw_dir: Path | None = None, transcribe: bool = True,
                 gap_ms: int = DEFAULT_GAP_MS, max_s: int = DEFAULT_MAX_S) -> list[dict]:
    day = str(day).replace("-", "")
    out_dir = memory_dir / "audio_grouped" / day
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = memory_dir / "derived" / f"{day}.groups.json"
    # 重跑时复用已有组文本：组 id 和成员 id 都没变就不重复调 ASR。
    # 这既是省钱，也让“补跑一天”不会把同一段音频再转一遍。
    prev: dict[str, dict] = {}
    prev_ids: dict[tuple[str, ...], dict] = {}
    if manifest.is_file():
        try:
            old = json.loads(manifest.read_text(encoding="utf-8"))
            for g in old.get("groups") or []:
                if isinstance(g, dict) and g.get("id"):
                    prev[str(g["id"])] = g
                    key = tuple(sorted(str(x) for x in (g.get("ids") or [])))
                    if key:
                        prev_ids[key] = g
        except Exception as exc:  # noqa: BLE001
            print(f"[group] previous manifest ignored: {exc}", file=sys.stderr)
    reused = 0
    groups: list[dict] = []
    seq = 0
    for chunk in _group_events(events, memory_dir, gap_ms, max_s):
        seq += 1
        first = chunk[0]
        start = _parse_ts(str(first.get("ts") or ""))
        if start is None:
            continue
        group_id = f"grp_{day}_{start.strftime('%H%M%S')}_{seq:02d}"
        out_path = out_dir / f"{group_id}.wav"
        rel_audio = str(out_path.relative_to(memory_dir))
        try:
            pcm, rate, dur_ms = _merge_wavs(
                [memory_dir / str(e.get("audio")) for e in chunk], out_path)
        except Exception as exc:  # noqa: BLE001
            print(f"[group] merge failed {group_id}: {exc}", file=sys.stderr)
            continue
        last = chunk[-1]
        last_start = _parse_ts(str(last.get("ts") or ""))
        end_ts = (last_start + timedelta(milliseconds=int(last.get("dur_ms") or 0))
                  ).isoformat() if last_start else str(last.get("ts") or "")
        ids = [str(e.get("id")) for e in chunk]
        g = {
            "id": group_id,
            "start_ts": str(first.get("ts") or ""),
            "end_ts": end_ts,
            "dur_ms": dur_ms,
            "ids": ids,
            "audio": rel_audio,
            "rate": rate,
            "text": "",
            "model": "SenseVoiceSmall",
            "pipeline": "batch-group",
            "gap_ms": gap_ms,
            "max_s": max_s,
        }
        old = prev.get(group_id) or prev_ids.get(tuple(sorted(ids))) or {}
        same_ids = tuple(sorted(str(x) for x in (old.get("ids") or []))) == tuple(sorted(ids))
        if same_ids and (old.get("text") or old.get("asr_done")) and not old.get("error"):
            # 空文本也可能是“这段确实没人声/只有噪声”的成功 ASR 结果；
            # 只要上次成功转过且组员没变，就不重复烧 token。
            g["text"] = str(old.get("text") or "")
            g["asr_done"] = True
            g["cached"] = True
            reused += 1
        elif transcribe:
            g["_pcm"] = pcm
        groups.append(g)

    todo = [g for g in groups if g.get("_pcm")]
    if todo:
        try:
            sys.path.insert(0, str(gw_dir or os.environ.get("PET_GW_DIR", "/opt/pet_gateway")))
            import ws_gateway as gw  # noqa: F401
            asyncio.run(_transcribe_groups(todo, gw))
        except Exception as exc:  # noqa: BLE001
            print(f"[group] ASR unavailable, audio kept only: {exc}", file=sys.stderr)
            for g in todo:
                g.pop("_pcm", None)
                g.setdefault("error", "asr unavailable")

    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({
        "date": day,
        "gap_ms": gap_ms,
        "max_s": max_s,
        "reused": reused,
        "groups": groups,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    return groups


def main() -> int:
    parser = argparse.ArgumentParser(description="Group adjacent desktop speech slices")
    parser.add_argument("--date", required=True)
    parser.add_argument("--memory-dir", default=os.environ.get("PET_MEMORY_DIR", "/opt/pet_memory"))
    parser.add_argument("--gw-dir", default=os.environ.get("PET_GW_DIR", "/opt/pet_gateway"))
    parser.add_argument("--gap-ms", type=int, default=DEFAULT_GAP_MS)
    parser.add_argument("--max-s", type=int, default=DEFAULT_MAX_S)
    parser.add_argument("--no-transcribe", action="store_true")
    args = parser.parse_args()
    memory_dir = Path(args.memory_dir).resolve()
    events = _read_events(args.date, memory_dir)
    groups = build_groups(args.date, events, memory_dir, Path(args.gw_dir),
                          transcribe=not args.no_transcribe,
                          gap_ms=args.gap_ms, max_s=args.max_s)
    speech_min = sum(int(g["dur_ms"]) for g in groups) / 60000.0
    print(f"[group] {args.date}: events={len(events)} groups={len(groups)} "
          f"speech_min={speech_min:.2f} gap_ms={args.gap_ms} max_s={args.max_s} "
          f"transcribed={sum(1 for g in groups if g.get('text'))}")
    for g in groups[:8]:
        print(f"[group] {g['id']} {g['dur_ms']}ms ids={len(g['ids'])} -> {g.get('text') or ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
