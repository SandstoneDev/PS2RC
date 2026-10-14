/* hle_me_audio.h - the audio worker on the Media Engine.
 *
 * Main core (game thread): IOP sound driver + engine A in state-only mode + the command recorder (spu2_split.h)
 * -> command ring (uncached). Media Engine: engine B replays the commands; in LIVE mode (1) every rendered block goes
 * 48 -> 44.1 kHz into the output ring and the ME also runs the WSOLA stretcher: the feeder thread only asks for a hop
 * and hands the ME's buffer to sceAudio. In SHADOW mode (2) the main core keeps rendering and playing as before and
 * the ME only hashes B's blocks: the main core compares (self-qualification on the hardware).
 *
 * Memory: ZmaCtl (uncached), the command ring (uncached), the ME-private region ZmaRegion (cached by the ME only
 * while it runs; every member on its own 64-byte lines) + the output ring's sample buffer + the ME stack.
 *
 * The same loop runs other subsystems' JOBS (hle_me_jobs.h - the public interface, priorities and contract) in its
 * idle branch, and a JOBS-ONLY mode (ZMA_JOBS: no audio, ctl + a small stack region of its own) serves them when no
 * audio runs on the ME.
 */
#ifndef HLE_ME_AUDIO_H
#define HLE_ME_AUDIO_H

#include <stdint.h>
#include "hle_me.h"
#include "spu2_lite.h"
#include "audio_stretch.h"
#include "hle_me_jobs.h"

#define ZMA_CMDS   8192u               /* command ring entries (2 words), a power of two */
#define ZMA_HOPS   4u                  /* output buffers the ME fills (the feeder's OUT_BUFS) */
#define ZMA_STACK  16384u              /* ME stack, at the top of the region */
#define ZMA_SHADOW_HIST 4096u          /* main's per-render hash history (shadow compare window) */

enum { ZMA_LIVE = 1, ZMA_SHADOW = 2, ZMA_JOBS = 3 };   /* ZMA_JOBS: no audio, only the job table */

typedef struct ZmaCtl {
    HleMeHdr h;                        /* 64 B: magic, heartbeat, state, exit_req, count, boots, fault */
    /* written by the main core */
    volatile uint32_t mode;            /* ZMA_LIVE / ZMA_SHADOW */
    volatile uint32_t region;          /* ZmaRegion * (cached address) */
    volatile uint32_t cmds;            /* command ring (uncached address) */
    volatile uint32_t cmd_wr;          /* commands written */
    volatile uint32_t hop_req;         /* feeder: +1 per hop request */
    volatile uint32_t hop_elapsed;     /* output frames of real time since the previous hop */
    volatile uint32_t hop_buf;         /* which ZmaRegion.hop[] */
    volatile uint32_t sim_hang;        /* simulation only (zma_work_sim): 1 = behave like a dead ME */
    volatile uint32_t jobs;            /* the job table (ZmaJobs, uncached address), 0 = no jobs */
    volatile uint32_t pad0[7];
    /* written by the ME */
    volatile uint32_t cmd_rd;          /* commands consumed (applied) */
    volatile uint32_t hop_done;        /* = hop_req when that hop is in hop[hop_buf] (written back) */
    volatile uint32_t hop_snd;         /* as_hop's result: 1 sound, 0 silence */
    volatile uint32_t sh_h, sh_n;      /* shadow: hash after render sh_n (h first, then n) */
    volatile uint32_t renders, frames48, produced44, ramw, ramw_all, ring_lost;
    volatile uint32_t busy_render, busy_hop, busy_cmd;   /* CP0 Count units */
    volatile uint32_t hops, bad_cmds;
    volatile uint32_t st_words[32];    /* the stretcher's AsStats after the last hop (word copy) */
} ZmaCtl;

typedef struct ZmaRegion {
    Spu2 b __attribute__((aligned(64)));                   /* engine B */
    AudioStretch st __attribute__((aligned(64)));          /* live: the ME's; otherwise the main core's */
    AsRing ring __attribute__((aligned(64)));              /* live: the ME's copy of the output ring */
    AsResampler rs __attribute__((aligned(64)));           /* live: 48 -> 44.1 kHz */
    int16_t out48[2 * SPU2_BLOCK_FRAMES] __attribute__((aligned(64)));
    int16_t tmp44[2 * 64] __attribute__((aligned(64)));
    int16_t hop[ZMA_HOPS][AS_HOP * 2] __attribute__((aligned(64)));
} ZmaRegion;

/* the ME-private memory block: ZmaRegion, then the output ring's samples (ring_frames x 4 B), then the ME stack */
#define ZMA_ALIGN64(n)          (((n) + 63u) & ~63u)
#define ZMA_RING_OFFSET         ZMA_ALIGN64((uint32_t)sizeof(ZmaRegion))
#define ZMA_REGION_BYTES(ring_frames) (ZMA_RING_OFFSET + (uint32_t)(ring_frames) * 4u + ZMA_STACK)
static inline int16_t *zma_ring_buf(ZmaRegion *r) { return (int16_t *)(void *)((uint8_t *)r + ZMA_RING_OFFSET); }

