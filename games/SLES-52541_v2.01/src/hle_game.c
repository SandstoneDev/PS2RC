/* hle_game.c - hooks on the game's own functions (not Sony library code): the idle thread, the code-overlay loader, and
 * stand-ins for translated functions the boot needs but the back-end could not translate yet (VU0 macro mode,
 * COP0 BC0x DMA waits) - each one documented with what the original does. */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_render.h"
#include "hle_prof.h"
#include "hle_xgame.h"
#include "hle_tune.h"
/* optional developer module (in-run A/B windows), not part of every build */
void hle_ab_init(void) __attribute__((weak));
void hle_ab_tick(void) __attribute__((weak));
void hle_ab_report(void) __attribute__((weak));
#include "hle_bl.h"

/* ------------------------------------------------------------------ idle thread (created at 0x245FF0, prio 127):
   the original spins `nop x5; b .` forever. On the host it advances virtual time to the next event instead. */
HLE(00245fd0)
{
    hle_milestone("idle thread first scheduled (all other threads blocked)");
    for (;;) {
        hle_idle_advance();
        hle_reschedule();
    }
}

/* ------------------------------------------------------------------ code overlays (MWo3 overlay files at 0x88E500)
   0x3C6CB0 overlay_load(name, addr): if name != current: FlushCache, 0x3C6B20(name, addr) reads the file with
   sceOpen/sceLseek/sceRead, clears bss, runs the overlay's static constructors (0x556DF0), remembers the name.
   The harness hooks the file reader 0x3C6B20: right after the image is in memory (before the constructors run) the
   dispatcher is switched to the matching translated overlay unit. */
rc_fn ov1_rc_lookup(uint32_t), ov2_rc_lookup(uint32_t), ov3_rc_lookup(uint32_t), ov4_rc_lookup(uint32_t),
    ov5_rc_lookup(uint32_t), ov6_rc_lookup(uint32_t), ov7_rc_lookup(uint32_t), ov8_rc_lookup(uint32_t);
void fn_003c6b20__guest(R5900Ctx *c);

void hle_overlays_register(void)
{
    rc_overlay_lookup[1] = ov1_rc_lookup;
    rc_overlay_lookup[2] = ov2_rc_lookup;
    rc_overlay_lookup[3] = ov3_rc_lookup;
    rc_overlay_lookup[4] = ov4_rc_lookup;
    rc_overlay_lookup[5] = ov5_rc_lookup;
    rc_overlay_lookup[6] = ov6_rc_lookup;
    rc_overlay_lookup[7] = ov7_rc_lookup;
    rc_overlay_lookup[8] = ov8_rc_lookup;
    rc_overlay_current = 0;
    rc_overlay_lo = 0x88E500;
    rc_overlay_hi = 0x8A5D80;                              /* largest slot end (the init and jpeg overlays are the biggest) */
}

/* ------------------------------------------------------------------ overlay 1: FMV player
   0x2459F0 loads overlay 1 and calls 0x89AB90 (init: SIF/IOP heap, sceGsResetGraph, sceMpegInit, sceSdRemoteInit ->
   sdrdrv RPC 0x80000701), 0x89AC60(path, flag, x) per movie (IPU decode + SPU2 audio via sdrdrv; 17 MMI functions
   of libmpeg are not translated), 0x89AC50 (shutdown). No IPU/SPU2 on the host: the movies are skipped.
   PSP: 0x89AC60(path, a1, a2) plays the install-time conversion <EBOOT dir>/movies/<NAME>.pmf with sceMpeg
   (hle_movie.c; a1 != 0: X skips at once, a1 == 0: after a2 bytes of the PSS - 2,750,000 for the first intro movie)
   and returns like the skip (v0 = 0; the caller 0x2459F0 does not read it); no PMF,
   ps2rc.cfg movies=0 or a refusing decoder: skipped as on the host. */
