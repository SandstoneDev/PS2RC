/* hle_xgame.h - this game's speed switches and their report (hle_xgame.c and developer modules, not in every
 * checkout). Defaults (chosen after a hardware A/B, measured x1.37 game speed): x_pal50, x_dma, x_rbuf = 1 together
 * with im2d_guest=0 (hle_ge.c / hle_render.c) and sky_guest=0 (hle_sky.c); x_vu0 stays 0 (measured 7 % slower on a
 * PSP); x_rs, x_tex, x_grass = 1 too, and postfx_guest / postfx_rad_guest = 0 (hle_postfx.c).
 *   x_sky     render driver fast paths (guest-identical unless stated where they are implemented)
 *   x_rbuf    1: batch_flush 0x3C50F0 empties the game's render buffer without running the batch renderers (VU1
 *                packets the HLE drops) - a timeline change like the guest mesh-pipeline switch of hle_render.c
 *   x_vu0     1: VU0 micro program #0 in float registers (a developer module; bit-identical, guest-identical) */
#ifndef HLE_XGAME_H
#define HLE_XGAME_H
#include "r5900_rt.h"
#include "hle_fast.h"
void hle_farcol_report(void) __attribute__((weak));   /* x_farcol summary (a developer module, not in every checkout) */
void hle_ag_report(void);                      /* x_ag* summary (hle_ag.c) */
void hle_far_report(void);                     /* far-entity policy summary (hle_far.c) */
int x_vu0_callms(R5900Ctx *c, uint32_t addr) __attribute__((weak));   /* a developer module (not in every checkout) */

void hle_x_report(void);                       /* one session.log line with the switches in effect */
#endif
