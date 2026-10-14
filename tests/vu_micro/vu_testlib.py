"""Synthetic material for the VU0 microcode translator tests: an encoder for VU micro instructions, a random program
generator, and a cycle-level reference simulator with exact arithmetic (rationals rounded toward zero, results below
2^-126 flushed to a signed zero, operands clamped). Everything is generated here; no game data.

The simulator runs the documented micro-mode model concretely (absolute cycle counter, FIFO pipelines), as the
translator describes it in ps2rc/vu/micro_gen.py, so the C the translator writes can be compared with it state by
state."""
import random
from fractions import Fraction
from math import isqrt

SGN = 0x80000000
FMAX = 0x7F7FFFFF
ONE = 0x3F800000


# ---------------------------------------------------------------------------------------------- exact floats
def clamp(u):
    e = (u >> 23) & 0xFF
    if e == 0:
        return u & SGN
    if e == 255:
        return (u & SGN) | FMAX
    return u


def val(u):
    """clean bits -> Fraction"""
    u = clamp(u)
    e = (u >> 23) & 0xFF
    if e == 0:
        return Fraction(0)
    m = (u & 0x7FFFFF) | 0x800000
    v = Fraction(m) * Fraction(2) ** (e - 150)
    return -v if u & SGN else v


def bits(v, zero_sign=0):
    """Fraction -> single bits, rounded toward zero, flush below 2^-126, FLT_MAX above"""
    if v == 0:
        return zero_sign << 31
    s = 1 if v < 0 else 0
    a = -v if s else v
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    if Fraction(2) ** (e + 1) <= a:
        e += 1
    if e < -126:
        return s << 31
    if e > 127:
        return (s << 31) | FMAX
    m = int(a / Fraction(2) ** (e - 23))          # floor = toward zero (a > 0)
    return (s << 31) | ((e + 127) << 23) | (m & 0x7FFFFF)


def fadd(a, b):
    a, b = clamp(a), clamp(b)
    r = val(a) + val(b)
    if r == 0:                                   # exact zero: -0 only for -0 + -0
        return SGN if (a & SGN) and (b & SGN) and (a & 0x7FFFFFFF) == 0 and (b & 0x7FFFFFFF) == 0 else 0
    return bits(r)


def fsub(a, b):
    return fadd(a, clamp(b) ^ SGN)


def fmul(a, b):
    a, b = clamp(a), clamp(b)
    return bits(val(a) * val(b), ((a ^ b) >> 31) & 1)


def fdiv(s, t):
    s, t = clamp(s), clamp(t)
    if t & 0x7FFFFFFF == 0:
        return ((s ^ t) & SGN) | FMAX
    return bits(val(s) / val(t), ((s ^ t) >> 31) & 1)


