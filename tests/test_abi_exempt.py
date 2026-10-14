"""Back-end integration of the non-ABI detector: ABI-mode programs with functions that must stay exact
(`abi_exempt`), `j` into a function's own code as a goto, and the decoder rule for SPECIAL words with nonzero
must-be-zero fields.

Each case is a hand-encoded R5900 program whose expected result follows from the EE instruction semantics (written
next to the case); the generated C runs on the host (WSL gcc). The mutation tests re-run the cases with one rule of
the implementation switched off (monkeypatched) and require the named case to fail - so every rule is needed and
covered:
  xflush     transfers onto an exempt function flush like an ABI call (no full write-back)      -> PTMF_T9, XTAIL_T9
  reloadall  /*RELOADALL*/ after a call of an exempt function is the ABI reload (v0/v1 only)     -> STEP_S5, OUT_T0
  exact      exempt functions translated in ABI mode                                             -> OUT_T0, STEP_S5
  protect    exact functions do not keep their callee-saved context across ABI-mode callees      -> CS_KEEP
  jgoto      `j` into the function's own code stays a tail call (dispatch of a non-start)        -> JLOOP
  jcfg       the dataflow CFG ignores the `j` back edge (FPU clean-state analysis)               -> JLOOP_FPU
"""
import os
import struct
import subprocess

import pytest
from ps2rc import config  # noqa: E402

from ps2rc import backend_c
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment
from ps2rc.r5900 import decode
from ps2rc.r5900.decode import SPECIAL_IGNORED, special_ignored_bits
from r5900_enc import (A0, A1, AT, GP, NOP, RA, S0, SP, T0, T9, V0, V1, ZERO, addiu, addu, beq, daddu, i_type, jal,
                       jr, ld, lq, lui, lw, r_type, sd, sq, sw)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_exempt")
BASE, SIZE = 0x1000, 0x3000
S5 = 21


def j(target): return (0x02 << 26) | ((target >> 2) & 0x3FFFFFF)
def bne(rs, rt, pc, target): return i_type(0x05, rs, rt, (target - pc - 4) >> 2)
def sll(rd, rt, sa): return r_type(0x00, rt=rt, rd=rd, sa=sa)
def xor(rd, rs, rt): return r_type(0x26, rs, rt, rd)
def mtc1(rt, fs): return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11)
def lwc1(ft, off, base): return i_type(0x31, base, ft, off)
def swc1(ft, off, base): return i_type(0x39, base, ft, off)
def add_s(fd, fs, ft): return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x00
def mul_s(fd, fs, ft): return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x02


def frame(body, size=32):
    """Metrowerks-style non-leaf function: frame, ra saved, body, ra restored, return."""
    return [addiu(SP, SP, -size), sd(RA, size - 16, SP)] + body + [ld(RA, size - 16, SP), jr(RA),
                                                                   addiu(SP, SP, size)]


# ---------------------------------------------------------------- the programs
# PTMF_T9 (a Metrowerks __ptmf_scall shape, "1-input t9"): the caller sets t9 right before the call; M reads *t9
PTMF, PTMF_M = 0x1000, 0x1100
# STEP_S5 (the first PoC-2 stress driver): STEP reads and steps its caller's s5, the caller reads s5 after the call
STEP_F, STEP = 0x1200, 0x1300
# OUT_T0 ("2-output"): the callee leaves its result in t0, the caller reads t0 after the call
OUT_F, OUT_O = 0x1400, 0x1480
# XTAIL_T9: like PTMF_T9 but the transfer is a `j` (tail call)
XT_F = 0x1500
# CS_KEEP: an exact function X calls an ABI-mode function Y that changes s0 (64-bit save/restore through its frame)
# and calls an exact function Z in between - the full flush before Z writes Y's s0 into the context; X's caller must
# still find its own s0 (all 128 bits) in the context after X returns
CS_X, CS_Y, CS_Z = 0x1600, 0x1680, 0x1700
# JLOOP: a counting loop closed by `j` to a label that is not a function start
JLOOP = 0x1800
# JLOOP_FPU: a loop closed by `j`; f1 is clean on loop entry and reloaded raw (LWC1) before the back edge
JFPU = 0x1900
# LAX: Sony libipu shape: inline constants 0x00ff00ff executed as no-ops before the return
LAX = 0x1A00

