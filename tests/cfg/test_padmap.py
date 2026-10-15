"""The player's control mapping (hle/psp/hle_padmap.c), compiled with the host gcc against tests/cfg/padmap_main.c:
the built-in layout equals the original fixed mapping for every button combination, values are parsed and checked
(unknown names, too few buttons, HOME, the port's L+R+SELECT, the system's L+R+Down+START, two inputs on the same
buttons -> that key takes its default), the "controls:" line, a SELECT tap reaches the game as a pulse in 3 frames,
and the friendly names reach the internal keys through the shared reader (hle_cfg.c + the pack's hle_settings.c)."""
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
}

READER_C = r"""
#include <stdio.h>
#include "hle_cfg.h"
static const char *g(const char *k) { const char *v = hle_cfg_get(k); return v ? v : "-"; }
int main(void)
{
    printf("mod=%s game=%s l2=%s rstick=%s\nnotes=%s\n%s\n", g("pad_mod"), g("pad_sel_game"), g("pad_l2"),
           g("pad_rstick"), hle_cfg_notes(), hle_cfg_settings_line());
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
    (tmp / "ps2rc.cfg").write_text("modifier = L\nselect_to_game=Tapp\nmap_l2 = L+R  # mine\nmap_rstick=r+analog\n",
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
    for c in ("eq_default", "eq_up_r3", "eq_up_l3", "eq_up_stick", "eq_written"):
        assert res[c].strip() == "equiv ok 200704", (c, res[c])   # 4096 button sets x 49 stick positions


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")), reason="needs bash + gcc")
def test_bad_values_fall_back(res):
    n = _notes(res["bad"])
    assert "map_l2=foo: unknown button name, select+l used" in n
    assert "map_r2=r: needs two or three buttons, select+r used" in n
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
    assert "map_start=cross: takes the button cross, start used" in n
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
