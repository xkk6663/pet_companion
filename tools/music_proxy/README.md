# music_proxy — 小智点歌「云侧音源代理」

> 单文件、零第三方依赖、`python3 music_proxy.py` 就能跑。
> 端侧 `ai_music` 只认 4 个 GET，换音源 = 改服务端，不用重烧固件。

---

## 1. 一句话说清楚它是干什么的

现在端侧点歌（`main/middleware/ai/ai_music.c`）是自己直连解析站、递归扫 JSON 找「像音频的 http 串」。
问题是：**端点会死、只有一条 url、没有任何元数据**（歌名/歌手/时长/歌词/封面全没有），所以既没法给 SD 卡上的文件起好名，也没法做「已下载列表」。

这一层把「找源」搬到云上，给端侧一个**稳定契约**：

```
端侧 ai_music ──4 个 GET──> music_proxy ──> ① 自建网易云 NodeJS API（首选）
                                          ├─> ② GD Studio（第三方解析站）
                                          └─> ③ qijieya meting（末位兜底）
```

---

## 2. 部署前必读：三条实测事实（2026-10-21，PC 直连实测）

| 事实 | 证据 | 结论 |
|---|---|---|
| **公网免费的网易云 API 镜像基本全灭** | `163api.qijieya.cn` → 502；`zm.armoe.cn` → 403「仅限 ZMusic 使用」；`music-api.focalors.ltd` → DNS 不存在；`wyy.xhily.com` → TLS handshake 失败 | 那 8 个镜像别指望，**必须自建**——你有云服务器，正好 |
| **qijieya 的搜索已经空了** | `GET api.qijieya.cn/meting/?type=search&limit=2&id=起风了` → HTTP 200 但 body 是 `[]`（0.27s）；另一次直接 13s 超时 | 端侧现在把它排第一位，**每次点歌都白等一轮**。接了这个代理后应该把端侧那家降级/删掉 |
| **GD Studio 能用，但会限流和抖动** | search/url/lyric/pic 四件套全通；同一 IP 连打（间隔 <1s）会返回 `HTTP 200 + []`，单次耗时 2~13s 波动 | 所以本服务内置「退避重试 + 结果缓存 + 双查询词判活」；端侧直连这家会很脆 |

**所以正确的分工是**：自建网易云 API 做主力（结构化元数据 + 稳定 id），第三方站只做应急兜底。

---

## 3. 部署 A：Docker Compose（推荐）

```bash
# 把整个 tools/music_proxy 目录传到云服务器
scp -r music_proxy/ root@<你的服务器>:/opt/

cd /opt/music_proxy
cp .env.example .env

# 1) 生成设备要用的 token
sed -i "s|^MUSIC_TOKEN=.*|MUSIC_TOKEN=$(openssl rand -hex 16)|" .env

# 2) 起服务（只起代理，不启用自建网易云 API）
docker compose up -d music-proxy

# 或者：连自建网易云 API 一起起
#    compose 里那个 netease-api 是可选服务，只要被 depends_on 引用就会一起起，
#    所以想用自建 API 就直接下面这条，然后把 .env 的 NETEASE_API 设成 http://netease-api:3000
docker compose up -d
sed -i "s|^NETEASE_API=.*|NETEASE_API=http://netease-api:3000|" .env
docker compose up -d --force-recreate music-proxy
```

校验：

```bash
curl -s http://127.0.0.1:8080/healthz
# {"code":0,"data":{"service":"music_proxy","version":"1.0.0","sources":["netease","gdstudio","qijieya"]}}
```

> `sources` 里出现 `netease` 说明自建 API 已被识别（启动日志也会打「音源顺序=...」）。

---

## 4. 部署 B：裸 Python + systemd（不想装 Docker 的话）

要求 **Python ≥ 3.9，无任何 pip 依赖**（只用标准库）。

