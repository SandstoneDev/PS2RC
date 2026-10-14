"""PNG reading (header + integrity) and a minimal writer, pure Python.

The reader checks what the menu needs to know without decoding pixels: size, bit depth, colour type, interlace,
chunk CRCs and the end chunk. The writer makes 8-bit RGB / RGBA files (used by the tests and the placeholder
pictures, so neither needs an imaging library). Converting other pictures needs Pillow (see image.py).
"""
import struct
import zlib

from .spec import PbpError

SIGNATURE = b'\x89PNG\r\n\x1a\n'
COLOR_TYPES = {0: 'L', 2: 'RGB', 3: 'P', 4: 'LA', 6: 'RGBA'}
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def is_png(data):
    return data[:8] == SIGNATURE


def info(data, what='PNG'):
    """-> dict(width, height, bit_depth, color_type, mode, interlace, bits_per_pixel); PbpError when damaged"""
    if not is_png(data):
        raise PbpError('%s: not a PNG file (no PNG signature)' % what)
    pos, first, ihdr, seen_end, seen_data = 8, True, None, False, False
    while pos < len(data):
        if pos + 12 > len(data):
            raise PbpError('%s: damaged PNG (a chunk is cut off at byte %d)' % (what, pos))
        length, ctype = struct.unpack('>I4s', data[pos:pos + 8])
        end = pos + 12 + length
        if end > len(data):
            raise PbpError('%s: damaged PNG (chunk %r runs past the end of the file)' % (what, ctype))
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack('>I', data[pos + 8 + length:end])[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise PbpError('%s: damaged PNG (CRC error in chunk %r)' % (what, ctype))
        if first:
            if ctype != b'IHDR' or length != 13:
                raise PbpError('%s: damaged PNG (the first chunk is not IHDR)' % what)
            ihdr = struct.unpack('>IIBBBBB', body)
            first = False
        if ctype == b'IDAT':
            seen_data = True
        if ctype == b'IEND':
            seen_end = True
            pos = end
            break
        pos = end
    if ihdr is None or not seen_end or not seen_data:
        raise PbpError('%s: damaged PNG (missing %s)' % (what, 'IHDR' if ihdr is None else
                                                         'image data' if not seen_data else 'end chunk'))
    w, h, depth, ctype, _comp, _filt, interlace = ihdr
    if ctype not in COLOR_TYPES or w == 0 or h == 0:
        raise PbpError('%s: damaged PNG (colour type %d, %dx%d)' % (what, ctype, w, h))
    return {'width': w, 'height': h, 'bit_depth': depth, 'color_type': ctype, 'mode': COLOR_TYPES[ctype],
            'interlace': interlace, 'bits_per_pixel': depth * CHANNELS[ctype], 'trailing_bytes': len(data) - pos}


def _chunk(ctype, body):
    return struct.pack('>I', len(body)) + ctype + body + struct.pack('>I', zlib.crc32(ctype + body) & 0xFFFFFFFF)


def encode(width, height, mode, pixels):
    """8-bit RGB or RGBA pixels (rows top to bottom, no padding) -> PNG bytes (no filter, zlib level 9)"""
    ch = {'RGB': 3, 'RGBA': 4}[mode]
    stride = width * ch
    if len(pixels) != stride * height:
        raise ValueError('pixel data %d bytes, expected %d' % (len(pixels), stride * height))
    raw = b''.join(b'\x00' + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2 if mode == 'RGB' else 6, 0, 0, 0)
    return SIGNATURE + _chunk(b'IHDR', ihdr) + _chunk(b'IDAT', zlib.compress(raw, 9)) + _chunk(b'IEND', b'')


def decode_rgb(data):
    """8-bit non-interlaced RGB/RGBA PNG -> (width, height, mode, pixels). Enough for tests and checks."""
    i = info(data)
    if i['bit_depth'] != 8 or i['mode'] not in ('RGB', 'RGBA') or i['interlace']:
        raise PbpError('decode_rgb: only 8-bit non-interlaced RGB/RGBA (this is %d-bit %s)' % (
            i['bit_depth'], i['mode']))
    pos, idat = 8, b''
    while pos < len(data):
        length, ctype = struct.unpack('>I4s', data[pos:pos + 8])
        if ctype == b'IDAT':
            idat += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw = zlib.decompress(idat)
    ch = 3 if i['mode'] == 'RGB' else 4
    w, h, stride = i['width'], i['height'], i['width'] * ch
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + a) & 0xFF
            elif f == 2:
                line[x] = (line[x] + b) & 0xFF
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        out += line
        prev = line
    return w, h, i['mode'], bytes(out)
