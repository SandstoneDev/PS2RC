/* hle_movie.c - a game's movies on the PSP: a PSMF player over the firmware's sceMpeg (the design follows the author's earlier
 * PSP movie player, which runs on the PSP-3000: same call order, same record sizes, same clock rules).
 *
 * Called from the pack's movie hook on the game PSP thread, inside guest thread 1's
 * coroutine: it blocks until the movie ends, then the hook returns to the game exactly as the skip did (v0 = 0).
 *   1. setup: sceUtilityLoadModule AVCODEC + MPEGBASE (unloaded again if loaded here), sceMpegInit, one memory block
 *      from the user partition (top end, freed at the end: work area, a 256-pack ring, the decoded picture, the
 *      sound ring) - nothing of it stays after the movie; <EBOOT dir>/movies/<NAME>.pmf through the platform's file
 *      layer (hle_plat_open/pread: the open-file-limit recovery of hle_psp.c), its PSMF header (stream offset/size,
 *      first/last 90 kHz timestamps), the AVC (0) and ATRAC3plus (1) streams registered.
 *   2. loop: packs into the ring (the ring callback reads the card on this thread), ATRAC access units decoded into
 *      2048-frame stereo blocks for a sceAudio channel fed by a small high-priority thread (the sound is the clock),
 *      AVC access units decoded by the Media Engine into the RAM picture (cache cleaned BEFORE each decode: the ME
 *      writes behind the CPU's cache), shown when the clock reaches its PTS through the GE renderer's display buffers
 *      (hle_ge_movie_frame: 8888 drawn as a texture into the 5650 buffer, or 565 block-copied); a picture more than
 *      2 frames late is dropped (at most 4 in a row). X skips (the caller's rule: unskippable_bytes).
 *   3. the end: sceMpegAvcDecodeStop hands out the held picture, the sound plays out (<= 3 s), a black frame, the
 *      teardown in reverse order. 5 s without any progress = failure (logged, the game goes on).
 * Facts the code relies on (measured on a PSP-3000 with that earlier player; pspsdk pspmpeg.h has the smaller records): the firmware's ring / AU / decode-mode
 * records are larger than pspsdk's (32 words / 64 B / 16 B: zeroed records of the real size around pspsdk's structs);
 * sceMpegAvcDecode's buffer argument is the address of a pointer, its last argument says whether a picture was written
 * (not necessarily the AU just given: PTS queue); 0x80618001 = no whole AU in the ring; the first sceMpegAtracDecode
 * (init 1) primes the decoder (its block is dropped); every sceMpeg call takes the address of the SceMpeg handle
 * (pspsdk declares sceMpegUnRegistStream with the handle by value - the firmware takes the address).
 * Virtual time stands still during the movie (no guest code runs); hle_progress is bumped (watchdog) and the pacing
 * anchor moves by the movie's real time (hle_plat_pace_shift), so the boot after it behaves as without the movie.
 * A PMF whose header lists no ATRAC3plus stream (the open converter ps2rc/movies/convert_movies_open.py + pmfmux.py:
 * video only) takes its sound from <NAME>.wav next to it (16-bit PCM or IMA ADPCM, 44.1 kHz, hle_wavsrc.c) through the
 * same sound ring / feeder / clock; the run option movie_dir=<dir> plays the movies of that folder only. A PMF with ATRAC3plus plays exactly as before. */
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspmpeg.h>
#include <psputility.h>
#include <psppower.h>
#include <stdlib.h>
#include <ctype.h>
#include "hle.h"
/* the display: the game pack's GE back-end (its render boundary), if any - without one no movie is played */
extern int hle_ge_on __attribute__((weak));
int hle_ge_movie_begin(void) __attribute__((weak));
void hle_ge_movie_frame(const void *pic, int rgba8888, uint32_t dump) __attribute__((weak));
void hle_ge_movie_end(void) __attribute__((weak));
#include "hle_cfg.h"                         /* the shared run-options table */
#include "hle_movie.h"
#include "hle_mem_psp.h"                      /* mem_volatile=1 */
#include "hle_wavsrc.h"                     /* the sound of a video-only PMF from <NAME>.wav next to it */

#define PACK 2048u                  /* the PSMF stream's unit (an MPEG-2 program stream pack) */
#define STRIDE 512                  /* the decoded picture's width in pixels (480 used) */
#define PIC_H 272
#define FRAME_TICKS 3003u           /* 90 kHz ticks a frame at 29.97 fps (the PSMF Encoder's one rate) */
#define ERR_NO_DATA 0x80618001u     /* sceMpegGet*Au: no whole access unit in the ring */
#define ERR_LOADED 0x80111102u      /* sceUtilityLoadModule: already loaded */
#define RING_PACKS 256              /* 512 KB of stream: ~2.5 s at 1.6 Mbit/s (spans the muxer's 1 s video lead) */
#define PUT_MIN 32                  /* read when this many packs are free (one card request ~1 ms whatever its size) */
#define PUT_MAX 64
#define AUD_FRAMES 2048             /* one ATRAC3plus block: 2048 stereo s16 frames at 44.1 kHz */
#define AUD_BYTES (AUD_FRAMES * 4)
#define AUD_SLOTS 16                /* 0.74 s of sound ring; one slot kept free for the block the DAC still reads */
#define AUD_LEAD_BLOCKS 12          /* decode the sound at most ~0.55 s ahead */
#define AUD_RATE 44100u
#define ATRAC_MAX 16384u            /* room for the ATRAC ES + output buffers (PPSSPP: 2112 + 8192) */

