"""ICON1.PMF: the animated menu icon, a PSMF movie of 144x80 pixels.

Checking a ready .pmf needs nothing but this file. Making one from a video or an animated GIF needs ffmpeg with the
libx264 encoder (user-installed; `--ffmpeg PATH`, the FFMPEG environment variable, or PATH); the PSMF muxer is
ps2rc.movies.pmfmux of this package.

The picture is H.264 Main profile level 2.1, 29.97 frames per second, progressive, no B pictures; the menu loops it.
Sound inside ICON1 would have to be ATRAC3plus, which this project cannot encode, so the icon is made without
sound (SND0.AT3 is the place for menu music). ICON1 and SND0 share one size budget (see spec.MEDIA_BUDGET); the
encoder lowers the bit rate until the movie fits what SND0 leaves.

PSMF header fields read here (big-endian; the same fields ps2rc/movies/pmfinfo.py documents):
    0x00 "PSMF" + 4 version digits, 0x08 u32 stream offset (0x800), 0x0C u32 stream size,
    0x54 / 0x5A 48-bit first / last timestamp (90 kHz), 0x80 u16 stream count,
    0x82 + 16 * i stream entries: +0 stream id (0xE0.. video, 0xBD audio), +4 u32 entry-point map offset (from the
    file start), +8 u32 entry-point count, video: +12 width / 16, +13 height / 16
    entry-point map: 10 bytes per random access point: u8 0, u8 0, u32 PTS of its IDR picture (90 kHz),
    u32 index of its 2048-byte pack counted from the stream offset (the layout PPSSPP's scePsmf reads)
add_entry_points() writes that map into a movie that has none (see encode(epmap=...)).
"""
import os
import shutil
import struct
import subprocess
import tempfile

from .spec import (PbpError, Issue, ICON1_W, ICON1_H, ICON1_FPS_NUM, ICON1_FPS_DEN, ICON1_DEFAULT_SECONDS,
                   MEDIA_BUDGET)

PACK = 2048
EP_STRIDE = 10
# accurate_rnd: the scaler's exact C path (an ffmpeg build with inline assembly but without the matching vertical
# filter wrote garbage pictures without it); the content check below catches any such build
_FIT = {
    'cover': 'scale=%d:%d:force_original_aspect_ratio=increase:flags={f},crop=%d:%d' % (
        ICON1_W, ICON1_H, ICON1_W, ICON1_H),
    'contain': 'scale=%d:%d:force_original_aspect_ratio=decrease:flags={f},pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black' % (
        ICON1_W, ICON1_H, ICON1_W, ICON1_H),
    'stretch': 'scale=%d:%d:flags={f}' % (ICON1_W, ICON1_H),
}
FIT_FILTERS = {k: v.replace('{f}', 'lanczos+accurate_rnd') for k, v in _FIT.items()}
FIT_RGB = {k: v.replace('{f}', 'bicubic') for k, v in _FIT.items()}          # the content check's reference
X264 = ':'.join([
    'ref=1', 'bframes=0', 'weightp=0', 'cabac=1', '8x8dct=0', 'slices=1',
    'keyint=30', 'min-keyint=30', 'scenecut=0',      # an IDR picture every 30 frames
    'aud=1', 'repeat-headers=1',                      # delimiter per picture, SPS/PPS before every IDR
    'nal-hrd=vbr', 'pic-struct=1', 'force-cfr=1', 'sar=1/1', 'threads=1',
])
BITEXACT = ['-fflags', '+bitexact', '-flags', '+bitexact', '-map_metadata', '-1']
STILL_EXT = ('.png', '.jpg', '.jpeg', '.bmp', '.tga', '.webp')


# ------------------------------------------------------------------------------------------ checking a ready file
def parse(data, what='ICON1.PMF'):
    data = bytes(data)
    if len(data) < 0x800 or data[:4] != b'PSMF' or not data[4:8].isdigit():
        raise PbpError('%s: not a PSMF movie (it must start with "PSMF" and 4 digits)' % what)
    offset, size = struct.unpack('>II', data[8:16])
    first = int.from_bytes(data[0x54:0x5A], 'big')
    last = int.from_bytes(data[0x5A:0x60], 'big')
    n = struct.unpack('>H', data[0x80:0x82])[0]
    streams = []
    for i in range(min(n, 16)):
        e = data[0x82 + 16 * i:0x92 + 16 * i]
        if len(e) < 16:
            break
        s = {'id': e[0], 'entry': 0x82 + 16 * i}
        s['ep_offset'], s['ep_count'] = struct.unpack('>II', e[4:12])
        if 0xE0 <= e[0] <= 0xEF:
            s.update(kind='video', width=e[12] * 16, height=e[13] * 16)
        elif e[0] == 0xBD:
            s.update(kind='audio')
        else:
            s.update(kind='other')
        streams.append(s)
    return {'version': data[4:8].decode('ascii'), 'offset': offset, 'stream_size': size, 'file_size': len(data),
            'seconds': (last - first) / 90000.0 if last > first else None, 'streams': streams}


