"""R5900 decoder: one 32-bit word -> Insn with mnemonic, structured operands and objdump-compatible text.

Encodings: the R5900 opcode maps (CPU, MMI, COP0, COP1) and the COP2 macro instructions, checked against GNU
binutils (the deviations are listed below and in tests/test_r5900_decode.py). Text format: GNU binutils 2.44 `objdump -m mips:5900 -M no-aliases` (the test
oracle), except that CVT.W.S keeps the R5900 name (objdump: trunc.w.s).

Operands are tuples:  ('r', n) GPR · ('f', n) FPR · ('vf', n, comps) VU float reg · ('vi', n) VU int reg ·
('c0', n) COP0 reg · ('c1', n) FPU control reg · ('acc', comps) · ('I',) ('Q',) ('R',) ·
('imm', value, 'd'|'x') · ('mem', base, offset) · ('target', addr) · ('vimem', reg, mode) with mode '', '++', '--'.
"""
from dataclasses import dataclass, field

GPR_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"]

COP0_NAMES = {0: "c0_index", 1: "c0_random", 2: "c0_entrylo0", 3: "c0_entrylo1", 4: "c0_context", 5: "c0_pagemask",
              6: "c0_wired", 8: "c0_badvaddr", 9: "c0_count", 10: "c0_entryhi", 11: "c0_compare", 12: "c0_sr",
              13: "c0_cause", 14: "c0_epc", 15: "c0_prid", 16: "c0_config", 23: "c0_badpaddr", 24: "c0_depc",
              25: "c0_perfcnt", 28: "c0_taglo", 29: "c0_taghi", 30: "c0_errorepc"}


@dataclass(slots=True)
class Insn:
    word: int
    pc: int
    valid: bool
    mnem: str = ".word"
    ops: tuple = ()

    @property
    def text(self):
        if not self.valid:
            return ".word\t0x%x" % self.word
        if not self.ops:
            return self.mnem
        return self.mnem + "\t" + ",".join(_fmt(o) for o in self.ops)


def _fmt(o):
    k = o[0]
    if k == "r":
        return GPR_NAMES[o[1]]
    if k == "f":
        return "$f%d" % o[1]
    if k == "vf":
        return "$vf%d%s" % (o[1], o[2])
    if k == "vi":
        return "$vi%d" % o[1]
    if k == "c0":
        return COP0_NAMES.get(o[1], "$%d" % o[1])
    if k == "c1":
        return {0: "c1_fir", 31: "c1_fcsr"}.get(o[1], "$%d" % o[1])
    if k == "acc":
        return "$ACC%s" % o[1]
    if k in ("I", "Q", "R"):
        return "$" + k
    if k == "imm":
        return ("%d" % o[1]) if o[2] == "d" else ("0x%x" % o[1])
    if k == "mem":
        return "%d(%s)" % (o[2], GPR_NAMES[o[1]])
    if k == "target":
        return "0x%x" % o[1]
    if k == "vimem":
        reg, mode = o[1], o[2]
        if mode == "++":
            return "($vi%d++)" % reg
        if mode == "--":
            return "(--$vi%d)" % reg
        return "($vi%d)" % reg
    raise ValueError(o)


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def _bad(w, pc):
    return Insn(w, pc, False)


def _ok(w, pc, mnem, *ops):
    return Insn(w, pc, True, mnem, ops)


def decode(w, pc=0):
    w &= 0xFFFFFFFF
    op = w >> 26
    h = _PRIMARY.get(op)
    if h is None:
        return _bad(w, pc)
    return h(w, pc)


# ---------------------------------------------------------------- field helpers
def RS(w): return (w >> 21) & 31
def RT(w): return (w >> 16) & 31
def RD(w): return (w >> 11) & 31
def SHAMT(w): return (w >> 6) & 31
def FN(w): return w & 63