/* the firmware's records around pspsdk's smaller declarations */
typedef union { SceMpegRingbuffer rb; uint32_t raw[32]; } MpegRing;
typedef union { SceMpegAu au; uint8_t raw[64]; } MpegAu;
typedef struct { SceInt32 mode, psm; uint8_t reserved[8]; } MpegAvcMode;
_Static_assert(sizeof(MpegRing) == 128 && sizeof(MpegAu) == 64 && sizeof(MpegAvcMode) == 16, "libmpeg record sizes");

/* ------------------------------------------------------------------ options (<EBOOT dir>/poc6.cfg) */
static int cfg_read, opt_movies = 1, opt_8888 = 1;
static int opt_skip = 1;                  /* movie_skip=0: X does not skip (unattended runs: PPSSPP takes keys / pads) */
static uint32_t opt_dump[16];             /* movie_dump=T,T,...: T = movie number x 10000 + picture (1-based); writes
                                            movie_disp_<T>.bmp of the displayed buffer - PPSSPP checks, never the card */
static int opt_ndump, movie_seq;
static uint32_t opt_test_skip[4];        /* movie_test_skip=T,...: an X press simulated at picture T (same tags; tests) */
static int opt_ntest_skip;
/* movie_mem=hole: the buffers the Media Engine, the GE and the audio DMA touch (picture, sound slots, ATRAC) come from
   the guest code hole (low RAM, < 0x0A000000; taken once, kept: ~690 KB of the hole) instead of the user partition
   (above the guest window: 0x0B... on the PSP-3000) - a fallback if a device shows no picture / no sound */
static int opt_mem_hole;
static char opt_data_dir[256];             /* run option disc_dir (hle_psp.c): the game data's folder - the
                                            movies are looked for in <disc_dir>movies/ first, then <EBOOT dir>movies/ */
static char opt_movie_dir[256];            /* movie_dir=<dir> - the movies only from there (a path with ':' as
                                            is, else relative to the EBOOT folder), e.g. a test folder's own set */
static uint8_t *hole_buf;

static void read_cfg(void)
{
    if (cfg_read) return;
    cfg_read = 1;
    for (int ci = 0; ci < hle_cfg_count(); ci++) {    /* the shared table (hle_cfg.c), whole file */
        {
            const char *k = hle_cfg_key(ci), *v = hle_cfg_val(ci);
            if (!strcmp(k, "movies")) opt_movies = atoi(v);
            else if (!strcmp(k, "movie_skip")) opt_skip = atoi(v);
            else if (!strcmp(k, "movie_psm")) opt_8888 = strcmp(v, "5650") != 0 && strcmp(v, "565") != 0;
            else if (!strcmp(k, "movie_mem")) opt_mem_hole = !strcmp(v, "hole");
            else if (!strcmp(k, "disc_dir") && *v && strlen(v) < sizeof opt_data_dir - 1)
                snprintf(opt_data_dir, sizeof opt_data_dir, "%s%s", v, v[strlen(v) - 1] == '/' ? "" : "/");
            else if (!strcmp(k, "movie_dir") && *v && strlen(v) + strlen(hle_plat_disc_root()) < sizeof opt_movie_dir - 1)
                snprintf(opt_movie_dir, sizeof opt_movie_dir, "%s%s%s", strchr(v, ':') ? "" : hle_plat_disc_root(), v,
                         v[strlen(v) - 1] == '/' ? "" : "/");
            else if (!strcmp(k, "movie_dump") || !strcmp(k, "movie_test_skip")) {
                int skip = k[6] == 't';
                uint32_t *dst = skip ? opt_test_skip : opt_dump;
                int *n = skip ? &opt_ntest_skip : &opt_ndump, max = skip ? 4 : 16;
                for (const char *q = v; *q && *n < max;) {
                    unsigned long t = strtoul(q, (char **)&q, 10);
                    if (t) dst[(*n)++] = (uint32_t)t;
                    while (*q && (*q < '0' || *q > '9')) q++;
                }
            }
        }
    }
}

static inline uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ------------------------------------------------------------------ the sound: a ring of decoded blocks + a feeder
   thread that only hands them to the DAC (sceAudioOutputPannedBlocking returns when the channel took the block) */
static struct {
    int16_t *slots;                   /* AUD_SLOTS x AUD_FRAMES stereo frames */
    volatile uint32_t wr, rd;         /* blocks pushed / handed to the DAC */
    volatile uint64_t rd_us;          /* system time of the last hand-over */
    volatile int quit, running;
    volatile uint32_t underruns;
    int ch;
} aud;

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int dry = 0;
    while (!aud.quit) {
        if (aud.rd == aud.wr) {
            if (!dry && aud.rd) aud.underruns++;
            dry = 1;
            sceKernelDelayThread(2000);
            continue;
        }
        dry = 0;
        sceAudioOutputPannedBlocking(aud.ch, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX,
                                     aud.slots + (aud.rd % AUD_SLOTS) * AUD_FRAMES * 2);
        aud.rd_us = now_us();
        aud.rd = aud.rd + 1;
    }
    aud.running = 0;
    return 0;
}

/* ------------------------------------------------------------------ the movie */
typedef struct {
    int h;                            /* platform file handle of the PMF */
    uint32_t pos, end;                /* next pack / end of the stream (file offsets) */
    uint32_t reads, read_fail;
} Reader;

