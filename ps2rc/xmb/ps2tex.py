"""Texture dictionaries on PS2 discs: the chunked binary-stream layout with PS2 (GS) rasters, read and decoded in pure
Python + Pillow (no numpy: the converter app has none). Used by disc_scan.py to offer the player's own pictures.

Layout (format facts, checked on discs by the tests):
* chunk = u32 type, u32 body size, u32 version, then the body; children follow one another inside a body.
* dictionary 0x16 { STRUCT 0x01 {u16 count, u16 device}, count x texture 0x15, EXTENSION 0x03 }.
* texture 0x15 { STRUCT {platform 'PS2\\0', u32 filter / addressing}, STRING 0x02 name, STRING mask,
  STRUCT { STRUCT (0x40-byte raster header), STRUCT (pixel block, then palette block) }, EXTENSION }.
* raster header: i32 width, height, depth; u16 raster format; i16 version; u64 TEX0; u32 palette offset; u32 TEX1
  low; u64 MIPTBP1, MIPTBP2; u32 pixel size, palette size, total size, mipmap value.
* pixel / palette block: per upload 0x50 bytes of GIF tags (A+D, NLOOP 3: TRXPOS 0x51, TRXREG 0x52, TRXDIR 0x53; then
  an IMAGE tag whose NLOOP counts quadwords) followed by the image data.
* PSMT8 / PSMT4 levels whose upload rectangle is half the level size are stored pre-swizzled (uploaded as PSMCT32 /
  PSMCT16); 256-entry CLUTs are in CSM1 order (index bits 3 and 4 swapped); PS2 alpha is 0..0x80.
* archive "VER2": u32 count, count x {u32 offset (2048-byte sectors), u16 size (sectors), u16 0, char name[24]}.
"""
import operator
import struct

STRUCT, STRING, EXTENSION, TEXTURE, DICTIONARY = 0x01, 0x02, 0x03, 0x15, 0x16
PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4 = 0x00, 0x01, 0x02, 0x0A, 0x13, 0x14
FMT_PIXEL_MASK, FMT_1555, FMT_8888, FMT_888 = 0x0F00, 0x100, 0x500, 0x600
FMT_PAL8, FMT_PAL4 = 0x2000, 0x4000
HDR = struct.Struct("<iiiHhQIIQQIIII")          # 0x40 bytes
SECTOR = 2048


def is_dictionary(head, size):
    """True when the first 16 bytes of a file of `size` bytes start a texture dictionary"""
    if len(head) < 16:
        return False
    t, s, _v, child = struct.unpack_from("<IIII", head, 0)
    return t == DICTIONARY and child == STRUCT and 16 < s and 12 + s <= size + SECTOR


def archive_entries(head):
    """[(name, byte offset, byte size)] of a "VER2" archive from its first bytes (None: not one / directory cut)"""
    if head[:4] != b"VER2" or len(head) < 8:
        return None
    n = struct.unpack_from("<I", head, 4)[0]
    if len(head) < 8 + 32 * n:
        return None
    out = []
    for i in range(n):
        off, size, _z = struct.unpack_from("<IHH", head, 8 + 32 * i)
        name = head[16 + 32 * i:40 + 32 * i].split(b"\0")[0].decode("latin1")
        out.append((name, off * SECTOR, size * SECTOR))
    return out


def archive_count(head):
    return struct.unpack_from("<I", head, 4)[0] if head[:4] == b"VER2" and len(head) >= 8 else 0


def chunks(data, o, end):
    """[(type, size, body offset)] of the chunks laid one after another in data[o:end]"""
    out = []
    while o + 12 <= end:
        t, s, _v = struct.unpack_from("<III", data, o)
        if o + 12 + s > end:
            break
        out.append((t, s, o + 12))
        o += 12 + s
    return out


class Upload:
    __slots__ = ("rrw", "rrh", "data_off", "data_len")


def _upload(blk, o):
    """the 0x50-byte header of one upload at blk[o:] -> Upload, None when it is not A+D(3) + IMAGE"""
    if o + 0x50 > len(blk):
        return None
    tag0, regs0 = struct.unpack_from("<QQ", blk, o)
    if not ((tag0 & 0x7FFF) == 3 and (tag0 >> 58) & 3 == 0 and (tag0 >> 60) & 0xF == 1 and regs0 & 0xF == 0xE):
        return None
    vals = {}
    for k in range(3):
        v, a = struct.unpack_from("<QQ", blk, o + 16 + 16 * k)
        vals[a & 0xFF] = v
    tag1 = struct.unpack_from("<Q", blk, o + 0x40)[0]
    if (tag1 >> 58) & 3 != 2:
        return None
    p = Upload()
    trxreg = vals.get(0x52, 0)
    p.rrw, p.rrh = trxreg & 0xFFF, (trxreg >> 32) & 0xFFF
    p.data_off, p.data_len = o + 0x50, (tag1 & 0x7FFF) * 16
    return p


