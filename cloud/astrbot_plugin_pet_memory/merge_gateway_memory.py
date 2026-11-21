# -*- coding: utf-8 -*-
"""把本地 S10 记忆段合并进云端网关，保留云端已有的流式 TTS 等改动。

只做文本拼接，不改其它函数：
  · 本地块：从「记忆：事件切片」到「插件主动推送」之前；
  · 云端文件：在同一个「插件主动推送」标记前插入。
"""
from __future__ import annotations

import sys
from pathlib import Path

LOCAL = Path(__file__).resolve().parents[1] / "gateway" / "ws_gateway.py"
SERVER = Path(sys.argv[1])
OUT = Path(sys.argv[2])

START = "# ---------------------------------------------------------------- 记忆：事件切片（S10 P3/P4）"
MARK = "# ---------------------------------------------------------------- 插件主动推送"


def main() -> None:
    local = LOCAL.read_text(encoding="utf-8")
    server = SERVER.read_text(encoding="utf-8")

    a = local.index(START)
    b = local.index(MARK, a)
    block = local[a:b].rstrip() + "\n\n\n"

    if START in server:
        print("server already has memory block; abort")
        raise SystemExit(2)
    i = server.index(MARK)
    merged = server[:i] + block + server[i:]
    OUT.write_text(merged, encoding="utf-8", newline="\n")

    print(f"block_lines={block.count(chr(10))}")
    print(f"out_lines={merged.count(chr(10))}")
    for token in ("/slice/{session_id}", "/memory/query", "/memory/resources",
                  "MEMORY_RESOURCES", "_iter_days", "load_day_events"):
        print(f"{token}: {merged.count(token)}")
    for token in ("tts_stream", "_sse_pcm_chunks", "tts_stream_endpoint",
                  "CIMG_PIXEL_CAP", "StreamingResponse"):
        print(f"preserve {token}: {merged.count(token)}")


if __name__ == "__main__":
    main()
