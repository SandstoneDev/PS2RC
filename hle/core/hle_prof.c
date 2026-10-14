/* hle_prof.c - the profiler core: zone timing, summaries, final tables, prof.bin capture, and the hardware-counter
 * report (the counters themselves come from the platform: hle/psp/hle_prof_psp.c).
 * Design and how to read the output: hle_prof.h.
 *
 * Attribution: every enter/leave/switch is an "event" that reads the clock once and gives the time since the previous
 * event to the zone on top of the running guest thread's stack (its self time; an empty stack = the thread's root
 * "thread N"). Each guest thread has its own clock (advanced only while it runs), so a zone's inclusive time is its
 * thread's clock at leave minus at enter - time in other threads is not counted. Recursion: inclusive time is added
 * for the outermost activation only. Accounting is per (parent zone, zone) edge, so the tree shows where each zone
 * was called from. Interrupt handlers run on the interrupted thread inside the "hle.intr" zone.
 * Phases: from hle_pz_init to the first main-loop iteration = boot (a short table), then the main loop (everything is
 * reset at its first iteration; per-iteration numbers divide by the number of complete iterations).
 * Integer fixed point everywhere in the zone and counter arithmetic (the game thread runs with the guest FPU mode,
 * round-to-zero + flush; no float compare can ever see an inf - on the PSP an FPU exception there powered the console
 * off).
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"

#ifdef HLE_PSP
#include <pspkernel.h>
#define PZ_TPS 1000000ull                      /* sceKernelGetSystemTimeLow: microseconds */
static uint32_t pz_lo;
static uint64_t pz_hi;
static inline uint64_t pz_now(void)            /* 32-bit us extended to 64 bits (game thread only) */
{
    uint32_t t = sceKernelGetSystemTimeLow();
    if (t < pz_lo) pz_hi += 1ull << 32;
    pz_lo = t;
    return pz_hi | t;
}
#define PZ_CLOCK_NAME "sceKernelGetSystemTimeLow"
#else
#include <time.h>
#define PZ_TPS 1000000000ull                   /* host: nanoseconds */
static inline uint64_t pz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define PZ_CLOCK_NAME "clock_gettime(MONOTONIC)"
#endif

#define PZ_HW_N 20
#define PZ_EDGES 1024
#define PZ_DEPTH 48
#define PZ_UNTRACKED (PZ_THREADS - 1)          /* guest thread ids >= 15: time counted, zones ignored */
#define PZ_HWZ 16                              /* zones with PZF_HW */
#define PZ_DOORS_PER_ITER 64

volatile int hle_pz_on;
int hle_pz_opt_on, hle_pz_opt_hw, hle_pz_opt_summary_s = 10, hle_pz_opt_summary_ms, hle_pz_opt_capture;

typedef struct { uint16_t parent, z; int32_t next; uint64_t calls, incl, self; } PzEdge;
typedef struct { uint16_t z, e; uint8_t outer, hw; uint64_t tc0; } PzFrame;
typedef struct { uint64_t calls, timed, ticks; } PzSamp;

static unsigned char pz_flags[PZ_MAX];
static uint16_t pz_rate[PZ_MAX];
static PzEdge pz_edge[PZ_EDGES];
static int pz_nedge;
static uint32_t pz_edge_full;
static int32_t pz_ehead[PZ_MAX];
static int pz_root[PZ_THREADS];
static PzFrame pz_stk[PZ_THREADS][PZ_DEPTH];
static int pz_sp[PZ_THREADS];
static uint64_t pz_tc[PZ_THREADS];
static int pz_slot, pz_top;
static uint64_t pz_last;
static uint16_t pz_active[PZ_MAX];
static uint64_t pz_zincl[PZ_MAX];
static PzSamp pz_samp[PZ_MAX];
static uint64_t pz_events, pz_sreads;          /* clock reads of the tree / of sampled zones */
static uint32_t pz_overflow, pz_mismatch;
static uint32_t pz_timer_ns, pz_pair_ns;       /* calibration: one clock read, one empty enter+leave */

/* phases / windows */
static int pz_ml;                              /* main loop started */
static uint64_t pz_iters;                      /* main-loop iteration starts since the phase began */
static uint64_t pz_t_init, pz_ml_t0, pz_win_t0, pz_iter_t0;
static uint64_t pz_t_init_host10;           /* hle_host_seconds() x 10 at pz_t_init: windows in boot-relative s */
static uint64_t pz_win_iters0;
static uint64_t pz_tc_ml0[PZ_THREADS];
static uint64_t pz_w_self[PZ_MAX], pz_w_calls[PZ_MAX], pz_w_samp_calls[PZ_MAX], pz_w_samp_ticks[PZ_MAX],
    pz_w_samp_timed[PZ_MAX];
static uint64_t pz_w_events, pz_w_sreads;

/* hardware counters */
static int pz_hw;                              /* background sums available */
static int pz_door;                            /* synchronous door available (prof_hw=2) */
static uint32_t pz_door_ns;                    /* calibrated cost of one door call */
static uint64_t pz_hw_ml0[PZ_HW_N], pz_hw_ml0_us, pz_hw_w0[PZ_HW_N], pz_hw_w0_us;
static int16_t pz_hwslot[PZ_MAX];
static int pz_nhwz;
static uint32_t pz_hwsnap[PZ_HWZ][PZ_HW_N];
static uint64_t pz_hwacc[PZ_HWZ][PZ_HW_N], pz_hwn[PZ_HWZ];
static uint8_t pz_hwopen[PZ_HWZ];
static uint64_t pz_doors, pz_doors_failed, pz_doors_skipped;
static unsigned pz_doors_iter;
static uint32_t pz_iter_raw[PZ_HW_N];
static int pz_iter_raw_ok;

/* the slowest main-loop iterations with the zones that took their time (any run length, no
   capture needed): per iteration one pass over the edge table gives every zone's self time of that iteration */
#define PZ_SLOW 12
#define PZ_SLOWZ 6
typedef struct { uint64_t it, ticks, s[PZ_SLOWZ]; uint16_t z[PZ_SLOWZ]; } PzSlow;
static uint64_t pz_it_prev[PZ_MAX], pz_it_cur[PZ_MAX];
static PzSlow pz_slow[PZ_SLOW], pz_wslow;
static int pz_nslow;
static void pz_slow_put(const char *label, const PzSlow *s);

/* capture (prof.bin) */
static uint32_t *pz_cap;
static uint8_t *pz_cap_base;
#define PZ_CAP_HDR 64
static unsigned pz_cap_n, pz_cap_max, pz_cap_words, pz_ncz;
static uint16_t pz_cz[PZ_MAX];                 /* dense capture zone list */
static uint64_t pz_cap_prev[PZ_MAX];

