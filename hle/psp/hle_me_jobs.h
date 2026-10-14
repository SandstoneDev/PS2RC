/* hle_me_jobs.h - the Media Engine JOB interface.
 *
 * The ME runs ONE loop (the pack's ME audio loop): the audio loop (audio_me=1 live / 2
 * shadow) or, when no audio runs there, the same loop in JOBS-ONLY mode. Other work runs on it as JOBS: a job is a
 * poll function the loop calls when it has nothing more urgent to do.
 *
 * Priorities (fixed, per loop iteration): 1. the exit request (stop / park / sleep), 2. the audio output hop (live:
 * the feeder's 40 ms deadline), 3. the audio commands (the game thread's barrier waits for them), 4. ONE unit of ONE
 * job (round-robin over the enabled jobs that have work), 5. idle back-off (~50 us of NOPs). So a job never delays a
 * hop by more than one unit, and the loop's exit / sleep handling stays between units.
 *
 * Job contract (int poll(int sim)):
 *  - called on the ME (sim = 0) or, in the emulator tests, on the simulation thread (sim = 1);
 *  - does at most ONE bounded unit of work (target <= 1 ms on the ME, typical 10-100 us) and returns 1, or returns 0
 *    at once (one uncached read) when it has nothing to do;
 *  - never blocks or spins waiting for the main core; no syscall, no libc, no VFPU, no jump table, no struct copy,
 *    no call outside its own checked code (the ME runs at the kernel-cached alias: the loop calls the job at
 *    0x80000000 | poll, and the job's own JALs stay there); checked like tests/phase6psp/z_me_check.py;
 *  - shares memory with the main core through uncached words (0x40000000 | addr) or its own explicit cache
 *    maintenance (hle_me_dinval / hle_me_dwb / hle_me_dwbinv_all - a writeback-invalidate of the whole D-cache is
 *    allowed: nobody's dirty lines are lost);
 *  - keeps its state in its own memory: between two units the loop may be stopped (exit), parked and cold-restarted
 *    (sleep: ps2rc_exc.prx; the loop writes the ME's D-cache back before it halts, so the job's cached state
 *    survives) or replaced by the other mode's loop (audio attach / hand-back) - a job sees only "no call for a
 *    while". hle_me_resume_epoch() (hle_me.h) moves once per wake-up: main-core waits on a job should restart their
 *    deadlines when it moves (as the audio waits do).
 *  The loop measures every unit (count, ME time, longest, units over the limit) - hle_me_jobs_info().
 *
 * Main-core API (PSP, AUDIO=1 builds; game thread or any user thread):
 */
#ifndef HLE_ME_JOBS_H
#define HLE_ME_JOBS_H

#include <stdint.h>

#define HLE_ME_JOBS 4                 /* job slots */

typedef int (*HleMeJobFn)(int sim);

/* register a job (idempotent per function); returns its slot (>= 0) or -1 (table full / no function). A new job is
   enabled. The loop sees it from its next idle round. */
int hle_me_job_add(HleMeJobFn poll, const char *name);
void hle_me_job_enable(int slot, int on);

/* 1 = the ME loop is running on the ME (or its simulation thread) and polls the jobs now; 0 = nobody
   (audio not attached yet, handed back, parked for a sleep without the PRX hook, exit). Cheap (no wait). */
int hle_me_jobs_polled(void);
/* 1 = the game's audio runs on the ME (audio_me live or shadow attached and not handed back) - hle_audio_psp.c */
int hle_audio_me_live(void);

/* jobs-only mode wanted (on = 1) / not wanted (0). While the audio owns the ME its loop polls the jobs and nothing
   else happens; otherwise (audio_me=0, the emulator's simulation, after an audio hand-back with an acknowledged stop)
   the ME loop is started in jobs-only mode - one ME bring-up, sleep and exit owner for everybody. When the audio
   attaches later it takes the ME over (the jobs continue in its loop). sim = 1: the loop runs on a main-core thread
   (emulator tests). Returns 1 = polled now (by either loop), 0 = not (why says why: emulator, no AUDIO model, ...). */
int hle_me_jobs_run(int on, int sim, char *why, int why_n);

/* the loop's idle back-off between polls in NOPs (0 = the default 8192, ~75 us);
   a longer one = fewer uncached polls on the shared bus, but up to that long before the ME sees new work */
void hle_me_jobs_idle(uint32_t nops);

/* statistics: one line (all slots: units, ME ms, longest unit us, units over the limit, by which loop) */
void hle_me_jobs_info(char *out, int n);
/* the ME's CP0 Count rate in MHz from the latest short calibration window (0 = not known yet). On the PSP-3000 the
   ME's Count runs at ~454 MHz (z4 heartbeat) and wraps every ~9.5 s - never difference it over longer windows. */
double hle_me_count_mhz(void);

#endif
