/* hle_bl.c - game time = real time, frame cap. See hle_bl.h for the keys.
 *
 * The real clock here is the platform's pacing clock (hle_plat_pace_now_us: PSP = system time - boot - movie time),
 * the one hle_plat_pace waits for, so "virtual time lags the real clock by L" means exactly that the next pace call
 * would not wait. Forgiving lag = moving that clock back (hle_plat_pace_adjust). Nothing here runs before the first
 * main-loop iteration, and with x_pace=0 nothing changes at all (only the optional clock log reads guest RAM).
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_plat.h"
#include "hle_fast.h"
#include "hle_bl.h"
#ifndef HLE_PSP
#include <time.h>
#endif

#define BL_GP 0x66B970u                 /* the game's gp; the game timer's globals (update 0x27B5F0, init 0x27B560) */
#define CT_MS (BL_GP + 0x924u)          /* the millisecond clock (u32; += frame ms, at most 300 per frame) */
#define CT_STEP (BL_GP + 0x940u)        /* the timestep (float, 1.0 = 20 ms; min(non-clipped, 3.0)) */
/* fps_cap: the game's frame limit is the render driver's: its VBlank handler 0x34D818 flips the display only
   when at least N VBlanks passed since the last flip (counter byte gp'+0x1101, gp' = *(gp+0x110C)); N = byte gp-0x3824,
   set to 2 by the game's video init 0x39B790 through the setter 0x34FB78 (cleared by 0x34FF34 at shutdown). The main
   loop waits for the previous flip (0x34DFD8), so N VBlanks per frame = 30 fps at 60 Hz, 25 at 50 Hz. */
#define SKY_VSYNCS (BL_GP - 0x3824u)    /* 0x66814C */
#define SKY_GP (BL_GP + 0x110Cu)        /* the gp the driver's VBlank handler loads */

/* hle_bl_mode / _ctr / _per are the core's hle_rtpace_* (hle_bl.h) */
static int inited, o_lag_ms = 34, o_max_ms = 250, o_cap, o_cap_raw, o_log_s, o_vs, vs_logged;
static uint64_t st_vs_writes;
static uint64_t st_forgiven_us, st_jump_cyc, st_jumps, st_syncs, st_cap_waits, st_cap_wait_us, st_idle_forgive;

static uint64_t us_to_cyc(uint64_t us) { return us / 1000000u * EE_HZ + us % 1000000u * EE_HZ / 1000000u; }
static uint64_t cyc_to_us(uint64_t c) { return c / EE_HZ * 1000000u + c % EE_HZ * 1000000u / EE_HZ; }

/* x_fpscap (friendly fps_cap) -> the cap in effect: 60 / 30 / 25 / 20 / 15 (never above the asked value; 0 = off).
   60 Hz VBlanks: 60 = 1 VBlank per frame, 30 = 2, 20 = 3, 15 = 4; 25 = the PAL timing (50 Hz VBlanks, 2 per frame). */
static int cap_quant(int n)
{
    if (n <= 0) return 0;
    return n >= 60 ? 60 : n >= 30 ? 30 : n >= 25 ? 25 : n >= 20 ? 20 : 15;
}
static int vsyncs_for(int cap) { return cap == 60 ? 1 : cap == 20 ? 3 : cap == 15 ? 4 : 2; }

int hle_bl_pal50(void)
{
    int raw = hle_x_opt("fpscap", 0), cap = cap_quant(raw);
    if (!cap) return hle_x_opt("pal50", 1);              /* no cap: x_pal50 as before */
    LOG("fps cap: x_fpscap=%d -> %d fps: VBlank %d Hz, %d VBlank(s) per frame%s%s\n", raw, cap,
        cap == 25 ? 50 : 60, vsyncs_for(cap), raw != cap ? " (quantised down to the VBlank grid)" : "",
        cap == 60 ? " - EXPERIMENTAL (frame-count based game logic runs per frame)" : "");
    return cap == 25;
}

static int game_clock_s(void)          /* seconds of the day on the in-game clock (0x66C02C/30/34), -1 = implausible */
{
    unsigned h = gb(0x66C02Cu), m = gb(0x66C030u), sec = gh(0x66C034u);
    if (h > 23 || m > 59) return -1;
    return (int)(h * 3600u + m * 60u + (sec < 60u ? sec : 0u));
}
static float timestep(void) { uint32_t w = gw(CT_STEP); float f; memcpy(&f, &w, 4); return f; }

