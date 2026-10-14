/* audio_host.c - see audio_host.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_host.h"
#include "audio_hostio.h"
#include "spu2_lite.h"

struct AudioHost {
    SndDrv a;
    Spu2 e;
    AudioHostIo io;
    uint8_t *ram;
    uint64_t last_us;                  /* clock of the previous fno 1 (minus the carried remainder) */
    int started;
    FILE *wav;
    uint32_t wav_frames;
    FILE *rec;                         /* HLE_AUDIO_REC=<file> records every RPC (audio bench input) */
};

static void *host_alloc(void *u, size_t n) { (void)u; return calloc(1, n); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wav_header(AudioHost *h)
{
    uint8_t w[44];
    uint32_t data = h->wav_frames * 4u;
    memcpy(w, "RIFF", 4); put32(w + 4, 36 + data); memcpy(w + 8, "WAVEfmt ", 8); put32(w + 16, 16);
    w[20] = 1; w[21] = 0; w[22] = 2; w[23] = 0; put32(w + 24, SPU2_RATE); put32(w + 28, SPU2_RATE * 4);
    w[32] = 4; w[33] = 0; w[34] = 16; w[35] = 0; memcpy(w + 36, "data", 4); put32(w + 40, data);
    fseek(h->wav, 0, SEEK_SET);
    fwrite(w, 1, sizeof w, h->wav);
    fseek(h->wav, 0, SEEK_END);
}

static void wav_sink(void *u, const int16_t *s, uint32_t n)
{
    AudioHost *h = (AudioHost *)u;
    fwrite(s, 4, n, h->wav);
    h->wav_frames += n;
}

AudioHost *audio_host_create(const char *disc_root, const char *wav_path,
                             void (*log)(void *user, int level, const char *fmt, ...), void *log_user)
{
    AudioHost *h = (AudioHost *)calloc(1, sizeof *h);
    if (!h) return 0;
    h->ram = (uint8_t *)calloc(1, SPU2_RAM_SIZE);
    if (!h->ram || audio_hostio_init(&h->io, disc_root) != 0) {
        free(h->ram);
        free(h);
        return 0;
    }
    spu2_init(&h->e, h->ram);
    SnddConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    audio_hostio_bind(&h->io, &cfg.io);
    spu2_bind_sndd(&h->e, &cfg.spu, &h->a, 1);
    cfg.alloc = host_alloc;
    cfg.log = log;
    cfg.log_user = log_user;
    sndd_init(&h->a, &cfg);
    const char *mv = getenv("HLE_AUDIO_VOICES");     /* the PSP's voice mix limit (spu2_lite mix_limit) */
    if (mv && *mv) h->e.mix_limit = (uint32_t)atoi(mv);
    if (wav_path && *wav_path && (h->wav = fopen(wav_path, "wb")) != 0) {
        wav_header(h);
        h->e.sink = wav_sink;
        h->e.sink_user = h;
    }
    /* the RPC stream as the driver saw it - "XAR1", then per call u32 fno, ssize, rsize, frame_ms + the send bytes -
       replayed by the audio bench (engine cost, bit-exactness of engine changes: reply and 48 kHz mix checksums) */
    const char *rp = getenv("HLE_AUDIO_REC");
    if (rp && *rp && (h->rec = fopen(rp, "wb")) != 0) fwrite("XAR1", 1, 4, h->rec);
    return h;
}

static void rec_call(AudioHost *h, uint32_t fno, const void *send, uint32_t ssize, uint32_t rsize, uint32_t ms)
{
    uint32_t hd[4] = {fno, ssize, rsize, ms};
    fwrite(hd, 4, 4, h->rec);
    if (ssize) fwrite(send, 1, ssize, h->rec);
}

int audio_host_rpc(AudioHost *h, uint32_t fno, const void *send, uint32_t ssize, void *recv, uint32_t rsize,
                   uint64_t now_us)
{
    uint32_t ms = 0;
    if (fno == 1) {
        if (!h->started) { h->started = 1; h->last_us = now_us; }
        uint64_t d = now_us > h->last_us ? now_us - h->last_us : 0;
        if (d > 10000000u) d = 10000000u;                    /* clamp pauses to 10 s of rendering */
        ms = (uint32_t)(d / 1000u);
        h->last_us += (uint64_t)ms * 1000u;
        if (now_us > h->last_us + 10000000u) h->last_us = now_us;
    }
    if (h->rec) rec_call(h, fno, send, ssize, rsize, ms);
    return sndd_rpc(&h->a, fno, (const uint8_t *)send, ssize, (uint8_t *)recv, rsize, ms);
}

SndDrv *audio_host_model(AudioHost *h) { return &h->a; }

void audio_host_destroy(AudioHost *h)
{
    if (!h) return;
    if (h->rec) fclose(h->rec);
    if (h->wav) {
        wav_header(h);
        fclose(h->wav);
    }
    audio_hostio_close(&h->io);
    free(h->ram);
    free(h);
}
