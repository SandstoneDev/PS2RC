/* hle_wavsrc.h - a streaming reader for RIFF/WAVE sound files: 16-bit PCM (format 1) or 4-bit IMA ADPCM
 * (format 0x11, the common block layout: per channel a 4-byte header with the first sample + step index, then 4-byte
 * groups of 8 samples per channel in turn). Used as the sound track of a video-only movie.
 *
 * No platform calls: the caller passes a read function and a staging buffer (the file is read in large pieces, one
 * request per staging buffer). Output is always stereo s16 interleaved (a mono file is duplicated). The IMA nibble
 * expansion is diff = ((2 * (n & 7) + 1) * step) >> 3, the form ffmpeg's encoder and decoder use, so the decode is
 * bit-exact with `ffmpeg -i file.wav -f s16le`. */
#ifndef HLE_WAVSRC_H
#define HLE_WAVSRC_H
#include <stdint.h>

typedef int64_t (*wavsrc_read_fn)(void *ctx, uint64_t offset, void *dst, uint32_t bytes);

enum {
    WAVSRC_OK = 0, WAVSRC_E_READ = -1, WAVSRC_E_NOT_WAVE = -2, WAVSRC_E_FORMAT = -3, WAVSRC_E_NO_DATA = -4,
    WAVSRC_E_STAGING = -5
};

typedef struct {
    wavsrc_read_fn read;
    void *ctx;
    uint32_t format;                  /* 1 = PCM s16, 0x11 = IMA ADPCM */
    uint32_t channels, rate, block_align, frames_per_block;
    uint64_t data_off, data_len;      /* the data chunk in the file */
    uint64_t total_frames;            /* from the fact chunk (IMA) or the data size (PCM) */
    uint64_t frames_done;
    uint64_t file_pos;                /* next byte of the data chunk to read (relative to data_off) */
    uint8_t *stage;                   /* staging buffer: whole blocks */
    uint32_t stage_size, stage_len, stage_pos;
    /* IMA: the block being decoded */
    uint32_t blk_frame;               /* next frame index inside the current block (0 = header sample) */
    int32_t pred[2];
    int32_t index[2];
    const uint8_t *blk;               /* the current block inside the staging buffer */
} WavSrc;

/* parses the header (RIFF, fmt, fact, data); 0 or a WAVSRC_E_* code */
int wavsrc_open(WavSrc *w, wavsrc_read_fn read, void *ctx, uint64_t file_size, uint8_t *stage, uint32_t stage_size);
/* up to `frames` stereo frames into dst; returns the frames written (0 = the end), or WAVSRC_E_READ */
int wavsrc_read(WavSrc *w, int16_t *dst, uint32_t frames);
const char *wavsrc_format_name(const WavSrc *w);

#endif
