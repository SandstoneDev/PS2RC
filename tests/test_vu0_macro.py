"""VU0 macro mode (COP2) and the MMI halfword instructions in the C back-end, on the host.

Hand-encoded guest functions (COP2 words from tests/poc_vu0/vu0enc.py, every one checked against the decoder) run
through function discovery + the C back-end (variants v0-v4) + WSL gcc; the expected results come from the Python
reference model tests/poc_vu0/vu0ref.py (exact rationals, written independently of the C runtime) over random and
edge-case vectors. The bit-exact comparison against PCSX2 itself is tests/poc_vu0 (marker oracle).

Covered: the matrix x vector idiom of game math helpers, every FMAC form (lane-wise, x/y/z/w
broadcast, I, Q; Fd/ACC; add/sub/mul/madd/msub), dest masks, fd == fs/ft aliasing, VF00, x - x, VMOVE/VMR32/VABS,
LQC2/SQC2/QMTC2/QMFC2 (128-bit GPR halves in the cached variants), CFC2/CTC2 register rules, the R register,
VCALLMS/VCALLMSR through the rt_vu0_callms hook (and rt_fatal without one), BC0F/BC0T(L) through rt_cpcond0, the
MMI halfword instructions (as Sony's libipu uses them).
"""
import os
import random
import struct
import subprocess
import sys

import pytest
from ps2rc import config  # noqa: E402

from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment
from ps2rc.r5900 import decode

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "poc_vu0"))
import vu0ref as ref  # noqa: E402
from vu0enc import asm  # noqa: E402

OUT = os.path.join(ROOT, "build", "poc_vu0", "unit")
ZERO, AT, V0, V1, A0, A1 = 0, 1, 2, 3, 4, 5
T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, RA = 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31
IN_SZ, OUT_SZ = 256, 256
BASE, SIZE = 0x1000, 0x2000


