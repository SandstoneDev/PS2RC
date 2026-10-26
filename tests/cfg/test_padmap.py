"""The player's control mapping (hle/psp/hle_padmap.c), compiled with the host gcc against tests/cfg/padmap_main.c:
the built-in layout equals the original fixed mapping for every button combination, values are parsed and checked
(unknown names, too few buttons, HOME, the port's L+R+SELECT, the system's L+R+Down+START, two inputs on the same
buttons -> that key takes its default), the "controls:" line, a SELECT tap reaches the game as a pulse in 3 frames,
and the friendly names reach the internal keys through the shared reader (hle_cfg.c + the pack's hle_settings.c).
Every input moveable (map_cross ... map_lstick, alternatives "a/b", +tap / +hold, analog directions), a
vehicle layer (car_*) and controls_preset=stories; the default is still the original mapping in both layers."""
import glob
import os
import shutil
import subprocess
import zlib

import pytest
from ps2rc import config

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# the game pack whose settings table carries the control keys (found by content: no game named here)
SETTINGS_C = next((p for p in sorted(glob.glob(os.path.join(ROOT, "games", "*", "src", "hle_settings.c")))
                   if '"map_l2"' in open(p, encoding="utf-8").read()), None)

DEFAULT_LINE = ("controls: modifier=select* select_to_game=always* map_l2=select+l* map_r2=select+r* "
                "map_l3=select+triangle* map_r3=select+circle* map_rstick=select+dpad* map_l1=l* map_r1=r* "
                "map_select=select* map_start=start*")

