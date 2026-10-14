"""Back-end v5: the S2/S3 size levers on hand-encoded R5900 programs whose expected results follow
from the EE instruction semantics, run on the host (WSL gcc) for v4 (reference) and the v5 steps; and the mutation
checks: each rule of the implementation (ps2rc/backend_c.py) is switched off in a COPY of the package (a source-text
replacement, never the real file) and the named cases must then fail - so every rule is needed and covered.

  nocs   (cs_pairs)        NOCS_BASIC, NOCS_LIVE (restored value read), NOCS_PEEK (slot read by another load),
                           NOCS_PTR (slot read through a register holding sp + k)
  q128   (h<n> locals)     Q_COPY (narrowed temp holding 128-bit data), Q_ARG / Q_RET (128-bit argument / result),
                           Q_MIX (context MMI op reading an upper half), Q_MOVE (paddub move idiom)
  hilo/acc/fcc             X_LO / X_ACC / X_FCC: written back before a transfer to an exempt (exact) function
  clamp1                   C1_RAW (raw SWC1 of a loaded value), C1_ARG (loaded value passed as an argument),
                           C1_OK (clamped once at the load)
  fclean                   FCL (f0 reloaded after a call is not clean)
"""
import os
import shutil
import struct
import subprocess
import sys

import pytest
from ps2rc import config  # noqa: E402

from ps2rc import backend_c
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.image import Image, Segment
from r5900_enc import (A0, A1, A2, NOP, RA, S0, SP, T0, V0, V1, ZERO, addiu, addu, beq, daddu, i_type, jal, jr,
                       ld, lq, lui, lw, mmi, paddub, r_type, sd, sq)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_v5")
BASE, SIZE = 0x1000, 0x2000
T1 = 9


def mtc1(rt, fs): return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11)
def lwc1(ft, off, base): return i_type(0x31, base, ft, off)
def swc1(ft, off, base): return i_type(0x39, base, ft, off)
def cop1s(fn, fd, fs, ft): return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn
def add_s(fd, fs, ft): return cop1s(0x00, fd, fs, ft)
def mul_s(fd, fs, ft): return cop1s(0x02, fd, fs, ft)
def mula_s(fs, ft): return cop1s(0x1A, 0, fs, ft)
def madd_s(fd, fs, ft): return cop1s(0x1C, fd, fs, ft)
def c_lt_s(fs, ft): return cop1s(0x34, 0, fs, ft)
def bc1t(pc, target): return (0x11 << 26) | (0x08 << 21) | (1 << 16) | (((target - pc - 4) >> 2) & 0xFFFF)
def mult(rs, rt): return r_type(0x18, rs, rt, 0)
def mflo(rd): return r_type(0x12, rd=rd)
def pcpyud(rd, rs, rt): return mmi(0x29, 0x0E, rd, rs, rt)
def j(target): return (0x02 << 26) | ((target >> 2) & 0x3FFFFFF)


def frame(body, size=32):
    return [addiu(SP, SP, -size), sd(RA, size - 16, SP)] + body + [ld(RA, size - 16, SP), jr(RA),
                                                                   addiu(SP, SP, size)]


NOCS_BASIC, NOCS_LEAF, NOCS_LIVE, NOCS_PEEK, NOCS_PTR = 0x1000, 0x1080, 0x1100, 0x1180, 0x1200
Q_COPY, Q_ARG, Q_ARG_C, Q_RET, Q_RET_C, Q_MIX, Q_MOVE = 0x1300, 0x1340, 0x1380, 0x13C0, 0x1400, 0x1440, 0x1480
X_LO, X_LO_C, X_FCC, X_FCC_C, X_ACC, X_ACC_C = 0x1500, 0x1540, 0x1580, 0x15C0, 0x1600, 0x1640
C1_RAW, C1_ARG, C1_ARG_C, C1_OK, FCL, FCL_C = 0x1680, 0x16C0, 0x1700, 0x1740, 0x1780, 0x17C0
CTX_SP_F, CTX_SP_G, CTX_SP_K, CTX_E, CTX_F, CTX_K = 0x1800, 0x1840, 0x1880, 0x18C0, 0x1900, 0x1940
CTX_E2, CTX_G2, CTX_E3, CTX_F3 = 0x1980, 0x19C0, 0x1A00, 0x1A40
DATA = 0x3000                                          # guest data used by the cases (harness fills it)

