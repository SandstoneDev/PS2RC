/* spu2_lite.h - a small software model of the PS2 SPU2 voice hardware as an IOP sound driver uses it: 48 voices over
 * a 2 MB SPU RAM image, PS-ADPCM blocks with the hardware loop flags (loop end / repeat / loop start), LSAX/NAX/ENVX,
 * 14-bit pitch on a 48 kHz clock (0 = hold, 0x3FFF = 4x), the ADSR envelope and the fixed / linear-sweep volume modes
 * (counter-based rate model of the hardware: psx-spx "SPU Envelope", same model as current PCSX2), the SPU2 IRQ on an
 * address, and a stereo s16 mix at 48 kHz (native) or 44.1 kHz (linear resampling of the mix).
 *
 * Not modelled (unused by the driver it was written for): reverb, noise, pitch modulation, external input,
 * per-core effect sends, the key-on delay, the decoder FIFO run-ahead of NAX. Interpolation is linear (0) or nearest
 * (1); the hardware's 4-tap Gaussian filter is an open item.
 *
 * Header + one C file, C99, no libc calls (the render path is meant to be portable to the PSP Media Engine).
 * Not thread-safe: the platform serialises the IOP model (register writes) and the renderer.
 */
#ifndef SPU2_LITE_H
#define SPU2_LITE_H

#include <stdint.h>

#define SPU2_RAM_SIZE     0x200000u
#define SPU2_VOICES       48
#define SPU2_RATE         48000u
#define SPU2_TIMER_TICKS  1584u        /* 33 ms at 48 kHz: period of the IOP volume timer */
#define SPU2_BLOCK_FRAMES 256u         /* internal 48 kHz block */

enum { SPU2_ENV_ATTACK, SPU2_ENV_DECAY, SPU2_ENV_SUSTAIN, SPU2_ENV_RELEASE, SPU2_ENV_STOPPED };
enum { SPU2_INTERP_LINEAR = 0, SPU2_INTERP_NEAREST = 1 };

typedef struct Spu2Vol {
    uint16_t reg;                      /* last register write */
    int32_t value;                     /* current level, -0x8000..0x7FFF */
    uint32_t counter;
} Spu2Vol;

typedef struct Spu2Voice {
    /* registers */
    uint16_t pitch, adsr1, adsr2;
    uint32_t ssa, lsa;                 /* byte addresses */
    uint8_t lsa_written;               /* LSAX written since key-on: loop-start flags no longer move LSA */
    Spu2Vol vl, vr;
    /* envelope */
    uint8_t phase;
    uint32_t env_counter;
    int32_t env;                       /* ENVX, 0..0x7FFF */
    /* sample generation */
    uint32_t blk;                      /* byte address of the current 16-byte block */
    uint8_t flags;                     /* loop flags of the current block */
    uint8_t idx;                       /* next sample of the block to consume (0..27) */
    uint8_t decoded;                   /* dec[] holds the current block */
    uint32_t counter;                  /* pitch counter, 12-bit fraction */
    int32_t s_prev, s_cur;             /* the two most recent samples */
    int32_t h1, h2;                    /* ADPCM predictor history */
    int16_t dec[28];
} Spu2Voice;

typedef struct Spu2 {
    uint8_t *ram;                      /* SPU2_RAM_SIZE bytes, owned by the platform */
    Spu2Voice v[SPU2_VOICES];
    uint64_t endx;
    uint32_t irqa;
    uint8_t irq_enable, irq_latched;
    int interp;
    int32_t mvol;                      /* master volume level (both cores: MVOL 0x3FFF) */
    /* IOP hardware timer emulation: callback every timer_period ticks of the 48 kHz clock */
    void (*timer_cb)(void *user);
    void *timer_user;
    uint32_t timer_period, timer_count;
    /* lockstep sink: receives every 48 kHz block rendered by spu2_advance */
    void (*sink)(void *user, const int16_t *stereo48, uint32_t frames);
    void *sink_user;
    int16_t sink48[SPU2_BLOCK_FRAMES * 2];
    /* 48 kHz -> output rate resampler */
    int16_t blk48[SPU2_BLOCK_FRAMES * 2];
    uint32_t blk48_len, blk48_pos;
    uint32_t rs_frac;                  /* 16.16 position between rs_prev and rs_cur */
    int32_t rs_prev[2], rs_cur[2];
    uint64_t clock;                    /* 48 kHz ticks rendered */
    uint32_t peak_voices;              /* max simultaneously sounding voices seen */
    /* test tap: every decoded sample a sounding voice consumes, in order (0 = off) */
    void (*tap)(void *user, unsigned voice, int16_t sample);
    void *tap_user;
    /* PSP: 0 = mix every sounding voice (the default, the host); N = mix only the N loudest of each block -
       the others keep their exact IOP-visible state (ENVX, NAX, flags, IRQ, ADPCM history) but add nothing to the output */
    uint32_t mix_limit;
    uint64_t ticks_mixed, ticks_quiet;   /* sounding-voice ticks mixed / advanced without mixing */
    /* 1 = the reference per-tick loops (spu2_vol_tick / spu2_env_tick per tick) instead
       of the bit-identical locals versions (x_slow_span, quiet_advance's tick loop) - A/B and tests only */
    int x_ref;
    /* 1 = state only - every sounding voice advances as a quiet voice (quiet_advance) and no block is decoded:
       what the IOP model observes (ENVX, NAX, ENDX, LSA, IRQ) without the samples (the main-core half of a Media Engine
       split; tests/phase6psp/x_audio --state-only measures it). The mix it produces is silence. */
    int x_state_only;
} Spu2;

