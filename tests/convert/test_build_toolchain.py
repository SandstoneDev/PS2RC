"""Build commands (the same flags as the developer Makefile + pspsdk build.mak), toolchain runners and detection,
step records."""
import json
import os
import re
import sys

import pytest

from ps2rc.convert import build as bmod
from ps2rc.convert import toolchain as tcmod
from ps2rc.convert.steps import ConvertError, Progress, StepRecord, fingerprint, tree_hash

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class StubTc:
    kind, description, version = "wsl", "stub", "15.2.0"

    def __init__(self):
        self.batches = []

    def pspdev(self):
        return "/opt/pspdev"

    def run_batch(self, jobs, cwd, nproc, progress=None, name="batch"):
        self.batches.append((name, [j.argv for j in jobs]))
        for j in jobs:
            j.rc, j.output = 0, ""
        return jobs


class StubPack:
    def __init__(self, ldflags):
        self.ldflags = ldflags

    def get(self, key, default=None):
        return {"build.psp_ldflags": self.ldflags}.get(key, default)

    def path(self, key, default=None):
        return None


def _bdir(tmp_path, order=True):
    b = tmp_path / "b"
    for d in ("gen", "hle/rt", "obj"):
        (b / d).mkdir(parents=True)
    (b / "gen" / "unit_001.c").write_text("int a;")
    (b / "gen" / "unit_000.c").write_text("int b;")
    (b / "gen" / "units.h").write_text("")
    (b / "hle" / "hle_main.c").write_text("int c;")
    (b / "hle" / "hle_ctx.S").write_text("")
    (b / "hle" / "hle.h").write_text("")
    (b / "hle" / "hooks.def").write_text("")
    (b / "hle" / "rt" / "r5900_rt.h").write_text("")
    if order:
        (b / "order.ld").write_text(".text : {\n}\n")
    (b / "prepare.json").write_text(json.dumps({"hle_defs": ["-DHLE_BUILD_NUM=0", '-DHLE_TRANSLATION="x"']}))
    return str(b)


def test_compile_and_link_commands(tmp_path):
    b = _bdir(tmp_path)
    tc = StubTc()
    opts = bmod.BuildOptions(jobs=3, y_gen="-fsched-pressure")
    builder = bmod.Builder(ROOT, StubPack(["-Wl,--wrap=x"]), b, tc, opts, Progress(), {}, str(tmp_path / "l.log"))
    jobs, ngen = builder.compile_jobs()
    assert ngen == 2
    gen0 = jobs[0][0].argv
    inc = ["-Ihle/rt", "-Ihle", "-Igen", "-I.", "-I/opt/pspdev/psp/include", "-I/opt/pspdev/psp/sdk/include"]
    assert gen0 == ["psp-gcc"] + inc + ["-O2", "-G0", "-std=gnu11", "-fno-fast-math", "-fno-math-errno",
                                        "-frounding-math", "-ffp-contract=off", "-D_PSP_FW_VERSION=600",
                                        "-ffunction-sections", "-fsched-pressure", "-w", "-c", "gen/unit_000.c",
                                        "-o", "obj/gen/unit_000.o"]
    hle = jobs[2][0].argv
    assert hle[hle.index("-D_PSP_FW_VERSION=600") + 1:] == builder.mk_hleflags + ["-DHLE_BUILD_NUM=0",
                                                                              '-DHLE_TRANSLATION="x"',
                                                                              "-ffunction-sections", "-c",
                                                                              "hle/hle_main.c", "-o",
                                                                              "obj/hle/hle_main.o"]
    assert jobs[3][0].argv[-4:] == ["-c", "hle/hle_ctx.S", "-o", "obj/hle/hle_ctx.o"]
    builder.link()
    link = tc.batches[0][1][0]
    objs = [a for a in link if a.endswith(".o")]
    assert objs == ["obj/gen/unit_000.o", "obj/gen/unit_001.o", "obj/hle/hle_main.o", "obj/hle/hle_ctx.o"]
    assert link[link.index("-L.") : link.index("-L.") + 6] == ["-L.", "-L/opt/pspdev/psp/lib",
                                                               "-L/opt/pspdev/psp/sdk/lib", "-Wl,--wrap=x",
                                                               "-Wl,--section-ordering-file=order.ld",
                                                               "-Wl,-zmax-page-size=128"]
    tail = builder.mk_libs + bmod.BUILDMAK_LIBS + ["-o", "ps2rc.elf"]
    assert link[-len(tail):] == tail
    assert [x[0] for x in tc.batches] == ["link", "fixup", "strip"]
    assert tc.batches[2][1][0] == ["psp-strip", "ps2rc.elf", "-o", "ps2rc_strip.elf"]


