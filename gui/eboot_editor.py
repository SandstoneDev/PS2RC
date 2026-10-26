"""EBOOT editor window of the converter (menu Tools > EBOOT editor...).

Opens any EBOOT.PBP (PS2RC games, PSP homebrew, PS1 eboots), shows its menu look and title, lets the player replace
ICON0 (16:9 wide 144x80 or square 80x80, with the crop box), ICON1 (a ready PMF or any clip), PIC0, PIC1, SND0 (a
ready AT3 or any audio) and the title, checks every file and saves. DATA.PSP / DATA.PSAR are kept byte for byte and
the original is kept as EBOOT.PBP.bak (ps2rc/pbp/editor.py does the work).

Texts: English here, other languages in gui/lang/<code>.json under the key "_eboot_editor" (the window's own text
check ignores keys starting with "_").
"""
import io
import os
import threading

from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QFileDialog, QGridLayout, QHBoxLayout, QLabel,
                               QLineEdit, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton, QVBoxLayout,
                               QWidget)

import i18n
from ps2rc.pbp import png
from ps2rc.pbp.editor import Eboot, ICON0_SHAPES, IMAGE_SPECS, EDITABLE, KIND_PS1, picture
from ps2rc.pbp.spec import PbpError, MEDIA_BUDGET, TITLE_MAX

T_EN = {
    "eb.menu.tools": "Tools",
    "eb.menu.editor": "EBOOT editor...",
    "eb.h": "EBOOT editor",
    "eb.sub": ("Change the icon, pictures, music and title of any EBOOT.PBP. The program and the game data inside "
               "stay exactly as they are."),
    "eb.open": "Open EBOOT.PBP...",
    "eb.none": "No EBOOT open.",
    "eb.kind": "%s, %s",
    "eb.kind.ps1": "PS1 eboot",
    "eb.kind.homebrew": "PSP homebrew",
    "eb.kind.ps2rc": "PS2RC game",
    "eb.kind.packed": "PSP program",
    "eb.kind.other": "unknown program",
    "eb.title": "Title",
    "eb.bytes": "%d of %d bytes",
    "eb.slot.ICON0.PNG": "Icon",
    "eb.slot.ICON1.PMF": "Animated icon",
    "eb.slot.PIC0.PNG": "Picture",
    "eb.slot.PIC1.PNG": "Background",
    "eb.slot.SND0.AT3": "Music",
    "eb.replace": "Replace...",
    "eb.crop": "Crop...",
    "eb.remove": "Remove",
    "eb.restore": "Original",
    "eb.export": "Save file...",
    "eb.empty": "(none)",
    "eb.shape.wide": "16:9 wide, 144x80",
    "eb.shape.square": "Square, 80x80",
    "eb.shape.tip": ("PSP programs use 144x80. PS1 eboots use 80x80: the PS1 emulator also reads the icon when the "
                     "game starts, and a 144x80 icon is only safe on PRO / ARK custom firmware."),
    "eb.checks": "Checks",
    "eb.ok": "No problems found.",
    "eb.fix": "Fix automatically",
    "eb.fixed": "Fixed: %s",
    "eb.backup": "Keep the original as EBOOT.PBP.bak",
    "eb.save": "Save",
    "eb.saveas": "Save as...",
    "eb.saving": "Saving...",
    "eb.saved": "Saved %s. Program and game data unchanged.",
    "eb.saved.bak": "Saved %s. Original kept as %s.",
    "eb.nothing": "Nothing changed.",
    "eb.busy": "Making %s...",
    "eb.error": "Not done: %s",
    "eb.unsaved": "Save the changes to %s?",
    "eb.replay": "Replay",
    "eb.preview": "Approximate preview of the PSP menu.",
    "eb.types.pbp": "PSP EBOOT",
    "eb.types.pic": "Pictures",
    "eb.types.clip": "Animated icon (PMF) or clip",
    "eb.types.snd": "Music (AT3) or audio",
    "eb.all": "All files",
}

