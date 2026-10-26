/* hle_iop_servers.c - this game's IOP-side services reached through SIF RPC (registered with the core's RPC registry,
 * hle_iop.c).
 *
 * The game's disc streaming module (RPC 0x65686577): the EE side is at 0x3001D0-0x3003E0 (one wrapper per fno):
 *   fno 0 Init(nchannels)               fno 1 Read(channel, ee_buffer, lsn, nsectors) -> 1 = accepted
 *   fno 2 GetStatus(channel) -> 0 ready / 255 busy / error   fno 3 Sync(channel) -> status   fno 4 GetLastPosn()
 *   The HLE reads synchronously from the extracted disc (LSN map), so every request is complete when it returns
 *   (the PSP can queue the reads for a reader thread instead: psp/hle_cdasync.c).
 * The game's sound driver module (RPC 0x65686580; the protocol: snd_driver.h). Two implementations:
 *   - the IOP sound driver (snd_driver.h) + the SPU2-lite engine in lockstep with the virtual clock - the host default
 *     when the build has the audio code (HLE_HAVE_AUDIO_MODEL); with HLE_AUDIO_WAV=<file> everything the engine
 *     renders is written as a 48 kHz WAV;
 *   - a protocol-correct silent responder (no sound) - host runs with --audio silent, PSP builds without the audio
 *     code (build option AUDIO=0) or with ps2rc.cfg audio=0 / no AUDIO folder on the memory stick.
 *   On the PSP the same driver runs in the same lockstep mode (psp/hle_audio_psp.c: sceAudio output, ~2.6 MB) - the
 *   replies equal the host's; their FNV-1a hash is logged at 1024, 2048, 4096, ... RPCs and at the end (compare host
 *   and PSP logs).
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* PZ_SCOPE zones */

#define RPC_DISC_STREAM 0x65686577u
#define RPC_AUDIO    0x65686580u

static uint32_t cd_last_posn;
static uint64_t cd_reads, cd_sectors, audio_calls;
static uint32_t audio_fno_count[64];

/* FNV-1a 64 of every sound driver reply (driver or responder): equal on the host and the PSP = identical replies */
static uint64_t aud_hash = 0xCBF29CE484222325ull;
static uint64_t aud_hashed;
static void aud_hash_reply(const uint8_t *p, uint32_t n)
{
    uint64_t h = aud_hash;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    aud_hash = h;
    aud_hashed++;
    if ((aud_hashed & (aud_hashed - 1)) == 0 && aud_hashed >= 1024)
        LOG("Sound driver replies: %llu, FNV-1a %016llx (vtime %.3f s)\n", (unsigned long long)aud_hashed,
            (unsigned long long)aud_hash, (double)hle_vtime / EE_HZ);
}

#ifdef HLE_HAVE_AUDIO_MODEL
#ifdef HLE_PSP
#include "hle_audio_psp.h"                     /* the driver + sceAudio output on the PSP */
static int aud;
#define AUD_MODEL() hle_audio_psp_model()
#else
#include "audio_host.h"
static AudioHost *aud;
#define AUD_MODEL() audio_host_model(aud)
#endif
int hle_audio_log_level;                       /* the driver's progress messages without --verbose (host env
                                                  HLE_AUDIO_LOG=1, PSP ps2rc.cfg audio_log=1): loads, track starts */
static void aud_log(void *user, int level, const char *fmt, ...)
{
    (void)user;
    if (level > hle_verbose && level > hle_audio_log_level) return;   /* errors always; progress with --verbose */
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t n = strlen(b);
    LOG("Sound driver model: %s%s", b, n && b[n - 1] == '\n' ? "" : "\n");
}
#endif

static int srv_disc_stream(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                        uint32_t rsize);
static int srv_audio(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                     uint32_t rsize);

void hle_pack_iop_init(void)
{
    hle_rpc_register(RPC_DISC_STREAM, srv_disc_stream);
    hle_rpc_register(RPC_AUDIO, srv_audio);
#ifdef HLE_HAVE_AUDIO_MODEL
    if (hle_opt_audio_model) {
#ifdef HLE_PSP
        aud = hle_audio_psp_init(aud_log);           /* reads ps2rc.cfg, incl. audio_log -> hle_audio_log_level */
        LOG("Sound driver: %s\n", aud ? "sound driver running, sceAudio output" : "silent responder");
#else
        const char *wav = getenv("HLE_AUDIO_WAV"), *lg = getenv("HLE_AUDIO_LOG");
        if (lg && *lg) hle_audio_log_level = atoi(lg);
        aud = audio_host_create(hle_disc_dir, wav, aud_log, 0);
        LOG("Sound driver: %s%s%s\n", aud ? "sound driver running" : "sound driver failed to start - silent responder",
            aud && wav ? ", WAV capture to " : "", aud && wav ? wav : "");
#endif
    }
#endif
}

