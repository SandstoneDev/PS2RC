/* hle_audio_psp.c - the game's audio on the PSP: see hle_audio_psp.h.
 *
 *  game thread (0x20)                                           feeder thread "hle_audio_out" (0x12)
 *  sound RPC (hle_iop_servers.c) -> sndd_rpc(..., frame_ms)       as_hop(): 512 frames from the ring at real time,
 *    = the virtual time since the last fno 1 (audio_host_rpc's    WSOLA stretch / varispeed / gaps (audio_stretch.c)
 *      arithmetic): the engine renders it in 64-sample chunks  -> sceAudioOutputPannedBlocking (one channel, 44.1 kHz,
 *      (servicing the IOP threads between chunks), reads from      4 rotating buffers)
 *      the disc layer synchronously (as the host: same replies)
 *    -> sink: 48 kHz -> 44.1 kHz (linear) -> AsRing (0.74 s)  --->
 * Nothing in the feeder touches the driver, the disc layer or the log: the two threads share only the ring counters and
 * a few statistics words. The statistics line is written by the game thread (inside the RPC) every audio_stats s.
 */
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared poc6.cfg table */
#include "hle_audio_psp.h"
#include "spu2_lite.h"
#include "audio_stretch.h"
#include "spu2_split.h"
#include "hle_me.h"
#include "hle_me_audio.h"
#include "hle_mem_psp.h"                        /* mem_volatile=1 -> the block in the hole */

int hle_psp_is_emulator(void);                   /* psp/hle_prof_psp.c */

/* poc6.cfg audio_me=0|1|2 - 0 = everything on the main core;
 * 1 (default; measured on a PSP: 0 mismatches in shadow mode, -5.8 ms per frame) = LIVE: once the game's main loop
 *     runs (the intro movies' sceMpeg also uses the ME), the Media Engine renders the
 *     samples (engine B replays the commands of the main core's state-only engine A, spu2_split.h), resamples
 *     them into the output ring and runs the stretcher; the feeder thread only asks it for hops; any stall, a sleep
 *     (the ME is parked in the power callback) or a refused start -> back to the main core (logged; after a sleep the
 *     ME is started again);
 * 2 = SHADOW: the main core renders and plays as before AND the ME renders the same blocks; their hashes are compared
 *     (hardware self-check; costs main-core time: the recorder).
 * The replies are the state-only engine's in every mode = the host's. */
enum { ZME_OFF = 0, ZME_ON = 1, ZME_FAILED = -1, ZME_SLEEP = -2, ZME_STOPPED = -3 };

#define RING_FRAMES  65536u                      /* 1.49 s at 44.1 kHz (256 KB): loading screens deliver ~0.5 s at once */
#define OUT_BUFS     4
#define ARENA_BYTES  (280u * 1024u)              /* the IOP's config tables: 270,924 B for the v2.01 files */
#define CBLOCK       16384u                      /* stream read cache block (one card request) */
#define MAX_FILES    48
#define DUMP_BUF     65536u

/* ------------------------------------------------------------------ options (<EBOOT dir>/poc6.cfg) */
static struct {
    int on, vol, sync, latency_ms, smax, interp_nearest, out, cache_kb, prio, stats_s, dump_s, voices, smin_pct, xref;
    int me, me_stall_ms, me_hop_ms, me_regs;         /* audio_me, audio_me_stall_ms, audio_me_hop_ms, _regs */
    int me_sim, me_sim_hang;                         /* emulator tests: audio_me_sim, audio_me_sim_hang */
    int me_sleep;                                    /* audio_me_sleep (1 = sleep through the PRX) */
} opt = {1, 100, AS_STRETCH, 120, 4, 0, 1, 128, 0x12, 60, 0, 16, 100, 0, 1, 500, 40, 0, 0, 0, 1};   /* audio_me=1,
                                                                                    audio_me_sleep=1 */
/* xref: 1 = the engine's reference per-tick loops (Spu2.x_ref) - hardware A/B only;
   0 = the bit-identical faster loops (same replies, same mix) */
/* smin_pct: the smallest stretch in percent. Draining a high fill by playing up to 4/3 faster (smin 75 %) made the
   music audibly speed up on loading screens, where the sound arrives in bursts of up to seconds of virtual time.
   100 = never faster than 1x: the excess is cut instead (a drop at 4 x the target latency). */

static void read_cfg(void)
{
    for (int ci = 0; ci < hle_cfg_count(); ci++) {    /* the shared table (hle_cfg.c), whole file */
        {
            const char *k = hle_cfg_key(ci), *v = hle_cfg_val(ci);
            int x = (int)strtol(v, NULL, 0);
            if (!strcmp(k, "audio")) opt.on = x;
            else if (!strcmp(k, "audio_vol")) opt.vol = x < 0 ? 0 : x > 100 ? 100 : x;
            else if (!strcmp(k, "audio_sync"))
                opt.sync = !strcmp(v, "gap") ? AS_GAP : !strcmp(v, "pitch") ? AS_PITCH : AS_STRETCH;
            else if (!strcmp(k, "audio_latency_ms")) opt.latency_ms = x < 40 ? 40 : x > 180 ? 180 : x;
            else if (!strcmp(k, "audio_stretch_max")) opt.smax = x < 1 ? 1 : x > 8 ? 8 : x;
            else if (!strcmp(k, "audio_stretch_min")) opt.smin_pct = x < 75 ? 75 : x > 100 ? 100 : x;
            else if (!strcmp(k, "audio_interp")) opt.interp_nearest = !strcmp(v, "nearest");
            else if (!strcmp(k, "audio_out")) opt.out = x;
            else if (!strcmp(k, "audio_cache_kb")) opt.cache_kb = x < 0 ? 0 : x > 512 ? 512 : x;
            else if (!strcmp(k, "audio_prio")) opt.prio = x < 0x08 ? 0x08 : x > 0x7F ? 0x7F : x;
            else if (!strcmp(k, "audio_stats")) opt.stats_s = x < 0 ? 0 : x;
            else if (!strcmp(k, "audio_dump")) opt.dump_s = x < 0 ? 0 : x > 600 ? 600 : x;
            else if (!strcmp(k, "audio_voices")) opt.voices = x < 0 ? 0 : x > 48 ? 48 : x;
            else if (!strcmp(k, "audio_log")) { extern int hle_audio_log_level; hle_audio_log_level = x; }   /* driver progress lines */
            else if (!strcmp(k, "audio_xref")) opt.xref = x != 0;
            else if (!strcmp(k, "audio_me")) opt.me = x < 0 ? 0 : x > 2 ? 2 : x;
            else if (!strcmp(k, "audio_me_stall_ms")) opt.me_stall_ms = x < 50 ? 50 : x > 5000 ? 5000 : x;
            else if (!strcmp(k, "audio_me_hop_ms")) opt.me_hop_ms = x < 10 ? 10 : x > 500 ? 500 : x;
            else if (!strcmp(k, "audio_me_regs")) opt.me_regs = x != 0;   /* 1 = OR the reset stub's register writes */
            else if (!strcmp(k, "audio_me_sim")) opt.me_sim = x != 0;      /* emulator test: a thread plays the ME */
            else if (!strcmp(k, "audio_me_sim_hang")) opt.me_sim_hang = x < 0 ? 0 : x;   /* ... dead after N s */
            else if (!strcmp(k, "audio_me_sleep")) opt.me_sleep = x != 0;  /* sleep via ps2rc_exc.prx */
        }
    }
}

