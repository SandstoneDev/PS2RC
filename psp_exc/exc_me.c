/* exc_me.c - ps2rc_exc.prx: Media Engine sleep support (see ps2rc_exc.h "Media Engine sleep support").
 *
 * Kernel functions are looked up through the CFW's finder at run time (sctrlHENFindFunction, already imported by this
 * module for the crash handler) - no new import: a module whose import cannot be linked would still load, but the
 * finder keeps the failure visible in the block instead of a "library not linked" stub. NIDs (6.60, uofw exports):
 *   sceSystemMemoryManager / sceSysEventForKernel (non-randomised): sceKernelReferSysEventHandler 0x68D55505,
 *   sceKernelRegisterSysEventHandler 0xCD9E4BB5, sceKernelUnregisterSysEventHandler 0xD7D3FDCD;
 *   sceLowIO_Driver / sceSysreg_driver: sceSysregMeBusClockEnable 0x38527743 (optional: the OS calls it before it
 *   restarts its ME after a resume).
 * Everything here runs in kernel mode: the hook from kuKernelCall (the EBOOT's thread), the handler from the kernel's
 * sysevent dispatcher during suspend / resume (phase 0: no thread switches - busy waits only, bounded by the 1 MHz
 * system timer at 0xBC600000). */
#include <pspkernel.h>
#include <pspsysevent.h>
#include "ps2rc_sctrl.h"                       /* sctrlHENFindFunction of the CFW (own import) */
#define PS2RC_EXC_KERNEL
#include "ps2rc_exc.h"

#define NID_REFER 0x68D55505u
#define NID_REGISTER 0xCD9E4BB5u
#define NID_UNREGISTER 0xD7D3FDCDu
#define NID_ME_BUS_CLOCK_ENABLE 0x38527743u

#define ME_RESET_VECTOR 0xBFC00000u
#define ME_RESET_REG 0xBC10004Cu                  /* bit 2: the ME held in reset */
#define SYS_TIMER 0xBC600000u                     /* free-running 1 MHz (sceKernelGetSystemTimeLow) */
#define BUSCLK_REG 0xBC100050u                    /* sysreg bus clock enables (read only here, for the record) */
#define HW(a) (*(volatile uint32_t *)(a))

int ps2rc_stub_linked(void *fn);                  /* exc_main.c */

static volatile Ps2rcMeBlock *g_me;
static PspSysEventHandler g_handler;
static PspSysEventHandler *g_mrpc;                /* the OS ME driver's handler record (its function swapped) */
static int (*g_mrpc_orig)(int, char *, void *, int *);   /* EvFn */
static int (*g_unreg)(PspSysEventHandler *);
static void (*g_clk)(void);
static int g_parked_live;                         /* the ME was running and is parked by us: restart at resume */
static int g_exitmode;                            /* hookfn(block, 2) - only record into the exit marker */
static const char g_name[] = "ps2rcMeSleep";

typedef int (*EvFn)(int, char *, void *, int *);
static EvFn kfn(EvFn f) { return (EvFn)((uint32_t)f | 0x80000000u); }   /* kernel segment, as exc_main.c's handler */

static void *find(const char *mod, const char *lib, uint32_t nid)
{
    if (!ps2rc_stub_linked((void *)ps2rc_find_function)) return 0;
    return (void *)ps2rc_find_function(mod, lib, nid);
}