SHAPES = ("wide", "square")
PIC_EXT = "*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tga"
CLIP_EXT = "*.pmf *.gif *.mp4 *.webm *.mkv *.avi *.mov *.apng *.webp"
SND_EXT = "*.at3 *.wav *.mp3 *.ogg *.flac *.m4a *.aac *.opus"


def tr(key, lang="en", *args):
    d = (i18n.TEXTS.get(lang) or {}).get("_eboot_editor") or {}
    s = d.get(key, T_EN.get(key, key))
    if args:
        try:
            return s % args
        except (TypeError, ValueError):
            return s
    return s


def human(n):
    return "%d B" % n if n < 10000 else "%.1f KB" % (n / 1000.0)


class _Bridge(QObject):
    call = Signal(object, object)


def make_icon1(src, budget):
    """-> (pmf bytes, frames for the preview or None, note). A ready .pmf is used as is."""
    if src.lower().endswith(".pmf"):
        with open(src, "rb") as f:
            data = f.read()
        return data, None, "ICON1.PMF: %s" % human(len(data))
    from ps2rc.xmb import clip
    frames = clip.frames(src, 0.0, clip.DEFAULT_SECONDS)
    pmf, rep = clip.make_icon1(None, budget=budget, frames_given=frames)
    return pmf, frames, "ICON1.PMF: %.1f s, %s" % (rep["seconds"], human(rep["bytes"]))


def make_snd0(src, budget):
    if src.lower().endswith(".at3"):
        with open(src, "rb") as f:
            data = f.read()
        return data, "SND0.AT3: %s" % human(len(data))
    from ps2rc.xmb import sound
    secs = min(sound.DEFAULT_SECONDS, sound.max_seconds(budget, sound.DEFAULT_CODEC))
    pcm = sound.shape(sound.read_pcm(src, 0.0, secs), loop=True)
    data, rep = sound.encode(pcm, loop=True, codec=sound.DEFAULT_CODEC)
    return data, "SND0.AT3: %.1f s, %s" % (rep["seconds"], human(rep["bytes"]))