def _uploads(blk):
    out, o = [], 0
    while o < len(blk):
        p = _upload(blk, o)
        if p is None:
            break
        out.append(p)
        o = p.data_off + p.data_len
    return out


def _transfer_min(psm, swizzled):
    """(min width, min height) of one upload"""
    if swizzled:
        return (16, 4) if psm == PSMT8 else (32, 4)
    return {PSMCT32: (2, 1), PSMCT16: (4, 1), PSMCT16S: (4, 1)}.get(psm, (8, 1))


class Texture:
    """one PS2 texture of a dictionary (level 0 is what the gallery shows)"""

    def __init__(self, data, body, size):
        ch = chunks(data, body, body + size)
        if [c[0] for c in ch[:4]] != [STRUCT, STRING, STRING, STRUCT]:
            raise ValueError("unexpected texture layout")
        self.platform = bytes(data[ch[0][2]:ch[0][2] + 4])
        self.name = bytes(data[ch[1][2]:ch[1][2] + ch[1][1]]).split(b"\0")[0].decode("latin1")
        r = ch[3]
        inner = chunks(data, r[2], r[2] + r[1])
        if len(inner) < 2 or inner[0][0] != STRUCT or inner[1][0] != STRUCT or inner[0][1] < HDR.size:
            raise ValueError("unexpected raster layout")
        (self.width, self.height, self.depth, self.raster_format, _ver, _tex0, _po, self.tex1low, _m1, _m2,
         pixel_size, palette_size, _total, _mm) = HDR.unpack_from(data, inner[0][2])
        d = inner[1][2]
        self.pix = bytes(data[d:d + pixel_size])
        self.pal = bytes(data[d + pixel_size:d + pixel_size + palette_size])

    @property
    def pixfmt(self):
        return self.raster_format & FMT_PIXEL_MASK

    @property
    def pal_kind(self):
        return 8 if self.raster_format & FMT_PAL8 else 4 if self.raster_format & FMT_PAL4 else 0

    @property
    def psm(self):
        if self.depth == 4:
            return PSMT4
        if self.depth == 8:
            return PSMT8
        if self.depth == 32:
            return PSMCT24 if self.pixfmt == FMT_888 else PSMCT32
        if self.depth == 16:
            return PSMCT16S if self.pixfmt == FMT_1555 else PSMCT16
        return -1

    def level0(self):
        """(width, height, stored row stride in texels, Upload, pre-swizzled)"""
        ups = _uploads(self.pix)
        if not ups:
            raise ValueError("no pixel upload")
        p, w, h = ups[0], max(self.width, 1), max(self.height, 1)
        sw = False
        if self.psm in (PSMT8, PSMT4):
            minw, minh = _transfer_min(self.psm, True)
            sw = p.rrw == max(w, minw) // 2 and p.rrh == max(h, minh) // 2
        minw, _minh = _transfer_min(self.psm, sw)
        return w, h, max(w, minw), p, sw


def parse(blob):
    """-> [Texture] of a dictionary (PS2 rasters only; others are skipped)"""
    t, s, _v = struct.unpack_from("<III", blob, 0)
    if t != DICTIONARY:
        raise ValueError("not a texture dictionary")
    body = chunks(blob, 12, min(12 + s, len(blob)))
    if not body or body[0][0] != STRUCT:
        raise ValueError("dictionary without its header")
    out = []
    for c in body[1:]:
        if c[0] == TEXTURE:
            tx = Texture(blob, c[2], c[1])
            if tx.platform == b"PS2\0":
                out.append(tx)
    return out


# ------------------------------------------------------------------------------------------------ decoding
CSM1 = [(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1) for i in range(256)]
_LO = bytes(v & 15 for v in range(256))
_HI = bytes(v >> 4 for v in range(256))
_ALPHA = bytes(min(255, 2 * a) for a in range(256))
_ROWS = {}


def _gather(data, idx):
    last = len(data) - 1
    idx = [i if i <= last else last for i in idx]
    if len(idx) == 1:
        return bytes([data[idx[0]]])
    return bytes(operator.itemgetter(*idx)(data))