PROGRAM = {
    # s0 saved/restored around a call; the caller's s0 must survive (C local in ABI mode: no code for the pair)
    NOCS_BASIC: [addiu(SP, SP, -32), sq(S0, 0, SP), sd(RA, 16, SP), daddu(S0, A0, ZERO), jal(NOCS_LEAF), NOP,
                 addu(V0, V0, S0), ld(RA, 16, SP), lq(S0, 0, SP), jr(RA), addiu(SP, SP, 32)],
    NOCS_LEAF: [jr(RA), addiu(V0, A0, 1)],
    # the restored s0 is read afterwards (v0 = restored s0 + 2*a0): the pair must stay
    NOCS_LIVE: [addiu(SP, SP, -32), sq(S0, 0, SP), daddu(S0, A0, ZERO), addu(V1, S0, S0), lq(S0, 0, SP),
                daddu(V0, S0, V1), jr(RA), addiu(SP, SP, 32)],
    # the save slot is read by another load (lw of the saved low word)
    NOCS_PEEK: [addiu(SP, SP, -32), sd(S0, 0, SP), daddu(S0, A0, ZERO), lw(V0, 0, SP), ld(S0, 0, SP), jr(RA),
                addiu(SP, SP, 32)],
    # ... through a register that holds sp + k
    NOCS_PTR: [addiu(SP, SP, -32), sd(S0, 8, SP), daddu(S0, A0, ZERO), addiu(A1, SP, 4), lw(V0, 4, A1),
               ld(S0, 8, SP), jr(RA), addiu(SP, SP, 32)],
    # 16-byte copy through a temporary the width analysis may narrow
    Q_COPY: [lq(T0, 0, A0), sq(T0, 0, A1), jr(RA), NOP],
    # 128-bit argument: the callee stores its a1 (all 128 bits)
    Q_ARG: frame([lq(A1, 0, A0), jal(Q_ARG_C), daddu(A0, A2, ZERO)]),
    Q_ARG_C: [sq(A1, 0, A0), jr(RA), NOP],
    # 128-bit result in v0, stored by the caller
    Q_RET: frame([jal(Q_RET_C), NOP, lui(T0, 0), addiu(T0, T0, DATA + 0x200), sq(V0, 0, T0)]),
    Q_RET_C: [lq(V0, 0, A0), jr(RA), NOP],
    # an MMI instruction that stays on the context reads the upper half of a register loaded by LQ
    Q_MIX: [lq(T0, 0, A0), pcpyud(T1, T0, T0), sq(T1, 0, A1), jr(RA), NOP],
    Q_MOVE: [lq(T0, 0, A0), paddub(T1, T0, ZERO), sq(T1, 0, A1), jr(RA), NOP],
    # LO / ACC / condition bit set before a call of an exact function that reads them
    X_LO: frame([mult(A0, A1), jal(X_LO_C), NOP]),
    X_LO_C: [mflo(V0), jr(RA), NOP],
    X_ACC: frame([mula_s(12, 13), jal(X_ACC_C), NOP]),
    X_ACC_C: [madd_s(0, 14, 15), jr(RA), NOP],
    X_FCC: frame([c_lt_s(12, 13), jal(X_FCC_C), NOP]),
    X_FCC_C: [addiu(V0, ZERO, 2), bc1t(X_FCC_C + 4, X_FCC_C + 16), NOP, addiu(V0, ZERO, 1), jr(RA), NOP],
    # a loaded value stored raw (and used through the clamp); f4/f5 (f0/f1 are written back at the return)
    C1_RAW: [lwc1(4, 0, A0), add_s(5, 4, 4), swc1(4, 0, A1), swc1(5, 4, A1), jr(RA), NOP],
    # a loaded value used through the clamp and passed as an argument, stored raw by the callee
    C1_ARG: frame([lwc1(12, 0, A0), add_s(4, 12, 12), jal(C1_ARG_C), NOP]),
    C1_ARG_C: [swc1(12, 0, A1), jr(RA), NOP],
    # a loaded value used only through the clamp: clamped once at the load
    C1_OK: [lwc1(4, 0, A0), mul_s(5, 4, 4), add_s(6, 4, 5), swc1(6, 0, A1), jr(RA), NOP],
    # f0 is clean before the call, reloaded (raw) after it
    FCL: frame([lui(T0, 0x3F80), mtc1(T0, 2), add_s(0, 2, 2), jal(FCL_C), NOP, mul_s(3, 0, 2), swc1(3, 0, A1)]),
    FCL_C: [lwc1(0, 0, A0), jr(RA), NOP],
    # ctx32 (the harness leaves a stale upper word in the slot first, as a one-word write-back may): K stores its sp
    # as a doubleword - every v5 load of the sp slot takes the low word; G passes sp through unchanged
    CTX_SP_F: frame([jal(CTX_SP_G), NOP]),
    CTX_SP_G: [j(CTX_SP_K), NOP],
    CTX_SP_K: [i_type(0x19, SP, SP, -16), sd(SP, 0, A1), jr(RA), i_type(0x19, SP, SP, 16)],   # daddiu: sp wide
    # an argument passed on by a tail call (F) or a call (F3) before the callee writes it: the caller must write it
    # back as a whole doubleword (K stores it as one); G2 reads it only as a narrowed local: one word is enough
    CTX_E: frame([addiu(A0, ZERO, 0x1234), jal(CTX_F), NOP]),
    CTX_F: [j(CTX_K), NOP],
    CTX_K: [sd(A0, 0, A1), jr(RA), NOP],
    CTX_E2: frame([addiu(A0, ZERO, 0x1234), jal(CTX_G2), NOP]),
    CTX_G2: [jr(RA), addu(V0, A0, A0)],
    CTX_E3: frame([addiu(A0, ZERO, 0x1234), jal(CTX_F3), NOP]),
    CTX_F3: frame([jal(CTX_K), NOP]),
}
SEEDS = sorted(PROGRAM)
EXEMPT = frozenset({X_LO_C, X_ACC_C, X_FCC_C})          # read state no ABI boundary passes (exact functions)
CODE = [(0x1000, 0x1B00)]