/* sceMpegRingbufferPut's callback (this thread): packs from the card straight into the ring */
static SceInt32 ring_fill(ScePVoid data, SceInt32 packets, ScePVoid param)
{
    Reader *r = param;
    uint32_t left = r->end > r->pos ? r->end - r->pos : 0u;
    uint32_t bytes = (uint32_t)packets * PACK;
    if (bytes > left) bytes = left;
    bytes -= bytes % PACK;
    if (!bytes) return 0;
    if (hle_plat_pread(r->h, r->pos, data, bytes) != (int64_t)bytes) { r->read_fail++; return -1; }
    r->pos += bytes;
    r->reads++;
    return (SceInt32)(bytes / PACK);
}

typedef struct {
    Reader rd;
    int avcodec_load, mpeg_load, inited, ring_built, created, decoded_any, decode_stopped;
    SceUID blk, athr;
    uint8_t *mem;                     /* the partition block: work area | ring | picture | sound slots */
    uint8_t *vol;                     /* the same block from the volatile partition (mem_volatile=1) */
    uint32_t vol_bytes;
    void *work, *ring_mem, *pic, *avc_es;
    uint8_t *atrac_es, *pcm;
    int work_size, ring_bytes, es_size, out_size;
    MpegRing ring;
    SceMpeg mpeg;
    SceMpegStream *avc, *atrac;
    MpegAu avc_au, atrac_au;
    int sound;                        /* the sound channel plays */
    uint32_t first_ts, last_ts;
    const char *fail_step;
    int fail_code;
    /* a PMF without an ATRAC3plus stream (ps2rc/movies/pmfmux.py) takes its sound from <NAME>.wav in the same folder
       (16-bit PCM or IMA ADPCM, 44.1 kHz; ps2rc/movies/convert_movies_open.py), read in 32 KB pieces */
    int has_atrac, side, side_h, side_err;
    WavSrc wav;
} Movie;

#define SIDE_STAGE 32768u           /* the .wav sound's staging buffer: one card read per ~0.19 s (PCM) / ~0.74 s (IMA) */

static int64_t side_read(void *ctx, uint64_t off, void *dst, uint32_t n)
{
    return hle_plat_pread((int)(intptr_t)ctx, off, dst, n);
}

static uint8_t header[PACK] __attribute__((aligned(64)));

static int fail(Movie *m, const char *step, int code)
{
    m->fail_step = step;
    m->fail_code = code;
    return 0;
}