/* ------------------------------------------------------------------ clock log windows */
typedef struct {
    double rt;                          /* real s (hle_host_seconds) */
    uint64_t vt, it;                    /* virtual cycles, iterations */
    uint32_t ms;                        /* the game timer's ms */

    int clk;                            /* in-game clock s */
    int64_t clk_acc;                    /* in-game seconds accumulated (wraps at midnight handled) */
} Snap;
static Snap w0, a0;                     /* window start, whole-run start */
static int w_on;
static double w_min = 1e9, w_max, last_rt, ts_sum, all_min = 1e9, all_max, ts_all;
static uint64_t ts_n, fast_n, all_n, fast_all, ts_all_n;
static int64_t clk_acc;
static int clk_prev = -1;

static void snap(Snap *s)
{
    s->rt = hle_host_seconds(); s->vt = hle_vtime; s->it = hle_mainloop_iters; s->ms = gw(CT_MS);
    s->clk = game_clock_s(); s->clk_acc = clk_acc;
}

static double cap_ms(void)
{
    if (o_cap > 0) return 1000.0 / o_cap;
    return 2000.0 / (hle_vblank_hz > 0 ? hle_vblank_hz : 50);   /* the game's own limit: 2 VBlanks */
}

static void log_window(const Snap *a, const Snap *b, const char *what)
{
    double rs = b->rt - a->rt, vs = (double)(b->vt - a->vt) / EE_HZ, gs = (double)(uint32_t)(b->ms - a->ms) / 1000.0;
    double cs = (double)(b->clk_acc - a->clk_acc);
    uint64_t n = b->it - a->it;
    int64_t lag = (int64_t)hle_plat_pace_now_us() - (int64_t)cyc_to_us(hle_vtime);
    double phys = 0.02 * (what[0] == 'w' ? ts_sum : ts_all);   /* sum of the timesteps = the physics' game time */
    LOG("clock: %s real %.2f s, virtual %.2f s, game ms clock %.2f s (x%.3f of real), physics %.2f s (x%.3f "
        "of real), in-game clock +%.0f "
        "game s = %.3f game minutes per real s, %llu iterations = %.2f fps (interval min %.1f max %.1f ms, %llu under "
        "%.1f ms), timestep avg %.3f, VBlank %d Hz, x_pace %d, lag now %lld ms, forgiven %.2f s, catch-up %.2f s in %llu "
        "jumps, cap waits %llu (%.2f s)\n", what, rs, vs, gs, rs > 0 ? gs / rs : 0.0, phys, rs > 0 ? phys / rs : 0.0, cs,
        rs > 0 ? cs / 60.0 / rs : 0.0,
        (unsigned long long)n, rs > 0 ? n / rs : 0.0, (w_on && what[0] == 'w' ? w_min : all_min) * 1000.0,
        (w_on && what[0] == 'w' ? w_max : all_max) * 1000.0,
        (unsigned long long)(what[0] == 'w' ? fast_n : fast_all), cap_ms() - 1.0,
        what[0] == 'w' ? (ts_n ? ts_sum / (double)ts_n : 0.0) : (ts_all_n ? ts_all / (double)ts_all_n : 0.0),
        hle_vblank_hz, hle_bl_mode, (long long)(lag / 1000),
        (double)st_forgiven_us / 1e6, (double)st_jump_cyc / EE_HZ, (unsigned long long)st_jumps,
        (unsigned long long)st_cap_waits, (double)st_cap_wait_us / 1e6);
}

/* ------------------------------------------------------------------ host pacing simulation */
#ifndef HLE_PSP
static int sim_n, sim_i;
static int sim_ms[16];
static double sim_last;
static void sim_init(void)
{
    const char *e = getenv("HLE_BL_SIM_MS");
    while (e && *e && sim_n < 16) {
        sim_ms[sim_n++] = atoi(e);
        e = strchr(e, ',');
        if (e) e++;
    }
    if (sim_n) {
        LOG("clock: host pacing simulation - main-loop iterations take at least");
        for (int i = 0; i < sim_n; i++) LOG(" %d", sim_ms[i]);
        LOG(" ms of real time (cycled)\n");
    }
}
static void sim_wait(void)
{
    if (!sim_n) return;
    double now = hle_host_seconds();
    if (sim_last > 0) {
        double due = sim_last + sim_ms[sim_i++ % sim_n] / 1000.0;
        if (due > now) {
            double d = due - now;
            struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
            nanosleep(&ts, NULL);
        }
    }
    sim_last = hle_host_seconds();
}
#endif

