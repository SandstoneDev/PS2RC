"""Content check of a converted movie / animated icon: decode a few pictures of the result and compare them with the
same pictures made from the source along a path that does not use the encoder's planar scaler.

Why: a broken ffmpeg build (an inline-assembly scaler without its matching vertical filter) once produced pictures
of green garbage while every count and size still looked right. The reference pictures are made in RGB (the source
is turned into RGB24 at its own size first, then scaled in RGB), the result is only decoded to RGB24 at its own size,
so a fault of the YUV scaler shows as a large difference.

The comparison is a PSNR of 8x8 block means (block PSNR): encoding noise and a small timing offset average out,
garbage does not. A good conversion measures far above THRESHOLD_DB, a broken one far below it.

Used by convert_movies_open.py (imported as a sibling module) and ps2rc.pbp.icon1 (as ps2rc.movies.framecheck);
plain Python, no package imports.
"""
import math
import os
import subprocess

THRESHOLD_DB = 25.0
BLOCK = 8


def _hidden():
    if os.name != 'nt':
        return {}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0
    return {'creationflags': 0x08000000, 'startupinfo': si}


class FrameCheckError(RuntimeError):
    pass


def rgb_frames(ffmpeg, path, w, h, n=1, start=None, vf=None, before=()):
    """-> up to n RGB24 pictures (bytes, w*h*3 each) of the first video stream. vf: a filter chain that must end at
    w x h; without it the stream must already be w x h (decoded only, converted to RGB at its own size)."""
    cmd = [ffmpeg, '-v', 'error', '-nostdin'] + list(before) + ['-i', path, '-map', '0:v:0', '-an']
    if start is not None:
        cmd += ['-ss', '%.3f' % start]            # after -i: decoded from the start (no seek into a group of pictures)
    cmd += ['-frames:v', str(n)]
    if vf:
        cmd += ['-vf', vf]
    cmd += ['-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **_hidden())
    if r.returncode:
        tail = (r.stderr or b'').decode('utf-8', 'replace').strip().splitlines()[-4:]
        raise FrameCheckError('ffmpeg could not decode %s for the picture check: %s' % (path, ' | '.join(tail)))
    size = w * h * 3
    raw = r.stdout
    return [raw[i:i + size] for i in range(0, len(raw) - size + 1, size)]


def block_means(rgb, w, h, block=BLOCK):
    """8x8 block means per colour channel -> list of floats"""
    bw, bh = w // block, h // block
    out = []
    row = w * 3
    for by in range(bh):
        for bx in range(bw):
            s = [0, 0, 0]
            for y in range(by * block, by * block + block):
                o = y * row + bx * block * 3
                px = rgb[o:o + block * 3]
                s[0] += sum(px[0::3])
                s[1] += sum(px[1::3])
                s[2] += sum(px[2::3])
            n = float(block * block)
            out += [s[0] / n, s[1] / n, s[2] / n]
    return out


def block_psnr(a, b, w, h, block=BLOCK):
    """PSNR (dB) of the 8x8 block means of two RGB24 pictures of the same size"""
    ma, mb = block_means(a, w, h, block), block_means(b, w, h, block)
    mse = sum((x - y) * (x - y) for x, y in zip(ma, mb)) / max(1, len(ma))
    return 99.0 if mse <= 1e-9 else 10.0 * math.log10(255.0 * 255.0 / mse)


def rgb_reference_filter(scale_filter, pre=''):
    """the reference chain: RGB24 at the source size first, then the same geometry (scale / pad / crop) in RGB"""
    return '%sformat=rgb24,%s' % (pre, scale_filter)


def check(ffmpeg, result, w, h, reference=None, source=None, ref_vf=None, times=(0.5,), what='movie',
          threshold=THRESHOLD_DB, before=()):
    """Compare pictures of `result` (decoded at w x h) with the reference: given RGB24 pictures (`reference`,
    indexed like the result's first pictures) or pictures of `source` through `ref_vf` at the same times.
    -> {'psnr': [dB per sample], 'best': dB}; raises FrameCheckError unless most samples reach threshold."""
    got = []
    if reference is not None:
        res = rgb_frames(ffmpeg, result, w, h, n=len(reference))
        for a, b in zip(res, reference):
            got.append(block_psnr(a, b, w, h))
    else:
        for t in times:
            a = rgb_frames(ffmpeg, result, w, h, n=1, start=t)
            b = rgb_frames(ffmpeg, source, w, h, n=1, start=t, vf=ref_vf, before=before)
            if a and b:
                got.append(block_psnr(a[0], b[0], w, h))
    if not got:
        raise FrameCheckError('%s: no pictures to compare (result %s)' % (what, result))
    best = max(got)
    good = sum(1 for g in got if g >= threshold)
    if good * 2 < len(got) or best < threshold:              # most samples must match (a scene cut may miss one)
        raise FrameCheckError('%s: the converted pictures do not match the source (block PSNR %s dB, needed %.0f): '
                              'the ffmpeg build is faulty (its scaler) - use another ffmpeg' %
                              (what, ', '.join('%.1f' % g for g in got), threshold))
    return {'psnr': [round(g, 1) for g in got], 'best': round(best, 1)}
