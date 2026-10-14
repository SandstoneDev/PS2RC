/* hle_prof_psp.c - PSP platform part of the profiler (hle/core/hle_prof.h): the ALLEGREX hardware counters.
 *
 * OFF unless the run options say prof_hw=1 or 2, and refused in PPSSPP (the emulator does not map 0xBC400000 and
 * crashes on any access). The counters are kernel-only: ps2rc_exc.prx (psp_exc/) takes the block
 * below at module start and runs its counter thread (50 ms windows, 64-bit sums: ps2rc_exc.h). prof_hw=2 also reads
 * the registers synchronously at a few zone boundaries through kubridge's kuKernelCall (ARK-4) and the reader the
 * module publishes in the block (~65 us per call measured on the PSP-3000).
 * Every way it can fail says so in the log (no block passed / module too old / thread not started / never armed /
 * died at register N / door refused) - silence would read like "no counters" and "broken counters" at once.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include "ps2rc_kcall.h"                     /* kuKernelCall of the CFW (own import) */
#include <string.h>
#include "hle.h"
#include "hle_prof.h"
#include "ps2rc_exc.h"

static Ps2rcPerfBlock perf_blk __attribute__((aligned(64)));
static int perf_passed;

/* PPSSPP answers its own "kemulator:" device; a real PSP has no such driver */
int hle_psp_is_emulator(void)
{
    int out = 0;
    return sceIoDevctl("kemulator:", 0x00000003, NULL, 0, &out, sizeof out) >= 0;
}

/* the block for ps2rc_exc_install2 (NULL unless prof_hw >= 1 on hardware) */
Ps2rcPerfBlock *hle_psp_perf_block(void)
{
    if (hle_pz_opt_hw < 1) return NULL;
    perf_passed = 1;
    return &perf_blk;
}

static int perf_live(void)
{
    return perf_passed && perf_blk.magic == PS2RC_PERF_MAGIC && perf_blk.step >= PS2RC_PERF_STEP_ARMED &&
           perf_blk.step < PS2RC_PERF_STEP_QUIT;
}

/* after the module start: wait (at most 600 ms) for the counter thread to arm, then one line for the log */
void hle_psp_perf_status(char *out, unsigned n)
{
    if (!perf_passed) { snprintf(out, n, "off"); return; }
    if (perf_blk.magic != PS2RC_PERF_MAGIC) {
        snprintf(out, n, "NOT taken by ps2rc_exc.prx (module older than build 22, or the crash handler is not armed) "
                 "- no counters");
        return;
    }
    for (int i = 0; i < 60 && perf_blk.step < PS2RC_PERF_STEP_ARMED; i++) sceKernelDelayThread(10000);
    uint32_t st = perf_blk.step;
    snprintf(out, n, "%s: thread %08x start %08x step %03x (%s) PROFEN before %08x windows %u readfn %08x",
             perf_live() ? "counting" : "NOT counting", (unsigned)perf_blk.thread_rc, (unsigned)perf_blk.start_rc,
             (unsigned)st, st == 0 ? "thread never ran" : st == PS2RC_PERF_STEP_STARTED ? "started, not armed yet" :
             st == PS2RC_PERF_STEP_READ_EN || st == PS2RC_PERF_STEP_WRITE_EN ? "stuck at PROFEN" :
             st >= PS2RC_PERF_STEP_ARM && st < PS2RC_PERF_STEP_ARMED ? "stuck in the arming snapshot" : "armed",
             (unsigned)perf_blk.en_before, (unsigned)perf_blk.windows, (unsigned)perf_blk.readfn);
}

/* a frozen breadcrumb tells where the kernel thread died (word = step - 0x200) */
uint32_t hle_psp_perf_step(void) { return perf_passed ? perf_blk.step : 0; }