def _btarget(w, pc):
    return ("target", (pc + 4 + (_s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF)


# ---------------------------------------------------------------- primary opcodes
def _j(name):
    def h(w, pc):
        return _ok(w, pc, name, ("target", ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)))
    return h


def _beq(name):
    def h(w, pc):
        return _ok(w, pc, name, ("r", RS(w)), ("r", RT(w)), _btarget(w, pc))
    return h


def _bz(name):  # blez/bgtz(+l): rt must be 0
    def h(w, pc):
        if RT(w):
            return _bad(w, pc)
        return _ok(w, pc, name, ("r", RS(w)), _btarget(w, pc))
    return h


def _immd(name):  # signed decimal immediate: rt, rs, imm
    def h(w, pc):
        return _ok(w, pc, name, ("r", RT(w)), ("r", RS(w)), ("imm", _s16(w & 0xFFFF), "d"))
    return h


def _immx(name):  # zero-extended hex immediate
    def h(w, pc):
        return _ok(w, pc, name, ("r", RT(w)), ("r", RS(w)), ("imm", w & 0xFFFF, "x"))
    return h


def _lui(w, pc):
    if RS(w):
        return _bad(w, pc)
    return _ok(w, pc, "lui", ("r", RT(w)), ("imm", w & 0xFFFF, "x"))


def _mem(name, kind="r"):
    def h(w, pc):
        return _ok(w, pc, name, (kind, RT(w)) if kind != "vf" else ("vf", RT(w), ""), ("mem", RS(w), _s16(w & 0xFFFF)))
    return h


def _cacheop(name):
    def h(w, pc):
        return _ok(w, pc, name, ("imm", RT(w), "x"), ("mem", RS(w), _s16(w & 0xFFFF)))
    return h


# ---------------------------------------------------------------- SPECIAL
_SHIFT_IMM = {0: "sll", 2: "srl", 3: "sra", 56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32", 62: "dsrl32", 63: "dsra32"}
_SHIFT_VAR = {4: "sllv", 6: "srlv", 7: "srav", 20: "dsllv", 22: "dsrlv", 23: "dsrav"}
_ALU3 = {10: "movz", 11: "movn", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor",
         39: "nor", 42: "slt", 43: "sltu", 44: "dadd", 45: "daddu", 46: "dsub", 47: "dsubu"}
_NEG = {34: "neg", 35: "negu", 46: "dneg", 47: "dnegu"}
_TRAP = {48: "tge", 49: "tgeu", 50: "tlt", 51: "tltu", 52: "teq", 54: "tne"}

# SPECIAL fields the instruction formats mark "0" (rs = bits 25..21, rt 20..16, rd 15..11, sa 10..6) and
# the EE ignores: an R5900 word with such a field nonzero executes as its base operation. PCSX2 (interpreter
# R5900OpcodeTables.cpp tbl_Special + R5900OpcodeImpl.cpp/Interpreter.cpp, recompiler x86/ix86-32/iR5900*.cpp) selects
# the operation by the function field alone and reads only the operand fields; Sony's libipu code executes its
# inline constant 0x00ff00ff = DSRA32 with rs = 7 as an instruction. GNU objdump
# rejects these words (.word). Function codes without an R5900 operation (1, 5, 14, 21, 28-31, 53, 55, 57, 61) stay
# invalid; SYNC keeps its stype check (0 = SYNC.L, 0x10 = SYNC.P), only its rs/rt/rd fields are ignored.
_RS, _RT, _RD, _SH = 31 << 21, 31 << 16, 31 << 11, 31 << 6
SPECIAL_IGNORED = {f: _RS for f in _SHIFT_IMM}
SPECIAL_IGNORED.update({f: _SH for f in _SHIFT_VAR})
SPECIAL_IGNORED.update({f: _SH for f in _ALU3})
SPECIAL_IGNORED.update({8: _RT | _RD | _SH, 9: _RT | _SH, 15: _RS | _RT | _RD, 16: _RS | _RT | _SH, 18: _RS | _RT | _SH,
                        17: _RT | _RD | _SH, 19: _RT | _RD | _SH, 24: _SH, 25: _SH, 26: _RD | _SH, 27: _RD | _SH,
                        40: _RS | _RT | _SH, 41: _RT | _RD | _SH})


def special_ignored_bits(w):
    """The nonzero must-be-zero bits of a SPECIAL word that the EE ignores (0 for any other word): such a word
    decodes as `decode(w & ~special_ignored_bits(w))` with its own word value."""
    w &= 0xFFFFFFFF
    return w & SPECIAL_IGNORED.get(FN(w), 0) if w >> 26 == 0 else 0


def _special(w, pc):
    f = FN(w)
    v = w & ~SPECIAL_IGNORED.get(f, 0)               # operand fields only (see SPECIAL_IGNORED)
    rs, rt, rd, sa = RS(v), RT(v), RD(v), SHAMT(v)
    if f in _SHIFT_IMM:
        if rs:
            return _bad(w, pc)
        return _ok(w, pc, _SHIFT_IMM[f], ("r", rd), ("r", rt), ("imm", sa, "x"))
    if f in _SHIFT_VAR:
        if sa:
            return _bad(w, pc)
        return _ok(w, pc, _SHIFT_VAR[f], ("r", rd), ("r", rt), ("r", rs))
    if f in _ALU3:
        if sa:
            return _bad(w, pc)
        if rs == 0 and f in _NEG:  # binutils prints sub* with rs=zero as neg* even with -M no-aliases
            return _ok(w, pc, _NEG[f], ("r", rd), ("r", rt))
        return _ok(w, pc, _ALU3[f], ("r", rd), ("r", rs), ("r", rt))
    if f == 8:  # jr
        if rt or rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "jr", ("r", rs))
    if f == 9:  # jalr rd, rs
        if rt or sa:
            return _bad(w, pc)
        if rd == 31:
            return _ok(w, pc, "jalr", ("r", rs))
        return _ok(w, pc, "jalr", ("r", rd), ("r", rs))
    if f == 12:
        code = (w >> 6) & 0xFFFFF
        return _ok(w, pc, "syscall", *((("imm", code, "x"),) if code else ()))
    if f == 13:
        c1, c2 = (w >> 16) & 0x3FF, (w >> 6) & 0x3FF
        if c1 == 0 and c2 == 0:
            return _ok(w, pc, "break")
        if c2 == 0:
            return _ok(w, pc, "break", ("imm", c1, "x"))
        return _ok(w, pc, "break", ("imm", c1, "x"), ("imm", c2, "x"))
    if f == 15:
        if rs or rt or rd:
            return _bad(w, pc)
        if sa == 0:
            return _ok(w, pc, "sync")
        if sa == 0x10:
            return _ok(w, pc, "sync.p")
        return _bad(w, pc)
    if f in (16, 18):  # mfhi/mflo rd
        if rs or rt or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mfhi" if f == 16 else "mflo", ("r", rd))
    if f in (17, 19):  # mthi/mtlo rs
        if rt or rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mthi" if f == 17 else "mtlo", ("r", rs))
    if f in (24, 25):  # mult/multu [rd,]rs,rt
        if sa:
            return _bad(w, pc)
        name = "mult" if f == 24 else "multu"
        if rd == 0:
            return _ok(w, pc, name, ("r", rs), ("r", rt))
        return _ok(w, pc, name, ("r", rd), ("r", rs), ("r", rt))
    if f in (26, 27):  # div/divu zero,rs,rt
        if rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "div" if f == 26 else "divu", ("r", 0), ("r", rs), ("r", rt))
    if f == 40:  # mfsa rd
        if rs or rt or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mfsa", ("r", rd))
    if f == 41:  # mtsa rs
        if rt or rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mtsa", ("r", rs))
    if f in _TRAP:
        code = (w >> 6) & 0x3FF
        return _ok(w, pc, _TRAP[f], ("r", rs), ("r", rt), *((("imm", code, "x"),) if code else ()))
    return _bad(w, pc)


