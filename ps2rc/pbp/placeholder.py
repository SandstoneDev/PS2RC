"""Neutral placeholder menu pictures drawn from code: a text icon, an information picture, a gradient background
and the frames of a short animation. No font files or imaging library are needed (a built-in 5x7 pixel font).
"""
import math

from . import png
from .spec import ICON0, PIC0, PIC1, ICON1_W, ICON1_H, ICON1_FPS_NUM, ICON1_FPS_DEN

FONT = {
    'A': ('01110', '10001', '10001', '11111', '10001', '10001', '10001'),
    'B': ('11110', '10001', '10001', '11110', '10001', '10001', '11110'),
    'C': ('01110', '10001', '10000', '10000', '10000', '10001', '01110'),
    'D': ('11110', '10001', '10001', '10001', '10001', '10001', '11110'),
    'E': ('11111', '10000', '10000', '11110', '10000', '10000', '11111'),
    'F': ('11111', '10000', '10000', '11110', '10000', '10000', '10000'),
    'G': ('01110', '10001', '10000', '10111', '10001', '10001', '01111'),
    'H': ('10001', '10001', '10001', '11111', '10001', '10001', '10001'),
    'I': ('01110', '00100', '00100', '00100', '00100', '00100', '01110'),
    'J': ('00111', '00010', '00010', '00010', '00010', '10010', '01100'),
    'K': ('10001', '10010', '10100', '11000', '10100', '10010', '10001'),
    'L': ('10000', '10000', '10000', '10000', '10000', '10000', '11111'),
    'M': ('10001', '11011', '10101', '10101', '10001', '10001', '10001'),
    'N': ('10001', '10001', '11001', '10101', '10011', '10001', '10001'),
    'O': ('01110', '10001', '10001', '10001', '10001', '10001', '01110'),
    'P': ('11110', '10001', '10001', '11110', '10000', '10000', '10000'),
    'Q': ('01110', '10001', '10001', '10001', '10101', '10010', '01101'),
    'R': ('11110', '10001', '10001', '11110', '10100', '10010', '10001'),
    'S': ('01111', '10000', '10000', '01110', '00001', '00001', '11110'),
    'T': ('11111', '00100', '00100', '00100', '00100', '00100', '00100'),
    'U': ('10001', '10001', '10001', '10001', '10001', '10001', '01110'),
    'V': ('10001', '10001', '10001', '10001', '10001', '01010', '00100'),
    'W': ('10001', '10001', '10001', '10101', '10101', '10101', '01010'),
    'X': ('10001', '10001', '01010', '00100', '01010', '10001', '10001'),
    'Y': ('10001', '10001', '01010', '00100', '00100', '00100', '00100'),
    'Z': ('11111', '00001', '00010', '00100', '01000', '10000', '11111'),
    '0': ('01110', '10001', '10011', '10101', '11001', '10001', '01110'),
    '1': ('00100', '01100', '00100', '00100', '00100', '00100', '01110'),
    '2': ('01110', '10001', '00001', '00010', '00100', '01000', '11111'),
    '3': ('11111', '00010', '00100', '00010', '00001', '10001', '01110'),
    '4': ('00010', '00110', '01010', '10010', '11111', '00010', '00010'),
    '5': ('11111', '10000', '11110', '00001', '00001', '10001', '01110'),
    '6': ('00110', '01000', '10000', '11110', '10001', '10001', '01110'),
    '7': ('11111', '00001', '00010', '00100', '01000', '01000', '01000'),
    '8': ('01110', '10001', '10001', '01110', '10001', '10001', '01110'),
    '9': ('01110', '10001', '10001', '01111', '00001', '00010', '01100'),
    ' ': ('00000',) * 7,
    '-': ('00000', '00000', '00000', '11111', '00000', '00000', '00000'),
    '.': ('00000', '00000', '00000', '00000', '00000', '01100', '01100'),
    ':': ('00000', '01100', '01100', '00000', '01100', '01100', '00000'),
}


