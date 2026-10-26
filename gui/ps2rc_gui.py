"""PS2RC converter - window (Qt for Python / PySide6, Qt Widgets; works offline). English by default, Russian from the
View menu or the language box.

    python gui/ps2rc_gui.py            (or the bundle's "PS2RC Converter.exe")

Start page: "Convert my disc" (disc -> where -> your own menu look -> game settings -> convert) or "Check / update a
PSP folder" (find converted games on a connected PSP, check their files, repair / update them).
Long work runs in worker threads; their results come back to the window through a queued Qt signal.
Remembered between runs in %APPDATA%\\PS2RC\\gui.json (written atomically, no lock file): language, interface size,
disc, output, menu look, settings, options.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for _p in (ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtCore import QCoreApplication, QObject, Qt, QTimer, Signal  # noqa: E402
from PySide6.QtGui import QAction, QActionGroup, QFont, QFontMetrics, QPalette  # noqa: E402
from PySide6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QComboBox, QDialog, QFileDialog,  # noqa: E402
                               QSplitter, QFormLayout, QFrame, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
                               QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton, QRadioButton,
                               QScrollArea, QSlider, QSpinBox, QTabWidget, QTreeWidget, QTreeWidgetItem, QVBoxLayout,
                               QWidget)

from ps2rc.convert import VERSION  # noqa: E402
from ps2rc.convert.steps import ConvertError, Progress  # noqa: E402
from ps2rc.pbp.spec import TITLE_MAX, MEDIA_BUDGET, MEDIA_BUDGET_MAX  # noqa: E402
from ps2rc.xmb import Project  # noqa: E402
from ps2rc.xmb import title as xtitle  # noqa: E402

import backend  # noqa: E402
import cfgform  # noqa: E402
import controls_model  # noqa: E402
import controls_profiles  # noqa: E402
import qt_wheel  # noqa: E402


def _legacy_cfg_names():
    """the older settings file names of the game packs ([runtime] cfg_legacy), read by the runtime as a fallback"""
    out = []
    try:
        from ps2rc import pack as _pk
        for d in sorted(os.listdir(_pk.games_dir())):
            try:
                n = _pk.load(os.path.join(_pk.games_dir(), d)).get("runtime.cfg_legacy")
            except (SystemExit, Exception):
                continue
            if n and n not in out:
                out.append(n)
    except OSError:
        pass
    return tuple(out)
import eta as etamod  # noqa: E402
import i18n  # noqa: E402
import psp_drives  # noqa: E402
from qt_widgets import CropDialog, PreviewWidget, qimage_from_pil, state_pixmap  # noqa: E402
from qt_busy import BusyRow, GalleryDialog, MusicDialog  # noqa: E402
from qt_controls import ConsolePicture, glyph_pixmap  # noqa: E402

APPDIR = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "PS2RC")
SETTINGS = os.path.join(APPDIR, "gui.json")
WIZARD = ("disc", "output", "customize", "settings", "controls", "convert")
# the controls page's suggestions: combinations, right stick values (any other value can be typed)
COMBO_SUGGEST = ("l+r", "select+l", "select+r", "select+triangle", "select+circle", "r+square", "l+triangle",
                 "r+triangle", "l+circle", "r+circle", "l+cross", "l+square", "l+analog_left", "l+analog_right",
                 "l+analog_up", "l+analog_down", "l+left", "l+right", "l+up", "l+down", "l+r+left", "l+r+right")
RSTICK_SUGGEST = ("select+dpad", "l+dpad", "r+dpad", "l+analog", "r+analog", "none")
UI_SIZES = (("small", -1), ("normal", 0), ("large", 2))       # View > Interface size: points added to the system font
CLIP_TYPES = "*.gif *.mp4 *.webm *.mkv *.mov *.avi *.m4v *.pmf"
MUSIC_TYPES = "*.wav *.mp3 *.ogg *.flac *.m4a *.aac *.opus *.at3"
PICTURE_TYPES = "*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tga"
GOOD, BAD, WARN, BLUE = "#2e9d4f", "#d9453b", "#c27c0e", "#3b82f6"


def local_dir(*parts):
    base = os.environ.get("LOCALAPPDATA") or APPDIR
    return os.path.join(base, "PS2RC", *parts)


PROJECT_URL = "https://github.com/SandstoneDev/PS2RC"
DISCORD_URL = "https://discord.gg/wV8x46ZTDu"
# the update plan's fixed reasons (ps2rc.convert.manifest.plan_from) -> texts of the window
WHY_KEYS = {"missing": "why.missing", "damaged": "why.damaged", "OK": "why.ok",
            "new in this converter version": "why.new", "changed in this converter version": "why.changed",
            "your settings": "why.yours", "your settings kept, new settings added": "why.merged",
            "missing: written from the template": "why.template",
            "an older version made it; not used any more": "why.unused",
            "the runtime or the game pack changed since it was made": "why.runtime"}
JOBS_RX = re.compile(r"^compile: (\d+) parallel jobs instead of \d+")


def show_in_explorer(path, select=False):
    """a folder (or a file, selected in its folder) in Explorer - never a file opened by its association - with a
    clean environment (no Qt / Python variables of ours)"""
    from ps2rc import proc
    env = proc.clean_env()
    if os.name == "nt":
        windir = os.environ.get("SystemRoot") or os.environ.get("windir") or ""
        exe = os.path.join(windir, "explorer.exe") if windir else "explorer.exe"
        args = [exe, "/select,", os.path.normpath(path)] if select else [exe, os.path.normpath(path)]
    else:
        args = ["xdg-open", os.path.dirname(path) if select else path]
    try:
        subprocess.Popen(args, env=env, close_fds=True)
    except OSError:
        pass


def work_dir(out):
    """the conversion's work folder: %LOCALAPPDATA%\\PS2RC\\work, or next to the output when the user folder's path
    has letters outside this Windows' code page (the PSP assembler / linker cannot open such paths)"""
    from ps2rc.xmb import media
    w = local_dir("work")
    return w if media.ansi_safe(w) or not out else os.path.join(out, "_ps2rc_work")


def safe_temp(work):
    """TEMP / TMP for the compiler: kept when plain ASCII, else <work>\\tmp, or ProgramData\\PS2RC\\tmp when the work
    folder is not ASCII either (measured: with a Cyrillic TEMP the PSP linker's LTO step cannot open its resolution
    file; with an accented one the assembler cannot read its .s file) - e.g. a Cyrillic user name"""
    from ps2rc.xmb import media
    if not work or all(media.ascii_path(os.environ.get(k) or work) for k in ("TEMP", "TMP")):
        return None
    t = os.path.join(work, "tmp")
    if not media.ascii_path(t):
        t = os.path.join(os.environ.get("ProgramData") or os.environ.get("PUBLIC") or work, "PS2RC", "tmp")
    os.makedirs(t, exist_ok=True)
    os.environ["TEMP"] = os.environ["TMP"] = t
    return t


def load_settings(path=SETTINGS):
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(d, path=SETTINGS):
    """atomic JSON write without lock files: a window killed while saving never blocks the next start"""
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = "%s.%d.tmp" % (path, os.getpid())
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=1)
        os.replace(tmp, path)
    except OSError:
        pass


def human_bytes(n):
    if n is None:
        return "?"
    if n == 0:
        return "0"
    if n >= 1e9:
        return "%.1f GB" % (n / 1e9)
    if n >= 1e6:
        return "%.1f MB" % (n / 1e6)
    if n >= 1e3:
        return "%d KB" % round(n / 1e3)
    return "%d B" % n


def system_lang():
    """the display language of the system: "ru" for Russian, "en" for every other language"""
    try:
        from PySide6.QtCore import QLocale
        langs = [x.lower() for x in QLocale.system().uiLanguages()]
        if langs:
            return "ru" if langs[0].startswith("ru") else "en"
    except Exception:          # no Qt locale: ask Windows
        pass
    if os.name == "nt":
        try:
            import ctypes
            return "ru" if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF == 0x19 else "en"
        except (AttributeError, OSError):
            pass
    return "en"


class Bridge(QObject):
    """worker threads -> the window's thread (queued signal)"""
    call = Signal(object, object)


def hint_color():
    """between the text and the window colour: secondary text on light and dark palettes"""
    pal = QApplication.palette()
    t, w = pal.color(QPalette.WindowText), pal.color(QPalette.Window)
    mix = lambda a, b: int(a * 0.55 + b * 0.45)          # noqa: E731
    return "#%02x%02x%02x" % (mix(t.red(), w.red()), mix(t.green(), w.green()), mix(t.blue(), w.blue()))


def set_kind(w, kind):
    """h1 / h2 / hint / good / bad / warn / None - fonts and colours that read on light and dark palettes"""
    f = QApplication.font()
    color = {"good": GOOD, "bad": BAD, "warn": WARN}.get(kind, "")
    if kind == "h1":
        f.setPointSizeF(f.pointSizeF() + 2)
        f.setBold(True)
    elif kind in ("h2", "good", "bad"):
        f.setBold(True)
    if kind == "hint":
        color = hint_color()
    w.setFont(f)
    w.setStyleSheet("color: %s;" % color if color else "")
    w.setProperty("kind", kind)


def label(text="", kind=None, wrap=False):
    w = QLabel(text)
    w.setWordWrap(wrap)
    set_kind(w, kind)
    return w


def hbox(*widgets, stretch=True, spacing=6):
    """a row of widgets; None = a stretch"""
    w = QWidget()
    lay = QHBoxLayout(w)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(spacing)
    for x in widgets:
        if x is None:
            lay.addStretch(1)
        else:
            lay.addWidget(x)
    if stretch:
        lay.addStretch(1)
    return w


