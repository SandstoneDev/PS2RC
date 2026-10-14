"""Synthetic material for the IOP recompiler tests: a small MIPS I assembler, an IRX writer and a reference R3000
interpreter. Everything is generated here; no game or SDK data."""
import os
import random
import struct
import subprocess

from ps2rc.iop.r3000 import decode

REG = {n: i for i, n in enumerate(["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
                                    "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0",
                                    "k1", "gp", "sp", "fp", "ra"])}
REG["s8"] = 30

SPECIAL = {"sll": 0, "srl": 2, "sra": 3, "sllv": 4, "srlv": 6, "srav": 7, "jr": 8, "jalr": 9, "syscall": 12,
           "break": 13, "mfhi": 16, "mthi": 17, "mflo": 18, "mtlo": 19, "mult": 24, "multu": 25, "div": 26,
           "divu": 27, "add": 32, "addu": 33, "sub": 34, "subu": 35, "and": 36, "or": 37, "xor": 38, "nor": 39,
           "slt": 42, "sltu": 43}
IMM = {"addi": 8, "addiu": 9, "slti": 10, "sltiu": 11, "andi": 12, "ori": 13, "xori": 14, "lui": 15}
MEM = {"lb": 32, "lh": 33, "lwl": 34, "lw": 35, "lbu": 36, "lhu": 37, "lwr": 38, "sb": 40, "sh": 41, "swl": 42,
       "sw": 43, "swr": 46}
BR = {"beq": 4, "bne": 5, "blez": 6, "bgtz": 7}
REGIMM = {"bltz": 0, "bgez": 1, "bltzal": 16, "bgezal": 17}

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def _r(s):
    s = s.strip().lstrip("$")
    return REG[s] if s in REG else int(s)


def _i(s):
    return int(s.strip(), 0)