# name -> (mode, args, stdin or None)
CASES = {
    "default": ("line", [], None),
    "eq_default": ("equiv", [], None),
    "eq_up_r3": ("equiv", ["pad_sel_up=r3"], None),
    "eq_up_l3": ("equiv", ["pad_sel_up=l3"], None),
    "eq_up_stick": ("equiv", ["pad_sel_up=stick"], None),
    "eq_written": ("equiv", ["pad_mod=select", "pad_sel_game=always", "pad_l2=select+l", "pad_r2=R+Select",
                             "pad_l3=select+triangle", "pad_r3=select+circle", "pad_rstick=select+dpad", "pad_l1=l",
                             "pad_r1=r", "pad_select=select", "pad_start=start"], None),
    # the new keys written at their defaults, the preset named, a vehicle layer equal to the foot one
    "eq_written_cg": ("equiv", ["pad_preset=Default", "pad_cross=cross", "pad_circle=circle", "pad_square=square",
                                "pad_triangle=triangle", "pad_up=up", "pad_down=down", "pad_left=left",
                                "pad_right=right", "pad_lstick=analog"], None),
    "eq_car_same": ("equiv", ["pad_c_l2=select+l", "pad_c_cross=cross", "pad_c_rstick=select+dpad",
                              "pad_c_lstick=analog"], None),
    "bad": ("line", ["pad_l2=foo", "pad_r2=r", "pad_l3=select+home", "pad_r3=l+r+select"], None),
    "sys": ("line", ["pad_r3=l+down+start", "pad_mod=x", "pad_sel_game=y"], None),
    "same": ("line", ["pad_l2=select+triangle"], None),
    "both": ("line", ["pad_l2=l+r", "pad_r2=r+l"], None),
    "swap": ("line", ["pad_l2=select+triangle", "pad_l3=select+l"], None),
    "fixed": ("line", ["pad_start=cross", "pad_l2=select+up"], None),
    "rs_bad": ("line", ["pad_rstick=up+dpad"], None),
    "mod_l": ("line", ["pad_mod=l"], None),
    "mod_none": ("line", ["pad_mod=none", "pad_rstick=analog"], None),
    "selup_analog": ("line", ["pad_rstick=r+analog", "pad_sel_up=r3"], None),
    # frame, PSP buttons (hex), stick x, y
    "tap": ("seq", ["pad_sel_game=tap"],
            "10 1 80 80\n10 1 80 80\n11 1 80 80\n12 0 80 80\n12 0 80 80\n14 0 80 80\n16 0 80 80\n18 0 80 80\n"),
    "tap_combo": ("seq", ["pad_sel_game=tap"], "10 1 80 80\n11 101 80 80\n12 1 80 80\n13 0 80 80\n14 0 80 80\n"),
    "tap_other": ("seq", ["pad_sel_game=tap"], "10 1 80 80\n11 4001 80 80\n12 1 80 80\n13 0 80 80\n14 0 80 80\n"),
    "always": ("seq", [], "10 1 80 80\n11 101 80 80\n12 0 80 80\n"),
    "lr": ("seq", ["pad_l2=l+r", "pad_r2=r+square"], "1 100 80 80\n2 300 80 80\n3 301 80 80\n4 8200 80 80\n"),
    "analog": ("seq", ["pad_rstick=r+analog"], "1 200 80 80\n2 200 ff 00\n3 0 ff 00\n4 2200 ff 80\n"),
    # --- every input, presets, vehicle layer
    "stories": ("line", ["pad_preset=stories"], None),
    "stories_car": ("car", ["pad_preset=stories"], None),
    "stories_mod": ("line", ["pad_preset=VCS", "pad_mod=r", "pad_c_up=up"], None),
    "bad_cg": ("line", ["pad_cross=tap", "pad_l1=l+tap+hold", "pad_lstick=r+analog", "pad_rstick=l+r+dpad",
                        "pad_l2=a/b", "pad_r3=up+dpad", "pad_l3=l+triangle/l+triangle", "pad_preset=gtx"], None),
    "swap_cs": ("line", ["pad_cross=square", "pad_square=cross"], None),
    "swap_seq": ("seq", ["pad_cross=square", "pad_square=cross"], "1 4000 80 80\n2 8000 80 80\n"),
    "alt": ("seq", ["pad_l2=select+l/left", "pad_left=l+left"], "1 80 80 80\n2 101 80 80\n3 180 80 80\n"),
    "ldpad": ("seq", ["pad_lstick=dpad", "pad_up=l+up", "pad_down=l+down", "pad_left=l+left", "pad_right=l+right"],
              "1 80 80 80\n2 10 80 80\n3 180 80 80\n4 0 ff 80\n"),
    "ldpad_line": ("line", ["pad_lstick=dpad", "pad_up=l+up", "pad_down=l+down", "pad_left=l+left",
                            "pad_right=l+right"], None),
    # foot: L + stick = right stick (left stick centred); L tap = L1 pulse; L held alone 8 frames = L1; L + R held
    # = R3 after 8 frames; R + L + stick = R1 + right stick (free aim); D-pad Left = L2; L + Left = Left
    # camera_invert (pad_cam_inv): the analog-stick right stick reversed per axis; the D-pad camera unchanged
    "cam_off": ("seq", ["pad_preset=stories"], "1 100 20 80\n2 100 80 e0\n3 0 80 80\n"),
    "cam_x": ("seq", ["pad_preset=stories", "pad_cam_inv=x"], "1 100 20 80\n2 100 80 e0\n3 0 80 80\n"),
    "cam_xy": ("seq", ["pad_preset=stories", "pad_cam_inv=XY"], "1 100 20 80\n2 100 80 e0\n3 0 80 80\n"),
    "cam_dpad": ("seq", ["pad_cam_inv=xy"], "1 11 80 80\n"),
    "cam_line": ("line", ["pad_preset=stories", "pad_cam_inv=3"], None),
    "cam_bad": ("line", ["pad_cam_inv=z"], None),
    "stories_foot": ("seq", ["pad_preset=stories"],
                 "1 100 80 80\n2 100 20 80\n3 0 80 80\n"
                 "10 100 80 80\n12 0 80 80\n14 0 80 80\n16 0 80 80\n18 0 80 80\n"
                 + "".join("%d 100 80 80\n" % f for f in range(20, 38, 2)) + "40 0 80 80\n"
                 + "".join("%d 300 80 80\n" % f for f in range(50, 68, 2)) + "70 0 80 80\n"
                 "80 200 80 80\n82 300 80 80\n84 300 ff 80\n86 200 80 80\n"
                 "90 80 80 80\n92 180 80 80\n94 40 80 80\n"),
    # vehicle: L + stick left = L2, + down = L2+R2, stick centred; R + steering = R1 + left stick; D-pad Right = Up
    # (radio), Down = L3 (horn), Up = R3; L + D-pad Up = right stick up; L + R + Right = Right; L + stick up = nothing
    "stories_veh": ("seq", ["pad_preset=stories"],
                "1 100 20 80 1\n2 100 80 ff 1\n3 200 20 80 1\n4 20 80 80 1\n5 40 80 80 1\n6 10 80 80 1\n"
                "7 110 80 80 1\n8 320 80 80 1\n9 100 80 20 1\n"),
    "ctx_default": ("seq", ["pad_c_l2=l+r"], "1 300 80 80 0\n2 300 80 80 1\n"),
    "tap_again": ("seq", ["pad_l1=l+tap"], "1 100 80 80\n2 0 80 80\n3 100 80 80\n4 100 80 80\n5 0 80 80\n"
                                         "6 0 80 80\n7 0 80 80\n8 0 80 80\n9 0 80 80\n"),
}