PROGRAM = {
    PTMF: frame([addiu(T9, SP, 8), sw(A1, 0, T9), jal(PTMF_M), NOP]),
    PTMF_M: [lw(AT, 0, T9), jr(RA), addu(V0, A0, AT)],
    STEP_F: [addiu(SP, SP, -32), sq(S5, 0, SP), sd(RA, 16, SP), daddu(S5, A0, ZERO), jal(STEP), NOP,
             daddu(V0, S5, ZERO), ld(RA, 16, SP), lq(S5, 0, SP), jr(RA), addiu(SP, SP, 32)],
    STEP: [sll(T0, S5, 13), xor(S5, S5, T0), jr(RA), NOP],
    OUT_F: frame([jal(OUT_O), NOP, daddu(V0, T0, ZERO)]),
    OUT_O: [jr(RA), addiu(T0, A0, 1)],
    XT_F: [addiu(T9, SP, -16), sw(A1, 0, T9), j(PTMF_M), NOP],
    # X switches gp without restoring it ("2-callee-saved gp", like the interrupt dispatchers): exact
    CS_X: [addiu(SP, SP, -32), sd(RA, 16, SP), jal(CS_Y), NOP, ld(RA, 16, SP), lui(GP, 0x99), jr(RA),
           addiu(SP, SP, 32)],
    CS_Y: [addiu(SP, SP, -32), sd(S0, 0, SP), sd(RA, 16, SP), addiu(S0, ZERO, 77), addiu(T9, ZERO, 0x40), jal(CS_Z),
           NOP, ld(RA, 16, SP), ld(S0, 0, SP), jr(RA), addiu(SP, SP, 32)],
    CS_Z: [jr(RA), addu(V0, T9, ZERO)],                    # reads the t9 its caller set right before: exact
    JLOOP: [addiu(V0, ZERO, 0), daddu(V1, A0, ZERO),
            beq(V1, ZERO, JLOOP + 8, JLOOP + 28), NOP, addiu(V0, V0, 3), j(JLOOP + 8), addiu(V1, V1, -1),
            jr(RA), NOP],
    JFPU: [lui(T0, 0x3F00), mtc1(T0, 2), add_s(1, 2, 2), addiu(V1, ZERO, 2),
           mul_s(3, 1, 2), swc1(3, 0, A1), addiu(A1, A1, 4), lwc1(1, 0, A0), addiu(V1, V1, -1),
           beq(V1, ZERO, JFPU + 36, JFPU + 52), NOP, j(JFPU + 16), NOP,
           jr(RA), NOP],
    LAX: [addiu(V0, A0, 7), NOP, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, jr(RA), NOP],
}
SEEDS = [PTMF, PTMF_M, STEP_F, STEP, OUT_F, OUT_O, XT_F, CS_X, CS_Y, CS_Z, JLOOP, JFPU, LAX]
# what the non-ABI detector says (checked below). Function discovery (v2) also makes the `j` targets JLOOP+8 and
# JFPU+16 functions of their own (crt0 loop entries: "3-fragment" + "1-input"). LAX is ABI-safe: the
# no-op dsra32 zero,ra,3 does not read ra as data (abi.py ignores instructions that only write $zero)
EXEMPT = frozenset({PTMF_M, STEP, OUT_O, CS_X, CS_Z, JLOOP + 8, JFPU + 16})
CODE = [(0x1000, 0x1B00)]
JLOOP_N = 3000000                  # iterations: a `j` emitted as a nested tail call would overflow the host stack

