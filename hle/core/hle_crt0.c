/* hle_crt0.c - the program's start-up code (ELF entry) from the game pack's facts ([boot.crt0] in pack.toml ->
 * hle_pack.h), for a build that does not use a pack module of its own for the entry (define HLE_CRT0_DATA).
 *
 * Why: a PS2 program's crt0 clears its BSS with loops that close with `j <loop head>`; function discovery may register
 * the loop heads as function starts and the back-end turns every `j` into a tail call, so the translated entry returns
 * after the first loop iteration. The sequence is the standard PS2 C start-up protocol (the one of ps2sdk's crt0.s):
 *   1. the BSS cleared ([bss_lo, bss_hi));
 *   2. SetupThread (syscall 60: gp, stack, stack size, argument block, thread-exit routine) -> sp, gp set;
 *   3. SetupHeap (syscall 61: heap start, heap size);
 *   4. the library start-up function, then FlushCache(0) (each called with the return address the guest's own call
 *      leaves, so the stack frames match the original);
 *   5. interrupts on (ei);
 *   6. main(argc, argv) with argc = the argument block's first word, argv = the block + 4;
 *   7. exit(main's result).
 * The 128-bit register zeroing of the original is implicit: a fresh context. */
#include <stdio.h>
#include <string.h>
#include "hle.h"

#if defined(HLE_CRT0_DATA) && defined(HLE_PACK_CRT0_ENTRY)
#define SXW(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static void call_at(R5900Ctx *c, uint32_t fn, uint32_t ra)
{
    c->r[31].d[0] = ra;
    rc_dispatch(c, fn);
}

HLE_FN(crt0_start)
{
    char m[48];
    snprintf(m, sizeof m, "crt0 entered (0x%x)", (unsigned)HLE_PACK_CRT0_ENTRY);
    hle_milestone(m);
    memset(gptr(HLE_PACK_CRT0_BSS_LO, HLE_PACK_CRT0_BSS_HI - HLE_PACK_CRT0_BSS_LO), 0,
           HLE_PACK_CRT0_BSS_HI - HLE_PACK_CRT0_BSS_LO);
    c->r[4].d[0] = SXW(HLE_PACK_CRT0_GP);                       /* SetupThread(gp, stack, size, args, root) */
    c->r[5].d[0] = SXW(HLE_PACK_CRT0_STACK);
    c->r[6].d[0] = HLE_PACK_CRT0_STACK_SIZE;
    c->r[7].d[0] = SXW(HLE_PACK_CRT0_ARGS);
    c->r[8].d[0] = SXW(HLE_PACK_CRT0_THREAD_EXIT);
    c->r[28].d[0] = SXW(HLE_PACK_CRT0_GP);
    c->r[3].d[0] = 60;
    rt_syscall(c, HLE_PACK_CRT0_ENTRY);
    c->r[29].d[0] = c->r[2].d[0];
    c->r[4].d[0] = SXW(HLE_PACK_CRT0_HEAP);                     /* SetupHeap(start, size) */
    c->r[5].d[0] = SXW(HLE_PACK_CRT0_HEAP_SIZE);
    c->r[3].d[0] = 61;
    rt_syscall(c, HLE_PACK_CRT0_ENTRY);
    call_at(c, HLE_PACK_CRT0_INIT, HLE_PACK_CRT0_INIT_RA);      /* the libraries' start-up */
    c->r[4].d[0] = 0;
    call_at(c, HLE_PACK_CRT0_FLUSH, HLE_PACK_CRT0_FLUSH_RA);    /* FlushCache(0) */
    rt_fatal(c, "ei", HLE_PACK_CRT0_FLUSH_RA, 0x42000038u);     /* ei */
    c->r[2].d[0] = SXW(HLE_PACK_CRT0_ARGS);
    c->r[4].d[0] = SXW(gw(HLE_PACK_CRT0_ARGS));                 /* main(argc, argv) */
    c->r[5].d[0] = SXW(HLE_PACK_CRT0_ARGS + 4u);
    call_at(c, HLE_PACK_CRT0_MAIN, HLE_PACK_CRT0_MAIN_RA);
    c->r[4].d[0] = c->r[2].d[0];                                /* exit(main()) */
    rc_dispatch(c, HLE_PACK_CRT0_EXIT);
}
#endif
