# -*- coding: utf-8 -*-
"""夜间轨（S10 P5）：补转写 + 抽取 + 统计 + 日报

职责（与 §4.4 对齐）：
  1. 只处理"白天没拿到文本"的切片（断网积压/ASR 失败），补转写成 annotation 事件；
  2. 用 LLM 抽取人 / 承诺 / 话题（失败则退化为规则抽取，不编造）；
  3. 统计（事件数、语音时长、首末时刻）；
  4. 产出 derived/<day>.json、derived/memory.db、nightly/<day>.report.json。

纪律：
  - 原始层只追加：补转写写成 kind=annotation 的新行，不改原事件；
  - 可复现：同一份输入跑两次，除 generated_at 外逐字段一致；
  - 无 LLM 时不猜：people/commitments 为空数组并标 source="rule"。

用法：
  python nightly.py --date 2026-11-15
  python nightly.py --date 2026-11-15 --no-llm
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
import time
import uuid
import wave
from pathlib import Path

try:
    import group_audio
except Exception:  # noqa: BLE001
    group_audio = None  # type: ignore[assignment]

ROOT = Path(__file__).resolve().parents[2]
GW_DIR = Path(os.environ.get("PET_GW_DIR", str(ROOT / "tools" / "diag")))
# 部署到 /opt/pet_memory/bin 时 ROOT 会算错；退回网关的实际安装目录
if not (GW_DIR / "ws_gateway.py").is_file() and Path("/opt/pet_gateway/ws_gateway.py").is_file():
    GW_DIR = Path("/opt/pet_gateway")

MEMORY_DIR = Path(os.environ.get("PET_MEMORY_DIR", "/opt/pet_memory"))
RAW_DIR = MEMORY_DIR / "raw"
AUDIO_DIR = MEMORY_DIR / "audio"
DERIVED_DIR = MEMORY_DIR / "derived"
NIGHTLY_DIR = MEMORY_DIR / "nightly"

# 少于这个语音时长不启动分组/ASR/LLM，避免一声“嗯”也烧一次 token。
# 需要放宽时用 PET_MIN_SPEECH_MS 覆盖，例如 1000。
MIN_SPEECH_MS = int(os.environ.get("PET_MIN_SPEECH_MS", "5000"))


def ensure_dirs() -> None:
    for d in (RAW_DIR, AUDIO_DIR, DERIVED_DIR, NIGHTLY_DIR):
        d.mkdir(parents=True, exist_ok=True)


def read_events(day: str) -> list[dict]:
    """读一天 raw，把 annotation 合并成覆盖值（原始行不改）。"""
    path = RAW_DIR / day / "events.jsonl"
    if not path.is_file():
        return []
    rows: list[dict] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    base = {r.get("id"): r for r in rows if r.get("kind") != "annotation"}
    for r in rows:
        if r.get("kind") == "annotation":
            tgt = base.get(r.get("target"))
            if tgt is not None and r.get("field"):
                tgt.setdefault("_ann", {})[r["field"]] = r.get("value")
    return [r for r in rows if r.get("kind") != "annotation"]


def event_text(ev: dict) -> str:
    ann = ev.get("_ann") or {}
    if ann.get("asr.text"):
        return str(ann["asr.text"])
    return str((ev.get("asr") or {}).get("text") or "")


def append_annotation(day: str, target: str, field: str, value: str, reason: str) -> str:
    ann_id = f"ann_{day}_{uuid.uuid4().hex[:8]}"
    row = {
        "id": ann_id, "ts": time.strftime("%Y-%m-%dT%H:%M:%S+08:00"),
        "kind": "annotation", "target": target, "field": field,
        "value": value, "reason": reason, "schema": 1,
    }
    path = RAW_DIR / day / "events.jsonl"
    with open(path, "a", encoding="utf-8") as fp:
        fp.write(json.dumps(row, ensure_ascii=False) + "\n")
        fp.flush()
        os.fsync(fp.fileno())
    return ann_id


def wav_to_pcm(path: Path) -> tuple[bytes, int]:
    with wave.open(str(path), "rb") as w:
        return w.readframes(w.getnframes()), w.getframerate()


def transcribe_backlog(day: str) -> int:
    """把 asr 空的切片补写成 annotation。返回补了几条。"""
    try:
        sys.path.insert(0, str(GW_DIR))
        import ws_gateway as gw  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        print(f"[nightly] 网关模块不可用，跳过补转写: {exc}")
        return 0

    fixed = 0
    for ev in read_events(day):
        if ev.get("kind") != "speech" or event_text(ev):
            continue
        p = event_audio_path(ev)
        if not p or not p.is_file():
            continue
        try:
            pcm, rate = wav_to_pcm(p)
            text = __import__("asyncio").run(gw.stt_transcribe(pcm, rate))
            if text.strip():
                append_annotation(day, ev["id"], "asr.text", text.strip(), "nightly-retry")
                fixed += 1
        except Exception as exc:  # noqa: BLE001
            print(f"[nightly] 补转写失败 {ev.get('id')}: {exc}")
    return fixed


LLM_PROMPT = """你是"桌面复盘整理器"。输入是某人一天桌面前的声音转写（每行 时间|文本）。
把这一天整理成晚上可以复盘的结构；只写转写里**明确出现**的内容，不推测、不补细节。
只输出一个 JSON 对象，不要解释：
{"headline":"一句话概括今天桌面前发生了什么",
 "themes":[{"name":"主题名","summary":"1-2 句","evidence":["原句片段"]}],
 "timeline":[{"period":"上午/下午/晚上或 HH:MM","summary":"这段时间在做什么","evidence":["原句片段"]}],
 "decisions":[{"text":"明确的结论/决定/取舍","evidence":["原句片段"]}],
 "learning":[{"point":"学到的知识/术语/待查问题","kind":"knowledge|term|question","evidence":["原句片段"]}],
 "open_loops":[{"text":"没做完/待跟进的事","due":"YYYY-MM-DD 或空串","evidence":["原句片段"]}],
 "next_actions":[{"text":"明天可以接着做的一件事","evidence":["原句片段"]}],
 "topics":["关键词"],
 "people":[{"name":"称呼","evidence":["原句片段"]}],
 "commitments":[{"text":"答应的事","person":"相关人，空串表示不确定","due":"YYYY-MM-DD 或空串","evidence":["原句片段"]}]}