def i_type(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def r_type(funct, rs=0, rt=0, rd=0, sa=0): return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
def mmi(funct, sub, rd, rs, rt): return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | funct
def addiu(rt, rs, imm): return i_type(0x09, rs, rt, imm)
def lw(rt, off, base): return i_type(0x23, base, rt, off)
def ld(rt, off, base): return i_type(0x37, base, rt, off)
def sd(rt, off, base): return i_type(0x3F, base, rt, off)
def lq(rt, off, base): return i_type(0x1E, base, rt, off)
def sq(rt, off, base): return i_type(0x1F, base, rt, off)
def jr(rs): return r_type(0x08, rs=rs)
def bc0(rt, pc, target): return i_type(0x10, 8, rt, (target - pc - 4) >> 2)     # rt: 0 f, 1 t, 2 fl, 3 tl
def beq(rs, rt, pc, target): return i_type(0x04, rs, rt, (target - pc - 4) >> 2)


V = asm
NOP = 0
RET = [jr(RA), NOP]


def lqc2s(first, count, off=0):
    return [V("lqc2", first + k, off + 16 * k, A0) for k in range(count)]


def sqc2s(regs, off=0):
    return [V("sqc2", r, off + 16 * k, A1) for k, r in enumerate(regs)]


# ---------------------------------------------------------------- guest functions (a0 = input, a1 = output)
F_MATVEC = 0x1000          # matrix x vector idiom: qmtc2 vector, 4 rows by lqc2, vmulax/vmadday/vmaddaz/vmaddw
F_FMAC = 0x1100            # Fd forms, masks, aliasing
F_ACC = 0x1200             # ACC forms, 3-operand madd/msub, x - x
F_IQ = 0x1300              # I/Q operands, vmove/vmr32/vabs
F_XFER = 0x1400            # transfers and control registers
F_RREG = 0x1500            # R register
F_CALLMS = 0x1600          # vcallms / vcallmsr hook
F_BC0 = 0x1700             # bc0f/bc0t polling
F_MMI = 0x1800             # MMI halfword ops
F_MATVEC3 = 0x1900         # 3-row idiom (vmaddz) + matrix x matrix (vmove.w)

PROGRAM = {
    F_MATVEC: [lq(T1, 0, A0), V("qmtc2", T1, 1)] + lqc2s(2, 5, 16) + [
        V("vmulax.xyz", "ACC", 2, 1), V("vmadday.xyz", "ACC", 3, 1), V("vmaddaz.xyz", "ACC", 4, 1),
        V("vmaddw.xyz", 6, 5, 0), V("qmfc2", T0, 6), sq(T0, 0, A1)] + sqc2s([6, 1], 16) + RET,
    F_FMAC: lqc2s(1, 15) + [
        V("vaddx.xyzw", 9, 1, 2), V("vsuby.xz", 10, 3, 4), V("vmulw.y", 11, 5, 6), V("vmula.xyzw", "ACC", 1, 2),
        V("vmaddz.xyw", 12, 3, 4), V("vmsubx.zw", 13, 5, 1), V("vadd.xyzw", 14, 14, 2), V("vmul.xyz", 15, 7, 15),
        V("vmulx.xyzw", 8, 8, 8), V("vsub.yw", 7, 7, 0)] + sqc2s([9, 10, 11, 12, 13, 14, 15, 8]) +
        [V("sqc2", 7, 128, A1)] + RET,
    F_ACC: lqc2s(1, 10) + [
        V("vadda.xyzw", "ACC", 1, 2), V("vmsubay.xy", "ACC", 3, 4), V("vsubaw.zw", "ACC", 5, 6),
        V("vmaddax.xyzw", "ACC", 1, 3), V("vmadd.xyzw", 7, 2, 4), V("vmsub.xyzw", 8, 5, 6),
        V("vmsuba.xyzw", "ACC", 1, 1), V("vmaddw.xyzw", 9, 2, 0), V("vsub.xyz", 10, 3, 3),
        V("vmulay.xyzw", "ACC", 6, 0), V("vmsubw.xyzw", 11, 4, 0)] + sqc2s([7, 8, 9, 10, 11]) + RET,
    F_IQ: [lw(T0, 0, A0), V("ctc2", T0, 21), lw(T1, 4, A0), V("ctc2", T1, 22)] + lqc2s(1, 9, 16) + [
        V("vmuli.xyzw", 3, 1, 0), V("vaddq.xy", 4, 2, 0), V("vmaddai.xyzw", "ACC", 1, 0), V("vmsubq.zw", 5, 2, 0),
        V("vmove.xzw", 6, 1), V("vmr32.xyzw", 7, 2), V("vmr32.y", 8, 8), V("vabs.xyz", 9, 1),
        V("vmsubai.xyzw", "ACC", 2, 0), V("vmadd.xyzw", 1, 3, 4), V("vmulaq.xyzw", "ACC", 3, 0),
        V("vsubi.xyzw", 2, 5, 0), V("cfc2", T2, 21), sd(T2, 0, A1), V("cfc2.i", T3, 22), sd(T3, 8, A1)] +
        sqc2s([3, 4, 5, 6, 7, 8, 9, 1, 2], 16) + RET,
    F_XFER: [lq(T0, 0, A0), ld(T0, 16, A0), V("qmtc2", T0, 1), V("sqc2", 1, 0, A1),
             lq(T1, 32, A0), lw(T1, 48, A0), V("qmtc2.i", T1, 2), V("sqc2", 2, 16, A1),
             V("qmtc2", ZERO, 3), V("sqc2", 3, 32, A1), V("qmfc2.i", T2, 0), sq(T2, 48, A1),
             V("sqc2", 0, 64, A1), V("qmtc2", T0, 0), V("lqc2", 0, 0, A0), V("sqc2", 0, 80, A1),
             lw(T3, 64, A0), V("ctc2", T3, 5), lq(T4, 80, A0), V("cfc2", T4, 5), sq(T4, 96, A1),
             lw(T5, 68, A0), V("ctc2", T5, 20), V("cfc2.i", T6, 20), sd(T6, 112, A1),
             lw(T7, 72, A0), V("ctc2", T7, 28), V("cfc2", T8, 28), sd(T8, 120, A1),
             V("ctc2", T3, 0), V("cfc2", T9, 0), sd(T9, 128, A1), lw(T3, 76, A0), V("ctc2", T3, 21),
             V("cfc2", T9, 21), sd(T9, 136, A1), V("ctc2", T3, 17), V("ctc2", T3, 29), V("cfc2", T9, 29),
             sd(T9, 144, A1), V("ctc2", T3, 27), V("cfc2", T9, 27), sd(T9, 152, A1),
             V("lqc2", 4, 96, A0), V("qmfc2", T2, 4), sq(T2, 160, A1), V("qmfc2", ZERO, 4),
             V("ctc2", T3, 16), V("cfc2", T9, 16), sd(T9, 176, A1)] + RET,
    F_RREG: [V("lqc2", 1, 0, A0), V("lqc2", 3, 16, A0), V("vrinit", 1, "y"), V("vrget.xyzw", 2),
             V("vrnext.xz", 3), V("vrnext.w", 3), V("vrxor", 1, "z"), V("vrnext.xyzw", 4), V("vrnext.x", 0),
             V("vrnext.y", 5), V("cfc2", T0, 20), sd(T0, 0, A1)] + sqc2s([2, 3, 4], 16) + RET,
    F_CALLMS: [V("lqc2", 1, 0, A0), lw(T0, 16, A0), V("ctc2", T0, 27), V("vcallms", 0x68), V("cfc2.i", T1, 1),
               V("qmfc2.i", T2, 1), sd(T1, 0, A1), sq(T2, 16, A1), V("vcallmsr"), V("cfc2.i", T1, 1),
               sd(T1, 32, A1)] + RET,
    # v0: polls of a bc0f loop; v1: polls of a bc0t loop (b back); t0: bc0tl, delay slot +1, fall-through +16
    F_BC0: [addiu(V0, ZERO, 0), addiu(V0, V0, 1), bc0(0, F_BC0 + 0x08, F_BC0 + 0x04), NOP,
            addiu(V1, ZERO, 0), addiu(V1, V1, 1), bc0(1, F_BC0 + 0x18, F_BC0 + 0x28), NOP,
            beq(ZERO, ZERO, F_BC0 + 0x20, F_BC0 + 0x14), NOP,
            addiu(T0, ZERO, 0), bc0(3, F_BC0 + 0x2C, F_BC0 + 0x38), addiu(T0, T0, 1), addiu(T0, T0, 16),
            sd(V0, 0, A1), sd(V1, 8, A1), sd(T0, 16, A1)] + RET,
    F_MMI: [lq(T0, 0, A0), lq(T1, 16, A0), lq(T2, 32, A0),
            mmi(0x08, 4, T3, T0, T1), sq(T3, 0, A1),          # paddh
            mmi(0x08, 6, T3, T0, T2), sq(T3, 16, A1),         # pcgth
            mmi(0x08, 7, T3, T1, T2), sq(T3, 32, A1),         # pmaxh
            mmi(0x28, 7, T3, T1, T2), sq(T3, 48, A1),         # pminh
            mmi(0x08, 27, T3, T0, T1), sq(T3, 64, A1),        # ppacb
            mmi(0x28, 26, T3, T1, T2), sq(T3, 80, A1),        # pextub
            mmi(52, 5, T3, 0, T0), sq(T3, 96, A1),            # psllh 5
            mmi(54, 15, T3, 0, T1), sq(T3, 112, A1),          # psrlh 15
            mmi(55, 3, T3, 0, T2), sq(T3, 128, A1),           # psrah 3
            mmi(54, 0, T3, 0, T2), sq(T3, 144, A1)] + RET,    # psrlh 0
    F_MATVEC3: lqc2s(1, 13) + [
        V("vmulax.xyz", "ACC", 2, 1), V("vmadday.xyz", "ACC", 3, 1), V("vmaddz.xyz", 6, 4, 1),
        V("vmulax.xyz", "ACC", 5, 8), V("vmadday.xyz", "ACC", 6, 8), V("vmaddz.xyz", 9, 7, 8),
        V("vmove.w", 9, 13), V("vmulax.xyzw", "ACC", 10, 11), V("vmadday.xyzw", "ACC", 11, 12),
        V("vmaddaz.xyzw", "ACC", 12, 13), V("vmaddw.xyzw", 12, 13, 0)] + sqc2s([6, 9, 12]) + RET,
}
SEEDS = sorted(PROGRAM)
CODE = [(BASE, BASE + SIZE)]


def image():
    mem = bytearray(SIZE)
    for a, words in PROGRAM.items():
        assert len(words) * 4 <= 0x100, hex(a)
        for k, w in enumerate(words):
            struct.pack_into("<I", mem, a - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<vu0>", entry=BASE, gp=0, segments=[seg], overlay_slots=[])


# ---------------------------------------------------------------- input vectors
def rand_float(rnd):
    r = rnd.random()
    if r < 0.40:
        return ref.f2u(rnd.uniform(-1e4, 1e4))
    if r < 0.50:
        return ref.f2u(float(rnd.randint(-8, 8)))
    if r < 0.62:                                           # zeros and denormals
        return rnd.getrandbits(1) << 31 | (rnd.getrandbits(23) if rnd.random() < 0.7 else 0)
    if r < 0.72:                                           # huge: products overflow (round toward zero -> FLT_MAX)
        return rnd.getrandbits(1) << 31 | rnd.randint(200, 254) << 23 | rnd.getrandbits(23)
    if r < 0.80:                                           # tiny: products underflow (flush to zero)
        return rnd.getrandbits(1) << 31 | rnd.randint(1, 40) << 23 | rnd.getrandbits(23)
    if r < 0.90:                                           # exponent 255 (no Inf/NaN on the VU)
        return rnd.getrandbits(1) << 31 | 255 << 23 | rnd.getrandbits(23) * rnd.getrandbits(1)
    return rnd.getrandbits(32)


def vectors(n, seed):
    rnd = random.Random(seed)
    out = []
    for k in range(n):
        lanes = [rand_float(rnd) for _ in range(IN_SZ // 4)]
        if k % 5 == 1:                                     # cancellation: lane pairs that sum to (almost) zero
            for j in range(0, len(lanes) - 4, 8):
                lanes[j + 4] = lanes[j] ^ 0x80000000
        out.append(struct.pack("<%dI" % len(lanes), *lanes))
    return out


# ---------------------------------------------------------------- expectations (reference model)
def q(b, k):
    return list(struct.unpack_from("<4I", b, 16 * k))


def pack(*lanes):
    return struct.pack("<%dI" % len(lanes), *lanes)


def F(regs, d, comps, fn):
    """Apply fn(lane) to the dest lanes of regs[d] (all lanes computed from the old state first)."""
    new = [fn(i) for i in range(4)]
    regs.setdefault(d, [None] * 4)
    for i, ch in enumerate("xyzw"):
        if ch in comps:
            regs[d][i] = new[i]


ONE = 0x3F800000


def vf_init(inp, first, count, off=0):
    regs = {0: [0, 0, 0, ONE]}
    for k in range(count):
        regs[first + k] = list(struct.unpack_from("<4I", inp, off + 16 * k))
    return regs


def exp_matvec(inp):
    vf = vf_init(inp, 2, 5, 16)
    vf[1] = q(inp, 0)
    acc = [0] * 4
    v = vf[1]
    for i in range(3):
        acc[i] = ref.mul(vf[2][i], v[0])
        acc[i] = ref.madd(acc[i], vf[3][i], v[1])
        acc[i] = ref.madd(acc[i], vf[4][i], v[2])
        vf[6][i] = ref.madd(acc[i], vf[5][i], ONE)
    return pack(*vf[6]) + pack(*vf[6]) + pack(*vf[1])


def exp_fmac(inp):
    vf = vf_init(inp, 1, 15)
    acc = [None] * 4
    F(vf, 9, "xyzw", lambda i: ref.add(vf[1][i], vf[2][0]))
    F(vf, 10, "xz", lambda i: ref.sub(vf[3][i], vf[4][1]))
    F(vf, 11, "y", lambda i: ref.mul(vf[5][i], vf[6][3]))
    acc = [ref.mul(vf[1][i], vf[2][i]) for i in range(4)]
    F(vf, 12, "xyw", lambda i: ref.madd(acc[i], vf[3][i], vf[4][2]))
    F(vf, 13, "zw", lambda i: ref.msub(acc[i], vf[5][i], vf[1][0]))
    F(vf, 14, "xyzw", lambda i: ref.add(vf[14][i], vf[2][i]))
    F(vf, 15, "xyz", lambda i: ref.mul(vf[7][i], vf[15][i]))
    F(vf, 8, "xyzw", lambda i: ref.mul(vf[8][i], vf[8][0]))
    F(vf, 7, "yw", lambda i: ref.sub(vf[7][i], vf[0][i]))
    return b"".join(pack(*vf[r]) for r in (9, 10, 11, 12, 13, 14, 15, 8, 7))


def exp_acc(inp):
    vf = vf_init(inp, 1, 10)
    acc = [ref.add(vf[1][i], vf[2][i]) for i in range(4)]
    acc = [ref.msub(acc[i], vf[3][i], vf[4][1]) if i < 2 else acc[i] for i in range(4)]
    acc = [ref.sub(vf[5][i], vf[6][3]) if i >= 2 else acc[i] for i in range(4)]
    acc = [ref.madd(acc[i], vf[1][i], vf[3][0]) for i in range(4)]
    F(vf, 7, "xyzw", lambda i: ref.madd(acc[i], vf[2][i], vf[4][i]))
    F(vf, 8, "xyzw", lambda i: ref.msub(acc[i], vf[5][i], vf[6][i]))
    acc = [ref.msub(acc[i], vf[1][i], vf[1][i]) for i in range(4)]
    F(vf, 9, "xyzw", lambda i: ref.madd(acc[i], vf[2][i], ONE))
    F(vf, 10, "xyz", lambda i: ref.sub(vf[3][i], vf[3][i]))
    acc = [ref.mul(vf[6][i], 0) for i in range(4)]
    F(vf, 11, "xyzw", lambda i: ref.msub(acc[i], vf[4][i], ONE))
    return b"".join(pack(*vf[r]) for r in (7, 8, 9, 10, 11))


def exp_iq_full(inp, acc_before):
    """F_IQ continues from the ACC that F_ACC left (the harness prints it as accin)."""
    i_reg, q_reg = struct.unpack_from("<2I", inp, 0)
    vf = vf_init(inp, 1, 9, 16)
    F(vf, 3, "xyzw", lambda i: ref.mul(vf[1][i], i_reg))
    F(vf, 4, "xy", lambda i: ref.add(vf[2][i], q_reg))
    acc = [ref.madd(acc_before[i], vf[1][i], i_reg) for i in range(4)]
    F(vf, 5, "zw", lambda i: ref.msub(acc[i], vf[2][i], q_reg))
    F(vf, 6, "xzw", lambda i: vf[1][i])
    F(vf, 7, "xyzw", lambda i: vf[2][(i + 1) & 3])
    F(vf, 8, "y", lambda i: vf[8][(i + 1) & 3])
    F(vf, 9, "xyz", lambda i: vf[1][i] & 0x7FFFFFFF)
    acc = [ref.msub(acc[i], vf[2][i], i_reg) for i in range(4)]
    F(vf, 1, "xyzw", lambda i: ref.madd(acc[i], vf[3][i], vf[4][i]))
    acc = [ref.mul(vf[3][i], q_reg) for i in range(4)]
    F(vf, 2, "xyzw", lambda i: ref.sub(vf[5][i], i_reg))
    head = struct.pack("<qq", struct.unpack("<i", struct.pack("<I", i_reg))[0],
                       struct.unpack("<i", struct.pack("<I", q_reg))[0])
    return head + b"".join(pack(*vf[r]) for r in (3, 4, 5, 6, 7, 8, 9, 1, 2)), acc


def sx32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def exp_xfer(inp):
    o = bytearray(OUT_SZ)
    t0 = inp[16:24] + inp[8:16]
    o[0:16] = t0
    t1 = struct.pack("<q", sx32(struct.unpack_from("<I", inp, 48)[0])) + inp[40:48]
    o[16:32] = t1
    o[48:64] = pack(0, 0, 0, ONE)
    o[64:80] = pack(0, 0, 0, ONE)
    o[80:96] = pack(0, 0, 0, ONE)
    v5 = struct.unpack_from("<I", inp, 64)[0]
    o[96:112] = struct.pack("<Q", v5 & 0xFFFF) + inp[88:96]
    r = struct.unpack_from("<I", inp, 68)[0]
    struct.pack_into("<Q", o, 112, ((r & 0x7FFFFF) | 0x3F800000) & 0x7FFFFF)
    fb = struct.unpack_from("<I", inp, 72)[0]
    struct.pack_into("<Q", o, 120, fb & 0x0C0C)
    struct.pack_into("<Q", o, 128, 0)
    i_reg = struct.unpack_from("<I", inp, 76)[0]
    struct.pack_into("<q", o, 136, sx32(i_reg))
    struct.pack_into("<q", o, 144, 0)                      # VPU-STAT read-only: still 0
    struct.pack_into("<q", o, 152, sx32(i_reg))            # CMSAR0: 32 bits
    o[160:176] = inp[96:112]
    struct.pack_into("<q", o, 176, sx32(i_reg & 0xFC0))    # status: only the sticky bits are writable
    return bytes(o)


def exp_rreg(inp):
    vf = vf_init(inp, 1, 1)
    vf[3] = q(inp, 1)
    r = ONE | (vf[1][1] & 0x7FFFFF)
    vf[2] = [r] * 4
    r = ref.rnext(r)
    vf[3][0] = vf[3][2] = r
    r = ref.rnext(r)
    vf[3][3] = r
    r = ONE | ((r ^ vf[1][2]) & 0x7FFFFF)
    r = ref.rnext(r)
    vf[4] = [r] * 4
    r = ref.rnext(ref.rnext(r))                            # vrnext.x vf0 advances too, vrnext.y vf5
    return struct.pack("<Q", r & 0x7FFFFF) + bytes(8) + b"".join(pack(*vf[k]) for k in (2, 3, 4))


def mmi_ref(inp):
    t0, t1, t2 = (struct.unpack_from("<8H", inp, 16 * k) for k in range(3))
    b0, b1, b2 = (inp[16 * k:16 * k + 16] for k in range(3))
    s = lambda v: v - 0x10000 if v & 0x8000 else v      # noqa: E731
    h = lambda xs: struct.pack("<8H", *[x & 0xFFFF for x in xs])   # noqa: E731
    o = h([a + b for a, b in zip(t0, t1)])
    o += h([0xFFFF if s(a) > s(b) else 0 for a, b in zip(t0, t2)])
    o += h([a if s(a) > s(b) else b for a, b in zip(t1, t2)])
    o += h([a if s(a) < s(b) else b for a, b in zip(t1, t2)])
    o += bytes(b1[2 * i] for i in range(8)) + bytes(b0[2 * i] for i in range(8))
    o += bytes(x for i in range(8) for x in (b2[8 + i], b1[8 + i]))
    o += h([a << 5 for a in t0])
    o += h([a >> 15 for a in t1])
    o += h([s(a) >> 3 for a in t2])
    o += h(list(t2))
    return o


# ---------------------------------------------------------------- harness
HARNESS = r"""
#include <fenv.h>
#include <setjmp.h>
#include <stdio.h>
#include <xmmintrin.h>
#include "r5900_rt.h"
#include "unit_rc.h"
#include "unit_vec.h"
static jmp_buf jb;
static const char *fwhat;
static uint32_t fpc, fval;
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t val)
{ (void)c; fwhat = what; fpc = pc; fval = val; longjmp(jb, 1); }
static uint8_t ram[0x10000];
static R5900Ctx c;
static void hook(R5900Ctx *x, uint32_t pc, uint32_t addr)
{ x->vu0.vi[1] = 0x8000u | addr; x->vu0.vf[1].w[0] = pc; x->vu0.vf[1].w[3] ^= 0xFFFFFFFFu; }
static int polls;
static int cond(R5900Ctx *x) { (void)x; return ++polls % 4 == 0; }
static void dump(const char *tag, int k, const uint8_t *p, int n)
{
    printf("%s.%d ", tag, k);
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}
static void run(const char *tag, int k, void (*fn)(R5900Ctx *), const uint8_t *in)
{
    memcpy(ram + 0x4000, in, 256); memset(ram + 0x5000, 0, 256);
    c.r[4].d[0] = 0x4000; c.r[5].d[0] = 0x5000; c.r[29].d[0] = 0x8000;
    if (!setjmp(jb)) { fn(&c); dump(tag, k, ram + 0x5000, 256); }
    else printf("%s.%d fatal=%s pc=%x val=%x\n", tag, k, fwhat, fpc, fval);
}
int main(void)
{
    fesetround(FE_TOWARDZERO);
    _mm_setcsr(_mm_getcsr() | 0x8040);                  /* FTZ | DAZ, like the PoC harnesses */
    memset(&c, 0, sizeof c); c.ram = ram; c.ram_mask = sizeof ram - 1;
    for (int k = 0; k < NVEC; k++) {
        run("matvec", k, fn_00001000, vec[k]); run("fmac", k, fn_00001100, vec[k]);
        run("acc", k, fn_00001200, vec[k]);
        dump("accin", k, (const uint8_t *)c.vu0.acc.w, 16);
        run("iq", k, fn_00001300, vec[k]);
        dump("accout", k, (const uint8_t *)c.vu0.acc.w, 16);
        run("xfer", k, fn_00001400, vec[k]); run("rreg", k, fn_00001500, vec[k]);
        run("mmi", k, fn_00001800, vec[k]); run("matvec3", k, fn_00001900, vec[k]);
    }
    rt_vu0_callms_hook = 0; run("callms_nohook", 0, fn_00001600, vec[0]);
    rt_vu0_callms_hook = hook; run("callms", 0, fn_00001600, vec[0]);
    rt_cpcond0_hook = cond; polls = 0; run("bc0", 0, fn_00001700, vec[0]);
    rt_cpcond0_hook = 0; run("bc0_default", 0, fn_00001700, vec[0]);
    return 0;
}
"""


def _wsl_path(p):
    return config.posix_path(p)


def compile_run(out, src, header, vecs):
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "unit_rc.c"), "w", newline="\n", encoding="ascii").write(src)
    open(os.path.join(out, "unit_rc.h"), "w", newline="\n", encoding="ascii").write(header)
    rows = ",\n".join("{%s}" % ",".join(str(b) for b in v) for v in vecs)
    open(os.path.join(out, "unit_vec.h"), "w", newline="\n", encoding="ascii").write(
        "#define NVEC %d\nstatic const uint8_t vec[NVEC][256] = {\n%s};\n" % (len(vecs), rows))
    open(os.path.join(out, "unit_main.c"), "w", newline="\n", encoding="ascii").write(HARNESS)
    script = os.path.join(out, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "set -e\ncd %s\ngcc -O2 -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-function "
        "-Wno-unused-but-set-variable -frounding-math -ffp-contract=off -I%s -I. unit_main.c unit_rc.c %s -lm -o unit && ./unit\n" % (
            _wsl_path(out), _wsl_path(os.path.join(ROOT, "runtime")),
            _wsl_path(os.path.join(ROOT, "runtime", "r5900_rt.c"))))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script), env=env, capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-3000:]
    return dict(line.split(" ", 1) for line in p.stdout.decode().splitlines() if line.strip())