static void recv_word(uint32_t recv, uint32_t rsize, uint32_t v)
{
    if (recv && rsize >= 4) {
        memset(gptr(recv, rsize), 0, rsize);
        sw_(recv, v);
    }
}

/* psp/hle_cdasync.c (PSP only, ps2rc.cfg disc_async=1): Read queued for a reader thread, GetStatus
   255 while it reads, Sync waits; absent (host) or off = the synchronous reads below */
int hle_cdasync_read(uint32_t ch, uint32_t lsn, uint32_t n, void *dst) __attribute__((weak));
int hle_cdasync_status(uint32_t ch) __attribute__((weak));
void hle_cdasync_sync(uint32_t ch) __attribute__((weak));
void hle_cdasync_report(void) __attribute__((weak));

#ifndef HLE_PSP
/* experiment (host only, env HLE_CF_CDASYNC=N): the PSP's asynchronous disc stream reads modelled on the host -
   Read only queues (the buffer is filled with a canary byte), GetStatus answers 255 for the next N polls of that
   channel, the data is copied at completion (GetStatus after N polls, Sync, or a new Read on the channel). A canary
   byte changed before completion = the game used the buffer while the read was still open (logged). */
static int cfa_n = -1;
static struct { int busy, polls; uint32_t lsn, n; uint8_t *dst; } cfa[8];
static uint64_t cfa_reads, cfa_early, cfa_complete_by_read;
static void cfa_complete(uint32_t ch)
{
    if (!cfa[ch].busy) return;
    uint32_t bytes = cfa[ch].n * 2048u, changed = 0, first = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < bytes; i++)
        if (cfa[ch].dst[i] != 0xCD) { if (!changed) first = i; changed++; }
    if (changed) {
        cfa_early++;
        if (cfa_early <= 40)
            LOG("CF CDASYNC EARLY USE: ch %u lsn %u (%s) x%u sectors: %u of %u buffer bytes changed before the read "
                "completed (first at +%u), vtime %.3f\n", ch, cfa[ch].lsn, hle_disc_name_of_lsn(cfa[ch].lsn), cfa[ch].n,
                changed, bytes, first, (double)hle_vtime / EE_HZ);
    }
    hle_disc_read_lsn(cfa[ch].lsn, cfa[ch].n, cfa[ch].dst);
    cfa[ch].busy = 0;
}
int hle_cdasync_read(uint32_t ch, uint32_t lsn, uint32_t n, void *dst)
{
    if (cfa_n < 0) { const char *e = getenv("HLE_CF_CDASYNC"); cfa_n = e ? atoi(e) : 0; }
    if (cfa_n <= 0 || ch >= 8 || !hle_mainloop_iters) return 0;
    if (cfa[ch].busy) { cfa_complete(ch); cfa_complete_by_read++; }
    cfa[ch].busy = 1;
    cfa[ch].polls = cfa_n;
    cfa[ch].lsn = lsn;
    cfa[ch].n = n;
    cfa[ch].dst = dst;
    memset(dst, 0xCD, (size_t)n * 2048u);
    cfa_reads++;
    return 1;
}
int hle_cdasync_status(uint32_t ch)
{
    if (ch >= 8 || !cfa[ch].busy) return 0;
    if (cfa[ch].polls-- > 0) return 255;
    cfa_complete(ch);
    return 0;
}
void hle_cdasync_sync(uint32_t ch) { if (ch < 8) cfa_complete(ch); }
void hle_cdasync_report(void)
{
    if (cfa_n > 0)
        LOG("CF CDASYNC: %llu reads queued (%d polls each), %llu completed by a new Read on the channel, %llu early uses\n",
            (unsigned long long)cfa_reads, cfa_n, (unsigned long long)cfa_complete_by_read, (unsigned long long)cfa_early);
}
#endif