static int movie_open(Movie *m, int64_t fsize, const char *side_path)
{
    if (fsize < 2 * (int64_t)PACK || hle_plat_pread(m->rd.h, 0, header, PACK) != PACK || memcmp(header, "PSMF", 4))
        return fail(m, "header", (int)fsize);
    m->first_ts = be32(header + 0x56);
    m->last_ts = be32(header + 0x5C);
    /* the stream table (u16 count at 0x80, 16-byte entries from 0x82: stream id, private id) - an
       ATRAC3plus stream is 0xBD / 0x0n; without one the sound comes from <NAME>.wav if that file exists */
    uint32_t nstreams = (uint32_t)header[0x80] << 8 | header[0x81];
    for (uint32_t i = 0; i < nstreams && 0x82 + 16 * (i + 1) <= PACK; i++)
        if (header[0x82 + 16 * i] == 0xBD && (header[0x83 + 16 * i] & 0xF0) == 0) m->has_atrac = 1;
    if (!m->has_atrac && side_path) {
        m->side_h = hle_plat_open(side_path);
        m->side = m->side_h >= 0;
    }
    m->avcodec_load = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (m->avcodec_load < 0 && (uint32_t)m->avcodec_load != ERR_LOADED) return fail(m, "avcodec", m->avcodec_load);
    m->mpeg_load = sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    if (m->mpeg_load < 0 && (uint32_t)m->mpeg_load != ERR_LOADED) return fail(m, "mpegbase", m->mpeg_load);
    int r = sceMpegInit();
    if (r < 0) return fail(m, "init", r);
    m->inited = 1;
    m->work_size = sceMpegQueryMemSize(0);
    if (m->work_size <= 0) return fail(m, "mem_size", m->work_size);
    m->ring_bytes = sceMpegRingbufferQueryMemSize(RING_PACKS);
    if (m->ring_bytes <= 0) return fail(m, "ring_size", m->ring_bytes);
    /* one block from the top of the user partition: nothing below it moves, all of it goes back at the end
       (movie_mem=hole: picture + sound slots + ATRAC buffers from the hole instead, taken once and kept) */
    uint32_t a = ((uint32_t)m->work_size + 63u) & ~63u, b = ((uint32_t)m->ring_bytes + 63u) & ~63u;
    uint32_t pic_bytes = STRIDE * PIC_H * (opt_8888 ? 4u : 2u);
    uint32_t dma_bytes = STRIDE * PIC_H * 4u + AUD_SLOTS * AUD_BYTES + ATRAC_MAX;   /* the 8888 size: either mode */
    uint32_t total = a + b + (opt_mem_hole ? 0u : dma_bytes) + (m->side ? SIDE_STAGE : 0u) + 64u;
    /* mem_volatile=1 - the block from the volatile partition (its last block,
       given back at the end); else (or when it does not fit) the user partition as before */
    m->vol = hle_psp_mem_volatile() ? hle_mem_vol_take(total, 0) : NULL;
    if (m->vol) {
        m->vol_bytes = total;
        m->mem = m->vol;                              /* 64-byte aligned */
    } else {
        m->blk = sceKernelAllocPartitionMemory(2, "hle_movie", PSP_SMEM_High, total, NULL);
        if (m->blk < 0) return fail(m, "oom", (int)m->blk);
        m->mem = (uint8_t *)(((uintptr_t)sceKernelGetBlockHeadAddr(m->blk) + 63u) & ~(uintptr_t)63u);
    }
    m->work = m->mem;
    m->ring_mem = m->mem + a;
    uint8_t *dma = m->mem + a + b;
    if (m->side) {                                     /* the .wav staging buffer (CPU only) at the block's end */
        int r = wavsrc_open(&m->wav, side_read, (void *)(intptr_t)m->side_h, (uint64_t)hle_plat_size(m->side_h),
                            m->mem + a + b + (opt_mem_hole ? 0u : dma_bytes), SIDE_STAGE);
        if (r == WAVSRC_OK && m->wav.rate != AUD_RATE) r = WAVSRC_E_FORMAT;
        if (r != WAVSRC_OK) { m->side = 0; m->side_err = r; }
    }
    if (opt_mem_hole) {
        if (!hole_buf) hole_buf = hle_plat_big_alloc(dma_bytes);   /* 64-byte aligned, zeroed; dies when full */
        dma = hole_buf;
    }
    m->pic = dma;
    aud.slots = (int16_t *)(dma + pic_bytes);
    m->atrac_es = dma + pic_bytes + AUD_SLOTS * AUD_BYTES;
    memset(m->work, 0, a + b);
    memset(aud.slots, 0, AUD_SLOTS * AUD_BYTES);
    sceKernelDcacheWritebackInvalidateRange(m->mem, a + b);        /* no dirty line over memory the ME writes */
    sceKernelDcacheWritebackInvalidateRange(dma, dma_bytes);
    r = sceMpegRingbufferConstruct(&m->ring.rb, RING_PACKS, m->ring_mem, m->ring_bytes, ring_fill, &m->rd);
    if (r < 0) return fail(m, "ring", r);
    m->ring_built = 1;
    r = sceMpegCreate(&m->mpeg, m->work, m->work_size, &m->ring.rb, STRIDE, 0, 0);
    if (r < 0) return fail(m, "create", r);
    m->created = 1;
    MpegAvcMode mode = {-1, opt_8888 ? SCE_MPEG_AVC_FORMAT_8888 : SCE_MPEG_AVC_FORMAT_5650, {0}};
    r = sceMpegAvcDecodeMode(&m->mpeg, (SceMpegAvcMode *)&mode);
    if (r < 0) return fail(m, "decode_mode", r);
    SceInt32 offset = 0, ssize = 0;
    r = sceMpegQueryStreamOffset(&m->mpeg, header, &offset);
    if (r < 0 || offset < (SceInt32)PACK) return fail(m, "stream_offset", r ? r : (int)offset);
    r = sceMpegQueryStreamSize(header, &ssize);
    if (r < 0 || ssize <= 0) return fail(m, "stream_size", r ? r : (int)ssize);
    m->rd.pos = (uint32_t)offset;
    m->rd.end = (uint32_t)((int64_t)offset + ssize < fsize ? (int64_t)offset + ssize : fsize);
    m->avc = sceMpegRegistStream(&m->mpeg, 0, 0);
    if (!m->avc) return fail(m, "regist_avc", 0);
    m->avc_es = sceMpegMallocAvcEsBuf(&m->mpeg);
    if (!m->avc_es) return fail(m, "avc_es", 0);
    r = sceMpegInitAu(&m->mpeg, m->avc_es, &m->avc_au.au);
    if (r < 0) return fail(m, "avc_au", r);
    /* an ATRAC3plus stream only when the header lists one (a registered stream that never comes would hold
       the ring: it frees only what every registered stream has passed) */
    m->atrac = m->has_atrac ? sceMpegRegistStream(&m->mpeg, 1, 0) : NULL;
    if (m->atrac) {
        SceInt32 es = 0, out = 0;
        r = sceMpegQueryAtracEsSize(&m->mpeg, &es, &out);
        uint32_t es_a = ((uint32_t)es + 63u) & ~63u;
        if (r < 0 || es <= 0 || out <= 0 || es_a + (uint32_t)out > ATRAC_MAX || out > AUD_BYTES)
            return fail(m, "atrac_size", r < 0 ? r : (int)out);
        m->es_size = (int)es;
        m->out_size = (int)out;
        m->pcm = m->atrac_es + es_a;
        r = sceMpegInitAu(&m->mpeg, m->atrac_es, &m->atrac_au.au);
        if (r < 0) return fail(m, "atrac_au", r);
    }
    if (m->atrac || m->side) {
        /* the sound channel + its feeder thread; without them the access units are still taken and dropped (the
           ring frees only what every registered stream has passed) */
        aud.ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUD_FRAMES, PSP_AUDIO_FORMAT_STEREO);
        if (aud.ch >= 0) {
            aud.wr = aud.rd = 0;
            aud.rd_us = 0;
            aud.quit = 0;
            aud.underruns = 0;
            aud.running = 1;
            m->athr = sceKernelCreateThread("hle_movie_audio", audio_thread, 0x10, 0x4000, THREAD_ATTR_USER, NULL);
            if (m->athr >= 0 && sceKernelStartThread(m->athr, 0, NULL) >= 0) m->sound = 1;
            else {
                aud.running = 0;
                LOG("movie: sound thread failed (%08x) - the picture plays on the system clock\n", (unsigned)m->athr);
            }
        } else
            LOG("movie: no sceAudio channel (%08x) - the picture plays on the system clock\n", (unsigned)aud.ch);
    }
    return 1;
}

