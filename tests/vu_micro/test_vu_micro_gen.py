"""VU0 microcode translator (ps2rc.vu.micro_gen): decoder, VIF-chain extraction, timing resolution, and the generated
C (both register modes) against a cycle-level reference simulator with exact arithmetic on random microprograms and
random register states. Synthetic only."""
import os
import struct
import subprocess

import pytest

from ps2rc.vu import micro_gen as G
from ps2rc.vu.micro_gen_isa import decode, decode_program, text
from vu_testlib import (up, lo, LOWER_NOP, to_bytes, random_program, random_state, Sim, rnd, ONE, fmul, fadd)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


# ---------------------------------------------------------------------------------------------- decoder / chain
def test_encoder_and_decoder_agree():
    m = decode(lo("IADDIU", it=6, is_=0, imm=0xE0), up("MADD", 1, 10, 0, 10, bc=2))
    assert "maddbc.w vf10, vf00, vf10z" in text(m) and "iaddiu vi06, vi00, 0xe0" in text(m)
    m = decode(0x3F000000, up("MUL", 14, 10, 17, 12, i=True))
    assert m.ibit and m.lo is None and "loi 0x3f000000" in text(m)
    m = decode(lo("DIV", fs=9, fsf=3, ft=8, ftf=3), up("CLIP", 14, 0, 17, 3, e=True))
    assert m.ebit and "div Q, vf09w, vf08w" in text(m) and "clipw.xyz vf17, vf03w" in text(m)
    assert decode(LOWER_NOP, up("OPMULA", 14, 0, 8, 5)).up.base == "OPMULA"
    assert decode(lo("FMAND", it=5, is_=6), up("NOP")).lo.name == "FMAND"
    assert decode(lo("BAL", it=15, imm=-3), up("NOP")).lo.imm == -3


def test_vif_chain_extraction():
    prog = [(LOWER_NOP, up("NOP")) for _ in range(5)]
    code = to_bytes(prog)
    mem = bytearray(0x100)
    # CNT tag (qwc covering MPG data), TTE upper half: VIF NOP + MPG num 5 addr 2
    qwc = (len(code) + 15) // 16
    struct.pack_into("<QII", mem, 0, (1 << 28) | qwc, 0, (0x4A << 24) | (5 << 16) | 2)
    mem[16:16 + len(code)] = code
    end = 16 + 16 * qwc
    struct.pack_into("<Q", mem, end, 7 << 28)
    out = G.extract_mpg(lambda a, n: bytes(mem[a:a + n]), 0)
    assert out[16:] == code and out[:16] == bytes(16)


def _fmand_src(prog):
    ex = G.Expander(decode_program(to_bytes(prog)))
    ex.expand(0)
    rd = [n for n in ex.nodes if n.m.lo is not None and n.m.lo.name == "FMAND"]
    assert len(rd) == 1
    return rd[0].mac_src


def test_static_flag_resolution_reads_the_producer_four_cycles_back():
    end = [(LOWER_NOP, up("NOP", e=True)), (LOWER_NOP, up("NOP"))]
    # A, B, C, nop, FMAND: the reader issues 4 cycles after A -> A's flags (pc 0)
    p1 = [(LOWER_NOP, up("ADD", 15, 1, 4, 5)), (LOWER_NOP, up("ADD", 15, 2, 4, 5)),
          (LOWER_NOP, up("ADD", 15, 3, 4, 5)), (LOWER_NOP, up("NOP")), (lo("FMAND", it=1, is_=7), up("NOP"))]
    assert _fmand_src(p1 + end) == 0
    # A, B, MOVE (independent), FMAND: issued 3 cycles after A -> still the caller's MAC (-1)
    p2 = [(LOWER_NOP, up("ADD", 15, 1, 4, 5)), (LOWER_NOP, up("ADD", 15, 2, 4, 5)),
          (lo("MOVE", dest=15, ft=20, fs=9), up("NOP")), (lo("FMAND", it=1, is_=7), up("NOP"))]
    assert _fmand_src(p2 + end) == -1
    # the MOVE reads B's result: it stalls 3 cycles, so the reader then sees B (pc 1)
    p3 = list(p2)
    p3[2] = (lo("MOVE", dest=15, ft=20, fs=2), up("NOP"))
    assert _fmand_src(p3 + end) == 1


def test_unsupported_is_reported():
    prog = [(lo("FMAND", it=1, is_=2), up("MAX", 15, 1, 2, 3)), (LOWER_NOP, up("NOP", e=True)),
            (LOWER_NOP, up("NOP"))]
    with pytest.raises(G.Unsupported):
        G.translate(to_bytes(prog), [0], "t")


def test_reference_arithmetic_rounds_toward_zero():
    from fractions import Fraction
    from vu_testlib import bits
    third = bits(Fraction(1, 3))                   # 1/3 toward zero: mantissa ...AAA, not ...AAB
    assert third & 0x7FFFFF == int("10" * 11, 2) and fmul(third, ONE) == third
    assert fadd(ONE, (127 - 24) << 23) == ONE      # 1 + 2^-24: toward zero
    assert fmul(1 << 23, 126 << 23) == 0           # 2^-126 * 0.5: flushed
    assert fmul(0x7F7FFFFF, 128 << 23) == 0x7F7FFFFF


