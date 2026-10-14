"""The converter's speed levers: cold-code split, prebuilt object store, md5 while staging (no game data)."""
import hashlib
import json
import os

from ps2rc.convert import build as bmod
from ps2rc.convert import prebuilt
from ps2rc.convert.stage import Md5Cache, manifest
from ps2rc.convert.steps import Progress

UNIT = "\n".join([
    '#include "main.h"', "",
    "__attribute__((cold)) void fn_00000100(R5900Ctx *c)", "{", "    c->r[2].d[0] = 1;", "}", "",
    "void fn_00000110(R5900Ctx *c)", "{", "    c->r[2].d[0] = 2;", "}", "",
    "__attribute__((cold)) void fn_00000120(R5900Ctx *c)", "{", "}", ""])


def test_split_cold(tmp_path):
    g = tmp_path / "gen"
    g.mkdir()
    (g / "main_000.c").write_text(UNIT, newline="")
    (g / "main_001.c").write_text('#include "main.h"\n\nvoid fn_00200000(R5900Ctx *c)\n{\n}\n', newline="")
    (g / "main_tab.c").write_text('#include "main.h"\n\nconst int table[] = {1};\n', newline="")
    n, nb = bmod.split_cold(str(g))
    assert n == 1 and nb > 0
    hot = (g / "main_000.c").read_text()
    cold = (g / "main_000_cold.c").read_text()
    assert "fn_00000110" in hot and "fn_00000100" not in hot
    assert "fn_00000100" in cold and "fn_00000120" in cold and "fn_00000110" not in cold
    assert cold.startswith('#include "main.h"')
    assert not (g / "main_001_cold.c").exists() and not (g / "main_tab_cold.c").exists()
    # every function survives exactly once
    allf = hot + cold
    for f in ("fn_00000100", "fn_00000110", "fn_00000120"):
        assert allf.count("void %s(" % f) == 1
    bmod.split_cold(str(g))                                # a second pass changes nothing
    assert (g / "main_000.c").read_text() == hot and (g / "main_000_cold.c").read_text() == cold


class Tc:
    kind, version, description = "native", "15.2.0", "stub"

    def __init__(self, root):
        self.root = root
        self.ran = []

    def pspdev(self):
        return self.root

    def run_batch(self, jobs, cwd, n, progress=None, name=""):
        for j in jobs:
            self.ran.append(j.label)
            out = os.path.join(cwd, j.label)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(("obj of " + j.argv[-3]).encode())
            j.rc, j.output = 0, ""
        return jobs


class Pack:
    def get(self, k, d=None):
        return d

    def path(self, k, d=None):
        return None


def _bdir(root):
    b = root / "b"
    for d in ("gen", "hle/rt", "obj"):
        (b / d).mkdir(parents=True)
    (b / "gen" / "main_000.c").write_text("int a;")
    (b / "hle" / "hle_main.c").write_text("int b;")
    (b / "hle" / "hle_ctx.S").write_text("")
    (b / "hle" / "hooks.def").write_text("")
    (b / "hle" / "rt" / "r5900_rt.h").write_text("")
    (b / "prepare.json").write_text(json.dumps({"hle_defs": ["-DX"]}))
    return str(b)


def test_prebuilt_store_round_trip(tmp_path):
    # build once with a toolchain installed in one folder, export the runtime objects
    b1 = _bdir(tmp_path / "pc1")
    tc1 = Tc(str(tmp_path / "pc1" / "toolchain"))
    bmod.Builder(str(tmp_path), Pack(), b1, tc1, bmod.BuildOptions(), Progress(), {}, str(tmp_path / "l1")).compile()
    store = str(tmp_path / "store")
    assert prebuilt.export(b1, store) == 2                 # hle_main.o + hle_ctx.o, not the generated unit
    # another PC, toolchain in another folder: the runtime objects come from the store, the game unit compiles
    b2 = _bdir(tmp_path / "pc2")
    tc2 = Tc(str(tmp_path / "pc2" / "elsewhere"))
    bmod.Builder(str(tmp_path), Pack(), b2, tc2, bmod.BuildOptions(prebuilt=[store]), Progress(), {},
                 str(tmp_path / "l2")).compile()
    assert tc2.ran == ["obj/gen/main_000.o"]
    assert open(os.path.join(b2, "obj", "hle", "hle_main.o"), "rb").read() == b"obj of hle/hle_main.c"
    # another compiler version: no reuse
    b3 = _bdir(tmp_path / "pc3")
    tc3 = Tc(str(tmp_path / "pc3" / "t"))
    tc3.version = "16.2.0"
    bmod.Builder(str(tmp_path), Pack(), b3, tc3, bmod.BuildOptions(prebuilt=[store]), Progress(), {},
                 str(tmp_path / "l3")).compile()
    assert sorted(tc3.ran) == ["obj/gen/main_000.o", "obj/hle/hle_ctx.o", "obj/hle/hle_main.o"]


