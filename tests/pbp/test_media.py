"""SND0.AT3 and ICON1.PMF: container checks on synthetic files; ICON1 encoding when ffmpeg + the muxer exist."""
import os
import random
import re
import struct
import subprocess

import pytest

from ps2rc.pbp import PbpError, at3, icon1, placeholder
from ps2rc.pbp.spec import MEDIA_BUDGET
from pbp_helpers import find_tool, make_at3, make_pmf, make_pmf_raps, make_png


def levels(issues):
    return {i.level for i in issues}


# ------------------------------------------------------------------------------------------ SND0.AT3
def test_at3_atrac3plus():
    a = at3.parse(make_at3(samples=44100 * 5, loop=(4048, 44100 * 5 + 4047)))
    assert a['codec'] == 'ATRAC3plus' and (a['rate'], a['channels']) == (44100, 2)
    assert a['seconds'] == pytest.approx(5.0) and a['loop'] == (4048, 44100 * 5 + 4047)
    assert a['block_align'] == 744 and a['riff_size_ok']
    assert not levels(at3.check(make_at3())) & {'error', 'spec', 'warn'}


@pytest.mark.parametrize('kw,level,needle', [
    (dict(tag=0x0270, align=384), 'warn', 'ATRAC3 (not'),
    (dict(tag=0x0001, align=4), 'error', 'PCM'),
    (dict(guid=b'\0' * 16), 'error', 'extensible'),
    (dict(rate=48000), 'spec', '48000 Hz'),
    (dict(channels=1), 'spec', '1 channel'),
    (dict(data_bytes=1000), 'warn', 'whole number'),
    (dict(fact=False), 'warn', 'length unknown'),
])
def test_at3_findings(kw, level, needle):
    found = at3.check(make_at3(**kw))
    assert any(i.level == level and needle in i.message for i in found), found


@pytest.mark.parametrize('data', [b'', b'RIFF\0\0\0\0WAVX', b'RIFF\x04\0\0\0WAVEfmt \xff\0\0\0'],
                         ids=['empty', 'not-wave', 'cut-chunk'])
def test_at3_damaged(data):
    with pytest.raises(PbpError):
        at3.parse(data)
    assert 'error' in levels(at3.check(data))


def test_at3_tags_match_ffmpeg(tmp_path):
    """the format tag / GUID facts against ffmpeg's RIFF demuxer (skipped without ffprobe)"""
    probe = find_tool('ffprobe', 'FFPROBE')
    if not probe and find_tool('ffmpeg', 'FFMPEG'):
        cand = os.path.join(os.path.dirname(find_tool('ffmpeg', 'FFMPEG')), 'ffprobe' + (
            '.exe' if find_tool('ffmpeg', 'FFMPEG').lower().endswith('.exe') else ''))
        probe = cand if os.path.isfile(cand) else None
    if not probe:
        pytest.skip('no ffprobe')
    for name, kw, want in (('p.at3', {}, 'atrac3p'), ('a.at3', dict(tag=0x0270, align=384), 'atrac3')):
        f = tmp_path / name
        f.write_bytes(make_at3(**kw))
        r = subprocess.run([probe, '-hide_banner', str(f)], capture_output=True, text=True)
        m = re.search(r'Audio: (\w+)', r.stderr)       # the stream line names the codec the tag maps to
        assert m and m.group(1) == want, r.stderr


# ------------------------------------------------------------------------------------------ ICON1.PMF (ready)
def test_pmf_header():
    m = icon1.parse(make_pmf(seconds=3.0, audio=True))
    assert m['version'] == '0015' and m['seconds'] == pytest.approx(3.0)
    assert [s['kind'] for s in m['streams']] == ['video', 'audio']
    assert (m['streams'][0]['width'], m['streams'][0]['height']) == (144, 80)
    assert not levels(icon1.check(make_pmf())) & {'error', 'spec', 'warn'}


@pytest.mark.parametrize('data,level', [
    (make_pmf(480, 272), 'spec'),
    (make_pmf()[:-100], 'error'),
    (b'PSMX0015' + b'\0' * 0x800, 'error'),
    (b'PSMF', 'error'),
], ids=['480x272', 'cut', 'magic', 'short'])
def test_pmf_findings(data, level):
    assert level in levels(icon1.check(data))


# ------------------------------------------------------------------------------------------ ICON1 encoding
def test_missing_ffmpeg_is_named(tmp_path):
    with pytest.raises(PbpError, match='ffmpeg'):
        icon1.find_ffmpeg(str(tmp_path / 'ffmpeg-nowhere'))


# ------------------------------------------------------------------------------------------ entry-point map
def test_add_entry_points():
    src = make_pmf_raps({0: 90000, 3: 180090, 5: 270180})
    out = icon1.add_entry_points(src)
    assert icon1.entry_points(src) == [] and icon1.entry_points(out) == [(90000, 0), (180090, 3), (270180, 5)]
    assert out[:8] == b'PSMF0012' and len(out) == len(src) and out[0x800:] == src[0x800:]
    # what changed in the header: the version, the stream entry's map offset/count, the 30 map bytes - nothing else
    changed = [i for i in range(0x800) if out[i] != src[i]]
    assert set(changed) <= set(range(4, 8)) | set(range(0x86, 0x8E)) | set(range(0x92, 0x92 + 30))
    assert out[0x86:0x8E] == struct.pack('>II', 0x92, 3)
    assert out[0x92:0x9C] == bytes(2) + struct.pack('>II', 90000, 0)
    assert 'entry points' in icon1.check(out)[-1].message and 'no entry-point map' in icon1.check(src)[-1].message
    assert icon1.add_entry_points(src, version=None)[:8] == b'PSMF0015'


