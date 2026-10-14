"""Tests of the converter window (gui/) and the menu-look module (ps2rc/xmb). Everything is generated here: no game
data, no PSP, no network. Optional tools: ffmpeg (FFMPEG or PATH), atracdenc (PS2RC_ATRACDENC or PATH)."""
import os
import sys

import pytest

# never a visible window or a Qt message box from the tests: Qt runs offscreen, whatever the caller set
os.environ["QT_QPA_PLATFORM"] = "offscreen"
if os.name == "nt" and not os.environ.get("QT_QPA_FONTDIR"):
    os.environ["QT_QPA_FONTDIR"] = os.path.join(os.environ.get("WINDIR", ""), "Fonts")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUI = os.path.join(ROOT, "gui")
for p in (ROOT, GUI, os.path.dirname(os.path.abspath(__file__))):
    if p not in sys.path:
        sys.path.insert(0, p)

import gui_helpers  # noqa: E402,F401  (the fake encoder script imports it from this folder)


@pytest.fixture
def ffmpeg():
    from ps2rc.xmb import media
    p = media.find_ffmpeg()
    if not p:
        pytest.skip("no ffmpeg (set FFMPEG or put it on PATH)")
    return p


@pytest.fixture
def no_ffmpeg(monkeypatch):
    from ps2rc.xmb import media
    monkeypatch.setattr(media, "find_ffmpeg", lambda given=None: None)


@pytest.fixture
def pillow():
    return pytest.importorskip("PIL.Image")


@pytest.fixture
def fake_encoder(tmp_path, monkeypatch):
    """a stand-in for atracdenc: a script with its command line that writes atracdenc's RIFF layout"""
    script = tmp_path / "fake_atracdenc.py"
    script.write_text(
        "import sys, wave, os\n"
        "sys.path.insert(0, %r)\n"
        "from gui_helpers import atracdenc_like, oma_like, atrac3_like\n"
        "a = sys.argv[1:]\n"
        "mode = a[a.index('-e') + 1]\n"
        "assert mode in ('atrac3plus', 'atrac3', 'atrac3_lp4') and '--container' not in a\n"
        "w = wave.open(a[a.index('-i') + 1]); n = w.getnframes(); w.close()\n"
        "out = a[a.index('-o') + 1]\n"
        "data = oma_like(n) if out.endswith('.oma') else atracdenc_like(n)\n"
        "if mode != 'atrac3plus':\n"
        "    data = atrac3_like(n, 384 if mode == 'atrac3' else 192, 0 if mode == 'atrac3' else 1)\n"
        "open(out, 'wb').write(data)\n"
        % os.path.dirname(os.path.abspath(__file__)))
    if os.name == "nt":
        exe = tmp_path / "atracdenc.cmd"
        exe.write_text('@"%s" "%s" %%*\n' % (sys.executable, script))
    else:
        exe = tmp_path / "atracdenc"
        exe.write_text('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (sys.executable, script))
        exe.chmod(0o755)
    monkeypatch.setenv("PS2RC_ATRACDENC", str(exe))
    return str(exe)
