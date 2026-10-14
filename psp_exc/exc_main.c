/* exc_main.c - ps2rc_exc.prx, the kernel half of the PS2RC crash handler (see ps2rc_exc.h).
 * The handler is registered through the CFW's function finder first (ARK-4: sctrlHENFindFunction resolves the
 * pre-6.xx NID), else through the plain ExceptionManagerForKernel import. Word 46 of the block says which path:
 * 1 = sctrlHENFindFunction, 2 = the import.
 * Build 22: the optional hardware-counter thread (ps2rc_exc.h "Hardware counters"); started only when the EBOOT passes
 * a counter block AND the handler is armed (a real CFW kernel: in PPSSPP both imports are unlinked and the module is
 * not even resident, so 0xBC400000 - unmapped there - is never touched). */
#include <stddef.h>
#include <pspkernel.h>
#include <pspexception.h>
#include "ps2rc_sctrl.h"                       /* sctrlHENFindFunction of the CFW (own import) */
#define PS2RC_EXC_KERNEL
#include "ps2rc_exc.h"

PSP_MODULE_INFO("ps2rc_exc", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

#define NID_REGISTER_DEFAULT_EXCEPTION_HANDLER 0x565C0B0Eu
#define NID_GET_ACTIVE_DEFAULT_EXCEPTION_HANDLER 0xDF83875Eu   /* libpspkernel stubs (pspdev), pre-6.xx NIDs */
#define NID_RELEASE_DEFAULT_EXCEPTION_HANDLER 0x291FF031u

uint32_t g_exc_regs, g_exc_report, g_exc_gp;      /* read by exc_asm.S */
uint32_t g_exc_prev;                              /* the default handler active before ours (exc_asm.S chains to it) */
static void *g_exc_h;                             /* our handler as registered (for the release) */
void ps2rc_exc_handler(void);

static int stub_linked(void *fn);
void ps2rc_me_take(uint32_t addr);               /* exc_me.c: Media Engine sleep support */
void ps2rc_me_stop(void);
int ps2rc_stub_linked(void *fn) { return stub_linked(fn); }

/* a kernel function through the CFW's finder (ARK resolves old NIDs) or NULL */
static void *exc_fn(uint32_t nid)
{
    if (!stub_linked((void *)ps2rc_find_function)) return NULL;
    return (void *)ps2rc_find_function("sceExceptionManager", "ExceptionManagerForKernel", nid);
}

/* an import stub the kernel linked to another kernel module is `j <function>` (target in kernel memory); an unlinked
   one must not be called (PPSSPP without ARK: sctrlHENFindFunction jumped to 0x8002013A "library not linked") */
static int stub_linked(void *fn)
{
    uint32_t w = *(volatile uint32_t *)fn;
    if ((w >> 26) != 2) return 0;
    uint32_t target = (((uint32_t)fn + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
    return (target & 0x7F000000u) == 0x08000000u;
}

/* ------------------------------------------------------------------ hardware counters (build 22) */
static volatile Ps2rcPerfBlock *g_perf;
static uint32_t g_prev[PS2RC_PERF_N];
static volatile int g_wrote_en;                   /* the thread set PROFEN = 1 (it was 0): ours to put back */

/* (the console switched off after sceKernelExitGame in prof_hw=1 sessions only, 2 of 5): at the quit
   the counter block is put back to its idle state - PROFEN 0, then words 1..19 cleared (pspsdk's
   pspDebugProfilerClear writes 0x04-0x50; 0x50 is left alone here: reading it killed a thread on the PSP-3000) - and
   read back for the exit log. Only when this module switched PROFEN on; a block found running belongs to someone
   else and is not touched. Breadcrumbs as everywhere: a write that kills the thread leaves its word in `step`. */
static void perf_restore(volatile Ps2rcPerfBlock *b)
{
    volatile uint32_t *r = (volatile uint32_t *)PS2RC_PERF_BASE;
    if (!g_wrote_en) {
        b->exit_en = r[0];
        b->exit_done = PS2RC_PERF_EXIT_FOUND_ON;
        return;
    }
    r[0] = 0u;                                     /* PROFEN off first: the counters stop */
    __asm__ volatile("sync");
    for (int i = 1; i < PS2RC_PERF_N; i++) {
        b->step = PS2RC_PERF_STEP_RESTORE + (uint32_t)i;
        r[i] = 0u;
    }
    __asm__ volatile("sync");
    uint32_t nz = 0u;
    for (int i = 1; i < PS2RC_PERF_N; i++)
        if (r[i]) nz |= 1u << i;
    b->exit_en = r[0];
    b->exit_nz = nz;
    g_wrote_en = 0;
    b->exit_done = PS2RC_PERF_EXIT_RESTORED;
}

/* synchronous reader for the EBOOT (kuKernelCall(readfn, {dst, n})): the free-running values, nothing is written -
   the counter thread and this reader never reset a register, so their differences stay valid together (the
   "two masters" lesson: a reset by one reader made the other one's deltas wrap to 4e9 in every counter) */
int ps2rc_perf_read(uint32_t *dst, int n)
{
    volatile uint32_t *r = (volatile uint32_t *)PS2RC_PERF_BASE;
    if (!dst || n <= 0) return 0;
    if (n > PS2RC_PERF_N) n = PS2RC_PERF_N;
    for (int i = 0; i < n; i++) dst[i] = r[i];
    return n;
}

static int perf_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    volatile Ps2rcPerfBlock *b = g_perf;
    volatile uint32_t *r = (volatile uint32_t *)PS2RC_PERF_BASE;
    uint32_t period = b->period_us >= 1000u && b->period_us <= 1000000u ? b->period_us : 50000u;
    b->step = PS2RC_PERF_STEP_STARTED;
    sceKernelDelayThread(period);                  /* the EBOOT is still starting: arm one period later */
    b->step = PS2RC_PERF_STEP_READ_EN;
    uint32_t en = r[0];
    b->en_before = en;
    if (!(en & 1u)) {
        b->step = PS2RC_PERF_STEP_WRITE_EN;
        g_wrote_en = 1;
        r[0] = 1u;                                 /* PROFEN (while counting the only register this module writes) */
        __asm__ volatile("sync");
    }
    for (int i = 0; i < PS2RC_PERF_N; i++) {
        b->step = PS2RC_PERF_STEP_ARM + (uint32_t)i;
        g_prev[i] = r[i];
    }
    uint32_t last = sceKernelGetSystemTimeLow();
    b->step = PS2RC_PERF_STEP_ARMED;
    for (;;) {
        sceKernelDelayThread(period);
        if (b->quit) break;
        uint32_t now = sceKernelGetSystemTimeLow();
        b->seq = b->seq + 1u;                      /* odd: update in progress */
        for (int i = 0; i < PS2RC_PERF_N; i++) {
            b->step = PS2RC_PERF_STEP_WINDOW + (uint32_t)i;
            uint32_t cur = r[i];
            b->tot[i] = b->tot[i] + (uint64_t)(uint32_t)(cur - g_prev[i]);
            g_prev[i] = cur;
        }
        b->us = b->us + (uint64_t)(uint32_t)(now - last);
        last = now;
        b->windows = b->windows + 1u;
        b->seq = b->seq + 1u;                      /* even: consistent */
        b->step = PS2RC_PERF_STEP_DONE;
    }
    perf_restore(b);                               /* PROFEN as it was found, counters cleared (round 5) */
    b->step = PS2RC_PERF_STEP_QUIT;
    sceKernelExitDeleteThread(0);
    return 0;
}

/* takes the EBOOT's counter block (user memory, 8-aligned) and starts the thread; the result codes land in the block */
static void perf_start(uint32_t addr)
{
    if (!addr || (addr & 7u) || addr < 0x08800000u || addr >= 0x0C000000u) return;
    volatile Ps2rcPerfBlock *pb = (volatile Ps2rcPerfBlock *)addr;
    pb->version = 1u;
    pb->step = 0u;
    pb->seq = 0u;
    pb->windows = 0u;
    pb->us = 0u;
    for (int i = 0; i < PS2RC_PERF_N; i++) pb->tot[i] = 0u;
    pb->readfn = (uint32_t)ps2rc_perf_read | 0x80000000u;
    g_perf = pb;
    SceUID th = sceKernelCreateThread("ps2rc_perf", perf_thread, 0x12, 0x1000, 0, NULL);
    pb->thread_rc = (uint32_t)th;
    pb->start_rc = th >= 0 ? (uint32_t)sceKernelStartThread(th, 0, NULL) : (uint32_t)th;
    pb->magic = PS2RC_PERF_MAGIC;                  /* last: the EBOOT trusts the other fields from here on */
}

int module_start(SceSize args, void *argp)
{
    const Ps2rcExcArgs *a = argp;
    if (args < 16 || !a || a->magic != PS2RC_EXC_MAGIC || !a->regs || !a->report) return 1;
    volatile uint32_t *blk = (volatile uint32_t *)a->regs;
    g_exc_report = a->report;
    g_exc_gp = a->gp;
    g_exc_regs = a->regs;
    void *h = (void *)((uint32_t)ps2rc_exc_handler | 0x80000000u);
    g_exc_h = h;
    {   /* build 26: remember the default handler we replace (exc_asm.S hands it every exception we do not take) */
        void *(*get)(void) = (void *(*)(void))exc_fn(NID_GET_ACTIVE_DEFAULT_EXCEPTION_HANDLER);
        g_exc_prev = get ? (uint32_t)get() : 0u;
        if (g_exc_prev == (uint32_t)h) g_exc_prev = 0u;
        blk[43] = g_exc_prev;
    }
    blk[44] = *(volatile uint32_t *)(void *)ps2rc_find_function;              /* the two import stubs' first */
    blk[45] = *(volatile uint32_t *)(void *)sceKernelRegisterDefaultExceptionHandler;   /* words, for the log */
    int (*reg)(void *) = NULL;
    if (stub_linked((void *)ps2rc_find_function))
        reg = (int (*)(void *))ps2rc_find_function("sceExceptionManager", "ExceptionManagerForKernel",
                                                    NID_REGISTER_DEFAULT_EXCEPTION_HANDLER);
    int r = (int)0x8002013Au;                      /* SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED */
    if (reg) { r = reg(h); blk[46] = 1; }
    else if (stub_linked((void *)sceKernelRegisterDefaultExceptionHandler)) {
        r = sceKernelRegisterDefaultExceptionHandler(h);
        blk[46] = 2;
    } else blk[46] = 3;                            /* neither linked */
    blk[PS2RC_EXC_REGRESULT] = (uint32_t)r;
    if (r < 0) g_exc_regs = 0;
    if (r >= 0 && args >= offsetof(Ps2rcExcArgs, me) && a->perf) perf_start(a->perf);   /* resident only: the thread
                                                                                       lives in this module's code */
    if (r >= 0 && args >= sizeof(Ps2rcExcArgs) && a->me) ps2rc_me_take(a->me);   /* exc_me.c */
    return r < 0 ? 1 : 0;                          /* 1: not resident */
}

/* the exit mode's part here (exc_me.c me_exitmode, hookfn on = 2) - what
   module_stop does except the ME handlers: disarm, the counter thread told to quit (PROFEN back if it does not), the
   default exception handler given back. The module stays loaded until the kernel reboot of sceKernelExitGame. */
void ps2rc_exc_exitmode_main(void)
{
    g_exc_regs = 0u;
    if (g_perf) {
        g_perf->quit = 1u;
        if (g_wrote_en && g_perf->step != PS2RC_PERF_STEP_QUIT) {
            *(volatile uint32_t *)PS2RC_PERF_BASE = 0u;
            __asm__ volatile("sync");
            g_wrote_en = 0;
            g_perf->exit_done = PS2RC_PERF_EXIT_STOP_ONLY;
        }
    }
    int (*rel)(void *) = (int (*)(void *))exc_fn(NID_RELEASE_DEFAULT_EXCEPTION_HANDLER);
    if (rel && g_exc_h) rel(g_exc_h);
    g_exc_h = 0;
}

/* build 26: the EBOOT stops + unloads this module before sceKernelExitGame (exc_user.c ps2rc_exc_uninstall): disarm,
   give the default exception handler back, so nothing in the kernel points into this module or the EBOOT's memory
   while the game goes away (2 of 5 exits of the build-25 A/B session switched the console off). */
int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    ps2rc_me_stop();                               /* our sysevent handler out, the OS ME driver's back */
    g_exc_regs = 0u;
    if (g_perf) {
        g_perf->quit = 1u;                         /* the counter thread (if any) leaves at its next window */
        if (g_wrote_en && g_perf->step != PS2RC_PERF_STEP_QUIT) {   /* it did not quit - PROFEN */
            *(volatile uint32_t *)PS2RC_PERF_BASE = 0u;             /* back off at least (one write it did before) */
            __asm__ volatile("sync");
            g_wrote_en = 0;
            g_perf->exit_done = PS2RC_PERF_EXIT_STOP_ONLY;
        }
    }
    int (*rel)(void *) = (int (*)(void *))exc_fn(NID_RELEASE_DEFAULT_EXCEPTION_HANDLER);
    if (rel && g_exc_h) rel(g_exc_h);
    return 0;
}