# ---------------------------------------------------------------- REGIMM
_REGIMM_B = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 16: "bltzal", 17: "bgezal", 18: "bltzall", 19: "bgezall"}
_REGIMM_T = {8: "tgei", 9: "tgeiu", 10: "tlti", 11: "tltiu", 12: "teqi", 14: "tnei"}


def _regimm(w, pc):
    rt = RT(w)
    if rt in _REGIMM_B:
        return _ok(w, pc, _REGIMM_B[rt], ("r", RS(w)), _btarget(w, pc))
    if rt in _REGIMM_T:
        return _ok(w, pc, _REGIMM_T[rt], ("r", RS(w)), ("imm", _s16(w & 0xFFFF), "d"))
    if rt in (24, 25):
        return _ok(w, pc, "mtsab" if rt == 24 else "mtsah", ("r", RS(w)), ("imm", _s16(w & 0xFFFF), "d"))
    return _bad(w, pc)


# ---------------------------------------------------------------- MMI
_MMI0 = {0: "paddw", 1: "psubw", 2: "pcgtw", 3: "pmaxw", 4: "paddh", 5: "psubh", 6: "pcgth", 7: "pmaxh", 8: "paddb",
         9: "psubb", 10: "pcgtb", 16: "paddsw", 17: "psubsw", 18: "pextlw", 19: "ppacw", 20: "paddsh", 21: "psubsh",
         22: "pextlh", 23: "ppach", 24: "paddsb", 25: "psubsb", 26: "pextlb", 27: "ppacb", 30: "pext5", 31: "ppac5"}