class App(QMainWindow):
    def __init__(self, settings_path=None):
        super().__init__()
        self.settings_path = settings_path or SETTINGS
        self.s = load_settings(self.settings_path)
        self.lang = self.choose_lang()
        self.bridge = Bridge()
        self.bridge.call.connect(lambda fn, a: self._safe(fn, a))
        self.project = Project.from_dict(self.s.get("project"))
        self._apply_music_choice()
        self.values = dict(self.s.get("settings_values") or {})
        self.ident, self.template_items, self.template_text = None, None, None
        self.ctl_folder, self.ctl_folder_vals, self.ctl_folder_items, self.ctl_folder_cfg = None, None, None, None
        self.built, self.icon1_frames = None, None
        self.arts, self.arts_busy = None, False
        self.page, self.preview, self.worker, self.progress, self.conv = None, None, None, None, None
        self.running = False
        self.result_folder = None
        self.log_lines, self.step_state, self.eta = [], {}, None
        self._asset_busy, self._asset_dirty = False, False
        self.selected_folder = self.s.get("check_folder")
        self.drive_list, self.folders = [], []
        self.next_btn, self.slot_ui = None, {}
        self._ctl_tab_i, self._ctl_msg, self._ctl_scroll = 0, "", {}
        self._base_font = QFont(QApplication.font())
        self._apply_size()
        self.setWindowTitle("%s %s" % (self.t("app"), VERSION))
        self.setMinimumSize(860, 560)
        size = self.s.get("size") or (960, 620)
        self.resize(int(size[0]), int(size[1]))
        self._asset_timer = QTimer(self)
        self._asset_timer.setSingleShot(True)
        self._asset_timer.timeout.connect(self._start_assets)
        self._eta_timer = QTimer(self)
        self._eta_timer.timeout.connect(self._update_eta)
        self._eta_timer.start(500)
        self.show_page("home")
        if self.s.get("source") and os.path.exists(self.s["source"]):
            self._identify_async(self.s["source"])

    # ------------------------------------------------------------------------------------------------ basics
    def t(self, key, *a):
        return i18n.tr(key, self.lang, *a)

    def _safe(self, fn, a):
        try:
            fn(*a)
        except RuntimeError:            # the widget it was for is gone (the page changed)
            pass

    def _ui(self, fn, *a):
        """run fn(*a) in the window's thread (from a worker)"""
        self.bridge.call.emit(fn, a)

    def _thread(self, fn, *a):
        th = threading.Thread(target=fn, args=a, daemon=True)
        th.start()
        return th

    def _apply_size(self):
        f = QFont(self._base_font)
        f.setPointSizeF(max(6.0, self._base_font.pointSizeF() + dict(UI_SIZES).get(self.s.get("ui_size"), 0)))
        QApplication.setFont(f)

    def _save(self):
        self.s.update(lang=self.lang, project=self.project.to_dict(), settings_values=self.values,
                      page=self.page, check_folder=self.selected_folder, size=[self.width(), self.height()])
        save_settings(self.s, self.settings_path)

    def _menu(self):
        mb = self.menuBar()
        mb.clear()
        view = mb.addMenu(self.t("menu.view"))
        sizes = view.addMenu(self.t("menu.size"))
        g = QActionGroup(self)
        for code, _d in UI_SIZES:
            a = QAction(self.t("size." + code), self, checkable=True)
            a.setChecked((self.s.get("ui_size") or "normal") == code)
            a.triggered.connect(lambda _c=False, c=code: self._set_size(c))
            g.addAction(a)
            sizes.addAction(a)
        # Tools > EBOOT editor
        import eboot_editor
        tools = mb.addMenu(eboot_editor.tr("eb.menu.tools", self.lang))
        a = QAction(eboot_editor.tr("eb.menu.editor", self.lang), self)
        a.triggered.connect(lambda: eboot_editor.open_editor(self, self.lang))
        tools.addAction(a)
        helpm = mb.addMenu(self.t("menu.help"))
        a = QAction(self.t("about.h"), self)
        a.triggered.connect(self._about)
        helpm.addAction(a)
        a = QAction(self.t("about.licences"), self)
        a.triggered.connect(self._open_licences)
        helpm.addAction(a)
        langs = view.addMenu(self.t("lang"))
        g2 = QActionGroup(self)
        choice = self.s.get("lang_choice") or "system"
        for code, name in self.lang_entries():
            a = QAction(name, self, checkable=True)
            a.setChecked(code == choice)
            a.triggered.connect(lambda _c=False, c=code: self._set_lang(c))
            g2.addAction(a)
            langs.addAction(a)

    def _licences_dir(self):
        """the app folder's licenses/ (the bundle), else the tree's gui/licenses"""
        for d in (os.path.join(os.path.dirname(ROOT), "licenses"), os.path.join(HERE, "licenses")):
            if os.path.isdir(d):
                return d
        return None

    def _open_licences(self):
        d = self._licences_dir()
        if d:
            show_in_explorer(d)

    def _about_dialog(self):
        """version, licence, credits, project and community links, third-party notices (Explorer only)"""
        d = QDialog(self)
        d.setWindowTitle(self.t("about.h"))
        lay = QVBoxLayout(d)
        top = QHBoxLayout()
        try:
            import app_icon
            pic = QLabel()
            pic.setPixmap(app_icon.qicon(app_icon.DEFAULT).pixmap(64, 64))
            top.addWidget(pic)
        except Exception:      # the icon is a nicety
            pass
        head = QLabel("<b>%s %s</b><br>%s" % (self.t("app"), VERSION, self.t("about.what")))
        head.setWordWrap(True)
        top.addWidget(head, 1)
        lay.addLayout(top)
        body = QLabel("%s<br>%s<br>%s <a href=\"%s\">%s</a><br>%s <a href=\"%s\">%s</a>" % (
            self.t("about.licence"), self.t("about.credits"), self.t("about.project"), PROJECT_URL,
            PROJECT_URL.split("//", 1)[1], self.t("about.discord"), DISCORD_URL, DISCORD_URL.split("//", 1)[1]))
        body.setTextFormat(Qt.RichText)
        body.setOpenExternalLinks(True)                    # a link the player clicks opens in his browser
        body.setWordWrap(True)
        lay.addWidget(body)
        note = label(self.t("about.third"), "hint", wrap=True)
        lay.addWidget(note)
        lic = QPushButton(self.t("about.licences"))
        lic.clicked.connect(self._open_licences)
        lic.setEnabled(self._licences_dir() is not None)
        close = QPushButton(self.t("about.close"))
        close.clicked.connect(d.accept)
        lay.addWidget(hbox(lic, None, close))
        d.setMinimumWidth(480)
        d.about = {"head": head, "body": body, "note": note, "licences": lic}
        return d

    def _about(self):
        self._about_dialog().exec()

    def lang_entries(self):
        """("system", "System default (English)") first, then every language"""
        sysname = dict(i18n.LANGS).get(system_lang(), "English")
        return [("system", "%s (%s)" % (self.t("lang.system"), sysname))] + list(i18n.LANGS)

    def _set_size(self, code):
        self.s["ui_size"] = code
        self._apply_size()
        self.show_page(self.page)

    def choose_lang(self):
        """the player's choice (View > Language), else the system's UI language: Russian -> ru, else English"""
        choice = self.s.get("lang_choice") or "system"
        if choice in [c for c, _n in i18n.LANGS]:
            return choice
        return system_lang()

    def _set_lang(self, code):
        """code: a language code, or "system" (follow the Windows display language again)"""
        self.s["lang_choice"] = code
        new = self.choose_lang()
        if new == self.lang and self.page is not None:
            self._save()
            self._menu()
            return
        self.lang = new
        self._load_template()
        self.setWindowTitle("%s %s" % (self.t("app"), VERSION))
        self.show_page(self.page)

    # ------------------------------------------------------------------------------------------------ pages
    def show_page(self, page):
        if self.preview is not None:
            self.preview.stop()
            self.preview = None
        self._stop_sound()
        self.page = page
        self.next_btn, self.slot_ui = None, {}
        self._menu()
        central = QWidget()
        outer = QVBoxLayout(central)
        outer.setContentsMargins(10, 6, 10, 8)
        outer.setSpacing(6)
        outer.addWidget(self._header())
        self.body = QWidget()
        self.body_lay = QVBoxLayout(self.body)
        self.body_lay.setContentsMargins(0, 0, 0, 0)
        self.body_lay.setSpacing(6)
        outer.addWidget(self.body, 1)
        self.footer = QHBoxLayout()
        outer.addLayout(self.footer)
        self.setCentralWidget(central)
        getattr(self, "_page_" + page)()
        self._save()

    def _header(self):
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        if self.page in WIZARD:
            i = WIZARD.index(self.page)
            for j, name in enumerate(self.t("steps")):
                lab = QLabel("%d %s" % (j + 1, name))
                f = QApplication.font()
                f.setBold(j == i)
                lab.setFont(f)
                lab.setStyleSheet("color: %s;" % (BLUE if j == i else GOOD if j < i else hint_color()))
                lay.addWidget(lab)
                if j < len(WIZARD) - 1:
                    lay.addWidget(label(">", "hint"))
        lay.addStretch(1)
        lay.addWidget(QLabel(self.t("lang") + ":"))
        cb = QComboBox()
        entries = self.lang_entries()
        for code, name in entries:
            cb.addItem(name, code)
        cb.setCurrentIndex([c for c, _n in entries].index(self.s.get("lang_choice") or "system"))
        cb.currentIndexChanged.connect(lambda _i: self._set_lang(cb.currentData()))
        lay.addWidget(cb)
        return w

    def _nav(self, back=None, nxt=None, next_text=None, next_ok=True):
        if back:
            b = QPushButton(self.t("back"))
            b.clicked.connect(lambda: self.show_page(back))
            self.footer.addWidget(b)
        h = QPushButton(self.t("home"))
        h.clicked.connect(lambda: self.show_page("home"))
        self.footer.addWidget(h)
        self.footer.addStretch(1)
        if nxt:
            b = QPushButton(next_text or self.t("next"))
            b.setDefault(True)
            b.clicked.connect(lambda: self.show_page(nxt))
            b.setEnabled(next_ok)
            self.footer.addWidget(b)
            self.next_btn = b
            return b

    def add(self, w, stretch=0):
        self.body_lay.addWidget(w, stretch)
        return w

    # --- home -------------------------------------------------------------------------------------------------
    def _page_home(self):
        self.body_lay.addSpacing(24)
        self.add(label(self.t("home.h"), "h1")).setAlignment(Qt.AlignHCenter)
        self.add(label(self.t("home.sub"), "hint")).setAlignment(Qt.AlignHCenter)
        self.body_lay.addSpacing(12)
        row = QHBoxLayout()
        row.addStretch(1)
        for key, page in (("home.convert", "disc"), ("home.check", "folders")):
            col = QVBoxLayout()
            b = QPushButton(self.t(key))
            f = QApplication.font()
            f.setBold(True)
            b.setFont(f)
            b.setMinimumSize(260, 44)
            b.clicked.connect(lambda _c=False, p=page: self.show_page(p))
            col.addWidget(b)
            d = label(self.t(key + ".sub"), "hint", wrap=True)
            d.setAlignment(Qt.AlignHCenter)
            d.setMaximumWidth(260)
            col.addWidget(d)
            row.addLayout(col)
            row.addSpacing(16)
        row.addStretch(1)
        self.body_lay.addLayout(row)
        self.body_lay.addStretch(1)
        self.footer.addStretch(1)
        self.footer.addWidget(label(self.t("home.version", VERSION), "hint"))

    # --- disc -------------------------------------------------------------------------------------------------
    def _page_disc(self):
        self.add(label(self.t("disc.h"), "h1"))
        self.add(label(self.t("disc.sub"), "hint", wrap=True))
        pick = QPushButton(self.t("disc.pick"))
        pick.clicked.connect(self._pick_disc)
        folder = QPushButton(self.t("disc.folder"))
        folder.clicked.connect(self._pick_disc_folder)
        self.add(hbox(pick, folder))
        src = self.s.get("source") or ""
        self.add(QLabel(src or self.t("disc.none")))
        self.disc_status = self.add(label("", wrap=True))
        self.disc_hint = self.add(label("", "hint", wrap=True))
        self.body_lay.addStretch(1)
        self._nav(None, "output", next_ok=bool(src and os.path.exists(src)))
        self._show_ident()

    def _pick_disc(self):
        p, _f = QFileDialog.getOpenFileName(self, self.t("disc.pick"),
                                            os.path.dirname(self.s.get("source") or "") or "",
                                            "%s (*.iso *.bin *.img);;%s (*.*)" % (self.t("disc.types"),
                                                                                  self.t("pick.all")))
        if p:
            self._set_source(p)

    def _pick_disc_folder(self):
        p = QFileDialog.getExistingDirectory(self, self.t("disc.folder"), self.s.get("source") or "")
        if p:
            self._set_source(p)

    def _set_source(self, p):
        self.s["source"] = os.path.normpath(p)
        self.ident, self.arts = None, None
        self._save()
        self._identify_async(self.s["source"])
        self.show_page("disc")

    def _identify_async(self, src):
        self.ident = {"busy": True}

        def work():
            try:
                res = backend.identify(src)
            except Exception as e:         # never let identification kill the window
                res = {"ok": False, "code": "internal", "message": "%s: %s" % (type(e).__name__, e), "hint": ""}
            self._ui(self._identified, src, res)
        self._thread(work)

    def _identified(self, src, res):
        if src != self.s.get("source"):
            return
        self.ident = res
        self._load_template()
        if self.page == "disc":
            self._show_ident()

    def _show_ident(self):
        if self.page != "disc":
            return
        r = self.ident
        ok = bool(self.s.get("source") and os.path.exists(self.s["source"]))
        if r is None or not self.s.get("source"):
            self.disc_status.setText("")
        elif r.get("busy"):
            self.disc_status.setText(self.t("disc.checking"))
            set_kind(self.disc_status, None)
        elif r.get("ok") and r.get("code") == "folder":
            self.disc_status.setText(self.t("disc.folder_ok"))
            set_kind(self.disc_status, "good")
        elif r.get("ok"):
            self.disc_status.setText(self.t("disc.ok", r.get("title"), r.get("serial"), r.get("version")))
            set_kind(self.disc_status, "good")
        else:
            what, todo = i18n.error_text(r.get("code"), r.get("message"), r.get("hint"), self.lang)
            self.disc_status.setText(self.t("disc.bad", what))
            set_kind(self.disc_status, "bad")
            detail = r.get("message") or ""
            self.disc_hint.setText("%s\n%s" % (self.t("err.what", todo), detail if detail != what else ""))
            ok = False
        if self.next_btn is not None:
            self.next_btn.setEnabled(ok)

    # --- output -----------------------------------------------------------------------------------------------
    def _page_output(self):
        self.add(label(self.t("out.h"), "h1"))
        self.add(label(self.t("out.sub"), "hint", wrap=True))
        self.drive_list = psp_drives.drives()
        self.out_group = QButtonGroup(self)
        cur = self.s.get("out_choice") or ""
        psp_seen = False
        for d in self.drive_list:
            key = "drive:" + d["root"]
            text = self.t("out.psp" if d["is_psp"] else "out.drive", d["root"].rstrip("\\/") +
                          (" %s" % d["label"] if d["label"] else ""), human_bytes(d["free"]))
            rb = QRadioButton(text)
            rb.setProperty("choice", key)
            rb.setChecked(cur == key)
            self.out_group.addButton(rb)
            self.add(rb)
            psp_seen = psp_seen or d["is_psp"]
        if not psp_seen:
            self.add(label(self.t("out.none"), "warn", wrap=True))
        pc = QRadioButton(self.t("out.pc"))
        pc.setProperty("choice", "pc")
        pc.setChecked(cur == "pc")
        self.out_group.addButton(pc)
        self.pc_edit = QLineEdit(self.s.get("out_pc") or "")
        self.pc_edit.setMinimumWidth(420)
        self.pc_edit.editingFinished.connect(self._out_changed)
        br = QPushButton(self.t("out.browse"))
        br.clicked.connect(self._pick_out)
        self.add(hbox(pc, self.pc_edit, br))
        if not any(b.isChecked() for b in self.out_group.buttons()):
            psp = [b for b in self.out_group.buttons() if str(b.property("choice")).startswith("drive:")]
            (psp[0] if psp and psp_seen else pc).setChecked(True)
        self.out_group.buttonClicked.connect(lambda _b: self._out_changed())
        ref = QPushButton(self.t("out.refresh"))
        ref.clicked.connect(lambda: self.show_page("output"))
        self.add(hbox(ref))
        self.out_target = self.add(label("", "h2", wrap=True))
        self.out_note = self.add(label("", "hint", wrap=True))
        self.body_lay.addStretch(1)
        self._nav("disc", "customize")
        self._out_changed()

    def _pick_out(self):
        p = QFileDialog.getExistingDirectory(self, self.t("out.pc"), self.pc_edit.text() or "")
        if p:
            self.pc_edit.setText(os.path.normpath(p))
            for b in self.out_group.buttons():
                if b.property("choice") == "pc":
                    b.setChecked(True)
            self._out_changed()

    def out_dir(self):
        """the folder that receives <game folder>/ (PSP/GAME on a PSP drive)"""
        c = self.s.get("out_choice") or ""
        if c.startswith("drive:"):
            return os.path.join(c[6:], "PSP", "GAME")
        return self.s.get("out_pc") or ""

    def out_is_drive(self):
        return (self.s.get("out_choice") or "").startswith("drive:")

    def _game_folder_name(self):
        """the PSP folder's name: the one the player accepted instead of a taken name, else PS2RC_<serial>"""
        ident = self.ident or {}
        alt = (self.s.get("game_folders") or {}).get(ident.get("serial") or "")
        return alt or ident.get("folder") or "PS2RC_GAME"

    def _out_changed(self):
        b = self.out_group.checkedButton()
        self.s["out_choice"] = b.property("choice") if b is not None else ""
        self.s["out_pc"] = self.pc_edit.text().strip()
        self._save()
        out = self.out_dir()
        if out:
            target = os.path.join(out, self._game_folder_name())
            self.out_target.setText(self.t("out.target", target))
            notes = []
            if backend.foreign_folder(target):
                notes.append(self.t("out.foreign_note"))
            elif os.path.isdir(target):
                notes.append(self.t("out.exists"))
            if self.out_is_drive():
                notes.append(self.t("out.slow"))
            self.out_note.setText("  ".join(notes))
        else:
            self.out_target.setText(self.t("out.need"))
            self.out_note.setText("")
        if self.next_btn is not None:
            self.next_btn.setEnabled(bool(out))

    # --- customize --------------------------------------------------------------------------------------------
    def _default_title(self):
        r = self.ident or {}
        return r.get("title") or "PS2RC"

    def _shown_title(self):
        return xtitle.clean_title(self.project.title) or self._default_title()

    def _page_customize(self):
        top = QHBoxLayout()
        top.addWidget(label(self.t("cus.h"), "h1"))
        top.addStretch(1)
        self.art_btn = QPushButton(self.t("gal.use_all"))
        self.art_btn.setToolTip(self.t("gal.use_all_tip"))
        self.art_btn.clicked.connect(self._use_game_art)
        self.art_btn.setEnabled(self._disc_art_possible())
        self.art_busy = BusyRow()
        self.art_busy.hide()
        top.addWidget(self.art_busy)
        top.addWidget(self.art_btn)
        reset = QPushButton(self.t("cus.reset"))
        reset.clicked.connect(self._reset_look)
        top.addWidget(reset)
        self.body_lay.addLayout(top)
        main = QHBoxLayout()
        tabs = QTabWidget()
        for lab, keys in (("cus.tab.title", ("title", "icon0")), ("cus.tab.icon1", ("icon1",)),
                          ("cus.tab.pic1", ("pic1",)), ("cus.tab.pic0", ("pic0",)), ("cus.tab.snd0", ("snd0",))):
            page = QWidget()
            lay = QVBoxLayout(page)
            lay.setContentsMargins(8, 8, 8, 8)
            lay.setSpacing(4)
            for k in keys:
                if k == "title":
                    self._title_box(lay)
                else:
                    self._slot_box(lay, k)
            lay.addStretch(1)
            tabs.addTab(page, self.t(lab))
        tabs.setCurrentIndex(self.s.get("cus_tab", 0) if isinstance(self.s.get("cus_tab"), int) else 0)
        tabs.currentChanged.connect(lambda i: self.s.__setitem__("cus_tab", i))
        self.cus_tabs = tabs
        main.addWidget(tabs, 1)
        right = QVBoxLayout()
        right.addWidget(label(self.t("pv.title"), "h2"))
        self.preview = PreviewWidget()
        self.preview.note = self.t("pv.nopillow")
        right.addWidget(self.preview)
        rep = QPushButton(self.t("pv.replay"))
        rep.clicked.connect(lambda: self.preview.replay())
        self.pv_sound = QCheckBox(self.t("pv.sound"))
        self.pv_sound.toggled.connect(self._preview_sound)
        right.addWidget(hbox(rep, self.pv_sound))
        self.budget_bar = QProgressBar()
        self.budget_bar.setRange(0, MEDIA_BUDGET)
        self.budget_bar.setTextVisible(False)
        self.budget_bar.setFixedWidth(PreviewWidget.W)
        right.addWidget(self.budget_bar)
        self.budget_label = label("")
        right.addWidget(self.budget_label)
        bh = label(self.t("budget.hint"), "hint", wrap=True)
        bh.setMaximumWidth(PreviewWidget.W)
        right.addWidget(bh)
        right.addStretch(1)
        main.addLayout(right)
        self.body_lay.addLayout(main, 1)
        self._nav("output", "settings")
        self._refresh_look()
        self._schedule_assets()

    def _title_box(self, lay):
        self.title_edit = QLineEdit(self.project.title)
        self.title_edit.setPlaceholderText(self._default_title())
        lay.addWidget(hbox(QLabel(self.t("cus.title") + ":"), self.title_edit, stretch=False))
        lay.addWidget(label(self.t("cus.title.default", self._default_title()), "hint"))
        self.title_msg = label("", "hint", wrap=True)
        lay.addWidget(self.title_msg)
        self.title_edit.textChanged.connect(lambda _t: self._title_changed())
        line = QFrame()
        line.setFrameShape(QFrame.HLine)
        lay.addWidget(line)
        self._title_changed(initial=True)

    def _title_changed(self, initial=False):
        text = self.title_edit.text()
        self.project.title = text
        msgs, kind = [], "hint"
        if text.strip():
            for level, code, detail in xtitle.check_title(text):
                msgs.append(self.t("title." + code, detail) if detail else self.t("title." + code))
                if level == "error":
                    kind = "bad"
                elif (level == "warn" or code == "long") and kind != "bad":
                    kind = "warn"
        n = len(xtitle.clean_title(text).encode("utf-8"))
        msgs.insert(0, self.t("cus.title.bytes", n, TITLE_MAX))
        self.title_msg.setText("   ".join(m for m in msgs if m))
        set_kind(self.title_msg, kind)
        if not initial:
            self._save()
            if self.preview is not None and self.preview.mock is not None:
                self.preview.mock.title = self._shown_title()

    def _slot_box(self, lay, key):
        s = self.project.slots[key]
        ui = {}
        self.slot_ui[key] = ui
        if key == "icon0":
            lay.addWidget(label(self.t("icon0.h"), "h2"))
        lay.addWidget(label(self.t(key + ".hint"), "hint", wrap=True))
        g = QButtonGroup(self)
        ui["group"] = g
        btns = []
        for mode, txt in (("none", "slot.none"), ("default", "slot.default"), ("file", "slot.file")):
            if mode == "default" and key not in ("icon0", "icon1", "pic0", "pic1"):
                continue
            rb = QRadioButton(self.t(txt))
            rb.setProperty("mode", mode)
            rb.setChecked(s["mode"] == mode)
            g.addButton(rb)
            btns.append(rb)
        g.buttonClicked.connect(lambda b, k=key: self._slot_mode(k, b.property("mode")))
        choose = QPushButton(self.t("slot.browse"))
        choose.clicked.connect(lambda _c=False, k=key: self._slot_pick(k))
        btns.append(choose)
        if key != "snd0":
            crop = QPushButton(self.t("slot.crop"))
            crop.clicked.connect(lambda _c=False, k=key: self._slot_crop(k))
            btns.append(crop)
        lay.addWidget(hbox(*btns))
        if key in ("icon0", "pic0", "pic1", "icon1"):
            disc = QPushButton(self.t("gal.from_disc"))
            disc.setEnabled(self._disc_art_possible())
            disc.setToolTip(self.t("gal.tip"))
            disc.clicked.connect(lambda _c=False, k=key: self._slot_from_disc(k))
            lay.addWidget(hbox(disc))
        ui["file"] = label(os.path.basename(s["path"]) if s["path"] else "", "hint")
        lay.addWidget(ui["file"])
        if key in ("icon1", "snd0"):
            self._range_box(lay, key)
        if key == "snd0":
            from ps2rc.xmb import sound
            enc, _msg = sound.status()
            ui["loop"] = QCheckBox(self.t("snd0.loop"))
            ui["loop"].setChecked(bool(s.get("loop", True)))
            ui["loop"].toggled.connect(lambda v: self._slot_param("snd0", "loop", bool(v)))
            disc_btn = QPushButton(self.t("mus.from_disc"))
            disc_btn.setEnabled(self._disc_music_possible())
            disc_btn.clicked.connect(self._music_from_disc)
            lay.addWidget(hbox(disc_btn))
            listen = QPushButton(self.t("snd0.listen"))
            listen.clicked.connect(self._listen)
            stop = QPushButton(self.t("snd0.stop"))
            stop.clicked.connect(self._stop_sound)
            lay.addWidget(hbox(ui["loop"], listen, stop))
            ui["codec"] = QComboBox()
            for k, codec in sound.CHOICES:
                ui["codec"].addItem(self.t("snd0.q." + k, int(sound.max_seconds(MEDIA_BUDGET, codec) + 0.5)), codec)
            ui["codec"].setCurrentIndex(max(0, ui["codec"].findData(self._music_codec())))
            ui["codec"].setToolTip(self.t("snd0.q.tip"))
            ui["codec"].currentIndexChanged.connect(lambda _i: self._set_music_codec(ui["codec"].currentData()))
            lay.addWidget(hbox(QLabel(self.t("snd0.q")), ui["codec"]))
            ui["codec_note"] = label(self.t("snd0.q.tip"), "hint", wrap=True)
            ui["codec_note"].setVisible(self._music_codec() != "atrac3plus")
            lay.addWidget(ui["codec_note"])
            if not enc:
                ui["off"] = True
                lay.addWidget(label(self.t("snd0.off", "").rstrip(": "), "bad"))
                if sound.find_encoder() and sound.windows_media_missing():
                    lay.addWidget(label(self.t("snd0.offmf", sound.MEDIA_FEATURE_PACK), "hint", wrap=True))
                else:
                    lay.addWidget(label(self.t("snd0.offhint"), "hint", wrap=True))
        ui["status"] = label("", wrap=True)
        lay.addWidget(ui["status"])

    def _range_box(self, lay, key):
        s = self.project.slots[key]
        ui = self.slot_ui[key]
        from ps2rc.xmb import clip, sound
        dur = self.s.get("dur_" + key) or 10.0
        default_len = clip.DEFAULT_SECONDS if key == "icon1" else sound.DEFAULT_SECONDS
        max_len = clip.MAX_SECONDS if key == "icon1" else max(1.0, sound.max_seconds(MEDIA_BUDGET,
                                                                                     self._music_codec()))
        grid = QGridLayout()
        grid.setHorizontalSpacing(6)
        for row, (lbl, field, lo, hi, val) in enumerate((
                (self.t("range.start"), "start", 0.0, max(0.5, dur - 0.5), s.get("start") or 0.0),
                (self.t("range.len"), "seconds", 1.0, max_len, s.get("seconds") or default_len))):
            sl = QSlider(Qt.Horizontal)
            sl.setRange(int(lo * 10), int(hi * 10))
            sl.setValue(int(round(min(max(val, lo), hi) * 10)))
            sl.setMinimumWidth(200)
            v = QLabel("%.1f" % (sl.value() / 10.0))
            v.setMinimumWidth(36)
            sl.valueChanged.connect(lambda x, lab=v: (lab.setText("%.1f" % (x / 10.0)), self._budget_live()))
            sl.sliderReleased.connect(lambda k=key, f=field, w=sl: self._slot_param(k, f, round(w.value() / 10.0,
                                                                                                    1)))
            ui["len" if field == "seconds" else "start"] = sl
            grid.addWidget(QLabel(lbl), row, 0)
            grid.addWidget(sl, row, 1)
            grid.addWidget(v, row, 2)
        w = QWidget()
        w.setLayout(grid)
        lay.addWidget(w)

    def _slot_mode(self, key, m):
        if m == "file" and not self.project.slots[key]["path"]:
            self._slot_pick(key)
            return
        self.project.set_mode(key, m)
        self._look_changed()

    def _set_checked_mode(self, key, mode):
        if key in self.slot_ui:
            for b in self.slot_ui[key]["group"].buttons():
                b.setChecked(b.property("mode") == mode)

    def _slot_pick(self, key):
        types = {"icon0": PICTURE_TYPES, "pic0": PICTURE_TYPES, "pic1": PICTURE_TYPES, "icon1": CLIP_TYPES,
                 "snd0": MUSIC_TYPES}[key]
        lab = {"icon1": "pick.clips", "snd0": "pick.music"}.get(key, "pick.pictures")
        p, _f = QFileDialog.getOpenFileName(self, self.t("slot.browse"), self.s.get("dir_" + key) or "",
                                            "%s (%s);;%s (*.*)" % (self.t(lab), types, self.t("pick.all")))
        if not p:
            self._set_checked_mode(key, self.project.slots[key]["mode"])
            return
        self._set_file(key, os.path.normpath(p))

    def _set_file(self, key, p, box=None):
        self.s["dir_" + key] = os.path.dirname(p)
        old = self.project.slots[key]
        self.project.set_file(key, p, start=0.0, seconds=old.get("seconds"), loop=old.get("loop", True), box=box)
        if self.page == "customize" and key in self.slot_ui:
            self._set_checked_mode(key, "file")
            self.slot_ui[key]["file"].setText(os.path.basename(p))
        if key in ("icon1", "snd0"):
            self._probe_duration(key, p)
        self._look_changed()

    def _probe_duration(self, key, path):
        def work():
            try:
                from ps2rc.xmb import media
                dur = media.probe(path).get("seconds")
            except Exception:          # unknown length: keep the slider range
                dur = None
            if dur:
                self._ui(self._set_duration, key, dur)
        self._thread(work)

    def _set_duration(self, key, dur):
        self.s["dur_" + key] = dur
        ui = self.slot_ui.get(key) if self.page == "customize" else None
        if ui and "start" in ui:
            ui["start"].setRange(0, int(max(0.5, dur - 0.5) * 10))

    def _slot_param(self, key, field, value):
        self.project.slots[key][field] = value
        self._look_changed()

    def _slot_crop(self, key):
        s = self.project.slots[key]
        if s["mode"] != "file" or not s["path"]:
            self._slot_pick(key)
            return
        try:
            from ps2rc.xmb import picture, clip
            from ps2rc.pbp.spec import IMAGES
            if key == "icon1":
                if s["path"].lower().endswith(".pmf"):
                    return
                img = picture.open_picture(clip.still(s["path"], s.get("start") or 0.0))
                size = (144, 80)
            else:
                img = picture.open_picture(s["path"])
                size = (IMAGES[key].width, IMAGES[key].height)
        except Exception as e:         # unreadable file: say so on the slot
            self._slot_error(key, str(e))
            return
        d = CropDialog(self, qimage_from_pil(img), size, s.get("box"), self.lang, title=self.t("slot.crop"))
        if d.exec() == QDialog.Accepted and d.result_box:
            s["box"] = d.result_box
            self._look_changed()

    def _apply_music_choice(self):
        """the player's music quality (remembered on its own: projects saved before the choice existed carry the old
        ATRAC3plus default)"""
        from ps2rc.xmb import sound
        codec = self.s.get("snd0_codec")
        self.project.slots["snd0"]["codec"] = codec if codec in sound.CODECS else sound.DEFAULT_CODEC

    def _music_codec(self):
        from ps2rc.xmb import sound
        c = self.project.slots["snd0"].get("codec")
        return c if c in sound.CODECS else sound.DEFAULT_CODEC

    def _set_music_codec(self, codec):
        from ps2rc.xmb import sound
        self.s["snd0_codec"] = codec
        self.project.slots["snd0"]["codec"] = codec
        ui = self.slot_ui.get("snd0") or {}
        if "len" in ui:
            longest = max(1.0, sound.max_seconds(MEDIA_BUDGET, codec))
            ui["len"].setRange(10, int(longest * 10))
            s = self.project.slots["snd0"]
            if s.get("seconds") and s["seconds"] > longest:
                s["seconds"] = int(longest * 10) / 10.0           # down to the slider's 0.1 s steps, never above
                ui["len"].setValue(int(s["seconds"] * 10))
        if "codec_note" in ui:
            ui["codec_note"].setVisible(codec != "atrac3plus")
        self._look_changed()

    def _reset_look(self):
        self.project = Project(self.project.title)
        self._apply_music_choice()
        self._save()
        self.show_page("customize")

    def _look_changed(self):
        self._save()
        self._schedule_assets()

    # pictures from the player's own disc ------------------------------------------------------------------------
    def _pack_dir(self):
        r = self.ident or {}
        return os.path.join(backend.repo(), "games", r["pack"]) if r.get("pack") else None

    def _disc_art_possible(self):
        from ps2rc.xmb import disc_art
        try:
            return bool(self._pack_dir() and self.s.get("source") and disc_art.find_provider(self._pack_dir()))
        except Exception:              # a broken provider: the buttons stay off
            return False

    def _art_worker(self, on_item, on_done, cancel=lambda: False):
        """read the disc's pictures in a worker: on_item(art) for each (window thread), on_done(arts, err)"""
        src, pack = self.s.get("source"), self._pack_dir()

        def work():
            from ps2rc.xmb import disc_art
            try:
                arts = disc_art.extract(src, local_dir("disc_art"), pack,
                                        on_item=lambda a: self._ui(on_item, a), cancel=cancel)
                err = None
            except Exception as e:     # shown, not fatal
                arts, err = [], str(e)
            self._ui(on_done, arts, err)
        self._thread(work)

    def _slot_from_disc(self, key):
        """the gallery opens at once; pictures appear while the disc is read (cached: the 2nd time at once)"""
        d = GalleryDialog(self, self.arts or [], self.lang, busy=self.arts is None)
        if self.arts is None:
            def done(arts, err):
                if not err:
                    self.arts = arts
                try:
                    d.finish_reading(err or None)
                except RuntimeError:
                    pass
            self._art_worker(lambda a: d.add_art(a) if not d.cancelled else None, done, lambda: d.cancelled)
        if d.exec() == QDialog.Accepted and d.chosen is not None:
            self._set_file(key, d.chosen.path)
            if key != "icon1":
                self._slot_crop(key)

    def _use_game_art(self):
        def use(arts):
            from ps2rc.xmb import disc_art
            for slot, (art, box) in disc_art.suggest(arts, self._pack_dir()).items():
                self._set_file(slot, art.path, box=box)
            self.show_page("customize")
        if self.arts is not None:
            use(self.arts)
            return
        self.art_btn.setEnabled(False)
        self.art_busy.set(self.t("gal.busy"))
        self.art_busy.show()
        count = [0]

        def item(_a):
            count[0] += 1
            if self.page == "customize":
                self.art_busy.set(self.t("gal.reading_n", count[0]))

        def done(arts, err):
            if self.page == "customize":
                self.art_busy.hide()
                self.art_btn.setEnabled(True)
            if err:
                QMessageBox.warning(self, self.t("app"), err)
                return
            self.arts = arts
            if not arts:
                QMessageBox.information(self, self.t("app"), self.t("gal.none"))
                return
            use(arts)
        self._art_worker(item, done)

    # music from the player's own disc -----------------------------------------------------------------------
    def _disc_music_possible(self):
        from ps2rc.xmb import disc_music
        try:
            return bool(self._pack_dir() and self.s.get("source") and disc_music.available(self._pack_dir()))
        except Exception:              # a broken provider: the button stays off
            return False

    def _music_from_disc(self):
        """the list opens at once (busy row, then the count); Listen / Use read the chosen part in a worker"""
        from ps2rc.xmb import disc_music, sound
        src, pack = self.s.get("source"), self._pack_dir()
        d = MusicDialog(self, self.lang)

        def listed(tracks, err):
            try:
                if err:
                    d.busy.finish(err)
                else:
                    self.music_tracks = tracks
                    d.set_tracks(tracks)
            except RuntimeError:
                pass
        if getattr(self, "music_tracks", None) is not None:
            d.set_tracks(self.music_tracks)
        else:
            def work():
                try:
                    tr = disc_music.list_music(src, local_dir("disc_art"), pack,
                                               progress=lambda a, b: self._ui(d.progress, a, b))
                    self._ui(listed, tr, None)
                except Exception as e:     # shown in the dialog
                    self._ui(listed, [], str(e))
            self._thread(work)

        def listen():
            t = d.current()
            if t is None:
                return
            d.listen_busy.start()
            start = d.start.value()

            def work():
                try:
                    path = disc_music.extract(src, pack, t, start, 15.0, local_dir("listen_disc.wav"))
                    self._ui(self._play_wav, path)
                except Exception as e:     # shown in the dialog
                    self._ui(d.busy.finish, str(e))
                self._ui(d.listen_busy.stop)
            self._thread(work)
        d.listen.clicked.connect(listen)
        d.stop.clicked.connect(self._stop_sound)
        ok = d.exec() == QDialog.Accepted and d.chosen is not None
        self._stop_sound()
        if not ok:
            return
        t, start = d.chosen, d.start.value()
        out = local_dir("disc_music", "%s_%d.wav" % ("".join(c if c.isalnum() else "_" for c in t.name), int(start)))
        longest = 2 * sound.max_seconds(MEDIA_BUDGET, self._music_codec())   # the Music tab cuts it to what fits
        self.slot_ui["snd0"]["status"].setText(self.t("mus.taking"))
        set_kind(self.slot_ui["snd0"]["status"], "hint")

        def take():
            try:
                disc_music.extract(src, pack, t, start, longest, out)
                self._ui(self._set_file, "snd0", out)
            except Exception as e:     # shown on the slot
                self._ui(self._slot_error, "snd0", str(e))
        self._thread(take)

    # assets are made in a worker (cached by content), then the preview and the budget follow -------------------
    def _schedule_assets(self):
        self._asset_timer.start(250)
        if self.page == "customize":
            for k, ui in self.slot_ui.items():
                if self.project.slots[k]["mode"] != "none":
                    ui["status"].setText(self.t("slot.busy"))
                    set_kind(ui["status"], "hint")

    def _start_assets(self):
        if self._asset_busy:
            self._asset_dirty = True
            return
        self._asset_busy = True
        proj = Project.from_dict(json.loads(json.dumps(self.project.to_dict())))

        def work():
            built = proj.build(local_dir("xmb"))
            frames = None
            s = proj.slots["icon1"]
            try:
                from ps2rc.xmb import clip
                from ps2rc.pbp import placeholder
                if s["mode"] == "default":
                    frames = placeholder.icon1_frames(proj.default_text, seconds=min(s["seconds"] or 3.0, 4.0))
                elif s["mode"] == "file" and s["path"] and not s["path"].lower().endswith(".pmf"):
                    frames = clip.frames(s["path"], s["start"], s["seconds"] or clip.DEFAULT_SECONDS, s["box"])
            except Exception as e:     # preview frames are a nicety: the slot status shows real errors
                built.errors.setdefault("icon1", str(e))
            self._ui(self._assets_done, built, frames)
        self._thread(work)

    def _assets_done(self, built, frames):
        self._asset_busy = False
        self.built, self.icon1_frames = built, frames
        if self._asset_dirty:
            self._asset_dirty = False
            self._start_assets()
        if self.page == "customize":
            self._refresh_look()

    def _slot_note(self, key):
        """what was done to the player's file, in a few words (cropped / trimmed / re-encoded)"""
        b = self.built
        slot = {"icon0": "ICON0", "icon1": "ICON1", "pic0": "PIC0", "pic1": "PIC1", "snd0": "SND0"}[key]
        notes = [n for n in (b.notes if b else []) if n.upper().startswith(slot) and "cached" not in n]
        if not notes:
            return ""
        note = notes[-1].split(":", 1)[-1].strip()
        path = self.project.slots[key].get("path") or ""
        return note.replace(path, os.path.basename(path)) if path else note

    def _refresh_look(self):
        b = self.built
        for k, ui in self.slot_ui.items():
            if b is None:
                continue
            if k in b.errors and k == "snd0" and "atracdenc" in b.errors[k] and "off" in ui:
                ui["status"].setText("")                    # already said by the "Music is OFF" lines
            elif k in b.errors:
                ui["status"].setText(b.errors[k])
                set_kind(ui["status"], "bad")
            elif k in b.sizes:
                note = self._slot_note(k)
                ui["status"].setText(self.t("slot.ok", human_bytes(b.sizes[k])) + ("  - " + note if note else ""))
                set_kind(ui["status"], "good")
            else:
                ui["status"].setText("")
        self._budget_live()
        if self.preview is None:
            return
        title, frames = self._shown_title(), self.icon1_frames
        token = object()
        self._mock_token = token

        def work():
            try:
                from ps2rc.xmb.preview import Mock

                def rd(k):
                    p = b.files.get(k) if b else None
                    return open(p, "rb").read() if p else None
                mock = Mock(pic1=rd("pic1"), pic0=rd("pic0"), icon0=rd("icon0"), icon1_frames=frames, title=title,
                            clock=time.strftime("%H:%M"))
                mock.compose(0.0)                              # warm up (fonts) here, not in the window's thread
                self._ui(self._mock_ready, token, mock, "")
            except ImportError:
                self._ui(self._mock_ready, token, None, "")
            except Exception as e:     # a broken file must not stop the window
                self._ui(self._mock_ready, token, None, str(e))
        self._thread(work)

    def _mock_ready(self, token, mock, err):
        if token is not getattr(self, "_mock_token", None) or self.preview is None:
            return
        if err:
            self.preview.note = err
        self.preview.set_mock(mock)

    def _budget_live(self):
        if self.page != "customize":
            return
        from ps2rc.xmb import sound
        b = self.built
        icon1 = (b.sizes.get("icon1") if b else 0) or 0
        snd0 = (b.sizes.get("snd0") if b else 0) or 0
        s = self.project.slots["snd0"]
        ui = self.slot_ui.get("snd0", {})
        if s["mode"] == "file" and s["path"] and not s["path"].lower().endswith(".at3") and "len" in ui \
                and sound.find_encoder():
            secs = ui["len"].value() / 10.0
            if not snd0 or abs(secs - (s.get("seconds") or sound.DEFAULT_SECONDS)) > 0.05:
                snd0 = sound.size_for(secs, self._music_codec())    # live while the slider moves
        used = icon1 + snd0
        self.budget_bar.setValue(min(used, MEDIA_BUDGET))
        text = self.t("budget", human_bytes(icon1), human_bytes(snd0), human_bytes(used))
        kind = None
        if used > MEDIA_BUDGET:
            text += "  " + self.t("budget.over")
            kind = "bad"
        self.budget_label.setText(text)
        set_kind(self.budget_label, kind)

    # music
    def _listen(self):
        if os.name != "nt":
            return
        s = self.project.slots["snd0"]
        b = self.built

        def work():
            try:
                from ps2rc.xmb import sound, media
                pcm = None
                if b and b.files.get("snd0") and media.find_ffmpeg():
                    pcm = sound.decode(open(b.files["snd0"], "rb").read())     # what the PSP will play
                elif s["mode"] == "file" and s["path"] and not s["path"].lower().endswith(".at3"):
                    pcm = sound.shape(sound.read_pcm(s["path"], s["start"], s["seconds"] or sound.DEFAULT_SECONDS),
                                      loop=s.get("loop", True))
                if not pcm:
                    return
                path = local_dir("listen.wav")
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(sound.wav_bytes(pcm))
                self._ui(self._play_wav, path)
            except Exception as e:     # shown on the slot
                self._ui(self._slot_error, "snd0", str(e))
        self._thread(work)

    def _play_wav(self, path):
        try:
            import winsound
            winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_LOOP)
        except (ImportError, RuntimeError):
            pass

    def _stop_sound(self):
        try:
            import winsound
            winsound.PlaySound(None, 0)
        except (ImportError, RuntimeError):
            pass

    def _preview_sound(self, on):
        if on:
            QTimer.singleShot(800, self._listen)
        else:
            self._stop_sound()

    def _slot_error(self, key, text):
        ui = self.slot_ui.get(key)
        if ui and self.page == "customize":
            ui["status"].setText(text)
            set_kind(ui["status"], "bad")

    # --- settings ---------------------------------------------------------------------------------------------
    def _load_template(self):
        r = self.ident or {}
        self.template_items, self.template_text = None, None
        if not r.get("pack"):
            return
        en = backend.settings_template(r["pack"], "en")
        if not en:
            return
        items = cfgform.parse(en)
        if self.lang != "en":
            loc = backend.settings_template(r["pack"], self.lang)
            if loc and loc != en:
                cfgform.merge_descriptions(items, cfgform.parse(loc))
        self.template_items, self.template_text = items, en

    def _page_settings(self):
        top = QHBoxLayout()
        top.addWidget(label(self.t("set.h"), "h1"))
        top.addStretch(1)
        self.body_lay.addLayout(top)
        self._nav("customize", "controls")
        r = self.ident or {}
        if not r.get("ok"):
            self.add(label(self.t("set.nodisc"), wrap=True))
            self.body_lay.addStretch(1)
            return
        if not self.template_items:
            self.add(label(self.t("set.none"), wrap=True))
            self.body_lay.addStretch(1)
            return
        rb = QPushButton(self.t("set.restore"))
        rb.clicked.connect(self._restore_settings)
        top.addWidget(rb)
        self.add(label(self.t("set.sub"), "hint", wrap=True))
        self.set_count = self.add(label(""))
        area = QScrollArea()
        area.setWidgetResizable(True)
        inner = QWidget()
        g = QGridLayout(inner)
        g.setColumnStretch(2, 1)
        g.setHorizontalSpacing(10)
        g.setVerticalSpacing(4)
        self.set_widgets = {}
        row, group, ctl_done = 0, None, False
        for it in self.template_items:
            if isinstance(it, cfgform.Group):
                group = (getattr(it, "title_local", None) or it.title).capitalize()
                continue
            if group:                                      # a group's title when its first row comes
                g.addWidget(label(group, "h2"), row, 0, 1, 3)
                row, group = row + 1, None
            if controls_model.is_control_key(it.key):      # these are on the controls page
                if not ctl_done:
                    row, ctl_done = self._controls_summary_row(g, row), True
                continue
            row = self._setting_row(g, row, it)
        g.setRowStretch(row, 1)
        area.setWidget(inner)
        self.add(area, 1)
        self._count_changed()

    def _setting_row(self, g, row, it):
        cur = self.values.get(it.key, it.value if not it.commented else "")
        name = label(self._setting_label(it), wrap=True)
        name.setMinimumWidth(150)
        name.setMaximumWidth(170)
        g.addWidget(name, row, 0, Qt.AlignTop)
        ctl = QWidget()
        cl = QHBoxLayout(ctl)
        cl.setContentsMargins(0, 0, 0, 0)
        mark = label("", "warn")
        if it.kind == "bool":
            cb = QCheckBox()
            cb.setChecked(str(cur) == "1")
            cb.toggled.connect(lambda v, i=it: self._set_value(i, "1" if v else "0"))
            cl.addWidget(cb)
        elif it.kind == "range":
            lo, hi = it.range
            try:
                iv = int(float(cur))
            except ValueError:
                iv = lo
            sl = QSlider(Qt.Horizontal)
            sl.setRange(lo, hi)
            sl.setValue(min(max(iv, lo), hi))
            sl.setFixedWidth(170)
            v = QLabel("%d%s" % (sl.value(), it.unit))
            v.setMinimumWidth(48)
            sl.valueChanged.connect(lambda x, lab=v, i=it: lab.setText("%d%s" % (x, i.unit)))
            sl.sliderReleased.connect(lambda i=it, w=sl: self._set_value(i, str(w.value())))
            cl.addWidget(sl)
            cl.addWidget(v)
        elif it.kind == "choice":
            combo = QComboBox()
            opts = ([""] if it.commented else []) + list(it.options)
            for o in opts:
                combo.addItem(self.t("set.notset") if o == "" else o, o)
            combo.setCurrentIndex(opts.index(str(cur)) if str(cur) in opts else 0)
            combo.activated.connect(lambda _i, i=it, c=combo: self._set_value(i, c.currentData()))
            cl.addWidget(combo)
        else:
            e = QLineEdit(str(cur))
            e.setFixedWidth(120)
            e.editingFinished.connect(lambda i=it, w=e: self._set_value(i, w.text()))
            cl.addWidget(e)
        cl.addWidget(mark)
        cl.addStretch(1)
        g.addWidget(ctl, row, 1, Qt.AlignTop)
        desc = self._describe(it)
        ours = it.default if it.default is not None else self.t("set.notset")
        has_ours = any(re.search(r"(?<!\w)%s\s*:" % re.escape(w), desc, re.I) for w in self._words("_ours_words"))
        txt = desc if has_ours else "%s  (%s)" % (desc, self.t("set.ours", ours))
        g.addWidget(label(txt, "hint", wrap=True), row, 2, Qt.AlignTop)
        self.set_widgets[it.key] = mark
        self._mark(it)
        return row + 1

    def _words(self, key):
        """marker words of the template's language (gui/lang/<code>.json), English ones always"""
        base = {"_ours_words": ["ours"], "_risk_words": ["RISK"]}[key]
        return base + list((i18n.TEXTS.get(self.lang) or {}).get(key, []))

    def _describe(self, it, risk=False):
        """the setting's description in the window's language: without 'key - ', split at the risk sentence"""
        text = " ".join(getattr(it, "comment_local", None) or it.comment)
        cut = [text.find(w) for w in self._words("_risk_words") if w in text]
        if cut:
            text = text[min(cut):] if risk else text[:min(cut)]
        elif risk:
            return it.risk
        text = re.sub(r"^%s\s*[-:]\s*" % re.escape(it.key), "", text).strip()
        return text[:1].upper() + text[1:]

    def _setting_label(self, it):
        loc = getattr(it, "comment_local", None)
        if self.lang != "en" and loc:
            first = re.sub(r"^%s\s*[-:]\s*" % re.escape(it.key), "", " ".join(loc))
            first = re.split(r"[,.:;(]", first)[0].strip()
            if len(first) > 32:                              # long clause: whole words up to 32 characters
                cut = first[:33].rsplit(" ", 1)[0]
                first = cut + "..." if len(cut) > 8 else ""
            if len(first) > 2:
                return first[:1].upper() + first[1:]
        return it.label

    def ask_risk(self, it):
        """True = the player confirms a risky change (a separate method so tests can answer it)"""
        return QMessageBox.question(self, self.t("set.risk.h"), self.t("set.risk", self._describe(it, risk=True))) \
            == QMessageBox.Yes

    def _set_value(self, it, v):
        v = it.normalize(v) if v not in ("", None) else ""
        old = self.values.get(it.key, it.default)
        if it.risk and v not in ("", it.default, old) and not self.ask_risk(it):
            self.show_page("settings")                     # back to the stored value
            return
        if v == "" or v == it.default:
            self.values.pop(it.key, None)
        else:
            self.values[it.key] = v
        self._mark(it)
        self._count_changed()
        self._save()

    def _mark(self, it):
        w = getattr(self, "set_widgets", {}).get(it.key)
        if w is not None and self.page == "settings":
            w.setText(self.t("set.changed") if it.key in self.values else "")

    def _count_changed(self):
        if self.page == "settings" and hasattr(self, "set_count"):
            n = len(self.overrides())
            self.set_count.setText(self.t("set.count", n) if n else "")

    def _restore_settings(self):
        self.values = {}
        self._save()
        self.show_page("settings")

    def overrides(self):
        if not self.template_items:
            return {}
        ov = cfgform.overrides(self.template_items, self.values)
        keys = {s.key for s in cfgform.settings(self.template_items)}
        ov.update({k: v for k, v in self.values.items()
                   if controls_model.is_control_key(k) and k not in keys and v not in ("", None)})
        return ov

    # --- controls ----------------------------------------------------------------------------------------------
    def _ctl_items(self):
        """the template whose words describe the PS2 buttons: the folder's own ps2rc.cfg on the update path"""
        return self.ctl_folder_items if self.page == "folder_controls" else self.template_items

    def _has_controls(self):
        """the game's template has the control mapping keys"""
        items = self._ctl_items()
        return bool(items) and any(getattr(s, "key", "") == "map_l2" for s in items)

    def _ctl_src(self):
        """the dict the controls page edits"""
        return self.ctl_folder_vals if self.page == "folder_controls" else self.values

    def _ctl_layout(self):
        return controls_model.Layout({k: v for k, v in self._ctl_src().items() if controls_model.is_control_key(k)})

    def _ctl_commit(self):
        """store the edited controls: the window's settings, or the folder's ps2rc.cfg (written at once)"""
        if self.page != "folder_controls":
            self._save()
            return ""
        return self._folder_cfg_write()

    def _controls_summary_row(self, g, row):
        lay = self._ctl_layout()
        n = len(lay.to_values()) - (lay.preset != "default")
        txt = self.t("ctl.preset." + lay.preset) + ("" if not n else "  (%s)" % self.t("ctl.changes", n))
        g.addWidget(label(self.t("ctl.h"), wrap=True), row, 0, Qt.AlignTop)
        b = QPushButton(self.t("ctl.change"))
        b.clicked.connect(lambda: self.show_page("controls"))
        g.addWidget(hbox(b), row, 1, Qt.AlignTop)
        g.addWidget(label(txt, "hint", wrap=True), row, 2, Qt.AlignTop)
        return row + 1

    def _ctl_desc(self, inp):
        """the game's words for a PS2 input: the parenthesis of its map_ line in the template (window language)"""
        for s in cfgform.settings(self._ctl_items() or []):
            if s.key == "map_" + inp:
                text = " ".join(getattr(s, "comment_local", None) or s.comment)
                m = re.search(r"\(([^()]*)\)", text)
                return m.group(1).strip() if m else ""
        return ""

    def _ctl_func_text(self, inp):
        d = self._ctl_desc(inp)
        return "%s - %s" % (self.t("ps2." + inp), d) if d else self.t("ps2." + inp)

    def _ctl_apply(self, lay, moved=None, msg=""):
        src = self._ctl_src()
        for k in [k for k in src if controls_model.is_control_key(k)]:
            del src[k]
        src.update(lay.to_values())
        if moved:
            msg = self.t("ctl.moved", ", ".join(self.t("ps2." + m) for m in dict.fromkeys(moved)))
        err = self._ctl_commit()
        self._ctl_msg = err or msg
        tabs = getattr(self, "ctl_tabs", None)
        if tabs is not None:
            self._ctl_tab_i = tabs.currentIndex()
            for L, area in getattr(self, "ctl_areas", {}).items():
                self._ctl_scroll[L] = area.verticalScrollBar().value()
        self.show_page(self.page if self.page == "folder_controls" else "controls")

    def _ctl_edit(self, fn, *a):
        lay = self._ctl_layout()
        try:
            moved = getattr(lay, fn)(*a)
        except ValueError as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("ctl.bad", str(e)))
            return
        self._ctl_apply(lay, moved if isinstance(moved, list) else None)

    def _ctl_preset(self, preset):
        lay = self._ctl_layout()
        lay.set_preset(preset)
        self._ctl_apply(lay)

    def _ctl_restore(self):
        lay = self._ctl_layout()
        lay.restore()
        self._ctl_apply(lay)

    def _ctl_func_box(self, current, nothing=True):
        cb = QComboBox()
        if nothing:
            cb.addItem(self.t("ctl.nothing"), "")
        for inp in controls_model.BUTTON_INPUTS:
            cb.addItem(self._ctl_func_text(inp), inp)
            cb.setItemData(cb.count() - 1, self._ctl_func_text(inp), Qt.ToolTipRole)
        i = cb.findData(current or "")
        cb.setCurrentIndex(max(0, i))
        cb.setMinimumContentsLength(16)
        cb.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        return cb

    def _ctl_press_box(self, flag):
        cb = QComboBox()
        for f in controls_model.FLAGS:
            cb.addItem(self.t("ctl.press." + (f or "plain")), f)
        cb.setCurrentIndex(max(0, cb.findData(flag or "")))
        cb.setMinimumContentsLength(9)
        cb.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        cb.setToolTip(self.t("ctl.combohint"))
        return cb

    def _page_controls(self):
        self.add(label(self.t("ctl.h"), "h1"))
        if self.page == "folder_controls":
            self._nav("update")
            self.add(label(self.t("ctl.folder", os.path.join(self.ctl_folder or "", self.ctl_folder_cfg or "")),
                           "hint", wrap=True))
        else:
            self._nav("settings", "convert")
        if not self._has_controls():
            self.add(label(self.t("ctl.none"), wrap=True))
            self.body_lay.addStretch(1)
            return
        lay = self._ctl_layout()
        # row 1: the scheme and Restore defaults, left-aligned so they never fall off the right edge
        sc = QComboBox()
        for p in controls_model.PRESETS:
            sc.addItem(self.t("ctl.preset." + p), p)
            sc.setItemData(sc.count() - 1, self.t("ctl.preset.tip." + p), Qt.ToolTipRole)
        sc.setCurrentIndex(controls_model.PRESETS.index(lay.preset))
        sc.setSizeAdjustPolicy(QComboBox.AdjustToContents)
        sc.activated.connect(lambda _i: self._ctl_preset(sc.currentData()))
        rb = QPushButton(self.t("ctl.restore"))
        rb.setToolTip(self.t("ctl.restore.tip"))
        rb.clicked.connect(self._ctl_restore)
        self.add(hbox(label(self.t("ctl.scheme"), "h2"), sc, rb))
        self.ctl_scheme, self.ctl_restore_btn = sc, rb
        # row 2: my profiles
        names = self._ctl_store().names()
        pc = QComboBox()
        pc.setMinimumContentsLength(16)
        pc.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        for n in names:
            pc.addItem(n, n)
        if not names:
            pc.addItem(self.t("prof.none"), "")
        cur = self.s.get("ctl_profile")
        if cur in names:
            pc.setCurrentIndex(names.index(cur))
        self.ctl_prof = pc
        btns = []
        for key, fn, need in (("prof.load", self._prof_load, True), ("prof.save", self._prof_save, False),
                              ("prof.rename", self._prof_rename, True), ("prof.delete", self._prof_delete, True),
                              ("prof.export", self._prof_export, False), ("prof.import", self._prof_import, False)):
            b = QPushButton(self.t(key))
            b.setToolTip(self.t(key + ".tip"))
            b.clicked.connect(lambda _c=False, f=fn: f())
            b.setEnabled(bool(names) or not need)
            btns.append(b)
        self.ctl_prof_btns = dict(zip(("load", "save", "rename", "delete", "export", "import"), btns))
        self.add(hbox(QLabel(self.t("prof.h")), pc, *btns))
        self.add(label(self.t("ctl.sub"), "hint", wrap=True))
        pic = ConsolePicture()
        pic.setFixedHeight(140)
        pic.picked.connect(self._ctl_pick)
        self.ctl_pic = self.add(pic)
        tabs = QTabWidget()
        self.ctl_tabs, self.ctl_areas, self.ctl_boxes = tabs, {}, {0: {}, 1: {}}
        for L in (0, 1):
            tabs.addTab(self._ctl_tab(lay, L), self.t("ctl.foot" if L == 0 else "ctl.car"))
        tabs.setCurrentIndex(self._ctl_tab_i)
        tabs.currentChanged.connect(lambda i: setattr(self, "_ctl_tab_i", i))
        self.add(tabs, 1)
        st = QCheckBox(self.t("ctl.seltap"))
        st.setChecked(lay.sel_tap)
        st.toggled.connect(lambda on: self._ctl_edit("set_select_tap", on))
        self.ctl_seltap = self.add(st)
        ci = QComboBox()
        for v in controls_model.CAM_INV:
            ci.addItem(self.t("ctl.caminv." + v), v)
        ci.setCurrentIndex(controls_model.CAM_INV.index(lay.cam_inv))
        ci.setToolTip(self.t("ctl.caminv.tip"))
        ci.currentIndexChanged.connect(lambda i, c=ci: self._ctl_edit("set_camera_invert", c.itemData(i)))
        self.ctl_caminv = ci
        self.add(hbox(QLabel(self.t("ctl.caminv")), ci))
        self.ctl_status = self.add(label(self._ctl_msg, "warn", wrap=True))
        self._ctl_msg = ""
        for L, v in self._ctl_scroll.items():
            area = self.ctl_areas.get(L)
            if area is not None:
                QTimer.singleShot(0, lambda a=area, x=v: a.verticalScrollBar().setValue(x))

    # --- the controls of a converted folder (check / update path) -----------------------------------------------
    def _folder_cfg_path(self, folder):
        for n in ("ps2rc.cfg",) + _legacy_cfg_names():      # the player's settings file (older folders: the pack's
                                                            # [runtime] cfg_legacy name)
            p = os.path.join(folder or "", n)
            if os.path.isfile(p):
                return p
        return None

    def _open_folder_controls(self, folder):
        p = self._folder_cfg_path(folder)
        if not p:
            return
        with open(p, encoding="utf-8-sig") as f:
            text = f.read()
        self.ctl_folder, self.ctl_folder_cfg = folder, os.path.basename(p)
        self.ctl_folder_items = cfgform.parse(text)
        vals = {}
        for it in cfgform.settings(self.ctl_folder_items):
            if controls_model.is_control_key(it.key) and not it.commented:
                vals[it.key] = it.value
        for ln in text.replace("\r\n", "\n").split("\n"):          # keys the template has no line for (car_*)
            m = cfgform.SETTING.match(ln.strip())
            if m and controls_model.is_control_key(m.group(1).lower()):
                vals[m.group(1).lower()] = m.group(2)
        self.ctl_folder_vals = controls_model.Layout(vals).to_values()
        self.show_page("folder_controls")

    def _folder_cfg_write(self):
        """the folder's ps2rc.cfg with the edited controls: changed keys on their own lines, keys back at the built-in
        value commented out, other settings untouched; written atomically; '' or the error text"""
        p = os.path.join(self.ctl_folder, self.ctl_folder_cfg)
        try:
            with open(p, encoding="utf-8-sig") as f:
                text = f.read()
            vals = dict(self.ctl_folder_vals)
            lines = text.replace("\r\n", "\n").split("\n")
            for i, ln in enumerate(lines):
                m = cfgform.SETTING.match(ln.strip())
                if m and controls_model.is_control_key(m.group(1).lower()) and m.group(1).lower() not in vals:
                    lines[i] = "#" + ln.strip()
            out = cfgform.apply_to_text("\n".join(lines), vals).rstrip("\n") + "\n"
            tmp = p + ".tmp"
            with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(out)
            os.replace(tmp, p)
        except OSError as e:
            return self.t("prof.bad", str(e))
        return ""

    def _page_folder_controls(self):
        self._page_controls()

    # --- control profiles (named, shareable as ps2rc.cfg text) -------------------------------------------------
    def _ctl_store(self):
        """profiles live next to the window's own settings file (per user), never in a game folder"""
        return controls_profiles.Store(os.path.join(os.path.dirname(self.settings_path), "controls_profiles"))

    def _ctl_values(self):
        return {k: v for k, v in self._ctl_src().items() if controls_model.is_control_key(k)}

    def _prof_selected(self):
        pc = getattr(self, "ctl_prof", None)
        return pc.currentData() if pc is not None else None

    def _prof_apply(self, vals, msg):
        self._ctl_apply(controls_model.Layout(vals), msg=msg)

    def ask_text(self, title, text, value=""):
        """a line of text from the player, None = cancelled (a method so tests can answer it)"""
        from PySide6.QtWidgets import QInputDialog
        v, ok = QInputDialog.getText(self, title, text, QLineEdit.Normal, value)
        return v if ok else None

    def ask_yes(self, title, text):
        return QMessageBox.question(self, title, text) == QMessageBox.Yes

    def ask_file(self, save, title, name=""):
        """a profile file path, None = cancelled"""
        flt = self.t("prof.files") + " (*.cfg *.txt);;*.*"
        start = os.path.join(self.s.get("prof_dir") or os.path.expanduser("~"), name)
        p = (QFileDialog.getSaveFileName if save else QFileDialog.getOpenFileName)(self, title, start, flt)[0]
        if p:
            self.s["prof_dir"] = os.path.dirname(p)
        return p or None

    def _prof_save(self):
        name = self.ask_text(self.t("prof.save"), self.t("prof.ask.name"), self._prof_selected() or "")
        if name is None:
            return
        st = self._ctl_store()
        try:
            name = controls_profiles.clean_name(name)
            if st.exists(name) and not self.ask_yes(self.t("prof.save"), self.t("prof.ask.replace", name)):
                return
            name = st.save(name, self._ctl_values())
        except (controls_profiles.ProfileError, OSError) as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.bad", str(e)))
            return
        self.s["ctl_profile"] = name
        self._ctl_apply(self._ctl_layout(), msg=self.t("prof.saved", name))

    def _prof_load(self):
        name = self._prof_selected()
        if not name:
            return
        try:
            vals = self._ctl_store().load(name)
        except (controls_profiles.ProfileError, OSError) as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.bad", str(e)))
            return
        self.s["ctl_profile"] = name
        self._prof_apply(vals, self.t("prof.loaded", name))

    def _prof_rename(self):
        old = self._prof_selected()
        if not old:
            return
        new = self.ask_text(self.t("prof.rename"), self.t("prof.ask.name"), old)
        if not new or new.strip() == old:
            return
        try:
            new = self._ctl_store().rename(old, new)
        except (controls_profiles.ProfileError, OSError) as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.bad", str(e)))
            return
        self.s["ctl_profile"] = new
        self._ctl_apply(self._ctl_layout(), msg=self.t("prof.renamed", old, new))

    def _prof_delete(self):
        name = self._prof_selected()
        if not name or not self.ask_yes(self.t("prof.delete"), self.t("prof.ask.delete", name)):
            return
        try:
            self._ctl_store().delete(name)
        except OSError as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.bad", str(e)))
            return
        self.s.pop("ctl_profile", None)
        self._ctl_apply(self._ctl_layout(), msg=self.t("prof.deleted", name))

    def _prof_export(self):
        name = self._prof_selected() or self.t("prof.default_name")
        path = self.ask_file(True, self.t("prof.export"), name + controls_profiles.EXT)
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(controls_profiles.to_text(name, self._ctl_values()))
        except (OSError, controls_profiles.ProfileError) as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.bad", str(e)))
            return
        self._ctl_apply(self._ctl_layout(), msg=self.t("prof.exported", path))

    def _prof_import(self):
        path = self.ask_file(False, self.t("prof.import"))
        if not path:
            return
        try:
            with open(path, encoding="utf-8-sig") as f:
                text = f.read(256 * 1024)
            name, vals = controls_profiles.parse_text(text, os.path.splitext(os.path.basename(path))[0])
            st = self._ctl_store()
            name = st.save(st.free_name(name), vals)
        except (OSError, UnicodeDecodeError, controls_profiles.ProfileError) as e:
            self._ctl_apply(self._ctl_layout(), msg=self.t("prof.badfile", str(e)))   # nothing applied
            return
        self.s["ctl_profile"] = name
        self._prof_apply(vals, self.t("prof.imported", name))

    def _ctl_pick(self, name):
        """a click on the picture: its row's box gets the focus and the button lights up"""
        L = self.ctl_tabs.currentIndex()
        box = self.ctl_boxes[L].get(name)
        self.ctl_pic.set_marked({name})
        if box is not None:
            self.ctl_areas[L].ensureWidgetVisible(box)
            box.setFocus()

    def _ctl_tab(self, lay, L):
        area = QScrollArea()
        area.setWidgetResizable(True)
        inner = QWidget()
        v = QVBoxLayout(inner)
        v.setSpacing(4)
        ratio = self.devicePixelRatioF() or 1.0
        if L == 1:
            v.addWidget(label(self.t("ctl.carnote") if lay.has_car else self.t("ctl.carsame"), "hint", wrap=True))
            cp = QPushButton(self.t("ctl.copy"))
            cp.clicked.connect(lambda: self._ctl_edit("copy_foot_to_vehicle"))
            cp.setEnabled(lay.has_car)
            v.addWidget(hbox(cp))
        v.addWidget(label(self.t("ctl.buttons"), "h2"))
        g = QGridLayout()
        g.setHorizontalSpacing(8)
        g.setVerticalSpacing(3)
        rows = lay.button_rows(L)
        for n, b in enumerate(controls_model.PSP_BUTTONS):
            r, col = n % 6, (n // 6) * 5
            ic = QLabel()
            ic.setPixmap(glyph_pixmap(b, 18, ratio))
            g.addWidget(ic, r, col)
            g.addWidget(QLabel(self.t("psp." + b)), r, col + 1)
            cur = rows[b]
            fb = self._ctl_func_box(cur[0] if cur else "")
            fb.activated.connect(lambda _i, b=b, fb=fb, L=L: self._ctl_button(L, b, fb.currentData(), None))
            pb = self._ctl_press_box(cur[1] if cur else "")
            pb.setEnabled(bool(cur))
            pb.activated.connect(lambda _i, b=b, fb=fb, pb=pb, L=L: self._ctl_button(L, b, fb.currentData(),
                                                                                 pb.currentData()))
            g.addWidget(fb, r, col + 2)
            g.addWidget(pb, r, col + 3)
            self.ctl_boxes[L][b] = fb
        g.setColumnStretch(2, 1)
        g.setColumnStretch(7, 1)
        g.setColumnMinimumWidth(4, 12)
        v.addLayout(g)
        v.addWidget(label(self.t("ctl.combos"), "h2"))
        v.addWidget(label(self.t("ctl.combohint"), "hint", wrap=True))
        cg = QGridLayout()
        cg.setHorizontalSpacing(8)
        cg.setVerticalSpacing(3)
        for r, row in enumerate(lay.combo_rows(L)):
            text, inp, flag = row
            eb = QComboBox()
            eb.setEditable(True)
            eb.addItems(COMBO_SUGGEST)
            eb.setEditText(text)
            eb.setMinimumContentsLength(14)
            fb = self._ctl_func_box(inp, nothing=False)
            pb = self._ctl_press_box(flag)
            commit = (lambda *_a, row=row, eb=eb, fb=fb, pb=pb, L=L:
                      self._ctl_combo(L, row, eb.currentText(), fb.currentData(), pb.currentData()))
            eb.lineEdit().editingFinished.connect(commit)
            eb.activated.connect(commit)
            fb.activated.connect(commit)
            pb.activated.connect(commit)
            rm = QPushButton(self.t("ctl.remove"))
            rm.clicked.connect(lambda _c=False, row=row, L=L: self._ctl_edit("set_combo", L, row, "", None))
            cg.addWidget(eb, r, 0)
            cg.addWidget(QLabel("->"), r, 1)
            cg.addWidget(fb, r, 2)
            cg.addWidget(pb, r, 3)
            cg.addWidget(rm, r, 4)
        cg.setColumnStretch(2, 1)
        v.addLayout(cg)
        add = QPushButton(self.t("ctl.add"))
        add.clicked.connect(lambda _c=False, L=L: self._ctl_add_combo(L))
        v.addWidget(hbox(add))
        v.addWidget(label(self.t("ctl.sticks"), "h2"))
        sg = QGridLayout()
        sg.setHorizontalSpacing(8)
        for r, (stick, opts) in enumerate((("lstick", ("analog", "dpad", "none")), ("rstick", RSTICK_SUGGEST))):
            ic = QLabel()
            ic.setPixmap(glyph_pixmap("analog", 18, ratio))
            sg.addWidget(ic, r, 0)
            sg.addWidget(QLabel(self._ctl_func_text(stick) if self._ctl_desc(stick) else self.t("ps2." + stick)), r, 1)
            sb = QComboBox()
            sb.setEditable(stick == "rstick")
            cur = controls_model.fmt_value(stick, lay.layers[L][stick])
            for o in opts:
                sb.addItem(self.t("ctl.stick." + o) if stick == "lstick" else o, o)
            if sb.findData(cur) < 0:
                sb.addItem(cur, cur)
            sb.setCurrentIndex(sb.findData(cur))
            if stick == "rstick":
                sb.lineEdit().editingFinished.connect(
                    lambda sb=sb, L=L, cur=cur: sb.currentText() != cur and
                    self._ctl_edit("set_stick", L, "rstick", sb.currentText()))
            sb.activated.connect(lambda _i, sb=sb, L=L, s=stick: self._ctl_edit("set_stick", L, s,
                                                                                sb.currentData() or sb.currentText()))
            sg.addWidget(sb, r, 2)
            self.ctl_boxes[L][stick if stick == "rstick" else "analog"] = sb
        sg.setColumnStretch(3, 1)
        v.addLayout(sg)
        v.addStretch(1)
        area.setWidget(inner)
        self.ctl_areas[L] = area
        return area

    def _ctl_button(self, L, button, inp, flag):
        lay = self._ctl_layout()
        if flag is None:                                   # a new function: keep the row's press kind when it had one
            cur = lay.button_rows(L).get(button)
            flag = cur[1] if cur and inp else ""
        self._ctl_edit("set_button", L, button, inp or None, flag or "")

    def _ctl_combo(self, L, row, text, inp, flag):
        text = "".join((text or "").split()).lower()
        if (text, inp, flag or "") == (row[0], row[1], row[2]):
            return
        self._ctl_edit("set_combo", L, row, text, inp, flag or "")

    def _ctl_add_combo(self, L):
        """a free combination for the new row (L + R first), mapped to nothing useful until the player picks"""
        lay = self._ctl_layout()
        used = {c for c, _o in controls_model._entries(lay.layers[L])}
        text = next((s for s in COMBO_SUGGEST if controls_model.parse_combo(s) not in used), None)
        if text is None:
            return
        free = [i for i in controls_model.BUTTON_INPUTS if not lay.layers[L][i]]
        self._ctl_edit("set_combo", L, None, text, free[0] if free else "l2", "")

    # --- convert ----------------------------------------------------------------------------------------------
    def _page_convert(self):
        self.add(label(self.t("conv.h"), "h1"))
        form = QFormLayout()
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(2)
        look = [k.upper() for k in ("icon0", "icon1", "pic0", "pic1", "snd0")
                if self.project.slots[k]["mode"] != "none"]
        out = os.path.join(self.out_dir(), self._game_folder_name()) if self.out_dir() else "-"
        for k, v in ((self.t("conv.disc"), self.s.get("source") or "-"), (self.t("conv.out"), out),
                     (self.t("conv.title"), self._shown_title()),
                     (self.t("conv.look"), ", ".join(look) or self.t("conv.look.none")),
                     (self.t("conv.settings"), self.t("set.count", len(self.overrides())) if self.overrides()
                      else self.t("set.ourvalues"))):
            val = QLabel(v)
            val.setTextInteractionFlags(Qt.TextSelectableByMouse)
            form.addRow(label(k + ":", "hint"), val)
        self.body_lay.addLayout(form)
        self._advanced()
        self._progress_area(self.t("conv.go"), self._start_convert)
        self._nav("controls")

    def _advanced(self):
        box = QGroupBox(self.t("adv.h"))
        lay = QHBoxLayout(box)
        lay.setContentsMargins(8, 4, 8, 4)
        self.adv = {}
        for key, txt, opts in (("movies", "adv.movies", ("auto", "skip", "convert")),
                               ("audio", "adv.audio", ("all", "min", "none"))):
            lay.addWidget(QLabel(self.t(txt)))
            c = QComboBox()
            for o in opts:
                c.addItem(self.t("adv.%s.%s" % (key, o)), o)
            c.setCurrentIndex(max(0, c.findData(self.s.get(key, opts[0]))))
            self.adv[key] = c
            lay.addWidget(c)
            lay.addSpacing(10)
        lay.addWidget(QLabel(self.t("adv.jobs")))
        sp = QSpinBox()
        sp.setRange(1, 64)
        sp.setValue(int(self.s.get("jobs") or os.cpu_count() or 4))
        self.adv["jobs"] = sp
        lay.addWidget(sp)
        lay.addStretch(1)
        self.add(box)

    def _progress_area(self, go_text, go_cmd):
        """buttons; a splitter: progress (step, overall bar, the step's own bar when it reports one, time left, the
        step list) above the log, which is always shown (stick-to-bottom scrolling, translator flood folded)"""
        self.btn_go = QPushButton(go_text)
        f = QApplication.font()
        f.setBold(True)
        self.btn_go.setFont(f)
        self.btn_go.setDefault(True)
        self.btn_go.clicked.connect(go_cmd)
        self.btn_stop = QPushButton(self.t("conv.stop"))
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self._stop)
        self.btn_open = QPushButton(self.t("conv.open"))
        self.btn_open.clicked.connect(self._open_out)
        self.btn_open.setVisible(False)
        copy = QPushButton(self.t("log.copy"))
        copy.clicked.connect(lambda: QApplication.clipboard().setText("\n".join(t for t, _g in self.log_lines)))
        openlog = QPushButton(self.t("log.open"))
        openlog.clicked.connect(self._open_log)
        self.add(hbox(self.btn_go, self.btn_stop, self.btn_open, None, copy, openlog, stretch=False))
        outer = QHBoxLayout()
        split = QSplitter(Qt.Vertical)
        top = QWidget()
        left = QVBoxLayout(top)
        left.setContentsMargins(0, 0, 0, 0)
        self.step_label = label("", "h2")
        left.addWidget(self.step_label)
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.sub = QProgressBar()
        self.sub.setRange(0, 1000)
        self.sub.setTextVisible(False)
        self.sub.hide()                                        # shown while the step reports its own progress
        left.addWidget(self.bar)
        left.addWidget(self.sub)
        self.left_label = label("", "hint")
        left.addWidget(self.left_label)
        self.result_label = label("", wrap=True)
        self.result_hint = label("", "hint", wrap=True)
        left.addWidget(self.result_label)
        left.addWidget(self.result_hint)
        left.addStretch(1)
        split.addWidget(top)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(20000)
        self.log.setFont(QFont("Consolas", max(7, int(QApplication.font().pointSizeF()))))
        self.log.setContextMenuPolicy(Qt.CustomContextMenu)
        self.log.customContextMenuRequested.connect(self._log_menu)
        split.addWidget(self.log)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setSizes([130, 400])
        outer.addWidget(split, 1)
        from ps2rc.convert.pipeline import STEPS
        steps = QVBoxLayout()
        steps.setSpacing(1)
        self.step_marks = {}
        for name, title in STEPS:
            icon, lab = QLabel(), QLabel(i18n.step_title(name, title, self.lang))
            steps.addWidget(hbox(icon, lab))
            self.step_marks[name] = (icon, lab)
            self._mark_step(name, self.step_state.get(name, "todo"))
        steps.addStretch(1)
        outer.addLayout(steps)
        self.body_lay.addLayout(outer, 1)
        self._render_log()
        if self.running:
            self.btn_go.setEnabled(False)
            self.btn_stop.setEnabled(True)

    # one line per translated function, and the translator's per-worker internals (fixpoint rounds with their
    # address lists, rule counters): in the log file always, on the screen only with "every line"
    VERBOSE_RX = re.compile(r"^\s*(\w+:[0-9a-fA-F]{6,8}\s|translate_psp: (wide fixpoint|syscall rule applied|"
                            r"\d+ hooked functions given full argument liveness))")

    def _log_menu(self, pos):
        menu = self.log.createStandardContextMenu()
        a = menu.addAction(self.t("log.verbose"))
        a.setCheckable(True)
        a.setChecked(bool(self.s.get("log_verbose")))
        a.toggled.connect(lambda v: (self.s.__setitem__("log_verbose", v), self._render_log()))
        menu.exec(self.log.mapToGlobal(pos))

    def _render_log(self):
        self.log.clear()
        self._folded = 0
        for line, tag in self.log_lines[-20000:]:
            self._show_line(line)
        self._flush_folded()

    def _flush_folded(self):
        if getattr(self, "_folded", 0):
            self.log.appendPlainText(self.t("log.more", self._folded))
            self._folded = 0

    def _show_line(self, text):
        """append one line; translator flood folded into one line per run of them unless verbose; the view follows
        the end only while the reader is at the end"""
        if not self.s.get("log_verbose") and self.VERBOSE_RX.match(text):
            self._folded = getattr(self, "_folded", 0) + 1
            if self._folded >= 2000:
                self._flush_folded()
            return
        self._flush_folded()
        sb = self.log.verticalScrollBar()
        at_end = sb.value() >= sb.maximum() - 2
        self.log.appendPlainText(text)
        if at_end:
            sb.setValue(sb.maximum())

    def _log_path(self):
        if not getattr(self, "log_file", None):
            d = local_dir("logs")
            os.makedirs(d, exist_ok=True)
            self.log_file = os.path.join(d, "convert_%s.log" % time.strftime("%Y%m%d_%H%M%S"))
        return self.log_file

    def _open_log(self):
        p = getattr(self, "log_file", None)
        if p and os.path.isfile(p):
            show_in_explorer(p, select=True)

    def _mark_step(self, name, state):
        """the step list: a painted status icon (no symbol glyphs) + the step's name"""
        self.step_state[name] = state
        m = getattr(self, "step_marks", {}).get(name)
        if m and self.page in ("convert", "update"):
            icon, lab = m
            size = QFontMetrics(QApplication.font()).height()
            icon.setPixmap(state_pixmap(state, size, self.devicePixelRatioF()))
            icon.setFixedSize(size, size)
            color = {"run": BLUE, "done": GOOD, "kept": GOOD, "fail": BAD}.get(state, hint_color())
            f = QApplication.font()
            f.setBold(state == "run")
            lab.setFont(f)
            lab.setStyleSheet("color: %s;" % color)

    def _say(self, text, tag=None, keep=True):
        if keep:
            self.log_lines.append((text, tag))
            try:
                with open(self._log_path(), "a", encoding="utf-8") as f:   # the whole log, every line
                    f.write(text + "\n")
            except OSError:
                pass
        if self.page in ("convert", "update") and getattr(self, "log", None) is not None:
            try:
                self._show_line(text)
            except RuntimeError:
                pass

    def options_kw(self):
        """everything the converter needs from this window -> Options keywords (except the asset files)"""
        ov = self.overrides()
        adv = getattr(self, "adv", None) if self.page == "convert" else None
        kw = {"title": xtitle.clean_title(self.project.title) or self._default_title(),
              "movies": adv["movies"].currentData() if adv else self.s.get("movies", "auto"),
              "audio": adv["audio"].currentData() if adv else self.s.get("audio", "all"),
              "jobs": int(adv["jobs"].value()) if adv else int(self.s.get("jobs") or os.cpu_count() or 4),
              "work": work_dir(self.out_dir()), "lang": self.lang}
        if self.out_dir():
            kw["folder"] = os.path.join(self.out_dir(), self._game_folder_name())   # PS2RC_<serial>, not the pack's
        from ps2rc.xmb import media
        ff = media.find_ffmpeg()
        if ff and os.path.isfile(os.path.join(os.path.dirname(ff), "ffprobe" + media.EXE)):
            kw["ffmpeg"] = ff                         # the intro movies (they need ffprobe next to it too)
        if backend.options_take("settings"):
            kw["settings"] = ov                       # written on the template's own lines
        else:
            kw["cfg"] = ["%s=%s" % kv for kv in ov.items()]  # older core: appended, the later line wins
        return kw

    def _start_convert(self):
        src, out = self.s.get("source"), self.out_dir()
        if not src or not os.path.exists(src):
            QMessageBox.critical(self, self.t("app"), self.t("disc.none"))
            self.show_page("disc")
            return
        if not out:
            QMessageBox.critical(self, self.t("app"), self.t("out.need"))
            self.show_page("output")
            return
        title = xtitle.clean_title(self.project.title)
        if title and not xtitle.usable(title):
            QMessageBox.critical(self, self.t("app"), self.t("title.too_long", len(title.encode("utf-8"))))
            self.show_page("customize")
            return
        if self.built is not None and self.built.media_bytes > MEDIA_BUDGET_MAX:
            QMessageBox.critical(self, self.t("app"), self.t("budget.over"))
            self.show_page("customize")
            return
        name = self._game_folder_name()
        if backend.foreign_folder(os.path.join(out, name)):
            alt = backend.free_folder_name(out, name)
            if alt == name or QMessageBox.question(
                    self, self.t("app"), self.t("out.foreign", os.path.join(out, name), alt)) != QMessageBox.Yes:
                return
            self.s.setdefault("game_folders", {})[(self.ident or {}).get("serial") or ""] = alt
        bad = self._unsafe_paths(out)
        if bad:
            QMessageBox.critical(self, self.t("app"), self.t("path.unsafe", "\n".join(bad)))
            return
        if self.page == "convert":
            for k in ("movies", "audio"):
                self.s[k] = self.adv[k].currentData()
            self.s["jobs"] = self.adv["jobs"].value()
        self._save()
        kw = self.options_kw()
        proj = Project.from_dict(json.loads(json.dumps(self.project.to_dict())))
        self._run_conversion(src, out, kw, proj)

    def _unsafe_paths(self, out):
        """folders the PSP toolchain must use whose path has characters outside this Windows' code page (its
        assembler / linker open files through the narrow C runtime: measured, an accented letter on a Japanese
        code page fails); Cyrillic on a Russian Windows is fine"""
        from ps2rc.xmb import media
        app = os.path.dirname(ROOT)
        return [p for p in (out,) if p and not media.ansi_safe(p)] + [p for p in (app,) if not media.ascii_path(p)]

    def _run_conversion(self, src, out, kw, proj, extra=None):
        """proj None: no menu look is passed (an update keeps the folder's own)"""
        safe_temp(kw.get("work"))
        from ps2rc.convert.pipeline import Conversion, Options, STEPS
        self.progress = Progress()
        self.progress.listeners.append(lambda ev: self._ui(self._event, ev))
        self.eta = etamod.Eta([n for n, _t in STEPS], self.s.get("eta_weights"))
        self.running, self.result_folder = True, None
        self.log_lines, self.log_file = [], None
        for n, _t in STEPS:
            self._mark_step(n, "todo")
        self.log.clear()
        self.btn_go.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.btn_open.setVisible(False)
        self.result_label.setText("")
        self.result_hint.setText("")
        self.bar.setValue(0)
        self.sub.setValue(0)
        self._say("--- %s %s" % (self.t("conv.go"), time.strftime("%H:%M:%S")), "info")

        def work():
            try:
                kw2 = dict(kw)
                if proj is not None:
                    self.progress.info(self.t("conv.making_look"))
                    built = proj.build(local_dir("xmb"), log=self.progress.log)
                    for key, err in built.errors.items():
                        self.progress.warn(self.t("conv.look_err", "%s: %s" % (key.upper(), err)))
                    kw2.update(built.options())
                kw2.update(extra or {})
                self.conv = Conversion(Options(src, out, **kw2), self.progress)
                res = self.conv.run()
                self._ui(self._event, ("finished", res))
            except ConvertError as e:
                self._ui(self._event, ("error", e))
            except Exception as e:      # a bug: show it, keep the window alive
                import traceback
                self._ui(self._event, ("error", ConvertError("internal", "%s: %s" % (type(e).__name__, e), "",
                                                             tail=traceback.format_exc().splitlines()[-12:])))
            finally:
                self.progress.close()
        self.worker = self._thread(work)

    def _stop(self):
        if self.progress:
            self.progress.cancelled = True
            self._say(self.t("conv.stopping"), "warn")

    def _open_out(self):
        d = self.result_folder or self.out_dir()
        if d and os.path.isdir(d):
            show_in_explorer(d)

    def _update_eta(self):
        if not (self.running and self.eta and self.page in ("convert", "update")):
            return
        conv = self.conv
        est = getattr(conv, "result", {}).get("estimate") if conv else None
        if est and not getattr(self.eta, "_est_set", False):
            self.eta.set_estimate(est.get("steps"))
            self.eta._est_set = True
        left = self.eta.left()
        if left is not None:
            self.left_label.setText(self.t("conv.left", etamod.fmt_left(left, self.lang)))
            self.bar.setValue(int(1000 * self.eta.fraction()))

    def _event(self, ev):
        k = ev[0]
        live = self.page in ("convert", "update")
        if k == "step":
            _k, i, n, name, title = ev
            self.eta.step(name)
            self._mark_step(name, "run")
            if live:
                self.step_label.setText(self.t("conv.step", i, n, i18n.step_title(name, title, self.lang)))
                self.sub.setValue(0)
                self.sub.hide()
            self._say("[%d/%d] %s" % (i, n, title), "info")
        elif k == "done":
            _k, name, secs, cached = ev
            self.eta.finish(name, secs, cached)
            self._mark_step(name, "kept" if cached else "done")
            self._say("    %s" % (self.t("conv.kept") if cached else "%.0f s" % secs))
        elif k == "pct":
            _k, name, done, total, _unit = ev
            if self.eta:
                self.eta.pct(name, done, total)
            if live:
                self.sub.setVisible(bool(total))
                self.sub.setValue(int(1000 * done / total) if total else 0)
        elif k == "info":
            self._say("    " + ev[1], "info")
        elif k == "warn":
            m = JOBS_RX.match(ev[1])
            if m:                                          # not a problem: plain words, no "!"
                self._say("    " + self.t("conv.jobs", int(m.group(1))), "info")
            else:
                self._say("    ! " + ev[1], "warn")
        elif k == "log":
            self._say("      " + ev[1])
        elif k == "finished":
            self._end()
            res = ev[1] or {}
            self._remember_times()
            if res.get("update") is not None and live:
                u = res["update"] or {}
                self.bar.setValue(1000)
                self.left_label.setText("")
                n = sum(v if isinstance(v, int) else len(v or []) for v in (u.get("written"), u.get("rewritten")))
                self.result_label.setText("%s (%d)" % (self.t("upd.done"), n))
                set_kind(self.result_label, "good")
            elif res.get("folder"):
                self.result_folder = res["folder"]
                if live:
                    self.bar.setValue(1000)
                    self.left_label.setText("")
                    self.step_label.setText(self.t("conv.done", res["folder"]))
                    self.result_label.setText(self.t("conv.done", res["folder"]))
                    set_kind(self.result_label, "good")
                    self.result_hint.setText(self.t("conv.done.psp" if self.out_is_drive() else "conv.done.pc"))
                    self.btn_open.setVisible(True)
        elif k == "error":
            e = ev[1]
            self._end()
            for n, st in list(self.step_state.items()):
                if st == "run":
                    self._mark_step(n, "fail")
            for line in e.lines():
                self._say(line, "err")
            what, todo = i18n.error_text(e.code, e.message, e.hint, self.lang)
            if live:
                self.left_label.setText("")
                self.result_label.setText("%s  (%s)" % (what, e.message) if what != e.message else what)
                set_kind(self.result_label, "bad")
                self.result_hint.setText(self.t("err.what", todo) + ("\nlog: %s" % e.log if e.log else ""))
                self.btn_go.setText(self.t("conv.again"))

    def _remember_times(self):
        if self.eta:
            w = dict(self.s.get("eta_weights") or {})
            for n, (secs, cached) in self.eta.done.items():
                if not cached and secs > 1:
                    w[n] = round(secs, 1)
            self.s["eta_weights"] = w
            self._save()

    def _end(self):
        self.running = False
        if self.page in ("convert", "update"):
            self.btn_go.setEnabled(True)
            self.btn_stop.setEnabled(False)

    # --- check / update ---------------------------------------------------------------------------------------
    def _page_folders(self):
        self.add(label(self.t("chk.h"), "h1"))
        self.scan_busy = self.add(BusyRow())
        self.scan_busy.set(self.t("chk.scanning"))
        self.found_label = self.add(label("", wrap=True))
        tv = QTreeWidget()
        tv.setRootIsDecorated(False)
        tv.setHeaderLabels([self.t("chk.col." + c) for c in ("title", "where", "ver", "state")])
        extra = list(self.s.get("check_extra") or [])

        def work():
            try:
                drives = psp_drives.drives()
                folders = psp_drives.game_folders(drives)
                for p in extra:
                    if os.path.isdir(p) and p not in [f["path"] for f in folders]:
                        m = psp_drives.read_manifest(p)
                        folders.append({"path": p, "drive": "", "name": os.path.basename(p),
                                        "title": psp_drives.folder_title(p),
                                        "version": psp_drives.manifest_version(m), "manifest": m is not None})
            except Exception:          # nothing found is shown as such
                drives, folders = [], []
            self._ui(self._folders_found, drives, folders, tv)
        self._thread(work)
        tv.setColumnWidth(0, 200)
        tv.setColumnWidth(1, 360)
        tv.setMaximumHeight(170)
        tv.currentItemChanged.connect(lambda cur, _p: self._folder_selected(cur))
        self.tv = self.add(tv)
        ref = QPushButton(self.t("out.refresh"))
        ref.clicked.connect(lambda: self.show_page("folders"))
        pick = QPushButton(self.t("chk.pick"))
        pick.clicked.connect(self._pick_check_folder)
        chk = QPushButton(self.t("chk.check"))
        chk.setDefault(True)
        chk.clicked.connect(self._check)
        upd = QPushButton(self.t("chk.update"))
        upd.clicked.connect(self._update_plan)
        self.add(hbox(chk, upd, None, ref, pick, stretch=False))
        self.chk_bar = QProgressBar()
        self.chk_bar.setRange(0, 1000)
        self.add(self.chk_bar)
        self.chk_label = self.add(label("", wrap=True))
        self.chk_hint = self.add(label("", "hint", wrap=True))
        self.repair_btn = QPushButton(self.t("chk.repair"))
        self.repair_btn.clicked.connect(self._update_plan)
        self.repair_row = self.add(hbox(self.repair_btn))
        self.repair_row.setVisible(False)
        self.add(label(self.t("upd.saves"), "hint"))
        self.body_lay.addStretch(1)
        self._nav(None)

    def _folders_found(self, drives, folders, tv):
        if self.page != "folders":
            return
        self.drive_list, self.folders = drives, folders
        self.scan_busy.finish("")
        self.scan_busy.hide()
        self.found_label.setText(self.t("chk.found") if folders else self.t("chk.none"))
        for f in folders:
            if f["version"]:
                state = self.t("chk.update_av") if psp_drives.update_available(f["version"], VERSION) else \
                    self.t("chk.uptodate")
            else:
                state = self.t("chk.unknown")
            it = QTreeWidgetItem([f["title"] or f["name"], f["path"], f["version"] or "?", state])
            it.setData(0, Qt.UserRole, f["path"])
            tv.addTopLevelItem(it)
            if f["path"] == self.selected_folder:
                tv.setCurrentItem(it)

    def _folder_selected(self, item):
        if item is not None:
            self.selected_folder = item.data(0, Qt.UserRole)
            self._save()

    def _pick_check_folder(self):
        p = QFileDialog.getExistingDirectory(self, self.t("chk.pick"), self.selected_folder or "")
        if p:
            p = os.path.normpath(p)
            ex = self.s.setdefault("check_extra", [])
            if p not in ex:
                ex.append(p)
            self.selected_folder = p
            self.show_page("folders")

    def _check(self):
        f = self.selected_folder
        if not f or not os.path.isdir(f):
            QMessageBox.information(self, self.t("app"), self.t("chk.select"))
            return
        self.chk_label.setText(self.t("chk.running", f))
        set_kind(self.chk_label, None)
        self.chk_hint.setText("")
        self.repair_row.setVisible(False)
        prog = Progress()

        def on(ev):
            if ev[0] == "pct" and ev[3]:
                self._ui(self._chk_pct, int(1000 * ev[2] / ev[3]))
        prog.listeners.append(on)

        def work():
            try:
                rep = backend.verify(f, prog)
                self._ui(self._checked, rep, None)
            except backend.NotAvailable as e:
                self._ui(self._checked, None, self.t("chk.na", e))
            except Exception as e:     # shown, not fatal
                self._ui(self._checked, None, "%s: %s" % (type(e).__name__, e))
        self._thread(work)

    def _chk_pct(self, v):
        if self.page == "folders":
            self.chk_bar.setValue(v)

    def _checked(self, rep, err):
        if self.page != "folders":
            return
        if err:
            self.chk_label.setText(err)
            set_kind(self.chk_label, "bad")
            return
        r = backend.report_lists(rep)
        self.chk_bar.setValue(1000)
        if r["ok"]:
            self.chk_label.setText(self.t("chk.allok", r["files"]) if r["files"] else self.t("chk.allok2"))
            set_kind(self.chk_label, "good")
        else:
            bad = r["missing"] + r["corrupt"]
            self.chk_label.setText(self.t("chk.bad", len(bad)))
            set_kind(self.chk_label, "bad")
            self.chk_hint.setText("\n".join(bad[:12]) + ("\n..." if len(bad) > 12 else ""))
            self.repair_row.setVisible(True)
        if r["user_changed"]:
            self.chk_hint.setText((self.chk_hint.text() + "\n" + self.t("chk.user", len(r["user_changed"]))).strip())

    def _update_plan(self):
        f = self.selected_folder
        if not f or not os.path.isdir(f):
            QMessageBox.information(self, self.t("app"), self.t("chk.select"))
            return
        self.show_page("update")

    def _page_update(self):
        f = self.selected_folder
        self.add(label(self.t("upd.h"), "h1"))
        self.add(label(f or "", "hint"))
        self.plan_label = self.add(label(self.t("upd.planning")))
        tv = QTreeWidget()
        tv.setRootIsDecorated(False)
        tv.setHeaderLabels([self.t("upd.col." + c) for c in ("file", "do", "why")])
        tv.setColumnWidth(0, 320)
        tv.setColumnWidth(1, 140)
        tv.setMaximumHeight(170)
        self.plan_tv = self.add(tv)
        pick = QPushButton(self.t("disc.pick"))
        pick.clicked.connect(self._pick_update_disc)
        self.upd_disc = label(self.s.get("source") or "", "hint")
        self.disc_row = self.add(hbox(label(self.t("upd.needdisc"), "warn"), pick, self.upd_disc))
        self.disc_row.setVisible(False)
        self.add(label(self.t("upd.saves"), "hint"))
        cb = QPushButton(self.t("ctl.change"))
        cb.setToolTip(self.t("ctl.folder.tip"))
        cb.clicked.connect(lambda: self._open_folder_controls(f))
        cb.setEnabled(self._folder_cfg_path(f) is not None)
        self.upd_controls = self.add(hbox(cb))
        self._progress_area(self.t("upd.go"), self._start_update)
        self.btn_go.setEnabled(False)
        self._nav("folders")
        self.plan_needs_disc = False

        def work():
            try:
                src = self.s.get("source") if self.s.get("source") and os.path.exists(self.s["source"]) else None
                plan = backend.plan_update(f, src)
                self._ui(self._planned, backend.plan_actions(plan), None)
            except backend.NotAvailable as e:
                self._ui(self._planned, None, self.t("chk.na", e))
            except Exception as e:     # shown, not fatal
                self._ui(self._planned, None, "%s: %s" % (type(e).__name__, e))
        self._thread(work)

    def _planned(self, actions, err):
        if self.page != "update":
            return
        if err:
            self.plan_label.setText(err)
            set_kind(self.plan_label, "bad")
            return
        todo = [a for a in actions if a[1] != "keep"]
        for path, act, why, _needs in todo:
            wkey = WHY_KEYS.get(why)
            self.plan_tv.addTopLevelItem(QTreeWidgetItem([path, self.t("act." + act) if ("act." + act) in i18n.T
                                                          else act, self.t(wkey) if wkey else why]))
        self.plan_needs_disc = any(a[3] for a in todo)
        if not todo:
            self.plan_label.setText(self.t("upd.nothing"))
            set_kind(self.plan_label, "good")
            return
        self.plan_label.setText("%d" % len(todo))
        self.disc_row.setVisible(self.plan_needs_disc)
        self.btn_go.setEnabled(True)

    def _pick_update_disc(self):
        p, _f = QFileDialog.getOpenFileName(self, self.t("disc.pick"), "",
                                            "%s (*.iso *.bin *.img);;%s (*.*)" % (self.t("disc.types"),
                                                                                  self.t("pick.all")))
        if p:
            self.s["source"] = os.path.normpath(p)
            self.upd_disc.setText(self.s["source"])
            self._save()

    def _start_update(self):
        f = self.selected_folder
        src = self.s.get("source")
        if self.plan_needs_disc and not (src and os.path.exists(src)):
            QMessageBox.information(self, self.t("app"), self.t("upd.needdisc"))
            return
        kw = {"work": work_dir(os.path.dirname(f)), "update": True, "dry_run": False, "folder": f,
              "movies": self.s.get("movies", "auto"), "audio": self.s.get("audio", "all")}
        self._run_conversion(src, os.path.dirname(f), kw, None)

    # --- close ------------------------------------------------------------------------------------------------
    def closeEvent(self, e):
        if self.running and self.worker and self.worker.is_alive():
            if QMessageBox.question(self, self.t("app"), self.t("conv.running")) != QMessageBox.Yes:
                e.ignore()
                return
            self._stop()
        self._stop_sound()
        self._save()
        e.accept()


