"""The EBOOT editor window (gui/eboot_editor.py) offscreen: open, title, 16:9 / square icon, crop path,
checks + fix, save with backup, texts in both languages, the main window's menu entry. Every PBP is generated here."""
import os
import struct

import pytest

pytest.importorskip("PySide6")
pytest.importorskip("PIL.Image")

import i18n  # noqa: E402
from ps2rc.pbp import png  # noqa: E402
from ps2rc.pbp.pbp import Pbp  # noqa: E402
from ps2rc.pbp.sfo import Sfo, make as make_sfo  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    import ps2rc_gui
    return ps2rc_gui.make_app([])


def flat(w, h, mode="RGB", color=(30, 120, 200)):
    ch = 3 if mode == "RGB" else 4
    return png.encode(w, h, mode, bytes(color + ((255,) if ch == 4 else ())) * (w * h))


def homebrew(tmp_path):
    p = Pbp()
    p["PARAM.SFO"] = make_sfo("Editor Test").build()
    p["ICON0.PNG"] = flat(144, 80)
    p["DATA.PSP"] = b"\x7fELF" + os.urandom(5000)
    path = tmp_path / "EBOOT.PBP"
    path.write_bytes(p.build())
    return path


def ps1(tmp_path):
    s = Sfo()
    for k, v in (("BOOTABLE", 1), ("CATEGORY", "ME"), ("DISC_ID", "HBRW00002"), ("DISC_VERSION", "1.00"),
                 ("PARENTAL_LEVEL", 1), ("PSP_SYSTEM_VER", "3.01"), ("REGION", 0x8000)):
        s.set(k, v)
    s.set("TITLE", "PS1 Editor Test", reserved=128)
    sfo, icon = s.build(), flat(80, 80)
    mod = bytearray(os.urandom(4000))
    mod[0:4] = b"~PSP"
    struct.pack_into("<I", mod, 0x2C, len(mod))
    pre = 40 + len(sfo) + len(icon)
    psar_off = 0x10000
    offs = [40, 40 + len(sfo)] + [pre] * 5 + [psar_off]
    psar = b"PSISOIMG0000" + os.urandom(20000)
    data = b"\0PBP" + struct.pack("<I", 0x10000) + struct.pack("<8I", *offs) + sfo + icon + bytes(mod) + \
        b"\0" * (psar_off - pre - len(mod)) + psar
    path = tmp_path / "EBOOT.PBP"
    path.write_bytes(data)
    return path, psar


@pytest.fixture
def editor(qapp):
    import eboot_editor
    d = eboot_editor.EbootEditor(None, "en")
    yield d, eboot_editor
    d.e = None                      # no unsaved-changes question at close
    d.close()


def test_open_title_icon_shapes_and_save(editor, tmp_path):
    d, _m = editor
    path, psar = ps1(tmp_path)
    assert d.open(str(path))
    assert d.e.kind == "ps1" and d.shape_now() == "square"
    assert d.title_edit.text() == "PS1 Editor Test"
    d.title_edit.setText("Renamed PS1")
    d._title_changed("Renamed PS1")
    pic = tmp_path / "wide.png"
    pic.write_bytes(flat(320, 180, color=(200, 50, 50)))
    d.shape.setCurrentIndex(0)                           # 16:9 wide on a PS1 eboot: allowed with a warning
    assert d.load_file("ICON0.PNG", str(pic))
    assert png.info(d.e.files["ICON0.PNG"])["width"] == 144
    assert "PS1 emulator" in d.status.text() and "PS1 emulator" in d.checks.toPlainText()
    assert d.fix_btn.isEnabled()
    d.shape.setCurrentIndex(1)
    d._shape_changed()                                   # back to square, from the chosen file
    assert png.info(d.e.files["ICON0.PNG"])["width"] == 80
    assert d.load_file("ICON0.PNG", str(pic), box=(0, 0, 180, 180))     # crop box path
    assert d.save(sync=True)
    p = Pbp.read(path)
    assert Sfo.parse(p["PARAM.SFO"]).get("TITLE") == "Renamed PS1" and p["DATA.PSAR"] == psar
    assert os.path.exists(str(path) + ".bak") and "kept" in d.status.text()
    assert d.e.changed() == []                           # reopened after the save


def test_fix_and_errors(editor, tmp_path):
    d, _m = editor
    path, _psar = ps1(tmp_path)
    d.open(str(path))
    d.e.set_file("ICON0.PNG", flat(144, 80))
    d.refresh()
    d._fix()
    assert png.info(d.e.files["ICON0.PNG"])["width"] == 80 and d.shape_now() == "square"
    bad = tmp_path / "bad.png"
    bad.write_bytes(b"not a picture")
    assert not d.load_file("PIC1.PNG", str(bad))
    assert "Not done" in d.status.text()
    d._title_changed("two\nlines")
    assert "Not done" in d.status.text()


def test_homebrew_wide_default_and_no_change_save(editor, tmp_path):
    d, _m = editor
    path = homebrew(tmp_path)
    before = path.read_bytes()
    d.open(str(path))
    assert d.shape_now() == "wide" and d.e.kind == "homebrew"
    assert d.save(sync=True) and path.read_bytes() == before
    assert not os.path.exists(str(path) + ".bak")
    snd = tmp_path / "x.at3"
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pbp"))
    from pbp_helpers import make_at3
    snd.write_bytes(make_at3())
    assert d.load_file("SND0.AT3", str(snd), sync=True)
    assert d.e.files["SND0.AT3"] == snd.read_bytes()
    out = tmp_path / "copy.pbp"
    assert d.save(str(out), sync=True) and Pbp.read(out)["SND0.AT3"] == snd.read_bytes()
    assert path.read_bytes() == before


def test_texts_both_languages():
    import eboot_editor
    ru = i18n.TEXTS["ru"]["_eboot_editor"]
    assert set(ru) == set(eboot_editor.T_EN)
    for k, en in eboot_editor.T_EN.items():
        assert en.count("%") == ru[k].count("%"), k
    assert eboot_editor.tr("eb.save", "ru") != eboot_editor.tr("eb.save", "en")
    src = open(eboot_editor.__file__, encoding="utf-8").read()
    assert all(ord(ch) < 128 for ch in src)


def test_main_window_menu_has_the_editor(qapp, tmp_path, monkeypatch):
    monkeypatch.setenv("APPDATA", str(tmp_path / "appdata"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    import ps2rc_gui
    app = ps2rc_gui.App(settings_path=str(tmp_path / "gui.json"))
    try:
        names = [a.text() for m in app.menuBar().actions() if m.menu() for a in m.menu().actions()]
        assert "EBOOT editor..." in names
        tools = [m for m in app.menuBar().actions() if m.text() == "Tools"][0]
        tools.menu().actions()[0].trigger()
        import eboot_editor
        opened = [w for w in app.findChildren(eboot_editor.EbootEditor)]
        assert opened
        for w in opened:
            w.close()
    finally:
        app.close()
