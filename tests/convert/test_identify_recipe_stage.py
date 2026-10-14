"""Identification, the pack recipe and the output folder staging (synthetic disc + synthetic pack)."""
import os

import pytest

from fakepack import PACK, make_repo
from isobuild import build_iso, ps2_files
from ps2rc.convert import disc as discmod
from ps2rc.convert.identify import IdentifyError, identify, list_packs
from ps2rc.convert.recipe import Recipe, RecipeError, apply_replacement
from ps2rc.convert.stage import Stager, manifest
from ps2rc.convert.steps import Progress


@pytest.fixture
def setup(tmp_path):
    files = ps2_files()
    iso = tmp_path / "game.iso"
    iso.write_bytes(build_iso(files))
    repo = make_repo(str(tmp_path / "repo"), files["SLES_123.45"])
    d = discmod.open_disc(str(iso))
    yield d, repo, files, tmp_path
    d.close()


def test_identify_ok(setup):
    d, repo, files, _ = setup
    ident = identify(d, repo)
    assert ident.pack.name == PACK and ident.serial == "SLES-12345" and ident.version == "1.00"
    assert ident.elf_size == len(files["SLES_123.45"])
    assert [n for n, _p in list_packs(repo)] == [PACK]


def test_identify_wrong_version(tmp_path, setup):
    _d, repo, files, _ = setup
    other = dict(files)
    other["SLES_123.45"] = b"\x7fELF" + b"\1" * 4000             # same game, another program
    iso = tmp_path / "v2.iso"
    iso.write_bytes(build_iso(other))
    d = discmod.open_disc(str(iso))
    with pytest.raises(IdentifyError) as e:
        identify(d, repo)
    assert e.value.code == "version" and PACK in e.value.hint
    d.close()


def test_identify_unknown_game_and_not_ps2(tmp_path, setup):
    _d, repo, files, _ = setup
    other = ps2_files(boot="SLUS_000.00")
    iso = tmp_path / "x.iso"
    iso.write_bytes(build_iso(other))
    d = discmod.open_disc(str(iso))
    with pytest.raises(IdentifyError) as e:
        identify(d, repo)
    assert e.value.code == "no_pack" and "SLUS-00000" in e.value.message and PACK in e.value.hint
    d.close()
    nocnf = {k: v for k, v in files.items() if k != "SYSTEM.CNF"}
    iso2 = tmp_path / "y.iso"
    iso2.write_bytes(build_iso(nocnf))
    d2 = discmod.open_disc(str(iso2))
    with pytest.raises(IdentifyError) as e:
        identify(d2, repo)
    assert e.value.code == "not_ps2"
    d2.close()
    with pytest.raises(IdentifyError) as e:
        identify(discmod.open_disc(str(tmp_path / "game.iso")), repo, pack_name="nope")
    assert e.value.code == "no_pack"


def test_recipe_selection_and_expand(setup):
    d, repo, _files, _ = setup
    r = Recipe(identify(d, repo).pack)
    assert r.source == {"convert": "convert.toml", "stage": "convert.toml", "disc": None}
    paths = [f.iso for f in d.files]
    sel, missing = r.stage_selection(paths, "all", "SLES_123.45")
    assert missing == []
    assert sel == {"DATA/A.DAT", "DATA/SUB/B.DAT", "MODELS/Y.TEX", "CONFIG.INI", "AUDIO/SFX/GEN01.PAK"}
    sel_none, _ = r.stage_selection(paths, None, "SLES_123.45")
    assert "AUDIO/SFX/GEN01.PAK" not in sel_none
    ctx = {"work": "/w", "repo": "/r", "disc": "/d", "analysis": "/a", "translation": "/t"}
    assert r.expand("--out", ctx) == "--out"
    assert os.path.normpath("/r/x.py") == r.expand("{repo}/x.py", ctx)
    with pytest.raises(RecipeError):
        r.expand("{translation}", {"work": "/w"})
    p, dev = r.pack_file("analysis/matches.json")
    assert p and not dev
    assert r.pack_file("analysis/none.json") == (None, False)


def test_replacement_rules():
    assert apply_replacement(b"a memory\t\t13550\r\n b", b"memory\t\t13550\r\n", b"memory\t\t\t9000\r\n", "S") == \
        b"a memory\t\t\t9000\r\n b"
    with pytest.raises(RecipeError, match="size"):
        apply_replacement(b"abc", b"b", b"xx", "S")
    with pytest.raises(RecipeError, match="0 times"):
        apply_replacement(b"abc", b"z", b"y", "S")


def test_stager_copies_replaces_and_cleans(setup):
    d, repo, files, tmp = setup
    r = Recipe(identify(d, repo).pack)
    folder = str(tmp / "out" / "PS2RC_TEST")
    prog = Progress()
    seen = []
    prog.listeners.append(seen.append)
    st = Stager(d, r, folder, prog)
    stale = os.path.join(folder, "disc", "OLD.BIN")
    os.makedirs(os.path.dirname(stale), exist_ok=True)
    open(stale, "wb").write(b"old")
    sel, total, lines = st.stage_disc("all", "SLES_123.45")
    assert not os.path.exists(stale)
    assert open(os.path.join(folder, "disc", "CONFIG.INI"), "rb").read() == b"memory\t\t\t9000\r\nother 1\r\n"
    assert open(os.path.join(folder, "disc", "DATA", "A.DAT"), "rb").read() == files["DATA/A.DAT"]
    assert any("REPLACED" in ln for ln in lines) and total == sum(f.size for f in sel)
    assert st.plan_bytes("all", "SLES_123.45") == total
    assert any(ev[0] == "pct" for ev in seen)
    sel2, _t, _l = st.stage_disc("all", "SLES_123.45")          # second run: nothing to copy, same result
    assert [f.iso for f in sel2] == [f.iso for f in sel]
    n, b = manifest(folder)
    rows = open(os.path.join(folder, "MANIFEST.md5")).read().split("\n")
    assert n == len(sel) and len([x for x in rows if x]) == n


def test_stager_missing_file(setup):
    d, repo, _files, tmp = setup
    r = Recipe(identify(d, repo).pack)
    r.data["stage"]["files"] = ["NOT/THERE.BIN"]
    with pytest.raises(Exception, match="missing on this disc"):
        Stager(d, r, str(tmp / "o"), Progress()).selection(None, "SLES_123.45")