READER_C = r"""
#include <stdio.h>
#include "hle_cfg.h"
static const char *g(const char *k) { const char *v = hle_cfg_get(k); return v ? v : "-"; }
int main(void)
{
    printf("mod=%s game=%s l2=%s rstick=%s\nnotes=%s\n%s\n", g("pad_mod"), g("pad_sel_game"), g("pad_l2"),
           g("pad_rstick"), hle_cfg_notes(), hle_cfg_settings_line());
    printf("preset=%s c_l2=%s cross=%s\n", g("pad_preset"), g("pad_c_l2"), g("pad_cross"));
    return 0;
}
"""


def _run(tmp):
    tag = "%08x" % zlib.crc32(ROOT.encode())
    p = config.posix_path(str(tmp))
    for name, (_m, _a, inp) in CASES.items():
        if inp is not None:
            (tmp / (name + ".in")).write_text(inp, encoding="ascii")
    (tmp / "reader.c").write_text(READER_C, encoding="ascii")
    (tmp / "ps2rc.cfg").write_text("modifier = L\nselect_to_game=Tapp\nmap_l2 = L+R  # mine\nmap_rstick=r+analog\n"
                                   "controls_preset = VCS\ncar_l2 = l+analog_left/left\nmap_cross=square\n",
                                   encoding="ascii")
    psp = config.posix_path(os.path.join(ROOT, "hle", "psp"))
    core = config.posix_path(os.path.join(ROOT, "hle", "core"))
    main = config.posix_path(os.path.join(HERE, "padmap_main.c"))
    sett = config.posix_path(SETTINGS_C) if SETTINGS_C else ""
    exe, rd = "/tmp/ps2rc_padmap_%s" % tag, "/tmp/ps2rc_padrd_%s" % tag
    # no shell variables: wsl.exe expands $NAME itself
    cmd = ("set -e; gcc -O1 -Wall -Wextra -Werror -I'%s' '%s/hle_padmap.c' '%s' -o %s; " % (psp, psp, main, exe) +
           ("gcc -O1 -Wall -DHLE_CFG_STANDALONE -I'%s' '%s/hle_cfg.c' '%s' '%s/reader.c' -o %s; "
            % (core, core, sett, p, rd) if sett else ""))
    for name, (mode, args, inp) in CASES.items():
        a = " ".join("'%s'" % x for x in args)
        cmd += "echo '== %s'; %s %s %s%s || true; " % (name, exe, mode, a, " < '%s/%s.in'" % (p, name) if inp else "")
    if sett:
        cmd += "echo '== reader'; HLE_CFG_DIR='%s' %s; " % (p, rd)
    cmd += "rm -f %s %s" % (exe, rd)
    r = subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=300)
    out = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    assert r.returncode == 0, out + r.stderr.decode("utf-8", "replace")
    res = {}
    for block in out.split("== ")[1:]:
        name, _, body = block.partition("\n")
        res[name.strip()] = body
    return res


@pytest.fixture(scope="module")
def res(tmp_path_factory):
    return _run(tmp_path_factory.mktemp("padmap"))


