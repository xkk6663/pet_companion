"""网关 HTTP 面：路由 / 上传闸 / 离线设备回退。用 FastAPI TestClient，不发真实网络。"""
from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

import ws_gateway as gw


@pytest.fixture(scope="module")
def client():
    with TestClient(gw.app) as c:
        yield c


@pytest.fixture
def upload_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(gw, "UPLOAD_DIR", tmp_path)
    return tmp_path


def test_health(client):
    body = client.get("/health").json()
    assert body["ok"] is True
    assert body["version"] == "0.8.1"
    assert "sessions" in body and "pending_actions" in body


def test_caps_without_device(client, monkeypatch):
    monkeypatch.setattr(gw, "SESSIONS", {})
    body = client.get("/caps").json()
    assert body["online"] is False
    assert body["tools"] == []


def test_caps_online_device(client, monkeypatch):
    sid = "xz-s31-test"
    monkeypatch.setattr(gw, "SESSIONS", {sid: {"caps": {
        "model": "ESP32-S31-Korvo-1",
        "tools": [{"name": "punch", "label": "打卡"}],
    }}})
    body = client.get(f"/caps?session_id={sid}").json()
    assert body["online"] is True
    assert body["model"] == "ESP32-S31-Korvo-1"
    assert body["tools"][0]["name"] == "punch"


def test_caps_session_known_but_no_caps(client, monkeypatch):
    sid = "xz-s31-nocaps"
    monkeypatch.setattr(gw, "SESSIONS", {sid: {}})
    body = client.get(f"/caps?session_id={sid}").json()
    assert body["online"] is False
    assert body["reason"] == "no_caps_yet"


def test_media_bad_suffix(client):
    assert client.get("/media/payload.exe").status_code == 400


def test_media_missing(client):
    assert client.get("/media/nope.png").status_code == 404


def test_upload_roundtrip_and_sanitized_name(client, upload_dir):
    r = client.post("/upload/device%20one", content=b"\xff\xd8fakejpeg")
    assert r.status_code == 200
    aid = r.json()["attachment_id"]
    assert aid.startswith("device_one_")
    assert "/" not in aid and " " not in aid
    assert (upload_dir / aid).is_file()
    assert (upload_dir / aid).read_bytes() == b"\xff\xd8fakejpeg"


def test_upload_empty_rejected(client, upload_dir):
    assert client.post("/upload/s1", content=b"").status_code == 400


def test_upload_oversize_rejected(client, upload_dir):
    body = b"x" * (gw.MAX_UPLOAD_BYTES + 1)
    assert client.post("/upload/s1", content=body).status_code == 413


def test_upload_keeps_only_32_files(client, upload_dir):
    for i in range(35):
        p = upload_dir / f"old_{i}.jpg"
        p.write_bytes(b"x")
        os.utime(p, (1000 + i, 1000 + i))
    assert client.post("/upload/s1", content=b"newjpeg").status_code == 200
    assert len(list(upload_dir.glob("*.jpg"))) == 32


def test_push_validation(client):
    assert client.post("/push", json={"text": "hi"}).status_code == 400
    assert client.post("/push", json={"session_id": "nope"}).status_code == 400


def test_push_offline_device(client, monkeypatch):
    monkeypatch.setattr(gw, "SESSIONS", {})
    body = client.post("/push", json={"session_id": "nope", "text": "hi"}).json()
    assert body["delivered"] is False
    assert body["reason"] == "device_offline"


def test_action_validation(client):
    assert client.post("/action", json={"name": "punch"}).status_code == 400
    assert client.post("/action", json={"session_id": "s1"}).status_code == 400


def test_action_offline_device(client, monkeypatch):
    monkeypatch.setattr(gw, "SESSIONS", {})
    body = client.post("/action", json={"session_id": "nope", "name": "punch"}).json()
    assert body["ok"] is False
    assert body["reason"] == "device_offline"
