/* hle_main.c - platform-independent core of the PS2RC HLE runtime: logging front end, milestones, the call-trace
 * ring, the end-of-run report, guest memory helpers, COP0 / rt_fatal handling, the VCALLMS modes and the boot
 * sequence shared by the host harness (hle_host.c: main, ELF loading, signals) and the PSP (psp/hle_psp.c).
 *
 * Exit codes: 0 guest exited / limit reached, 3 fatal guest event (unsupported, unknown dispatch target...),
 *             4 harness error, 5 deadlock, 6 host signal, 7 watchdog.
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: profiler (iteration boundary, final tables, VU0 zone) */
#include "hle_hitch.h"                         /* frame-time spikes */

int hle_verbose;
uint8_t *hle_ram;
uint8_t *hle_spr;
uint32_t hle_ram_end = HLE_RAM_SIZE;
const char *hle_elf_path;
int hle_max_seconds = 600;
uint64_t hle_max_frames;
uint64_t hle_opt_mainloop_limit;
#ifdef HLE_HAVE_VU0_MICRO
int hle_opt_vcallms = VCALLMS_HLE;               /* the pack's VU0 micro-program HLE (hle_pack_vcallms) */
#else
int hle_opt_vcallms = VCALLMS_STRICT;
#endif
volatile uint64_t hle_progress;
uint64_t hle_stat_syscalls, hle_stat_mmio, hle_stat_hooks;
uint32_t hle_cop0[32];
static char exit_reason[160];

const char *hle_exit_reason(void) { return exit_reason; }
void hle_set_exit_reason(const char *r)
{
    if (!exit_reason[0]) snprintf(exit_reason, sizeof exit_reason, "%s", r);
}

void hle_logf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) {                                   /* truncated: keep the line structure */
        n = sizeof buf - 1;
        buf[n - 1] = '\n';
    }
    hle_plat_log_write(buf, (size_t)n);
}

/* ------------------------------------------------------------------ milestones / trace ring */
#define MAX_MS 128
static struct { char what[96]; uint64_t vtime, frame; double host; int thread; } ms[MAX_MS];
static int nms;