def check(data, what='ICON1.PMF'):
    try:
        m = parse(data, what)
    except PbpError as e:
        return [Issue('error', what, str(e).split(': ', 1)[-1])]
    out = []
    if m['offset'] != 0x800 or m['offset'] + m['stream_size'] != m['file_size']:
        out.append(Issue('error', what, 'header says %d + %d bytes, the file has %d (cut off or damaged)' % (
            m['offset'], m['stream_size'], m['file_size'])))
    video = [s for s in m['streams'] if s['kind'] == 'video']
    if len(video) != 1:
        out.append(Issue('error', what, '%d video streams, must be 1' % len(video)))
    elif (video[0]['width'], video[0]['height']) != (ICON1_W, ICON1_H):
        out.append(Issue('spec', what, 'picture %dx%d, must be %dx%d (give the source video instead: the packer '
                                       're-encodes it)' % (video[0]['width'], video[0]['height'], ICON1_W, ICON1_H)))
    if any(s['kind'] == 'audio' for s in m['streams']):
        out.append(Issue('info', what, 'has a sound stream (when SND0.AT3 is present too, only SND0 is expected '
                                       'to play)'))
    ep = video[0]['ep_count'] if len(video) == 1 else 0
    out.append(Issue('info', what, 'PSMF %s, %s, %d bytes, %s' % (
        m['version'], '%.2f s' % m['seconds'] if m['seconds'] else 'length unknown', m['file_size'],
        '%d entry points' % ep if ep else 'no entry-point map')))
    return out


def entry_points(data):
    """-> [(PTS, pack index)] of the video's entry-point map (empty when it has none)"""
    m = parse(data)
    video = [s for s in m['streams'] if s['kind'] == 'video']
    if not video or not video[0]['ep_count']:
        return []
    o, n = video[0]['ep_offset'], video[0]['ep_count']
    return [struct.unpack('>II', data[o + EP_STRIDE * i + 2:o + EP_STRIDE * (i + 1)]) for i in range(n)]


def _pes_pts(b):
    return (((b[0] >> 1) & 7) << 30) | ((((b[1] << 8) | b[2]) >> 1) << 15) | (((b[3] << 8) | b[4]) >> 1)


