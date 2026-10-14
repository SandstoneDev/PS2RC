"""Fixtures for the PBP packer tests. Everything is generated here: no game data, no platform files."""
import pytest

from pbp_helpers import find_tool


@pytest.fixture
def ffmpeg():
    p = find_tool('ffmpeg', 'FFMPEG')
    if not p:
        pytest.skip('no ffmpeg (set FFMPEG or put it on PATH)')
    return p


@pytest.fixture
def pmfmux():
    from ps2rc.pbp import icon1
    return icon1.find_muxer()
