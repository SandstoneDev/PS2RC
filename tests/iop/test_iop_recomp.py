"""IOP static recompiler (ps2rc.iop): decoder, IRX loader, analysis, and generated C against a reference interpreter.
Synthetic modules only."""
import os
import struct

import pytest

from ps2rc.iop import irx as irxmod
from ps2rc.iop.analysis import analyse
from ps2rc.iop.gen_c import generate
from ps2rc.iop.r3000 import decode
from iop_testlib import Asm, build_irx, import_table, Ref, rnd, wsl_run, wsl_path, gcc_line, M32


# ---------------------------------------------------------------------------------------------- decoder
def test_decoder_round_trip_of_the_assembler():
    src = """
        addiu a0, a1, -5
        lui t0, 0x1234
        ori t0, t0, 0xabcd
        sll v0, v1, 3
        srav v0, v1, a2
        mult a0, a1
        divu t1, t2
        mflo s0
        lw ra, 20(sp)
        swl t3, -3(s7)
        sltiu v0, a0, -1
    """
    words, _ = Asm()(src).assemble()
    got = [decode(w, 4 * k).text for k, w in enumerate(words)]
    assert got == ["addiu\ta0,a1,-5", "lui\tt0,0x1234", "ori\tt0,t0,0xabcd", "sll\tv0,v1,3", "srav\tv0,v1,a2",
                   "mult\ta0,a1", "divu\tt1,t2", "mflo\ts0", "lw\tra,20(sp)", "swl\tt3,-3(s7)",
                   "sltiu\tv0,a0,-1"]
    assert decode(0x00000000).text == "nop"
    assert not decode(0xFC000000).valid                      # opcode 63: not MIPS I
    assert not decode(0x4A000000).valid                      # COP2: not on the IOP


def test_branch_targets_and_dests():
    a = Asm()("""
    top:
        beq a0, zero, top
        nop
        bgezal a1, top
        nop
        jal top
        nop
    """)
    w, rel = a.assemble()
    b = decode(w[0], 0)
    assert b.target == 0 and b.is_branch and b.has_delay_slot
    assert decode(w[2], 8).dest() == 31
    assert decode(w[4], 16).target == 0
    assert rel == [(16, irxmod.R_MIPS_26)]


# ---------------------------------------------------------------------------------------------- IRX loader
def _module(base_text, data=b"", bss=0x40, entry_label="start", imports=()):
    pre = []
    for lib, nums in imports:
        pre += import_table(lib, nums)
    a = Asm(origin=4 * len(pre))(base_text)
    extra = {}
    k = 0
    for lib, nums in imports:                               # stub labels: <lib>_<num>
        k += 5
        for n in nums:
            extra["%s_%d" % (lib, n)] = 4 * k
            k += 2
        k += 2
    words, rel = a.assemble(extra)
    tw = pre + words
    return build_irx(tw, data=data, bss=bss, relocs=rel, entry=a.labels[entry_label], gp=0x7FF0), a.labels


def test_irx_relocations_follow_the_base():
    blob, L = _module("""
    start:
        la a0, buf
        jal fn
        nop
        jr ra
        nop
    fn:
        jr ra
        addiu v0, a0, 1
    buf:
        .word fn
    """)
    for base in (0, 0x1000, 0x23450):
        x = irxmod.load(blob, base)
        assert x.entry == L["start"] and x.name == "testmod"
        hi, lo = x.word(base + 0), x.word(base + 4)
        v = ((hi & 0xFFFF) << 16) + (((lo & 0xFFFF) ^ 0x8000) - 0x8000)
        assert v == base + L["buf"]
        assert decode(x.word(base + 8), base + 8).target == base + L["fn"]
        assert x.word(base + L["buf"]) == base + L["fn"]


def test_imports_exports_and_analysis():
    blob, L = _module("""
    start:
        addiu sp, sp, -16
        sw ra, 12(sp)
        la a0, cb
        jal thsemap_8
        nop
        jal helper
        nop
        lw ra, 12(sp)
        j thsemap_6           # tail call into another module
        addiu sp, sp, 16
    helper:
        bne a0, zero, out
        nop
        b helper
        nop
    out:
        jr ra
        nop
    cb:
        jr ra
        addu v0, zero, zero
    """, imports=[("thsemap", [6, 8])])
    x = irxmod.load(blob, 0x1000)
    assert [(i.lib, [f.num for f in i.fns]) for i in x.imports] == [("thsemap", [6, 8])]
    p = analyse(x)
    assert not p.issues
    assert p.entries == sorted(0x1000 + L[k] for k in ("start", "helper", "cb"))
    assert p.callbacks == [0x1000 + L["cb"]]
    f = p.functions[0x1000 + L["start"]]
    assert {s - 0x1000 for s in f.imports} == {L["thsemap_6"], L["thsemap_8"]}
    assert f.calls == {0x1000 + L["helper"]}