class Asm:
    """Two-pass assembler. Addresses are module-relative (the IRX image starts at 0). `relocs` collects the
    relocations an IRX needs (la/jal/j/.word label)."""

    def __init__(self, origin=0):
        self.origin = origin
        self.lines = []

    def __call__(self, text):
        for ln in text.strip().splitlines():
            ln = ln.split("#")[0].strip()
            if ln:
                self.lines.append(ln)
        return self

    def assemble(self, extra_labels=None):
        labels = dict(extra_labels or {})
        pc = self.origin
        for ln in self.lines:                        # pass 1: sizes
            if ln.endswith(":"):
                labels[ln[:-1]] = pc
                continue
            op = ln.split()[0]
            pc += 8 if op in ("la", "li") else 4
        self.labels = labels
        words, relocs = [], []
        pc = self.origin
        for ln in self.lines:
            if ln.endswith(":"):
                continue
            w, rl = self._one(ln, pc, labels)
            for k, t in rl:
                relocs.append((pc + 4 * k, t))
            words.extend(w)
            pc += 4 * len(w)
        return words, relocs

    def _one(self, ln, pc, L):
        parts = ln.split(None, 1)
        op = parts[0]
        a = [x.strip() for x in parts[1].split(",")] if len(parts) > 1 else []
        tgt = lambda s: L[s] if s in L else _i(s)
        if op == "nop":
            return [0], []
        if op == ".word":
            if a[0] in L:
                return [L[a[0]]], [(0, R_MIPS_32)]
            return [_i(a[0]) & 0xFFFFFFFF], []
        if op == "li":
            v = _i(a[1]) & 0xFFFFFFFF
            rt = _r(a[0])
            return [(15 << 26) | (rt << 16) | (v >> 16), (13 << 26) | (rt << 21) | (rt << 16) | (v & 0xFFFF)], []
        if op == "la":
            v = tgt(a[1])
            rt = _r(a[0])
            hi = ((v + 0x8000) >> 16) & 0xFFFF
            return [(15 << 26) | (rt << 16) | hi, (9 << 26) | (rt << 21) | (rt << 16) | (v & 0xFFFF)], \
                [(0, R_MIPS_HI16), (1, R_MIPS_LO16)]
        if op == "b":
            return self._one("beq zero, zero, " + a[0], pc, L)
        if op in SPECIAL:
            fn = SPECIAL[op]
            if op in ("sll", "srl", "sra"):
                return [(_r(a[1]) << 16) | (_r(a[0]) << 11) | ((_i(a[2]) & 31) << 6) | fn], []
            if op in ("sllv", "srlv", "srav"):
                return [(_r(a[2]) << 21) | (_r(a[1]) << 16) | (_r(a[0]) << 11) | fn], []
            if op == "jr" or op in ("mthi", "mtlo"):
                return [(_r(a[0]) << 21) | fn], []
            if op == "jalr":
                rd, rs = (_r(a[0]), _r(a[1])) if len(a) == 2 else (31, _r(a[0]))
                return [(rs << 21) | (rd << 11) | fn], []
            if op in ("mfhi", "mflo"):
                return [(_r(a[0]) << 11) | fn], []
            if op in ("mult", "multu", "div", "divu"):
                return [(_r(a[0]) << 21) | (_r(a[1]) << 16) | fn], []
            if op in ("syscall", "break"):
                return [fn], []
            return [(_r(a[1]) << 21) | (_r(a[2]) << 16) | (_r(a[0]) << 11) | fn], []
        if op in IMM:
            if op == "lui":
                return [(15 << 26) | (_r(a[0]) << 16) | (_i(a[1]) & 0xFFFF)], []
            return [(IMM[op] << 26) | (_r(a[1]) << 21) | (_r(a[0]) << 16) | (_i(a[2]) & 0xFFFF)], []
        if op in MEM:
            off, base = a[1].rstrip(")").split("(")
            return [(MEM[op] << 26) | (_r(base) << 21) | (_r(a[0]) << 16) | (_i(off or "0") & 0xFFFF)], []
        if op in BR:
            if op in ("beq", "bne"):
                rs, rt, t = _r(a[0]), _r(a[1]), tgt(a[2])
            else:
                rs, rt, t = _r(a[0]), 0, tgt(a[1])
            return [(BR[op] << 26) | (rs << 21) | (rt << 16) | (((t - pc - 4) >> 2) & 0xFFFF)], []
        if op in REGIMM:
            t = tgt(a[1])
            return [(1 << 26) | (_r(a[0]) << 21) | (REGIMM[op] << 16) | (((t - pc - 4) >> 2) & 0xFFFF)], []
        if op in ("j", "jal"):
            t = tgt(a[0])
            return [((2 if op == "j" else 3) << 26) | ((t >> 2) & 0x3FFFFFF)], [(0, R_MIPS_26)]
        raise ValueError("cannot assemble %r" % ln)


def import_table(lib, nums, version=0x101):
    """Words of an import stub table (magic, 0, version, name[8], stubs, terminator)."""
    name = lib.encode().ljust(8, b"\0")
    w = [0x41E0 << 16, 0, version] + list(struct.unpack("<II", name))
    for n in nums:
        w += [(31 << 21) | 8, (9 << 26) | n]
    return w + [0, 0]


def build_irx(text_words, data=b"", bss=0, relocs=(), entry=0, gp=0, name="testmod", version=0x101):
    """An IRX (ELF type 0xFF80) with .text/.data/.bss, REL relocations and an .iopmod section."""
    text = b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in text_words)
    tsz = len(text)
    data = bytes(data)
    while len(data) % 4:
        data += b"\0"
    dsz = len(data)
    shstr = b"\0.shstrtab\0.text\0.rel.text\0.data\0.bss\0.iopmod\0"
    off = lambda s: shstr.index(s)
    iopmod = struct.pack("<IIIIIIH", 0xFFFFFFFF, entry, gp, tsz, dsz, bss, version) + name.encode() + b"\0"
    while len(iopmod) % 4:
        iopmod += b"\0"
    rel = b"".join(struct.pack("<II", o, t) for o, t in relocs)
    ehsize, phsize = 52, 32
    o_iopmod = ehsize + 2 * phsize
    o_text = (o_iopmod + len(iopmod) + 15) & ~15
    o_data = o_text + tsz
    o_rel = o_data + dsz
    o_shstr = o_rel + len(rel)
    o_sh = (o_shstr + len(shstr) + 3) & ~3
    secs = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (off(b".shstrtab"), 3, 0, 0, o_shstr, len(shstr), 0, 0, 1, 0),
        (off(b".text"), 1, 6, 0, o_text, tsz, 0, 0, 16, 0),
        (off(b".rel.text"), 9, 0, 0, o_rel, len(rel), 0, 2, 4, 8),
        (off(b".data"), 1, 3, tsz, o_data, dsz, 0, 0, 16, 0),
        (off(b".bss"), 8, 3, tsz + dsz, o_data + dsz, bss, 0, 0, 16, 0),
        (off(b".iopmod"), 0x70000080, 0, 0, o_iopmod, len(iopmod), 0, 0, 4, 0),
    ]
    eh = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 0xFF80, 8, 1, entry, ehsize, o_sh, 0,
                                                             ehsize, phsize, 2, 40, len(secs), 1)
    ph = struct.pack("<IIIIIIII", 0x70000080, o_iopmod, 0, 0, len(iopmod), 0, 4, 4)
    ph += struct.pack("<IIIIIIII", 1, o_text, 0, 0, tsz + dsz, tsz + dsz + bss, 7, 16)
    out = bytearray(eh + ph + iopmod)
    out += b"\0" * (o_text - len(out))
    out += text + data + rel + shstr
    out += b"\0" * (o_sh - len(out))
    for s in secs:
        out += struct.pack("<IIIIIIIIII", *s)
    return bytes(out)


