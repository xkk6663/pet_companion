"""测试基座：把仓库根与 tools/diag 放进 sys.path。

- 仓库根：`tests/support/*` 与 `main/...` 路径定位；
- tools/diag：直接 import 云端网关 `ws_gateway`（被测的是线上那份代码本身，
  不复制、不改写、不加测试钩子）。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for p in (ROOT, ROOT / "tools" / "diag"):
    s = str(p)
    if s not in sys.path:
        sys.path.insert(0, s)
