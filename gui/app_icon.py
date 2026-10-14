"""The converter's own icon, drawn here with QPainter (no picture files in the repository; no game, console-maker or
button marks). Colours: the project's splash (navy background, sand accent, white).

    python gui/app_icon.py ICO_PATH [--variant N]          multi-size .ico (16, 24, 32, 48, 64, 128, 256)
    python gui/app_icon.py --sheet PNG_PATH                 the variants side by side at 256 and 32 px

Variants: 1 = a disc, an arrow, a handheld (the conversion); 2 = the "PS2RC" mark; 3 = a disc turning into a
handheld screen, with a turning arrow. The window uses DEFAULT unless the player's choice says otherwise.
"""
import os
import sys

DEFAULT = 2                 # the "PS2RC" mark
SIZES = (16, 24, 32, 48, 64, 128, 256)
NAVY_TOP, NAVY_BOT = "#11151e", "#05060a"
SAND, WHITE, GREY = "#f0be5c", "#ffffff", "#acb4c2"


def _app():
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication             # a widgets application: the window may come later
    if QGuiApplication.instance() is None:
        if not os.environ.get("QT_QPA_PLATFORM"):
            os.environ["QT_QPA_PLATFORM"] = "offscreen"      # a command-line run never opens a window
        if os.name == "nt" and not os.environ.get("QT_QPA_FONTDIR"):
            os.environ["QT_QPA_FONTDIR"] = os.path.join(os.environ.get("WINDIR", ""), "Fonts")
        return QApplication([sys.argv[0]])
    return QGuiApplication.instance()


def paint(variant, size):
    """-> QImage size x size, ARGB, drawn on a 256-unit canvas scaled to the size"""
    from PySide6.QtCore import QPointF, QRectF, Qt
    from PySide6.QtGui import (QBrush, QColor, QFont, QImage, QLinearGradient, QPainter, QPainterPath, QPen)
    img = QImage(size, size, QImage.Format_ARGB32_Premultiplied)
    img.fill(Qt.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing, True)
    p.setRenderHint(QPainter.TextAntialiasing, True)
    p.scale(size / 256.0, size / 256.0)
    small = size <= 24                                   # thicker shapes, fewer details for tiny sizes
    grad = QLinearGradient(0, 0, 0, 256)
    grad.setColorAt(0, QColor(NAVY_TOP))
    grad.setColorAt(1, QColor(NAVY_BOT))
    p.setPen(Qt.NoPen)
    p.setBrush(QBrush(grad))
    p.drawRoundedRect(QRectF(8, 8, 240, 240), 52, 52)
    sand, white = QColor(SAND), QColor(WHITE)

    def disc(cx, cy, r, hole=True):
        p.setPen(Qt.NoPen)
        p.setBrush(sand)
        p.drawEllipse(QPointF(cx, cy), r, r)
        if hole:
            p.setBrush(QColor(NAVY_TOP))
            p.drawEllipse(QPointF(cx, cy), r * 0.28, r * 0.28)

    def handheld(x, y, w, h):
        p.setPen(Qt.NoPen)
        p.setBrush(white)
        p.drawRoundedRect(QRectF(x, y, w, h), h * 0.42, h * 0.42)
        p.setBrush(QColor(NAVY_BOT))
        sw = w * 0.52
        p.drawRoundedRect(QRectF(x + (w - sw) / 2, y + h * 0.18, sw, h * 0.64), 6, 6)

    if variant == 1:
        disc(70, 128, 42 if not small else 54)
        pen = QPen(white if small else sand, 18 if small else 14)
        pen.setCapStyle(Qt.RoundCap)
        pen.setJoinStyle(Qt.RoundJoin)
        p.setPen(pen)
        p.drawLine(QPointF(122, 128), QPointF(142, 128))
        head = QPainterPath()
        head.moveTo(QPointF(132, 114))
        head.lineTo(QPointF(146, 128))
        head.lineTo(QPointF(132, 142))
        p.setBrush(Qt.NoBrush)
        p.drawPath(head)
        handheld(156, 102, 76, 52) if not small else handheld(152, 96, 88, 64)
    elif variant == 2:
        f = QFont("Segoe UI", 10)
        f.setBold(True)
        f.setPixelSize(118 if not small else 150)
        p.setFont(f)
        p.setPen(white)
        if small:
            p.drawText(QRectF(0, 8, 256, 240), Qt.AlignCenter, "RC")
        else:
            p.drawText(QRectF(0, 70, 256, 150), Qt.AlignHCenter | Qt.AlignVCenter, "RC")
            f2 = QFont(f)
            f2.setPixelSize(52)
            p.setFont(f2)
            p.setPen(sand)
            p.drawText(QRectF(0, 24, 256, 64), Qt.AlignHCenter | Qt.AlignVCenter, "PS2")
            p.setPen(Qt.NoPen)
            p.setBrush(sand)
            p.drawRoundedRect(QRectF(58, 206, 140, 12), 6, 6)
    else:
        handheld(56, 92, 168, 104) if not small else handheld(40, 80, 184, 124)
        disc(84, 104, 56 if not small else 64)
        if not small:
            pen = QPen(sand, 12)
            pen.setCapStyle(Qt.RoundCap)
            p.setPen(pen)
            p.setBrush(Qt.NoBrush)
            p.drawArc(QRectF(150, 26, 80, 80), 200 * 16, -230 * 16)
            p.setPen(Qt.NoPen)
            p.setBrush(sand)
            tip = QPainterPath()
            tip.moveTo(QPointF(218, 86))
            tip.lineTo(QPointF(236, 100))
            tip.lineTo(QPointF(212, 106))
            tip.closeSubpath()
            p.drawPath(tip)
    p.end()
    return img