def test_no_layout_no_function_sections(tmp_path):
    b = _bdir(tmp_path, order=False)
    builder = bmod.Builder(ROOT, StubPack([]), b, StubTc(), bmod.BuildOptions(y_gen=""), Progress(), {}, "l")
    jobs, _n = builder.compile_jobs()
    assert "-ffunction-sections" not in jobs[0][0].argv


def test_flags_come_from_the_makefiles(tmp_path):
    """The build follows the checkout's Makefiles: a flag changed there reaches the converter (here: a fake checkout
    with its own LIBS and a kernel module with an assembler import file)."""
    repo = tmp_path / "repo"
    (repo / "mk").mkdir(parents=True)
    nl = chr(10)
    (repo / "mk" / "Makefile.psp").write_text(nl.join([
        "FPFLAGS  = -fno-fast-math   # comment", "CFLAGS   = -O2 -G0 -std=gnu11 $(FPFLAGS)",
        "HLEFLAGS = -DHLE_PSP -Wall $(HLE_DEFS)", "LIBS     = -lpspgu", "LIBS    += -lpspaudio",
        "PSP_LARGE_MEMORY = 1", "ifneq ($(X),)", "LDFLAGS += -Wl,--x", "endif", ""]))
    (repo / "psp_exc").mkdir()
    (repo / "psp_exc" / "Makefile").write_text(nl.join(["OBJS = exc_main.o own.o", "CFLAGS = -O2 -Wall",
                                                        "PSP_FW_VERSION = 660", ""]))
    for f in ("exc_main.c", "own.S", "ps2rc_exc.h"):
        (repo / "psp_exc" / f).write_text("")
    cfl, hfl, libs, mem, src = bmod.makefile_flags(str(repo))
    assert cfl == ["-O2", "-G0", "-std=gnu11", "-fno-fast-math"] and hfl == ["-DHLE_PSP", "-Wall"]
    assert libs == ["-lpspgu", "-lpspaudio"] and mem == 1 and src == "mk/Makefile.psp"
    assert bmod.memsize(None) == 2 and bmod.memsize("0") == 0 and bmod.memsize("1", 150) == 1
    b = _bdir(tmp_path)
    tc = StubTc()
    builder = bmod.Builder(str(repo), StubPack([]), b, tc, bmod.BuildOptions(), Progress(), {}, str(tmp_path / "l"))
    builder.link()
    assert tc.batches[0][1][0][-10:] == ["-lpspgu", "-lpspaudio"] + bmod.BUILDMAK_LIBS + ["-o", "ps2rc.elf"]
    tc.batches.clear()
    builder.build_prx()
    cc = tc.batches[0][1]
    assert [a[-1] for a in cc] == ["exc_main.c", "own.S"] and "-D_PSP_FW_VERSION=660" in cc[0]
    link = tc.batches[1][1][0]
    assert link[link.index("-nostdlib") + 1] == "-lpspdebug"                  # no LIBS in that Makefile
    assert "exc_main.o" in link and "own.o" in link


def test_flags_match_the_developer_makefile():
    """The fallback constants are the Makefile values (the LIBS of a newer Makefile may only drop libraries)."""
    mk = next((m for m in (os.path.join(ROOT, "mk", "Makefile.psp"), os.path.join(ROOT, "tests", "phase6psp", "Makefile"))
               if os.path.exists(m)), "")
    if not mk:
        pytest.skip("developer Makefile not in this checkout")
    v = bmod.make_vars(mk)
    assert v["FPFLAGS"].split() == bmod.FPFLAGS
    assert v["CFLAGS"].split() == bmod.BASE_CFLAGS
    assert v["HLEFLAGS"].split() == bmod.HLEFLAGS
    assert set(v["LIBS"].split()) <= set(bmod.LIBS)


