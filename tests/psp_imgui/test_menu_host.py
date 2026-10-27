"""lib/psp_imgui + the in-game settings menu (hle/psp/hle_menu.cpp) on the host (tests/psp_imgui/host_menu.cpp,
built with the host g++ by build_host.sh): the marquee timing (derma-style ping-pong), the pool allocator, and a scripted
menu session on the real player / developer templates - values stepped, a save that keeps the files' comments, frames
rasterised in software (the T4 font as the GE samples it) and the pool never full."""
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
PACK = next((os.path.dirname(os.path.dirname(p)) for p in sorted(glob.glob(os.path.join(ROOT, "games", "*", "src",
                                                                                      "hle_settings_menu.c")))), None)
pytestmark = [pytest.mark.wsl,
              pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("g++")),
                                 reason="needs bash + g++"),
              pytest.mark.skipif(PACK is None, reason="no game pack with a menu registry")]


def _run(cmd, timeout=600):
    r = subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=timeout)
    out = r.stdout.decode("utf-8", "replace")
    assert r.returncode == 0, out + r.stderr.decode("utf-8", "replace")
    return out


@pytest.fixture(scope="module")
def exe():
    e = "/tmp/ps2rc_cm_host_menu_%08x" % zlib.crc32(ROOT.encode())
    _run("sh '%s/tests/psp_imgui/build_host.sh' '%s' %s" % (config.posix_path(ROOT), config.posix_path(ROOT), e))
    yield e
    _run("rm -f %s" % e)


def test_marquee_ping_pong(exe):
    # 30 px over: hold 0.8 s, slide 1 s at 30 px/s, hold 0.8 s, slide back 1 s (cycle 3.6 s)
    t = dict(ln.split() for ln in _run("%s --marquee" % exe).splitlines())
    assert t["0.0"] == "0" and t["0.5"] == "0" and t["0.8"] == "0"
    assert t["1.0"] == "6" and t["1.8"] == "30" and t["2.3"] == "30" and t["2.6"] == "30"
    assert t["3.0"] == "18" and t["3.6"] == "0" and t["4.0"] == "0" and t["4.9"] == "15"
    assert t["fit"] == "0"


def test_pool_allocator(exe):
    m = re.search(r"pool used before (\d+) after (\d+) peak (\d+) failed (\d+)", _run("%s --pool" % exe))
    assert m and m.group(1) == m.group(2) and int(m.group(4)) == 0


def test_menu_session_saves_with_comments(exe, tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    shutil.copy(os.path.join(PACK, "cfg", "player_en.cfg"), cfg / "ps2rc.cfg")
    shutil.copy(os.path.join(PACK, "cfg", "dev.cfg"), cfg / "ps2rc_dev.cfg")
    before = (cfg / "ps2rc.cfg").read_text(encoding="utf-8").splitlines()
    dev_before = (cfg / "ps2rc_dev.cfg").read_text(encoding="utf-8").splitlines()
    # Graphics: draw distance +2 steps, fog limit +3; World: peds +1; Controls (after the layout): modifier, L2;
    # Developer: verbose +1;
    # save (START), Square help, close
    script = ("w5,R,R,shot,D,D,R,R,R,RT,R,shot,RT,RT,D,R,D,D,R,shot,RT,RT,D,D,D,D,D,D,R,shot,S,shot,O,ST,"
              "shot,O")
    p = config.posix_path(str(tmp_path))
    _run("HLE_CFG_DIR='%s/cfg' %s '%s' '%s' 320" % (p, exe, p, script))
    log = (tmp_path / "log.txt").read_text(encoding="utf-8")
    assert "menu: draw_distance 40 -> 45 (tune_far=45, live)" in log
    assert "menu: draw_distance 45 -> 50 (tune_far=50, live)" in log
    assert "menu: fog_limit 90 -> 95 (tune_fogmax=95, live)" in log
    assert "menu: peds 25 -> 30 (tune_peds=30, after restart)" in log    # densities: after a restart
    assert "menu: modifier select -> l" in log and "menu: map_l2 select+l -> l+r" in log
    assert re.search(r"menu: saved \d+ setting\(s\): ps2rc.cfg 5, ps2rc_dev.cfg 1", log), log
    res = dict(kv.split("=") for kv in log.strip().splitlines()[-1].split()[1:])
    assert res["rc"] == "0" and res["failed"] == "0" and int(res["saves"]) == 1
    after = (cfg / "ps2rc.cfg").read_text(encoding="utf-8").splitlines()
    # every comment line kept, the values changed in place, new keys after their commented template line
    assert [ln for ln in before if ln.startswith("#")] == [ln for ln in after if ln.startswith("#")]
    assert "draw_distance=50" in after and "fog_limit=95" in after and "peds=30" in after
    assert after[after.index("#modifier=select") + 1] == "modifier=l"
    assert after[after.index("#map_l2=select+l") + 1] == "map_l2=l+r"
    assert len(after) == len(before) + 2
    dev_after = (cfg / "ps2rc_dev.cfg").read_text(encoding="utf-8").splitlines()
    assert dev_after[:len(dev_before)] == [("verbose=1" if ln == "verbose=0" else ln) for ln in dev_before]
    shots = sorted(tmp_path.glob("shot_*.ppm"))
    assert len(shots) == 6
    for s in shots:                                   # a real frame: many colours, the text is there
        data = s.read_bytes()[15:]
        assert len(set(data[i:i + 3] for i in range(0, len(data), 3 * 97))) > 40


@pytest.mark.parametrize("template,pad_kb", [("player_ru.cfg", 0), ("player_en.cfg", 70)])
def test_menu_saves_big_player_file(exe, tmp_path, template, pad_kb):
    # the 0.2.0 Russian template is over 24 KB: a fixed 24 KB buffer refused it ("too long"); the buffers now grow
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    text = open(os.path.join(PACK, "cfg", template), encoding="utf-8").read()
    text += "".join("# padding line %05d for a long settings file\n" % i for i in range(pad_kb * 1024 // 44))
    (cfg / "ps2rc.cfg").write_text(text, encoding="utf-8", newline="\n")
    shutil.copy(os.path.join(PACK, "cfg", "dev.cfg"), cfg / "ps2rc_dev.cfg")
    assert len(text.encode("utf-8")) > 24 * 1024
    p = config.posix_path(str(tmp_path))
    _run("HLE_CFG_DIR='%s/cfg' %s '%s' '%s' 320" % (p, exe, p, "w5,R,ST,O"))
    log = (tmp_path / "log.txt").read_text(encoding="utf-8")
    assert "save FAILED" not in log, log
    assert re.search(r"menu: saved 1 setting\(s\): ps2rc.cfg 1, ps2rc_dev.cfg 0", log), log
    after = (cfg / "ps2rc.cfg").read_text(encoding="utf-8")
    assert after.count("\n") == text.count("\n") and "draw_distance=45" in after
