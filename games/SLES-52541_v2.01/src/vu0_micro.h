/* vu0_micro.h - interface of the HLE of this game's VU0 micro program #0 (VCALLMS).
 *
 * The game (v2.01) loads exactly one VU0 micro program: 245 instructions DMAed to VU0 micro memory address 0 through
 * VIF0 by the init overlay (chain at ELF 0x5A2A00). The EE calls three of its entry points with VCALLMS (0x020, 0x068,
 * 0x3A0) and reads the results back from VU0 registers. A hand-written implementation is a developer module (not in
 * every checkout); it runs the subroutine to completion on c->vu0 with the results PCSX2 gives for the same microcode
 * (checked in PCSX2: results, every register the program writes, timing-dependent flag reads).

 *
 * Requirements: the host FPU in the runtime's mode (round toward zero + flush-to-zero, r5900_rt.h), code compiled
 * without floating-point contraction (-ffp-contract=off, as every runtime unit). VU0 micro memory is assumed to hold
 * program #0 (the game uploads nothing else to VU0); vu0_micro_is_prog0() can check a copy of the uploaded bytes.
 */
#ifndef VU0_MICRO_H
#define VU0_MICRO_H

#include "r5900_rt.h"

#define VU0_MICRO_PROG0_INSNS 245u                   /* instructions loaded at VU0 micro address 0 */
#define VU0_MICRO_PROG0_BYTES (8u * VU0_MICRO_PROG0_INSNS)

/* Run the VU0 micro subroutine at byte address `addr` (VCALLMS imm15 * 8, VCALLMSR CMSAR0 * 8) on c->vu0.
   Returns 0 when it handled the address (0x20, 0x68, 0x3A0), nonzero (and changes nothing) otherwise. */
int rt_vu0_callms_hle(R5900Ctx *c, uint32_t addr);

/* 1 when the `nbytes` bytes at `code` are program #0 (1960 bytes, FNV-1a 64 compared), else 0 */
int vu0_micro_is_prog0(const void *code, uint32_t nbytes);

#endif
