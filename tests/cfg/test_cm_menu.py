"""The in-game settings menu's pure parts on the host.
- the game pack's registry (src/hle_settings_menu.c) covers every friendly name (hle_settings.c) and every key of the
  developer template (cfg/dev.cfg); its recommended values are the player template's (cfg/player_en.cfg);
- hle/core/hle_cfg_save.c writes settings back keeping comments / layout (the last line of a key, the internal key's
  lines, CRLF, BOM, commented template lines, the menu section, developer routing);
- hle/core/hle_cfg.c's run-time hle_cfg_set (friendly -> internal, clamping, words, notes untouched, pool compaction)."""
import glob
import os
import re
import shutil
import subprocess
import zlib

import pytest
from ps2rc import config

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# the game pack with the menu registry (found by content: no game named here)
PACK = next((os.path.dirname(os.path.dirname(p)) for p in sorted(glob.glob(os.path.join(ROOT, "games", "*", "src",
                                                                                      "hle_settings_menu.c")))), None)
NEED = pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")),
                          reason="needs bash + gcc")


def _read(p):
    return open(p, encoding="utf-8").read()


def _aliases():
    src = _read(os.path.join(PACK, "src", "hle_settings.c"))
    body = src[src.index("aliases[] = {"):src.index("};", src.index("aliases[] = {"))]
    return dict(re.findall(r'\{"(\w+)",\s*"(\w+)"', body))


def _registry():
    src = _read(os.path.join(PACK, "src", "hle_settings_menu.c"))
    body = src[src.index("reg[] = {"):src.index("};", src.index("reg[] = {"))]
    out = {}
    for line in body.splitlines():
        m = re.match(r'\s*\{"(\w+)",\s*"(\w+)",\s*"([^"]*)",\s*"((?:[^"\\]|\\.)*)",\s*(NULL|"[^"]*")', line)
        if m:
            out[m.group(1)] = {"cat": m.group(2), "rec": None if m.group(5) == "NULL" else m.group(5)[1:-1],
                               "dev": "HS_DEV" in line, "ro": "HS_RO" in line}
    return out


def _cfg_keys(path):
    return dict(re.findall(r"^([a-z_0-9]+)\s*=\s*(\S+)", _read(path), re.M))


@pytest.mark.skipif(PACK is None, reason="no game pack with a menu registry")
def test_registry_covers_every_setting():
    al, reg = _aliases(), _registry()
    assert len(reg) > 40
    missing = sorted(set(al) - set(reg))
    assert not missing, "friendly names without a menu line (hle_settings_menu.c): %s" % missing
    for k, r in reg.items():
        if not r["dev"]:
            assert k in al, "player menu line %s is no friendly name" % k
    dev = _cfg_keys(os.path.join(PACK, "cfg", "dev.cfg"))
    assert not sorted(set(dev) - set(reg)), "dev.cfg keys without a menu line: %s" % sorted(set(dev) - set(reg))
    for k in dev:
        assert reg[k]["dev"], k


@pytest.mark.skipif(PACK is None, reason="no game pack with a menu registry")
def test_recommended_values_are_the_templates():
    reg, tpl = _registry(), _cfg_keys(os.path.join(PACK, "cfg", "player_en.cfg"))
    for k, v in tpl.items():
        assert k in reg, k
        assert reg[k]["rec"] == v, "%s: menu recommends %s, template has %s" % (k, reg[k]["rec"], v)
    # the Russian template has the same values
    assert _cfg_keys(os.path.join(PACK, "cfg", "player_ru.cfg")) == tpl


