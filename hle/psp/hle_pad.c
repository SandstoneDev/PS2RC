/* hle_pad.c - the PSP controls as the game's DualShock 2 (port 0), for the libpad HLE in hle/core/hle_sce.c.
 *
 * The PSP has no L2/R2/L3/R3 and no right stick, so SELECT is a modifier (it still reaches the game as SELECT):
 *   PSP              PS2                       PSP + SELECT held     PS2
 *   Cross/Circle/    Cross/Circle/              L                    L2
 *   Square/Triangle  Square/Triangle            R                    R2
 *   L / R            L1 / R1                    Triangle             L3
 *   START / SELECT   START / SELECT             Circle               R3
 *   D-pad            D-pad                      D-pad                right stick (full deflection)
 *   analog stick     left stick (dead zone)
 * Returned mask: PS2 button bits, active HIGH (scePadRead inverts): low byte SELECT L3 R3 START UP RIGHT DOWN LEFT,
 * high byte L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE. sticks[] = right X, right Y, left X, left Y (0x80 = centre).
 */
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared poc6.cfg table */

#define PS2_SELECT   0x0001u
#define PS2_L3       0x0002u
#define PS2_R3       0x0004u
#define PS2_START    0x0008u
#define PS2_UP       0x0010u
#define PS2_RIGHT    0x0020u
#define PS2_DOWN     0x0040u
#define PS2_LEFT     0x0080u
#define PS2_L2       0x0100u
#define PS2_R2       0x0200u
#define PS2_L1       0x0400u
#define PS2_R1       0x0800u
#define PS2_TRIANGLE 0x1000u
#define PS2_CIRCLE   0x2000u
#define PS2_CROSS    0x4000u
#define PS2_SQUARE   0x8000u

#define DEAD 40                                      /* PSP sticks rest anywhere in ~128 +- 30 */

static int pad_ready;
static uint32_t pad_last;
static unsigned pad_logged;
/* SELECT + UP: right stick up (default), or a PS2 stick click with the run option pad_sel_up=r3|l3 (SELECT + Circle
   is always R3) */
static uint16_t sel_up_bit = 0;

/* run option pad_script=T0-T1:MASK,... - scripted presses by virtual time (seconds; MASK = the PS2 bits below),
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

static void poke_load(const char *s)               /* the value of poc6.cfg poke (hle_cfg.c) */
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
    LOG("pad: %d scripted pokes from poc6.cfg poke\n", n_pokes);
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
    const char *su = hle_cfg_get("pad_sel_up");
    if (su) sel_up_bit = !strncmp(su, "l3", 2) ? PS2_L3 : !strncmp(su, "stick", 5) ? 0 : PS2_R3;
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
    LOG("pad: %d scripted presses from poc6.cfg pad_script\n", n_pad_ev);
}

static uint8_t stick(unsigned v)
{
    int d = (int)v - 128;
    if (d > -DEAD && d < DEAD) return 0x80;
    int s = d > 0 ? (d - DEAD) * 127 / (127 - DEAD) : (d + DEAD) * 128 / (128 - DEAD);   /* rescale past the dead zone */
    s += 128;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

uint16_t hle_plat_pad_read(uint8_t sticks[4])
{
    if (!pad_ready) {
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        pad_ready = 1;
        pad_script_load();
        LOG("pad: PSP controls -> DualShock 2 port 0 (SELECT = modifier: +L L2, +R R2, +Triangle L3, +Circle R3, "
            "+Up %s, +D-pad right stick)\n", sel_up_bit == PS2_R3 ? "R3 (sub-missions)" : sel_up_bit == PS2_L3 ? "L3"
                                                                                        : "right stick up");
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
    int mod = (b & PSP_CTRL_SELECT) != 0;
    if (b & PSP_CTRL_SELECT) m |= PS2_SELECT;
    if (b & PSP_CTRL_START) m |= PS2_START;
    if (b & PSP_CTRL_CROSS) m |= PS2_CROSS;
    if (b & PSP_CTRL_SQUARE) m |= PS2_SQUARE;
    if (mod) {
        if (b & PSP_CTRL_TRIANGLE) m |= PS2_L3;
        if (b & PSP_CTRL_CIRCLE) m |= PS2_R3;
        if (b & PSP_CTRL_LTRIGGER) m |= PS2_L2;
        if (b & PSP_CTRL_RTRIGGER) m |= PS2_R2;
    } else {
        if (b & PSP_CTRL_TRIANGLE) m |= PS2_TRIANGLE;
        if (b & PSP_CTRL_CIRCLE) m |= PS2_CIRCLE;
        if (b & PSP_CTRL_LTRIGGER) m |= PS2_L1;
        if (b & PSP_CTRL_RTRIGGER) m |= PS2_R1;
    }
    uint8_t rx = 0x80, ry = 0x80;
    if (mod) {                                       /* D-pad = right stick (camera) */
        if (b & PSP_CTRL_LEFT) rx = 0x00;
        if (b & PSP_CTRL_RIGHT) rx = 0xFF;
        if (b & PSP_CTRL_UP) {
            if (sel_up_bit) m |= sel_up_bit;         /* build 33: SELECT + UP = R3 (sub-missions) by default */
            else ry = 0x00;
        }
        if (b & PSP_CTRL_DOWN) ry = 0xFF;
    } else {
        if (b & PSP_CTRL_UP) m |= PS2_UP;
        if (b & PSP_CTRL_DOWN) m |= PS2_DOWN;
        if (b & PSP_CTRL_LEFT) m |= PS2_LEFT;
        if (b & PSP_CTRL_RIGHT) m |= PS2_RIGHT;
    }
    sticks[0] = rx;
    sticks[1] = ry;
    sticks[2] = stick(p.Lx);
    sticks[3] = stick(p.Ly);
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
