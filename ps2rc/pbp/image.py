"""Still pictures for the menu: ICON0.PNG, PIC0.PNG, PIC1.PNG.

A picture that already has the exact size and pixel format is used byte for byte. Anything else is converted with
Pillow (an optional dependency: `python -m pip install pillow`): any format Pillow reads, resized to the exact size
and written as an 8-bit RGB (ICON0, PIC1) or RGBA (PIC0) PNG. Without Pillow the packer says what is wrong and how to
fix it by hand.

fit = 'cover'   scale until the picture covers the whole target, crop the overflow evenly (default);
      'contain' scale until the whole picture fits, fill the rest (transparent for RGBA, `background` for RGB);
      'stretch' scale both axes to the target (changes the aspect ratio).
"""
import io

from . import png
from .spec import PbpError, Issue

FITS = ('cover', 'contain', 'stretch')


def check(data, spec, allow_alpha=False):
    """issues of a PNG against an ImageSpec (no conversion)"""
    what = spec.slot
    try:
        i = png.info(data, what)
    except PbpError as e:
        return [Issue('error', what, str(e).split(': ', 1)[-1])]
    out = []
    if (i['width'], i['height']) != (spec.width, spec.height):
        out.append(Issue('spec', what, '%dx%d pixels, must be %dx%d' % (i['width'], i['height'], spec.width,
                                                                       spec.height)))
    modes = (spec.mode, 'RGBA') if allow_alpha else (spec.mode,)
    if i['bit_depth'] != 8 or i['mode'] not in modes:
        out.append(Issue('spec', what, '%d-bit %s (%d bits per pixel), must be 8-bit %s (%d bits per pixel)' % (
            i['bit_depth'], i['mode'], i['bits_per_pixel'], spec.mode, spec.bits)))
    elif i['mode'] != spec.mode:
        out.append(Issue('warn', what, 'RGBA with transparency; the documented format is %d-bit %s and how the '
                                       'menu draws the alpha channel here is Unknown' % (spec.bits, spec.mode)))
    if i['interlace']:
        out.append(Issue('spec', what, 'interlaced PNG; write it non-interlaced'))
    return out


def _pillow():
    try:
        from PIL import Image
    except ImportError:
        return None
    return Image


def prepare(src, spec, fit='cover', background=(0, 0, 0), keep_alpha=False):
    """src: bytes or a path. -> (png_bytes, list of note strings). PbpError when it cannot be made valid."""
    if fit not in FITS:
        raise PbpError('fit must be one of %s, not %r' % (', '.join(FITS), fit))
    if not isinstance(src, (bytes, bytearray)):
        name = str(src)
        try:
            with open(src, 'rb') as f:
                data = f.read()
        except OSError as e:
            raise PbpError('%s: cannot read %s (%s)' % (spec.slot, name, e.strerror or e))
    else:
        name, data = 'the given data', bytes(src)
    want_mode = 'RGBA' if keep_alpha else spec.mode
    if png.is_png(data):
        found = check(data, spec, allow_alpha=keep_alpha)
        if not [i for i in found if i.level in ('error', 'spec')]:
            return data, ['%s: %s used as is (%dx%d %s)' % (spec.slot, name, spec.width, spec.height,
                                                            png.info(data)['mode'])]
        damaged = [i for i in found if i.level == 'error']
        if damaged:
            raise PbpError('%s: %s: %s' % (spec.slot, name, damaged[0].message))
    Image = _pillow()
    if Image is None:
        why = '; '.join(i.message for i in check(data, spec)) if png.is_png(data) else 'not a PNG file'
        raise PbpError('%s: %s is not usable as is (%s). Install Pillow (python -m pip install pillow) so the '
                       'packer can convert it, or save it yourself as a %dx%d %d-bit %s PNG.' % (
                           spec.slot, name, why, spec.width, spec.height, spec.bits, spec.mode))
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
    except Exception as e:      # Pillow raises several unrelated exception types for unreadable files
        raise PbpError('%s: %s is not a picture Pillow can read (%s)' % (spec.slot, name, e))
    if getattr(im, 'is_animated', False):
        im.seek(0)
    src_w, src_h = im.size
    im = im.convert('RGBA')
    W, H = spec.width, spec.height
    if fit == 'stretch':
        out = im.resize((W, H), Image.LANCZOS)
        how = 'stretched'
    elif fit == 'cover':
        s = max(W / src_w, H / src_h)
        sw, sh = max(W, round(src_w * s)), max(H, round(src_h * s))
        big = im.resize((sw, sh), Image.LANCZOS)
        x0, y0 = (sw - W) // 2, (sh - H) // 2
        out = big.crop((x0, y0, x0 + W, y0 + H))
        how = 'scaled to cover, cropped %d px left/right, %d px top/bottom' % (x0, y0)
    else:
        s = min(W / src_w, H / src_h)
        sw, sh = max(1, min(W, round(src_w * s))), max(1, min(H, round(src_h * s)))
        small = im.resize((sw, sh), Image.LANCZOS)
        out = Image.new('RGBA', (W, H), (0, 0, 0, 0) if want_mode == 'RGBA' else tuple(background) + (255,))
        out.paste(small, ((W - sw) // 2, (H - sh) // 2), small)
        how = 'scaled to fit, bars %d px left/right, %d px top/bottom' % ((W - sw) // 2, (H - sh) // 2)
    if want_mode == 'RGB':
        flat = Image.new('RGBA', (W, H), tuple(background) + (255,))
        flat.alpha_composite(out)
        out = flat.convert('RGB')
    buf = io.BytesIO()
    out.save(buf, 'PNG', optimize=True)
    result = buf.getvalue()
    probs = [i for i in check(result, spec, allow_alpha=keep_alpha) if i.level in ('error', 'spec')]
    if probs:   # cannot happen unless Pillow changed its PNG writer; never pack a file we would refuse
        raise PbpError('%s: internal: the converted picture fails its own check: %s' % (spec.slot, probs[0]))
    return result, ['%s: %s %dx%d -> %dx%d %s (%s)' % (spec.slot, name, src_w, src_h, W, H, want_mode, how)]
