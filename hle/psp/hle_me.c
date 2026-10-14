/* hle_me.c - Media Engine bring-up: see hle_me.h. PSP only; the pack's audio split uses it (its ME mixer loop), and so
 * may a standalone heartbeat test EBOOT. */
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)   /* only the audio split uses the ME */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include "ps2rc_kcall.h"                                    /* kuKernelCall of the CFW (own import) */
#include <string.h>
#include <stdio.h>
#include "hle_me.h"
#include "ps2rc_exc.h"                                       /* round 4: ps2rc_me_block (sleep support of the PRX) */

#define ME_RESET_VECTOR 0xBFC00000u
#define ME_RESET_REG    0xBC10004Cu       /* bit 2 = ME held in reset */
#define ME_RESET_BIT    0x04u
#define HW(a)           (*(volatile uint32_t *)(a))

/* boot block, read by the stub through the uncached alias (the ME's D-cache is not trustworthy at reset):
   [0] control block (uncached) [1] stack top [2] work function (kseg0) [3] private region start [4] its end
   [5] register variant (hle_me_set_regs) [6] [7] 0xBC100040 / 0xBC100050 as the stub found them (written by the ME)
   [8..11] the exception record  [12] cold start (round 4): 1 = initialise the ME's caches with Index Store Tag (after a
   power cycle - set by ps2rc_exc.prx at resume, or hle_me_set_cold for the heartbeat's test) */
uint32_t hle_me_bootblk[16] __attribute__((aligned(64)));
static int g_regs_or, g_cold;

/* The reset stub: copied to 0xBFC00000, runs uncached in kernel mode on the ME. No stack until $sp is loaded at the
   end; only $k0/$k1/$t0-$t3 are used. */