def expected(vecs, got):
    """{tag: expected text}; the I/Q test continues from the ACC the harness printed before it (accin)."""
    exp = {}
    for k, inp in enumerate(vecs):
        for tag, fn in (("matvec", exp_matvec), ("fmac", exp_fmac), ("acc", exp_acc), ("xfer", exp_xfer),
                        ("rreg", exp_rreg), ("mmi", mmi_ref), ("matvec3", exp_matvec3)):
            out = fn(inp)
            exp["%s.%d" % (tag, k)] = (out + bytes(OUT_SZ - len(out))).hex()
        acc_before = list(struct.unpack("<4I", bytes.fromhex(got["accin.%d" % k])))
        out, acc_after = exp_iq_full(inp, acc_before)
        exp["iq.%d" % k] = (out + bytes(OUT_SZ - len(out))).hex()
        exp["accout.%d" % k] = pack(*acc_after).hex()
    return exp


def exp_matvec3(inp):
    vf = vf_init(inp, 1, 13)
    acc = [None] * 4
    for d, rows, v in ((6, (2, 3, 4), 1), (9, (5, 6, 7), 8)):
        for i in range(3):
            a = ref.mul(vf[rows[0]][i], vf[v][0])
            a = ref.madd(a, vf[rows[1]][i], vf[v][1])
            vf[d][i] = ref.madd(a, vf[rows[2]][i], vf[v][2])
    vf[9][3] = vf[13][3]
    acc = [ref.mul(vf[10][i], vf[11][0]) for i in range(4)]
    acc = [ref.madd(acc[i], vf[11][i], vf[12][1]) for i in range(4)]
    acc = [ref.madd(acc[i], vf[12][i], vf[13][2]) for i in range(4)]
    vf[12] = [ref.madd(acc[i], vf[13][i], ONE) for i in range(4)]
    return b"".join(pack(*vf[r]) for r in (6, 9, 12))


