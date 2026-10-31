#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
music_proxy — 小智点歌「云侧音源代理」单文件服务（仅 Python 标准库）

为什么要有这一层
----------------
端侧 ai_music.c 现在是「哪家能返回一个像音频的 http 串就播那个」：
  · 端点会死（2026-10-21 实测 api.qijieya.cn/meting 13s 超时）
  · 只有一条 url，没有歌名/歌手/时长/歌词/封面 ⇒ 下载到 SD 时无法命名、无法显示
  · 各家 JSON 形状不同，端侧只能写「递归扫 JSON 找 http 串」这种瞎猜逻辑
把「找源」搬到云上，端侧只认 4 个固定 GET + 结构化 JSON：
  换源 / 加减镜像 = 改这里，不用重烧固件。

作者注：全部标准库，零 pip 依赖，`python3 music_proxy.py` 即可跑。
"""

import base64
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.0.0"

# ---------------------------------------------------------------- 配置

def _env(name, default=""):
    return os.environ.get(name, default).strip()


CFG = {
    # ★ 设备侧必须带这个 token（Header: X-Music-Token 或 ?token=）
    "token": _env("MUSIC_TOKEN", ""),
    # 自建网易云 NodeJS API（binaryify/netease_cloud_music_api），留空则不启用
    "netease_api": _env("NETEASE_API", "").rstrip("/"),
    "host": _env("HOST", "0.0.0.0"),
    "port": int(_env("PORT", "8080") or 8080),
    # 音源优先级，逗号分隔；可用 netease / gdstudio / qijieya
    "order": [s.strip() for s in _env("SOURCE_ORDER", "netease,gdstudio,qijieya").split(",") if s.strip()],
    # 出口代理（云服务器一般不需要；本机调试 Clash 时填 http://127.0.0.1:7891）
    "http_proxy": _env("HTTP_PROXY_OUT", ""),
    "up_timeout": float(_env("UPSTREAM_TIMEOUT", "8") or 8),
    "max_limit": int(_env("MAX_LIMIT", "10") or 10),
    "rate_per_min": int(_env("RATE_PER_MIN", "120") or 120),
    # 直链有效期很短（网易云 CDN 带时间戳，通常 20 分钟），缓存保守取 5 分钟
    "url_ttl": 300,
    "search_ttl": 300,
}

GD_BASE = "https://music-api.gdstudio.xyz/api.php"
QJ_BASE = "https://api.qijieya.cn/meting/"

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0 Safari/537.36")

_cache = {}
_cache_lock = threading.Lock()


def log(*a):
    print(time.strftime("[%m-%d %H:%M:%S]"), *a, flush=True)


# ---------------------------------------------------------------- 缓存

def cache_get(key):
    with _cache_lock:
        item = _cache.get(key)
        if not item:
            return None
        exp, val = item
        if exp < time.time():
            _cache.pop(key, None)
            return None
        return val


def cache_put(key, val, ttl):
    with _cache_lock:
        if len(_cache) > 512:
            now = time.time()
            for k in [k for k, (e, _) in _cache.items() if e < now]:
                _cache.pop(k, None)
            if len(_cache) > 512:
                _cache.clear()
        _cache[key] = (time.time() + ttl, val)


# ---------------------------------------------------------------- HTTP 出站

def _build_opener():
    # ★ 显式传空 dict = 不读系统代理。本机 Clash TUN / http_proxy 会劫持出站请求，
    #   表现为「服务端一切正常但拿不到数据」，云服务器上则无影响。
    if CFG["http_proxy"]:
        handler = urllib.request.ProxyHandler({"http": CFG["http_proxy"], "https": CFG["http_proxy"]})
    else:
        handler = urllib.request.ProxyHandler({})
    return urllib.request.build_opener(handler)


OPENER = _build_opener()


def http_get_raw(url, headers=None, timeout=None):
    """返回 (status, headers, body_bytes)。4xx/5xx 不抛异常，由调用方判。"""
    req = urllib.request.Request(url, headers={"User-Agent": UA, **(headers or {})})
    try:
        with OPENER.open(req, timeout=timeout or CFG["up_timeout"]) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read()
    except Exception as e:  # 超时 / DNS / TLS
        return 0, {}, str(e).encode()


def http_get_json(url, headers=None, timeout=None):
    st, _, body = http_get_raw(url, headers, timeout)
    if st != 200 or not body:
        return None, st
    try:
        return json.loads(body.decode("utf-8", "ignore")), st
    except Exception:
        return None, st


def http_post_form_json(url, form, headers=None):
    data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(url, data=data, headers={
        "User-Agent": UA,
        "Content-Type": "application/x-www-form-urlencoded",
        **(headers or {}),
    })
    try:
        with OPENER.open(req, timeout=CFG["up_timeout"]) as r:
            return json.loads(r.read().decode("utf-8", "ignore")), 200
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read().decode("utf-8", "ignore")), e.code
        except Exception:
            return None, e.code
    except Exception:
        return None, 0


# ---------------------------------------------------------------- id 打包
# 端侧只把它当不透明字符串。gdstudio 需要三个 id（取链/歌词/封面）才能拼完整，
# 又不想在服务端保存会话状态 ⇒ 把三个 id 一起塞进 id 里（stateless）。
#   "gd.<b64url(json)>"  /  "ncm.<song_id>"  /  "qj.<song_id>"

def pack_id(prefix, obj):
    raw = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode()
    return prefix + "." + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def unpack_id(sid):
    prefix, _, body = (sid or "").partition(".")
    if prefix in ("ncm", "qj"):
        return prefix, {"id": body}
    if prefix != "gd" or not body:
        return "", {}
    pad = "=" * (-len(body) % 4)
    try:
        return "gd", json.loads(base64.urlsafe_b64decode(body + pad))
    except Exception:
        return "", {}


# ---------------------------------------------------------------- 音源实现
# 统一契约：search() -> [song]；song 结构见 norm_song()
#          url(sid, br) -> {url, br, size} ；lyric(sid) -> str ；pic(sid, size) -> str

def norm_song(sid, name, artist, album=None, duration_ms=None, cover=None, source=""):
    return {
        "id": sid,
        "name": (name or "").strip(),
        "artist": (artist or "").strip(),
        "album": (album or "").strip(),
        "duration_ms": int(duration_ms or 0),
        "cover": cover or "",
        "source": source,
    }


class NeteaseBackend:
    """自建 / 公网的网易云 NodeJS API（binaryify/netease_cloud_music_api）。

    这是唯一能给出「结构化元数据 + 正确 id」的源，所以排第一。
    ★ 注意：本接口无鉴权，公网暴露必须靠本服务这层 token 挡；不要直接把 3000 端口开出去。
    """
    name = "netease"
    BR_LEVEL = {128: "standard", 192: "higher", 320: "exhigh", 999: "lossless"}

    def available(self):
        return bool(CFG["netease_api"])

    def _get(self, path):
        return http_get_json(CFG["netease_api"] + path, headers={"Referer": "https://music.163.com/"})

    def search(self, q, limit):
        url = "%s/search" % CFG["netease_api"]
        js, _ = http_post_form_json(url, {"keywords": q, "limit": limit, "type": 1, "offset": 0},
                                    headers={"Referer": "https://music.163.com/"})
        if not isinstance(js, dict) or not isinstance(js.get("result"), dict):
            return []
        out = []
        for s in (js["result"].get("songs") or [])[:limit]:
            artists = s.get("artists") or s.get("ar") or []
            album = s.get("album") or s.get("al") or {}
            out.append(norm_song(
                sid="ncm.%s" % s.get("id"),
                name=s.get("name"),
                artist="、".join(a.get("name", "") for a in artists if a.get("name")),
                album=album.get("name"),
                duration_ms=s.get("duration") or s.get("dt"),
                cover=(album.get("picUrl") or "").replace("http://", "https://"),
                source=self.name,
            ))
        return [s for s in out if s["name"] and s["id"] != "ncm.None"]

    def url(self, sid, br):
        level = self.BR_LEVEL.get(int(br), "standard")
        js, _ = self._get("/song/url/v1?id=%s&level=%s" % (sid, level))
        if not isinstance(js, dict):
            return None
        data = js.get("data") or []
        if not data or not data[0].get("url"):
            # 老版本 API 没有 v1，退一档
            js, _ = self._get("/song/url?id=%s" % sid)
            data = (js or {}).get("data") or []
            if not data or not data[0].get("url"):
                return None
        d = data[0]
        return {"url": d["url"].replace("http://", "https://"),
                "br": d.get("br") or int(br),
                "size": d.get("size") or 0}

    def lyric(self, sid):
        js, _ = self._get("/lyric?id=%s" % sid)
        if not isinstance(js, dict):
            return ""
        return (js.get("lrc") or {}).get("lyric") or ""

    def pic(self, sid, size):
        js, _ = self._get("/song/detail?ids=%s" % sid)
        try:
            return (js["songs"][0]["al"]["picUrl"] or "").replace("http://", "https://")
        except Exception:
            return ""


class GdStudioBackend:
    """GD Studio（第三方网易云解析站）。search/url/lyric/pic 一套齐活。

    2026-10-21 实测：四件套全可用，url 直接返回真 mp3 直链（不靠 302）。
    性质：第三方解析代理，随时可能关停 ⇒ 必须有备用源，且参数要能线上改。
    ★ 实测频率上限：同一 IP 连打（间隔 <1s）会返回 HTTP 200 + 空数组，单次耗时 2~7s。
      所以这里必须「失败退避重试」，且上层必须有缓存——端侧直连这家会很脆。
    """
    name = "gdstudio"

    def available(self):
        return True

    def search(self, q, limit):
        api = "%s?types=search&source=netease&name=%s&count=%d" % (GD_BASE, urllib.parse.quote(q), limit)
        js = None
        for attempt in range(3):
            js, st = http_get_json(api)
            if isinstance(js, list) and js:
                break
            log("gdstudio search 空响应（attempt=%d http=%s），退避重试" % (attempt + 1, st))
            time.sleep(0.4 * (attempt + 1))
        if not isinstance(js, list):
            return []
        out = []
        for s in js[:limit]:
            uid = str(s.get("url_id") or s.get("id") or "")
            if not uid:
                continue
            # ★ id 要短（端侧 URL 缓冲按 512 字节算，且要能塞进 MCP 参数）：
            #   网易云绝大多数歌 url_id == lyric_id == pic_id，重复的就不写。
            lid, pid = str(s.get("lyric_id") or ""), str(s.get("pic_id") or "")
            payload = {"u": uid}
            if lid and lid != uid:
                payload["l"] = lid
            if pid and pid != uid:
                payload["p"] = pid
            sid = pack_id("gd", payload)
            artists = s.get("artist") or []
            out.append(norm_song(
                sid=sid,
                name=s.get("name"),
                artist="、".join(artists) if isinstance(artists, list) else str(artists),
                album=s.get("album"),
                cover="",   # 需要二次请求 types=pic，留空让端侧按需取
                source=self.name,
            ))
        return out

    def url(self, sid, br):
        api = "%s?types=url&source=netease&br=%d&id=%s" % (GD_BASE, int(br), urllib.parse.quote(sid))
        js = None
        for attempt in range(2):
            js, _ = http_get_json(api, timeout=15)
            if isinstance(js, dict) and js.get("url"):
                break
            time.sleep(0.4 * (attempt + 1))
        if not isinstance(js, dict) or not js.get("url"):
            return None
        return {"url": js["url"], "br": js.get("br") or int(br), "size": js.get("size") or 0}

    def lyric(self, sid):
        if not sid:
            return ""
        api = "%s?types=lyric&source=netease&id=%s" % (GD_BASE, urllib.parse.quote(sid))
        for attempt in range(2):
            js, _ = http_get_json(api, timeout=15)
            lrc = (js or {}).get("lyric") if isinstance(js, dict) else None
            if lrc:
                return lrc
            time.sleep(0.3 * (attempt + 1))
        return ""

    def pic(self, sid, size):
        if not sid:
            return ""
        js, _ = http_get_json("%s?types=pic&source=netease&id=%s&size=%d"
                              % (GD_BASE, urllib.parse.quote(sid), int(size)))
        return (js or {}).get("url") or "" if isinstance(js, dict) else ""


class QijieyaBackend:
    """qijieya meting（端侧原来用的那家）。2026-10-21 本机 13s 超时，保留作末位兜底。

    meting 的 search 只按歌名返回，id 是网易云 song id；取链走 type=url&id=，
    会 302 到真 mp3（端侧播放器能跟 302，但下载必须由本服务跟随）。
    """
    name = "qijieya"

    def available(self):
        return True

    def search(self, q, limit):
        js, _ = http_get_json("%s?type=search&limit=%d&id=%s"
                              % (QJ_BASE, limit, urllib.parse.quote(q)), timeout=6)
        if not isinstance(js, list):
            return []
        out = []
        for s in js[:limit]:
            nid = s.get("id") or s.get("song_id")
            if not nid:
                continue
            out.append(norm_song(
                sid="qj.%s" % nid,
                name=s.get("name") or s.get("title"),
                artist=s.get("artist") or s.get("author") or "",
                album=s.get("album"),
                cover=(s.get("pic") or "").replace("http://", "https://"),
                source=self.name,
            ))
        return out

    def url(self, sid, br):
        js, _ = http_get_json("%s?type=url&id=%s" % (QJ_BASE, sid), timeout=8)
        if isinstance(js, list) and js:
            js = js[0]
        if not isinstance(js, dict) or not js.get("url"):
            return None
        return {"url": js["url"], "br": int(br), "size": 0}

    def lyric(self, sid):
        js, _ = http_get_json("%s?type=lrc&id=%s" % (QJ_BASE, sid), timeout=8)
        if isinstance(js, dict):
            return js.get("lyric") or ""
        return js if isinstance(js, str) else ""

    def pic(self, sid, size):
        return ""


BACKENDS = {b.name: b for b in (NeteaseBackend(), GdStudioBackend(), QijieyaBackend())}


def active_backends():
    return [BACKENDS[n] for n in CFG["order"] if n in BACKENDS and BACKENDS[n].available()]


def pick_backend(sid):
    prefix, _ = unpack_id(sid)
    if prefix == "ncm":
        return BACKENDS["netease"]
    if prefix == "qj":
        return BACKENDS["qijieya"]
    if prefix == "gd":
        return BACKENDS["gdstudio"]
    return None


def sid_payload(sid):
    """取后端真正需要的 id：netease/qijieya 就是歌 id，gdstudio 是 url_id。"""
    prefix, obj = unpack_id(sid)
    if not prefix:
        return ""
    return obj.get("id") or obj.get("u") or ""


def sid_lyric_id(sid):
    prefix, obj = unpack_id(sid)
    if prefix == "gd":
        return obj.get("l") or obj.get("u") or ""
    return obj.get("id") or ""


def sid_pic_id(sid):
    prefix, obj = unpack_id(sid)
    if prefix == "gd":
        return obj.get("p") or obj.get("u") or ""
    return obj.get("id") or ""


# ---------------------------------------------------------------- 业务聚合

def do_search(q, limit):
    ck = "s:%s:%d" % (q, limit)
    hit = cache_get(ck)
    if hit is not None:
        return hit, "cache"
    limit = max(1, min(limit, CFG["max_limit"]))
    for b in active_backends():
        try:
            songs = b.search(q, limit)
        except Exception as e:
            log("search %s via %s 异常: %r" % (q, b.name, e))
            songs = []
        if songs:
            cache_put(ck, songs, CFG["search_ttl"])
            log("search '%s' -> %d 条 via %s" % (q, len(songs), b.name))
            return songs, b.name
    return [], "none"


def do_url(sid, br, refresh=False):
    b = pick_backend(sid)
    if not b:
        return None, "bad_id"
    ck = "u:%s:%d" % (sid, br)
    if not refresh:
        hit = cache_get(ck)
        if hit is not None:
            return hit, "cache"
    try:
        info = b.url(sid_payload(sid), br)
    except Exception as e:
        log("url %s 异常: %r" % (sid, e))
        info = None
    if not info:
        return None, "no_url"
    cache_put(ck, info, CFG["url_ttl"])
    return info, b.name


# ---------------------------------------------------------------- HTTP 层

_rate = {}
_rate_lock = threading.Lock()


def rate_ok(ip):
    now = time.time()
    with _rate_lock:
        slot = _rate.get(ip)
        if not slot or now - slot[1] > 60:
            _rate[ip] = [1, now]
            return True
        if slot[0] >= CFG["rate_per_min"]:
            return False
        slot[0] += 1
        return True


class Handler(BaseHTTPRequestHandler):
    server_version = "music_proxy/" + VERSION
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # 自己打，避免默认日志刷屏

    # ---------- 工具 ----------

    def _q(self, name, default=""):
        vals = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query).get(name)
        return (vals[0] if vals else default)

    def _auth(self):
        tok = self.headers.get("X-Music-Token") or self._q("token")
        return bool(CFG["token"]) and tok == CFG["token"]

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def _err(self, msg, code=400, **extra):
        self._json({"code": code, "msg": msg, **extra}, code if code >= 400 else 400)

    # ---------- 路由 ----------

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path.rstrip("/") or "/"
        ip = self.client_address[0]

        if path in ("/healthz", "/"):
            return self._json({"code": 0, "data": {"service": "music_proxy", "version": VERSION,
                                                   "sources": [b.name for b in active_backends()]}})

        if not rate_ok(ip):
            return self._err("rate limited", 429)
        if not CFG["token"]:
            return self._err("server 未配置 MUSIC_TOKEN，拒绝服务", 503)
        if not self._auth():
            return self._err("unauthorized", 401)

        try:
            if path == "/v1/search":
                q = self._q("q").strip()
                if not q:
                    return self._err("missing q")
                limit = int(self._q("limit", "5") or 5)
                songs, src = do_search(q, limit)
                if not songs:
                    return self._json({"code": 404, "msg": "no result", "data": []}, 200)
                return self._json({"code": 0, "data": songs, "source": src})

            if path == "/v1/url":
                sid = self._q("id")
                br = int(self._q("br", "128") or 128)
                if not sid:
                    return self._err("missing id")
                info, src = do_url(sid, br)
                if not info:
                    return self._json({"code": 404, "msg": "no playable url", "source": src}, 200)
                return self._json({"code": 0, "data": info, "source": src})

            if path == "/v1/lyric":
                sid = self._q("id")
                b = pick_backend(sid)
                if not b:
                    return self._err("bad id")
                lrc = b.lyric(sid_lyric_id(sid)) or ""
                return self._json({"code": 0, "data": {"lrc": lrc}})

            if path == "/v1/pic":
                sid = self._q("id")
                b = pick_backend(sid)
                if not b:
                    return self._err("bad id")
                url = b.pic(sid_pic_id(sid), int(self._q("size", "300") or 300)) or ""
                return self._json({"code": 0, "data": {"url": url}})

            if path == "/v1/download":
                return self._download()

            if path == "/v1/sources":
                return self._sources()

            return self._err("unknown path: %s" % path, 404)
        except Exception as e:
            log("处理 %s 异常: %r" % (path, e))
            return self._err("internal: %r" % e, 500)

    # ---------- 下载（流式代理，不做落盘） ----------

    def _download(self):
        """流式转发直链（不落盘）。设备侧拿到的是「长度确定、无 302、可 Range」的音源。

        ★ 网易云 CDN 直链带时间戳，会过期 / 偶发挂起 ⇒ 第一次失败必须
          「丢掉缓存重新取链」再试一次，否则设备表现为「下载一半断」。
        """
        sid = self._q("id")
        br = int(self._q("br", "128") or 128)
        rng = self.headers.get("Range")
        up, info, src = None, None, ""
        for attempt in (0, 1):
            info, src = do_url(sid, br, refresh=bool(attempt))
            if not info:
                continue
            fwd = {"User-Agent": UA, "Referer": "https://music.163.com/"}
            if rng:
                fwd["Range"] = rng
            try:
                up = OPENER.open(urllib.request.Request(info["url"], headers=fwd), timeout=15)
            except Exception as e:
                log("download 打开直链失败(attempt=%d): %r" % (attempt, e))
                up = None
                continue
            if up.status in (403, 404, 410) and attempt == 0:
                log("download 直链已过期 HTTP %s，重新取链" % up.status)
                up.close()
                up = None
                continue
            break
        if up is None:
            return self._err("upstream open failed", 502)

        self.send_response(up.status)
        for h in ("Content-Type", "Content-Length", "Content-Range", "Accept-Ranges"):
            v = up.headers.get(h)
            if v:
                self.send_header(h, v)
        if not up.headers.get("Content-Type"):
            self.send_header("Content-Type", "audio/mpeg")
        self.send_header("X-Source", src)
        self.end_headers()
        sent = 0
        try:
            while True:
                chunk = up.read(16384)
                if not chunk:
                    break
                self.wfile.write(chunk)
                sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            log("download 客户端中断（已发 %d B）" % sent)
        finally:
            up.close()
        log("download %s br=%d -> %d B (%s)" % (sid[:24], br, sent, src))

    # ---------- 音源体检 ----------

    def _sources(self):
        # ★ 体检本身会打音源，高频调用会把自己打成被限流 ⇒ 结果缓存 60s。
        #   ok  = 「这家能搜到歌」；url = 取链结果（某首歌取不到链不等于源挂了）
        #   默认只测搜索（快，设备/监控可周期调）；?deep=1 才连取链一起测（慢，部署验收用）
        deep = self._q("deep") == "1"
        hit = cache_get("sources:deep" if deep else "sources")
        if hit is not None:
            return self._json({"code": 0, "data": hit, "cached": True})
        queries = ("起风了", "稻香") if deep else ("起风了",)
        res = []
        for b in active_backends():
            t0 = time.time()
            search_ok, url_state, detail = False, "skip", ""
            # 第三方站会「单次查询空响应」但换首歌就好 ⇒ 多个查询词各试一次再判死
            for q in queries:
                try:
                    songs = b.search(q, 2)
                except Exception as e:
                    songs, detail = [], repr(e)[:80]
                if songs:
                    search_ok = True
                    if deep:
                        url_state = "ok" if b.url(sid_payload(songs[0]["id"]), 128) else "fail"
                    break
                detail = detail or "search_empty"
            res.append({"name": b.name, "ok": search_ok, "url": url_state,
                        "detail": detail, "ms": int((time.time() - t0) * 1000)})
        cache_put("sources:deep" if deep else "sources", res, 60)
        return self._json({"code": 0, "data": res})


def main():
    if not CFG["token"]:
        log("!! 未设置 MUSIC_TOKEN —— 服务会拒绝所有业务请求（/healthz 除外）")
        log("!! 启动示例: MUSIC_TOKEN=$(openssl rand -hex 16) python3 music_proxy.py")
    if not CFG["netease_api"]:
        log("提示: 未设置 NETEASE_API，网易云直连源不启用（只剩第三方解析站）")
    socket.setdefaulttimeout(CFG["up_timeout"])
    log("music_proxy %s 监听 %s:%d，音源顺序=%s"
        % (VERSION, CFG["host"], CFG["port"], ",".join(b.name for b in active_backends())))
    ThreadingHTTPServer((CFG["host"], CFG["port"]), Handler).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
