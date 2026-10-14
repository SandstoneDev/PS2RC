/* hle_ctx.S - coroutine switch for the PoC-6 HLE runtime on the PSP (Allegrex, psp-gcc EABI).
 *
 * Guest threads are coroutines inside ONE PSP thread (the EE kernel rules are enforced by src/hle/hle_sched.c: a
 * switch only happens at a safe point, by an explicit call). The switch is an ordinary function call, so only the
 * callee-saved state has to move: s0-s7, fp, sp, ra, gp and - to be safe whatever the FP ABI says - f20-f31.
 * The FPU control register (FCR31 = 0x01000001, the EE model) is per PSP thread and stays as it is.
 *
 *   void hle_ctx_swap_asm(uint32_t save[24], const uint32_t load[24]);
 *   hle_ctx_trampoline: first "return address" of a new context: calls s0 (the entry function, never returns).
 * Layout of the 24 words: 0-7 s0-s7, 8 fp, 9 sp, 10 ra, 11 gp, 12-23 f20-f31 (src/hle/hle_plat.h hle_hostctx).
 */
        .set    noreorder
        .set    noat
        .text

        .align  4
        .globl  hle_ctx_swap_asm
        .ent    hle_ctx_swap_asm
hle_ctx_swap_asm:
        sw      $16, 0($4)
        sw      $17, 4($4)
        sw      $18, 8($4)
        sw      $19, 12($4)
        sw      $20, 16($4)
        sw      $21, 20($4)
        sw      $22, 24($4)
        sw      $23, 28($4)
        sw      $30, 32($4)
        sw      $29, 36($4)
        sw      $31, 40($4)
        sw      $28, 44($4)
        swc1    $f20, 48($4)
        swc1    $f21, 52($4)
        swc1    $f22, 56($4)
        swc1    $f23, 60($4)
        swc1    $f24, 64($4)
        swc1    $f25, 68($4)
        swc1    $f26, 72($4)
        swc1    $f27, 76($4)
        swc1    $f28, 80($4)
        swc1    $f29, 84($4)
        swc1    $f30, 88($4)
        swc1    $f31, 92($4)
        lw      $16, 0($5)
        lw      $17, 4($5)
        lw      $18, 8($5)
        lw      $19, 12($5)
        lw      $20, 16($5)
        lw      $21, 20($5)
        lw      $22, 24($5)
        lw      $23, 28($5)
        lw      $30, 32($5)
        lw      $29, 36($5)
        lw      $31, 40($5)
        lw      $28, 44($5)
        lwc1    $f20, 48($5)
        lwc1    $f21, 52($5)
        lwc1    $f22, 56($5)
        lwc1    $f23, 60($5)
        lwc1    $f24, 64($5)
        lwc1    $f25, 68($5)
        lwc1    $f26, 72($5)
        lwc1    $f27, 76($5)
        lwc1    $f28, 80($5)
        lwc1    $f29, 84($5)
        lwc1    $f30, 88($5)
        lwc1    $f31, 92($5)
        jr      $31
        nop
        .end    hle_ctx_swap_asm

        .align  4
        .globl  hle_ctx_trampoline
        .ent    hle_ctx_trampoline
hle_ctx_trampoline:
        jalr    $16
        nop
        break
        nop
        .end    hle_ctx_trampoline