/* word-wise FNV-1a 32 of stereo frames (shadow compare: the same function on both cores) */
static inline uint32_t zma_hash(uint32_t h, const int16_t *s, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t w = (uint32_t)(uint16_t)s[2 * i] | (uint32_t)(uint16_t)s[2 * i + 1] << 16;
        h = (h ^ w) * 16777619u;
    }
    return h;
}
#define ZMA_HASH0 2166136261u

/* the job table (hle_me_jobs.h): one uncached 64-byte aligned block shared by every loop. Main writes fn / on
   / n / over_counts; the ME writes rr and the per-slot statistics (busy: 64-bit CP0 Count sum, lo then hi). */
typedef struct ZmaJobSlot {
    volatile uint32_t fn;              /* main: the poll function's plain address (the ME calls 0x80000000 | fn) */
    volatile uint32_t on;              /* main: 1 = enabled */
    volatile uint32_t units;           /* ME: units done (calls that returned 1) */
    volatile uint32_t busy_lo, busy_hi;   /* ME: their Count units (64 bit) */
    volatile uint32_t max;             /* ME: the longest unit (Count units) */
    volatile uint32_t over;            /* ME: units longer than ZmaJobs.over_counts (1 ms) */
    volatile uint32_t pad;
} ZmaJobSlot;
typedef struct ZmaJobs {
    volatile uint32_t n;               /* main: slots in use */
    volatile uint32_t rr;              /* ME: the next slot to try (round-robin) */
    volatile uint32_t over_counts;     /* main: 1 ms in ME Count units (0 = not calibrated yet: not counted) */
    volatile uint32_t idle_nops;       /* main (hle_me_jobs_idle): NOPs of the idle back-off, 0 = 8192 */
    volatile uint32_t pad[12];
    ZmaJobSlot s[HLE_ME_JOBS];
} ZmaJobs;
_Static_assert(sizeof(ZmaJobSlot) == 32u && sizeof(ZmaJobs) == 64u + 32u * HLE_ME_JOBS, "ZmaJobs layout");

/* the ME's work function (runs on the ME only; started by hle_me_start) */
void zma_work(volatile HleMeHdr *hdr);
/* the same loop as a main-core thread body (emulator integration test, audio_me_sim=1): no cache ops, no CP0, yields
   while idle; returns after exit_req */
void zma_work_sim(volatile HleMeHdr *hdr);

/* main side: the waits return 0 when the ME made no progress for `stall_ms` (or its heartbeat stopped) */
typedef struct ZmaHost {
    volatile ZmaCtl *c;                /* uncached */
    volatile uint32_t *cmds;           /* uncached */
    ZmaRegion *r;                      /* cached address (pass to sceAudio; never dereference while the ME runs) */
    uint32_t wr;                       /* main's copy of cmd_wr */
    int stall_ms;
    volatile uint32_t *epoch;          /* hle_me_resume_epoch() - a sleep in a wait is not a stall */
    /* statistics */
    uint64_t waits_us, wait_max_us, barrier_us, full_us;
    uint32_t barriers, fulls;
} ZmaHost;

void zma_host_init(ZmaHost *z, volatile ZmaCtl *c_uncached, volatile uint32_t *cmds_uncached, ZmaRegion *r, int mode);
int zma_push(ZmaHost *z, uint32_t w0, uint32_t w1);        /* 0 = the ME stalled (ring full, no progress) */
int zma_barrier(ZmaHost *z);                               /* every command consumed; 0 = stalled */
/* feeder: one hop; returns 1 sound / 0 silence, -1 = the ME did not answer within timeout_us */
int zma_hop(ZmaHost *z, uint32_t elapsed, uint32_t buf, uint32_t timeout_us, uint32_t *waited_us);
void zma_stats(const ZmaHost *z, AsStats *out);            /* the ME's stretcher statistics (uncached copy) */

/* the loop owner's side of the job interface (hle_me_jobs.h is the users' side):
   zma_jobs_yield   - before another loop of ours takes the ME: stops the jobs-only loop (handshake) if it runs;
   zma_jobs_bind    - after zma_host_init, before the start: this control block polls the job table;
   zma_jobs_unbind  - that loop stopped / was parked; restart_ok = 1: start the jobs-only loop if jobs want one
                      (an audio hand-back), 0: not (exit, sleep);
   zma_jobs_tick    - call regularly (>= every few s): the ME Count rate calibration (short windows: it wraps). */
void zma_jobs_yield(void);
void zma_jobs_bind(volatile ZmaCtl *c, int sim);
void zma_jobs_unbind(volatile ZmaCtl *c, int restart_ok);
void zma_jobs_tick(void);
int zma_jobs_count(void);                                  /* slots in use */
void zma_jobs_sleep_allowed(int on);                       /* the game's audio_me_sleep for the jobs-only loop too */

#endif
