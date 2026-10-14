"""Generate the PoC-1 guest program (R5900 test functions + driver data) and its input vectors.

Every test function follows one convention: a0 = input block (64 bytes), a1 = output block (32 bytes). Integer tests
read four 64-bit inputs x,y,z,w (ld 0/8/16/24), MMI tests three 128-bit inputs (lq 0/16/32), FPU tests float bit
patterns (lwc1). Results are stored to the output block (sd / sq / swc1). Only t-registers are used, so the driver's
s-registers survive. Overflow-trapping ADD/ADDI/SUB are not used with data-dependent operands.

Outputs (build/poc1/): poc1_tests.S, tests.json (name, kind, vectors), vectors.bin (all input blocks, test-major).
"""
import json
import os
import random
import struct
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "poc1")
NVEC = 4096
IN_SZ, OUT_SZ = 64, 32

LD4 = "ld $8,0($4)\n ld $9,8($4)\n ld $10,16($4)\n ld $11,24($4)\n"      # t0..t3 = x,y,z,w
LQ3 = "lq $8,0($4)\n lq $9,16($4)\n lq $10,32($4)\n"                       # t0..t2 = 128-bit x,y,z
LF3 = "lwc1 $f1,0($4)\n lwc1 $f2,4($4)\n lwc1 $f3,8($4)\n lwc1 $f4,12($4)\n"


def st4(*regs):
    return "".join("sd $%d,%d($5)\n" % (r, 8 * k) for k, r in enumerate(regs))


def sq2(a, b):
    return "sq $%d,0($5)\n sq $%d,16($5)\n" % (a, b)


def sf4(*fr):
    return "".join("swc1 $f%d,%d($5)\n" % (r, 4 * k) for k, r in enumerate(fr))


TESTS = []


def t(name, kind, body):
    TESTS.append((name, kind, body))


# ---- integer 32-bit ALU (results must be sign-extended 64-bit; inputs carry random upper bits)
t("alu_addsub", "int", LD4 + "addu $12,$8,$9\n subu $13,$8,$9\n addiu $14,$8,1234\n addiu $15,$9,-32768\n" + st4(12, 13, 14, 15))
t("alu_logic", "int", LD4 + "and $12,$8,$9\n or $13,$8,$9\n xor $14,$8,$9\n nor $15,$8,$9\n" + st4(12, 13, 14, 15))
t("alu_imm", "int", LD4 + "andi $12,$8,0x8f0f\n ori $13,$8,0x1234\n xori $14,$9,0xffff\n lui $15,0x8001\n ori $15,$15,0x00ff\n" + st4(12, 13, 14, 15))
t("alu_slt", "int", LD4 + "slt $12,$8,$9\n sltu $13,$8,$9\n slti $14,$8,-5\n sltiu $15,$9,100\n" + st4(12, 13, 14, 15))
t("shift32", "int", LD4 + "sll $12,$8,7\n srl $13,$8,31\n sra $14,$9,13\n sllv $15,$8,$10\n" + st4(12, 13, 14, 15))
t("shift32v", "int", LD4 + "srlv $12,$8,$10\n srav $13,$9,$11\n sll $14,$8,0\n sra $15,$8,0\n" + st4(12, 13, 14, 15))
t("shift64", "int", LD4 + "dsll $12,$8,3\n dsrl $13,$8,9\n dsra $14,$9,17\n dsll32 $15,$9,5\n" + st4(12, 13, 14, 15))
t("shift64b", "int", LD4 + "dsrl32 $12,$8,5\n dsra32 $13,$9,1\n dsllv $14,$8,$10\n dsrav $15,$9,$11\n" + st4(12, 13, 14, 15))
t("alu64", "int", LD4 + "daddu $12,$8,$9\n dsubu $13,$8,$9\n daddiu $14,$8,-7\n dsrlv $15,$8,$11\n" + st4(12, 13, 14, 15))
t("idioms64", "int", LD4 + "daddu $12,$8,$0\n dsll32 $13,$9,0\n dsra32 $13,$13,0\n dsll32 $14,$9,0\n dsrl32 $14,$14,0\n"
  "addi $15,$0,-2\n and $15,$15,$8\n" + st4(12, 13, 14, 15))
