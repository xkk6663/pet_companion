"""云端网关（tools/diag/ws_gateway.py）的脱机单测。

被测对象是**线上那份代码本身**：不复制实现、不加测试专用分支。
覆盖的都是纯函数 / 单次协程，不发真实网络请求。
"""
from __future__ import annotations

import asyncio
import io
import json
import struct
import wave

import pytest
from PIL import Image

import ws_gateway as gw


# ---------------------------------------------------------------- PCM / WAV
def test_pcm16_to_wav_header_and_frames():
    pcm = b"\x00\x01" * 320                    # 20 ms @16k = 320 帧 = 640 B
    raw = gw.pcm16_to_wav(pcm, 16000)
    with wave.open(io.BytesIO(raw), "rb") as w:
        assert w.getnchannels() == 1
        assert w.getsampwidth() == 2
        assert w.getframerate() == 16000
        assert w.getnframes() == 320
        assert w.readframes(320) == pcm
    assert raw[:4] == b"RIFF"
    assert raw[8:12] == b"WAVE"
    assert len(raw) == 44 + len(pcm)


def _riff(payload: bytes, data_size_field: int = 0xFFFFFFFF) -> bytes:
    fmt = b"fmt " + struct.pack("<I", 16) + struct.pack(
        "<HHIIHH", 1, 1, 16000, 32000, 2, 16)
    data = b"data" + struct.pack("<I", data_size_field) + payload
    body = b"WAVE" + fmt + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def test_normalize_wav_rewrites_placeholders_and_drops_ff_markers():
    payload = b"\x01\x02" * 118 + b"\xff\xff" + b"\x03\x04"
    out = gw.normalize_wav(_riff(payload))
    assert out[:4] == b"RIFF" and out[8:12] == b"WAVE"
    assert struct.unpack("<I", out[4:8])[0] == 36 + 238
    assert out[36:40] == b"data"
    assert struct.unpack("<I", out[40:44])[0] == 238
    assert b"\xff" not in out


def test_normalize_wav_pads_odd_data():
    payload = b"\x01\x02" * 118 + b"\xff\xff" + b"\x03"       # 239 -> 去掉 FF 对 -> 237 -> 补 1
    out = gw.normalize_wav(_riff(payload))
    n = struct.unpack("<I", out[40:44])[0]
    assert n % 2 == 0 and n == 238


def test_normalize_wav_passthrough_when_not_wav():
    raw = b"not-a-wav"
    assert gw.normalize_wav(raw) == raw


def test_normalize_wav_passthrough_when_data_too_short():
    raw = _riff(b"\x01\x02" * 10)              # 10 样本 < 100
    assert gw.normalize_wav(raw) == raw


def test_normalize_wav_passthrough_without_data_chunk():
    fmt = b"fmt " + struct.pack("<I", 16) + struct.pack(
        "<HHIIHH", 1, 1, 16000, 32000, 2, 16)
    raw = b"RIFF" + struct.pack("<I", 4 + len(fmt)) + b"WAVE" + fmt
    assert gw.normalize_wav(raw) == raw


# ---------------------------------------------------------------- 文本闸
def test_tts_shorten_keeps_short_text():
    assert gw.tts_shorten("  你好呀  ") == "你好呀"
    assert gw.tts_shorten("") == ""


def test_tts_shorten_cuts_at_sentence_boundary():
    text = "甲" * 150 + "。" + "乙" * 100
    out = gw.tts_shorten(text)
    assert out == "甲" * 150 + "。"
    assert len(out) <= gw.TTS_MAX_CHARS


def test_tts_shorten_hard_cut_without_boundary():
    text = "a" * 260
    assert len(gw.tts_shorten(text)) == gw.TTS_MAX_CHARS


def test_tts_shorten_ignores_early_boundary():
    text = "短。" + "甲" * 250                   # 句号在 200 字以内但 < 100 位
    assert len(gw.tts_shorten(text)) == gw.TTS_MAX_CHARS


def test_clean_reply_strips_mode_markers():
    assert gw.clean_reply_text("⟦MODE:chat⟧你好") == "你好"
    assert gw.clean_reply_text("[MODE:work] 回复") == "回复"
    assert gw.clean_reply_text("【MODE:chat】在呢") == "在呢"


def test_clean_reply_collapses_duplicated_halves():
    half = "今天天气真好"
    assert gw.clean_reply_text(half + half) == half


def test_clean_reply_collapses_repeated_tail():
    tail = "tail-repeat-"                       # 12 字
    assert gw.clean_reply_text("lead" + tail + tail) == "lead" + tail


def test_clean_reply_leaves_normal_text_alone():
    text = "行，放《晴天》。"
    assert gw.clean_reply_text(text) == text


# ---------------------------------------------------------------- 路径安全
@pytest.mark.parametrize("sid", ["../../etc/passwd", "a/b\\c", "设备-1", "a b"])
def test_safe_name_neutralizes_path_chars(sid):
    out = gw.safe_name(sid)
    assert "/" not in out and "\\" not in out
    assert set(out) <= set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")


def test_safe_name_empty_and_suffix():
    assert gw.safe_name("") == "dev"
    assert gw.safe_name("xz-s31-fc50", "_1.jpg") == "xz-s31-fc50_1.jpg"