HARNESS = r"""
#include <stdio.h>
#include <setjmp.h>
#include <fenv.h>
#include "r5900_rt.h"
#include "unit_image.h"
#include "unit_rc.h"
static jmp_buf jb;
static const char *fwhat;
static uint32_t fpc;
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; (void)val; fwhat = what; fpc = pc; longjmp(jb, 1); }
static uint8_t ram[0x10000];
static R5900Ctx c;
static void reset(void)
{
    memset(&c, 0, sizeof c); memset(ram, 0, sizeof ram); memcpy(ram + 0x1000, unit_image, sizeof unit_image);
    c.ram = ram; c.ram_mask = sizeof ram - 1; c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
    for (int r = 1; r < 32; r++) if (r != 29 && r != 31) { c.r[r].d[0] = 0x5A5A0000u + 0x100u * r; c.r[r].d[1] = 0xC0FFEE00ull + r; }
    c.r[25].d[0] = 0x4000;                               /* a stale t9 points at other data */
    rt_sw(&c, 0x4000, 0x11111111u);
}
static int run(void (*fn)(R5900Ctx *))
{
    if (!setjmp(jb)) { fn(&c); return 0; }
    printf(" fatal=%s@%x", fwhat, fpc);
    return 1;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);                   /* a crashing case keeps the lines printed before it */
    fesetround(FE_TOWARDZERO);
    reset(); c.r[4].d[0] = 100; c.r[5].d[0] = 23;
    printf("PTMF_T9"); if (!run(fn_00001000)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 0x1234;
    printf("STEP_S5"); if (!run(fn_00001200)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 41;
    printf("OUT_T0"); if (!run(fn_00001400)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 100; c.r[5].d[0] = 23;
    printf("XTAIL_T9"); if (!run(fn_00001500)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[16].d[0] = 0x1234; c.r[16].d[1] = 0xABCD;
    printf("CS_KEEP"); if (!run(fn_00001600)) printf(" s0=%llx:%llx v0=%llx", (unsigned long long)c.r[16].d[1],
                                                  (unsigned long long)c.r[16].d[0], (unsigned long long)c.r[2].d[0]);
    printf("\n");
    reset(); c.r[4].d[0] = JLOOP_N;
    printf("JLOOP"); if (!run(fn_00001800)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    reset(); c.r[4].d[0] = 0x3000; c.r[5].d[0] = 0x3100; rt_sw(&c, 0x3000, 0x7F800000u);
    printf("JLOOP_FPU"); if (!run(fn_00001900)) printf(" out=%08x,%08x", rt_lw(&c, 0x3100), rt_lw(&c, 0x3104));
    printf("\n");
    reset(); c.r[4].d[0] = 9;
    printf("LAX"); if (!run(fn_00001a00)) printf(" v0=%llx", (unsigned long long)c.r[2].d[0]); printf("\n");
    return 0;
}
"""


def sx32(v):
    v &= 0xFFFFFFFF
    return v | 0xFFFFFFFF00000000 if v & 0x80000000 else v


EXPECTED = {
    "PTMF_T9": "v0=%x" % (100 + 23),                       # lw at,0(t9) reads what the caller stored at sp+8
    "STEP_S5": "v0=%x" % (0x1234 ^ sx32(0x1234 << 13)),    # s5 ^= sll(s5, 13), read back by the caller
    "OUT_T0": "v0=%x" % 42,                                # t0 = a0 + 1
    "XTAIL_T9": "v0=%x" % (100 + 23),
    "CS_KEEP": "s0=abcd:1234 v0=40",                       # s0 is callee-saved: X's caller gets its s0 back
    "JLOOP": "v0=%x" % (3 * JLOOP_N),                      # JLOOP_N iterations of v0 += 3
    "JLOOP_FPU": "out=3f000000,7effffff",                  # 1.0*0.5; then +Inf (clamped to FLT_MAX)*0.5
    "LAX": "v0=%x" % 16,                                   # the 0x00ff00ff words are dsra32 zero,ra,3: no-ops
}