```bash
sudo mkdir -p /opt/music_proxy
sudo cp music_proxy.py probe.py .env /opt/music_proxy/
sudo cp music-proxy.service /etc/systemd/system/

sudo useradd -r -s /usr/sbin/nologin music || true
sudo chown -R music:music /opt/music_proxy

sudo systemctl daemon-reload
sudo systemctl enable --now music-proxy
systemctl status music-proxy --no-pager
journalctl -u music-proxy -f        # 看日志：搜索命中哪个源、下载了多少字节
```

---

## 5. HTTPS 反代（设备端必须走 HTTPS 或内网 HTTP）

设备端 `esp_http_client` 用的是证书 bundle 校验（`crt_bundle_attach`）。
**用公网 CA 签的证书就没问题；自签证书设备会直接拒连**（`ESP_ERR_MBEDTLS_SSL_SETUP_FAILED`）。

### Nginx

```nginx
server {
    listen 443 ssl http2;
    server_name music.example.com;

    ssl_certificate     /etc/letsencrypt/live/music.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/music.example.com/privkey.pem;

    # 下载是音频流，别让反代缓冲
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_buffering off;          # ★ 必须：否则设备拿到的 Content-Length 会先被缓冲
        proxy_read_timeout 120s;
        client_max_body_size 1m;
    }
}
```

### Caddy（更省事，自动证书）

```
music.example.com {
    reverse_proxy 127.0.0.1:8080 {
        flush_interval -1
    }
}
```

---

## 6. 接口文档（设备侧契约）

**所有响应都是 `{"code":0,...}`；`code` 非 0 表示业务错误（HTTP 状态码同时给出）。**
**鉴权**：Header `X-Music-Token: <token>`，或 query `?token=<token>`（播放器只吃一个 URL 时用 query 更方便）。
`/healthz` 免鉴权，其余全部需要。

### 6.1 `GET /v1/search?q=<歌名>&limit=5`

```json
{
  "code": 0,
  "source": "netease",
  "data": [
    {
      "id": "ncm.186016",
      "name": "起风了",
      "artist": "冯沁苑(买辣椒也用券)",
      "album": "起风了",
      "duration_ms": 325000,
      "cover": "https://p1.music.126.net/xxx.jpg",
      "source": "netease"
    }
  ]
}
```

- `id` 是**不透明字符串**，端侧不要解析，原样回传即可。前缀只是内部标记：
  `ncm.` = 自建网易云 API、`gd.` = GD Studio、`qj.` = qijieya。
- 搜不到时：`HTTP 200` + `{"code":404,"msg":"no result","data":[]}`（故意不用 4xx，端侧少一个分支）。
- `cover` 可能为空（gd 源不返回封面，需要 `/v1/pic` 现取）。

### 6.2 `GET /v1/url?id=<id>&br=128|192|320|999`

```json
{"code":0,"source":"netease","data":{"url":"https://m801.music.126.net/...mp3","br":128,"size":4464684}}
```

- `br`：128=标准、320=较高（`exhigh`）、999=无损。默认 128。
- 直链**带时间戳会过期**（通常 20 分钟），服务端只缓存 5 分钟；过期会自动重新取。

### 6.3 `GET /v1/lyric?id=<id>`

```json
{"code":0,"data":{"lrc":"[00:00.000] 作词 : 米果\n[00:30.542]这一路上走走停停\n..."}}
```

无歌词时返回空串（不是错误）。

### 6.4 `GET /v1/pic?id=<id>&size=300`

```json
{"code":0,"data":{"url":"https://p2.music.126.net/xxx.jpg?param=300y300"}}
```

### 6.5 `GET /v1/download?id=<id>&br=128` ★ 设备下载走这个

直接回音频字节流（不是 JSON）：