# ---------------------------------------------------------------- 意图
@pytest.fixture
def caps_session():
    sid = "test-s1"
    gw.SESSIONS[sid] = {"caps": {"tools": [
        {"name": "punch", "aliases": "打卡|签到|打个卡"},
        {"name": "music", "aliases": "放首歌|放首|播放",
         "arg_mode": "tail", "arg_key": "name"},
        {"name": "volume", "aliases": "大点声|小声点",
         "arg_mode": "tail", "arg_key": "volume"},
    ]}}
    yield sid
    gw.SESSIONS.pop(sid, None)


def test_intent_exact_alias_from_device_caps(caps_session):
    assert gw.match_intent("签到", caps_session) == {"name": "punch", "args": {}}


def test_intent_longest_alias_wins(caps_session):
    got = gw.match_intent("放首歌晴天", caps_session)
    assert got == {"name": "music",
                   "args": {"__text": "放首歌晴天", "name": "晴天"}}


def test_intent_tail_strips_book_marks(caps_session):
    assert gw.match_intent("播放《晴天》", caps_session)["args"]["name"] == "晴天"


def test_intent_tail_without_arg_keeps_original_text(caps_session):
    got = gw.match_intent("大点声", caps_session)
    assert got == {"name": "volume", "args": {"__text": "大点声"}}


def test_intent_fallback_aliases_without_caps():
    assert gw.match_intent("上班打卡", "no-such") == {"name": "punch", "args": {}}


@pytest.mark.parametrize("text,song", [
    ("放首晴天", "晴天"),
    ("点一首晴天", "晴天"),
    ("听《晴天》", "晴天"),
])
def test_intent_song_fallback(text, song):
    got = gw.match_intent(text, "no-such")
    assert got is not None and got["name"] == "music"
    assert got["args"]["name"] == song


def test_intent_song_without_name():
    assert gw.match_intent("点歌", "no-such") == {"name": "music", "args": {}}


@pytest.mark.parametrize("text", ["帮我打卡", "请帮我打卡", "我要打个卡"])
def test_intent_strips_filler(text):
    assert gw.match_intent(text, "no-such") == {"name": "punch", "args": {}}


@pytest.mark.parametrize("text", ["今天几号", "打卡了吗", "", "。。。", "随便说说"])
def test_intent_does_not_misfire(text):
    assert gw.match_intent(text, "no-such") is None


# ---------------------------------------------------------------- 回话模板
def test_action_reply_punch():
    assert gw.action_reply("punch", {}, {"ok": True, "data": "「张三」打卡成功"}) \
        == "章盖上了 —— 「张三」打卡成功"
    assert gw.action_reply("punch", {}, {"ok": False, "data": ""}) == "打卡没成"


def test_action_reply_capture():
    assert gw.action_reply("capture", {}, {"ok": True, "data": "照片已存"}) == "拍了，照片已存"


def test_action_reply_music():
    assert gw.action_reply("music", {"name": "晴天"}, {"ok": True, "data": "晴天"}) \
        == "行，放《晴天》。"
    assert gw.action_reply("music", {}, {"ok": False, "data": "INVALID_STATE"}) \
        == "先按一下录音键把我叫醒，再说歌名。"
    assert gw.action_reply("music", {"name": "晴天"}, {"ok": False, "data": "没找到"}) \
        == "放不了《晴天》：没找到"


def test_action_reply_other_tools():
    assert gw.action_reply("wallpaper", {}, {"ok": True}) == "换了张新的，抬头看看。"
    assert gw.action_reply("volume", {}, {"ok": True, "data": "80"}) == "调到 80 了。"
    assert gw.action_reply("unknown", {}, {"ok": True, "data": "done"}) == "done"
    assert gw.action_reply("unknown", {}, {"ok": False, "data": ""}) == "unknown"


# ---------------------------------------------------------------- 图片归一化
@pytest.fixture
def media_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(gw, "MEDIA_DIR", tmp_path)
    return tmp_path


def test_normalize_image_missing_returns_none(media_dir):
    assert gw.normalize_image("nope.png") is None


def test_normalize_image_keeps_compliant_jpeg(media_dir):
    Image.new("RGB", (640, 360)).save(media_dir / "ok.jpg", "JPEG", quality=85)
    assert gw.normalize_image("ok.jpg") == "ok.jpg"


def test_normalize_image_shrinks_oversized_png(media_dir):
    Image.new("RGB", (1600, 1200)).save(media_dir / "big.png")
    name = gw.normalize_image("big.png")
    assert name == "big__n.jpg"
    with Image.open(media_dir / name) as im:
        assert im.format == "JPEG"
        assert im.size[0] * im.size[1] <= gw.CIMG_PIXEL_CAP
        assert not im.info.get("progressive")
    assert (media_dir / name).stat().st_size <= gw.CIMG_BYTES_CAP


def test_normalize_image_reuses_cache(media_dir):
    Image.new("RGB", (1600, 1200)).save(media_dir / "big.png")
    assert gw.normalize_image("big.png") == "big__n.jpg"
    first = (media_dir / "big__n.jpg").stat().st_mtime
    assert gw.normalize_image("big.png") == "big__n.jpg"
    assert (media_dir / "big__n.jpg").stat().st_mtime == first


# ---------------------------------------------------------------- 媒体下载口
def test_media_rejects_traversal_and_bad_suffix():
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as e:
        asyncio.run(gw.media("../passwd"))
    assert e.value.status_code == 400
    with pytest.raises(HTTPException) as e:
        asyncio.run(gw.media("payload.exe"))
    assert e.value.status_code == 400
    with pytest.raises(HTTPException) as e:
        asyncio.run(gw.media("missing.png"))
    assert e.value.status_code == 404