def test_unsupported_code_is_reported_not_guessed():
    blob, _ = _module("""
    start:
        lw t0, 0(a0)
        nop
        jr t0                 # no jump table: an indirect tail call, resolved at run time
        nop
    """)
    p = analyse(irxmod.load(blob, 0))
    assert not p.errors() and any("indirect tail call" in t for _, _, t in p.issues)
    blob, _ = _module("""
    start:
        lw t0, 0(a0)
        addu v0, t0, t0       # reads the loaded register in the load delay slot
        jr ra
        nop
    """)
    assert any("load-delay" in t for _, _, t in analyse(irxmod.load(blob, 0)).errors())
    with pytest.raises(ValueError):
        generate(irxmod.load(blob, 0), "m")


SWITCH = """
start:
    la t0, f_switch
    jr ra
    nop
f_switch:
    andi t0, a0, 3
    sll t0, t0, 2
    la t1, jtab
    addu t1, t1, t0
    lw t1, 0(t1)
    nop
    jr t1
    addiu v1, zero, 5
case0:
    b sw_end
    addiu v0, zero, 10
case1:
    b sw_end
    addiu v0, a1, 20
case2:
    xor v0, a1, a2
    b sw_end
    nop
case3:
    addiu v0, a2, 40
sw_end:
    jr ra
    addu v1, v1, v0
"""


def _switch_module():
    """f_switch: a switch through a jump table in .data (R_MIPS_32 entries)."""
    pre = Asm()(SWITCH)
    words, rel = pre.assemble({"jtab": 0})
    n = 4 * len(words)
    a = Asm()(SWITCH)
    words, rel = a.assemble({"jtab": n})
    L = a.labels
    data = b"".join(struct.pack("<I", L[k]) for k in ("case0", "case1", "case2", "case3"))
    rel = rel + [(n + 4 * k, irxmod.R_MIPS_32) for k in range(4)]
    return build_irx(words, data=data, bss=0x40, relocs=rel, entry=L["start"]), L


def test_jump_table_recovered():
    blob, L = _switch_module()
    x = irxmod.load(blob, 0x1000)
    p = analyse(x)
    assert not p.issues, p.issues
    f = p.functions[0x1000 + L["f_switch"]]
    assert f.jtab == {0x1000 + L["case%d" % k] for k in range(4)}
    assert 0x1000 + L["case0"] not in p.entries        # case labels are not functions


@pytest.mark.wsl
def test_jump_table_runs(tmp_path):
    blob, L = _switch_module()
    _differential(tmp_path, blob, 0x1000, [L["f_switch"]], 5)


