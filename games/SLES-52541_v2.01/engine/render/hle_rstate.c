/* hle_rstate.c - native answer for the render driver's "set render state" (drv_set_state: a0 = state id, a1 = value)
 * and "extended set" (drv_set_state_ext: a0 = case, a1 = value or pointer) entries.
 *
 * Clean-room module, written from a behavioural specification only. Each handled call makes the same changes
 * to the driver's state words (GS register images and driver flags, all addressed relative to gp) and returns the
 * same v0 (f0 for extended case 4) as the original, but appends no GS register data to the driver's DMA packet (the
 * GE back-end never consumes it). Calls not handled here return 0 and the caller runs the original.
 *
 * cfg key x_rsn (host env HLE_X_RSN): 0 = off, 1 = on (default), 2 = check mode: for every call this module would
 * handle, run it, save the words, restore them, run the original and compare (words + v0, + f0 for extended case 4);
 * the first differences are logged, the report prints the counts. cfg key x_rsr (env HLE_X_RSR, default 0): 1 = hand
 * rasters not yet set up by the driver's texture cache (type != 2, driver data word +88 == 0) to the original
 * (diagnosis; spec rev 1 behaviour). */
#include "hle.h"
#include "hle_fast.h"

/* ---- the driver state words: byte offsets from gp */
#define W_ALPHA_REF_SCALE (-32696)   /* float, state 30 only (not handled here) */
#define W_ZBUF (-14288)              /* 64: GS ZBUF image, bit 32 ZMSK */
#define W_TEST (-14272)              /* 64: GS TEST image */
#define W_FOGCOL (-14256)            /* 64: GS FOGCOL image */
#define W_CLAMP (-14248)             /* 64: GS CLAMP image */
#define W_TEX1 (-14240)              /* 64: GS TEX1 image */
#define W_ALPHA (-14232)             /* 64: GS ALPHA image */
#define W_FLAGS (-14216)             /* 64: driver flags */
#define W_BLEND_SRC (-14208)
#define W_BLEND_DST (-14204)
#define W_EXT_MODE (-14200)
#define W_MODE_BYTE (-14128)         /* 8: bit 0 fog on, bit 5 culling off */
#define W_PLUGIN_OFS 4376            /* read-only: raster -> driver data offset */
#define W_RASTER 4384
#define W_RASTER_ALPHA 4388
#define W_VERTEX_ALPHA 4392
#define W_CULL 4396
#define W_FOG_TABLE 4404
#define W_EXT1_ENABLE 4416           /* read-only */
#define W_EXT1_VALUE 4420
#define W_EXT4_A 4464
#define W_EXT4_B 4468

#define BLEND_TABLE 0x00601488u      /* guest data: 6 x 6 signed 32-bit ALPHA images, (S, D) at +4 S + 24 D */
#define FOG_TABLE_DEFAULT 0x0082AFD0u

/* FLAGS bits */
#define F_SHADE (1u << 3)
#define F_TEX (1u << 4)
#define F_FOG (1u << 5)
#define F_ABLEND (1u << 6)
#define F_PERSP (1u << 8)

static uint32_t gp_;
static int rsn = 1, rsr = 0, rsn_gen = -1;
static inline uint32_t A(int off) { return gp_ + (uint32_t)off; }
static inline uint32_t rd(int off) { return gw(A(off)); }
static inline void wr(int off, uint32_t v) { sw_(A(off), v); }
static inline uint32_t rd_hi(int off) { return gw(A(off) + 4); }
static inline void wr_hi(int off, uint32_t v) { sw_(A(off) + 4, v); }
static inline void wr64(int off, uint32_t lo, uint32_t hi) { wr(off, lo); wr_hi(off, hi); }
static inline void setbit(int off, uint32_t m, int on) { uint32_t w = rd(off); wr(off, on ? (w | m) : (w & ~m)); }

/* FLAGS bit 6 := (VERTEX_ALPHA or RASTER_ALPHA) != 0 */
static void upd_ablend(void) { setbit(W_FLAGS, F_ABLEND, (rd(W_VERTEX_ALPHA) | rd(W_RASTER_ALPHA)) != 0); }