def test_translation_name(tmp_path):
    (tmp_path / "stats.json").write_text(json.dumps({"variant": "v5s4y2", "function_set": "a/b/v2",
                                                     "exempt": {"main": [1, 2, 3]}}))
    name, var = bmod.translation_name(str(tmp_path), "call", "/x/layout", "mb,be")
    assert name == "v5s4y2,v2,exempt=3,alias=call,layout=layout,y=mb+be" and var == "v5s4y2"


def test_native_batch_runs_in_parallel(tmp_path):
    tc = tcmod.NativeToolchain.__new__(tcmod.NativeToolchain)
    tc.root, tc.bin, tc.env, tc.description, tc.version = str(tmp_path), str(tmp_path), dict(os.environ), "x", "1"
    jobs = [tcmod.Job([sys.executable, "-c", "import sys; print('job', %d); sys.exit(%d)" % (i, i % 2)], "j%d" % i)
            for i in range(6)]
    prog = Progress()
    seen = []
    prog.listeners.append(seen.append)
    tc.run_batch(jobs, str(tmp_path), 3, prog, "test")
    assert [j.rc for j in jobs] == [0, 1, 0, 1, 0, 1]
    assert all("job %d" % i in j.output for i, j in enumerate(jobs))
    assert seen[-1][:4] == ("pct", "test", 6, 6)


def test_path_translation():
    d = tcmod.DockerToolchain("img", os.path.abspath("w"), "15.2.0")
    assert d.tool_path(os.path.join(os.path.abspath("w"), "b", "c.o")) == "/w/b/c.o"
    with pytest.raises(ConvertError):
        d.tool_path(os.path.abspath(os.path.join("elsewhere", "x")))
    if os.name != "nt":
        return
    p = os.path.abspath(os.sep + os.path.join("Games", "x y", "a.c"))      # <current drive>:\Games\x y\a.c
    drive = p[0].lower()
    assert tcmod._win_to_wsl(p) == "/mnt/%s/Games/x y/a.c" % drive
    w = tcmod.WslToolchain("Debian", "/opt/pspdev", "15.2.0")
    assert w.argv(["psp-gcc", tcmod.HostPath(p), "-O2"]) == ["psp-gcc", "/mnt/%s/Games/x y/a.c" % drive, "-O2"]


def test_detect_filters_versions(monkeypatch):
    class T:
        def __init__(self, v, d):
            self.version, self.description, self.kind = v, d, "native"
    monkeypatch.setattr(tcmod, "bundled_dir", lambda: "B")
    monkeypatch.setattr(tcmod, "_native_at", lambda d, how: T("4.3.5", "old") if d == "B" else None)
    monkeypatch.setattr(tcmod, "_wsl_distros", lambda: ["D"])
    monkeypatch.setattr(tcmod, "_wsl_find", lambda d: T("16.2.0", "wsl16"))
    monkeypatch.setattr(tcmod, "_docker_find", lambda *a, **k: None)
    monkeypatch.delenv("PSPDEV", raising=False)
    monkeypatch.delenv("PS2RC_TOOLCHAIN", raising=False)
    monkeypatch.setattr(tcmod.shutil, "which", lambda n: None)
    t, tried = tcmod.detect(want="15.2.0")
    assert t.description == "wsl16"                           # mismatched major: used only as the fallback
    assert any("too old" in x for x in tried) and any("fallback" in x for x in tried)
    monkeypatch.setattr(tcmod, "_wsl_find", lambda d: None)
    with pytest.raises(ConvertError) as e:
        tcmod.detect(want="15.2.0")
    assert e.value.code == "no_toolchain"