int hle_plat_hw_totals(uint64_t tot[20], uint64_t *us)
{
    if (!perf_live()) return 0;
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s1 = perf_blk.seq;
        if (s1 & 1u) continue;                 /* cannot happen from a lower-priority thread; harmless */
        for (int i = 0; i < PS2RC_PERF_N && i < 20; i++) tot[i] = perf_blk.tot[i];
        *us = perf_blk.us;
        if (perf_blk.seq == s1) return 1;
    }
    return 0;
}

int hle_plat_hw_door(uint32_t raw[20])
{
    if (hle_pz_opt_hw < 2 || !perf_live() || !perf_blk.readfn) return 0;
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    memset(raw, 0, 20 * sizeof raw[0]);
    a.arg[0] = (uint32_t)(uintptr_t)raw;
    a.arg[1] = 20;
    if (ps2rc_kcall((void *)(uintptr_t)perf_blk.readfn, &a) < 0) return 0;
    return raw[1] != 0 || raw[2] != 0;         /* the reader ran (the clock counters are never 0 while counting) */
}

/* the capture buffer (poc6.cfg prof_capture=N): a block from the free user memory (~6 MB after the guest window on
   the PSP-3000, build 21), high end; never the 768 KB newlib heap the disc layer allocates its paths from */
void *hle_plat_prof_alloc(size_t n)
{
    SceUID id = sceKernelAllocPartitionMemory(2, "hle_prof_cap", PSP_SMEM_High, n, NULL);
    if (id < 0) return NULL;
    void *p = sceKernelGetBlockHeadAddr(id);
    if (p) memset(p, 0, n);
    return p;
}

/* before the EBOOT exits: stop the counter thread (it must not sleep inside the module while the game goes away).
   Returns -1 = no counter thread, 1 = it left the module (step QUIT), 0 = it did not within 1 s -
   then the module must NOT be unloaded (its code would vanish under a sleeping kernel thread). The thread sleeps one
   50 ms window between checks; it writes step QUIT right before sceKernelExitDeleteThread and runs at a higher
   priority than the caller, so it is out of the module once QUIT is seen. */
int hle_psp_perf_shutdown(void)
{
    if (!perf_passed || perf_blk.magic != PS2RC_PERF_MAGIC) return -1;
    if (perf_blk.step == PS2RC_PERF_STEP_QUIT) return 1;
    perf_blk.quit = 1;
    for (int i = 0; i < 100 && perf_blk.step != PS2RC_PERF_STEP_QUIT; i++) sceKernelDelayThread(10000);
    if (perf_blk.step != PS2RC_PERF_STEP_QUIT) return 0;
    sceKernelDelayThread(10000);                           /* let it finish sceKernelExitDeleteThread */
    return 1;
}

/* what the module did to the counter registers at the quit (exit log, after hle_psp_perf_shutdown)
   + the counter thread's uid looked up again (0x80020198 = unknown thread id: it is gone) */
void hle_psp_perf_exit_info(char *out, unsigned n)
{
    if (!perf_passed || perf_blk.magic != PS2RC_PERF_MAGIC) { snprintf(out, n, "no counter block"); return; }
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    int tr = sceKernelReferThreadStatus((SceUID)perf_blk.thread_rc, &ti);
    uint32_t d = perf_blk.exit_done;
    snprintf(out, n, "registers %s (PROFEN read back %08x, words still non-zero %08x), step %03x, thread status %08x",
             d == PS2RC_PERF_EXIT_RESTORED ? "restored: PROFEN 0 + words 1-19 cleared"
             : d == PS2RC_PERF_EXIT_FOUND_ON ? "left as found (PROFEN was on before us)"
             : d == PS2RC_PERF_EXIT_STOP_ONLY ? "PROFEN 0 by the module stop only (thread did not quit)"
             : "NOT restored (module older than round 5, or the thread died)",
             (unsigned)perf_blk.exit_en, (unsigned)perf_blk.exit_nz, (unsigned)perf_blk.step, (unsigned)tr);
}
