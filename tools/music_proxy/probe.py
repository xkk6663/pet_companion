#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
probe.py — music_proxy 部署后自检（真值探针，不是 curl 看一眼就算过）

用法:
    python3 probe.py http://127.0.0.1:8080 <MUSIC_TOKEN>
    python3 probe.py https://music.example.com <TOKEN> --song 起风了

它按设备真实调用顺序走一遍：healthz → sources → search → url → download →
lyric，并把每步的真实响应（码率/字节数/耗时/来源）打出来。
★ 这套顺序就是端侧 ai_music 的调用顺序，任何一步 FAIL 都会让"下载到 SD"失败。
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

UA = "music_proxy-probe/1.0"
FAIL = []


def req(base, path, token=None, headers=None, read_bytes=0, timeout=30):
    url = base.rstrip("/") + path
    h = {"User-Agent": UA}
    if token:
        h["X-Music-Token"] = token
    h.update(headers or {})
    t0 = time.time()
    r = urllib.request.Request(url, headers=h)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            if read_bytes:
                body = resp.read(read_bytes)
                return resp.status, dict(resp.headers), body, int((time.time() - t0) * 1000)
            return resp.status, dict(resp.headers), resp.read(), int((time.time() - t0) * 1000)
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read()[:400], int((time.time() - t0) * 1000)
    except Exception as e:
        return 0, {}, str(e).encode(), int((time.time() - t0) * 1000)


def check(name, cond, detail, optional=False):
    """optional=True：缺了不阻塞（歌词/封面属于锦上添花，没有也照样能播能下载）。"""
    if cond:
        mark = "PASS"
    elif optional:
        mark = "WARN"
    else:
        mark = "FAIL"
        FAIL.append(name)
    print("  [%s] %-26s %s" % (mark, name, detail))
    return cond


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    base, token = sys.argv[1], sys.argv[2]
    song = "起风了"
    if "--song" in sys.argv:
        song = sys.argv[sys.argv.index("--song") + 1]

    print("\n=== music_proxy 自检  %s ===\n" % base)

    st, _, body, ms = req(base, "/healthz")
    ok = st == 200 and b"music_proxy" in body
    check("healthz", ok, "HTTP %s %dms" % (st, ms))
    if not ok:
        print("\n服务没起来或地址不对，后面的不再继续。")
        return 1

    st, _, body, ms = req(base, "/v1/search?q=x")  # 故意不带 token
    check("auth 必须拒绝", st == 401, "HTTP %s" % st)

    st, _, body, ms = req(base, "/v1/sources?deep=1", token, timeout=90)
    try:
        src = json.loads(body).get("data") or []
        good = [s["name"] for s in src if s.get("ok")]
        detail = ", ".join("%s(搜:%s 链:%s %dms)" % (s["name"], "ok" if s.get("ok") else "fail",
                                                     s.get("url", "?"), s.get("ms", 0)) for s in src)
        check("sources", bool(good), "可用=%s | %s" % (good or "无", detail))
    except Exception:
        check("sources", False, body[:160])

    st, _, body, ms = req(base, "/v1/search?q=%s&limit=3" % urllib.parse.quote(song), token)
    songs = []
    try:
        songs = (json.loads(body) or {}).get("data") or []
    except Exception:
        pass
    ok = check("search", bool(songs), "HTTP %s %dms 首条=%s"
               % (st, ms, ("%s - %s [%s]" % (songs[0]["name"], songs[0]["artist"], songs[0]["source"])) if songs else body[:120]))
    if not ok:
        print("\n结果:\n  FAIL: %s" % ", ".join(FAIL))
        return 1

    sid = songs[0]["id"]
    print("       id = %s" % sid[:72])

    st, _, body, ms = req(base, "/v1/url?id=%s&br=128" % urllib.parse.quote(sid), token)
    info = {}
    try:
        info = (json.loads(body) or {}).get("data") or {}
    except Exception:
        pass
    check("url(128k)", bool(info.get("url")),
          "HTTP %s %dms size=%.2fMB br=%s" % (st, ms, (info.get("size") or 0) / 1048576.0, info.get("br")))

    st, hdr, part, ms = req(base, "/v1/download?id=%s&br=128" % urllib.parse.quote(sid), token, read_bytes=65536)
    check("download 前 64KB", st == 200 and len(part) >= 8192,
          "HTTP %s %dms got=%dB ctype=%s clen=%s"
          % (st, ms, len(part), hdr.get("Content-Type"), hdr.get("Content-Length")))

    st, hdr, part2, ms = req(base, "/v1/download?id=%s&br=128" % urllib.parse.quote(sid), token,
                             headers={"Range": "bytes=0-1023"}, read_bytes=1024)
    check("download Range", st in (200, 206), "HTTP %s got=%dB crange=%s" % (st, len(part2), hdr.get("Content-Range")))

    st, _, body, ms = req(base, "/v1/lyric?id=%s" % urllib.parse.quote(sid), token)
    lrc = ""
    try:
        lrc = ((json.loads(body) or {}).get("data") or {}).get("lrc") or ""
    except Exception:
        pass
    check("lyric", bool(lrc.strip()), "HTTP %s %dms %dB 首行=%r"
          % (st, ms, len(lrc), lrc.splitlines()[0] if lrc else ""), optional=True)

    print("\n结果: %s" % ("全部 PASS" if not FAIL else "FAIL: " + ", ".join(FAIL)))
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