static int disc_stream(R5900Ctx *c, uint32_t fno, uint32_t send, uint32_t recv, uint32_t rsize)
{
    switch (fno) {
    case 0:
        LOG("disc stream: Init(%u channels)\n", send ? gw(send) : 0);
        hle_milestone("disc stream RPC bound and initialised");
        return 0;
    case 1: {
        uint32_t ch = gw(send), buf = gw(send + 4), lsn = gw(send + 8), n = gw(send + 12);
        cd_reads++;
        cd_sectors += n;
        hle_safe_point(c, 4000 + (uint64_t)n * 2048 / 8);
        VLOG("disc stream: Read(ch %u, buf %08x, lsn %u (%s +%u), %u sectors)\n", ch, buf, lsn,
             hle_disc_name_of_lsn(lsn), 0, n);
        if (n > 0x4000) hle_die(c, "disc stream read of %u sectors", n);
        if (hle_cdasync_read && hle_cdasync_read(ch, lsn, n, gptr(buf, n * 2048))) {
            /* queued for the reader thread */
        } else if (hle_disc_read_lsn(lsn, n, gptr(buf, n * 2048)) != 0)
            LOG("disc stream: read lsn %u x%u touches sectors outside any file (zero-filled)\n", lsn, n);
        cd_last_posn = lsn + n;
        if (cd_reads == 1) hle_milestone("first disc stream sector read");
        recv_word(recv, rsize, 1);
        return 0;
    }
    case 2: case 3: {
        uint32_t ch = send ? gw(send) : 0, st = 0;
        if (fno == 3 && hle_cdasync_sync) hle_cdasync_sync(ch);
        else if (fno == 2 && hle_cdasync_status) st = (uint32_t)hle_cdasync_status(ch);
        recv_word(recv, rsize, st);
        return 0;
    }
    case 4:
        recv_word(recv, rsize, cd_last_posn);
        return 0;
    default:
        LOG("disc stream: unknown fno %u\n", fno);
        recv_word(recv, rsize, 0);
        return 0;
    }
}

/* ---- the sound driver: protocol-correct silent responder. fno 0 (init, 16 B): reply word0 = 1 (else the game's
   audio hardware init fails). fno 1 (per frame, 0xAD0-byte state image, request 0x876040 -> reply 0x876B40): the
   reply is the request with every requested bank/sound reported resident, voices that play for AUD_SOUND_MS then end,
   virtual sounds of AUD_SOUND_MS without loop, and the track state machine -6 -> -1 -> -2 -> (slot-0 voice keyed on)
   -3 -> ... so the cutscene track preload wait (0x55A1B0) and the other track loops progress. No sound is produced. */
#define AUD_PKT 0xAD0
#define AUD_BANK_REQ 0x000      /* i16[60] */
#define AUD_SOUND_REQ 0x078     /* i16[60] */
#define AUD_PLAY_POS 0x594      /* i16[48] */
#define AUD_BANK_SLOT 0x5F4     /* i16[48] */
#define AUD_SOUND_ID 0x654      /* i16[48] */
#define AUD_START_PCT 0x6B4     /* u8[48] */
#define AUD_VIRT_A 0x6E4        /* i16[200]: slot -> length ms */
#define AUD_VIRT_B 0x874        /* i16[200]: sound -> loop start ms */
#define AUD_TRACK_ID 0xAB0
#define AUD_NEXT_TRACK 0xAB4
#define AUD_PLAYING_TRACK 0xAB8
#define AUD_TRACK_TIME 0xABC
#define AUD_TRACK_LEN 0xAC0
#define AUD_SOUND_MS 1000

static struct { int active, slot, sound; uint64_t start_ms; } voice[48];
static struct { int state, id, next; uint64_t play_start_ms; int32_t offset_ms; } track = {-6, -1, -1, 0, 0};

static uint64_t now_ms(void) { return hle_vtime * 1000 / EE_HZ; }
static int16_t rd16(const uint8_t *b, int off) { int16_t v; memcpy(&v, b + off, 2); return v; }
static void wr16(uint8_t *b, int off, int16_t v) { memcpy(b + off, &v, 2); }
static int32_t rd32(const uint8_t *b, int off) { int32_t v; memcpy(&v, b + off, 4); return v; }
static void wr32(uint8_t *b, int off, int32_t v) { memcpy(b + off, &v, 4); }