/* state 1: texture raster. 1 = handled (v0 set), 0 = the original sets the raster up (x_rsr=1 only) */
static int st_raster(R5900Ctx *c, uint32_t v)
{
    if (v == 0) {
        wr(W_RASTER, 0);
        wr(W_RASTER_ALPHA, 0);
        setbit(W_FLAGS, F_TEX, 0);
        setbit(W_FLAGS, F_ABLEND, rd(W_VERTEX_ALPHA) != 0);
        setbit(W_TEST, 1u, 0);
        RET(1);
        return 1;
    }
    uint32_t d = v + rd(W_PLUGIN_OFS);
    uint32_t type = gb(v + 32) & 7u, fmt = gb(v + 35) & 0xFu;
    if (rsr && type != 2 && gw(d + 88) == 0) return 0;
    wr(W_RASTER, v);
    uint32_t h = gh(d + 20);
    uint32_t lo = rd(W_TEX1), hi = rd_hi(W_TEX1);
    lo &= ~((1u << 2) | (1u << 3) | (1u << 4) | (1u << 9) | (1u << 19) | (1u << 20));
    lo |= ((h >> 12) & 3u) << 19;                              /* L */
    hi = (hi & ~0xFFFu) | (h & 0xFFFu);                        /* K */
    lo |= gb(d + 22);
    wr64(W_TEX1, lo, hi);
    setbit(W_FLAGS, F_TEX, 1);
    uint32_t ra = fmt != 6;
    wr(W_RASTER_ALPHA, ra);
    upd_ablend();
    setbit(W_TEST, 1u, ra);
    RET(1);
    return 1;
}

/* states 10 / 11: blend factor codes and the ALPHA image from the guest's table */
static void st_blend(R5900Ctx *c, int dst, uint32_t v)
{
    int code;
    switch (v) {
    case 1: code = 0; break;
    case 2: code = 1; break;
    case 5: code = 2; break;
    case 6: code = 3; break;
    case 7: code = 4; break;
    case 8: code = 5; break;
    default: RET(0); return;
    }
    wr(dst ? W_BLEND_DST : W_BLEND_SRC, (uint32_t)code);
    uint32_t s = rd(W_BLEND_SRC), d = rd(W_BLEND_DST);
    int32_t e = (int32_t)gw(BLEND_TABLE + 4u * s + 24u * d);
    wr64(W_ALPHA, (uint32_t)e, e < 0 ? 0xFFFFFFFFu : 0u);
    if (e < 0) { RET(0); return; }
    if (s == 1 && d == 1) wr_hi(W_ALPHA, rd_hi(W_ALPHA) | (1u << 7));   /* bit 39 */
    RET(1);
}

/* drv_set_state: 1 = handled (v0 set), 0 = not handled (nothing changed) */
static int native_set(R5900Ctx *c, uint32_t s, uint32_t v)
{
    static const uint16_t filt[7] = {0, 0x000, 0x060, 0x080, 0x120, 0x0C0, 0x160};
    static const uint8_t atst[9] = {0, 0x1, 0x5, 0x9, 0x7, 0xD, 0xF, 0xB, 0x2};
    uint32_t w;
    switch (s) {
    case 1:
        return st_raster(c, v);
    case 2:
        if (v == 1) wr64(W_CLAMP, 0, 0);
        else if (v == 3) wr64(W_CLAMP, 5, 0);
        else { RET(0); return 1; }
        RET(1);
        return 1;
    case 3:
    case 4: {
        uint32_t sh = s == 3 ? 0 : 2;
        w = rd(W_CLAMP);
        if (v == 1) w &= ~(3u << sh);
        else if (v == 3) w |= 1u << sh;
        else { RET(0); return 1; }
        wr(W_CLAMP, w);
        RET(1);
        return 1;
    }
    case 5:
        if (v == 0) { RET(0); return 1; }
        setbit(W_FLAGS, F_PERSP, 0);
        RET(1);
        return 1;
    case 6:
        wr(W_TEST, (rd(W_TEST) & ~(7u << 16)) | ((v == 0 ? 3u : 5u) << 16));
        RET(1);
        return 1;
    case 7:
        if (v == 1) setbit(W_FLAGS, F_SHADE, 0);
        else if (v == 2) setbit(W_FLAGS, F_SHADE, 1);
        else { RET(0); return 1; }
        RET(1);
        return 1;
    case 8:
        if (v == 0) wr_hi(W_ZBUF, rd_hi(W_ZBUF) | 1u);
        else {
            wr_hi(W_ZBUF, rd_hi(W_ZBUF) & ~1u);
            w = rd(W_TEST);
            if (!(w & (1u << 16))) wr(W_TEST, (w & ~(7u << 16)) | (3u << 16));
        }
        RET(1);
        return 1;
    case 9:
        if (v < 1 || v > 6) { RET(0); return 1; }
        wr(W_TEX1, (rd(W_TEX1) & ~0x1E0u) | filt[v]);
        RET(1);
        return 1;
    case 10:
    case 11:
        st_blend(c, s == 11, v);
        return 1;
    case 12:
        wr(W_VERTEX_ALPHA, v != 0);
        upd_ablend();
        RET(1);
        return 1;
    case 14:
        setbit(W_FLAGS, F_FOG, v != 0);
        w = gb(A(W_MODE_BYTE));
        sb_(A(W_MODE_BYTE), (uint8_t)(v != 0 ? (w | 1u) : (w & ~1u)));
        RET(1);
        return 1;
    case 15:
        wr64(W_FOGCOL, ((v >> 16) & 0xFFu) | (((v >> 8) & 0xFFu) << 8) | ((v & 0xFFu) << 16), 0);
        RET(1);
        return 1;
    case 16:
        if (v != 1) { RET(0); return 1; }
        wr(W_FOG_TABLE, FOG_TABLE_DEFAULT);
        RET(1);
        return 1;
    case 20:
        w = gb(A(W_MODE_BYTE));
        sb_(A(W_MODE_BYTE), (uint8_t)(v == 1 ? (w & ~0x20u) : (w | 0x20u)));
        wr(W_CULL, v);
        RET(1);
        return 1;
    case 29:
        if (v < 1 || v > 8) { RET(0); return 1; }
        w = (rd(W_TEST) & ~0xFu) | atst[v];
        if (w & 1u) w &= ~(3u << 12);
        wr(W_TEST, w);
        RET(1);
        return 1;
    case 30:
        return 0;                                  /* FPU conversion with ALPHA_REF_SCALE: the original */
    default:                                       /* 0, 13, 17-19, 21-28, >= 31: no effect */
        RET(0);
        return 1;
    }
}

