"""The conversion run up to the build (synthetic disc, synthetic pack, fake commands, fake toolchain): environment of
the commands, resumability, re-runs after a change, errors and the command line."""
import json
import os
import sys

import pytest

from fakepack import make_repo
from isobuild import build_iso, ps2_files
from ps2rc.convert import pipeline
from ps2rc.convert.pipeline import Conversion, Options
from ps2rc.convert.steps import ConvertError, Progress


class FakeTc:
    kind, description, version = "native", "fake toolchain", "15.2.0"


@pytest.fixture
def env(tmp_path, monkeypatch):
    files = ps2_files()
    iso = tmp_path / "game.iso"
    iso.write_bytes(build_iso(files))
    repo = make_repo(str(tmp_path / "repo"), files["SLES_123.45"])
    monkeypatch.setattr(pipeline.tcmod, "detect", lambda *a, **k: (FakeTc(), ["fake"]))
    for k in ("PS2RC_WORK", "PS2RC_DISC", "PS2RC_ANALYSIS", "PS2RC_PACK", "PS2RC_LOCAL"):
        monkeypatch.delenv(k, raising=False)
    return str(iso), repo, str(tmp_path / "out")


def _run(iso, repo, out, **kw):
    prog = Progress()
    events = []
    prog.listeners.append(events.append)
    res = Conversion(Options(iso, out, repo=repo, stop_after="extras", **kw), prog).run()
    prog.close()
    return res, events


def _done(events):
    return {ev[1]: ev[3] for ev in events if ev[0] == "done"}


def test_first_run_then_cached(env):
    iso, repo, out = env
    res, ev = _run(iso, repo, out)
    work = os.path.join(os.environ["LOCALAPPDATA"], "PS2RC", "work")
    assert not os.path.exists(os.path.join(out, "_ps2rc_work"))      # never inside the output
    done = _done(ev)
    assert done["extract"] is False and done["analysis"] is False and done["translate"] is False
    assert open(os.path.join(work, "trans", "unit_000.c")).read().startswith("int f(void)")
    assert os.path.exists(os.path.join(work, "disc", "SLES_123.45"))
    assert os.path.exists(os.path.join(work, "discmap.txt")) and os.path.exists(os.path.join(work, "iso_meta.bin"))
    assert res["estimate"]["stage_bytes"] > 0
    local = open(os.path.join(work, "local.toml")).read()
    assert "[games.\"SLES-12345_v1.00\"]" in local
    # second run: every step kept
    res2, ev2 = _run(iso, repo, out)
    done2 = _done(ev2)
    assert all(done2[k] for k in ("extract", "analysis", "translate", "extras"))
    assert open(os.path.join(work, "counter_translate")).read() == "x"
    # an output removed -> that step runs again
    os.remove(os.path.join(work, "phase6psp", "trapset.txt"))
    _res3, ev3 = _run(iso, repo, out)
    assert _done(ev3)["extras"] is False and _done(ev3)["translate"] is True
    # --restart runs everything
    _r4, ev4 = _run(iso, repo, out, restart=True)
    assert not any(_done(ev4).values())
    assert open(os.path.join(work, "counter_translate")).read() == "xx"


def test_recipe_change_reruns_translation(env):
    iso, repo, out = env
    _run(iso, repo, out)
    p = os.path.join(repo, "games", "SLES-12345_v1.00", "convert.toml")
    with open(p, "a") as f:
        f.write("\n# changed\n")
    _r, ev = _run(iso, repo, out)
    assert _done(ev)["translate"] is False


def test_failing_command_reports_log(tmp_path, monkeypatch):
    files = ps2_files()
    iso = tmp_path / "g.iso"
    iso.write_bytes(build_iso(files))
    repo = make_repo(str(tmp_path / "repo"), files["SLES_123.45"], analysis_cmd='["{repo}/scripts/fake.py", "fail"]')
    monkeypatch.setattr(pipeline.tcmod, "detect", lambda *a, **k: (FakeTc(), ["fake"]))
    with pytest.raises(ConvertError) as e:
        _run(str(iso), repo, str(tmp_path / "out"))
    assert e.value.code == "step_failed" and e.value.log.endswith("analysis.log")
    assert any("on purpose" in t for t in e.value.tail)


def test_estimate_only_and_disk_check(env, monkeypatch):
    iso, repo, out = env
    res, ev = _run(iso, repo, out, estimate_only=True)
    assert "estimate" in res and not os.path.exists(os.path.join(os.environ["LOCALAPPDATA"], "PS2RC", "work", "disc", "SLES_123.45"))
    from ps2rc.convert import estimate as est
    monkeypatch.setattr(est.shutil, "disk_usage", lambda p: type("U", (), {"free": 1000})())
    with pytest.raises(ConvertError) as e:
        _run(iso, repo, out, estimate_only=True)
    assert e.value.code == "disk"


def test_no_pack_message(tmp_path, monkeypatch):
    iso = tmp_path / "g.iso"
    iso.write_bytes(build_iso(ps2_files(boot="SCES_000.00")))
    repo = make_repo(str(tmp_path / "repo"), b"x")
    with pytest.raises(ConvertError) as e:
        _run(str(iso), repo, str(tmp_path / "out"))
    assert e.value.code == "no_pack" and "SCES-00000" in e.value.message


