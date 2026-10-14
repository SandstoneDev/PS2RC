"""Exactness of the back-end's speed levers.

* back-end lever `ufuse` (variant v5s4y1): adjacent LDL/LDR, LWL/LWR, SDL/SDR, SWL/SWR pairs of one register and base
  become one unaligned access (rmb_uld / rmb_ulw / rmb_usd / rmb_usw). Hand-encoded functions run on the host for v0
  (the reference: every instruction through the context, the EE merge semantics), v5s4 and v5s4y1, with the base at
  all 8 alignments, both pair orders, negative offsets, sign extension of LWL/LWR, and pairs that must not be fused.
* runtime-header lever `mb` (ps2rc/pipeline/patch_rt_header.py --y mb): constant window mask + the base pointer through an
  empty asm. The same functions run with the patched header in a 32 MB window (the PSP / HLE mask) with RMB_CHECK.
* runtime-header lever `be` (--y be): the back-edge macro's text (hle_where on the slow path only).
"""
import os
import struct
import subprocess
import sys

import pytest
from ps2rc import config  # noqa: E402

from ps2rc import backend_c
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment
from r5900_enc import A0, A1, A2, A3, NOP, RA, T0, T1, T2, i_type, jr, sd

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_y_levers")
BASE, SIZE = 0x1000, 0x1000


def lwl(rt, off, base): return i_type(0x22, base, rt, off)
def lwr(rt, off, base): return i_type(0x26, base, rt, off)
def ldl(rt, off, base): return i_type(0x1A, base, rt, off)
def ldr(rt, off, base): return i_type(0x1B, base, rt, off)
def swl(rt, off, base): return i_type(0x2A, base, rt, off)
def swr(rt, off, base): return i_type(0x2E, base, rt, off)
def sdl(rt, off, base): return i_type(0x2C, base, rt, off)
def sdr(rt, off, base): return i_type(0x2D, base, rt, off)