REWRITE = {
    # name: (input, insert, edits, expected output, done flags)
    "replace_keep_comment": ("# c\ndraw_distance = 40   # mine\nfog=250\n", 1, ["draw_distance|tune_far|55|0"],
                             "# c\ndraw_distance = 55   # mine\nfog=250\n", "1"),
    "last_line_wins": ("a=1\nb=2\na=3\n", 1, ["a||9|0"], "a=1\nb=2\na=9\n", "1"),
    "internal_key_line": ("tune_far=40\n", 1, ["draw_distance|tune_far|60|0"], "draw_distance=60\n", "1"),
    "crlf_after_template": ("x=1\r\n#map_l2=select+l\r\ny=2\r\n", 1, ["map_l2|pad_l2|l+r|0"],
                            "x=1\r\n#map_l2=select+l\r\nmap_l2=l+r\r\ny=2\r\n", "1"),
    "append_header": ("x=1", 1, ["newkey||5|0"], "x=1\n# --- changed in the game menu\nnewkey=5\n", "1"),
    "header_once": ("x=1\n# --- changed in the game menu\nk=1\n", 1, ["k||2|0", "m||3|0"],
                    "x=1\n# --- changed in the game menu\nk=2\nm=3\n", "11"),
    "dev_not_in_player": ("x=1\n", 1, ["verbose||1|1"], "x=1\n", "0"),
    "dev_into_dev": ("pace=1\n", 2, ["verbose||1|1", "pace||0|1", "peds|tune_peds|50|0"],
                     "pace=0\n# --- changed in the game menu\nverbose=1\n", "110"),
    "existing_only": ("a=1\n#b=2\n", 0, ["b||3|0", "a||4|0"], "a=4\n#b=2\n", "01"),
    "bom": ("\xef\xbb\xbfdraw_distance=40\n", 1, ["draw_distance|tune_far|45|0"], "\xef\xbb\xbfdraw_distance=45\n", "1"),
    "case_and_spaces": ("  Draw_Distance\t=\t40 ; old\n", 1, ["draw_distance|tune_far|70|0"],
                        "  draw_distance\t=\t70 ; old\n", "1"),
    "empty_file": ("", 1, ["peds|tune_peds|50|0"], "# --- changed in the game menu\npeds=50\n", "1"),
    # unset (the menu's "foot" for car_ keys): every active line commented, comments kept, nothing inserted
    "unset_lines": ("x=1\ncar_l2 = l+r   # mine\ny=2\npad_c_l2=r+square\r\n", 1, ["car_l2|pad_c_l2|<unset>|0"],
                    "x=1\n#car_l2 = l+r   # mine\ny=2\n#pad_c_l2=r+square\r\n", "1"),
    "unset_absent": ("x=1\n#car_l2=l+r\n", 1, ["car_l2|pad_c_l2|<unset>|0"], "x=1\n#car_l2=l+r\n", "1"),
    "unset_absent_step0": ("x=1\n", 0, ["car_l2|pad_c_l2|<unset>|0"], "x=1\n", "0"),
    "unset_bom": ("\xef\xbb\xbfcar_up=l+up\n", 1, ["car_up|pad_c_up|<unset>|0"], "\xef\xbb\xbf#car_up=l+up\n", "1"),
    "set_after_unset": ("#car_l2=l+r\n", 1, ["car_l2|pad_c_l2|r+square|0"], "#car_l2=l+r\ncar_l2=r+square\n", "1"),
}


def _wsl_run(cmd):
    r = subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=300)
    out = r.stdout.decode("latin-1").replace("\r\r\n", "\r\n")
    assert r.returncode == 0, out + r.stderr.decode("utf-8", "replace")
    return out


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("cm_menu")
    tag = "%08x" % zlib.crc32(ROOT.encode())
    exe = "/tmp/ps2rc_cm_menu_%s" % tag
    core = config.posix_path(os.path.join(ROOT, "hle", "core"))
    psp = config.posix_path(os.path.join(ROOT, "hle", "psp"))
    sett = config.posix_path(os.path.join(PACK, "src", "hle_settings.c"))
    main = config.posix_path(os.path.join(HERE, "cm_menu_main.c"))
    _wsl_run("set -e; gcc -O1 -Wall -Werror -I'%s' -c '%s/hle_cfg_save.c' -o %s_s.o; gcc -O1 -Wall -DHLE_CFG_STANDALONE "
             "-I'%s' -I'%s' '%s/hle_cfg.c' '%s/hle_padmap.c' '%s' '%s' %s_s.o -o %s; rm -f %s_s.o"
             % (core, core, exe, core, psp, core, psp, sett, main, exe, exe, exe))
    yield tmp, exe
    _wsl_run("rm -f %s" % exe)


