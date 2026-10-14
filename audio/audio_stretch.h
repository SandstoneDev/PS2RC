/* audio_stretch.h - the output stage of a game's audio on the PSP.
 *
 * A pack's IOP sound-driver model + the SPU2-lite engine (spu2_lite.h) run in lockstep with the game's VIRTUAL time on
 * the game thread, exactly as on the host, so every driver reply is the host's. What the engine renders goes into an AsRing
 * (44.1 kHz stereo s16, one producer = the game thread, one consumer = the sceAudio feeder thread). The feeder pulls
 * hops of AS_HOP frames at REAL time. While the game runs slower than real time (virtual time falls behind it: ~0.5x in
 * gameplay on the PSP-3000 was measured), less sound is produced than the DAC plays; AudioStretch keeps it continuous:
 *   AS_STRETCH (default) WSOLA time-stretch: grains of AS_N frames with linear cross-fades at 50 % overlap, each grain
 *                        placed (AMDF search within +-AS_SEEK frames of its nominal position) where it best continues
 *                        the previous one - same pitch, slower tempo (the picture is in slow motion too);
 *   AS_PITCH             varispeed: the input is resampled - pitch and tempo drop together (a slowed tape);
 *   AS_GAP               no rate change: what is produced plays at 1x, the rest is silence (underruns).
 * The stretch factor s (output frames per input frame, Q16) follows the measured production rate p (s = 1/p) with a
 * proportional pull of the fill's LOW-WATERMARK (the fill just before a burst arrives: the sound comes in bursts, one per
 * game frame, so the fill saws) toward the target. With p >= 0.97 and the low-watermark within +-50 % of the
 * target, s = 1 exactly and a hop is the input itself bit for bit (the two cross-fade weights sum to 32768). A
 * low-watermark above 4x the target, or a fill about to overrun the ring, is cut back to the target (a drop); a fill
 * below one grain is an underrun: the last grain fades out, silence follows, and play resumes at the target fill.
 * C99, no libc, no floating point, no OS calls: host-testable (tests/audio_psp/), callable from any PSP thread.
 */
#ifndef AUDIO_STRETCH_H
#define AUDIO_STRETCH_H

#include <stdint.h>

#define AS_RATE  44100u       /* frames per second of the ring and of the output */
#define AS_N     1024u        /* grain length, frames (23.2 ms) */
#define AS_HOP   512u         /* output frames per hop = AS_N / 2 (11.6 ms) */
#define AS_SEEK  256u         /* search radius around the nominal position, frames (5.8 ms) */
#define AS_CORR  384u         /* frames compared per candidate (8.7 ms) */
#define AS_DEC   8u           /* the coarse search works on every AS_DEC-th frame (mono) */
#define AS_ONE   65536u       /* 1.0 in Q16 */

/* the engine's 48 kHz mix -> 44.1 kHz, linear interpolation (step 48000/44100 in Q16 = 71332: 44,099.98 Hz) */
#define AS_RS_STEP 71332u
typedef struct AsResampler {
    uint32_t pos;                       /* next output frame, Q16, relative to prev */
    int32_t prev[2];                    /* the last input frame */
} AsResampler;
/* n input frames -> returns the output frames written to out (at most n * 44100 / 48000 + 1) */
uint32_t as_resample48(AsResampler *rs, const int16_t *in, uint32_t n, int16_t *out);

/* single-producer / single-consumer ring of stereo frames; counters run freely (32-bit wrap is harmless) */
typedef struct AsRing {
    int16_t *buf;                       /* cap frames, L/R interleaved */
    uint32_t cap, mask;                 /* frames; a power of two */
    volatile uint32_t wr;               /* frames written (producer) */
    volatile uint32_t rd;               /* the consumer still reads frames >= rd (consumer) */
    uint32_t lost;                      /* frames the producer could not store (ring full) */
} AsRing;

void as_ring_init(AsRing *r, int16_t *buf, uint32_t cap_frames);           /* cap_frames: a power of two */
uint32_t as_ring_write(AsRing *r, const int16_t *stereo, uint32_t frames);  /* returns frames stored */

enum { AS_STRETCH = 0, AS_GAP = 1, AS_PITCH = 2 };

typedef struct AsStats {
    uint32_t hops, hops_sound, hops_silent;   /* output hops: all / with sound / silence (prebuffering, starved) */
    uint32_t hops_unity;                      /* sounding hops at s = 1 exactly (bit-exact pass-through) */
    uint32_t underruns;                       /* sounding -> starved transitions */
    uint32_t drops;                           /* latency cut back to the target */
    uint64_t frames_dropped;                  /* input frames skipped by drops */
    uint64_t frames_in;                       /* input frames consumed (nominal advance) */
    uint64_t s_sum;                           /* sum of s (Q16) over sounding hops */
    uint32_t s_min, s_max;                    /* Q16, over sounding hops */
    uint64_t fill_sum;                        /* sum of the fill (frames) at sounding hops */
    int32_t peak;                             /* max |sample| written */
} AsStats;

typedef struct AudioStretch {
    AsRing *ring;
    int mode;
    uint32_t target;                    /* fill to keep ahead of the nominal position, frames */
    uint32_t smax, smin;                /* Q16 */
    int playing;
    uint32_t nom, nom_frac;             /* nominal read position: frames + 16-bit fraction */
    uint32_t prev_src;                  /* start of the previous grain (stretch) */
    uint32_t last_wr;
    uint64_t sum_in, sum_out;           /* decaying sums of produced input frames / elapsed output frames */
    uint32_t p;                         /* production rate, input frames per output frame, Q16 */
    uint32_t s;                         /* stretch of the last hop, Q16 */
    uint32_t fill_low;                  /* low-watermark of the fill: its trough before each burst (controlled) */
    int32_t tail[AS_HOP * 2];           /* the previous grain's second half x its fade-out weight (Q15, unnormalised) */
    int16_t dm_c[(2u * AS_SEEK + AS_CORR) / AS_DEC + 1];   /* coarse search: decimated mono of the candidates */
    int16_t dm_t[AS_CORR / AS_DEC];                        /* ... and of the natural continuation */
    AsStats st;
} AudioStretch;

/* target_frames: the fill kept ahead (latency); smax_q16: largest stretch (e.g. 4 x AS_ONE) */
void as_init(AudioStretch *a, AsRing *r, int mode, uint32_t target_frames, uint32_t smax_q16);
/* one hop of AS_HOP stereo frames into out; elapsed = output frames of real time since the previous call (the DAC
   clock; it measures the production rate). Returns 1 when out holds sound (incl. a fade-out), 0 for silence. */
int as_hop(AudioStretch *a, int16_t *out, uint32_t elapsed);
/* frames available ahead of the nominal position */
uint32_t as_fill(const AudioStretch *a);

#endif