__asm__(
    "    .section .text.hle_me_stub,\"ax\",@progbits\n"
    "    .align 4\n"
    "    .globl hle_me_stub\n"
    "    .globl hle_me_stub_end\n"
    "    .set push\n"
    "    .set noreorder\n"
    "    .set noat\n"
    "hle_me_stub:\n"
    "    la    $t0, hle_me_bootblk\n"
    "    lui   $t1, 0x4000\n"
    "    or    $t0, $t0, $t1\n"               /* boot block through the uncached alias */
    "    lw    $t2, 20($t0)\n"                /* [5] register variant: 0 = plain writes (proven), 1 = OR */
    "    lui   $k0, 0xBC10\n"
    "    lw    $t3, 0x40($k0)\n"
    "    sw    $t3, 24($t0)\n"                /* [6] = 0xBC100040 as found */
    "    li    $k1, 2\n"
    "    beq   $t2, $zero, 5f\n"
    "    nop\n"
    "    or    $k1, $k1, $t3\n"
    "5:  sw    $k1, 0x40($k0)\n"              /* 0xBC100040 = 2 (| old): the ME may address all 64 MB */
    "    lw    $t3, 0x50($k0)\n"
    "    sw    $t3, 28($t0)\n"                /* [7] = 0xBC100050 as found */
    "    li    $k1, 6\n"
    "    beq   $t2, $zero, 6f\n"
    "    nop\n"
    "    or    $k1, $k1, $t3\n"
    "6:  sw    $k1, 0x50($k0)\n"              /* 0xBC100050 = 6 (| old): bus clocks RegA / RegB */
    "    li    $k1, -1\n"
    "    sw    $k1, 0x04($k0)\n"              /* 0xBC100004: NMI flags */
    "    sync\n"
    "    lui   $k0, 0x3000\n"
    "    mtc0  $k0, $12\n"                    /* Status: CU0|CU1 (FPU usable), BEV=0, interrupts off */
    "    sync\n"
    "    la    $k0, hle_me_fault\n"           /* exception handler (Allegrex CP0 $25, as mcidclan's core sets it): */
    "    lui   $k1, 0x8000\n"                 /* any exception on the ME records itself and halts - no wild code */
    "    or    $k0, $k0, $k1\n"
    "    mtc0  $k0, $25\n"
    "    sync\n"
    "    lw    $t2, 48($t0)\n"                /* [12] cold start: the caches came back from a power cycle undefined */
    "    beq   $t2, $zero, 8f\n"             /* (their state after a power cycle is not defined) - initialise them */
    "    nop\n"                               /* both with Index Store Tag and TagLo/TagHi = 0, as the OS's ME boot */
    "    mtc0  $zero, $28\n"                  /* code does (uofw me_wrapper/init.S): nothing is written back */
    "    mtc0  $zero, $29\n"
    "    sync\n"
    "    li    $k0, 16384\n"
    "9:  addiu $k0, $k0, -64\n"
    "    cache 0x01, 0($k0)\n"
    "    cache 0x11, 0($k0)\n"
    "    bne   $k0, $zero, 9b\n"
    "    nop\n"
    "    b     10f\n"
    "    nop\n"
    "8:  li    $k0, 0\n"                      /* I-cache: index invalidate all (8 KB of indexes, both ways) */
    "    li    $k1, 8192\n"
    "1:  cache 0x04, 0($k0)\n"
    "    cache 0x04, 0($k0)\n"
    "    addiu $k0, $k0, 64\n"
    "    bne   $k0, $k1, 1b\n"
    "    nop\n"
    "    lw    $t2, 12($t0)\n"                /* D-cache: DISCARD (hit invalidate, no writeback) the private region */
    "    lw    $t3, 16($t0)\n"
    "2:  sltu  $k0, $t2, $t3\n"
    "    beq   $k0, $zero, 3f\n"
    "    nop\n"
    "    cache 0x19, 0($t2)\n"
    "    b     2b\n"
    "    addiu $t2, $t2, 64\n"
    "3:  li    $k0, 0\n"                      /* D-cache: index writeback-invalidate the rest */
    "    li    $k1, 8192\n"
    "4:  cache 0x14, 0($k0)\n"
    "    cache 0x14, 0($k0)\n"
    "    addiu $k0, $k0, 64\n"
    "    bne   $k0, $k1, 4b\n"
    "    nop\n"
    "10: sync\n"
    "    lw    $sp, 4($t0)\n"
    "    lw    $a0, 0($t0)\n"
    "    lw    $k1, 8($t0)\n"
    "    la    $gp, _gp\n"
    "    jr    $k1\n"
    "    nop\n"
    "hle_me_stub_end:\n"
    "    nop\n"
    /* the ME's exception handler (runs at its kseg0 alias): Cause / EPC / BadVAddr + a marker into the boot block
       [8..11] through the uncached alias, then HALT for good. The main core sees the stall and logs these words. */
    "    .align 4\n"
    "    .globl hle_me_fault\n"
    "hle_me_fault:\n"
    "    la    $k0, hle_me_bootblk\n"
    "    lui   $k1, 0x4000\n"
    "    or    $k0, $k0, $k1\n"
    "    mfc0  $k1, $13\n"
    "    sw    $k1, 32($k0)\n"
    "    mfc0  $k1, $14\n"
    "    sw    $k1, 36($k0)\n"
    "    mfc0  $k1, $8\n"
    "    sw    $k1, 40($k0)\n"
    "    lui   $k1, 0xDEAD\n"
    "    ori   $k1, $k1, 0xBEEF\n"
    "    sw    $k1, 44($k0)\n"
    "    sync\n"
    "7:  .word 0x70000000\n"
    "    b     7b\n"
    "    nop\n"
    "    .set pop\n"
    "    .text\n");
extern char hle_me_stub[], hle_me_stub_end[];

static volatile HleMeHdr *g_ctl;
static int g_running;
static int g_sleep_hook;                                     /* round 4: ps2rc_exc.prx's sleep hook is installed */
static int g_lock_rc = 1;                                    /* scePowerLock at the first takeover (hle_me.h) */
static uint32_t g_reset_seen = 0xFFFFFFFFu;

/* ------------------------------------------------------------------ kernel-mode helpers (kuKernelCall) */
static int k_start(uint32_t a1, uint32_t a2)
{
    (void)a1; (void)a2;
    volatile uint32_t *dst = (volatile uint32_t *)ME_RESET_VECTOR;
    const uint32_t *src = (const uint32_t *)hle_me_stub;
    uint32_t n = (uint32_t)(hle_me_stub_end - hle_me_stub + 3) / 4u + 1u;
    uint32_t r = HW(ME_RESET_REG);
    if (g_reset_seen == 0xFFFFFFFFu) g_reset_seen = r;
    HW(ME_RESET_REG) = r | ME_RESET_BIT;                     /* hold the ME (it may be running an older copy) */
    __asm__ volatile("sync");
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    HW(ME_RESET_REG) = r | ME_RESET_BIT;
    HW(ME_RESET_REG) = r & ~ME_RESET_BIT;                    /* release: the ME starts at 0xBFC00000 */
    __asm__ volatile("sync");
    return 0;
}