@NEED
@pytest.mark.wsl
@pytest.mark.skipif(PACK is None, reason="no game pack")
def test_rewrite_keeps_the_file(built):
    tmp, exe = built
    cmd = ""
    for name, (inp, ins, edits, _exp, _done) in REWRITE.items():
        (tmp / (name + ".cfg")).write_bytes(inp.encode("latin-1"))
        cmd += "echo '@@ %s'; %s rewrite '%s/%s.cfg' %d %s; " % (name, exe, config.posix_path(str(tmp)), name, ins,
                                                              " ".join("'%s'" % e for e in edits))
    out = _wsl_run(cmd)
    res = {}
    for block in out.split("@@ ")[1:]:
        name, _, body = block.partition("\n")
        res[name.strip()] = body
    for name, (_inp, _ins, _edits, exp, done) in REWRITE.items():
        body = res[name]
        text, _, tail = body.rpartition("--done ")
        assert text == exp, (name, text, exp)
        assert tail.strip() == done, (name, tail)


@NEED
@pytest.mark.wsl
@pytest.mark.skipif(PACK is None, reason="no game pack")
def test_runtime_set_and_pool(built):
    tmp, exe = built
    d = tmp / "set"
    d.mkdir(exist_ok=True)
    (d / "ps2rc.cfg").write_text("draw_distance=40\nfog=9999\nbogus=1\n", encoding="ascii")
    (d / "ps2rc_dev.cfg").write_text("d=7\n", encoding="ascii")
    p = config.posix_path(str(d))
    out = _wsl_run("HLE_CFG_DIR='%s' %s set draw_distance=55 draw_distance=500 textures=half shadows=0 overclock=999 "
                   "modifier=L fps_cap=off verbose=2; HLE_CFG_DIR='%s' %s stress" % (p, exe, p, exe))
    lines = out.splitlines()
    assert lines[0] == "ok tune_far=55 friendly=55"
    assert lines[1] == "ok tune_far=100 friendly=100"                 # clamped like a file line
    assert lines[2] == "ok tex_half=128 friendly=half"
    assert lines[3] == "ok x_fxskip=1 friendly=off"                  # inverted key
    assert lines[4].startswith("bad cpu_auto=")                      # words-only key refuses other values
    assert lines[5] == "ok pad_mod=l friendly=l"
    assert lines[6] == "ok x_fpscap=0 friendly=off"
    assert lines[7] == "ok verbose=2 friendly=-"
    assert lines[8] == "notes_same=1"                                # the boot notes are not touched
    # 20000 changes: the pool is compacted, nothing lost
    assert lines[9] == "count 4->5 tune_far=55 zz=%d d=7" % ((19998 * 7919) % 100000)


@NEED
@pytest.mark.wsl
@pytest.mark.skipif(PACK is None, reason="no game pack")
def test_menu_chord(built):
    """L + R + Cross + Down opens the menu on the last press; the game gets neither that read nor the release after
    the menu; refused combinations: too short / long, unknown, L+R+SELECT, the custom firmware's exit combinations."""
    tmp, exe = built
    out = _wsl_run("%s chord l+r+cross+down L+R+Cross+Down select+start l+r+down l+r+select l+r+down+start "
                   "r+l+down+select l x+y l+r+cross+down+square" % exe).splitlines()
    assert out[0] == "4340 " and out[1] == "4340 " and out[2] == "0009 " and out[3] == "0340 "
    assert out[4] == "0000 contains L+R+SELECT (the fps counter)"
    assert out[5].startswith("0000 contains L+R+Down+START") and out[6].startswith("0000 contains L+R+")
    assert out[7] == "0000 needs two to four buttons" and out[8] == "0000 unknown button name"
    assert out[9] == "0000 needs two to four buttons"
    seq = "0100\n0300\n4300\n4340\n4340\n4340 close\n4340\n4300\n0000\n4300\n4340\n0040\n0000\n0040\n"
    (tmp / "chord.in").write_text(seq, encoding="ascii")
    got = _wsl_run("%s chordseq < '%s/chord.in'" % (exe, config.posix_path(str(tmp)))).splitlines()
    # partial combinations reach the game; the completing read opens and hides all four; still held: hidden;
    # after the menu: hidden until released; a second press opens again; Down alone afterwards is the game's
    assert got == ["0100", "0300", "4300", "0000 OPEN", "0000", "0000", "0000", "0000", "4300", "0000 OPEN", "0000",
                   "0000", "0040"], got


