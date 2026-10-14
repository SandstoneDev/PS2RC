"""ICON0 / PIC0 / PIC1 from any picture, through a crop box.

A crop box is (x0, y0, x1, y1) in the SOURCE picture's pixels, with the target's aspect ratio (144:80, 310:180,
480:272). The default box is the largest centred one ("cover": fills the slot, cuts the overflow). The box is
scaled to the slot size; PIC0 keeps transparency, ICON0 and PIC1 are flattened onto `background` (24-bit RGB).
An exact PNG without a box is used byte for byte (ps2rc.pbp.image.prepare).
"""
import io
import struct

from ps2rc.pbp import image as pimage
from ps2rc.pbp.spec import PbpError, IMAGES

SIZES = {k: (s.width, s.height) for k, s in IMAGES.items()}


def pillow():
    try:
        from PIL import Image
        return Image
    except ImportError:
        return None


def need_pillow(what):
    raise PbpError("%s: Pillow is needed for this picture (python -m pip install pillow); the converter app ships "
                   "it" % what)


def open_picture(src):
    """-> a Pillow RGBA image (first frame of an animation). src: path or bytes."""
    Image = pillow()
    if Image is None:
        need_pillow("picture")
    try:
        im = Image.open(io.BytesIO(src) if isinstance(src, (bytes, bytearray)) else src)
        if getattr(im, "is_animated", False):
            im.seek(0)
        im.load()
    except PbpError:
        raise
    except Exception as e:          # Pillow raises unrelated types for unreadable files
        raise PbpError("cannot read the picture %s (%s)" % ("" if isinstance(src, (bytes, bytearray)) else src, e))
    return im.convert("RGBA")


def default_box(src_w, src_h, out_w, out_h):
    """largest centred box with the out_w:out_h aspect inside the source"""
    if src_w * out_h >= src_h * out_w:          # source wider than the target: cut left/right
        bw, bh = src_h * out_w / out_h, src_h
    else:
        bw, bh = src_w, src_w * out_h / out_w
    x0, y0 = (src_w - bw) / 2.0, (src_h - bh) / 2.0
    return (round(x0), round(y0), round(x0 + bw), round(y0 + bh))


def clamp_box(box, src_w, src_h, out_w, out_h, min_w=8):
    """keep the aspect, stay inside the source, at least min_w px wide. -> int box"""
    x0, y0, x1, y1 = (float(v) for v in box)
    w = max(min_w, min(x1 - x0, src_w, src_h * out_w / out_h))
    h = w * out_h / out_w
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    x0 = min(max(0.0, cx - w / 2), src_w - w)
    y0 = min(max(0.0, cy - h / 2), src_h - h)
    return (round(x0), round(y0), round(x0 + w), round(y0 + h))


KEEP_CHUNKS = (b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND")


def strip_png(data):
    """-> (PNG without text / colour-profile / time / EXIF chunks, bytes removed). The picture is unchanged."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return data, 0
    out, pos = [data[:8]], 8
    while pos + 8 <= len(data):
        n, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 12 + n
        if ctype in KEEP_CHUNKS:
            out.append(data[pos:end])
        pos = end
    clean = b"".join(out)
    return clean, len(data) - len(clean)


def render(src, key, box=None, fit="cover", background=(0, 0, 0)):
    """-> (png_bytes, notes) for slot key 'icon0' | 'pic0' | 'pic1'. Always the slot's exact size and depth (ICON0 /
    PIC1 24-bit RGB without alpha, PIC0 32-bit RGBA), compressed, without metadata chunks."""
    data, notes = _render(src, key, box, fit, background)
    data, cut = strip_png(data)
    if cut:
        notes = notes + ["%s: %d bytes of metadata removed" % (IMAGES[key].slot, cut)]
    return data, notes


def _render(src, key, box, fit, background):
    spec = IMAGES[key]
    if box is None:
        return pimage.prepare(src, spec, fit=fit, background=background)
    Image = pillow()
    if Image is None:
        need_pillow(spec.slot)
    im = open_picture(src)
    W, H = spec.width, spec.height
    b = clamp_box(box, im.width, im.height, W, H, min_w=1)
    out = im.crop(b).resize((W, H), Image.LANCZOS)
    if spec.mode == "RGB":
        flat = Image.new("RGBA", (W, H), tuple(background) + (255,))
        flat.alpha_composite(out)
        out = flat.convert("RGB")
    buf = io.BytesIO()
    out.save(buf, "PNG", optimize=True)
    data = buf.getvalue()
    probs = [i for i in pimage.check(data, spec) if i.level in ("error", "spec")]
    if probs:
        raise PbpError("%s: internal: the converted picture fails its own check: %s" % (spec.slot, probs[0]))
    return data, ["%s: %dx%d, crop %s -> %dx%d %s" % (spec.slot, im.width, im.height, b, W, H, spec.mode)]
