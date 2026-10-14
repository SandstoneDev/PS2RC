# smoke.S - PCSX2 execution-oracle smoke test (PS2 EE / R5900, little endian).
#
# Computes a handful of R5900-specific results (32-bit ops sign-extending into
# 64-bit GPRs, 64-bit shifts/adds, the 3-operand MULT/MULTU and HI/LO, 128-bit
# LQ/SQ and two MMI ops), stores them into `results`, then writes DONE_MAGIC to
# `done` and spins forever.  The host polls `done` through PINE and then reads
# `results` (see src/ps2rc/oracle/).
#
# The expected values are modelled independently in
# tests/oracle/test_pcsx2_smoke.py - never derive them from this file or the ELF.
# The only things the two sides share are the *inputs* below and the slot order.
#
# Every result slot is pre-filled with a sentinel so a store that never happened
# is visible.  `stage` is a progress word for debugging hangs (1 = entered,
# 2 = all results stored).
#
# Build: tests/oracle/smoke/build.sh (WSL, pspdev binutils: psp-as -march=r5900).

	.set	noreorder
	.set	noat

	.globl	DONE_MAGIC
	.equ	DONE_MAGIC, 0x600DF00D

# ---------------------------------------------------------------------------
	.text
	.globl	_start
	.type	_start, @function
	.ent	_start
_start:
	# Private stack; do not rely on whatever $sp the loader left us.
	lui	$sp, %hi(stack_top)
	addiu	$sp, $sp, %lo(stack_top)

	# stage = 1 (entered)
	addiu	$t0, $zero, 1
	lui	$s7, %hi(stage)
	sw	$t0, %lo(stage)($s7)

	# 1. ADDU wraps at 32 bits and sign-extends: 0x7FFFFFFF + 1
	lui	$t0, 0x7FFF
	ori	$t0, $t0, 0xFFFF		# t0 = 0x7FFFFFFF
	addiu	$t1, $zero, 1
	addu	$t2, $t0, $t1
	lui	$s7, %hi(res_addu)
	sd	$t2, %lo(res_addu)($s7)

	# 2. 64-bit: DSLL32 then DADDU with a LUI-sign-extended operand
	lui	$t0, 0x1234
	ori	$t0, $t0, 0x5678		# t0 = 0x12345678
	dsll32	$t0, $t0, 0			# t0 <<= 32
	lui	$t1, 0x9ABC			# LUI sign-extends bit 31
	ori	$t1, $t1, 0xDEF0
	daddu	$t2, $t0, $t1			# t2 kept for MMI test 7
	lui	$s7, %hi(res_daddu)
	sd	$t2, %lo(res_daddu)($s7)

	# 3. Build B = 0xFEDCBA9876543210 and DSRA32 it by 4 (arith. >> 36)
	lui	$t3, 0xFEDC
	ori	$t3, $t3, 0xBA98
	dsll32	$t3, $t3, 0			# t3 = 0xFEDCBA98_00000000
	lui	$t4, 0x7654
	ori	$t4, $t4, 0x3210		# t4 = 0x76543210 (bit 31 clear)
	daddu	$t5, $t3, $t4			# t5 = B, kept for MMI test 7
	dsra32	$t6, $t5, 4
	lui	$s7, %hi(res_dsra32)
	sd	$t6, %lo(res_dsra32)($s7)

	# 4. 3-operand MULT: rd = LO; 0x10000 * 0x8000 = 0x80000000 (LO sign-extends)
	lui	$t0, 0x0001			# 0x00010000
	ori	$t1, $zero, 0x8000		# 0x00008000
	mult	$t7, $t0, $t1
	mflo	$t8
	mfhi	$t9
	lui	$s7, %hi(res_mult_rd)
	sd	$t7, %lo(res_mult_rd)($s7)
	lui	$s7, %hi(res_mult_lo)
	sd	$t8, %lo(res_mult_lo)($s7)
	lui	$s7, %hi(res_mult_hi)
	sd	$t9, %lo(res_mult_hi)($s7)

	# 5. Signed MULT with a negative operand: -7 * 0x12345678
	addiu	$t0, $zero, -7
	lui	$t1, 0x1234
	ori	$t1, $t1, 0x5678
	mult	$t7, $t0, $t1
	mfhi	$t9
	lui	$s7, %hi(res_mult2_rd)
	sd	$t7, %lo(res_mult2_rd)($s7)
	lui	$s7, %hi(res_mult2_hi)
	sd	$t9, %lo(res_mult2_hi)($s7)

	# 6. 3-operand MULTU: 0xFFFFFFFF * 0xFFFFFFFF (HI = 0xFFFFFFFE, sign-extended)
	addiu	$t0, $zero, -1			# low word 0xFFFFFFFF
	addiu	$t1, $zero, -1
	multu	$t7, $t0, $t1
	mfhi	$t9
	lui	$s7, %hi(res_multu_rd)
	sd	$t7, %lo(res_multu_rd)($s7)
	lui	$s7, %hi(res_multu_hi)
	sd	$t9, %lo(res_multu_hi)($s7)

	# 7. MMI PCPYLD: rd.hi64 = rs.lo64 (daddu result), rd.lo64 = rt.lo64 (B); SQ
	pcpyld	$t7, $t2, $t5
	lui	$s7, %hi(res_pcpyld)
	sq	$t7, %lo(res_pcpyld)($s7)

	# 8. LQ two vectors, MMI PADDW (per-word wrap-around add), SQ
	lui	$s7, %hi(vec_a)
	lq	$t0, %lo(vec_a)($s7)
	lui	$s7, %hi(vec_b)
	lq	$t1, %lo(vec_b)($s7)
	paddw	$t7, $t0, $t1
	lui	$s7, %hi(res_paddw)
	sq	$t7, %lo(res_paddw)($s7)

	# stage = 2 (all results stored), then publish DONE_MAGIC last
	addiu	$t0, $zero, 2
	lui	$s7, %hi(stage)
	sw	$t0, %lo(stage)($s7)
	sync
	lui	$t0, %hi(DONE_MAGIC)
	addiu	$t0, $t0, %lo(DONE_MAGIC)
	lui	$s7, %hi(done)
	sw	$t0, %lo(done)($s7)
	sync

