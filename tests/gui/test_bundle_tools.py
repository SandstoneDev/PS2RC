"""The app bundle's optional programs (ffmpeg, atracdenc): copied with their licence, a source note, and found by
the window there; refused without a licence file."""
import os

import pytest

import build_app


def _tool(tmp_path, name, licence=True):
    d = tmp_path / ("src_" + name)
    d.mkdir()
    (d / (name + ".exe")).write_bytes(b"MZ")
    (d / "helper.dll").write_bytes(b"MZ")
    (d / "notes.bin").write_bytes(b"x")
    if licence:
        (d / "COPYING.LGPL").write_text("LGPL text")
    return str(d)


def test_tool_copied_with_licence_and_source_note(tmp_path, monkeypatch):
    out, lic = tmp_path / "PS2RC_Converter", tmp_path / "PS2RC_Converter" / "licenses"
    lic.mkdir(parents=True)
    n = build_app.add_tool("atracdenc", _tool(tmp_path, "atracdenc"), str(out), str(lic))
    t = out / "tools" / "atracdenc"
    assert sorted(os.listdir(t)) == ["COPYING.LGPL", "atracdenc.exe", "helper.dll"] and n == 3
    assert (lic / "atracdenc" / "COPYING.LGPL").read_text() == "LGPL text"
    note = (lic / "atracdenc" / "SOURCE.txt").read_text()
    assert "github.com/dcherednik/atracdenc" in note and "separate program" in note
    # the window looks there: <bundle>/tools/<name>/ next to <bundle>/app
    from ps2rc.xmb import media
    monkeypatch.setattr(media, "REPO", str(out / "app"))
    monkeypatch.delenv("PS2RC_ATRACDENC", raising=False)
    found = media.find_tool("atracdenc", None, "PS2RC_ATRACDENC")
    if os.name == "nt":
        assert found == str(t / "atracdenc.exe")


def test_tool_without_licence_refused(tmp_path):
    out = tmp_path / "B"
    (out / "licenses").mkdir(parents=True)
    with pytest.raises(SystemExit, match="licence"):
        build_app.add_tool("ffmpeg", _tool(tmp_path, "ffmpeg", licence=False), str(out), str(out / "licenses"))
    with pytest.raises(SystemExit, match="no ffmpeg"):
        build_app.add_tool("ffmpeg", str(tmp_path), str(out), str(out / "licenses"))


def test_app_icon_variants_and_ico(tmp_path):
    pytest.importorskip("PySide6")
    pytest.importorskip("PIL")
    import app_icon
    from PIL import Image
    app_icon._app()
    imgs = {v: app_icon.paint(v, 64) for v in (1, 2, 3)}
    assert imgs[1] != imgs[2] and imgs[2] != imgs[3]
    for v in (1, 2, 3):
        corner = imgs[v].pixelColor(0, 0).alpha()
        middle = imgs[v].pixelColor(32, 32).alpha()
        assert corner == 0 and middle == 255                    # rounded tile on a transparent ground
    ico = app_icon.write_ico(str(tmp_path / "x.ico"), 1)
    assert sorted(Image.open(ico).info["sizes"]) == [(s, s) for s in app_icon.SIZES]
    sheet = app_icon.write_sheet(str(tmp_path / "sheet.png"))
    assert Image.open(sheet).size[0] > 3 * 256


@pytest.mark.skipif(os.name != "nt", reason="Windows exe resources")
def test_launcher_gets_the_icon(tmp_path):
    pytest.importorskip("PySide6")
    cc = build_app.find_cc(None)
    if not cc or not os.path.isfile(os.path.join(os.path.dirname(cc), "windres.exe")):
        pytest.skip("no MinGW gcc + windres")
    ico = build_app.make_icon(str(tmp_path))
    exe = str(tmp_path / "PS2RC Converter.exe")
    assert ico and build_app.build_launcher(cc, exe, ico)
    import ctypes
    n = ctypes.windll.shell32.ExtractIconExW(exe, -1, None, None, 0)
    assert n >= 1                                               # the exe carries its own icon


