/* hle_tune.h - less game work per frame through the game's own variables.
 *
 * The tunables are applied by the daycycle_update hook 0x278320 (hooks.def), right after the guest original computed
 * this frame's time-of-day colour set - once per game_process iteration, before the camera update, the car generator
 * and game_loop's far-plane set-up read them. Values the game rewrites (the time-of-day set every frame, the density
 * multipliers by mission scripts) are re-applied on top of the game's own value. Every key defaults to OFF (= the
 * original game; the hook then only calls the original and writes nothing).
 *
 * PSP: ps2rc.cfg keys (the EBOOT's folder); host: environment variables HLE_TUNE_<KEY> (e.g. HLE_TUNE_FAR=50).
 *   tune_far=P      far clip (the current time-of-day set's far clip 0x707490 -> camera far plane, the renderer's
 *                   world scan) and fog start (0x707494) x P % (default 100)
 *   tune_farmax=M   ... and at most M metres (default 0 = no cap); the fog start follows the far clip's ratio
 *   tune_fog=P      fog start x P % on top of tune_far (default 100; lower = the fog starts nearer, hides the far edge)
 *   tune_fogmax=P   the fog start at most P % of the applied far clip (default 0 = off) - with tune_far=60
 *                   tune_fog=167 the fog starts where the game put it and still completes at the nearer far clip
 *                   (a fog start >= the far clip would switch the GE fog off)
 *   tune_drivefar=P far clip (and with it the fog start) x P % while the player drives a vehicle faster than
 *                   tune_drivev m/s (default 0 = off); eased 1 % per frame, run-time switchable (in-run A/B: an A/B
 *                   switch jumps to the new factor)
 *   tune_drivev=V   ... the speed threshold in m/s (default 8)
 *   tune_lod=P      the LOD distance scale 0x666E1C (PS2 1.0 = P 100): every model's draw / LOD-switch distance, the
 *                   vehicle / ped LOD distances x P % (default 100)
 *   tune_peds=P     the ped density multiplier 0x66592C x P % of the game's value (default 100)
 *   tune_cars=P     the car density multiplier 0x6659B4 x P % of the game's value (default 100)
 *   tune_maxpeds=N  the ped limit 0x665934 (game 25) = N, the interior limit 0x665938 (game 40) x N / 25
 *                   (default -1 = off)
 *   tune_maxcars=N  the car limit 0x6659B8 (game 12) = N (default -1 = off)
 * Streaming:
 *   tune_strmem=KB  the streaming memory budget 0x667688 = KB x 1024 (the stream settings file's "memory": 9000 on the
 *                   PSP, 13550 in the PS2's own file); at 9000 KB the working set of the starting area does not fit
 *                   and the game reloads ~1.5 models every frame (default 0 = the file's value)
 *   tune_strfree=KB  with tune_strmem: keep KB of the game heap (heap descriptor 0x66D500) free - the budget shrinks
 *                   to what the heap holds, never below the file's value (default 0 = fixed budget)
 *   tune_vehmodels=N the wanted number of resident vehicle models 0x66768C (settings file "vehicles", 12)
 *                   (default -1 = off)
 *   tune_strlog=N   diagnostic: the N models loaded most often over virtual 50-95 s (model id, loads, info byte)
 *   tune_stats=S    every S seconds of virtual time one log line with the game's own per-frame counters (visible
 *                   entities / LODs from the render-list build, peds, cars, far clip, fog) - read only
 *                   (default 0 = off); the end-of-run report always has the totals over virtual 50-95 s (the
 *                   gameplay span the comparison scripts use) when any key or tune_stats is set
 */
#ifndef HLE_TUNE_H
#define HLE_TUNE_H

void hle_tune_init(void);      /* reads the keys, logs them (hle_boot_init) */
void hle_tune_report(void);    /* end-of-run summary (final_report) */
int hle_tune_set(const char *key, const char *value);   /* run-time far / farmax / fog / fogmax / lod
                                                          (in-run A/B); 1 = applied, 0 = not a run-time key */
void hle_bc_report(void) __attribute__((weak));      /* optional developer module (not in every checkout): drive aid counters (from hle_tune_report) */
void hle_bc_tick(void) __attribute__((weak));        /* the same developer module: position log / test aids, once per game_process */
void hle_dev_tick(void) __attribute__((weak));        /* optional developer module (mission watch / replay aid), once per game_process */
void hle_dev_report(void) __attribute__((weak));      /* its counters (from hle_tune_report) */

#endif
