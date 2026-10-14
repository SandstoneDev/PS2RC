"""The open movie path: ps2rc/movies/pmfmux.py (H.264 -> PSMF), pmfinfo.py
(reader + layout rules), pss.py (PS2 PSS sound), the PSP player's WAV reader hle/psp/hle_wavsrc.c (host-compiled
with gcc, compared with ffmpeg's decode), and the player wiring.

Crafted streams cover the packetizer's edge cases without any encoder; the ffmpeg tests (libx264) are skipped when no
ffmpeg is found (FFMPEG environment variable, local.toml [tools] ffmpeg or PATH)."""
import os
import zlib
import random
import shutil
import struct
import subprocess
import sys
import wave

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'ps2rc', 'movies'))
import pmfinfo  # noqa: E402
import pmfmux  # noqa: E402
import pss  # noqa: E402

# parameter sets as libx264 writes them for 480x272 Main@2.1 (encoder settings, no picture content)
SPS = bytes.fromhex('674d4015da07823b016a0c0c0c800001f48000753070000016e360001c9c3454401f162ea0')
PPS = bytes.fromhex('68ee01acb2')


def ffmpeg_path():
    for c in (os.environ.get('FFMPEG'), config.tool('ffmpeg'), shutil.which('ffmpeg')):
        if c and os.path.isfile(c):
            return c
    return None


# ------------------------------------------------------------------------------------------------ crafted streams
def au(size, idr, ref=True, btype=False):
    """one access unit of exactly `size` bytes: AUD (+ SPS, PPS) + one slice with filler payload"""
    head = b'\0\0\0\1\x09' + (b'\x10' if idr else b'\x30')
    if idr:
        head += b'\0\0\0\1' + SPS + b'\0\0\0\1' + PPS
        nal = bytes([0x65, 0x88])                     # IDR, first_mb 0, slice_type 7 (I)
    else:
        nal = bytes([0x41 if ref else 0x01, 0xA0 if btype else 0x9A])   # first_mb 0, slice_type 6 (B) / 5 (P)
    fill = size - len(head) - 3 - len(nal)
    assert fill >= 1
    return head + b'\0\0\1' + nal + b'\x55' * fill


def es_of(sizes, gop):
    return b''.join(au(s, k % gop == 0) for k, s in enumerate(sizes))


def mux_check(es, tmp_path, name='t.pmf', **kw):
    p = str(tmp_path / name)
    st = pmfmux.write_checked(es, p, **kw)
    info = pmfinfo.analyze(p)
    assert pmfinfo.check(info) == []
    return p, st, info


def test_random_streams_hold_every_rule(tmp_path):
    rnd = random.Random(7)
    for trial in range(6):
        gop = rnd.choice([1, 2, 5, 12, 59, 120])
        n = rnd.randint(1, 260)
        sizes = [rnd.choice([rnd.randint(60, 400), rnd.randint(400, 4000), rnd.randint(4000, 30000)])
                 for _ in range(n)]
        sizes = [max(s, 80) for s in sizes]
        p, st, info = mux_check(es_of(sizes, gop), tmp_path, 'r%d.pmf' % trial, recovery_sei=trial % 2 == 1)
        assert st['access_units'] == n and info['aus'] == n and info['raps'] == (n + gop - 1) // gop
        assert info['header']['last_ts'] - info['header']['first_ts'] == 3003 * n
        assert os.path.getsize(p) % 2048 == 0
        if trial % 2 == 0:
            assert info['_es'] == es_of(sizes, gop)                    # the ES comes back unchanged
        else:                                                          # + a recovery point SEI per IDR picture
            assert len(info['_es']) == len(es_of(sizes, gop)) + 8 * info['raps']


@pytest.mark.parametrize('free', [1, 3, 5, 6, 7, 40])
def test_pack_end_stuffing_or_padding(tmp_path, free):
    """a one-picture GOP whose data leaves `free` bytes in the RAP pack: 1-5 -> PES header stuffing, >= 6 -> padding"""
    cap = 2048 - 14 - 18 - (6 + 18 + 4) - 9 - 10 - 3
    p, st, info = mux_check(es_of([cap - free, 300], 1), tmp_path, recovery_sei=False)
    variants = info['pes_variants']
    if free < 6:
        assert any(k.startswith('e0 81c1 hl%d ' % (13 + free)) for k in variants), variants
        assert 'bb bf e0' in info['layouts']
    else:
        assert 'bb bf e0 be' in info['layouts']


