#!/usr/bin/env python3
"""从设备抓取 LVGL 真实渲染界面。

数据源是 RGB 面板的 framebuffer（见 main/middleware/net/screen_shot.c），
不是 PC 仿真，也不是 lv_snapshot 的重新渲染，所以抓到的就是屏幕正在
扫描输出的像素。

用法：
  python tools/diag/screen_shot.py --host 192.168.4.1
  python tools/diag/screen_shot.py --host 192.168.4.1 --fmt jpg --count 5 --interval 1
  python tools/diag/screen_shot.py --host 192.168.4.1 --scale 2 --out .screenshots

默认输出到仓库根目录的 .screenshots/。
"""
import argparse
import datetime as dt
import pathlib
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAGIC = {"bmp": b"BM", "jpg": b"\xff\xd8"}
# 局域网设备不应该走系统代理
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def grab(host, fmt, scale, quality, timeout):
    url = (f"http://{host}/api/display/shot"
           f"?fmt={fmt}&scale={scale}&q={quality}")
    req = urllib.request.Request(url, headers={"Cache-Control": "no-store"})
    with OPENER.open(req, timeout=timeout) as resp:
        return resp.read()


def main():
    ap = argparse.ArgumentParser(description="抓取设备端真实屏幕（RGB framebuffer）")
    ap.add_argument("--host", default="192.168.4.1",
                    help="设备 IP（AP 模式 192.168.4.1；STA 模式填路由器分配地址）")
    ap.add_argument("--fmt", choices=["bmp", "jpg"], default="bmp",
                    help="bmp = 无损；jpg = 体积小（设备端软编，较慢）")
    ap.add_argument("--scale", type=int, choices=[1, 2], default=1,
                    help="1 = 800x480；2 = 400x240（2x2 抽样）")
    ap.add_argument("--quality", type=int, default=80, help="JPEG 质量 1~100")
    ap.add_argument("--out", default=".screenshots", help="输出目录")
    ap.add_argument("--count", type=int, default=1, help="连续抓取张数")
    ap.add_argument("--interval", type=float, default=1.0, help="连续抓取间隔（秒）")
    ap.add_argument("--timeout", type=float, default=30.0, help="单次 HTTP 超时")
    args = ap.parse_args()

    ext = "jpg" if args.fmt == "jpg" else "bmp"
    outdir = pathlib.Path(args.out)
    if not outdir.is_absolute():
        outdir = ROOT / outdir
    outdir.mkdir(parents=True, exist_ok=True)

    for i in range(max(1, args.count)):
        try:
            data = grab(args.host, args.fmt, args.scale, args.quality, args.timeout)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            print(f"[FAIL] HTTP {e.code}: {body}", file=sys.stderr)
            return 1
        except Exception as e:                      # 连接失败/超时
            print(f"[FAIL] {args.host}: {e}", file=sys.stderr)
            return 1

        if not data.startswith(MAGIC[args.fmt]):
            print(f"[FAIL] 返回内容不是 {args.fmt}: {data[:64]!r}", file=sys.stderr)
            return 1

        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        name = (f"screen_{stamp}_{i:02d}.{ext}" if args.count > 1
                else f"screen_{stamp}.{ext}")
        path = outdir / name
        path.write_bytes(data)
        print(f"[OK] {path} ({len(data)} B)")

        if i + 1 < args.count:
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