def _swz_template(w, y):
    """row y of a pre-swizzled upload = base(y) + template(w, two bits of y), in units (bytes for PSMT8, nibbles for
    PSMT4): the GS places PSMT8 texels inside PSMCT32 blocks this way"""
    s, lane0 = (((y >> 1) ^ (y >> 2)) & 1) << 2, (y >> 1) & 1
    key = (w, s, lane0)
    t = _ROWS.get(key)
    if t is None:
        a = [lane0 | (((x >> 3) & 1) << 1) | ((((x ^ s) & 7) | ((x >> 1) & ~7)) << 2) for x in range(w)]
        half = [v >> 1 for v in a]
        t = _ROWS[key] = (a, max(a), operator.itemgetter(*a) if w > 1 else None, half,
                          operator.itemgetter(*half) if w > 1 else None)
    return t


def _swz_row(data, mv, w, y, ww, nibbles):
    a, top, get, half, get_half = _swz_template(w, y)
    base = ((y & 1) | ((y >> 1) & ~1)) * 2 * ww
    if nibbles:
        base >>= 1
        top, get, a = top >> 1, get_half, half
    if get is not None and base + top < len(data):
        return bytes(get(mv[base:]))
    return _gather(data, [base + v for v in a])


def _indices(t, w, h, stride, p, sw):
    data = t.pix[p.data_off:p.data_off + p.data_len]
    mv = memoryview(data)
    rows = []
    for y in range(h):
        if t.psm == PSMT8:
            rows.append(_swz_row(data, mv, w, y, stride, False) if sw else _gather(data, range(y * stride,
                                                                                               y * stride + w)))
        elif sw:
            rows.append(_swz_row(data, mv, w, y, stride, True).translate(_HI if (y >> 1) & 1 else _LO))
        else:
            a0 = y * stride
            raw = _gather(data, range(a0 >> 1, ((a0 + w + 1) >> 1) + 1))
            both = bytearray(2 * len(raw))
            both[0::2], both[1::2] = raw.translate(_LO), raw.translate(_HI)
            rows.append(bytes(both[a0 & 1:(a0 & 1) + w]))
    return b"".join(rows)


def _clut(t):
    ups = _uploads(t.pal)
    if not ups:
        raise ValueError("no palette upload")
    raw = t.pal[ups[0].data_off:ups[0].data_off + ups[0].data_len]
    n = 256 if t.pal_kind == 8 else 16
    if t.pixfmt == FMT_8888:
        c = [tuple(raw[4 * i:4 * i + 4].ljust(4, b"\0")) for i in range(n)]
    elif t.pixfmt == FMT_1555:
        c = []
        for i in range(n):
            v = int.from_bytes(raw[2 * i:2 * i + 2].ljust(2, b"\0"), "little")
            c.append(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, (v >> 15) * 0x80))
    else:
        raise ValueError("unsupported palette format %x" % t.pixfmt)
    return [c[k] for k in CSM1] if n == 256 else c


def decode(t):
    """-> Pillow RGBA image of level 0 (alpha 0..255)"""
    from PIL import Image
    w, h, stride, p, sw = t.level0()
    if t.pal_kind:
        if t.psm not in (PSMT8, PSMT4):
            raise ValueError("palettised texture of depth %d" % t.depth)
        im = Image.frombytes("P", (w, h), _indices(t, w, h, stride, p, sw))
        pal = bytearray()
        for r, g, b, a in _clut(t):
            pal += bytes((r, g, b, _ALPHA[a]))
        pal += bytes(4 * (256 - len(pal) // 4))
        im.putpalette(bytes(pal), "RGBA")
        return im.convert("RGBA")
    data = t.pix[p.data_off:p.data_off + p.data_len]
    if t.depth == 32:
        im = Image.frombytes("RGBA", (w, h), bytes(data[:w * h * 4]).ljust(w * h * 4, b"\0"))
        if t.pixfmt == FMT_888:
            im.putalpha(255)
            return im
    elif t.depth == 16:
        px = bytearray()
        for k in range(w * h):
            v = int.from_bytes(data[2 * k:2 * k + 2].ljust(2, b"\0"), "little")
            px += bytes(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, (v >> 15) * 0x80))
        im = Image.frombytes("RGBA", (w, h), bytes(px))
    else:
        raise ValueError("unsupported depth %d" % t.depth)
    r, g, b, a = im.split()
    return Image.merge("RGBA", (r, g, b, a.point(list(_ALPHA))))
