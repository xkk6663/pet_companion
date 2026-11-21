# -*- coding: utf-8 -*-
"""每日转写与记忆（astrbot_plugin_pet_memory）

把 pet_companion 云端已经跑通的记忆链路收成一个可启停的 AstrBot 插件：

    设备/VAD 切片 ──> 网关 /slice ──> raw/<day>/events.jsonl
                                        │
                     夜间 nightly.py ──> nightly/<day>.report.json
                                        │
    本插件 ──> /memory/query、/memory/resources/read ──> 大肥鱼/指令

设计边界（与 S10 D5 一致）：
  · 模型侧只注册**一条**工具 `pet_memory_query`，加记忆维度只加 resource，
    工具表不变；
  · 插件只读云端记忆，不直接改 raw；日报由夜间轨产出；
  · 关插件必须真的关能力：动态注册的工具在 terminate() 里 remove_func()，
    否则 FuncTool 默认 active=True，会绕过插件启用检查。

配置见 _conf_schema.json；网关地址默认走容器到宿主机的 bridge。
"""

from __future__ import annotations

import asyncio
import json
import re
import sqlite3
import time
import urllib.error
import urllib.request
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Any

from astrbot.api import logger
from astrbot.api.event import AstrMessageEvent, MessageChain, filter
from astrbot.api.star import Context, Star, StarTools

TOOL_NAME = "pet_memory_query"

TOOL_DESC = (
    "查询桌宠每日转写形成的个人记忆：事件流、人物、承诺、日报。"
    "只在用户问“昨天/今天/上周发生了什么”“某人说过什么”“我答应过谁什么”"
    "“有没有日报”时调用；不要用它回答设备在线状态。"
    "返回结构化 JSON，每条带 built_from（可回溯到原始事件 id）。"
)

# 与固件 caps 的 params 同构：type/name/description
TOOL_ARGS = [
    {"type": "string", "name": "type",
     "description": "查询维度：day（默认，按天取事件）、person（按人物）、"
                    "commitments（承诺库）、search（关键词）"},
    {"type": "string", "name": "person",
     "description": "人物称呼，type=person 时使用"},
    {"type": "string", "name": "q",
     "description": "关键词，type=search 时使用"},
    {"type": "string", "name": "date_from",
     "description": "起始日期 YYYY-MM-DD；不填默认今天"},
    {"type": "string", "name": "date_to",
     "description": "结束日期 YYYY-MM-DD；不填等同 date_from"},
    {"type": "number", "name": "limit",
     "description": "最多返回条数，默认 20，最大 50"},
]

_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

GROWTH_SYSTEM_FALLBACK = (
    "你是用户定稿的桌宠伴侣「大肥鱼」：说话短、有点傲娇但关心人，"
    "不写小作文，不编造事实。"
)
_JSON_RE = re.compile(r"\{.*\}", re.S)
_NUM_RE = re.compile(r"\d+(?:\.\d+)?")


def _today() -> date:
    return date.today()


def _day_str(d: date) -> str:
    return d.strftime("%Y%m%d")


def _norm_day(value: str, default: date | None = None) -> str:
    """把 2026-11-14 / 20261114 归一成 YYYYMMDD；非法输入退回默认日。"""
    s = (value or "").strip().replace("-", "").replace("/", "")
    if len(s) == 8 and s.isdigit():
        try:
            datetime.strptime(s, "%Y%m%d")
            return s
        except ValueError:
            pass
    return _day_str(default or _today())


def _pretty_day(day: str) -> str:
    s = (day or "").replace("-", "")
    if len(s) == 8 and s.isdigit():
        return f"{s[4:6]}-{s[6:8]}"
    return day or "?"


def _pretty_ts(ts: str) -> str:
    s = (ts or "").strip()
    if "T" in s:
        return s.split("T", 1)[1][:5]
    if len(s) >= 16:
        return s[11:16]
    return s


def _clip(text: str, limit: int = 120) -> str:
    text = (text or "").strip().replace("\n", " ")
    return text if len(text) <= limit else text[:limit] + "…"


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _iso_now() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