def special_expectations(vecs):
    inp = vecs[0]
    callms = bytearray(OUT_SZ)
    vf1 = list(struct.unpack_from("<4I", inp, 0))
    cms = struct.unpack_from("<I", inp, 16)[0]
    struct.pack_into("<Q", callms, 0, 0x8000 | 0x68)
    struct.pack_into("<4I", callms, 16, F_CALLMS + 12, vf1[1], vf1[2], vf1[3] ^ 0xFFFFFFFF)
    struct.pack_into("<Q", callms, 32, (0x8000 | ((cms & 0xFFFF) * 8)) & 0xFFFF)   # CFC2 VI01: 16 bits
    bc0 = bytearray(OUT_SZ)
    struct.pack_into("<3Q", bc0, 0, 4, 4, 16)          # hook true every 4th poll: 4 + 4 polls, then bc0tl not taken
    bc0d = bytearray(OUT_SZ)
    struct.pack_into("<3Q", bc0d, 0, 1, 1, 1)           # default CPCOND0 = 1: loops exit at once, bc0tl taken
    return {"callms_nohook.0": "fatal=vcallms pc=%x val=68" % (F_CALLMS + 12), "callms.0": callms.hex(),
            "bc0.0": bc0.hex(), "bc0_default.0": bc0d.hex()}


