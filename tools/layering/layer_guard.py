#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""layer_guard.py — pet_companion 专用分层护栏（评估/CI 通用）。

与 embedded-mcu-layer-decoupling 的通用 layer_audit.py 的区别：
  通用脚本把 "include 厂商头" 一律算违规，对 ESP-IDF 工程会大面积误报
  （esp_log.h / esp_err.h 是平台基础头；camera_service.c 本身就是驱动）。
  本脚本改为按 "谁在 include 谁" 判层，只盯真正的架构裂缝：

  G1 UPWARD_DEP      低层 include 高层（反向依赖），例：periph -> net
  G2 UI_BOARD_HEAD   ui/ 直接 include 板级/BSP/厂商硬件头
  G3 HEADER_LEAK     middleware 的**公共头**里出现板级或厂商类型（接口不干净）
  G4 UI_IN_MIDDLE   middleware 出现 ui/ 头或 lvgl.h（中间层认识 UI）
  G5 GLOBAL_VAR_HDR 公共头里的 extern 可变全局变量（跨模块共享状态）
  G6 CYCLE           middleware 子目录 / 顶层目录之间的 include 环

用法：
    python tools/layering/layer_guard.py --root . --format text
    python tools/layering/layer_guard.py --root . --format json --out layers.json
    python tools/layering/layer_guard.py --root . --baseline layers.json   # 只看新增
    python tools/layering/layer_guard.py --root . --update-baseline layers.json
    python tools/layering/layer_guard.py --root . --fail-on high           # CI
退出码：0 通过 / 1 达到阈值 / 2 参数错误。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path

SEV = {"high": 3, "medium": 2, "low": 1}
SRC_EXT = (".c", ".h", ".cpp", ".hpp")
EXCLUDE_DIRS = ("build", "managed_components", ".workbuddy", "components",
                ".venv", "tests", "docs", "littlefs_content", ".git", ".pytest_cache")

# 目标分层（rank 越小越底层）。当前目录树用 PATH_MAP 过渡映射，
# 迁移完成后可把白名单条目删掉，规则自然收紧。
RANK = {"platform": 1, "drivers": 2, "services": 3, "business": 4, "ui": 5, "app": 6}

# 目录级默认层号
PATH_MAP = [
    ("main/platform", "platform"),
    ("main/drivers", "drivers"),
    ("main/middleware/periph", "drivers"),
    ("main/middleware/camera", "drivers"),      # 目录内再按 FILE_MAP 细分
    ("main/middleware/bluetooth", "services"),
    ("main/middleware/mem", "drivers"),
    ("main/middleware/media", "services"),
    ("main/middleware/audio", "services"),
    ("main/middleware/net", "services"),
    ("main/middleware/face_db", "services"),    # 人脸库 = 存储层
    ("main/middleware/ai", "business"),
    ("main/ui", "ui"),
    ("main/app", "app"),
]