def add_entry_points(data, version='0012'):
    """A PSMF movie with one video stream and no entry-point map -> the same movie with a map (one entry per pack
    that starts with a system header, i.e. per random access point / IDR picture) and the given version digits.
    The stream bytes are not touched; only the 2048-byte header changes."""
    data = bytearray(data)
    m = parse(bytes(data))
    video = [s for s in m['streams'] if s['kind'] == 'video']
    if len(video) != 1:
        raise PbpError('ICON1: entry points need exactly one video stream (%d found)' % len(video))
    v = video[0]
    if v['ep_count']:
        raise PbpError('ICON1: the movie already has an entry-point map (%d entries)' % v['ep_count'])
    entries = []
    for k in range(m['stream_size'] // PACK):
        pk = data[m['offset'] + k * PACK:m['offset'] + (k + 1) * PACK]
        if pk[14:18] != b'\x00\x00\x01\xbb':           # pack header (14 bytes), then a system header = RAP pack
            continue
        i = pk.find(b'\x00\x00\x01' + bytes([v['id']]), 18)
        if i < 0 or not pk[i + 7] & 0x80:
            raise PbpError('ICON1: random access pack %d has no video PES with a time stamp' % k)
        entries.append((_pes_pts(pk[i + 9:i + 14]), k))
    if not entries or entries[0][1] != 0:
        raise PbpError('ICON1: the stream does not start with a random access point')
    where = 0x82 + 16 * len(m['streams'])
    if where + EP_STRIDE * len(entries) > m['offset']:
        raise PbpError('ICON1: %d entry points do not fit the header (use fewer IDR pictures)' % len(entries))
    if any(data[where:where + EP_STRIDE * len(entries)]):
        raise PbpError('ICON1: the header space after the stream table is not empty')
    for n, (pts, k) in enumerate(entries):
        data[where + EP_STRIDE * n:where + EP_STRIDE * (n + 1)] = struct.pack('>BBII', 0, 0, pts, k)
    struct.pack_into('>II', data, v['entry'] + 4, where, len(entries))
    if version:
        if not (len(version) == 4 and version.isdigit()):
            raise PbpError('ICON1: version must be 4 digits, not %r' % version)
        data[4:8] = version.encode('ascii')
    return bytes(data)


# ------------------------------------------------------------------------------------------ external tools
def find_ffmpeg(given=None):
    for cand in (given, os.environ.get('FFMPEG')):
        if cand:
            if os.path.isfile(cand):
                return cand
            raise PbpError('ffmpeg not found at %s' % cand)
    p = shutil.which('ffmpeg')
    if not p:
        raise PbpError('making ICON1.PMF from a video needs ffmpeg with libx264 (https://ffmpeg.org). Install it, '
                       'then put it on PATH, set FFMPEG, or pass --ffmpeg PATH. Or give a ready 144x80 .pmf file.')
    return p


def find_muxer():
    """-> the PSMF muxer module (ps2rc.movies.pmfmux)"""
    from ..movies import pmfmux
    return pmfmux


def _hidden():
    """no console window on Windows (the converter window has none: ffmpeg would open its own)"""
    from .. import proc
    return proc.hidden()


def _run(cmd, stdin=None):
    r = subprocess.run(cmd, input=stdin, capture_output=True, **_hidden())
    if r.returncode:
        tail = (r.stderr or b'').decode('utf-8', 'replace').strip().splitlines()[-6:]
        raise PbpError('ffmpeg failed (exit %d): %s' % (r.returncode, ' | '.join(tail)))
    return r


def _has_x264(ffmpeg):
    r = subprocess.run([ffmpeg, '-hide_banner', '-encoders'], capture_output=True, **_hidden())
    return b'libx264' in (r.stdout or b'')


def _picture_check(ffmpeg, pmf, work, frames, src, fit, fps, seconds):
    """decode the made icon and compare it with its input (ps2rc.movies.framecheck): a faulty ffmpeg build fails
    here instead of putting garbage into the menu"""
    from ..movies import framecheck
    path = os.path.join(work, 'icon1_check.pmf')
    with open(path, 'wb') as f:
        f.write(pmf)
    try:
        if frames is not None:
            return framecheck.check(ffmpeg, path, ICON1_W, ICON1_H, reference=frames[:5], what='ICON1')
        ref = framecheck.rgb_reference_filter(FIT_RGB[fit], pre='fps=%s,' % fps)
        ts = [0.2, round(max(0.2, (seconds or 1.0) * 0.5), 2)]
        return framecheck.check(ffmpeg, path, ICON1_W, ICON1_H, source=src, ref_vf=ref, times=ts, what='ICON1')
    except framecheck.FrameCheckError as e:
        raise PbpError(str(e))


# ------------------------------------------------------------------------------------------ making one
def encode(src=None, frames=None, budget=MEDIA_BUDGET, max_seconds=ICON1_DEFAULT_SECONDS, fit='cover',
           ffmpeg=None, muxer=None, work_dir=None, attempts=6, epmap=False):
    """-> (pmf_bytes, report). src = a video / animated GIF path; or frames = list of 144x80 RGB24 frames
    (29.97 fps). The bit rate starts from the budget and is lowered until the movie fits it. epmap=True adds an
    entry-point map and writes version 0012 (add_entry_points; the size does not change)."""
    if (src is None) == (frames is None):
        raise PbpError('ICON1: give a source video or a list of frames')
    if fit not in FIT_FILTERS:
        raise PbpError('fit must be one of %s, not %r' % (', '.join(FIT_FILTERS), fit))
    if src is not None:
        if not os.path.isfile(src):
            raise PbpError('ICON1: %s not found' % src)
        if os.path.splitext(src)[1].lower() in STILL_EXT:
            raise PbpError('ICON1: %s is a still picture; ICON1 is an animation (video or animated GIF). A still '
                           'icon belongs in ICON0.' % src)
    if budget < 8 * 1024:
        raise PbpError('ICON1: only %d bytes left of the ICON1 + SND0 budget; use a shorter or smaller SND0.AT3 '
                       'or no ICON1' % budget)
    ffmpeg = find_ffmpeg(ffmpeg)
    if not _has_x264(ffmpeg):
        raise PbpError('this ffmpeg (%s) has no libx264 encoder; install a build with libx264' % ffmpeg)
    mux = muxer if muxer is not None else find_muxer()
    fps = '%d/%d' % (ICON1_FPS_NUM, ICON1_FPS_DEN)
    if frames is not None:
        frame_bytes = ICON1_W * ICON1_H * 3
        bad = [i for i, f in enumerate(frames) if len(f) != frame_bytes]
        if bad or not frames:
            raise PbpError('ICON1: frames must be %dx%d RGB24 (%d bytes each); frame %s is not' % (
                ICON1_W, ICON1_H, frame_bytes, bad[0] if bad else 0))
        n = min(len(frames), int(max_seconds * ICON1_FPS_NUM / ICON1_FPS_DEN + 0.5))
        seconds = n * ICON1_FPS_DEN / float(ICON1_FPS_NUM)
        source = ['-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '%dx%d' % (ICON1_W, ICON1_H), '-r', fps, '-i', '-']
        vf = 'setsar=1,format=yuv420p'
        stdin = b''.join(frames[:n])
        src_name = '%d generated frames' % n
    else:
        source = ['-t', '%.3f' % max_seconds, '-i', src]
        vf = 'fps=%s,%s,setsar=1,format=yuv420p' % (fps, FIT_FILTERS[fit])
        stdin = None
        seconds = max_seconds
        src_name = src
    own_tmp = work_dir is None
    work = tempfile.mkdtemp(prefix='icon1_') if own_tmp else work_dir
    os.makedirs(work, exist_ok=True)
    es_path = os.path.join(work, 'icon1.264')
    pmf_path = os.path.join(work, 'icon1.pmf')
    target = budget * 0.92
    kbps = min(3000, max(16, int(target * 8 / 1000.0 / max(seconds, 0.5))))
    tries, best, raised = [], None, False
    try:
        for _ in range(attempts):
            maxrate = min(3000, max(2 * kbps, 64))
            bufsize = min(1875, maxrate)
            x264 = X264 + ':vbv-maxrate=%d:vbv-bufsize=%d' % (maxrate, bufsize)
            cmd = ([ffmpeg, '-v', 'error', '-y'] + source +
                   ['-map', '0:v:0', '-an', '-vf', vf, '-sws_flags', 'lanczos+accurate_rnd',   # also the inserted
                    '-c:v', 'libx264', '-profile:v', 'main', '-level:v', '2.1',                   # rgb24 -> yuv420p
                    '-preset', 'slow', '-b:v', '%dk' % kbps, '-x264-params', x264] + BITEXACT + ['-f', 'h264',
                                                                                              es_path])
            _run(cmd, stdin)
            with open(es_path, 'rb') as f:
                es = f.read()
            if not es:
                raise PbpError('ICON1: ffmpeg produced no video from %s' % src_name)
            try:
                stats = mux.write_checked(es, pmf_path, vbv_kbit=bufsize)
            except Exception as e:      # the muxer raises its own MuxError for a stream it cannot use
                raise PbpError('ICON1: the PSMF muxer refused the encoded stream: %s' % e)
            size = os.path.getsize(pmf_path)
            seconds = stats.get('seconds', seconds)
            tries.append((kbps, size))
            if size <= budget:
                with open(pmf_path, 'rb') as f:
                    pmf = f.read()
                if epmap:
                    pmf = add_entry_points(pmf)
                best = (pmf, {'source': src_name, 'kbps': kbps, 'bytes': size, 'seconds': seconds,
                              'frames': stats.get('access_units'), 'tries': tries, 'budget': budget,
                              'entry_points': len(entry_points(pmf))})
                # a clip shorter than assumed leaves room: raise the rate once, keep this result as the fallback
                if size >= 0.7 * target or raised or kbps >= 3000:
                    break
                raised = True
                kbps = min(3000, int(kbps * target / size))
                continue
            if best is not None:
                break
            kbps = int(kbps * target / size)
            if kbps < 16:
                break
        if best is None:
            raise PbpError('ICON1: cannot fit %s into %d bytes (tries kbit/s -> bytes: %s); use a shorter clip '
                           '(--icon1-seconds) or a smaller SND0.AT3' % (src_name, budget, tries))
        best[1]['picture_check'] = _picture_check(ffmpeg, best[0], work, frames, src, fit, fps, best[1]['seconds'])
        return best
    finally:
        if own_tmp:
            shutil.rmtree(work, ignore_errors=True)