# ---------------------------------------------------------------- tests
def test_cop2_encoder_matches_the_decoder():
    """Every COP2 word of the test programs decodes back to the instruction the encoder was asked for."""
    cases = [(("vmulax.xyz", "ACC", 2, 1), "vmulax.xyz\t$ACCxyz,$vf2xyz,$vf1x"),
             (("vmadday.xyz", "ACC", 3, 1), "vmadday.xyz\t$ACCxyz,$vf3xyz,$vf1y"),
             (("vmaddw.xyz", 6, 5, 0), "vmaddw.xyz\t$vf6xyz,$vf5xyz,$vf0w"),
             (("vsub.xyz", 4, 4, 4), "vsub.xyz\t$vf4xyz,$vf4xyz,$vf4xyz"),
             (("vmove.w", 9, 13), "vmove.w\t$vf9w,$vf13w"),
             (("vrinit", 1, "x"), "vrinit\t$R,$vf1x"), (("vrnext.x", 1), "vrnext.x\t$vf1x,$R"),
             (("vrget.x", 1), "vrget.x\t$vf1x,$R"), (("vcallms", 0x3a0), "vcallms\t0x3a0"),
             (("qmtc2", 10, 1), "qmtc2\tt2,$vf1"), (("cfc2.i", 2, 1), "cfc2.i\tv0,$vi1"),
             (("lqc2", 2, 16, 5), "lqc2\t$vf2,16(a1)"), (("sqc2", 6, -16, 4), "sqc2\t$vf6,-16(a0)"),
             (("vmuli.xyzw", 3, 1), "vmuli.xyzw\t$vf3xyzw,$vf1xyzw,$I"),
             (("vmaddaq.xy", "ACC", 3), "vmaddaq.xy\t$ACCxy,$vf3xy,$Q")]
    for args, text in cases:
        assert decode(asm(*args), 0).text == text, (args, decode(asm(*args), 0).text)
    for a, words in PROGRAM.items():
        for k, w in enumerate(words):
            assert decode(w, a + 4 * k).valid, (hex(a + 4 * k), hex(w))
    # encodings of the matrix x vector idiom, checked against objdump
    assert asm("vmulax.xyz", "ACC", 2, 1) == 0x4BC111BC and asm("vmaddw.xyz", 6, 5, 0) == 0x4BC0298B
    assert asm("qmtc2", 10, 1) == 0x48AA0800 and asm("qmfc2", 8, 6) == 0x48283000