Q16 = bytes([0x11, 0x22, 0x33, 0x44, 0x81, 0x92, 0xA3, 0xB4, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC])

HARNESS = r"""
#include <stdio.h>
#include <setjmp.h>
#include <fenv.h>
#include "r5900_rt.h"
#include "unit_image.h"
#include "unit_rc.h"
static jmp_buf jb;
static const char *fwhat;
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; (void)pc; (void)val; fwhat = what; longjmp(jb, 1); }
static uint8_t ram[0x10000];
static R5900Ctx c;
static const uint8_t q16[16] = {%(q16)s};
static void reset(void)
{
    memset(&c, 0, sizeof c); memset(ram, 0, sizeof ram); memcpy(ram + 0x1000, unit_image, sizeof unit_image);
    c.ram = ram; c.ram_mask = sizeof ram - 1; c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
    for (int r = 1; r < 32; r++) if (r != 29 && r != 31) { c.r[r].d[0] = 0x5A5A0000u + 0x100u * r; c.r[r].d[1] = 0xC0FFEE00ull + r; }
    c.r[16].d[0] = 0x0102030405060708ull; c.r[16].d[1] = 0xA1A2A3A4A5A6A7A8ull;
    memcpy(ram + 0x3000, q16, 16);
    c.r[4].d[0] = 0x3000; c.r[5].d[0] = 0x3100; c.r[6].d[0] = 0x3180;
    c.lo.d[0] = 0x77777777u; c.fcr31 = 0; c.acc.u = 0x40000000u;
}
static int run(void (*fn)(R5900Ctx *))
{
    if (!setjmp(jb)) { fn(&c); return 0; }
    printf(" fatal=%%s", fwhat);
    return 1;
}
static void hex(const char *tag, uint32_t a, int n)
{
    printf(" %%s=", tag);
    for (int i = 0; i < n; i++) printf("%%02x", ram[a + i]);
}
static void fset(int n, uint32_t v) { c.fpr[n].u = v; }
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fesetround(FE_TOWARDZERO);
    reset(); c.r[4].d[0] = 40;
    printf("NOCS_BASIC"); if (!run(fn_00001000)) printf(" v0=%%llx s0=%%llx:%%llx", (unsigned long long)c.r[2].d[0],
        (unsigned long long)c.r[16].d[1], (unsigned long long)c.r[16].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 40;
    printf("NOCS_LIVE"); if (!run(fn_00001100)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 40;
    printf("NOCS_PEEK"); if (!run(fn_00001180)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 40;
    printf("NOCS_PTR"); if (!run(fn_00001200)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); printf("Q_COPY"); if (!run(fn_00001300)) hex("m", 0x3100, 16); printf("\n");
    reset(); printf("Q_ARG"); if (!run(fn_00001340)) hex("m", 0x3180, 16); printf("\n");
    reset(); printf("Q_RET"); if (!run(fn_000013c0)) hex("m", 0x3200, 16); printf("\n");
    reset(); printf("Q_MIX"); if (!run(fn_00001440)) hex("m", 0x3100, 16); printf("\n");
    reset(); printf("Q_MOVE"); if (!run(fn_00001480)) hex("m", 0x3100, 16); printf("\n");
    reset(); c.r[4].d[0] = 1000; c.r[5].d[0] = 77;
    printf("X_LO"); if (!run(fn_00001500)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); fset(12, 0x40400000u); fset(13, 0x40800000u); fset(14, 0x3F800000u); fset(15, 0x40000000u);
    printf("X_ACC"); if (!run(fn_00001600)) printf(" f0=%%08x", c.fpr[0].u); printf("\n");
    reset(); fset(12, 0x3F800000u); fset(13, 0x40000000u);
    printf("X_FCC"); if (!run(fn_00001580)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); rt_sw(&c, 0x3000, 0x7F800000u);
    printf("C1_RAW"); if (!run(fn_00001680)) hex("m", 0x3100, 8); printf("\n");
    reset(); rt_sw(&c, 0x3000, 0x7F800001u);
    printf("C1_ARG"); if (!run(fn_000016c0)) hex("m", 0x3100, 4); printf("\n");
    reset(); rt_sw(&c, 0x3000, 0x00000001u);
    printf("C1_OK"); if (!run(fn_00001740)) hex("m", 0x3100, 4); printf("\n");
    reset(); rt_sw(&c, 0x3000, 0x7F800000u);
    printf("FCL"); if (!run(fn_00001780)) hex("m", 0x3100, 4); printf("\n");
#ifdef CTX32_CASES
    /* the v5 ctx32 contract: the upper word of a one-word written slot is stale - poison it */
    reset(); c.r[29].w[1] = 0xDEADBEEFu;
    printf("CTX_SP"); if (!run(fn_00001800)) hex("m", 0x3100, 8); printf("\n");
    reset(); c.r[4].w[1] = 0xDEADBEEFu;
    printf("CTX_TAIL"); if (!run(fn_000018c0)) hex("m", 0x3100, 8); printf("\n");
    reset(); c.r[4].w[1] = 0xDEADBEEFu;
    printf("CTX_CALL"); if (!run(fn_00001a00)) hex("m", 0x3100, 8); printf("\n");
    reset();
    printf("CTX_LOW"); if (!run(fn_00001980)) printf(" v0=%%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
#endif
    return 0;
}
"""