_MMI1 = {1: "pabsw", 2: "pceqw", 3: "pminw", 4: "padsbh", 5: "pabsh", 6: "pceqh", 7: "pminh", 10: "pceqb",
         16: "padduw", 17: "psubuw", 18: "pextuw", 20: "padduh", 21: "psubuh", 22: "pextuh", 24: "paddub",
         25: "psubub", 26: "pextub", 27: "qfsrv"}
_MMI2 = {0: "pmaddw", 2: "psllvw", 3: "psrlvw", 4: "pmsubw", 8: "pmfhi", 9: "pmflo", 10: "pinth", 12: "pmultw",
         13: "pdivw", 14: "pcpyld", 16: "pmaddh", 17: "phmadh", 18: "pand", 19: "pxor", 20: "pmsubh", 21: "phmsbh",
         26: "pexeh", 27: "prevh", 28: "pmulth", 29: "pdivbw", 30: "pexew", 31: "prot3w"}
_MMI3 = {0: "pmadduw", 3: "psravw", 8: "pmthi", 9: "pmtlo", 10: "pinteh", 12: "pmultuw", 13: "pdivuw", 14: "pcpyud",
         18: "por", 19: "pnor", 26: "pexch", 27: "pcpyh", 30: "pexcw"}
_MMI_RD_RT = {"pext5", "ppac5", "pabsw", "pabsh", "pexeh", "prevh", "pexew", "prot3w", "pexch", "pcpyh", "pexcw"}
_MMI_RS_RT = {"pdivw", "pdivuw", "pdivbw"}
_MMI_SHIFT_VAR = {"psllvw", "psrlvw", "psravw"}
_PMFHL = {0: "pmfhl.lw", 1: "pmfhl.uw", 2: "pmfhl.slw", 3: "pmfhl.lh", 4: "pmfhl.sh"}
_MMI_SHIFT_IMM = {52: "psllh", 54: "psrlh", 55: "psrah", 60: "psllw", 62: "psrlw", 63: "psraw"}
_MMI_MULDIV = {0: "madd", 1: "maddu", 24: "mult1", 25: "multu1", 32: "madd1", 33: "maddu1"}


