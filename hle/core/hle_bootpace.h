/* hle_bootpace.h - the boot / loading screens paced to the real clock.
 *
 * Why: the game's boot ends its guest work near virtual 5-10 s and its loading screens (with the loading music, an
 * sound-driver stream) then run until virtual ~40 s. On the PSP the CPU-bound init leaves virtual time 15-25 s BEHIND the
 * real clock, and pacing only ever waits (hle_plat_pace), so the loading screens ran unpaced at 4-6x real time (b52 HW:
 * vtime 10 -> 40 s in 5.2 s real). The IOP audio model renders virtual time, the output stage plays real time and
 * never faster than 1x (audio_stretch_min=100): the excess was cut (60-85 drops per 30 s) = the music jumping
 * forward = "the sound on the loading screen is sped up". the x_pace starts at the first main-loop iteration,
 * so it does not cover this phase.
 *
 * poc6.cfg keys (host: environment HLE_X_<KEY>), read at the first time charge; default = as before:
 *   x_bootpace=0|1|2  0 (default): as before. Only before the first main-loop iteration:
 *                     1 = no debt: at idle points lag above x_bootlag ms is forgiven - the loading screens run at 1x
 *                         real time after the slow init (the CPU-bound init itself is unchanged);
 *                     2 = real time (the x_pace=2 for the boot): additionally virtual time follows the real
 *                         clock while the guest works (every x_paceper time charges, jumps of at most x_pacemax ms),
 *                         so the loading screens' clock and the music advance with real time during the slow init too.
 *   x_bootlag=100     mode 1: lag kept at idle points (ms).
 *   (x_pacemax=250, x_paceper=64: shared with the x_pace=2.)
 * At the first main-loop iteration it logs 'boot pacing: ...' and switches itself off (the x_pace
 * takes over from there). Not meant with pace_boot=0 (the boot is not paced at all).
 * Host only (pacing simulation, with --paced): HLE_BN_SIMK=k - before the first main-loop iteration every time charge
 * of c EE cycles also takes k x c of real time (the PSP's slow init); HLE_BN_SIMIDLE=us - every idle jump before the
 * main loop costs that much real time (the PSP's speed limit on the loading screens); HLE_BN_TIMELINE=file - 'real_s vtime_s' samples
 * every 5 ms of real time (tests/phase6psp/bn_outsim.c replays the PSP output stage on it).
 */
#ifndef HLE_BOOTPACE_H
#define HLE_BOOTPACE_H
#include <stdint.h>

extern int hle_bootpace_on;                   /* hle_time_charge calls hle_bootpace_charge while non-zero (1 until the cfg is read) */
void hle_bootpace_charge(uint64_t cycles);    /* hle_time_charge, after virtual time advanced */
void hle_bootpace_pace(uint64_t vtime);       /* hle_sched.c idle points, before hle_plat_pace(vtime) */

#endif
