"""ICON1 (animated menu icon) from a clip: GIF / MP4 / WebM / any video ffmpeg reads.

    frames(src, start, seconds, box)  -> list of 144x80 RGB24 frames at 29.97 fps (also the GUI preview)
    make_icon1(src, start, seconds, box, budget) -> (PMF bytes, report)   (ps2rc.pbp.icon1.encode on those frames)

The clip is cut to [start, start + seconds] (seconds <= MAX_SECONDS), the crop box (source pixels, 144:80 aspect,
default = the centred "cover" box) is scaled to 144x80. Without ffmpeg an animated GIF still gives preview frames
(Pillow); making the PMF always needs ffmpeg with libx264 (H.264 encoder).
"""
from ps2rc.pbp import icon1 as picon1
from ps2rc.pbp.spec import PbpError, ICON1_W, ICON1_H, ICON1_FPS_NUM, ICON1_FPS_DEN, MEDIA_BUDGET

from . import media
from .picture import default_box, clamp_box, pillow

MAX_SECONDS = 10.0
DEFAULT_SECONDS = 3.0
FRAME_BYTES = ICON1_W * ICON1_H * 3
FPS = ICON1_FPS_NUM / float(ICON1_FPS_DEN)


def _box(box, w, h):
    if not w or not h:
        return None
    return clamp_box(box, w, h, ICON1_W, ICON1_H, min_w=1) if box else default_box(w, h, ICON1_W, ICON1_H)


def frames(src, start=0.0, seconds=DEFAULT_SECONDS, box=None, ffmpeg=None, info=None):
    seconds = max(0.1, min(float(seconds), MAX_SECONDS))
    start = max(0.0, float(start or 0))
    ff = media.find_ffmpeg(ffmpeg)
    if ff:
        info = info or media.probe(src, ff)
        if not info.get("video"):
            raise PbpError("%s: no picture stream (not a clip)" % src)
        b = _box(box, info["width"], info["height"])
        crop = "crop=%d:%d:%d:%d," % (b[2] - b[0], b[3] - b[1], b[0], b[1]) if b else ""
        vf = "%sscale=%d:%d:flags=lanczos+accurate_rnd,setsar=1,fps=%d/%d" % (crop, ICON1_W, ICON1_H, ICON1_FPS_NUM,
                                                                  ICON1_FPS_DEN)
        loop = ["-stream_loop", "-1"] if str(src).lower().endswith((".gif", ".webp", ".apng")) else []  # GIFs loop
        cmd = [ff, "-v", "error"] + loop + ["-ss", "%.3f" % start, "-t", "%.3f" % seconds, "-i", src, "-an", "-vf", vf,
               "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
        raw = media.run(cmd).stdout
        out = [raw[i:i + FRAME_BYTES] for i in range(0, len(raw) - FRAME_BYTES + 1, FRAME_BYTES)]
        if not out:
            raise PbpError("%s: no frames between %.1f s and %.1f s" % (src, start, start + seconds))
        return out
    return _gif_frames(src, start, seconds, box)


def _gif_frames(src, start, seconds, box):
    Image = pillow()
    if Image is None:
        raise PbpError("%s: ffmpeg is needed to read this clip" % src)
    try:
        im = Image.open(src)
        n = getattr(im, "n_frames", 1)
        times, t = [], 0.0
        for i in range(n):
            im.seek(i)
            times.append(t)
            t += (im.info.get("duration", 100) or 100) / 1000.0
    except Exception as e:      # Pillow raises unrelated types for unreadable files
        raise PbpError("%s: cannot read it without ffmpeg (%s)" % (src, e))
    total = t
    b = _box(box, im.width, im.height)
    out, cache = [], {}
    k = 0
    while k / FPS < seconds:
        tt = start + k / FPS
        if total:
            tt %= total                              # a GIF loops
        idx = max(i for i, s in enumerate(times) if s <= tt + 1e-9)
        if idx not in cache:
            im.seek(idx)
            fr = im.convert("RGBA").crop(b).resize((ICON1_W, ICON1_H), Image.LANCZOS)
            flat = Image.new("RGBA", fr.size, (0, 0, 0, 255))
            flat.alpha_composite(fr)
            cache[idx] = flat.convert("RGB").tobytes()
        out.append(cache[idx])
        k += 1
    return out


def make_icon1(src, start=0.0, seconds=DEFAULT_SECONDS, box=None, budget=MEDIA_BUDGET, ffmpeg=None, epmap=True,
               frames_given=None):
    """-> (pmf_bytes, report). epmap=True: version 0012 + an entry-point map (the layout of the PSMF icons we
    measured); whether the menu needs it is Unknown."""
    ff = media.find_ffmpeg(ffmpeg)
    if not ff:
        raise PbpError("the animated icon needs ffmpeg with libx264 (the converter app ships it as a separate "
                       "program in tools/ffmpeg; or install it and put it on PATH)")
    fr = frames_given if frames_given is not None else frames(src, start, seconds, box, ff)
    pmf, rep = picon1.encode(frames=fr, budget=budget, max_seconds=min(MAX_SECONDS, len(fr) / FPS + 0.01),
                             ffmpeg=ff, epmap=epmap)
    rep["start"], rep["source"] = start, src
    return pmf, rep


def still(src, at=0.0, ffmpeg=None):
    """-> PNG bytes of the clip's picture at `at` seconds, full size (for the crop box editor)."""
    ff = media.find_ffmpeg(ffmpeg)
    if ff:
        cmd = [ff, "-v", "error", "-ss", "%.3f" % max(0.0, at), "-i", src, "-frames:v", "1", "-f", "image2pipe",
               "-vcodec", "png", "-"]
        data = media.run(cmd).stdout
        if data:
            return data
        raise PbpError("%s: no picture at %.1f s" % (src, at))
    Image = pillow()
    if Image is None:
        raise PbpError("%s: ffmpeg is needed to read this clip" % src)
    import io
    try:
        im = Image.open(src)
        im.seek(0)
        buf = io.BytesIO()
        im.convert("RGBA").save(buf, "PNG")
        return buf.getvalue()
    except Exception as e:      # Pillow raises unrelated types for unreadable files
        raise PbpError("%s: cannot read it without ffmpeg (%s)" % (src, e))