- 响应头透传 `Content-Type` / `Content-Length` / `Accept-Ranges` / `Content-Range`，额外带 `X-Source`。
- 支持 `Range:` 请求头（可断点续传，返回 206）。
- **直链过期或挂起时会自动「丢缓存重新取链」再试一次** —— 这是设备侧最需要的保证：拿到的流长度确定、无 302、不会因为 CDN 换链而断。
- 上游彻底失败 → `HTTP 502`。

`Content-Length` 对下载极重要：端侧可用它做「剩余空间预检」和「写盘大小校验」。

### 6.6 `GET /v1/sources[?deep=1]`

```json
{"code":0,"data":[
  {"name":"netease","ok":true,"url":"skip","detail":"","ms":42},
  {"name":"gdstudio","ok":true,"url":"ok","detail":"","ms":6261},
  {"name":"qijieya","ok":false,"url":"skip","detail":"search_empty","ms":1000}
]}
```

- 默认只测「能不能搜到」（快，可给监控周期调）；`?deep=1` 连取链一起测（慢，部署验收用）。
- 结果缓存 60s —— 体检自己也打音源，别让健康检查把自己打成被限流。

### 6.7 错误码

| HTTP | body.code | 含义 |
|---|---|---|
| 401 | 401 | token 缺失/错误 |
| 429 | 429 | 该 IP 超过 `RATE_PER_MIN` |
| 400 | 400 | 缺参数（如 `q`） |
| 404 | 404 | 路径不存在 / 搜不到 / 取不到可播链 |
| 502 | 502 | 上游直链打不开（已自动重试过一次） |
| 503 | 503 | 服务端没配 `MUSIC_TOKEN`（拒绝裸奔） |

---

## 7. 部署后自检（必做）

```bash
python3 probe.py http://127.0.0.1:8080 <你的MUSIC_TOKEN> --song 起风了
```

它按**设备真实调用顺序**走一遍：`healthz → 鉴权必须拒绝 → sources → search → url → download 前 64KB → download Range → lyric`。
本机实测输出（只有第三方站、无自建 API 时的样子）：

```
  [PASS] healthz                    HTTP 200 113ms
  [PASS] auth 必须拒绝               HTTP 401
  [PASS] sources                    可用=['gdstudio'] | gdstudio(搜:ok 链:ok 6261ms), qijieya(搜:fail 链:skip 1000ms)
  [PASS] search                     HTTP 200 12438ms 首条=起风了 - 冯沁苑(买辣椒也用券) [gdstudio]
  [PASS] url(128k)                  HTTP 200 5558ms size=4.97MB br=128
  [PASS] download 前 64KB            HTTP 200 189ms got=65536B ctype=audio/mpeg clen=5214920
  [PASS] download Range             HTTP 206 got=1024B crange=bytes 0-1023/5214920
  [PASS] lyric                      HTTP 200 1650ms 1677B 首行='[00:00.000] 作词 : 米果'
结果: 全部 PASS
```

> ⚠ 上面那些 **6~12 秒**是「只有第三方解析站」的耗时，且我当时在反复打它（限流）。
> 自建网易云 API 起来后，`search/url` 应在 **200ms 量级**——这是接自建 API 最大的收益。

`[WARN] lyric` 不算失败：歌词缺失不阻塞播放和下载。

---

## 8. 设备侧怎么接（下一步改 `ai_music.c` 的要点）

### 8.1 建议的工具划分

| MCP 工具 | 服务端调用 | 说明 |
|---|---|---|
| `self.music.play(name)` | `/v1/search` → 取第 1 条 → 播 `download` URL | 会话内 `audio_playback_play` |
| `self.music.download(name)` | `/v1/search` → `/v1/lyric` → `/v1/download` 流式写 `/sdcard/music/` | 落盘 |
| `self.music.list` | 纯本地（扫 `/sdcard/music`） | 「你都下了什么歌」 |
| `self.music.stop` | 无 | 现有实现不变 |

### 8.2 一个关键简化：**播放器可以直接吃 `/v1/download` 的 URL**

```
https://music.example.com/v1/download?id=<id>&br=128&token=<token>
```