static int same(const char *a, const char *b)
{
    if (!a) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* main-core caches: D index writeback-invalidate (8 KB of indexes, twice), I index invalidate */
static void main_caches(void)
{
    __asm__ volatile("sync" ::: "memory");
    for (uint32_t i = 0; i < 8192u; i += 64u) {
        __asm__ volatile("cache 0x14, 0(%0)" : : "r"(i) : "memory");
        __asm__ volatile("cache 0x14, 0(%0)" : : "r"(i) : "memory");
    }
    for (uint32_t i = 0; i < 8192u; i += 64u) {
        __asm__ volatile("cache 0x04, 0(%0)" : : "r"(i) : "memory");
        __asm__ volatile("cache 0x04, 0(%0)" : : "r"(i) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}

/* SUSPEND_PHASE0_5: stop the ME's loop (it writes its cache back and halts: HleMeHdr state 2), hold it in reset */
static void me_park(volatile Ps2rcMeBlock *b)
{
    volatile uint32_t *c = (volatile uint32_t *)b->ctl;   /* HleMeHdr: [2] state, [3] exit_req */
    int ack = 0;
    uint32_t t0 = HW(SYS_TIMER), dt = 0;
    if (c && b->live) {
        c[3] = 1u;
        __asm__ volatile("sync" ::: "memory");
        for (uint32_t i = 0; i < 4000000u; i++) {          /* <= 20 ms (a block or a hop at most, ~1 ms) */
            if (c[2] == 2u) { ack = 1; break; }
            dt = HW(SYS_TIMER) - t0;
            if (dt > 20000u) break;
        }
        dt = HW(SYS_TIMER) - t0;
    }
    HW(ME_RESET_REG) = HW(ME_RESET_REG) | 0x04u;           /* held from here on (also when it did not answer) */
    __asm__ volatile("sync" ::: "memory");
    b->park_ack = (uint32_t)ack;
    b->park_us = dt;
    b->busclk_suspend = HW(BUSCLK_REG);
    g_parked_live = ack;                                    /* only a clean park is restarted */
}

/* RESUME_PHASE0_5: the stub again (the reset-vector SRAM did not keep it), a cold start, the reset released */
static void me_restart(volatile Ps2rcMeBlock *b)
{
    b->reset_reg = HW(ME_RESET_REG);
    b->busclk_resume = HW(BUSCLK_REG);
    b->busclk_after = b->busclk_resume;
    if (!g_parked_live) return;
    g_parked_live = 0;
    volatile uint32_t *c = (volatile uint32_t *)b->ctl;
    uint32_t *boot = (uint32_t *)b->bootblk;
    const uint32_t *src = (const uint32_t *)b->stub;
    if (!c || !boot || !src || !b->stub_words || b->stub_words > 128u) return;
    c[3] = 0u;                                              /* exit_req */
    c[2] = 0u;                                              /* state */
    boot[PS2RC_ME_BOOT_COLD] = 1u;                          /* the stub: Index Store Tag, no writeback of garbage */
    volatile uint32_t *dst = (volatile uint32_t *)ME_RESET_VECTOR;
    uint32_t r = HW(ME_RESET_REG);
    HW(ME_RESET_REG) = r | 0x04u;
    __asm__ volatile("sync" ::: "memory");
    if (g_clk) {                                            /* the ME's bus clock first, as the OS's resume does */
        g_clk();
        b->busclk_after = HW(BUSCLK_REG);
    }
    for (uint32_t i = 0; i < b->stub_words; i++) dst[i] = src[i];
    main_caches();                                          /* boot block + everything the ME reads, to RAM */
    HW(ME_RESET_REG) = r | 0x04u;
    HW(ME_RESET_REG) = r & ~0x04u;                          /* the ME starts at 0xBFC00000 */
    __asm__ volatile("sync" ::: "memory");
    b->restarts = b->restarts + 1u;
}

/* exit mode - one sysevent call into the marker (KSEG1, the ME held in reset; no block, no EBOOT memory) */
static void az_mark_event(uint32_t ev, int noop)
{
    volatile uint32_t *m = (volatile uint32_t *)PS2RC_AZ_MARK_ADDR;
    if (m[0] != PS2RC_AZ_MARK_MAGIC) return;
    uint32_t n = m[3] + m[4];
    if (noop) { m[4] = m[4] + 1u; m[6] = ev; }
    else { m[3] = m[3] + 1u; m[5] = ev; }
    if (n < 8u) m[7 + n] = ev | (noop ? 0x80000000u : 0u);
    uint32_t x = PS2RC_AZ_MARK_XOR;
    for (int i = 0; i < 15; i++) x ^= m[i];
    m[15] = x;
    __asm__ volatile("sync" ::: "memory");
}

static void az_mark_arm(uint32_t seq, uint32_t state)
{
    volatile uint32_t *m = (volatile uint32_t *)PS2RC_AZ_MARK_ADDR;
    for (int i = 1; i < PS2RC_AZ_MARK_WORDS; i++) m[i] = 0u;
    m[1] = seq;
    m[2] = state;
    m[0] = PS2RC_AZ_MARK_MAGIC;
    uint32_t x = PS2RC_AZ_MARK_XOR;
    for (int i = 0; i < 15; i++) x ^= m[i];
    m[15] = x;
    __asm__ volatile("sync" ::: "memory");
}

static int me_event(int ev, char *name, void *param, int *result)
{
    (void)name; (void)param; (void)result;
    if (g_exitmode) { az_mark_event((uint32_t)ev, 0); return 0; }   /* the EBOOT is going away */
    volatile Ps2rcMeBlock *b = g_me;
    if (!b) return 0;
    b->ev_ring[b->ev_n % 16u] = (uint32_t)ev;
    b->ev_n = b->ev_n + 1u;
    b->events = b->events + 1u;
    b->last_event = (uint32_t)ev;
    if ((uint32_t)ev == PS2RC_ME_EV_SUSPEND) {
        b->suspends = b->suspends + 1u;
        me_park(b);
    } else if ((uint32_t)ev == PS2RC_ME_EV_RESUME) {
        me_restart(b);
        b->resumes = b->resumes + 1u;                       /* last: the EBOOT's waits restart their deadlines */
    }
    return 0;
}

/* stands in for the OS ME driver's handler while we own the ME: its program is gone, nothing to save or wait for */
static int me_noop(int ev, char *name, void *param, int *result)
{
    (void)name; (void)param; (void)result;
    if (g_exitmode) az_mark_event((uint32_t)ev, 1);
    return 0;
}

static void me_uninstall(void)
{
    volatile Ps2rcMeBlock *b = g_me;
    if (g_mrpc && g_mrpc_orig && g_mrpc->handler == kfn(me_noop)) g_mrpc->handler = g_mrpc_orig;   /* the OS's back */
    g_mrpc = 0;
    if (b && b->installed && g_unreg) g_unreg(&g_handler);
    if (b) b->installed = 0u;
}

void ps2rc_exc_exitmode_main(void);              /* exc_main.c: exception handler back, counter thread told to quit */

/* on = 2 - exit mode (ps2rc_exc.h "exit mode"): the module stays loaded, the ME handlers stay installed and
   only record into the marker; the exception handler goes back to the kernel. The EBOOT calls it after the ME is held
   in reset (exit 1/6) and does not stop or unload the module afterwards. */
static int me_exitmode(volatile Ps2rcMeBlock *b)
{
    uint32_t state = 1u | (b->installed ? 2u : 0u) | (g_mrpc && g_mrpc->handler == kfn(me_noop) ? 4u : 0u);
    uint32_t reset = HW(ME_RESET_REG) & 0x04u;
    if (reset) az_mark_arm(b->exit_seq, state);    /* the reset-vector RAM only while the ME is held */
    g_exitmode = 1;
    b->live = 0u;
    ps2rc_exc_exitmode_main();
    g_me = 0;                                       /* no more writes into the EBOOT's block from here on */
    b->exitmode_rc = 0x415A0000u | state | (reset ? 0x100u : 0u);
    return 0;
}

/* kuKernelCall(hookfn, {block, on}): on = 1 installs (idempotent), 0 uninstalls, 2 = exit mode */
int ps2rc_me_hook(uint32_t blk, uint32_t on)
{
    volatile Ps2rcMeBlock *b = g_me;
    if (!b || blk != (uint32_t)b) return -4;
    if (on == 2u) return me_exitmode(b);
    if (!on) { me_uninstall(); return 0; }
    if (b->installed) return 0;
    if (!ps2rc_stub_linked((void *)ps2rc_find_function)) { b->hook_rc = -1; return -1; }
    PspSysEventHandler *(*refer)(void) =
        (PspSysEventHandler * (*)(void))find("sceSystemMemoryManager", "sceSysEventForKernel", NID_REFER);
    int (*reg)(PspSysEventHandler *) =
        (int (*)(PspSysEventHandler *))find("sceSystemMemoryManager", "sceSysEventForKernel", NID_REGISTER);
    g_unreg = (int (*)(PspSysEventHandler *))find("sceSystemMemoryManager", "sceSysEventForKernel", NID_UNREGISTER);
    if (!refer || !reg || !g_unreg) { b->hook_rc = -2; return -2; }
    g_clk = (void (*)(void))find("sceLowIO_Driver", "sceSysreg_driver", NID_ME_BUS_CLOCK_ENABLE);
    b->clk_fn = (uint32_t)g_clk;
    uint32_t gp;
    __asm__ volatile("move %0, $28" : "=r"(gp));
    g_handler.size = sizeof g_handler;
    g_handler.name = (char *)g_name;
    g_handler.type_mask = 0x01FFFF00;                   /* suspend + resume (+ speed change), as SceMeRpc */
    g_handler.handler = kfn(me_event);
    g_handler.r28 = (int)gp;
    g_handler.busy = 0;
    g_handler.next = 0;
    int r = reg(&g_handler);
    b->reg_rc = r;
    if (r < 0) { b->hook_rc = -3; return -3; }
    b->installed = 1u;
    /* the OS ME driver's handler: neutralised for the session (one word, restored in module_stop) */
    for (PspSysEventHandler *h = refer(); h; h = h->next) {
        if (h != &g_handler && same(h->name, "SceMeRpc")) {
            g_mrpc = h;
            g_mrpc_orig = h->handler;
            b->mrpc_orig = (uint32_t)h->handler;
            h->handler = kfn(me_noop);
            b->mrpc_found = 1u;
            break;
        }
    }
    b->hook_rc = 0;
    return 0;
}

/* module_start: take the EBOOT's block (only when this module stays resident) */
void ps2rc_me_take(uint32_t addr)
{
    if (!addr || (addr & 63u) || addr < 0x08800000u || addr >= 0x0C000000u) return;
    volatile Ps2rcMeBlock *b = (volatile Ps2rcMeBlock *)addr;
    b->version = 2u;                                    /* 2 = hookfn knows on = 2 (exit mode) */
    b->hookfn = (uint32_t)ps2rc_me_hook | 0x80000000u;
    g_me = b;
    b->magic = PS2RC_ME_MAGIC;                          /* last: the EBOOT trusts the block from here on */
}

/* module_stop (the exit sequence's "exit 5/6"): nothing of the kernel may point into this module afterwards */
void ps2rc_me_stop(void)
{
    if (!g_exitmode) me_uninstall();               /* after the exit mode the handlers stay (a module_stop
                                                      the kernel itself might run before its reboot must not hand
                                                      SceMeRpc back to the OS handler, which would wait forever) */
    g_me = 0;
}
