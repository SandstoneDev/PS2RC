/* audio_stretch.c - see audio_stretch.h. */
#include "audio_stretch.h"

#if defined(__GNUC__)
#define AS_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define AS_BARRIER() do { } while (0)
#endif

#define AS_FINE (AS_DEC - 1u)                     /* fine search: +-7 frames around the best coarse candidate */
#define AS_NC   ((2u * AS_SEEK) / AS_DEC + 1u)    /* coarse candidates: 65 */
#define AS_NT   (AS_CORR / AS_DEC)                /* compared points per candidate: 48 */

static int32_t clamp16(int32_t x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }
static int after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }   /* counter a later than b (wrap-safe) */

uint32_t as_resample48(AsResampler *rs, const int16_t *in, uint32_t n, int16_t *out)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t cl = in[2u * i], cr = in[2u * i + 1u];
        while (rs->pos < 65536u) {                  /* output frames between the previous input frame and this one */
            int32_t f = (int32_t)rs->pos;
            out[2u * k] = (int16_t)(rs->prev[0] + (int32_t)(((int64_t)(cl - rs->prev[0]) * f) >> 16));
            out[2u * k + 1u] = (int16_t)(rs->prev[1] + (int32_t)(((int64_t)(cr - rs->prev[1]) * f) >> 16));
            k++;
            rs->pos += AS_RS_STEP;
        }
        rs->pos -= 65536u;
        rs->prev[0] = cl;
        rs->prev[1] = cr;
    }
    return k;
}

void as_ring_init(AsRing *r, int16_t *buf, uint32_t cap_frames)
{
    r->buf = buf;
    r->cap = cap_frames;
    r->mask = cap_frames - 1u;
    r->wr = 0;
    r->rd = 0;
    r->lost = 0;
    for (uint32_t i = 0; i < 2u * cap_frames; i++) buf[i] = 0;
}

uint32_t as_ring_write(AsRing *r, const int16_t *src, uint32_t n)
{
    uint32_t wr = r->wr, rd = r->rd;
    uint32_t used = wr - rd, room = used < r->cap ? r->cap - used : 0u;
    if (n > room) {
        r->lost += n - room;
        n = room;
    }
    int16_t *b = r->buf;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (wr + i) & r->mask;
        b[2u * k] = src[2u * i];
        b[2u * k + 1u] = src[2u * i + 1u];
    }
    AS_BARRIER();                                 /* the samples before the counter (single core: program order) */
    r->wr = wr + n;
    return n;
}

void as_init(AudioStretch *a, AsRing *r, int mode, uint32_t target_frames, uint32_t smax_q16)
{
    uint8_t *p = (uint8_t *)a;
    for (uint32_t i = 0; i < (uint32_t)sizeof *a; i++) p[i] = 0;
    a->ring = r;
    a->mode = mode;
    uint32_t tmin = AS_N + AS_SEEK + AS_HOP;
    a->target = target_frames < tmin ? tmin : target_frames;
    if (a->target > r->cap / 4u) a->target = r->cap / 4u > tmin ? r->cap / 4u : tmin;
    a->smax = smax_q16 < AS_ONE ? AS_ONE : smax_q16 > 8u * AS_ONE ? 8u * AS_ONE : smax_q16;
    a->smin = AS_ONE * 3u / 4u;
    a->p = AS_ONE;
    a->s = AS_ONE;
    a->sum_in = a->sum_out = 16u * AS_HOP;      /* start at p = 1, light: the first seconds' data dominate soon */
    a->nom = r->wr;
    a->last_wr = r->wr;
    a->st.s_min = 0xFFFFFFFFu;
}

uint32_t as_fill(const AudioStretch *a)
{
    uint32_t f = a->ring->wr - a->nom;
    return f > 0x80000000u ? 0u : f;
}

static int32_t mono(const AsRing *r, uint32_t k)
{
    const int16_t *p = r->buf + 2u * (k & r->mask);
    return ((int32_t)p[0] + (int32_t)p[1]) >> 1;
}

/* the frames below `keep` may be overwritten by the producer (monotonic) */
static void release(AudioStretch *a, uint32_t keep)
{
    if (after(keep, a->ring->rd)) a->ring->rd = keep;
}

static void silence(int16_t *out)
{
    for (uint32_t i = 0; i < 2u * AS_HOP; i++) out[i] = 0;
}

/* start of the grain that best continues the previous one: AMDF (sum of |difference|) of the decimated mono signal,
   coarse over +-AS_SEEK in steps of AS_DEC, then +-AS_FINE frames around the best coarse candidate */