def _le(b):
    return b.hex()


EXPECTED = {
    "NOCS_BASIC": "v0=%x s0=a1a2a3a4a5a6a7a8:102030405060708" % (41 + 40),   # (a0+1) + a0; s0 kept
    "NOCS_LIVE": "v0=%x" % ((0x0102030405060708 + 80) & (1 << 64) - 1),       # restored s0 + 2*a0
    "NOCS_PEEK": "v0=5060708",                                                  # sign-extended low word of s0
    "NOCS_PTR": "v0=5060708",
    "Q_COPY": "m=" + _le(Q16),
    "Q_ARG": "m=" + _le(Q16),
    "Q_RET": "m=" + _le(Q16),
    "Q_MIX": "m=" + _le(Q16[8:] + Q16[8:]),                                    # pcpyud t1,t0,t0: {t0.hi, t0.hi}
    "Q_MOVE": "m=" + _le(Q16),
    "X_LO": "v0=%x" % (1000 * 77),                                              # LO of mult
    "X_ACC": "f0=41600000",                                                     # 3*4 + 1*2 = 14 (ACC from mula)
    "X_FCC": "v0=2",                                                            # 1.0 < 2.0: branch taken
    "C1_RAW": "m=" + _le(struct.pack("<II", 0x7F800000, 0x7F7FFFFF)),          # raw Inf stored; FMAX+FMAX = FMAX
    "C1_ARG": "m=" + _le(struct.pack("<I", 0x7F800001)),                       # the raw NaN bits reach the callee
    "C1_OK": "m=" + _le(struct.pack("<I", 0)),                                  # denormal -> 0: 0*0 + 0
    "FCL": "m=" + _le(struct.pack("<I", 0x7F7FFFFF)),                          # f0 = Inf -> FMAX, * 1.0
}
EXPECTED_CTX = {                                        # v5s3 (ctx32) only: run with stale upper words in the slots
    "CTX_SP": "m=" + _le(struct.pack("<Q", 0x8000 - 32 - 16)),                 # K's sp, a canonical doubleword
    "CTX_TAIL": "m=" + _le(struct.pack("<Q", 0x1234)),
    "CTX_CALL": "m=" + _le(struct.pack("<Q", 0x1234)),
    "CTX_LOW": "v0=%x" % 0x2468,
}