t("mult", "int", LD4 + "mult $12,$8,$9\n mfhi $13\n multu $14,$8,$9\n mfhi $15\n" + st4(12, 13, 14, 15))
t("div", "int", LD4 + "div $0,$8,$9\n mflo $12\n mfhi $13\n divu $0,$8,$10\n mflo $14\n mfhi $15\n" + st4(12, 13, 14, 15))
t("madd", "int", LD4 + "mthi $10\n mtlo $11\n madd $12,$8,$9\n mfhi $13\n mtlo $10\n mthi $11\n maddu $14,$8,$9\n mfhi $15\n" + st4(12, 13, 14, 15))
t("pipe1", "int", LD4 + "mult1 $12,$8,$9\n mfhi1 $13\n div1 $0,$10,$11\n mflo1 $14\n mfhi1 $15\n" + st4(12, 13, 14, 15))
t("madd1", "int", LD4 + "mthi1 $10\n mtlo1 $11\n madd1 $12,$8,$9\n mfhi1 $13\n maddu1 $14,$9,$8\n mflo1 $15\n" + st4(12, 13, 14, 15))
t("movzn", "int", LD4 + "move $12,$11\n movz $12,$8,$10\n move $13,$11\n movn $13,$9,$10\n slt $14,$8,$9\n movn $14,$10,$14\n move $15,$0\n movz $15,$8,$15\n" + st4(12, 13, 14, 15))
t("branch", "int", LD4 +
  "move $12,$0\n beq $8,$9,1f\n addiu $12,$12,1\n addiu $12,$12,16\n1: move $13,$0\n bne $8,$9,2f\n addiu $13,$13,1\n addiu $13,$13,16\n"
  "2: move $14,$0\n bltz $8,3f\n addiu $14,$14,1\n addiu $14,$14,16\n3: move $15,$0\n blez $9,4f\n addiu $15,$15,1\n addiu $15,$15,16\n4:" + st4(12, 13, 14, 15))
t("branch_likely", "int", LD4 +
  "move $12,$0\n beql $8,$9,1f\n addiu $12,$12,1\n addiu $12,$12,16\n1: move $13,$0\n bnel $8,$9,2f\n addiu $13,$13,1\n addiu $13,$13,16\n"
  "2: move $14,$0\n bgezl $8,3f\n addiu $14,$14,1\n addiu $14,$14,16\n3: move $15,$0\n bgtzl $9,4f\n addiu $15,$15,1\n addiu $15,$15,16\n4:" + st4(12, 13, 14, 15))
t("unaligned", "int", LD4 + "andi $8,$10,7\n addu $8,$8,$4\n ldl $12,7($8)\n ldr $12,0($8)\n andi $9,$11,3\n addu $9,$9,$4\n lwl $13,3($9)\n lwr $13,0($9)\n"
  "lb $14,5($4)\n lhu $15,6($4)\n" + st4(12, 13, 14, 15))