def test_step_record(tmp_path):
    rec = StepRecord(str(tmp_path / "w"))
    out = tmp_path / "o.txt"
    out.write_text("abc")
    d = tmp_path / "dir"
    d.mkdir()
    (d / "x").write_text("1")
    fp = fingerprint("a", 1, {"b": [2]})
    assert fp == fingerprint("a", 1, {"b": [2]}) and fp != fingerprint("a", 2)
    rec.save("s", fp, [str(out), str(d)], 1.5)
    assert rec.valid("s", fp) and not rec.valid("s", "other") and rec.seconds("s") == 1.5
    (d / "y").write_text("2")
    assert not rec.valid("s", fp)
    rec.forget("s")
    assert not rec.valid("s", fp)
    assert tree_hash(str(d)) != tree_hash(str(tmp_path / "missing"))
    e = ConvertError("x", "msg", "hint", log="l.log", tail=["t1"])
    assert e.lines() == ["ERROR (x): msg", "  what to do: hint", "  log: l.log", "  last lines:", "    t1"]


def test_explicit_toolchain_wins(monkeypatch):
    class T:
        def __init__(self, v, d):
            self.version, self.description, self.kind = v, d, "native"
    monkeypatch.setattr(tcmod, "_native_at", lambda d, how: T("16.2.0", "mine") if d == "X" else None)
    monkeypatch.setattr(tcmod, "_wsl_find", lambda d: T("15.2.0", "wsl15"))
    monkeypatch.setattr(tcmod, "_wsl_distros", lambda: ["D"])
    t, tried = tcmod.detect(explicit="X", want="15.2.0")
    assert t.description == "mine" and any("asked for" in x for x in tried)


def test_jobs_capped_by_memory():
    assert tcmod.cap_jobs(12, None) == 12
    assert tcmod.cap_jobs(12, 5000) == 5000 // tcmod.MB_PER_JOB < 12 and tcmod.cap_jobs(12, 2000) == 2000 // tcmod.MB_PER_JOB
    assert tcmod.cap_jobs(12, 100) == 1 and tcmod.cap_jobs(12, 64000) == 12
    assert tcmod.cap_jobs(4, 64000) == 4
    t = tcmod.Toolchain()
    t.avail_mb = 1400
    assert t.jobs(8) == 2


def test_gpl_libraries_refused_by_default(tmp_path):
    """psp-cfw-sdk's GPL-3.0 libraries are never linked by default: the fallback flags and the kernel module's default
    objects use the own import stubs; a checkout whose Makefiles still name the libraries is refused unless a
    developer allows it."""
    assert not set(bmod.GPL_LIBS) & set(bmod.LIBS + bmod.BUILDMAK_LIBS)
    assert "ps2rc_sctrl.o" in bmod.PRX_DEFAULT["OBJS"] and not bmod.PRX_DEFAULT["LIBS"]
    repo = tmp_path / "repo"
    (repo / "mk").mkdir(parents=True)
    nl = chr(10)
    (repo / "mk" / "Makefile.psp").write_text(nl.join(["CFLAGS = -O2", "LIBS = -lpspgu -lpspkubridge", ""]))
    (repo / "psp_exc").mkdir()
    (repo / "psp_exc" / "Makefile").write_text(nl.join(["OBJS = exc_main.o", "LIBS = -lpspsystemctrl_kernel", ""]))
    (repo / "psp_exc" / "exc_main.c").write_text("")
    b = _bdir(tmp_path)
    builder = bmod.Builder(str(repo), StubPack([]), b, StubTc(), bmod.BuildOptions(), Progress(), {}, str(tmp_path / "l"))
    with pytest.raises(ConvertError) as e:
        builder.link()
    assert e.value.code == "gpl_lib" and "-lpspkubridge" in e.value.message
    with pytest.raises(ConvertError) as e:
        builder.build_prx()
    assert e.value.code == "gpl_lib"
    seen = []
    prog = Progress()
    prog.listeners.append(seen.append)
    tc = StubTc()
    allowed = bmod.Builder(str(repo), StubPack([]), b, tc, bmod.BuildOptions(allow_gpl_libs=True), prog, {},
                           str(tmp_path / "l"))
    allowed.link()
    assert "-lpspkubridge" in tc.batches[0][1][0] and any(ev[0] == "warn" for ev in seen)
