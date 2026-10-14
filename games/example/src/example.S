# example.S - the PS2RC example program: a small PS2 (EE, R5900) program written for this repository.
#
# What it does, so that every stage of the pipeline has something to show:
#   _start   sets up its stack and heap through two EE kernel calls (SetupThread, SetupHeap), calls main, then Exit;
#   main     prints a line through the C library's write(), runs 60 "frames" (frame + DelayThread(20 ms) each),
#            prints its state and returns 0;
#   frame    a little 32-bit, 64-bit and 128-bit (MMI paddw, lq / sq) arithmetic on its data;
#   write, DelayThread, Exit: stand-ins for library functions. A real game links Sony's libraries there; the HLE core
#            implements them by name and the example pack's hooks.def says where they are (the "Sony-library binding"
#            a game pack provides). frame is traced as the pack's main loop ([runtime] main_loop).
# Built with the pspdev binutils in R5900 mode (games/example/build_elf.sh); no PS2 toolchain is needed.
	.set noreorder
	.set noat

	.section .text.start, "ax"
	.globl _start
	.ent _start
_start:
	lui	$28, %hi(_gp)
	addiu	$28, $28, %lo(_gp)
	move	$4, $28			# SetupThread(gp, stack = -1: top of RAM, stack size, args, root)
	addiu	$5, $0, -1
	lui	$6, 0x0001		# 64 KB
	lui	$7, %hi(args)
	addiu	$7, $7, %lo(args)
	move	$8, $0
	addiu	$3, $0, 60
	syscall
	move	$29, $2			# sp = the stack top the kernel returns
	lui	$4, %hi(_end)		# SetupHeap(_end, -1)
	addiu	$4, $4, %lo(_end)
	addiu	$5, $0, -1
	addiu	$3, $0, 61
	syscall
	jal	main
	nop
	jal	Exit			# Exit(main())
	move	$4, $2
1:	beq	$0, $0, 1b
	nop
	.end _start

	.text
	.globl main
	.ent main
main:
	addiu	$29, $29, -48
	sd	$31, 0($29)
	sd	$16, 8($29)
	sd	$17, 16($29)
	addiu	$4, $0, 1		# write(1, msg_hello, length)
	lui	$5, %hi(msg_hello)
	addiu	$5, $5, %lo(msg_hello)
	jal	write
	addiu	$6, $0, msg_hello_end - msg_hello
	move	$16, $0			# frame number
2:	jal	frame
	move	$4, $16
	ori	$4, $0, 20000		# DelayThread(20000 us)
	jal	DelayThread
	nop
	addiu	$16, $16, 1
	slti	$1, $16, 60
	bne	$1, $0, 2b
	nop
	jal	print_state
	nop
	move	$2, $0			# return 0
	ld	$31, 0($29)
	ld	$16, 8($29)
	ld	$17, 16($29)
	jr	$31
	addiu	$29, $29, 48
	.end main

# print_state: "PS2RC example: state XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX\n" (state, then the 4 words of acc)
	.globl print_state
	.ent print_state
print_state:
	addiu	$29, $29, -32
	sd	$31, 0($29)
	sd	$16, 8($29)
	sd	$17, 16($29)
	lui	$5, %hi(outbuf)
	addiu	$5, $5, %lo(outbuf)
	lui	$4, %hi(msg_state)
	jal	copystr
	addiu	$4, $4, %lo(msg_state)
	move	$5, $2
	lui	$8, %hi(state)
	jal	hex8
	lw	$4, %lo(state)($8)
	lui	$16, %hi(acc)
	addiu	$16, $16, %lo(acc)
	addiu	$17, $0, 4
3:	addiu	$8, $0, 0x20		# ' '
	sb	$8, 0($2)
	addiu	$5, $2, 1
	lw	$4, 0($16)
	jal	hex8
	addiu	$16, $16, 4
	addiu	$17, $17, -1
	bne	$17, $0, 3b
	nop
	addiu	$8, $0, 0x0A		# '\n'
	sb	$8, 0($2)
	addiu	$2, $2, 1
	lui	$5, %hi(outbuf)
	addiu	$5, $5, %lo(outbuf)
	subu	$6, $2, $5
	jal	write			# write(1, outbuf, length)
	addiu	$4, $0, 1
	ld	$31, 0($29)
	ld	$16, 8($29)
	ld	$17, 16($29)
	jr	$31
	addiu	$29, $29, 32
	.end print_state

