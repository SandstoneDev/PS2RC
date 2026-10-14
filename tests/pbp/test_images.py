"""PNG reader/writer and the still-picture validators / converters (ICON0, PIC0, PIC1)."""
import io
import struct
import zlib

import pytest

from ps2rc.pbp import ICON0, PIC0, PIC1, PbpError, image, png
from pbp_helpers import gradient, make_png


def levels(issues):
    return {i.level for i in issues}


@pytest.mark.parametrize('mode,ch', [('RGB', 3), ('RGBA', 4)])
def test_png_round_trip(mode, ch):
    px = gradient(37, 11, ch)
    data = png.encode(37, 11, mode, px)
    i = png.info(data)
    assert (i['width'], i['height'], i['bit_depth'], i['mode'], i['interlace']) == (37, 11, 8, mode, 0)
    assert i['bits_per_pixel'] == 8 * ch
    assert png.decode_rgb(data) == (37, 11, mode, px)


def test_png_decode_filters():
    pil = pytest.importorskip('PIL.Image')
    im = pil.new('RGB', (40, 20))
    im.putdata([(x * 6, y * 12, (x * y) & 255) for y in range(20) for x in range(40)])
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)               # Pillow chooses adaptive row filters
    assert png.decode_rgb(buf.getvalue())[3] == im.tobytes()


def _rechunk(data, ctype, body):
    """replace the body of the first chunk of type ctype (CRC recomputed)"""
    pos = 8
    while True:
        n, t = struct.unpack('>I4s', data[pos:pos + 8])
        if t == ctype:
            new = struct.pack('>I', len(body)) + t + body + struct.pack('>I', zlib.crc32(t + body) & 0xFFFFFFFF)
            return data[:pos] + new + data[pos + 12 + n:]
        pos += 12 + n


@pytest.mark.parametrize('damage', ['crc', 'cut', 'noend', 'sig'])
def test_png_damaged(damage):
    data = bytearray(make_png(20, 10))
    if damage == 'crc':
        data[45] ^= 0xFF            # inside the IDAT body: the chunk walk is intact, only the CRC differs
    elif damage == 'cut':
        data = data[:-20]
    elif damage == 'noend':
        data = data[:-12]
    else:
        data[1] = 0
    with pytest.raises(PbpError):
        png.info(bytes(data))
    assert 'error' in levels(image.check(bytes(data), ICON0))


def test_check_exact_files():
    assert not image.check(make_png(144, 80), ICON0)
    assert not image.check(make_png(480, 272), PIC1)
    assert not image.check(make_png(310, 180, 'RGBA'), PIC0)


def test_check_wrong_files():
    assert 'spec' in levels(image.check(make_png(145, 80), ICON0))
    assert 'spec' in levels(image.check(make_png(144, 80, 'RGBA'), ICON0))
    assert levels(image.check(make_png(144, 80, 'RGBA'), ICON0, allow_alpha=True)) == {'warn'}
    assert 'spec' in levels(image.check(make_png(310, 180), PIC0))       # PIC0 must carry alpha
    sixteen = _rechunk(make_png(144, 80), b'IHDR', struct.pack('>IIBBBBB', 144, 80, 16, 2, 0, 0, 0))
    assert 'spec' in levels(image.check(sixteen, ICON0))
    laced = _rechunk(make_png(144, 80), b'IHDR', struct.pack('>IIBBBBB', 144, 80, 8, 2, 0, 0, 1))
    assert 'spec' in levels(image.check(laced, ICON0))


def test_exact_file_kept_byte_for_byte(tmp_path):
    data = make_png(144, 80)
    out, notes = image.prepare(data, ICON0)
    assert out == data and 'as is' in notes[0]
    f = tmp_path / 'i.png'
    f.write_bytes(data)
    assert image.prepare(str(f), ICON0)[0] == data


def test_without_pillow_a_wrong_file_is_refused_with_a_hint(monkeypatch):
    monkeypatch.setattr(image, '_pillow', lambda: None)
    with pytest.raises(PbpError, match='Pillow'):
        image.prepare(make_png(200, 100), ICON0)
    assert image.prepare(make_png(144, 80), ICON0)[0] == make_png(144, 80)


def test_missing_file_is_a_clear_error(tmp_path):
    with pytest.raises(PbpError, match='cannot read'):
        image.prepare(str(tmp_path / 'nope.png'), ICON0)


# ------------------------------------------------------------------------------------------ with Pillow
def halves(w, h, left=(255, 0, 0), right=(0, 0, 255)):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes(left if x < w // 2 else right)
    return png.encode(w, h, 'RGB', bytes(px))


def pixel(data, x, y):
    w, h, mode, px = png.decode_rgb(data)
    ch = 3 if mode == 'RGB' else 4
    return tuple(px[(y * w + x) * ch:(y * w + x + 1) * ch])


def test_cover_crops_the_long_side():
    pytest.importorskip('PIL')
    src = halves(400, 80)                          # 5:1 -> cover 144x80 keeps the middle 144 of 400 columns
    out, notes = image.prepare(src, ICON0, fit='cover')
    i = png.info(out)
    assert (i['width'], i['height'], i['mode'], i['bit_depth']) == (144, 80, 'RGB', 8)
    assert pixel(out, 2, 40)[0] > 200 and pixel(out, 141, 40)[2] > 200      # red left, blue right
    assert 'cropped 128 px left/right' in notes[0]
    assert not [x for x in image.check(out, ICON0) if x.level in ('error', 'spec')]


def test_contain_adds_bars():
    pytest.importorskip('PIL')
    tall = png.encode(40, 80, 'RGB', b'\xff\xff\xff' * 40 * 80)
    out, _ = image.prepare(tall, ICON0, fit='contain', background=(0, 255, 0))
    assert pixel(out, 0, 40) == (0, 255, 0) and pixel(out, 72, 40) == (255, 255, 255)
    out, _ = image.prepare(tall, PIC0, fit='contain')                       # RGBA target: transparent bars
    assert png.info(out)['mode'] == 'RGBA' and pixel(out, 0, 90)[3] == 0 and pixel(out, 155, 90)[3] == 255


def test_stretch_and_alpha_flattening():
    pil = pytest.importorskip('PIL.Image')
    im = pil.new('RGBA', (100, 100), (255, 0, 0, 0))                       # fully transparent red
    buf = io.BytesIO()
    im.save(buf, 'PNG')
    out, _ = image.prepare(buf.getvalue(), PIC1, fit='stretch', background=(10, 20, 30))
    assert png.info(out)['mode'] == 'RGB' and pixel(out, 240, 136) == (10, 20, 30)
    out, _ = image.prepare(buf.getvalue(), ICON0, keep_alpha=True)
    assert png.info(out)['mode'] == 'RGBA'


def test_other_formats_are_converted():
    pil = pytest.importorskip('PIL.Image')
    buf = io.BytesIO()
    pil.new('RGB', (64, 64), (200, 100, 50)).save(buf, 'JPEG')
    out, notes = image.prepare(buf.getvalue(), PIC1)
    assert (png.info(out)['width'], png.info(out)['height']) == (480, 272)
    with pytest.raises(PbpError, match='not a picture'):
        image.prepare(b'certainly not a picture', PIC1)


def test_bad_fit_name():
    with pytest.raises(PbpError):
        image.prepare(make_png(144, 80), ICON0, fit='zoom')