static void audio_frame(const uint8_t *req, uint8_t *rep)
{
    memmove(rep, req, AUD_PKT);                              /* banks/sounds: echo = resident */
    uint64_t now = now_ms();
    /* track requests first, so a slot-0 key-on in the same packet can move -2 -> -3 */
    int32_t tid = rd32(req, AUD_TRACK_ID), ttime = rd32(req, AUD_TRACK_TIME);
    if (tid < 0) {
        if (track.state != -6) VLOG("Sound driver: track %d stopped\n", track.id);
        track.state = -6;
        track.id = -1;
    } else if (tid != track.id && ttime >= 0) {
        LOG("Sound driver: track %d requested (start %d ms) -> loading\n", tid, ttime);
        track.state = -1;
        track.id = tid;
        track.offset_ms = ttime;
    } else if (track.state == -1) {
        track.state = -2;
    } else if (track.state == -4) {
        track.state = -6;
    }
    track.next = rd32(req, AUD_NEXT_TRACK);
    for (int v = 0; v < 48; v++) {
        int16_t slot = rd16(req, AUD_BANK_SLOT + 2 * v), snd = rd16(req, AUD_SOUND_ID + 2 * v);
        int16_t pos = rd16(req, AUD_PLAY_POS + 2 * v);
        if (slot < 0) {
            voice[v].active = 0;
            wr16(rep, AUD_PLAY_POS + 2 * v, -1);
            continue;
        }
        int fresh = !voice[v].active || voice[v].slot != slot || voice[v].sound != snd || pos >= 0;
        if (fresh) {
            int32_t start = pos >= 0 && !req[AUD_START_PCT + v] ? pos : 0;
            voice[v].active = 1;
            voice[v].slot = slot;
            voice[v].sound = snd;
            voice[v].start_ms = now - (uint64_t)start;
            wr16(rep, AUD_PLAY_POS + 2 * v, (int16_t)start);
            if (slot == 0 && track.state == -2) {             /* stream voices keyed on: the track plays */
                track.state = -3;
                track.play_start_ms = now;
            }
            continue;
        }
        uint64_t el = now - voice[v].start_ms;
        if (slot == 0) {                                     /* stream voice: lives as long as the track plays */
            wr16(rep, AUD_PLAY_POS + 2 * v, track.state == -3 ? (int16_t)(el & 0x7FFF) : -1);
            if (track.state != -3) voice[v].active = 0;
        } else if (el >= AUD_SOUND_MS) {
            voice[v].active = 0;
            wr16(rep, AUD_PLAY_POS + 2 * v, -1);
        } else {
            wr16(rep, AUD_PLAY_POS + 2 * v, (int16_t)el);
        }
    }
    for (int i = 0; i < 200; i++) {                          /* virtual sounds: length / loop start */
        int ok = rd16(req, AUD_VIRT_A + 2 * i) >= 0 && rd16(req, AUD_VIRT_B + 2 * i) >= 0;
        wr16(rep, AUD_VIRT_A + 2 * i, ok ? AUD_SOUND_MS : -1);
        wr16(rep, AUD_VIRT_B + 2 * i, -1);
    }
    wr32(rep, AUD_TRACK_ID, track.id);
    wr32(rep, AUD_NEXT_TRACK, track.next);
    wr32(rep, AUD_PLAYING_TRACK, track.state == -3 || track.state == -4 ? track.id : -1);
    if (track.state == -3) {
        uint64_t t = now - track.play_start_ms + (uint64_t)(track.offset_ms > 0 ? track.offset_ms : 0);
        wr32(rep, AUD_TRACK_TIME, (int32_t)(t & 0x7FFFFFFF));
    } else {
        wr32(rep, AUD_TRACK_TIME, track.state);
    }
    wr32(rep, AUD_TRACK_LEN, 0);                             /* length Unknown without TrakLkup parsing */
}