class Canvas:
    def __init__(self, w, h, ch, fill):
        self.w, self.h, self.ch = w, h, ch
        self.px = bytearray(bytes(fill[:ch]) * (w * h))

    def put(self, x, y, color):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * self.ch
            self.px[i:i + self.ch] = bytes(color[:self.ch])

    def rect(self, x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.put(x, y, color)

    def text(self, s, cx, cy, scale, color, shadow=None):
        """draw s centred on (cx, cy); unknown characters draw as blanks"""
        s = s.upper()
        tw, th = (6 * len(s) - 1) * scale, 7 * scale
        x0, y0 = cx - tw // 2, cy - th // 2
        for layer, (dx, dy, col) in enumerate(((scale // 3 or 1, scale // 3 or 1, shadow), (0, 0, color))):
            if col is None:
                continue
            for k, c in enumerate(s):
                for row, bits in enumerate(FONT.get(c, FONT[' '])):
                    for col_i, b in enumerate(bits):
                        if b == '1':
                            self.rect(x0 + dx + (6 * k + col_i) * scale, y0 + dy + row * scale,
                                      x0 + dx + (6 * k + col_i + 1) * scale, y0 + dy + (row + 1) * scale, col)

    def fit_scale(self, s, max_w, max_h):
        return max(1, min(max_w // max(1, 6 * len(s) - 1), max_h // 7))


def icon0(text='PS2RC'):
    """144x80 RGB: white text on dark blue with a frame"""
    c = Canvas(ICON0.width, ICON0.height, 3, (16, 32, 72))
    c.rect(0, 0, c.w, 3, (90, 140, 230))
    c.rect(0, c.h - 3, c.w, c.h, (90, 140, 230))
    c.text(text, c.w // 2, c.h // 2, c.fit_scale(text, c.w - 16, c.h - 24), (255, 255, 255), (0, 0, 0))
    return png.encode(c.w, c.h, 'RGB', c.px)


def pic0(text='PS2RC', sub=''):
    """310x180 RGBA: text on a half-transparent panel, transparent elsewhere"""
    c = Canvas(PIC0.width, PIC0.height, 4, (0, 0, 0, 0))
    c.rect(10, 40, c.w - 10, c.h - 40, (0, 0, 0, 120))
    c.text(text, c.w // 2, 75, c.fit_scale(text, c.w - 40, 40), (255, 255, 255, 255), (0, 0, 0, 255))
    if sub:
        c.text(sub, c.w // 2, 115, c.fit_scale(sub, c.w - 40, 16), (255, 220, 120, 255))
    return png.encode(c.w, c.h, 'RGBA', c.px)


def pic1():
    """480x272 RGB: a diagonal gradient (purple to teal) with soft stripes"""
    w, h = PIC1.width, PIC1.height
    px = bytearray()
    for y in range(h):
        for x in range(w):
            t = (x / (w - 1) * 0.7 + y / (h - 1) * 0.3)
            stripe = 10 * math.sin((x + 2 * y) / 23.0)
            r = 70 * (1 - t) + 10 * t + stripe
            g = 20 * (1 - t) + 120 * t + stripe
            b = 110 * (1 - t) + 130 * t + stripe
            px += bytes((max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b)))))
    return png.encode(w, h, 'RGB', px)


def icon1_frames(text='PS2RC', seconds=4.0):
    """frames of a looping animation (144x80 RGB24, 29.97 fps): text on orange, a white block crossing the icon
    once per loop and a pulsing bar, so a moving icon is easy to tell from the still ICON0"""
    n = max(1, int(round(seconds * ICON1_FPS_NUM / ICON1_FPS_DEN)))
    frames = []
    for i in range(n):
        t = i / float(n)
        c = Canvas(ICON1_W, ICON1_H, 3, (200, 90, 20))
        c.text(text, c.w // 2, 34, c.fit_scale(text, c.w - 20, 36), (255, 255, 255), (60, 20, 0))
        bx = int(-16 + t * (c.w + 16))
        c.rect(bx, 60, bx + 16, 72, (255, 255, 255))
        level = int((0.5 + 0.5 * math.sin(2 * math.pi * t)) * (c.w - 8))
        c.rect(4, 74, 4 + level, 77, (255, 230, 150))
        frames.append(bytes(c.px))
    return frames
