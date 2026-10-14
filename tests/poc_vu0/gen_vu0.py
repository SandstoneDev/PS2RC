"""Generate the VU0 oracle test program (R5900 test functions + driver data) and its input vectors.

Convention (as PoC-1): a0 = input block (256 bytes), a1 = output block (128 bytes); only t/v/a registers are used.
COP2 instructions are emitted as .word (tests/poc_vu0/vu0enc.py). Every test writes the VU0 registers it reads
(sources by LQC2/QMTC2/CTC2, destination lanes outside the dest mask preloaded), so each output depends only on its
own input vector. ACC is read out with `vmsubx.xyzw vfN, vf0, vf0x` (ACC - (+0) = ACC exactly, -0 included).

Vector classes per float test: NORD "ordinary" vectors (no lane with exponent 255) then NEDGE "edge" vectors (some
lanes with exponent 255, i.e. Inf/NaN bit patterns - ordinary numbers on the VU, clamped by PCSX2). Raw tests (moves,
control registers, MMI) get NORD + NEDGE random vectors.

Outputs (build/poc_vu0/, or PS2RC_VU0_OUT): vu0_tests.S, vu0_data.S, vectors.bin, tests.json.
More tests: PS2RC_VU0_EXTRA = files (os.pathsep-separated) run with this module's helpers (t, W, lqc2, sqc2, loads,
stores, acc_out) after the tests below - e.g. a game pack's own idioms; give them their own PS2RC_VU0_OUT.
"""
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vu0enc import word  # noqa: E402

OUT = os.environ.get("PS2RC_VU0_OUT") or os.path.join(HERE, "..", "..", "build", "poc_vu0")
NORD, NEDGE = 1024, 512
IN_SZ, OUT_SZ = 256, 128
A0, A1 = 4, 5


def lqc2(vf, off, base=A0):
    return word("lqc2", vf, off, base)


def sqc2(vf, off, base=A1):
    return word("sqc2", vf, off, base)


def loads(first, count, off=0):
    return [lqc2(first + k, off + 16 * k) for k in range(count)]


def stores(regs, off=0):
    return [sqc2(r, off + 16 * k) for k, r in enumerate(regs)]


def acc_out(vf, comps="xyzw"):
    """ACC lanes `comps` -> vf (ACC - (+0) * ... = ACC exactly)."""
    return word("vmsubx." + comps, vf, 0, 0)


W = word
TESTS = []


def t(name, kind, lines, ref="both"):
    """kind: 'fl' (float lanes, ordinary + edge vectors) or 'raw' (random bytes); ref: oracle runs the host must
    match: 'both' (interp everywhere, rec on ordinary vectors) or 'rec' (a documented interpreter quirk)."""
    TESTS.append((name, kind, lines, ref))


# ---------------------------------------------------------------- transfers and control registers
t("xfer_q", "raw", loads(1, 2) + stores([1, 2]) + [
    W("qmfc2", 8, 1), "sq $8,32($5)", "lq $9,32($4)", W("qmtc2", 9, 3), sqc2(3, 48), W("qmtc2.i", 9, 4),
    W("qmfc2.i", 10, 4), "sq $10,64($5)", sqc2(0, 80), W("qmfc2", 11, 0), "sq $11,96($5)", W("qmtc2", 0, 5),
    sqc2(5, 112)])
t("xfer_gpr64", "raw", [
    "lq $8,0($4)", "ld $8,16($4)", W("qmtc2", 8, 1), sqc2(1, 0),
    "lq $9,32($4)", "lw $9,48($4)", W("qmtc2", 9, 2), sqc2(2, 16),
    "lq $10,64($4)", "daddiu $10,$10,1", W("qmtc2", 10, 3), sqc2(3, 32),
    "lq $11,80($4)", lqc2(4, 96), W("qmfc2", 11, 4), "daddiu $11,$11,3", "sq $11,48($5)",
    "lq $12,112($4)", "lw $13,128($4)", W("ctc2", 13, 3), W("cfc2", 12, 3), "sq $12,64($5)",
    "lq $14,144($4)", W("cfc2.i", 14, 3), "sq $14,80($5)",
    "lq $15,160($4)", lqc2(5, 176), W("qmfc2", 15, 5), "sw $15,96($5)", "prot3w $24,$15", "sw $24,100($5)",
    "prot3w $15,$24", "sw $15,104($5)", "sq $24,112($5)"], ref="rec")