def image(program=PROGRAM):
    mem = bytearray(SIZE)
    for a, words in program.items():
        for k, w in enumerate(words):
            struct.pack_into("<I", mem, a - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<unit>", entry=BASE, gp=0, segments=[seg], overlay_slots=[])


def _wsl_path(p):
    return config.posix_path(p)


def options(variant, img, funcs, exempt):
    """emit_program options of a back-end variant; exempt functions get an abi=False narrowing plan."""
    if variant == "v0":
        return {}
    opts = {"cache": True, "fpu_fast": variant != "v1" and variant != "v2", "abi": variant != "v1" and
            variant not in ("v2", "v3")}
    if variant != "v1":
        pa = narrowing_plan(img, funcs, abi=opts["abi"], roots=set(SEEDS))
        pe = narrowing_plan(img, funcs, abi=False, roots=set(SEEDS))
        opts["narrow"] = {a: (pe[a] if a in exempt else pa[a]) for a in funcs}
    if opts["abi"]:
        opts["livein"] = argument_livein(img, funcs)
    opts["abi_exempt"] = exempt                          # v1-v3: calls of exempt functions reload everything
    if variant.startswith("v5"):
        opts["v5"] = backend_c.V5_LEVELS[variant]
    return opts


def build_and_run(tag, variant, exempt=EXEMPT, crash_ok=False):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    src, header = emit_program(img, fs, roots=SEEDS, **options(variant, img, fs.functions, exempt))
    out = os.path.join(OUT, tag)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    data = img.read(BASE, SIZE)
    open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(
        "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data)))
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(
        "#define JLOOP_N %du\n" % JLOOP_N + HARNESS)
    script = os.path.join(out, "run.sh")
    # -O1: no sibling-call optimisation, so a loop emitted as tail calls really nests (8 MB host stack). Exit code 99:
    # the build failed (never accepted); otherwise the program's own exit status (a crash only with crash_ok)
    open(script, "w", newline="\n", encoding="ascii").write(
        "cd %s\nrm -f unit\ngcc -O1 -std=gnu11 -w -frounding-math -ffp-contract=off -I%s -I. unit_main.c unit_rc.c "
        "%s -lm -o unit || exit 99\nulimit -s 8192\n./unit\n" % (
            _wsl_path(out), _wsl_path(os.path.join(ROOT, "runtime")),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script), env=env, capture_output=True)
    assert p.returncode != 99, p.stderr.decode(errors="replace")[-3000:]
    assert crash_ok or p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    return {ln.split(" ", 1)[0]: ln.split(" ", 1)[1].strip() if " " in ln else "" for ln in
            p.stdout.decode().splitlines() if ln.strip()}


# ---------------------------------------------------------------- static checks (no WSL)
def test_detector_agrees_with_the_exempt_set_of_the_cases():
    from ps2rc.analysis.abi import Unit, analyse
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    res = analyse([Unit("main", img, fs.functions)])
    exact = {s for (_u, s), v in res.items() if v.exact}
    assert exact == EXEMPT, (sorted(hex(a) for a in EXEMPT - exact), sorted(hex(a) for a in exact - EXEMPT))


def test_inline_constant_0x00ff00ff_is_dsra32_and_the_function_is_whole():
    i = decode(0x00FF00FF, 0x1EC0)
    assert i.valid and i.text == "dsra32\tzero,ra,0x3"
    assert special_ignored_bits(0x00FF00FF) == 7 << 21
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    f = fs.functions[LAX]
    assert f.well_formed and LAX + 24 in f.addrs and LAX + 28 in f.addrs      # jr ra + its delay slot


def test_special_words_with_ignored_fields_decode_as_their_base_operation():
    """Every SPECIAL function code with ignored fields: setting all of them gives the base decode (same text, same
    operands); function codes without an R5900 operation stay invalid whatever the fields; other opcode classes keep
    their checks (LUI with rs != 0, BLEZ with rt != 0 stay invalid)."""
    for f, mask in SPECIAL_IGNORED.items():
        base = (3 << 21 | 5 << 16 | 7 << 11 | 9 << 6 | f) & ~mask
        if f == 15:
            base = f                                     # SYNC: stype 0 (0x10 = SYNC.P)
        b = decode(base, 0x1000)
        assert b.valid, hex(base)
        for w in (base | mask, base | (mask & -mask)):
            d = decode(w, 0x1000)
            assert d.valid and (d.mnem, d.ops, d.text) == (b.mnem, b.ops, b.text), (hex(w), d.text, b.text)
            assert special_ignored_bits(w) == w & mask
    for f in (1, 5, 14, 21, 28, 29, 30, 31, 53, 55, 57, 61):
        assert f not in SPECIAL_IGNORED
        assert not decode(f).valid and not decode(f | 0x03FFFFC0).valid
    assert not decode(0x0F << 26 | 1 << 21 | 2 << 16).valid           # lui with rs != 0
    assert not decode(0x06 << 26 | 1 << 16 | 4).valid                 # blez with rt != 0
    assert not decode(15 | 0x05 << 6).valid                           # SYNC with an undefined stype


def test_j_into_own_code_is_a_goto_and_exempt_transfers_are_full_boundaries():
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    funcs = fs.functions
    src, _h = emit_program(img, fs, roots=SEEDS, **options("v4", img, funcs, EXEMPT))
    body = lambda a: src.split("void fn_%08x(R5900Ctx *c)\n{" % a)[1].split("\n}\n")[0]    # noqa: E731
    assert "goto L_%08x;" % (JLOOP + 8) in body(JLOOP) and "rc_dispatch" not in body(JLOOP)
    ptmf = body(PTMF)
    assert "c->r[25].d[0] = " in ptmf.split("fn_%08x(c)" % PTMF_M)[0].split("\n")[-1]   # t9 written back
    assert "fn_00001100(c); r" in ptmf                                                 # RELOADALL after it
    assert "rt_cs_save" in body(CS_X) and "rt_cs_save" not in body(CS_Y)                # exact caller only
    # exempt functions are exact: the return writes back what they changed (t0, s5)
    assert "c->r[8].d[0] = " in body(OUT_O) and "c->r[21].d[0] = " in body(STEP)


# ---------------------------------------------------------------- host runs
@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4", "v5s1", "v5s2", "v5s3", "v5s4"])
def test_cases_behave_like_the_ee(variant):
    got = build_and_run(variant, variant)
    bad = {k: (got.get(k), v) for k, v in EXPECTED.items() if got.get(k) != v}
    assert not bad, bad


def _mutate(monkeypatch, name):
    if name == "xflush":
        monkeypatch.setattr(backend_c, "_xk", lambda kind, target: kind)
    elif name == "reloadall":
        monkeypatch.setattr(backend_c, "_reload_after", lambda target: "/*RELOAD*/")
    elif name == "exact":
        real = backend_c.translate_function

        def tf(img, start, addrs, jt=None, starts=(), cache=False, narrow=frozenset(), fpu_fast=False, abi=False,
               livein=None, exempt=None, program_abi=None, v5=None):
            return real(img, start, addrs, jt, starts, cache, frozenset(), fpu_fast, bool(program_abi), livein,
                        exempt, program_abi, v5)
        monkeypatch.setattr(backend_c, "translate_function", tf)
    elif name == "protect":
        monkeypatch.setattr(backend_c, "_pcall", lambda expr, target=None: expr + ";")
    elif name == "jgoto":                              # the old emission: every `j` is a tail call
        monkeypatch.setattr(backend_c, "_j_goto", lambda target, insns: False)
    elif name == "jcfg":
        real_cfg = backend_c.fn_cfg

        def cfg(img, start, addrs, jump_tables, func_starts):
            from types import SimpleNamespace
            from ps2rc.analysis.width import _Fn
            fn = SimpleNamespace(start=start, addrs=set(addrs),
                                 jump_tables=[(jr_, None, list(t)) for jr_, t in jump_tables.items()])
            f = _Fn(img, fn, set(func_starts))
            f.attach_jump_tables(fn.jump_tables)
            return f
        assert real_cfg
        monkeypatch.setattr(backend_c, "fn_cfg", cfg)


MUTANTS = {"xflush": ("v4", ["PTMF_T9", "XTAIL_T9"]), "reloadall": ("v4", ["STEP_S5", "OUT_T0"]),
           "exact": ("v4", ["OUT_T0", "STEP_S5"]), "protect": ("v4", ["CS_KEEP"]),
           "jgoto": ("v1", ["JLOOP"]), "jcfg": ("v3", ["JLOOP_FPU"])}


@pytest.mark.wsl
@pytest.mark.parametrize("mutant", sorted(MUTANTS))
def test_each_rule_is_needed(monkeypatch, mutant):
    variant, must_fail = MUTANTS[mutant]
    _mutate(monkeypatch, mutant)
    got = build_and_run("mut_" + mutant, variant, crash_ok=True)
    failed = sorted(k for k, v in EXPECTED.items() if got.get(k) != v)
    assert set(must_fail) <= set(failed), "mutant %s: failed %s, expected at least %s" % (mutant, failed, must_fail)