/* reverse order of movie_open (Sony's samples: decoder stop, flush, free the ES buffer, unregister, delete, destruct
   the ring, finish, unload) */
static void movie_close(Movie *m)
{
    if (m->athr >= 0) {
        aud.quit = 1;
        SceUInt t = 500000;
        sceKernelWaitThreadEnd(m->athr, &t);
        sceKernelDeleteThread(m->athr);
        m->athr = -1;
    }
    if (aud.ch >= 0) {                                     /* let the DAC finish its last block(s), then release */
        for (int i = 0; i < 40 && sceAudioGetChannelRestLen(aud.ch) > 0; i++) sceKernelDelayThread(5000);
        int r = sceAudioChRelease(aud.ch);
        if (r < 0) LOG("movie: sceAudioChRelease(%d) = %08x\n", aud.ch, (unsigned)r);
        aud.ch = -1;
    }
    m->sound = 0;
    if (m->created) {
        if (m->decoded_any && !m->decode_stopped) {        /* skipped: the decoder gives up the picture it holds */
            void *bufs[1] = {m->pic};
            SceInt32 got = 0;
            sceKernelDcacheWritebackInvalidateRange(m->pic, STRIDE * PIC_H * (opt_8888 ? 4u : 2u));
            sceMpegAvcDecodeStop(&m->mpeg, STRIDE, bufs, &got);
            m->decode_stopped = 1;
        }
        sceMpegFlushAllStream(&m->mpeg);
        if (m->avc_es) sceMpegFreeAvcEsBuf(&m->mpeg, m->avc_es);
        if (m->avc) sceMpegUnRegistStream((SceMpeg)&m->mpeg, m->avc);       /* Sony: SceMpeg *mp */
        if (m->atrac) sceMpegUnRegistStream((SceMpeg)&m->mpeg, m->atrac);
        sceMpegDelete(&m->mpeg);
        m->created = 0;
    }
    m->avc_es = NULL;
    m->avc = m->atrac = NULL;
    if (m->ring_built) { sceMpegRingbufferDestruct(&m->ring.rb); m->ring_built = 0; }
    if (m->inited) { sceMpegFinish(); m->inited = 0; }
    if (m->blk >= 0) { sceKernelFreePartitionMemory(m->blk); m->blk = -1; }
    if (m->vol) {
        if (!hle_mem_vol_give_back(m->vol, m->vol_bytes)) LOG("movie: volatile block NOT given back\n");
        m->vol = NULL;
    }
    aud.slots = NULL;
    if (m->mpeg_load >= 0) sceUtilityUnloadModule(PSP_MODULE_AV_MPEGBASE);
    if (m->avcodec_load >= 0) sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    m->mpeg_load = m->avcodec_load = -1;
}

/* the movie's time in 90 kHz ticks: while the sound plays, the position of the block the DAC is on (the block handed
   before the last one; between hand-overs interpolated, at most one block) from the first pushed block's PTS; when the
   sound runs dry (card / demuxer late - the picture must go on, or a ring full of pictures waiting for the clock never
   lets the sound arrive) and after it ended, the system clock on from the last value */
static int audio_anchored;
static uint32_t audio_pts0, hold;
static uint64_t hold_us;

static uint32_t movie_clock(int sound)
{
    uint32_t rd = aud.rd;
    if (sound && audio_anchored && rd >= 2 && aud.wr > rd) {
        uint64_t frames = (uint64_t)(rd - 2) * AUD_FRAMES;
        uint64_t since = now_us() - aud.rd_us;
        uint64_t extra = since * AUD_RATE / 1000000u;
        frames += extra < AUD_FRAMES ? extra : AUD_FRAMES;
        hold = audio_pts0 + (uint32_t)(frames * 90000u / AUD_RATE);
        hold_us = now_us();
        return hold;
    }
    if (!hold_us) return hold;
    return hold + (uint32_t)((now_us() - hold_us) * 9u / 100u);
}

enum { END_FINISHED, END_SKIPPED, END_FAILED };
static const char *const end_names[] = {"finished", "skipped (X)", "failed"};