void hle_milestone(const char *what)
{
    for (int i = 0; i < nms; i++) if (strcmp(ms[i].what, what) == 0) return;
    if (nms < MAX_MS) {
        snprintf(ms[nms].what, sizeof ms[nms].what, "%s", what);
        ms[nms].vtime = hle_vtime;
        ms[nms].frame = hle_frame;
        ms[nms].host = hle_host_seconds();
        ms[nms].thread = hle_cur ? hle_cur->id : 0;
        nms++;
    }
    char line[256];
    snprintf(line, sizeof line, "MILESTONE %s (vtime %.3f s, frame %llu, host %.1f s, thread %d)\n", what,
             (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame, hle_host_seconds(), hle_cur ? hle_cur->id : 0);
    hle_plat_log_write(line, strlen(line));
    hle_plat_milestone(line);
    hle_plat_log_flush();
}

int hle_milestone_reached(const char *prefix)
{
    for (int i = 0; i < nms; i++) if (strncmp(ms[i].what, prefix, strlen(prefix)) == 0) return 1;
    return 0;
}

void hle_milestones_dump(void)
{
    LOG("milestones reached (%d):\n", nms);
    for (int i = 0; i < nms; i++)
        LOG("  %-60s vtime %8.3f s  frame %6llu  host %7.1f s  thread %d\n", ms[i].what,
            (double)ms[i].vtime / EE_HZ, (unsigned long long)ms[i].frame, ms[i].host, ms[i].thread);
}

#define TRACE_RING 48
static struct { const char *name; uint32_t addr, a[4]; uint64_t vtime; int thread; } ring[TRACE_RING];
static unsigned ring_pos;

void hle_trace_call(const char *name, uint32_t addr, R5900Ctx *c)
{
    unsigned k = ring_pos++ % TRACE_RING;
    ring[k].name = name;
    ring[k].addr = addr;
    for (int i = 0; i < 4; i++) ring[k].a[i] = (uint32_t)c->r[4 + i].d[0];
    ring[k].vtime = hle_vtime;
    HleThread *t = hle_thread_of(c);
    ring[k].thread = t ? t->id : 0;
    hle_stat_hooks++;
    VLOG("[%d] %s(%08x, %08x, %08x, %08x)\n", ring[k].thread, name, ring[k].a[0], ring[k].a[1], ring[k].a[2],
         ring[k].a[3]);
}

void hle_trace_ring_dump(void)
{
    unsigned n = ring_pos < TRACE_RING ? ring_pos : TRACE_RING;
    LOG("last %u hooked/traced calls (oldest first):\n", n);
    for (unsigned i = 0; i < n; i++) {
        unsigned k = (ring_pos - n + i) % TRACE_RING;
        LOG("  [thr %d] %-28s @%08x (%08x, %08x, %08x, %08x) vtime %.4f\n", ring[k].thread, ring[k].name,
            ring[k].addr, ring[k].a[0], ring[k].a[1], ring[k].a[2], ring[k].a[3], (double)ring[k].vtime / EE_HZ);
    }
}

void hle_backtrace_dump(R5900Ctx *c)
{
    hle_plat_backtrace();
    if (c) {
        LOG("guest regs: ra %08x sp %08x gp %08x v0 %08x v1 %08x a0 %08x a1 %08x a2 %08x a3 %08x t0 %08x s0 %08x "
            "s1 %08x s2 %08x\n", (uint32_t)c->r[31].d[0], (uint32_t)c->r[29].d[0], (uint32_t)c->r[28].d[0],
            (uint32_t)c->r[2].d[0], (uint32_t)c->r[3].d[0], (uint32_t)c->r[4].d[0], (uint32_t)c->r[5].d[0],
            (uint32_t)c->r[6].d[0], (uint32_t)c->r[7].d[0], (uint32_t)c->r[8].d[0], (uint32_t)c->r[16].d[0],
            (uint32_t)c->r[17].d[0], (uint32_t)c->r[18].d[0]);
    }
    if (hle_pack_crash_report) hle_pack_crash_report(c);    /* the pack's view (e.g. the script state) */
}

/* main-loop timing: real time between consecutive calls of the main-loop function (the pack's [runtime] main_loop) -
   the speed of the game */
static double ml_first, ml_last, ml_sum, ml_min = 1e9, ml_max, ml_recent[100];
static uint64_t ml_n;
static int ml_started;
void hle_mainloop_tick(void)
{
    hle_pz_iteration();
    hle_hitch_tick();                          /* frame-time histogram + slowest iterations */
    if (hle_pack_frame_tick) hle_pack_frame_tick();   /* the pack's per-iteration work (frame cap, A/B windows) */
    double now = hle_host_seconds();
    if (!ml_started) { ml_started = 1; ml_first = ml_last = now; return; }
    double d = now - ml_last;
    ml_last = now;
    ml_recent[ml_n % 100] = d;
    ml_n++;
    ml_sum += d;
    if (d < ml_min) ml_min = d;
    if (d > ml_max) ml_max = d;
}

void hle_mainloop_timing_report(void)
{
    if (!ml_n) return;
    unsigned k = ml_n < 100 ? (unsigned)ml_n : 100u;
    double r = 0;
    for (unsigned i = 0; i < k; i++) r += ml_recent[i];
    LOG("main-loop real time: first iteration at %.1f s, %llu intervals, avg %.1f ms, min %.1f ms, max %.1f ms, "
        "last %u avg %.1f ms (%.1f iterations/s)\n", ml_first, (unsigned long long)ml_n, 1000.0 * ml_sum / ml_n,
        1000.0 * ml_min, 1000.0 * ml_max, k, 1000.0 * r / k, k / (r > 0 ? r : 1));
    hle_hitch_report();
    if (hle_pack_timing_report) hle_pack_timing_report();
}

static uint64_t vcallms_count;
uint64_t hle_stat_vcallms_hle;                 /* counted by the pack's hle_pack_vcallms */
static void final_report(const char *why)
{
    hle_pz_report_final();                     /* first: the end-of-run dump below is not part of the last iteration */
    LOG("==== end of run: %s ====\n", why);
    hle_milestones_dump();
    hle_trace_ring_dump();
    hle_sched_dump();
    hle_stack_report();
    hle_mmio_report();
    hle_disc_report();
    hle_iop_report();
    hle_pack_report();                     /* the game pack's modules */
    if (vcallms_count) LOG("VCALLMS answered 'no hit' (experiment): %llu\n", (unsigned long long)vcallms_count);
    if (hle_stat_vcallms_hle) LOG("VCALLMS run by the VU0 micro HLE: %llu\n", (unsigned long long)hle_stat_vcallms_hle);
    LOG("main-loop iterations (0x%X calls): %llu\n", (unsigned)HLE_PACK_MAIN_LOOP_ADDR, (unsigned long long)hle_mainloop_iters);
    hle_mainloop_timing_report();
    LOG("wait-loop fast-forward (idle_skip=%d): %llu jumps, %.3f s of virtual time skipped\n", hle_opt_idle_skip,
        (unsigned long long)hle_stat_idle_skips, (double)hle_stat_idle_skipped_cycles / EE_HZ);
    {
        extern uint64_t hle_stat_fast_memcpy, hle_stat_fast_memset;
        LOG("native libc copies: memcpy %llu, memset %llu\n", (unsigned long long)hle_stat_fast_memcpy,
            (unsigned long long)hle_stat_fast_memset);
        extern uint64_t hle_stat_dispatch_miss;
        LOG("indirect-call cache misses: %llu\n", (unsigned long long)hle_stat_dispatch_miss);
    }
    LOG("stats: syscalls %llu, hooked/traced calls %llu, MMIO accesses %llu, interrupt handler calls %llu, "
        "thread switches %llu, loop back-edges ~%llu, host %.1f s\n", (unsigned long long)hle_stat_syscalls,
        (unsigned long long)hle_stat_hooks, (unsigned long long)hle_stat_mmio, (unsigned long long)hle_stat_intr,
        (unsigned long long)hle_stat_switches, (unsigned long long)hle_stat_backedges, hle_host_seconds());
    hle_plat_log_flush();
}

void hle_exit(int code)
{
    static int exiting;
    if (!exiting) {
        exiting = 1;
        char why[64];
        snprintf(why, sizeof why, "exit code %d", code);
        if (!exit_reason[0]) hle_set_exit_reason(code == 0 ? "limit or guest exit" : why);
        final_report(why);
    }
    hle_plat_exit(code);
}

void hle_die(R5900Ctx *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("FATAL: %s\n", buf);
    char r[160];
    snprintf(r, sizeof r, "fatal: %s", buf);
    hle_set_exit_reason(r);
    hle_backtrace_dump(c);
    hle_exit(3);
}

/* debug watch (host --watch HEX): a guest word checked at every safe point; a change is logged with the running
   call chain (the writer or a function close to it: safe points come every 1024 loop back-edges, syscalls, ...) */
uint32_t hle_watch_addr[4];
int hle_watch_n;
static uint32_t watch_last[4];
static unsigned watch_hits;
static void watch_check(void)
{
    for (int i = 0; i < hle_watch_n; i++) {
        uint32_t v = gw(hle_watch_addr[i]);
        if (v == watch_last[i]) continue;
        if (watch_hits++ < 64) {
            float f, g;
            memcpy(&f, &v, 4);
            memcpy(&g, &watch_last[i], 4);
            LOG("WATCH %08x: %08x (%g) -> %08x (%g) at vtime %.4f, thread %d%s\n", hle_watch_addr[i], watch_last[i], g,
                v, f, (double)hle_vtime / EE_HZ, hle_cur ? hle_cur->id : 0, hle_in_intr ? " (interrupt)" : "");
            hle_plat_backtrace();
        }
        watch_last[i] = v;
    }
}

static unsigned limit_tick;
volatile int hle_exit_requested;
void hle_check_limits(void)
{
    hle_progress++;
    if (hle_exit_requested) {                      /* build 30: HOME -> Exit (PSP exit callback, psp/hle_psp.c) */
        LOG("exit requested (HOME menu) at main-loop iteration %llu\n", (unsigned long long)hle_mainloop_iters);
        hle_set_exit_reason("HOME: exit requested by the user");
        hle_exit(0);
    }
    if (hle_watch_n) watch_check();
    if (hle_max_frames && hle_frame >= hle_max_frames) {
        LOG("frame limit reached (%llu)\n", (unsigned long long)hle_frame);
        hle_set_exit_reason("frame limit reached");
        hle_exit(0);
    }
    if (hle_opt_mainloop_limit && hle_mainloop_iters >= hle_opt_mainloop_limit) {
        LOG("main-loop iteration limit reached (%llu)\n", (unsigned long long)hle_mainloop_iters);
        hle_set_exit_reason("main-loop iteration limit reached");
        hle_exit(0);
    }
    if ((++limit_tick & 1023) == 0 && hle_host_seconds() > hle_max_seconds) {
        LOG("host time limit reached (%d s)\n", hle_max_seconds);
        hle_set_exit_reason("host time limit reached");
        hle_exit(0);
    }
}

/* ------------------------------------------------------------------ guest memory helpers */
void *gptr(uint32_t a, uint32_t n)
{
    if (a < hle_ram_end && a + n <= hle_ram_end) return hle_ram + a;
    uint32_t p = a & 0x1FFFFFFFu;                            /* KSEG / uncached mirrors */
    if ((a >> 28) == 2 || (a >> 28) == 3 || (a >> 28) == 8 || (a >> 28) == 9 || (a >> 28) == 0xA) {
        p = a & 0x0FFFFFFFu;
        if (p + n <= hle_ram_end) return hle_ram + p;
    }
    if (a >= 0x70000000u && a + n <= 0x70000000u + HLE_SPR_SIZE) return hle_spr + (a - 0x70000000u);
    LOG("FATAL: bad guest pointer %08x (+%u)\n", a, n);
    hle_die(NULL, "bad guest pointer %08x", a);
}

const char *gstr(uint32_t a)
{
    static char buf[8][512];
    static int k;
    char *b = buf[k++ & 7];
    int i = 0;
    for (; i < 511; i++) {
        uint32_t x = a + i;
        uint8_t ch = x < hle_ram_end ? hle_ram[x] : (x >= 0x70000000u && x < 0x70004000u ? hle_spr[x - 0x70000000u] : 0);
        if (!ch) break;
        b[i] = (char)ch;
    }
    b[i] = 0;
    return b;
}

void gcopy_out(uint32_t dst, const void *src, uint32_t n) { memcpy(gptr(dst, n), src, n); }
void gcopy_in(void *dst, uint32_t src, uint32_t n) { memcpy(dst, gptr(src, n), n); }

uint32_t hle_call(R5900Ctx *c, uint32_t addr, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 8; i++) c->r[4 + i].d[0] = (uint64_t)(int64_t)(int32_t)va_arg(ap, uint32_t);
    va_end(ap);
    uint64_t ra = c->r[31].d[0];
    c->r[31].d[0] = 0;
    rc_dispatch(c, addr);
    c->r[31].d[0] = ra;
    return (uint32_t)c->r[2].d[0];
}