def _mmi(w, pc):
    f, rs, rt, rd, sa = FN(w), RS(w), RT(w), RD(w), SHAMT(w)
    if f in (8, 40, 9, 41):
        table = {8: _MMI0, 40: _MMI1, 9: _MMI2, 41: _MMI3}[f]
        name = table.get(sa)
        if name is None:
            return _bad(w, pc)
        if name in _MMI_RD_RT:
            if rs:
                return _bad(w, pc)
            return _ok(w, pc, name, ("r", rd), ("r", rt))
        if name in _MMI_RS_RT:
            if rd:
                return _bad(w, pc)
            return _ok(w, pc, name, ("r", rs), ("r", rt))
        if name in ("pmfhi", "pmflo"):
            if rs or rt:
                return _bad(w, pc)
            return _ok(w, pc, name, ("r", rd))
        if name in ("pmthi", "pmtlo"):
            if rt or rd:
                return _bad(w, pc)
            return _ok(w, pc, name, ("r", rs))
        if name in _MMI_SHIFT_VAR:
            return _ok(w, pc, name, ("r", rd), ("r", rt), ("r", rs))
        return _ok(w, pc, name, ("r", rd), ("r", rs), ("r", rt))
    if f in _MMI_MULDIV:
        name = _MMI_MULDIV[f]
        if sa:
            return _bad(w, pc)
        if rd == 0:
            return _ok(w, pc, name, ("r", rs), ("r", rt))
        return _ok(w, pc, name, ("r", rd), ("r", rs), ("r", rt))
    if f in (26, 27):  # div1/divu1 zero,rs,rt
        if rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "div1" if f == 26 else "divu1", ("r", 0), ("r", rs), ("r", rt))
    if f == 4:  # plzcw rd, rs
        if rt or sa:
            return _bad(w, pc)
        return _ok(w, pc, "plzcw", ("r", rd), ("r", rs))
    if f in (16, 18):
        if rs or rt or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mfhi1" if f == 16 else "mflo1", ("r", rd))
    if f in (17, 19):
        if rt or rd or sa:
            return _bad(w, pc)
        return _ok(w, pc, "mthi1" if f == 17 else "mtlo1", ("r", rs))
    if f in _MMI_SHIFT_IMM:
        if rs:
            return _bad(w, pc)
        return _ok(w, pc, _MMI_SHIFT_IMM[f], ("r", rd), ("r", rt), ("imm", sa, "x"))
    if f == 48:  # pmfhl.fmt rd
        name = _PMFHL.get(sa)
        if name is None or rs or rt:
            return _bad(w, pc)
        return _ok(w, pc, name, ("r", rd))
    if f == 49:  # pmthl.lw rs
        if sa or rt or rd:
            return _bad(w, pc)
        return _ok(w, pc, "pmthl.lw", ("r", rs))
    return _bad(w, pc)


# ---------------------------------------------------------------- COP0
_C0 = {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 24: "eret", 56: "ei", 57: "di"}
_DEBUG_MF = {0: "mfbpc", 2: "mfiab", 3: "mfiabm", 4: "mfdab", 5: "mfdabm", 6: "mfdvb", 7: "mfdvbm"}
_DEBUG_MT = {0: "mtbpc", 2: "mtiab", 3: "mtiabm", 4: "mtdab", 5: "mtdabm", 6: "mtdvb", 7: "mtdvbm"}


def _cop0(w, pc):
    rs, rt, rd, low = RS(w), RT(w), RD(w), w & 0x7FF
    if rs in (0, 4):
        mf = rs == 0
        if rd == 24:  # debug registers
            name = (_DEBUG_MF if mf else _DEBUG_MT).get(low)
            return _ok(w, pc, name, ("r", rt)) if name else _bad(w, pc)
        if rd == 25:  # performance counters
            reg = (low >> 1) & 31
            if low & ~0x3F:
                return _bad(w, pc)
            if low & 1:
                return _ok(w, pc, "mfpc" if mf else "mtpc", ("r", rt), ("imm", reg, "d"))
            return _ok(w, pc, "mfps" if mf else "mtps", ("r", rt), ("imm", reg, "d"))
        if low:
            return _bad(w, pc)
        return _ok(w, pc, "mfc0" if mf else "mtc0", ("r", rt), ("c0", rd))
    if rs == 8:
        name = {0: "bc0f", 1: "bc0t", 2: "bc0fl", 3: "bc0tl"}.get(rt)
        return _ok(w, pc, name, _btarget(w, pc)) if name else _bad(w, pc)
    if rs == 16:
        name = _C0.get(FN(w))
        if name is None or (w >> 6) & 0x7FFFF:
            return _bad(w, pc)
        return _ok(w, pc, name)
    return _bad(w, pc)


# ---------------------------------------------------------------- COP1 (FPU)
_FPU3 = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 22: "rsqrt.s", 28: "madd.s", 29: "msub.s", 40: "max.s",
         41: "min.s"}
_FPU_ACC = {24: "adda.s", 25: "suba.s", 26: "mula.s", 30: "madda.s", 31: "msuba.s"}
_FPU_CMP = {48: "c.f.s", 50: "c.eq.s", 52: "c.lt.s", 54: "c.le.s"}