def fsqrt(t):
    t = clamp(t) & 0x7FFFFFFF
    v = val(t)
    if v == 0:
        return 0
    k = 128
    n = v * (Fraction(2) ** (2 * k))
    r = isqrt(n.numerator // n.denominator)
    return bits(Fraction(r) / Fraction(2) ** k)


def frsqrt(s, t):
    s, t = clamp(s), clamp(t)
    if t & 0x7FFFFFFF == 0:
        if s & 0x7FFFFFFF == 0:
            return (s ^ t) & SGN
        return ((s ^ t) & SGN) | FMAX
    return fdiv(s, fsqrt(t))


def itof(u, shift):
    v = u - (1 << 32) if u & SGN else u
    return bits(Fraction(v) / (1 << shift))


def mac_bits(r, lane):
    sh = 3 - lane
    return ((1 if (r & 0x7FFFFFFF) == 0 else 0) << sh) | ((r >> 31) << (4 + sh))


def clip_judge(x, y, z, w):
    a = (w & 0x7FFFFFFF) if (w & 0x7F800000) else 0x007FFFFF
    r = 0
    for k, v in enumerate((x, y, z)):
        if (v & 0x7FFFFFFF) > a:
            r |= (2 if v & SGN else 1) << (2 * k)
    return r


# ---------------------------------------------------------------------------------------------- encoder
UP_BC = {"ADD": 0, "SUB": 1, "MADD": 2, "MSUB": 3, "MUL": 6}
UP_IQ = {"MULq": 28, "MULi": 30, "ADDq": 32, "MADDq": 33, "ADDi": 34, "MADDi": 35, "SUBq": 36, "MSUBq": 37,
         "SUBi": 38, "MSUBi": 39}
UP_V = {"ADD": 40, "MADD": 41, "MUL": 42, "MAX": 43, "SUB": 44, "MSUB": 45, "OPMSUB": 46, "MINI": 47}
UP2_BC = {"ADDA": 0, "SUBA": 1, "MADDA": 2, "MSUBA": 3, "MULA": 6}
UP2 = {"ITOF0": 16, "ITOF4": 17, "ITOF12": 18, "ITOF15": 19, "MULAq": 28, "ABS": 29, "MULAi": 30, "CLIP": 31,
       "ADDAq": 32, "MADDAq": 33, "ADDAi": 34, "MADDAi": 35, "SUBAq": 36, "MSUBAq": 37, "SUBAi": 38, "MSUBAi": 39,
       "ADDA": 40, "MADDA": 41, "MULA": 42, "SUBA": 44, "MSUBA": 45, "OPMULA": 46, "NOP": 47}
LOWER_NOP = 0x8000033C


def up(name, dest=15, fd=0, fs=0, ft=0, bc=None, e=False, i=False):
    w = (dest << 21) | (ft << 16) | (fs << 11)
    if bc is not None and name in UP_BC:
        w |= (fd << 6) | (UP_BC[name] << 2) | bc
    elif bc is not None and name in UP2_BC:
        code = (UP2_BC[name] << 2) | bc
        w |= ((code >> 2) << 6) | 0x3C | (code & 3)
    elif name in UP_IQ:
        w |= (fd << 6) | UP_IQ[name]
    elif name in UP_V:
        w |= (fd << 6) | UP_V[name]
    else:
        code = UP2[name]
        w |= ((code >> 2) << 6) | 0x3C | (code & 3)
    return w | (e << 30) | (i << 31)


def lo_op(fn, dest=0, ft=0, fs=0, fd=0):
    return (0x40 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn


def lo_sp(code, dest=0, ft=0, fs=0):
    return (0x40 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | 0x3C | (code & 3)


def lo(name, **k):
    g = lambda n: k.get(n, 0)
    if name == "NOP":
        return LOWER_NOP
    if name in ("IADD", "ISUB", "IAND", "IOR"):
        return lo_op({"IADD": 48, "ISUB": 49, "IAND": 52, "IOR": 53}[name], ft=g("it"), fs=g("is_"), fd=g("id_"))
    if name == "IADDI":
        return lo_op(50, ft=g("it"), fs=g("is_"), fd=g("imm") & 31)
    if name in ("IADDIU", "ISUBIU"):
        imm = g("imm") & 0x7FFF
        return ((8 if name == "IADDIU" else 9) << 25) | ((imm >> 11) << 21) | (g("it") << 16) | (g("is_") << 11) | \
            (imm & 0x7FF)
    if name in ("MOVE", "MR32"):
        return lo_sp(48 if name == "MOVE" else 49, dest=g("dest"), ft=g("ft"), fs=g("fs"))
    if name == "MTIR":
        return lo_sp(60, dest=g("fsf"), ft=g("it"), fs=g("fs"))
    if name == "MFIR":
        return lo_sp(61, dest=g("dest"), ft=g("ft"), fs=g("is_"))
    if name in ("DIV", "RSQRT"):
        return lo_sp(56 if name == "DIV" else 58, dest=(g("ftf") << 2) | g("fsf"), ft=g("ft"), fs=g("fs"))
    if name == "SQRT":
        return lo_sp(57, dest=g("ftf") << 2, ft=g("ft"))
    if name == "WAITQ":
        return lo_sp(59)
    if name in ("FMAND", "FMOR"):
        return ((0x1A if name == "FMAND" else 0x1B) << 25) | (g("it") << 16) | (g("is_") << 11)
    if name == "FMEQ":
        return (0x18 << 25) | (1 << 16) | (g("is_") << 11)
    if name in ("FCAND", "FCOR", "FCEQ"):
        return ({"FCAND": 0x12, "FCOR": 0x13, "FCEQ": 0x10}[name] << 25) | (g("imm") & 0xFFFFFF)
    if name == "FCGET":
        return (0x1C << 25) | (g("it") << 16)
    if name in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ", "B", "BAL"):
        op = {"IBEQ": 0x28, "IBNE": 0x29, "IBLTZ": 0x2C, "IBGTZ": 0x2D, "IBLEZ": 0x2E, "IBGEZ": 0x2F, "B": 0x20,
              "BAL": 0x21}[name]
        return (op << 25) | (g("it") << 16) | (g("is_") << 11) | (g("imm") & 0x7FF)
    if name == "JR":
        return (0x24 << 25) | (g("is_") << 11)
    raise ValueError(name)


# ---------------------------------------------------------------------------------------------- random programs
def lanes(mask):
    return [i for i in range(4) if mask & (8 >> i)]


def random_program(r, n=40, sub=True):
    """[(lower, upper)] words; entry at 0. Shapes: random arithmetic / flag / Q / integer instructions, forward
    branches with ordinary delay slots, an optional BAL subroutine (ending with JR vi15), E-bit end."""
    body = []                                     # entries: (lower, upper) or ("BR", kind) placeholders
    vf = lambda: r.randrange(1, 32)
    vi = lambda: r.randrange(1, 14)
    dest = lambda: r.choice([15, 15, 14, 8, 4, 2, 1, 12, 3, 7])
    for _ in range(n):
        k = r.random()
        uw = up("NOP")
        lw = lo("NOP")
        if k < 0.45:
            kind = r.random()
            if kind < 0.35:
                nm = r.choice(["ADD", "SUB", "MUL", "MADD", "MSUB"])
                uw = up(nm, dest(), vf(), r.choice([0, vf(), vf()]), r.choice([0, vf(), vf()]),
                        bc=r.choice([None, None, 0, 1, 2, 3]))
            elif kind < 0.5:
                nm = r.choice(list(UP_IQ))
                uw = up(nm, dest(), vf(), vf(), 0)
            elif kind < 0.65:
                nm = r.choice(["ADDA", "SUBA", "MULA", "MADDA", "MSUBA"])
                uw = up(nm, dest(), 0, vf(), vf(), bc=r.choice([None, 0, 1, 2, 3]))
            elif kind < 0.72:
                uw = up(r.choice(["ADDAq", "MULAi", "MADDAi", "SUBAq", "MSUBAq"]), dest(), 0, vf(), 0)
            elif kind < 0.82:
                uw = up("OPMULA", 14, 0, vf(), vf())
                body.append((lw, uw))
                uw = up("OPMSUB", 14, vf(), vf(), vf())
            elif kind < 0.9:
                uw = up(r.choice(["ITOF0", "ITOF4", "ITOF12", "ITOF15", "ABS"]), dest(), 0, vf(), vf())
            else:
                uw = up("CLIP", 14, 0, vf(), vf())
        if r.random() < 0.6:
            kind = r.random()
            if kind < 0.25:
                nm = r.choice(["IADD", "ISUB", "IAND", "IOR", "IADDI", "IADDIU", "ISUBIU"])
                lw = lo(nm, it=vi(), is_=r.choice([0, vi()]), id_=vi(), imm=r.randrange(0, 0x7FFF))
            elif kind < 0.4:
                lw = lo(r.choice(["FMAND", "FMOR", "FMEQ"]), it=vi(), is_=vi())
            elif kind < 0.55:
                lw = lo(r.choice(["FCAND", "FCOR", "FCEQ", "FCGET"]), it=vi(), imm=r.randrange(1 << 24))
            elif kind < 0.7:
                lw = lo(r.choice(["DIV", "SQRT", "RSQRT", "WAITQ"]), ft=r.choice([0, vf()]), fs=r.choice([0, vf()]),
                        ftf=r.randrange(4), fsf=r.randrange(4))
            elif kind < 0.9:
                nm = r.choice(["MOVE", "MR32", "MFIR", "MTIR"])
                lw = lo(nm, dest=dest(), ft=vf(), fs=r.choice([0, vf()]), is_=vi(), it=vi(), fsf=r.randrange(4))
            else:
                lw, uw = r.getrandbits(32), uw | (1 << 31)      # LOI
        if lw != LOWER_NOP and not (uw >> 31) and _vf_dst_lo(lw) and _vf_dst_lo(lw) == _vf_dst_up(uw):
            lw = LOWER_NOP                           # upper and lower must not write the same VF
        body.append((lw, uw))
        if r.random() < 0.12:
            body.append(("BR", r.choice(["IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"])))
        if sub and r.random() < 0.05:
            body.append(("BAL", 0))
    # resolve: every branch gets a NOP-lower instruction before it (no VI-delay hazard) and an ordinary delay slot
    prog = []
    fixups = []
    for item in body:
        if item[0] == "BR" or item[0] == "BAL":
            prog.append((LOWER_NOP, up("NOP")))
            fixups.append((len(prog), item))
            prog.append(None)
            prog.append((LOWER_NOP, up("ADD", 15, r.randrange(1, 32), r.randrange(32), r.randrange(32))))  # delay slot
            continue
        prog.append(item)
    end = len(prog)
    prog.append((LOWER_NOP, up("NOP", e=True)))
    prog.append((LOWER_NOP, up("NOP")))
    sub_at = len(prog)
    sub_body = [(lo("IADDIU", it=r.randrange(1, 14), is_=0, imm=r.randrange(100)), up("ADD", 15, r.randrange(1, 32),
                                                                                       r.randrange(1, 32),
                                                                                       r.randrange(1, 32))),
                (LOWER_NOP, up("MUL", 7, r.randrange(1, 32), r.randrange(1, 32), r.randrange(1, 32))),
                (LOWER_NOP, up("NOP")),
                (lo("JR", is_=15), up("NOP")),
                (LOWER_NOP, up("NOP"))]
    prog += sub_body
    for at, item in fixups:
        if item[0] == "BAL":
            prog[at] = (lo("BAL", it=15, imm=sub_at - (at + 1)), up("NOP"))
        else:
            t = min(end, at + 2 + r.randrange(0, 6))
            prog[at] = (lo(item[1], it=r.randrange(0, 14), is_=r.randrange(0, 14), imm=t - (at + 1)), up("NOP"))
    return prog


def _vf_dst_up(uw):
    from ps2rc.vu.micro_gen_isa import _upper
    u = _upper(uw)
    if u is None:
        return 0
    w = u.vf_write()
    return w[0] if w else 0


def _vf_dst_lo(lw):
    from ps2rc.vu.micro_gen_isa import _lower
    lo_ = _lower(lw)
    if lo_ is None:
        return 0
    w = lo_.vf_write()
    return w[0] if w else 0


def to_bytes(prog):
    out = bytearray()
    for lw, uw in prog:
        out += lw.to_bytes(4, "little") + uw.to_bytes(4, "little")
    return bytes(out)


def random_word(r):
    k = r.random()
    if k < 0.45:
        e = r.randrange(110, 145)
        return (r.getrandbits(1) << 31) | (e << 23) | r.getrandbits(23)
    if k < 0.6:
        return r.randrange(-3000, 3000) & 0xFFFFFFFF          # small ints (ITOF sources)
    if k < 0.7:
        return r.choice([0, SGN, ONE, ONE | SGN, 0x7F800000, 0xFF800000, 0x7FFFFFFF, 0x00000001, 0x80000003,
                         FMAX, 0x00400000])
    return r.getrandbits(32)


def random_state(r):
    return {"vf": [[0, 0, 0, ONE]] + [[random_word(r) for _ in range(4)] for _ in range(31)],
            "acc": [random_word(r) for _ in range(4)], "vi": [0] + [r.getrandbits(16) for _ in range(15)],
            "i": random_word(r), "q": random_word(r), "mac": r.getrandbits(8), "clip": r.getrandbits(24),
            "status": r.getrandbits(12)}


# ---------------------------------------------------------------------------------------------- reference simulator
class Sim:
    def __init__(self, code):
        from ps2rc.vu.micro_gen_isa import decode_program
        self.p = decode_program(code)

    def rd(self, r, i):
        return (0, 0, 0, ONE)[i] if r == 0 else self.vf[r][i]

    def run(self, st, entry, limit=5000):
        self.vf = [list(v) for v in st["vf"]]
        self.acc = list(st["acc"])
        self.vi = list(st["vi"])
        self.I, self.Q = st["i"], st["q"]
        self.mac = st["mac"]                 # visible MAC (16 bits; Z/S modelled)
        self.clip = st["clip"]               # visible clip register
        run_mac, run_clip = self.mac, self.clip
        cycle = 0
        fmac = []                            # [s, writes [(reg, mask)], mac snapshot, clip value or None]
        fdiv = None                          # [s, latency, value]
        last_q = None
        br = None
        eb = 0
        last_ialu = (None, None)             # (cycle, reg)
        pc = entry
        for _ in range(limit):
            m = self.p[pc]
            u, lo = m.up, m.lo
            cycle += 1
            reads = list(u.vf_reads()) if u.base != "NOP" else []
            lname = lo.name if lo is not None else "LOI"
            lo_pipe = None
            if lo is not None:
                if lname in ("MOVE", "MR32", "MFIR", "FMAND", "FMOR", "FMEQ", "FCAND", "FCOR", "FCEQ", "FCGET"):
                    lo_pipe = "fmac" if not (lname in ("MOVE", "MR32", "MFIR") and lo.ft == 0) else None
                elif lname in ("DIV", "SQRT", "RSQRT", "WAITQ"):
                    lo_pipe = "fdiv"
                if lo_pipe or lname == "MTIR":
                    reads += lo.vf_reads()
            for reg, mask in reads:
                if not reg:
                    continue
                for e in fmac:
                    if cycle - e[0] < 4 and any(rr == reg and mm & mask for rr, mm in e[1]):
                        cycle = max(cycle, e[0] + 4)
            if lo_pipe == "fdiv" and fdiv is not None:
                cycle = max(cycle, fdiv[0] + fdiv[1])
            while fmac and cycle - fmac[0][0] >= 4:
                e = fmac.pop(0)
                self.mac = e[2]
                if e[3] is not None:
                    self.clip = e[3]
            if fdiv is not None and cycle - fdiv[0] >= fdiv[1]:
                self.Q, fdiv = fdiv[2], None
            # upper
            uout = []
            if u.base in ("ADD", "SUB", "MUL", "MADD", "MSUB"):
                mac = 0
                for i in lanes(u.dest):
                    a = self.rd(u.fs, i)
                    b = self.I if u.src2 == "i" else self.Q if u.src2 == "q" else \
                        self.rd(u.ft, u.bc if u.bc >= 0 else i)
                    f = {"ADD": lambda: fadd(a, b), "SUB": lambda: fsub(a, b), "MUL": lambda: fmul(a, b),
                         "MADD": lambda: fadd(self.acc[i], fmul(a, b)),
                         "MSUB": lambda: fsub(self.acc[i], fmul(a, b))}[u.base]()
                    mac |= mac_bits(f, i)
                    uout.append(("acc" if u.acc_dst else "vf", u.fd, i, f))
                run_mac = mac
            elif u.base in ("OPMULA", "OPMSUB"):
                rot = ((1, 2), (2, 0), (0, 1))
                mac = 0
                for i in range(3):
                    p = fmul(self.rd(u.fs, rot[i][0]), self.rd(u.ft, rot[i][1]))
                    f = p if u.base == "OPMULA" else fsub(self.acc[i], p)
                    mac |= mac_bits(f, i)
                    uout.append(("acc" if u.base == "OPMULA" else "vf", u.fd, i, f))
                run_mac = mac | (run_mac & 0x1111)
            elif u.base == "ITOF":
                for i in lanes(u.dest):
                    uout.append(("vf", u.ft, i, itof(self.rd(u.fs, i), u.shift)))
            elif u.base == "ABS":
                for i in lanes(u.dest):
                    uout.append(("vf", u.ft, i, self.rd(u.fs, i) & 0x7FFFFFFF))
            elif u.base == "CLIP":
                run_clip = ((run_clip << 6) | clip_judge(self.rd(u.fs, 0), self.rd(u.fs, 1), self.rd(u.fs, 2),
                                                          self.rd(u.ft, 3))) & 0xFFFFFF
            # lower
            lout = []
            vi = self.vi
            setvi = lambda r_, v: vi.__setitem__(r_, v & 0xFFFF) if r_ else None
            ialu = None
            pend = None
            nbr = None
            if lo is not None:
                if lname in ("MOVE", "MR32") and lo.ft:
                    for i in lanes(lo.dest):
                        lout.append((lo.ft, i, self.rd(lo.fs, i if lname == "MOVE" else (i + 1) & 3)))
                elif lname == "MFIR" and lo.ft:
                    v = vi[lo.is_] if lo.is_ else 0
                    for i in lanes(lo.dest):
                        lout.append((lo.ft, i, (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF))
                elif lname == "MTIR":
                    setvi(lo.it, self.rd(lo.fs, lo.fsf))
                    ialu = lo.it
                elif lname in ("IADD", "ISUB", "IAND", "IOR"):
                    a, b = (vi[lo.is_] if lo.is_ else 0), (vi[lo.it] if lo.it else 0)
                    setvi(lo.id_, {"IADD": a + b, "ISUB": a - b, "IAND": a & b, "IOR": a | b}[lname])
                    ialu = lo.id_
                elif lname == "IADDI":
                    setvi(lo.it, (vi[lo.is_] if lo.is_ else 0) + lo.imm)
                    ialu = lo.it
                elif lname in ("IADDIU", "ISUBIU"):
                    a = vi[lo.is_] if lo.is_ else 0
                    setvi(lo.it, a + lo.imm if lname == "IADDIU" else a - lo.imm)
                    ialu = lo.it
                elif lname == "FMAND":
                    setvi(lo.it, (vi[lo.is_] if lo.is_ else 0) & self.mac)
                elif lname == "FMOR":
                    setvi(lo.it, (vi[lo.is_] if lo.is_ else 0) | self.mac)
                elif lname == "FMEQ":
                    setvi(1, 1 if (self.mac & 0xFFFF) == (vi[lo.is_] if lo.is_ else 0) else 0)
                elif lname == "FCAND":
                    setvi(1, 1 if self.clip & lo.imm else 0)
                elif lname == "FCOR":
                    setvi(1, 1 if (self.clip | lo.imm) & 0xFFFFFF == 0xFFFFFF else 0)
                elif lname == "FCEQ":
                    setvi(1, 1 if self.clip & 0xFFFFFF == lo.imm else 0)
                elif lname == "FCGET":
                    setvi(lo.it, self.clip & 0xFFF)
                elif lname == "DIV":
                    pend = (7, fdiv_(self.rd(lo.fs, lo.fsf), self.rd(lo.ft, lo.ftf)))
                elif lname == "SQRT":
                    pend = (7, fsqrt(self.rd(lo.ft, lo.ftf)))
                elif lname == "RSQRT":
                    pend = (13, frsqrt(self.rd(lo.fs, lo.fsf), self.rd(lo.ft, lo.ftf)))
                elif lname in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
                    for rr in ([lo.is_, lo.it] if lname in ("IBEQ", "IBNE") else [lo.is_]):
                        if rr and last_ialu == (cycle - 1, rr):
                            raise RuntimeError("VI delay hazard")
                    a = vi[lo.is_] if lo.is_ else 0
                    b = vi[lo.it] if lo.it else 0
                    sa = a - 0x10000 if a & 0x8000 else a
                    take = {"IBEQ": a == b, "IBNE": a != b, "IBLTZ": sa < 0, "IBGTZ": sa > 0, "IBLEZ": sa <= 0,
                            "IBGEZ": sa >= 0}[lname]
                    if take:
                        nbr = m.index + 1 + lo.imm
                elif lname == "B":
                    nbr = m.index + 1 + lo.imm
                elif lname == "BAL":
                    setvi(lo.it, m.index + 2)
                    nbr = m.index + 1 + lo.imm
                elif lname == "JR":
                    nbr = vi[lo.is_]
            # writes
            for kind, rr, i, f in uout:
                if kind == "acc":
                    self.acc[i] = f
                elif rr:
                    self.vf[rr][i] = f
            for rr, i, f in lout:
                self.vf[rr][i] = f
            if m.ibit:
                self.I = m.lower
            if pend is not None:
                fdiv = [cycle, pend[0], pend[1]]
                last_q = pend[1]
            if u.base != "NOP" or lo_pipe == "fmac":
                ws = []
                if u.base != "NOP":
                    w_ = u.vf_write()
                    if w_ and w_[0]:
                        ws.append(w_)
                if lo_pipe == "fmac":
                    w2 = lo.vf_write()
                    if w2 and w2[0]:
                        ws.append(w2)
                fmac.append([cycle, ws, run_mac, run_clip if u.base == "CLIP" else None])
            last_ialu = (cycle, ialu) if ialu else (None, None)
            # control
            npc = pc + 1
            if br is not None:
                br[0] -= 1
                if br[0] == 0:
                    npc, br = br[1], None
            if nbr is not None:
                br = [1, nbr]
            if m.ebit:
                eb = 2
            if eb:
                eb -= 1
                if eb == 0:
                    self.mac, self.clip = run_mac, run_clip
                    if last_q is not None:
                        self.Q = last_q
                    return {"vf": self.vf, "acc": self.acc, "vi": self.vi, "i": self.I, "q": self.Q,
                            "mac": self.mac, "clip": self.clip, "tpc": npc}
            pc = npc
        raise RuntimeError("program did not end")


def fdiv_(s, t):
    return fdiv(s, t)


def rnd(seed):
    return random.Random(seed)