t("ctrl", "raw", [
    "lw $8,0($4)", W("ctc2", 8, 1), W("cfc2", 9, 1), "sd $9,0($5)",
    "lw $8,4($4)", W("ctc2", 8, 15), W("cfc2", 9, 15), "sd $9,8($5)",
    "lw $8,8($4)", W("ctc2", 8, 20), W("cfc2", 9, 20), "sd $9,16($5)",
    "lw $8,12($4)", W("ctc2", 8, 21), W("cfc2", 9, 21), "sd $9,24($5)",
    "lw $8,16($4)", W("ctc2", 8, 22), W("cfc2", 9, 22), "sd $9,32($5)",
    "lw $8,20($4)", W("ctc2", 8, 23), W("cfc2", 9, 23), "sd $9,40($5)",
    "lw $8,24($4)", W("ctc2", 8, 27), W("cfc2", 9, 27), "sd $9,48($5)",
    "lw $8,28($4)", "andi $8,$8,0x0e0e", W("ctc2", 8, 28), W("cfc2", 9, 28), "sd $9,56($5)",
    W("cfc2", 9, 29), "sd $9,64($5)",
    W("ctc2", 8, 0), W("cfc2", 9, 0), "sd $9,72($5)",
    "lw $8,32($4)", W("ctc2", 8, 18), W("cfc2", 9, 18), "sd $9,80($5)",
    "lw $8,36($4)", W("ctc2.i", 8, 7), W("cfc2.i", 9, 7), "sd $9,88($5)",
    W("cfc2", 9, 1), "sd $9,96($5)",
    "lq $10,48($4)", W("cfc2", 10, 15), "sq $10,112($5)"], ref="rec")
t("rreg", "fl", loads(1, 1) + [lqc2(3, 16), lqc2(5, 32),
    W("vrinit", 1, "y"), W("vrget.xyzw", 2), W("vrnext.xz", 3), W("vrnext.w", 3), W("vrxor", 1, "z"),
    W("vrnext.xyzw", 4), W("vrnext.y", 5), W("cfc2", 8, 20), "sd $8,0($5)"] + stores([2, 3, 4, 5], 16) + [
    W("vrinit", 1, "w"), W("vrnext.x", 6), W("vrnext.y", 6), W("vrnext.z", 6), W("vrnext.w", 6), sqc2(6, 80),
    W("vrxor", 2, "x"), W("vrget.yz", 6), sqc2(6, 96), W("cfc2", 8, 20), "sd $8,112($5)"])
t("rnext_vf0", "fl", loads(1, 1) + [W("vrinit", 1, "x"), W("vrnext.x", 0), W("vrnext.xyzw", 2), sqc2(2, 0),
                                    W("cfc2", 8, 20), "sd $8,16($5)"], ref="rec")

# ---------------------------------------------------------------- FMAC: Fd forms
for op in ("vadd", "vsub", "vmul"):
    t(op[1:] + "_fd", "fl", loads(1, 8) + [
        W(op + "x.xyzw", 5, 1, 2), W(op + "y.xyz", 6, 2, 3), W(op + "z.xz", 7, 3, 4), W(op + "w.w", 8, 4, 1),
        W(op + ".xyzw", 2, 1, 2), W(op + ".yz", 1, 1, 3), W(op + ".xyzw", 3, 3, 3)] + stores([5, 6, 7, 8, 2, 1, 3]))
for op in ("vmadd", "vmsub"):
    t(op[1:] + "_fd", "fl", loads(1, 8) + [
        W("vmula.xyzw", "ACC", 1, 2), W(op + "x.xyzw", 5, 3, 4), W(op + "y.xyz", 6, 4, 3),
        W("vmula.xyzw", "ACC", 2, 3), W(op + "z.xw", 7, 1, 2), W(op + "w.y", 8, 2, 4), W(op + ".xyzw", 4, 1, 4),
        W(op + ".zw", 1, 1, 1), acc_out(9)] + stores([5, 6, 7, 8, 4, 1, 9]))