static int audio(R5900Ctx *c, uint32_t fno, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize)
{
    (void)c;
    audio_calls++;
    audio_fno_count[fno & 63]++;
    if (audio_fno_count[fno & 63] <= 2)
        LOG("Sound driver RPC fno %u send %08x/%u recv %08x/%u\n", fno, send, ssize, recv, rsize);
    if (audio_calls == 1) hle_milestone("first sound driver RPC");
#ifdef HLE_HAVE_AUDIO_MODEL
    if (aud) {                                               /* the sound driver, lockstep on the virtual clock */
        uint64_t us = hle_vtime / EE_HZ * 1000000ull + hle_vtime % EE_HZ * 1000000ull / EE_HZ;
        {
            PZ_SCOPE(audio);                                 /* driver + engine render (+ its disc reads) */
#ifdef HLE_PSP
            hle_audio_psp_rpc(fno, send ? gptr(send, ssize) : NULL, ssize, recv ? gptr(recv, rsize) : NULL, rsize, us);
#else
            audio_host_rpc(aud, fno, send ? gptr(send, ssize) : NULL, ssize, recv ? gptr(recv, rsize) : NULL, rsize, us);
#endif
        }
        if (fno == 0) {
            LOG("Sound driver: init -> %u (IOP audio model)\n", recv && rsize >= 4 ? gw(recv) : 0);
            hle_milestone("Sound driver initialised (IOP audio model)");
        }
        return 0;
    }
#endif
    if (fno == 0) {
        uint32_t host = send ? gw(send) : 0;
        if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
        if (recv && rsize >= 4) sw_(recv, 1);                /* managers started */
        LOG("Sound driver: init (host flag %u) -> 1\n", host);
        hle_milestone("Sound driver initialised (silent responder)");
        return 0;
    }
    if (fno == 1 && send && recv && ssize >= AUD_PKT && rsize >= AUD_PKT) {
        audio_frame(gptr(send, AUD_PKT), gptr(recv, AUD_PKT));
        return 0;
    }
    LOG("Sound driver: unexpected fno %u (send %u / recv %u bytes) - reply zero-filled\n", fno, ssize, rsize);
    if (recv && rsize) memset(gptr(recv, rsize), 0, rsize);
    return 0;
}

/* the stream track's life in the log, both platforms, every implementation - one line per change of
   (requested id, replied id, playing id, state), per change of the stream voices (slot 0 = the stream ring) the EE
   keys and whether they are audible, and per halving / doubling of their level. Cutscene / radio / ambience / jingle
   tracks can be followed on the hardware. */
static void aud_track_trace(const uint8_t *req, const uint8_t *rep)
{
    static int32_t l_req = -100, l_id = -100, l_play = -100, l_st = -100;
    static uint64_t l_vmask = 0, l_amask = 0;
    int32_t q = rd32(req, AUD_TRACK_ID), id = rd32(rep, AUD_TRACK_ID), play = rd32(rep, AUD_PLAYING_TRACK);
    int32_t t = rd32(rep, AUD_TRACK_TIME), st = t < 0 ? t : 0;
    static int32_t l_tmax, l_len;                /* furthest play time / length of the playing track (0xAC0) */
    if (t > l_tmax) { l_tmax = t; l_len = rd32(rep, AUD_TRACK_TIME + 4); }
    if (q != l_req || id != l_id || play != l_play || st != l_st) {
        static const char *const names[] = {"stopped", "?", "ended", "playing", "ready", "loading"};  /* -6..-1 */
        char pl[64] = "";
        if (q == -1 && l_tmax > 0)               /* the EE stops it: how far it got (a skipped cutscene: < length) */
            snprintf(pl, sizeof pl, ", played to %d of %d ms", l_tmax, l_len);
        LOG("Sound driver track: request %d (next %d, start %d ms, flags %02x) -> id %d, playing %d, %s %d%s (vtime %.3f s)\n",
            q, rd32(req, AUD_NEXT_TRACK), rd32(req, AUD_TRACK_TIME), req[0xAC5], id, play,
            t >= 0 ? "time" : t >= -6 ? names[t + 6] : "state", t, pl, (double)hle_vtime / EE_HZ);
        if (q == -1 || q != l_req) l_tmax = 0;
        l_req = q; l_id = id; l_play = play; l_st = st;
    }
    uint64_t vm = 0, am = 0;                     /* voices on slot 0 / of those, audible (volume and pitch != 0) */
    int lv = 0;                                  /* the stream's level: loudest audible one, max(|L|, |R|) */
    for (int v = 0; v < 48; v++)
        if (rd16(req, AUD_BANK_SLOT + 2 * v) == 0) {
            int vl = abs((int)rd16(req, 0x4D4 + 4 * v)), vr = abs((int)rd16(req, 0x4D4 + 4 * v + 2));
            vm |= 1ull << v;
            if ((vl || vr) && rd32(req, 0x414 + 4 * v)) {
                am |= 1ull << v;
                if (vl > lv) lv = vl;
                if (vr > lv) lv = vr;
            }
        }
#ifndef HLE_PSP
    {   /* host test aid: HLE_T2_VOICES=t[,t...] (virtual s) - every voice the EE has keyed (slot, sound, volume,
           pitch) once at each of those times */
        static int vinit, vn, vk;
        static double vt[16];
        if (!vinit) {
            vinit = 1;
            for (const char *e = getenv("HLE_T2_VOICES"); e && *e && vn < 16; e = strchr(e, ',') ? strchr(e, ',') + 1 : NULL)
                vt[vn++] = atof(e);
        }
        if (vk < vn && (double)hle_vtime / EE_HZ >= vt[vk]) {
            vk++;
            char b[2048];
            int n = 0, cnt = 0;
            for (int v = 0; v < 48 && n < (int)sizeof b - 64; v++) {
                int sl = rd16(req, AUD_BANK_SLOT + 2 * v);
                if (sl < 0) continue;
                cnt++;
                n += snprintf(b + n, sizeof b - (size_t)n, " v%d(slot %d sound %d vol %d/%d freq %d)", v, sl,
                              rd16(req, AUD_SOUND_ID + 2 * v), rd16(req, 0x4D4 + 4 * v), rd16(req, 0x4D4 + 4 * v + 2),
                              rd32(req, 0x414 + 4 * v));
            }
            b[n] = 0;
            LOG("T2 voices at vtime %.3f: %d keyed:%s\n", (double)hle_vtime / EE_HZ, cnt, b);
        }
    }
#endif
    /* a fade / mute of a keyed stream - one line when the level halves or doubles against the last line (or goes
       to / from 0), at most every 0.5 s of virtual time */
    static int l_lv = -1;
    static uint64_t l_lv_t;
    if (!vm) l_lv = -1;
    else if ((l_lv < 0 || (lv == 0) != (l_lv == 0) || lv > 2 * l_lv || 2 * lv < l_lv) &&
             (l_lv < 0 || hle_vtime - l_lv_t >= EE_HZ / 2)) {
        if (l_lv >= 0)
            LOG("Sound driver stream level: %d (was %d) (vtime %.3f s)\n", lv, l_lv, (double)hle_vtime / EE_HZ);
        l_lv = lv;
        l_lv_t = hle_vtime;
    }
    if (vm != l_vmask || am != l_amask) {
        char b[256];
        int n = 0;
        for (int v = 0; v < 48 && n < (int)sizeof b - 48; v++)
            if (vm >> v & 1)
                n += snprintf(b + n, sizeof b - (size_t)n, " v%d(sound %d vol %d/%d freq %u pos %d)", v,
                              rd16(req, AUD_SOUND_ID + 2 * v), rd16(req, 0x4D4 + 4 * v), rd16(req, 0x4D4 + 4 * v + 2),
                              (unsigned)rd32(req, 0x414 + 4 * v), rd16(req, AUD_PLAY_POS + 2 * v));
        b[n] = 0;
        LOG("Sound driver stream voices: %d keyed, %d audible:%s (vtime %.3f s)\n", __builtin_popcountll(vm),
            __builtin_popcountll(am), n ? b : " none", (double)hle_vtime / EE_HZ);
        l_vmask = vm; l_amask = am;
    }
}

