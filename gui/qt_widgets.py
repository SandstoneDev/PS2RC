"""Qt widgets of the converter window: PSP menu preview, crop box editor, disc artwork gallery."""
import time

from PySide6.QtCore import Qt, QRect, QRectF, QSize, QTimer, Signal
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (QComboBox, QDialog, QDialogButtonBox, QHBoxLayout, QLabel, QListWidget,
                               QListWidgetItem, QVBoxLayout, QWidget)

import i18n


def qimage_from_pil(img):
    """Pillow image -> QImage (a copy: the bytes may go away)"""
    img = img.convert("RGBA")
    data = img.tobytes("raw", "RGBA")
    q = QImage(data, img.width, img.height, img.width * 4, QImage.Format_RGBA8888)
    return q.copy()


class PreviewWidget(QWidget):
    """480x272 animated PSP-menu mock (ps2rc.xmb.preview.Mock), drawn at a fixed scale"""
    W, H = 480, 272

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(self.W, self.H)
        self.mock, self.t0, self.image, self.note = None, time.time(), None, ""
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)

    def set_mock(self, mock, note=""):
        self.mock, self.note = mock, note
        self.replay()

    def replay(self):
        self.t0 = time.time()
        self.timer.start(40)
        self._tick()

    def stop(self):
        self.timer.stop()

    def _tick(self):
        if self.mock is None:
            self.image = None
            self.timer.stop()
        else:
            t = time.time() - self.t0
            self.image = qimage_from_pil(self.mock.compose(t))
            if t > 2.0 and not self.mock.frames:
                self.timer.stop()                          # still picture: nothing moves any more
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        if self.image is not None:
            p.setRenderHint(QPainter.SmoothPixmapTransform, True)
            p.drawImage(QRect(0, 0, self.W, self.H), self.image)
        else:
            p.fillRect(self.rect(), QColor("#14324a"))
            p.setPen(QColor("#cde"))
            p.drawText(self.rect(), Qt.AlignCenter | Qt.TextWordWrap, self.note)
        p.end()


class CropCanvas(QWidget):
    changed = Signal()

    def __init__(self, image, out_size, box, parent=None):
        super().__init__(parent)
        from ps2rc.xmb.picture import default_box, clamp_box
        self._default, self._clamp = default_box, clamp_box
        self.img = image                                   # QImage, full size
        self.out_w, self.out_h = out_size
        sw, sh = image.width(), image.height()
        self.scale = min(640.0 / sw, 400.0 / sh, 4.0)
        self.setFixedSize(max(1, int(sw * self.scale)), max(1, int(sh * self.scale)))
        self.box = list(clamp_box(box, sw, sh, *out_size)) if box else list(default_box(sw, sh, *out_size))
        self._mode, self._last = None, None

    def reset(self):
        self.box = list(self._default(self.img.width(), self.img.height(), self.out_w, self.out_h))
        self.update()
        self.changed.emit()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.SmoothPixmapTransform, True)
        p.drawImage(self.rect(), self.img)
        s = self.scale
        x0, y0, x1, y1 = (v * s for v in self.box)
        p.fillRect(QRectF(0, 0, self.width(), y0), QColor(0, 0, 0, 110))
        p.fillRect(QRectF(0, y1, self.width(), self.height() - y1), QColor(0, 0, 0, 110))
        p.fillRect(QRectF(0, y0, x0, y1 - y0), QColor(0, 0, 0, 110))
        p.fillRect(QRectF(x1, y0, self.width() - x1, y1 - y0), QColor(0, 0, 0, 110))
        p.setPen(QPen(QColor("#ffd400"), 2))
        p.drawRect(QRectF(x0, y0, x1 - x0, y1 - y0))
        for cx, cy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
            p.fillRect(QRectF(cx - 4, cy - 4, 8, 8), QColor("#ffd400"))
        p.end()

    def mousePressEvent(self, e):
        s = self.scale
        x, y = e.position().x() / s, e.position().y() / s
        x0, y0, x1, y1 = self.box
        tol = 10 / s
        self._mode = None
        for name, (cx, cy) in {"nw": (x0, y0), "ne": (x1, y0), "sw": (x0, y1), "se": (x1, y1)}.items():
            if abs(x - cx) <= tol and abs(y - cy) <= tol:
                self._mode = name
        if self._mode is None and x0 <= x <= x1 and y0 <= y <= y1:
            self._mode = "move"
        self._last = (x, y)

    def mouseMoveEvent(self, e):
        if not self._mode:
            return
        s = self.scale
        x, y = e.position().x() / s, e.position().y() / s
        x0, y0, x1, y1 = self.box
        W, H = self.img.width(), self.img.height()
        if self._mode == "move":
            dx, dy = x - self._last[0], y - self._last[1]
            self._last = (x, y)
            w, h = x1 - x0, y1 - y0
            nx0, ny0 = min(max(0, x0 + dx), W - w), min(max(0, y0 + dy), H - h)
            self.box = [nx0, ny0, nx0 + w, ny0 + h]
        else:
            ax = x1 if "w" in self._mode else x0
            ay = y1 if "n" in self._mode else y0
            w = max(8.0, abs(x - ax))
            h = w * self.out_h / self.out_w
            nx0 = ax - w if "w" in self._mode else ax
            ny0 = ay - h if "n" in self._mode else ay
            if nx0 < 0 or ny0 < 0 or nx0 + w > W or ny0 + h > H:
                return
            self.box = [nx0, ny0, nx0 + w, ny0 + h]
        self.update()
        self.changed.emit()

    def wheelEvent(self, e):
        x0, y0, x1, y1 = self.box
        f = 0.9 if e.angleDelta().y() > 0 else 1.1
        cx, cy, w = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) * f
        h = w * self.out_h / self.out_w
        self.box = list(self._clamp((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), self.img.width(),
                                    self.img.height(), self.out_w, self.out_h))
        self.update()
        self.changed.emit()


