/* hle_bootpace.c - the boot / loading screens paced to the real clock.
 * See hle_bootpace.h for the keys. The real clock is the platform's pacing clock (hle_plat_pace_now_us: PSP = system time -
 * boot - movie time), the one hle_plat_pace waits for; forgiving lag = moving that clock back (hle_plat_pace_adjust),
 * the same mechanism as the x_pace (hle_bl.c), here only before the first main-loop iteration.
 * With x_bootpace=0 (default) and no host simulation variables, hle_bootpace_on drops to 0 at the first time charge and
 * nothing here runs again (no log line either).
 */
#include <stdio.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_plat.h"
#include "hle_fast.h"
#include "hle_bootpace.h"
#ifndef HLE_PSP
#include <time.h>
#endif

int hle_bootpace_on = 1;
static int mode, o_lag_ms = 100, o_max_ms = 250, done;
static uint32_t ctr, per = 64;
static uint64_t st_forgiven_us, st_forgive_n, st_jumps, st_jump_cyc, st_syncs;

static uint64_t us_to_cyc(uint64_t us) { return us / 1000000u * EE_HZ + us % 1000000u * EE_HZ / 1000000u; }
static uint64_t cyc_to_us(uint64_t c) { return c / EE_HZ * 1000000u + c % EE_HZ * 1000000u / EE_HZ; }

#ifndef HLE_PSP
static double sim_k, sim_idle, sim_t0 = -1, sim_work, tl_last = -1;
static FILE *tl;
static void sim_init(void)
{
    const char *k = getenv("HLE_BN_SIMK"), *t = getenv("HLE_BN_TIMELINE"), *id = getenv("HLE_BN_SIMIDLE");
    if (k && *k) sim_k = atof(k);
    if (id && *id) sim_idle = atof(id) / 1e6;
    if (t && *t) tl = fopen(t, "w");
    if (sim_k > 0 || sim_idle > 0 || tl)
        LOG("boot pacing: host simulation - guest work x%.2f of its virtual time, %.0f us per idle jump before "
            "the main loop%s%s\n", sim_k, sim_idle * 1e6, tl ? ", timeline to " : "", tl ? t : "");
}
static void sim_charge(uint64_t cycles)
{
    double now = hle_host_seconds();
    if (tl && now - tl_last >= 0.005) {
        tl_last = now;
        fprintf(tl, "%.6f %.6f %llu\n", now, (double)hle_vtime / EE_HZ, (unsigned long long)hle_mainloop_iters);
    }
    if (sim_k <= 0 || hle_mainloop_iters) return;
    if (sim_t0 < 0) sim_t0 = now;
    sim_work += sim_k * (double)cycles / EE_HZ;
    double due = sim_t0 + sim_work;
    if (due > now + 0.002) {                            /* sleep in >= 2 ms pieces (cheap) */
        double d = due - now;
        struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
        nanosleep(&ts, NULL);
    }
}
#endif

static void init(void)
{
    mode = hle_x_opt("bootpace", 0);
    if (mode < 0 || mode > 2) mode = 0;
    o_lag_ms = hle_x_opt("bootlag", 100);
    o_max_ms = hle_x_opt("pacemax", 250);
    int p = hle_x_opt("paceper", 64);
    per = p > 0 ? (uint32_t)p : 64u;
    if (o_lag_ms < 0) o_lag_ms = 0;
    if (o_max_ms < 0) o_max_ms = 0;
    hle_bootpace_on = mode != 0;
#ifndef HLE_PSP
    sim_init();
    if (sim_k > 0 || sim_idle > 0 || tl) hle_bootpace_on = 1;
#endif
    if (mode)
        LOG("config: x_bootpace=%d (%s) x_bootlag=%d x_pacemax=%d x_paceper=%u - until the first main-loop "
            "iteration\n", mode, mode == 2 ? "real time" : "no debt", o_lag_ms, o_max_ms, (unsigned)per);
}

static void end_boot(void)
{
    done = 1;
    int64_t lag = (int64_t)hle_plat_pace_now_us() - (int64_t)cyc_to_us(hle_vtime);
    if (mode)
        LOG("boot pacing: x_bootpace=%d: at the first main-loop iteration (vtime %.3f s, real %.1f s) the real "
            "clock was %lld ms %s virtual time; lag forgiven %.2f s (%llu times at idle points), catch-up %.2f s in %llu "
            "jumps (%llu real-clock checks)\n", mode, (double)hle_vtime / EE_HZ, hle_host_seconds(),
            (long long)((lag < 0 ? -lag : lag) / 1000), lag < 0 ? "behind" : "ahead of",
            (double)st_forgiven_us / 1e6, (unsigned long long)st_forgive_n, (double)st_jump_cyc / EE_HZ,
            (unsigned long long)st_jumps, (unsigned long long)st_syncs);
}

static void forgive(uint64_t keep_us, uint64_t due_us)
{
    uint64_t now = hle_plat_pace_now_us();
    if (now <= due_us || now - due_us <= keep_us) return;
    uint64_t f = now - due_us - keep_us;
    hle_plat_pace_adjust((int64_t)f);
    st_forgiven_us += f;
    st_forgive_n++;
}

static void rt_sync(void)                               /* mode 2: virtual time follows the real clock (cf. hle_bl.c) */
{
    st_syncs++;
    uint64_t now = hle_plat_pace_now_us(), v = cyc_to_us(hle_vtime);
    if (now <= v) return;
    uint64_t lag = now - v, max = (uint64_t)o_max_ms * 1000u;
    if (lag > max) {
        hle_plat_pace_adjust((int64_t)(lag - max));
        st_forgiven_us += lag - max;
        lag = max;
    }
    uint64_t c = us_to_cyc(lag);
    hle_vtime += c;                                     /* the caller (hle_time_charge) processes the due events */
    st_jump_cyc += c;
    st_jumps++;
}

void hle_bootpace_charge(uint64_t cycles)
{
    static int read;
    if (!read) { read = 1; init(); if (!hle_bootpace_on) return; }   /* first call: read the keys */
#ifndef HLE_PSP
    sim_charge(cycles);
#else
    (void)cycles;
#endif
    if (done) return;
    if (hle_mainloop_iters) {
        end_boot();
#ifndef HLE_PSP
        if (tl) return;                                 /* the timeline keeps sampling the main loop */
#endif
        hle_bootpace_on = 0;
        return;
    }
    if (mode == 2 && ++ctr >= per) { ctr = 0; rt_sync(); }
}

void hle_bootpace_pace(uint64_t vtime)
{
#ifndef HLE_PSP
    if (sim_idle > 0 && !hle_mainloop_iters) {         /* host simulation: the PSP's real cost of an idle wake-up */
        struct timespec ts = {0, (long)(sim_idle * 1e9)};
        nanosleep(&ts, NULL);
    }
#endif
    if (!mode || done || hle_mainloop_iters) return;
    forgive((uint64_t)(mode == 1 ? o_lag_ms : o_max_ms) * 1000u, cyc_to_us(vtime));
}
