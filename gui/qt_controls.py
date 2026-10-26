"""Pictures of the controls page: a neutral, hand-drawn outline of a handheld console (no maker's logo, no product
photo) whose buttons light up and can be clicked, and small button glyphs for the mapping rows. Colours follow the
window's palette (light and dark)."""
from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPalette, QPen, QPixmap, QPolygonF
from PySide6.QtWidgets import QApplication, QWidget

ACCENT = QColor("#3b82f6")
FACE = ("triangle", "circle", "cross", "square")
ARROWS = ("up", "down", "left", "right")


def _pal(role):
    return QApplication.palette().color(role)


def _ink():
    return _pal(QPalette.WindowText)


def draw_glyph(p, name, r, ink, fill=None):
    """one control's symbol inside rect r: face button symbols, D-pad arrows, L / R tabs, SELECT / START pills, the
    analog stick"""
    p.save()
    pen = QPen(ink, max(1.2, r.width() / 11))
    pen.setCapStyle(Qt.RoundCap)
    pen.setJoinStyle(Qt.RoundJoin)
    c = r.center()
    s = min(r.width(), r.height())
    if name in FACE:
        p.setPen(QPen(ink, max(1.0, s / 16)))
        p.setBrush(fill if fill is not None else Qt.NoBrush)
        p.drawEllipse(c, s * 0.46, s * 0.46)
        p.setPen(pen)
        p.setBrush(Qt.NoBrush)
        k = s * 0.2
        if name == "cross":
            p.drawLine(QPointF(c.x() - k, c.y() - k), QPointF(c.x() + k, c.y() + k))
            p.drawLine(QPointF(c.x() - k, c.y() + k), QPointF(c.x() + k, c.y() - k))
        elif name == "circle":
            p.drawEllipse(c, k * 1.1, k * 1.1)
        elif name == "square":
            p.drawPolygon(QPolygonF([QPointF(c.x() - k, c.y() - k), QPointF(c.x() + k, c.y() - k),
                                     QPointF(c.x() + k, c.y() + k), QPointF(c.x() - k, c.y() + k)]))
        else:
            p.drawPolygon(QPolygonF([QPointF(c.x(), c.y() - k * 1.2), QPointF(c.x() + k * 1.15, c.y() + k * 0.85),
                                     QPointF(c.x() - k * 1.15, c.y() + k * 0.85)]))
    elif name in ARROWS:
        p.setPen(QPen(ink, max(1.0, s / 16)))
        p.setBrush(fill if fill is not None else Qt.NoBrush)
        p.drawRoundedRect(QRectF(c.x() - s * 0.42, c.y() - s * 0.42, s * 0.84, s * 0.84), s * 0.15, s * 0.15)
        k = s * 0.2
        pts = {"up": [(0, -1), (1, 0.6), (-1, 0.6)], "down": [(0, 1), (1, -0.6), (-1, -0.6)],
               "left": [(-1, 0), (0.6, 1), (0.6, -1)], "right": [(1, 0), (-0.6, 1), (-0.6, -1)]}[name]
        p.setPen(Qt.NoPen)
        p.setBrush(ink)
        p.drawPolygon(QPolygonF([QPointF(c.x() + x * k, c.y() + y * k) for x, y in pts]))
    elif name in ("l", "r"):
        p.setPen(QPen(ink, max(1.0, s / 16)))
        p.setBrush(fill if fill is not None else Qt.NoBrush)
        p.drawRoundedRect(QRectF(r.x() + 1, c.y() - s * 0.3, r.width() - 2, s * 0.6), s * 0.2, s * 0.2)
        f = QFont(QApplication.font())
        f.setBold(True)
        f.setPixelSize(max(6, int(s * 0.42)))
        p.setFont(f)
        p.setPen(ink)
        p.drawText(r, Qt.AlignCenter, name.upper())
    elif name in ("select", "start"):
        p.setPen(QPen(ink, max(1.0, s / 16)))
        p.setBrush(fill if fill is not None else Qt.NoBrush)
        h = s * 0.34
        p.drawRoundedRect(QRectF(r.x() + 1, c.y() - h / 2, r.width() - 2, h), h / 2, h / 2)
    elif name == "analog":
        p.setPen(QPen(ink, max(1.0, s / 16)))
        p.setBrush(fill if fill is not None else Qt.NoBrush)
        p.drawEllipse(c, s * 0.42, s * 0.42)
        p.setBrush(ink)
        p.drawEllipse(c, s * 0.12, s * 0.12)
    p.restore()


