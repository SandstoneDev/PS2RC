/* hle_wavsrc.c - a streaming RIFF/WAVE reader (16-bit PCM or 4-bit IMA ADPCM) -> stereo s16; see hle_wavsrc.h.
 * Plain C, no platform calls (the movie player passes its file layer's read; a host test compiles it alone). */
#include <string.h>
#include "hle_wavsrc.h"

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

const char *wavsrc_format_name(const WavSrc *w)
{
    return w->format == 1 ? "PCM s16" : w->format == 0x11 ? "IMA ADPCM" : "?";
}

int wavsrc_open(WavSrc *w, wavsrc_read_fn read, void *ctx, uint64_t file_size, uint8_t *stage, uint32_t stage_size)
{
    memset(w, 0, sizeof *w);
    w->read = read;
    w->ctx = ctx;
    uint8_t h[20];
    if (read(ctx, 0, h, 12) != 12) return WAVSRC_E_READ;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return WAVSRC_E_NOT_WAVE;
    uint64_t pos = 12, fact = 0;
    int have_fmt = 0;
    while (pos + 8 <= file_size) {
        if (read(ctx, pos, h, 8) != 8) return WAVSRC_E_READ;
        uint32_t size = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            if (size < 16 || read(ctx, pos + 8, h, 16) != 16) return WAVSRC_E_FORMAT;
            w->format = le16(h);
            w->channels = le16(h + 2);
            w->rate = le32(h + 4);
            w->block_align = le16(h + 12);
            uint32_t bits = le16(h + 14), ch = w->channels;
            if (ch < 1 || ch > 2) return WAVSRC_E_FORMAT;
            if (w->format == 1) {
                if (bits != 16 || w->block_align != 2 * ch) return WAVSRC_E_FORMAT;
                w->frames_per_block = 1;
            } else if (w->format == 0x11) {
                if (bits != 4 || w->block_align <= 4 * ch || (w->block_align - 4 * ch) % (4 * ch)) return WAVSRC_E_FORMAT;
                w->frames_per_block = (w->block_align - 4 * ch) * 2 / ch + 1;
            } else
                return WAVSRC_E_FORMAT;
            have_fmt = 1;
        } else if (!memcmp(h, "fact", 4) && size >= 4) {
            if (read(ctx, pos + 8, h, 4) != 4) return WAVSRC_E_READ;
            fact = le32(h);
        } else if (!memcmp(h, "data", 4)) {
            w->data_off = pos + 8;
            w->data_len = size;
            if (w->data_off + w->data_len > file_size) w->data_len = file_size - w->data_off;
            break;
        }
        pos += 8 + (uint64_t)size + (size & 1u);
    }
    if (!have_fmt) return WAVSRC_E_FORMAT;
    if (!w->data_off) return WAVSRC_E_NO_DATA;
    uint64_t blocks = w->data_len / w->block_align;
    w->total_frames = blocks * w->frames_per_block;
    if (w->format == 0x11 && fact && fact < w->total_frames) w->total_frames = fact;
    if (!w->total_frames) return WAVSRC_E_NO_DATA;
    w->stage = stage;
    w->stage_size = stage_size - stage_size % w->block_align;
    if (w->stage_size < w->block_align) return WAVSRC_E_STAGING;
    return WAVSRC_OK;
}

/* the next whole blocks of the data chunk into the staging buffer: 1, 0 at the end, WAVSRC_E_READ */
static int refill(WavSrc *w)
{
    uint64_t left = w->data_len - w->file_pos;
    left -= left % w->block_align;
    if (!left) return 0;
    uint32_t n = left < w->stage_size ? (uint32_t)left : w->stage_size;
    if (w->read(w->ctx, w->data_off + w->file_pos, w->stage, n) != (int64_t)n) return WAVSRC_E_READ;
    w->file_pos += n;
    w->stage_len = n;
    w->stage_pos = 0;
    return 1;
}

static inline int32_t ima_expand(int32_t *pred, int32_t *index, unsigned n)
{
    int32_t step = ima_step[*index];
    int32_t diff = ((2 * (int32_t)(n & 7) + 1) * step) >> 3;
    int32_t p = (n & 8) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    int32_t i = *index + ima_index[n & 15];
    *index = i < 0 ? 0 : i > 88 ? 88 : i;
    *pred = p;
    return p;
}

int wavsrc_read(WavSrc *w, int16_t *dst, uint32_t frames)
{
    uint32_t out = 0, ch = w->channels;
    while (out < frames && w->frames_done < w->total_frames) {
        if (w->format == 1) {
            if (w->stage_pos >= w->stage_len) {
                int r = refill(w);
                if (r < 0) return r;
                if (!r) break;
            }
            uint32_t n = (w->stage_len - w->stage_pos) / w->block_align;
            if (n > frames - out) n = frames - out;
            if (n > w->total_frames - w->frames_done) n = (uint32_t)(w->total_frames - w->frames_done);
            const uint8_t *p = w->stage + w->stage_pos;
            for (uint32_t i = 0; i < n; i++, p += w->block_align, out++) {
                int16_t l = (int16_t)le16(p);
                dst[2 * out] = l;
                dst[2 * out + 1] = ch == 2 ? (int16_t)le16(p + 2) : l;
            }
            w->stage_pos += n * w->block_align;
            w->frames_done += n;
            continue;
        }
        if (!w->blk || w->blk_frame >= w->frames_per_block) {        /* IMA: the next block */
            if (w->stage_pos >= w->stage_len) {
                int r = refill(w);
                if (r < 0) return r;
                if (!r) break;
            }
            w->blk = w->stage + w->stage_pos;
            w->stage_pos += w->block_align;
            w->blk_frame = 0;
            for (uint32_t c = 0; c < ch; c++) {
                w->pred[c] = (int16_t)le16(w->blk + 4 * c);
                w->index[c] = w->blk[4 * c + 2] > 88 ? 88 : w->blk[4 * c + 2];
            }
        }
        int32_t s[2] = {0, 0};
        if (w->blk_frame == 0) {
            s[0] = w->pred[0];
            s[1] = w->pred[ch - 1];
        } else {
            uint32_t i = w->blk_frame - 1;                            /* sample index inside the channel's nibbles */
            for (uint32_t c = 0; c < ch; c++) {
                uint8_t b = w->blk[4 * ch + ((i >> 3) * ch + c) * 4 + ((i & 7) >> 1)];
                s[c] = ima_expand(&w->pred[c], &w->index[c], (i & 1) ? (unsigned)(b >> 4) : (unsigned)(b & 15));
            }
            if (ch == 1) s[1] = s[0];
        }
        dst[2 * out] = (int16_t)s[0];
        dst[2 * out + 1] = (int16_t)s[1];
        out++;
        w->blk_frame++;
        w->frames_done++;
    }
    return (int)out;
}