def _line(body):
    return body.split("\n")[0]


def _notes(body):
    return body.split("\n")[1]


def _seq(body):
    return [ln for ln in body.split("\n") if ln.strip()]


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_default_is_the_original_mapping(res):
    assert _line(res["default"]) == DEFAULT_LINE and _notes(res["default"]) == "notes: "
    for c in ("eq_default", "eq_up_r3", "eq_up_l3", "eq_up_stick", "eq_written", "eq_written_cg", "eq_car_same"):
        assert res[c].strip() == "equiv ok 200704", (c, res[c])   # 4096 button sets x 49 stick positions


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_bad_values_fall_back(res):
    n = _notes(res["bad"])
    assert "map_l2=foo: unknown button name, select+l used" in n
    assert "map_r2=r: same buttons as map_r1, select+r used" in n   # one button is fine now
    assert "map_l3=select+home: HOME belongs to the system, select+triangle used" in n
    assert "map_r3=l+r+select: contains L+R+SELECT (the fps counter), select+circle used" in n
    assert "map_l2=select+l! map_r2=select+r! map_l3=select+triangle! map_r3=select+circle!" in _line(res["bad"])
    n = _notes(res["sys"])
    assert "map_r3=l+down+start: three of L+R+Down+START (the system menu)" in n
    assert "modifier=x: not select, l, r or none, select used" in n and "select_to_game=y: not always or tap" in n
    assert "modifier=select! select_to_game=always!" in _line(res["sys"])
    assert "map_rstick=up+dpad: use <button>+dpad" in _notes(res["rs_bad"])
    assert "map_rstick=analog: analog needs a modifier" in _notes(res["mod_none"])


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_conflicts(res):
    assert "map_l2=select+triangle: same buttons as map_l3, select+l used" in _notes(res["same"])
    assert "map_l2=select+l! " in _line(res["same"]) and "map_l3=select+triangle* " in _line(res["same"])
    # both changed onto the same buttons: the later key gives way
    assert "map_l2=l+r " in _line(res["both"]) and "map_r2=r+l: same buttons as map_l2, select+r used" in \
        _notes(res["both"])
    # a swap is two changes without a conflict
    assert "map_l2=select+triangle map_r2=select+r* map_l3=select+l " in _line(res["swap"])
    assert _notes(res["swap"]) == "notes: "
    n = _notes(res["fixed"])
    assert "map_start=cross: same buttons as map_cross, start used" in n
    assert "map_l2=select+up: same buttons as map_rstick, select+l used" in n


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_modifier_defaults(res):
    assert ("modifier=l select_to_game=always* map_l2=select+l* map_r2=l+r* map_l3=l+triangle* map_r3=l+circle* "
            "map_rstick=l+dpad*") in _line(res["mod_l"])
    assert "map_l2=none* map_r2=none* map_l3=none* map_r3=none* map_rstick=none!" in _line(res["mod_none"])
    assert "select_up=r3: applies only to a D-pad right stick, ignored" in _notes(res["selup_analog"])


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_select_tap_pulse(res):
    # held: nothing; released at frame 12: SELECT in the frames 12, 14, 16 (3 distinct frames, any number of reads)
    assert [ln[:4] for ln in _seq(res["tap"])] == ["0000"] * 3 + ["0001"] * 4 + ["0000"]
    # SELECT + L = L2 without SELECT, and no pulse after a combination
    assert [ln[:4] for ln in _seq(res["tap_combo"])] == ["0000", "0100", "0000", "0000", "0000"]
    # SELECT + Cross: Cross reaches the game, SELECT does not, no pulse
    assert [ln[:4] for ln in _seq(res["tap_other"])] == ["0000", "4000", "0000", "0000", "0000"]
    # always (built-in): SELECT with everything, SELECT + L = SELECT + L2
    assert [ln[:4] for ln in _seq(res["always"])] == ["0001", "0101", "0000"]


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_combinations_and_analog(res):
    # L = L1; L+R = L2 (no L1 / R1); L+R+SELECT = SELECT + L2; R+Square = R2 (no R1, no Square)
    assert [ln[:4] for ln in _seq(res["lr"])] == ["0400", "0100", "0101", "0200"]
    # R = R1; R + stick = right stick (left stick centred, no R1); stick alone = left stick; R + Circle + stick
    assert _seq(res["analog"]) == ["0800 80 80 80 80", "0000 ff 00 80 80", "0000 80 80 ff 00",
                                   "2000 ff 80 80 80"]


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_friendly_names_through_the_reader(res):
    if not SETTINGS_C:
        pytest.skip("no game pack with the control keys")
    body = res["reader"]
    assert body.startswith("mod=l game=- l2=L+R rstick=r+analog\n")       # enum lower-cased, a bad word dropped
    assert "select_to_game=tapp is not a valid value (ignored)" in body
    assert " modifier=l select_to_game=-* map_l2=L+R " in body
    # the new keys reach the table, and stay out of the "settings:" line (the "controls:" lines show them)
    assert "preset=vcs c_l2=l+analog_left/left cross=square" in body   # the old name: still accepted
    assert "car_l2" not in body and "controls_preset" not in body and "map_cross" not in body