def test_reference_model_basics():
    assert ref.mul(0x7F7FFFFF, 0x40000000) == 0x7F7FFFFF                 # overflow -> FLT_MAX (round toward zero)
    assert ref.mul(0x00800000, 0x3F000000) == 0                          # underflow -> 0
    assert ref.mul(0x80800000, 0x3F000000) == 0x80000000                 # ... with the sign
    assert ref.add(0x3F800000, 0xBF800000) == 0                          # x - x = +0
    assert ref.add(0x80000000, 0x80000000) == 0x80000000
    assert ref.mul(0x7F800000, 0x3F800000) == 0x7F7FFFFF                 # exponent 255 operand = FLT_MAX
    assert ref.mul(0x7FC00000, 0) == 0 and ref.mul(0xFFC00000, 0x3F800000) == 0xFF7FFFFF
    assert ref.add(0x3F800000, 0x33800000) == 0x3F800000                 # 1 + 2^-24 truncates
    assert ref.add(0x3F800001, 0xB3800000) == 0x3F800000                 # (1 + ulp) - 2^-24 truncates
    assert ref.rnext(0x3F800001) == 0x3F800002


@pytest.mark.wsl
@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4"])
def test_vu0_macro_matches_the_reference_model(variant):
    img = image()
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    assert set(SEEDS) <= set(fs.functions)
    opts = {}
    if variant != "v0":
        opts = {"cache": True, "fpu_fast": variant in ("v3", "v4"), "abi": variant == "v4"}
    if variant in ("v2", "v3", "v4"):
        opts["narrow"] = narrowing_plan(img, fs.functions, abi=variant == "v4", roots=set(SEEDS))
    if variant == "v4":
        opts["livein"] = argument_livein(img, fs.functions)
    src, header = emit_program(img, fs, roots=SEEDS, **opts)
    assert "unsupported" not in src
    vecs = vectors(64, 20261003)
    got = compile_run(os.path.join(OUT, variant), src, header, vecs)
    exp = expected(vecs, got)
    exp.update(special_expectations(vecs))
    bad = {k: (got.get(k), v) for k, v in exp.items() if got.get(k) != v}
    assert not bad, "%d mismatches, first: %s" % (len(bad), sorted(bad.items())[:3])