def test_rapi_fields_against_the_packs(tmp_path):
    sizes = [5000, 2100, 30, 30, 2500, 9000, 100] * 3
    p, st, info = mux_check(es_of(sizes, 7), tmp_path, recovery_sei=False)
    r = info['_raps'][0]['body']
    assert r[:2] == b'\x01\xe0' and struct.unpack('>H', r[16:18])[0] == 7
    assert [struct.unpack('>HH', r[18 + 4 * j:22 + 4 * j]) for j in range(7)] == [(0x80, s) for s in sizes[:7]]
    # pack offsets of the last byte of pictures 1..4: 5000 B from a 1906-byte first pack -> pack 2 (+2), ...
    assert list(struct.unpack('>4H', r[2:10])) == [2, 3, 3, 3]


def test_short_group_marks_missing_pictures(tmp_path):
    """a group of pictures with fewer than four pictures: 0xFFFF for the missing pack offsets (as the reference tool
    writes for a one-picture last group)"""
    p, st, info = mux_check(es_of([3000, 400, 400, 400, 400, 2500, 300], 5), tmp_path, recovery_sei=False)
    last = struct.unpack('>4H', info['_raps'][-1]['body'][2:10])          # a two-picture last group
    assert last[2:] == (0xFFFF, 0xFFFF) and 0xFFFF not in last[:2]
    p, st, info = mux_check(es_of([3000, 400, 400, 400, 400, 2500], 5), tmp_path, 'one.pmf', recovery_sei=False)
    last = struct.unpack('>4H', info['_raps'][-1]['body'][2:10])          # a one-picture last group
    assert last[1:] == (0xFFFF, 0xFFFF, 0xFFFF) and last[0] != 0xFFFF


def test_icon_version(tmp_path):
    data, st = pmfmux.mux(es_of([3000, 400, 400], 3), icon=True)
    assert data[:8] == b'PSMF0014'
    data, st = pmfmux.mux(es_of([3000, 400, 400], 3))
    assert data[:8] == b'PSMF0015'
    with pytest.raises(pmfmux.MuxError):
        pmfmux.mux(es_of([3000], 1), version='15')


def test_check_catches_a_broken_record(tmp_path):
    """mutation: the rules must see a wrong size entry and a wrong pack offset"""
    p, st, info = mux_check(es_of([3000, 500, 500, 500, 700] * 2, 5), tmp_path)
    data = bytearray(open(p, 'rb').read())
    at = data.find(b'\x00\x00\x01\xbf') + 6
    for off, needle in ((18 + 4 * 1 + 3, 'size table'), (3, 'pack offsets')):
        bad = bytearray(data)
        bad[at + off] ^= 0x01
        q = str(tmp_path / 'bad.pmf')
        open(q, 'wb').write(bad)
        probs = pmfinfo.check(pmfinfo.analyze(q))
        assert any(needle in x for x in probs), probs


@pytest.mark.parametrize('case', ['b_slices', 'non_ref', 'some_aud', 'too_big', 'p_first'])
def test_mux_refuses(case):
    if case == 'b_slices':
        es = au(3000, True) + au(500, False, btype=True)
    elif case == 'non_ref':
        es = au(3000, True) + au(500, False, ref=False)
    elif case == 'some_aud':                               # delimiters on some pictures only
        es = au(3000, True) + au(500, False)[6:]
    elif case == 'too_big':
        es = au(70000, True)
    else:
        es = au(500, False) + au(3000, True)
    with pytest.raises(pmfmux.MuxError):
        pmfmux.mux(es)


def test_delimiters_added_when_the_stream_has_none(tmp_path):
    """OpenH264-style input: no access unit delimiters at all -> one is added per picture (SPS/PPS/SEI and a slice
    with first_mb 0 start a new access unit)"""
    sizes = [3000, 400, 400, 2500, 300]
    plain = b''.join(au(s, k % 3 == 0)[6:] for k, s in enumerate(sizes))
    with pytest.raises(pmfmux.MuxError):
        pmfmux.mux(plain, insert_aud=False)
    p, st, info = mux_check(plain, tmp_path, recovery_sei=False)
    assert st['access_units'] == 5 and info['raps'] == 2
    assert info['_es'] == b''.join(b'\x00\x00\x00\x01\x09\xf0' + au(s, k % 3 == 0)[6:] for k, s in enumerate(sizes))


