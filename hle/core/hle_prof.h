/* hle_prof.h - the profiler: where the main loop's time goes.
 *
 * Zones = timed regions on the running guest thread, depth-aware (a stack per guest thread; inclusive time runs on the
 * thread's own clock, so time spent in other guest threads is not counted), self = inclusive - zoned children:
 *   - guest zones: translated functions listed in src/hle/zones.def; ps2rc/pipeline/prepare_gen.py renames their body
 *     <name>__zbody and generates the wrapper <name> (gen/hle_zones.c), which calls hle_pz_wrap;
 *   - HLE zones: PZ_SCOPE(id) at the top of an HLE function (list below);
 *   - sampled zones (PZF_SAMPLE): hot short paths, timed on 1 call in N only and kept OUT of the tree (their time stays
 *     in the caller's self; they get their own table: calls, us per call, estimated ms per iteration).
 * Clock: sceKernelGetSystemTimeLow on the PSP (1 us, a syscall - its cost is measured at start and printed), the host
 * monotonic clock in ns. Output: a summary every prof_summary seconds and the final tables in the log, an optional
 * binary per-iteration capture (prof.bin, tests/phase6psp/prof_bin.py). Hardware counters (PSP only, prof_hw >= 1)
 * come through the platform (src/hle/psp/hle_prof_psp.c, ps2rc_exc.prx). Off unless enabled (ps2rc.cfg prof=1 / host
 * --prof); profiling never charges virtual time, so the guest runs exactly as without it.
 */
#ifndef HLE_PROF_H
#define HLE_PROF_H

#include <stdint.h>
#include "r5900_rt.h"
#if defined(__has_include) && __has_include("hle_pack.h")
#include "hle_pack.h"                    /* HLE_PACK_ZONE_LIST: the pack modules' zones */
#endif
#ifndef HLE_PACK_ZONE_LIST
#define HLE_PACK_ZONE_LIST(X)
#endif

#define PZF_GUEST  1          /* zones.def: wraps the guest original of a HOOK/TRACE function (fn_X__guest) */
#define PZF_HW     2          /* prof_hw=2: hardware counters read at the zone's boundaries (a kernel call each) */
#define PZF_SAMPLE 4          /* timed on 1 call in PZ_SAMPLE_RATE, outside the zone tree */
#define PZ_SAMPLE_RATE 16     /* guest sampled zones; HLE zones give their own rate below */

/* zone ids: guest zones 0..PZ_GUEST_MAX-1 (gen/hle_zones.c order), HLE zones from PZ_HLE_BASE, one root per tracked
   guest thread from PZ_ROOT_BASE ("thread N" = its time outside every zone) */
#define PZ_GUEST_MAX 255
#define PZ_HLE_BASE 256
#define PZ_ROOT_BASE 384
#define PZ_THREADS 16         /* guest thread ids 0..15 tracked (0 = before the first guest thread); others share 15 */
#define PZ_MAX 512