/* drv_set_state_ext: always handled */
static int native_ext(R5900Ctx *c, uint32_t k, uint32_t a)
{
    uint32_t x;
    switch (k) {
    case 1:
        if (rd(W_EXT1_ENABLE) == 0) { RET(0); return 1; }
        wr(W_EXT1_VALUE, a == 0 ? 0 : a == 0x55555555u ? 0x55555555u : 1u);
        RET(1);
        return 1;
    case 2:
        wr64(W_ALPHA, a & 0xFFu, (a >> 24) & 0xFFu);
        RET(1);
        return 1;
    case 3:
        wr(W_TEST, (rd(W_TEST) & ~0xFFFFu) | (a & 0xFFFFu));
        RET(1);
        return 1;
    case 4:
        x = gw(a);
        wr(W_EXT4_A, 0);
        wr(W_EXT4_B, x);
        c->fpr[0].u = x;
        RET(1);
        return 1;
    case 5:
        x = gw(a);
        if (x < 1 || x > 7) { RET(0); return 1; }
        wr(W_EXT_MODE, x);
        RET(1);
        return 1;
    default:
        RET(0);
        return 1;
    }
}

/* ---- check mode: snapshot / restore / compare of the words this module may write */
static const struct { int off; uint8_t size; const char *name; } words[] = {
    {W_ZBUF, 4, "ZBUF.lo"}, {W_ZBUF + 4, 4, "ZBUF.hi"}, {W_TEST, 4, "TEST.lo"}, {W_TEST + 4, 4, "TEST.hi"},
    {W_FOGCOL, 4, "FOGCOL.lo"}, {W_FOGCOL + 4, 4, "FOGCOL.hi"}, {W_CLAMP, 4, "CLAMP.lo"}, {W_CLAMP + 4, 4, "CLAMP.hi"},
    {W_TEX1, 4, "TEX1.lo"}, {W_TEX1 + 4, 4, "TEX1.hi"}, {W_ALPHA, 4, "ALPHA.lo"}, {W_ALPHA + 4, 4, "ALPHA.hi"},
    {W_FLAGS, 4, "FLAGS.lo"}, {W_FLAGS + 4, 4, "FLAGS.hi"}, {W_BLEND_SRC, 4, "BLEND_SRC"},
    {W_BLEND_DST, 4, "BLEND_DST"}, {W_EXT_MODE, 4, "EXT_MODE"}, {W_MODE_BYTE, 1, "MODE_BYTE"},
    {W_RASTER, 4, "RASTER"}, {W_RASTER_ALPHA, 4, "RASTER_ALPHA"}, {W_VERTEX_ALPHA, 4, "VERTEX_ALPHA"},
    {W_CULL, 4, "CULL"}, {W_FOG_TABLE, 4, "FOG_TABLE"}, {W_EXT1_VALUE, 4, "EXT1_VALUE"}, {W_EXT4_A, 4, "EXT4_A"},
    {W_EXT4_B, 4, "EXT4_B"},
};
#define NW ((int)(sizeof words / sizeof words[0]))