/* ------------------------------------------------------------------ COP0 + rt_fatal */
#define ST_IE 0x1u
#define ST_EXL 0x2u
#define ST_EIE 0x10000u

void hle_cop0_init(void)
{
    hle_cop0[15] = 0x2E20;           /* PRId: R5900 */
    hle_cop0[16] = 0x00000440;       /* Config (cache sizes) */
}

static uint32_t cop0_status(void)
{
    uint32_t st = 0x70030C00u;       /* CU0-2, EDI, EIE, IM2/IM3 (INTC, DMAC) */
    if (hle_in_intr) return (st & ~ST_EIE) | ST_EXL;
    st |= ST_IE;
    if (hle_cur && hle_cur->intr_disabled) st &= ~ST_EIE;
    return st;
}

static uint32_t cop0_read(int rd)
{
    switch (rd) {
    case 12: return cop0_status();
    case 9:                          /* Count: EE cycles; reading it is a (cheap) safe point so poll loops progress */
        hle_time_charge(64);
        return (uint32_t)hle_vtime;
    default: return hle_cop0[rd & 31];
    }
}

/* Translated function at a guest address that the dispatch tables do not list:
   1. a function translated outside the tables (hle_fnmap.c: overlay static constructors after .text);
   2. a call target inside nop padding that function discovery attached to the function before it: e.g. an overlay
      function translated from 16 nops before its code - entering at the first nop is exact. */
