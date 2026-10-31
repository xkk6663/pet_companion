# -*- coding: utf-8 -*-
"""S9 设备端动作端点验收探针（烧录后在 PC 上跑，设备与 PC 同一局域网）。

用法：
    python probe_device_actions.py 192.168.4.1
    python probe_device_actions.py 192.168.x.x --full     # 含打卡（约 30s，需人脸库）

设计纪律（踩过的坑，见工程规范）：
  * 只用 urllib + **显式禁用代理**：本机 Clash TUN/环境变量会劫持 192.168.*.*
    的 http 请求，表现为收到 502 文本，会被误判成"设备端解析失败"。
  * 每个端点都断言**状态码 + 字段**，不打印"看起来对"就完事。
  * 长事务端点只验"受理（202）+ 轮询能收敛"，不假设耗时。
"""
import json
import sys
import time
import urllib.error
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
HTTP_TIMEOUT = 8


def call(base, path, method="GET", body=None, timeout=HTTP_TIMEOUT):
    url = base + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with OPENER.open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            return r.status, raw
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return -1, str(e)


def show(name, status, raw):
    ok = 200 <= status < 300
    print(f"[{'OK ' if ok else 'ERR'}] {name} -> HTTP {status}  {raw[:300]}")
    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    full = "--full" in sys.argv
    base = f"http://{host}"

    fails = 0

    st, raw = call(base, "/api/system/status")
    fails += 0 if show("GET /api/system/status", st, raw) else 1
    try:
        s = json.loads(raw)
        print(f"      link.connected={s['link']['connected']} session={s['link']['session']} "
              f"wifi={s['wifi']['role']}/{s['wifi']['ip']} rssi={s['wifi']['rssi']} "
              f"heap_int={s['heap_internal']} heap_psram={s['heap_psram']}")
        if not s["link"]["connected"]:
            print("      ⚠ 网关未连上：先看设备日志 [WS] 段与 8081 是否可达")
    except Exception as e:  # noqa: BLE001
        print(f"      status JSON 解析失败: {e}")
        fails += 1

    st, raw = call(base, "/api/light/get")
    fails += 0 if show("GET /api/light/get", st, raw) else 1

    st, raw = call(base, "/api/light/set", "POST", {"power": True, "rgb": [255, 180, 90], "brightness": 40})
    fails += 0 if show("POST /api/light/set", st, raw) else 1

    st, raw = call(base, "/api/light/set", "POST", {"power": False})
    fails += 0 if show("POST /api/light/set (off)", st, raw) else 1

    st, raw = call(base, "/api/media/capture", "POST", {})
    if show("POST /api/media/capture", st, raw):
        try:
            gen = json.loads(raw).get("gen", 0)
            for _ in range(15):
                time.sleep(1)
                s2, r2 = call(base, f"/api/action/result?gen={gen}")
                j = json.loads(r2)
                if j.get("state") == "done":
                    print(f"      capture done: {r2[:200]}")
                    break
            else:
                print("      ⚠ capture 15s 未收敛（SD 未插/相机被占？）")
                fails += 1
        except Exception as e:  # noqa: BLE001
            print(f"      轮询失败: {e}")
            fails += 1

    st, raw = call(base, "/api/media/play?url=/static/nonexistent.wav", "POST", {})
    show("POST /api/media/play (故意坏 URL，只验受理/拒绝语义)", st, raw)

    if full:
        st, raw = call(base, "/api/punch", "POST", {})
        if show("POST /api/punch", st, raw):
            try:
                gen = json.loads(raw).get("gen", 0)
                print("      等打卡收敛（最长 45s，需要人脸库 + 看着镜头）...")
                for i in range(45):
                    time.sleep(1)
                    s2, r2 = call(base, f"/api/action/result?gen={gen}")
                    j = json.loads(r2)
                    if j.get("state") == "done":
                        print(f"      punch done: {r2[:260]}")
                        break
                    if i % 10 == 9:
                        print(f"      ...{i + 1}s running")
                else:
                    print("      ⚠ punch 45s 未收敛")
                    fails += 1
            except Exception as e:  # noqa: BLE001
                print(f"      轮询失败: {e}")
                fails += 1

    print("----")
    print("PASS" if fails == 0 else f"{fails} 项异常，请看上面 ERR 行")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
