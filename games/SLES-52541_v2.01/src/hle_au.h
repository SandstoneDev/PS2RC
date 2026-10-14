/* hle_au.h - the 30 fps ceiling test's own switches and instrument.
 *
 * x_fxskip=MASK (poc6.cfg; host HLE_X_FXSKIP; default 0 = the guest originals): skip render-only guest effect passes,
 *   switchable at run time (in-run A/B key; re-read when hle_x_gen changes):
 *     1 = shadows: shadows_static 0x117700 + shadows_stored 0x116180 (the blob shadows of peds / cars / objects, the
 *         headlight and street-lamp light pools; their triangles reach the HLE as render buffer types 8 / 9 - the sky
 *         module's sky_tris draws them). Measured on a PSP-3000, 0x116180 alone: 0.88 ms per frame; 0x117700 is not a
 *         zone (unknown). Player loses: every shadow and light pool.
 *     2 = plane trails: plane_trails 0x2E1350 (3 sky contrails, faded by the time of day): 0.44 ms per frame
 *         measured on a PSP-3000. Player loses: the white contrails in the sky (rarely visible).
 *   Guest effect: the skipped passes only fill the game's render buffer (no game state read later; the stored-shadow
 *   list is rebuilt by the game every frame), but the guest does less work, so the timeline differs like every cut.
 * ge_busy=US (poc6.cfg, PSP only; default 0 = off): a sampler thread (priority 0x1F, just above the game thread)
 *   wakes every US microseconds and asks sceGeDrawSync(1) (peek) whether the GE is drawing (2), stalled on the CPU's
 *   list end (3), done / idle (0), queued (1) or something else; GE busy share = drawing / samples. Every state is
 *   counted separately so a firmware that never answers "drawing" shows as such (Unknown on hardware until a run).
 *   Per in-run A/B variant: GE busy ms per iteration = share x the variant's mean iteration time (the A/B runner is a
 *   developer module, not in every checkout).

 */
#ifndef HLE_AU_H
#define HLE_AU_H
#include <stdint.h>

#define AU_GE_STATES 6                 /* 0 done, 1 queued, 2 drawing, 3 stall, 4 other >= 4, 5 negative (error) */
void hle_au_init(void);                /* reads ge_busy, starts the sampler (PSP); called by hle_ab_init */
void hle_au_ge_counts(uint32_t out[AU_GE_STATES]);   /* running sample counts (all zero when off / host) */
int hle_au_ge_on(void);
void hle_au_report(void);              /* end of run (called by hle_ab_report) */

#endif