# ---------------------------------------------------------------------- every input, presets, vehicle layer
SKIP = pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")),
                          reason="needs bash + gcc")
STORIES_LINE = ("controls: controls_preset=stories modifier=l* select_to_game=always* map_l2=left* map_r2=right* "
            "map_l3=down* map_r3=l+r+hold* map_rstick=l+analog* map_l1=l+hold* map_r1=r* map_select=select* "
            "map_start=start* map_down=l+down* map_left=l+left* map_right=l+right*")
STORIES_CAR = ("controls in a vehicle: car_l2=l+analog_left/l+analog_down* car_r2=l+analog_right/l+analog_down* "
           "car_r3=up* car_rstick=l+dpad* car_up=right* car_down=left* car_left=l+r+left* car_right=l+r+right*")


@pytest.mark.wsl
@SKIP
def test_stories_preset_lines(res):
    # the preset's two layers are conflict-free (no notes) and logged as built-in
    assert _line(res["stories"]) == STORIES_LINE and _notes(res["stories"]) == "notes: "
    assert _line(res["stories_car"]) == STORIES_CAR and _notes(res["stories_car"]) == "notes: "
    n = _notes(res["stories_mod"])
    assert "modifier=r: applies only to controls_preset=default, ignored" in n
    assert "in a vehicle: car_up=up: same buttons as car_r3, right used" in n
    assert " modifier=l! " in _line(res["stories_mod"])


@pytest.mark.wsl
@SKIP
def test_new_values_checked(res):
    n = _notes(res["bad_cg"])
    for want in ("controls_preset=gtx: not default or stories, default used",
                 "map_l2=a/b: unknown button name, select+l used",
                 "map_l3=l+triangle/l+triangle: the same combination twice, select+triangle used",
                 "map_r3=up+dpad: dpad only for map_rstick / map_lstick, select+circle used",
                 "map_rstick=l+r+dpad: three of L+R+Down+START (the system menu), select+dpad used",
                 "map_l1=l+tap+hold: tap and hold together, l used",
                 "map_cross=tap: needs a button, cross used",
                 "map_lstick=r+analog: use analog, dpad, <button>+dpad or none, analog used"):
        assert want in n, want
    assert "controls: controls_preset=default! " in _line(res["bad_cg"])


@pytest.mark.wsl
@SKIP
def test_full_remap(res):
    # Cross <-> Square swapped: two changes, no conflict; PSP Cross gives PS2 Square and back
    assert _line(res["swap_cs"]).endswith(" map_cross=square map_square=cross") and _notes(res["swap_cs"]) == "notes: "
    assert [ln[:4] for ln in _seq(res["swap_seq"])] == ["8000", "4000"]
    # alternatives: L2 on SELECT+L or on Left; Left itself moved to L+Left (Left alone = L2 only)
    assert [ln[:4] for ln in _seq(res["alt"])] == ["0100", "0101", "0080"]
    # the left stick on the D-pad (the D-pad buttons moved to L + D-pad): full deflection, the analog stick ignored
    assert _notes(res["ldpad_line"]) == "notes: " and "map_lstick=dpad" in _line(res["ldpad_line"])
    assert _seq(res["ldpad"]) == ["0000 80 80 00 80", "0000 80 80 80 00", "0080 80 80 80 80", "0000 80 80 80 80"]