没有就返回空数组；主题、时间线、结论、学习点各不超过 5 条；不要编造。"""


def _clean_items(data: dict, key: str, fields: tuple[str, ...], limit: int = 8) -> list[dict]:
    """把 LLM 返回的列表字段收敛成稳定结构；非法项直接丢。"""
    raw = data.get(key)
    if not isinstance(raw, list):
        return []
    out = []
    for it in raw:
        if isinstance(it, str):
            it = {fields[0]: it}
        if not isinstance(it, dict):
            continue
        item = {}
        for f in fields:
            if f == "evidence":
                item[f] = [str(x).strip() for x in (it.get(f) or []) if str(x).strip()][:3]
            else:
                item[f] = str(it.get(f) or "").strip()
        if not item.get(fields[0]):
            continue
        out.append(item)
    return out[:limit]


def llm_extract(lines: list[str]):
    """调 AstrBot 的 LLM；不可用返回 None。"""
    try:
        sys.path.insert(0, str(GW_DIR))
        import ws_gateway as gw
        prompt = LLM_PROMPT + "\n\n输入：\n" + "\n".join(lines)
        reply = __import__("asyncio").run(gw.astrbot_chat("pet-memory-nightly", prompt))
    except Exception as exc:  # noqa: BLE001
        print(f"[nightly] LLM 不可用，退化规则抽取: {exc}")
        return None
    text = (reply or "").strip()
    m = re.search(r"\{[\s\S]*\}", text)
    if not m:
        print("[nightly] LLM 未返回 JSON，退化规则抽取")
        return None
    try:
        data = json.loads(m.group(0))
        assert isinstance(data, dict)
        return {
            "headline": str(data.get("headline") or "").strip(),
            "themes": _clean_items(data, "themes", ("name", "summary", "evidence")),
            "timeline": _clean_items(data, "timeline", ("period", "summary", "evidence")),
            "decisions": _clean_items(data, "decisions", ("text", "evidence")),
            "learning": _clean_items(data, "learning", ("point", "kind", "evidence")),
            "open_loops": _clean_items(data, "open_loops", ("text", "due", "evidence")),
            "next_actions": _clean_items(data, "next_actions", ("text", "evidence"), limit=3),
            "people": [p for p in (data.get("people") or []) if isinstance(p, dict) and p.get("name")],
            "commitments": [c for c in (data.get("commitments") or []) if isinstance(c, dict) and c.get("text")],
            "topics": [str(t) for t in (data.get("topics") or []) if t],
        }
    except Exception as exc:  # noqa: BLE001
        print(f"[nightly] LLM JSON 解析失败: {exc}")
        return None


COMMIT_RE = re.compile(r"(答应|承诺|说好|保证|回头|明天|后天|周五|周末|之前).*", re.S)


def rule_extract(events: list[dict]):
    """无 LLM 时的保守抽取：只认带承诺词的句子，不抽人物/话题。"""
    commits = []
    for ev in events:
        t = event_text(ev).strip()
        if t and COMMIT_RE.search(t):
            commits.append({"text": t, "person": "", "due": "",
                            "evidence": [t], "built_from": [ev.get("id")]})
    return {"headline": "", "themes": [], "timeline": [], "decisions": [],
            "learning": [], "open_loops": [], "next_actions": [],
            "people": [], "commitments": commits, "topics": []}


def attach_built_from(events: list[dict], items: list[dict],
                      extra_events: list[dict] | None = None) -> list[dict]:
    """把 evidence 文本反查到事件 id 上，形成 built_from。

    extra_events 是云端合并组（一条文本对应多个原事件）。命中组文本时，
    把组内所有原事件 id 都写入 built_from。
    """
    out = []
    for it in items:
        ids = []
        ev_words: list[tuple[str, str, list[str]]] = [
            (str(ev.get("id")), event_text(ev), [str(ev.get("id"))]) for ev in events
        ]
        for g in (extra_events or []):
            if g.get("text"):
                ev_words.append((str(g.get("id")), str(g.get("text")),
                                 [str(x) for x in (g.get("ids") or [])]))
        for e in (it.get("evidence") or []):
            for _, txt, id_list in ev_words:
                if e and e in txt:
                    ids.extend(id_list)
        it = dict(it)
        it["built_from"] = sorted(set(ids)) if ids else sorted(
            str(x) for x in (it.get("built_from") or []))
        out.append(it)
    return out


def event_audio_path(ev: dict) -> Path | None:
    raw = str(ev.get("audio") or "").strip()
    if not raw:
        return None
    p = Path(raw)
    return p if p.is_absolute() else MEMORY_DIR / p


def compute_stats(events: list[dict]) -> dict:
    speech = [e for e in events if e.get("kind") == "speech"]
    ms = sum(int(e.get("dur_ms") or 0) for e in speech)
    text_count = sum(1 for e in speech if event_text(e))
    audio_count = sum(
        1 for e in speech
        if (p := event_audio_path(e)) is not None and p.is_file()
    )
    stamps = sorted(e.get("ts") for e in events if e.get("ts"))
    return {
        "events": len(events),
        "speech": len(speech),
        "speech_ms": ms,
        "speech_min": round(ms / 60000.0, 2),
        "text_count": text_count,
        "audio_count": audio_count,
        "first_ts": stamps[0] if stamps else "",
        "last_ts": stamps[-1] if stamps else "",
    }


def write_empty_report(day: str, stats: dict, reason: str) -> dict:
    """空数据短路：不分组、不 ASR、不 LLM，只留一个标记给插件跳过推送。"""
    report = {
        "date": day,
        "schema_version": 2,
        "empty": True,
        "reason": reason,
        "headline": "",
        "themes": [],
        "timeline": [],
        "decisions": [],
        "learning": [],
        "open_loops": [],
        "next_actions": [],
        "stats": stats,
        "groups": [],
        "people": [],
        "commitments": [],
        "topics": [],
        "source": "empty",
        "backlog_fixed": 0,
        "group_asr": 0,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S+08:00"),
    }
    (DERIVED_DIR / f"{day}.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    (NIGHTLY_DIR / f"{day}.report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    print(f"[nightly] {day}: empty, skip ASR/LLM ({reason}) "
          f"speech={stats.get('speech_min', 0)}min text={stats.get('text_count', 0)} "
          f"audio={stats.get('audio_count', 0)}")
    return report


def write_sqlite(day: str, people: list[dict], commitments: list[dict]) -> None:
    db = DERIVED_DIR / "memory.db"
    con = sqlite3.connect(str(db))
    try:
        con.execute("CREATE TABLE IF NOT EXISTS people("
                    "name TEXT PRIMARY KEY, first_seen TEXT, last_seen TEXT, mentions INTEGER)")
        con.execute("CREATE TABLE IF NOT EXISTS commitments("
                    "id TEXT PRIMARY KEY, day TEXT, person TEXT, text TEXT, due TEXT, "
                    "status TEXT, built_from TEXT)")
        for p in people:
            name = p["name"]
            con.execute(
                "INSERT INTO people(name,first_seen,last_seen,mentions) VALUES(?,?,?,1) "
                "ON CONFLICT(name) DO UPDATE SET last_seen=excluded.last_seen, "
                "mentions=people.mentions+1",
                (name, day, day))
        for i, c in enumerate(commitments):
            cid = c.get("id") or f"cm_{day}_{i:03d}"
            con.execute(
                "INSERT OR REPLACE INTO commitments(id,day,person,text,due,status,built_from) "
                "VALUES(?,?,?,?,?,?,?)",
                (cid, day, c.get("person", ""), c.get("text", ""), c.get("due", ""),
                 c.get("status", "open"), json.dumps(c.get("built_from") or [], ensure_ascii=False)))
        con.commit()
    finally:
        con.close()


def run(day: str, use_llm: bool = True) -> dict:
    ensure_dirs()
    events = read_events(day)
    stats = compute_stats(events)

    # 空数据/过短/无音频无文本：直接写 empty 标记，不调分组 ASR，也不调 LLM。
    if stats["speech"] <= 0:
        return write_empty_report(day, stats, "no_speech")
    if int(stats["speech_ms"]) < MIN_SPEECH_MS:
        return write_empty_report(day, stats, "speech_too_short")
    if int(stats["text_count"]) <= 0 and int(stats["audio_count"]) <= 0:
        return write_empty_report(day, stats, "no_text_no_audio")

    # 云端分组：把相邻短切片合成 30-60 秒的完整语音段，再对整段做一次 ASR。
    # 组文本用于夜间复盘；raw 不改，manifest 写 derived/<day>.groups.json。
    groups: list[dict] = []
    built_groups: list[dict] = []
    if use_llm and group_audio is not None and events and int(stats["audio_count"]) > 0:
        try:
            built_groups = group_audio.build_groups(
                day, events, MEMORY_DIR, GW_DIR, transcribe=True,
                gap_ms=30000, max_s=60)
            groups = [g for g in built_groups if g.get("text")]
        except Exception as exc:  # noqa: BLE001
            print(f"[nightly] 云端分组失败，回退逐片: {exc}")
            built_groups = []
            groups = []

    fixed = 0
    # 只有“分组根本没建起来”才回退逐片；如果组已建好但全是空文本，
    # 说明这段确实没人声/ASR 已成功返回空，不要再逐片重烧一次。
    if use_llm and not built_groups:
        fixed = transcribe_backlog(day)
        events = read_events(day)

    stats = compute_stats(events)
    if groups:
        stats["group_count"] = len(groups)
        stats["group_speech_min"] = round(
            sum(int(g.get("dur_ms") or 0) for g in groups) / 60000.0, 2)

    if groups:
        lines = [f"{g.get('start_ts','')}|{g.get('text','')}" for g in groups
                 if g.get("text")]
    else:
        lines = [f"{e.get('ts','')}|{event_text(e)}" for e in events
                 if e.get("kind") == "speech" and event_text(e)]

    # 有音频但 ASR 全失败，或没有任何可用文本：不要再让 LLM 空跑。
    if not lines:
        return write_empty_report(day, stats, "no_usable_text")

    extracted = llm_extract(lines) if use_llm else None
    source = "llm"
    if extracted is None:
        extracted = rule_extract(events)
        source = "rule"

    extra = groups or None
    people = attach_built_from(events, extracted.get("people") or [], extra_events=extra)
    commitments = attach_built_from(events, [
        dict(c, id=f"cm_{day}_{i:03d}") for i, c in enumerate(extracted.get("commitments") or [])
    ], extra_events=extra)
    topics = sorted({str(t).strip() for t in (extracted.get("topics") or []) if str(t).strip()})
    headline = str(extracted.get("headline") or "").strip()
    themes = attach_built_from(events, extracted.get("themes") or [], extra_events=extra)
    timeline = attach_built_from(events, extracted.get("timeline") or [], extra_events=extra)
    decisions = attach_built_from(events, extracted.get("decisions") or [], extra_events=extra)
    learning = attach_built_from(events, extracted.get("learning") or [], extra_events=extra)
    open_loops = attach_built_from(events, extracted.get("open_loops") or [], extra_events=extra)
    next_actions = attach_built_from(events, extracted.get("next_actions") or [], extra_events=extra)
    if not topics:
        topics = [str(t.get("name")) for t in themes if t.get("name")]

    report = {
        "date": day,
        "schema_version": 2,
        "empty": False,
        "reason": "",
        "headline": headline,
        "themes": themes,
        "timeline": timeline,
        "decisions": decisions,
        "learning": learning,
        "open_loops": open_loops,
        "next_actions": next_actions,
        "stats": stats,
        "groups": [
            {"id": g.get("id"), "start_ts": g.get("start_ts"), "end_ts": g.get("end_ts"),
             "dur_ms": g.get("dur_ms"), "ids": g.get("ids"), "audio": g.get("audio"),
             "text": g.get("text")}
            for g in groups
        ],
        "people": people,
        "commitments": commitments,
        "topics": topics,
        "source": source,
        "backlog_fixed": fixed,
        "group_asr": len(groups),
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S+08:00"),
    }
    (DERIVED_DIR / f"{day}.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    (NIGHTLY_DIR / f"{day}.report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    write_sqlite(day, people, commitments)
    print(f"[nightly] {day}: events={stats['events']} speech={stats['speech_min']}min "
          f"groups={len(groups)} themes={len(themes)} timeline={len(timeline)} "
          f"learning={len(learning)} open_loops={len(open_loops)} "
          f"source={source} fixed={fixed}")
    return report


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=time.strftime("%Y%m%d"))
    ap.add_argument("--no-llm", action="store_true")
    ap.add_argument("--memory-dir", default="")
    args = ap.parse_args()
    if args.memory_dir:
        global MEMORY_DIR, RAW_DIR, AUDIO_DIR, DERIVED_DIR, NIGHTLY_DIR
        MEMORY_DIR = Path(args.memory_dir)
        RAW_DIR = MEMORY_DIR / "raw"
        AUDIO_DIR = MEMORY_DIR / "audio"
        DERIVED_DIR = MEMORY_DIR / "derived"
        NIGHTLY_DIR = MEMORY_DIR / "nightly"
    run(args.date.replace("-", ""), use_llm=not args.no_llm)


if __name__ == "__main__":
    main()