def test_cold_units_get_their_level(tmp_path):
    b = _bdir(tmp_path)
    (tmp_path / "b" / "gen" / "main_000_cold.c").write_text("int c;")
    builder = bmod.Builder(str(tmp_path), Pack(), b, Tc(str(tmp_path)), bmod.BuildOptions(cold_opt="O1"), Progress(),
                           {}, str(tmp_path / "l"))
    jobs, _n = builder.compile_jobs()
    argv = {j.label: j.argv for j, _s, _d in jobs}
    assert "-O1" in argv["obj/gen/main_000_cold.o"] and "-O1" not in argv["obj/gen/main_000.o"]
    assert argv["obj/gen/main_000_cold.o"].index("-O1") > argv["obj/gen/main_000_cold.o"].index("-O2")


def test_manifest_uses_the_stage_md5(tmp_path):
    f = tmp_path / "folder"
    f.mkdir()
    (f / "a.bin").write_bytes(b"abc")
    cache = Md5Cache(str(tmp_path / "md5.json"))
    cache.put(str(f / "a.bin"), "0" * 32)                  # what the stage computed while writing
    manifest(str(f), None, cache)
    assert "0" * 32 + "  a.bin" in (f / "MANIFEST.md5").read_text()
    (f / "a.bin").write_bytes(b"abcd")                     # changed afterwards: hashed again
    os.utime(f / "a.bin", ns=(1, 2))
    manifest(str(f), None, Md5Cache(str(tmp_path / "md5.json")))
    assert hashlib.md5(b"abcd").hexdigest() in (f / "MANIFEST.md5").read_text()


def test_stage_hard_links_a_disc_folder(tmp_path):
    from ps2rc.convert.disc import DiscFile
    from ps2rc.convert.stage import Stager
    src = tmp_path / "discdir"
    src.mkdir()
    (src / "A.BIN").write_bytes(b"x" * 100)
    (src / "B.BIN").write_bytes(b"old")

    class FakeDisc:
        files = [DiscFile(30, 100, "A.BIN;1", "A.BIN"), DiscFile(31, 3, "B.BIN;1", "B.BIN")]

        def host_path(self, f):
            return str(src / f.rel)

        def read(self, f):
            return (src / f.rel).read_bytes()

        def iter_chunks(self, f):
            yield self.read(f)

    class FakeRecipe:
        def stage_selection(self, names, audio, boot):
            return names, []

        def replacements(self):
            return {"B.BIN;1": (b"old", b"new")}

    out = tmp_path / "out"
    st = Stager(FakeDisc(), FakeRecipe(), str(out), Progress())
    st.stage_disc(None, None)
    assert st.linked == 1
    assert os.path.samefile(out / "disc" / "A.BIN", src / "A.BIN")
    assert (src / "B.BIN").read_bytes() == b"old"            # a replaced file is a new file, the source untouched


def test_split_cold_keeps_prototypes_in_both_halves(tmp_path):
    """A prototype after a cold function (the translator writes one before its first caller) and a one-line profiler
    wrapper: the prototype goes into both halves, the wrapper stays hot; a unit with a table stays whole."""
    g = tmp_path / "gen"
    g.mkdir()
    unit = "\n".join([
        '#include "main.h"', "",
        "__attribute__((cold)) void fn_00000100(R5900Ctx *c)", "{", "    c->r[2].d[0] = 1;", "}", "",
        "void y_nat_00000110(R5900Ctx *c);",
        "void fn_00000110__zbody(R5900Ctx *c)", "{", "    if (c->pc) {", "        y_nat_00000110(c);", "    }", "}", "",
        "void fn_00000110(R5900Ctx *c) { if (hle_pz_on) hle_pz_wrap(1, fn_00000110__zbody, c); "
        "else fn_00000110__zbody(c); }", ""])
    (g / "main_000.c").write_text(unit, newline="")
    (g / "main_001.c").write_text(unit + "static const int t[] = {1};\n", newline="")
    assert bmod.split_cold(str(g))[0] == 1
    hot, cold = (g / "main_000.c").read_text(), (g / "main_000_cold.c").read_text()
    assert "void y_nat_00000110(R5900Ctx *c);" in hot and "void y_nat_00000110(R5900Ctx *c);" in cold
    assert "hle_pz_wrap" in hot and "hle_pz_wrap" not in cold and "fn_00000100(R5900Ctx" in cold
    assert hot.count("{") == hot.count("}") and cold.count("{") == cold.count("}")
    assert not (g / "main_001_cold.c").exists()