static uint32_t seek_best(AudioStretch *a, uint32_t nom, uint32_t tgt)
{
    const AsRing *r = a->ring;
    uint32_t lo = nom - AS_SEEK;
    for (uint32_t i = 0; i < AS_NT; i++) a->dm_t[i] = (int16_t)mono(r, tgt + i * AS_DEC);
    for (uint32_t i = 0; i < AS_NC + AS_NT; i++) a->dm_c[i] = (int16_t)mono(r, lo + i * AS_DEC);
    uint32_t best_j = AS_SEEK / AS_DEC, best = 0xFFFFFFFFu;
    for (uint32_t j = 0; j < AS_NC; j++) {
        uint32_t sum = 0;
        const int16_t *c = a->dm_c + j;
        for (uint32_t i = 0; i < AS_NT && sum < best; i++) {
            int32_t d = (int32_t)c[i] - (int32_t)a->dm_t[i];
            sum += (uint32_t)(d < 0 ? -d : d);
        }
        if (sum < best) { best = sum; best_j = j; }
    }
    uint32_t c0 = lo + best_j * AS_DEC, cbest = c0;
    for (int32_t f = -(int32_t)AS_FINE; f <= (int32_t)AS_FINE; f++) {
        if (f == 0) continue;
        int32_t off = (int32_t)(best_j * AS_DEC) + f;
        if (off < 0 || off > (int32_t)(2u * AS_SEEK)) continue;
        uint32_t c = c0 + (uint32_t)f, sum = 0;
        for (uint32_t i = 0; i < AS_NT && sum < best; i++) {
            int32_t d = mono(r, c + i * AS_DEC) - (int32_t)a->dm_t[i];
            sum += (uint32_t)(d < 0 ? -d : d);
        }
        if (sum < best) { best = sum; cbest = c; }
    }
    return cbest;
}

/* the controlled quantity is the LOW-WATERMARK of the fill (its value just before each burst arrives), not the fill
   itself: the game delivers its sound in bursts (one RPC per game frame, up to ~0.5 s at once on loading screens), so
   at steady state the fill saws between the low-watermark and low-watermark + one burst; keeping the low-watermark at
   the target leaves exactly the margin that bridges the gap to the next burst */
static uint32_t stretch_of(AudioStretch *a, uint32_t fill)
{
    if (a->mode == AS_GAP) return AS_ONE;
    fill = a->fill_low;
    uint32_t base;
    if (a->p >= AS_ONE * 95u / 100u) base = AS_ONE;                        /* real time (within 5 %) */
    else if ((uint64_t)a->p * a->smax < (uint64_t)AS_ONE * AS_ONE) base = a->smax;
    else base = (uint32_t)(((uint64_t)AS_ONE << 16) / a->p);                /* s = 1 / p */
    int64_t err = ((int64_t)a->target - (int64_t)fill) * 65536 / (int64_t)a->target;
    if (err > 65536) err = 65536;
    if (err < -65536) err = -65536;
    if (base == AS_ONE && err <= 32768 && err >= -32768) return AS_ONE;    /* dead band: exact pass-through */
    int64_t s = (int64_t)base + (int64_t)base * err / 131072;               /* base x (1 + err / 2) */
    if (s < (int64_t)a->smin) s = a->smin;
    if (s > (int64_t)a->smax) s = a->smax;
    return (uint32_t)s;
}

static void advance(AudioStretch *a, uint32_t s)
{
    uint64_t q = ((uint64_t)AS_HOP << 32) / s;      /* input frames of this hop, Q16 */
    uint32_t f = a->nom_frac + (uint32_t)(q & 0xFFFFu);
    uint32_t n = (uint32_t)(q >> 16) + (f >> 16);
    a->nom_frac = f & 0xFFFFu;
    a->nom += n;
    a->st.frames_in += n;
}

