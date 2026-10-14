"""C back-end control flow beyond PoC-1 (PoC-2 needs): link register written before the delay slot, JALR through the
dispatch table (target read before the delay slot), jump-table switch, indirect tail call, conditional branch to another
function (conditional tail call), SWL/SWR/SDL/SDR at every alignment, BREAK reaching the harness' rt_fatal.

The guest snippets are hand-encoded R5900 words; the expected values come from the MIPS/EE ISA definitions (written
independently below), the generated C runs on the host (WSL gcc)."""
import os
import struct
import subprocess

import pytest
from ps2rc import config  # noqa: E402

from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_backend")

ZERO, AT, V0, V1, A0, A1, T9, SP, RA = 0, 1, 2, 3, 4, 5, 25, 29, 31


def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def addiu(rt, rs, imm): return i_type(0x09, rs, rt, imm)
def sltiu(rt, rs, imm): return i_type(0x0B, rs, rt, imm)
def lui(rt, imm): return i_type(0x0F, 0, rt, imm)
def lw(rt, off, base): return i_type(0x23, base, rt, off)
def ld(rt, off, base): return i_type(0x37, base, rt, off)
def sd(rt, off, base): return i_type(0x3F, base, rt, off)
def swl(rt, off, base): return i_type(0x2A, base, rt, off)
def swr(rt, off, base): return i_type(0x2E, base, rt, off)
def sdl(rt, off, base): return i_type(0x2C, base, rt, off)
def sdr(rt, off, base): return i_type(0x2D, base, rt, off)
def sll(rd, rt, sa): return r_type(0x00, rt=rt, rd=rd, sa=sa)
def addu(rd, rs, rt): return r_type(0x21, rs, rt, rd)
def daddu(rd, rs, rt): return r_type(0x2D, rs, rt, rd)
def jr(rs): return r_type(0x08, rs=rs)
def jalr(rs, rd=RA): return r_type(0x09, rs=rs, rd=rd)
def jal(target): return (0x03 << 26) | ((target >> 2) & 0x3FFFFFF)
def brk(code): return r_type(0x0D) | (code << 16)


def beq(rs, rt, pc, target):
    return i_type(0x04, rs, rt, (target - pc - 4) >> 2)


NOP = 0
BASE, SIZE = 0x1000, 0x1000
TABLE = 0x1500
PROGRAM = {
    # A: JAL writes ra before its delay slot (the slot reads the new ra)
    0x1000: [addiu(SP, SP, -16), sd(RA, 0, SP), jal(0x1100), daddu(A0, RA, ZERO), ld(RA, 0, SP), jr(RA),
             addiu(SP, SP, 16)],
    0x1100: [jr(RA), daddu(V0, A0, ZERO)],
    # B: JALR reads its target before the delay slot changes t9; the callee sees the changed t9
    0x1200: [addiu(SP, SP, -16), sd(RA, 0, SP), jalr(T9), addiu(T9, T9, 8), ld(RA, 0, SP), jr(RA),
             addiu(SP, SP, 16)],
    0x1300: [jr(RA), daddu(V0, T9, ZERO)],
    # C: jump-table switch; the jr delay slot increments the index register the cases read
    0x1400: [sltiu(AT, A0, 4), beq(AT, ZERO, 0x1404, 0x1448), sll(V0, A0, 2), lui(V1, 0), addiu(V1, V1, TABLE),
             addu(V0, V0, V1), lw(V0, 0, V0), jr(V0), addiu(A0, A0, 1),
             jr(RA), addiu(V0, A0, 100), jr(RA), addiu(V0, A0, 200), jr(RA), addiu(V0, A0, 300),
             jr(RA), addiu(V0, A0, 400), NOP, jr(RA), addiu(V0, ZERO, -1)],
    TABLE: [0x1424, 0x142C, 0x1434, 0x143C],
    # D: indirect tail call (jr through a register that is not a jump table)
    0x1600: [jr(T9), addiu(A0, A0, 5)],
    0x1700: [jr(RA), daddu(V0, A0, ZERO)],
    # E: unaligned stores
    0x1800: [swl(A1, 0, A0), jr(RA), NOP],
    0x1810: [swr(A1, 0, A0), jr(RA), NOP],
    0x1820: [sdl(A1, 0, A0), jr(RA), NOP],
    0x1830: [sdr(A1, 0, A0), jr(RA), NOP],
    # F: BREAK must reach the harness
    0x1900: [brk(7), jr(RA), NOP],
    # G: conditional branch to the start of another function = conditional tail call (delay slot runs either way)
    0x1A00: [beq(A0, ZERO, 0x1A00, 0x1B00), addiu(V0, ZERO, 1), jr(RA), addiu(V0, V0, 2)],
    0x1B00: [jr(RA), addiu(V0, V0, 100)],
    # H: v1 written on one path only; the caller's 64-bit v1 must survive the other path untouched (v2 write-back)
    0x1C00: [beq(A0, ZERO, 0x1C00, 0x1C0C), NOP, addiu(V1, ZERO, 5), jr(RA), NOP],
}
SEEDS = [0x1000, 0x1200, 0x1300, 0x1400, 0x1600, 0x1700, 0x1800, 0x1810, 0x1820, 0x1830, 0x1900, 0x1A00, 0x1B00,
         0x1C00]