#if !defined(HLE_PSP) || defined(HLE_T2_AIDS)
#ifdef HLE_PSP
/* PSP test builds only (-DHLE_T2_AIDS, never a package): the same fixed teleport as a host test run - on foot onto
   the first mission's start marker of the main script (2495.2 -1687.0 12.5, 1 x 1 x 2 m) at 60 s, after the intro
   skip */
static const char *t2_getenv(const char *k)
{
    return !strcmp(k, "HLE_T2_TELEPORT") ? "60:2495.2:-1687.0:13.0" : NULL;
}
#else
#include <math.h>
#define t2_getenv getenv
#endif
/* host test aid: HLE_T2_TELEPORT=vtime:x:y:z[,...] moves the player - his vehicle when he is in one (the player
   vehicle lookup 0x2640xx: ped+0x4A5 bit 0 = in a vehicle, ped+0x5CC = the vehicle; matrix at entity+0x14, position
   +0x30) - once per entry, checked at every sound driver frame; used to reach mission triggers without riding there.
   Changes the game only when set; never in a PSP package (a PSP test build with -DHLE_T2_AIDS has the fixed spec
   above). */
static void t2_teleport(void)
{
    static int init, n, next;
    static struct { double t; float p[3]; } tp[8];
    if (!init) {
        init = 1;
        const char *e = t2_getenv("HLE_T2_TELEPORT");
        while (e && *e && n < 8) {
            if (sscanf(e, "%lf:%f:%f:%f", &tp[n].t, &tp[n].p[0], &tp[n].p[1], &tp[n].p[2]) == 4) n++;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
#ifndef HLE_PSP
    static double plog_next = -1;
    if (plog_next < 0) plog_next = getenv("HLE_T2_PEDLOG") ? 40.0 : 1e18;
    if ((double)hle_vtime / EE_HZ >= plog_next) {                 /* where the player is, once per virtual second */
        plog_next += 1.0;
        uint32_t pp = gw(0x0070A260u), pm = pp ? gw(pp + 0x14) : 0, pv = pp ? gw(pp + 0x5CC) : 0;
        float xyz[3] = {0, 0, 0};
        if (pm) memcpy(xyz, gptr(pm + 0x30, 12), 12);
        uint32_t ip = gw(0x006B26C4u);                             /* the intro mission thread's IP */
        char hx[100];
        int hn = 0;
        if (ip > 0x100000u && ip < 0x1B00000u)
            for (int k = 0; k < 40; k++) hn += snprintf(hx + hn, sizeof hx - (size_t)hn, "%02x", ((const uint8_t *)gptr(ip + (uint32_t)k, 1))[0]);
        hx[hn] = 0;
        LOG("T2 player at vtime %.1f: ped %08x pos %.1f %.1f %.1f flags+4A5 %02x vehicle %08x; intro thread ip %08x: %s\n",
            (double)hle_vtime / EE_HZ, pp, xyz[0], xyz[1], xyz[2], pp ? ((const uint8_t *)gptr(pp + 0x4A5, 1))[0] : 0, pv,
            ip, hx);
        /* the running script threads (+0 next, +4 prev, +8 name[8], +0x14 IP), walked both ways from the one at
           0x6B26B0 */
        char sl[1024];
        int sn = 0;
        static uint32_t head_ptr;                                    /* the active script list's head pointer, found once */
        uint32_t s0 = 0x006B26B0u, s = s0;
        if (!head_ptr) {
            for (int k = 0; k < 40 && s; k++) { uint32_t pr = gw(s + 4); if (!pr || pr < 0x100000u || pr > 0x1B00000u) break; s = pr; }
            for (uint32_t a = 0x5A2000u; a < 0x8A5D80u && !head_ptr; a += 4)
                if (gw(a) == s && a != s + 4) head_ptr = a;
            LOG("T2 scripts: list head %08x -> pointer at %08x\n", s, head_ptr);
        }
        if (head_ptr) s = gw(head_ptr);
        for (int k = 0; k < 64 && s && s > 0x100000u && s < 0x1B00000u && sn < (int)sizeof sl - 40; k++) {
            char nm[9];
            memcpy(nm, gptr(s + 8, 8), 8);
            nm[8] = 0;
            for (int q = 0; q < 8; q++) if (nm[q] && (nm[q] < 32 || nm[q] > 126)) nm[q] = '?';
            sn += snprintf(sl + sn, sizeof sl - (size_t)sn, " %s@%08x", nm, gw(s + 0x14));
            s = gw(s);
        }
        sl[sn] = 0;
        LOG("T2 scripts:%s\n", sl);
    }
    /* HLE_T2_RIDE=t0:x:y:z:speed[,...]: from t0 on, move the player's vehicle (or ped) in a straight line toward x y z at
       speed m/s, one step per sound driver frame (a ride without steering; the legs run one after the other) */
    static int rinit, rn, rk;
    static struct { double t; float p[3], v; } rd_[8];
    static double rlast;
    if (!rinit) {
        rinit = 1;
        const char *e = getenv("HLE_T2_RIDE");
        while (e && *e && rn < 8) {
            if (sscanf(e, "%lf:%f:%f:%f:%f", &rd_[rn].t, &rd_[rn].p[0], &rd_[rn].p[1], &rd_[rn].p[2], &rd_[rn].v) == 5) rn++;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    double tnow = (double)hle_vtime / EE_HZ;
    if (rk < rn && tnow >= rd_[rk].t) {
        uint32_t ped = gw(0x0070A260u);
        uint32_t ent = ped && (((const uint8_t *)gptr(ped + 0x4A5, 1))[0] & 1) && gw(ped + 0x5CC) ? gw(ped + 0x5CC) : ped;
        uint32_t m = ent ? gw(ent + 0x14) : 0;
        double dt = rlast > 0 ? tnow - rlast : 0;
        rlast = tnow;
        if (m && dt > 0 && dt < 0.5) {
            float cur[3];
            memcpy(cur, gptr(m + 0x30, 12), 12);
            double dx = rd_[rk].p[0] - cur[0], dy = rd_[rk].p[1] - cur[1], dz = rd_[rk].p[2] - cur[2];
            double d = sqrt(dx * dx + dy * dy + dz * dz), step = rd_[rk].v * dt;
            if (d <= step || d < 0.5) {
                LOG("T2 ride: leg %d reached %.1f %.1f %.1f at vtime %.3f (%s)\n", rk, rd_[rk].p[0], rd_[rk].p[1], rd_[rk].p[2],
                    tnow, ent == ped ? "ped" : "vehicle");
                rk++;
                rlast = 0;
            } else {
                float nw[3] = {(float)(cur[0] + dx * step / d), (float)(cur[1] + dy * step / d), (float)(cur[2] + dz * step / d)};
                for (int i = 0; i < 3; i++) { uint32_t w; memcpy(&w, &nw[i], 4); sw_(m + 0x30 + 4u * (uint32_t)i, w); }
            }
        }
    }
#endif
    if (next >= n || (double)hle_vtime / EE_HZ < tp[next].t) return;
    uint32_t ped = gw(0x0070A260u);
    uint32_t ent = ped && (((const uint8_t *)gptr(ped + 0x4A5, 1))[0] & 1) && gw(ped + 0x5CC) ? gw(ped + 0x5CC) : ped;
    uint32_t m = ent ? gw(ent + 0x14) : 0;
    if (m) {
        for (int i = 0; i < 3; i++) { uint32_t w; memcpy(&w, &tp[next].p[i], 4); sw_(m + 0x30 + 4u * (uint32_t)i, w); }
        LOG("T2 test teleport (HLE_T2_TELEPORT) at vtime %.3f: %s %08x -> %.1f %.1f %.1f (ped %08x flags+4A5 %02x +5CC %08x)\n",
            (double)hle_vtime / EE_HZ, ent == ped ? "player ped" : "player vehicle", ent, tp[next].p[0], tp[next].p[1],
            tp[next].p[2], ped, ((const uint8_t *)gptr(ped + 0x4A5, 1))[0], gw(ped + 0x5CC));
    }
    next++;
}
#endif

static int srv_disc_stream(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                        uint32_t rsize)
{
    (void)mode;
    (void)ssize;
    return disc_stream(c, fno, send, recv, rsize);
}

static int srv_audio(R5900Ctx *c, uint32_t fno, uint32_t mode, uint32_t send, uint32_t ssize, uint32_t recv,
                     uint32_t rsize)
{
    (void)mode;
    uint8_t req_copy[AUD_PKT];                           /* the request as sent (the reply overwrites it) */
    int tr = fno == 1 && send && recv && ssize >= AUD_PKT && rsize >= AUD_PKT;
    if (tr) memcpy(req_copy, gptr(send, AUD_PKT), AUD_PKT);
    int r = audio(c, fno, send, ssize, recv, rsize);
    if (recv && rsize) aud_hash_reply(gptr(recv, rsize), rsize);   /* reply fingerprint */
    if (tr) aud_track_trace(req_copy, gptr(recv, AUD_PKT));
#if !defined(HLE_PSP) || defined(HLE_T2_AIDS)
    if (tr) t2_teleport();
#endif
    return r;
}

void hle_pack_iop_report(void)
{
    LOG("IOP: disc stream reads %llu (%llu sectors), sound driver calls %llu\n", (unsigned long long)cd_reads,
        (unsigned long long)cd_sectors, (unsigned long long)audio_calls);
    if (hle_cdasync_report) hle_cdasync_report();             /* reader thread statistics */
    LOG("Sound driver replies: %llu, FNV-1a %016llx (end of run)\n", (unsigned long long)aud_hashed,
        (unsigned long long)aud_hash);
#ifdef HLE_HAVE_AUDIO_MODEL
    if (aud) {
        const SnddStats *s = &AUD_MODEL()->st;
        LOG("IOP audio model: frames %u, bank loads %u ok / %u failed, sound starts %u (%u failed), tracks started %u, "
            "track fills %u, track ends %u, state %d\n", s->frames, s->loads_ok, s->loads_failed, s->starts,
            s->starts_failed, s->tracks_started, s->track_fills, s->track_ends, (int)AUD_MODEL()->state);
#ifdef HLE_PSP
        hle_audio_psp_report();                              /* output statistics; stops the feeder thread */
        aud = 0;
#else
        audio_host_destroy(aud);                             /* closes the WAV capture (header) */
        aud = NULL;
#endif
    }
#endif
}
