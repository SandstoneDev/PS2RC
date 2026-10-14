"""The converter window (Qt / PySide6) end to end with a fake converter core: every page in both languages, the menu
look reaches the converter as exact files, settings reach it as cfg keys, progress / errors / check / update are
shown plainly, choices are remembered, interface size works, the preview renders. Runs offscreen (no window on the
screen); skipped where PySide6 is not installed."""
import os
import time

import pytest

pytest.importorskip("PySide6")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
if os.name == "nt":
    os.environ.setdefault("QT_QPA_FONTDIR", os.path.join(os.environ.get("WINDIR", ""), "Fonts"))

import i18n  # noqa: E402
from test_gui_parts import TEMPLATE  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    import ps2rc_gui
    return ps2rc_gui.make_app([])


@pytest.fixture
def gui(qapp, tmp_path, monkeypatch):
    monkeypatch.setenv("APPDATA", str(tmp_path / "appdata"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    tpl = tmp_path / "tpl_en.cfg"
    tpl.write_text(TEMPLATE, encoding="utf-8")
    (tmp_path / "tpl_ru.cfg").write_text(TEMPLATE.replace("GRAPHICS", "GRAFIKA"), encoding="utf-8")
    monkeypatch.setenv("PS2RC_CFG_TEMPLATE", str(tpl))
    import ps2rc_gui
    import backend
    monkeypatch.setattr(backend, "identify", lambda src: {"ok": True, "serial": "TEST-00001", "version": "1.00",
                                                          "pack": "example", "title": "PS2RC TEST-00001",
                                                          "folder": "PS2RC_TEST", "code": None, "message": "",
                                                          "hint": ""})
    monkeypatch.setattr(ps2rc_gui, "system_lang", lambda: "en")     # deterministic on any test PC
    iso = tmp_path / "game.iso"
    iso.write_bytes(b"\0" * 2048)
    path = str(tmp_path / "appdata" / "gui.json")
    app = ps2rc_gui.App(settings_path=path)
    app.show()
    app.s["source"] = str(iso)
    app.s["out_choice"] = "pc"
    app.s["out_pc"] = str(tmp_path / "out")
    app._identify_async(str(iso))
    pump(qapp, lambda: app.ident and not app.ident.get("busy"))
    yield app, ps2rc_gui, qapp, tmp_path, monkeypatch
    app.running = False
    app.close()


def test_folder_name_and_a_foreign_folder(gui, tmp_path):
    """PS2RC_<serial> by default; a folder of that name the converter did not make is never overwritten"""
    app, ps2rc_gui, qapp, _tmp, monkeypatch = gui
    import backend
    from PySide6.QtWidgets import QMessageBox
    assert backend.default_folder("ABCD-12345") == "PS2RC_ABCD12345"
    assert backend.default_folder("x" * 40) == ("PS2RC_" + "X" * 40)[:32] and backend.default_folder("") == \
        "PS2RC_GAME"
    app.ident["folder"] = backend.default_folder(app.ident["serial"])
    out = app.out_dir()
    target = os.path.join(out, app._game_folder_name())
    os.makedirs(target)
    open(os.path.join(target, "EBOOT.PBP"), "wb").close()           # someone else's folder (no manifest)
    asked, started = [], []
    monkeypatch.setattr(QMessageBox, "question", lambda *a: asked.append(a[2]) or QMessageBox.No)
    monkeypatch.setattr(app, "_run_conversion", lambda *a, **k: started.append(a))
    app._start_convert()
    assert not started and asked and target in asked[0] and "_2" in asked[0]
    monkeypatch.setattr(QMessageBox, "question", lambda *a: QMessageBox.Yes)
    app._start_convert()
    assert started and started[0][2]["folder"] == target + "_2"
    with open(os.path.join(target, "ps2rc_manifest.json"), "w") as f:          # now it is ours: no question
        f.write("{}")
    assert not backend.foreign_folder(target)


def test_open_folder_and_log_only_through_explorer(gui, monkeypatch):
    app, ps2rc_gui, qapp, tmp_path, _mp = gui
    seen = []

    class FakePopen:
        def __init__(self, args, env=None, **kw):
            seen.append((args, env))
    monkeypatch.setattr(ps2rc_gui.subprocess, "Popen", FakePopen)
    monkeypatch.setattr(os, "startfile", lambda *a: seen.append(("startfile", a)), raising=False)
    monkeypatch.setenv("QT_PLUGIN_PATH", "x")
    d = tmp_path / "res"
    d.mkdir()
    app.result_folder = str(d)
    app._open_out()
    log = tmp_path / "c.log"
    log.write_text("x")
    app.log_file = str(log)
    app._open_log()
    assert len(seen) == 2 and all(a[0] != "startfile" for a in seen)
    if os.name == "nt":
        assert seen[0][0][0].lower().endswith("explorer.exe") and seen[1][0][1] == "/select,"
    assert all("QT_PLUGIN_PATH" not in env for _a, env in seen)


def test_translator_details_folded_and_jobs_in_plain_words(gui):
    app, ps2rc_gui, qapp, tmp_path, _mp = gui
    app.show_page("convert")
    app.log_lines, app.log_file = [], None
    app._render_log()
    for _ in range(3):
        app._say("          translate_psp: syscall rule applied 0 times")
        app._say("          translate_psp: 0 hooked functions given full argument liveness")
        app._say("          translate_psp: wide fixpoint (2 rounds, 0 s): wide_rets 1706 (+1694); new rets: %08x"
                 % FIRST_FN)
    app._event(("warn", "compile: 6 parallel jobs instead of 12 - the free memory where the compiler runs allows "
                        "about 512 MB per job"))
    text = app.log.toPlainText()
    assert "new rets" not in text and "syscall rule" not in text and "9 more" in text
    assert "Compiling with 6 jobs" in text and "!" not in text


FIRST_FN = 5262408                    # a guest function address as the translator prints it (log folding test)


def pump(qapp, until=lambda: False, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        qapp.processEvents()
        if until():
            return True
        time.sleep(0.01)
    return False


def texts(widget):
    from PySide6.QtWidgets import QAbstractButton, QLabel
    return " ".join(w.text() for w in widget.findChildren(QLabel) + widget.findChildren(QAbstractButton))


def test_every_page_both_languages(gui):
    app, mod, qapp, _tmp, _mp = gui
    for lang in ("en", "ru"):
        app._set_lang(lang)
        for page in ("home", "disc", "output", "customize", "settings", "convert", "folders", "update"):
            app.show_page(page)
            pump(qapp, timeout=0.2)
            assert app.page == page
        app.show_page("home")
        t = texts(app.centralWidget())
        ru = i18n.tr("home.convert", "ru")
        assert ("Convert my disc" in t) == (lang == "en") and (ru in t) == (lang == "ru") and ru != "Convert my disc"
    app.show_page("settings")
    assert "Grafika" in texts(app.centralWidget())              # the Russian template's group names
    menus = [a.text() for a in app.menuBar().actions()]
    assert menus == [i18n.tr("menu.view", "ru")]


def test_interface_size_and_compact_fonts(gui):
    app, mod, qapp, _tmp, _mp = gui
    base = app._base_font.pointSizeF()
    assert qapp.font().pointSizeF() == base                     # Normal = the system's own size
    app._set_size("large")
    assert qapp.font().pointSizeF() == base + 2
    app._set_size("small")
    assert qapp.font().pointSizeF() == base - 1
    app._set_size("normal")
    app.show_page("convert")
    h1 = [w for w in app.centralWidget().findChildren(mod.QLabel) if w.property("kind") == "h1"][0]
    assert h1.font().pointSizeF() <= base + 2 and app.btn_go.font().pointSizeF() == base   # no huge buttons


def test_disc_page_states(gui, monkeypatch):
    app, mod, qapp, _tmp, _mp = gui
    import backend
    app.show_page("disc")
    assert "TEST-00001" in app.disc_status.text() and app.next_btn.isEnabled()
    monkeypatch.setattr(backend, "identify", lambda src: {"ok": False, "code": "version", "message": "v9 no",
                                                          "hint": "use v1"})
    app._identify_async(app.s["source"])
    pump(qapp, lambda: app.ident and not app.ident.get("busy"))
    app.show_page("disc")
    assert "not supported" in app.disc_status.text() and not app.next_btn.isEnabled()


def test_settings_reach_the_converter(gui, monkeypatch):
    app, mod, qapp, _tmp, _mp = gui
    app.show_page("settings")
    items = {i.key: i for i in app.template_items if hasattr(i, "key")}
    app._set_value(items["draw_distance"], "70")
    app._set_value(items["mirrors"], "1")
    monkeypatch.setattr(app, "ask_risk", lambda it: False)
    app._set_value(items["overclock"], "365")                     # risk declined: not changed
    assert app.overrides() == {"draw_distance": "70", "mirrors": "1"}
    monkeypatch.setattr(app, "ask_risk", lambda it: True)
    app._set_value(items["overclock"], "365")
    app.show_page("convert")
    kw = app.options_kw()
    want = {"draw_distance": "70", "mirrors": "1", "overclock": "365"}
    if "settings" in kw:                                   # the core writes them on the template's lines
        assert kw["settings"] == want and "cfg" not in kw
    else:                                                  # older core: appended lines
        assert sorted(kw["cfg"]) == sorted("%s=%s" % kv for kv in want.items())
    assert kw["lang"] == "en" and kw["work"] and kw["title"] == "PS2RC TEST-00001"   # neutral default title
    app._restore_settings()
    assert app.overrides() == {}


def test_convert_passes_exact_assets_and_shows_progress(gui, monkeypatch):
    app, mod, qapp, tmp, _mp = gui
    from ps2rc.convert import pipeline
    from ps2rc.pbp import placeholder, png
    seen = {}

    class FakeConv:
        def __init__(self, opts, progress):
            seen["opts"], self.p, self.result = opts, progress, {}

        def run(self):
            self.p.emit("step", 1, 14, "identify", "Identify the disc")
            self.result["estimate"] = {"steps": {"analysis": 100, "translate": 100, "compile": 50}}
            self.p.emit("pct", "identify", 1, 2, "x")
            self.p.emit("done", "identify", 0.1, False)
            self.p.emit("step", 8, 14, "compile", "Compile (psp-gcc)")
            time.sleep(0.3)
            self.p.emit("done", "compile", 0.3, True)
            return {"folder": str(tmp / "out" / "PS2RC_TEST")}
    monkeypatch.setattr(pipeline, "Conversion", FakeConv)
    pic = tmp / "back.png"
    pic.write_bytes(placeholder.pic1())
    app.project.set_file("pic1", str(pic))
    app.project.title = "  My   Game "
    app.show_page("convert")
    app._start_convert()
    assert pump(qapp, lambda: not app.running, timeout=20)
    o = seen["opts"]
    assert o.title == "My Game" and o.source == app.s["source"] and o.out == str(tmp / "out")
    assert png.info(open(o.icon0, "rb").read())["width"] == 144           # default icon, an exact PNG
    assert open(o.pic1, "rb").read() == placeholder.pic1()               # an exact PNG is kept byte for byte
    assert o.icon1 is None and o.snd0 is None and o.pic0 is None
    assert "PS2RC_TEST" in app.result_label.text() and app.result_label.property("kind") == "good"
    assert app.step_state["identify"] == "done" and app.step_state["compile"] == "kept"
    assert "eta_weights" in app.s


def test_errors_are_plain(gui, monkeypatch):
    app, mod, qapp, tmp, _mp = gui
    from ps2rc.convert import pipeline
    from ps2rc.convert.steps import ConvertError

    class Boom:
        def __init__(self, opts, progress):
            self.p, self.result = progress, {}

        def run(self):
            self.p.emit("step", 12, 14, "stage", "Copy")
            raise ConvertError("disk", "not enough free space on E:", "free some space", log="x.log")
    monkeypatch.setattr(pipeline, "Conversion", Boom)
    app._set_lang("ru")
    app.show_page("convert")
    app._start_convert()
    assert pump(qapp, lambda: not app.running, timeout=20)
    assert app.result_label.text().startswith(i18n.error_text("disk", "", "", "ru")[0])
    assert i18n.tr("err.what", "ru", "")[:6] in app.result_hint.text() and app.step_state["stage"] == "fail"
    assert app.btn_go.text() == i18n.tr("conv.again", "ru")


def test_check_and_update_pages(gui, monkeypatch):
    app, mod, qapp, tmp, _mp = gui
    import backend
    folder = tmp / "PSP" / "GAME" / "PS2RC_TEST"
    folder.mkdir(parents=True)
    app.selected_folder = str(folder)

    class Rep:
        ok, missing, corrupt, extra, user_changed, files = True, [], [], [], ["poc6.cfg"], 312
    monkeypatch.setattr(backend, "verify", lambda f, progress=None: Rep())
    app.show_page("folders")
    app._check()
    assert pump(qapp, lambda: "312" in app.chk_label.text())
    assert "1" in app.chk_hint.text()
    Rep.ok, Rep.corrupt = False, ["disc/a.img", "EBOOT.PBP"]
    app._check()
    assert pump(qapp, lambda: "2 file" in app.chk_label.text())
    assert not app.repair_row.isHidden()
    monkeypatch.setattr(backend, "plan_update", lambda f, src=None, opts=None: {"actions": [
        ("EBOOT.PBP", "rebuild", "made by an older converter"), ("disc/a.img", "copy", "damaged"),
        ("poc6.cfg", "merge_cfg", "new settings"), ("save.dat", "keep", "")]})
    app.s["source"] = None
    app.show_page("update")
    assert pump(qapp, lambda: app.plan_tv.topLevelItemCount() == 3)
    assert app.plan_needs_disc and not app.disc_row.isHidden() and app.btn_go.isEnabled()
    monkeypatch.setattr(backend, "verify", lambda f, progress=None: (_ for _ in ()).throw(
        backend.NotAvailable("this converter version cannot check folders yet")))
    app.show_page("folders")
    app._check()
    assert pump(qapp, lambda: "cannot check" in app.chk_label.text())


def test_choices_are_remembered(gui):
    app, mod, qapp, tmp, _mp = gui
    app.project.title = "Remember me"
    app.values = {"draw_distance": "55"}
    app.s["ui_size"] = "large"
    app._set_lang("ru")
    app.close()
    app2 = mod.App(settings_path=str(tmp / "appdata" / "gui.json"))
    try:
        assert app2.lang == "ru" and app2.project.title == "Remember me" and app2.values == {"draw_distance": "55"}
        assert app2.s["out_pc"] == str(tmp / "out") and app2.s["ui_size"] == "large"
    finally:
        app2.s["ui_size"] = "normal"
        app2._apply_size()
        app2.close()


def test_preview_renders_and_shot(gui):
    app, mod, qapp, tmp, _mp = gui
    pytest.importorskip("PIL.Image")
    app.show_page("customize")
    assert pump(qapp, lambda: app.built is not None and app.preview.mock is not None, timeout=20)
    pump(qapp, timeout=1.2)
    img = app.preview.grab().toImage()
    colors = {img.pixel(x, y) for x in range(0, img.width(), 7) for y in range(0, img.height(), 7)}
    assert len(colors) > 50                                # background gradient + icon, not an empty widget
    app.grab().save(str(tmp / "customize.png"))
    assert os.path.getsize(tmp / "customize.png") > 10000


def test_gallery_from_disc_art(gui, monkeypatch, pillow):
    app, mod, qapp, tmp, _mp = gui
    from ps2rc.xmb import disc_art
    from qt_widgets import GalleryDialog
    arts = []
    for i, cat in enumerate(("loading", "loading", "title", "background")):
        p = tmp / ("a%d.png" % i)
        pillow.new("RGB", (640, 480), (60 * i, 100, 200)).save(str(p))
        arts.append(disc_art.Art("pic%d" % i, cat, str(p), 640, 480, "X.BIN"))
    d = GalleryDialog(app, arts, "en")
    assert d.list.count() == 4 and d.filter.count() == 4         # All + 3 categories present
    d.filter.setCurrentIndex(1)
    assert d.list.count() == 2
    d._ok()
    assert d.chosen.name == "pic0"
    # one click: the pack's suggestion goes into the slots
    monkeypatch.setattr(app, "_pack_dir", lambda: str(tmp))
    (tmp / "xmb_artwork.py").write_text("SUGGEST = {'pic1': ('pic2', None), 'icon0': ('pic0', (0, 0, 288, 160))}\n"
                                        "def artwork(disc, out_dir, log):\n    return []\n")
    app.arts = arts
    app.show_page("customize")
    app._use_game_art()
    assert app.project.slots["pic1"]["path"] == arts[2].path and app.project.slots["icon0"]["box"] == [0, 0, 288, 160]


def test_step_icons_are_painted_not_glyphs(gui):
    """the step list must not depend on symbol glyphs of the UI font (an empty box on some Windows systems)"""
    app, mod, qapp, _tmp, _mp = gui
    from qt_widgets import state_pixmap, STATE_COLORS
    app.show_page("convert")
    for i, (name, st) in enumerate(zip(list(app.step_marks)[:4], ("done", "kept", "run", "fail"))):
        app._mark_step(name, st)
        icon, lab = app.step_marks[name]
        assert not icon.pixmap().isNull() and all(ord(ch) < 0x2000 for ch in lab.text() if not ch.isalpha())
    for dpr in (1.0, 1.5, 2.0):
        for st in ("todo", "run", "done", "fail"):
            pm = state_pixmap(st, 16, dpr)
            img = pm.toImage()
            assert img.width() == round(16 * dpr) and abs(pm.devicePixelRatio() - dpr) < 1e-6
            if st in ("done", "fail"):                             # a filled disc of the state's colour + white mark
                edge = img.pixelColor(int(img.width() * 0.5), int(img.height() * 0.2)).name()
                assert edge == STATE_COLORS[st]
    src = open(mod.__file__, encoding="utf-8").read()
    assert all(ord(ch) < 128 for ch in src), "symbol / non-ASCII characters in the window's code"


def test_language_follows_the_system_until_chosen(gui, monkeypatch):
    app, mod, qapp, tmp, _mp = gui
    path = str(tmp / "appdata" / "fresh.json")
    monkeypatch.setattr(mod, "system_lang", lambda: "ru")
    a = mod.App(settings_path=path)                               # first start on a Russian Windows
    assert a.lang == "ru" and a.options_kw()["lang"] == "ru"      # the player cfg follows too
    entries = a.lang_entries()
    assert entries[0][0] == "system" and i18n.tr("lang.system", "ru") in entries[0][1]
    a._set_lang("en")                                             # the player's choice wins and is kept
    assert a.lang == "en" and a.s["lang_choice"] == "en"
    a.close()
    b = mod.App(settings_path=path)
    assert b.lang == "en"
    b._set_lang("system")                                         # back to automatic
    assert b.lang == "ru" and b.s["lang_choice"] == "system"
    b.close()
    monkeypatch.setattr(mod, "system_lang", lambda: "en")
    c = mod.App(settings_path=str(tmp / "appdata" / "fresh2.json"))
    assert c.lang == "en"
    c.close()


def test_system_lang_maps_only_russian(monkeypatch):
    pytest.importorskip("PySide6")
    import ps2rc_gui
    from PySide6.QtCore import QLocale
    for tags, want in ((["ru-RU", "en-US"], "ru"), (["uk-UA"], "en"), (["de-DE"], "en"), (["ja-JP"], "en")):
        class Loc:
            def uiLanguages(self, _t=tags):
                return _t
        monkeypatch.setattr(QLocale, "system", staticmethod(lambda: Loc()))
        assert ps2rc_gui.system_lang() == want, tags


def test_gallery_opens_at_once_and_fills_in(gui, pillow):
    app, mod, qapp, tmp, _mp = gui
    from ps2rc.xmb import disc_art
    from qt_busy import GalleryDialog
    d = GalleryDialog(app, [], "en", busy=True)
    assert d.list.count() == 0 and not d.busy.spinner.isHidden()
    for i in range(3):
        p = tmp / ("g%d.png" % i)
        pillow.new("RGB", (640, 480), (80 * i, 90, 160)).save(str(p))
        d.add_art(disc_art.Art("g%d" % i, "loading", str(p), 640, 480))
    assert d.list.count() == 3 and "3" in d.busy.text.text()
    assert pump(qapp, lambda: len(d._icons) == 3, timeout=10)          # thumbnails made in a worker
    d.finish_reading()
    assert d.busy.spinner.isHidden() and "3" in d.busy.text.text()
    d.close()


def test_music_dialog_lists_filters_and_searches(gui):
    app, mod, qapp, tmp, _mp = gui
    from ps2rc.xmb.disc_music import Track
    from qt_busy import MusicDialog
    d = MusicDialog(app, "en")
    d.progress(25, 100)
    assert d.busy.bar.maximum() == 100 and d.busy.bar.value() == 25
    d.set_tracks([Track("AA 1", "radio", 200, 1), Track("CUT 1", "cutscene", 80, 2), Track("AA 2", "radio", 45, 3)])
    assert d.tree.topLevelItemCount() == 3 and d.tree.topLevelItem(0).text(2) == "3:20"
    d.filter.setCurrentIndex(d.filter.findData("cutscene"))
    assert d.tree.topLevelItemCount() == 1
    d.filter.setCurrentIndex(0)
    d.search.setText("aa")
    assert d.tree.topLevelItemCount() == 2
    d._ok()
    assert d.chosen.name == "AA 1"


def test_log_always_shown_folded_and_saved(gui):
    app, mod, qapp, tmp, _mp = gui
    app.show_page("convert")
    assert app.log.isVisible() or not app.log.isHidden()
    app.log_lines, app.log_file = [], None
    app._render_log()
    app._say("[4/14] Translate the program to C")
    for i in range(50):
        app._say("main:%08x 33 insns 2-callee-saved" % (FIRST_FN + 4 * i))
    app._say("translate: 157 units")
    text = app.log.toPlainText()
    assert "50 more lines" in text and "main:%08x" % FIRST_FN not in text and "157 units" in text
    app.s["log_verbose"] = True
    app._render_log()
    assert "main:%08x" % FIRST_FN in app.log.toPlainText()
    app.s["log_verbose"] = False
    log = open(app.log_file, encoding="utf-8").read()
    assert log.count("insns") == 50                                    # the file has every line


def test_folders_are_found_in_the_background(gui, monkeypatch):
    app, mod, qapp, tmp, _mp = gui
    import psp_drives
    import time as _t
    slow = {"called": False}

    def drives():
        _t.sleep(0.5)
        slow["called"] = True
        return []
    monkeypatch.setattr(psp_drives, "drives", drives)
    t0 = _t.perf_counter()
    app.show_page("folders")
    assert _t.perf_counter() - t0 < 0.4 and not app.scan_busy.spinner.isHidden()   # the page is there at once
    assert pump(qapp, lambda: slow["called"] and app.scan_busy.isHidden(), timeout=10)
    assert app.found_label.text() == i18n.tr("chk.none", "en")


def test_convert_refuses_folders_the_toolchain_cannot_use(gui):
    app, ps2rc_gui, qapp, tmp_path, monkeypatch = gui
    from ps2rc.xmb import media
    from PySide6.QtWidgets import QMessageBox
    monkeypatch.setattr(media, "ansi_safe", lambda p: "out" not in os.path.basename(p))
    shown = []
    monkeypatch.setattr(QMessageBox, "critical", lambda *a: shown.append(a[2]))
    started = []
    monkeypatch.setattr(app, "_run_conversion", lambda *a, **k: started.append(a))
    app._start_convert()
    assert not started and shown and str(tmp_path / "out") in shown[0] and "English letters" in shown[0]
    monkeypatch.setattr(media, "ansi_safe", lambda p: True)
    monkeypatch.setattr(media, "ascii_path", lambda p: False)       # the converter folder itself: ASCII only
    app._start_convert()
    assert not started and len(shown) == 2 and os.path.dirname(ps2rc_gui.ROOT) in shown[1]
    monkeypatch.setattr(media, "ascii_path", lambda p: True)
    app._start_convert()
    assert started


def test_work_and_temp_fall_back_to_usable_paths(qapp, tmp_path, monkeypatch):
    """a user folder with letters outside the code page: work next to the output, TEMP inside the work folder"""
    import ps2rc_gui
    from ps2rc.xmb import media
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "bad_user" / "Local"))
    monkeypatch.setenv("TEMP", str(tmp_path / "bad_user" / "Temp"))
    monkeypatch.setenv("TMP", str(tmp_path / "bad_user" / "Temp"))
    monkeypatch.setattr(media, "ansi_safe", lambda p: "bad_user" not in p)
    monkeypatch.setattr(media, "ascii_path", lambda p: "bad_user" not in p)
    out = str(tmp_path / "out")
    w = ps2rc_gui.work_dir(out)
    assert w == os.path.join(out, "_ps2rc_work")
    t = ps2rc_gui.safe_temp(w)
    assert t == os.path.join(w, "tmp") and os.environ["TEMP"] == t == os.environ["TMP"] and os.path.isdir(t)
    monkeypatch.setenv("TEMP", str(tmp_path / "bad_user" / "Temp"))
    monkeypatch.setenv("TMP", str(tmp_path / "bad_user" / "Temp"))
    monkeypatch.setenv("ProgramData", str(tmp_path / "pd"))
    assert ps2rc_gui.safe_temp(str(tmp_path / "bad_user" / "w")) == str(tmp_path / "pd" / "PS2RC" / "tmp")
    monkeypatch.setattr(media, "ansi_safe", lambda p: True)
    monkeypatch.setattr(media, "ascii_path", lambda p: True)
    assert ps2rc_gui.work_dir(out) == ps2rc_gui.local_dir("work") and ps2rc_gui.safe_temp(w) is None


def test_music_quality_choice(gui, fake_encoder):
    """three choices, ATRAC3 132k by default; the length slider and the budget follow; remembered on its own"""
    app, ps2rc_gui, qapp, tmp_path, monkeypatch = gui
    from ps2rc.xmb import sound
    from ps2rc.pbp.spec import MEDIA_BUDGET
    import wave
    wav = tmp_path / "song.wav"
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"\0" * 4 * 44100 * 70)
    app.show_page("customize")
    app._set_file("snd0", str(wav))
    app.show_page("customize")
    ui = app.slot_ui["snd0"]
    combo = ui["codec"]
    assert combo.count() == 3 and combo.currentData() == "atrac3" == app.project.slots["snd0"]["codec"]
    assert "~30" in combo.currentText() and "ATRAC3 132k" in combo.currentText()
    assert ui["len"].maximum() == int(sound.max_seconds(MEDIA_BUDGET, "atrac3") * 10)
    assert not ui["codec_note"].isHidden()                         # the ATRAC3 note
    combo.setCurrentIndex(combo.findData("atrac3_lp4"))
    assert ui["len"].maximum() == int(sound.max_seconds(MEDIA_BUDGET, "atrac3_lp4") * 10) > 590
    ui["len"].setValue(500)
    app._budget_live()
    assert "of 500 KB" in app.budget_label.text() and app.s["snd0_codec"] == "atrac3_lp4"
    combo.setCurrentIndex(combo.findData("atrac3plus"))
    assert ui["len"].maximum() == 110 and ui["codec_note"].isHidden()
    app._set_file("snd0", str(wav))                                   # a new file keeps the choice
    assert app.project.slots["snd0"]["codec"] == "atrac3plus"
    app._save()
    again = ps2rc_gui.App(settings_path=app.settings_path)
    assert again.project.slots["snd0"]["codec"] == "atrac3plus"
    again.close()
