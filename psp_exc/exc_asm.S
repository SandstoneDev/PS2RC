/* exc_asm.S - the default exception handler of ps2rc_exc.prx (kernel mode; see ps2rc_exc.h).
 *
 * Entered from the PSP exception manager with the faulting thread's registers, except v0/v1, which the manager keeps
 * in the COP0 control registers $4/$5 (the same convention pspsdk's _pspDebugExceptionHandler relies on). Uses v0 as
 * the block pointer and v1 / at as scratch, all saved first. Resumes in user mode (Status is left as it was) at the
 * EBOOT's report function, a0 = the block, gp = the EBOOT's gp, sp = fault sp - 4 KB rounded down to 16.
 */
        .set    noreorder
        .set    noat
        .text

        .align  4
        .globl  ps2rc_exc_handler
        .ent    ps2rc_exc_handler
ps2rc_exc_handler:
        nop
        nop
        lui     $2, %hi(g_exc_regs)
        lw      $2, %lo(g_exc_regs)($2)
        beq     $2, $0, 9f                      /* not armed / already used once: spin */
        nop
        sw      $1, 4($2)
        cfc0    $1, $4                          /* the thread's v0 */
        sw      $1, 8($2)
        cfc0    $1, $5                          /* the thread's v1 */
        sw      $1, 12($2)
        sw      $4, 16($2)
        sw      $5, 20($2)
        sw      $6, 24($2)
        sw      $7, 28($2)
        sw      $8, 32($2)
        sw      $9, 36($2)
        sw      $10, 40($2)
        sw      $11, 44($2)
        sw      $12, 48($2)
        sw      $13, 52($2)
        sw      $14, 56($2)
        sw      $15, 60($2)
        sw      $16, 64($2)
        sw      $17, 68($2)
        sw      $18, 72($2)
        sw      $19, 76($2)
        sw      $20, 80($2)
        sw      $21, 84($2)
        sw      $22, 88($2)
        sw      $23, 92($2)
        sw      $24, 96($2)
        sw      $25, 100($2)
        sw      $26, 104($2)
        sw      $27, 108($2)
        sw      $28, 112($2)
        sw      $29, 116($2)
        sw      $30, 120($2)
        sw      $31, 124($2)
        sw      $0, 0($2)
        mfc0    $3, $14                         /* EPC */
        sw      $3, 128($2)
        mfc0    $3, $13                         /* Cause */
        sw      $3, 132($2)
        mfc0    $3, $8                          /* BadVAddr */
        sw      $3, 136($2)
        mfc0    $3, $12                         /* Status */
        sw      $3, 140($2)
        mflo    $3
        sw      $3, 144($2)
        mfhi    $3
        sw      $3, 148($2)
        lui     $3, 0xC4A5
        ori     $3, $3, 0xC4A5
        sw      $3, 160($2)                     /* PS2RC_EXC_HIT */
        lui     $3, %hi(g_exc_regs)
        sw      $0, %lo(g_exc_regs)($3)         /* one shot */
        lui     $3, %hi(g_exc_report)
        lw      $3, %lo(g_exc_report)($3)
        mtc0    $3, $14                         /* resume at the report function */
        lui     $3, %hi(g_exc_gp)
        lw      $28, %lo(g_exc_gp)($3)
        lw      $29, 116($2)
        addiu   $29, $29, -4096
        addiu   $1, $0, -16
        and     $29, $29, $1
        move    $4, $2
        sync
        nop
        nop
        eret
        nop
9:      lui     $2, %hi(g_exc_prev)             /* not armed (used once, or released): hand the exception to the */
        lw      $2, %lo(g_exc_prev)($2)         /* default handler that was active before ours (build 26: spinning */
        beq     $2, $0, 8f                      /* here froze the console while the game exited) */
        nop
        jr      $2
        nop
8:      b       8b
        nop
        .end    ps2rc_exc_handler
