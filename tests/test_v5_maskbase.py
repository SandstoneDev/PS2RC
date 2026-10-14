"""Back-end v5 step S4, lever `maskbase`: guest addresses as rmb_*(m_, base, offset) - the
window mask on the base register only, the offset added after it (runtime/r5900_rt.h, RMB_GUARD / RMB_CHECK).

Hand-encoded R5900 functions exercise every access form the back-end emits (byte/half/word/double loads and stores,
LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR, LQ/SQ, LWC1/SWC1, LQC2/SQC2) with negative and positive offsets, through RAM, its
KSEG0 / uncached mirrors and the scratchpad (aliased at 0). They run on the host (WSL gcc) for v0 (the reference),
v5s3 and v5s4 in a window with guard areas: v5s4 must give the v0 results with no access leaving the window
(RMB_CHECK build). The one case maskbase does not translate like v4 - an end pointer (base = window size, masked to 0)
with a negative offset - is reported by the check build and, without it, reads the guard area instead of the end of
the window (the documented contract: RMB_GUARD bytes around the window, such accesses are not exact)."""
import os
import struct
import subprocess

import pytest
from ps2rc import config  # noqa: E402

from ps2rc import backend_c
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program, translate_function
from ps2rc.image import Image, Segment
from r5900_enc import A0, A1, A2, NOP, RA, T0, T1, ZERO, addiu, i_type, jr, ld, lq, lw, sd, sq, sw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_maskbase")
BASE, SIZE = 0x1000, 0x1000


def lb(rt, off, base): return i_type(0x20, base, rt, off)
def lh(rt, off, base): return i_type(0x21, base, rt, off)
def lwl(rt, off, base): return i_type(0x22, base, rt, off)
def lbu(rt, off, base): return i_type(0x24, base, rt, off)
def lhu(rt, off, base): return i_type(0x25, base, rt, off)
def lwr(rt, off, base): return i_type(0x26, base, rt, off)
def lwu(rt, off, base): return i_type(0x27, base, rt, off)
def sb(rt, off, base): return i_type(0x28, base, rt, off)
def sh(rt, off, base): return i_type(0x29, base, rt, off)
def swl(rt, off, base): return i_type(0x2A, base, rt, off)
def sdl(rt, off, base): return i_type(0x2C, base, rt, off)
def sdr(rt, off, base): return i_type(0x2D, base, rt, off)
def swr(rt, off, base): return i_type(0x2E, base, rt, off)
def ldl(rt, off, base): return i_type(0x1A, base, rt, off)
def ldr(rt, off, base): return i_type(0x1B, base, rt, off)
def lwc1(ft, off, base): return i_type(0x31, base, ft, off)
def swc1(ft, off, base): return i_type(0x39, base, ft, off)
def lqc2(ft, off, base): return i_type(0x36, base, ft, off)
def sqc2(ft, off, base): return i_type(0x3E, base, ft, off)