/* ------------------------------------------------------------------ state */
typedef struct { char path[48]; int idx; uint32_t size; } AFile;
typedef struct { int file; uint32_t blk, len, used; uint8_t *data; } CBlk;

static struct {
    void (*log)(void *, int, const char *, ...);
    SceUID mem_id;
    uint8_t *mem;
    uint32_t mem_bytes;
    uint32_t carved;                             /* bytes of the block handed out */
    uint8_t *arena;
    SndDrv *a;
    Spu2 *e;
    AsRing ring;
    AudioStretch *st;
    int16_t *out;                                /* OUT_BUFS x AS_HOP stereo frames */
    CBlk *cache;
    int ncache;
    uint32_t cache_tick;
    AFile files[MAX_FILES];
    int nfiles;
    /* clock (audio_host_rpc's arithmetic) */
    uint64_t last_us;
    int started;
    AsResampler rs;                              /* 48 -> 44.1 kHz */
    /* feeder */
    SceUID thid;
    int ch, vol;
    volatile int quit, running;
    volatile uint32_t feed_busy_us, feed_blocks, feed_wait_us;
    /* statistics (game thread) */
    uint64_t rpc_us, io_us, produced, rpcs;
    uint32_t io_reads, io_hits, io_misses, io_errors, io_missing;
    uint64_t io_bytes;
    /* the statistics window */
    uint64_t w_t0_us, w_vt0_us, w_rpc_us, w_io_us, w_produced;
    uint32_t w_busy_us, w_underruns, w_drops, w_hops_sound, w_unity, w_frames, w_maxms;
    uint64_t w_ssum, w_fill, w_tmix, w_tquiet;
    /* PPSSPP check: the 48 kHz lockstep mix as a WAV */
    SceUID dump_fd;
    uint8_t *dump_buf;
    uint32_t dump_len, dump_frames, dump_max;
    /* the Media Engine split */
    uint8_t *spu_ram;
    Spu2Rec rec;                                 /* the SnddSpu ops go through it when audio_me != 0 */
    ZmaHost z;
    volatile ZmaCtl *mctl;                       /* uncached */
    volatile uint32_t *mcmds;                    /* uncached */
    ZmaRegion *reg;                              /* the ME's region (output ring, stretcher, B, stack) */
    uint32_t reg_bytes;
    int me_state, me_mode, me_attaches, me_demotes, me_live_at_stop;
    volatile int me_fault;                       /* set by the feeder: the ME did not deliver a hop in time */
    char me_why[160];
    uint32_t pw_parks, pw_resumes, pw_switches, pw_prx_resumes;
    int sleep_tried;
    uint64_t pw_tick_us;
    uint64_t me_t0_us, me_live_us, me_t_demote;  /* attached since / total time attached / last hand-back */
    uint64_t produced_base;                      /* A.produced when the ME took over the output */
    SceUID sim_th;                               /* audio_me_sim: the thread that plays the ME */
    /* feeder hand-over: the game thread asks, the feeder switches between hops */
    volatile uint32_t fd_req, fd_ack;
    volatile int fd_mode_req;                    /* 0 main hops, 1 the ME's hops, 2 paused */
    volatile uint32_t fd_me_wait_us, fd_me_hops, fd_me_timeouts;
    /* shadow compare */
    uint32_t *sh_hist, sh_h, sh_main_n, sh_cmp_n, sh_compared, sh_mism, sh_first;
    /* the statistics window */
    uint32_t w_me_count, w_me_busy_r, w_me_busy_h, w_me_renders, w_fd_me_wait;
    uint64_t w_me_waits, w_me_t_us;
} A;

static inline uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* ------------------------------------------------------------------ memory: one partition block, carved */
static void *carve(uint32_t n)
{
    uint32_t o = (A.carved + 63u) & ~63u;
    if (o + n > A.mem_bytes) return NULL;
    A.carved = o + n;
    return A.mem + o;
}

static uint32_t tables_used;
static void *arena_alloc(void *user, size_t n)    /* SnddConfig.alloc: the IOP's config tables (fno 0 only) */
{
    (void)user;
    uint32_t o = (tables_used + 15u) & ~15u;
    if (o + n > ARENA_BYTES) {
        LOG("audio: config tables need more than %u KB (asked %u B more) - the model's init will fail\n",
            ARENA_BYTES >> 10, (unsigned)n);
        return NULL;
    }
    tables_used = o + (uint32_t)n;
    return A.arena + o;                          /* zeroed with the block */
}

/* ------------------------------------------------------------------ data: the disc layer (synchronous, as the host) */
static AFile *afile(const char *path)
{
    for (int i = 0; i < A.nfiles; i++)
        if (!strcmp(A.files[i].path, path)) return &A.files[i];
    AFile *f = &A.files[A.nfiles < MAX_FILES ? A.nfiles++ : MAX_FILES - 1];
    snprintf(f->path, sizeof f->path, "%s", path);
    f->size = 0;
    f->idx = hle_disc_open(path, &f->size);      /* "AUDIO\SFX\GENRL01.PAK" -> disc/audio/SFX/GENRL01.pak */
    if (f->idx < 0) {
        A.io_missing++;
        LOG("audio: %s not readable on the memory stick (convert again with sound: --audio all or min) - "
            "the IOP model sees a read error\n", path);
    }
    return f;
}

static int io_size(void *user, const char *path, uint32_t *size_out)
{
    (void)user;
    AFile *f = afile(path);
    if (f->idx < 0) return SNDD_IO_ERROR;
    *size_out = f->size;
    return SNDD_IO_OK;
}

/* bytes [off, off+n) of a stream pak through the block cache (16 KB requests instead of 0.1-4 KB ones) */
static int cached_read(AFile *f, uint32_t off, uint8_t *dst, uint32_t n)
{
    while (n) {
        uint32_t blk = off / CBLOCK, in = off % CBLOCK;
        CBlk *c = NULL, *lru = &A.cache[0];
        for (int i = 0; i < A.ncache; i++) {
            if (A.cache[i].len && A.cache[i].file == f->idx && A.cache[i].blk == blk) { c = &A.cache[i]; break; }
            if (A.cache[i].used < lru->used) lru = &A.cache[i];
        }
        if (c) {
            A.io_hits++;
        } else {
            c = lru;
            uint32_t bo = blk * CBLOCK, len = f->size - bo < CBLOCK ? f->size - bo : CBLOCK;
            c->len = 0;
            if (hle_disc_read_fd(f->idx, bo, c->data, len) != (int64_t)len) return 0;
            c->file = f->idx;
            c->blk = blk;
            c->len = len;
            A.io_misses++;
            A.io_reads++;
            A.io_bytes += len;
        }
        c->used = ++A.cache_tick;
        uint32_t k = c->len > in ? c->len - in : 0;
        if (k > n) k = n;
        if (!k) return 0;
        memcpy(dst, c->data + in, k);
        dst += k;
        off += k;
        n -= k;
    }
    return 1;
}

static int io_read(void *user, const char *path, uint32_t off, void *dst, uint32_t n)
{
    (void)user;
    AFile *f = afile(path);
    if (f->idx < 0 || off > f->size || n > f->size - off) { A.io_errors++; return SNDD_IO_ERROR; }
    if (!n) return SNDD_IO_OK;
    uint64_t t0 = now_us();
    int ok;
    if (A.ncache && n < CBLOCK && !strncmp(path, "AUDIO\\STREAMS\\", 14)) {
        ok = cached_read(f, off, dst, n);
    } else {
        ok = hle_disc_read_fd(f->idx, off, dst, n) == (int64_t)n;
        A.io_reads++;
        A.io_bytes += n;
    }
    A.io_us += now_us() - t0;
    if (!ok) { A.io_errors++; return SNDD_IO_ERROR; }
    return SNDD_IO_OK;
}

/* ------------------------------------------------------------------ PPSSPP check: WAV of the 48 kHz lockstep mix */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void dump_header(uint32_t frames)
{
    uint8_t w[44];
    memcpy(w, "RIFF", 4); put32(w + 4, 36 + frames * 4u); memcpy(w + 8, "WAVEfmt ", 8); put32(w + 16, 16);
    w[20] = 1; w[21] = 0; w[22] = 2; w[23] = 0; put32(w + 24, SPU2_RATE); put32(w + 28, SPU2_RATE * 4u);
    w[32] = 4; w[33] = 0; w[34] = 16; w[35] = 0; memcpy(w + 36, "data", 4); put32(w + 40, frames * 4u);
    sceIoLseek(A.dump_fd, 0, PSP_SEEK_SET);
    sceIoWrite(A.dump_fd, w, sizeof w);
    sceIoLseek(A.dump_fd, 0, PSP_SEEK_END);
}

static void dump_close(void)
{
    if (A.dump_fd < 0) return;
    if (A.dump_len) sceIoWrite(A.dump_fd, A.dump_buf, A.dump_len);
    A.dump_len = 0;
    dump_header(A.dump_frames);
    sceIoClose(A.dump_fd);
    A.dump_fd = -1;
    LOG("audio: audio_dump.wav closed: %u frames at 48 kHz (%.1f s of virtual time)\n", A.dump_frames,
        A.dump_frames / 48000.0);
}

static void dump(const int16_t *s48, uint32_t n)
{
    if (A.dump_fd < 0) return;
    if (n > A.dump_max - A.dump_frames) n = A.dump_max - A.dump_frames;
    uint32_t bytes = n * 4u;
    const uint8_t *p = (const uint8_t *)s48;
    while (bytes) {
        uint32_t k = DUMP_BUF - A.dump_len < bytes ? DUMP_BUF - A.dump_len : bytes;
        memcpy(A.dump_buf + A.dump_len, p, k);
        A.dump_len += k;
        p += k;
        bytes -= k;
        if (A.dump_len == DUMP_BUF) { sceIoWrite(A.dump_fd, A.dump_buf, DUMP_BUF); A.dump_len = 0; }
    }
    A.dump_frames += n;
    if (A.dump_frames >= A.dump_max) dump_close();
}

/* ------------------------------------------------------------------ the engine's sink: 48 kHz -> 44.1 kHz -> ring */
static void sink(void *user, const int16_t *s, uint32_t n)
{
    (void)user;
    if (A.dump_fd >= 0) dump(s, n);
    if (A.me_state == ZME_ON && A.me_mode == ZMA_SHADOW && A.sh_hist) {   /* one call = one engine block */
        A.sh_h = zma_hash(A.sh_h, s, n);
        A.sh_hist[A.sh_main_n % ZMA_SHADOW_HIST] = A.sh_h;
        A.sh_main_n++;
    }
    int16_t tmp[2 * 64];
    while (n) {                                      /* 64 input frames -> at most 59 output frames */
        uint32_t k = n < 64u ? n : 64u;
        uint32_t m = as_resample48(&A.rs, s, k, tmp);
        if (m) { as_ring_write(&A.ring, tmp, m); A.produced += m; }
        s += 2u * k;
        n -= k;
    }
}
/* ------------------------------------------------------------------ the feeder thread */
static int feeder(SceSize args, void *argp)
{
    (void)args; (void)argp;
    A.running = 1;
    uint64_t last = now_us(), carry = 0;
    int cur = 0, mode = 0;
    while (!A.quit) {
        if (A.fd_ack != A.fd_req) {                      /* the game thread hands the output over */
            mode = A.fd_mode_req;
            __asm__ volatile("sync" ::: "memory");
            A.fd_ack = A.fd_req;
        }
        if (mode == 2) { sceKernelDelayThread(5000); last = now_us(); continue; }     /* paused */
        uint64_t t = now_us();
        uint64_t f100 = (t - last) * AS_RATE + carry;     /* output frames of real time since the last hop */
        last = t;
        uint32_t elapsed = (uint32_t)(f100 / 1000000u);
        carry = f100 % 1000000u;
        if (mode == 1) {                                  /* the ME runs the stretcher: ask for one hop */
            uint32_t waited = 0;
            int snd = zma_hop(&A.z, elapsed, (uint32_t)cur, (uint32_t)opt.me_hop_ms * 1000u, &waited);
            A.fd_me_wait_us += waited;
            if (snd < 0) { A.fd_me_timeouts++; A.me_fault = 1; sceKernelDelayThread(5000); continue; }
            A.fd_me_hops++;
            if (!snd) { sceKernelDelayThread(8000); continue; }
            /* the ME wrote the hop to RAM; if the audio driver reads it through this core's cache, an older lap's
               lines of the same buffer may still be there (clean - this core never writes it): drop them (writeback
               first: with audio_me_sim the "ME" is a thread of this core and its lines are the dirty, current ones) */
            sceKernelDcacheWritebackInvalidateRange(A.reg->hop[cur], sizeof A.reg->hop[cur]);
            uint32_t t1 = sceKernelGetSystemTimeLow();
            sceAudioOutputPannedBlocking(A.ch, A.vol, A.vol, A.reg->hop[cur]);
            A.feed_wait_us += sceKernelGetSystemTimeLow() - t1;
            A.feed_blocks++;
            cur = (cur + 1) % OUT_BUFS;
            continue;
        }
        int16_t *o = A.out + (uint32_t)cur * AS_HOP * 2u;
        uint32_t t0 = sceKernelGetSystemTimeLow();
        int snd = as_hop(A.st, o, elapsed);
        A.feed_busy_us += sceKernelGetSystemTimeLow() - t0;
        if (!snd) {                                       /* prebuffering / starved: the channel drains, no block */
            sceKernelDelayThread(8000);
            continue;
        }
        uint32_t t1 = sceKernelGetSystemTimeLow();
        sceAudioOutputPannedBlocking(A.ch, A.vol, A.vol, o);
        A.feed_wait_us += sceKernelGetSystemTimeLow() - t1;
        A.feed_blocks++;
        cur = (cur + 1) % OUT_BUFS;
    }
    A.running = 0;
    return 0;
}

/* ------------------------------------------------------------------ the Media Engine split (game thread) */
_Static_assert(OUT_BUFS == ZMA_HOPS, "the feeder's buffers = the ME's hop buffers");

/* audio_me_sim=1 (emulator integration test): zma_work_sim on a main-core thread instead of the ME - the same loop,
   the same protocol, no cache ops; it yields while idle (priority 0x18: between the feeder and the game thread) */
static int sim_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    zma_work_sim(&A.mctl->h);
    return 0;
}

static int me_start(char *why, int why_n)
{
    if (!opt.me_sim) return hle_me_start(zma_work, &A.mctl->h, A.reg, A.reg_bytes, ZMA_STACK, why, why_n);
    A.mctl->h.magic = HLE_ME_MAGIC;
    A.sim_th = sceKernelCreateThread("zme_sim", sim_thread, 0x18, 0x8000, THREAD_ATTR_USER, NULL);
    if (A.sim_th < 0 || sceKernelStartThread(A.sim_th, 0, NULL) < 0) {
        snprintf(why, why_n, "simulation thread failed (%08x)", (unsigned)A.sim_th);
        A.sim_th = -1;
        return 0;
    }
    for (int i = 0; i < 100 && A.mctl->h.state != 1; i++) sceKernelDelayThread(1000);
    return A.mctl->h.state == 1;
}

/* park (no handshake) / stop (handshake): the real ME is held in reset; the simulation thread is asked to leave */
static int me_halt(int handshake_ms)
{
    if (!opt.me_sim) return handshake_ms ? hle_me_stop(handshake_ms) : (hle_me_park(), 0);
    int ok = 0;
    if (A.sim_th >= 0) {
        A.mctl->h.exit_req = 1;
        for (int i = 0; i < 300 && A.mctl->h.state != 2; i++) sceKernelDelayThread(1000);
        ok = A.mctl->h.state == 2;
        if (ok) sceKernelDeleteThread(A.sim_th);
        A.sim_th = -1;
    }
    return ok;
}

/* switch the feeder between hops (0 main, 1 ME, 2 paused); 1 = done (or no feeder), 0 = it did not answer in 300 ms */
static int feeder_mode(int m)
{
    if (A.thid < 0 || !A.running) return 1;
    A.fd_mode_req = m;
    __asm__ volatile("sync" ::: "memory");
    uint32_t req = A.fd_req + 1u;
    A.fd_req = req;
    for (int i = 0; i < 300; i++) {
        if (A.fd_ack == req) return 1;
        sceKernelDelayThread(1000);
    }
    LOG("audio: ME: the feeder thread did not switch to mode %d within 300 ms\n", m);
    return 0;
}

static void me_demote(const char *why, int state)
{
    if (A.me_state != ZME_ON) return;
    me_halt(0);                                  /* held in reset: it may be dead, stalled or about to sleep */
    zma_jobs_unbind(A.mctl, state == ZME_FAILED);   /* jobs that want the ME get the jobs-only loop */
    uint32_t ca, ep, bv;
    if (hle_me_exception(&ca, &ep, &bv))         /* its handler recorded the exception (hle_me.c) */
        LOG("audio: the ME took an exception: Cause %08x EPC %08x BadVAddr %08x (map EPC & 0x0FFFFFFF with poc6.elf)\n",
            (unsigned)ca, (unsigned)ep, (unsigned)bv);
    uint64_t now = now_us();
    A.me_live_us += now - A.me_t0_us;
    A.me_t_demote = now;
    A.me_state = state;
    A.me_demotes++;
    if (A.me_mode == ZMA_LIVE) {
        feeder_mode(2);
        /* the ME's ring and stretcher in RAM are not trustworthy (its cache was not written back): start fresh */
        A.produced += A.mctl->produced44;
        AsStats keep;                            /* the statistics carry on (the windows count from them) */
        zma_stats(&A.z, &keep);
        uint32_t lost = A.mctl->ring_lost;
        as_ring_init(&A.ring, zma_ring_buf(A.reg), RING_FRAMES);
        A.ring.lost = lost;
        as_init(A.st, &A.ring, opt.sync, (uint32_t)opt.latency_ms * AS_RATE / 1000u, (uint32_t)opt.smax * AS_ONE);
        A.st->smin = (uint32_t)opt.smin_pct * AS_ONE / 100u;
        A.st->st = keep;
        spu2_rec_set_mode(&A.rec, 0);            /* the full engine again (its first block after this is silent) */
        feeder_mode(0);
    } else {
        spu2_rec_set_mode(&A.rec, 0);
    }
    LOG("audio: ME %s stopped after %.1f s: %s - main-core mixing%s\n", A.me_mode == ZMA_LIVE ? "live" : "shadow",
        (double)(now - A.me_t0_us) / 1e6, why, state == ZME_SLEEP ? " until the PSP wakes up" : "");
}

static void me_attach(void)
{
    char why[200];
    if (hle_psp_is_emulator() && !opt.me_sim) {  /* PPSSPP does not emulate the ME: say so once, mix on the main core */
        A.me_state = ZME_FAILED;
        snprintf(A.me_why, sizeof A.me_why, "emulator");
        LOG("audio: ME not started (emulator: PPSSPP does not emulate the Media Engine) - main-core mixing "
            "(audio_me=%d)\n", opt.me);
        return;
    }
    int mode = opt.me == 2 ? ZMA_SHADOW : ZMA_LIVE;
    if (mode == ZMA_SHADOW && !A.sh_hist) { A.me_state = ZME_FAILED; LOG("audio: ME shadow: no memory\n"); return; }
    if (mode == ZMA_LIVE && !feeder_mode(2)) { A.me_state = ZME_FAILED; return; }
    /* engine B = A now (A has been the full engine so far): no timer, no sink, no hooks */
    memcpy(&A.reg->b, A.e, sizeof(Spu2));
    A.reg->b.timer_cb = 0;
    A.reg->b.timer_user = 0;
    A.reg->b.sink = 0;
    A.reg->b.sink_user = 0;
    A.reg->b.tap = 0;
    A.reg->b.x_state_only = 0;
    if (mode == ZMA_LIVE) {                      /* the output stage moves to the ME as it stands */
        A.reg->ring = A.ring;
        A.reg->st.ring = &A.reg->ring;
        A.reg->rs = A.rs;
    }
    A.z.stall_ms = opt.me_stall_ms;
    zma_host_init(&A.z, A.mctl, A.mcmds, A.reg, mode);
    zma_jobs_yield();                            /* a jobs-only loop gives the ME to the audio (handshake) */
    zma_jobs_bind(A.mctl, opt.me_sim);           /* ... whose loop polls the jobs from now on (hle_me_jobs.h) */
    {                                            /* the stretcher's statistics carry on (the ME continues A.st) */
        const uint32_t *sw = (const uint32_t *)(const void *)&A.st->st;
        for (uint32_t i = 0; i < sizeof(AsStats) / 4u; i++) A.mctl->st_words[i] = sw[i];
        A.mctl->ring_lost = A.ring.lost;
    }
    A.sh_h = ZMA_HASH0;
    A.sh_main_n = A.sh_cmp_n = A.sh_compared = A.sh_mism = A.sh_first = 0;
    A.me_mode = mode;
    A.me_fault = 0;
    spu2_rec_set_mode(&A.rec, mode == ZMA_LIVE ? 1 : 2);
    if (opt.me_sleep && !opt.me_sim && !A.sleep_tried) {   /* before the first takeover (no scePowerLock) */
        char w[240];
        A.sleep_tried = 1;
        int on = hle_me_sleep_enable(w, sizeof w);
        LOG("audio: ME sleep support (audio_me_sleep=1): %s - %s\n", on ? "ON, the PSP may sleep with the ME taken"
            : "NOT available, sleep held off (scePowerLock) from the takeover on", w);
    }
    uint64_t t0 = now_us();
    int ok = me_start(why, sizeof why);
    if (!ok) {
        spu2_rec_set_mode(&A.rec, 0);
        if (mode == ZMA_LIVE) { A.st->ring = &A.ring; feeder_mode(0); }
        A.me_state = ZME_FAILED;
        snprintf(A.me_why, sizeof A.me_why, "%s", why);
        LOG("audio: ME not started (%s) - main-core mixing (audio_me=%d)\n", why, opt.me);
        zma_jobs_unbind(A.mctl, 1);              /* jobs that want the ME get the jobs-only loop */
        return;
    }
    A.me_state = ZME_ON;
    A.me_attaches++;
    A.me_t0_us = now_us();
    HleMePower p;
    hle_me_power_events(&p);
    A.pw_parks = p.parks;
    A.pw_resumes = p.resumes;
    A.w_me_count = A.mctl->h.count;
    A.w_me_t_us = A.me_t0_us;
    A.w_me_busy_r = A.w_me_busy_h = A.w_me_renders = 0;
    A.w_me_waits = 0;
    A.w_fd_me_wait = A.fd_me_wait_us;
    if (mode == ZMA_LIVE) feeder_mode(1);
    LOG("audio: ME %s%s: the Media Engine renders the samples%s (start %.1f ms, attach #%d, main-loop iteration %llu); "
        "control %p, commands %p (%u), region %p + %u KB; %s %08x)\n", mode == ZMA_LIVE ? "LIVE" : "SHADOW",
        opt.me_sim ? " (SIMULATED by a main-core thread: audio_me_sim=1)" : "",
        mode == ZMA_LIVE ? ", resamples and stretches them; the feeder only plays its hops"
                         : " too and the hashes are compared (the main core's mix is what plays)",
        (double)(A.me_t0_us - t0) / 1000.0, A.me_attaches, (unsigned long long)hle_mainloop_iters, (void *)A.mctl,
        (void *)A.mcmds, ZMA_CMDS, (void *)A.reg, A.reg_bytes >> 10,
        hle_me_sleep_hooked() ? "sleep through ps2rc_exc.prx (scePowerLock not taken:"
                              : "sleep held off for the rest of the session (scePowerLock",
        (unsigned)hle_me_power_lock_rc());
}

/* (hle_me_jobs.h) the audio runs on the ME (live or shadow, attached and not handed back) */
int hle_audio_me_live(void) { return A.me_state == ZME_ON; }

static void shadow_compare(void)
{
    volatile ZmaCtl *c = A.mctl;
    uint32_t n = c->sh_n, h = c->sh_h;
    if (n != c->sh_n || n <= A.sh_cmp_n || n > A.sh_main_n || A.sh_main_n - n >= ZMA_SHADOW_HIST) return;
    A.sh_cmp_n = n;
    A.sh_compared++;
    if (A.sh_hist[(n - 1u) % ZMA_SHADOW_HIST] != h) {
        if (!A.sh_mism) {
            A.sh_first = n;
            LOG("audio: ME shadow: MISMATCH at render %u (main %08x, ME %08x)\n", n,
                (unsigned)A.sh_hist[(n - 1u) % ZMA_SHADOW_HIST], (unsigned)h);
        }
        A.sh_mism++;
        A.sh_cmp_n = 0xFFFFFFFFu;                /* cumulative hashes: everything after differs too - stop comparing */
    }
}

/* once per sound driver frame RPC, before the driver runs */
static void me_service(void)
{
    HleMePower p;
    hle_me_power_events(&p);
    zma_jobs_tick();                             /* the ME Count calibration (short windows) */
    if (hle_me_power_lock_rc() != 1 && !hle_me_sleep_hooked()) {   /* taken over, no sleep hook: no sleep */
        uint64_t now = now_us();
        if (now - A.pw_tick_us > 1000000u) {     /* hold off the idle auto-sleep too (it would hang the same way) */
            A.pw_tick_us = now;
            scePowerTick(PSP_POWER_TICK_SUSPEND);
        }
        if (p.switches != A.pw_switches && !hle_me_sleep_hooked()) {
            A.pw_switches = p.switches;
            LOG("audio: the power switch was moved - sleep is held off while this session has taken the Media Engine "
                "(the OS's ME driver would wait forever at suspend; scePowerLock %08x); audio_me=0 keeps sleep "
                "working\n", (unsigned)hle_me_power_lock_rc());
        }
        if (hle_me_sleep_hooked() && *hle_me_resume_epoch() != A.pw_prx_resumes) {
            char w[240];
            A.pw_prx_resumes = *hle_me_resume_epoch();
            hle_me_sleep_info(w, sizeof w);
            LOG("audio: the PSP slept and woke up with the ME taken (%s); ME heartbeat %u, boots %u\n", w,
                (unsigned)A.mctl->h.heartbeat, (unsigned)A.mctl->h.boots);
        }
    }
    if (!opt.me || A.me_state == ZME_FAILED || A.me_state == ZME_STOPPED) return;
    if (A.me_state == ZME_ON) {
        if (p.parks != A.pw_parks) { me_demote("the PSP went to sleep (the power callback parked the ME)", ZME_SLEEP); return; }
        if (A.me_fault) { me_demote("the ME did not deliver a hop in time", ZME_FAILED); return; }
        if (A.mctl->h.fault) {
            snprintf(A.me_why, sizeof A.me_why, "the ME refused command %08x", (unsigned)A.mctl->h.fault_arg);
            me_demote(A.me_why, ZME_FAILED);
            return;
        }
        if (A.me_mode == ZMA_SHADOW) shadow_compare();
        if (opt.me_sim && opt.me_sim_hang && !A.mctl->sim_hang && now_us() - A.me_t0_us > (uint64_t)opt.me_sim_hang * 1000000u) {
            A.mctl->sim_hang = 1;                /* emulator test of the hand-back: the "ME" stops working */
            LOG("audio: ME simulation: hanging now (audio_me_sim_hang=%d)\n", opt.me_sim_hang);
        }
        return;
    }
    if (A.me_state == ZME_SLEEP) {               /* woken up - or the switch was flicked without a sleep (10 s) */
        if ((p.resumes != A.pw_resumes || now_us() - A.me_t_demote > 10000000u) && A.me_attaches < 16) me_attach();
        return;
    }
    if (hle_mainloop_iters > 0) me_attach();     /* ZME_OFF: the intro movies (sceMpeg uses the ME) are over */
}

/* Spu2Rec callbacks (game thread, inside the driver) */
static void me_push(void *u, uint32_t w0, uint32_t w1)
{
    (void)u;
    if (A.me_state != ZME_ON) return;
    if (!zma_push(&A.z, w0, w1)) me_demote("the ME stopped consuming commands (ring full)", ZME_FAILED);
}

static void me_barrier(void *u)
{
    (void)u;
    if (A.me_state != ZME_ON) return;
    if (!zma_barrier(&A.z)) me_demote("the ME did not catch up before an SPU RAM write", ZME_FAILED);
}

static void me_ram_written(void *u, uint32_t addr, uint32_t size)
{
    (void)u;
    uintptr_t a = ((uintptr_t)A.spu_ram + addr) & ~63u, e = ((uintptr_t)A.spu_ram + addr + size + 63u) & ~63u;
    sceKernelDcacheWritebackRange((void *)a, (unsigned)(e - a));   /* the bytes to RAM before the ME may read them */
}

/* ------------------------------------------------------------------ set-up */
int hle_audio_psp_init(void (*log)(void *user, int level, const char *fmt, ...))
{
    memset(&A, 0, sizeof A);
    A.mem_id = -1;
    A.thid = -1;
    A.ch = -1;
    A.dump_fd = -1;
    A.sim_th = -1;
    A.log = log;
    read_cfg();
    if (!opt.on) { LOG("audio: off (poc6.cfg audio=0) - sound driver silent responder\n"); return 0; }
    /* the AUDIO folder must be on the card: without its config tables the driver's init would answer 0 (the game's
       audio hardware init fails) - keep the silent responder then */
    uint32_t sz = 0;
    if (hle_disc_open("AUDIO\\CONFIG\\BANKSLOT.DAT", &sz) < 0) {
        LOG("audio: AUDIO/CONFIG/BankSlot.dat not on the memory stick (converted with --audio none?) - sound driver silent responder\n");
        return 0;
    }
    opt.cache_kb = opt.cache_kb / (CBLOCK >> 10) * (CBLOCK >> 10);
    A.ncache = opt.cache_kb / (int)(CBLOCK >> 10);
    uint32_t need = SPU2_RAM_SIZE + ARENA_BYTES + (uint32_t)sizeof(Spu2) +
                    (uint32_t)sizeof(AudioStretch) + RING_FRAMES * 4u + OUT_BUFS * AS_HOP * 4u +
                    (uint32_t)A.ncache * (CBLOCK + (uint32_t)sizeof(CBlk)) + (16u + (uint32_t)A.ncache) * 64u;
    if (opt.me) {                                /* the ME region replaces the stretcher + ring carve */
        A.reg_bytes = ZMA_REGION_BYTES(RING_FRAMES);
        need += A.reg_bytes - (uint32_t)sizeof(AudioStretch) - RING_FRAMES * 4u + ZMA_ALIGN64((uint32_t)sizeof(ZmaCtl)) +
                ZMA_CMDS * 8u + 3u * 64u;
    }
    unsigned free0 = (unsigned)(sceKernelTotalFreeMemSize() >> 10);
    /* poc6.cfg mem_volatile=1 - the GE buffers live in the volatile partition,
       so the guest code hole has room for this block (the user memory it leaves goes to the game heap) */
    uint8_t *in_hole = hle_psp_mem_volatile() ? hle_psp_hole_take(need, 64) : NULL;
    if (in_hole) {
        LOG("audio: %u KB in the guest code hole (mem_volatile=1), %u KB of the hole left\n", need >> 10,
            hle_psp_hole_free_kb());
        A.mem_id = -1;
        A.mem = in_hole;
        A.mem_bytes = need;
        memset(A.mem, 0, need);
    } else
        A.mem_id = sceKernelAllocPartitionMemory(2, "ps2rc_audio", PSP_SMEM_Low, need, NULL);
    if (!in_hole && A.mem_id < 0) {
        LOG("audio: cannot allocate %u KB (err %08x, free %u KB, largest %u KB) - sound driver silent responder\n",
            need >> 10, (unsigned)A.mem_id, free0, (unsigned)(sceKernelMaxFreeMemSize() >> 10));
        A.mem_id = -1;
        return 0;
    }
    if (!in_hole) {
        A.mem = (uint8_t *)sceKernelGetBlockHeadAddr(A.mem_id);
        A.mem_bytes = need;
        memset(A.mem, 0, need);
    }
    uint8_t *spu_ram = carve(SPU2_RAM_SIZE);
    A.arena = carve(ARENA_BYTES);
    A.a = snd_driver_instance();                  /* the snd_driver.h singleton (in the EBOOT's bss) */
    A.e = carve(sizeof(Spu2));
    int16_t *ring;
    if (opt.me) {                                /* ME region (stretcher, ring, B, hops, stack) + shared words */
        A.reg = carve(A.reg_bytes);
        A.st = &A.reg->st;
        ring = zma_ring_buf(A.reg);
        uint8_t *c = carve(ZMA_ALIGN64((uint32_t)sizeof(ZmaCtl)));
        uint8_t *q = carve(ZMA_CMDS * 8u);
        /* zeroed through the cache by the memset above: write back + drop those lines BEFORE the uncached use (a
           dirty cached line evicted later would overwrite what the ME wrote) */
        sceKernelDcacheWritebackInvalidateRange(c, ZMA_ALIGN64((uint32_t)sizeof(ZmaCtl)));
        sceKernelDcacheWritebackInvalidateRange(q, ZMA_CMDS * 8u);
        A.mctl = (volatile ZmaCtl *)((uintptr_t)c | 0x40000000u);
        A.mcmds = (volatile uint32_t *)((uintptr_t)q | 0x40000000u);
        A.sh_hist = malloc(ZMA_SHADOW_HIST * 4u);
    } else {
        A.st = carve(sizeof(AudioStretch));
        ring = carve(RING_FRAMES * 4u);
    }
    A.out = carve(OUT_BUFS * AS_HOP * 4u);
    A.cache = A.ncache ? carve((uint32_t)A.ncache * (uint32_t)sizeof(CBlk)) : NULL;
    for (int i = 0; i < A.ncache; i++) {
        A.cache[i].data = carve(CBLOCK);
        A.cache[i].file = -1;
    }
    /* driver + engine, exactly as audio_host_create binds them (lockstep), the sink into the ring */
    spu2_init(A.e, spu_ram);
    A.e->interp = opt.interp_nearest ? SPU2_INTERP_NEAREST : SPU2_INTERP_LINEAR;
    A.e->mix_limit = (uint32_t)opt.voices;      /* the loudest N sounding voices are mixed; replies unaffected */
    A.e->x_ref = opt.xref;                       /* reference engine loops (A/B) */
    SnddConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.io.user = NULL;
    cfg.io.size = io_size;
    cfg.io.read = io_read;
    A.spu_ram = spu_ram;
    zma_jobs_sleep_allowed(opt.me_sleep && !opt.me_sim);   /* a jobs-only loop sleeps like the audio's */
    if (opt.me) {                                /* the recorder between the driver and the engine (mode 0 =
                                                    transparent until the ME takes over) */
        spu2_rec_bind(&A.rec, A.e, &cfg.spu, A.a);
        A.rec.push = me_push;
        A.rec.barrier = me_barrier;
        A.rec.ram_written = me_ram_written;
        A.rec.user = NULL;
        hle_me_set_regs(opt.me_regs);
        int pw = hle_me_power_init();
        LOG("audio: audio_me=%d (%s): the Media Engine takes over once the main loop runs; power callback %s (%08x)\n",
            opt.me, opt.me == 2 ? "shadow - the ME's blocks are compared, the main core's play" : "live",
            pw >= 0 ? "registered" : "NOT registered", (unsigned)pw);
    } else {
        spu2_bind_sndd(A.e, &cfg.spu, A.a, 1);
    }
    cfg.alloc = arena_alloc;
    cfg.log = log;
    sndd_init(A.a, &cfg);
    as_ring_init(&A.ring, ring, RING_FRAMES);
    as_init(A.st, &A.ring, opt.sync, (uint32_t)opt.latency_ms * AS_RATE / 1000u, (uint32_t)opt.smax * AS_ONE);
    A.st->smin = (uint32_t)opt.smin_pct * AS_ONE / 100u;   /* as_init's 3/4 overridden (default 1x) */
    A.e->sink = sink;
    A.e->sink_user = NULL;
    LOG("audio: IOP audio model + SPU2-lite in lockstep with virtual time; memory %u KB in one partition block "
        "(SPU RAM 2048, config tables %u, structs %u, ring %u = %.2f s, stream cache %u, out %u; free %u -> %u KB, "
        "largest %u KB)\n", need >> 10, ARENA_BYTES >> 10,
        (unsigned)((sizeof(Spu2) + sizeof(AudioStretch)) >> 10), (RING_FRAMES * 4u) >> 10,
        (double)RING_FRAMES / AS_RATE, (unsigned)A.ncache * (CBLOCK >> 10), (OUT_BUFS * AS_HOP * 4u) >> 10, free0,
        (unsigned)(sceKernelTotalFreeMemSize() >> 10), (unsigned)(sceKernelMaxFreeMemSize() >> 10));
    /* the output: one sceAudio channel + the feeder thread */
    if (opt.out) {
        A.vol = PSP_AUDIO_VOLUME_MAX * opt.vol / 100;
        A.ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AS_HOP, PSP_AUDIO_FORMAT_STEREO);
        if (A.ch < 0) {
            LOG("audio: no sceAudio channel (%08x) - the model runs, nothing is played\n", (unsigned)A.ch);
        } else {
            A.thid = sceKernelCreateThread("hle_audio_out", feeder, opt.prio, 0x4000, THREAD_ATTR_USER, NULL);
            if (A.thid < 0 || sceKernelStartThread(A.thid, 0, NULL) < 0) {
                LOG("audio: feeder thread failed (%08x) - the model runs, nothing is played\n", (unsigned)A.thid);
                sceAudioChRelease(A.ch);
                A.ch = -1;
                A.thid = -1;
            }
        }
    } else {
        LOG("audio: audio_out=0 - the model runs, nothing is played\n");
    }
    static const char *const sync_names[] = {"stretch", "gap", "pitch"};
    LOG("audio: output %s: channel %d, volume %d%%, sync %s, latency %d ms, stretch %d%%..%dx, interp %s, voices mixed %d, "
        "feeder priority 0x%02x, stats every %d s%s\n", A.ch >= 0 ? "on" : "OFF", A.ch, opt.vol, sync_names[opt.sync],
        opt.latency_ms, opt.smin_pct, opt.smax, opt.interp_nearest ? "nearest" : "linear", opt.voices, opt.prio, opt.stats_s,
        opt.dump_s ? ", audio_dump requested" : "");
    if (opt.xref) LOG("audio: engine reference loops (poc6.cfg audio_xref=1, A/B)\n");
    if (opt.dump_s) {
        if (!hle_psp_is_emulator()) {
            LOG("audio: audio_dump refused on the PSP (card writes during the game; emulator only)\n");
        } else {
            char path[320];
            snprintf(path, sizeof path, "%saudio_dump.wav", hle_plat_disc_root());
            A.dump_buf = malloc(DUMP_BUF);
            A.dump_fd = A.dump_buf ? sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777) : -1;
            if (A.dump_fd >= 0) {
                A.dump_max = (uint32_t)opt.dump_s * 48000u;
                dump_header(0);
                LOG("audio: writing the first %d s of the 48 kHz lockstep mix to %s\n", opt.dump_s, path);
            } else {
                LOG("audio: cannot create %s\n", path);
            }
        }
    }
    return 1;
}

SndDrv *hle_audio_psp_model(void) { return A.a; }

/* ------------------------------------------------------------------ statistics */
static void stats_line(const char *what, uint64_t now, uint64_t vt_us)
{
    double real_s = (double)(now - A.w_t0_us) / 1e6, virt_s = (double)(vt_us - A.w_vt0_us) / 1e6;
    if (real_s <= 0) real_s = 1e-6;
    int me_live = A.me_state == ZME_ON && A.me_mode == ZMA_LIVE;
    AsStats sv;
    if (me_live) zma_stats(&A.z, &sv);           /* the ME's stretcher (its own memory: never read it) */
    else sv = A.st->st;
    const AsStats *s = &sv;
    uint64_t produced = A.produced + (me_live ? A.mctl->produced44 : 0u);
    uint32_t lost = me_live ? A.mctl->ring_lost : A.ring.lost;   /* the ME's ring copy carries the count on */
    uint32_t busy = A.feed_busy_us;
    uint32_t sound = s->hops_sound - A.w_hops_sound, under = s->underruns - A.w_underruns;
    double savg = sound ? (double)(s->s_sum - A.w_ssum) / sound / AS_ONE : 0.0;
    double fill_ms = sound ? (double)(s->fill_sum - A.w_fill) / sound * 1000.0 / AS_RATE : 0.0;
    double model_ms = (double)(A.rpc_us - A.w_rpc_us - (A.io_us - A.w_io_us)) / 1000.0;
    LOG("audio %s: %.1f s real / %.1f s virtual: produced %.1f s of sound, played %u hops (%.1f s, %u bit-exact), "
        "underruns %u (%.1f/min), drops %u, stretch avg %.2f, fill avg %.0f ms, %u game frames (largest %u ms), voices "
        "mixed %.1f of %.1f sounding; CPU: model+engine %.1f ms/s (game thread), feeder %.1f ms/s; io %.1f ms/s; ring "
        "lost %u frames; peak voices %u\n", what, real_s, virt_s,
        (double)(produced - A.w_produced) / AS_RATE, sound, sound * (double)AS_HOP / AS_RATE,
        s->hops_unity - A.w_unity, under, under * 60.0 / real_s, s->drops - A.w_drops, savg, fill_ms, A.w_frames,
        A.w_maxms, virt_s > 0 ? (double)(A.e->ticks_mixed - A.w_tmix) / 48000.0 / virt_s : 0.0,
        virt_s > 0 ? (double)(A.e->ticks_mixed - A.w_tmix + A.e->ticks_quiet - A.w_tquiet) / 48000.0 / virt_s : 0.0,
        model_ms / real_s, (busy - A.w_busy_us) / 1000.0 / real_s, (double)(A.io_us - A.w_io_us) / 1000.0 / real_s,
        lost, A.e->peak_voices);
    if (opt.me) {                                /* the Media Engine */
        volatile ZmaCtl *c = A.mctl;
        if (A.me_state == ZME_ON) {
            uint32_t cnt = c->h.count;
            double mhz = opt.me_sim ? 0.0 : hle_me_count_mhz();   /* 0.2-5 s windows (Count wraps in ~9.5 s
                                                    at the PSP-3000 ME's 454 MHz: a 10 s difference read 25 MHz) */
            if (mhz < 1.0) mhz = 0.0;            /* not calibrated / Count did not run - no ME times then */
            char metime[96];
            if (mhz > 0)
                snprintf(metime, sizeof metime, "%.1f ms/s of the ME; hops %.1f ms/s; ME clock %.1f MHz",
                         (double)(c->busy_render - A.w_me_busy_r) / mhz / 1000.0 / real_s,
                         (double)(c->busy_hop - A.w_me_busy_h) / mhz / 1000.0 / real_s, mhz);
            else
                snprintf(metime, sizeof metime, "the ME's own time n/a: its CP0 Count does not run");
            LOG("audio ME %s: %u blocks rendered on the ME (%s); the game thread "
                "waited %.1f ms/s for it (barriers %u, ring full %u, longest %.1f ms); the feeder waited %.1f ms/s for "
                "hops (%u timeouts); SPU RAM writes %u; commands %llu (%llu no-op writes dropped)%s",
                A.me_mode == ZMA_LIVE ? "live" : "shadow", (unsigned)(c->renders - A.w_me_renders), metime,
                (double)(A.z.waits_us - A.w_me_waits) / 1000.0 / real_s, A.z.barriers, A.z.fulls,
                (double)A.z.wait_max_us / 1000.0, (double)(A.fd_me_wait_us - A.w_fd_me_wait) / 1000.0 / real_s,
                A.fd_me_timeouts, (unsigned)c->ramw, (unsigned long long)A.rec.cmds,
                (unsigned long long)A.rec.dropped, A.me_mode == ZMA_SHADOW ? "" : "\n");
            if (A.me_mode == ZMA_SHADOW)
                LOG("; shadow: %u comparisons, %u mismatches%s (main %u blocks, ME %u)\n", A.sh_compared, A.sh_mism,
                    A.sh_mism ? " - FIRST AT THE BLOCK LOGGED ABOVE" : "", A.sh_main_n, (unsigned)c->sh_n);
            if (zma_jobs_count() > 0) {          /* other subsystems' jobs on the same loop */
                char jl[600];
                hle_me_jobs_info(jl, sizeof jl);
                LOG("audio %s\n", jl);
            }
            A.w_me_count = cnt;
            A.w_me_t_us = now;
            A.w_me_busy_r = c->busy_render;
            A.w_me_busy_h = c->busy_hop;
            A.w_me_renders = c->renders;
            A.w_me_waits = A.z.waits_us;
            A.w_fd_me_wait = A.fd_me_wait_us;
        } else {
            LOG("audio ME: off (%s)\n", A.me_state == ZME_OFF ? "waiting for the main loop" :
                A.me_state == ZME_SLEEP ? "parked for sleep, waiting for the wake-up" : A.me_why);
            if (zma_jobs_count() > 0) {
                char jl[600];
                hle_me_jobs_info(jl, sizeof jl);
                LOG("audio %s\n", jl);
            }
        }
    }
    A.w_t0_us = now;
    A.w_vt0_us = vt_us;
    A.w_rpc_us = A.rpc_us;
    A.w_io_us = A.io_us;
    A.w_produced = produced;
    A.w_busy_us = busy;
    A.w_underruns = s->underruns;
    A.w_drops = s->drops;
    A.w_hops_sound = s->hops_sound;
    A.w_unity = s->hops_unity;
    A.w_ssum = s->s_sum;
    A.w_fill = s->fill_sum;
    A.w_frames = A.w_maxms = 0;
    A.w_tmix = A.e->ticks_mixed;
    A.w_tquiet = A.e->ticks_quiet;
}

/* ------------------------------------------------------------------ the RPC (game thread) */
int hle_audio_psp_rpc(uint32_t fno, const void *send, uint32_t ssize, void *recv, uint32_t rsize, uint64_t now_us_v)
{
    uint32_t ms = 0;
    if (fno == 1) {                              /* audio_host.c audio_host_rpc, unchanged */
        if (!A.started) {
            A.started = 1;
            A.last_us = now_us_v;
            A.w_t0_us = now_us();
            A.w_vt0_us = now_us_v;
        }
        uint64_t d = now_us_v > A.last_us ? now_us_v - A.last_us : 0;
        A.w_frames++;
        if (d > 10000000u) d = 10000000u;
        ms = (uint32_t)(d / 1000u);
        A.last_us += (uint64_t)ms * 1000u;
        if (ms > A.w_maxms) A.w_maxms = ms;          /* the largest burst of sound one RPC renders */
        if (now_us_v > A.last_us + 10000000u) A.last_us = now_us_v;
    }
    if (fno == 1 && opt.me) me_service();        /* attach / hand back / shadow compare */
    uint64_t t0 = now_us();
    sndd_rpc(A.a, fno, (const uint8_t *)send, ssize, (uint8_t *)recv, rsize, ms);
    uint64_t t1 = now_us();
    A.rpc_us += t1 - t0;
    A.rpcs++;
    if (fno == 1 && opt.stats_s && t1 - A.w_t0_us >= (uint64_t)opt.stats_s * 1000000u) stats_line("window", t1, now_us_v);
    return 0;
}

/* stop the feeder thread and give the sceAudio channel back - before sceKernelExitGame on every
   exit path (end of run, HOME, crash). Returns 1 = stopped (or nothing ran), 0 = the feeder did not leave within
   0.5 s (it stays: the channel is not released under it). Idempotent. */
int hle_audio_psp_stop(void)
{
    int ok = 1;
    if (A.thid >= 0) {
        A.quit = 1;
        for (int i = 0; i < 100 && A.running; i++) sceKernelDelayThread(5000);
        if (!A.running) { sceKernelDeleteThread(A.thid); A.thid = -1; }
        else ok = 0;
    }
    if (ok && A.ch >= 0) { sceAudioChRelease(A.ch); A.ch = -1; }
    if (A.me_state == ZME_ON || A.me_state == ZME_SLEEP) {   /* stop request, then held in reset */
        int acked = A.me_state == ZME_ON ? me_halt(100) : 1;
        A.me_live_at_stop = A.me_state == ZME_ON && A.me_mode == ZMA_LIVE;   /* the ME had the output at the end */
        if (A.me_state == ZME_ON) A.me_live_us += now_us() - A.me_t0_us;
        A.me_state = ZME_STOPPED;
        zma_jobs_unbind(A.mctl, 0);
        LOG("audio: ME stopped (%s), held in reset\n", acked ? "acknowledged" : "no acknowledgement");
    }
    return ok;
}

void hle_audio_psp_report(void)
{
    if (!A.a) return;
    hle_audio_psp_stop();                        /* stop the feeder, give the channel back */
    dump_close();
    uint64_t now = now_us();
    int me_out = A.me_live_at_stop;              /* the output stage was the ME's at the end */
    if (me_out) A.me_state = ZME_ON;             /* the final window still reads the ME's copies */
    stats_line("final window", now, A.last_us);
    if (me_out) A.me_state = ZME_STOPPED;
    AsStats sv;
    if (me_out) zma_stats(&A.z, &sv);
    else sv = A.st->st;
    const AsStats *s = &sv;
    uint64_t produced = A.produced + (me_out ? A.mctl->produced44 : 0u);
    uint32_t lost = me_out ? A.mctl->ring_lost : A.ring.lost;
    if (opt.me)
        LOG("audio: ME totals: audio_me=%d, attached %d time(s), handed back %d time(s), %.1f s with the ME%s%s; game "
            "thread waited %.0f ms for it (%u barriers, %u ring-full waits); feeder: %u ME hops, %u timeouts\n",
            opt.me, A.me_attaches, A.me_demotes, (double)A.me_live_us / 1e6, A.me_why[0] ? "; last refusal: " : "",
            A.me_why, (double)A.z.waits_us / 1000.0, A.z.barriers, A.z.fulls, A.fd_me_hops, A.fd_me_timeouts);
    LOG("audio: totals: %llu RPCs, model+engine %.0f ms on the game thread (incl. io %.0f ms), %.1f s of sound "
        "produced; output %u blocks, %u underruns, %u drops (%.1f s cut), stretch %.2f..%.2f, peak sample %d; io %u "
        "card reads (%.1f MB; stream cache %u hits / %u misses), %u read errors, %u files not on the card; ring "
        "lost %u frames; feeder waited %.1f s in sceAudio\n", (unsigned long long)A.rpcs, A.rpc_us / 1000.0,
        A.io_us / 1000.0, (double)produced / AS_RATE, A.feed_blocks, s->underruns, s->drops,
        (double)s->frames_dropped / AS_RATE, s->s_min == 0xFFFFFFFFu ? 0.0 : (double)s->s_min / AS_ONE,
        (double)s->s_max / AS_ONE, (int)s->peak, A.io_reads, (double)A.io_bytes / 1e6, A.io_hits, A.io_misses,
        A.io_errors, A.io_missing, lost, A.feed_wait_us / 1e6);
}

#else
typedef int hle_audio_psp_unused;                /* AUDIO=0 build or host: nothing here */
#endif