static int k_park(uint32_t a1, uint32_t a2)
{
    (void)a1; (void)a2;
    HW(ME_RESET_REG) = HW(ME_RESET_REG) | ME_RESET_BIT;      /* held in reset until the next k_start */
    __asm__ volatile("sync");
    return 0;
}

static int kcall(int (*fn)(uint32_t, uint32_t))
{
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    return ps2rc_kcall((void *)fn, &a);
}

static int is_emulator(void)
{
    int out = 0;
    return sceIoDevctl("kemulator:", 0x00000003, NULL, 0, &out, sizeof out) >= 0;
}

int hle_me_start(void (*work)(volatile HleMeHdr *ctl), volatile HleMeHdr *ctl, void *region, uint32_t region_bytes,
                 uint32_t stack_bytes, char *why, int why_n)
{
    if (why && why_n) why[0] = 0;
    if (is_emulator()) {
        if (why) snprintf(why, why_n, "emulator (PPSSPP does not emulate the Media Engine)");
        return 0;
    }
    if (((uintptr_t)region & 63u) || (region_bytes & 63u) || stack_bytes > region_bytes) {
        if (why) snprintf(why, why_n, "bad region %p + %u", region, (unsigned)region_bytes);
        return 0;
    }
    if (g_running) hle_me_stop(50);
    uintptr_t rs = (uintptr_t)region & 0x3FFFFFFFu, re = rs + region_bytes;
    hle_me_bootblk[0] = (uint32_t)(uintptr_t)ctl;
    hle_me_bootblk[1] = (uint32_t)(re - 16u) | 0;            /* stack top (cached user address, inside the region) */
    hle_me_bootblk[2] = (uint32_t)(uintptr_t)work | 0x80000000u;
    hle_me_bootblk[3] = (uint32_t)rs;
    hle_me_bootblk[4] = (uint32_t)re;
    hle_me_bootblk[5] = (uint32_t)g_regs_or;
    hle_me_bootblk[6] = hle_me_bootblk[7] = 0xFFFFFFFFu;
    hle_me_bootblk[8] = hle_me_bootblk[9] = hle_me_bootblk[10] = hle_me_bootblk[11] = 0;
    {   /* cold start (Index Store Tag) when asked, or when the PSP slept since the last start: the ME's D-cache
           came back undefined - a writeback of it would scatter garbage over RAM */
        static uint32_t seen_resumes;
        uint32_t rs_now = ps2rc_me_block.resumes;
        hle_me_bootblk[12] = (uint32_t)(g_cold || (g_sleep_hook && rs_now != seen_resumes));
        seen_resumes = rs_now;
    }
    ctl->magic = HLE_ME_MAGIC;
    ctl->state = 0;
    ctl->exit_req = 0;
    ctl->fault = 0;
    uint32_t hb0 = ctl->heartbeat, boots0 = ctl->boots;
    volatile Ps2rcMeBlock *mb = &ps2rc_me_block;
    if (g_sleep_hook) {                                      /* round 4: the PRX parks / restarts the ME at sleep */
        mb->live = 0;
        mb->ctl = (uint32_t)(uintptr_t)ctl;
        __asm__ volatile("sync" ::: "memory");
    } else if (g_lock_rc == 1) {
        g_lock_rc = scePowerLock(0);                         /* from now on a suspend must not run (hle_me.h) */
    }
    int r = kcall(k_start);                                  /* k_start writes the caches back (boot block, region) */
    if (r < 0) {
        if (why) snprintf(why, why_n, "kuKernelCall refused (%08x) - kubridge (ARK-4) missing?", (unsigned)r);
        return 0;
    }
    g_ctl = ctl;
    for (int i = 0; i < 20; i++) {                            /* <= 100 ms: the loop must run and count */
        sceKernelDelayThread(5000);
        if (ctl->boots != boots0 && ctl->state == 1 && ctl->heartbeat != hb0) {
            uint32_t a = ctl->heartbeat;
            sceKernelDelayThread(2000);
            if (ctl->heartbeat != a) { g_running = 1; mb->live = (uint32_t)g_sleep_hook; return 1; }
        }
    }
    if (why) snprintf(why, why_n, "heartbeat DEAD (boots %u->%u, state %u, heartbeat %u->%u, reset reg was %08x)",
                      (unsigned)boots0, (unsigned)ctl->boots, (unsigned)ctl->state, (unsigned)hb0,
                      (unsigned)ctl->heartbeat, (unsigned)g_reset_seen);
    kcall(k_park);
    return 0;
}