t("acc_bc", "fl", loads(1, 6) + [
    W("vaddax.xyzw", "ACC", 1, 2), acc_out(10), sqc2(10, 0),
    W("vsubay.xyz", "ACC", 3, 4), acc_out(10), sqc2(10, 16),
    W("vmulaz.xw", "ACC", 5, 6), acc_out(10), sqc2(10, 32),
    W("vmaddaw.xyzw", "ACC", 1, 3), acc_out(10), sqc2(10, 48),
    W("vmsubax.yz", "ACC", 2, 4), acc_out(10), sqc2(10, 64),
    W("vmulaw.xyzw", "ACC", 6, 1), W("vmaddax.xyzw", "ACC", 2, 5), W("vmsubay.xyzw", "ACC", 3, 4), acc_out(10),
    sqc2(10, 80), W("vsubaz.xyzw", "ACC", 4, 6), W("vaddaw.xz", "ACC", 5, 3), acc_out(10), sqc2(10, 96)])
t("acc3", "fl", loads(1, 6) + [
    W("vadda.xyzw", "ACC", 1, 2), acc_out(10), sqc2(10, 0),
    W("vsuba.xyz", "ACC", 3, 4), acc_out(10), sqc2(10, 16),
    W("vmula.xzw", "ACC", 5, 6), acc_out(10), sqc2(10, 32),
    W("vmadda.xyzw", "ACC", 1, 3), acc_out(10), sqc2(10, 48),
    W("vmsuba.xyw", "ACC", 2, 4), acc_out(10), sqc2(10, 64),
    W("vmula.xyzw", "ACC", 6, 6), W("vmadda.xyzw", "ACC", 5, 5), W("vmsuba.xyzw", "ACC", 4, 4), acc_out(10),
    sqc2(10, 80)])
t("iq", "fl", ["lw $8,0($4)", W("ctc2", 8, 21), "lw $9,4($4)", W("ctc2", 9, 22)] + loads(1, 6, 16) + [
    lqc2(8, 112), lqc2(9, 128), lqc2(11, 144), lqc2(14, 160),
    W("vmuli.xyzw", 7, 1), W("vaddq.xyz", 8, 2), W("vsubi.xw", 9, 3), W("vmulaq.xyzw", "ACC", 4),
    W("vmaddi.xyzw", 10, 5), W("vmsubq.yz", 11, 6), W("vaddai.xyzw", "ACC", 1), W("vmsubai.xyzw", "ACC", 2),
    acc_out(12), W("vsubaq.xy", "ACC", 3), W("vmaddaq.zw", "ACC", 4), W("vmulai.y", "ACC", 5), acc_out(13),
    W("vaddi.yzw", 14, 6)] + stores([7, 8, 9, 10, 11, 12, 13, 14]))
t("iq2", "fl", ["lw $8,0($4)", W("ctc2", 8, 21), "lw $9,4($4)", W("ctc2", 9, 22)] + loads(1, 6, 16) + [
    W("vmulaq.xyzw", "ACC", 6), W("vsubq.xyzw", 1, 1), W("vmulq.xyzw", 2, 2), W("vmaddq.xyzw", 3, 3),
    W("vmsubi.xyzw", 4, 4), W("vaddq.xyzw", 5, 5), W("vaddaq.xyzw", "ACC", 1), W("vsubai.xyz", "ACC", 2),
    W("vmsubaq.yw", "ACC", 3), acc_out(6)] + stores([1, 2, 3, 4, 5, 6]))
t("move", "fl", loads(1, 6) + [
    W("vmove.xyzw", 3, 1), W("vmove.w", 4, 2), W("vmove.xz", 5, 1), W("vmr32.xyzw", 6, 2), W("vmr32.y", 1, 1),
    W("vabs.xyzw", 7, 2), W("vabs.xz", 2, 2), W("vmr32.xyzw", 8, 0), W("vmove.xyzw", 9, 0), W("vnop"),
    W("vwaitq")] + stores([3, 4, 5, 6, 1, 7, 2, 8]))


