/* hle_bl.h - game time = real time, frame cap.
 *
 * Why: virtual time never runs ahead of the real clock at idle points (hle_plat_pace), but it may fall behind it, and
 * that lag was never forgiven. The boot alone leaves virtual time ~30 s behind the real clock (measured on a PSP:
 * frame 2000 = vtime 40 s at real 69.9 s), so in the main loop the pacing never waited again: every iteration faster than the game's
 * 2-VBlank limit (40 ms PAL) ran unpaced and the game clock ran up to 1.3x faster than real time ("time accelerated").
 *
 * poc6.cfg keys (host: environment HLE_X_<KEY>), all read once at the first main-loop iteration; defaults = as before:
 *   x_pace=0|1|2    0 (default): as before. 1: no debt - from the first main-loop iteration virtual time may lag the
 *                   real clock by at most x_pacelag ms (the rest is forgiven): the game's own 2-VBlank limit becomes a
 *                   real-time cap (25 fps at 50 Hz, 30 fps at x_pal50=0); below the cap the game runs in slow motion.
 *                   2: real time (the PS2's behaviour) - additionally virtual time follows the real clock while the guest
 *                   works (every x_paceper time charges): VBlanks and the EE timer the game's timer reads
 *                   (GetTimerSystemTime) advance with real time, so a slow frame gets a longer timestep exactly as on a
 *                   PS2 that drops frames (the timer update 0x27B5F0 clamps: timestep 0.5..3.0 = 10..60 ms, the
 *                   millisecond clock +300 ms per frame at most). Lag above x_pacemax ms (hitches, sleep) is forgiven.
 *   x_pacelag=34    mode 1: lag kept (ms).
 *   x_pacemax=250   mode 2: largest catch-up (ms); more lag is forgiven (the game pauses instead of jumping).
 *   x_paceper=64    mode 2: real-clock check every N time charges (~50 checks per game frame).
 *   x_fpscap=0      friendly name fps_cap (hle_settings.c). 0 = off (as before). N > 0 = frame cap, quantised DOWN to the
 *                   VBlank grid: 60 / 30 / 20 / 15 fps = 1 / 2 / 3 / 4 VBlanks of 60 Hz per frame, 25 = the PAL timing
 *                   (50 Hz VBlanks, 2 per frame). It sets the VBlank rate at sceGsResetGraph (overrides x_pal50), the
 *                   game's own limit (the render driver's VBlanks per flip, byte 0x66814C, normally 2), x_pace=2 unless
 *                   x_pace is given, and a hard real-time floor of 1000/cap ms per iteration. 60 = experimental.
 *   x_clocklog=0    >0: every N real seconds one 'clock: window' line: real s, virtual s, the game's millisecond clock
 *                   (gp+0x924) and the in-game clock against real time, fps.
 *   host only: HLE_BL_SIM_MS=a[,b,...]
 - a main-loop iteration takes at least a (then b, ...) ms of real time (pacing
 *   simulation, with --paced-main).
 */
#ifndef HLE_BL_H
#define HLE_BL_H
#include <stdint.h>

#include "hle.h"                         /* hle_rtpace_*: the core counts the time charges */
#define hle_bl_mode hle_rtpace_mode     /* x_pace (0 until the first main-loop iteration) */
#define hle_bl_ctr hle_rtpace_ctr       /* mode 2: time-charge counter / period */
#define hle_bl_per hle_rtpace_per

void hle_bl_tick(void);                 /* hle_mainloop_tick: an iteration starts */
void hle_bl_pace(uint64_t vtime);       /* hle_sched.c idle points, before hle_plat_pace(vtime) */
void hle_bl_rt_sync(void);              /* mode 2: virtual time follows the real clock (from hle_time_charge) */
void hle_bl_report(void);               /* end of run */
int hle_bl_pal50(void);                 /* sceGsResetGraph: 1 = 50 Hz VBlanks (x_fpscap 25, or x_pal50 without a cap) */

#endif
