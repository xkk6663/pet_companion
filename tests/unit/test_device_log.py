"""串口日志事件化：用真机验收清单里的真实行格式做回归。"""
from __future__ import annotations

from pathlib import Path

import pytest

from support.device_log import (
    CRASH_MARKERS,
    event_names,
    find,
    has_crash,
    min_stack_left,
    parse_line,
    parse_lines,
)

SAMPLE = Path(__file__).resolve().parents[1] / "samples" / "pet_loop.log"


@pytest.fixture(scope="module")
def events():
    return parse_lines(SAMPLE.read_text(encoding="utf-8").splitlines())


def test_sample_counts(events):
    names = event_names(events)
    assert names.count("caps") == 2
    assert names.count("evt_rx") == 5
    assert names.count("evt_action") == 2
    assert names.count("accept") == 2
    assert names.count("result") == 2
    assert names.count("stack_left") == 2


def test_caps_fields(events):
    caps = find(events, "caps")[0]
    assert caps["status"] == "ESP_OK"
    assert caps["bytes"] == 8120
    assert caps["tools"] == 16
    assert caps["mcp_tools"] == 15


def test_caps_legacy_without_mcp_count():
    ev = parse_line("I (12) AI_ACT: caps -> ESP_OK (500 B, 4 tools)")
    assert ev is not None and ev.ev == "caps"
    assert ev["tools"] == 4
    assert ev["mcp_tools"] is None


def test_result_success_and_data(events):
    res = find(events, "result")[0]
    assert res["name"] == "punch"
    assert res["id"] == 1
    assert res["ok"] is True
    assert res["status"] == "ESP_OK"
    assert res["data"] == "「张三」打卡成功，照片已存"


def test_accept_args_are_kept_verbatim(events):
    acc = find(events, "accept")[1]
    assert acc["name"] == "music"
    assert acc["id"] == 2
    assert acc["args"] == '{"__text":"放首晴天","name":"晴天"}'


def test_stack_floor(events):
    assert min_stack_left(events) == 5048
    assert min_stack_left(events) >= 1500      # 验收清单：低于 1500 要抬栈


def test_action_accept_before_result_and_ids_match(events):
    for ev in find(events, "accept"):
        same = [r for r in find(events, "result")
                if r["id"] == ev["id"] and r["name"] == ev["name"]]
        assert len(same) == 1
        assert events.index(ev) < events.index(same[0])


def test_no_crash_in_golden_sample(events):
    assert not has_crash(events)


@pytest.mark.parametrize("marker", CRASH_MARKERS)
def test_crash_marker_detected(marker):
    line = f"E (999) esp_system: {marker}"
    ev = parse_line(line)
    assert ev is not None and ev.ev == "crash"
    assert ev["marker"] == marker


def test_evt_frames(events):
    frames = [e["frame"] for e in find(events, "evt_rx")]
    assert frames == ["system", "emotion", "text", "emotion", "error"]


def test_irrelevant_lines_ignored(events):
    for line in ("I (1024) boot: app_main start",
                 "I (1310) WIFI: sta got ip 192.168.10.23",
                 "",
                 "   ",
                 "garbage without format"):
        assert parse_line(line) is None


def test_crlf_and_whitespace_tolerated():
    ev = parse_line("I (1) AI_ACT: stack left 4096 B after 'capture'\r\n")
    assert ev is not None and ev["bytes"] == 4096


def test_deterministic(events):
    again = parse_lines(SAMPLE.read_text(encoding="utf-8").splitlines())
    assert event_names(again) == event_names(events)