# ------------------------------------------------------------------------------------- clean-Windows dependencies
def test_system_dll_classes():
    import bundle_deps as bd
    assert bd.classify_system("api-ms-win-crt-runtime-l1-1-0.dll") == "apiset"
    assert bd.classify_system("MSVCP140.dll") == "vcruntime"
    assert bd.classify_system("vcruntime140_1.dll") == "vcruntime"
    assert bd.classify_system("d3dx9_43.dll") in ("other", "missing")         # a DirectX redistributable
    assert bd.classify_system("no_such_part_ps2rc.dll") == "missing"
    if os.name == "nt":
        assert bd.classify_system("kernel32.dll") == "os"
        assert bd.classify_system("propsys.dll") == "os"
        assert bd.classify_system("icuuc.dll") == "os"
        assert bd.classify_system("mfplat.dll") == "media"


@pytest.mark.skipif(os.name != "nt", reason="Windows PE files")
def test_pe_imports_and_folder_check(tmp_path):
    """a Qt module copied alone into tools/: its Qt DLL is missing and the C++ runtime is not part of Windows; the
    runtime fix copies the runtime (and what it needs in turn), the Qt DLL stays reported"""
    import shutil
    import bundle_deps as bd
    PySide6 = pytest.importorskip("PySide6")
    qdir = os.path.dirname(PySide6.__file__)
    imp, _delay = bd.pe_imports(os.path.join(qdir, "Qt6Widgets.dll"))
    assert "qt6core.dll" in imp and "msvcp140.dll" in imp
    assert bd.pe_imports(__file__) == ([], [])
    root = tmp_path / "PS2RC_Converter"
    tool = root / "tools" / "x"
    tool.mkdir(parents=True)
    shutil.copy2(os.path.join(qdir, "Qt6Widgets.dll"), str(tool / "x.dll"))
    r = bd.check(str(root))
    probs = {(d, k) for _f, d, k in r["problems"]}
    assert ("qt6core.dll", "missing") in probs and ("msvcp140.dll", "vcruntime") in probs
    copied = bd.vc_runtime_fix(str(root), qdir, [str(tool)])
    assert {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"} <= {d.lower() for _f, d in copied}
    r = bd.check(str(root))
    assert [k for _f, _d, k in r["problems"]] and all(not k.startswith("vcruntime") for _f, _d, k in r["problems"])
    # python's own folders are searched for its modules: a module in DLLs/ finds python314.dll next to python.exe
    assert os.path.join(str(root), "python") in bd.search_dirs(str(root), os.path.join(str(root), "python", "DLLs",
                                                                                          "a.pyd"))


@pytest.mark.skipif(os.name != "nt", reason="Windows exit codes")
def test_program_that_cannot_start_is_explained():
    import sys
    from ps2rc.pbp.spec import PbpError
    from ps2rc.xmb import media
    not_found = 3221225781                                         # STATUS_DLL_NOT_FOUND
    assert "DLL" in media.windows_start_failure(not_found)
    assert media.windows_start_failure(not_found - 2 ** 32) == media.windows_start_failure(not_found)
    assert media.windows_start_failure(1) is None
    with pytest.raises(PbpError, match="could not start: a DLL it needs is missing"):
        media.run([sys.executable, "-c", "import os; os._exit(-1073741515)"], what="atracdenc")


def test_paths_the_toolchain_can_use():
    from ps2rc.xmb import media
    assert media.ansi_safe(os.path.join("PS2RC", "out folder")) and media.ascii_path("PS2RC out")
    assert not media.ascii_path("\u041f\u0430\u043f\u043a\u0430")     # Cyrillic: never for the toolchain's own folder
    if os.name == "nt":
        assert not media.ansi_safe("\u0d9a\u0dbb")         # Sinhala: in no Windows ANSI code page


def test_music_off_on_windows_without_media_foundation(monkeypatch, tmp_path):
    from ps2rc.xmb import sound
    exe = tmp_path / "atracdenc.exe"
    exe.write_bytes(b"MZ")
    monkeypatch.setenv("PS2RC_ATRACDENC", str(exe))
    monkeypatch.setattr(sound, "windows_media_missing", lambda: True)
    p, msg = sound.status()
    assert p is None and "Media Feature Pack" in msg and sound.MEDIA_FEATURE_PACK in msg
    monkeypatch.setattr(sound, "windows_media_missing", lambda: False)
    assert sound.status()[0] == str(exe)


def test_default_icon_is_the_owners_choice():
    import app_icon
    assert app_icon.DEFAULT == 2                     # variant 2, the "PS2RC" mark
