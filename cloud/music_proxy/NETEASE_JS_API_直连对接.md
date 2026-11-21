# 网易云 NodeJS API：云上部署 + 小智端侧直连对接

> 这份文档解决的是**你原本问的那件事**：把 `binaryify/netease_cloud_music_api` 部署到你的云服务器，
> 然后端侧 `ai_music` 直接调它。
>
> 和同目录 `README.md`（`music_proxy`）的关系：
> - **本文 = 只用网易云 JS API，端侧直连**：元件最少（1 个容器），但端侧解析变复杂。
> - `README.md` = 在 JS API 前面加一层我写的代理：端侧只认 4 个 GET，换源不用重烧固件。
>
> 两者不冲突：代理就是把 JS API 当上游。**先按本文把 JS API 跑起来，代理要不要加后面再定。**

---

## 1. 部署（5 分钟）

### 1.1 只监听本机，不裸奔

```bash
docker run -d --name netease_api \
  --restart unless-stopped \
  -p 127.0.0.1:3000:3000 \
  binaryify/netease_cloud_music_api:latest

# 验活
curl -s "http://127.0.0.1:3000/search?keywords=%E8%B5%B7%E9%A3%8E%E4%BA%86&limit=1" | head -c 300
```

★ **`-p 127.0.0.1:3000:3000` 里的 `127.0.0.1` 不能省**。这个 API **没有任何鉴权**，
`-p 3000:3000` 等于把全网音乐直链白送出去，第二天就会被刷。

### 1.2 用 Nginx 加 token 再暴露出去

```nginx
server {
    listen 443 ssl http2;
    server_name music.example.com;

    ssl_certificate     /etc/letsencrypt/live/music.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/music.example.com/privkey.pem;

    # 设备必须带这个头（或改成 ?token= 走 query）
    location / {
        if ($http_x_music_token != "换成你的随机串") { return 401; }

        proxy_pass http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_buffering off;          # 音频流别缓冲
        proxy_read_timeout 120s;
    }
}
```

★ **证书必须是公网 CA 签的**（Let's Encrypt 就行）。端侧 `esp_http_client` 用 `crt_bundle_attach` 校验证书，
自签证书会直接 `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` —— 这是 §73.27 踩过的坑（忘写 `crt_bundle_attach`
同样报这个错）。

如果只在局域网用（设备与服务器同网段），可以走 `http://192.168.x.x:3000`，省掉 HTTPS，
但 `-p 127.0.0.1` 要改成 `-p 192.168.x.x:3000`。

---

## 2. 端侧要用的 4 个接口（含真实字段）

请求形态取自 `astrbot_plugin_music/core/platform/ncm_nodejs.py`（该插件调的就是这套 API，可直接对照）。

### 2.1 搜索 —— `POST /search`（表单）

```
POST /search
Content-Type: application/x-www-form-urlencoded

keywords=起风了&limit=5&type=1&offset=0
```

```json
{"result":{"songs":[{
    "id": 1330348068,
    "name": "起风了",
    "artists": [{"id": 12085, "name": "冯沁苑(买辣椒也用券)"}],
    "album": {"id": 75113681, "name": "起风了", "picUrl": "http://p1.music.126.net/xxx.jpg"},
    "duration": 325000
  }], "songCount": 100}, "code": 200}
```

设备要取的东西（就这几个，别全解）：

| 用途 | 路径 |
|---|---|
| 歌 id（后面取链用） | `result.songs[0].id` |
| 歌名（落盘文件名） | `result.songs[0].name` |
| 歌手 | `result.songs[0].artists[0].name`（多个用 `、` 连） |
| 时长(ms) | `result.songs[0].duration` |
| 封面 | `result.songs[0].album.picUrl`（`http://` 要换成 `https://`） |

★ **`GET /search?keywords=...&limit=5` 也能用**，返回结构相同，端侧实现更省事（不用发 POST）。
如果只是想最快跑通，先用 GET。

### 2.2 取播放直链 —— `GET /song/url/v1?id=<id>&level=standard`

```json
{"data":[{"id":1330348068,"url":"http://m801.music.126.net/.../xxx.mp3",
          "br":128000,"size":4464684,"code":200,"fee":0,"level":"standard"}],"code":200}
```

```json
{"data":[{"id":123,"url":null,"code":404,"fee":1}],"code":200}
```

- `level`：`standard`(128k) / `higher`(192k) / `exhigh`(320k) / `lossless`。**不带 cookie 时只有 `standard` 稳定可用**。
- 老接口 `GET /song/url?id=<id>` 也还在，返回结构一样（插件用的就是这个），可以作为退路。
- ★ **`url: null` 是常态**（VIP / 无版权 / 地区限制）。必须判空——端侧现在的 `music_find_url` 是「找到 http 串就用」，
  直连 netease 后如果还这么写，会把 `album.picUrl` 当成音频播出去。
