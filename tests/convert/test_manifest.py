"""The PSP folder's manifest, the folder check, the update and the player's settings file (synthetic disc, synthetic
pack with cfg templates, the compiler steps replaced): a damaged byte is found, the update fixes only what is wrong
and keeps the player's settings and saves."""
import os

import pytest

from fakepack import PACK, make_repo
from isobuild import build_iso, ps2_files
from ps2rc.convert import cfgfile, pipeline
from ps2rc.convert import manifest as mf
from ps2rc.convert.pipeline import Conversion, Options
from ps2rc.convert.steps import ConvertError, Progress

TPL_EN = """# Game settings (test)
# draw_distance - how far. 10 to 100. Ours: 40.
draw_distance=40
# peds - pedestrians. Ours: 25.
peds=25
# intro_movies - 1 = play, 0 = skip.
intro_movies=1
sound=1
# select_up - remove the "#" to use it.
#select_up=r3
"""
TPL_RU = TPL_EN.replace("# Game settings (test)", "# Settings in the second language (test)")
DEV = "# dev\nvcallms=hle\nx_pace=2\n"


class FakeTc:
    kind, description, version = "native", "fake toolchain", "15.2.0"


def _fake_build(monkeypatch):
    from ps2rc.convert import build as bmod

    def prepare(self, trans):
        for d in ("gen", "hle"):
            os.makedirs(os.path.join(self.bdir, d), exist_ok=True)
        open(os.path.join(self.bdir, "prepare.json"), "w").write("{}")

    def compile_(self):
        os.makedirs(os.path.join(self.bdir, "obj"), exist_ok=True)
        open(os.path.join(self.bdir, "obj", "a.o"), "wb").write(b"o")
        return []

    def link(self):
        for n in ("poc6.elf", "poc6_strip.elf"):
            open(os.path.join(self.bdir, n), "wb").write(b"\x7fELF" + bytes(60))

    def prx(self):
        os.makedirs(os.path.join(self.bdir, "prx"), exist_ok=True)
        open(os.path.join(self.bdir, "prx", "ps2rc_exc.prx"), "wb").write(b"prx")
    monkeypatch.setattr(bmod.Builder, "prepare", prepare)
    monkeypatch.setattr(bmod.Builder, "compile", compile_)
    monkeypatch.setattr(bmod.Builder, "link", link)
    monkeypatch.setattr(bmod.Builder, "build_prx", prx)
    monkeypatch.setattr(bmod, "prx_dir", lambda repo: repo)