def test_sei_userdata_dropped_and_recovery_point_added():
    ud = pmfmux.sei_nal(0x06, [(5, b'\x11' * 16 + b'x264 banner')])
    bp = pmfmux.sei_nal(0x06, [(0, b'\x98\xb8\x00\xaf\xcc')])
    a = au(3000, True)
    i = a.find(b'\0\0\1\x65')
    es = a[:i] + b'\0\0\1' + bp + b'\0\0\1' + ud + a[i:]
    aus, _ = pmfmux.access_units(es)
    types = [(n[0] & 0x1F, pmfmux.sei_messages(n)[0][0] if n[0] & 0x1F == 6 else None) for n in aus[0]['nals']]
    assert types == [(9, None), (7, None), (8, None), (6, 0), (6, 6), (5, None)]
    assert pmfmux.sei_messages(pmfmux.RECOVERY_SEI) == [(6, b'\xc4')]


# (a game's converted movies are checked by its pack's tests)


def test_x264_icon_144x80(tmp_path):
    """an XMB movie icon as ps2rc/pbp would make it: 144x80, 2 s loop, video only, version 0014 (library use)"""
    ff = ffmpeg_path()
    if not ff:
        pytest.skip('no ffmpeg')
    sys.path.insert(0, os.path.join(ROOT, 'ps2rc', 'movies'))
    import convert_movies_open as cmo
    es = str(tmp_path / 'i.264')
    r = subprocess.run([ff, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=144x80:rate=30000/1001:d=2',
                        '-c:v', 'libx264', '-profile:v', 'main', '-level:v', '2.1', '-b:v', '300k',
                        '-x264-params', cmo.X264 + ':no-deblock=1', '-pix_fmt', 'yuv420p', '-f', 'h264', es],
                       capture_output=True)
    if r.returncode != 0:
        pytest.skip('ffmpeg without libx264')
    p = str(tmp_path / 'ICON1.PMF')
    st = pmfmux.write_checked(open(es, 'rb').read(), p, icon=True)
    info = pmfinfo.analyze(p)
    assert info['header']['magic'] == 'PSMF0014' and st['access_units'] == 60
    assert [(s['kind'], s['width'], s['height']) for s in info['header']['streams']] == [('avc', 144, 80)]


# ------------------------------------------------------------------------------------------------ PSS sound
def test_pss_sound_and_av_offset():
    def pes_pkt(sid, payload, pts=None):
        hdr, flags = b'', 0
        if pts is not None:
            flags, hdr = 0x80, bytes([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, ((pts >> 14) & 0xFE) | 1,
                                      (pts >> 7) & 0xFF, ((pts << 1) & 0xFE) | 1])
        body = bytes([0x81, flags, len(hdr)]) + hdr + payload
        return b'\0\0\1' + bytes([sid]) + struct.pack('>H', len(body)) + body
    left, right = list(range(32)), [-v - 7 for v in range(32)]
    body = b''.join(struct.pack('<4h', *left[k:k + 4]) + struct.pack('<4h', *right[k:k + 4]) for k in range(0, 32, 4))
    snd = b'SShd' + struct.pack('<I', 24) + struct.pack('<6i', 1, 48000, 2, 8, -1, -1) + b'SSbd' + \
        struct.pack('<I', len(body)) + body
    data = b'\0\0\1\xba' + bytes([0x44, 0, 4, 0, 4, 1, 0, 0, 3, 0xF8]) + pes_pkt(0xE0, b'\0\0\1\xb3', 5004)
    data += pes_pkt(0xBD, b'\xff\xa0\0\0' + snd[:30], 6504) + pes_pkt(0xBD, b'\xff\xa0\0\0' + snd[30:]) + b'\0\0\1\xb9'
    hdr, b, st = pss.sound(data)
    assert (hdr['codec'], hdr['rate'], hdr['channels'], hdr['interleave']) == (1, 48000, 2, 8)
    assert (st['first_video_pts'], st['first_audio_pts']) == (5004, 6504)
    pcm, frames, dropped = pss.deinterleave_pcm16(hdr, b)
    v = struct.unpack('<64h', pcm)
    assert frames == 32 and dropped == 0 and list(v[0::2]) == left and list(v[1::2]) == right
    hdr['codec'] = 0x10
    with pytest.raises(pss.PssError):
        pss.deinterleave_pcm16(hdr, b)


# ------------------------------------------------------------------------------------------------ the PSP WAV reader
def _wsl(cmd, timeout=300):
    return subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=timeout)


def _wsl_path(p):
    return config.posix_path(os.path.abspath(p))


