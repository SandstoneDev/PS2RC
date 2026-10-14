"""VU micro-mode instruction set for the microcode translator (ps2rc.vu.micro_gen).

A microinstruction is 64 bits: the lower word at the lower address, the upper word above it. Upper bit 31 (I) makes
the lower word a float immediate for the I register (LOI); E (bit 30) ends the program after the next instruction.
Field layout and opcode numbers: the public VU instruction-set documentation (as used by open-source PS2 emulators).

decode(lower, upper, index) -> Micro with `up` and `lo` (Op objects) carrying the operands, the registers they read /
write (VF lanes as (reg, mask) with mask bits x=8 y=4 z=2 w=1), the pipeline they use and their flag behaviour.
"""
from dataclasses import dataclass, field

XYZW = "xyzw"

# upper: opcode 0..59 (bc = opcode & 3 for the broadcast forms)
_UP_BC = {0: "ADD", 1: "SUB", 2: "MADD", 3: "MSUB", 4: "MAX", 5: "MINI", 6: "MUL"}
_UP = {28: "MULq", 29: "MAXi", 30: "MULi", 31: "MINIi", 32: "ADDq", 33: "MADDq", 34: "ADDi", 35: "MADDi",
       36: "SUBq", 37: "MSUBq", 38: "SUBi", 39: "MSUBi", 40: "ADD", 41: "MADD", 42: "MUL", 43: "MAX", 44: "SUB",
       45: "MSUB", 46: "OPMSUB", 47: "MINI"}
# upper special (opcode 60..63): index = bits 10..6 << 2 | bits 1..0
_UP2_BC = {0: "ADDA", 1: "SUBA", 2: "MADDA", 3: "MSUBA", 6: "MULA"}
_UP2 = {16: "ITOF0", 17: "ITOF4", 18: "ITOF12", 19: "ITOF15", 20: "FTOI0", 21: "FTOI4", 22: "FTOI12", 23: "FTOI15",
        28: "MULAq", 29: "ABS", 30: "MULAi", 31: "CLIP", 32: "ADDAq", 33: "MADDAq", 34: "ADDAi", 35: "MADDAi",
        36: "SUBAq", 37: "MSUBAq", 38: "SUBAi", 39: "MSUBAi", 40: "ADDA", 41: "MADDA", 42: "MULA", 44: "SUBA",
        45: "MSUBA", 46: "OPMULA", 47: "NOP"}
# lower, bit 31 clear: opcode = bits 31..25
_LO = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ", 0x11: "FCSET",
       0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ",
       0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR", 0x28: "IBEQ",
       0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
# lower, bits 31..25 = 0x40: function bits 5..0, special (60..63) index = bits 10..6 << 2 | bits 1..0
_LO_OP = {48: "IADD", 49: "ISUB", 50: "IADDI", 52: "IAND", 53: "IOR"}
_LO_SP = {48: "MOVE", 49: "MR32", 52: "LQI", 53: "SQI", 54: "LQD", 55: "SQD", 56: "DIV", 57: "SQRT", 58: "RSQRT",
          59: "WAITQ", 60: "MTIR", 61: "MFIR", 62: "ILWR", 63: "ISWR", 64: "RNEXT", 65: "RGET", 66: "RINIT",
          67: "RXOR", 100: "MFP", 104: "XTOP", 105: "XITOP", 108: "XGKICK", 112: "ESADD", 113: "ERSADD",
          114: "ELENG", 115: "ERLENG", 116: "EATANxy", 117: "EATANxz", 118: "ESUM", 120: "ESQRT", 121: "ERSQRT",
          122: "ERCPR", 123: "WAITP", 124: "ESIN", 125: "EATAN", 126: "EEXP"}

