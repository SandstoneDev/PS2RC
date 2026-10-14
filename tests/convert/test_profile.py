"""Pack build profiles ([profile.<name>] of pack.toml): sources left out / added / include-only, hooks left out of the
build's hooks.def copy, their zones without PZF_GUEST, natives off, the generators' include-only outputs."""
import os

from ps2rc.convert import build as bmod


class FakePack:
    def __init__(self, d, tab):
        self.dir, self.tab = d, tab

    def get(self, key, default=None):
        cur = self.tab
        for k in key.split("."):
            if not isinstance(cur, dict) or k not in cur:
                return default
            cur = cur[k]
        return cur

    def expand(self, s):
        return os.path.join(self.dir, s)

    def path(self, key, default=None):
        v = self.get(key)
        return os.path.join(self.dir, v) if v else default


def _write(p, text=""):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", newline="\n") as f:
        f.write(text)


def test_profile_sources(tmp_path):
    pk = str(tmp_path / "pack")
    for n in ("src/a.c", "src/world.c", "src/a.h", "glue/unity.c", "inc/gen_inc.c", "inc/gen_inc.h"):
        _write(os.path.join(pk, n), "/* x */\n")
    pack = FakePack(pk, {})
    copies = bmod._dir_files(os.path.join(pk, "src"))
    prof = {"exclude_sources": ["src/world.c"], "extra_sources": ["glue/unity.c"], "include_sources": ["inc"],
            "defines": ["HLE_CRT0_DATA"]}
    out, defs = bmod.profile_sources(copies, ["-DX"], pack, prof, str(tmp_path))
    names = [n for _s, n in out]
    assert "world.c" not in names and "a.c" in names and "unity.c" in names
    assert "rt/gen_inc.c" in names and "rt/gen_inc.h" in names and "gen_inc.c" not in names
    assert defs == ["-DX", "-DHLE_CRT0_DATA"]
    assert bmod.profile_sources(copies, ["-DX"], pack, {}, str(tmp_path)) == (copies, ["-DX"])


def test_profile_hooks_and_zones(tmp_path):
    pk = str(tmp_path / "pack")
    _write(os.path.join(pk, "hooks.def"), "HOOK(00000108, start)\nHOOK(00200000, World_Process)\n"
                                         "/* comment */\nTRACE(00300000, other, 1)\n")
    _write(os.path.join(pk, "zones.def"), "ZONE(00200000, World_Process, PZF_GUEST|PZF_HW)   /* (I) x */\n"
                                         "ZONE(00300000, other, PZF_GUEST)\n")
    pack = FakePack(pk, {"build": {"hooks": "hooks.def", "zones": "zones.def", "natives": "n.txt"}})
    opts = bmod.BuildOptions(profile={"exclude_hooks": ["World_Process"], "natives": ""})
    b = bmod.Builder.__new__(bmod.Builder)
    b.pack, b.opts, b.bdir = pack, opts, str(tmp_path / "bdir")
    os.makedirs(b.bdir)
    h = open(b.profile_hooks()).read()
    assert "World_Process" not in h and "HOOK(00000108, start)" in h and "TRACE(00300000" in h
    z = open(b.profile_zones()).read()
    assert "ZONE(00200000, World_Process, PZF_HW)   /* (I) x */" in z and "ZONE(00300000, other, PZF_GUEST)" in z
    b.opts = bmod.BuildOptions(profile={})
    assert b.profile_hooks() == os.path.join(pk, "hooks.def") and b.profile_zones() == os.path.join(pk, "zones.def")