class CropDialog(QDialog):
    """the picture with a frame of the slot's aspect; .result = box in source pixels"""

    def __init__(self, parent, image, out_size, box, lang="en", title=""):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.result_box = None
        lay = QVBoxLayout(self)
        lay.addWidget(QLabel(i18n.tr("crop.h", lang)))
        row = QHBoxLayout()
        self.canvas = CropCanvas(image, out_size, box)
        row.addWidget(self.canvas)
        side = QVBoxLayout()
        self.pv = QLabel()
        pw = 200 if out_size[0] > 160 else out_size[0] * 2
        self.pv_size = QSize(pw, max(1, int(pw * out_size[1] / out_size[0])))
        self.pv.setFixedSize(self.pv_size)
        side.addWidget(self.pv)
        side.addWidget(QLabel("%dx%d" % out_size))
        side.addStretch(1)
        row.addLayout(side)
        lay.addLayout(row)
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel | QDialogButtonBox.Reset)
        bb.button(QDialogButtonBox.Ok).setText(i18n.tr("crop.ok", lang))
        bb.button(QDialogButtonBox.Cancel).setText(i18n.tr("crop.cancel", lang))
        bb.button(QDialogButtonBox.Reset).setText(i18n.tr("crop.reset", lang))
        bb.button(QDialogButtonBox.Reset).clicked.connect(self.canvas.reset)
        bb.accepted.connect(self._ok)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)
        self.canvas.changed.connect(self._preview)
        self._preview()

    def _preview(self):
        b = [int(round(v)) for v in self.canvas.box]
        part = self.canvas.img.copy(QRect(b[0], b[1], b[2] - b[0], b[3] - b[1]))
        self.pv.setPixmap(QPixmap.fromImage(part).scaled(self.pv_size, Qt.IgnoreAspectRatio, Qt.SmoothTransformation))

    def _ok(self):
        self.result_box = [int(round(v)) for v in self.canvas.box]
        self.accept()


from qt_busy import GalleryDialog  # noqa: E402,F401  (moved; kept importable from here)


STATE_COLORS = {"todo": "#9aa0a6", "run": "#3b82f6", "done": "#2e9d4f", "kept": "#2e9d4f", "fail": "#d9453b"}


def state_pixmap(state, size, dpr=1.0):
    """a status icon painted here (no font glyphs: a missing symbol shows as an empty box on some systems):
    todo = grey ring, run = blue play triangle, done / kept = green disc with a white tick, fail = red disc with a
    white cross; drawn at the screen's pixel ratio, so it stays sharp at 100 / 150 / 200 %"""
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QBrush, QPainterPath
    pm = QPixmap(max(1, int(round(size * dpr))), max(1, int(round(size * dpr))))
    pm.setDevicePixelRatio(dpr)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing, True)
    c = QColor(STATE_COLORS.get(state, "#9aa0a6"))
    s = float(size)
    m = s * 0.08
    if state == "todo":
        p.setPen(QPen(c, max(1.0, s * 0.1)))
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(QRectF(m * 2, m * 2, s - m * 4, s - m * 4))
    elif state == "run":
        path = QPainterPath()
        path.moveTo(QPointF(s * 0.25, s * 0.15))
        path.lineTo(QPointF(s * 0.85, s * 0.5))
        path.lineTo(QPointF(s * 0.25, s * 0.85))
        path.closeSubpath()
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(c))
        p.drawPath(path)
    else:
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(c))
        p.drawEllipse(QRectF(m, m, s - 2 * m, s - 2 * m))
        pen = QPen(QColor("white"), max(1.2, s * 0.13))
        pen.setCapStyle(Qt.RoundCap)
        pen.setJoinStyle(Qt.RoundJoin)
        p.setPen(pen)
        if state == "fail":
            p.drawLine(QPointF(s * 0.33, s * 0.33), QPointF(s * 0.67, s * 0.67))
            p.drawLine(QPointF(s * 0.67, s * 0.33), QPointF(s * 0.33, s * 0.67))
        else:
            tick = QPainterPath()
            tick.moveTo(QPointF(s * 0.28, s * 0.52))
            tick.lineTo(QPointF(s * 0.44, s * 0.68))
            tick.lineTo(QPointF(s * 0.73, s * 0.35))
            p.setBrush(Qt.NoBrush)
            p.drawPath(tick)
    p.end()
    return pm