# ---------------------------------------------------------------- dependency chains (clean-lane tracking, v3+)
t("chain", "fl", loads(1, 3) + [
    W("vmul.xyzw", 4, 1, 2), W("vadd.xyzw", 5, 4, 3), W("vmulax.xyzw", "ACC", 5, 4),
    W("vmadday.xyzw", "ACC", 4, 5), W("vmsubz.xyzw", 6, 5, 4), W("vsubw.xyzw", 7, 6, 5), W("vmul.xyzw", 8, 7, 7),
    W("vmaddaw.xyzw", "ACC", 8, 6), W("vmsubx.xyzw", 9, 8, 1), W("vmove.xyzw", 10, 9), W("vmr32.xyzw", 11, 10),
    W("vabs.xyzw", 12, 11), W("vadd.xyzw", 13, 12, 1)] + stores([5, 6, 7, 8, 9, 11, 12, 13]))
t("chain_gpr", "fl", ["lq $8,0($4)", W("qmtc2", 8, 1), "lq $9,16($4)", W("qmtc2", 9, 2),
                      W("vmul.xyzw", 3, 1, 2), W("qmfc2", 10, 3), "paddw $11,$10,$8", "psrlw $11,$11,2",
                      W("qmtc2", 11, 4),
                      W("vmul.xyzw", 5, 4, 3), W("vmula.xyzw", "ACC", 5, 4), W("vmaddw.xyzw", 6, 3, 0),
                      lqc2(7, 32), W("vmove.xz", 6, 7), W("vadd.xyzw", 8, 6, 6), "sq $11,0($5)"] +
    stores([3, 4, 5, 6, 8], 16))

# ---------------------------------------------------------------- MMI halfword instructions (as libipu uses them)
t("mmi_h1", "raw", ["lq $8,0($4)", "lq $9,16($4)", "lq $10,32($4)",
                    "paddh $11,$8,$9", "sq $11,0($5)", "pcgth $11,$8,$10", "sq $11,16($5)",
                    "pmaxh $11,$9,$10", "sq $11,32($5)", "pminh $11,$9,$10", "sq $11,48($5)",
                    "pcgth $11,$9,$9", "sq $11,64($5)", "paddh $11,$10,$10", "sq $11,80($5)",
                    "pmaxh $11,$8,$8", "sq $11,96($5)", "pminh $11,$10,$8", "sq $11,112($5)"])
t("mmi_h2", "raw", ["lq $8,0($4)", "lq $9,16($4)", "lq $10,32($4)",
                    "ppacb $11,$8,$9", "sq $11,0($5)", "pextub $11,$9,$10", "sq $11,16($5)",
                    "psllh $11,$8,5", "sq $11,32($5)", "psrlh $11,$9,15", "sq $11,48($5)",
                    "psrah $11,$10,3", "sq $11,64($5)", "psrlh $11,$10,0", "sq $11,80($5)",
                    "psrah $11,$8,15", "sq $11,96($5)", "psllh $11,$9,12", "sq $11,112($5)"])

# ---------------------------------------------------------------- COP0 condition (DMA idle)
t("bc0", "raw", ["move $8,$0", "bc0t 1f", "nop", "addiu $8,$8,1", "1: bc0f 2f", "nop", "addiu $8,$8,2",
                 "2: move $9,$0", "bc0tl 3f", "addiu $9,$9,4", "addiu $9,$9,8", "3: sd $8,0($5)", "sd $9,8($5)"])


# ---------------------------------------------------------------- vectors
def rand_lane(rnd, edge):
    r = rnd.random()
    if edge and r < 0.15:                                    # exponent 255: Inf / NaN bit patterns
        return rnd.getrandbits(1) << 31 | 255 << 23 | (rnd.getrandbits(23) if rnd.random() < 0.6 else 0)
    r = rnd.random()
    if r < 0.32:
        return struct.unpack("<I", struct.pack("<f", rnd.uniform(-1e4, 1e4)))[0]
    if r < 0.42:
        return struct.unpack("<I", struct.pack("<f", float(rnd.randint(-16, 16)) / rnd.choice((1, 2, 4))))[0]
    if r < 0.52:                                             # zeros and denormals
        return rnd.getrandbits(1) << 31 | (rnd.getrandbits(23) if rnd.random() < 0.7 else 0)
    if r < 0.62:                                             # huge: products overflow
        return rnd.getrandbits(1) << 31 | rnd.randint(190, 254) << 23 | rnd.getrandbits(23)
    if r < 0.70:                                             # tiny: products underflow
        return rnd.getrandbits(1) << 31 | rnd.randint(1, 50) << 23 | rnd.getrandbits(23)
    if r < 0.85:                                             # full mantissa, moderate exponent: rounding
        return rnd.getrandbits(1) << 31 | rnd.randint(100, 154) << 23 | rnd.getrandbits(23)
    while True:                                              # anything but exponent 255
        v = rnd.getrandbits(32)
        if (v >> 23) & 0xFF != 255:
            return v


