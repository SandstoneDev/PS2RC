"""The picture check of converted movies / ICON1 (ps2rc.movies.framecheck) and the scaler settings that avoid the
faulty-build garbage (accurate_rnd, --disable-asm). The encoding tests need ffmpeg with libx264 (skipped without)."""
import os

import pytest

from ps2rc.movies import framecheck
from ps2rc.pbp import PbpError, icon1, placeholder
from ps2rc.pbp.spec import ICON1_W, ICON1_H

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def gradient(w, h, shift=0):
    return bytes(((x * 255 // w + shift) & 255, (y * 255 // h) & 255, 96) [c]
                 for y in range(h) for x in range(w) for c in range(3))


def test_block_psnr_same_and_different():
    a = gradient(48, 32)
    assert framecheck.block_psnr(a, a, 48, 32) == 99.0
    green = bytes((0, 255, 0) * (48 * 32))
    assert framecheck.block_psnr(a, green, 48, 32) < 10
    noisy = bytes((v + (7 if i % 2 else -7)) & 255 if 8 < v < 247 else v for i, v in enumerate(a))
    assert framecheck.block_psnr(a, noisy, 48, 32) > 40          # pixel noise averages out in the block means


def test_scalers_use_the_exact_path():
    assert all('accurate_rnd' in f for f in icon1.FIT_FILTERS.values())
    assert all('format=rgb24' not in f and 'accurate_rnd' not in f for f in icon1.FIT_RGB.values())
    with open(os.path.join(ROOT, 'ps2rc', 'movies', 'convert_movies_open.py'), encoding='utf-8') as f:
        src = f.read()
    assert 'flags=lanczos+accurate_rnd' in src and 'framecheck.check(' in src
    assert "'-sws_flags', 'lanczos+accurate_rnd'" in src
    with open(os.path.join(ROOT, 'ps2rc', 'pbp', 'icon1.py'), encoding='utf-8') as f:
        assert "'-sws_flags', 'lanczos+accurate_rnd'" in f.read()
    with open(os.path.join(ROOT, 'ps2rc', 'xmb', 'clip.py'), encoding='utf-8') as f:
        assert 'flags=lanczos+accurate_rnd' in f.read()
    with open(os.path.join(ROOT, 'gui', 'ffmpeg_min', 'build_ffmpeg_min.sh'), encoding='utf-8') as f:
        sh = f.read()
    assert 'ASM=--disable-asm' in sh and 'ASM=--disable-x86asm' not in sh


def test_icon1_reports_its_picture_check(ffmpeg, pmfmux):
    frames = placeholder.icon1_frames(seconds=1.0)
    pmf, rep = icon1.encode(frames=frames, ffmpeg=ffmpeg, muxer=pmfmux)
    pc = rep['picture_check']
    assert pc['best'] >= framecheck.THRESHOLD_DB and len(pc['psnr']) == 5


def test_check_fails_loudly_on_wrong_pictures(tmp_path, ffmpeg, pmfmux):
    frames = placeholder.icon1_frames(seconds=1.0)
    pmf, _ = icon1.encode(frames=frames, ffmpeg=ffmpeg, muxer=pmfmux)
    path = tmp_path / 'icon1.pmf'
    path.write_bytes(pmf)
    garbage = [bytes((0, 255, 0) * (ICON1_W * ICON1_H))] * 5       # what the faulty build looked like: green
    with pytest.raises(framecheck.FrameCheckError, match='do not match'):
        framecheck.check(ffmpeg, str(path), ICON1_W, ICON1_H, reference=garbage, what='ICON1')


def test_icon1_from_a_clip_matches_its_source(tmp_path, ffmpeg, pmfmux):
    # a 320x240 clip with a vertical size change on the way to 144x80 (the case the faulty build broke)
    raw = tmp_path / 'clip.rgb'
    with open(raw, 'wb') as f:
        for i in range(30):
            f.write(gradient(320, 240, shift=i * 4))
    clip = tmp_path / 'clip.mp4'
    import subprocess
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '320x240', '-r', '30',
                    '-i', str(raw), '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-f', 'h264', str(tmp_path / 'c.264')],
                   check=True)
    clip = tmp_path / 'c.264'
    try:
        pmf, rep = icon1.encode(src=str(clip), ffmpeg=ffmpeg, muxer=pmfmux, max_seconds=1.0)
    except PbpError as e:
        pytest.fail(str(e))
    assert rep['picture_check']['best'] >= framecheck.THRESHOLD_DB