class EbootEditor(QDialog):
    def __init__(self, parent=None, lang="en", start_dir=""):
        super().__init__(parent)
        self.lang, self.start_dir = lang, start_dir
        self.e = None
        self.src = {}              # slot -> (source path, crop box) of a picture chosen in this session
        self.frames = None         # ICON1 preview frames made in this session
        self.busy = False
        self.bridge = _Bridge()
        self.bridge.call.connect(lambda fn, a: fn(*a))
        self.setWindowTitle(self.t("eb.h"))
        self.setMinimumSize(980, 620)
        self._build()
        self.refresh()

    def t(self, key, *a):
        return tr(key, self.lang, *a)

    # ------------------------------------------------------------------------------------------ layout
    def _build(self):
        top = QVBoxLayout(self)
        sub = QLabel(self.t("eb.sub"))
        sub.setWordWrap(True)
        top.addWidget(sub)
        row = QHBoxLayout()
        self.open_btn = QPushButton(self.t("eb.open"))
        self.open_btn.clicked.connect(self._pick)
        row.addWidget(self.open_btn)
        self.path_lbl = QLabel(self.t("eb.none"))
        self.path_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
        row.addWidget(self.path_lbl, 1)
        top.addLayout(row)
        body = QHBoxLayout()
        left = QVBoxLayout()
        g = QGridLayout()
        g.addWidget(QLabel(self.t("eb.title")), 0, 0)
        self.title_edit = QLineEdit()
        self.title_edit.textEdited.connect(self._title_changed)
        g.addWidget(self.title_edit, 0, 1, 1, 4)
        self.title_bytes = QLabel("")
        g.addWidget(self.title_bytes, 0, 5)
        self.rows = {}
        for r, slot in enumerate(EDITABLE, start=1):
            name = QLabel(self.t("eb.slot." + slot))
            view = QLabel()
            view.setFixedSize(150, 86)
            view.setAlignment(Qt.AlignCenter)
            view.setStyleSheet("border: 1px solid #888;")
            info = QLabel("")
            info.setWordWrap(True)
            btns = QHBoxLayout()
            rep = QPushButton(self.t("eb.replace"))
            rep.clicked.connect(lambda _c=False, s=slot: self._replace(s))
            btns.addWidget(rep)
            crop = None
            if slot in IMAGE_SPECS:
                crop = QPushButton(self.t("eb.crop"))
                crop.clicked.connect(lambda _c=False, s=slot: self._crop(s))
                btns.addWidget(crop)
            rem = QPushButton(self.t("eb.remove"))
            rem.clicked.connect(lambda _c=False, s=slot: self._remove(s))
            btns.addWidget(rem)
            res = QPushButton(self.t("eb.restore"))
            res.clicked.connect(lambda _c=False, s=slot: self._restore(s))
            btns.addWidget(res)
            exp = QPushButton(self.t("eb.export"))
            exp.clicked.connect(lambda _c=False, s=slot: self._export(s))
            btns.addWidget(exp)
            btns.addStretch(1)
            cell = QWidget()
            cl = QVBoxLayout(cell)
            cl.setContentsMargins(0, 0, 0, 0)
            cl.addWidget(info)
            if slot == "ICON0.PNG":
                self.shape = QComboBox()
                for s in SHAPES:
                    self.shape.addItem(self.t("eb.shape." + s), s)
                self.shape.setToolTip(self.t("eb.shape.tip"))
                self.shape.activated.connect(self._shape_changed)
                cl.addWidget(self.shape)
            cl.addLayout(btns)
            g.addWidget(name, r, 0, Qt.AlignTop)
            g.addWidget(view, r, 1, Qt.AlignTop)
            g.addWidget(cell, r, 2, 1, 4)
            self.rows[slot] = dict(view=view, info=info, buttons=[rep, crop, rem, res, exp])
        left.addLayout(g)
        left.addWidget(QLabel("<b>%s</b>" % self.t("eb.checks")))
        self.checks = QPlainTextEdit()
        self.checks.setReadOnly(True)
        self.checks.setMaximumHeight(110)
        left.addWidget(self.checks)
        bottom = QHBoxLayout()
        self.fix_btn = QPushButton(self.t("eb.fix"))
        self.fix_btn.clicked.connect(self._fix)
        bottom.addWidget(self.fix_btn)
        self.backup = QCheckBox(self.t("eb.backup"))
        self.backup.setChecked(True)
        bottom.addWidget(self.backup)
        bottom.addStretch(1)
        self.save_btn = QPushButton(self.t("eb.save"))
        self.save_btn.clicked.connect(lambda: self.save())
        self.saveas_btn = QPushButton(self.t("eb.saveas"))
        self.saveas_btn.clicked.connect(self._save_as)
        bottom.addWidget(self.save_btn)
        bottom.addWidget(self.saveas_btn)
        left.addLayout(bottom)
        self.bar = QProgressBar()
        self.bar.setVisible(False)
        left.addWidget(self.bar)
        self.status = QLabel("")
        self.status.setWordWrap(True)
        left.addWidget(self.status)
        body.addLayout(left, 1)
        right = QVBoxLayout()
        from qt_widgets import PreviewWidget
        self.preview = PreviewWidget()
        right.addWidget(self.preview)
        right.addWidget(QLabel(self.t("eb.preview")))
        rp = QPushButton(self.t("eb.replay"))
        rp.clicked.connect(lambda: self.preview.replay())
        right.addWidget(rp, 0, Qt.AlignLeft)
        right.addStretch(1)
        body.addLayout(right)
        top.addLayout(body)

    # ------------------------------------------------------------------------------------------ helpers
    def say(self, text, kind=None):
        color = {"bad": "#d9453b", "good": "#2e9d4f"}.get(kind, "")
        self.status.setStyleSheet("color: %s;" % color if color else "")
        self.status.setText(text)

    def _thread(self, work, done):
        """work() in a thread; done(result, error) back in the window's thread"""
        self.busy = True
        self._enable()

        def run():
            try:
                r, err = work(), None
            except (PbpError, OSError) as x:
                r, err = None, str(x)
            except Exception as x:          # anything else from a tool: shown, never a crash of the window
                r, err = None, "%s: %s" % (type(x).__name__, x)
            self.bridge.call.emit(self._finish, (done, r, err))
        threading.Thread(target=run, daemon=True).start()

    def _finish(self, done, r, err):
        self.busy = False
        done(r, err)
        self._enable()

    def _enable(self):
        on = self.e is not None and not self.busy
        for row in self.rows.values():
            for b in row["buttons"]:
                if b is not None:
                    b.setEnabled(on)
        for w in (self.title_edit, self.shape, self.save_btn, self.saveas_btn, self.backup):
            w.setEnabled(on)
        self.open_btn.setEnabled(not self.busy)
        self.fix_btn.setEnabled(on and bool(self.e.fixable()))
        if on:
            for slot, row in self.rows.items():
                row["buttons"][3].setEnabled(self.e.files[slot] != self.e.orig[slot])
                row["buttons"][4].setEnabled(bool(self.e.files[slot]))
                if row["buttons"][1] is not None:
                    row["buttons"][1].setEnabled(slot in self.src or bool(self.e.files[slot]))

    def shape_now(self):
        return self.shape.currentData() or "wide"

    def spec_for(self, slot):
        return ICON0_SHAPES[self.shape_now()] if slot == "ICON0.PNG" else IMAGE_SPECS[slot]

    # ------------------------------------------------------------------------------------------ open / show
    def _pick(self):
        p, _f = QFileDialog.getOpenFileName(self, self.t("eb.open"), self.start_dir,
                                            "%s (*.pbp *.PBP);;%s (*)" % (self.t("eb.types.pbp"), self.t("eb.all")))
        if p:
            self.open(p)

    def open(self, path):
        if not self._confirm_unsaved():
            return False
        try:
            self.e = Eboot(path)
        except PbpError as x:
            self.say(self.t("eb.error", x), "bad")
            return False
        self.start_dir = os.path.dirname(path)
        self.src, self.frames = {}, None
        shape = self.e.icon0_shape()
        shape = shape if shape in SHAPES else self.e.default_icon0_shape()
        self.shape.setCurrentIndex(SHAPES.index(shape))
        self.title_edit.setText(self.e.title())
        self.say("")
        self.refresh()
        return True

    def refresh(self):
        e = self.e
        if e is None:
            self.path_lbl.setText(self.t("eb.none"))
            for row in self.rows.values():
                row["view"].clear()
                row["info"].setText("")
            self.checks.setPlainText("")
            self._enable()
            return
        kind = self.t("eb.kind." + e.kind)
        self.path_lbl.setText(self.t("eb.kind", e.path, kind))
        n = len(self.title_edit.text().encode("utf-8"))
        self.title_bytes.setText(self.t("eb.bytes", n, TITLE_MAX))
        for slot, row in self.rows.items():
            data = e.files[slot]
            row["view"].clear()
            if not data:
                row["info"].setText(self.t("eb.empty"))
                continue
            text = human(len(data))
            if slot in IMAGE_SPECS:
                try:
                    i = png.info(data)
                    text = "%dx%d %d-bit %s, %s" % (i["width"], i["height"], i["bit_depth"], i["mode"], text)
                except PbpError as x:
                    text = str(x)
                q = QImage.fromData(data)
                if not q.isNull():
                    row["view"].setPixmap(QPixmap.fromImage(q).scaled(148, 84, Qt.KeepAspectRatio,
                                                                       Qt.SmoothTransformation))
            if data != e.orig[slot]:
                text += " *"
            row["info"].setText(text)
        found = [i for i in e.issues() if i.level != "info"]
        self.checks.setPlainText("\n".join("%s: %s" % (i.what, i.message) for i in found) or self.t("eb.ok"))
        self._update_preview()
        self._enable()

    def _update_preview(self):
        try:
            from PIL import Image
            from ps2rc.xmb.preview import Mock
        except ImportError:
            return
        e = self.e

        def pil(slot):
            d = e.files[slot]
            try:
                return Image.open(io.BytesIO(d)).convert("RGBA") if d else None
            except Exception:       # a damaged picture is listed under Checks; the preview leaves it out
                return None
        icon = pil("ICON0.PNG")
        if icon is not None and icon.size == (80, 80):
            box = Image.new("RGBA", (144, 80), (0, 0, 0, 0))
            box.paste(icon, (32, 0), icon)
            icon = box
        frames = self.frames if (self.frames and e.files["ICON1.PMF"]) else None
        try:
            mock = Mock(pic1=pil("PIC1.PNG"), pic0=pil("PIC0.PNG"), icon0=icon, icon1_frames=frames,
                        title=self.title_edit.text())
            self.preview.set_mock(mock, self.t("eb.preview"))
        except Exception:           # the preview is a nicety
            pass

    # ------------------------------------------------------------------------------------------ edits
    def _title_changed(self, text):
        if self.e is None:
            return
        try:
            self.e.set_title(text)
            self.say("")
        except PbpError as x:
            self.say(self.t("eb.error", x), "bad")
        self.refresh()

    def _replace(self, slot):
        ext = {"ICON1.PMF": ("eb.types.clip", CLIP_EXT), "SND0.AT3": ("eb.types.snd", SND_EXT)}.get(
            slot, ("eb.types.pic", PIC_EXT))
        p, _f = QFileDialog.getOpenFileName(self, self.t("eb.replace"), self.start_dir,
                                            "%s (%s);;%s (*)" % (self.t(ext[0]), ext[1], self.t("eb.all")))
        if p:
            self.start_dir = os.path.dirname(p)
            self.load_file(slot, p)

    def load_file(self, slot, path, box=None, sync=False):
        """put a file into a slot: pictures through the crop box, clips / audio through the converter's tools"""
        e = self.e
        if slot in IMAGE_SPECS:
            try:
                data, note = picture(path, self.spec_for(slot), box=box)
                found = e.set_file(slot, data)
            except PbpError as x:
                self.say(self.t("eb.error", x), "bad")
                return False
            self.src[slot] = (path, box)
            warn = [i.message for i in found if i.level == "warn"]
            self.say(warn[0] if warn else note, "bad" if warn else None)
            self.refresh()
            return True
        other = e.files["SND0.AT3" if slot == "ICON1.PMF" else "ICON1.PMF"]
        budget = MEDIA_BUDGET - len(other)

        def work():
            if slot == "ICON1.PMF":
                return make_icon1(path, budget)
            data, note = make_snd0(path, budget)
            return data, None, note

        def done(r, err):
            if err is None:
                try:
                    found = e.set_file(slot, r[0])
                    if slot == "ICON1.PMF":
                        self.frames = r[1]
                    warn = [i.message for i in found if i.level == "warn"]
                    self.say(warn[0] if warn else r[2], "bad" if warn else None)
                except PbpError as x:
                    err = str(x)
            if err is not None:
                self.say(self.t("eb.error", err), "bad")
            self.refresh()
        self.say(self.t("eb.busy", self.t("eb.slot." + slot)))
        if sync:
            try:
                r, err = work(), None
            except (PbpError, OSError) as x:
                r, err = None, str(x)
            done(r, err)
            return err is None
        self._thread(work, done)
        return True

    def _crop(self, slot):
        src = self.src.get(slot)
        path, box = src if src else (None, None)
        raw = None
        if path is None:
            raw = self.e.files[slot]
        from qt_widgets import CropDialog
        q = QImage(path) if path else QImage.fromData(raw)
        if q.isNull():
            self.say(self.t("eb.error", "cannot read the picture"), "bad")
            return
        spec = self.spec_for(slot)
        d = CropDialog(self, q, (spec.width, spec.height), box, self.lang, self.t("eb.crop"))
        if d.exec() and d.result_box:
            if path is None:            # crop the picture that is in the EBOOT now
                tmp_box = d.result_box
                try:
                    data, note = picture(raw, spec, box=tmp_box)
                    self.e.set_file(slot, data)
                    self.say(note)
                except PbpError as x:
                    self.say(self.t("eb.error", x), "bad")
                self.refresh()
            else:
                self.load_file(slot, path, d.result_box)

    def _shape_changed(self, _i=None):
        """re-make ICON0 in the chosen shape (from the file chosen in this session, else from the EBOOT's own icon)"""
        if self.e is None or not self.e.files["ICON0.PNG"]:
            return
        if self.e.icon0_shape() == self.shape_now():
            return
        if "ICON0.PNG" in self.src:
            self.load_file("ICON0.PNG", self.src["ICON0.PNG"][0])      # the old crop box had the other aspect
            return
        try:
            data, note = picture(self.e.files["ICON0.PNG"], self.spec_for("ICON0.PNG"))
            found = self.e.set_file("ICON0.PNG", data)
            warn = [i.message for i in found if i.level == "warn"]
            self.say(warn[0] if warn else note, "bad" if warn else None)
        except PbpError as x:
            self.say(self.t("eb.error", x), "bad")
        self.refresh()

    def _remove(self, slot):
        self.e.remove(slot)
        self.src.pop(slot, None)
        self.refresh()

    def _restore(self, slot):
        self.e.restore(slot)
        self.src.pop(slot, None)
        if slot == "ICON0.PNG":
            sh = self.e.icon0_shape()
            if sh in SHAPES:
                self.shape.setCurrentIndex(SHAPES.index(sh))
        self.refresh()

    def _export(self, slot):
        p, _f = QFileDialog.getSaveFileName(self, self.t("eb.export"), os.path.join(self.start_dir, slot))
        if p:
            try:
                with open(p, "wb") as f:
                    f.write(self.e.files[slot])
            except OSError as x:
                self.say(self.t("eb.error", x.strerror or x), "bad")

    def _fix(self):
        try:
            notes = self.e.fix()
        except PbpError as x:
            self.say(self.t("eb.error", x), "bad")
            return
        sh = self.e.icon0_shape()
        if sh in SHAPES:
            self.shape.setCurrentIndex(SHAPES.index(sh))
        self.src.clear()
        self.say(self.t("eb.fixed", "; ".join(notes)), "good")
        self.refresh()

    # ------------------------------------------------------------------------------------------ save
    def _save_as(self):
        p, _f = QFileDialog.getSaveFileName(self, self.t("eb.saveas"), self.e.path,
                                            "%s (*.pbp *.PBP)" % self.t("eb.types.pbp"))
        if p:
            self.save(p)

    def save(self, out=None, sync=False):
        e = self.e
        if e is None:
            return False
        if out is None and not e.changed():
            self.say(self.t("eb.nothing"))
            return True
        self.bar.setVisible(True)
        self.bar.setRange(0, 1000)
        self.bar.setValue(0)
        self.say(self.t("eb.saving"))

        def prog(done, total):
            self.bridge.call.emit(self.bar.setValue, (int(1000 * done / max(1, total)),))

        def work():
            return e.write(out=out, backup=self.backup.isChecked(), progress=None if sync else prog)

        def done(res, err):
            self.bar.setVisible(False)
            if err is not None:
                self.say(self.t("eb.error", err), "bad")
                return
            keep_src, keep_frames = self.src, self.frames
            self.e = Eboot(res["path"])
            self.src, self.frames = keep_src, keep_frames
            if res["backup"]:
                self.say(self.t("eb.saved.bak", res["path"], res["backup"]), "good")
            else:
                self.say(self.t("eb.saved", res["path"]), "good")
            self.refresh()
        if sync:
            try:
                r, err = work(), None
            except (PbpError, OSError) as x:
                r, err = None, str(x)
            done(r, err)
            return err is None
        self._thread(work, done)
        return True

    def _confirm_unsaved(self):
        if self.e is None or not self.e.changed() or self.busy:
            return not self.busy
        a = QMessageBox.question(self, self.t("eb.h"), self.t("eb.unsaved", self.e.path),
                                 QMessageBox.Save | QMessageBox.Discard | QMessageBox.Cancel)
        if a == QMessageBox.Save:
            return self.save(sync=True)
        return a == QMessageBox.Discard

    def closeEvent(self, ev):
        if self._confirm_unsaved():
            self.preview.stop()
            ev.accept()
        else:
            ev.ignore()


def open_editor(parent, lang="en", path=None):
    """the window's menu entry: a non-modal editor window"""
    d = EbootEditor(parent, lang)
    d.setAttribute(Qt.WA_DeleteOnClose, True)
    if path:
        d.open(path)
    d.show()
    return d