def _pad(exe, d, *sets):
    out = _wsl_run("HLE_CFG_DIR='%s' %s pad %s" % (config.posix_path(str(d)), exe, " ".join("'%s'" % s for s in sets)))
    res = dict(ln.split(": ", 1) for ln in out.splitlines() if ": " in ln)
    assert not [ln for ln in out.splitlines() if ln.startswith("bad ")], out
    return res["foot"], res["car"]


def _cfg_dir(tmp, name, text):
    d = tmp / name
    d.mkdir(exist_ok=True)
    (d / "ps2rc.cfg").write_bytes(text.encode("utf-8"))
    (d / "ps2rc_dev.cfg").write_bytes(open(os.path.join(PACK, "cfg", "dev.cfg"), "rb").read())
    return d


@NEED
@pytest.mark.wsl
@pytest.mark.skipif(PACK is None, reason="no game pack")
def test_menu_controls_live_and_saved(built):
    """controls_preset / map_ / car_ changed in the menu rebuild BOTH layers at once (hle_pad_reconfig = padmap_config
    on the cfg table), car_X=foot = not set, and the saved file gives the same mapping after a restart."""
    tmp, exe = built
    tpl = open(os.path.join(PACK, "cfg", "player_en.cfg"), encoding="utf-8").read()
    base = _cfg_dir(tmp, "pad_base", tpl)
    foot0, car0 = _pad(exe, base)
    assert car0 == ""                                             # default layout: no vehicle layer
    # default -> stories: the on-foot AND the vehicle layer of the preset, as if it were in the file
    live = _pad(exe, base, "controls_preset=stories")
    fresh = _pad(exe, _cfg_dir(tmp, "pad_st", tpl + "controls_preset=stories\n"))
    assert live == fresh
    assert "controls_preset=stories" in live[0] and live[1].startswith("controls in a vehicle:")
    assert live[1] != "controls in a vehicle: (as on foot)"
    sd = _pad(exe, base, "controls_preset=stories", "controls_preset=default")   # and back (the line names it)
    assert (sd[0].replace("controls_preset=default ", ""), sd[1]) == (foot0, car0)
    # overrides on top of the preset; foot = not set again
    over = _pad(exe, base, "controls_preset=stories", "map_l2=r+square", "car_l2=l+r")
    assert "map_l2=r+square" in over[0] and "car_l2=l+r" in over[1]
    back = _pad(exe, base, "controls_preset=stories", "map_l2=r+square", "car_l2=l+r", "car_l2=foot")
    assert back == _pad(exe, base, "controls_preset=stories", "map_l2=r+square")
    assert "car_l2=l+r" not in back[1]
    # a car_ key alone (default layout) makes a vehicle layer; foot removes it again
    assert "car_l2=l+r" in _pad(exe, base, "car_l2=l+r")[1]
    assert _pad(exe, base, "car_l2=l+r", "car_l2=foot") == (foot0, car0)
    # save (the menu's three steps on a player file without a developer edit) -> restart = the same mapping
    p = config.posix_path(str(base / "ps2rc.cfg"))
    edits = ["controls_preset|pad_preset|stories|0", "map_l2|pad_l2|r+square|0", "car_l2|pad_c_l2|l+r|0",
             "car_cross|pad_c_cross|<unset>|0"]
    out = _wsl_run("%s rewrite '%s' 1 %s" % (exe, p, " ".join("'%s'" % e for e in edits)))
    text, _, done = out.rpartition("--done ")
    assert done.strip() == "1111"
    assert [ln for ln in tpl.splitlines() if ln.startswith("#")] == [ln for ln in text.splitlines()   # comments kept
                                                                    if ln.startswith("#") and "game menu" not in ln]
    assert _pad(exe, _cfg_dir(tmp, "pad_saved", text)) == over
    # unset car_l2 in the saved file -> its line commented, the mapping = without it
    (tmp / "saved.cfg").write_bytes(text.encode("latin-1"))
    out2 = _wsl_run("%s rewrite '%s' 1 'car_l2|pad_c_l2|<unset>|0'" % (exe, config.posix_path(str(tmp / "saved.cfg"))))
    text2, _, done2 = out2.rpartition("--done ")
    assert done2.strip() == "1" and "#car_l2=l+r" in text2 and "\ncar_l2=" not in text2
    assert _pad(exe, _cfg_dir(tmp, "pad_unset", text2)) == back
