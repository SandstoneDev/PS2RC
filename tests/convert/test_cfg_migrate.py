"""A folder converted by an earlier version whose run options files still have the pack's older name ([runtime]
cfg_legacy): the update carries the player's values into the current name, keeps his old file as <name>.old, removes
the converter's own unchanged developer file, and records it in the manifest. Also: the folder's default name is
PS2RC_<serial> on the command line too, and an update without --folder finds a folder of another name by its
manifest."""
import os

from fakepack import PACK
from ps2rc.convert import cfgfile
from ps2rc.convert import manifest as mf
from test_manifest import DEV, _conv, env  # noqa: F401  (the fixture)

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def _legacy(repo, name="old.cfg"):
    p = os.path.join(repo, "games", PACK, "pack.toml")
    t = open(p, encoding="utf-8").read()
    if "cfg_legacy" not in t:
        t = t.replace('cfg = "test.cfg"\n', 'cfg = "test.cfg"\ncfg_legacy = "%s"\n' % name)
        open(p, "w", encoding="utf-8", newline="\n").write(t)


def _as_earlier_version(folder, edit_dev=False):
    """The folder as the earlier version left it: the files under the older names, in the manifest too."""
    m = mf.load(folder)
    for new, old in (("test.cfg", "old.cfg"), ("test_dev.cfg", "old_dev.cfg")):
        os.replace(os.path.join(folder, new), os.path.join(folder, old))
        m["files"][old] = m["files"].pop(new)
    m["cfg"]["name"], m["cfg"]["dev"] = "old.cfg", "old_dev.cfg"
    mf.write(folder, m)
    p = os.path.join(folder, "old.cfg")
    txt = open(p, encoding="utf-8").read().replace("peds=25", "peds=70") + "my_key=5\n"
    open(p, "w", encoding="utf-8", newline="\n").write(txt)
    if edit_dev:
        with open(os.path.join(folder, "old_dev.cfg"), "a", encoding="utf-8", newline="\n") as f:
            f.write("my_dev=1\n")
    return txt


def test_cfg_moves_and_keep_old(tmp_path):
    d = str(tmp_path)
    assert cfgfile.cfg_moves(d, "new.cfg", None) == [] and cfgfile.cfg_moves(d, "new.cfg", "new.cfg") == []
    (tmp_path / "old.cfg").write_text("a=1")
    (tmp_path / "old_dev.cfg").write_text("d=1")
    assert cfgfile.cfg_moves(d, "new.cfg", "old.cfg") == [("old.cfg", "new.cfg"), ("old_dev.cfg", "new_dev.cfg")]
    (tmp_path / "new.cfg").write_text("a=2")                 # already moved: only the dev file is left
    assert cfgfile.cfg_moves(d, "new.cfg", "old.cfg") == [("old_dev.cfg", "new_dev.cfg")]
    assert cfgfile.keep_old(d, "old.cfg") == "old.cfg.old"
    (tmp_path / "old.cfg").write_text("again")
    assert cfgfile.keep_old(d, "old.cfg") == "old.cfg.old.2"
    assert (tmp_path / "old.cfg.old").read_text() == "a=1" and not (tmp_path / "old.cfg").exists()
    assert all(mf.is_runtime_file(n, cfgfile.KEPT_OLD) for n in ("old.cfg.old", "old.cfg.old.2"))
    assert not mf.is_runtime_file("old.cfg", cfgfile.KEPT_OLD)


def test_default_folder_is_serial(env):  # noqa: F811
    iso, repo, out = env
    res, _ = _conv(iso, repo, out)
    assert os.path.basename(res["folder"]) == "PS2RC_SLES12345"      # not the pack's [stage] folder / module name


