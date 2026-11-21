# -*- coding: utf-8 -*-
"""astrbot_plugin_pet_memory 自测（不依赖真实 AstrBot / 云端）

做法：
  · 如果环境里没有 astrbot，就注入最小桩模块，只验证本插件的逻辑；
  · 起一个本地假网关，模拟 /health、/memory/query、/memory/resources/read；
  · 覆盖：查询、日报渲染、工具注册/摘除、关闭态、每日推送。

运行：
  python selftest_plugin.py
"""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import sys
import tempfile
import types
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent


# ---------------------------------------------------------------- astrbot 桩
def _install_stubs():
    try:
        import astrbot  # noqa: F401
        return False
    except Exception:  # noqa: BLE001
        pass

    astrbot = types.ModuleType("astrbot")
    api = types.ModuleType("astrbot.api")

    class _Logger:
        def info(self, *a, **k): pass
        def warning(self, *a, **k): pass
        def error(self, *a, **k): pass

    api.logger = _Logger()

    event_mod = types.ModuleType("astrbot.api.event")

    class AstrMessageEvent:  # noqa: D101
        def __init__(self, text=""):
            self.message_str = text
            self.unified_msg_origin = "webchat:FriendMessage:test"

        def plain_result(self, text):
            return types.SimpleNamespace(text=text)

    class MessageChain:  # noqa: D101
        def __init__(self):
            self.items = []

        def message(self, text):
            self.items.append(text)
            return self

    class _Filter:
        @staticmethod
        def command(_name):
            def deco(fn):
                return fn
            return deco

        @staticmethod
        def on_llm_request():
            def deco(fn):
                return fn
            return deco

    event_mod.AstrMessageEvent = AstrMessageEvent
    event_mod.MessageChain = MessageChain
    event_mod.filter = _Filter()

    star_mod = types.ModuleType("astrbot.api.star")

    class Context:  # noqa: D101
        pass

    class Star:  # noqa: D101
        def __init__(self, context):
            self.context = context

    class StarTools:  # noqa: D101
        @staticmethod
        def get_data_dir(_plugin_name=None):
            return Path(tempfile.gettempdir()) / "pet_memory_selftest_data"

    star_mod.Context = Context
    star_mod.Star = Star
    star_mod.StarTools = StarTools

    api.event = event_mod
    api.star = star_mod
    astrbot.api = api
    sys.modules.update({
        "astrbot": astrbot,
        "astrbot.api": api,
        "astrbot.api.event": event_mod,
        "astrbot.api.star": star_mod,
    })
    return True