def quiet_qt():
    """Qt's messages (also a fatal "no Qt platform plugin could be initialized") go to stderr - the start log of
    gui/run_gui.py - instead of a modal Windows message box; no Windows error dialogs for this process"""
    from PySide6.QtCore import QtMsgType, qInstallMessageHandler

    def handler(kind, _ctx, msg):
        stream = sys.stderr or sys.__stderr__
        if stream is not None:                           # pythonw without a log: nothing to write to
            stream.write("qt %s: %s\n" % (str(kind).split(".")[-1], msg))
            stream.flush()
        if kind == QtMsgType.QtFatalMsg:
            os._exit(3)
    qInstallMessageHandler(handler)
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)   # no critical-error / GP-fault boxes


def make_app(argv=None):
    """the QApplication: the system's UI font (Segoe UI 9 pt on Windows) and Fusion widgets; Qt handles HiDPI"""
    if QApplication.instance() is None:
        quiet_qt()
        if os.name == "nt":
            try:                                           # the taskbar groups and shows the window's own icon
                import ctypes
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("SandstoneDev.PS2RC.Converter")
            except (AttributeError, OSError):
                pass
    if QApplication.instance() is None:
        try:                                               # our Qt's plugins, set here and not in the environment
            import PySide6
            plugins = os.path.join(os.path.dirname(PySide6.__file__), "plugins")
            if os.path.isdir(plugins):
                QCoreApplication.addLibraryPath(plugins)
        except ImportError:
            pass
    app = QApplication.instance() or QApplication(argv or sys.argv)
    qt_wheel.install(app)                                  # the wheel scrolls pages, never changes a value by itself
    try:
        import app_icon
        app.setWindowIcon(app_icon.qicon(load_settings().get("icon_variant") or app_icon.DEFAULT))
    except Exception:          # an icon is a nicety: never stop the window for it
        pass
    app.setStyle("Fusion")
    if app.font().family() in ("Sans Serif", "") and os.name == "nt":
        app.setFont(QFont("Segoe UI", 9))                  # offscreen / minimal platforms have no system font
    return app


def main():
    app = make_app(sys.argv)
    from ps2rc import proc
    # nothing of our Python / Qt reaches other programs: helper scripts run under python.exe (a hidden console the
    # tools they start share, no console windows), and the environment loses our Qt / Python variables - also for
    # programs Windows starts from a file dialog or an Explorer window opened here
    sys.executable = proc.python_for_children()
    proc.scrub_environment()
    try:
        w = App()
    except Exception:                                      # into the start log (gui/run_gui.py) / the console
        import traceback
        traceback.print_exc()
        return 1
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