int as_hop(AudioStretch *a, int16_t *out, uint32_t elapsed)
{
    AsRing *r = a->ring;
    uint32_t wr = r->wr;
    AS_BARRIER();                                   /* the counter before the samples */
    a->st.hops++;
    /* production rate: input frames per output frame of real time = ratio of two decaying sums (~256 hops, 3 s: the game
       delivers its sound in bursts up to ~0.5 s apart), so one large burst counts in full */
    uint32_t d = wr - a->last_wr;
    a->last_wr = wr;
    if (elapsed > AS_RATE) elapsed = AS_RATE;    /* a long sleep counts as one second */
    a->sum_in += d;
    a->sum_in -= a->sum_in / 256u;
    a->sum_out += elapsed;
    a->sum_out -= a->sum_out / 256u;
    if (a->sum_out) {
        uint64_t p = (a->sum_in << 16) / a->sum_out;
        a->p = p > 16u * AS_ONE ? 16u * AS_ONE : (uint32_t)p;
    }
    uint32_t fill = as_fill(a);
    /* low-watermark = the fill just before each burst arrives (the trough of the saw), smoothed 1:1 with the previous
       one; while nothing arrives it follows the fill down */
    if (d) {
        uint32_t trough = fill > d ? fill - d : 0u;
        a->fill_low = a->playing ? (a->fill_low + trough) / 2u : trough;
    } else if (fill < a->fill_low) {
        a->fill_low = fill;
    }
    /* latency bound: the low-watermark above 4 x the target (the sound lags that much even before a burst), or the fill
       about to overrun the ring -> cut back to the target */
    uint32_t hard = r->cap - 2u * AS_N - AS_SEEK - AS_HOP;
    uint32_t cut = 0;
    if (a->playing && a->fill_low > 4u * a->target) cut = a->fill_low - a->target;
    if (fill > hard && fill - a->target > cut) cut = fill - a->target;
    if (cut) {
        a->nom += cut;
        a->st.drops++;
        a->st.frames_dropped += cut;
        fill -= cut;
        a->fill_low = a->fill_low > cut ? a->fill_low - cut : 0u;
    }
    if (!a->playing) {
        if (fill < a->target) {                     /* prebuffering (start, after an underrun) */
            silence(out);
            a->st.hops_silent++;
            release(a, a->nom - AS_SEEK - AS_HOP);
            return 0;
        }
        a->playing = 1;
        a->fill_low = a->target;                    /* start as if at the target (fill >= target here) */
        a->prev_src = a->nom - AS_HOP;              /* natural continuation = the nominal position */
        for (uint32_t i = 0; i < 2u * AS_HOP; i++) a->tail[i] = 0;   /* fade in */
    }
    uint32_t s = stretch_of(a, fill);
    const int16_t *b = r->buf;
    const uint32_t m = r->mask;
    int32_t peak = a->st.peak;
    if (a->mode == AS_PITCH) {
        uint32_t step = (uint32_t)(((uint64_t)AS_ONE << 16) / s);   /* input frames per output frame, Q16 */
        uint32_t need = (uint32_t)(((uint64_t)step * AS_HOP) >> 16) + 2u;
        if (fill < need) {                          /* starved */
            silence(out);
            a->playing = 0;
            a->st.underruns++;
            a->st.hops_silent++;
            return 0;
        }
        uint32_t k = a->nom, f = a->nom_frac;
        for (uint32_t i = 0; i < AS_HOP; i++) {
            for (uint32_t ch = 0; ch < 2u; ch++) {
                int32_t x0 = b[2u * (k & m) + ch], x1 = b[2u * ((k + 1u) & m) + ch];
                int32_t v = x0 + (int32_t)(((int64_t)(x1 - x0) * (int64_t)f) >> 16);
                out[2u * i + ch] = (int16_t)v;
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            f += step;
            k += f >> 16;
            f &= 0xFFFFu;
        }
        a->st.frames_in += k - a->nom;
        a->nom = k;
        a->nom_frac = f;
        release(a, a->nom - 1u);
    } else {
        if (fill < AS_N + AS_SEEK) {                /* starved: the previous grain fades out, then silence */
            for (uint32_t i = 0; i < 2u * AS_HOP; i++) {
                int32_t v = clamp16(a->tail[i] >> 15);
                out[i] = (int16_t)v;
                a->tail[i] = 0;
            }
            a->playing = 0;
            a->st.underruns++;
            a->st.hops_silent++;
            release(a, a->nom - AS_SEEK - AS_HOP);
            return 1;
        }
        uint32_t nom = a->nom, tgt = a->prev_src + AS_HOP;
        uint32_t src = (s == AS_ONE && tgt == nom) ? nom : seek_best(a, nom, tgt);
        if (s == AS_ONE && src == tgt) a->st.hops_unity++;
        /* overlap-add: the previous grain's fade-out + this grain's fade-in (weights i*64 and 32768 - i*64) */
        for (uint32_t i = 0; i < AS_HOP; i++) {
            int32_t w = (int32_t)(i * (32768u / AS_HOP));
            uint32_t k1 = 2u * ((src + i) & m), k2 = 2u * ((src + AS_HOP + i) & m);
            for (uint32_t ch = 0; ch < 2u; ch++) {
                int32_t v = clamp16((a->tail[2u * i + ch] + (int32_t)b[k1 + ch] * w) >> 15);
                out[2u * i + ch] = (int16_t)v;
                a->tail[2u * i + ch] = (int32_t)b[k2 + ch] * (32768 - w);
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
        }
        a->prev_src = src;
        advance(a, s);
        uint32_t keep = a->prev_src + AS_HOP, lo = a->nom - AS_SEEK;
        release(a, after(keep, lo) ? lo : keep);
    }
    a->st.peak = peak;
    a->s = s;
    a->st.hops_sound++;
    a->st.s_sum += s;
    if (s < a->st.s_min) a->st.s_min = s;
    if (s > a->st.s_max) a->st.s_max = s;
    a->st.fill_sum += fill;
    return 1;
}