void spu2_init(Spu2 *s, uint8_t *ram);          /* zeroes nothing in ram except the 0x5000 park block */

/* register interface (libsd semantics; also as the operation table Spu2Ops below) */
void spu2_set_param(Spu2 *s, unsigned voice, unsigned param, uint16_t value);   /* 0 VOLL 1 VOLR 2 PITCH 3 ADSR1 4 ADSR2 */
void spu2_set_addr(Spu2 *s, unsigned voice, unsigned which, uint32_t byte_addr); /* 0 SSA 1 LSAX */
uint16_t spu2_get_envx(const Spu2 *s, unsigned voice);
uint32_t spu2_get_nax(const Spu2 *s, unsigned voice);
void spu2_key_on(Spu2 *s, unsigned core, uint32_t mask24);
void spu2_key_off(Spu2 *s, unsigned core, uint32_t mask24);
void spu2_set_irq(Spu2 *s, uint32_t byte_addr, int enable);
int spu2_irq_take(Spu2 *s);

/* rendering */
void spu2_render48(Spu2 *s, int16_t *out, uint32_t frames);   /* out may be 0 (advance only) */
void spu2_advance(Spu2 *s, uint32_t frames48);                /* render48 into the sink (lockstep mode) */
void spu2_render(Spu2 *s, int16_t *out, uint32_t frames, uint32_t out_rate); /* 48000 or e.g. 44100 */

/* tests / state import: put voice vi at byte address nax (block = nax & ~15, next sample from the word at nax) */
void spu2_set_position(Spu2 *s, unsigned vi, uint32_t nax);

/* pure helpers, exposed for tests */
void spu2_adpcm_block(const uint8_t *blk16, int16_t *out28, int32_t *h1, int32_t *h2);
int32_t spu2_vol_fixed(uint16_t reg);                          /* fixed-mode register -> level */
void spu2_env_tick(Spu2Voice *v);                              /* one 48 kHz envelope step */
void spu2_vol_tick(Spu2Vol *vol);                              /* one 48 kHz sweep step */
uint32_t spu2_sounding(const Spu2 *s);                         /* voices with ENVX > 0 */

/* The sound hardware as an IOP sound-driver model sees it through libsd: a table of operations. Voice v = core v/24,
 * voice v%24; addresses are SPU RAM byte addresses (0..SPU2_RAM_SIZE-1). */
typedef struct Spu2Ops {
    void *user;
    uint8_t *ram;                                         /* SPU RAM image, SPU2_RAM_SIZE bytes */
    void (*set_param)(void *user, unsigned voice, unsigned param, uint16_t value);
    void (*set_addr)(void *user, unsigned voice, unsigned which, uint32_t byte_addr);
    uint16_t (*get_envx)(void *user, unsigned voice);
    uint32_t (*get_nax)(void *user, unsigned voice);      /* byte address */
    void (*key_on)(void *user, unsigned core, uint32_t mask24);
    void (*key_off)(void *user, unsigned core, uint32_t mask24);
    void (*set_irq)(void *user, uint32_t byte_addr, int enable); /* IRQA (both cores) + core-0 IRQ enable */
    int  (*irq_take)(void *user);                         /* 1 if the IRQ fired since the last call (then cleared) */
    /* optional: SPU RAM [addr, addr+size) was written by a "DMA" (cache maintenance for a Media Engine mixer) */
    void (*dma_done)(void *user, uint32_t addr, uint32_t size);
    /* optional, lockstep mode only: let the voice engine run `samples` ticks of its 48 kHz clock */
    void (*advance)(void *user, uint32_t samples48k);
    /* optional (spu2_split.h): SPU RAM [addr, addr+size) is ABOUT to be written by a "DMA" - a second engine that
     * reads the image on another core must first consume everything rendered before this point */
    void (*dma_begin)(void *user, uint32_t addr, uint32_t size);
} Spu2Ops;

/* bind as a sound-driver model's hardware: fills `ops` with this engine's operations (advance only in lockstep mode);
 * timer_cb(timer_user) is the driver's hardware timer, called every SPU2_TIMER_TICKS of the render clock (none when
 * timer_user is 0). */
void spu2_bind_ops(Spu2 *s, Spu2Ops *ops, void (*timer_cb)(void *), void *timer_user, int lockstep);

#endif /* SPU2_LITE_H */
