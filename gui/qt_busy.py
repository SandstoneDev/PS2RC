"""Qt widgets for long work: a drawn spinner, a busy row (spinner + text + bar), the disc picture gallery that opens at
once and fills in while the disc is read, the music-from-the-disc list."""
import threading

from PySide6.QtCore import QObject, QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox, QHBoxLayout, QLabel, QLineEdit,
                               QListWidget, QListWidgetItem, QProgressBar, QPushButton, QTreeWidget, QTreeWidgetItem,
                               QVBoxLayout, QWidget)

import i18n


class Spinner(QWidget):
    """a turning arc drawn with QPainter (no font glyphs), for work whose length is not known yet"""

    def __init__(self, parent=None, size=18):
        super().__init__(parent)
        self.setFixedSize(size, size)
        self.angle = 0
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._step)
        self.timer.start(40)

    def _step(self):
        self.angle = (self.angle + 30) % 360
        self.update()

    def stop(self):
        self.timer.stop()
        self.hide()

    def start(self):
        self.show()
        self.timer.start(40)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w = self.width()
        pen = QPen(QColor("#3b82f6"), max(2.0, w / 8.0))
        pen.setCapStyle(Qt.RoundCap)
        p.setPen(pen)
        m = w / 6.0
        p.drawArc(QRectF(m, m, w - 2 * m, w - 2 * m), -self.angle * 16, 270 * 16)
        p.end()


class BusyRow(QWidget):
    """spinner + text + a progress bar: busy (no total yet) or counting (done / total)"""

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self.spinner = Spinner(self)
        self.text = QLabel("")
        self.bar = QProgressBar()
        self.bar.setRange(0, 0)
        self.bar.setMaximumWidth(220)
        self.bar.setTextVisible(False)
        lay.addWidget(self.spinner)
        lay.addWidget(self.text, 1)
        lay.addWidget(self.bar)

    def set(self, text, done=None, total=None):
        self.spinner.start()
        self.bar.show()
        self.text.setText(text)
        if total:
            self.bar.setRange(0, total)
            self.bar.setValue(done or 0)
        else:
            self.bar.setRange(0, 0)

    def finish(self, text=""):
        self.spinner.stop()
        self.bar.hide()
        self.text.setText(text)


class _Previews(QObject):
    ready = Signal(str, QImage)


class GalleryDialog(QDialog):
    """pictures from the player's own disc (ps2rc.xmb.disc_art): opens at once; pictures arrive while the disc is read
    (add_art), thumbnails are decoded and scaled in a worker thread; a category filter"""

    def __init__(self, parent, arts, lang="en", busy=False):
        super().__init__(parent)
        self.lang = lang
        self.setWindowTitle(i18n.tr("gal.h", lang))
        self.arts, self.chosen, self.cancelled = [], None, False
        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        top.addWidget(QLabel(i18n.tr("gal.show", lang)))
        self.filter = QComboBox()
        self.filter.addItem(i18n.tr("gal.all", lang), "")
        self.filter.currentIndexChanged.connect(self._fill)
        top.addWidget(self.filter)
        top.addStretch(1)
        lay.addLayout(top)
        self.busy = BusyRow()
        lay.addWidget(self.busy)
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.IconMode)
        self.list.setIconSize(QSize(160, 120))
        self.list.setResizeMode(QListWidget.Adjust)
        self.list.setMovement(QListWidget.Static)
        self.list.setSpacing(6)
        self.list.setMinimumSize(720, 420)
        self.list.itemDoubleClicked.connect(lambda _i: self._ok())
        lay.addWidget(self.list)
        lay.addWidget(QLabel(i18n.tr("gal.note", lang)))
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.button(QDialogButtonBox.Ok).setText(i18n.tr("gal.use", lang))
        bb.button(QDialogButtonBox.Cancel).setText(i18n.tr("crop.cancel", lang))
        bb.accepted.connect(self._ok)
        bb.rejected.connect(self._cancel)
        lay.addWidget(bb)
        self._placeholder = QPixmap(160, 120)
        self._placeholder.fill(QColor("#2a2f3a"))
        self._icons = {}
        self._previews = _Previews()
        self._previews.ready.connect(self._preview_ready)
        self._queue, self._worker = [], None
        for a in arts:
            self.add_art(a)
        if busy:
            self.busy.set(i18n.tr("gal.reading_n", lang, len(self.arts)))
        else:
            self.finish_reading()

    def add_art(self, a):
        self.arts.append(a)
        if a.category in ("loading", "title", "background", "logo") and self.filter.findData(a.category) < 0:
            self.filter.addItem(i18n.tr("gal." + a.category, self.lang), a.category)
        self.busy.set(i18n.tr("gal.reading_n", self.lang, len(self.arts)))
        if not self.filter.currentData() or self.filter.currentData() == a.category:
            self._add_item(a)
        self._queue.append(a.path)
        self._kick()

    def finish_reading(self, text=None):
        self.busy.finish(text if text is not None else i18n.tr("gal.count", self.lang, len(self.arts)))

    def _add_item(self, a):
        icon = self._icons.get(a.path) or QIcon(self._placeholder)
        it = QListWidgetItem(icon, "%s\n%dx%d" % (a.name, a.width, a.height))
        it.setData(Qt.UserRole, a)
        self.list.addItem(it)
        if self.list.count() == 1:
            self.list.setCurrentRow(0)

    def _kick(self):
        """thumbnails off the window's thread (QImage is thread-safe, QPixmap is made in the window's thread)"""
        if self._worker is not None and self._worker.is_alive():
            return

        def work():
            while self._queue and not self.cancelled:
                path = self._queue.pop(0)
                img = QImage(path)
                if not img.isNull():
                    try:
                        self._previews.ready.emit(path, img.scaled(160, 120, Qt.KeepAspectRatio,
                                                                 Qt.SmoothTransformation))
                    except RuntimeError:       # the dialog was closed meanwhile
                        return
        self._worker = threading.Thread(target=work, daemon=True)
        self._worker.start()

    def _preview_ready(self, path, img):
        icon = QIcon(QPixmap.fromImage(img))
        self._icons[path] = icon
        for i in range(self.list.count()):
            it = self.list.item(i)
            if it.data(Qt.UserRole).path == path:
                it.setIcon(icon)

    def _fill(self):
        cat = self.filter.currentData()
        self.list.clear()
        for a in self.arts:
            if not cat or a.category == cat:
                self._add_item(a)

    def _ok(self):
        it = self.list.currentItem()
        if it is not None:
            self.chosen = it.data(Qt.UserRole)
            self.cancelled = True
            self.accept()

    def _cancel(self):
        self.cancelled = True
        self.reject()

    def closeEvent(self, e):
        self.cancelled = True
        super().closeEvent(e)