@pytest.mark.parametrize('case', ['twice', 'no-rap-first', 'two-videos'])
def test_add_entry_points_refuses(case):
    src = make_pmf_raps({0: 90000})
    if case == 'twice':
        src = icon1.add_entry_points(src)
    elif case == 'no-rap-first':
        src = make_pmf_raps({2: 90000})
    else:
        b = bytearray(src)
        struct.pack_into('>H', b, 0x80, 2)
        b[0x92] = 0xE1
        src = bytes(b)
    with pytest.raises(PbpError):
        icon1.add_entry_points(src)


def test_still_picture_is_not_an_animation(tmp_path):
    f = tmp_path / 'still.png'
    f.write_bytes(make_png(144, 80))
    with pytest.raises(PbpError, match='still picture'):
        icon1.encode(src=str(f))


def test_encode_frames(ffmpeg, pmfmux):
    frames = placeholder.icon1_frames(seconds=2.0)
    assert len(frames) == 60
    pmf, rep = icon1.encode(frames=frames, ffmpeg=ffmpeg, muxer=pmfmux)
    assert not levels(icon1.check(pmf)) & {'error', 'spec', 'warn'}
    m = icon1.parse(pmf)
    assert m['seconds'] == pytest.approx(60 * 1001 / 30000.0, abs=0.01)
    assert len(pmf) == rep['bytes'] <= MEDIA_BUDGET and rep['frames'] == 60


def test_encode_with_entry_points(ffmpeg, pmfmux):
    from ps2rc.movies import pmfinfo
    frames = placeholder.icon1_frames(seconds=2.0)
    plain, _ = icon1.encode(frames=frames, ffmpeg=ffmpeg, muxer=pmfmux)
    mapped, rep = icon1.encode(frames=frames, ffmpeg=ffmpeg, muxer=pmfmux, epmap=True)
    assert mapped[0x800:] == plain[0x800:] and mapped[:8] == b'PSMF0012' and rep['entry_points'] == 2
    assert icon1.entry_points(mapped) == [(90000, 0), (90000 + 30 * 3003, icon1.entry_points(mapped)[1][1])]
    k = icon1.entry_points(mapped)[1][1]
    assert mapped[0x800 + k * 2048 + 14:0x800 + k * 2048 + 18] == bytes.fromhex('000001bb')  # the map points at a RAP pack
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, 'm.pmf')
        with open(f, 'wb') as fh:
            fh.write(mapped)
        info = pmfinfo.analyze(f)
        assert pmfinfo.check(info) == [] and info['header']['streams'][0]['ep_count'] == 2


def test_encode_video_into_a_small_budget(tmp_path, ffmpeg, pmfmux):
    src = tmp_path / 'clip.mp4'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=25:duration=3',
                    '-pix_fmt', 'yuv420p', str(src)], check=True)
    pmf, rep = icon1.encode(src=str(src), budget=40 * 1024, ffmpeg=ffmpeg, muxer=pmfmux)
    assert len(pmf) <= 40 * 1024
    m = icon1.parse(pmf)
    assert (m['streams'][0]['width'], m['streams'][0]['height']) == (144, 80)
    assert m['seconds'] == pytest.approx(3.0, abs=0.05)          # 25 fps in, 29.97 fps out, same length


def test_encode_gif_with_seconds_cap(tmp_path, ffmpeg, pmfmux):
    src = tmp_path / 'anim.gif'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=size=100x100:rate=10:duration=4',
                    str(src)], check=True)
    pmf, rep = icon1.encode(src=str(src), max_seconds=1.5, fit='contain', ffmpeg=ffmpeg, muxer=pmfmux)
    assert icon1.parse(pmf)['seconds'] == pytest.approx(1.5, abs=0.05)


def test_noise_never_exceeds_the_budget(ffmpeg, pmfmux):
    """random pictures do not compress: the first guess overshoots, so the loop must lower the rate or refuse"""
    rnd = random.Random(1)
    frames = [bytes(rnd.getrandbits(8) for _ in range(144 * 80 * 3)) for _ in range(30)]
    for budget in (60 * 1024, 16 * 1024):
        try:
            pmf, rep = icon1.encode(frames=frames, budget=budget, ffmpeg=ffmpeg, muxer=pmfmux)
        except PbpError as e:
            assert 'cannot fit' in str(e)
            continue
        assert len(pmf) <= budget and rep['tries'][-1][1] <= budget


def test_budget_too_small_is_refused(ffmpeg, pmfmux):
    with pytest.raises(PbpError, match='budget'):
        icon1.encode(frames=placeholder.icon1_frames(seconds=1.0), budget=4000, ffmpeg=ffmpeg, muxer=pmfmux)