int hle_me_stop(int timeout_ms)
{
    int acked = 0;
    ps2rc_me_block.live = 0;
    __asm__ volatile("sync" ::: "memory");
    if (g_ctl && g_running) {
        g_ctl->exit_req = 1;
        for (int i = 0; i < timeout_ms; i++) {
            if (g_ctl->state == 2) { acked = 1; break; }
            sceKernelDelayThread(1000);
        }
    }
    if (g_ctl) kcall(k_park);
    g_running = 0;
    return acked;
}

int hle_me_park(void)
{
    ps2rc_me_block.live = 0;
    g_running = 0;
    return kcall(k_park);
}

int hle_me_running(void) { return g_running; }
volatile HleMeHdr *hle_me_owner(void) { return g_running ? g_ctl : 0; }
void hle_me_set_regs(int or_mode) { g_regs_or = or_mode ? 1 : 0; }
void hle_me_regs_found(uint32_t *r40, uint32_t *r50)
{
    volatile uint32_t *b = (volatile uint32_t *)((uintptr_t)hle_me_bootblk | 0x40000000u);
    *r40 = b[6];
    *r50 = b[7];
}
uint32_t hle_me_reset_reg_seen(void) { return g_reset_seen; }
int hle_me_exception(uint32_t *cause, uint32_t *epc, uint32_t *badvaddr)
{
    volatile uint32_t *k = (volatile uint32_t *)((uintptr_t)hle_me_bootblk | 0x40000000u);
    *cause = k[8];
    *epc = k[9];
    *badvaddr = k[10];
    return k[11] == 0xDEADBEEFu;
}

/* ------------------------------------------------------------------ sleep / power switch */
static volatile uint32_t pw_parks, pw_resumes, pw_switches, pw_flags;
static int pw_status = 1;                                    /* 1 = not started */
static int pw_slot = -1;                                     /* the slot scePowerRegisterCallback gave */

static int power_cb(int unknown, int flags, void *arg)
{
    (void)unknown; (void)arg;
    pw_flags = (uint32_t)flags;
    if (flags & PSP_POWER_CB_POWER_SWITCH) pw_switches++;
    /* a power-down under way (it got past the lock), or the switch with no lock: hold the ME in reset at least -
       the OS's own ME driver will still wait for its ME program (hle_me.h), nothing here can answer it */
    if (!g_sleep_hook &&
        ((flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) ||
         ((flags & PSP_POWER_CB_POWER_SWITCH) && g_lock_rc < 0))) {
        if (g_running || g_ctl) {
            g_running = 0;
            kcall(k_park);
            pw_parks++;
        }
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) pw_resumes++;
    return 0;
}

static int power_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    SceUID cb = sceKernelCreateCallback("ps2rc_me_pwr", power_cb, NULL);
    pw_status = cb < 0 ? cb : scePowerRegisterCallback(-1, cb);
    if (pw_status >= 0) pw_slot = pw_status;                  /* -1 asks for a free slot: the result is its number */
    if (pw_status < 0 && cb >= 0) { pw_status = scePowerRegisterCallback(0, cb); if (pw_status >= 0) pw_slot = 0; }
    sceKernelSleepThreadCB();
    return 0;
}

int hle_me_power_init(void)
{
    if (pw_status != 1) return pw_status;
    pw_status = 0x7FFFFFFF;                                  /* starting */
    SceUID th = sceKernelCreateThread("ps2rc_me_pwr", power_thread, 0x11, 0x1000, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) { pw_status = th < 0 ? th : -1; return pw_status; }
    for (int i = 0; i < 20 && pw_status == 0x7FFFFFFF; i++) sceKernelDelayThread(1000);
    return pw_status;
}

