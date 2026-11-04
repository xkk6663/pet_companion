"""设备串口日志 → 结构化事件。

只做**读取与断言**，不改固件：把 `docs/项目规划/实机验收清单_2026-10-31.md`
里人眼看的那些行（`AI_ACT: caps -> …`、`AI_ACT: accept …`、`AI_ACT: result …`、
`EVT: rx <- …`、`AI_ACT: stack left …`）变成可判据的字段。

行格式照 ESP-IDF：`<级别> (<tick>) <TAG>: <消息>`。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Iterable

# 任意 ESP-IDF 日志行：级别 + tick + tag + 消息
LINE_RE = re.compile(r"^([IWEDV]) \(\d+\) ([^:]+): (.+)$")

_LEVELS = {"I": "info", "W": "warn", "E": "error", "D": "debug", "V": "verbose"}

# AI_ACT（ai_action.c）
CAPS_RE = re.compile(
    r"caps -> (\S+) \((\d+) B, (\d+) tools(?:, (\d+) from mcp engine)?\)")
ACCEPT_RE = re.compile(r"accept '([^']+)' id=(\d+) args=(.*)$")
RESULT_RE = re.compile(r"result (\S+) id=(\d+) ok=([01]) -> (\S+) \((.*)\)$")
STACK_RE = re.compile(r"stack left (\d+) B after '([^']+)'")

# EVT（event_bus.c）
RX_RE = re.compile(r"rx <- (\S+)")
ACTION_RE = re.compile(r"action '([^']+)' -> (\S+)")

# 崩溃/看门狗标记（出现任意一条即判失败）
CRASH_MARKERS = (
    "Guru Meditation Error",
    "abort() was called",
    "assert failed",
    "task_wdt",
    "Stack canary watchpoint",
    "Cache error",
    "Core  0 panic'ed",
    "Core  1 panic'ed",
)


@dataclass
class Event:
    ev: str
    line: str
    tag: str = ""
    level: str = ""
    data: dict = field(default_factory=dict)

    def __getitem__(self, key: str):
        return self.data[key]


def parse_line(line: str) -> Event | None:
    """单行 → Event；无关行返回 None（不抛异常，日志里什么都可能有）。"""
    line = line.rstrip("\r\n")
    if not line:
        return None

    for marker in CRASH_MARKERS:
        if marker in line:
            return Event("crash", line, data={"marker": marker})

    m = LINE_RE.match(line)
    if not m:
        return None
    level, tag, msg = m.group(1), m.group(2), m.group(3)
    base = {"tag": tag, "level": _LEVELS[level]}

    if tag == "AI_ACT":
        mm = CAPS_RE.search(msg)
        if mm:
            return Event("caps", line, **base, data={
                "status": mm.group(1),
                "bytes": int(mm.group(2)),
                "tools": int(mm.group(3)),
                "mcp_tools": int(mm.group(4)) if mm.group(4) else None,
            })
        mm = ACCEPT_RE.search(msg)
        if mm:
            return Event("accept", line, **base, data={
                "name": mm.group(1), "id": int(mm.group(2)),
                "args": mm.group(3),
            })
        mm = RESULT_RE.search(msg)
        if mm:
            return Event("result", line, **base, data={
                "name": mm.group(1), "id": int(mm.group(2)),
                "ok": mm.group(3) == "1", "status": mm.group(4),
                "data": mm.group(5),
            })
        mm = STACK_RE.search(msg)
        if mm:
            return Event("stack_left", line, **base, data={
                "bytes": int(mm.group(1)), "action": mm.group(2),
            })
        return None

    if tag == "EVT":
        mm = RX_RE.search(msg)
        if mm:
            return Event("evt_rx", line, **base, data={"frame": mm.group(1)})
        mm = ACTION_RE.search(msg)
        if mm:
            return Event("evt_action", line, **base, data={
                "name": mm.group(1), "status": mm.group(2),
            })
        return None

    return None


def parse_lines(lines: Iterable[str]) -> list[Event]:
    out = []
    for ln in lines:
        ev = parse_line(ln)
        if ev is not None:
            out.append(ev)
    return out


def event_names(events: Iterable[Event]) -> list[str]:
    return [e.ev for e in events]


def find(events: Iterable[Event], name: str) -> list[Event]:
    return [e for e in events if e.ev == name]


def has_crash(events: Iterable[Event]) -> bool:
    return any(e.ev == "crash" for e in events)


def min_stack_left(events: Iterable[Event], default: int = 1 << 30) -> int:
    """所有 `stack left` 采样里的最小值；一条都没有时返回 default。"""
    vals = [e["bytes"] for e in events if e.ev == "stack_left"]
    return min(vals) if vals else default