/* build 21: JALR / indirect-call dispatch with a direct-mapped cache in front of the generated table search
   (rc_dispatch_table: a binary search over 11,874 entries = ~14 dependent loads, mostly D-cache misses on the PSP's
   16 KB cache; ~6,200 indirect calls per main-loop iteration). Entries are tagged with the overlay loaded in the
   window (-1 = main program), so an overlay swap never reuses a stale target. Misses and unknown targets take the
   generated path (rt_fatal -> hle_fn_lookup / nop aliases), unchanged. */
void rc_dispatch_table(R5900Ctx *c, uint32_t a);
#define RC_DCACHE 2048
static struct { uint32_t a; int32_t ov; rc_fn f; } rc_dcache[RC_DCACHE];
uint64_t hle_stat_dispatch_miss;
void rc_dispatch(R5900Ctx *c, uint32_t a)
{
    int32_t ov = a - rc_overlay_lo < rc_overlay_hi - rc_overlay_lo ? rc_overlay_current : -1;
    unsigned k = ((a >> 2) ^ (a >> 13)) & (RC_DCACHE - 1);
    if (rc_dcache[k].f && rc_dcache[k].a == a && rc_dcache[k].ov == ov) { rc_dcache[k].f(c); return; }
    hle_stat_dispatch_miss++;
    rc_fn f;
    if (ov >= 0) {
        rc_lookup_fn l = rc_overlay_lookup[ov & (RC_MAX_OVERLAYS - 1)];
        f = l ? l(a) : 0;
    } else {
        f = rc_main_lookup(a);
    }
    if (!f) { rc_dispatch_table(c, a); return; }
    rc_dcache[k].a = a;
    rc_dcache[k].ov = ov;
    rc_dcache[k].f = f;
    f(c);
}