def glyph_pixmap(name, size=20, ratio=1.0):
    """a row icon, drawn at the screen's pixel ratio"""
    pm = QPixmap(int(size * 1.6 * ratio) if name in ("l", "r", "select", "start") else int(size * ratio),
                 int(size * ratio))
    pm.setDevicePixelRatio(ratio)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    w = pm.width() / ratio
    draw_glyph(p, name, QRectF(0, 0, w, size), _ink())
    p.end()
    return pm


class ConsolePicture(QWidget):
    """the outline of the handheld; `marked` controls are filled with the accent colour, a click emits picked(name)"""
    picked = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.marked, self.hover, self.areas = set(), None, {}
        self.setMouseTracking(True)
        self.setMinimumSize(360, 150)

    def set_marked(self, names):
        self.marked = set(names)
        self.update()

    def _layout(self):
        """control name -> rect, from the widget size (aspect about 2.3 : 1, centred)"""
        W, H = self.width(), self.height()
        h = min(H - 8, (W - 8) / 2.3)
        w = h * 2.3
        x0, y0 = (W - w) / 2, (H - h) / 2 + h * 0.04
        body = QRectF(x0, y0 + h * 0.08, w, h * 0.9)
        a = {"body": body, "screen": QRectF(x0 + w * 0.25, y0 + h * 0.2, w * 0.5, h * 0.6)}
        a["l"] = QRectF(x0 + w * 0.06, y0, w * 0.14, h * 0.12)
        a["r"] = QRectF(x0 + w * 0.80, y0, w * 0.14, h * 0.12)
        b = h * 0.13
        dx, dy = x0 + w * 0.125, y0 + h * 0.42
        a["up"] = QRectF(dx - b / 2, dy - b * 1.5, b, b)
        a["down"] = QRectF(dx - b / 2, dy + b * 0.5, b, b)
        a["left"] = QRectF(dx - b * 1.5, dy - b / 2, b, b)
        a["right"] = QRectF(dx + b * 0.5, dy - b / 2, b, b)
        a["analog"] = QRectF(dx - b * 0.75, y0 + h * 0.66, b * 1.5, b * 1.5)
        fx, fy, fb = x0 + w * 0.875, y0 + h * 0.42, h * 0.135
        a["triangle"] = QRectF(fx - fb / 2, fy - fb * 1.55, fb, fb)
        a["cross"] = QRectF(fx - fb / 2, fy + fb * 0.55, fb, fb)
        a["square"] = QRectF(fx - fb * 1.55, fy - fb / 2, fb, fb)
        a["circle"] = QRectF(fx + fb * 0.55, fy - fb / 2, fb, fb)
        a["select"] = QRectF(x0 + w * 0.58, y0 + h * 0.85, w * 0.065, h * 0.08)
        a["start"] = QRectF(x0 + w * 0.665, y0 + h * 0.85, w * 0.065, h * 0.08)
        return a

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        a = self.areas = self._layout()
        ink, base = _ink(), _pal(QPalette.Button)
        body = a["body"]
        for k in ("l", "r"):                                 # the shoulder buttons behind the body
            draw_glyph(p, k, a[k], ink, ACCENT if k in self.marked else (base.lighter(115) if k == self.hover
                                                                         else base))
        path = QPainterPath()
        path.addRoundedRect(body, body.height() * 0.42, body.height() * 0.42)
        p.setPen(QPen(ink, 1.4))
        p.setBrush(base)
        p.drawPath(path)
        p.setBrush(QColor("#1d2026"))
        p.setPen(QPen(ink, 1.0))
        p.drawRoundedRect(a["screen"], 3, 3)
        for k in FACE + ARROWS + ("select", "start", "analog"):
            fill = ACCENT if k in self.marked else (base.lighter(125) if k == self.hover else None)
            draw_glyph(p, k, a[k], ink if k not in self.marked else QColor("white") if k in ARROWS else ink, fill)
        p.end()

    def _at(self, pos):
        for k, r in self.areas.items():
            if k not in ("body", "screen") and r.adjusted(-3, -3, 3, 3).contains(QPointF(pos)):
                return k
        return None

    def mouseMoveEvent(self, e):
        k = self._at(e.position() if hasattr(e, "position") else e.pos())
        if k != self.hover:
            self.hover = k
            self.setCursor(Qt.PointingHandCursor if k else Qt.ArrowCursor)
            self.update()

    def leaveEvent(self, _e):
        self.hover = None
        self.update()

    def mousePressEvent(self, e):
        k = self._at(e.position() if hasattr(e, "position") else e.pos())
        if k:
            self.picked.emit(k)
