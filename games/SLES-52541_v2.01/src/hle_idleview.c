/* hle_idleview.c - when the idle camera starts (hook idle_view_due 0x207C60).
 *
 * Guest data (SLES-52541 v2.01, checked in PCSX2): the idle camera object is at 0x6FFF20. Every frame in which the
 * player character stands still on foot (follow camera, single player), the game adds 1 to its frame count (+148);
 * any other frame zeroes it, and the count restarts when pad 0's last-input time (0x701840: the game's millisecond
 * clock 0x66C294 of the last frame with a button or a stick past the game's dead zone) changes. idle_view_due
 * 0x207C60 (this = 0x6FFF20) answers count > 30 x (+40: a float in ms, 90000.0) / 1000 = 2700 FRAMES - a frame count
 * that assumes 30 fps. Once on, the idle camera sets bit 0 of 0x8412AC (the HUD hides) and drifts to nearby peds and
 * cars; the next input ends it.
 *
 * PS2 (PAL, 25 fps): 2700 frames = 108 s after the last input (PCSX2: on at count 2704, 107.9 s after the last
 * input). Here the game runs 25 frames per VIRTUAL second, but on a PSP-3000 virtual time runs at ~0.48x real time
 * (measured: ~12 game frames/s), so the same 2700 frames take ~225 s of real standing still. Nothing in the pad
 * emulation moves the last-input time: sticks 0x80 (PSP dead zone 40, the game's own +-42), no pressure bytes, no
 * PSP-only buttons.
 *
 * The hook: the game's own answer runs first (its side effects stay); then, per cfg key idle_view= (PSP cfg file;
 * host: env HLE_IDLE_VIEW), the answer is
 *   ps2   (PSP default)  real time: the frames of the current count, timed with the platform clock (a frame slower
 *                        than 0.5 s - pause menu, loading - counts 0.5 s), >= m_TimeControlsIdleForIdleToKickIn x 30/25
 *                        (= 108 s, the PAL PS2's time)
 *   N     (seconds)      the same with N seconds
 *   game  (host default) the game's own frame count (deterministic; host runs and gates)
 * Every on / off is logged ("idle cam: on ...").
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared cfg table */
#ifdef HLE_PSP
#include <pspiofilemgr.h>
#endif

#define IC_PAD0_LTT   0x00701840u          /* pad 0: the clock (ms) of the last input */
#define IC_TIMER_MS   0x0066C294u          /* the game's millisecond clock (gp+2340) */
#define IC_DT_MAX     0.5                  /* s: a longer gap between two counted frames is a pause / loading screen */

enum { IC_GAME, IC_PS2, IC_SECONDS };
#ifdef HLE_PSP
static int ic_mode = IC_PS2;
#else
static int ic_mode = IC_GAME;
#endif
static double ic_seconds;
static int ic_ready, ic_on;
static uint32_t ic_last_tick, ic_logs;
static double ic_acc, ic_last_t;

static void ic_set(const char *v)
{
    while (*v == ' ') v++;
    if (!strncmp(v, "game", 4)) ic_mode = IC_GAME;
    else if (!strncmp(v, "ps2", 3)) ic_mode = IC_PS2;
    else if (*v >= '0' && *v <= '9') {
        double s = strtod(v, NULL);
        if (s > 0.0) { ic_mode = IC_SECONDS; ic_seconds = s; }
        else ic_mode = IC_GAME;
    }
}

static void ic_load(void)
{
    ic_ready = 1;
#ifdef HLE_PSP
    const char *s = hle_cfg_get("idle_view");          /* the shared table (hle_cfg.c) */
    if (s) ic_set(s);
#else
    const char *e = getenv("HLE_IDLE_VIEW");
    if (e && *e) ic_set(e);
#endif
    char what[64];
    if (ic_mode == IC_SECONDS) snprintf(what, sizeof what, "%.1f (s of real time)", ic_seconds);
    else snprintf(what, sizeof what, "%s", ic_mode == IC_GAME ? "game (the game's own count: 2700 frames)"
                                                               : "ps2 (real time = the PAL PS2's 108 s)");
    LOG("idle view: 0x207C60 hook, idle_view=%s\n", what);
}

void GUEST(00207c60)(R5900Ctx *c);
HLE(00207c60)                               /* idle_view_due(this = the idle camera 0x6FFF20) */
{
    if (!ic_ready) ic_load();
    uint32_t self = ARG(0), tick = gw(self + 148);
    double now = hle_host_seconds();
    if (tick == 0 || tick < ic_last_tick) ic_acc = 0.0;     /* the count restarted: input, or the player moved */
    else if (tick != ic_last_tick) {
        double dt = now - ic_last_t;
        ic_acc += dt < 0.0 ? 0.0 : dt > IC_DT_MAX ? IC_DT_MAX : dt;
    }
    if (tick != ic_last_tick) ic_last_t = now;
    ic_last_tick = tick;

    GUEST(00207c60)(c);
    int on = (uint32_t)c->r[2].d[0] != 0;
    if (ic_mode != IC_GAME) {
        uint32_t kw = gw(self + 40);
        float kick;
        memcpy(&kick, &kw, 4);
        double need = ic_mode == IC_SECONDS ? ic_seconds : (double)kick / 1000.0 * 30.0 / 25.0;
        on = tick > 0 && ic_acc >= need;
        RET(on);
    }
    if (on != ic_on && ic_logs < 200) {
        ic_logs++;
        uint32_t ltt = gw(IC_PAD0_LTT), ms = gw(IC_TIMER_MS);
        LOG("idle cam: %s at vtime %.1f s, frame %llu: %u frames standing still (%.1f s real), game clock %.1f s "
            "after the last pad input\n", on ? "ON" : "off", (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame,
            tick, ic_acc, (double)(int32_t)(ms - ltt) / 1000.0);
    }
    ic_on = on;
}