/* ------------------------------------------------------------------ names */
static const char *const pz_hle_name[] = {
#define PZ_X_(id, name, fl, rate) name,
    HLE_ZONE_LIST(PZ_X_)
#undef PZ_X_
};
static const unsigned char pz_hle_flags[] = {
#define PZ_X_(id, name, fl, rate) fl,
    HLE_ZONE_LIST(PZ_X_)
#undef PZ_X_
};
static const uint16_t pz_hle_rate[] = {
#define PZ_X_(id, name, fl, rate) rate,
    HLE_ZONE_LIST(PZ_X_)
#undef PZ_X_
};
#define PZ_NHLE ((int)(sizeof pz_hle_rate / sizeof pz_hle_rate[0]))

static const char *pz_name(int z, char *buf, size_t n)
{
    if (z >= 0 && z < hle_pz_nguest) {
        if (hle_pz_gov[z]) snprintf(buf, n, "%s@ov%u:%x", hle_pz_gname[z], hle_pz_gov[z], (unsigned)hle_pz_gaddr[z]);
        else snprintf(buf, n, "%s@%x", hle_pz_gname[z], (unsigned)hle_pz_gaddr[z]);
    } else if (z >= PZ_HLE_BASE && z < PZ_HLE_BASE + PZ_NHLE) snprintf(buf, n, "%s", pz_hle_name[z - PZ_HLE_BASE]);
    else if (z >= PZ_ROOT_BASE && z < PZ_ROOT_BASE + PZ_THREADS) {
        int t = z - PZ_ROOT_BASE;
        if (t == 0) snprintf(buf, n, "(boot, no guest thread)");
        else if (t == PZ_UNTRACKED) snprintf(buf, n, "thread>=%d", PZ_UNTRACKED);
        else snprintf(buf, n, "thread %d", t);
    } else snprintf(buf, n, "(edge table full)");
    return buf;
}

/* fixed point: hundredths of a millisecond per n */
static uint64_t pz_ms100(uint64_t ticks, uint64_t n) { return n ? ticks * 100000ull / PZ_TPS / n : 0; }
static uint64_t pz_us10(uint64_t ticks, uint64_t n) { return n ? ticks * 10000000ull / PZ_TPS / n : 0; }   /* 0.1 us */
static uint64_t pz_us100(uint64_t ticks, uint64_t n) { return n ? ticks * 100000000ull / PZ_TPS / n : 0; } /* 0.01 us */
#define MS(v) (unsigned long long)((v) / 100), (unsigned long long)((v) % 100)
#define D1(v) (unsigned long long)((v) / 10), (unsigned long long)((v) % 10)

/* ------------------------------------------------------------------ the tree */
static int pz_edge_get(int parent, int z)
{
    for (int32_t e = pz_ehead[z]; e >= 0; e = pz_edge[e].next)
        if (pz_edge[e].parent == parent) return e;
    if (pz_nedge >= PZ_EDGES) { pz_edge_full++; return 0; }
    int e = pz_nedge++;
    pz_edge[e].parent = (uint16_t)parent;
    pz_edge[e].z = (uint16_t)z;
    pz_edge[e].next = pz_ehead[z];
    pz_ehead[z] = e;
    return e;
}

static inline int pz_slot_of(int tid) { return tid >= 0 && tid < PZ_UNTRACKED ? tid : PZ_UNTRACKED; }

static int pz_top_of(int s)
{
    int sp = pz_sp[s] < PZ_DEPTH ? pz_sp[s] : PZ_DEPTH;
    return sp > 0 ? pz_stk[s][sp - 1].e : pz_root[s];
}

static inline void pz_event(void)
{
    uint64_t now = pz_now();
    uint64_t d = now - pz_last;
    pz_last = now;
    pz_tc[pz_slot] += d;
    pz_edge[pz_top].self += d;
    pz_events++;
}

/* hardware counters at zone boundaries (prof_hw=2): one kernel call before the enter event and one after the leave
   event, so the door's own time lands in the PARENT's self, not in the zone (its counters include ~1 door) */
static int pz_hw_begin(int z)
{
    int k = pz_hwslot[z];
    if (k < 0 || pz_hwopen[k]) return 0;
    if (pz_doors_iter >= PZ_DOORS_PER_ITER) { pz_doors_skipped++; return 0; }
    pz_doors_iter++;
    pz_doors++;
    if (!hle_plat_hw_door(pz_hwsnap[k])) { pz_doors_failed++; return 0; }
    pz_hwopen[k] = 1;
    return 1;
}

static void pz_hw_end(int z)
{
    int k = pz_hwslot[z];
    uint32_t now[PZ_HW_N];
    pz_hwopen[k] = 0;
    pz_doors++;
    if (!hle_plat_hw_door(now)) { pz_doors_failed++; return; }
    for (int i = 0; i < PZ_HW_N; i++) pz_hwacc[k][i] += (uint32_t)(now[i] - pz_hwsnap[k][i]);
    pz_hwn[k]++;
}

void hle_pz_enter(int z)
{
    if (!hle_pz_on) return;
    int s = pz_slot, hw = 0;
    if (pz_door && s != PZ_UNTRACKED && (pz_flags[z] & PZF_HW) && pz_sp[s] < PZ_DEPTH) hw = pz_hw_begin(z);
    pz_event();
    if (s == PZ_UNTRACKED) return;
    int sp = pz_sp[s]++;
    if (sp >= PZ_DEPTH) { pz_overflow++; return; }
    int parent = sp ? pz_stk[s][sp - 1].z : PZ_ROOT_BASE + s;
    int e = pz_edge_get(parent, z);
    PzFrame *f = &pz_stk[s][sp];
    f->z = (uint16_t)z;
    f->e = (uint16_t)e;
    f->outer = pz_active[z]++ == 0;
    f->hw = (uint8_t)hw;
    f->tc0 = pz_tc[s];
    pz_edge[e].calls++;
    pz_top = e;
}

void hle_pz_leave(int z)
{
    if (!hle_pz_on) return;
    pz_event();
    int s = pz_slot;
    if (s == PZ_UNTRACKED) return;
    if (pz_sp[s] <= 0) { pz_mismatch++; return; }
    int sp = --pz_sp[s];
    if (sp >= PZ_DEPTH) { pz_top = pz_top_of(s); return; }
    PzFrame *f = &pz_stk[s][sp];
    if (f->z != z) {                           /* frames that never left (should not happen): drop them */
        int k = sp - 1;
        while (k >= 0 && pz_stk[s][k].z != z) k--;
        pz_mismatch++;
        if (k < 0) { pz_sp[s]++; return; }     /* this zone is not open here: ignore the leave */
        for (int j = sp; j > k; j--) {
            pz_active[pz_stk[s][j].z]--;
            if (pz_stk[s][j].hw) pz_hwopen[pz_hwslot[pz_stk[s][j].z]] = 0;
        }
        pz_sp[s] = sp = k;
        f = &pz_stk[s][k];
    }
    uint64_t dt = pz_tc[s] - f->tc0;
    pz_edge[f->e].incl += dt;
    if (f->outer) pz_zincl[z] += dt;
    pz_active[z]--;
    pz_top = pz_top_of(s);
    if (f->hw) pz_hw_end(z);
}

/* sampled zones measure on the running guest thread's clock too (a register access can reach a safe point that
   switches threads: the other thread's time must not land in the sample) */