# ---------------------------------------------------------------------------------------------- reference interpreter
M32 = 0xFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


class Ref:
    """Straightforward R3000 interpreter (one instruction at a time, delay slots, no load delay)."""

    def __init__(self, mem, words_at):
        self.mem = mem                      # bytearray, address = index
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.fetch = words_at

    def lw(self, a):
        return struct.unpack_from("<I", self.mem, a)[0]

    def sw(self, a, v):
        struct.pack_into("<I", self.mem, a, v & M32)

    def call(self, entry, ret=0xDEAD << 16, limit=200000):
        r = self.r
        r[31] = ret
        pc, npc = entry, entry + 4
        steps = 0
        while pc != ret:
            steps += 1
            if steps > limit:
                raise RuntimeError("runaway")
            i = decode(self.fetch(pc), pc)
            nnpc = npc + 4
            o = i.op
            rs, rt = r[i.rs], r[i.rt]
            d = None
            if o == "sll": d = (i.rd, (rt << i.sa) & M32)
            elif o == "srl": d = (i.rd, rt >> i.sa)
            elif o == "sra": d = (i.rd, (s32(rt) >> i.sa) & M32)
            elif o == "sllv": d = (i.rd, (rt << (rs & 31)) & M32)
            elif o == "srlv": d = (i.rd, rt >> (rs & 31))
            elif o == "srav": d = (i.rd, (s32(rt) >> (rs & 31)) & M32)
            elif o in ("addu", "add"): d = (i.rd, (rs + rt) & M32)
            elif o in ("subu", "sub"): d = (i.rd, (rs - rt) & M32)
            elif o == "and": d = (i.rd, rs & rt)
            elif o == "or": d = (i.rd, rs | rt)
            elif o == "xor": d = (i.rd, rs ^ rt)
            elif o == "nor": d = (i.rd, ~(rs | rt) & M32)
            elif o == "slt": d = (i.rd, int(s32(rs) < s32(rt)))
            elif o == "sltu": d = (i.rd, int(rs < rt))
            elif o in ("addiu", "addi"): d = (i.rt, (rs + i.imm) & M32)
            elif o == "slti": d = (i.rt, int(s32(rs) < i.imm))
            elif o == "sltiu": d = (i.rt, int(rs < (i.imm & M32)))
            elif o == "andi": d = (i.rt, rs & i.imm)
            elif o == "ori": d = (i.rt, rs | i.imm)
            elif o == "xori": d = (i.rt, rs ^ i.imm)
            elif o == "lui": d = (i.rt, (i.imm << 16) & M32)
            elif o == "mult":
                p = s32(rs) * s32(rt)
                self.lo, self.hi = p & M32, (p >> 32) & M32
            elif o == "multu":
                p = rs * rt
                self.lo, self.hi = p & M32, (p >> 32) & M32
            elif o == "div":
                a, b = s32(rs), s32(rt)
                if b == 0:
                    self.lo, self.hi = (M32 if a >= 0 else 1), a & M32
                elif a == -0x80000000 and b == -1:
                    self.lo, self.hi = 0x80000000, 0
                else:
                    q = abs(a) // abs(b)
                    q = q if (a >= 0) == (b >= 0) else -q
                    self.lo, self.hi = q & M32, (a - q * b) & M32
            elif o == "divu":
                if rt == 0:
                    self.lo, self.hi = M32, rs
                else:
                    self.lo, self.hi = rs // rt, rs % rt
            elif o == "mfhi": d = (i.rd, self.hi)
            elif o == "mflo": d = (i.rd, self.lo)
            elif o == "mthi": self.hi = rs
            elif o == "mtlo": self.lo = rs
            elif o in ("lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "sb", "sh", "sw", "swl", "swr"):
                a = (rs + i.imm) & M32
                if o == "lb": d = (i.rt, s32(self.mem[a] | (0xFFFFFF00 if self.mem[a] & 0x80 else 0)) & M32)
                elif o == "lbu": d = (i.rt, self.mem[a])
                elif o == "lh":
                    v = struct.unpack_from("<H", self.mem, a)[0]
                    d = (i.rt, (v | (0xFFFF0000 if v & 0x8000 else 0)) & M32)
                elif o == "lhu": d = (i.rt, struct.unpack_from("<H", self.mem, a)[0])
                elif o == "lw": d = (i.rt, self.lw(a))
                elif o in ("lwl", "lwr"):
                    w = self.lw(a & ~3)
                    k = a & 3
                    if o == "lwl":
                        sh = (3 - k) * 8
                        d = (i.rt, ((rt & ((1 << sh) - 1)) | (w << sh)) & M32)
                    else:
                        sh = k * 8
                        d = (i.rt, ((rt & ~(M32 >> sh)) | (w >> sh)) & M32)
                elif o == "sb": self.mem[a] = rt & 0xFF
                elif o == "sh": struct.pack_into("<H", self.mem, a, rt & 0xFFFF)
                elif o == "sw": self.sw(a, rt)
                elif o in ("swl", "swr"):
                    w = self.lw(a & ~3)
                    k = a & 3
                    if o == "swl":
                        sh = (3 - k) * 8
                        w = (w & ~(M32 >> sh)) | (rt >> sh)
                    else:
                        sh = k * 8
                        w = (w & ((1 << sh) - 1)) | (rt << sh)
                    self.sw(a & ~3, w)
            elif o in ("beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"):
                c = {"beq": rs == rt, "bne": rs != rt, "blez": s32(rs) <= 0, "bgtz": s32(rs) > 0,
                     "bltz": s32(rs) < 0, "bgez": s32(rs) >= 0, "bltzal": s32(rs) < 0, "bgezal": s32(rs) >= 0}[o]
                if o in ("bltzal", "bgezal"):
                    r[31] = (pc + 8) & M32
                if c:
                    nnpc = i.target
            elif o == "j": nnpc = i.target
            elif o == "jal":
                r[31] = (pc + 8) & M32
                nnpc = i.target
            elif o == "jr": nnpc = rs
            elif o == "jalr":
                if i.rd:
                    r[i.rd] = (pc + 8) & M32
                nnpc = rs
            else:
                raise RuntimeError("ref: %s" % i.text)
            if d and d[0]:
                r[d[0]] = d[1] & M32
            pc, npc = npc, nnpc
        return steps


# ---------------------------------------------------------------------------------------------- WSL helpers
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RT_DIR = os.path.join(ROOT, "ps2rc", "iop", "rt")


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def wsl_run(script_text, workdir):
    script = os.path.join(workdir, "run.sh")
    open(script, "w", newline="\n").write("set -e\ncd %s\n%s\n" % (wsl_path(workdir), script_text))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(["wsl.exe", "-d", "Debian", "--", "bash", wsl_path(script)], env=env, capture_output=True)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def gcc_line(sources, out, extra=""):
    rt = wsl_path(RT_DIR)
    return ("gcc -O2 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable %s -I%s "
            "-I. -o %s %s %s/iop_rt.c %s/iop_rt_libs.c %s/iop_rt_ucontext.c" % (extra, rt, out, " ".join(sources), rt,
                                                                               rt, rt))


def rnd(seed):
    return random.Random(seed)