rc_fn hle_fn_lookup(int ov, uint32_t a)
{
    for (int i = 0; i < hle_n_extra_fns; i++)
        if (hle_extra_fns[i].ov == ov && hle_extra_fns[i].addr == a) return hle_extra_fns[i].fn;
    if (ov) {
        rc_lookup_fn l = rc_overlay_lookup[ov & (RC_MAX_OVERLAYS - 1)];
        return l ? l(a) : NULL;
    }
    return rc_main_lookup(a);
}

static rc_fn nop_alias_lookup(int ov, uint32_t a);
static rc_fn nop_alias(int ov, uint32_t a)
{
    static struct { int ov; uint32_t a; rc_fn f; } cache[64];
    unsigned h = (a >> 2) & 63;
    if (cache[h].f && cache[h].a == a && cache[h].ov == ov) return cache[h].f;
    rc_fn f = nop_alias_lookup(ov, a);
    if (f) { cache[h].ov = ov; cache[h].a = a; cache[h].f = f; }
    return f;
}

static rc_fn nop_alias_lookup(int ov, uint32_t a)
{
    if (a & 3) return NULL;
    rc_fn f = hle_fn_lookup(ov, a);
    if (f) return f;
    uint32_t s = a;
    while (s > a - 1024 && s >= 4 && s - 4 < hle_ram_end && gw(s - 4) == 0) s -= 4;
    for (uint32_t x = s; x < a; x += 4) {
        f = hle_fn_lookup(ov, x);
        if (f) {
            static uint32_t logged[64];
            static int nlogged;
            int seen = 0;
            for (int i = 0; i < nlogged; i++) seen |= logged[i] == a;
            if (!seen && nlogged < 64) {
                char sym[48];
                if (ov) snprintf(sym, sizeof sym, "ov%d_fn_%08x", ov, x);
                else snprintf(sym, sizeof sym, "fn_%08x", x);
                logged[nlogged++] = a;
                LOG("call target %08x%s is inside the nop padding of %s - entered there (discovery: entry "
                    "%08x missing)\n", a, ov ? " (overlay)" : "", sym, a);
            }
            return f;
        }
    }
    return NULL;
}

