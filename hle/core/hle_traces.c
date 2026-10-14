/* hle_traces.c - generated-by-preprocessor call-log wrappers for every TRACE / TRACE_OV entry of the game pack's
 * hooks.def: the original (renamed fn_<addr>__guest by prepare_gen) still runs; the wrapper records the call in the
 * trace ring, logs it at --verbose 1, and reports a milestone on the first call when flags & 1; flags & 8 logs the
 * 64-bit a0 / a1 in and v0 / f0 out; every other flag bit is the game pack's (hle_pack_trace). */
#include "hle.h"

uint64_t hle_mainloop_iters;                    /* calls of the pack's main-loop function (game frames) */

static void trace_enter(const char *name, uint32_t addr, R5900Ctx *c, int flags, int *seen)
{
    if (addr == HLE_PACK_MAIN_LOOP_ADDR) { hle_mainloop_iters++; hle_mainloop_tick(); }
    if (addr == HLE_PACK_SELFTEST_ADDR && hle_selftest_pending) {
        hle_selftest_pending = 0;
        hle_selftest_run(c);
    }
    hle_trace_call(name, addr, c);
    if ((flags & ~9) && hle_pack_trace) hle_pack_trace(name, addr, c, flags, 0);   /* other flags: the pack's views */
    if (flags & 8) {                    /* debug: 64-bit a0/a1 at entry of calls 1-8 and every 256th (as on leave) */
        static unsigned m[4];
        unsigned k = (addr >> 4) & 3, i = ++m[k];
        if (i <= 8 || (i & 255) == 0)
            LOG("TRACE %s #%u enter a0 %016llx a1 %016llx vtime %.4f\n", name, i, (unsigned long long)c->r[4].d[0],
                (unsigned long long)c->r[5].d[0], (double)hle_vtime / EE_HZ);
    }
    if ((flags & 1) && !*seen) {
        char m[96];
        snprintf(m, sizeof m, "%s (%08x) entered", name, addr);
        hle_milestone(m);
    }
    *seen = 1;
}

static void trace_leave(const char *name, uint32_t addr, R5900Ctx *c, int flags, int *returned)
{
    if ((flags & ~9) && hle_pack_trace) hle_pack_trace(name, addr, c, flags, 1);
    if (flags & 8) {                    /* debug: return values (v0 64-bit, f0) of calls 1-8 and every 256th */
        static unsigned n[4];
        unsigned k = (addr >> 4) & 3, i = ++n[k];
        if (i <= 8 || (i & 255) == 0) {
            float f;
            memcpy(&f, &c->fpr[0].u, 4);
            LOG("TRACE %s #%u returns v0 %016llx f0 %g (%08x) a0-in? vtime %.4f\n", name, i,
                (unsigned long long)c->r[2].d[0], f, c->fpr[0].u, (double)hle_vtime / EE_HZ);
        }
    }
    if ((flags & 1) && !*returned) {
        char m[96];
        snprintf(m, sizeof m, "%s (%08x) returned v0=%08x", name, addr, (uint32_t)c->r[2].d[0]);
        hle_milestone(m);
    }
    *returned = 1;
}

#define HOOK(addr, name)
#define HOOK_OV(n, addr, name)
#define TRACE(addr, name, flags)                                                                                    \
    void fn_##addr##__guest(R5900Ctx *c);                                                                           \
    void fn_##addr(R5900Ctx *c)                                                                                     \
    {                                                                                                               \
        static int seen, returned;                                                                                  \
        trace_enter(#name, 0x##addr, c, flags, &seen);                                                              \
        fn_##addr##__guest(c);                                                                                      \
        trace_leave(#name, 0x##addr, c, flags, &returned);                                                          \
    }
#define TRACE_OV(n, addr, name, flags)                                                                             \
    void ov##n##_fn_##addr##__guest(R5900Ctx *c);                                                                  \
    void ov##n##_fn_##addr(R5900Ctx *c)                                                                            \
    {                                                                                                               \
        static int seen, returned;                                                                                  \
        trace_enter(#name, 0x##addr, c, flags, &seen);                                                              \
        ov##n##_fn_##addr##__guest(c);                                                                             \
        trace_leave(#name, 0x##addr, c, flags, &returned);                                                          \
    }
#include "hooks.def"