好处：
1. 端侧不用自己解析 302、不用管直链过期（代理已经处理了）；
2. URL 长度可控（`base ≈ 40` + `id ≤ 100` + token ≈ 32 ⇒ **< 200 字节**，远小于 `MUSIC_URL_MAX 512`）；
3. 播放和下载走同一个入口，行为一致。

代价：音频要过一遍云服务器带宽（128k 约 16 KB/s，一首 5MB 的歌 ≈ 5MB 流量）。

### 8.3 落盘流程（`/sdcard/music/`）

```
1. GET /v1/search?q=            → 拿到 name / artist / id
2. GET /v1/lyric?id=            → 可选，失败不阻塞
3. 文件名 = "<artist>-<name>.mp3"（去掉 / \ : * ? " < > | 和控制字符，截断到 ~80 字节）
4. f_getfree 预检：剩余 < Content-Length + 1MB 就拒绝并回话
5. GET /v1/download?id=&br=128  → 流式 fwrite 到 "<name>.mp3.part"
6. 收尾：bytes_written == Content-Length 才 rename 成 .mp3（否则删 .part）
```

★ `.part` + rename 是必须的：SD 上留下半个 mp3 会让 `esp_player` 解到一半报错，而且用户分不清是文件坏了还是卡坏了。

### 8.4 内存红线（本工程特有）

- 内部 RAM 余量只有 ~4KB（上电实测 `largest=55,296`，WiFi 那笔内部 DMA 51,200 搬不走）。
- 下载任务**栈放 PSRAM**（纯网络 + SD 写，不碰 flash，符合 F35）；
- **DMA 缓冲（8~16KB）必须内部 RAM** —— 这是硬缺口，落地前要么先缩 `.bss`，要么把缓冲压到 4KB。
- 别在 UDP/MCP 回调里做下载：`cb_*` 跑在 MCP 引擎任务栈上，必须另起任务（照抄 `music_kick_task`）。

---

## 9. 运维

| 场景 | 做法 |
|---|---|
| 某个源挂了 | 改 `.env` 的 `SOURCE_ORDER`（比如去掉 `qijieya`），`docker compose up -d --force-recreate music-proxy` |
| 想加新源 | 在 `music_proxy.py` 里加一个 `class XxxBackend`（实现 `search/url/lyric/pic` 四个方法 + `available()`），加进 `BACKENDS`，改 `SOURCE_ORDER`；`pick_backend()` 里加前缀映射 |
| 看请求是否正常 | `docker logs -f music_proxy`：会打「search '起风了' -> 3 条 via netease」「download ncm.186016 br=128 -> 5214920 B」 |
| 被刷流量 | 缩 `RATE_PER_MIN`；token 换掉；必要时在 Nginx 加 `limit_req` |
| 排查「设备连不上」 | 先 `curl https://你的域名/healthz`；再确认证书是公网 CA（自签设备拒连）；再确认 token |

---

## 10. 版权与合规（说在前面）

- 自建网易云 API 只解决了「接口稳定」，**不解决版权**：VIP / 无版权曲目拿不到音频（`/song/url` 返回 `url: null`，本服务会判为 `no_url` 并继续下一首/报 404）。
- 第三方解析站（GD Studio 等）是绕过付费墙的解析代理，属灰色地带，随时可能关停——这也是本服务做多源回退的原因。
- **自己学习、自己设备用没问题；不要做成对外提供的点歌服务，也不要批量下载整库**。真要做产品，音源必须换正版授权。

---

## 11. 文件清单

| 文件 | 用途 |
|---|---|
| `music_proxy.py` | 服务本体（单文件，标准库） |
| `probe.py` | 部署后真值自检 |
| `Dockerfile` / `docker-compose.yml` | 容器部署（compose 里带可选的自建网易云 API） |
| `music-proxy.service` | systemd 部署 |
| `.env.example` | 配置模板 |