@pytest.fixture(scope='module')
def wavsrc_host():
    try:
        ok = _wsl('true', 60).returncode == 0
    except Exception:
        ok = False
    if not ok:
        pytest.skip('needs WSL')
    d = '/tmp/ps2rc_wavsrc_%08x' % zlib.crc32(ROOT.encode())    # per checkout
    out = d + '/wavsrc_host'
    r = _wsl('mkdir -p ' + d + ' && gcc -O2 -Wall -Wextra -Werror -std=c11 -I%s -o %s %s %s' % (
        _wsl_path(os.path.join(ROOT, 'hle', 'psp')), out, _wsl_path(os.path.join(HERE, 'wavsrc_host.c')),
        _wsl_path(os.path.join(ROOT, 'hle', 'psp', 'hle_wavsrc.c'))))
    assert r.returncode == 0, r.stderr.decode(errors='replace')
    return out


@pytest.mark.wsl
@pytest.mark.parametrize('codec,ch,block,stage,chunk', [
    ('adpcm_ima_wav', 2, 2048, 32768, 2048),
    ('adpcm_ima_wav', 2, 2048, 4096, 777),       # staging smaller than a few blocks, odd read sizes
    ('adpcm_ima_wav', 1, 512, 3000, 2048),       # mono (duplicated), staging not a multiple of the block
    ('pcm_s16le', 2, 0, 32768, 2048),
    ('pcm_s16le', 1, 0, 1000, 333),
])
def test_wavsrc_matches_ffmpeg(tmp_path, wavsrc_host, codec, ch, block, stage, chunk):
    ff = ffmpeg_path()
    if not ff:
        pytest.skip('no ffmpeg')
    rnd = random.Random(3)
    n = 44100 * 2 + 1234
    src = str(tmp_path / 'src.wav')
    import math
    with wave.open(src, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        fr = bytearray()
        for i in range(n):
            l_ = int(12000 * math.sin(i * 0.031) + rnd.randint(-3000, 3000) + (20000 if (i // 4000) % 2 else 0))
            r_ = int(9000 * math.sin(i * 0.0071 + 1) * (1 if i < n // 2 else -1))
            fr += struct.pack('<hh', max(-32768, min(32767, l_)), max(-32768, min(32767, r_)))
        w.writeframes(bytes(fr))
    enc = str(tmp_path / 'enc.wav')
    cmd = [ff, '-v', 'error', '-y', '-i', src, '-ac', str(ch), '-c:a', codec]
    if block:
        cmd += ['-block_size', str(block)]
    subprocess.run(cmd + ['-fflags', '+bitexact', '-flags', '+bitexact', '-map_metadata', '-1', enc], check=True)
    ref = subprocess.run([ff, '-v', 'error', '-i', enc, '-f', 's16le', '-acodec', 'pcm_s16le', '-'],
                         capture_output=True, check=True).stdout
    raw = str(tmp_path / 'out.raw')
    r = _wsl('%s %s %s %d %d' % (wavsrc_host, _wsl_path(enc), _wsl_path(raw), stage, chunk))
    txt = r.stdout.decode()
    assert r.returncode == 0, txt + r.stderr.decode(errors='replace')
    kv = dict(x.split('=') for x in txt.split())
    got = open(raw, 'rb').read()
    frames = int(kv['frames'])
    assert frames == int(kv['total']) and abs(frames - n) < max(block, 1)
    ours = struct.unpack('<%dh' % (2 * frames), got)
    if ch == 1:
        theirs = struct.unpack('<%dh' % frames, ref[:2 * frames])
        assert list(ours[0::2]) == list(theirs) and ours[0::2] == ours[1::2]
    else:
        theirs = struct.unpack('<%dh' % (2 * frames), ref[:4 * frames])
        assert list(ours) == list(theirs)


# ------------------------------------------------------------------------------------------------ wiring
def test_player_wiring():
    src = open(os.path.join(ROOT, 'hle', 'psp', 'hle_movie.c'), encoding='utf-8').read()
    assert 'm->has_atrac ? sceMpegRegistStream(&m->mpeg, 1, 0) : NULL' in src     # no phantom ATRAC stream
    assert 'm.atrac && !audio_done' in src and 'm.side && !audio_done' in src
    assert '"movie_dir"' in src and 'wavsrc_read(&m.wav, slot, AUD_FRAMES)' in src
    assert 'audio_pts0 = m.first_ts' in src
    for f in ('hle_wavsrc.c', 'hle_wavsrc.h'):
        txt = open(os.path.join(ROOT, 'hle', 'psp', f), encoding='utf-8').read()
        assert '#include <psp' not in txt                                         # plain C (host-compiled above)