CODE = [(0x1000, 0x1500), (0x1600, 0x2000)]

# I: a jump-table case that function discovery made the start of another function (seen in a large game's
#    switch): the case is a tail call into that function, which reads s0 set before the switch - a register
#    no ABI rule passes, so even ABI mode must write everything back there
S0 = 16
TABLE_I = 0x1510
PROGRAM_I = {
    0x1D00: [addiu(S0, ZERO, 7), sltiu(AT, A0, 2), beq(AT, ZERO, 0x1D08, 0x1D30), sll(V0, A0, 2), lui(V1, 0),
             addiu(V1, V1, TABLE_I), addu(V0, V0, V1), lw(V0, 0, V0), jr(V0), NOP,
             jr(RA), addiu(V0, A0, 100), jr(RA), addiu(V0, ZERO, -1)],
    TABLE_I: [0x1D28, 0x1E00],
    0x1E00: [jr(RA), daddu(V0, S0, A0)],
}
SEEDS_I = [0x1D00, 0x1E00]


def image(program=PROGRAM):
    mem = bytearray(SIZE)
    for a, words in program.items():
        for k, w in enumerate(words):
            struct.pack_into("<I", mem, a - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<unit>", entry=0x1000, gp=0, segments=[seg], overlay_slots=[])


# ---- reference models (MIPS IV / EE Core ISM, little-endian), written byte-wise
def ref_swl(mem, addr, reg):
    b = addr & 3
    for i in range(b + 1):
        mem[(addr & ~3) + i] = (reg >> (8 * (3 - b + i))) & 0xFF


def ref_swr(mem, addr, reg):
    b = addr & 3
    for i in range(4 - b):
        mem[addr + i] = (reg >> (8 * i)) & 0xFF


def ref_sdl(mem, addr, reg):
    b = addr & 7
    for i in range(b + 1):
        mem[(addr & ~7) + i] = (reg >> (8 * (7 - b + i))) & 0xFF


def ref_sdr(mem, addr, reg):
    b = addr & 7
    for i in range(8 - b):
        mem[addr + i] = (reg >> (8 * i)) & 0xFF


STORE_VALUE = 0x1122334455667788
HARNESS_HEAD = r"""
#include <stdio.h>
#include <setjmp.h>
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
}
static void run(const char *tag, void (*fn)(R5900Ctx *))
{
    if (!setjmp(jb)) { fn(&c); printf("%s v0=%llx\n", tag, (unsigned long long)c.r[2].d[0]); }
    else printf("%s fatal=%s pc=%x\n", tag, fwhat, fpc);
}
"""
HARNESS = HARNESS_HEAD + r"""
int main(void)
{
    reset(); run("A", fn_00001000);
    reset(); c.r[25].d[0] = 0x1300; run("B", fn_00001200);
    for (int k = 0; k < 6; k++) { char t[8]; reset(); c.r[4].d[0] = k; snprintf(t, sizeof t, "C%d", k); run(t, fn_00001400); }
    reset(); c.r[25].d[0] = 0x1700; c.r[4].d[0] = 10; run("D", fn_00001600);
    void (*st[4])(R5900Ctx *) = {fn_00001800, fn_00001810, fn_00001820, fn_00001830};
    for (int f = 0; f < 4; f++)
        for (int off = 0; off < 8; off++) {
            reset();
            for (int i = 0; i < 16; i++) ram[0x3000 + i] = (uint8_t)(0xA5 + 0x3B * i);
            c.r[4].d[0] = 0x3000 + off; c.r[5].d[0] = 0x1122334455667788ull;
            st[f](&c);
            printf("E%d.%d", f, off);
            for (int i = 0; i < 16; i++) printf(" %02x", ram[0x3000 + i]);
            printf("\n");
        }
    reset(); run("F", fn_00001900);
    reset(); c.r[4].d[0] = 0; run("G0", fn_00001a00);
    reset(); c.r[4].d[0] = 1; run("G1", fn_00001a00);
    for (int k = 0; k < 2; k++) {
        reset(); c.r[4].d[0] = k; c.r[3].d[0] = 0x1122334455667788ull; fn_00001c00(&c);
        printf("H%d v1=%llx\n", k, (unsigned long long)c.r[3].d[0]);
    }
    return 0;
}
"""


def _wsl_path(p):
    return config.posix_path(p)


def expected():
    exp = {"A": "v0=1010", "B": "v0=1308", "D": "v0=f", "F": "fatal=break pc=1900", "G0": "v0=65", "G1": "v0=3",
           "H0": "v1=1122334455667788", "H1": "v1=5"}
    for k in range(6):
        exp["C%d" % k] = "v0=%x" % ((k + 1) + 100 * (k + 1)) if k < 4 else "v0=ffffffffffffffff"
    for f, ref in enumerate((ref_swl, ref_swr, ref_sdl, ref_sdr)):
        for off in range(8):
            mem = bytearray((0xA5 + 0x3B * i) & 0xFF for i in range(16))
            ref(mem, off, STORE_VALUE)
            exp["E%d.%d" % (f, off)] = " ".join("%02x" % b for b in mem)
    return exp


def _compile_run(out, img, src, header, harness):
    """Build the generated unit with the harness on the host (WSL gcc), run it: {tag: result}."""
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    data = img.read(BASE, SIZE)
    hdr = "static const unsigned char unit_image[%d] = {%s};\n" % (SIZE, ",".join(str(b) for b in data))
    open(os.path.join(out, "unit_image.h"), "w", newline="\n", encoding="ascii").write(hdr)
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(harness)
    script = os.path.join(out, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "set -e\ncd %s\ngcc -O1 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-function "
        "-I%s -I. unit_main.c unit_rc.c %s -o unit && ./unit\n" % (
            _wsl_path(out), _wsl_path(os.path.join(ROOT, "runtime")),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script), env=env, capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    return dict(line.split(" ", 1) for line in p.stdout.decode().splitlines() if line.strip())


@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3"])
def test_generated_control_flow_and_unaligned_stores_behave_like_the_ee(variant):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    assert TABLE not in fs.functions
    narrow = narrowing_plan(img, fs.functions) if variant in ("v2", "v3") else None
    src, header = emit_program(img, fs, roots=SEEDS, cache=variant != "v0", narrow=narrow, fpu_fast=variant == "v3")
    got = _compile_run(os.path.join(OUT, variant), img, src, header, HARNESS)
    exp = expected()
    bad = {k: (got.get(k), v) for k, v in exp.items() if got.get(k) != v}
    assert not bad, bad


HARNESS_I = HARNESS_HEAD + r"""
int main(void)
{
    for (int k = 0; k < 3; k++) { char t[8]; reset(); c.r[4].d[0] = k; snprintf(t, sizeof t, "I%d", k); run(t, fn_00001d00); }
    return 0;
}
"""


@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4"])
def test_jump_table_case_owned_by_another_function_is_a_tail_call_with_full_write_back(variant):
    img = image(PROGRAM_I)
    fs = discover_functions(img, CODE, extra_seeds=SEEDS_I)
    targets = {t for _jr, _tab, ts in fs.functions[0x1D00].jump_tables for t in ts}
    assert 0x1E00 in targets and 0x1E00 not in fs.functions[0x1D00].addrs     # the case really is another function
    opts = {}
    if variant != "v0":
        opts = {"cache": True, "fpu_fast": variant in ("v3", "v4"), "abi": variant == "v4"}
    if variant in ("v2", "v3", "v4"):
        opts["narrow"] = narrowing_plan(img, fs.functions, abi=variant == "v4", roots=set(SEEDS_I))
    if variant == "v4":
        opts["livein"] = argument_livein(img, fs.functions)
    src, header = emit_program(img, fs, roots=SEEDS_I, **opts)
    got = _compile_run(os.path.join(OUT, "jt_" + variant), img, src, header, HARNESS_I)
    assert got == {"I0": "v0=64", "I1": "v0=8", "I2": "v0=ffffffffffffffff"}
