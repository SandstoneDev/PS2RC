/* hle_pad.c - the PSP controls as the game's DualShock 2 (port 0), for the libpad HLE in hle/core/hle_sce.c.
 *
 * The PSP has no L2/R2/L3/R3 and no right stick. Built-in layout (ps2rc.cfg can change it: hle_padmap.h):
 *   PSP              PS2                       PSP + SELECT held     PS2
 *   Cross/Circle/    Cross/Circle/              L                    L2
 *   Square/Triangle  Square/Triangle            R                    R2
 *   L / R            L1 / R1                    Triangle             L3
 *   START / SELECT   START / SELECT             Circle               R3
 *   D-pad            D-pad                      D-pad                right stick (full deflection)
 *   analog stick     left stick (dead zone)
 * SELECT still reaches the game as SELECT (select_to_game=always), or only as a short pulse after a lone tap (tap).
 * Returned mask: PS2 button bits, active HIGH (scePadRead inverts): low byte SELECT L3 R3 START UP RIGHT DOWN LEFT,
 * high byte L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE. sticks[] = right X, right Y, left X, left Y (0x80 = centre).
 */
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_padmap.h"              /* the player's mapping (pure C, host-tested) */

_Static_assert(PM_SELECT == PSP_CTRL_SELECT && PM_START == PSP_CTRL_START && PM_UP == PSP_CTRL_UP &&
               PM_RIGHT == PSP_CTRL_RIGHT && PM_DOWN == PSP_CTRL_DOWN && PM_LEFT == PSP_CTRL_LEFT &&
               PM_L == PSP_CTRL_LTRIGGER && PM_R == PSP_CTRL_RTRIGGER && PM_TRIANGLE == PSP_CTRL_TRIANGLE &&
               PM_CIRCLE == PSP_CTRL_CIRCLE && PM_CROSS == PSP_CTRL_CROSS && PM_SQUARE == PSP_CTRL_SQUARE &&
               PM_HOME == PSP_CTRL_HOME, "hle_padmap.h button bits = pspctrl.h");

static int pad_ready;
static uint32_t pad_last;
static unsigned pad_logged;
static PadMap pad_map;                              /* ps2rc.cfg map_* / modifier / select_to_game / select_up */
static PadTap pad_tap;

/* run option pad_script=T0-T1:MASK,... - scripted presses by virtual time (seconds; MASK = PS2 bits as returned),
   ORed into the PSP buttons: the host's --pad on the PSP, for deterministic PPSSPP runs (e.g. into the pause menu) */
#define MAX_PAD_EV 32
static struct { double t0, t1; uint16_t m; int16_t lx, ly; } pad_ev[MAX_PAD_EV];
/* "T0-T1:MASK@XX,YY" also holds the LEFT stick at XX,YY (hex, 80 = centre) for the interval, as the host's --pad -
   scripted gameplay for A/B runs */
static int n_pad_ev;

/* run option poke=T:ADDR=BYTE,... - the host's --poke on the PSP (emulator checks): one guest byte written once when
   the virtual time reaches T */
#define MAX_POKES 32
static struct { double t; uint32_t a; uint8_t v, done; } pokes[MAX_POKES];
static int n_pokes;

static void poke_load(const char *s)               /* the value of ps2rc.cfg poke (hle_cfg.c) */
{
    if (!s) return;
    while (*s && *s != '\n' && *s != '\r' && n_pokes < MAX_POKES) {
        char *e;
        double t = strtod(s, &e);
        if (*e != ':') break;
        unsigned long a = strtoul(e + 1, &e, 16);
        if (*e != '=') break;
        unsigned long v = strtoul(e + 1, &e, 16);
        pokes[n_pokes].t = t;
        pokes[n_pokes].a = (uint32_t)a;
        pokes[n_pokes].v = (uint8_t)v;
        n_pokes++;
        s = e;
        if (*s == ',') s++;
    }
    LOG("pad: %d scripted pokes from ps2rc.cfg poke\n", n_pokes);
}