#ifdef HLE_PSP
#include "hle_movie.h"                         /* the core's PSP layer (copied next to this file by the build) */
#endif
HLE_OV(1, 0089ab90) { hle_trace_call("movie_init (skipped)", 0x89ab90, c); RET(0); }
HLE_OV(1, 0089ac50) { hle_trace_call("movie_shutdown (skipped)", 0x89ac50, c); RET(0); }
HLE_OV(1, 0089ac60)
{
#ifdef HLE_PSP
    if (hle_movie_play(gstr(ARG(0)), ARG(1), ARG(2))) {
        hle_trace_call("movie_play (PSP movie)", 0x89ac60, c);
        RET(0);
        return;
    }
#endif
    LOG("FMV \"%s\" skipped (no IPU/SPU2 on the host)\n", gstr(ARG(0)));
    hle_trace_call("movie_play (skipped)", 0x89ac60, c);
    hle_milestone("intro movies skipped (movie overlay 0x89AC60)");
    RET(0);
}

HLE(003c6b20)
{
    uint32_t name = ARG(0), addr = ARG(1);
    LOG("loading code overlay \"%s\" to %08x\n", gstr(name), addr);
    hle_trace_call("overlay_file_load", 0x3c6b20, c);
    fn_003c6b20__guest(c);
    hle_overlay_loaded(addr);
}

/* ================================================================== the VU0 micro-program glue and the self-test of
   this game (game-specific parts kept out of the core's hle_main.c) */
#ifdef HLE_HAVE_VU0_MICRO
#include "vu0_micro.h"
static void vcallms_hle(R5900Ctx *c, uint32_t pc, uint32_t addr)
{
    if (!hle_stat_vcallms_hle++) {
        /* the microcode the game uploaded (the init overlay DMAs the chain at 0x5A2A00 to VU0) must be program #0 */
        int ok = vu0_micro_is_prog0(gptr(0x005A2A10u, VU0_MICRO_PROG0_BYTES), VU0_MICRO_PROG0_BYTES);
        LOG("VCALLMS at %08x (VU0 %04x): first call into the VU0 micro-program module; "
            "microcode at 0x5A2A10 is program #0: %s\n", pc, addr, ok ? "yes" : "NO");
        hle_milestone("first VCALLMS run by the VU0 micro HLE");
        if (!ok) hle_die(c, "VU0 microcode at 0x5A2A10 is not program #0 - the micro HLE does not apply");
    }
    int r;
    /* x_vu0=1 runs the same program in float registers (a developer module, not in every checkout; bit-identical to
       rt_vu0_callms_hle on the oracle vectors + random states) */
    static int x_vu0 = -1, x_vu0_gen = -1;      /* re-read when hle_x_gen changes (run-time A/B) */
    if (x_vu0_gen != hle_x_gen) { x_vu0_gen = hle_x_gen; x_vu0 = hle_x_opt("vu0", 0) != 0; }
    { PZ_SCOPE(vu0); r = x_vu0 && x_vu0_callms ? x_vu0_callms(c, addr) : rt_vu0_callms_hle(c, addr); }
    if (r != 0)
        hle_die(c, "VCALLMS at %08x: VU0 micro address %04x is not one of program #0's entries (VU0 micro HLE)", pc,
                addr);
}
/* the core's VCALLMS mode "hle" calls it (hle.h hle_pack_vcallms) */
void hle_pack_vcallms(R5900Ctx *c, uint32_t pc, uint32_t addr) __attribute__((alias("vcallms_hle")));
#endif

/* ------------------------------------------------------------------ debug self-test (host --selftest): translated
   library functions in isolation, run once at the entry of game_init_data (libc and the heap are initialised by
   then) on a saved copy of the guest context, with a scratch area below the current guest stack: MSL sscanf with %f
   and _strtod_r on known strings - compares translation variants (v0 vs v4) function by function */