# hex8(value a0, destination a1) -> v0 = a1 + 8: eight lowercase hex digits, most significant first
	.globl hex8
	.ent hex8
hex8:
	addiu	$8, $0, 8
	lui	$9, %hi(hexdigits)
	addiu	$9, $9, %lo(hexdigits)
4:	srl	$10, $4, 28
	addu	$10, $9, $10
	lbu	$10, 0($10)
	sb	$10, 0($5)
	sll	$4, $4, 4
	addiu	$8, $8, -1
	bne	$8, $0, 4b
	addiu	$5, $5, 1
	jr	$31
	move	$2, $5
	.end hex8

# copystr(source a0, destination a1) -> v0 = the end of the copy (the terminating 0 is not copied)
	.globl copystr
	.ent copystr
copystr:
	lbu	$8, 0($4)
	beq	$8, $0, 5f
	nop
	sb	$8, 0($5)
	addiu	$4, $4, 1
	beq	$0, $0, copystr
	addiu	$5, $5, 1
5:	jr	$31
	move	$2, $5
	.end copystr

# frame(n): state = state * 0x5851F42D + table[n & 15] (32-bit); acc += {n, state, n * n, ~n} word by word (MMI)
	.section .text.frame, "ax"
	.globl frame
	.ent frame
frame:
	lui	$8, %hi(state)
	addiu	$8, $8, %lo(state)
	lw	$9, 0($8)
	lui	$10, 0x5851
	ori	$10, $10, 0xF42D
	mult	$9, $9, $10		# R5900 three-operand multiply: rd = LO
	andi	$11, $4, 15
	sll	$11, $11, 2
	lui	$12, %hi(table)
	addiu	$12, $12, %lo(table)
	addu	$12, $12, $11
	lw	$13, 0($12)
	addu	$9, $9, $13
	sw	$9, 0($8)
	lui	$14, %hi(vec)
	addiu	$14, $14, %lo(vec)
	sw	$4, 0($14)
	sw	$9, 4($14)
	mult	$15, $4, $4
	sw	$15, 8($14)
	nor	$15, $4, $0
	sw	$15, 12($14)
	lq	$24, 0($14)		# 128-bit load
	lui	$25, %hi(acc)
	addiu	$25, $25, %lo(acc)
	lq	$1, 0($25)
	paddw	$1, $1, $24		# four 32-bit adds
	sq	$1, 0($25)
	jr	$31
	nop
	.end frame

# library stand-ins (bound by name in the pack's hooks.def; the HLE core replaces them)
	.section .text.lib, "ax"
	.globl write
	.ent write
write:					# write(fd, buffer, length): console output of the C library
	jr	$31
	addiu	$2, $0, -1
	.end write
	.globl DelayThread
	.ent DelayThread
DelayThread:				# DelayThread(microseconds): kernel library wait
	jr	$31
	move	$2, $0
	.end DelayThread
	.globl Exit
	.ent Exit
Exit:					# Exit(code): back to the system
	beq	$0, $0, Exit
	nop
	.end Exit

	.data
	.align	4
args:	.space	64
msg_hello:
	.ascii	"PS2RC example: hello from a PS2 program translated to C\n"
msg_hello_end:
msg_state:
	.asciz	"PS2RC example: state "
hexdigits:
	.ascii	"0123456789abcdef"
	.align	4
table:	.word	0x9E3779B9, 0x7F4A7C15, 0x85EBCA6B, 0xC2B2AE35, 0x27D4EB2F, 0x165667B1, 0xD3A2646C, 0xFD7046C5
	.word	0xB55A4F09, 0x68E31DA4, 0x1B873593, 0xCC9E2D51, 0xE6546B64, 0x3C6EF372, 0xA54FF53A, 0x510E527F
state:	.word	1
	.align	4
vec:	.space	16
acc:	.space	16
outbuf:	.space	128
