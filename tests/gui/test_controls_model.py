"""The controls page's model (gui/controls_model.py, no Qt): it resolves the cfg keys like the runtime
(hle/psp/hle_padmap.c), every edit keeps the mapping free of conflicts, and it writes only values that differ from the
chosen layout. The last test compiles the runtime's mapping on the host (WSL gcc) and checks that the values the page
writes give exactly the mapping the page shows, with no note."""
import os
import shutil
import subprocess
import zlib

import pytest

import controls_model as cm
from ps2rc import config

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_LINE = ("controls: modifier=select select_to_game=always map_l2=select+l map_r2=select+r "
                "map_l3=select+triangle map_r3=select+circle map_rstick=select+dpad map_l1=l map_r1=r "
                "map_select=select map_start=start")


def test_built_in_layouts():
    lay = cm.Layout({})
    assert lay.line() == DEFAULT_LINE and lay.to_values() == {} and not lay.has_car and lay.car_line() == ""
    assert lay.button_rows(0)["cross"] == ("cross", "") and lay.button_rows(0)["l"] == ("l1", "")
    assert ("select+l", "l2", "") in lay.combo_rows(0)
    v = cm.Layout({"controls_preset": "stories"})
    assert v.to_values() == {"controls_preset": "stories"} and v.has_car
    assert v.button_rows(0)["left"] == ("l2", "") and v.button_rows(1)["left"] == ("down", "")
    assert v.button_rows(0)["l"] == ("l1", "hold")
    assert cm.fmt_value("rstick", v.layers[0]["rstick"]) == "l+analog"
    assert cm.fmt_value("rstick", v.layers[1]["rstick"]) == "l+dpad"
    assert ("l+analog_down", "l2", "") in v.combo_rows(1) and ("l+analog_down", "r2", "") in v.combo_rows(1)


def test_old_preset_name_is_read_as_stories():
    lay = cm.Layout({"controls_preset": "VCS"})          # an earlier name, accepted, written as the new one
    assert lay.preset == "stories" and lay.to_values() == {"controls_preset": "stories"}
    assert "vcs" not in cm.PRESETS


def test_swap_and_alternatives():
    lay = cm.Layout({})
    lay.set_button(0, "cross", "square")                   # Cross now gives Square: PS2 Cross has no button left
    assert lay.to_values() == {"map_cross": "none", "map_square": "square/cross"}
    lay.set_button(0, "square", "cross")                   # ... and Square gives Cross: a clean swap
    assert lay.to_values() == {"map_cross": "square", "map_square": "cross"}
    assert lay.button_rows(0)["cross"] == ("square", "") and lay.button_rows(1)["cross"] == ("square", "")
    lay.set_button(0, "l", "l1", "hold")
    assert lay.to_values()["map_l1"] == "l+hold"
    lay.set_button(0, "start", None)
    assert lay.to_values()["map_start"] == "none"


def test_combinations_and_sticks_take_their_buttons():
    lay = cm.Layout({})
    moved = lay.set_combo(0, None, "l+r", "l2")            # a second way to L2
    assert lay.to_values() == {"map_l2": "select+l/l+r"} and not moved
    moved = lay.set_combo(0, None, "l+r", "r3")            # the same buttons for R3: L2 gives them up
    assert lay.to_values() == {"map_r3": "select+circle/l+r"} and moved == ["l2"]
    moved = lay.set_stick(0, "rstick", "l+analog")
    assert lay.to_values()["map_rstick"] == "l+analog"
    with pytest.raises(ValueError, match="L\\+R\\+SELECT"):
        lay.set_combo(0, None, "l+r+select", "l3")
    with pytest.raises(ValueError, match="needs a button"):
        lay.set_combo(0, None, "analog_left", "l3")
    old = ("l+r", "r3", "")
    lay.set_combo(0, old, "", None)                        # removed
    assert "map_r3" not in lay.to_values()


def test_vehicle_layer():
    v = cm.Layout({"controls_preset": "stories"})
    v.set_button(0, "cross", "square")                     # equal on foot and in a vehicle: follows
    assert v.to_values() == {"controls_preset": "stories", "map_cross": "none", "map_square": "square/cross"}
    v.set_button(0, "left", "l3")                          # differs in a vehicle: on foot only
    vals = v.to_values()
    assert vals["map_l2"] == "none" and vals["map_l3"] == "down/left" and vals["car_l2"] == \
        "l+analog_left/l+analog_down" and vals["car_l3"] == "down"
    d = cm.Layout({})
    d.set_button(1, "up", "r3")                            # the default layout gets a vehicle layer of its own
    assert d.has_car and d.to_values() == {"car_r3": "select+circle/up", "car_up": "none"}
    d.copy_foot_to_vehicle()
    assert d.to_values() == {} and not d.has_car


def test_car_foot_means_not_set():
    """car_<input>=foot (the in-game menu's first choice, until Save comments the line out) = the key not set, as in
    hle_padmap.c"""
    assert cm.Layout({"car_l2": "foot", "car_cross": " Foot "}).to_values() == {}
    assert not cm.Layout({"car_l2": "foot"}).has_car
    st = cm.Layout({"controls_preset": "stories"})
    assert cm.Layout({"controls_preset": "stories", "car_l2": "foot"}).car_line() == st.car_line()
    assert "car_l2=l+r" in cm.Layout({"car_l2": "l+r"}).car_line()