void hle_selftest_run(R5900Ctx *c)
{
    R5900Ctx save = *c;
    uint32_t sp = ((uint32_t)c->r[29].d[0] - 0x2000) & ~15u, str = sp - 0x400, fmt = sp - 0x300, out = sp - 0x200;
    const char *s = "1608.19 -1721.8 26 0.5 -0.25", *f = "%f %f %f %f %f";
    gcopy_out(str, s, (uint32_t)strlen(s) + 1);
    gcopy_out(fmt, f, (uint32_t)strlen(f) + 1);
    memset(gptr(out, 64), 0, 64);
    c->r[29].d[0] = sp - 0x800;
    uint32_t r = hle_call(c, 0x005517a8u, 7, str, fmt, out, out + 4, out + 8, out + 12, out + 16);
    float v[5];
    memcpy(v, gptr(out, 20), 20);
    LOG("SELFTEST sscanf(\"%s\", \"%s\") = %d -> %g %g %g %g %g (%08x %08x %08x %08x %08x)\n", s, f, (int)r, v[0], v[1],
        v[2], v[3], v[4], gw(out), gw(out + 4), gw(out + 8), gw(out + 12), gw(out + 16));
    const char *nums[] = {"1608.19", "-1721.8", "26", "0.5", "123456789", "1e10"};
    for (int i = 0; i < 6; i++) {
        gcopy_out(str, nums[i], (uint32_t)strlen(nums[i]) + 1);
        c->r[29].d[0] = sp - 0x800;
        hle_call(c, 0x00552818u, 3, gw(0x0062b708u) /* _impure_ptr */, str, 0);
        LOG("SELFTEST _strtod_r(\"%s\") -> v0 %016llx v1 %016llx\n", nums[i], (unsigned long long)c->r[2].d[0],
            (unsigned long long)c->r[3].d[0]);
    }
    /* libgcc soft-double helpers with full 64-bit register arguments (a0, a1), results in v0 / f0 */
    static const struct { uint32_t fn; const char *name; uint64_t a0, a1; } t[] = {
        {0x0039be60u, "_f_ulltof", 0x000000011F058122ull, 0},
        {0x00531448u, "__floatdisf", 0x000000011F058122ull, 0},
        {0x005323e0u, "litodp", 1, 0},
        {0x00531ea8u, "dpmul", 0x3FF0000000000000ull, 0x40F0000000000000ull},
        {0x00531de8u, "dpadd", 0x41F0000000000000ull, 0x41BF058122000000ull},
        {0x00532568u, "dptofp", 0x41F1F05812200000ull, 0},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        c->r[29].d[0] = sp - 0x800;
        c->r[4].d[0] = t[i].a0;
        c->r[5].d[0] = t[i].a1;
        uint64_t ra = c->r[31].d[0];
        c->r[31].d[0] = 0;
        rc_dispatch(c, t[i].fn);
        c->r[31].d[0] = ra;
        float f;
        memcpy(&f, &c->fpr[0].u, 4);
        LOG("SELFTEST %s(%016llx, %016llx) -> v0 %016llx f0 %g (%08x)\n", t[i].name, (unsigned long long)t[i].a0,
            (unsigned long long)t[i].a1, (unsigned long long)c->r[2].d[0], f, c->fpr[0].u);
    }
    *c = save;
}

/* ================================================================== the game pack's interface (hle.h) */
#ifndef HLE_PSP
/* host harness options of the render boundary (hle_render.c): --frames DIR, --frame-every N, --frame-max N,
   --render-log N, --render-trace N */
int hle_pack_host_arg(int argc, char **argv, int *pi)
{
    int i = *pi;
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) hle_opt_frames_dir = argv[++i];
    else if (!strcmp(argv[i], "--frame-every") && i + 1 < argc) hle_opt_frame_every = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--frame-max") && i + 1 < argc) hle_opt_frame_max = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--render-log") && i + 1 < argc) hle_opt_render_log = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--render-trace") && i + 1 < argc) hle_opt_render_trace = strtoull(argv[++i], 0, 10);
    else return 0;
    *pi = i;
    return 1;
}
#endif

void hle_pack_boot_init(void)
{
    hle_render_init();
    hle_tune_init();                                         /* tune_* keys (hle_tune.c) */
    if (hle_ab_init) hle_ab_init();                                           /* ab_* keys (developer module), default off */
    hle_overlays_register();
}

void hle_pack_report(void)
{
    hle_render_report();
    hle_tune_report();                     /* tune_* summary */
}

/* ================================================================== the debug views of the TRACE flags 2 / 4 / 16
   (this game's structures, kept out of the core's hle_traces.c) */