typedef struct { uint32_t w[NW]; uint64_t v0; uint32_t f0; } Snap;

static void snap(R5900Ctx *c, Snap *s)
{
    for (int i = 0; i < NW; i++) s->w[i] = words[i].size == 1 ? gb(A(words[i].off)) : rd(words[i].off);
    s->v0 = c->r[2].d[0];
    s->f0 = c->fpr[0].u;
}

static void restore(R5900Ctx *c, const Snap *s)
{
    for (int i = 0; i < NW; i++) {
        if (words[i].size == 1) sb_(A(words[i].off), (uint8_t)s->w[i]);
        else wr(words[i].off, s->w[i]);
    }
    c->r[2].d[0] = s->v0;
    c->fpr[0].u = s->f0;
}

static uint64_t n_native, n_orig, n_check, n_diff, n_ext_native, n_ext_check;
static uint64_t n_orig_s1, n_orig_s30;

static int mode(void)
{
    if (rsn_gen != hle_x_gen) { rsn = hle_x_opt("rsn", 1); rsr = hle_x_opt("rsr", 0); rsn_gen = hle_x_gen; }
    return rsn;
}

/* compare the native result (nat) with what the original left; log the first differences */
static void compare(R5900Ctx *c, const char *what, uint32_t s, uint32_t v, const Snap *nat, int cmp_f0)
{
    Snap o;
    snap(c, &o);
    int bad = 0;
    for (int i = 0; i < NW; i++) {
        if (nat->w[i] == o.w[i]) continue;
        if (n_diff + bad < 8)
            LOG("rstate check: %s %u value %08x: word %s native %08x original %08x\n", what, (unsigned)s, (unsigned)v,
                words[i].name, (unsigned)nat->w[i], (unsigned)o.w[i]);
        bad++;
    }
    if (nat->v0 != o.v0) {
        if (n_diff + bad < 8)
            LOG("rstate check: %s %u value %08x: word v0 native %08x original %08x\n", what, (unsigned)s,
                (unsigned)v, (unsigned)(uint32_t)nat->v0, (unsigned)(uint32_t)o.v0);
        bad++;
    }
    if (cmp_f0 && nat->f0 != o.f0) {
        if (n_diff + bad < 8)
            LOG("rstate check: %s %u value %08x: word f0 native %08x original %08x\n", what, (unsigned)s,
                (unsigned)v, (unsigned)nat->f0, (unsigned)o.f0);
        bad++;
    }
    if (bad) n_diff++;
}

int hle_rstate_set(R5900Ctx *c, uint32_t s, uint32_t v, void (*orig)(R5900Ctx *))
{
    int m = mode();
    if (m == 0) return 0;
    gp_ = (uint32_t)c->r[28].d[0];
    Snap before, nat;
    if (m == 2) snap(c, &before);
    if (!native_set(c, s, v)) {
        n_orig++;
        if (s == 1) n_orig_s1++;
        else if (s == 30) n_orig_s30++;
        return 0;
    }
    if (m != 2) { n_native++; return 1; }
    snap(c, &nat);
    restore(c, &before);
    orig(c);
    gp_ = (uint32_t)c->r[28].d[0];
    compare(c, "state", s, v, &nat, 0);
    n_check++;
    return 2;
}

int hle_rstate_set_ext(R5900Ctx *c, void (*orig)(R5900Ctx *))
{
    int m = mode();
    if (m == 0) return 0;
    uint32_t k = ARG(0), a = ARG(1);
    gp_ = (uint32_t)c->r[28].d[0];
    Snap before, nat;
    if (m == 2) snap(c, &before);
    if (!native_ext(c, k, a)) return 0;
    if (m != 2) { n_ext_native++; return 1; }
    snap(c, &nat);
    restore(c, &before);
    orig(c);
    gp_ = (uint32_t)c->r[28].d[0];
    compare(c, "ext case", k, a, &nat, k == 4);
    n_ext_check++;
    return 2;
}

void hle_rstate_report(void)
{
    LOG("render state native (x_rsn=%d, x_rsr=%d): set %llu native, %llu original (state 1 raster setup %llu, "
        "state 30 %llu), ext %llu native\n", rsn, rsr, (unsigned long long)n_native, (unsigned long long)n_orig,
        (unsigned long long)n_orig_s1, (unsigned long long)n_orig_s30, (unsigned long long)n_ext_native);
    if (n_check || n_ext_check)
        LOG("render state check: %llu set + %llu ext calls compared, %llu with differences\n",
            (unsigned long long)n_check, (unsigned long long)n_ext_check, (unsigned long long)n_diff);
}