/*  X(id, display name, flags, sample rate) */
#define HLE_ZONE_LIST(X) \
    X(syscall, "hle.syscall", 0, 1)        /* EE kernel syscall dispatcher (sys_entry) incl. its safe point */ \
    X(intr,    "hle.intr",    0, 1)        /* INTC/DMAC/alarm handlers run at a safe point (run_interrupts) */ \
    X(dma,     "hle.dma",     0, 1)        /* DMA kick model: scratchpad copies, chains (hle_dma_kick) */ \
    X(rpc,     "hle.rpc",     0, 1)        /* SIF RPC servers of the pack (hle_rpc_call) */ \
    X(audio,   "hle.audio",   0, 1)        /* the pack's IOP audio model + SPU2-lite lockstep render (in rpc) */ \
    X(disc,    "io.disc",     0, 1)        /* disc file reads incl. opens (hle_disc_read_fd -> sceIo) */ \
    X(log,     "io.log",      0, 1)        /* session.log writes (PSP: open/append/close per commit) */ \
    X(pace,    "idle.pace",   0, 1)        /* waiting for the real clock (virtual time ahead): CPU idle */ \
    X(ge_draw, "ge.draw",     0, 1)        /* PSP GE back-end of the pack: 2D draws / clear / camera -> lists */ \
    X(ge_flip, "ge.flip",     0, 1)        /* PSP GE back-end: list finish + sceGuSync + display flip (GE wait) */ \
    X(vu0,     "hle.vu0",     PZF_SAMPLE, 16)   /* VCALLMS -> VU0 micro program #0 HLE (rt_vu0_callms_hle) */ \
    X(memcpy,  "hle.memcpy",  PZF_SAMPLE, 16)   /* native memcpy hook of the pack */ \
    X(memset,  "hle.memset",  PZF_SAMPLE, 16)   /* native memset hook of the pack */ \
    X(memtrap, "hle.memtrap", PZF_SAMPLE, 64)   /* trap-set accessor slow path (scratchpad, mirrors, registers) */ \
    HLE_PACK_ZONE_LIST(X)                  /* the game pack's own zones ([runtime] zones) */ \
    X(null,    "prof.null",   0, 1)        /* empty zone: calibration of the instrument's own cost */

enum {
    PZ_HLE_FIRST_ = PZ_HLE_BASE - 1,
#define PZ_X_(id, name, fl, rate) PZ_##id,
    HLE_ZONE_LIST(PZ_X_)
#undef PZ_X_
    PZ_HLE_END_
};

extern volatile int hle_pz_on;          /* zone timing active (set by hle_pz_init) */
/* options, set by the platform before hle_pz_init (PSP ps2rc.cfg, host command line) */
extern int hle_pz_opt_on;               /* prof=1 */
extern int hle_pz_opt_hw;               /* prof_hw=0|1|2 (PSP only): 1 = counter thread, 2 = + zone doors */
extern int hle_pz_opt_summary_s;        /* prof_summary=N seconds between summaries (default 10, 0 = none) */
extern int hle_pz_opt_summary_ms;       /* host --prof-summary-ms: the same in ms (tests; overrides) */
extern int hle_pz_opt_capture;          /* prof_capture=N iterations recorded into prof.bin (0 = none) */

void hle_pz_init(void);                 /* calibrates, prints the configuration line; no-op unless hle_pz_opt_on */
void hle_pz_enter(int z);
void hle_pz_leave(int z);
void hle_pz_wrap(int z, void (*body)(R5900Ctx *), R5900Ctx *c);   /* generated guest-zone wrappers */
void hle_pz_switch(int guest_thread_id);    /* the scheduler switches to another guest thread */
void hle_pz_thread_reset(int guest_thread_id);  /* a guest thread (re)starts / exits: drop its open zones */
void hle_pz_iteration(void);            /* a main-loop iteration starts (main_loop entry) */
void hle_pz_report_final(void);         /* end of run: final tables (+ prof.bin) */

/* scope helper for HLE functions: PZ_SCOPE(dma); at the top of the body (GCC cleanup attribute closes it) */
typedef struct { int z; int on; uint64_t t0; } hle_pz_scope;
void hle_pz_scope_start(hle_pz_scope *s);
void hle_pz_scope_stop(hle_pz_scope *s);
static inline hle_pz_scope hle_pz_scope_begin(int z)
{
    hle_pz_scope s = {z, 0, 0};
    if (hle_pz_on) hle_pz_scope_start(&s);
    return s;
}
static inline void hle_pz_scope_end(hle_pz_scope *s) { if (s->on) hle_pz_scope_stop(s); }
#define PZ_SCOPE(id) hle_pz_scope pz_scope_##id __attribute__((cleanup(hle_pz_scope_end))) = hle_pz_scope_begin(PZ_##id)

/* generated by prepare_gen.py (gen/hle_zones.c): the zones.def table */
extern const int hle_pz_nguest;
extern const char *const hle_pz_gname[];
extern const uint32_t hle_pz_gaddr[];
extern const unsigned char hle_pz_gov[];
extern const unsigned char hle_pz_gflags[];

#endif
