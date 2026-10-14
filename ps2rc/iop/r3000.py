"""MIPS I (R3000A, the PS2 I/O processor) instruction decoder.

One 32-bit word -> Insn(op, rd, rs, rt, imm, sa, target) with a GNU-objdump-like text form. Encodings from the public
MIPS I architecture (MIPS R3000 manuals). Only what the IOP can execute: no 64-bit operations, no branch-likely, no
FPU. COP0 transfers (mfc0/mtc0/rfe) are decoded; COP2 and other coprocessors are reported as invalid.
"""
from dataclasses import dataclass

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

# instruction classes used by the analyses and the C back-end
LOADS = {"lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr"}
STORES = {"sb", "sh", "sw", "swl", "swr"}
BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
JUMPS = {"j", "jal", "jr", "jalr"}


@dataclass(slots=True)
class Insn:
    word: int
    pc: int
    op: str = ".word"          # mnemonic, ".word" when invalid
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0               # sign- or zero-extended as the instruction uses it
    target: int = 0            # branch / jump target address

    @property
    def valid(self):
        return self.op != ".word"

    @property
    def is_branch(self):
        return self.op in BRANCHES

    @property
    def is_jump(self):
        return self.op in JUMPS

    @property
    def has_delay_slot(self):
        return self.op in BRANCHES or self.op in JUMPS

    @property
    def is_load(self):
        return self.op in LOADS

    @property
    def is_store(self):
        return self.op in STORES

    def dest(self):
        """GPR written by the instruction (0 = none)."""
        o = self.op
        if o in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui") or o in LOADS:
            return self.rt
        if o in ("sll", "srl", "sra", "sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or", "xor", "nor",
                 "slt", "sltu", "mfhi", "mflo", "jalr"):
            return self.rd
        if o in ("jal", "bltzal", "bgezal"):
            return 31
        if o == "mfc0":
            return self.rt
        return 0

    def sources(self):
        """GPRs read by the instruction."""
        o = self.op
        if o in ("sll", "srl", "sra"):
            return (self.rt,)
        if o in ("sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                 "mult", "multu", "div", "divu", "beq", "bne"):
            return (self.rs, self.rt)
        if o in ("jr", "jalr", "mthi", "mtlo", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal",
                 "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori") or o in ("lb", "lbu", "lh", "lhu", "lw"):
            return (self.rs,)
        if o in ("lwl", "lwr"):
            return (self.rs, self.rt)
        if o in STORES:
            return (self.rs, self.rt)
        if o == "mtc0":
            return (self.rt,)
        return ()

    @property
    def text(self):
        return fmt(self)


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


_SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 12: "syscall",
            13: "break", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult", 25: "multu", 26: "div",
            27: "divu", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
            42: "slt", 43: "sltu"}
_IMM_S = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu"}
_IMM_U = {12: "andi", 13: "ori", 14: "xori"}
_MEM = {32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl",
        43: "sw", 46: "swr"}


def decode(w, pc=0):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    i = Insn(w, pc)
    if op == 0:
        m = _SPECIAL.get(fn)
        if m is None:
            return i
        i.op, i.rs, i.rt, i.rd, i.sa = m, rs, rt, rd, sa
        if m == "syscall" or m == "break":
            i.imm = (w >> 6) & 0xFFFFF
        return i
    if op == 1:
        m = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt)
        if m is None:
            return i
        i.op, i.rs, i.imm = m, rs, _s16(w & 0xFFFF)
        i.target = (pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        return i
    if op in (2, 3):
        i.op = "j" if op == 2 else "jal"
        i.target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return i
    if op in (4, 5, 6, 7):
        i.op = {4: "beq", 5: "bne", 6: "blez", 7: "bgtz"}[op]
        if op in (6, 7) and rt != 0:
            i.op = ".word"
            return i
        i.rs, i.rt, i.imm = rs, rt, _s16(w & 0xFFFF)
        i.target = (pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        return i
    if op in _IMM_S:
        i.op, i.rs, i.rt, i.imm = _IMM_S[op], rs, rt, _s16(w & 0xFFFF)
        return i
    if op in _IMM_U:
        i.op, i.rs, i.rt, i.imm = _IMM_U[op], rs, rt, w & 0xFFFF
        return i
    if op == 15:
        i.op, i.rt, i.imm = "lui", rt, w & 0xFFFF
        return i
    if op == 16:
        if rs == 0:
            i.op, i.rt, i.rd = "mfc0", rt, rd
        elif rs == 4:
            i.op, i.rt, i.rd = "mtc0", rt, rd
        elif rs == 16 and fn == 16:
            i.op = "rfe"
        return i
    if op in _MEM:
        i.op, i.rs, i.rt, i.imm = _MEM[op], rs, rt, _s16(w & 0xFFFF)
        return i
    return i


def fmt(i):
    o = i.op
    if o == ".word":
        return ".word\t0x%08x" % i.word
    g = GPR
    if o in ("sll", "srl", "sra"):
        if i.word == 0:
            return "nop"
        return "%s\t%s,%s,%d" % (o, g[i.rd], g[i.rt], i.sa)
    if o in ("sllv", "srlv", "srav"):
        return "%s\t%s,%s,%s" % (o, g[i.rd], g[i.rt], g[i.rs])
    if o == "jr":
        return "jr\t%s" % g[i.rs]
    if o == "jalr":
        return "jalr\t%s,%s" % (g[i.rd], g[i.rs])
    if o in ("syscall", "break", "rfe"):
        return o
    if o in ("mfhi", "mflo"):
        return "%s\t%s" % (o, g[i.rd])
    if o in ("mthi", "mtlo"):
        return "%s\t%s" % (o, g[i.rs])
    if o in ("mult", "multu", "div", "divu"):
        return "%s\t%s,%s" % (o, g[i.rs], g[i.rt])
    if o in ("add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"):
        return "%s\t%s,%s,%s" % (o, g[i.rd], g[i.rs], g[i.rt])
    if o in ("bltz", "bgez", "bltzal", "bgezal", "blez", "bgtz"):
        return "%s\t%s,0x%x" % (o, g[i.rs], i.target)
    if o in ("beq", "bne"):
        return "%s\t%s,%s,0x%x" % (o, g[i.rs], g[i.rt], i.target)
    if o in ("j", "jal"):
        return "%s\t0x%x" % (o, i.target)
    if o in ("addi", "addiu", "slti", "sltiu"):
        return "%s\t%s,%s,%d" % (o, g[i.rt], g[i.rs], i.imm)
    if o in ("andi", "ori", "xori"):
        return "%s\t%s,%s,0x%x" % (o, g[i.rt], g[i.rs], i.imm)
    if o == "lui":
        return "lui\t%s,0x%x" % (g[i.rt], i.imm)
    if o in ("mfc0", "mtc0"):
        return "%s\t%s,$%d" % (o, g[i.rt], i.rd)
    if o in LOADS or o in STORES:
        return "%s\t%s,%d(%s)" % (o, g[i.rt], i.imm, g[i.rs])
    return o
