"""Phase 4 runtime: overlay dispatch and the EE syscall table.

Two overlays occupy the same guest window (0x3000-0x3100), each translated as its own unit (ova_/ovb_) that calls
main code directly; main code reaches the window with JAL and JALR, and the dispatcher must run the code of the
overlay currently loaded (rc_overlay_current). SYSCALL looks up rt_syscall_table by the number in v1 (negative =
interrupt-context variant, same slot); an empty slot reaches rt_fatal. Checked for back-ends v0 and v4."""
import os
import subprocess

import pytest
from ps2rc import config  # noqa: E402

from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from r5900_enc import (A0, RA, SP, T9, V0, V1, NOP, ZERO, addiu, image, jal, jr, ld, r_type, sd)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_overlay")


def jalr(rs): return r_type(0x09, rs=rs, rd=RA)


SYSCALL = 0x0000000C


def frame_call(target, arg):
    """addiu sp,-16; sd ra; jal target; addiu a0,zero,arg; ld ra; jr ra; addiu sp,16"""
    return [addiu(SP, SP, -16), sd(RA, 0, SP), jal(target), addiu(A0, ZERO, arg), ld(RA, 0, SP), jr(RA),
            addiu(SP, SP, 16)]


MAIN = {
    0x1000: frame_call(0x3000, 0),                                    # JAL into the overlay window
    0x1100: [addiu(SP, SP, -16), sd(RA, 0, SP), jalr(T9), NOP, ld(RA, 0, SP), jr(RA), addiu(SP, SP, 16)],
    0x1200: [jr(RA), addiu(V0, A0, 1000)],                            # main helper called by the overlays
    0x1300: [addiu(V1, ZERO, 0x40), SYSCALL, jr(RA), NOP],
    0x1340: [addiu(V1, ZERO, -0x40), SYSCALL, jr(RA), NOP],
    0x1380: [addiu(V1, ZERO, 0x41), SYSCALL, jr(RA), NOP],
}
MAIN_SEEDS = sorted(MAIN)
OVERLAYS = {
    "ova": {0x3000: frame_call(0x1200, 1), 0x3040: [jr(RA), addiu(V0, ZERO, 11)]},
    "ovb": {0x3000: frame_call(0x1200, 2), 0x3040: [jr(RA), addiu(V0, ZERO, 22)]},
}

HARNESS = r"""
#include <stdio.h>
#include <setjmp.h>
#include "r5900_rt.h"
#include "main_rc.h"
#include "ova_rc.h"
#include "ovb_rc.h"
static jmp_buf jb;
static const char *fwhat;
static uint32_t fpc, fval;
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; fwhat = what; fpc = pc; fval = val; longjmp(jb, 1); }
static uint8_t ram[0x10000];
static R5900Ctx c;
static void sys_double(R5900Ctx *c) { c->r[2].d[0] = c->r[4].d[0] * 2; }
static void reset(void)
{
    memset(&c, 0, sizeof c);
    c.ram = ram; c.ram_mask = sizeof ram - 1; c.r[29].d[0] = 0x8000; c.r[31].d[0] = 0xDEAD0000;
}
static void run(const char *tag, void (*fn)(R5900Ctx *))
{
    if (!setjmp(jb)) { fn(&c); printf("%s v0=%lld\n", tag, (long long)c.r[2].d[0]); }
    else printf("%s fatal=%s pc=%x val=%x\n", tag, fwhat, fpc, fval);
}
int main(void)
{
    rc_overlay_lo = 0x3000; rc_overlay_hi = 0x3100;
    rc_overlay_lookup[1] = ova_rc_lookup;
    rc_overlay_lookup[2] = ovb_rc_lookup;
    rt_syscall_table[0x40] = sys_double;
    for (int ov = 0; ov <= 2; ov++) {
        char t[16];
        rc_overlay_current = ov;
        reset(); snprintf(t, sizeof t, "JAL%d", ov); run(t, fn_00001000);
        reset(); c.r[25].d[0] = 0x3040; snprintf(t, sizeof t, "JALR%d", ov); run(t, fn_00001100);
    }
    reset(); c.r[4].d[0] = 21; run("SYS", fn_00001300);
    reset(); c.r[4].d[0] = 21; run("SYSNEG", fn_00001340);
    reset(); c.r[4].d[0] = 21; run("SYSBAD", fn_00001380);
    return 0;
}
"""
EXPECTED = {"JAL0": "fatal=overlay pc=0 val=3000", "JALR0": "fatal=overlay pc=0 val=3040",
            "JAL1": "v0=1001", "JALR1": "v0=11", "JAL2": "v0=1002", "JALR2": "v0=22",
            "SYS": "v0=42", "SYSNEG": "v0=42", "SYSBAD": "fatal=syscall pc=1384 val=41"}


def _wsl_path(p):
    return config.posix_path(p)


def _translate(variant, out):
    opts = {} if variant == "v0" else {"cache": True, "fpu_fast": True, "abi": True}
    main_img = image(MAIN)
    main_fs = discover_functions(main_img, [(0x1000, 0x2000)], extra_seeds=MAIN_SEEDS)
    if variant != "v0":
        opts["narrow"] = narrowing_plan(main_img, main_fs.functions, abi=True, roots=set(MAIN_SEEDS))
        opts["livein"] = argument_livein(main_img, main_fs.functions)
    src, hdr = emit_program(main_img, main_fs, roots=MAIN_SEEDS, **opts)
    files = {"main_rc.c": src, "main_rc.h": hdr}
    external = {a: "fn_%08x" % a for a in main_fs.functions}
    for tag, prog in OVERLAYS.items():
        img = image(prog, base=0x3000, size=0x100)
        fs = discover_functions(img, [(0x3000, 0x3100)], extra_seeds=sorted(prog))
        o = dict(opts)
        if variant != "v0":
            o["narrow"] = narrowing_plan(img, fs.functions, abi=True, roots=set(prog))
            o["livein"] = {**argument_livein(main_img, main_fs.functions), **argument_livein(img, fs.functions)}
        s, h = emit_program(img, fs, roots=sorted(prog), prefix=tag + "_", dispatch="rc_dispatch",
                            external=external, lookup=True, **o)
        files[tag + "_rc.c"], files[tag + "_rc.h"] = s, h
    for name, text in files.items():
        open(os.path.join(out, name), "w", newline="\n", encoding="ascii").write(text)


@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v0", "v4"])
def test_overlay_dispatch_and_syscall_table(variant):
    out = os.path.join(OUT, variant)
    os.makedirs(out, exist_ok=True)
    _translate(variant, out)
    open(os.path.join(out, "main.c"), "w", newline="\n", encoding="ascii").write(HARNESS)
    script = os.path.join(out, "run.sh")
    rt = os.path.join(ROOT, "runtime")
    open(script, "w", newline="\n", encoding="ascii").write(
        "set -e\ncd %s\ngcc -O1 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-function "
        "-Wno-unused-but-set-variable -I%s -I. main.c main_rc.c ova_rc.c ovb_rc.c %s -o t && ./t\n"
        % (_wsl_path(out), _wsl_path(rt), _wsl_path(os.path.join(rt, "r5900_rt.c"))))
    p = subprocess.run(config.bash_argv(script),
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    got = dict(line.split(" ", 1) for line in p.stdout.decode().splitlines() if line.strip())
    bad = {k: (got.get(k), v) for k, v in EXPECTED.items() if got.get(k) != v}
    assert not bad, bad
