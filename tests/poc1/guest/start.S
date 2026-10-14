# PoC-1 guest driver (R5900). Walks test_table: {u32 func, u32 nvec, u32 in_ptr, u32 out_ptr}; for every vector calls
# func(a0 = in block, a1 = out block); then stores 0x600DF00D to done_flag and spins. Uses only s-registers across calls.
	.set noreorder
	.set noat
	.text
	.globl _start
	.ent _start
_start:
	lui	$29, %hi(stack_top)
	addiu	$29, $29, %lo(stack_top)
	lui	$16, %hi(test_table)
	addiu	$16, $16, %lo(test_table)
	lui	$23, %hi(test_table_end)
	addiu	$23, $23, %lo(test_table_end)
1:	beq	$16, $23, 9f
	nop
	lw	$17, 0($16)
	lw	$18, 4($16)
	lw	$19, 8($16)
	lw	$20, 12($16)
2:	beq	$18, $0, 3f
	nop
	addu	$4, $19, $0
	jalr	$17
	addu	$5, $20, $0
	addiu	$19, $19, 64
	addiu	$20, $20, 32
	beq	$0, $0, 2b
	addiu	$18, $18, -1
3:	beq	$0, $0, 1b
	addiu	$16, $16, 16
9:	lui	$8, %hi(done_flag)
	addiu	$8, $8, %lo(done_flag)
	lui	$9, 0x600D
	ori	$9, $9, 0xF00D
	sw	$9, 0($8)
	sync
8:	beq	$0, $0, 8b
	nop
	.end _start