def image():
    mem = bytearray(SIZE)
    for a, words in PROGRAM.items():
        for k, w in enumerate(words):
            struct.pack_into("<I", mem, a - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<unit>", entry=BASE, gp=0, segments=[seg], overlay_slots=[])


def _wsl_path(p):
    return config.posix_path(p)


def build_and_run(tag, variant, bc=backend_c, crash_ok=False, ctx=False):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    funcs = fs.functions
    pa = narrowing_plan(img, funcs, abi=True, roots=set(SEEDS))
    pe = narrowing_plan(img, funcs, abi=False, roots=set(SEEDS))
    opts = {"cache": True, "fpu_fast": True, "abi": True, "livein": argument_livein(img, funcs),
            "narrow": {a: (pe[a] if a in EXEMPT else pa[a]) for a in funcs}, "abi_exempt": EXEMPT}
    if variant != "v4":
        opts["v5"] = bc.V5_LEVELS[variant]
    src, header = bc.emit_program(img, fs, roots=SEEDS, **opts)
    out = os.path.join(OUT, tag)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    data = img.read(BASE, SIZE)
    open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(
        "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data)))
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(
        HARNESS % {"q16": ",".join(str(b) for b in Q16)})
    script = os.path.join(out, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "cd %s\nrm -f unit\ngcc -O1 -std=gnu11 -w -frounding-math -ffp-contract=off %s-I%s -I. unit_main.c "
        "unit_rc.c %s -lm -o unit || exit 99\n./unit\n" % (
            _wsl_path(out), "-DCTX32_CASES " if ctx else "", _wsl_path(os.path.join(ROOT, "runtime")),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    p = subprocess.run(config.bash_argv(script),
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode != 99, p.stderr.decode(errors="replace")[-3000:]
    assert crash_ok or p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    return {ln.split(" ", 1)[0]: ln.split(" ", 1)[1].strip() if " " in ln else "" for ln in
            p.stdout.decode().splitlines() if ln.strip()}, src


# ---------------------------------------------------------------- the cases, v4 and every v5 step
@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v4", "v5s1", "v5s2", "v5s3", "v5s4"])
def test_cases_behave_like_the_ee(variant):
    ctx = variant == "v5s3"                             # the ctx32 contract cases (stale upper words) from v5s3 on
    got, _src = build_and_run(variant, variant, ctx=ctx)
    exp = dict(EXPECTED, **(EXPECTED_CTX if ctx else {}))
    bad = {k: (got.get(k), v) for k, v in exp.items() if got.get(k) != v}
    assert not bad, bad


@pytest.mark.wsl
def test_levers_change_the_code_where_expected():
    """The v5s3 code really uses the levers on these cases (else the cases would prove nothing)."""
    _got, src = build_and_run("v5s3_code", "v5s3")
    body = lambda a: src.split("void fn_%08x(R5900Ctx *c)\n{" % a)[1].split("\n}\n")[0]    # noqa: E731
    assert "rm_sq" not in body(NOCS_BASIC) and "rm_lq" not in body(NOCS_BASIC)          # pair dropped
    assert "rm_lq" in body(NOCS_LIVE) and "rm_ld" in body(NOCS_PEEK)                     # pairs kept
    assert "h8 = q_.d[1]" in body(Q_COPY) and "uint64_t r8" in body(Q_COPY)              # local, not narrowed
    assert "rl_mult(&lo_0, &hi_0" in body(X_LO) and "c->lo.d[0] = lo_0;" in body(X_LO)
    assert "f4 = f_in5(rm_lw" in body(C1_OK) and "f_in5(rm_lw" not in body(C1_RAW) + body(C1_ARG)
    assert "c->r[4].w[0] = (uint32_t)r4;" in body(CTX_E2)                                # G2 reads a0 low only
    assert "c->r[4].d[0] = (uint64_t)(int64_t)r4;" in body(CTX_E) + body(CTX_E3)          # F, F3 pass it on
    assert "c->r[29].w[0] = (uint32_t)r29;" in body(CTX_SP_F)


# ---------------------------------------------------------------- mutation checks on a copy of the package
MUTANTS = {
    # name: (text in backend_c.py, replacement, variant, cases that must fail)
    "nocs_live": ("        if any(any(live[t] >> r & 1 for t, _e in f.succ[a]) for a in mine):\n            continue\n",
                  "", "v5s3", ["NOCS_LIVE"]),
    "nocs_peek": ("        if any(a not in mine and lo < slot + width and slot < lo + w for a, lo, w in loads):\n"
                  "            continue\n", "", "v5s3", ["NOCS_PEEK", "NOCS_PTR"]),
    "nocs_ptr": ("            if base in exact:\n                loads.append((a, exact[base] + off, _FRAME_W[m]))",
                 "            if base == 29 and base in exact:\n"
                 "                loads.append((a, exact[base] + off, _FRAME_W[m]))", "v5s3", ["NOCS_PTR"]),
    "q128_narrow": ("    if conv and narrow:\n        narrow = narrow - _q128_unnarrow(f, conv, narrow)\n", "",
                    "v5s2", ["Q_COPY"]),
    "q128_hcall": ("                h &= am\n", "                h = 0\n", "v5s2", ["Q_ARG"]),
    "q128_hreload": ('                      ["h%d = c->r[%d].d[1];" % (n, n) for n in used_h if n in (2, 3)])',
                     "                      [])", "v5s2", ["Q_RET"]),
    "q128_hsync": ('        pre += "".join(" c->r[%d].d[1] = h%d;" % (r, r) for r in hs)\n', "", "v5s2", ["Q_MIX"]),
    "xstore": ("            out += xstore ", "            out += [] ", "v5s2", ["X_LO", "X_ACC", "X_FCC"]),
    "clamp1_raw": ("            raw |= 1 << o[1]", "            clamped |= 1 << o[1]", "v5s2", ["C1_RAW"]),
    "clamp1_flush": ("            ro |= flush[a]\n            ao |= flush[a]\n            c, r = rd[a]",
                     "            c, r = rd[a]", "v5s2", ["C1_ARG"]),
    "fclean_f01": ("                v = (out[0] & ~0x3, out[1] and keep_acc)", "                v = (out[0], out[1] and keep_acc)",
                   "v5s2", ["FCL"]),
    "ctx32_load": ('            return "(int32_t)c->r[%d].w[0]" % n if n in narrow else "SX32(c->r[%d].w[0])" % n',
                   '            return "(int32_t)c->r[%d].d[0]" % n if n in narrow else "c->r[%d].d[0]" % n',
                   "v5s3", ["CTX_SP"]),
    "lowonly_exits": ('        for e in f.exits[a]:\n            if e[0] in ("tail", "call"):\n'
                      '                exposed |= out & readers(e[1])\n', "", "v5s3", ["CTX_TAIL"]),
    "lowonly_calls": ("            if e is not None:\n                exposed |= out & readers(e[1])\n",
                      "            pass\n", "v5s3", ["CTX_CALL"]),
}


def _mutant_package(name):
    """A copy of src/ps2rc as package ps2rc_mut_<name> with one replacement in backend_c.py (must match once)."""
    old, new, _v, _f = MUTANTS[name]
    pkg = "ps2rc_mut_" + name
    base = os.path.join(OUT, "mutpkg")
    dst = os.path.join(base, pkg)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "ps2rc"), dst, ignore=shutil.ignore_patterns("__pycache__"))
    path = os.path.join(dst, "backend_c.py")
    text = open(path, encoding="utf-8").read()
    assert text.count(old) == 1, "mutation anchor of %s not found exactly once" % name
    open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new))
    if base not in sys.path:
        sys.path.insert(0, base)
    import importlib
    return importlib.import_module(pkg + ".backend_c")


@pytest.mark.wsl
@pytest.mark.parametrize("mutant", sorted(MUTANTS))
def test_each_rule_is_needed(mutant):
    _old, _new, variant, must_fail = MUTANTS[mutant]
    bc = _mutant_package(mutant)
    ctx = variant == "v5s3"
    got, _src = build_and_run("mut_" + mutant, variant, bc=bc, crash_ok=True, ctx=ctx)
    exp = dict(EXPECTED, **(EXPECTED_CTX if ctx else {}))
    failed = sorted(k for k, v in exp.items() if got.get(k) != v)
    assert set(must_fail) <= set(failed), "mutant %s: failed %s, expected at least %s" % (mutant, failed, must_fail)