# 文件级覆盖：camera/ 与 media/ 内部混了三层，逐个点名（迁移后删掉这些条目）
FILE_MAP = {
    # L2 驱动（机制）
    "main/middleware/camera/camera_service": "drivers",
    "main/middleware/camera/ov3660_fmt": "drivers",
    "main/middleware/camera/jpeg_util": "drivers",
    "main/middleware/media/avi_mjpeg_muxer": "drivers",
    "main/middleware/media/avi_mjpeg_demuxer": "drivers",
    # L3 服务（策略/管线）
    "main/middleware/camera/viewfinder": "services",
    "main/middleware/camera/face_service": "services",
    "main/middleware/camera/face_recog_service": "services",
    "main/middleware/camera/capture_service": "services",
    "main/middleware/media/file_mgr_service": "services",
    "main/middleware/media/record_video_service": "services",
    # L4 业务
    "main/middleware/camera/punch_service": "business",
    "main/middleware/media/wallpaper_service": "business",
    # middleware/ai 目录内混了"传输/会话（L3）"与"业务用例（L4）"，逐文件点名。
    # 判定依据：模块只做协议/通道/会话生命周期 = L3；做"用户意图 → 编排多个服务" = L4。
    # L3 传输与会话
    "main/middleware/ai/ws_client": "services",
    "main/middleware/ai/event_bus": "services",
    "main/middleware/ai/xiaozhi_service": "services",
    "main/middleware/ai/cloud_media_service": "services",
    "main/middleware/ai/voice_service": "services",
    "main/middleware/ai/slice_client": "services",
    "main/middleware/ai/music_source": "services",
    "main/middleware/ai/music_lib": "services",
    # L4 业务用例（工具调用 / 打卡 / 拍摄 / 曲库 / 灯 / 说话 / 设备信息 / 网页动作端点）
    "main/middleware/ai/ai_action": "business",
    "main/middleware/ai/ai_punch": "business",
    "main/middleware/ai/ai_camera": "business",
    "main/middleware/ai/ai_music": "business",
    "main/middleware/ai/ai_light": "business",
    "main/middleware/ai/ai_speaker": "business",
    "main/middleware/ai/ai_device": "business",
    "main/middleware/ai/ai_action_endpoints": "business",
}

# 允许出现在 .h 里的平台基础头（只报"板级/硬件类型"泄漏）
HEADER_ALLOW = {"esp_err.h", "esp_log.h", "sdkconfig.h", "freertos/FreeRTOS.h",
                "freertos/task.h", "freertos/queue.h", "freertos/semphr.h",
                "freertos/event_groups.h", "stdint.h", "stddef.h", "stdbool.h",
                "string.h", "stdio.h", "inttypes.h", "math.h"}

HARDWARE_INCLUDE = re.compile(
    r'#\s*include\s*[<"](bsp/[^">]+|driver/[^">]+|esp_lcd[^">]*|esp_video[^">]*|'
    r'esp_cam_sensor[^">]*|esp_codec_dev[^">]*|esp_bt[^">]*|esp_littlefs[^">]*|'
    r'esp_vfs_fat[^">]*|sdmmc[^">]*|esp_wifi[^">]*|esp_netif[^">]*|'
    r'esp_jpeg[^">]*|esp_gmf[^">]*|esp_gsc[^">]*|esp_sr[^">]*|hand_[^">]*)[">]')

GENERIC_INCLUDE = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')
EXTERN_VAR = re.compile(r'\s*extern\s+[^();]*?[A-Za-z_][A-Za-z0-9_]*\s*(\[[^\]]*\])?\s*;')


@dataclass
class Finding:
    rule: str
    severity: str
    layer: str
    path: str
    line: int
    snippet: str
    message: str
    fingerprint: str


def fp(rule: str, rel: str, code: str) -> str:
    norm = re.sub(r'\s+', ' ', code.strip())
    return hashlib.sha1(f"{rule}|{rel}|{norm}".encode("utf-8", "replace")).hexdigest()[:16]


def layer_of(rel: str) -> tuple[str, str]:
    """返回 (layer 名, 所属模块键)"""
    stem = rel.rsplit(".", 1)[0]
    for key, lay in FILE_MAP.items():
        if stem == key:
            dir_part = key.rsplit("/", 1)[0]
            return lay, dir_part.rsplit("/", 1)[-1]
    for prefix, lay in PATH_MAP:
        if rel.startswith(prefix + "/") or rel == prefix:
            mod = rel[len(prefix) + 1:].split("/")[0] if "/" in rel[len(prefix) + 1:] else prefix
            return lay, mod
    return "other", "other"