void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value)
{
    if (strcmp(what, "mfc0") == 0) {
        int rt = (value >> 16) & 31, rd = (value >> 11) & 31;
        uint32_t v = cop0_read(rd);
        if (rt) c->r[rt].d[0] = (uint64_t)(int64_t)(int32_t)v;
        VVLOG("mfc0 $%d <- cop0[%d] = %08x at %08x\n", rt, rd, v, pc);
        return;
    }
    if (strcmp(what, "mtc0") == 0) {
        int rt = (value >> 16) & 31, rd = (value >> 11) & 31;
        uint32_t v = (uint32_t)c->r[rt].d[0];
        if (rd == 12) {
            if (!hle_in_intr && hle_cur) hle_cur->intr_disabled = !(v & ST_EIE);
        } else {
            hle_cop0[rd] = v;
        }
        VLOG("mtc0 cop0[%d] <- %08x at %08x\n", rd, v, pc);
        if (rd == 12) hle_deliver_interrupts();
        return;
    }
    if (strcmp(what, "di") == 0) {
        if (!hle_in_intr && hle_cur) hle_cur->intr_disabled = 1;
        return;
    }
    if (strcmp(what, "ei") == 0) {
        if (!hle_in_intr && hle_cur) {
            hle_cur->intr_disabled = 0;
            hle_safe_point(c, 16);
        }
        return;
    }
    if (strcmp(what, "break") == 0) {
        LOG("guest BREAK %u at %08x (thread %d) - continuing\n", (value >> 6) & 0xFFFFF, pc,
            hle_cur ? hle_cur->id : 0);
        return;
    }
    if (strcmp(what, "trap") == 0) {
        LOG("guest TRAP at %08x (insn %08x) - continuing\n", pc, value);
        return;
    }
    if (strcmp(what, "unsupported") == 0)
        hle_die(c, "unsupported (stubbed) guest function %08x called (VU0 macro / COP0 bc0 / MMI not translated)",
                pc);
    if (strcmp(what, "dispatch") == 0 || strcmp(what, "overlay") == 0) {
        int ov = what[0] == 'o' ? (int)pc : 0;
        rc_fn f = nop_alias(ov, value);
        if (f) { f(c); return; }
        if (ov) hle_die(c, "call into the overlay window at %08x but overlay %d has no function there", value, ov);
        hle_die(c, "JALR/indirect jump to unknown guest address %08x", value);
    }
    if (strcmp(what, "vcallms") == 0)
        hle_die(c, "VCALLMS at %08x: VU0 micro subroutine at VU0 address %04x (VU0 micro mode is not emulated)", pc,
                value);
    hle_die(c, "rt_fatal(%s) at %08x value %08x", what, pc, value);
}

/* ------------------------------------------------------------------ VCALLMS modes
   hle (default when the pack provides hle_pack_vcallms and defines HLE_HAVE_VU0_MICRO): the pack's HLE of its VU0
     micro programs.
   strict: VU0 micro mode is not emulated - the first VCALLMS is the documented blocker (rt_fatal).
   nohit (EXPERIMENT ONLY): for VCALLMS sites that read a hit flag from VI01 after the call (collision primitives),
     answers "no hit" (VI01 = 0) without running the microprogram, so the boot can be followed past the first
     VCALLMS to find the NEXT blockers. The results are wrong; never use it for anything else. */
static void vcallms_nohit(R5900Ctx *c, uint32_t pc, uint32_t addr)
{
    if (vcallms_count++ < 8) LOG("VCALLMS at %08x (VU0 %04x) answered 'no hit' (--vcallms-nohit experiment)\n", pc, addr);
    c->vu0.vi[1] = 0;
}


void hle_set_vcallms_mode(int mode)
{
    hle_opt_vcallms = mode;
    rt_vu0_callms_hook = NULL;
    if (mode == VCALLMS_NOHIT) rt_vu0_callms_hook = vcallms_nohit;
    if (mode == VCALLMS_HLE) {
        if (hle_pack_vcallms) rt_vu0_callms_hook = hle_pack_vcallms;   /* the pack's VU0 micro-program HLE */
        else {
            LOG("VCALLMS mode 'hle' requested but this game pack has no VU0 micro-program HLE - strict\n");
            hle_opt_vcallms = VCALLMS_STRICT;
        }
    }
}

int hle_selftest_pending;
/* the pack's self-test (host --selftest; weak default: none) */
__attribute__((weak)) void hle_selftest_run(R5900Ctx *c)
{
    (void)c;
    LOG("SELFTEST: this game pack has no self-test\n");
}

/* ------------------------------------------------------------------ boot (after guest RAM, ELF data and disc map) */
/* the game pack's interface (hle.h): empty defaults for a pack without modules */
__attribute__((weak)) void hle_pack_boot_init(void) {}
__attribute__((weak)) void hle_pack_report(void) {}
__attribute__((weak)) void hle_pack_iop_init(void) {}
__attribute__((weak)) void hle_pack_iop_report(void) {}

void hle_boot_init(uint32_t entry)
{
    /* one line per step: on the PSP each is committed to session.log at once (a crash names the step) */
    hle_spr = hle_ram;                                       /* scratchpad alias: guest RAM 0x0000-0x3FFF */
    LOG("boot init: COP0, MMIO register model\n");
    hle_cop0_init();
    hle_mmio_init();
    LOG("boot init: syscalls, IOP\n");
    hle_syscalls_install();
    hle_iop_init();
    LOG("boot init: render hooks, overlays\n");
    hle_pack_boot_init();                                    /* the game pack's modules and its overlay table */
    LOG("boot init: scheduler (guest thread 1 at %08x)\n", entry);
    hle_sched_init(entry);
}