- `url` 是 **`http://`**，不是 https。端侧 `esp_http_client` 用 http 没问题，但**别把
  `crt_bundle_attach` 的 https 配置套上去当协议判断**；同时要注意 `m801.music.126.net` 会 302，
  播放器（`esp_audio_simple_player` / `esp_gmf_io_http`）会自己跟随，**下载要自己跟**（`esp_http_client` 默认跟随）。

### 2.3 歌词 —— `GET /lyric?id=<id>`

```json
{"lrc":{"version":1,"lyric":"[00:00.000] 作词 : 米果\n[00:30.542]这一路上走走停停\n..."},
 "tlyric":{"version":1,"lyric":"..."},"code":200}
```

只取 `lrc.lyric`。

### 2.4 封面 —— `GET /song/detail?ids=<id>`

```json
{"songs":[{"id":1330348068,"al":{"picUrl":"http://p1.music.126.net/xxx.jpg"},
           "ar":[{"name":"冯沁苑(买辣椒也用券)"}],"dt":325000}],"code":200}
```

取 `songs[0].al.picUrl`。**其实搜索响应里已经有 `album.picUrl` 了，正常不用单独调这个。**

> 附带：热评 `POST /comment/hot`（`id` + `type=0`）。端侧用不上。

---

## 3. 端侧要改什么（`main/middleware/ai/ai_music.c`）

| # | 改动 | 为什么 |
|---|---|---|
| 1 | 搜索从「GET 拼歌名到解析站」改成「GET/POST 到 `/search`」 | 域名换成一个固定 base，不再有多端点轮询 |
| 2 | **响应体缓冲从 2048 提到 16 KB（PSRAM）** | ★ netease 的 search 响应**5 首约 8~15 KB**（每首几十个字段），现在的 2KB 装不下，`cJSON_Parse` 会直接失败。歌词（含翻译）也有 5~10 KB |
| 3 | 解析从「递归找 http 串」改成「取固定字段」 | 见 §2.1 表；顺带消灭「把封面当音频」这类误判 |
| 4 | 加一次 `/song/url/v1` 调用，判 `url == null` | VIP/无版权必然 null |
| 5 | 直链是 `http://` | 该请求不要带 `crt_bundle_attach`；`/search` 那步仍是 https 要带 |
| 6 | 下载到 SD 时用 `esp_http_client` 流式写 `.part`，用 `Content-Length` 校验 | 见下 |

内存口径（本工程铁律）：`/search` 的 16 KB body 走 PSRAM；下载缓冲 8~16 KB **必须内部 RAM**
（DMA 要求），而内部 RAM 现在只剩 ~4 KB —— 这就是为什么下载这块要先腾内存。

---

## 4. 三个坑（都是实测过的）

1. **公网镜像全灭**（2026-10-21 实测）：`163api.qijieya.cn` → 502、`zm.armoe.cn` → 403「仅限 ZMusic 使用」、
   `music-api.focalors.ltd` → DNS 不存在、`wyy.xhily.com` → TLS 握手失败。所以**必须自建**，别想着找个免费域名顶上。
2. **没鉴权 = 会被刷**：这个 API 不校验身份，公网暴露前必须有 Nginx token（§1.2），
   或者干脆只在内网/内网穿透里用。
3. **版权是硬墙**：周杰伦、五月天这类网易云没版权的歌，`/song/url` 直接 `url: null`。
   技术方案解决不了，只能换源或换平台（QQ 音乐那套要 uin + cookie 登录态，端侧不好搞）。

---

## 5. 两条路怎么选

| | 直连 JS API（本文） | 加 music_proxy（README.md） |
|---|---|---|
| 元件数 | 1 个容器 | 2 个容器（多一层 Python，无依赖） |
| 端侧改动 | 搜索+解析+取链+判空+下载，全在固件里 | 只要 4 个 GET + 结构固定的 JSON |
| 换源/加减镜像 | **要改固件重烧** | 改 `.env` 重启容器 |
| 端侧响应体 | 8~15 KB（要抬 buffer） | 每次 ≤ 3 KB（够小，现有 2KB 稍微抬一点即可） |
| 直链过期 | 端侧自己重取 | 代理自动丢缓存重取 |
| 流量 | 设备直连 netease CDN | 音频过一遍云服务器带宽 |

**建议顺序**：先按本文把 JS API 跑起来（这一步两边都要做，不白干），
然后看你要不要那层代理——如果只是一台设备自己玩，直连完全够；如果以后要点歌列表/多源/换源，代理更省事。
