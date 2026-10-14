"""Exactness of the back-end levers vwt (VF lanes defined by LQC2 / QMTC2 written through to the context), mloc
(MMI word shuffles PCPYLD / PEXTLW / PROT3W and QMTC2 / QMFC2 on the C locals; variants v5s4y3rxw / v5s4y3rxm) and the
experimental mlocx (also PCPYUD / PEXTUW / PAND / POR / PXOR / PNOR; in no variant).

Hand-encoded functions (128-bit loads, the converted instructions, 128-bit stores, VU0 transfers and a VU0 multiply)
run on the host for v0 (the reference: every instruction through the context) and the lever variants; the memory they
write and the VU0 registers at the return must be equal.
"""
import os
import struct
import subprocess

import pytest
from ps2rc import config

from ps2rc import backend_c
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment
from ps2rc.r5900 import decode
from r5900_enc import A0, A1, NOP, RA, T0, T1, T2, T3, jr, lq, mmi, sq

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_mloc_levers")
BASE, SIZE = 0x1000, 0x1000


def pcpyld(d, s, t): return mmi(0x09, 0x0E, d, s, t)
def pcpyud(d, s, t): return mmi(0x29, 0x0E, d, s, t)
def pextlw(d, s, t): return mmi(0x08, 0x12, d, s, t)
def pextuw(d, s, t): return mmi(0x28, 0x12, d, s, t)
def prot3w(d, t): return mmi(0x09, 0x1F, d, 0, t)
def pand(d, s, t): return mmi(0x09, 0x12, d, s, t)
def pxor(d, s, t): return mmi(0x09, 0x13, d, s, t)
def por(d, s, t): return mmi(0x29, 0x12, d, s, t)
def pnor(d, s, t): return mmi(0x29, 0x13, d, s, t)
def qmtc2(rt, vf): return (0x12 << 26) | (0x05 << 21) | (rt << 16) | (vf << 11)
def qmfc2(rt, vf): return (0x12 << 26) | (0x01 << 21) | (rt << 16) | (vf << 11)
def lqc2(vf, off, base): return (0x36 << 26) | (base << 21) | (vf << 16) | (off & 0xFFFF)
def sqc2(vf, off, base): return (0x3E << 26) | (base << 21) | (vf << 16) | (off & 0xFFFF)
def vmul(fd, fs, ft): return (0x12 << 26) | (1 << 25) | (0xF << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x2A


# a0 = input area 0x4000 (64 bytes of patterns), a1 = output area 0x6000
CASES = {
    "SHUF": (0x1000, [lq(T0, 0, A0), lq(T1, 16, A0), pcpyld(T2, T0, T1), sq(T2, 0, A1),
                      pcpyud(T3, T0, T1), sq(T3, 16, A1), pextlw(T2, T0, T1), sq(T2, 32, A1),
                      pextuw(T3, T1, T0), sq(T3, 48, A1), prot3w(T0, T0), sq(T0, 64, A1), jr(RA), NOP]),
    "BITS": (0x1100, [lq(T0, 0, A0), lq(T1, 32, A0), pand(T2, T0, T1), sq(T2, 0, A1), por(T3, T0, T1), sq(T3, 16, A1),
                      pxor(T2, T0, T1), sq(T2, 32, A1), pnor(T3, T0, T1), sq(T3, 48, A1),
                      pcpyld(T0, T0, T0), sq(T0, 64, A1), jr(RA), NOP]),
    "VU0": (0x1200, [lq(T0, 0, A0), qmtc2(T0, 1), lqc2(2, 16, A0), vmul(3, 1, 2), qmfc2(T1, 3), sq(T1, 0, A1),
                     prot3w(T2, T1), sq(T2, 16, A1), qmtc2(T2, 4), sqc2(4, 32, A1), sqc2(2, 48, A1), jr(RA), NOP]),
}
SEEDS = sorted(a for a, _c in CASES.values())
CODE = [(0x1000, 0x1300)]

HARNESS = r"""
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
#define WIN 0x02000000u
static uint8_t *buf;
static R5900Ctx c __attribute__((aligned(16)));
static void run(const char *tag, void (*fn)(R5900Ctx *))
{
    uint8_t *ram = buf + RMB_GUARD;
    memset(ram, 0, 0x8000);
    memcpy(ram + 0x1000, unit_image, sizeof unit_image);
    static const uint32_t pat[16] = {0x3F800000u, 0x40000000u, 0xC0400000u, 0x80000001u, 0x12345678u, 0x9ABCDEF0u,
                                     0x0F0F0F0Fu, 0xF0F0F0F0u, 0x41200000u, 0xBF000000u, 0x7F7FFFFFu, 0x00000001u,
                                     0xDEADBEEFu, 0x01234567u, 0x89ABCDEFu, 0x55555555u};
    memcpy(ram + 0x4000, pat, sizeof pat);
    memset(&c, 0, sizeof c);
    c.ram = ram; c.ram_mask = WIN - 1;
    c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
    c.r[4].d[0] = 0x4000; c.r[5].d[0] = 0x6000;
    fn(&c);
    printf("%s out=", tag);
    for (int j = 0; j < 80; j++) printf("%02x", ram[0x6000 + j]);
    printf(" vf=");
    for (int v = 1; v < 5; v++) for (int k = 0; k < 4; k++) printf("%08x", c.vu0.vf[v].w[k]);
    printf("\n");
}
int main(void)
{
    fesetround(FE_TOWARDZERO);
    buf = malloc(RMB_GUARD + WIN + RMB_GUARD);
    if (!buf) return 2;
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


def options(variant, img, funcs):
    if variant == "v0":
        return {}
    levers = backend_c.V5_STEPS["v5s4y3rxm"] | {"mlocx"} if variant == "mlocx" else backend_c.V5_STEPS[variant]
    return {"cache": True, "fpu_fast": True, "abi": True, "narrow": narrowing_plan(img, funcs, abi=True,
            roots=set(SEEDS)), "livein": argument_livein(img, funcs), "v5": levers}


def generate(variant):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    return img, emit_program(img, fs, roots=SEEDS, **options(variant, img, fs.functions))


def test_encodings():
    want = {"SHUF": ["pcpyld", "pcpyud", "pextlw", "pextuw", "prot3w"], "BITS": ["pand", "por", "pxor", "pnor"],
            "VU0": ["qmtc2", "lqc2", "vmul", "qmfc2", "sqc2"]}
    for name, (a, code) in CASES.items():
        mn = [decode(w, a + 4 * k).mnem.split(".")[0] for k, w in enumerate(code)]
        for m in want[name]:
            assert m in mn, (name, m, mn)


def test_local_forms_in_the_generated_code():
    _img, (src, _h) = generate("v5s4y3rxm")
    _img, (ref, _h) = generate("v5s4y3rx")
    assert "mmi_pcpyld(" in ref and "mmi_prot3w(" in ref                  # the default variant is unchanged
    for k in ("mmi_pcpyld(", "mmi_pextlw(", "mmi_prot3w("):
        assert k not in src, k
    assert "mmi_pcpyud(" in src and "mmi_pnor(" in src                      # lever mlocx only
    vu = src.split("void fn_00001200(R5900Ctx *c)\n{")[1].split("\n}\n")[0]
    assert "c->vu0.vf[2].w[0] = v2_0;" in vu                                # vwt: the LQC2 lanes written through


def _wsl(p):
    return config.posix_path(p)


def build_and_run(tag, variant):
    img, (src, header) = generate(variant)
    out = os.path.join(OUT, tag)
    os.makedirs(out, exist_ok=True)
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
        "cd %s\nrm -f unit\ngcc -O2 -std=gnu11 -w -frounding-math -ffp-contract=off -DRMB_CHECK -I%s -I. unit_main.c "
        "unit_rc.c %s -lm -o unit || exit 99\n./unit\n" % (
            _wsl(out), _wsl(os.path.join(ROOT, "runtime")), _wsl(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    p = subprocess.run(config.bash_argv(script), env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, (p.stdout + p.stderr).decode(errors="replace")[-3000:]
    return dict(ln.split(" ", 1) for ln in p.stdout.decode().splitlines())


@pytest.mark.wsl
def test_levers_equal_v0():
    ref = build_and_run("v0", "v0")
    assert len(ref) == len(CASES)
    for variant in ("v5s4y3rx", "v5s4y3rxw", "v5s4y3rxm", "mlocx"):
        assert build_and_run(variant, variant) == ref, variant