def qicon(variant=DEFAULT):
    from PySide6.QtGui import QIcon, QPixmap
    icon = QIcon()
    for s in SIZES:
        icon.addPixmap(QPixmap.fromImage(paint(variant, s)))
    return icon


def _pil(qimg):
    from PIL import Image
    from PySide6.QtGui import QImage
    q = qimg.convertToFormat(QImage.Format_RGBA8888)
    return Image.frombuffer("RGBA", (q.width(), q.height()), bytes(q.constBits()), "raw", "RGBA", 0, 1).copy()


def write_ico(path, variant=DEFAULT):
    """one image per size, each drawn at its own size (not scaled from 256)"""
    _app()
    imgs = [_pil(paint(variant, s)) for s in SIZES]
    big = imgs[-1]
    big.save(path, format="ICO", sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])
    return path


def write_sheet(path):
    """the variants side by side at 256 and 32 px, labelled, on a light background"""
    _app()
    from PIL import Image, ImageDraw
    pad, n = 24, 3
    sheet = Image.new("RGB", (pad + n * (256 + pad), 256 + 32 + 3 * pad + 40), (236, 238, 242))
    d = ImageDraw.Draw(sheet)
    for i in range(n):
        x = pad + i * (256 + pad)
        sheet.paste(_pil(paint(i + 1, 256)), (x, pad), _pil(paint(i + 1, 256)))
        small = _pil(paint(i + 1, 32))
        sheet.paste(small, (x + 112, 256 + 2 * pad), small)
        tiny = _pil(paint(i + 1, 16))
        sheet.paste(tiny, (x + 160, 256 + 2 * pad + 8), tiny)
        d.text((x, 256 + 2 * pad + 44), "variant %d" % (i + 1), fill=(20, 20, 20))
    sheet.save(path)
    return path


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args[:1] == ["--sheet"]:
        print(write_sheet(args[1]))
        return 0
    variant = int(args[args.index("--variant") + 1]) if "--variant" in args else DEFAULT
    print(write_ico(args[0], variant))
    return 0


if __name__ == "__main__":
    sys.exit(main())