class PetMemory(Star):
    def __init__(self, context: Context, config: dict | None = None):
        super().__init__(context)
        self.config = config if config is not None else {}
        self.enabled = bool(self.config.get("enabled", True))
        self.gateway = str(self.config.get("gateway_base") or "http://172.18.0.1:8081").rstrip("/")
        self.timeout = float(self.config.get("request_timeout") or 15)
        self.default_range_days = max(1, int(self.config.get("default_range_days") or 7))
        self.max_items = max(1, min(50, int(self.config.get("max_items") or 20)))

        # 桌面复盘模块：独立开关，和查询模块共用同一个插件，避免两个插件状态不一致。
        self.growth_enabled = bool(self.config.get("growth_enabled", True))
        self.growth_time = str(self.config.get("growth_time") or "21:30").strip()
        self.growth_push_enabled = bool(self.config.get("growth_push_enabled", True))
        self.growth_targets = list(self.config.get("growth_push_targets") or [])
        self.growth_persona_id = str(self.config.get("growth_persona_id") or "").strip()
        self.growth_lookback_days = max(1, min(31, int(self.config.get("growth_lookback_days") or 7)))
        self.growth_max_chars = max(200, min(3000, int(self.config.get("growth_max_chars") or 1200)))

        self._tool_on = False
        self._stop = asyncio.Event()
        self._push_task = None
        self._growth_task = None
        self._last_push_day = ""
        self._last_growth_push_day = ""
        self._growth_retry_after = 0.0
        self._growth_cache = ""
        self._growth_cache_at = 0.0
        self._persona_cache: str | None = None
        self._last_growth_error = ""

        try:
            self.growth_dir = StarTools.get_data_dir("astrbot_plugin_pet_memory") / "growth"
        except Exception:  # noqa: BLE001
            self.growth_dir = Path(__file__).resolve().parent / "growth"
        try:
            self.growth_dir.mkdir(parents=True, exist_ok=True)
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 创建桌面复盘目录失败: {e}")

        if self.enabled:
            self._register_tool()
        else:
            logger.info("[pet_memory] 配置为关闭：不注册 pet_memory_query")

        self._refresh_growth_cache()

        try:
            self._push_task = asyncio.get_running_loop().create_task(self._push_loop())
            logger.info("[pet_memory] 每日推送检查任务已启动（时间 %s）",
                        self.config.get("daily_push_time") or "08:30")
        except RuntimeError:
            logger.warning("[pet_memory] 无运行中的事件循环，每日推送未启动")

        try:
            self._growth_task = asyncio.get_running_loop().create_task(self._growth_loop())
            logger.info("[pet_memory] 桌面复盘检查任务已启动（时间 %s，开关 %s）",
                        self.growth_time, "开" if self.growth_enabled else "关")
        except RuntimeError:
            logger.warning("[pet_memory] 无运行中的事件循环，桌面复盘未启动")

    # ---------------------------------------------------------------- 生命周期
    def _register_tool(self) -> None:
        try:
            mgr = self.context.provider_manager.llm_tools
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 拿不到 llm_tools: {e}")
            return
        try:
            mgr.remove_func(TOOL_NAME)      # 重载时先清旧的，避免重复 handler
        except Exception:  # noqa: BLE001
            pass
        try:
            mgr.add_func(TOOL_NAME, TOOL_ARGS, TOOL_DESC, self._tool_query)
            self._tool_on = True
            logger.info(f"[pet_memory] 工具已注册：{TOOL_NAME}")
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 注册 {TOOL_NAME} 失败: {e}")

    def _unregister_tool(self) -> None:
        if not self._tool_on:
            return
        try:
            self.context.provider_manager.llm_tools.remove_func(TOOL_NAME)
            logger.info(f"[pet_memory] 工具已摘除：{TOOL_NAME}")
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 摘除 {TOOL_NAME} 失败: {e}")
        self._tool_on = False

    def _tool_visible(self) -> bool | None:
        """工具是否真的在 llm_tools 的 func_list 里；拿不到管理器返回 None。"""
        try:
            funcs = getattr(self.context.provider_manager.llm_tools, "func_list", None)
            if funcs is None:
                return None
            for f in funcs:
                name = getattr(f, "name", None)
                if name is None and isinstance(f, dict):
                    name = f.get("name")
                if name == TOOL_NAME:
                    return True
            return False
        except Exception:  # noqa: BLE001
            return None

    @filter.on_llm_request()
    async def inject_growth_context(self, event: AstrMessageEvent, req=None):
        """在人格请求前注入最近桌面复盘；只读，不改 memory 库。"""
        if not self.growth_enabled:
            return
        block = self._growth_prompt_block()
        if not block:
            return
        try:
            if isinstance(req, dict):
                base = str(req.get("system_prompt") or "")
                if block not in base:
                    req["system_prompt"] = (base + "\n\n" + block).strip()
                    logger.info(f"[pet_memory] 已注入桌面复盘上下文（{len(block)} 字）")
                return
            base = str(getattr(req, "system_prompt", "") or "")
            if block not in base:
                setattr(req, "system_prompt", (base + "\n\n" + block).strip())
                logger.info(f"[pet_memory] 已注入桌面复盘上下文（{len(block)} 字）")
        except Exception as e:  # noqa: BLE001
            logger.warning(f"[pet_memory] 注入桌面复盘上下文失败: {e}")

    async def terminate(self):
        """插件被关闭/卸载时调用：停后台任务，并真的把工具摘掉。"""
        self._stop.set()
        for attr in ("_push_task", "_growth_task"):
            task = getattr(self, attr, None)
            if task is not None:
                task.cancel()
                try:
                    await task
                except (asyncio.CancelledError, Exception):  # noqa: BLE001
                    pass
                setattr(self, attr, None)
        self._unregister_tool()
        logger.info("[pet_memory] 已停用")

    # ---------------------------------------------------------------- HTTP
    def _request(self, method: str, path: str, payload: dict | None = None,
                 timeout: float | None = None) -> Any:
        url = self.gateway + path
        body = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            url, data=body, method=method,
            headers={"Content-Type": "application/json"} if body else {},
        )
        with _OPENER.open(req, timeout=timeout or self.timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    async def _get(self, path: str, timeout: float | None = None) -> Any:
        return await asyncio.to_thread(self._request, "GET", path, None, timeout)

    async def _post(self, path: str, payload: dict, timeout: float | None = None) -> Any:
        return await asyncio.to_thread(self._request, "POST", path, payload, timeout)

    async def _query(self, qtype: str, date_from: str, date_to: str,
                     person: str = "", q: str = "", limit: int = 0) -> dict:
        payload = {
            "type": qtype,
            "date_from": date_from,
            "date_to": date_to,
            "limit": limit or self.max_items,
        }
        if person:
            payload["person"] = person
        if q:
            payload["q"] = q
        return await self._post("/memory/query", payload)

    async def _resource(self, uri: str, params: dict) -> Any:
        res = await self._post("/memory/resources/read", {"uri": uri, "params": params})
        for c in res.get("contents") or []:
            text = c.get("text")
            if text:
                return json.loads(text)
        return None

    async def _report(self, day: str) -> dict | None:
        """取某天日报；不存在返回 None（不抛异常）。"""
        try:
            return await self._resource("memory://day", {"date": day})
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise

    # ---------------------------------------------------------------- 成长记忆
    def _growth_path(self, day: str) -> Path:
        return self.growth_dir / f"{day}.json"

    def _load_digest(self, day: str) -> dict | None:
        path = self._growth_path(day)
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            logger.warning(f"[pet_memory] 读取桌面复盘 {day} 失败: {e}")
            return None

    def _save_digest(self, digest: dict) -> None:
        day = str(digest.get("date") or "").replace("-", "")
        if len(day) != 8 or not day.isdigit():
            return
        path = self._growth_path(day)
        tmp = path.with_suffix(".json.tmp")
        try:
            tmp.write_text(json.dumps(digest, ensure_ascii=False, indent=2),
                           encoding="utf-8")
            tmp.replace(path)
            self._refresh_growth_cache()
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 写桌面复盘 {day} 失败: {e}")

    def _latest_digests(self, limit: int = 3) -> list[dict]:
        try:
            files = [p for p in self.growth_dir.glob("*.json")
                     if p.stem.isdigit() and len(p.stem) == 8]
        except Exception:  # noqa: BLE001
            return []
        files.sort(key=lambda p: p.stem, reverse=True)
        out = []
        for p in files[:max(1, limit)]:
            try:
                out.append(json.loads(p.read_text(encoding="utf-8")))
            except Exception:  # noqa: BLE001
                continue
        return out

    def _refresh_growth_cache(self) -> None:
        """把最近三天摘要压成一段只读事实，供 LLM 请求注入。"""
        digests = self._latest_digests(3)
        if not digests:
            self._growth_cache = ""
            self._growth_cache_at = time.time()
            return
        lines = ["【桌面复盘记忆·最近三天（只读事实，不得据此编造）】"]
        for d in digests:
            facts = d.get("facts") or {}
            st = facts.get("stats") or {}
            day = _pretty_day(str(d.get("date") or facts.get("date") or ""))
            themes = [str(t.get("name") or "") for t in (facts.get("themes") or [])
                      if t.get("name")]
            commits = facts.get("commitments") or []
            open_commits = [c for c in commits
                            if str(c.get("status") or "open").lower()
                            not in ("done", "closed", "completed", "已兑现", "完成")]
            bits = [f"{day}：事件 {_safe_int(st.get('events'))} 条，"
                    f"语音 {_safe_float(st.get('speech_min')):.1f} 分钟"]
            headline = _clip(str(facts.get("headline") or d.get("headline") or ""), 60)
            if headline:
                bits.append("概括 " + headline)
            if themes:
                bits.append("主题 " + "、".join(themes[:4]))
            if open_commits:
                first = _clip(str(open_commits[0].get("text") or ""), 40)
                bits.append(f"未闭环 {len(open_commits)} 条（{first}）")
            next_action = _clip(str(d.get("next_action") or ""), 50)
            if next_action:
                bits.append("明天 " + next_action)
            judgment = _clip(str(d.get("judgment") or ""), 80)
            if judgment:
                bits.append("复盘 " + judgment)
            lines.append("- " + "；".join(bits))
        lines.append("以上只是背景；回答用户时不要把这些当成新发生的事，也不要补造细节。")
        block = "\n".join(lines)
        if len(block) > self.growth_max_chars:
            block = block[:self.growth_max_chars] + "\n…"
        self._growth_cache = block
        self._growth_cache_at = time.time()

    def _growth_prompt_block(self) -> str:
        if not self.growth_enabled:
            return ""
        if not self._growth_cache:
            self._refresh_growth_cache()
        return self._growth_cache

    def _persona_prompt(self) -> str:
        """读 AstrBot 的 persona 提示词；读不到时退回一条短兜底。

        只读 data_v4.db 的 personas 表，不写任何东西。默认取 system_prompt
        最长的一条作为当前主 persona；也可以在插件配置里指定 persona_id。
        """
        if self._persona_cache is not None:
            return self._persona_cache
        self._persona_id = self.growth_persona_id
        try:
            db = self.growth_dir.parents[2] / "data_v4.db"
            if not db.is_file():
                raise FileNotFoundError(str(db))
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=5)
            try:
                if self.growth_persona_id:
                    row = con.execute(
                        "select persona_id, system_prompt from personas "
                        "where persona_id = ? limit 1",
                        (self.growth_persona_id,),
                    ).fetchone()
                else:
                    row = con.execute(
                        "select persona_id, system_prompt from personas "
                        "order by length(system_prompt) desc limit 1"
                    ).fetchone()
            finally:
                con.close()
            if row and str(row[1] or "").strip():
                self._persona_id = str(row[0] or "")
                self._persona_cache = str(row[1]).strip()
                logger.info(f"[pet_memory] 使用 persona={self._persona_id or '?'} "
                            f"生成桌面复盘（{len(self._persona_cache)} 字）")
                return self._persona_cache
        except Exception as e:  # noqa: BLE001
            logger.warning(f"[pet_memory] 读取 persona 失败，使用兜底: {e}")
        self._persona_cache = GROWTH_SYSTEM_FALLBACK
        return self._persona_cache

    def _collect_facts(self, report: dict) -> dict:
        day = _norm_day(str(report.get("date") or ""), _today())
        st = report.get("stats") or {}
        people = []
        for p in report.get("people") or []:
            name = str(p.get("name") or "").strip()
            if not name:
                continue
            people.append({
                "name": name,
                "evidence": list(p.get("evidence") or [])[:3],
                "built_from": list(p.get("built_from") or []),
            })
        commits = []
        for c in report.get("commitments") or []:
            text = str(c.get("text") or "").strip()
            if not text:
                continue
            commits.append({
                "id": str(c.get("id") or ""),
                "text": text,
                "person": str(c.get("person") or ""),
                "due": str(c.get("due") or ""),
                "status": str(c.get("status") or "open"),
                "built_from": list(c.get("built_from") or []),
            })
        review = {}
        for key in ("themes", "timeline", "decisions", "learning",
                    "open_loops", "next_actions"):
            review[key] = [dict(x) for x in (report.get(key) or [])
                           if isinstance(x, dict)][:8]
        return {
            "date": day,
            "schema_version": _safe_int(report.get("schema_version")),
            "empty": bool(report.get("empty")),
            "reason": str(report.get("reason") or ""),
            "headline": str(report.get("headline") or "").strip(),
            "themes": review["themes"],
            "timeline": review["timeline"],
            "decisions": review["decisions"],
            "learning": review["learning"],
            "open_loops": review["open_loops"],
            "next_actions": review["next_actions"],
            "stats": {
                "events": _safe_int(st.get("events")),
                "speech_ms": _safe_int(st.get("speech_ms")),
                "speech_min": _safe_float(st.get("speech_min")),
                "text_count": _safe_int(st.get("text_count")),
                "audio_count": _safe_int(st.get("audio_count")),
                "group_count": _safe_int(st.get("group_count")),
                "group_asr": _safe_int(st.get("group_asr")),
                "first_ts": str(st.get("first_ts") or ""),
                "last_ts": str(st.get("last_ts") or ""),
            },
            "people": people,
            "commitments": commits,
            "topics": [str(t) for t in (report.get("topics") or []) if str(t).strip()],
            "source": str(report.get("source") or "?"),
            "backlog_fixed": _safe_int(report.get("backlog_fixed")),
            "built_from": [f"nightly/{day}.report.json"],
        }

    async def _collect_trend(self, day: str) -> dict:
        end = datetime.strptime(day, "%Y%m%d").date()
        reports = 0
        events = 0
        speech_min = 0.0
        active_days = 0
        people_days: dict[str, int] = {}
        topics: dict[str, int] = {}
        built_from: list[str] = []
        for back in range(self.growth_lookback_days):
            d = _day_str(end - timedelta(days=back))
            try:
                report = await self._report(d)
            except Exception:  # noqa: BLE001
                report = None
            if not report:
                continue
            reports += 1
            built_from.append(f"nightly/{d}.report.json")
            st = report.get("stats") or {}
            ev = _safe_int(st.get("events"))
            events += ev
            if ev > 0:
                active_days += 1
            speech_min += _safe_float(st.get("speech_min"))
            for p in report.get("people") or []:
                name = str(p.get("name") or "").strip()
                if name:
                    people_days[name] = people_days.get(name, 0) + 1
            for t in report.get("topics") or []:
                text = str(t).strip()
                if text:
                    topics[text] = topics.get(text, 0) + 1
        return {
            "days": self.growth_lookback_days,
            "reports": reports,
            "events": events,
            "speech_min": round(speech_min, 1),
            "active_days": active_days,
            "people": [
                {"name": k, "days": v}
                for k, v in sorted(people_days.items(),
                                   key=lambda kv: (-kv[1], kv[0]))[:4]
            ],
            "topics": [
                k for k, _ in sorted(topics.items(),
                                     key=lambda kv: (-kv[1], kv[0]))[:6]
            ],
            "built_from": built_from,
        }

    async def _collect_open_commitments(self, day: str) -> list[dict]:
        end = datetime.strptime(day, "%Y%m%d").date()
        date_from = _day_str(end - timedelta(days=self.growth_lookback_days))
        try:
            rows = await self._resource(
                "memory://commitments",
                {"date_from": date_from, "date_to": day},
            )
        except Exception as e:  # noqa: BLE001
            logger.warning(f"[pet_memory] 读承诺库失败: {e}")
            return []
        out = []
        seen = set()
        for c in rows or []:
            status = str(c.get("status") or "open").strip().lower()
            if status in ("done", "closed", "completed", "已兑现", "完成"):
                continue
            key = str(c.get("id") or c.get("text") or "")
            if key in seen:
                continue
            seen.add(key)
            out.append({
                "id": str(c.get("id") or ""),
                "text": str(c.get("text") or "").strip(),
                "person": str(c.get("person") or "").strip(),
                "due": str(c.get("due") or "").strip(),
                "status": status or "open",
                "built_from": list(c.get("built_from") or []),
            })
        return out[:8]

    def _allowed_numbers(self, facts: dict, trend: dict, open_commits: list[dict]) -> set[str]:
        st = facts.get("stats") or {}
        allowed = {
            str(_safe_int(st.get("events"))),
            f"{_safe_float(st.get('speech_min')):.1f}",
            f"{_safe_float(st.get('speech_min')):g}",
            str(len(open_commits)),
            str(len(facts.get("commitments") or [])),
            str(len(facts.get("people") or [])),
            str(_safe_int(trend.get("active_days"))),
            str(_safe_int(trend.get("reports"))),
            str(_safe_int(trend.get("events"))),
            f"{_safe_float(trend.get('speech_min')):.1f}",
        }
        # 允许把 12.5 分钟说成“12 分钟”这种取整表达，但仍不接受事实外数字。
        for a in list(allowed):
            if "." in a:
                allowed.add(a.split(".", 1)[0])
        return {a for a in allowed if a not in ("", None)}

    def _fallback_reflection(self, facts: dict, trend: dict,
                             open_commits: list[dict]) -> dict:
        st = facts.get("stats") or {}
        events = _safe_int(st.get("events"))
        speech = _safe_float(st.get("speech_min"))
        themes = [str(t.get("name") or "") for t in facts.get("themes") or []
                  if t.get("name")]
        if events <= 0:
            headline = "今天桌面前几乎没有留下声音。"
            judgment = "今天没有记录到事件。"
            suggestion = "如果设备在，明天确认它是否在监听；如果不在，不用硬编。"
            message = "今天没什么可说的，明天记得把我放在桌上。"
            next_action = ""
        else:
            headline = f"今天桌面前留下 {events} 条记录，语音 {speech:.1f} 分钟。"
            if themes:
                headline += "主要围绕" + "、".join(themes[:3]) + "。"
            judgment = headline
            if open_commits:
                suggestion = f"还有 {len(open_commits)} 条没闭环，明天先挑最久的一条。"
                next_action = str(open_commits[0].get("text") or "")
            else:
                suggestion = "今天没有明显欠账，明天接着把主题往下推。"
                next_action = ""
            message = (f"今天桌面前记了 {events} 条，语音 {speech:.1f} 分钟。"
                       + ("还有 %d 条没闭环，明天别装忘。" % len(open_commits)
                          if open_commits else "今天没新欠账。"))
        return {
            "headline": headline,
            "review": headline + (" 明天：" + suggestion if suggestion else ""),
            "next_action": next_action,
            "judgment": judgment,
            "suggestion": suggestion,
            "message": _clip(message, 240),
            "facts_used": ["stats.events", "stats.speech_min", "themes"],
            "source": "rule",
        }

    async def _llm_reflection(self, facts: dict, trend: dict,
                              open_commits: list[dict]) -> dict:
        payload = {
            "today": facts,
            "recent": trend,
            "open_commitments": open_commits,
        }
        prompt = (
            "下面是你今天和最近几天的结构化事实（不是原始转写）。"
            "请只根据这些事实，输出一个 JSON 对象，不要输出 JSON 以外的字：\n"
            '{"headline":"一句话概括今天桌面前发生了什么",'
            '"review":"3-5 句今日桌面复盘，按时间线/主题/结论/学习点归纳",'
            '"next_action":"明天第一件可以接着做的事，没有就空串",'
            '"judgment":"你对今天的判断，1-2 句",'
            '"suggestion":"对用户的一句建议",'
            '"message":"准备发到微信的短话，必须用大肥鱼口吻",'
            '"facts_used":["你实际引用的事实字段"]}\n'
            "规则：\n"
            "1. 不得新增事实、人物、承诺、数字；没出现的人不要提。\n"
            "2. 数字只能来自 facts 和 recent，不要自己算新数字。\n"
            "3. review 不超过 300 字，message 控制在 80 字以内，不写小作文。\n"
            "4. 如果今天事实很少，就说少，不要编故事。\n"
            "5. 这是桌面复盘，不是监督训话；语气是大肥鱼，但内容要具体、可回看。\n"
            "事实：\n" + json.dumps(payload, ensure_ascii=False)
        )
        umo = (self.growth_targets[0] if self.growth_targets else "") or ""
        try:
            provider = await self.context.get_using_provider_async(umo)
        except Exception as e:  # noqa: BLE001
            logger.warning(f"[pet_memory] 取 provider 失败: {e}")
            provider = None
        if provider is None:
            raise RuntimeError("没有可用的文本 provider")
        meta = getattr(provider, "meta", None)
        provider_id = str(meta().id) if callable(meta) else ""
        if not provider_id:
            raise RuntimeError("provider 没有 id")
        resp = await asyncio.wait_for(
            self.context.llm_generate(
                chat_provider_id=provider_id,
                prompt=prompt,
                system_prompt=(
                    self._persona_prompt() + "\n\n"
                    "你正在写睡前桌面复盘。只读事实，不编造；"
                    "先概括今天桌面前发生了什么，再归纳主题、结论和学习点，"
                    "最后给一句明天可以接着做的事。不要监督训话。"
                ),
                max_tokens=600,
                temperature=0.6,
            ),
            timeout=75,
        )
        raw = str(getattr(resp, "completion_text", "") or "").strip()
        m = _JSON_RE.search(raw)
        if not m:
            raise ValueError("LLM 没有返回 JSON")
        data = json.loads(m.group(0))
        if not isinstance(data, dict):
            raise ValueError("LLM JSON 不是对象")
        judgment = _clip(str(data.get("judgment") or ""), 240)
        suggestion = _clip(str(data.get("suggestion") or ""), 200)
        message = _clip(str(data.get("message") or ""), 240)
        headline = _clip(str(data.get("headline") or ""), 120)
        review = _clip(str(data.get("review") or ""), 300)
        next_action = _clip(str(data.get("next_action") or ""), 120)
        if not judgment or not message:
            raise ValueError("LLM 缺少 judgment/message")
        if not review:
            review = "；".join(x for x in (headline, judgment, suggestion) if x)
        allowed = self._allowed_numbers(facts, trend, open_commits)
        bad = [n for n in _NUM_RE.findall(message)
               if n not in allowed and n.rstrip("0").rstrip(".") not in allowed]
        if bad:
            raise ValueError(f"微信话出现事实外数字: {bad}")
        return {
            "headline": headline or judgment,
            "review": review,
            "next_action": next_action,
            "judgment": judgment,
            "suggestion": suggestion,
            "message": message,
            "facts_used": [str(x) for x in (data.get("facts_used") or [])][:8],
            "source": "llm",
        }

    async def _generate_digest(self, day: str, force: bool = False) -> dict | None:
        day = _norm_day(day, _today())
        old = self._load_digest(day)
        if old and not force:
            return old
        try:
            report = await self._report(day)
        except Exception as e:  # noqa: BLE001
            self._last_growth_error = f"读日报失败：{e}"
            logger.error(f"[pet_memory] 读日报失败 {day}: {e}")
            return None
        if not report:
            self._last_growth_error = f"{day} 没有日报"
            logger.info(f"[pet_memory] {day} 没有日报，跳过桌面复盘")
            return None

        facts = self._collect_facts(report)
        trend = await self._collect_trend(day)
        open_commits = await self._collect_open_commitments(day)
        fstats = facts.get("stats") or {}
        # 空数据、无文本、或夜间已标记 empty：不再让 LLM 生成复盘，也不推送。
        empty = (bool(facts.get("empty"))
                 or _safe_int(fstats.get("events")) <= 0
                 or (_safe_int(fstats.get("text_count")) <= 0
                     and _safe_float(fstats.get("speech_min")) <= 0))
        if empty:
            reflection = {
                "headline": "今天没有记录到事件。",
                "review": "今天桌面前没有留下可复盘的语音。",
                "next_action": "",
                "judgment": "今天没有记录到事件。",
                "suggestion": "如果设备在，明天确认它是否在监听；如果不在，就不用硬编。",
                "message": "",
                "facts_used": ["stats.events"],
                "source": "empty",
            }
        else:
            try:
                reflection = await self._llm_reflection(facts, trend, open_commits)
            except Exception as e:  # noqa: BLE001
                self._last_growth_error = f"LLM 生成失败，已退化为规则：{e}"
                logger.warning(f"[pet_memory] 桌面复盘 LLM 失败，退化规则: {e}")
                reflection = self._fallback_reflection(facts, trend, open_commits)

        old_push = (old or {}).get("push") or {}
        digest = {
            "ok": True,
            "empty": empty,
            "date": day,
            "generated_at": _iso_now(),
            "source": "pet_growth",
            "built_from": [f"nightly/{day}.report.json"] + list(trend.get("built_from") or []),
            "facts": facts,
            "trend": trend,
            "open_commitments": open_commits,
            "headline": reflection.get("headline") or "",
            "review": reflection.get("review") or "",
            "next_action": reflection.get("next_action") or "",
            "judgment": reflection.get("judgment") or "",
            "suggestion": reflection.get("suggestion") or "",
            "message": reflection.get("message") or "",
            "facts_used": reflection.get("facts_used") or [],
            "reflection_source": reflection.get("source") or "?",
            "persona_id": getattr(self, "_persona_id", "") or "",
            "push": {
                "enabled": self.growth_push_enabled,
                "sent_at": old_push.get("sent_at"),
                "targets": list(old_push.get("targets") or []),
            },
            "graph_write": {
                "enabled": bool(self.config.get("growth_graph_write_enabled", False)),
                "status": "reserved",
            },
        }
        self._save_digest(digest)
        logger.info(f"[pet_memory] 桌面复盘 {day} 已生成（{len(digest['message'])} 字，"
                    f"来源 {digest['reflection_source']}）")
        return digest

    def _digest_text(self, digest: dict) -> str:
        facts = digest.get("facts") or {}
        st = facts.get("stats") or {}
        day = _pretty_day(str(digest.get("date") or ""))
        lines = [f"{day} 桌面复盘（{digest.get('reflection_source') or '?'}）"]
        if digest.get("headline") or facts.get("headline"):
            lines.append(str(digest.get("headline") or facts.get("headline")))
        lines.append(f"事件 {_safe_int(st.get('events'))} 条 · "
                     f"语音 {_safe_float(st.get('speech_min')):.1f} 分钟")
        themes = [str(t.get("name") or "") for t in facts.get("themes") or []
                  if t.get("name")]
        if themes:
            lines.append("主题：" + "、".join(themes[:6]))
        timeline = facts.get("timeline") or []
        if timeline:
            lines.append("时间线：")
            for t in timeline[:5]:
                lines.append(f"- {t.get('period') or '?'}：{_clip(str(t.get('summary') or ''), 80)}")
        decisions = facts.get("decisions") or []
        if decisions:
            lines.append("结论：")
            for d in decisions[:4]:
                lines.append("- " + _clip(str(d.get("text") or ""), 80))
        learning = facts.get("learning") or []
        if learning:
            lines.append("学习点：")
            for item in learning[:4]:
                lines.append("- " + _clip(str(item.get("point") or ""), 80))
        open_loops = facts.get("open_loops") or []
        if open_loops:
            lines.append(f"未闭环：{len(open_loops)} 条")
            for item in open_loops[:3]:
                lines.append("- " + _clip(str(item.get("text") or ""), 80))
        if digest.get("next_action"):
            lines.append("明天：" + str(digest.get("next_action")))
        if digest.get("review"):
            lines.append("复盘：" + str(digest.get("review")))
        if digest.get("message"):
            lines.append("大肥鱼：" + str(digest.get("message")))
        lines.append("推送：" + ("已发" if (digest.get("push") or {}).get("sent_at") else "未发"))
        return "\n".join(lines)

    def _push_text(self, digest: dict) -> str:
        """微信只发一张短复盘卡，不发整份日报。"""
        facts = digest.get("facts") or {}
        st = facts.get("stats") or {}
        day = _pretty_day(str(digest.get("date") or ""))
        lines = [f"【{day} 桌面复盘】"]
        headline = str(digest.get("headline") or facts.get("headline") or "").strip()
        if headline:
            lines.append(headline)
        themes = [str(t.get("name") or "") for t in facts.get("themes") or []
                  if t.get("name")]
        if themes:
            lines.append("主题：" + "、".join(themes[:5]))
        timeline = facts.get("timeline") or []
        if timeline:
            lines.append("时间线：")
            for t in timeline[:3]:
                lines.append(f"- {t.get('period') or '?'}：{_clip(str(t.get('summary') or ''), 50)}")
        decisions = facts.get("decisions") or []
        if decisions:
            lines.append("结论：" + "；".join(
                _clip(str(d.get("text") or ""), 40) for d in decisions[:2]))
        learning = facts.get("learning") or []
        if learning:
            lines.append("学到：" + "；".join(
                _clip(str(item.get("point") or ""), 40) for item in learning[:2]))
        open_loops = facts.get("open_loops") or []
        if open_loops:
            lines.append("没闭环：" + "；".join(
                _clip(str(item.get("text") or ""), 40) for item in open_loops[:2]))
        if digest.get("next_action"):
            lines.append("明天：" + _clip(str(digest.get("next_action")), 60))
        if digest.get("message"):
            lines.append("大肥鱼：" + _clip(str(digest.get("message")), 80))
        lines.append(f"（{_safe_int(st.get('events'))} 条 · "
                     f"{_safe_float(st.get('speech_min')):.1f} 分钟）")
        return "\n".join(lines).strip()

    def _growth_due(self, now: datetime) -> bool:
        try:
            hh, mm = [int(x) for x in self.growth_time.split(":", 1)]
            target = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
        except Exception:  # noqa: BLE001
            return False
        delta = now - target
        return timedelta(0) <= delta <= timedelta(hours=4)

    async def _growth_loop(self):
        while not self._stop.is_set():
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=30)
                break
            except asyncio.TimeoutError:
                pass
            try:
                await self._growth_push_once()
            except Exception as e:  # noqa: BLE001
                self._last_growth_error = f"桌面复盘检查失败：{e}"
                logger.error(f"[pet_memory] 桌面复盘检查失败: {e}")

    async def _growth_push_once(self):
        if not self.growth_enabled or not self.growth_push_enabled:
            return
        if not self.growth_targets:
            return
        now = datetime.now()
        if not self._growth_due(now):
            return
        if time.time() < self._growth_retry_after:
            return
        day = now.strftime("%Y%m%d")
        if self._last_growth_push_day == day:
            return
        digest = await self._generate_digest(day, force=True)
        if not digest:
            self._growth_retry_after = time.time() + 300
            return
        if digest.get("empty"):
            self._last_growth_push_day = day
            logger.info(f"[pet_memory] {day} 桌面复盘为空，不推送")
            return
        if (digest.get("push") or {}).get("sent_at"):
            self._last_growth_push_day = day
            return
        text = self._push_text(digest).strip()
        if not text:
            self._last_growth_push_day = day
            logger.info(f"[pet_memory] {day} 没有可推送的桌面复盘内容")
            return
        sent = []
        for umo in list(self.growth_targets):
            try:
                await self.context.send_message(umo, MessageChain().message(text))
                sent.append(umo)
                logger.info(f"[pet_memory] 桌面复盘已推送到 {umo}")
            except Exception as e:  # noqa: BLE001
                logger.error(f"[pet_memory] 桌面复盘推送到 {umo} 失败: {e}")
        if sent:
            digest.setdefault("push", {})
            digest["push"]["sent_at"] = _iso_now()
            digest["push"]["targets"] = sent
            self._save_digest(digest)
        self._last_growth_push_day = day

    # ---------------------------------------------------------------- 渲染
    def _report_text(self, report: dict) -> str:
        st = report.get("stats") or {}
        day = _pretty_day(str(report.get("date") or ""))
        source = report.get("source") or "?"
        fixed = int(report.get("backlog_fixed") or 0)
        lines = [f"{day} 记忆日报（来源 {source}"
                 + (f"，补转写 {fixed} 条" if fixed else "") + "）"]

        span = ""
        if st.get("first_ts") or st.get("last_ts"):
            span = f"（{_pretty_ts(st.get('first_ts',''))}–{_pretty_ts(st.get('last_ts',''))}）"
        lines.append(f"事件 {int(st.get('events') or 0)} 条 · "
                     f"语音 {float(st.get('speech_min') or 0):.1f} 分钟{span}")

        people = report.get("people") or []
        if people:
            names = [str(p.get("name") or "").strip() for p in people if p.get("name")]
            shown = "、".join(names[:5])
            if len(names) > 5:
                shown += f" 等 {len(names)} 人"
            lines.append(f"人物：{shown}")
        else:
            lines.append("人物：未抽取到")

        commits = report.get("commitments") or []
        if commits:
            lines.append(f"承诺 {len(commits)} 条：")
            for c in commits[:8]:
                who = str(c.get("person") or "").strip()
                due = str(c.get("due") or "").strip()
                prefix = f"{who}：" if who else ""
                suffix = f"（due {due}）" if due else ""
                lines.append(f"- {prefix}{_clip(str(c.get('text') or ''))}{suffix}")
            if len(commits) > 8:
                lines.append(f"- 另有 {len(commits) - 8} 条，用 /记忆承诺 查全量")
        else:
            lines.append("承诺：无")

        topics = report.get("topics") or []
        if topics:
            lines.append("话题：" + "、".join(str(t) for t in topics[:8]))

        if not report.get("people") and not report.get("commitments") \
                and not report.get("topics") and not st.get("events"):
            lines.append("这天没有记录到事件。")
        return "\n".join(lines)

    def _events_text(self, data: dict, title: str) -> str:
        items = data.get("items") or []
        st = data.get("stats") or {}
        head = (f"{title}：匹配 {st.get('events', len(items))} 条 · "
                f"语音 {float(st.get('speech_ms') or 0) / 60000:.1f} 分钟")
        if not items:
            return head + "\n没有匹配的记录。"
        lines = [head]
        for it in items[-self.max_items:]:
            kind = "语音" if it.get("kind") == "speech" else str(it.get("kind") or "事件")
            lines.append(f"- {_pretty_ts(str(it.get('ts') or ''))} {kind}：{_clip(str(it.get('text') or '（空）'))}")
        return "\n".join(lines)

    def _commitments_text(self, commits: list[dict], days: int) -> str:
        head = f"最近 {days} 天的承诺：{len(commits)} 条"
        if not commits:
            return head + "\n没有抽取到承诺。"
        lines = [head]
        for c in commits[:30]:
            who = str(c.get("person") or "").strip() or "未定人"
            due = str(c.get("due") or "").strip()
            status = str(c.get("status") or "open")
            suffix = f"（due {due}，{status}）" if due else f"（{status}）"
            lines.append(f"- {who}：{_clip(str(c.get('text') or ''))}{suffix}")
        if len(commits) > 30:
            lines.append(f"- 另有 {len(commits) - 30} 条未显示")
        return "\n".join(lines)

    # ---------------------------------------------------------------- 指令
    @filter.command("记忆")
    async def cmd_status(self, event: AstrMessageEvent):
        """查看记忆插件状态、网关连通性和最近一份日报。"""
        visible = self._tool_visible()
        if self._tool_on and visible is True:
            tool_state = "已注册（管理器可见）"
        elif self._tool_on and visible is False:
            tool_state = "已注册（管理器不可见，需排查）"
        elif self._tool_on:
            tool_state = "已注册（管理器不可查）"
        else:
            tool_state = "未注册"
        lines = [f"记忆插件：{'已启用' if self.enabled else '已关闭'}"
                 f" · 工具 {tool_state}"]

        try:
            health = await self._get("/health", timeout=8)
            lines.append(f"网关：{self.gateway} 在线（v{health.get('version', '?')}，"
                         f"设备会话 {len(health.get('sessions') or {})} 个）")
        except Exception as e:  # noqa: BLE001
            lines.append(f"网关：不可达（{e}）")

        latest = None
        for back in range(0, 3):
            d = _day_str(_today() - timedelta(days=back))
            try:
                latest = await self._report(d)
            except Exception:  # noqa: BLE001
                latest = None
            if latest:
                break
        if latest:
            st = latest.get("stats") or {}
            lines.append(f"最近日报：{_pretty_day(str(latest.get('date') or ''))}"
                         f" · 事件 {int(st.get('events') or 0)} 条"
                         f" · 语音 {float(st.get('speech_min') or 0):.1f} 分钟"
                         f" · 来源 {latest.get('source') or '?'}")

        targets = self.config.get("daily_push_targets") or []
        push_on = bool(self.config.get("daily_push_enabled"))
        lines.append(f"每日推送：{'开' if push_on else '关'} · "
                     f"{self.config.get('daily_push_time') or '08:30'} · "
                     f"订阅 {len(targets)} 个会话")
        latest_digest = self._latest_digests(1)
        if latest_digest:
            d = latest_digest[0]
            lines.append(f"桌面复盘：{_pretty_day(str(d.get('date') or ''))}"
                         f" · {'已生成' if d.get('ok') else '未生成'}"
                         f" · 推送 {'已发' if (d.get('push') or {}).get('sent_at') else '未发'}")
        lines.append(f"桌面复盘模块：{'开' if self.growth_enabled else '关'} · "
                     f"{self.growth_time} · 微信订阅 {len(self.growth_targets)} 个会话")
        lines.append("指令：/记忆日报 [日期] · /记忆查 关键词 · /记忆人物 称呼 · "
                     "/记忆承诺 [天数] · /记忆订阅 · /记忆推送 08:30 · /记忆开关 on|off")
        lines.append("成长：/成长 · /桌面复盘 [日期] · /成长生成 [日期] · "
                     "/成长订阅 · /桌面复盘推送 on|off|HH:MM · /成长开关 on|off")
        yield event.plain_result("\n".join(lines))

    @filter.command("记忆日报")
    async def cmd_report(self, event: AstrMessageEvent, day: str = ""):
        """读某天的记忆日报，默认昨天。"""
        if not self.enabled:
            yield event.plain_result("记忆插件已关闭，先发 /记忆开关 on。")
            return
        if not day:
            args = self._args(event)
            day = args[0] if args else ""
        day = _norm_day(day, _today() - timedelta(days=1))
        try:
            report = await self._report(day)
        except Exception as e:  # noqa: BLE001
            yield event.plain_result(f"读日报失败：{e}")
            return
        if not report:
            yield event.plain_result(
                f"{_pretty_day(day)} 还没有日报。夜间任务可能没跑，"
                f"或这天没有事件。")
            return
        yield event.plain_result(self._report_text(report))

    @filter.command("记忆查")
    async def cmd_search(self, event: AstrMessageEvent, q: str = ""):
        """按关键词查最近几天的事件。"""
        if not self.enabled:
            yield event.plain_result("记忆插件已关闭，先发 /记忆开关 on。")
            return
        if not q:
            args = self._args(event)
            q = " ".join(args).strip()
        if not q:
            yield event.plain_result("用法：/记忆查 关键词（可多个词）")
            return
        date_to = _day_str(_today())
        date_from = _day_str(_today() - timedelta(days=self.default_range_days - 1))
        try:
            data = await self._query("search", date_from, date_to, q=q)
        except Exception as e:  # noqa: BLE001
            yield event.plain_result(f"查询失败：{e}")
            return
        yield event.plain_result(self._events_text(data, f"搜索「{q}」"))

    @filter.command("记忆人物")
    async def cmd_person(self, event: AstrMessageEvent, person: str = ""):
        """按人物称呼查最近几天说过的话。"""
        if not self.enabled:
            yield event.plain_result("记忆插件已关闭，先发 /记忆开关 on。")
            return
        if not person:
            args = self._args(event)
            person = " ".join(args).strip()
        if not person:
            yield event.plain_result("用法：/记忆人物 称呼（例如 /记忆人物 张总）")
            return
        date_to = _day_str(_today())
        date_from = _day_str(_today() - timedelta(days=self.default_range_days - 1))
        try:
            data = await self._query("person", date_from, date_to, person=person)
        except Exception as e:  # noqa: BLE001
            yield event.plain_result(f"查询失败：{e}")
            return
        yield event.plain_result(self._events_text(data, f"关于「{person}」"))

    @filter.command("记忆承诺")
    async def cmd_commitments(self, event: AstrMessageEvent, days: int = 0):
        """查最近 N 天抽取出的承诺，默认 7 天。"""
        if not self.enabled:
            yield event.plain_result("记忆插件已关闭，先发 /记忆开关 on。")
            return
        if not days:
            args = self._args(event)
            try:
                days = int(args[0]) if args else self.default_range_days
            except ValueError:
                yield event.plain_result("用法：/记忆承诺 [天数]，天数 1-31")
                return
        days = max(1, min(31, int(days)))
        date_to = _day_str(_today())
        date_from = _day_str(_today() - timedelta(days=days - 1))
        try:
            commits = await self._resource(
                "memory://commitments", {"date_from": date_from, "date_to": date_to})
        except Exception as e:  # noqa: BLE001
            yield event.plain_result(f"读承诺库失败：{e}")
            return
        yield event.plain_result(self._commitments_text(commits or [], days))

    @filter.command("记忆订阅")
    async def cmd_subscribe(self, event: AstrMessageEvent):
        """把当前会话加入每日日报推送。"""
        umo = str(getattr(event, "unified_msg_origin", "") or "").strip()
        if not umo:
            yield event.plain_result("拿不到当前会话标识，订阅失败。")
            return
        targets = list(self.config.get("daily_push_targets") or [])
        if umo in targets:
            yield event.plain_result(f"当前会话已订阅（共 {len(targets)} 个）。")
            return
        targets.append(umo)
        self.config["daily_push_targets"] = targets
        self._save()
        yield event.plain_result(
            f"已订阅每日日报（共 {len(targets)} 个会话）。"
            f"推送时间 {self.config.get('daily_push_time') or '08:30'}，"
            f"当前{'已开启' if self.config.get('daily_push_enabled') else '未开启'}；"
            f"发 /记忆推送 on 开启。")

    @filter.command("记忆退订")
    async def cmd_unsubscribe(self, event: AstrMessageEvent):
        """把当前会话从每日日报推送里移除。"""
        umo = str(getattr(event, "unified_msg_origin", "") or "").strip()
        targets = list(self.config.get("daily_push_targets") or [])
        if umo not in targets:
            yield event.plain_result("当前会话没有订阅。")
            return
        targets = [t for t in targets if t != umo]
        self.config["daily_push_targets"] = targets
        self._save()
        yield event.plain_result(f"已退订（剩 {len(targets)} 个会话）。")

    @filter.command("记忆推送")
    async def cmd_push(self, event: AstrMessageEvent, value: str = ""):
        """开关每日推送或改时间：/记忆推送 on|off|08:30。"""
        if not value:
            args = self._args(event)
            value = args[0] if args else ""
        if not value:
            on = bool(self.config.get("daily_push_enabled"))
            yield event.plain_result(
                f"每日推送：{'开' if on else '关'} · "
                f"{self.config.get('daily_push_time') or '08:30'} · "
                f"订阅 {len(self.config.get('daily_push_targets') or [])} 个会话\n"
                f"用法：/记忆推送 on|off|HH:MM")
            return
        value = value.strip().lower()
        if value in ("on", "开"):
            self.config["daily_push_enabled"] = True
        elif value in ("off", "关"):
            self.config["daily_push_enabled"] = False
        else:
            try:
                datetime.strptime(value, "%H:%M")
            except ValueError:
                yield event.plain_result("时间格式应为 HH:MM，例如 /记忆推送 08:30")
                return
            self.config["daily_push_time"] = value
            self.config["daily_push_enabled"] = True
        self._save()
        yield event.plain_result(
            f"每日推送：{'开' if self.config.get('daily_push_enabled') else '关'} · "
            f"{self.config.get('daily_push_time') or '08:30'}")

    @filter.command("成长")
    async def cmd_growth_status(self, event: AstrMessageEvent):
        """查看桌面复盘模块状态、最近摘要和推送订阅。"""
        latest = self._latest_digests(1)
        lines = [
            f"桌面复盘模块：{'已启用' if self.growth_enabled else '已关闭'}"
            f" · 每晚 {self.growth_time}",
            f"微信推送：{'开' if self.growth_push_enabled else '关'}"
            f" · 订阅 {len(self.growth_targets)} 个会话",
            f"数据目录：{self.growth_dir}",
        ]
        if latest:
            d = latest[0]
            lines.append(
                f"最近摘要：{_pretty_day(str(d.get('date') or ''))}"
                f" · 事件 {_safe_int((d.get('facts') or {}).get('stats', {}).get('events'))} 条"
                f" · {'已推' if (d.get('push') or {}).get('sent_at') else '未推'}"
                f" · 来源 {d.get('reflection_source') or '?'}"
            )
            if d.get("message"):
                lines.append("最近微信话：" + str(d.get("message")))
        else:
            lines.append("最近摘要：还没有（等晚间日报或发 /成长生成）")
        if self._last_growth_error:
            lines.append("最近错误：" + self._last_growth_error)
        lines.append("指令：/桌面复盘 [日期] · /成长生成 [日期] · /成长订阅 · "
                     "/桌面复盘推送 on|off|HH:MM · /成长开关 on|off · /成长测试")
        yield event.plain_result("\n".join(lines))

    @filter.command("桌面复盘")
    async def cmd_growth_digest(self, event: AstrMessageEvent, day: str = ""):
        """查看某天桌面复盘，默认最近一份。"""
        if not day:
            args = self._args(event)
            day = args[0] if args else ""
        if day:
            day = _norm_day(day, _today())
            digest = self._load_digest(day)
            if not digest:
                yield event.plain_result(f"{_pretty_day(day)} 还没有桌面复盘。")
                return
        else:
            latest = self._latest_digests(1)
            if not latest:
                yield event.plain_result("还没有桌面复盘。先等晚间日报，或发 /成长生成。")
                return
            digest = latest[0]
        yield event.plain_result(self._digest_text(digest))

    @filter.command("成长生成")
    async def cmd_growth_generate(self, event: AstrMessageEvent, day: str = ""):
        """手动生成某天摘要，默认今天；用于验收和补跑。"""
        if not day:
            args = self._args(event)
            day = args[0] if args else ""
        day = _norm_day(day, _today())
        if not self.growth_enabled:
            yield event.plain_result("桌面复盘模块已关闭，先发 /成长开关 on。")
            return
        digest = await self._generate_digest(day, force=True)
        if not digest:
            yield event.plain_result(
                f"{_pretty_day(day)} 没有日报，无法生成摘要。"
                f"先确认晚间 nightly 已跑（{self._last_growth_error}）。")
            return
        if digest.get("empty"):
            yield event.plain_result(
                f"{_pretty_day(day)} 日报为空，已记录摘要但不推送。")
            return
        yield event.plain_result(self._digest_text(digest))

    @filter.command("成长订阅")
    async def cmd_growth_subscribe(self, event: AstrMessageEvent):
        """把当前微信会话加入桌面复盘推送。"""
        umo = str(getattr(event, "unified_msg_origin", "") or "").strip()
        if not umo:
            yield event.plain_result("拿不到当前会话标识，订阅失败。")
            return
        if umo not in self.growth_targets:
            self.growth_targets.append(umo)
            self.config["growth_push_targets"] = list(self.growth_targets)
            self._save()
        yield event.plain_result(
            f"已订阅桌面复盘（共 {len(self.growth_targets)} 个会话）。"
            f"每晚 {self.growth_time}，当前推送"
            f"{'已开启' if self.growth_push_enabled else '未开启，发 /桌面复盘推送 on 开启'}。")

    @filter.command("成长测试")
    async def cmd_growth_test(self, event: AstrMessageEvent):
        """向已订阅会话发送一条推送链路测试消息，不写摘要、不动日报。"""
        if not self.growth_targets:
            yield event.plain_result("还没有订阅会话。先在目标微信会话发 /成长订阅。")
            return
        text = "【大肥鱼·桌面复盘】推送链路测试：今晚到点，我会在这里留一张复盘卡。"
        sent, failed = 0, 0
        for umo in self.growth_targets:
            try:
                await self.context.send_message(umo, MessageChain().message(text))
                sent += 1
                logger.info(f"[pet_memory] 成长测试消息已推送到 {umo}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                logger.error(f"[pet_memory] 成长测试推送到 {umo} 失败: {e}")
        yield event.plain_result(f"测试推送完成：成功 {sent}，失败 {failed}。")

    @filter.command("成长退订")
    async def cmd_growth_unsubscribe(self, event: AstrMessageEvent):
        """把当前会话从桌面复盘推送里移除。"""
        umo = str(getattr(event, "unified_msg_origin", "") or "").strip()
        if umo not in self.growth_targets:
            yield event.plain_result("当前会话没有订阅桌面复盘。")
            return
        self.growth_targets = [t for t in self.growth_targets if t != umo]
        self.config["growth_push_targets"] = list(self.growth_targets)
        self._save()
        yield event.plain_result(f"已退订桌面复盘（剩 {len(self.growth_targets)} 个会话）。")

    @filter.command("桌面复盘推送")
    async def cmd_growth_push(self, event: AstrMessageEvent, value: str = ""):
        """开关桌面复盘推送或改时间：/桌面复盘推送 on|off|HH:MM。"""
        if not value:
            args = self._args(event)
            value = args[0] if args else ""
        if not value:
            yield event.plain_result(
                f"桌面复盘推送：{'开' if self.growth_push_enabled else '关'} · "
                f"{self.growth_time} · 订阅 {len(self.growth_targets)} 个会话\n"
                f"用法：/桌面复盘推送 on|off|HH:MM")
            return
        value = value.strip().lower()
        if value in ("on", "开"):
            self.growth_push_enabled = True
            self.config["growth_push_enabled"] = True
        elif value in ("off", "关"):
            self.growth_push_enabled = False
            self.config["growth_push_enabled"] = False
        else:
            try:
                datetime.strptime(value, "%H:%M")
            except ValueError:
                yield event.plain_result("时间格式应为 HH:MM，例如 /桌面复盘推送 21:30")
                return
            self.growth_time = value
            self.config["growth_time"] = value
            self.growth_push_enabled = True
            self.config["growth_push_enabled"] = True
        self._save()
        yield event.plain_result(
            f"桌面复盘推送：{'开' if self.growth_push_enabled else '关'} · "
            f"{self.growth_time} · 订阅 {len(self.growth_targets)} 个会话")

    @filter.command("成长开关")
    async def cmd_growth_toggle(self, event: AstrMessageEvent, value: str = ""):
        """启用/关闭桌面复盘模块：/成长开关 on|off。"""
        if not value:
            args = self._args(event)
            value = args[0] if args else ""
        value = value.lower()
        if value in ("on", "开"):
            self.growth_enabled = True
            self.config["growth_enabled"] = True
            self._refresh_growth_cache()
            self._save()
            yield event.plain_result("桌面复盘模块已启用；晚间摘要、人格注入、微信推送恢复。")
        elif value in ("off", "关"):
            self.growth_enabled = False
            self.config["growth_enabled"] = False
            self._growth_cache = ""
            self._save()
            yield event.plain_result("桌面复盘模块已关闭；已有摘要保留，不再生成/推送/注入。")
        else:
            yield event.plain_result(
                f"当前：{'已启用' if self.growth_enabled else '已关闭'}\n"
                f"用法：/成长开关 on|off")

    @filter.command("记忆开关")
    async def cmd_toggle(self, event: AstrMessageEvent, value: str = ""):
        """启用/关闭记忆插件：/记忆开关 on|off。"""
        if not value:
            args = self._args(event)
            value = args[0] if args else ""
        value = value.lower()
        if value in ("on", "开"):
            self.enabled = True
            self.config["enabled"] = True
            self._save()
            self._register_tool()
            yield event.plain_result("记忆插件已启用，pet_memory_query 已注册。")
        elif value in ("off", "关"):
            self.enabled = False
            self.config["enabled"] = False
            self._save()
            self._unregister_tool()
            yield event.plain_result("记忆插件已关闭，pet_memory_query 已摘除；"
                                     "raw/日报数据保留。")
        else:
            yield event.plain_result(
                f"当前：{'已启用' if self.enabled else '已关闭'}\n用法：/记忆开关 on|off")

    # ---------------------------------------------------------------- LLM 工具
    async def _tool_query(self, event: AstrMessageEvent, *args, **kwargs) -> str:
        """pet_memory_query 的统一入口：只读云端契约，返回结构化 JSON。"""
        if args:
            for arg in args:
                if isinstance(arg, dict):
                    kwargs = {**arg, **kwargs}
        if not self.enabled:
            return json.dumps({"ok": False, "reason": "记忆插件已关闭"},
                              ensure_ascii=False)
        qtype = str(kwargs.get("type") or "day").strip().lower()
        if qtype not in ("day", "person", "commitments", "search"):
            qtype = "day"
        date_from = _norm_day(str(kwargs.get("date_from") or ""), _today())
        date_to = _norm_day(str(kwargs.get("date_to") or ""), _today())
        person = str(kwargs.get("person") or "").strip()
        q = str(kwargs.get("q") or "").strip()
        try:
            limit = max(1, min(50, int(kwargs.get("limit") or self.max_items)))
        except (TypeError, ValueError):
            limit = self.max_items

        try:
            if qtype == "commitments":
                data = await self._resource(
                    "memory://commitments", {"date_from": date_from, "date_to": date_to})
                out = {"ok": True, "type": qtype, "range": [date_from, date_to],
                       "items": (data or [])[:limit]}
            else:
                out = await self._query(qtype, date_from, date_to,
                                        person=person, q=q, limit=limit)
            out["source"] = "pet_memory"
            return json.dumps(out, ensure_ascii=False)
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 工具查询失败: {e}")
            return json.dumps({"ok": False, "reason": f"记忆网关不可达或出错：{e}"},
                              ensure_ascii=False)

    # ---------------------------------------------------------------- 每日推送
    async def _push_loop(self):
        while not self._stop.is_set():
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=30)
                break
            except asyncio.TimeoutError:
                pass
            try:
                await self._push_once()
            except Exception as e:  # noqa: BLE001
                logger.error(f"[pet_memory] 每日推送检查失败: {e}")

    async def _push_once(self):
        if not self.enabled or not self.config.get("daily_push_enabled"):
            return
        targets = list(self.config.get("daily_push_targets") or [])
        if not targets:
            return
        now = datetime.now()
        if now.strftime("%H:%M") != str(self.config.get("daily_push_time") or "08:30"):
            return
        today = now.strftime("%Y%m%d")
        if self._last_push_day == today:
            return
        self._last_push_day = today

        day = _day_str(now.date() - timedelta(days=1))
        try:
            report = await self._report(day)
        except Exception as e:  # noqa: BLE001
            logger.error(f"[pet_memory] 取日报失败 {day}: {e}")
            return
        if not report:
            logger.info(f"[pet_memory] {day} 没有日报，跳过推送")
            return

        text = self._report_text(report)
        for umo in targets:
            try:
                await self.context.send_message(umo, MessageChain().message(text))
                logger.info(f"[pet_memory] 日报已推送到 {umo}")
            except Exception as e:  # noqa: BLE001
                logger.error(f"[pet_memory] 推送到 {umo} 失败: {e}")

    # ---------------------------------------------------------------- 杂项
    def _save(self):
        save = getattr(self.config, "save_config", None)
        if callable(save):
            try:
                save()
            except Exception as e:  # noqa: BLE001
                logger.error(f"[pet_memory] 保存配置失败: {e}")
        else:
            logger.warning("[pet_memory] 配置对象不支持 save_config，改动只在内存生效")

    def _args(self, event: AstrMessageEvent) -> list[str]:
        # AstrBot 的 CommandFilter 会把解析后的参数放进 event._extras["parsed_params"]，
        # 但 message_str 只剩指令名；优先读这个，避免参数丢失。
        try:
            parsed = event.get_extra("parsed_params") or {}
            if isinstance(parsed, dict) and parsed:
                return [str(v) for v in parsed.values() if v not in (None, "")]
            handlers = event.get_extra("handlers_parsed_params") or {}
            if isinstance(handlers, dict):
                for params in handlers.values():
                    if isinstance(params, dict) and params:
                        vals = [str(v) for v in params.values() if v not in (None, "")]
                        if vals:
                            return vals
        except Exception:  # noqa: BLE001
            pass
        raw = ""
        obj = getattr(event, "message_obj", None)
        object_str = str(getattr(obj, "message_str", "") or "")
        if object_str:
            raw = object_str
        for comp in (getattr(obj, "message", None) or []):
            text = getattr(comp, "text", None)
            if text and str(text) not in raw:
                raw += str(text)
        if not raw:
            raw = str(getattr(event, "message_str", "") or "")
        raw = raw.strip().lstrip("/")
        parts = raw.split()
        logger.debug(f"[pet_memory] _args raw={raw!r} parts={parts!r}")
        if not parts:
            return []
        if parts[0].startswith("记忆") or parts[0].startswith("成长"):
            return parts[1:]
        return parts