spin:
	b	spin
	nop
	.end	_start
	.size	_start, . - _start

# ---------------------------------------------------------------------------
	.data
	.align	6				# 64-byte aligned results block

	.globl	results
	.type	results, @object
results:
	.globl	res_addu
res_addu:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x00  sd
	.globl	res_daddu
res_daddu:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x08  sd
	.globl	res_dsra32
res_dsra32:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x10  sd
	.globl	res_mult_rd
res_mult_rd:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x18  sd
	.globl	res_mult_lo
res_mult_lo:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x20  sd
	.globl	res_mult_hi
res_mult_hi:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x28  sd
	.globl	res_mult2_rd
res_mult2_rd:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x30  sd
	.globl	res_mult2_hi
res_mult2_hi:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x38  sd
	.globl	res_multu_rd
res_multu_rd:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x40  sd
	.globl	res_multu_hi
res_multu_hi:	.word	0xA5A5A5A5, 0xA5A5A5A5	# +0x48  sd
	.globl	res_pcpyld
res_pcpyld:	.word	0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5	# +0x50  sq
	.globl	res_paddw
res_paddw:	.word	0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5	# +0x60  sq
results_end:
	.size	results, results_end - results

	.align	4
	.globl	vec_a
vec_a:	.word	0x00000001, 0x00000002, 0x00000003, 0x7FFFFFFF
	.globl	vec_b
vec_b:	.word	0x00000010, 0x00000020, 0x00000030, 0x00000001

	.align	2
	.globl	done
	.type	done, @object
done:	.word	0
	.size	done, 4
	.globl	stage
	.type	stage, @object
stage:	.word	0
	.size	stage, 4

# ---------------------------------------------------------------------------
	.bss
	.align	4
stack:	.space	4096
	.globl	stack_top
stack_top:
