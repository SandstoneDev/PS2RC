/* snd_driver.h - the interface between our code and the game's IOP sound driver (SLES-52541 v2.01), the IOP side of
 * the EE<->IOP audio protocol. The public build translates the driver from the player's own disc with the IOP
 * generator (ps2rc.iop) and runs it on the generic IOP runtime through iop/snd_glue.c; callers only see this header.
 *
 * Protocol facts:
 *   - SIF RPC server 0x65686580;
 *   - fno 0: init, a 16-byte packet (word0: host flag in, 1 = driver started / 0 out);
 *   - fno 1: one 0xAD0-byte state-image packet per EE frame; the reply is the IOP's edit of a copy of it;
 *   - track states (reply word at 0xABC while no track plays): -6 stopped, -1 loading, -2 ready, -3 playing, -4 ended;
 *   - a 33 ms hardware timer (the driver's volume handler), clocked here by the voice engine.
 * Platform callbacks: all disc I/O goes through SnddIo (read bytes of a disc file at an offset, possibly
 * asynchronously), all sound hardware access through SnddSpu (libsd-like register operations; spu2_lite.h implements
 * it). sndd_service runs the IOP threads until they all wait; their only blocking points are semaphores and the
 * platform's data reads. The same code runs over host files (tests, tools, the host boot harness) and over sceIo on
 * the PSP.
 *
 * C99, freestanding apart from memcpy/memset; no OS calls. Little-endian hosts only (x86, Allegrex), like the packet
 * itself.
 */
#ifndef SND_DRIVER_H
#define SND_DRIVER_H


#include <stddef.h>
#include <stdint.h>

#define SNDD_RPC_ID            0x65686580u
#define SNDD_PACKET_SIZE       0xAD0      /* fno 1 send/recv size */
#define SNDD_INIT_PACKET_SIZE  16         /* fno 0 */
#define SNDD_SPU_RAM_SIZE      0x200000u
#define SNDD_TIMER_US          33000u     /* hardware timer period of the volume handler */

/* track states (reply 0xABC while not playing) */
#define SNDD_TRK_STOPPED  (-6)
#define SNDD_TRK_LOADING  (-1)
#define SNDD_TRK_READY    (-2)
#define SNDD_TRK_PLAYING  (-3)
#define SNDD_TRK_ENDED    (-4)

/* ------------------------------------------------------------------ platform interfaces */

enum { SNDD_IO_OK = 0, SNDD_IO_PENDING = 1, SNDD_IO_ERROR = -1 };

/* Data access. `path` is the disc path the IOP itself builds in host0: mode, e.g. "AUDIO\\CONFIG\\BANKSLOT.DAT",
 * "AUDIO\\SFX\\GENRL01.PAK", "AUDIO\\STREAMS\\CUTSCENE.PAK" (upper case, backslashes). */
typedef struct SnddIo {
    void *user;
    /* File size in bytes; synchronous. Returns SNDD_IO_OK or SNDD_IO_ERROR. */
    int (*size)(void *user, const char *path, uint32_t *size_out);
    /* Read `size` bytes at byte `offset` into dst. SNDD_IO_OK when the bytes are in dst, SNDD_IO_ERROR on failure
     * (missing file, short read), SNDD_IO_PENDING when the read is not finished yet: the driver then calls again later
     * with identical arguments and does not touch dst in between (an asynchronous platform keeps a small table of
     * outstanding requests). During sndd_rpc_init (fno 0, blocking on the PS2 too) PENDING is simply retried. */
    int (*read)(void *user, const char *path, uint32_t offset, void *dst, uint32_t size);
} SnddIo;

/* Voice parameter ids (libsd SD_VP_* order) and address ids. */
enum { SNDD_VP_VOLL = 0, SNDD_VP_VOLR = 1, SNDD_VP_PITCH = 2, SNDD_VP_ADSR1 = 3, SNDD_VP_ADSR2 = 4 };
enum { SNDD_VA_SSA = 0, SNDD_VA_LSAX = 1 };

/* Sound hardware, as the IOP sees it through libsd: the core's SPU2 operation table (spu2_lite.h Spu2Ops).
 * Voice v = core v/24, voice v%24. Addresses are SPU RAM byte addresses (0..0x1FFFFF). advance (lockstep mode only):
 * when set, sndd_rpc_frame advances the engine by frame_ms before handling the packet and services SPU2 IRQs as they
 * occur. */
#include "spu2_lite.h"
#include "spu2_split.h"
typedef Spu2Ops SnddSpu;

typedef struct SnddConfig {
    SnddIo io;
    SnddSpu spu;
    void *(*alloc)(void *user, size_t bytes);             /* called only during sndd_rpc_init (config tables) */
    void *alloc_user;
    /* optional: the module's Kprintf messages (printf format). level 0 = errors, 1 = progress, 2 = chatty */
    void (*log)(void *user, int level, const char *fmt, ...);
    void *log_user;
} SnddConfig;

typedef struct SnddStats {
    uint32_t frames, inits;
    uint32_t loads_ok, loads_failed, loads_refused_playing, loads_refused_loading, loads_invalid;
    uint32_t starts, starts_failed, deferred, keyoffs, stops_nulled;
    uint32_t tracks_started, track_fills, track_irqs, track_chains, track_ends;
    uint32_t io_pending;
} SnddStats;

/* a checkout with a local driver implementation brings its own state through snd_driver_local.h; otherwise the
   state the generated driver's glue keeps */
typedef struct SnddSlot SnddSlot;
#if defined(__has_include)
#if __has_include("snd_driver_local.h")
#define SNDD_HAVE_MODEL_STATE 1
#endif
#endif
#ifdef SNDD_HAVE_MODEL_STATE
#include "snd_driver_local.h"
#else
typedef struct SndDrv {
    SnddConfig cfg;
    int initialised;                   /* fno 0 done */
    uint32_t host_flag;                /* fno 0 word0 */
    int32_t state;                     /* track state (SNDD_TRK_*) */
    uint32_t timer_ticks;
    SnddStats st;
} SndDrv;
#endif

/* ------------------------------------------------------------------ API */

/* Set the driver up (no I/O). cfg is copied. Returns 0. */
int sndd_init(SndDrv *a, const SnddConfig *cfg);

/* fno 0: `buf` is the 16-byte RPC buffer (send copied into recv by the caller or the same memory); word0 = host flag
 * in, 1 (driver started) or 0 out. Loads the config tables through SnddIo and programs the SPU defaults. */
int32_t sndd_rpc_init(SndDrv *a, uint8_t *buf);

/* fno 1: reply = the IOP's edit of a copy of send (both SNDD_PACKET_SIZE bytes; they may alias). frame_ms = IOP time
 * since the previous call; it only matters in lockstep mode (SnddSpu.advance set). The background threads (loads,
 * streamed tracks) are run after the reply has been built, as on the IOP. */
void sndd_rpc_frame(SndDrv *a, const uint8_t *send, uint8_t *reply, uint32_t frame_ms);

/* Generic RPC entry: fno 0, 1; anything else replies word0 = 0. Returns 0. */
int sndd_rpc(SndDrv *a, uint32_t fno, const uint8_t *send, uint32_t ssize, uint8_t *recv, uint32_t rsize,
             uint32_t frame_ms);

/* Run the IOP background threads (loads, streamed tracks) until all of them wait; takes a pending SPU2 IRQ
 * first. Called by sndd_rpc_frame; a threaded platform also calls it when the SPU reports an IRQ or a read completes. */
void sndd_service(SndDrv *a);

/* The 33 ms hardware-timer handler: voice volumes for all 48 voices. The voice engine calls it every
 * 1584 samples of its 48 kHz clock (spu2_lite does that when bound with spu2_bind_sndd). */
void sndd_timer_tick(SndDrv *a);

/* Pure helpers (exposed for tests): the IOP's pitch and volume register conversions. */
uint16_t sndd_pitch(uint16_t rate, uint32_t freq);
uint16_t sndd_vol_reg(int16_t v);
uint16_t sndd_sweep_reg(int16_t applied, int16_t target, int16_t *delta_out);

/* Accessors used by tests and tools. */
const SnddSlot *sndd_slot(const SndDrv *a, int s);
const char *sndd_pak_name(const SndDrv *a, int pak);
const char *sndd_strm_name(const SndDrv *a, int pak);

/* Convenience singleton (one sound driver per process). */
int snd_driver_init(const SnddConfig *cfg);
void snd_driver_frame(const uint8_t *send_packet, uint8_t *reply_packet, uint32_t frame_ms);
SndDrv *snd_driver_instance(void);

/* binding to the core's SPU2 engine with this driver's hardware timer (the core's spu2_bind_ops / spu2_rec_bind_ops) */
static inline void sndd_timer_cb_(void *a) { sndd_timer_tick((SndDrv *)a); }
static inline void spu2_bind_sndd(Spu2 *s, SnddSpu *ops, SndDrv *a, int lockstep)
{
    spu2_bind_ops(s, ops, sndd_timer_cb_, a, lockstep);
}
static inline void spu2_rec_bind(Spu2Rec *r, Spu2 *e, SnddSpu *ops, SndDrv *a)
{
    spu2_rec_bind_ops(r, e, ops, sndd_timer_cb_, a);
}

#endif /* SND_DRIVER_H */