FMAC_BASE = {"ADD", "SUB", "MUL", "MADD", "MSUB"}          # set the MAC flag
BRANCHES = {"B", "BAL", "JR", "JALR", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"}


@dataclass
class Op:
    name: str = "NOP"
    dest: int = 0                # field mask x=8 y=4 z=2 w=1
    fs: int = 0
    ft: int = 0
    fd: int = 0
    bc: int = -1                 # broadcast lane of ft (0..3) or -1
    src2: str = "vf"             # second operand: "vf" (ft lanes / bc), "i", "q"
    acc_dst: bool = False        # result to ACC
    base: str = ""               # ADD SUB MUL MADD MSUB OPMULA OPMSUB ITOF FTOI ABS CLIP MAX MINI NOP
    shift: int = 0               # ITOF/FTOI fraction bits
    # lower
    it: int = 0
    is_: int = 0
    id_: int = 0
    fsf: int = 0
    ftf: int = 0
    imm: int = 0
    word: int = 0

    @property
    def is_branch(self):
        return self.name in BRANCHES

    def vf_reads(self):
        """[(reg, lane mask)] of VF registers read (VF00 included; callers skip it)."""
        n = self.name
        if self.base in ("ITOF", "FTOI", "ABS"):
            return [(self.fs, self.dest)]
        if self.base == "CLIP":
            return [(self.fs, 0xE), (self.ft, 0x1)]
        if self.base in ("OPMULA", "OPMSUB"):
            return [(self.fs, 0xE), (self.ft, 0xE)]
        if self.base in ("ADD", "SUB", "MUL", "MADD", "MSUB", "MAX", "MINI"):
            r = [(self.fs, self.dest)]
            if self.src2 == "vf":
                r.append((self.ft, (8 >> self.bc) if self.bc >= 0 else self.dest))
            return r
        if n in ("MOVE", "MR32"):
            if n == "MOVE":
                return [(self.fs, self.dest)]
            rot = ((self.dest << 1) | (self.dest >> 3)) & 0xF        # MR32: lane i reads lane (i + 1) & 3
            return [(self.fs, rot)]
        if n == "DIV" or n == "RSQRT":
            return [(self.fs, 8 >> self.fsf), (self.ft, 8 >> self.ftf)]
        if n == "SQRT":
            return [(self.ft, 8 >> self.ftf)]
        if n == "MTIR":
            return [(self.fs, 8 >> self.fsf)]
        return []

    def vf_write(self):
        """(reg, mask) written to VF (None for ACC / none)."""
        if self.base in ("ITOF", "FTOI", "ABS"):
            return (self.ft, self.dest)
        if self.base in ("ADD", "SUB", "MUL", "MADD", "MSUB", "MAX", "MINI"):
            return None if self.acc_dst else (self.fd, self.dest)
        if self.base == "OPMSUB":
            return (self.fd, 0xE)
        if self.name in ("MOVE", "MR32", "MFIR"):
            return (self.ft, self.dest)
        return None

    def reads_acc(self):
        return self.base in ("MADD", "MSUB", "OPMSUB")

    def writes_acc(self):
        return self.acc_dst or self.base == "OPMULA"

    def sets_mac(self):
        return self.base in FMAC_BASE or self.base in ("OPMULA", "OPMSUB")


@dataclass
class Micro:
    index: int
    lower: int
    upper: int
    up: Op
    lo: Op = None                # None when the lower word is an I immediate
    ibit: bool = False
    ebit: bool = False
    mbit: bool = False
    dbit: bool = False
    tbit: bool = False
    unknown: list = field(default_factory=list)

    @property
    def loi(self):
        return self.ibit


def _upper(w):
    u = Op(word=w)
    u.dest = (w >> 21) & 15
    u.ft, u.fs, u.fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    f = w & 63
    if f < 28:
        b = _UP_BC.get(f >> 2)
        if b is None:
            return None
        u.name, u.base, u.bc = b + "bc", b, f & 3
    elif f < 60:
        nm = _UP.get(f)
        if nm is None:
            return None
        u.name = nm
        if nm.endswith("i") and nm != "MINI":
            u.base, u.src2 = nm[:-1], "i"
        elif nm.endswith("q"):
            u.base, u.src2 = nm[:-1], "q"
        else:
            u.base = nm
    else:
        code = ((w >> 6) & 31) << 2 | (w & 3)
        if code < 28 and (code >> 2) in _UP2_BC:
            b = _UP2_BC[code >> 2]
            u.name, u.base, u.bc, u.acc_dst = b + "bc", b[:-1], code & 3, True
        else:
            nm = _UP2.get(code)
            if nm is None:
                return None
            u.name = nm
            if nm.startswith("ITOF") or nm.startswith("FTOI"):
                u.base, u.shift = nm[:4], int(nm[4:])
            elif nm in ("ABS", "CLIP", "NOP", "OPMULA"):
                u.base = nm
            else:                                                # xxxA / xxxAq / xxxAi: result to ACC
                core = nm
                if nm[-1] in "iq":
                    u.src2, core = nm[-1], nm[:-1]
                u.base, u.acc_dst = core[:-1], True
    return u


def _lower(w):
    lo = Op(word=w)
    lo.dest = (w >> 21) & 15
    lo.it = lo.ft = (w >> 16) & 31
    lo.is_ = lo.fs = (w >> 11) & 31
    lo.id_ = (w >> 6) & 31
    lo.fsf, lo.ftf = (w >> 21) & 3, (w >> 23) & 3
    if w >> 31:
        if (w >> 25) != 0x40:
            return None
        f = w & 63
        nm = _LO_SP.get(((w >> 6) & 31) << 2 | (w & 3)) if f >= 60 else _LO_OP.get(f)
        if nm is None:
            return None
        lo.name = nm
        if nm == "IADDI":
            v = (w >> 6) & 31
            lo.imm = v - 32 if v & 16 else v
        return lo
    nm = _LO.get(w >> 25)
    if nm is None:
        return None
    lo.name = nm
    if nm in ("IADDIU", "ISUBIU"):
        lo.imm = ((w >> 10) & 0x7800) | (w & 0x7FF)
    elif nm in ("FCEQ", "FCSET", "FCAND", "FCOR"):
        lo.imm = w & 0xFFFFFF
    elif nm in ("FSEQ", "FSSET", "FSAND", "FSOR"):
        lo.imm = ((w >> 10) & 0x800) | (w & 0x7FF)
    elif nm in BRANCHES or nm in ("LQ", "SQ", "ILW", "ISW"):
        v = w & 0x7FF
        lo.imm = v - 0x800 if v & 0x400 else v
    return lo


def decode(lower, upper, index=0):
    up = _upper(upper)
    m = Micro(index, lower, upper, up if up is not None else Op(name="?", word=upper))
    if up is None:
        m.unknown.append("upper %08x" % upper)
    m.ibit, m.ebit, m.mbit, m.dbit, m.tbit = (bool(upper >> b & 1) for b in (31, 30, 29, 28, 27))
    if not m.ibit:
        lo = _lower(lower)
        m.lo = lo if lo is not None else Op(name="?", word=lower)
        if lo is None:
            m.unknown.append("lower %08x" % lower)
    return m


def decode_program(code):
    """bytes (multiple of 8) -> [Micro]"""
    out = []
    for k in range(len(code) // 8):
        lw = int.from_bytes(code[8 * k:8 * k + 4], "little")
        uw = int.from_bytes(code[8 * k + 4:8 * k + 8], "little")
        out.append(decode(lw, uw, k))
    return out


def dest_str(d):
    return "".join(c for c, b in zip(XYZW, (8, 4, 2, 1)) if d & b)


def text(m):
    """One-line listing of a microinstruction."""
    u = m.up
    if u.name == "NOP":
        us = "nop"
    elif u.base == "CLIP":
        us = "clipw.xyz vf%02d, vf%02dw" % (u.fs, u.ft)
    else:
        d = "ACC" if (u.acc_dst or u.base == "OPMULA") else "vf%02d" % (u.ft if u.base in ("ITOF", "FTOI", "ABS")
                                                                         else u.fd)
        t = {"i": "I", "q": "Q"}.get(u.src2, "vf%02d%s" % (u.ft, XYZW[u.bc] if u.bc >= 0 else ""))
        us = "%s.%s %s, vf%02d%s" % (u.name.lower(), dest_str(u.dest) or "xyz", d, u.fs,
                                     "" if u.base in ("ITOF", "FTOI", "ABS") else ", " + t)
    flags = "".join(c for c, b in (("I", m.ibit), ("E", m.ebit), ("M", m.mbit), ("D", m.dbit), ("T", m.tbit)) if b)
    if m.ibit:
        ls = "loi 0x%08x" % m.lower
    else:
        lo = m.lo
        ls = lo.name.lower()
        if lo.name in BRANCHES:
            ls += " vi%02d, vi%02d, 0x%03x" % (lo.it, lo.is_, m.index + 1 + lo.imm)
        elif lo.name in ("IADDIU", "ISUBIU"):
            ls += " vi%02d, vi%02d, 0x%x" % (lo.it, lo.is_, lo.imm)
        elif lo.name in ("FCAND", "FCOR", "FCEQ", "FCSET"):
            ls += " vi01, 0x%x" % lo.imm
        elif lo.name in ("FMAND", "FMOR", "FMEQ"):
            ls += " vi%02d, vi%02d" % (lo.it, lo.is_)
        elif lo.name in ("MOVE", "MR32"):
            ls += ".%s vf%02d, vf%02d" % (dest_str(lo.dest), lo.ft, lo.fs)
        elif lo.name in ("DIV", "RSQRT"):
            ls += " Q, vf%02d%s, vf%02d%s" % (lo.fs, XYZW[lo.fsf], lo.ft, XYZW[lo.ftf])
        elif lo.name == "SQRT":
            ls += " Q, vf%02d%s" % (lo.ft, XYZW[lo.ftf])
        elif lo.name in ("IADD", "ISUB", "IAND", "IOR"):
            ls += " vi%02d, vi%02d, vi%02d" % (lo.id_, lo.is_, lo.it)
    return "%03x: %-34s %s%s" % (m.index, us, ls, (" [%s]" % flags) if flags else "")