int hle_movie_play(const char *guest_path, uint32_t skippable, uint32_t unskip_bytes)
{
    read_cfg();
    char gpath[96], name[40];
    snprintf(gpath, sizeof gpath, "%s", guest_path ? guest_path : "");
    const char *b = gpath;
    for (const char *p = gpath; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    size_t k = 0;
    for (; b[k] && b[k] != '.' && b[k] != ';' && k + 1 < sizeof name; k++) name[k] = (char)toupper((unsigned char)b[k]);
    name[k] = 0;
    if (!opt_movies) { LOG("movie %s: not played (poc6.cfg movies=0)\n", gpath); return 0; }
    if (!&hle_ge_on || !hle_ge_on) { LOG("movie %s: not played (render=text: no GE display)\n", gpath); return 0; }
    if (!k) return 0;
    char path[320];
    if (opt_movie_dir[0]) snprintf(path, sizeof path, "%s%s.pmf", opt_movie_dir, name);   /* movie_dir: only there */
    else snprintf(path, sizeof path, "%smovies/%s.pmf", opt_data_dir[0] ? opt_data_dir : hle_plat_disc_root(), name);
    uint64_t t0 = now_us();
    movie_seq++;
    unsigned free0 = (unsigned)(sceKernelTotalFreeMemSize() >> 10), maxb0 = (unsigned)(sceKernelMaxFreeMemSize() >> 10);
    int h = hle_plat_open(path);
    if (h < 0 && opt_data_dir[0] && !opt_movie_dir[0]) {  /* disc_dir set: the EBOOT's own folder as well */
        snprintf(path, sizeof path, "%smovies/%s.pmf", hle_plat_disc_root(), name);
        h = hle_plat_open(path);
    }
    if (h < 0) {
        LOG("movie %s: no %s - skipped (tests/movies/convert_movies.py makes it)\n", gpath, path);
        hle_plat_pace_shift(now_us() - t0);
        return 0;
    }
    int64_t fsize = hle_plat_size(h);
    uint32_t lsn = 0, pss_size = 0;
    hle_disc_lookup(gpath, &lsn, &pss_size);

    Movie m;
    memset(&m, 0, sizeof m);
    m.rd.h = h;
    m.blk = m.athr = -1;
    m.avcodec_load = m.mpeg_load = -1;
    m.side_h = -1;
    aud.ch = -1;
    audio_anchored = 0;
    hold = 0;
    hold_us = 0;
    int end = END_FINISHED, shown = 0;
    uint32_t dropped = 0, decoded = 0, audio_aus = 0, presses_ignored = 0;
    unsigned free1 = 0;
    char side_path[320];                                   /* <the PMF's folder>/<NAME>.wav */
    snprintf(side_path, sizeof side_path, "%.*s.wav", (int)(strlen(path) - 4), path);
    if (!movie_open(&m, fsize, side_path) || !hle_ge_movie_begin()) {
        if (!m.fail_step) m.fail_step = "ge";
        end = END_FAILED;
    } else {
        free1 = (unsigned)(sceKernelTotalFreeMemSize() >> 10);
        /* the PS2 rule (the caller's): with skippable == 0 the movie becomes skippable once the bytes still to read
           drop below (file size - unskippable_bytes), i.e. after that many bytes of the PSS; here the same share of
           the movie's playing time (the PMF reader runs up to a 512 KB ring ahead of the picture: its position would
           allow the skip far too early - at a fifth of the movie in an emulator test) */
        uint32_t span = m.last_ts - m.first_ts, skip_from = 0, shown_pts = m.first_ts;
        if (!skippable)
            skip_from = pss_size > unskip_bytes ? (uint32_t)((uint64_t)span * unskip_bytes / pss_size)
                                                : (pss_size ? span + 1u : 0u);
        char snd[160];                                     /* where the sound comes from */
        if (m.has_atrac) snprintf(snd, sizeof snd, "%s", m.sound ? "on" : m.atrac ? "dropped (no channel)" : "none");
        else if (m.side)
            snprintf(snd, sizeof snd, "%s from %s (%s, %u Hz, %u ch, %.1f s)", m.sound ? "on" : "dropped (no channel)",
                     side_path, wavsrc_format_name(&m.wav), (unsigned)m.wav.rate, (unsigned)m.wav.channels,
                     (double)m.wav.total_frames / m.wav.rate);
        else if (m.side_err)
            snprintf(snd, sizeof snd, "none (%s unusable: error %d, rate %u)", side_path, m.side_err,
                     (unsigned)m.wav.rate);
        else snprintf(snd, sizeof snd, "none (no ATRAC3plus stream, no %s)", side_path);
        LOG("movie %s: %s %lld KB, %.1f s, ring %d packs (%d KB), work %d KB, picture %s at %p (%s), sound %s, "
            "skippable %s, free %u KB (largest %u) -> %u KB\n", gpath, path, (long long)(fsize >> 10),
            (double)(m.last_ts - m.first_ts) / 90000.0, RING_PACKS, m.ring_bytes >> 10, m.work_size >> 10,
            opt_8888 ? "8888" : "565", m.pic, opt_mem_hole ? "hole" : m.vol ? "volatile partition" : "partition", snd,
            skippable ? "at once" : skip_from ? "after the first part" : "no", free0, maxb0, free1);
        if (skip_from) LOG("movie %s: X skips from %.1f s on (PS2: after %u of the PSS's %u bytes)\n", gpath,
                           (double)skip_from / 90000.0, (unsigned)unskip_bytes, (unsigned)pss_size);
        int read_done = 0, video_done = 0, stopped = 0, frame_pending = 0, atrac_init = 1;
        int audio_done = !m.atrac && !(m.side && m.sound);
        uint32_t frame_pts = 0, last_au_pts = m.first_ts - FRAME_TICKS, drop_run = 0;
        uint32_t ptsq[16];
        int qh = 0, qn = 0;
        uint64_t last_progress = now_us(), no_data_us = 0, drain_us = 0, power_us = 0;
        uint32_t last_rd = 0;
        hold = m.first_ts;
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t prev_buttons = pad.Buttons;               /* a button already held does not skip */
        uint32_t test_fired = 0;
        for (;;) {
            hle_progress++;                                /* the watchdog: the game thread is alive */
            uint64_t tnow = now_us();
            sceCtrlPeekBufferPositive(&pad, 1);
            uint32_t pressed = pad.Buttons & ~prev_buttons;
            prev_buttons = pad.Buttons;
            uint32_t tag = (uint32_t)movie_seq * 10000u + (uint32_t)shown;
            for (int i = 0; i < opt_ntest_skip; i++)       /* movie_test_skip=T: a simulated X press at picture T */
                if (opt_test_skip[i] == tag && test_fired != tag) { pressed |= PSP_CTRL_CROSS; test_fired = tag; }
            if ((pressed & PSP_CTRL_CROSS) && !opt_skip && test_fired != tag) { presses_ignored++; pressed = 0; }
            if (pressed & PSP_CTRL_CROSS) {
                if (skippable || shown_pts - m.first_ts >= skip_from) { end = END_SKIPPED; break; }
                presses_ignored++;
            }
            if (tnow >= power_us) { scePowerTick(PSP_POWER_TICK_ALL); power_us = tnow + 1000000u; }
            int worked = 0;
            /* 1. the stream into the ring: a read when 32+ packs are free */
            if (!read_done) {
                int avail = sceMpegRingbufferAvailableSize(&m.ring.rb);
                if (avail >= PUT_MIN || (avail > 0 && m.rd.end - m.rd.pos < PUT_MIN * PACK)) {
                    int put = sceMpegRingbufferPut(&m.ring.rb, avail < PUT_MAX ? avail : PUT_MAX, avail);
                    if (put < 0 || m.rd.read_fail) { fail(&m, "read", put < 0 ? put : (int)m.rd.read_fail); end = END_FAILED; break; }
                    worked |= put > 0;
                }
                read_done = m.rd.pos >= m.rd.end;
            }
            int ring_empty = sceMpegRingbufferAvailableSize(&m.ring.rb) >= RING_PACKS;
            /* 2. the sound: access units while the sound ring has room, at most ~0.55 s ahead; without the channel
               they are taken and dropped */
            for (int n = 0; m.atrac && !audio_done && n < 6; n++) {
                if (m.sound && (aud.wr - aud.rd >= AUD_LEAD_BLOCKS || aud.wr - aud.rd >= AUD_SLOTS - 2)) break;
                void *body = NULL;
                int r = sceMpegGetAtracAu(&m.mpeg, m.atrac, &m.atrac_au.au, &body);
                if ((uint32_t)r == ERR_NO_DATA) { if (read_done && ring_empty) audio_done = 1; break; }
                if (r < 0) { LOG("movie: sceMpegGetAtracAu %08x - the rest without sound\n", (unsigned)r); audio_done = 1; break; }
                audio_aus++;
                worked = 1;
                if (!m.sound) continue;
                sceKernelDcacheWritebackInvalidateRange(m.pcm, (unsigned)m.out_size);
                int d = sceMpegAtracDecode(&m.mpeg, &m.atrac_au.au, m.pcm, atrac_init ? 1 : 0);
                if (d < 0) {
                    LOG("movie: sceMpegAtracDecode %08x - the rest without sound\n", (unsigned)d);
                    aud.quit = 1;                          /* the feeder ends; the clock goes on from the system timer */
                    m.sound = 0;
                    continue;
                }
                if (!atrac_init) {                         /* the init call's block is dropped */
                    if (!audio_anchored) {
                        audio_pts0 = m.atrac_au.au.iPts != 0xFFFFFFFFu ? m.atrac_au.au.iPts : m.first_ts;
                        audio_anchored = 1;
                    }
                    int16_t *slot = aud.slots + (aud.wr % AUD_SLOTS) * AUD_FRAMES * 2;
                    memcpy(slot, m.pcm, (size_t)m.out_size);
                    if (m.out_size < AUD_BYTES) memset((uint8_t *)slot + m.out_size, 0, AUD_BYTES - (size_t)m.out_size);
                    sceKernelDcacheWritebackRange(slot, AUD_BYTES);
                    __sync_synchronize();
                    aud.wr = aud.wr + 1;                   /* publish last */
                }
                atrac_init = 0;
            }
            /* 2b. the .wav sound of a video-only PMF - blocks of 2048 frames into the same ring, the same
               lead; its first frame is the first picture's time (the header's first timestamp) */
            for (int n = 0; m.side && !audio_done && n < 6; n++) {
                if (aud.wr - aud.rd >= AUD_LEAD_BLOCKS || aud.wr - aud.rd >= AUD_SLOTS - 2) break;
                int16_t *slot = aud.slots + (aud.wr % AUD_SLOTS) * AUD_FRAMES * 2;
                int got = wavsrc_read(&m.wav, slot, AUD_FRAMES);
                if (got < 0) {
                    LOG("movie: %s read error %d - the rest without sound\n", side_path, got);
                    aud.quit = 1;                          /* the feeder ends; the clock goes on from the system timer */
                    m.sound = 0;
                    audio_done = 1;
                    break;
                }
                if (got > 0) {
                    if (got < AUD_FRAMES) memset(slot + 2 * got, 0, (size_t)(AUD_FRAMES - got) * 4u);
                    if (!audio_anchored) { audio_pts0 = m.first_ts; audio_anchored = 1; }
                    sceKernelDcacheWritebackRange(slot, AUD_BYTES);
                    __sync_synchronize();
                    aud.wr = aud.wr + 1;                   /* publish last */
                    audio_aus++;
                    worked = 1;
                }
                if (got < AUD_FRAMES) { audio_done = 1; break; }
            }
            /* 3. the picture: the next access unit decoded into the RAM picture */
            if (!video_done && !frame_pending) {
                void *bufs[1] = {m.pic};
                SceInt32 got = 0;
                if (stopped) video_done = 1;
                else {
                    SceInt32 attr = 0;
                    int r = sceMpegGetAvcAu(&m.mpeg, m.avc, &m.avc_au.au, &attr);
                    if ((uint32_t)r == ERR_NO_DATA) {
                        if (!no_data_us) no_data_us = tnow;
                        /* the end: everything read and demuxed (or nothing for 2 s after the last read) - the decoder
                           hands out the picture it still holds */
                        if (read_done && (ring_empty || tnow - no_data_us > 2000000u)) {
                            sceKernelDcacheWritebackInvalidateRange(m.pic, STRIDE * PIC_H * (opt_8888 ? 4u : 2u));
                            int s = m.decoded_any ? sceMpegAvcDecodeStop(&m.mpeg, STRIDE, bufs, &got) : 0;
                            m.decode_stopped = 1;
                            stopped = 1;
                            if (s >= 0 && got > 0 && qn > 0) {
                                frame_pending = 1;
                                frame_pts = ptsq[qh];
                                qh = (qh + 1) % 16;
                                qn--;
                            } else
                                video_done = 1;
                        }
                    } else if (r < 0) {
                        fail(&m, "avc_au", r);
                        end = END_FAILED;
                        break;
                    } else {
                        no_data_us = 0;
                        sceKernelDcacheWritebackInvalidateRange(m.pic, STRIDE * PIC_H * (opt_8888 ? 4u : 2u));
                        int d = sceMpegAvcDecode(&m.mpeg, &m.avc_au.au, STRIDE, bufs, &got);
                        if (d < 0) { fail(&m, "avc_decode", d); end = END_FAILED; break; }
                        m.decoded_any = 1;
                        decoded++;
                        uint32_t pts = m.avc_au.au.iPts != 0xFFFFFFFFu ? m.avc_au.au.iPts : last_au_pts + FRAME_TICKS;
                        last_au_pts = pts;
                        if (qn == 16) { qh = (qh + 1) % 16; qn--; }
                        ptsq[(qh + qn) % 16] = pts;
                        qn++;
                        if (got > 0) {
                            frame_pending = 1;
                            frame_pts = ptsq[qh];
                            qh = (qh + 1) % 16;
                            qn--;
                        }
                        worked = 1;
                    }
                }
            }
            /* 4. show the picture when the clock reaches it (half a frame early is fine); more than 2 frames late:
               dropped, at most 4 in a row */
            int flipped = 0;
            if (frame_pending) {
                if (!shown && !audio_anchored) hold = frame_pts;   /* no sound yet: the first picture starts the clock */
                int32_t late = (int32_t)(movie_clock(m.sound) - frame_pts);
                if (late > (int32_t)(2 * FRAME_TICKS) && drop_run < 4 && !stopped) {
                    frame_pending = 0;
                    dropped++;
                    drop_run++;
                    worked = 1;
                } else if (late >= -(int32_t)(FRAME_TICKS / 2)) {
                    uint32_t tag = (uint32_t)movie_seq * 10000u + (uint32_t)shown + 1u, dump = 0;
                    for (int i = 0; i < opt_ndump; i++) if (opt_dump[i] == tag) dump = tag;
                    hle_ge_movie_frame(m.pic, opt_8888, dump);
                    shown_pts = frame_pts;
                    if (!hold_us) hold_us = now_us();      /* the system clock runs from the first picture */
                    frame_pending = 0;
                    drop_run = 0;
                    shown++;
                    flipped = worked = 1;
                }
            }
            /* 5. the end: the pictures are done and the sound has played out (at most 3 s of draining) */
            if (video_done && !frame_pending && (audio_done || !m.sound)) {
                if (!m.sound || aud.rd == aud.wr) break;
                if (!drain_us) drain_us = tnow;
                if (tnow - drain_us > 3000000u) break;
            }
            /* 6. nothing moves for 5 s (no read, no decode, no sound handed out): give up */
            uint32_t rdn = aud.rd;
            if (worked || rdn != last_rd) { last_progress = tnow; last_rd = rdn; }
            else if (tnow - last_progress > 5000000u) { fail(&m, "stall", shown); end = END_FAILED; break; }
            if (!flipped) sceKernelDelayThread(worked ? 1000 : 3000);
        }
    }
    uint32_t underruns = aud.underruns, reads = m.rd.reads, read_kb = m.rd.pos > PACK ? (m.rd.pos - PACK) >> 10 : 0;
    movie_close(&m);
    if (shown) hle_ge_movie_end();                         /* black until the game's first picture */
    uint64_t took = now_us() - t0;
    hle_plat_pace_shift(took);
    unsigned free2 = (unsigned)(sceKernelTotalFreeMemSize() >> 10);
    LOG("movie %s: %s%s%s (code %08x), %d pictures shown, %u dropped, %u decoded, %u sound units, %u sound underruns, "
        "%u reads (%u KB), X ignored %u (not skippable yet), %.2f s real (pacing moved by it), free %u -> %u -> %u KB\n",
        gpath, end_names[end], m.fail_step ? " at step " : "", m.fail_step ? m.fail_step : "", (unsigned)m.fail_code,
        shown, dropped, decoded, audio_aus, underruns, reads, read_kb, presses_ignored, (double)took / 1e6, free0,
        free1, free2);
    char ms[96];
    snprintf(ms, sizeof ms, "FMV %s %s (%d pictures)", b, end == END_FAILED ? "failed" : end == END_SKIPPED ? "skipped by X" : "played", shown);
    hle_milestone(ms);
    return shown > 0;
}
#endif