static void pokes_apply(void)
{
    double t = (double)hle_vtime / EE_HZ;
    for (int i = 0; i < n_pokes; i++)
        if (!pokes[i].done && t >= pokes[i].t) {
            pokes[i].done = 1;
            LOG("poke (script): guest byte %08x = %02x (was %02x) at vtime %.3f s, frame %llu\n", (unsigned)pokes[i].a,
                pokes[i].v, gb(pokes[i].a), t, (unsigned long long)hle_frame);
            sb_(pokes[i].a, pokes[i].v);
        }
}

static void pad_script_load(void)
{
    /* the shared table (hle_cfg.c, whole file; the keys may now be on the first line too) */
    poke_load(hle_cfg_get("poke"));
    padmap_config(&pad_map, hle_cfg_get);
    char *s = (char *)hle_cfg_get("pad_script");  /* a private copy in hle_cfg.c's pool (read only here) */
    if (!s) return;
    while (*s && *s != '\n' && *s != '\r' && n_pad_ev < MAX_PAD_EV) {
        double a = strtod(s, &s);
        if (*s != '-') break;
        double b = strtod(s + 1, &s);
        if (*s != ':') break;
        unsigned long m = strtoul(s + 1, &s, 16);
        pad_ev[n_pad_ev].t0 = a;
        pad_ev[n_pad_ev].t1 = b;
        pad_ev[n_pad_ev].m = (uint16_t)m;
        pad_ev[n_pad_ev].lx = pad_ev[n_pad_ev].ly = -1;
        if (*s == '@') {
            unsigned long x = strtoul(s + 1, &s, 16);
            if (*s != ',') break;
            unsigned long y = strtoul(s + 1, &s, 16);
            pad_ev[n_pad_ev].lx = (int16_t)(x & 0xFF);
            pad_ev[n_pad_ev].ly = (int16_t)(y & 0xFF);
        }
        n_pad_ev++;
        if (*s == ',') s++;
    }
    LOG("pad: %d scripted presses from ps2rc.cfg pad_script\n", n_pad_ev);
}

uint16_t hle_plat_pad_read(uint8_t sticks[4])
{
    if (!pad_ready) {
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        pad_ready = 1;
        pad_script_load();
        /* "controls: ..." = the PSP controls -> DualShock 2 port 0, as in effect */
        LOG("%s (* = built-in, not in the cfg file; ! = the file's value was not used, see the notes)\n", pad_map.line);
        if (pad_map.notes[0]) LOG("controls notes: %s\n", pad_map.notes);
    }
    if (n_pokes) pokes_apply();
    SceCtrlData p;
    if (sceCtrlPeekBufferPositive(&p, 1) <= 0) return 0;
    uint32_t b = p.Buttons;
    uint16_t m = 0;
    if (n_pad_ev) {
        double t = (double)hle_vtime / EE_HZ;
        for (int i = 0; i < n_pad_ev; i++)
            if (t >= pad_ev[i].t0 && t < pad_ev[i].t1) m |= pad_ev[i].m;
    }
    m |= padmap_apply(&pad_map, &pad_tap, b, p.Lx, p.Ly, hle_frame, sticks);
    if (n_pad_ev) {                                  /* scripted left stick */
        double t = (double)hle_vtime / EE_HZ;
        for (int i = 0; i < n_pad_ev; i++)
            if (pad_ev[i].lx >= 0 && t >= pad_ev[i].t0 && t < pad_ev[i].t1) {
                sticks[2] = (uint8_t)pad_ev[i].lx;
                sticks[3] = (uint8_t)pad_ev[i].ly;
            }
    }
    if (m != pad_last && m && pad_logged < 40) {     /* the first presses, for the log */
        pad_logged++;
        LOG("pad: buttons %04x (vtime %.3f s, frame %llu)\n", m, (double)hle_vtime / EE_HZ,
            (unsigned long long)hle_frame);
    }
    pad_last = m;
    return m;
}