def test_presets_and_restore():
    lay = cm.Layout({"map_l2": "l+r", "select_to_game": "tap", "select_up": "r3"})
    lay.set_preset("stories")
    assert lay.to_values() == {"controls_preset": "stories", "select_to_game": "tap", "select_up": "r3"}
    lay.set_button(0, "triangle", "circle")
    lay.restore()
    assert lay.to_values() == {"controls_preset": "stories", "select_to_game": "tap", "select_up": "r3"}
    lay.set_preset("default")
    lay.set_select_tap(False)
    assert lay.to_values() == {"select_up": "r3"}


def test_bad_file_values_fall_back_like_the_runtime():
    lay = cm.Layout({"map_l2": "foo", "map_r2": "r", "car_l3": "select+home"})
    assert any(n.startswith("map_l2=foo: unknown button name") for n in lay.notes)
    assert "r2: same buttons as r1" in lay.notes
    assert cm.fmt_value("l2", lay.layers[0]["l2"]) == "select+l"


def test_texts_are_the_runtime_s():
    c_src = open(os.path.join(ROOT, "hle", "psp", "hle_padmap.c"), encoding="utf-8").read()
    for layer in cm.STORIES:                                   # the preset table is the runtime's
        assert all('"%s"' % layer[i] in c_src for i in cm.INPUTS)
    for n, b in cm.NAMES:
        assert '{"%s", ' % n in c_src


# ---------------------------------------------------------------------------------------- against the runtime
EDITS = [
    ("default", []),
    ("stories", []),
    ("swap", [("set_button", 0, "cross", "square", ""), ("set_button", 0, "square", "cross", "")]),
    ("stories_edit", [("preset", "stories"), ("set_button", 0, "left", "l3", ""), ("set_combo", 1, None, "l+square", "l1",
                                                                          "tap"),
                  ("set_stick", 0, "rstick", "r+analog")]),
    ("car_own", [("set_button", 1, "up", "r3", ""), ("set_stick", 1, "lstick", "none"),
                 ("set_combo", 0, None, "l+analog_left", "l2", "")]),
    ("dpad_stick", [("set_stick", 0, "rstick", "dpad"), ("set_button", 0, "start", None, "")]),
]

KEYMAP = {k: "pad_preset" if k == "controls_preset" else "pad_mod" if k == "modifier" else
          "pad_sel_game" if k == "select_to_game" else "pad_sel_up" if k == "select_up" else
          k.replace("map_", "pad_", 1).replace("car_", "pad_c_", 1) for k in cm.KEYS}


def _run_edits(steps):
    lay = cm.Layout({})
    for st in steps:
        if st[0] == "preset":
            lay.set_preset(st[1])
        else:
            getattr(lay, st[0])(*st[1:]) if st[0] != "set_stick" else lay.set_stick(*st[1:4])
    return lay


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_written_values_give_the_shown_mapping(tmp_path):
    tag = "%08x" % zlib.crc32(ROOT.encode())
    psp = config.posix_path(os.path.join(ROOT, "hle", "psp"))
    main = config.posix_path(os.path.join(ROOT, "tests", "cfg", "padmap_main.c"))
    exe = "/tmp/ps2rc_ctlmodel_%s" % tag
    cmd = "set -e; gcc -O1 -Wall -Werror -I'%s' '%s/hle_padmap.c' '%s' -o %s; " % (psp, psp, main, exe)
    want = {}
    for name, steps in EDITS:
        lay = _run_edits(steps)
        vals = lay.to_values()
        if name == "stories":
            vals = {"controls_preset": "stories"}
            lay = cm.Layout(vals)
        args = " ".join("'%s=%s'" % (KEYMAP[k], v) for k, v in vals.items())
        cmd += "echo '== %s'; %s line %s; %s car %s; " % (name, exe, args, exe, args)
        want[name] = (lay.line(), lay.car_line())
    cmd += "rm -f %s" % exe
    r = subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=300)
    out = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    assert r.returncode == 0, out + r.stderr.decode("utf-8", "replace")
    for block in out.split("== ")[1:]:
        name, _, body = block.partition("\n")
        lines = body.split("\n")
        line, notes1, car, notes2 = lines[0], lines[1], lines[2], lines[3]
        strip = lambda s: s.replace("*", "").replace("!", "")          # noqa: E731
        assert notes1 == "notes: " and notes2 == "notes: ", (name, notes1)
        assert strip(line) == want[name.strip()][0], (name, line, want[name.strip()][0])
        assert strip(car) == want[name.strip()][1], (name, car, want[name.strip()][1])


def test_camera_invert():
    """camera_invert=off|x|y|xy: kept in the values, the runtime's line ends with it, a layout change keeps it"""
    lay = cm.Layout({"controls_preset": "stories", "camera_invert": "XY"})
    assert lay.cam_inv == "xy" and lay.to_values() == {"controls_preset": "stories", "camera_invert": "xy"}
    assert lay.line().endswith(" camera_invert=xy")
    lay.set_preset("default")
    assert lay.cam_inv == "xy"
    lay.set_camera_invert("off")
    assert "camera_invert" not in lay.to_values() and "camera_invert" not in lay.line()
    assert cm.Layout({"camera_invert": "2"}).cam_inv == "y" and cm.Layout({"camera_invert": "zz"}).cam_inv == "off"
    with pytest.raises(ValueError):
        lay.set_camera_invert("z")