/* flags & 2 (debug): log the first 48 calls with the four floats a1 points to (a rectangle / vector argument) */
static void trace_rect(const char *name, R5900Ctx *c)
{
    static unsigned n;
    if (n++ >= 48) return;
    uint32_t p = (uint32_t)c->r[5].d[0], w[4];
    for (int i = 0; i < 4; i++) w[i] = p < hle_ram_end - 16 ? gw(p + 4 * i) : 0;
    float f[4];
    memcpy(f, w, sizeof f);
    LOG("TRACE %s #%u: a0 %08x a1 %08x -> %g %g %g %g (%08x %08x %08x %08x) vtime %.4f\n", name, n,
        (uint32_t)c->r[4].d[0], p, f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3], (double)hle_vtime / EE_HZ);
}

/* flags & 4 (debug): the entity a0 points to - simple transform (+4: pos, heading) and the matrix (+0x14) */

static void trace_placeable(const char *name, R5900Ctx *c)
{
    static unsigned n;
    if (n++ >= 48) return;
    uint32_t e = (uint32_t)c->r[4].d[0];
    if (e >= hle_ram_end - 0x20) return;
    float p[4];
    uint32_t w[4];
    for (int i = 0; i < 4; i++) w[i] = gw(e + 4 + 4 * i);
    memcpy(p, w, sizeof p);
    uint32_t m = gw(e + 0x14);
    LOG("TRACE %s #%u: entity %08x pos %g %g %g heading %g matrix %08x", name, n, e, p[0], p[1], p[2], p[3], m);
    if (m && m < hle_ram_end - 0x40) {
        float q[4];
        for (int i = 0; i < 4; i++) w[i] = gw(m + 0x30 + 4 * i);
        memcpy(q, w, sizeof q);
        LOG(" -> matrix pos %g %g %g (%08x %08x %08x)", q[0], q[1], q[2], w[0], w[1], w[2]);
    }
    LOG("\n");
}

/* flags & 16 (debug): the loading-screen state block gp+6572..6636 (0x66CF1C..), first 24 calls, enter and leave */
static void trace_lsblock(const char *name, const char *when)
{
    uint32_t b = 0x0066B970u + 6572u;
    float f[7];
    uint32_t w[7];
    for (int i = 0; i < 7; i++) w[i] = gw(b + 36 + 4 * i);                 /* 6608..6632: floats */
    memcpy(f, w, sizeof f);
    LOG("TRACE %s %s: flags 6572..6604 %02x %02x %02x %02x %02x %02x %02x %02x %02x | 6608 %g 6612 %g 6616 %g 6620 %g "
        "6624 %g 6628 %g | 6632 %02x 6636 %02x vtime %.4f thr %d\n", name, when, gb(b), gb(b + 4), gb(b + 8),
        gb(b + 12), gb(b + 16), gb(b + 20), gb(b + 24), gb(b + 28), gb(b + 32), f[0], f[1], f[2], f[3], f[4], f[5],
        gb(b + 60), gb(b + 64), (double)hle_vtime / EE_HZ, hle_cur ? hle_cur->id : 0);
}

void hle_pack_trace(const char *name, uint32_t addr, R5900Ctx *c, int flags, int leave)
{
    if (!leave) {
        if (flags & 2) trace_rect(name, c);
        if (flags & 4) trace_placeable(name, c);
        if (flags & 16) {
            static unsigned n1[4];
            if (n1[(addr >> 4) & 3]++ < 24) trace_lsblock(name, "enter");
        }
    } else if (flags & 16) {
        static unsigned n2[4];
        if (n2[(addr >> 4) & 3]++ < 24) trace_lsblock(name, "leave");
    }
}

/* per main-loop iteration / end of run (core hooks, hle.h): the in-run A/B windows, the frame cap */
void hle_pack_frame_tick(void)
{
    if (hle_ab_tick) hle_ab_tick();
    hle_bl_tick();
}
void hle_pack_timing_report(void)
{
    if (hle_ab_report) hle_ab_report();
    hle_bl_report();
}