/* (az_exit=1): the power callback out before sceKernelExitGame; 0x7FFFFFFF = none registered, else the
   scePowerUnregisterCallback result. *lock_held = 1 when this session holds the no-hook scePowerLock: it is NOT
   released here (a power switch / auto-sleep held off by it would fire at the unlock, in the middle of the exit, with
   the OS's ME program gone - hle_me.h); the kernel reboot of sceKernelExitGame clears it. */
int hle_me_power_shutdown(int *lock_held)
{
    *lock_held = g_lock_rc == 0;
    if (pw_slot < 0) return 0x7FFFFFFF;
    int r = scePowerUnregisterCallback(pw_slot);
    pw_slot = -1;
    return r;
}

void hle_me_power_events(HleMePower *out)
{
    out->parks = pw_parks;
    out->resumes = pw_resumes;
    out->switches = pw_switches;
    out->last_flags = pw_flags;
}

int hle_me_power_lock_rc(void) { return g_lock_rc; }

/* ------------------------------------------------------------------ round 4: sleep with the ME taken over */
void hle_me_set_cold(int on) { g_cold = on ? 1 : 0; }

int hle_me_sleep_enable(char *why, int why_n)
{
    volatile Ps2rcMeBlock *b = &ps2rc_me_block;
    if (why && why_n) why[0] = 0;
    if (g_sleep_hook) return 1;
    if (is_emulator()) { if (why) snprintf(why, why_n, "emulator"); return 0; }
    if (b->magic != PS2RC_ME_MAGIC || !b->hookfn) {
        if (why) snprintf(why, why_n, "ps2rc_exc.prx not resident or older than round 4 (block magic %08x)",
                          (unsigned)b->magic);
        return 0;
    }
    b->stub = (uint32_t)(uintptr_t)hle_me_stub;
    b->stub_words = (uint32_t)(hle_me_stub_end - hle_me_stub + 3) / 4u + 1u;
    b->bootblk = (uint32_t)(uintptr_t)hle_me_bootblk;
    b->live = 0;
    b->ctl = 0;
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    a.arg[0] = (uint32_t)(uintptr_t)b;
    a.arg[1] = 1;
    int r = ps2rc_kcall((void *)(uintptr_t)b->hookfn, &a);
    if (r < 0 || (int)a.ret[0] < 0 || !b->installed) {
        if (why) snprintf(why, why_n, "hook refused: kuKernelCall %08x, hook %d (step %d), register %08x",
                          (unsigned)r, (int)a.ret[0], (int)b->hook_rc, (unsigned)b->reg_rc);
        return 0;
    }
    g_sleep_hook = 1;
    if (why) {
        char h[48];
        if (b->mrpc_found) snprintf(h, sizeof h, "replaced (was %08x)", (unsigned)b->mrpc_orig);
        else snprintf(h, sizeof h, "not loaded");
        snprintf(why, why_n, "kernel hook installed (register %08x; OS ME driver handler %s; bus-clock fn %s)",
                 (unsigned)b->reg_rc, h, b->clk_fn ? "found" : "not found");
    }
    return 1;
}

int hle_me_sleep_hooked(void) { return g_sleep_hook; }

static uint32_t g_epoch0;
volatile uint32_t *hle_me_resume_epoch(void)
{
    return g_sleep_hook ? &ps2rc_me_block.resumes : &g_epoch0;
}

void hle_me_sleep_info(char *out, int n)
{
    volatile Ps2rcMeBlock *b = &ps2rc_me_block;
    snprintf(out, n, "sleep hook %s: suspends %u, resumes %u, last park %s in %u us, restarts %u, reset reg at resume "
             "%08x, bus clocks %08x / %08x -> %08x, events %u (last %05x)", g_sleep_hook ? "on" : "off",
             (unsigned)b->suspends, (unsigned)b->resumes, b->park_ack ? "acknowledged" : "NOT acknowledged",
             (unsigned)b->park_us, (unsigned)b->restarts, (unsigned)b->reset_reg, (unsigned)b->busclk_suspend,
             (unsigned)b->busclk_resume, (unsigned)b->busclk_after, (unsigned)b->events, (unsigned)b->last_event);
}

#else
typedef int hle_me_unused;
#endif