# ---------------------------------------------------------------------------------------------- generated C vs reference
DRIVER = r"""
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include "r5900_rt.h"
#include "cases.h"
int tprog(R5900Ctx *c, uint32_t addr);
int main(void)
{
    fesetround(FE_TOWARDZERO);
    _mm_setcsr((_mm_getcsr() & ~0x0040u) | 0x8000u);         /* flush to zero, no denormals-are-zero */
    static R5900Ctx c;
    for (int k = 0; k < NCASES; k++) {
        const uint32_t *s = cases[k];
        memset(&c.vu0, 0, sizeof c.vu0);
        for (int r = 1; r < 32; r++) for (int i = 0; i < 4; i++) c.vu0.vf[r].w[i] = s[4 * r + i];
        for (int i = 0; i < 4; i++) c.vu0.acc.w[i] = s[128 + i];
        for (int r = 1; r < 16; r++) c.vu0.vi[r] = s[132 + r];
        c.vu0.vi[21] = s[148]; c.vu0.vi[22] = s[149]; c.vu0.vi[17] = s[150]; c.vu0.vi[18] = s[151];
        c.vu0.vi[16] = s[152];
        if (tprog(&c, 0)) { printf("K%d unknown entry\n", k); continue; }
        printf("K%d", k);
        for (int r = 1; r < 32; r++) for (int i = 0; i < 4; i++) printf(" %08x", c.vu0.vf[r].w[i]);
        for (int i = 0; i < 4; i++) printf(" %08x", c.vu0.acc.w[i]);
        for (int r = 1; r < 16; r++) printf(" %04x", c.vu0.vi[r] & 0xFFFFu);
        printf(" %08x %08x %04x %06x %x\n", c.vu0.vi[21], c.vu0.vi[22], c.vu0.vi[17] & 0xFFFFu,
               c.vu0.vi[18] & 0xFFFFFFu, c.vu0.vi[26]);
    }
    return 0;
}
"""


def _expect(o):
    return " ".join(["%08x" % o["vf"][r][i] for r in range(1, 32) for i in range(4)] +
                    ["%08x" % v for v in o["acc"]] + ["%04x" % (o["vi"][r] & 0xFFFF) for r in range(1, 16)] +
                    ["%08x" % o["i"], "%08x" % o["q"], "%04x" % (o["mac"] & 0xFFFF), "%06x" % (o["clip"] & 0xFFFFFF),
                     "%x" % o["tpc"]])


def _flat(st):
    w = [0] * 153
    for r in range(32):
        for i in range(4):
            w[4 * r + i] = st["vf"][r][i]
    w[128:132] = st["acc"]
    for r in range(16):
        w[132 + r] = st["vi"][r]
    w[148], w[149], w[150], w[151], w[152] = st["i"], st["q"], st["mac"], st["clip"], st["status"]
    return w


@pytest.mark.wsl
@pytest.mark.parametrize("seed", [1, 2, 3, 4, 5, 6])
@pytest.mark.parametrize("mode", ["memory", "locals"])
def test_generated_c_matches_the_reference(tmp_path, seed, mode):
    r = rnd(seed)
    prog = None
    for attempt in range(20):                       # skip the (rare) programs the translator refuses
        prog = random_program(r, n=80)
        code = to_bytes(prog)
        try:
            src, stats = G.translate(code, [0], "tprog", mode)
            break
        except G.Unsupported:
            prog = None
    assert prog is not None
    sim = Sim(code)
    cases, exp = [], {}
    for k in range(64):
        st = random_state(r)
        cases.append(_flat(st))
        exp["K%d" % k] = _expect(sim.run(st, 0))
    d = tmp_path
    (d / "tprog.c").write_text(src, newline="\n")
    (d / "driver.c").write_text(DRIVER, newline="\n")
    NL = chr(10)
    with open(d / "cases.h", "w", newline=NL) as f:
        f.write("#define NCASES %d" % len(cases) + NL)
        f.write("static const uint32_t cases[][153] = {%s};" % ",".join(
            "{%s}" % ",".join("0x%xu" % v for v in c) for c in cases) + NL)
    rt = wsl_path(os.path.join(ROOT, "runtime"))
    script = ("set -e\ncd %s\ngcc -O2 -std=gnu11 -frounding-math -ffp-contract=off -fno-fast-math -fno-math-errno "
              "-Wall -Wno-unused-label -I%s -I. -o t driver.c tprog.c -lm\n./t\n" % (wsl_path(str(d)), rt))
    (d / "run.sh").write_text(script, newline="\n")
    p = subprocess.run(["wsl.exe", "-d", "Debian", "--", "bash", wsl_path(str(d / "run.sh"))],
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    got = dict(l.split(" ", 1) for l in p.stdout.decode().splitlines() if l.startswith("K"))
    bad = [k for k in exp if got.get(k) != exp[k]]
    if bad:
        a, b = exp[bad[0]].split(), got.get(bad[0], "").split()
        names = ["vf%02d%s" % (r_, "xyzw"[i]) for r_ in range(1, 32) for i in range(4)] + \
            ["acc" + c for c in "xyzw"] + ["vi%02d" % r_ for r_ in range(1, 16)] + ["I", "Q", "MAC", "clip", "TPC"]
        diff = [(names[i], a[i], b[i] if i < len(b) else None) for i in range(len(a)) if i >= len(b) or a[i] != b[i]]
        pytest.fail("%d of %d states differ; first %s: %s" % (len(bad), len(exp), bad[0], diff[:8]))