def test_cli_errors(tmp_path, capsys):
    from ps2rc.convert.__main__ import main
    assert main([str(tmp_path / "none.iso"), "--out", str(tmp_path / "o"), "--icon0", str(tmp_path / "x.png")]) == 2
    (tmp_path / "bad.iso").write_bytes(b"\0" * 50000)
    assert main([str(tmp_path / "bad.iso"), "--out", str(tmp_path / "o"), "-q"]) == 1
    err = capsys.readouterr().err
    assert "ERROR (input)" in err and "not an ISO9660 image" in err
    assert main([str(tmp_path / "bad.iso"), "--out", str(tmp_path / "o"), "--cfg", "novalue"]) == 2


def test_cli_runs_with_fake_pack(env, capsys, monkeypatch):
    iso, repo, out = env
    from ps2rc.convert import __main__ as cli
    real = cli.parser

    def parser():
        ap = real()
        ap.set_defaults(repo=repo)
        return ap
    monkeypatch.setattr(cli, "parser", parser)
    orig_options = pipeline.Options

    def opts(*a, **k):
        k["repo"] = repo
        return orig_options(*a, **k)
    monkeypatch.setattr(pipeline, "Options", opts)
    rc = cli.main([iso, "--out", out, "--stop-after", "extras", "--movies", "skip", "--audio", "min"])
    text = capsys.readouterr().out
    assert rc == 0 and "[ 4/14] Translate the program to C" in text and "stopped after step extras" in text


def test_full_run_with_fake_build(env, monkeypatch):
    """identify .. verify with the compiler steps replaced: the EBOOT carries the player's title and icon, the
    folder holds the staged files (replacement applied), the cfg, the README, the manifest; a second run keeps
    everything."""
    iso, repo, out = env
    from ps2rc.convert import build as bmod
    from ps2rc.pbp import Pbp, Sfo, placeholder

    def prepare(self, trans):
        os.makedirs(os.path.join(self.bdir, "gen"), exist_ok=True)
        os.makedirs(os.path.join(self.bdir, "hle"), exist_ok=True)
        open(os.path.join(self.bdir, "prepare.json"), "w").write("{}")

    def compile_(self):
        os.makedirs(os.path.join(self.bdir, "obj"), exist_ok=True)
        open(os.path.join(self.bdir, "obj", "a.o"), "wb").write(b"o")
        return []

    def link(self):
        elf = b"\x7fELF" + bytes(60)
        for n in ("ps2rc.elf", "ps2rc_strip.elf"):
            open(os.path.join(self.bdir, n), "wb").write(elf)
        return os.path.join(self.bdir, "ps2rc.elf"), os.path.join(self.bdir, "ps2rc_strip.elf")

    def prx(self):
        os.makedirs(os.path.join(self.bdir, "prx"), exist_ok=True)
        p = os.path.join(self.bdir, "prx", "ps2rc_exc.prx")
        open(p, "wb").write(b"prx")
        return p
    monkeypatch.setattr(bmod.Builder, "prepare", prepare)
    monkeypatch.setattr(bmod.Builder, "compile", compile_)
    monkeypatch.setattr(bmod.Builder, "link", link)
    monkeypatch.setattr(bmod.Builder, "build_prx", prx)
    monkeypatch.setattr(pipeline, "prx_dir", lambda repo: repo, raising=False)
    from ps2rc.convert import build as b2
    monkeypatch.setattr(b2, "prx_dir", lambda repo: repo)
    icon = os.path.join(out + "_assets", "icon.png")
    os.makedirs(os.path.dirname(icon))
    open(icon, "wb").write(placeholder.icon0("T"))
    prog = Progress()
    events = []
    prog.listeners.append(events.append)
    res = Conversion(Options(iso, out, repo=repo, title="My PSP title", icon0=icon, audio="all", movies="auto"),
                     prog).run()
    folder = res["folder"]
    assert os.path.basename(folder) == "PS2RC_SLES12345"          # PS2RC_<serial>, as in the window
    p = Pbp.read(os.path.join(folder, "EBOOT.PBP"))
    sfo = Sfo.parse(p["PARAM.SFO"])
    assert sfo.get("TITLE") == "My PSP title" and sfo.get("DISC_ID") == "PSRC12345" and sfo.get("MEMSIZE") == 1
    assert p["ICON0.PNG"] == placeholder.icon0("T")
    assert open(os.path.join(folder, "disc", "CONFIG.INI"), "rb").read().startswith(b"memory\t\t\t9000")
    assert os.path.exists(os.path.join(folder, "disc", "AUDIO", "SFX", "GEN01.PAK"))
    assert not os.path.exists(os.path.join(folder, "disc", "MODELS", "X.IMG"))
    cfg = open(os.path.join(folder, "test.cfg")).read()
    assert "render=ge" in cfg and "movies=0" in cfg and "audio=" in cfg
    assert os.path.exists(os.path.join(folder, "README_PS2RC.txt"))
    assert "EBOOT.PBP" in open(os.path.join(folder, "MANIFEST.md5")).read()
    assert any(ev[0] == "info" and "movies: skipped" in ev[1] for ev in events)
    prog2 = Progress()
    ev2 = []
    prog2.listeners.append(ev2.append)
    Conversion(Options(iso, out, repo=repo, title="My PSP title", icon0=icon, audio="all"), prog2).run()
    kept = {e[1] for e in ev2 if e[0] == "done" and e[3]}
    assert {"extract", "analysis", "translate", "extras", "prepare", "compile", "link", "prx", "eboot", "stage"} <= kept
    # another title: only the EBOOT and the stage step run again
    prog3 = Progress()
    ev3 = []
    prog3.listeners.append(ev3.append)
    Conversion(Options(iso, out, repo=repo, title="Other", icon0=icon, audio="all"), prog3).run()
    ran = {e[1] for e in ev3 if e[0] == "done" and not e[3]}
    assert {"eboot", "stage"} <= ran and "compile" not in ran