@pytest.mark.wsl
@SKIP
def test_stories_on_foot(res):
    s = _seq(res["stories_foot"])
    # L: nothing yet; L + stick left: right stick left, left stick centred; release: no L1
    assert s[0:3] == ["0000 80 80 80 80", "0000 2f 80 80 80", "0000 80 80 80 80"]
    # L tapped: L1 in 3 frames
    assert [x[:4] for x in s[3:8]] == ["0000", "0400", "0400", "0400", "0000"]
    # L held alone: L1 from the 8th frame, no pulse on release
    assert [x[:4] for x in s[8:18]] == ["0000"] * 7 + ["0400", "0400", "0000"]
    # L + R held: R1 at first, R3 (look behind) from the 8th frame with R1 silent, nothing on release
    assert [x[:4] for x in s[18:28]] == ["0800"] * 7 + ["0004", "0004", "0000"]
    # R, R + L, R + L + stick (free aim: R1 + right stick), R
    assert s[28:32] == ["0800 80 80 80 80", "0800 80 80 80 80", "0800 ff 80 80 80", "0800 80 80 80 80"]
    # D-pad Left = L2 (previous weapon), L + Left = Left, Down = L3 (crouch)
    assert [x[:4] for x in s[32:35]] == ["0100", "0080", "0002"]


@pytest.mark.wsl
@SKIP
def test_stories_in_a_vehicle(res):
    assert _seq(res["stories_veh"]) == [
        "0100 80 80 80 80",   # L + stick left: L2 (look left), no steering
        "0300 80 80 80 80",   # L + stick down: L2 + R2 (look behind)
        "0800 80 80 2f 80",   # R + steering: R1 (handbrake) and the left stick
        "0010 80 80 80 80",   # D-pad Right: Up (radio)
        "0002 80 80 80 80",   # D-pad Down: L3 (horn)
        "0004 80 80 80 80",   # D-pad Up: R3 (sub-mission)
        "0000 80 00 80 80",   # L + D-pad Up: right stick up (turret, jet nozzles)
        "0020 80 80 80 80",   # L + R + D-pad Right: Right (skip trip)
        "0000 80 80 80 2f"]   # L + stick up: nothing, steering untouched


@pytest.mark.wsl
@SKIP
def test_vehicle_layer_and_taps(res):
    # car_l2=l+r: L + R = L2 only in a vehicle (on foot L1 + R1)
    assert [x[:4] for x in _seq(res["ctx_default"])] == ["0c00", "0100"]
    # a second tap while the first pulse runs: the pulse ends after 3 frames, the second tap gives its own
    assert [x[:4] for x in _seq(res["tap_again"])] == ["0000", "0400", "0400", "0400", "0400", "0400", "0400",
                                                       "0000", "0000"]


@pytest.mark.wsl
@SKIP
def test_camera_invert(res):
    off, x, xy = _seq(res["cam_off"]), _seq(res["cam_x"]), _seq(res["cam_xy"])
    assert off[0] == "0000 2f 80 80 80" and x[0] == "0000 d1 80 80 80" and xy[0] == "0000 d1 80 80 80"
    ry = int(off[1].split()[2], 16)
    assert ry > 0xc0 and x[1] == off[1]                                  # x only: Y as before
    assert int(xy[1].split()[2], 16) == 0x100 - ry                       # around the centre 0x80
    assert off[2] == x[2] == xy[2] == "0000 80 80 80 80"                 # released: centred, not inverted
    assert _seq(res["cam_dpad"]) == ["0001 80 00 80 80"]                 # SELECT + Up: the D-pad camera unchanged
    assert _line(res["cam_line"]).endswith(" camera_invert=xy")
    assert "camera_invert" not in _line(res["default"])
    assert "camera_invert=z" in _notes(res["cam_bad"])
