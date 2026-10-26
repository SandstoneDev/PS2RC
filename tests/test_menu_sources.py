"""The in-game settings menu needs its C++ library in the tree: a release (or an export of the developer tree) without
lib/psp_imgui builds games without the menu, and the converter only warns. These tests catch such a tree."""
import os
import subprocess
import sys
import types

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

# every file the menu build takes from lib/ (ps2rc/convert/build.py _cpp_files, mk/build.sh) + the licence notes
MENU_LIB = ["lib/psp_imgui/psp_imgui.cpp", "lib/psp_imgui/psp_imgui.h", "lib/psp_imgui/pimgui_imconfig.h",
            "lib/psp_imgui/pimgui_font13.h", "lib/psp_imgui/LICENSE.md", "lib/psp_imgui/README.md",
            "lib/psp_imgui/third_party/imgui/LICENSE.txt", "lib/psp_imgui/third_party/imgui/imgui.cpp",
            "lib/psp_imgui/third_party/imgui/imgui.h", "lib/psp_imgui/third_party/imgui/imgui_draw.cpp",
            "lib/psp_imgui/third_party/imgui/imgui_tables.cpp", "lib/psp_imgui/third_party/imgui/imgui_widgets.cpp",
            "lib/psp_imgui/third_party/imgui/imgui_internal.h", "lib/psp_imgui/third_party/imgui/imconfig.h",
            "lib/psp_imgui/third_party/imgui/imstb_rectpack.h", "lib/psp_imgui/third_party/imgui/imstb_textedit.h",
            "lib/psp_imgui/third_party/imgui/imstb_truetype.h"]


def missing(tree):
    return [f for f in MENU_LIB if not os.path.isfile(os.path.join(tree, f))]


def test_menu_library_in_tree():
    if not os.path.isfile(os.path.join(ROOT, "hle", "psp", "hle_menu.cpp")):
        pytest.skip("no in-game menu in this tree")
    assert missing(ROOT) == []


EXPORT = os.path.join(ROOT, "tools", "release", "export_public.py")


@pytest.mark.skipif(not os.path.isfile(EXPORT), reason="release tooling is not part of this checkout")
def test_export_keeps_menu_library(tmp_path):
    out = tmp_path / "export"
    r = subprocess.run([sys.executable, EXPORT, str(out), "--manifest", str(tmp_path / "m.json")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert missing(str(out)) == []


def test_converter_warns_without_menu_library(tmp_path):
    from ps2rc.convert.build import Builder
    hd = tmp_path / "hle"
    hd.mkdir()
    (hd / "hle_menu_psp.c").write_text("/* glue */\n")
    said = []
    fake = types.SimpleNamespace(bdir=str(tmp_path), progress=types.SimpleNamespace(warn=said.append), tc=None)
    Builder._menu_check(fake)
    assert said and "lib/psp_imgui" in said[0] and "NO in-game settings menu" in said[0]
    assert not (hd / "hle_menu_psp.c").exists()        # the glue left the build: no undefined hle_menu_ui at the link
    said.clear()
    (hd / "hle_menu_psp.c").write_text("/* glue */\n")
    (hd / "psp_imgui.cpp").write_text("// lib\n")
    fake._have_cxx = True
    Builder._menu_check(fake)
    assert said == []