def _templates(repo, en=TPL_EN, ru=TPL_RU, dev=DEV):
    pk = os.path.join(repo, "games", PACK)
    os.makedirs(os.path.join(pk, "cfg"), exist_ok=True)
    for name, text in (("player_en.cfg", en), ("player_ru.cfg", ru), ("dev.cfg", dev)):
        with open(os.path.join(pk, "cfg", name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    toml = open(os.path.join(pk, "pack.toml"), encoding="utf-8").read()
    if "[cfg]" not in toml:
        with open(os.path.join(pk, "pack.toml"), "a", encoding="utf-8", newline="\n") as f:
            f.write('[cfg]\nplayer = { en = "cfg/player_en.cfg", ru = "cfg/player_ru.cfg" }\ndev = "cfg/dev.cfg"\n'
                    'aliases = { movies = "intro_movies", audio = "sound" }\n')
    conv = os.path.join(pk, "convert.toml")
    s = open(conv, encoding="utf-8").read()
    if "runtime_files" not in s:
        s = s.replace('[stage.cfg]', 'runtime_files = ["memcard/*", "session*.log"]\n[stage.cfg]')
        open(conv, "w", encoding="utf-8", newline="\n").write(s)


@pytest.fixture
def env(tmp_path, monkeypatch):
    files = ps2_files()
    iso = tmp_path / "game.iso"
    iso.write_bytes(build_iso(files))
    repo = make_repo(str(tmp_path / "repo"), files["SLES_123.45"])
    _templates(repo)
    monkeypatch.setattr(pipeline.tcmod, "detect", lambda *a, **k: (FakeTc(), ["fake"]))
    for k in ("PS2RC_WORK", "PS2RC_DISC", "PS2RC_ANALYSIS", "PS2RC_PACK", "PS2RC_LOCAL"):
        monkeypatch.delenv(k, raising=False)
    _fake_build(monkeypatch)
    return str(iso), repo, str(tmp_path / "out")


def _conv(iso, repo, out, **kw):
    prog = Progress()
    ev = []
    prog.listeners.append(ev.append)
    res = Conversion(Options(iso, out, repo=repo, audio="all", **kw), prog).run()
    prog.close()
    return res, ev


# ------------------------------------------------------------------ the settings file
def test_render_puts_values_on_their_lines():
    t = cfgfile.render_player(TPL_EN, {"peds": "50", "select_up": "l3", "new_key": "7"}, [("x_extra", "1")])
    assert "\npeds=50\n" in t and "\nselect_up=l3\n" in t and "#select_up" not in t
    assert t.index("new_key=7") > t.index("sound=1") and cfgfile.ADDED_SECTION in t
    assert cfgfile.parse(t)["x_extra"] == "1" and t.count("draw_distance=") == 1
    assert cfgfile.render_player(TPL_EN, {"peds": "25"}) == TPL_EN          # same value: byte-identical


def test_merge_keeps_player_values_adds_and_drops():
    old = cfgfile.render_player(TPL_EN, {"peds": "70"}) + "my_key=5\n"
    new_tpl = TPL_EN.replace("sound=1\n", "") + "# fps_cap - frames. Ours: 30.\nfps_cap=30\n"
    text, info = cfgfile.merge_player(old, new_tpl, cfgfile.keys(TPL_EN))
    v = cfgfile.parse(text)
    assert v["peds"] == "70" and v["fps_cap"] == "30" and v["my_key"] == "5" and "sound" not in v
    assert "#sound=1" in text and cfgfile.OLD_SECTION in text and cfgfile.OWN_SECTION in text
    assert info["kept"] == ["peds"] and info["added"] == ["fps_cap"] and info["dropped"] == ["sound"]
    # a key forgotten (the converter's own earlier decision): the new template's value applies
    text2, _ = cfgfile.merge_player(cfgfile.render_player(TPL_EN, {"intro_movies": "0"}), TPL_EN,
                                    cfgfile.keys(TPL_EN), forget={"intro_movies"})
    assert cfgfile.parse(text2)["intro_movies"] == "1"


def test_player_cfg_template_api(env):
    _iso, repo, _out = env
    from ps2rc.convert.identify import list_packs
    pk = dict(list_packs(repo))[PACK]
    assert cfgfile.player_cfg_template(pk, lang="ru") == TPL_RU
    assert cfgfile.player_cfg_template(pk, lang="xx") == TPL_EN            # unknown language: the default
    assert cfgfile.languages(pk) == ["en", "ru"]


# ------------------------------------------------------------------ conversion, check, update
def test_cfg_files_language_and_settings(env):
    iso, repo, out = env
    res, _ev = _conv(iso, repo, out, lang="ru", settings={"draw_distance": "60"})
    folder = res["folder"]
    cfg = open(os.path.join(folder, "test.cfg"), encoding="utf-8").read()
    assert cfg.startswith("# Settings in the second language") and "\ndraw_distance=60\n" in cfg
    assert "\nintro_movies=0\n" in cfg          # no movies in this pack: the converter's decision on its own line
    assert open(os.path.join(folder, "test_dev.cfg")).read() == DEV
    m = mf.load(folder)
    assert m["cfg"]["lang"] == "ru" and m["files"]["test.cfg"]["kind"] == "user"
    assert m["files"]["disc/DATA/A.DAT"]["kind"] == "game_data" and m["files"]["EBOOT.PBP"]["kind"] == "generated"
    assert m["game"]["serial"] == "SLES-12345" and len(m["disc"]["data_sha256"]) == 64
    with pytest.raises(ConvertError):
        _conv(iso, repo, out + "2", lang="de")


def test_corrupt_byte_found_and_update_fixes_only_it(env):
    iso, repo, out = env
    res, _ev = _conv(iso, repo, out)
    folder = res["folder"]
    rep = mf.verify_folder(folder)
    assert rep.ok and rep.files > 8 and not rep.extra, rep.text()
    # damage: one byte of a data file, a missing file; the player's edits, a save, a log, a stray file
    a = os.path.join(folder, "disc", "DATA", "A.DAT")
    data = bytearray(open(a, "rb").read())
    data[100] ^= 1
    open(a, "wb").write(bytes(data))
    os.remove(os.path.join(folder, "disc", "MODELS", "Y.TEX"))
    cfgp = os.path.join(folder, "test.cfg")
    txt = open(cfgp, encoding="utf-8").read().replace("peds=25", "peds=70") + "my_key=5\n"
    open(cfgp, "w", encoding="utf-8", newline="\n").write(txt)
    os.makedirs(os.path.join(folder, "memcard", "slot1"))
    save = os.path.join(folder, "memcard", "slot1", "SAVE0")
    open(save, "wb").write(b"my save")
    open(os.path.join(folder, "session.log"), "w").write("log")
    open(os.path.join(folder, "stray.txt"), "w").write("?")
    rep = mf.verify_folder(folder)
    assert rep.corrupt == ["disc/DATA/A.DAT"] and rep.missing == ["disc/MODELS/Y.TEX"]
    assert rep.user_changed == ["test.cfg"] and rep.extra == ["stray.txt"] and not rep.ok
    assert "Damaged" in rep.text() and "A.DAT" in rep.text()
    # the plan without the disc: copies needed, the settings kept
    plan = mf.plan_update(folder, repo=repo)
    acts = {p: a for p, a, _r, _d in plan.actions}
    assert acts["disc/DATA/A.DAT"] == "copy" and acts["disc/MODELS/Y.TEX"] == "copy" and plan.needs_disc
    assert acts["test.cfg"] == "keep" and acts["disc/DATA/SUB/B.DAT"] == "keep"
    # dry run with the disc: nothing written
    res_d, _ = _conv(iso, repo, out, update=True, dry_run=True)
    assert "disc/DATA/A.DAT" in res_d["plan_text"] and not mf.verify_folder(folder).ok
    # a newer pack: a new setting, one dropped
    _templates(repo, en=TPL_EN.replace("sound=1\n", "") + "fps_cap=30\n")
    res_u, ev = _conv(iso, repo, out, update=True)
    rep = mf.verify_folder(folder)
    assert rep.ok, rep.text()
    assert open(a, "rb").read() == b"a" * 5000
    v = cfgfile.parse(open(cfgp, encoding="utf-8").read())
    assert v["peds"] == "70" and v["my_key"] == "5" and v["fps_cap"] == "30"
    assert v["sound"] == "0"                         # no sound model in this pack: the converter's decision
    assert open(save, "rb").read() == b"my save" and os.path.exists(os.path.join(folder, "session.log"))
    up = res_u["update"]
    assert up["rewritten"] == 0 and up["written"] <= 8         # not the whole folder again
    assert any(e[0] == "info" and "your values kept" in e[1] for e in ev)


def test_update_refuses_another_game(env, tmp_path):
    iso, repo, out = env
    res, _ = _conv(iso, repo, out)
    m = mf.load(res["folder"])
    m["game"]["version"] = "9.99"
    mf.write(res["folder"], m)
    with pytest.raises(ConvertError) as e:
        _conv(iso, repo, out, update=True)
    assert e.value.code == "update"


def test_readback_rewrites_a_bad_write(env, monkeypatch):
    iso, repo, out = env
    from ps2rc.convert import stage as stmod
    calls = {"n": 0}
    real = stmod.Stager._readback

    def flaky(self, path, sha):
        calls["n"] += 1
        if calls["n"] == 2:                          # the second file written reads back wrong once
            return False
        return real(self, path, sha)
    monkeypatch.setattr(stmod.Stager, "_readback", flaky)
    res, ev = _conv(iso, repo, out, verify_writes=True)
    assert any(e[0] == "warn" and "read back wrong" in e[1] for e in ev)
    assert mf.verify_folder(res["folder"]).ok


def test_cli_verify_exit_codes(env, capsys):
    iso, repo, out = env
    res, _ = _conv(iso, repo, out)
    from ps2rc.convert.__main__ import main
    assert main(["verify", res["folder"]]) == 0
    assert "all" in capsys.readouterr().out
    os.remove(os.path.join(res["folder"], "disc", "DATA", "A.DAT"))
    assert main(["verify", res["folder"]]) == 3
    assert "Missing" in capsys.readouterr().out
