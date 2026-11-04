"""设备侧纯逻辑模块的 host 单测：用本机 gcc 真编译真跑（同一份 .c，不复制实现）。

CI runner 自带 gcc；三个模块分别是：
  - avi_mjpeg_muxer/demuxer（文件头注释就写明"纯 C99、可脱离 IDF 单测"）
  - ringbuf（只依赖 libc）
  - face_quality（只依赖 face_service.h 的类型，用 tests/host/stubs/esp_err.h 顶替）
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "tests" / "host"
EXE = ".exe" if os.name == "nt" else ""


def _gcc() -> str:
    gcc = shutil.which("gcc")
    if not gcc:
        pytest.fail("找不到 gcc：host C 用例需要本机编译器（CI runner 自带）")
    return gcc


def _build(tmp_path: Path, name: str, sources: list[Path], includes: list[Path]) -> Path:
    exe = tmp_path / (name + EXE)
    cmd = [_gcc(), "-std=c99", "-Wall", "-Wextra", "-O1"]
    for inc in includes:
        cmd += ["-I", str(inc)]
    cmd += [str(s) for s in sources]
    cmd += ["-o", str(exe)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.fail(f"{name} 编译失败：\n{proc.stdout}\n{proc.stderr}")
    return exe


def _run(exe: Path, *args: str) -> None:
    proc = subprocess.run([str(exe), *args], cwd=exe.parent,
                          capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.fail(f"{exe.name} 失败（exit={proc.returncode}）：\n"
                    f"{proc.stdout}\n{proc.stderr}")


@pytest.mark.offline
def test_avi_muxer_demuxer_roundtrip(tmp_path):
    exe = _build(
        tmp_path, "avi_roundtrip",
        [HOST / "test_avi_roundtrip.c",
         ROOT / "main/middleware/media/avi_mjpeg_muxer.c",
         ROOT / "main/middleware/media/avi_mjpeg_demuxer.c"],
        [ROOT / "main/middleware/media"])
    _run(exe, str(tmp_path / "out.avi"), str(tmp_path / "abort.avi"))


@pytest.mark.offline
def test_ringbuf_host_cases(tmp_path):
    exe = _build(
        tmp_path, "ringbuf_test",
        [HOST / "test_ringbuf.c", ROOT / "main/3rd/ringbuf/ringbuf.c"],
        [ROOT / "main/3rd/ringbuf"])
    _run(exe)


@pytest.mark.offline
def test_face_quality_thresholds(tmp_path):
    exe = _build(
        tmp_path, "face_quality_test",
        [HOST / "test_face_quality.c", ROOT / "main/middleware/camera/face_quality.c"],
        [HOST / "stubs", ROOT / "main/middleware/camera"])
    _run(exe)