# a0 = 0x4010 + k (k = 0..7, the alignment under test), a1 = output area 0x6000, a2 = a 64-bit pattern
CASES = {
    "LDLR": (0x1000, [ldl(T0, 7, A0), ldr(T0, 0, A0), sd(T0, 0, A1),
                      ldr(T1, 8, A0), ldl(T1, 15, A0), sd(T1, 8, A1),
                      ldl(T2, -1, A0), ldr(T2, -8, A0), sd(T2, 16, A1), jr(RA), NOP]),
    "LWLR": (0x1100, [lwl(T0, 3, A0), lwr(T0, 0, A0), sd(T0, 0, A1),
                      lwr(T1, 5, A0), lwl(T1, 8, A0), sd(T1, 8, A1),
                      lwl(T2, -13, A0), lwr(T2, -16, A0), sd(T2, 16, A1), jr(RA), NOP]),
    "SDLR": (0x1200, [sdl(A2, 7, A0), sdr(A2, 0, A0), sdr(A2, 10, A0), sdl(A2, 17, A0),
                      sdl(A2, -9, A0), sdr(A2, -16, A0), jr(RA), NOP]),
    "SWLR": (0x1300, [swl(A2, 3, A0), swr(A2, 0, A0), swr(A2, 6, A0), swl(A2, 9, A0),
                      swl(A2, -13, A0), swr(A2, -16, A0), jr(RA), NOP]),
    # not pairs: the base is the loaded register; different registers; offsets 6 apart; a lone LDL keeps the merge
    "NOFUSE": (0x1400, [ldl(A3, 7, A0), ldr(T0, 0, A0), sd(A3, 0, A1), sd(T0, 8, A1),
                        lwl(T1, 2, A0), lwr(T1, -4, A0), sd(T1, 16, A1),
                        ldl(T2, 23, A0), sd(T2, 24, A1), jr(RA), NOP]),
    "SELF": (0x1500, [i_type(0x09, A0, T0, 0), ldl(T0, 7, T0), ldr(T0, 0, T0), sd(T0, 0, A1), jr(RA), NOP]),
}
SEEDS = sorted(a for a, _c in CASES.values())
CODE = [(0x1000, 0x1600)]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include "r5900_rt.h"
#include "unit_image.h"
#include "unit_rc.h"
static unsigned mm;
#ifdef RMB_CHECK
void rmb_mismatch(uint32_t base, int32_t off) { (void)base; (void)off; mm++; }
#endif
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; printf("FATAL %s %x %x\n", what, pc, val); exit(1); }
#define WIN 0x02000000u
static uint8_t *buf;
static R5900Ctx c __attribute__((aligned(16)));
static void run(const char *tag, void (*fn)(R5900Ctx *))
{
    uint8_t *ram = buf + RMB_GUARD;
    for (int k = 0; k < 8; k++) {
        memset(ram, 0, 0x8000);
        memcpy(ram + 0x1000, unit_image, sizeof unit_image);
        for (int j = 0; j < 64; j++) ram[0x3ff0 + j] = (uint8_t)(0x80 + 5 * j);
        memset(&c, 0, sizeof c);
        c.ram = ram; c.ram_mask = WIN - 1;
        c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
        c.r[4].d[0] = 0x4010u + (uint32_t)k; c.r[5].d[0] = 0x6000; c.r[6].d[0] = 0xF877665544332211ull;
        c.r[7].d[0] = 0x1111111111111111ull; c.r[8].d[0] = 0x2222222222222222ull;
        c.r[9].d[0] = 0x3333333333333333ull; c.r[10].d[0] = 0x4444444444444444ull;
        mm = 0;
        fn(&c);
        printf("%s/%d mm=%u out=", tag, k, mm);
        for (int j = 0; j < 32; j++) printf("%02x", ram[0x6000 + j]);
        printf(" mem=");
        for (int j = 0; j < 64; j++) printf("%02x", ram[0x3ff0 + j]);
        printf("\n");
    }
}
int main(void)
{
    fesetround(FE_TOWARDZERO);
    buf = malloc(RMB_GUARD + WIN + RMB_GUARD);
    if (!buf) return 2;
    memset(buf, 0xEE, RMB_GUARD + WIN + RMB_GUARD);
/*CALLS*/
    return 0;
}
"""


def image():
    mem = bytearray(SIZE)
    for a, code in CASES.values():
        for k, w in enumerate(code):
            struct.pack_into("<I", mem, a - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<unit>", entry=BASE, gp=0, segments=[seg], overlay_slots=[])


def _wsl_path(p):
    return config.posix_path(p)


def options(variant, img, funcs):
    if variant == "v0":
        return {}
    return {"cache": True, "fpu_fast": True, "abi": True, "narrow": narrowing_plan(img, funcs, abi=True,
            roots=set(SEEDS)), "livein": argument_livein(img, funcs), "v5": backend_c.V5_STEPS[variant]}


def generate(variant):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    return img, emit_program(img, fs, roots=SEEDS, **options(variant, img, fs.functions))


def header_mb(out):
    """r5900_rt.h with the header lever mb (patch_rt_header.patch_y on the original text)."""
    from ps2rc.pipeline import patch_rt_header
    text = open(os.path.join(ROOT, "runtime", "r5900_rt.h"), encoding="utf-8").read()
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "r5900_rt.h"), "w", newline="\n", encoding="utf-8").write(
        patch_rt_header.patch_y(text, ["mb"], "check"))
    return out


def build_and_run(tag, variant, check, mb=False):
    img, (src, header) = generate(variant)
    out = os.path.join(OUT, tag)
    os.makedirs(out, exist_ok=True)
    inc = header_mb(os.path.join(out, "rt")) if mb else os.path.join(ROOT, "runtime")
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    data = img.read(BASE, SIZE)
    open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(
        "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data)))
    calls = "\n".join('    run("%s", fn_%08x);' % (n, a) for n, (a, _c) in CASES.items())
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(
        HARNESS.replace("/*CALLS*/", calls))
    script = os.path.join(out, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "cd %s\nrm -f unit\ngcc -O2 -std=gnu11 -w -frounding-math -ffp-contract=off %s -I%s -I. unit_main.c unit_rc.c "
        "%s -lm -o unit || exit 99\n./unit\n" % (
            _wsl_path(out), "-DRMB_CHECK" if check else "", _wsl_path(inc),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script), env=env, capture_output=True)
    assert p.returncode == 0, (p.stdout + p.stderr).decode(errors="replace")[-3000:]
    res = {}
    for ln in p.stdout.decode().splitlines():
        name, mm, rest = ln.split(" ", 2)
        res[name] = (int(mm.split("=")[1]), rest)
    assert len(res) == 8 * len(CASES), res
    return res, src


def test_ufuse_pairs_in_the_generated_code():
    _img, (src, _h) = generate("v5s4y1")
    _img, (s4, _h) = generate("v5s4")
    for k in ("rmb_uld(", "rmb_ulw(", "rmb_usd(", "rmb_usw("):
        assert k in src, k
        assert k not in s4, k                                     # v5s4 (the default) is unchanged
    body = src.split("void fn_00001400(R5900Ctx *c)\n{")[1].split("\n}\n")[0]
    assert "rmb_ldl(" in body and "rmb_ldr(" in body and "rmb_lwl(" in body and "rmb_uld(" not in body
    self_ = src.split("void fn_00001500(R5900Ctx *c)\n{")[1].split("\n}\n")[0]
    assert "rmb_uld(" not in self_                                 # a load pair that overwrites its own base
    lw_ = src.split("void fn_00001100(R5900Ctx *c)\n{")[1].split("\n}\n")[0]
    assert lw_.count("rmb_ulw(") == 3 and "rmb_lwl(" not in lw_


@pytest.mark.wsl
def test_ufuse_and_mb_equal_v0():
    ref, _s = build_and_run("v0", "v0", check=False)
    for tag, variant, check, mb in (("s4", "v5s4", False, False), ("y1", "v5s4y1", False, False),
                                    ("y1_check", "v5s4y1", True, False), ("y1_mb", "v5s4y1", False, True),
                                    ("y1_mb_check", "v5s4y1", True, True), ("s4_mb_check", "v5s4", True, True)):
        res, _src = build_and_run(tag, variant, check, mb)
        for name in ref:
            assert res[name][1] == ref[name][1], (tag, name, res[name][1], ref[name][1])
            assert res[name][0] == 0, (tag, name)                  # no access left the window


def test_header_lever_text():
    from ps2rc.pipeline import patch_rt_header
    orig = open(os.path.join(ROOT, "runtime", "r5900_rt.h"), encoding="utf-8").read()
    psp = patch_rt_header.patch(orig, "psp", "call", True)
    y = patch_rt_header.patch_y(psp, ["mb", "be"], "psp")
    assert "#define RT_Y_MASK 0x01FFFFFFu" in y and '__asm__("" : "+r"(p))' in y
    assert "(m).mask" not in y.split("#define RMB_PTR")[1].split("\n")[0]
    assert "{ hle_where = (a); hle_backedge_slow(c); }" in y
    assert patch_rt_header.patch_y(psp, [], "psp") == psp           # no lever: unchanged
    chk = patch_rt_header.patch_y(patch_rt_header.patch(orig, "check", "call", True), ["mb"], "check")
    assert "if (m.mask != 0x01FFFFFFu) rmb_mismatch" in chk
    with pytest.raises(SystemExit):
        patch_rt_header.patch_y(psp, ["nope"], "psp")


@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v5s4", "v5s4y2"])
def test_vfloc_vu0_macro_matches_the_reference_model(variant):
    """Lever vfloc (VF / ACC lanes in C locals): the VU0 macro programs of tests/test_vu0_macro.py (matrix x vector,
    every FMAC form, dest masks, aliasing, VF00, moves, LQC2/SQC2/QMTC2/QMFC2, CFC2/CTC2, the R register, VCALLMS
    through the hook - the micro subroutine reads and writes c->vu0 - and BC0x) against the exact Python reference."""
    import test_vu0_macro as T
    img = T.image()
    fs = discover_functions(img, T.CODE, extra_seeds=T.SEEDS)
    opts = {"cache": True, "fpu_fast": True, "abi": True,
            "narrow": narrowing_plan(img, fs.functions, abi=True, roots=set(T.SEEDS)),
            "livein": argument_livein(img, fs.functions), "v5": backend_c.V5_STEPS[variant]}
    src, header = emit_program(img, fs, roots=T.SEEDS, **opts)
    assert "unsupported" not in src
    if variant == "v5s4y2":
        assert "vu0_fmac(c," not in src and "vu0_fmac1(" in src and "c->vu0.vf[" in src
    vecs = T.vectors(64, 20261003)
    got = T.compile_run(os.path.join(OUT, "vu0_" + variant), src, header, vecs)
    exp = T.expected(vecs, got)
    exp.update(T.special_expectations(vecs))
    bad = {k: (got.get(k), v) for k, v in exp.items() if got.get(k) != v}
    assert not bad, "%d mismatches, first: %s" % (len(bad), sorted(bad.items())[:3])


# VU0 state across calls (lever vfloc): A loads VF1 / VF2, calls B (reads VF2, writes VF1 and ACC), adds and stores;
# B's VF writes must reach A, A's must reach B; C calls A twice through a loop (dirty set across a back edge + call)
VF_CASES = {
    "A": 0x1000, "B": 0x1100, "C": 0x1200,
}


def _vf_program():
    sys.path.insert(0, os.path.join(ROOT, "tests", "poc_vu0"))
    from vu0enc import asm as V
    from r5900_enc import S0, SP, addiu, beq, jal, lw, sw
    a = [V("lqc2", 1, 0, A0), V("lqc2", 2, 16, A0), addiu(SP, SP, -16), sw(RA, 0, SP),
         jal(0x1100), sw(A1, 4, SP),              # a1 is caller-saved: kept on the stack across the call
         lw(A1, 4, SP), V("vadd.xyzw", 3, 1, 2), V("sqc2", 3, 0, A1), V("sqc2", 1, 16, A1),
         lw(RA, 0, SP), jr(RA), addiu(SP, SP, 16)]
    b = [V("vmul.xyz", 1, 2, 2), V("vmulax.xyzw", "ACC", 2, 2), V("vmaddw.w", 1, 2, 0), jr(RA), NOP]
    c = [addiu(SP, SP, -16), sw(RA, 0, SP), sw(S0, 4, SP), addiu(S0, 0, 2),
         jal(0x1000), addiu(A1, A1, 32),          # loop: A(a0, a1 += 32) twice
         addiu(S0, S0, -1), beq(S0, 0, 0x1200 + 4 * 7, 0x1200 + 4 * 11), NOP,
         beq(0, 0, 0x1200 + 4 * 9, 0x1200 + 4 * 4), NOP,
         V("sqc2", 1, 0, A1), lw(S0, 4, SP), lw(RA, 0, SP), jr(RA), addiu(SP, SP, 16)]
    mem = bytearray(SIZE)
    for addr, code in ((0x1000, a), (0x1100, b), (0x1200, c)):
        for k, w in enumerate(code):
            struct.pack_into("<I", mem, addr - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<unit>", entry=BASE, gp=0, segments=[seg], overlay_slots=[])


VF_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include "r5900_rt.h"
#include "unit_image.h"
#include "unit_rc.h"
#ifdef RMB_CHECK
void rmb_mismatch(uint32_t base, int32_t off) { (void)base; (void)off; }
#endif
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; printf("FATAL %s %x %x\n", what, pc, val); exit(1); }
static uint8_t buf[RMB_GUARD + 0x10000 + RMB_GUARD] __attribute__((aligned(64)));
static R5900Ctx c __attribute__((aligned(16)));
int main(void)
{
    fesetround(FE_TOWARDZERO);
    uint8_t *ram = buf + RMB_GUARD;
    memcpy(ram + 0x1000, unit_image, sizeof unit_image);
    static const float in[8] = {1.5f, -2.25f, 3.0f, 0.5f, 1.25f, 2.0f, -0.75f, 4.0f};
    memcpy(ram + 0x4000, in, sizeof in);
    c.ram = ram; c.ram_mask = 0xFFFF;
    c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000; c.r[4].d[0] = 0x4000; c.r[5].d[0] = 0x6000;
    fn_00001200(&c);
    for (int k = 0; k < 112; k++) printf("%02x", ram[0x6000 + k]);
    printf(" acc");
    for (int k = 0; k < 4; k++) printf(" %08x", c.vu0.acc.w[k]);
    for (int r = 1; r < 4; r++) for (int k = 0; k < 4; k++) printf(" %08x", c.vu0.vf[r].w[k]);
    printf("\n");
    return 0;
}
"""


