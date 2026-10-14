"""An approximate PSP-menu mock (480x272) of the selected game, for the converter window (needs Pillow).

Timeline after the entry gets focus (the order commonly described for the PSP menu; exact times and positions are
Unknown and the mock says "approximate"): the background PIC1 appears at once (short fade), then PIC0 fades in and
the animated icon ICON1 replaces the still ICON0 and loops; the menu music starts about then.
"""
import math

from ps2rc.pbp.spec import ICON1_W, ICON1_H, ICON1_FPS_NUM, ICON1_FPS_DEN

W, H = 480, 272
ICON_XY = (24, 96)
PIC0_XY = (170, 46)
TITLE_XY = (180, 136)
T_PIC1 = 0.25          # fade of the background
T_START = 0.8          # PIC0 / ICON1 / music start
T_FADE = 0.5
FPS = ICON1_FPS_NUM / float(ICON1_FPS_DEN)


def _font(size):
    from PIL import ImageFont
    for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


class Mock:
    """state for compose(): Pillow images or None; icon1_frames = list of 144x80 RGB24 bytes"""
    _base_cache = None

    def __init__(self, pic1=None, pic0=None, icon0=None, icon1_frames=None, title="", clock="12:00"):
        from PIL import Image
        self.Image = Image
        self.pic1 = self._load(pic1, (W, H), "RGB")
        self.pic0 = self._load(pic0, (310, 180), "RGBA")
        self.icon0 = self._load(icon0, (144, 80), "RGBA")
        self.frames = [Image.frombytes("RGB", (ICON1_W, ICON1_H), f) for f in (icon1_frames or [])]
        self.title, self.clock = title, clock
        if Mock._base_cache is None:
            Mock._base_cache = self._menu_background()
        self._base = Mock._base_cache
        self._font_t, self._font_s = _font(15), _font(11)

    def _load(self, src, size, mode):
        if src is None:
            return None
        import io
        im = src if hasattr(src, "size") else self.Image.open(io.BytesIO(src))
        im = im.convert(mode)
        return im if im.size == size else im.resize(size)

    def _menu_background(self):
        """the menu's own background (a calm blue gradient with a soft wave) when the game has no PIC1; built with
        whole-image operations (fast enough for the window's thread)"""
        Image = self.Image
        from PIL import ImageChops, ImageFilter
        g = Image.linear_gradient("L").resize((W, H))                 # 0 at the top .. 255 at the bottom
        r = g.point(lambda v: 20 + v * 30 // 255)
        gg = g.point(lambda v: 60 + v * 50 // 255)
        b = g.point(lambda v: 120 + v * 60 // 255)
        wave = Image.new("L", (W, H), 0)
        from PIL import ImageDraw
        d = ImageDraw.Draw(wave)
        pts = [(x, 150 + 25 * math.sin(x / 70.0)) for x in range(0, W + 8, 8)]
        d.line(pts, fill=18, width=30)
        wave = wave.filter(ImageFilter.GaussianBlur(10)).point(lambda v: min(255, v * 2))
        return Image.merge("RGB", [ImageChops.add(c, wave) for c in (r, gg, b)])

    def compose(self, t):
        """-> Pillow RGB image of the menu `t` seconds after the entry got focus"""
        from PIL import ImageDraw
        img = self._base.copy()
        if self.pic1 is not None:
            img = self.Image.blend(img, self.pic1, max(0.0, min(1.0, t / T_PIC1)))
        a = max(0.0, min(1.0, (t - T_START) / T_FADE))
        if self.pic0 is not None and a > 0:
            layer = self.pic0.copy()
            alpha = layer.getchannel("A").point(lambda v: int(v * a))
            layer.putalpha(alpha)
            img.paste(layer, PIC0_XY, layer)
        d = ImageDraw.Draw(img, "RGBA")
        d.text((W - 12, 8), self.clock, fill=(255, 255, 255, 220), font=self._font_s, anchor="ra")
        # the icon: ICON0, cross-faded into the looping ICON1
        ix, iy = ICON_XY
        if self.icon0 is not None:
            img.paste(self.icon0, (ix, iy), self.icon0)
        else:
            d.rounded_rectangle((ix, iy, ix + 143, iy + 79), 8, outline=(255, 255, 255, 140), width=2)
        if self.frames and t >= T_START:
            fr = self.frames[int((t - T_START) * FPS) % len(self.frames)]
            img.paste(self.Image.blend(img.crop((ix, iy, ix + 144, iy + 80)), fr, min(1.0, a * 1.25)), (ix, iy))
        d = ImageDraw.Draw(img, "RGBA")
        title = self.title or ""
        if title:
            d.text((TITLE_XY[0] + 1, TITLE_XY[1] + 1), title, fill=(0, 0, 0, 140), font=self._font_t, anchor="lm")
            d.text(TITLE_XY, title, fill=(255, 255, 255, 255), font=self._font_t, anchor="lm")
        return img


def ppm(img):
    """Pillow RGB image -> PPM bytes (Tk's PhotoImage reads them without Pillow's ImageTk)"""
    w, h = img.size
    return b"P6\n%d %d\n255\n" % (w, h) + img.convert("RGB").tobytes()
