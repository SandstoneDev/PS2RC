"""The community QR splash of the main PSP build - data generator, preview and check tool.

    python tools/dev/splash_gen.py gen      [--font-dir DIR]   # rewrite the GENERATED block of hle_splash.c
                                                               # + the previews in <work dir>/splash/
    python tools/dev/splash_gen.py preview                     # previews only (from hle_splash.c's block)
    python tools/dev/splash_gen.py decode                      # print the QR payload of hle_splash.c
    python tools/dev/splash_gen.py compare  DUMP.bmp           # a splash BMP (test build) vs the preview

The splash (hle/psp/hle_splash.c, compiled in with mk/build.sh SPLASH=1) is drawn by the CPU, one scanline at a time,
into a 480x272 8888 frame buffer: a vertical background gradient, the white QR panel (29x29 modules of 6 px + the
4-module quiet zone) on the left, and anti-aliased text lines on the right. The text is NOT drawn from a font at run
time: this tool renders each line once with DejaVu Sans (fonts-dejavu-core, free licence; copies in
<work dir>/splash/fonts) into a 4-bit alpha bitmap and writes those bitmaps, the QR matrix and the layout into the
GENERATED block of hle_splash.c. The preview uses exactly the C code's integer arithmetic, so a frame dumped by the
test build (SPLASH=test, splash_test.bmp) must equal splash_preview_dim.png pixel for pixel.

The QR matrix (QR_BYTES: u8 n = 29, then 29 rows of 4 bytes, bit 7 first, 1 = dark) encodes the community link;
decode() reads it back (version 3, EC level M, mask 1, byte mode) to https://discord.gg/wV8x46ZTDu.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc.config import work_dir as _work_dir  # noqa: E402
WORK = _work_dir()                               # local.toml [paths] work, default build/
SPLASH_C = os.path.join(ROOT, "hle", "psp", "hle_splash.c")
OUT = os.path.join(WORK, "splash")
FONT_DIR = os.path.join(OUT, "fonts")

QR_URL = "https://discord.gg/wV8x46ZTDu"
QR_BYTES = bytes([                                    # the community link (see decode())
    0x1D,
    0xFE, 0xFF, 0x03, 0xF8, 0x82, 0x19, 0x02, 0x08, 0xBA, 0xCC, 0x7A, 0xE8, 0xBA, 0x39, 0xDA, 0xE8,
    0xBA, 0x21, 0xCA, 0xE8, 0x82, 0x8E, 0xA2, 0x08, 0xFE, 0xAA, 0xAB, 0xF8, 0x00, 0x1C, 0x18, 0x00,
    0xA3, 0x4D, 0xB1, 0x28, 0xB9, 0x58, 0xAB, 0x18, 0x7F, 0xAE, 0x90, 0x68, 0x94, 0xD3, 0x9E, 0x40,
    0x6F, 0xFE, 0x53, 0x08, 0x15, 0xAE, 0x7F, 0x18, 0x46, 0xE1, 0x36, 0x08, 0x3C, 0x6C, 0x1A, 0x00,
    0xB3, 0x75, 0xCA, 0x08, 0x4D, 0x48, 0xB3, 0x38, 0xFA, 0x06, 0xD2, 0xC8, 0x18, 0x6B, 0x98, 0x80,
    0xFF, 0xF6, 0x5F, 0xD0, 0x00, 0xE6, 0x08, 0xE8, 0xFE, 0xA1, 0x4A, 0x88, 0x82, 0x0C, 0x18, 0x90,
    0xBA, 0x4D, 0x9F, 0xC0, 0xBA, 0x64, 0xB8, 0xF0, 0xBA, 0xFE, 0xCC, 0x98, 0x82, 0x1F, 0xAE, 0x40,
    0xFE, 0xC8, 0xEC, 0xC8])

# ------------------------------------------------------------------ layout (480x272; colours r, g, b)
W, H = 480, 272
MOD = 6                                                # QR module size in pixels
QUIET = 4                                              # quiet zone in modules (phones need it)
PANEL = (29 + 2 * QUIET) * MOD                         # 222
PX, PY = 25, (H - PANEL) // 2                          # panel top-left (25, 25)
CX = PX + PANEL + 24                                   # text column x (271)
CW = W - 14 - CX                                       # text column width (195)
BG_TOP, BG_BOT = (17, 21, 30), (5, 6, 10)
GREY, ACCENT, HINT, WHITE = (172, 180, 194), (240, 190, 92), (118, 128, 146), (255, 255, 255)
DIV_Y, DIV_W, DIV_H = 133, 44, 2                       # accent bar between the two blocks
LVL_DIM = 72                                           # prompt level before the minimum time (0..256)
# (text, size, bold, colour, line top y; None = bottom-aligned with the QR panel)
LINES = [
    ("Join the", 15, False, GREY, 33),
    ("Sandstone_PSP", 23, True, ACCENT, 51),
    ("community", 15, False, GREY, 80),
    ("Scan the QR code to join", 12, False, HINT, 104),
    ("Recompiler author:", 15, False, GREY, 147),
    ("SandstoneDev", 23, True, WHITE, 165),
    ("Press any button to continue", 13, False, WHITE, None),
]
PROMPT = len(LINES) - 1


def qr_matrix(b=QR_BYTES):
    n = b[0]
    stride = (n + 7) // 8
    return [[(b[1 + y * stride + (x >> 3)] >> (7 - (x & 7))) & 1 for x in range(n)] for y in range(n)]


def decode(M):
    """Byte-mode QR (versions 1-4, one or two blocks) -> (payload bytes, version, EC level, mask). No error correction:
    the matrix must be clean (it is: generated, never scanned)."""
    n = len(M)
    ver = (n - 17) // 4
    bits = [M[8][x] for x in list(range(0, 6)) + [7, 8]] + [M[y][8] for y in [7] + list(range(5, -1, -1))]
    fmt = int("".join(map(str, bits)), 2) ^ 0b101010000010010
    ec, mask = fmt >> 13, (fmt >> 10) & 7
    align = {1: None, 2: 18, 3: 22, 4: 26}[ver]

    def func(r, c):
        if (r < 9 and c < 9) or (r < 9 and c >= n - 8) or (r >= n - 8 and c < 9) or r == 6 or c == 6:
            return True
        return align is not None and align - 2 <= r <= align + 2 and align - 2 <= c <= align + 2

    masks = [lambda r, c: (r + c) % 2 == 0, lambda r, c: r % 2 == 0, lambda r, c: c % 3 == 0,
             lambda r, c: (r + c) % 3 == 0, lambda r, c: (r // 2 + c // 3) % 2 == 0,
             lambda r, c: (r * c) % 2 + (r * c) % 3 == 0, lambda r, c: ((r * c) % 2 + (r * c) % 3) % 2 == 0,
             lambda r, c: ((r + c) % 2 + (r * c) % 3) % 2 == 0]
    data, c, up = [], n - 1, True
    while c > 0:
        if c == 6:
            c -= 1
        for r in (range(n - 1, -1, -1) if up else range(n)):
            for cc in (c, c - 1):
                if not func(r, cc):
                    data.append(M[r][cc] ^ (1 if masks[mask](r, cc) else 0))
        up, c = not up, c - 2
    cw = [int("".join(map(str, data[i:i + 8])), 2) for i in range(0, len(data) - 7, 8)]
    level = {1: "L", 0: "M", 3: "Q", 2: "H"}[ec]
    ndata, nblk = {1: {"L": (19, 1), "M": (16, 1), "Q": (13, 1), "H": (9, 1)},
                   2: {"L": (34, 1), "M": (28, 1), "Q": (22, 1), "H": (16, 1)},
                   3: {"L": (55, 1), "M": (44, 1), "Q": (34, 2), "H": (26, 2)},
                   4: {"L": (80, 1), "M": (64, 2), "Q": (48, 2), "H": (36, 4)}}[ver][level]
    per = ndata // nblk
    d = [cw[i * nblk + b] for b in range(nblk) for i in range(per)]
    bs = "".join(format(x, "08b") for x in d)
    if int(bs[:4], 2) != 4:
        raise ValueError("not byte mode")
    ln = int(bs[4:12], 2)
    return bytes(int(bs[12 + 8 * i:20 + 8 * i], 2) for i in range(ln)), ver, level, mask


# ------------------------------------------------------------------ text bitmaps
def render_lines(font_dir=FONT_DIR):
    from PIL import Image, ImageDraw, ImageFont
    items = []
    for text, size, bold, col, top in LINES:
        path = os.path.join(font_dir, "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")
        while True:                                    # largest size <= the requested one that fits the column
            font = ImageFont.truetype(path, size)
            x0, y0, x1, y1 = font.getbbox(text, anchor="la")
            if x1 - min(x0, 0) <= CW or size <= 8:
                break
            size -= 1
        img = Image.new("L", (x1 + 4, y1 + 4), 0)
        ImageDraw.Draw(img).text((0, 0), text, font=font, fill=255, anchor="la")
        bx0, by0, bx1, by1 = img.getbbox()
        img = img.crop((bx0, by0, bx1, by1))
        w, h = img.size
        if top is None:                                # bottom-aligned with the QR panel
            y = PY + PANEL - h
        else:
            y = top + by0
        a4 = [(v * 15 + 127) // 255 for v in img.getdata()]
        items.append({"text": text, "size": size, "bold": bold, "rgb": col, "x": CX + bx0, "y": y, "w": w, "h": h,
                      "a4": a4})
    for i, a in enumerate(items):                      # no overlaps (the C renderer blends each over the background)
        assert a["x"] + a["w"] <= W - 8, a["text"]
        for b in items[i + 1:]:
            assert a["y"] + a["h"] <= b["y"] or b["y"] + b["h"] <= a["y"], (a["text"], b["text"])
        assert not (a["y"] < DIV_Y + DIV_H and DIV_Y < a["y"] + a["h"]), a["text"]
    return items


def pack(items):
    """4-bit alpha, rows padded to whole bytes, high nibble = left pixel"""
    blob, offs = bytearray(), []
    for it in items:
        offs.append(len(blob))
        w, h = it["w"], it["h"]
        for y in range(h):
            row = it["a4"][y * w:(y + 1) * w] + [0]
            for x in range(0, w, 2):
                blob.append(row[x] << 4 | row[x + 1])
    return bytes(blob), offs


def c_rgb(c):
    return "0x%02X%02X%02Xu" % c


def gen_block(items):
    blob, offs = pack(items)
    L = []
    L.append("/* ---- BEGIN GENERATED by tools/dev/splash_gen.py gen - do not edit by hand ---- */")
    L.append("#define SPL_QR_URL \"%s\"" % QR_URL)
    L.append("#define SPL_W %d" % W)
    L.append("#define SPL_H %d" % H)
    L.append("#define SPL_MOD %d               /* QR module, pixels */" % MOD)
    L.append("#define SPL_QUIET %d             /* quiet zone, modules */" % QUIET)
    L.append("#define SPL_PX %d" % PX)
    L.append("#define SPL_PY %d" % PY)
    L.append("#define SPL_BG_TOP %s       /* 0xRRGGBB */" % c_rgb(BG_TOP))
    L.append("#define SPL_BG_BOT %s" % c_rgb(BG_BOT))
    L.append("#define SPL_DIV_X %d" % CX)
    L.append("#define SPL_DIV_Y %d" % DIV_Y)
    L.append("#define SPL_DIV_W %d" % DIV_W)
    L.append("#define SPL_DIV_H %d" % DIV_H)
    L.append("#define SPL_DIV_RGB %s" % c_rgb(ACCENT))
    L.append("#define SPL_LVL_DIM %d" % LVL_DIM)
    L.append("#define SPL_PROMPT %d            /* the item drawn at the pulsing level */" % PROMPT)
    L.append("#define SPL_NITEMS %d" % len(items))
    L.append("static const unsigned char spl_qr[%d] = {   /* u8 n, n rows x ceil(n/8) bytes, bit 7 first, 1 = dark */"
             % len(QR_BYTES))
    for i in range(0, len(QR_BYTES), 16):
        L.append("    " + ",".join("0x%02X" % v for v in QR_BYTES[i:i + 16]) + ",")
    L.append("};")
    L.append("static const char *const spl_text[SPL_NITEMS] = {")
    for it in items:
        L.append("    \"%s\",   /* DejaVu Sans%s %d px */" % (it["text"], " Bold" if it["bold"] else "", it["size"]))
    L.append("};")
    L.append("static const SplItem spl_items[SPL_NITEMS] = {   /* x, y, w, h, 0xRRGGBB, offset into spl_alpha */")
    for it, off in zip(items, offs):
        L.append("    {%d, %d, %d, %d, %s, %d}," % (it["x"], it["y"], it["w"], it["h"], c_rgb(it["rgb"]), off))
    L.append("};")
    L.append("static const unsigned char spl_alpha[%d] = {   /* 4-bit alpha, rows padded to bytes, high nibble first */"
             % len(blob))
    for i in range(0, len(blob), 24):
        L.append("    " + ",".join("0x%02X" % v for v in blob[i:i + 24]) + ",")
    L.append("};")
    L.append("/* ---- END GENERATED ---- */")
    return "\n".join(L) + "\n"


def write_block(items):
    src = open(SPLASH_C, encoding="utf-8").read()
    new = re.sub(r"/\* ---- BEGIN GENERATED.*?/\* ---- END GENERATED ---- \*/\n", lambda m: gen_block(items), src,
                 count=1, flags=re.S)
    if new == src:
        print("hle_splash.c: generated block unchanged")
        return
    open(SPLASH_C, "w", encoding="utf-8", newline="\n").write(new)
    print("hle_splash.c: generated block rewritten (%d items)" % len(items))


def read_block():
    """the items / QR back from hle_splash.c (the previews and the checks use what the EBOOT contains)"""
    src = open(SPLASH_C, encoding="utf-8").read()
    blk = re.search(r"/\* ---- BEGIN GENERATED.*?END GENERATED", src, re.S).group(0)
    hexes = lambda name: bytes(int(v, 16) for v in re.findall(  # noqa: E731
        r"0x([0-9A-F]{2})", re.search(r"%s\[\d+\] = \{.*?\n(.*?)\};" % name, blk, re.S).group(1)))
    qr, alpha = hexes("spl_qr"), hexes("spl_alpha")
    texts = re.findall(r'^    "(.*?)",', re.search(r"spl_text\[SPL_NITEMS\] = \{(.*?)\};", blk, re.S).group(1), re.M)
    rows = re.findall(r"\{(-?\d+), (-?\d+), (\d+), (\d+), 0x([0-9A-F]{6})u, (\d+)\}",
                      re.search(r"spl_items\[SPL_NITEMS\] = \{(.*?)\};", blk, re.S).group(1))
    items = []
    for t, (x, y, w, h, rgb, off) in zip(texts, rows):
        x, y, w, h, off = int(x), int(y), int(w), int(h), int(off)
        a4 = []
        stride = (w + 1) // 2
        for r in range(h):
            for c in range(w):
                v = alpha[off + r * stride + c // 2]
                a4.append(v >> 4 if c % 2 == 0 else v & 15)
        items.append({"text": t, "x": x, "y": y, "w": w, "h": h, "rgb": tuple(bytes.fromhex(rgb)), "a4": a4})
    defs = dict((k, int(v, 0)) for k, v in re.findall(r"#define (SPL_\w+) (0x[0-9A-F]+|\d+)u?", blk))
    return qr, items, defs


# ------------------------------------------------------------------ the C renderer's arithmetic
def render(qr, items, defs, lvl):
    """the frame hle_splash.c draws with the prompt at level lvl (0..256) -> rows of (r, g, b)"""
    M = qr_matrix(qr)
    n = len(M)
    mod, quiet, px, py = defs["SPL_MOD"], defs["SPL_QUIET"], defs["SPL_PX"], defs["SPL_PY"]
    panel = (n + 2 * quiet) * mod
    top = [(defs["SPL_BG_TOP"] >> s) & 255 for s in (16, 8, 0)]
    bot = [(defs["SPL_BG_BOT"] >> s) & 255 for s in (16, 8, 0)]
    div = tuple((defs["SPL_DIV_RGB"] >> s) & 255 for s in (16, 8, 0))
    out = []
    for y in range(H):
        bg = tuple((top[i] * (H - 1 - y) + bot[i] * y) // (H - 1) for i in range(3))
        line = [bg] * W
        if py <= y < py + panel:
            my = (y - py) // mod - quiet
            for x in range(px, px + panel):
                mx = (x - px) // mod - quiet
                dark = 0 <= mx < n and 0 <= my < n and M[my][mx]
                line[x] = (0, 0, 0) if dark else (255, 255, 255)
        if defs["SPL_DIV_Y"] <= y < defs["SPL_DIV_Y"] + defs["SPL_DIV_H"]:
            for x in range(defs["SPL_DIV_X"], defs["SPL_DIV_X"] + defs["SPL_DIV_W"]):
                line[x] = div
        for k, it in enumerate(items):
            if not it["y"] <= y < it["y"] + it["h"]:
                continue
            level = lvl if k == defs["SPL_PROMPT"] else 256
            r = y - it["y"]
            for c in range(it["w"]):
                a = it["a4"][r * it["w"] + c] * 17
                if not a:
                    continue
                a = (a * level + 128) >> 8
                b = line[it["x"] + c]
                line[it["x"] + c] = tuple((b[i] * (255 - a) + it["rgb"][i] * a + 127) // 255 for i in range(3))
        out.append(line)
    return out


def save_png(rows, path, scale=1):
    from PIL import Image
    img = Image.new("RGB", (W, H))
    img.putdata([p for row in rows for p in row])
    if scale != 1:
        img = img.resize((W * scale, H * scale), Image.NEAREST)
    img.save(path)


def read_bmp(path):
    b = open(path, "rb").read()
    off, = struct.unpack_from("<I", b, 10)
    w, h = struct.unpack_from("<ii", b, 18)
    bpp, = struct.unpack_from("<H", b, 28)
    assert bpp == 24 and w == W and abs(h) == H, (w, h, bpp)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(H):
        src = (H - 1 - y) if h > 0 else y
        p = off + src * stride
        rows.append([(b[p + 3 * x + 2], b[p + 3 * x + 1], b[p + 3 * x]) for x in range(w)])
    return rows


def previews():
    qr, items, defs = read_block()
    os.makedirs(OUT, exist_ok=True)
    lit = render(qr, items, defs, 256)
    dim = render(qr, items, defs, defs["SPL_LVL_DIM"])
    save_png(lit, os.path.join(OUT, "splash_preview.png"))
    save_png(lit, os.path.join(OUT, "splash_preview_2x.png"), 2)
    save_png(dim, os.path.join(OUT, "splash_preview_dim.png"))
    print("previews: %s (prompt lit), splash_preview_2x.png, splash_preview_dim.png (the first frame = the test dump)"
          % os.path.join(OUT, "splash_preview.png"))
    return dim


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "preview"
    if cmd == "gen":
        fd = a[a.index("--font-dir") + 1] if "--font-dir" in a else FONT_DIR
        items = render_lines(fd)
        for it in items:
            print("%-30s %2d px%s at (%d, %d) %dx%d" % (it["text"], it["size"], " bold" if it["bold"] else "", it["x"],
                                                        it["y"], it["w"], it["h"]))
        write_block(items)
        previews()
    elif cmd == "preview":
        previews()
    elif cmd == "decode":
        qr, items, defs = read_block()
        print(decode(qr_matrix(qr)))
        print("texts:", [it["text"] for it in items])
    elif cmd == "compare":
        qr, items, defs = read_block()
        want = render(qr, items, defs, defs["SPL_LVL_DIM"])
        got = read_bmp(a[1])
        diff = sum(1 for y in range(H) for x in range(W) if want[y][x] != got[y][x])
        mx = max(abs(want[y][x][i] - got[y][x][i]) for y in range(H) for x in range(W) for i in range(3))
        print("compare %s vs the preview: %d of %d pixels differ, max channel difference %d" % (a[1], diff, W * H, mx))
        return 0 if diff == 0 else 1
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