def float_vector(rnd, edge):
    lanes = [rand_lane(rnd, edge) for _ in range(IN_SZ // 4)]
    mode = rnd.random()
    if mode < 0.15:                                          # exact cancellation between quadwords
        for j in range(16, len(lanes)):
            if rnd.random() < 0.5:
                lanes[j] = lanes[j - 16] ^ 0x80000000
    elif mode < 0.30:                                        # near cancellation (differ in the last bits)
        for j in range(16, len(lanes)):
            if rnd.random() < 0.5:
                lanes[j] = ((lanes[j - 16] ^ 0x80000000) + rnd.randint(-3, 3)) & 0xFFFFFFFF
    if not edge:                                             # the +-3 above can wrap into exponent 255
        lanes = [v & ~0x00800000 if (v >> 23) & 0xFF == 255 else v for v in lanes]
    return struct.pack("<%dI" % len(lanes), *lanes)


for _extra in filter(None, os.environ.get("PS2RC_VU0_EXTRA", "").split(os.pathsep)):   # see the docstring
    exec(compile(open(_extra, encoding="utf-8").read(), _extra, "exec"), dict(globals()))


def vector(kind, rnd, edge):
    if kind == "fl":
        return float_vector(rnd, edge)
    return bytes(rnd.getrandbits(8) for _ in range(IN_SZ))


def main():
    os.makedirs(OUT, exist_ok=True)
    rnd = random.Random(20261003)
    lines = ["# generated by tests/poc_vu0/gen_vu0.py - do not edit", "\t.set noreorder", "\t.set noat", "\t.text"]
    meta, blob = [], bytearray()
    nvec = NORD + NEDGE
    for name, kind, body, ref in TESTS:
        lines += ["\t.globl t_%s" % name, "\t.ent t_%s" % name, "t_%s:" % name]
        lines += [("" if ln.endswith(":") or ln[:2] in ("1:", "2:", "3:") else "\t") + ln for ln in body]
        lines += ["\tjr $31", "\tnop", "\t.end t_%s" % name, ""]
        meta.append({"name": name, "kind": kind, "ref": ref, "nvec": nvec, "nord": NORD, "in_off": len(blob)})
        for k in range(nvec):
            blob += vector(kind, rnd, k >= NORD)
    open(os.path.join(OUT, "vu0_tests.S"), "w", newline="\n", encoding="ascii").write("\n".join(lines) + "\n")
    open(os.path.join(OUT, "vectors.bin"), "wb").write(blob)
    json.dump({"in_size": IN_SZ, "out_size": OUT_SZ, "tests": meta},
              open(os.path.join(OUT, "tests.json"), "w", encoding="utf-8"), indent=1)
    d = ["# generated by tests/poc_vu0/gen_vu0.py", "\t.data", "\t.align 6", "\t.globl inputs", "inputs:",
         '\t.incbin "vectors.bin"', "\t.align 4", "\t.globl test_table", "test_table:"]
    for k, m in enumerate(meta):
        d.append("\t.word t_%s, %d, inputs+%d, outputs+%d" % (m["name"], nvec, m["in_off"], k * nvec * OUT_SZ))
    d += ["\t.globl test_table_end", "test_table_end:", "\t.globl done_flag", "done_flag:", "\t.word 0",
          "\t.bss", "\t.align 6", "\t.globl outputs", "outputs:", "\t.space %d" % (len(meta) * nvec * OUT_SZ),
          "\t.align 4", "\t.space 65536", "\t.globl stack_top", "stack_top:", "\t.space 64"]
    open(os.path.join(OUT, "vu0_data.S"), "w", newline="\n", encoding="ascii").write("\n".join(d) + "\n")
    print("%d tests, %d vectors, %.1f MB inputs" % (len(TESTS), len(TESTS) * nvec, len(blob) / 1e6))


if __name__ == "__main__":
    sys.exit(main())