def _cop1(w, pc):
    rs, rt, fs, fd, f = RS(w), RT(w), RD(w), SHAMT(w), FN(w)
    if rs in (0, 4):
        if w & 0x7FF:
            return _bad(w, pc)
        return _ok(w, pc, "mfc1" if rs == 0 else "mtc1", ("r", rt), ("f", fs))
    if rs in (2, 6):
        if w & 0x7FF:
            return _bad(w, pc)
        return _ok(w, pc, "cfc1" if rs == 2 else "ctc1", ("r", rt), ("c1", fs))
    if rs == 8:
        name = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(rt)
        return _ok(w, pc, name, _btarget(w, pc)) if name else _bad(w, pc)
    if rs == 16:
        if f in _FPU3:
            return _ok(w, pc, _FPU3[f], ("f", fd), ("f", fs), ("f", rt))
        if f in _FPU_ACC:
            if fd:
                return _bad(w, pc)
            return _ok(w, pc, _FPU_ACC[f], ("f", fs), ("f", rt))
        if f in _FPU_CMP:
            if fd:
                return _bad(w, pc)
            return _ok(w, pc, _FPU_CMP[f], ("f", fs), ("f", rt))
        if f == 4:  # sqrt.s fd, ft  (the fs field is reserved; like PCSX2 we ignore it)
            return _ok(w, pc, "sqrt.s", ("f", fd), ("f", rt))
        if f in (5, 6, 7, 36):  # abs/mov/neg/cvt.w fd, fs
            if rt:
                return _bad(w, pc)
            return _ok(w, pc, {5: "abs.s", 6: "mov.s", 7: "neg.s", 36: "cvt.w.s"}[f], ("f", fd), ("f", fs))
        return _bad(w, pc)
    if rs == 20 and f == 32:
        if rt:
            return _bad(w, pc)
        return _ok(w, pc, "cvt.s.w", ("f", fd), ("f", fs))
    return _bad(w, pc)


# ---------------------------------------------------------------- COP2 (VU0 macro mode)
def _dest(w):
    return "".join(c for c, b in zip("xyzw", (24, 23, 22, 21)) if w >> b & 1)


_BC = "xyzw"
_VU_SP1 = {}
for _i, _n in enumerate(["vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini", "vmul"]):
    for _b in range(4):
        _VU_SP1[_i * 4 + _b] = (_n + _BC[_b], "bc")
_VU_SP1.update({28: ("vmulq", "q"), 29: ("vmaxi", "i"), 30: ("vmuli", "i"), 31: ("vminii", "i"), 32: ("vaddq", "q"),
                33: ("vmaddq", "q"), 34: ("vaddi", "i"), 35: ("vmaddi", "i"), 36: ("vsubq", "q"),
                37: ("vmsubq", "q"), 38: ("vsubi", "i"), 39: ("vmsubi", "i"), 40: ("vadd", "3"), 41: ("vmadd", "3"),
                42: ("vmul", "3"), 43: ("vmax", "3"), 44: ("vsub", "3"), 45: ("vmsub", "3"), 46: ("vopmsub", "3"),
                47: ("vmini", "3"), 48: ("viadd", "i3"), 49: ("visub", "i3"), 50: ("viaddi", "iimm"),
                52: ("viand", "i3"), 53: ("vior", "i3"), 56: ("vcallms", "call"), 57: ("vcallmsr", "callr")})
_VU_SP2 = {}
for _i, _n in enumerate(["vadda", "vsuba", "vmadda", "vmsuba"]):
    for _b in range(4):
        _VU_SP2[_i * 4 + _b] = (_n + _BC[_b], "accbc")
for _b in range(4):
    _VU_SP2[24 + _b] = ("vmula" + _BC[_b], "accbc")