# ---------------------------------------------------------------- 假网关
REPORT = {
    "date": "20261114",
    "stats": {"events": 2, "speech": 1, "speech_ms": 7000, "speech_min": 0.12,
              "first_ts": "2026-11-14T20:31:00+08:00",
              "last_ts": "2026-11-14T22:10:00+08:00"},
    "people": [{"name": "张总", "evidence": ["张总说周五看方案"], "built_from": ["evt_a"]}],
    "commitments": [{"id": "cm_20261114_000", "text": "周五给张总发方案",
                     "person": "张总", "due": "2026-11-20", "status": "open",
                     "built_from": ["evt_a"]}],
    "topics": ["方案", "周会"],
    "source": "llm",
    "backlog_fixed": 0,
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # noqa: D401
        pass

    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send(200, {"ok": True, "version": "0.8.1", "sessions": {}})
        else:
            self._send(404, {"ok": False})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        payload = json.loads(self.rfile.read(n).decode() or "{}")
        if self.path == "/memory/query":
            qtype = payload.get("type") or "day"
            text = "张总说周五看方案" if qtype != "search" else "方案已经发过去了"
            self._send(200, {
                "ok": True, "type": qtype,
                "range": [payload.get("date_from"), payload.get("date_to")],
                "stats": {"events": 1, "speech_ms": 7000},
                "items": [{"id": "evt_a", "ts": "2026-11-14T20:31:00+08:00",
                           "kind": "speech", "text": text, "built_from": ["evt_a"]}],
            })
        elif self.path == "/memory/resources/read":
            uri = payload.get("uri")
            if uri == "memory://day":
                self._send(200, {"ok": True, "contents": [{
                    "uri": uri, "text": json.dumps(REPORT, ensure_ascii=False)}]})
            elif uri == "memory://commitments":
                self._send(200, {"ok": True, "contents": [{
                    "uri": uri, "text": json.dumps(REPORT["commitments"], ensure_ascii=False)}]})
            else:
                self._send(404, {"ok": False})
        else:
            self._send(404, {"ok": False})


# ---------------------------------------------------------------- 假上下文
class FakeConfig(dict):
    def save_config(self):
        self["_saved"] = True


class FakeTools:
    def __init__(self):
        self.added, self.removed = [], []

    def add_func(self, name, args, desc, handler):
        self.added.append((name, args, desc, handler))

    def remove_func(self, name):
        self.removed.append(name)


class FakeProvider:
    def meta(self):
        return types.SimpleNamespace(id="fake-provider")


class FakeContext:
    def __init__(self):
        self.provider_manager = types.SimpleNamespace(llm_tools=FakeTools())
        self.sent = []
        self.llm_calls = []

    async def send_message(self, umo, chain):
        self.sent.append((umo, chain))

    async def get_using_provider_async(self, umo=None):
        return FakeProvider()

    async def llm_generate(self, **kwargs):
        self.llm_calls.append(kwargs)
        return types.SimpleNamespace(completion_text=json.dumps({
            "judgment": "今天有 2 条记录，至少不是空白。",
            "suggestion": "明天先处理那条承诺。",
            "message": "今天记了 2 条，语音 0.1 分钟，还有 1 条承诺悬着。",
            "facts_used": ["stats.events", "stats.speech_min"],
        }, ensure_ascii=False))


# ---------------------------------------------------------------- 用例
def main():
    used_stub = _install_stubs()
    shutil.rmtree(Path(tempfile.gettempdir()) / "pet_memory_selftest_data",
                  ignore_errors=True)
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    import threading
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    sys.path.insert(0, str(HERE))
    import main as plugin_mod

    cfg = FakeConfig({
        "enabled": True,
        "gateway_base": f"http://127.0.0.1:{port}",
        "request_timeout": 5,
        "default_range_days": 7,
        "max_items": 20,
        "daily_push_enabled": True,
        "daily_push_time": datetime.now().strftime("%H:%M"),
        "daily_push_targets": ["webchat:FriendMessage:test"],
        "growth_enabled": True,
        "growth_time": datetime.now().strftime("%H:%M"),
        "growth_push_enabled": True,
        "growth_push_targets": ["webchat:FriendMessage:test"],
    })
    ctx = FakeContext()
    p = plugin_mod.PetMemory(ctx, cfg)

    assert p._tool_on, "工具未注册"
    assert ctx.provider_manager.llm_tools.added[0][0] == "pet_memory_query"
    assert len(ctx.provider_manager.llm_tools.added[0][1]) == 6

    # 1) 网关
    health = asyncio.run(p._get("/health"))
    assert health["ok"] and health["version"] == "0.8.1"

    # 2) 日报渲染
    report = asyncio.run(p._report("20261114"))
    text = p._report_text(report)
    assert "11-14 记忆日报" in text
    assert "事件 2 条" in text and "0.1 分钟" in text
    assert "张总" in text and "周五给张总发方案" in text
    assert "方案" in text and "llm" in text

    # 3) 事件 / 承诺渲染
    data = asyncio.run(p._query("search", "20261108", "20261114", q="方案"))
    ev_text = p._events_text(data, "搜索「方案」")
    assert "匹配 1 条" in ev_text and "方案已经发过去了" in ev_text
    commits = asyncio.run(p._resource(
        "memory://commitments", {"date_from": "20261108", "date_to": "20261114"}))
    cm_text = p._commitments_text(commits, 7)
    assert "1 条" in cm_text and "due 2026-11-20" in cm_text

    # 4) 工具返回
    raw = asyncio.run(p._tool_query(plugin_mod.AstrMessageEvent(), type="search", q="方案"))
    out = json.loads(raw)
    assert out["ok"] and out["items"][0]["built_from"] == ["evt_a"]
    # AstrBot 有的版本把工具参数作为第二个位置参数传入；这里锁住兼容性
    raw2 = asyncio.run(p._tool_query(plugin_mod.AstrMessageEvent(), {"type": "search", "q": "方案"}))
    assert json.loads(raw2)["ok"], "工具位置参数兼容性回归"

    # 5) 每日推送（时间已对齐当前分钟）
    ctx.sent.clear()
    asyncio.run(p._push_once())
    assert ctx.sent and "11-14 记忆日报" in ctx.sent[0][1].items[0]

    # 5.5) 成长摘要、人格注入、成长推送
    digest = asyncio.run(p._generate_digest("20261114", force=True))
    assert digest and digest["ok"] and not digest["empty"]
    assert digest["facts"]["stats"]["events"] == 2
    assert digest["facts"]["people"][0]["name"] == "张总"
    assert digest["open_commitments"][0]["text"] == "周五给张总发方案"
    assert digest["message"] and "承诺" in digest["message"]
    assert (p.growth_dir / "20261114.json").is_file()

    block = p._growth_prompt_block()
    assert "成长记忆" in block and "张总" in block and "未兑现承诺" in block
    req = types.SimpleNamespace(system_prompt="原有系统提示")
    asyncio.run(p.inject_growth_context(plugin_mod.AstrMessageEvent(), req))
    assert "成长记忆" in req.system_prompt and "原有系统提示" in req.system_prompt

    ctx.sent.clear()
    p._last_growth_push_day = ""
    p._growth_retry_after = 0
    asyncio.run(p._growth_push_once())
    assert ctx.sent and "今天记了 2 条" in ctx.sent[0][1].items[0]

    async def _run_growth_test():
        async for _ in p.cmd_growth_test(plugin_mod.AstrMessageEvent()):
            pass

    ctx.sent.clear()
    asyncio.run(_run_growth_test())
    assert ctx.sent and "推送链路测试" in ctx.sent[0][1].items[0]

    # 6) 关闭态
    p2 = plugin_mod.PetMemory(FakeContext(), FakeConfig({
        "enabled": False, "gateway_base": f"http://127.0.0.1:{port}"}))
    assert not p2._tool_on
    assert not p2.context.provider_manager.llm_tools.added

    # 7) terminate 摘工具 + 取消任务
    asyncio.run(p.terminate())
    assert "pet_memory_query" in ctx.provider_manager.llm_tools.removed

    # 8) 日期归一
    assert plugin_mod._norm_day("2026-11-14") == "20261114"
    assert plugin_mod._norm_day("bad", datetime(2026, 11, 14)) == "20261114"

    srv.shutdown()
    print(f"[selftest] PASS  (astrbot stub={'yes' if used_stub else 'no'})")


if __name__ == "__main__":
    main()