static inline uint64_t pz_thread_now(void) { return pz_tc[pz_slot] + (pz_now() - pz_last); }

void hle_pz_scope_start(hle_pz_scope *s)
{
    int z = s->z;
    if (pz_flags[z] & PZF_SAMPLE) {
        PzSamp *p = &pz_samp[z];
        if ((p->calls++ & (uint64_t)(pz_rate[z] - 1)) == 0) {
            s->on = 2;
            s->t0 = pz_thread_now();
        }
        return;
    }
    hle_pz_enter(z);
    s->on = 1;
}

void hle_pz_scope_stop(hle_pz_scope *s)
{
    if (s->on == 2) {
        PzSamp *p = &pz_samp[s->z];
        p->ticks += pz_thread_now() - s->t0;
        p->timed++;
        pz_sreads += 2;
        return;
    }
    hle_pz_leave(s->z);
}

void hle_pz_wrap(int z, void (*body)(R5900Ctx *), R5900Ctx *c)
{
    hle_pz_scope s = hle_pz_scope_begin(z);
    body(c);
    hle_pz_scope_end(&s);
}

void hle_pz_switch(int tid)
{
    int s = pz_slot_of(tid);
    if (!hle_pz_on) { pz_slot = s; return; }
    pz_event();
    pz_slot = s;
    pz_top = pz_top_of(s);
}

void hle_pz_thread_reset(int tid)
{
    int s = pz_slot_of(tid);
    if (!hle_pz_on || s == PZ_UNTRACKED) return;
    if (s == pz_slot) pz_event();
    while (pz_sp[s] > 0) {
        int sp = --pz_sp[s];
        if (sp >= PZ_DEPTH) continue;
        pz_active[pz_stk[s][sp].z]--;
        if (pz_stk[s][sp].hw) pz_hwopen[pz_hwslot[pz_stk[s][sp].z]] = 0;
    }
    if (s == pz_slot) pz_top = pz_root[s];
}

/* ------------------------------------------------------------------ per-zone sums */
static uint64_t pz_zself(int z)
{
    uint64_t v = 0;
    for (int32_t e = pz_ehead[z]; e >= 0; e = pz_edge[e].next) v += pz_edge[e].self;
    return v;
}
static uint64_t pz_zcalls(int z)
{
    if (pz_flags[z] & PZF_SAMPLE) return pz_samp[z].calls;
    uint64_t v = 0;
    for (int32_t e = pz_ehead[z]; e >= 0; e = pz_edge[e].next) v += pz_edge[e].calls;
    return v;
}
static int pz_defined(int z)
{
    return (z >= 0 && z < hle_pz_nguest) || (z >= PZ_HLE_BASE && z < PZ_HLE_BASE + PZ_NHLE) ||
           (z >= PZ_ROOT_BASE && z < PZ_ROOT_BASE + PZ_THREADS);
}

/* ------------------------------------------------------------------ output: one log write per report block (on the
   PSP every hle_plat_log_write may be a memory-stick commit) */
static char pz_out[6144];
static unsigned pz_outn;
static void pz_put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pz_put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(pz_out + pz_outn, sizeof pz_out - pz_outn, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    pz_outn += (unsigned)n < sizeof pz_out - pz_outn ? (unsigned)n : (unsigned)(sizeof pz_out - pz_outn - 1);
    if (pz_outn > sizeof pz_out - 400) {       /* nearly full: write out, keep going */
        hle_plat_log_write(pz_out, pz_outn);
        pz_outn = 0;
    }
}
static void pz_flush(void)
{
    if (pz_outn) hle_plat_log_write(pz_out, pz_outn);
    pz_outn = 0;
}

/* ------------------------------------------------------------------ hardware counter report */
static uint64_t pm(uint64_t a, uint64_t b) { return b ? a * 1000ull / b : 0; }        /* per mille */
static uint64_t pk(uint64_t a, uint64_t b) { return b ? a * 10000ull / b : 0; }       /* per 1000 inst, x10 */

static void pz_hw_report(const char *label, const uint64_t *d, uint64_t us)
{
    uint64_t ck = d[1], gck = d[2], si = d[3], sm = d[4], sf = d[5], sv = d[6], sl = d[7], bus = d[8];
    uint64_t uld = d[9], ust = d[10], cld = d[11], cst = d[12], im = d[13], dm = d[14], wb = d[15];
    uint64_t stalls = si + sm + sf + sv, busy = gck > sl ? gck - sl : 0;
    int ok = gck >= stalls + sl && gck > 0;
    uint64_t inst = ok ? gck - stalls - sl : 0;
    pz_put("PROF hw %s: %llu.%03llu s; per us: ck %llu gck %llu inst %llu; load %llu.%llu%%; CPI %llu.%02llu; busy "
           "cycles: exec %llu.%llu%% mem-stall %llu.%llu%% int-stall %llu.%llu%% fpu-stall %llu.%llu%% vfpu-stall "
           "%llu.%llu%%\n", label, (unsigned long long)(us / 1000000), (unsigned long long)(us / 1000 % 1000),
           (unsigned long long)(us ? ck / us : 0), (unsigned long long)(us ? gck / us : 0),
           (unsigned long long)(us ? inst / us : 0), D1(pm(busy, gck)),
           (unsigned long long)(inst ? busy / inst : 0), (unsigned long long)(inst ? busy * 100 / inst % 100 : 0),
           D1(pm(inst, busy)), D1(pm(sm, busy)), D1(pm(si, busy)), D1(pm(sf, busy)), D1(pm(sv, busy)));
    pz_put("PROF hw %s: per 1000 inst: I-miss %llu.%llu D-miss %llu.%llu D-writeback %llu.%llu cached ld+st %llu "
           "uncached ld %llu.%llu st %llu.%llu fpu %llu vfpu %llu cop0 %llu.%llu; I-hit %llu.%llu%% D-hit %llu.%llu%%; "
           "mem-stall cycles per miss %llu; bus busy %llu.%llu%% of ck\n", label, D1(pk(im, inst)), D1(pk(dm, inst)),
           D1(pk(wb, inst)), (unsigned long long)(pk(cld + cst, inst) / 10), D1(pk(uld, inst)), D1(pk(ust, inst)),
           (unsigned long long)(pk(d[17], inst) / 10), (unsigned long long)(pk(d[18], inst) / 10), D1(pk(d[16], inst)),
           D1(inst ? 1000 - (pm(im, inst) > 1000 ? 1000 : pm(im, inst)) : 0),
           D1(cld + cst ? 1000 - (pm(dm, cld + cst) > 1000 ? 1000 : pm(dm, cld + cst)) : 0),
           (unsigned long long)(im + dm ? sm / (im + dm) : 0), D1(pm(bus, ck)));
    const char *v;
    if (!ok) v = "n/a: CPU cycles < stalls + sleep (the counter model does not hold here - see the raw line)";
    else if (pm(busy, gck) < 500) v = "mostly IDLE (CPU load < 50 %: waiting, not computing)";
    else if (pm(sm, busy) >= 350) v = im > dm ? "MEMORY-BOUND (>= 35 % of busy cycles stall on memory; I-cache misses "
                                                "outnumber D-cache misses: code size / layout)"
                                              : "MEMORY-BOUND (>= 35 % of busy cycles stall on memory; D-cache misses "
                                                "outnumber I-cache misses: data access pattern)";
    else if (pm(inst, busy) >= 600) v = "INSTRUCTION-BOUND (>= 60 % of busy cycles retire an instruction: fewer "
                                        "instructions = faster)";
    else v = "MIXED (no stall class >= 35 %, < 60 % of busy cycles execute)";
    pz_put("PROF hw %s: VERDICT %s\n", label, v);
}