@pytest.mark.wsl
def test_vfloc_vu0_state_crosses_calls():
    img = _vf_program()
    seeds = [0x1000, 0x1100, 0x1200]
    res = {}
    for variant in ("v0", "v5s4", "v5s4y2"):
        fs = discover_functions(img, [(0x1000, 0x1300)], extra_seeds=seeds)
        opts = {} if variant == "v0" else {
            "cache": True, "fpu_fast": True, "abi": True, "narrow": narrowing_plan(img, fs.functions, abi=True,
                                                                                   roots=set(seeds)),
            "livein": argument_livein(img, fs.functions), "v5": backend_c.V5_STEPS[variant]}
        src, header = emit_program(img, fs, roots=seeds, **opts)
        if variant == "v5s4y2":
            assert "v1_0" in src and "c->vu0.vf[1].w[0] = v1_0;" in src
        out = os.path.join(OUT, "vfcall_" + variant)
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
        open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
        data = img.read(BASE, SIZE)
        open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(
            "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data)))
        open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(VF_HARNESS)
        script = os.path.join(out, "run.sh")
        open(script, "w", newline="\n", encoding="ascii").write(
            "cd %s\nrm -f unit\ngcc -O2 -std=gnu11 -w -frounding-math -ffp-contract=off -I%s -I. unit_main.c "
            "unit_rc.c %s -lm -o unit || exit 99\n./unit\n" % (
                _wsl_path(out), _wsl_path(os.path.join(ROOT, "runtime")),
                _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        p = subprocess.run(config.bash_argv(script), env=env,
                           capture_output=True)
        assert p.returncode == 0, (p.stdout + p.stderr).decode(errors="replace")[-3000:]
        res[variant] = p.stdout.decode().strip()
    assert res["v5s4"] == res["v0"]
    assert res["v5s4y2"] == res["v0"], (res["v5s4y2"], res["v0"])