def read_text(p: Path) -> str:
    raw = p.read_bytes()
    for enc in ("utf-8-sig", "utf-8", "gb18030"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1", errors="replace")


def iter_files(root: Path):
    for dp, dn, fn in os.walk(root):
        rel_dir = Path(dp).relative_to(root).as_posix()
        dn[:] = [d for d in dn if not any(x in (rel_dir + "/" + d) for x in EXCLUDE_DIRS)]
        for f in fn:
            if f.endswith(SRC_EXT):
                yield Path(dp) / f


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--format", choices=("text", "json"), default="text")
    ap.add_argument("--out")
    ap.add_argument("--baseline")
    ap.add_argument("--update-baseline")
    ap.add_argument("--fail-on", choices=("none", "low", "medium", "high"), default="none")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    root = Path(a.root).expanduser().resolve()
    if not root.is_dir():
        print(f"错误：目录不存在 {root}", file=sys.stderr)
        return 2

    files = {p.relative_to(root).as_posix(): p for p in iter_files(root)}
    layers = {rel: layer_of(rel) for rel in files}
    by_basename = {}
    for rel in files:
        by_basename.setdefault(rel.split("/")[-1], []).append(rel)

    findings: list[Finding] = []
    edge = {}   # (src_module_key, tgt_module_key) -> set(src_rel)

    def add(rule, sev, lay, rel, ln, code, msg):
        findings.append(Finding(rule, sev, lay, rel, ln, code.strip()[:160], msg,
                                fp(rule, rel, code)))

    for rel, p in files.items():
        lay, mod = layers[rel]
        if lay == "other":
            continue
        text = read_text(p)
        is_hdr = rel.endswith((".h", ".hpp"))
        for ln, line in enumerate(text.splitlines(), 1):
            s = line.strip()
            if s.startswith("//") or s.startswith("*"):
                continue
            # 显式豁免：行内或上一行写 `// layer-guard:ignore <理由>`。
            # 用于**编译期就没生效**的依赖（如 #if CONFIG_XXX=n 才 include 的探针），
            # 静态扫描看不到预处理条件，只能由人举证；理由必须写在注释里。
            if "layer-guard:ignore" in line:
                continue
            if ln >= 2:
                prev = text.splitlines()[ln - 2]
                if "layer-guard:ignore" in prev:
                    continue
            m = GENERIC_INCLUDE.search(line)
            if m:
                inc = m.group(1)
                base = inc.split("/")[-1]
                # 解析本地 include 到同仓库文件
                tgt = None
                if inc in files:
                    tgt = inc
                elif base in by_basename and len(by_basename[base]) == 1:
                    tgt = by_basename[base][0]
                elif base in by_basename:
                    cands = [c for c in by_basename[base] if c.startswith(inc.split("/")[0])] \
                             or by_basename[base]
                    tgt = cands[0]
                if tgt and tgt != rel:
                    tlay, tmod = layers[tgt]
                    edge.setdefault((rel, tgt), None)
                    if tlay != "other" and tlay != lay:
                        if RANK[tlay] > RANK[lay]:
                            add("G1", "high", lay, rel, ln, line,
                                f"反向依赖：{lay}({mod}) -> {tlay}({tmod})，"
                                f"应改为回调/事件或注入")
                # G2 ui 直接吃板级/硬件头
                if lay == "ui" and HARDWARE_INCLUDE.search(line):
                    add("G2", "high", lay, rel, ln, line,
                        "UI 层直接 include 板级/硬件头，换屏/换板要改 UI")
                # G4 中间层认识 UI
                if lay in ("drivers", "services", "business", "platform"):
                    if inc.startswith("ui/") or base == "lvgl.h":
                        add("G4", "high", lay, rel, ln, line,
                            "中间层 include UI/LVGL，层次倒置")
                # G3 公共头带厂商/板级类型
                # 判据只认"硬件/板级"（HARDWARE_INCLUDE 里那张白名单）：
                #   · 协议/通道 SDK（esp_http_server / esp_mcp_engine / esp_websocket_client …）
                #     属于"与云端对话的接口"，不是硬件依赖，不算 G3；
                #   · 驱动自己的公共头（ov3660_fmt.h 带 esp_cam_sensor_types）由
                #     PATH_MAP 判为 drivers，允许 —— 驱动就该认识自己的硬件。
                if is_hdr and lay in ("drivers", "services", "business", "platform"):
                    if HARDWARE_INCLUDE.search(line):
                        add("G3", "high", lay, rel, ln, line,
                            "公共头暴露板级/硬件类型，消费者被迫继承硬件依赖")
            if is_hdr and lay in ("drivers", "services", "business", "platform"):
                if EXTERN_VAR.match(line) and "(" not in line and "const" not in line:
                    add("G5", "medium", lay, rel, ln, line,
                        "公共头里的 extern 可变全局，跨模块共享状态")

    # G6 环：在同层模块之间检测
    graph: dict[str, set[str]] = {}
    for (src, tgt) in edge:
        slay, smod = layers[src]
        tlay, tmod = layers[tgt]
        if slay == tlay and slay in ("drivers", "services", "business"):
            graph.setdefault(f"{slay}:{smod}", set()).add(f"{slay}:{tmod}")

    def scc(nodes):
        idx, low, st, on, res = {}, {}, [], set(), []
        cnt = [0]

        def dfs(u):
            idx[u] = low[u] = cnt[0]; cnt[0] += 1
            st.append(u); on.add(u)
            for v in graph.get(u, ()):
                if v not in idx:
                    dfs(v); low[u] = min(low[u], low[v])
                elif v in on:
                    low[u] = min(low[u], idx[v])
            if low[u] == idx[u]:
                comp = []
                while True:
                    w = st.pop(); on.discard(w); comp.append(w)
                    if w == u:
                        break
                if len(comp) > 1:
                    res.append(sorted(comp))

        for n in nodes:
            if n not in idx:
                dfs(n)
        return res

    for comp in scc(sorted(graph)):
        add("G6", "medium", "mixed", "<graph>", 0, " <-> ".join(comp),
            f"模块环（{len(comp)} 个成员）：改动任一方都会波及全体")

    findings.sort(key=lambda f: (-SEV[f.severity], f.path, f.line))
    base = set()
    note = ""
    if a.baseline and Path(a.baseline).is_file():
        data = json.loads(read_text(Path(a.baseline)))
        base = {f["fingerprint"] for f in data.get("findings", [])}
        note = f"基线 {a.baseline}：{len(base)} 条存量"
        findings = [f for f in findings if f.fingerprint not in base]

    summary = {
        "total": len(findings),
        "by_severity": {k: sum(1 for f in findings if f.severity == k) for k in SEV},
        "by_rule": {r: sum(1 for f in findings if f.rule == r)
                    for r in sorted({f.rule for f in findings})},
        "files": len({f.path for f in findings}),
    }
    payload = {"root": str(root), "summary": summary,
               "findings": [asdict(f) for f in findings]}

    if a.update_baseline:
        Path(a.update_baseline).write_text(
            json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"已写入基线 {a.update_baseline}（{len(findings)} 条）")

    if a.format == "json":
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        if a.out:
            Path(a.out).write_text(text, encoding="utf-8")
            print(f"已写入 {a.out}")
        else:
            print(text)
    elif a.quiet:
        print(f"total={summary['total']} high={summary['by_severity']['high']} "
              f"medium={summary['by_severity']['medium']} low={summary['by_severity']['low']}")
    else:
        print("=" * 74)
        print("layer_guard — pet_companion 分层护栏")
        print("=" * 74)
        if note:
            print(note)
        print(f"共 {summary['total']} 处 / {summary['files']} 文件 | high={summary['by_severity']['high']} "
              f"medium={summary['by_severity']['medium']} low={summary['by_severity']['low']}")
        if summary["by_rule"]:
            print("规则: " + ", ".join(f"{k}={v}" for k, v in summary["by_rule"].items()))
        print("-" * 74)
        cur = None
        for f in findings:
            if f.path != cur:
                cur = f.path
                print(f"\n{f.path} [{f.layer}]")
            print(f"  {f.line:>5} {f.severity.upper():<6} {f.rule} {f.message}")
            if f.snippet:
                print(f"        | {f.snippet}")

    if a.fail_on != "none":
        th = SEV[a.fail_on]
        if any(SEV[f.severity] >= th for f in findings):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
