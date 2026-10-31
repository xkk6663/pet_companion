#!/usr/bin/env python3
"""串口日志观察器（S9 闭环用）。

比 docs/_serial_capture.py 多三件事：
  1. `--reset`：用 DTR/RTS 走 esptool 的复位序列，从**第一行启动日志**开始抓
     （否则连的是"已经跑了半小时"的状态，内存/连接一次性日志全错过）。
  2. `--grep`：只打印命中的行（配合 --out 保留全量）。
  3. `--out`：全量落文件，UTF-8，方便之后回看。

用法（Windows，Git Bash 或 PowerShell 都行）：
    python tools/diag/serial_watch.py --port COM9 --secs 45 --reset
    python tools/diag/serial_watch.py --port COM9 --secs 30 --grep "PET|WS|MEM"
"""
from __future__ import annotations

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("缺少 pyserial：pip install pyserial")


def hard_reset(ser: "serial.Serial") -> None:
    """esptool 同款：DTR=GPIO0，RTS=EN。拉低 EN 再放开 = 复位进正常启动。"""
    ser.setDTR(False)   # IO0 = 高（正常启动，不进下载模式）
    ser.setRTS(True)    # EN = 低（复位）
    time.sleep(0.12)
    ser.setRTS(False)   # EN = 高（放开复位）

    # 有些板子 RTS/DTR 反接，或用 USB-JTAG 直连；这里再补一次"反相"序列，
    # 目的是无论哪种接法都能真的复位一次，而不是靠运气。
    time.sleep(0.05)
    ser.setDTR(True)
    time.sleep(0.05)
    ser.setDTR(False)
    try:
        ser.reset_input_buffer()
    except Exception:  # noqa: BLE001
        pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=30.0)
    ap.add_argument("--reset", action="store_true", help="先硬复位，抓完整启动日志")
    ap.add_argument("--grep", default="", help="只打印匹配的行（正则，忽略大小写）")
    ap.add_argument("--out", default="", help="全量日志落盘路径")
    args = ap.parse_args()

    pat = re.compile(args.grep, re.I) if args.grep else None

    ser = serial.Serial(args.port, args.baud, timeout=0.4)
    # 打开时 pyserial 会把 DTR/RTS 拉高，某些板子因此进下载模式 —— 先清干净。
    ser.setDTR(False)
    ser.setRTS(False)

    if args.reset:
        hard_reset(ser)
        print("--- [serial_watch] 已发送复位序列，抓启动日志 ---", flush=True)

    fout = open(args.out, "a", encoding="utf-8") if args.out else None
    end = time.time() + args.secs
    nbytes = 0
    try:
        while time.time() < end:
            raw = ser.readline()
            if not raw:
                continue
            nbytes += len(raw)
            line = raw.decode("utf-8", "replace").rstrip()
            if fout:
                fout.write(line + "\n")
                fout.flush()
            if pat is None or pat.search(line):
                print(line, flush=True)
    except KeyboardInterrupt:
        print("--- [serial_watch] Ctrl-C 结束 ---", flush=True)
    finally:
        if fout:
            fout.close()
        ser.close()
    print(f"--- [serial_watch] {nbytes} bytes / {args.secs:.0f}s ---", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