_VU_SP2.update({16: ("vitof0", "2"), 17: ("vitof4", "2"), 18: ("vitof12", "2"), 19: ("vitof15", "2"),
                20: ("vftoi0", "2"), 21: ("vftoi4", "2"), 22: ("vftoi12", "2"), 23: ("vftoi15", "2"),
                28: ("vmulaq", "accq"), 29: ("vabs", "2"), 30: ("vmulai", "acci"), 31: ("vclipw", "clip"),
                32: ("vaddaq", "accq"), 33: ("vmaddaq", "accq"), 34: ("vaddai", "acci"), 35: ("vmaddai", "acci"),
                36: ("vsubaq", "accq"), 37: ("vmsubaq", "accq"), 38: ("vsubai", "acci"), 39: ("vmsubai", "acci"),
                40: ("vadda", "acc3"), 41: ("vmadda", "acc3"), 42: ("vmula", "acc3"), 44: ("vsuba", "acc3"),
                45: ("vmsuba", "acc3"), 46: ("vopmula", "acc3"), 47: ("vnop", "none"), 48: ("vmove", "2"),
                49: ("vmr32", "2"), 52: ("vlqi", "lqi"), 53: ("vsqi", "sqi"), 54: ("vlqd", "lqd"),
                55: ("vsqd", "sqd"), 56: ("vdiv", "div"), 57: ("vsqrt", "sqrt"), 58: ("vrsqrt", "div"),
                59: ("vwaitq", "none"), 60: ("vmtir", "mtir"), 61: ("vmfir", "mfir"), 62: ("vilwr", "ilwr"),
                63: ("viswr", "iswr"), 64: ("vrnext", "rnext"), 65: ("vrget", "rnext"), 66: ("vrinit", "rinit"),
                67: ("vrxor", "rinit")})


def _cop2(w, pc):
    rs, rt = RS(w), RT(w)
    if rs in (1, 2, 5, 6):  # qmfc2 / cfc2 / qmtc2 / ctc2 (bit 0 = interlock)
        if w & 0x7FE:
            return _bad(w, pc)
        name = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2"}[rs]
        if w & 1:
            name += ".i"
        reg = ("vf", RD(w), "") if rs in (1, 5) else ("vi", RD(w))
        return _ok(w, pc, name, ("r", rt), reg)
    if rs == 8:
        name = {0: "bc2f", 1: "bc2t", 2: "bc2fl", 3: "bc2tl"}.get(rt)
        return _ok(w, pc, name, _btarget(w, pc)) if name else _bad(w, pc)
    if rs < 16:
        return _bad(w, pc)
    f = FN(w)
    if f >= 60:
        ent = _VU_SP2.get(((w >> 6) & 31) << 2 | (w & 3))
    else:
        ent = _VU_SP1.get(f)
    if ent is None:
        return _bad(w, pc)
    name, form = ent
    d = _dest(w)
    ft, fs, fd = RT(w), RD(w), SHAMT(w)
    dest_bits = (w >> 21) & 0xF
    # reserved fields that must be zero / fixed (the instruction formats)
    if form in ("i3", "iimm", "call") and dest_bits:
        return _bad(w, pc)
    if form in ("q", "i", "accq", "acci") and ft:
        return _bad(w, pc)
    if (name in ("vopmsub", "vopmula") or form == "clip") and dest_bits != 0xE:
        return _bad(w, pc)
    if form == "callr" and (dest_bits or ft or fs != 27 or fd):
        return _bad(w, pc)
    if form == "none" and (dest_bits or ft or fs):
        return _bad(w, pc)
    if form == "sqrt" and (fs or (w >> 21) & 3):
        return _bad(w, pc)
    if form == "rinit" and (ft or (w >> 23) & 3):
        return _bad(w, pc)
    if form == "rnext" and fs:
        return _bad(w, pc)
    if form == "mtir" and (w >> 23) & 3:
        return _bad(w, pc)
    if form in ("ilwr", "iswr") and dest_bits not in (1, 2, 4, 8):  # exactly one field (VU UM ILWR/ISWR)
        return _bad(w, pc)
    m = name + "." + d
    if form == "bc":
        return _ok(w, pc, m, ("vf", fd, d), ("vf", fs, d), ("vf", ft, _BC[w & 3]))
    if form == "3":
        return _ok(w, pc, m, ("vf", fd, d), ("vf", fs, d), ("vf", ft, d))
    if form in ("q", "i"):
        return _ok(w, pc, m, ("vf", fd, d), ("vf", fs, d), ("Q",) if form == "q" else ("I",))
    if form == "accbc":
        return _ok(w, pc, m, ("acc", d), ("vf", fs, d), ("vf", ft, _BC[w & 3]))
    if form == "acc3":
        return _ok(w, pc, m, ("acc", d), ("vf", fs, d), ("vf", ft, d))
    if form in ("accq", "acci"):
        return _ok(w, pc, m, ("acc", d), ("vf", fs, d), ("Q",) if form == "accq" else ("I",))
    if form == "2":
        return _ok(w, pc, m, ("vf", ft, d), ("vf", fs, d))
    if form == "clip":
        return _ok(w, pc, m, ("vf", fs, d), ("vf", ft, "w"))
    if form == "none":
        return _ok(w, pc, name)
    if form == "i3":
        return _ok(w, pc, name, ("vi", fd), ("vi", fs), ("vi", ft))
    if form == "iimm":
        imm = fd - 32 if fd & 16 else fd
        return _ok(w, pc, name, ("vi", ft), ("vi", fs), ("imm", imm, "d"))
    if form == "call":
        return _ok(w, pc, name, ("imm", ((w >> 6) & 0x7FFF) << 3, "x"))
    if form == "callr":
        return _ok(w, pc, name, ("vi", 27))
    if form in ("lqi", "lqd"):
        return _ok(w, pc, m, ("vf", ft, d), ("vimem", fs, "++" if form == "lqi" else "--"))
    if form in ("sqi", "sqd"):
        return _ok(w, pc, m, ("vf", fs, d), ("vimem", ft, "++" if form == "sqi" else "--"))
    if form == "div":
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        return _ok(w, pc, name, ("Q",), ("vf", fs, _BC[fsf]), ("vf", ft, _BC[ftf]))
    if form == "sqrt":
        ftf = (w >> 23) & 3
        return _ok(w, pc, name, ("Q",), ("vf", ft, _BC[ftf]))
    if form == "mtir":
        return _ok(w, pc, name, ("vi", ft), ("vf", fs, _BC[(w >> 21) & 3]))
    if form == "mfir":
        return _ok(w, pc, m, ("vf", ft, d), ("vi", fs))
    if form in ("ilwr", "iswr"):
        return _ok(w, pc, m, ("vi", ft), ("vimem", fs, ""))
    if form == "rnext":
        return _ok(w, pc, m, ("vf", ft, d), ("R",))
    if form == "rinit":
        return _ok(w, pc, name, ("R",), ("vf", fs, _BC[(w >> 21) & 3]))
    return _bad(w, pc)