class MusicDialog(QDialog):
    """music from the player's own disc (ps2rc.xmb.disc_music): a list with a category filter and a search box,
    listening to a piece, the start of the part to use"""

    def __init__(self, parent, lang="en"):
        super().__init__(parent)
        self.lang = lang
        self.setWindowTitle(i18n.tr("mus.h", lang))
        self.tracks, self.chosen, self.cancelled = [], None, False
        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        top.addWidget(QLabel(i18n.tr("gal.show", lang)))
        self.filter = QComboBox()
        self.filter.addItem(i18n.tr("gal.all", lang), "")
        for c in ("radio", "cutscene", "beats", "ambience", "adverts"):
            self.filter.addItem(i18n.tr("mus." + c, lang), c)
        self.filter.currentIndexChanged.connect(self._fill)
        top.addWidget(self.filter)
        self.search = QLineEdit()
        self.search.setPlaceholderText(i18n.tr("mus.search", lang))
        self.search.textChanged.connect(self._fill)
        top.addWidget(self.search, 1)
        lay.addLayout(top)
        self.busy = BusyRow()
        lay.addWidget(self.busy)
        self.tree = QTreeWidget()
        self.tree.setRootIsDecorated(False)
        self.tree.setHeaderLabels([i18n.tr("mus.col.name", lang), i18n.tr("mus.col.kind", lang),
                                   i18n.tr("mus.col.len", lang)])
        self.tree.setColumnWidth(0, 220)
        self.tree.setColumnWidth(1, 160)
        self.tree.setMinimumSize(560, 360)
        self.tree.itemDoubleClicked.connect(lambda _i, _c: self._ok())
        lay.addWidget(self.tree)
        row = QHBoxLayout()
        row.addWidget(QLabel(i18n.tr("mus.start", lang)))
        self.start = QDoubleSpinBox()
        self.start.setRange(0.0, 3600.0)
        self.start.setDecimals(1)
        self.start.setSuffix(" s")
        row.addWidget(self.start)
        self.listen = QPushButton(i18n.tr("snd0.listen", lang))
        self.stop = QPushButton(i18n.tr("snd0.stop", lang))
        row.addWidget(self.listen)
        row.addWidget(self.stop)
        self.listen_busy = Spinner()
        self.listen_busy.stop()
        row.addWidget(self.listen_busy)
        row.addStretch(1)
        lay.addLayout(row)
        note = QLabel(i18n.tr("mus.note", lang))
        note.setWordWrap(True)
        lay.addWidget(note)
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        self.ok_button = bb.button(QDialogButtonBox.Ok)
        self.ok_button.setText(i18n.tr("mus.use", lang))
        bb.button(QDialogButtonBox.Cancel).setText(i18n.tr("crop.cancel", lang))
        bb.accepted.connect(self._ok)
        bb.rejected.connect(self._cancel)
        lay.addWidget(bb)
        self.busy.set(i18n.tr("mus.reading", lang))

    def progress(self, done, total):
        self.busy.set(i18n.tr("mus.reading_n", self.lang, done, total), done, total)

    def set_tracks(self, tracks):
        self.tracks = tracks
        self.busy.finish(i18n.tr("mus.count", self.lang, len(tracks)))
        self._fill()

    def current(self):
        it = self.tree.currentItem()
        return it.data(0, Qt.UserRole) if it is not None else None

    def _fill(self):
        cat, q = self.filter.currentData(), self.search.text().strip().lower()
        self.tree.clear()
        for t in self.tracks:
            if (cat and t.category != cat) or (q and q not in t.name.lower()):
                continue
            m, sec = divmod(int(t.seconds), 60)
            it = QTreeWidgetItem([t.name, i18n.tr("mus." + t.category, self.lang), "%d:%02d" % (m, sec)])
            it.setData(0, Qt.UserRole, t)
            self.tree.addTopLevelItem(it)
        if self.tree.topLevelItemCount():
            self.tree.setCurrentItem(self.tree.topLevelItem(0))

    def _ok(self):
        if self.current() is not None:
            self.chosen = self.current()
            self.accept()

    def _cancel(self):
        self.cancelled = True
        self.reject()
