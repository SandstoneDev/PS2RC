/* wavsrc_host.c - host harness for src/hle/psp/hle_wavsrc.c (tests/movies/test_open_movies.py): decodes a WAV file
 * with the PSP player's reader and writes raw stereo s16le.
 *   wavsrc_host IN.wav OUT.raw [stage_bytes] [chunk_frames]
 * Prints "open=<code> format=.. channels=.. rate=.. block=.. fpb=.. total=.." and "frames=<written>". */
#include <stdio.h>
#include <stdlib.h>
#include "hle_wavsrc.h"

static int64_t rd(void *ctx, uint64_t off, void *dst, uint32_t n)
{
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return -1;
    return (int64_t)fread(dst, 1, n, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f);
    uint32_t stage_size = argc > 3 ? (uint32_t)atoi(argv[3]) : 32768u;
    uint32_t chunk = argc > 4 ? (uint32_t)atoi(argv[4]) : 2048u;
    uint8_t *stage = malloc(stage_size);
    int16_t *buf = malloc((size_t)chunk * 4u);
    WavSrc w;
    int r = wavsrc_open(&w, rd, f, size, stage, stage_size);
    printf("open=%d format=%u channels=%u rate=%u block=%u fpb=%u total=%llu\n", r, (unsigned)w.format,
           (unsigned)w.channels, (unsigned)w.rate, (unsigned)w.block_align, (unsigned)w.frames_per_block,
           (unsigned long long)w.total_frames);
    if (r) return 1;
    FILE *o = fopen(argv[2], "wb");
    unsigned long long frames = 0;
    int n;
    while ((n = wavsrc_read(&w, buf, chunk)) > 0) {
        fwrite(buf, 4, (size_t)n, o);
        frames += (unsigned)n;
    }
    fclose(o);
    printf("frames=%llu last=%d\n", frames, n);
    return n < 0 ? 4 : 0;
}