def test_update_migrates_old_names(env):  # noqa: F811
    iso, repo, out = env
    _legacy(repo)
    res, _ = _conv(iso, repo, out)
    folder = res["folder"]
    edited = _as_earlier_version(folder)
    # the plan without the disc
    plan = mf.plan_update(folder, repo=repo)
    acts = {p: (a, r) for p, a, r, _d in plan.actions}
    assert acts["test.cfg"][0] == "merge_cfg" and "old.cfg" in acts["test.cfg"][1]
    assert acts["old.cfg"][0] == "rename" and "old.cfg.old" in acts["old.cfg"][1]
    assert acts["old_dev.cfg"][0] == "remove" and "test_dev.cfg" in acts["old_dev.cfg"][1]
    # the plan with the disc (dry run): the same, nothing written
    res_d, _ = _conv(iso, repo, out, update=True, dry_run=True)
    acts = {a["path"]: a["action"] for a in res_d["plan"]["actions"]}
    assert acts["test.cfg"] == "merge_cfg" and acts["old.cfg"] == "rename" and acts["old_dev.cfg"] == "remove"
    assert os.path.isfile(os.path.join(folder, "old.cfg")) and not os.path.exists(os.path.join(folder, "test.cfg"))
    # the update
    _res_u, ev = _conv(iso, repo, out, update=True)
    v = cfgfile.parse(open(os.path.join(folder, "test.cfg"), encoding="utf-8").read())
    assert v["peds"] == "70" and v["my_key"] == "5"                  # the player's values under the new name
    assert open(os.path.join(folder, "old.cfg.old"), encoding="utf-8").read() == edited   # his file, unchanged
    assert not any(os.path.exists(os.path.join(folder, n)) for n in ("old.cfg", "old_dev.cfg"))
    assert open(os.path.join(folder, "test_dev.cfg"), encoding="utf-8").read() == DEV
    assert any(e[0] == "info" and "(from old.cfg)" in e[1] for e in ev)
    m = mf.load(folder)
    assert "old.cfg" not in m["files"] and "old.cfg.old" not in m["files"] and "test.cfg" in m["files"]
    assert m["files"]["test.cfg"]["kind"] == "user"
    mig = {e["from"]: e for e in m["cfg_migrated"]}
    assert mig["old.cfg"]["to"] == "test.cfg" and mig["old.cfg"]["kept_as"] == "old.cfg.old"
    assert mig["old_dev.cfg"]["to"] == "test_dev.cfg" and mig["old_dev.cfg"].get("removed") is True
    rep = mf.verify_folder(folder)
    assert rep.ok and not rep.extra, rep.text()                      # the .old file is not "extra"
    # a second update: nothing left to move, the record and the .old file stay
    _conv(iso, repo, out, update=True)
    m2 = mf.load(folder)
    assert m2["cfg_migrated"] == m["cfg_migrated"]
    assert open(os.path.join(folder, "old.cfg.old"), encoding="utf-8").read() == edited
    assert cfgfile.parse(open(os.path.join(folder, "test.cfg"), encoding="utf-8").read())["peds"] == "70"


def test_update_keeps_an_edited_dev_file(env):  # noqa: F811
    iso, repo, out = env
    _legacy(repo)
    res, _ = _conv(iso, repo, out)
    folder = res["folder"]
    _as_earlier_version(folder, edit_dev=True)
    acts = {p: a for p, a, _r, _d in mf.plan_update(folder, repo=repo).actions}
    assert acts["old_dev.cfg"] == "rename"
    _conv(iso, repo, out, update=True)
    assert "my_dev=1" in open(os.path.join(folder, "old_dev.cfg.old"), encoding="utf-8").read()
    assert open(os.path.join(folder, "test_dev.cfg"), encoding="utf-8").read() == DEV
    mig = {e["from"]: e for e in mf.load(folder)["cfg_migrated"]}
    assert mig["old_dev.cfg"]["kept_as"] == "old_dev.cfg.old"


def test_update_without_folder_finds_it_by_manifest(env):  # noqa: F811
    iso, repo, out = env
    res, _ = _conv(iso, repo, out)
    other = os.path.join(out, "PS2RC_OLDNAME")
    os.replace(res["folder"], other)
    res_u, ev = _conv(iso, repo, out, update=True)
    assert os.path.normcase(res_u["folder"]) == os.path.normcase(other)
    assert not os.path.exists(os.path.join(out, "PS2RC_SLES12345"))
    assert any(e[0] == "info" and "PS2RC_OLDNAME" in e[1] for e in ev)


def test_pack_names():
    """Every game pack of the checkout: the settings file is ps2rc.cfg (an older name only as cfg_legacy, a different
    .cfg name), no fixed PSP folder name (the converter names it after the serial)."""
    import tomllib
    games = os.path.join(ROOT, "games")
    seen = 0
    for name in sorted(os.listdir(games)):
        p = os.path.join(games, name, "pack.toml")
        if name.startswith("_") or not os.path.isfile(p):
            continue
        t = tomllib.load(open(p, "rb"))
        rt, st = t.get("runtime", {}), t.get("stage", {})
        assert rt.get("cfg", "ps2rc.cfg") == "ps2rc.cfg", name
        if "cfg_legacy" in rt:
            assert rt["cfg_legacy"] != "ps2rc.cfg" and rt["cfg_legacy"].endswith(".cfg"), name
        assert "folder" not in st, name
        seen += 1
    assert seen >= 1