static void pz_hw_raw(const char *label, const uint64_t *d, uint64_t us)
{
    pz_put("PROF hw raw %s: us=%llu", label, (unsigned long long)us);
    for (int i = 1; i < PZ_HW_N; i++) pz_put(" w%d=%llu", i, (unsigned long long)d[i]);
    pz_put("\n");
}

static int pz_hw_now(uint64_t *tot, uint64_t *us) { return pz_hw && hle_plat_hw_totals(tot, us); }

/* ------------------------------------------------------------------ calibration + start */
void hle_pz_init(void)
{
    if (!hle_pz_opt_on && hle_pz_opt_hw < 1) return;   /* prof_hw >= 1 implies prof=1 */
    hle_pz_opt_on = 1;
    for (int z = 0; z < PZ_MAX; z++) { pz_ehead[z] = -1; pz_hwslot[z] = -1; pz_rate[z] = 1; }
    for (int z = 0; z < hle_pz_nguest && z < PZ_GUEST_MAX; z++) {
        pz_flags[z] = hle_pz_gflags[z];
        pz_rate[z] = PZ_SAMPLE_RATE;
    }
    for (int i = 0; i < PZ_NHLE; i++) {
        pz_flags[PZ_HLE_BASE + i] = pz_hle_flags[i];
        pz_rate[PZ_HLE_BASE + i] = pz_hle_rate[i] ? pz_hle_rate[i] : 1;
    }
    pz_nedge = 1;                                  /* edge 0: sink when the table is full */
    pz_edge[0].parent = pz_edge[0].z = 0xFFFF;
    pz_edge[0].next = -1;
    for (int t = 0; t < PZ_THREADS; t++) pz_root[t] = pz_edge_get(0xFFFF, PZ_ROOT_BASE + t);
    pz_slot = hle_cur ? pz_slot_of(hle_cur->id) : 0;
    pz_top = pz_root[pz_slot];
    /* the clock: 2000 reads */
    uint64_t a = pz_now();
    for (int i = 0; i < 2000; i++) (void)pz_now();
    uint64_t b = pz_now();
    pz_timer_ns = (uint32_t)((b - a) * (1000000000ull / PZ_TPS) / 2001);
    /* an empty zone, 1000 times (tree bookkeeping + 2 reads) */
    pz_last = pz_now();
    hle_pz_on = 1;
    a = pz_now();
    for (int i = 0; i < 1000; i++) { hle_pz_enter(PZ_null); hle_pz_leave(PZ_null); }
    b = pz_now();
    pz_pair_ns = (uint32_t)((b - a) * (1000000000ull / PZ_TPS) / 1000);
    for (int e = 0; e < pz_nedge; e++) pz_edge[e].calls = pz_edge[e].incl = pz_edge[e].self = 0;
    memset(pz_zincl, 0, sizeof pz_zincl);
    memset(pz_tc, 0, sizeof pz_tc);
    pz_events = 0;
    /* hardware counters (PSP platform; the host has none) */
    int k = 0;
    for (int z = 0; z < hle_pz_nguest; z++)
        if ((pz_flags[z] & PZF_HW) && k < PZ_HWZ) pz_hwslot[z] = (int16_t)k++;
    pz_nhwz = k;
    uint64_t tot[PZ_HW_N], us;
    pz_hw = hle_pz_opt_hw >= 1 && hle_plat_hw_totals(tot, &us);
    if (pz_hw && hle_pz_opt_hw >= 2) {
        uint32_t r0[PZ_HW_N], r1[PZ_HW_N];
        uint32_t v[9] = {0};
        int good = 1;
        for (int i = 0; i < 9 && good; i++) {
            uint64_t t0 = pz_now();
            good = hle_plat_hw_door(r0) && hle_plat_hw_door(r1);
            v[i] = (uint32_t)((pz_now() - t0) * (1000000000ull / PZ_TPS) / 2);
        }
        for (int i = 1; i < 9; i++)                /* median */
            for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { uint32_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
        if (good && r1[1] != r0[1]) { pz_door = 1; pz_door_ns = v[4]; }
        else hle_logf("PROF hw: zone doors OFF (%s)\n", good ? "counters frozen between two reads" :
                      "kernel call refused (kubridge kuKernelCall / ps2rc_exc.prx readfn)");
    }
    pz_last = pz_now();
    pz_t_init = pz_last;
    pz_t_init_host10 = (uint64_t)(hle_host_seconds() * 10.0);
    hle_logf("PROF on: %d guest zones (src/hle/zones.def), %d HLE zones; clock %s %u ns/read, empty zone %u ns; "
             "summary every %d s; capture %d iterations; hardware counters %s%s (kernel door %u us)\n", hle_pz_nguest,
             PZ_NHLE - 1, PZ_CLOCK_NAME, (unsigned)pz_timer_ns, (unsigned)pz_pair_ns, hle_pz_opt_summary_s,
             hle_pz_opt_capture, !hle_pz_opt_hw ? "off (prof_hw=0)" : pz_hw ? "on" : "UNAVAILABLE",
             pz_door ? " + zone doors" : "", (unsigned)(pz_door_ns / 1000));
}

/* ------------------------------------------------------------------ reports */
typedef struct { int z; uint64_t v; } PzRank;
static int pz_rank_cmp(const void *a, const void *b)
{
    uint64_t x = ((const PzRank *)a)->v, y = ((const PzRank *)b)->v;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* estimated time of a sampled zone (ticks): mean of the timed calls x all calls, minus one clock read per timed pair
   (the part of the two reads inside the interval) */
static uint64_t pz_samp_est(uint64_t calls, uint64_t timed, uint64_t ticks)
{
    if (!timed) return 0;
    uint64_t corr = (uint64_t)pz_timer_ns * timed * PZ_TPS / 1000000000ull;
    uint64_t t = ticks > corr ? ticks - corr : 0;
    return t * calls / timed;
}

static void pz_flat(const char *label, uint64_t n_iter, int top)
{
    static PzRank r[PZ_MAX];
    int nr = 0;
    for (int z = 0; z < PZ_MAX; z++) {
        if (!pz_defined(z) || (pz_flags[z] & PZF_SAMPLE) || z == PZ_null) continue;
        uint64_t s = pz_zself(z);
        if (s) r[nr++] = (PzRank){z, s};
    }
    qsort(r, nr, sizeof r[0], pz_rank_cmp);
    pz_put("PROF %s, by self time (ms%s): ", label, n_iter ? "/iteration" : " total");
    char nb[64];
    for (int i = 0; i < nr && i < top; i++) {
        uint64_t v = pz_ms100(r[i].v, n_iter ? n_iter : 1);
        pz_put("%s%s %llu.%02llu", i % 6 ? ", " : "", pz_name(r[i].z, nb, sizeof nb), MS(v));
        if (i % 6 == 5 && i + 1 < nr && i + 1 < top) pz_put("\nPROF %s (cont.): ", label);
    }
    pz_put("\n");
}

static void pz_sampled_table(const char *label, uint64_t n_iter, const uint64_t *calls0, const uint64_t *ticks0,
                             const uint64_t *timed0)
{
    char nb[64];
    int any = 0;
    for (int z = 0; z < PZ_MAX; z++) {
        if (!pz_defined(z) || !(pz_flags[z] & PZF_SAMPLE)) continue;
        uint64_t c = pz_samp[z].calls - (calls0 ? calls0[z] : 0), t = pz_samp[z].ticks - (ticks0 ? ticks0[z] : 0),
                 m = pz_samp[z].timed - (timed0 ? timed0[z] : 0);
        if (!c) continue;
        if (!any) pz_put("PROF %s sampled paths (1 call in N timed; NOT in the tree, their time is part of the "
                         "callers' self):\n", label);
        any = 1;
        uint64_t est = pz_samp_est(c, m, t);
        uint64_t calls_it100 = n_iter ? c * 100 / n_iter : c * 100;
        uint64_t est_ms = pz_ms100(est, n_iter ? n_iter : 1);
        uint64_t corr = (uint64_t)pz_timer_ns * m * PZ_TPS / 1000000000ull;      /* one clock read per sample */
        uint64_t per = m ? pz_us100(t > corr ? t - corr : 0, m) : 0;
        pz_put("PROF   %-34s 1/%-3u calls%s %llu.%02llu, %llu.%02llu us/call (%llu timed), ~%llu.%02llu ms%s\n",
               pz_name(z, nb, sizeof nb), (unsigned)pz_rate[z], n_iter ? "/it" : "", MS(calls_it100), MS(per),
               (unsigned long long)m, MS(est_ms), n_iter ? "/it" : " total");
    }
}

static void pz_snapshot_window(void)
{
    for (int z = 0; z < PZ_MAX; z++) {
        if (!pz_defined(z)) continue;
        pz_w_self[z] = pz_zself(z);
        pz_w_calls[z] = pz_zcalls(z);
        pz_w_samp_calls[z] = pz_samp[z].calls;
        pz_w_samp_ticks[z] = pz_samp[z].ticks;
        pz_w_samp_timed[z] = pz_samp[z].timed;
    }
    pz_w_events = pz_events;
    pz_w_sreads = pz_sreads;
    pz_win_iters0 = pz_iters;
    if (pz_hw) pz_hw_now(pz_hw_w0, &pz_hw_w0_us);
}

static void pz_reset_phase(void)
{
    for (int e = 0; e < pz_nedge; e++) pz_edge[e].calls = pz_edge[e].incl = pz_edge[e].self = 0;
    memset(pz_zincl, 0, sizeof pz_zincl);
    memset(pz_samp, 0, sizeof pz_samp);
    pz_events = pz_sreads = 0;
    pz_doors = pz_doors_failed = pz_doors_skipped = 0;
    memset(pz_hwacc, 0, sizeof pz_hwacc);
    memset(pz_hwn, 0, sizeof pz_hwn);
    for (int s = 0; s < PZ_THREADS; s++) {         /* open frames count from here */
        int n = pz_sp[s] < PZ_DEPTH ? pz_sp[s] : PZ_DEPTH;
        for (int i = 0; i < n; i++) pz_stk[s][i].tc0 = pz_tc[s];
        pz_tc_ml0[s] = pz_tc[s];
    }
}

static void pz_window_report(uint64_t now)
{
    uint64_t it = pz_iters - pz_win_iters0, dt = now - pz_win_t0;
    if (!it) return;
    static PzRank r[PZ_MAX];
    int nr = 0;
    for (int z = 0; z < PZ_MAX; z++) {
        if (!pz_defined(z) || (pz_flags[z] & PZF_SAMPLE) || z == PZ_null) continue;
        uint64_t s = pz_zself(z) - pz_w_self[z];
        if (s) r[nr++] = (PzRank){z, s};
    }
    qsort(r, nr, sizeof r[0], pz_rank_cmp);
    uint64_t ms_it = pz_ms100(dt, it), pace = pz_ms100(pz_zself(PZ_pace) - pz_w_self[PZ_pace], it);
    uint64_t io = pz_ms100(pz_zself(PZ_disc) - pz_w_self[PZ_disc] + pz_zself(PZ_log) - pz_w_self[PZ_log], it);
    /* instrument: every zone pair costs one calibrated empty zone, every timed sample two clock reads (ms100) */
    uint64_t ovh = ((pz_events - pz_w_events) / 2 * pz_pair_ns + (pz_sreads - pz_w_sreads) * pz_timer_ns) / 10000ull / it;
    pz_put("PROF window real %llu.%llu-%llu.%llu s: %llu iterations, %llu.%02llu ms/it (idle.pace %llu.%02llu, io "
           "%llu.%02llu, instrument ~%llu.%02llu)\n", D1(pz_t_init_host10 + (pz_win_t0 - pz_t_init) * 10 / PZ_TPS),
           D1(pz_t_init_host10 + (now - pz_t_init) * 10 / PZ_TPS), (unsigned long long)it, MS(ms_it), MS(pace), MS(io), MS(ovh));
    char nb[64];
    for (int i = 0; i < nr && i < 12; i++) {
        uint64_t v = pz_ms100(r[i].v, it);
        if (i % 6 == 0) pz_put("%sPROF window self ms/it%s:", i ? "\n" : "", i ? " (cont.)" : "");
        pz_put(" %s %llu.%02llu", pz_name(r[i].z, nb, sizeof nb), MS(v));
    }
    pz_put("\n");
    int ns = 0;
    for (int z = 0; z < PZ_MAX; z++) {
        if (!pz_defined(z) || !(pz_flags[z] & PZF_SAMPLE)) continue;
        uint64_t c = pz_samp[z].calls - pz_w_samp_calls[z];
        if (!c) continue;
        uint64_t est = pz_samp_est(c, pz_samp[z].timed - pz_w_samp_timed[z], pz_samp[z].ticks - pz_w_samp_ticks[z]);
        uint64_t v = pz_ms100(est, it);
        if (ns % 4 == 0) pz_put("%sPROF window sampled ~ms/it (calls/it):", ns ? "\n" : "");
        pz_put(" %s %llu.%02llu (%llu)", pz_name(z, nb, sizeof nb), MS(v), (unsigned long long)(c / it));
        ns++;
    }
    if (ns) pz_put("\n");
    if (pz_wslow.ticks) pz_slow_put("PROF window slowest", &pz_wslow);
    memset(&pz_wslow, 0, sizeof pz_wslow);
    if (pz_hw) {
        uint64_t tot[PZ_HW_N], us, d[PZ_HW_N];
        if (pz_hw_now(tot, &us) && us > pz_hw_w0_us) {      /* no new 50 ms window in 10 s = the thread is gone */
            for (int i = 0; i < PZ_HW_N; i++) d[i] = tot[i] - pz_hw_w0[i];
            pz_hw_report("window", d, us - pz_hw_w0_us);
        } else pz_put("PROF hw window: NO DATA - the counter thread stopped (its step is in 'hardware counters at "
                      "the end')\n");
    }
    pz_flush();
    pz_win_t0 = now;
    pz_snapshot_window();
}

/* capture: one record per completed main-loop iteration: [iteration ticks, then per capture zone: self ticks (tree
   zones) or calls (sampled zones), then the 20 counter deltas of the iteration when the door is on] */
static void pz_capture_start(void)
{
    if (hle_pz_opt_capture <= 0) return;
    pz_ncz = 0;
    for (int z = 0; z < PZ_MAX; z++)
        if (pz_defined(z) && z != PZ_null) pz_cz[pz_ncz++] = (uint16_t)z;
    pz_cap_words = 1 + pz_ncz + (pz_door ? PZ_HW_N : 0);
    pz_cap_max = (unsigned)hle_pz_opt_capture;
    if (pz_cap_max > 4000) pz_cap_max = 4000;
    /* one block: header + zone table + records (written as is at the end); from the platform's free memory, not
       the PSP's small newlib heap (the disc layer needs that one) */
    pz_cap_base = hle_plat_prof_alloc(PZ_CAP_HDR + (size_t)pz_ncz * 48 + (size_t)pz_cap_max * pz_cap_words * 4);
    if (!pz_cap_base) {
        hle_logf("PROF capture: no memory for %u iterations x %u words - capture off\n", pz_cap_max, pz_cap_words);
        pz_cap_max = 0;
        return;
    }
    pz_cap = (uint32_t *)(void *)(pz_cap_base + PZ_CAP_HDR + (size_t)pz_ncz * 48);
    for (unsigned i = 0; i < pz_ncz; i++) {
        int z = pz_cz[i];
        pz_cap_prev[z] = (pz_flags[z] & PZF_SAMPLE) ? pz_samp[z].calls : pz_zself(z);
    }
}

static void pz_capture_record(uint64_t iter_ticks, const uint32_t *raw0, const uint32_t *raw1)
{
    if (!pz_cap || pz_cap_n >= pz_cap_max) return;
    uint32_t *w = pz_cap + (size_t)pz_cap_n * pz_cap_words;
    w[0] = (uint32_t)iter_ticks;
    for (unsigned i = 0; i < pz_ncz; i++) {
        int z = pz_cz[i];
        uint64_t v = (pz_flags[z] & PZF_SAMPLE) ? pz_samp[z].calls : pz_zself(z);
        w[1 + i] = (uint32_t)(v - pz_cap_prev[z]);
        pz_cap_prev[z] = v;
    }
    if (pz_door)
        for (int i = 0; i < PZ_HW_N; i++) w[1 + pz_ncz + i] = raw0 && raw1 ? raw1[i] - raw0[i] : 0;
    pz_cap_n++;
}

static void pz_capture_write(void)
{
    if (!pz_cap || !pz_cap_n) return;
    /* header 16 words: magic "PZB1", version, build, ticks per second, zones, records, words per record, hw words,
       timer ns, empty-zone ns, door ns, 5 reserved; then per zone: id u16, flags u8, ov u8, addr u32, name 40 bytes;
       then the records */
    size_t hdr = PZ_CAP_HDR, zt = (size_t)pz_ncz * 48, rec = (size_t)pz_cap_n * pz_cap_words * 4;
    uint8_t *buf = pz_cap_base;
    uint32_t h[16] = {0x31425A50u, 1,
#ifdef HLE_BUILD_NUM
                      HLE_BUILD_NUM,
#else
                      0,
#endif
                      (uint32_t)PZ_TPS, pz_ncz, pz_cap_n, pz_cap_words, pz_door ? PZ_HW_N : 0, pz_timer_ns,
                      pz_pair_ns, pz_door_ns, 0, 0, 0, 0, 0};
    memcpy(buf, h, hdr);
    for (unsigned i = 0; i < pz_ncz; i++) {
        uint8_t *p = buf + hdr + i * 48;
        int z = pz_cz[i];
        uint16_t id = (uint16_t)z;
        uint32_t addr = z < hle_pz_nguest ? hle_pz_gaddr[z] : 0;
        char nb[64];
        memset(p, 0, 48);
        memcpy(p, &id, 2);
        p[2] = pz_flags[z];
        p[3] = z < hle_pz_nguest ? hle_pz_gov[z] : 0;
        memcpy(p + 4, &addr, 4);
        strncpy((char *)p + 8, z < hle_pz_nguest ? hle_pz_gname[z] : pz_name(z, nb, sizeof nb), 39);
    }
    int r = hle_plat_write_file("prof.bin", buf, hdr + zt + rec);
    hle_logf("PROF capture: prof.bin %s (%u iterations x %u words, %u bytes; tests/phase6psp/prof_bin.py)\n",
             r == 0 ? "written" : "NOT written", pz_cap_n, pz_cap_words, (unsigned)(hdr + zt + rec));
}

static void pz_boot_report(uint64_t now)
{
    pz_put("PROF ==== boot phase: %llu.%llu s real from the profiler start to the first main-loop iteration ====\n",
           D1((now - pz_t_init) * 10 / PZ_TPS));
    pz_flat("boot", 0, 18);
    pz_sampled_table("boot", 0, NULL, NULL, NULL);
    if (pz_hw) {
        uint64_t tot[PZ_HW_N], us;
        if (pz_hw_now(tot, &us)) pz_hw_report("boot (since the counter thread armed)", tot, us);
    }
    pz_flush();
}

static void pz_slow_fill(PzSlow *s, uint64_t it, uint64_t ticks)
{
    memset(s, 0, sizeof *s);
    s->it = it;
    s->ticks = ticks;
    for (int z = 0; z < PZ_MAX; z++) {
        if (pz_flags[z] & PZF_SAMPLE) continue;
        uint64_t d = pz_it_cur[z] - pz_it_prev[z];
        if (!d || d <= s->s[PZ_SLOWZ - 1]) continue;
        int i = PZ_SLOWZ - 1;
        while (i > 0 && s->s[i - 1] < d) { s->s[i] = s->s[i - 1]; s->z[i] = s->z[i - 1]; i--; }
        s->s[i] = d;
        s->z[i] = (uint16_t)z;
    }
}

static void pz_slow_note(uint64_t ticks)
{
    memset(pz_it_cur, 0, sizeof pz_it_cur);
    for (int e = 1; e < pz_nedge; e++)
        if (pz_edge[e].z < PZ_MAX) pz_it_cur[pz_edge[e].z] += pz_edge[e].self;
    uint64_t it = pz_iters - 1;
    if (pz_nslow < PZ_SLOW || ticks > pz_slow[pz_nslow - 1].ticks) {
        int i = pz_nslow < PZ_SLOW ? pz_nslow++ : PZ_SLOW - 1;
        while (i > 0 && pz_slow[i - 1].ticks < ticks) { pz_slow[i] = pz_slow[i - 1]; i--; }
        pz_slow_fill(&pz_slow[i], it, ticks);
    }
    if (ticks > pz_wslow.ticks) pz_slow_fill(&pz_wslow, it, ticks);
    memcpy(pz_it_prev, pz_it_cur, sizeof pz_it_prev);
}

static void pz_slow_put(const char *label, const PzSlow *s)
{
    char nb[64];
    pz_put("%s iteration %llu: %llu.%02llu ms =", label, (unsigned long long)s->it, MS(pz_ms100(s->ticks, 1)));
    for (int i = 0; i < PZ_SLOWZ && s->s[i]; i++)
        pz_put("%s %s %llu.%02llu", i ? "," : "", pz_name(s->z[i], nb, sizeof nb), MS(pz_ms100(s->s[i], 1)));
    pz_put("\n");
}

void hle_pz_iteration(void)
{
    if (!hle_pz_on) return;
    pz_event();
    uint64_t now = pz_last;
    uint32_t raw[PZ_HW_N];
    int raw_ok = 0;
    if (pz_door && pz_doors_iter < PZ_DOORS_PER_ITER + 4) {   /* the per-iteration window (capture) */
        pz_doors++;
        raw_ok = hle_plat_hw_door(raw);
        if (!raw_ok) pz_doors_failed++;
    }
    pz_doors_iter = 0;
    if (!pz_ml) {
        pz_boot_report(now);
        pz_reset_phase();
        pz_ml = 1;
        pz_ml_t0 = pz_win_t0 = now;
        pz_iters = 0;
        if (pz_hw) pz_hw_now(pz_hw_ml0, &pz_hw_ml0_us);
        pz_snapshot_window();
        pz_capture_start();
        memset(pz_it_prev, 0, sizeof pz_it_prev);  /* the edges were just reset */
        hle_logf("PROF main loop starts (real %.1f s): per-iteration zone tables from here; a summary every %d ms\n",
                 hle_host_seconds(), hle_pz_opt_summary_ms > 0 ? hle_pz_opt_summary_ms : hle_pz_opt_summary_s * 1000);
    } else {
        pz_capture_record(now - pz_iter_t0, pz_iter_raw_ok && raw_ok ? pz_iter_raw : NULL, raw_ok ? raw : NULL);
        pz_slow_note(now - pz_iter_t0);
    }
    if (raw_ok) memcpy(pz_iter_raw, raw, sizeof raw);
    pz_iter_raw_ok = raw_ok;
    pz_iter_t0 = now;
    pz_iters++;
    uint64_t per = hle_pz_opt_summary_ms > 0 ? (uint64_t)hle_pz_opt_summary_ms * PZ_TPS / 1000
                                             : (uint64_t)(hle_pz_opt_summary_s > 0 ? hle_pz_opt_summary_s : 0) * PZ_TPS;
    if (per && now - pz_win_t0 >= per) pz_window_report(now);
}

/* ---- the final tree: rows per (parent, zone) edge, siblings by inclusive time; a zone called from several parents
   shows its children under the first row printed (later rows say "children shown above") */
static uint64_t pz_rep_div, pz_rep_iter_ticks;
static uint8_t pz_expanded[PZ_MAX];
static unsigned pz_pruned;

static uint64_t pz_edge_incl_now(int e)
{
    /* inclusive incl. still-open activations (the run stops inside the last iteration) */
    uint64_t v = pz_edge[e].incl;
    for (int s = 0; s < PZ_THREADS; s++) {
        int n = pz_sp[s] < PZ_DEPTH ? pz_sp[s] : PZ_DEPTH;
        for (int i = 0; i < n; i++)
            if (pz_stk[s][i].e == e) v += pz_tc[s] - pz_stk[s][i].tc0;
    }
    return v;
}

static void pz_tree(int e, int depth)
{
    char nb[64], ind[48];
    int z = pz_edge[e].z;
    uint64_t incl = z >= PZ_ROOT_BASE && z < PZ_ROOT_BASE + PZ_THREADS ? pz_tc[z - PZ_ROOT_BASE] -
                    pz_tc_ml0[z - PZ_ROOT_BASE] : pz_edge_incl_now(e);
    uint64_t calls = pz_edge[e].calls;
    int d = depth < 20 ? depth : 20;
    memset(ind, ' ', (size_t)d * 2);
    ind[d * 2] = 0;
    uint64_t im = pz_ms100(incl, pz_rep_div), sm = pz_ms100(pz_edge[e].self, pz_rep_div);
    uint64_t pc = pz_rep_iter_ticks ? incl * 1000ull / pz_rep_div / pz_rep_iter_ticks : 0;
    int again = pz_expanded[z], has_kids = 0;
    for (int c = 1; c < pz_nedge && !has_kids; c++) has_kids = pz_edge[c].parent == z && pz_edge[c].calls;
    pz_put("PROF %s%-*s %9llu.%02llu %9llu.%02llu %8llu.%02llu %5llu.%llu %9llu.%llu%s\n", ind, 40 - d * 2,
           pz_name(z, nb, sizeof nb), MS(calls * 100 / pz_rep_div), MS(im), MS(sm), D1(pc),
           D1(calls ? pz_us10(incl, calls) : 0), again && has_kids ? "  (children shown above)" : "");
    if (again || depth > 12) return;
    pz_expanded[z] = 1;
    static int kids[PZ_MAX];
    int nk = 0;
    for (int c = 1; c < pz_nedge && nk < PZ_MAX; c++) {
        if (pz_edge[c].parent != z) continue;
        uint64_t ci = pz_edge_incl_now(c);
        if (pz_ms100(ci, pz_rep_div) < 1 && pz_edge[c].calls * 100 / pz_rep_div < 5) { pz_pruned++; continue; }
        kids[nk++] = c;
    }
    for (int i = 1; i < nk; i++)                   /* by inclusive time, largest first */
        for (int j = i; j > 0 && pz_edge_incl_now(kids[j - 1]) < pz_edge_incl_now(kids[j]); j--) {
            int t = kids[j]; kids[j] = kids[j - 1]; kids[j - 1] = t;
        }
    int my[nk > 0 ? nk : 1];                       /* the static list is reused by the recursion */
    memcpy(my, kids, (size_t)nk * sizeof my[0]);
    for (int i = 0; i < nk; i++) pz_tree(my[i], depth + 1);
}

void hle_pz_report_final(void)
{
    if (!hle_pz_on) return;
    pz_event();
    uint64_t now = pz_last;
    hle_pz_on = 0;                                 /* nothing below is timed */
    if (!pz_ml) {
        pz_boot_report(now);
        pz_put("PROF the main loop was not reached: no per-iteration tables\n");
        pz_flush();
        return;
    }
    uint64_t intervals = pz_iters > 1 ? pz_iters - 1 : 1, span = now - pz_ml_t0;
    pz_rep_div = intervals;
    pz_rep_iter_ticks = span / intervals;
    uint64_t reads = pz_events + pz_sreads;
    uint64_t ovh100 = (pz_events / 2 * pz_pair_ns + pz_sreads * pz_timer_ns + pz_doors * pz_door_ns) / 10000ull / intervals;
    uint64_t it100 = pz_ms100(span, intervals);
    pz_put("PROF ==== main loop: %llu iterations, %llu.%llu s real (%llu.%02llu ms per iteration over %llu intervals); "
           "clock %u ns/read, empty zone %u ns, %llu reads/it + %llu kernel doors/it => instrument ~%llu.%02llu ms/it "
           "(%llu.%llu%%) ====\n",
           (unsigned long long)pz_iters, D1(span * 10 / PZ_TPS), MS(it100), (unsigned long long)intervals,
           (unsigned)pz_timer_ns, (unsigned)pz_pair_ns, (unsigned long long)(reads / intervals),
           (unsigned long long)(pz_doors / intervals), MS(ovh100), D1(it100 ? ovh100 * 1000 / it100 : 0));
    pz_put("PROF %-40s %12s %12s %11s %7s %11s\n", "zone tree (main loop, per iteration)", "calls/it", "incl ms/it",
           "self ms/it", "%iter", "us/call");
    pz_flush();
    memset(pz_expanded, 0, sizeof pz_expanded);
    pz_pruned = 0;
    for (int t = 0; t < PZ_THREADS; t++) {
        uint64_t tt = pz_tc[t] - pz_tc_ml0[t];
        if (pz_ms100(tt, intervals) < 1) continue;
        pz_tree(pz_root[t], 0);
        pz_flush();
    }
    uint64_t sum_t = 0;
    for (int t = 0; t < PZ_THREADS; t++) sum_t += pz_tc[t] - pz_tc_ml0[t];
    pz_put("PROF tree check: thread clocks sum %llu.%02llu ms/it = %llu.%llu%% of the real time; %u rows pruned "
           "(< 0.01 ms/it and < 0.05 calls/it); %u stack overflows, %u enter/leave mismatches, %u edge-table overflows\n",
           MS(pz_ms100(sum_t, intervals)), D1(span ? sum_t * 1000 / span : 0), pz_pruned, (unsigned)pz_overflow,
           (unsigned)pz_mismatch, (unsigned)pz_edge_full);
    {   /* the one-line answer: where an iteration goes, by kind (self times; sampled paths are estimates inside the
           self time of their callers, so they are moved from "game code" to their own kind) */
        uint64_t idle = pz_zself(PZ_pace), io = pz_zself(PZ_disc) + pz_zself(PZ_log);
        uint64_t hle = pz_zself(PZ_syscall) + pz_zself(PZ_intr) + pz_zself(PZ_dma) + pz_zself(PZ_rpc);
        uint64_t hs = 0, gs = 0;
        for (int z = 0; z < PZ_MAX; z++) {
            if (!pz_defined(z) || !(pz_flags[z] & PZF_SAMPLE)) continue;
            uint64_t e = pz_samp_est(pz_samp[z].calls, pz_samp[z].timed, pz_samp[z].ticks);
            if (z >= PZ_HLE_BASE) hs += e; else gs += e;
        }
        uint64_t ge = pz_zself(PZ_ge_draw) + pz_zself(PZ_ge_flip);
        uint64_t used = idle + io + hle + hs + ge, game = sum_t > used ? sum_t - used : 0;
        pz_put("PROF split ms/it: game code %llu.%02llu (of which sampled guest leaves ~%llu.%02llu), HLE %llu.%02llu "
               "(syscall/intr/dma/rpc) + ~%llu.%02llu (vu0/memcpy/memset/memtrap, sampled), GE back-end %llu.%02llu "
               "(draw %llu.%02llu, flip %llu.%02llu), io %llu.%02llu (disc "
               "%llu.%02llu, log %llu.%02llu), idle.pace %llu.%02llu; instrument ~%llu.%02llu is inside these\n",
               MS(pz_ms100(game, intervals)), MS(pz_ms100(gs, intervals)), MS(pz_ms100(hle, intervals)),
               MS(pz_ms100(hs, intervals)), MS(pz_ms100(ge, intervals)), MS(pz_ms100(pz_zself(PZ_ge_draw), intervals)),
               MS(pz_ms100(pz_zself(PZ_ge_flip), intervals)), MS(pz_ms100(io, intervals)),
               MS(pz_ms100(pz_zself(PZ_disc), intervals)),
               MS(pz_ms100(pz_zself(PZ_log), intervals)), MS(pz_ms100(idle, intervals)), MS(ovh100));
    }
    pz_flat("main loop", intervals, 24);
    pz_sampled_table("main loop", intervals, NULL, NULL, NULL);
    pz_put("PROF slowest main-loop iterations (top %d zones by self time in that iteration):\n",
           PZ_SLOWZ);
    for (int i = 0; i < pz_nslow; i++) {
        char lab[32];
        snprintf(lab, sizeof lab, "PROF slow %2d:", i + 1);
        pz_slow_put(lab, &pz_slow[i]);
    }
    pz_flush();
    if (pz_hw) {
        uint64_t tot[PZ_HW_N], us, d[PZ_HW_N];
        if (pz_hw_now(tot, &us) && us > pz_hw_ml0_us) {
            for (int i = 0; i < PZ_HW_N; i++) d[i] = tot[i] - pz_hw_ml0[i];
            pz_hw_report("main loop (whole CPU, 50 ms windows)", d, us - pz_hw_ml0_us);
            pz_hw_raw("main loop", d, us - pz_hw_ml0_us);
        } else pz_put("PROF hw main loop: NO DATA - the counter thread stopped or never counted in the main loop (its "
                      "step is in 'hardware counters at the end')\n");
        char nb[64], lab[96];
        for (int z = 0; z < PZ_MAX; z++) {
            int k = pz_hwslot[z];
            if (k < 0 || !pz_hwn[k]) continue;
            uint64_t us_z = 0;
            /* zone window length from the system clock count (word 1) is in PLL units: use the zone's time */
            us_z = pz_zincl[z] * 1000000ull / PZ_TPS;
            snprintf(lab, sizeof lab, "zone %s (%llu calls)", pz_name(z, nb, sizeof nb), (unsigned long long)pz_hwn[k]);
            pz_hw_report(lab, pz_hwacc[k], us_z);
        }
        if (pz_door) pz_put("PROF hw doors: %llu calls (%llu failed, %llu skipped over %d/iteration), %u us each "
                            "(in the parents' self time)\n", (unsigned long long)pz_doors,
                            (unsigned long long)pz_doors_failed, (unsigned long long)pz_doors_skipped, PZ_DOORS_PER_ITER,
                            (unsigned)(pz_door_ns / 1000));
        pz_flush();
    }
    pz_capture_write();
}