_PRIMARY = {
    0: _special, 1: _regimm, 2: _j("j"), 3: _j("jal"), 4: _beq("beq"), 5: _beq("bne"), 6: _bz("blez"),
    7: _bz("bgtz"), 8: _immd("addi"), 9: _immd("addiu"), 10: _immd("slti"), 11: _immd("sltiu"), 12: _immx("andi"),
    13: _immx("ori"), 14: _immx("xori"), 15: _lui, 16: _cop0, 17: _cop1, 18: _cop2, 20: _beq("beql"),
    21: _beq("bnel"), 22: _bz("blezl"), 23: _bz("bgtzl"), 24: _immd("daddi"), 25: _immd("daddiu"),
    26: _mem("ldl"), 27: _mem("ldr"), 28: _mmi, 30: _mem("lq"), 31: _mem("sq"), 32: _mem("lb"), 33: _mem("lh"),
    34: _mem("lwl"), 35: _mem("lw"), 36: _mem("lbu"), 37: _mem("lhu"), 38: _mem("lwr"), 39: _mem("lwu"),
    40: _mem("sb"), 41: _mem("sh"), 42: _mem("swl"), 43: _mem("sw"), 44: _mem("sdl"), 45: _mem("sdr"),
    46: _mem("swr"), 47: _cacheop("cache"), 49: _mem("lwc1", "f"), 51: _cacheop("pref"), 54: _mem("lqc2", "vf"),
    55: _mem("ld"), 57: _mem("swc1", "f"), 62: _mem("sqc2", "vf"), 63: _mem("sd"),
}
