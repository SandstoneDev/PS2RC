/* hle_hitch.h - frame-time spikes ("hitches") of the main loop.
 *
 * Always on (no profiler needed, a few integer adds per iteration): the real time of every main-loop iteration
 * (the pack's [runtime] main_loop function, entry to entry, hle_mainloop_tick) goes into a 1 ms histogram; the slowest
 * iterations are kept with a breakdown taken from cumulative microsecond counters the subsystems add to on the game
 * thread (hle_hitch_us[]: disc reads, waits for async disc reads, session.log commits, GE texture conversion, GE sync
 * waits, the pack's streaming work, the pack's mesh conversion). "other" = the rest of the iteration (the game's code
 * and everything not listed). Output: a "hitch window" line every hitch_window seconds (PSP run options / host env
 * HLE_HITCH_WINDOW, default 30, 0 = none) and the final tables: the
 * histogram, percentiles, the time lost in spikes (iterations > 2 x the median), the slowest iterations with their
 * breakdown and the sum of every source over the spike iterations.
 */
#ifndef HLE_HITCH_H
#define HLE_HITCH_H

#include <stdint.h>

enum {
    HITCH_DISC,      /* disc file reads + opens on the game thread (PSP: hle_plat_pread / vf_ensure) */
    HITCH_DWAIT,     /* the game thread waiting for an async disc read of the pack (disc_async=1) */
    HITCH_LOG,       /* session.log commits on the game thread (PSP: open / append / close) */
    HITCH_TEXCONV,   /* GE texture conversion on first use (psp/hle_ge.c tex_lookup) */
    HITCH_GESYNC,    /* waiting for the GE: mid-frame resyncs + the flip's list sync (psp/hle_ge.c list_sync) */
    HITCH_STREAM,    /* the pack's streaming hook (e.g. the conversion of streamed models), minus the above */
    HITCH_MESH,      /* the pack's mesh conversion (its counter hle_hitch_mesh_us, see hle_hitch.c) */
    HITCH_N
};

extern uint64_t hle_hitch_us[HITCH_N];
extern int hle_hitch_opt_window;            /* seconds between window lines (0 = none) */
extern int hle_hitch_opt_logstream;         /* hitch_logstream=N: the pack's streaming hook logs conversions >= N us */
extern uint64_t hle_hitch_it_n;             /* main-loop iterations seen so far */

void hle_hitch_cfg(const char *k, const char *v);   /* poc6.cfg hitch_* keys */
void hle_hitch_tick(void);                  /* a main-loop iteration starts (hle_mainloop_tick) */
void hle_hitch_report(void);                /* end of run */
uint64_t hle_hitch_now_us(void);            /* the clock used by the counters (game thread) */

#endif