static void init(void)
{
    inited = 1;
    hle_bl_mode = hle_x_opt("pace", 0);
    o_lag_ms = hle_x_opt("pacelag", 34);
    o_max_ms = hle_x_opt("pacemax", 250);
    int per = hle_x_opt("paceper", 64);
    hle_bl_per = per > 0 ? (uint32_t)per : 64u;
    o_cap_raw = hle_x_opt("fpscap", 0);
    o_cap = cap_quant(o_cap_raw);
    o_vs = o_cap ? vsyncs_for(o_cap) : 0;
    o_log_s = hle_x_opt("clocklog", 0);
    hle_bl_mode = hle_x_opt("pace", -1);
    if (hle_bl_mode < 0) hle_bl_mode = o_cap ? 2 : 0;     /* fps_cap without x_pace: real-time pacing */
    if (hle_bl_mode > 2) hle_bl_mode = 0;
    if (o_lag_ms < 0) o_lag_ms = 0;
    if (o_max_ms < 0) o_max_ms = 0;
    int64_t lag = (int64_t)hle_plat_pace_now_us() - (int64_t)cyc_to_us(hle_vtime);
    if (hle_bl_mode) {                                   /* start the main loop with virtual time = the real clock */
        hle_plat_pace_adjust(lag);
    }
    if (hle_bl_mode || o_cap || o_log_s)
        LOG("config: x_pace=%d (%s) x_pacelag=%d x_pacemax=%d x_paceper=%u x_fpscap=%d (cap %d fps, driver VBlanks "
            "per frame %u -> %d, driver gp %08x) x_clocklog=%d; VBlank "
            "%d Hz (game limit %.1f ms); at the first main-loop iteration the real clock was %lld ms %s virtual time%s\n",
            hle_bl_mode, hle_bl_mode == 2 ? "real time" : hle_bl_mode == 1 ? "no debt" : "as before", o_lag_ms, o_max_ms,
            (unsigned)hle_bl_per, o_cap_raw, o_cap, gb(SKY_VSYNCS), o_vs ? o_vs : gb(SKY_VSYNCS), gw(SKY_GP), o_log_s,
            hle_vblank_hz, (o_vs ? o_vs : 2) * 1000.0 / (hle_vblank_hz ? hle_vblank_hz : 50),
            (long long)((lag < 0 ? -lag : lag) / 1000), lag < 0 ? "behind" : "ahead of",
            hle_bl_mode ? " (re-aligned)" : " (x_pace=0: kept - the old behaviour)");
#ifndef HLE_PSP
    sim_init();
#endif
}

/* the in-game menu changes fps_cap (x_fpscap through hle_x_set): the cap and the driver's VBlanks per frame
   follow from the next iteration (off: the game's own 2); the VBlank rate (25 = the 50 Hz PAL timing) stays the boot's.
   Nothing happens unless hle_x_set was called (hle_x_gen stays 0). */
static int bl_gen;
static void bl_recap(void)
{
    bl_gen = hle_x_gen;
    int raw = hle_x_opt("fpscap", 0);
    if (raw == o_cap_raw) return;
    o_cap_raw = raw;
    o_cap = cap_quant(raw);
    o_vs = o_cap ? vsyncs_for(o_cap) : 2;
    LOG("fps cap: x_fpscap=%d -> %d fps at run time (driver VBlanks per frame %d)\n", raw, o_cap, o_vs);
}