t("lq_mask", "mmi", "lq $8,7($4)\n lq $9,31($4)\n" + sq2(8, 9))
# ---- MMI (128-bit)
t("mmi_add", "mmi", LQ3 + "paddw $11,$8,$9\n paddub $12,$8,$9\n" + sq2(11, 12))
t("mmi_sub", "mmi", LQ3 + "psubb $11,$8,$9\n padduw $12,$8,$10\n" + sq2(11, 12))
t("mmi_logic", "mmi", LQ3 + "pand $11,$8,$9\n pnor $12,$8,$10\n" + sq2(11, 12))
t("mmi_logic2", "mmi", LQ3 + "por $11,$8,$9\n pxor $12,$9,$10\n" + sq2(11, 12))
t("mmi_copy", "mmi", LQ3 + "pcpyld $11,$8,$9\n pcpyud $12,$8,$9\n" + sq2(11, 12))
t("mmi_ext", "mmi", LQ3 + "pextlw $11,$8,$9\n pextlb $12,$8,$10\n" + sq2(11, 12))
t("mmi_ext2", "mmi", LQ3 + "pextlh $11,$9,$10\n pextuw $12,$8,$9\n" + sq2(11, 12))
t("mmi_pack", "mmi", LQ3 + "ppacw $11,$8,$9\n ppac5 $12,$10\n" + sq2(11, 12))
t("mmi_perm", "mmi", LQ3 + "pexew $11,$8\n prot3w $12,$9\n" + sq2(11, 12))
t("mmi_perm2", "mmi", LQ3 + "pcpyh $11,$8\n pinteh $12,$8,$9\n" + sq2(11, 12))
t("mmi_shift", "mmi", LQ3 + "psllw $11,$8,5\n psrlw $12,$9,7\n" + sq2(11, 12))
t("mmi_cmp", "mmi", LQ3 + "pcgtw $11,$8,$9\n pceqw $12,$8,$10\n" + sq2(11, 12))
t("mmi_minmax", "mmi", LQ3 + "pmaxw $11,$8,$9\n pminw $12,$8,$9\n" + sq2(11, 12))
t("mmi_qfsrv", "mmi", LQ3 + "lw $13,48($4)\n andi $13,$13,15\n mtsab $13,0\n qfsrv $11,$8,$9\n mfsa $14\n pcpyld $12,$14,$14\n" + sq2(11, 12))
t("mmi_plzcw", "mmi", LQ3 + "por $11,$0,$0\n plzcw $11,$8\n psubsw $12,$8,$9\n" + sq2(11, 12))
# ---- FPU (R5900: single precision, no Inf/NaN/denormals, round toward zero, saturate to +-Fmax)
FINIT = "ctc1 $0,$31\n"
t("fpu_add", "fpu", FINIT + LF3 + "add.s $f5,$f1,$f2\n sub.s $f6,$f1,$f2\n mul.s $f7,$f1,$f2\n div.s $f8,$f1,$f2\n" + sf4(5, 6, 7, 8))
# sqrt.s $f5,$f1 in the R5900 encoding (COP1 S-format funct 4, operand in ft = 1, fd = 5). GNU as (-march=r5900) would
# put the operand in fs, which the EE (and PCSX2) read as sqrt($f0) - binutils bug, see tests/test_r5900_decode.py.
SQRT_S_F5_F1 = 17 << 26 | 16 << 21 | 1 << 16 | 5 << 6 | 4
t("fpu_unary", "fpu", FINIT + LF3 + ".word %d\n rsqrt.s" % SQRT_S_F5_F1 + " $f6,$f2,$f1\n abs.s $f7,$f3\n neg.s $f8,$f3\n" + sf4(5, 6, 7, 8))
t("fpu_minmax", "fpu", FINIT + LF3 + "max.s $f5,$f1,$f2\n min.s $f6,$f1,$f2\n mov.s $f7,$f1\n add.s $f8,$f1,$f1\n" + sf4(5, 6, 7, 8))
t("fpu_acc", "fpu", FINIT + LF3 + "mula.s $f1,$f2\n madd.s $f5,$f3,$f4\n adda.s $f1,$f2\n msub.s $f6,$f3,$f4\n"
  "suba.s $f3,$f4\n madda.s $f1,$f2\n madd.s $f7,$f2,$f3\n msuba.s $f2,$f3\n msub.s $f8,$f1,$f1\n" + sf4(5, 6, 7, 8))
t("fpu_cvt", "fpu", FINIT + LF3 + "trunc.w.s $f5,$f1\n cvt.s.w $f6,$f2\n trunc.w.s $f7,$f3\n cvt.s.w $f8,$f4\n" + sf4(5, 6, 7, 8))
t("fpu_cmp", "int", FINIT + LF3 +
  "move $12,$0\n c.lt.s $f1,$f2\n bc1f 1f\n nop\n addiu $12,$12,1\n1: c.le.s $f1,$f2\n bc1f 2f\n nop\n addiu $12,$12,2\n"
  "2: c.eq.s $f1,$f3\n bc1f 3f\n nop\n addiu $12,$12,4\n3: c.f.s $f1,$f2\n bc1t 4f\n nop\n addiu $12,$12,8\n4: cfc1 $13,$31\n"
  "c.lt.s $f2,$f1\n move $14,$0\n bc1tl 5f\n addiu $14,$14,1\n addiu $14,$14,16\n5: mfc1 $15,$f1\n" + st4(12, 13, 14, 15))


# ---------------------------------------------------------------- vectors
EDGE64 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
          0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF80000000, 0x00000000FFFFFFFF, 31, 32, 63]