# ---------------------------------------------------------------------------------------------- random programs
POOL = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 30]
SRC = POOL + [23, 29]
DATA = 0x100000                    # s7: data window (256 bytes)
R3 = ["addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "sllv", "srlv", "srav", "add", "sub"]
IM = ["addiu", "slti", "sltiu", "andi", "ori", "xori", "addi"]
RN = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1",
      "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def _random_function(r, name, n):
    body = []
    pending_load = None

    def reg(pool):
        return RN[r.choice(pool)]

    while len(body) < n:
        k = r.random()
        if k < 0.30:
            ins = "%s %s, %s, %s" % (r.choice(R3), reg(POOL), reg(SRC), reg(SRC))
        elif k < 0.45:
            op = r.choice(IM)
            v = r.randrange(-32768, 32768) if op in ("addiu", "slti", "sltiu", "addi") else r.randrange(65536)
            ins = "%s %s, %s, %d" % (op, reg(POOL), reg(SRC), v)
        elif k < 0.52:
            ins = "%s %s, %s, %d" % (r.choice(["sll", "srl", "sra"]), reg(POOL), reg(SRC), r.randrange(32))
        elif k < 0.55:
            ins = "lui %s, %d" % (reg(POOL), r.randrange(65536))
        elif k < 0.62:
            ins = "%s %s, %s" % (r.choice(["mult", "multu", "div", "divu"]), reg(SRC), reg(SRC))
        elif k < 0.67:
            ins = r.choice(["mfhi %s", "mflo %s", "mthi %s", "mtlo %s"]) % reg(POOL)
        elif k < 0.85:
            op = r.choice(["lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "sb", "sh", "sw", "swl", "swr"])
            align = {"lh": 2, "lhu": 2, "sh": 2, "lw": 4, "sw": 4}.get(op, 1)
            off = r.randrange(0, 248) // align * align
            ins = "%s %s, %d(s7)" % (op, reg(POOL if op[0] == "l" else SRC), off)
        else:
            body.append(("BR", r.choice(["beq", "bne", "blez", "bgtz", "bltz", "bgez"]), reg(SRC), reg(SRC),
                         r.randrange(1, 7)))
            continue
        body.append(ins)
    # resolve: branches forward, delay slots filled, no load-delay use, no branch in a delay slot
    out = ["%s:" % name, "addiu sp, sp, -16", "sw ra, 12(sp)"]
    lab = 0
    pending = {}
    i = 0
    flat = []
    for b in body:
        if isinstance(b, tuple):
            flat.append(b)
        else:
            flat.append(b)
    idx = 0
    for b in flat:
        if idx in pending:
            for l in pending.pop(idx):
                out.append(l + ":")
        if isinstance(b, tuple):
            _, op, a, c, skip = b
            l = "%s_l%d" % (name, lab)
            lab += 1
            tgt = idx + 1 + skip
            pending.setdefault(min(tgt, len(flat)), []).append(l)
            if op in ("beq", "bne"):
                out.append("%s %s, %s, %s" % (op, a, c, l))
            else:
                out.append("%s %s, %s" % (op, a, l))
            out.append("addu %s, %s, %s" % (RN[r.choice(POOL)], RN[r.choice(SRC)], RN[r.choice(SRC)]))
        else:
            out.append(b)
            if b.split()[0] in ("lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr"):
                out.append("nop")                        # keep the load delay slot free of a use
        idx += 1
    for l in pending.get(len(flat), []):
        out.append(l + ":")
    out += ["jal helper", "addu a0, v0, s0", "lw ra, 12(sp)", "nop", "jr ra", "addiu sp, sp, 16"]
    return out


def _random_module(seed, nfun=24, ninsn=48):
    r = rnd(seed)
    names = ["f%d" % k for k in range(nfun)]
    # the functions are handed out by address (callbacks): the analysis finds them through the lui/addiu pairs
    lines = ["start:"] + ["la t0, %s" % n for n in names] + ["jr ra", "nop",
                                                              "helper:", "xor v1, a0, a1", "jr ra", "addu v0, v1, a2"]
    for n in names:
        lines += _random_function(r, n, ninsn)
    a = Asm()("\n".join(lines))
    words, rel = a.assemble()
    return build_irx(words, bss=0x100, relocs=rel, entry=a.labels["start"]), a.labels, names


DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "iop_rt.h"
#include "iop_rt_ucontext.h"
#include "cases.h"
extern const IopModule tmod;
int main(void)
{
    IopRtConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    iop_rt_coro_ucontext(&cfg.coro);
    IopRt *rt = iop_rt_create(&cfg);
    if (!rt || iop_rt_load(rt, &tmod, 0, 0, 8) < 0) { printf("load failed\n"); return 1; }
    uint8_t *mem = iop_rt_mem(rt);
    for (int k = 0; k < NCASES; k++) {
        memcpy(mem + 0x100000, case_data[k], 256);
        IopCpu c;
        memset(&c, 0, sizeof c);
        c.mem = mem;
        c.rt = rt;
        memcpy(c.r, case_regs[k], sizeof c.r);
        c.hi = case_regs[k][32]; c.lo = case_regs[k][33];
        c.r[29] = 0x80000; c.r[23] = 0x100000; c.r[31] = 0xDEADu << 16;
        IopFn f = tmod.fn_at(case_fn[k]);
        if (!f) { printf("C%d nofn\n", k); continue; }
        f(&c);
        printf("C%d", k);
        for (int i = 0; i < 32; i++) printf(" %08x", i == 31 ? 0 : c.r[i]);
        printf(" %08x %08x ", c.hi, c.lo);
        for (int i = 0; i < 256; i++) printf("%02x", mem[0x100000 + i]);
        printf("\n");
    }
    return 0;
}
"""


def _differential(tmp_path, blob, base, fns, seed):
    """Run every function of `fns` (labels) on random registers / data through the generated C and the reference."""
    x = irxmod.load(blob, base)
    src, prog = generate(x, "tmod")
    assert all(sv == "info" for sv, _, _ in prog.issues), prog.issues
    r = rnd(seed * 7919)
    cases = []
    for k in range(3 * len(fns)):
        fn = base + fns[k % len(fns)]
        regs = [0] + [r.getrandbits(32) if r.random() < 0.8 else r.choice([0, 1, M32, 0x80000000, 7, 0x7FFFFFFF])
                      for _ in range(31)] + [r.getrandbits(32), r.getrandbits(32)]
        data = bytes(r.getrandbits(8) for _ in range(256))
        cases.append((fn, regs, data))
    exp = {}
    for k, (fn, regs, data) in enumerate(cases):
        mem = bytearray(0x200000 + 64)
        mem[base:base + len(x.image)] = x.image
        mem[DATA:DATA + 256] = data
        ref = Ref(mem, lambda a: struct.unpack_from("<I", mem, a)[0])
        ref.r = list(regs[:32])
        ref.hi, ref.lo = regs[32], regs[33]
        ref.r[29], ref.r[23] = 0x80000, DATA
        ref.call(fn)
        exp["C%d" % k] = " ".join(["%08x" % (0 if i == 31 else ref.r[i]) for i in range(32)] +
                                  ["%08x" % ref.hi, "%08x" % ref.lo, mem[DATA:DATA + 256].hex()])
    d = tmp_path
    NL = chr(10)
    (d / "tmod.c").write_text(src, newline=NL)
    (d / "driver.c").write_text(DRIVER, newline=NL)
    with open(d / "cases.h", "w", newline=NL) as f:
        f.write("#define NCASES %d" % len(cases) + NL)
        f.write("static const uint32_t case_fn[] = {%s};" % ",".join("0x%xu" % c[0] for c in cases) + NL)
        f.write("static const uint32_t case_regs[][34] = {%s};" % ",".join(
            "{%s}" % ",".join("0x%xu" % v for v in c[1]) for c in cases) + NL)
        f.write("static const uint8_t case_data[][256] = {%s};" % ",".join(
            "{%s}" % ",".join(str(b) for b in c[2]) for c in cases) + NL)
    rc, out, err = wsl_run(gcc_line(["driver.c", "tmod.c"], "t", "-DIOP_CHECK") + " && ./t", str(d))
    assert rc == 0, err[-3000:]
    got = dict(l.split(" ", 1) for l in out.splitlines() if l.startswith("C"))
    bad = [k for k in exp if got.get(k) != exp[k]]
    assert not bad, (bad[:3], [(exp[k], got.get(k)) for k in bad[:1]])


@pytest.mark.wsl
@pytest.mark.parametrize("seed,base", [(1, 0x1000), (2, 0x2340), (3, 0x1000)])
def test_generated_c_matches_the_reference_interpreter(tmp_path, seed, base):
    blob, L, names = _random_module(seed)
    _differential(tmp_path, blob, base, [L[n] for n in names], seed)


CONTROL = """
start:
    la t0, f_link
    la t0, f_jalr
    la t0, f_slot
    la t0, f_tail
    la t0, f_ctail
    la t0, f_fall
    la t0, f_next
    jr ra
    nop
callee:
    addu v0, a0, a1
    jr ra
    sll v1, v0, 1
f_link:
    addiu sp, sp, -16
    sw ra, 12(sp)
    bgezal a2, callee
    addiu a0, a0, 3
    bltzal a2, callee
    addiu a1, a1, 5
    xor s0, v0, v1
    lw ra, 12(sp)
    nop
    jr ra
    addiu sp, sp, 16
f_jalr:
    addiu sp, sp, -16
    sw ra, 12(sp)
    la t9, callee
    jalr ra, t9
    addu a0, a0, a3
    lw ra, 12(sp)
    nop
    jr ra
    addiu sp, sp, 16
f_slot:
    andi t0, a0, 3
    bne t0, zero, f_slot_x
    nop
    b f_slot_y
f_slot_x:                   # the delay slot of the b above, and a branch target
    addiu v0, a1, 5
    addiu v0, v0, 1
f_slot_y:
    jr ra
    xor v1, v0, a0
f_tail:
    addiu a0, a0, 100
    j callee
    addiu a1, a1, 200
f_ctail:
    slt t1, a0, a1
    bne t1, zero, callee
    subu a2, a0, a1
    jr ra
    xor v0, a0, a2
f_fall:
    addiu a0, a0, 9
    addu a1, a1, a0
f_next:
    or v0, a0, a1
    jr ra
    nop
"""


@pytest.mark.wsl
def test_links_indirect_calls_delay_slot_targets_tail_calls(tmp_path):
    blob, L = _module(CONTROL, bss=0x100)
    _differential(tmp_path, blob, 0x1000, [L[k] for k in ("f_link", "f_jalr", "f_slot", "f_tail", "f_ctail",
                                                         "f_fall")], 11)


