/* hle_stream.c - this game's streaming loader timed for the hitch breakdown (kept out of the core's hle_hitch.c):
 * the HITCH_STREAM source and its report line. */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_hitch.h"

static uint64_t st_stream_calls, st_stream_conv, st_stream_us, st_stream_max_us;

void hle_pack_hitch_report(void)
{
    if (st_stream_calls)
        LOG("hitch: stream_channel 0x2C2CD0 (the game's model conversion): %llu calls, %llu that "
            "converted, %llu ms in all (nested disc / log / GE time excluded), max %llu.%llu ms\n",
            (unsigned long long)st_stream_calls, (unsigned long long)st_stream_conv,
            (unsigned long long)(st_stream_us / 1000u), (unsigned long long)(st_stream_max_us / 1000u),
            (unsigned long long)(st_stream_max_us / 100u % 10u));
}

/* ------------------------------------------------------------------ the game's own model conversion
   stream_channel 0x2C2CD0 (callers: the requested-model loader 0x2C3DF0, the synchronous loads 0x2C3F30 / 0x2C4150 /
   0x2C4230, 0x2C0330): when a channel's disc read is complete it converts the models in the buffer (model stream
   parsing, textures, collision, ...). Timed here minus the sources nested in it (a big model's second read, log
   lines, ...). */
void GUEST(002c2cd0)(R5900Ctx *c);
/* hitch_logstream=N (host HLE_HITCH_LOGSTREAM): log every conversion of N us or more - channel, the model ids in the
   channel (the channels at 0x7FF3B0 + ch x 0x98: model ids[16] +0, load status +0x80; the ids' ranges:
   < 20000 model, < 25000 texture dictionary, < 25255 collision, < 25511 placement, < 25575 data, < 25755 animation,
   < 25819 path, above: script) */

HLE(002c2cd0)
{
    uint32_t ch = ARG(0), base = 0x007FF3B0u + ch * 0x98u;
    uint32_t ls0 = ch < 8 ? gw(base + 0x80u) : 0;
    uint64_t t0 = hle_hitch_now_us(), o0 = 0, o1 = 0;
    for (int k = 0; k < HITCH_STREAM; k++) o0 += hle_hitch_us[k];
    uint32_t ids[16];
    for (int i = 0; i < 16; i++) ids[i] = ch < 8 ? gw(base + 4u * (uint32_t)i) : 0xFFFFFFFFu;
    GUEST(002c2cd0)(c);
    uint64_t dt = hle_hitch_now_us() - t0;
    for (int k = 0; k < HITCH_STREAM; k++) o1 += hle_hitch_us[k];
    uint64_t own = dt > o1 - o0 ? dt - (o1 - o0) : 0;
    hle_hitch_us[HITCH_STREAM] += own;
    st_stream_calls++;
    uint32_t ls1 = ch < 8 ? gw(base + 0x80u) : 0;
    if (ls0 != ls1 || own > 1000u) {
        st_stream_conv++;
        st_stream_us += own;
        if (own > st_stream_max_us) st_stream_max_us = own;
    }
#ifndef HLE_PSP
    if (hle_hitch_opt_logstream < 0) {
        const char *e = getenv("HLE_HITCH_LOGSTREAM");
        hle_hitch_opt_logstream = e && *e ? atoi(e) : 0;
    }
#endif
    if (hle_hitch_opt_logstream > 0 && own >= (uint64_t)hle_hitch_opt_logstream) {
        char b[400];
        int o = snprintf(b, sizeof b, "hitch stream: iteration %llu ch %u status %u->%u, %llu us:", (unsigned long long)hle_hitch_it_n,
                         ch, ls0, ls1, (unsigned long long)own);
        for (int i = 0; i < 16 && o < (int)sizeof b - 16; i++) {
            int32_t id = (int32_t)ids[i];
            if (id < 0) continue;
            const char *k = id < 20000 ? "model" : id < 25000 ? "texture" : id < 25255 ? "col" : id < 25511 ? "place"
                          : id < 25575 ? "dat" : id < 25755 ? "anim" : id < 25819 ? "path" : "script";
            o += snprintf(b + o, sizeof b - o, " %s%d", k, id < 20000 ? id : id < 25000 ? id - 20000 : id);
        }
        LOG("%s\n", b);
    }
}