def rand64(rnd):
    r = rnd.random()
    if r < 0.25:
        return rnd.choice(EDGE64)
    if r < 0.45:
        v = rnd.getrandbits(32)                    # canonical sign-extended 32-bit value
        return v | (0xFFFFFFFF00000000 if v & 0x80000000 else 0)
    if r < 0.55:
        return rnd.getrandbits(8) - 128 & 0xFFFFFFFFFFFFFFFF
    return rnd.getrandbits(64)


def randf(rnd):
    r = rnd.random()
    if r < 0.45:                                   # ordinary floats
        return struct.unpack("<I", struct.pack("<f", rnd.uniform(-1e6, 1e6)))[0]
    if r < 0.55:                                   # small integers as floats
        return struct.unpack("<I", struct.pack("<f", float(rnd.randint(-20, 20))))[0]
    if r < 0.85:                                   # any exponent incl. 0 (denormal/zero) and 255 (no Inf/NaN on EE)
        exp = rnd.choice([0, 1, 2, 100, 126, 127, 128, 200, 252, 253, 254, 255])
        return rnd.getrandbits(1) << 31 | exp << 23 | rnd.getrandbits(23)
    return rnd.getrandbits(32)                     # raw patterns (also used as ints by cvt.s.w)


def vector(kind, rnd):
    if kind in ("int",):
        vals = [rand64(rnd) for _ in range(4)]
        if rnd.random() < 0.15:
            vals[1] = vals[0]                       # make equality branches taken
        if rnd.random() < 0.05:
            vals[1] = 0                             # divide by zero
        return struct.pack("<4Q", *vals) + bytes(32)
    if kind == "mmi":
        return bytes(rnd.getrandbits(8) for _ in range(64))
    if kind == "fpu":
        fs = [randf(rnd) for _ in range(4)]
        if rnd.random() < 0.1:
            fs[1] = fs[0]
        if rnd.random() < 0.05:
            fs[1] = 0
        return struct.pack("<4I", *fs) + bytes(48)
    raise ValueError(kind)


def main():
    os.makedirs(OUT, exist_ok=True)
    rnd = random.Random(20261003)
    lines = ["# generated by tests/poc1/gen_poc1.py - do not edit", "\t.set noreorder", "\t.set noat", "\t.text"]
    meta = []
    blob = bytearray()
    for name, kind, body in TESTS:
        lines += ["\t.globl t_%s" % name, "\t.ent t_%s" % name, "t_%s:" % name]
        lines += ["\t" + ln.strip() for ln in body.strip().split("\n")]
        lines += ["\tjr $31", "\tnop", "\t.end t_%s" % name, ""]
        fn_kind = kind
        # fpu_cmp reads float inputs but writes integer results
        vec_kind = "fpu" if name.startswith("fpu") else kind
        meta.append({"name": name, "kind": fn_kind, "vec_kind": vec_kind, "nvec": NVEC, "in_off": len(blob)})
        for _ in range(NVEC):
            blob += vector(vec_kind, rnd)
    open(os.path.join(OUT, "poc1_tests.S"), "w", newline="\n").write("\n".join(lines) + "\n")
    open(os.path.join(OUT, "vectors.bin"), "wb").write(blob)
    json.dump({"in_size": IN_SZ, "out_size": OUT_SZ, "tests": meta}, open(os.path.join(OUT, "tests.json"), "w"), indent=1)
    d = ["# generated by tests/poc1/gen_poc1.py", "\t.data", "\t.align 6", "\t.globl inputs", "inputs:",
         '\t.incbin "vectors.bin"', "\t.align 4", "\t.globl test_table", "test_table:"]
    for k, m in enumerate(meta):
        d.append("\t.word t_%s, %d, inputs+%d, outputs+%d" % (m["name"], NVEC, m["in_off"], k * NVEC * OUT_SZ))
    d += ["\t.globl test_table_end", "test_table_end:", "\t.globl done_flag", "done_flag:", "\t.word 0",
          "\t.bss", "\t.align 6", "\t.globl outputs", "outputs:", "\t.space %d" % (len(meta) * NVEC * OUT_SZ),
          "\t.align 4", "\t.space 65536", "\t.globl stack_top", "stack_top:", "\t.space 64"]
    open(os.path.join(OUT, "poc1_data.S"), "w", newline="\n").write("\n".join(d) + "\n")
    print("%d tests, %d vectors, %.1f MB inputs" % (len(TESTS), len(TESTS) * NVEC, len(blob) / 1e6))


if __name__ == "__main__":
    sys.exit(main())