# every case: a0 = base pointer (set by the harness), a1 = output area (0x6000), a2 = a 64-bit pattern
CASES = {
    # name: (address, code, a0)
    "LOADS": (0x1000, [lb(T0, -16, A0), sd(T0, 0, A1), lbu(T0, -15, A0), sd(T0, 8, A1), lh(T0, -14, A0),
                       sd(T0, 16, A1), lhu(T0, 2, A0), sd(T0, 24, A1), lw(T0, 4, A0), sd(T0, 32, A1),
                       lwu(T0, -4, A0), sd(T0, 40, A1), ld(T0, 8, A0), sd(T0, 48, A1), jr(RA), NOP], 0x4010),
    "STORES": (0x1100, [sb(A2, -32, A0), sh(A2, -30, A0), sw(A2, -28, A0), sd(A2, -24, A0), sb(A2, 7, A0),
                        sh(A2, 10, A0), sw(A2, 12, A0), sd(A2, 16, A0), jr(RA), NOP], 0x6020),
    "UNALIGNED": (0x1200, [lwl(T0, -13, A0), lwr(T0, -16, A0), sd(T0, 0, A1), ldl(T1, -1, A0), ldr(T1, -8, A0),
                           sd(T1, 8, A1), swl(A2, 19, A1), swr(A2, 16, A1), sdl(A2, 31, A1), sdr(A2, 24, A1),
                           jr(RA), NOP], 0x4013),
    "QUAD": (0x1300, [lq(T0, -16, A0), sq(T0, 32, A1), lq(T1, 7, A0), sq(T1, 16, A1), jr(RA), NOP], 0x4025),
    "FPU": (0x1400, [lwc1(1, -4, A0), swc1(1, 0, A1), lwc1(2, 12, A0), swc1(2, 4, A1), jr(RA), NOP], 0x4008),
    "VU0": (0x1480, [lqc2(1, -16, A0), sqc2(1, 16, A1), jr(RA), NOP], 0x4020),
    "KSEG0": (0x1500, [lw(T0, -16, A0), sd(T0, 0, A1), sw(A2, -12, A0), jr(RA), NOP], 0x80004010),
    "UNCACHED": (0x1580, [lw(T0, -16, A0), sd(T0, 0, A1), sw(A2, -8, A0), jr(RA), NOP], 0x20004010),
    "SPR": (0x1600, [lw(T0, 16, A0), sd(T0, 0, A1), sw(A2, 20, A0), jr(RA), NOP], 0x70000000),
    "END": (0x1680, [lw(T0, -16, A0), sd(T0, 0, A1), jr(RA), NOP], 0x00010000),   # end pointer: not exact
}
SEEDS = sorted(a for a, _c, _b in CASES.values())
CODE = [(0x1000, 0x1700)]
WINDOW = 0x10000

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
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
static uint8_t buf[RMB_GUARD + 0x10000 + RMB_GUARD] __attribute__((aligned(64)));
static R5900Ctx c __attribute__((aligned(16)));
static void run(const char *tag, void (*fn)(R5900Ctx *), uint32_t a0)
{
    uint8_t *ram = buf + RMB_GUARD;
    memset(buf, 0xEE, sizeof buf);                        /* guard areas: 0xEE */
    memset(ram, 0, 0x10000);
    memcpy(ram + 0x1000, unit_image, sizeof unit_image);
    for (int k = 0; k < 64; k++) ram[0x4000 + k] = (uint8_t)(0x80 + 3 * k);
    for (int k = 0; k < 16; k++) ram[0xfff0 + k] = (uint8_t)(0x40 + k);
    for (int k = 0; k < 32; k++) ram[k] = (uint8_t)(0x20 + k);            /* scratchpad alias */
    memset(&c, 0, sizeof c);
    c.ram = ram; c.ram_mask = 0x10000 - 1;
    c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
    c.r[4].d[0] = (uint64_t)(int64_t)(int32_t)a0; c.r[5].d[0] = 0x6000; c.r[6].d[0] = 0x8877665544332211ull;
    mm = 0;
    fn(&c);
    printf("%s mm=%u out=", tag, mm);
    for (int k = 0; k < 64; k++) printf("%02x", ram[0x6000 + k]);
    printf(" mem=");
    for (int k = 0; k < 64; k++) printf("%02x", ram[0x4000 + k]);
    printf(" lo=");
    for (int k = 0; k < 32; k++) printf("%02x", ram[k]);
    printf("\n");
}
int main(void)
{
    fesetround(FE_TOWARDZERO);
/*CALLS*/
    return 0;
}
"""


def image():
    mem = bytearray(SIZE)
    for a, code, _b in CASES.values():
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
            roots=set(SEEDS)), "livein": argument_livein(img, funcs), "v5": backend_c.V5_LEVELS[variant]}


def build_and_run(tag, variant, check):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    src, header = emit_program(img, fs, roots=SEEDS, **options(variant, img, fs.functions))
    out = os.path.join(OUT, tag)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    data = img.read(BASE, SIZE)
    open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(
        "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data)))
    calls = "\n".join('    run("%s", fn_%08x, 0x%08xu);' % (n, a, b) for n, (a, _c, b) in CASES.items())
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(
        "#include <stdlib.h>\n" + HARNESS.replace("/*CALLS*/", calls))
    script = os.path.join(out, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "cd %s\nrm -f unit\ngcc -O2 -std=gnu11 -w -frounding-math -ffp-contract=off %s -I%s -I. unit_main.c unit_rc.c "
        "%s -lm -o unit || exit 99\n./unit\n" % (
            _wsl_path(out), "-DRMB_CHECK" if check else "", _wsl_path(os.path.join(ROOT, "runtime")),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script), env=env, capture_output=True)
    assert p.returncode == 0, (p.stdout + p.stderr).decode(errors="replace")[-3000:]
    res = {}
    for ln in p.stdout.decode().splitlines():
        name, mm, rest = ln.split(" ", 2)
        res[name] = (int(mm.split("=")[1]), rest)
    assert set(res) == set(CASES), res
    return res, src


def test_exact_functions_and_v5s3_keep_the_full_address():
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    fn = fs.functions[CASES["LOADS"][0]]
    abi = translate_function(img, fn.start, fn.addrs, cache=True, fpu_fast=True, abi=True, livein={},
                             v5=backend_c.V5_LEVELS["v5s4"])
    exact = translate_function(img, fn.start, fn.addrs, cache=True, fpu_fast=True, abi=False,
                               v5=backend_c.V5_LEVELS["v5s4"])
    s3 = translate_function(img, fn.start, fn.addrs, cache=True, fpu_fast=True, abi=True, livein={},
                            v5=backend_c.V5_LEVELS["v5s3"])
    assert "rmb_lb(m_, " in abi and "rm_lb(" not in abi and "/*B*/" not in abi
    assert "rmb_" not in exact and "rm_lb(m_, " in exact          # exact functions: not a v5 lever there
    assert "rmb_" not in s3


@pytest.mark.wsl
def test_maskbase_accesses_equal_v0_inside_the_window():
    ref, _s = build_and_run("v0", "v0", check=False)
    s3, _s = build_and_run("v5s3", "v5s3", check=False)
    s4c, src = build_and_run("v5s4_check", "v5s4", check=True)
    s4, _s = build_and_run("v5s4", "v5s4", check=False)
    for k in ("rmb_lb(", "rmb_lh(", "rmb_lw(", "rmb_ld(", "rmb_sb(", "rmb_sh(", "rmb_sw(", "rmb_sd(", "rmb_lwl(",
              "rmb_lwr(", "rmb_ldl(", "rmb_ldr(", "rmb_swl(", "rmb_swr(", "rmb_sdl(", "rmb_sdr(", "rmb_lq(", "rmb_sq("):
        assert k in src, k                                   # every accessor form is exercised
    for name in CASES:
        assert s3[name][1] == ref[name][1], name
        assert s4c[name][1] == ref[name][1], name            # the check build takes the rm_* address when it reports
        if name == "END":
            assert s4c[name][0] == 1                         # the end pointer with a negative offset is reported
            assert s4[name][1] != ref[name][1]               # ... and reads the guard area (0xEE) without the check
            assert s4[name][1].startswith("out=eeeeeeeeffffffff")
        else:
            assert s4c[name][0] == 0, name                   # no other access leaves the window
            assert s4[name][1] == ref[name][1], name