void hle_bl_tick(void)
{
    if (!inited) init();
    if (bl_gen != hle_x_gen) bl_recap();
#ifndef HLE_PSP
    sim_wait();
#endif
    if (o_vs && gb(SKY_VSYNCS) != (uint8_t)o_vs) {       /* the game's own limit: N VBlanks per flip (see SKY_VSYNCS) */
        if (!vs_logged++)
            LOG("fps cap: driver VBlanks per frame %u -> %d at main-loop iteration %llu\n", gb(SKY_VSYNCS), o_vs,
                (unsigned long long)hle_mainloop_iters);
        sb_(SKY_VSYNCS, (uint8_t)o_vs);
        st_vs_writes++;
    }
    if (o_cap > 0 && last_rt > 0) {                      /* hard cap: never sooner than 1000/N ms after the last start */
        double due = last_rt + 1.0 / o_cap, now = hle_host_seconds();
        if (due > now) {
            st_cap_waits++;
            st_cap_wait_us += (uint64_t)((due - now) * 1e6);
            for (int k = 0; k < 4 && due > now; k++) {   /* a host sleep may return early (signals) */
                hle_plat_pace(us_to_cyc(hle_plat_pace_now_us() + (uint64_t)((due - now) * 1e6) + 1u));
                now = hle_host_seconds();
            }
        }
    }
    double now = hle_host_seconds();
    int clk = game_clock_s();
    if (clk >= 0 && clk_prev >= 0) {                     /* a frame advances the clock by <= 18 game s (300 ms); */
        int dc = (clk - clk_prev + 86400) % 86400;       /* more = the game set the time (script, load, sleep): skip */
        if (dc <= 60) clk_acc += dc;
    }
    if (clk >= 0) clk_prev = clk;
    if (last_rt > 0 && hle_mainloop_iters > 150) {       /* statistics after the mission start's own loads */
        double d = now - last_rt;
        if (d < w_min) w_min = d;
        if (d > w_max) w_max = d;
        if (d < all_min) all_min = d;
        if (d > all_max) all_max = d;
        if (d * 1000.0 < cap_ms() - 1.0) { fast_n++; fast_all++; }
        all_n++;
        float ts = timestep();
        if (ts > 0.0f && ts < 100.0f) { ts_sum += ts; ts_n++; ts_all += ts; ts_all_n++; }
    }
    last_rt = now;
    if (!o_log_s || hle_mainloop_iters < 150) return;
    if (!w_on) { w_on = 1; snap(&w0); a0 = w0; w_min = 1e9; w_max = 0; fast_n = 0; ts_sum = 0; ts_n = 0; return; }
    if (now - w0.rt < (double)o_log_s) return;
    Snap b;
    snap(&b);
    log_window(&w0, &b, "window");
    w0 = b;
    w_min = 1e9; w_max = 0; fast_n = 0; ts_sum = 0; ts_n = 0;
}

void hle_bl_pace(uint64_t vtime)
{
    if (!hle_bl_mode || !hle_mainloop_iters) return;
    uint64_t now = hle_plat_pace_now_us(), due = cyc_to_us(vtime);
    if (now <= due) return;
    uint64_t lag = now - due, keep = (uint64_t)(hle_bl_mode == 1 ? o_lag_ms : o_max_ms) * 1000u;
    if (lag > keep) {
        hle_plat_pace_adjust((int64_t)(lag - keep));
        st_forgiven_us += lag - keep;
        st_idle_forgive++;
    }
}

void hle_bl_rt_sync(void)
{
    if (!hle_mainloop_iters) return;
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

void hle_bl_report(void)
{
    if (!inited || (!hle_bl_mode && !o_cap && !o_log_s)) return;
    if (o_log_s && w_on) {
        Snap b;
        snap(&b);
        log_window(&a0, &b, "summary");
    }
    LOG("clock: x_pace=%d: real-clock checks %llu, catch-up jumps %llu (%.2f s of virtual time), lag forgiven "
        "%.2f s (%llu times at idle points), cap waits %llu (%.2f s); iteration intervals min %.1f max %.1f ms, %llu of "
        "%llu under %.1f ms; fps cap %d (asked %d), driver VBlanks per frame %d (%llu writes)\n", hle_bl_mode,
        (unsigned long long)st_syncs, (unsigned long long)st_jumps,
        (double)st_jump_cyc / EE_HZ, (double)st_forgiven_us / 1e6, (unsigned long long)st_idle_forgive,
        (unsigned long long)st_cap_waits, (double)st_cap_wait_us / 1e6, all_min < 1e8 ? all_min * 1000.0 : 0.0,
        all_max * 1000.0, (unsigned long long)fast_all, (unsigned long long)all_n, cap_ms() - 1.0, o_cap, o_cap_raw,
        o_vs ? o_vs : (int)gb(SKY_VSYNCS), (unsigned long long)st_vs_writes);
}

/* the core's pacing hooks (hle.h) */
void hle_pack_rt_sync(void) { hle_bl_rt_sync(); }
void hle_pack_pace(uint64_t vtime) { hle_bl_pace(vtime); }
int hle_pack_vblank_pal50(void) { return hle_bl_pal50(); }
