/* hle_au.c - x_fxskip (render-only guest effect passes, run-time switchable) and
 * the ge_busy GE sampler. Keys and semantics: hle_au.h. Default: nothing changes (the hooks call the originals; no
 * thread). */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared poc6.cfg table */
#include "hle_au.h"
#include "hle_fast.h"
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#endif

/* ------------------------------------------------------------------ x_fxskip */
void GUEST(00116180)(R5900Ctx *c);
void GUEST(00117700)(R5900Ctx *c);
void GUEST(002e1350)(R5900Ctx *c);
static int fx_mask, fx_gen = -1;                 /* re-read when hle_x_gen changes (run-time A/B switches) */
static uint64_t fx_skipped[3], fx_ran[3];
static inline int fxskip(void)
{
    if (fx_gen != hle_x_gen) { fx_gen = hle_x_gen; fx_mask = hle_x_opt("fxskip", 0); }
    return fx_mask;
}
HLE(00116180)                                    /* shadows_stored(): the stored (per-frame) shadows */
{
    if (fxskip() & 1) { fx_skipped[0]++; return; }
    fx_ran[0]++;
    GUEST(00116180)(c);
}
HLE(00117700)                                    /* shadows_static(): the static shadows */
{
    if (fxskip() & 1) { fx_skipped[1]++; return; }
    fx_ran[1]++;
    GUEST(00117700)(c);
}
HLE(002e1350)                                    /* plane_trails(): the sky contrails */
{
    if (fxskip() & 2) { fx_skipped[2]++; return; }
    fx_ran[2]++;
    GUEST(002e1350)(c);
}

/* ------------------------------------------------------------------ ge_busy */
static int gb_period;                             /* us; 0 = off */
static volatile uint32_t gb_n[AU_GE_STATES];
static volatile int gb_quit, gb_running;
static int gb_err;

int hle_au_ge_on(void) { return gb_running; }

void hle_au_ge_counts(uint32_t out[AU_GE_STATES])
{
    for (int i = 0; i < AU_GE_STATES; i++) out[i] = gb_n[i];
}

#ifdef HLE_PSP
static int gb_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!gb_quit) {
        int s = sceGeDrawSync(1);                 /* peek: no wait */
        int k = s < 0 ? 5 : s >= 4 ? 4 : s;
        gb_n[k]++;
        sceKernelDelayThread((SceUInt)gb_period);
    }
    gb_running = 0;
    return 0;
}

static void gb_load(void)
{
    const char *v = hle_cfg_get("ge_busy");          /* the shared table (hle_cfg.c) */

    if (v) gb_period = atoi(v);
}
#endif

void hle_au_init(void)
{
#ifdef HLE_PSP
    gb_load();
    if (gb_period <= 0) return;
    if (gb_period < 200) gb_period = 200;
    SceUID t = sceKernelCreateThread("au_gebusy", gb_thread, 0x1F, 0x1000, 0, NULL);
    if (t < 0) { gb_err = t; LOG("ge_busy: sampler thread NOT created (%08x)\n", (unsigned)t); return; }
    gb_running = 1;
    int r = sceKernelStartThread(t, 0, NULL);
    if (r < 0) { gb_running = 0; gb_err = r; LOG("ge_busy: sampler NOT started (%08x)\n", (unsigned)r); return; }
    LOG("ge_busy: GE sampler on, every %d us (sceGeDrawSync peek; thread priority 0x1F)\n", gb_period);
#endif
}

void hle_au_report(void)
{
    gb_quit = 1;                                  /* the sampler ends with the run (before the exit sequence) */
    if (fx_skipped[0] + fx_skipped[1] + fx_skipped[2])
        LOG("x_fxskip: skipped / ran - stored shadows %llu / %llu, static shadows %llu / %llu, "
            "plane trails %llu / %llu\n", (unsigned long long)fx_skipped[0], (unsigned long long)fx_ran[0],
            (unsigned long long)fx_skipped[1], (unsigned long long)fx_ran[1], (unsigned long long)fx_skipped[2],
            (unsigned long long)fx_ran[2]);
    if (gb_period <= 0) return;
    uint32_t n[AU_GE_STATES];
    hle_au_ge_counts(n);
    uint64_t all = 0;
    for (int i = 0; i < AU_GE_STATES; i++) all += n[i];
    LOG("ge_busy: %llu samples every %d us (whole run): drawing %u, stalled on the list end %u, done/idle %u, "
        "queued %u, other %u, error %u -> GE busy %.1f %% of the time%s\n", (unsigned long long)all, gb_period, n[2],
        n[3], n[0], n[1], n[4], n[5], all ? 100.0 * (double)n[2] / (double)all : 0.0,
        gb_err ? " (sampler failed to start)" : "");
}
