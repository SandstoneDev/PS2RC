/* hle_w3d_me.c - the 3D world's GE lists built on the Media Engine (design: hle_w3d_me.h). Three parts:
 *   1. the BUILDER (w3dme_poll and everything it calls): runs on the ME (no syscalls, no libc, no VFPU, no jump tables,
 *      no struct copies - a separate check script scans the object), or on a main-core thread / the host thread
 *      with sim = 1 (no cache operations). It reads the job stream, classifies chunks / bone groups, decides the state
 *      and writes GE words (PSP) or host operations into the arena, and the part list for the main core;
 *   2. the PSP ENCODER (enc_*): the GE words of what psp/hle_ge3d.c's send_global / send_proj / send_lights /
 *      send_state / world / bones / mesh / range and the texture bind (hle_ge.c apply_state) send - checked word for
 *      word against libpspgu on the main core at start-up (psp/hle_ge3d.c w3d_be_me_selftest; a mismatch keeps w3d_me
 *      off);
 *   3. the MAIN-CORE side: memory, the stream writer, the waits, the runners (the ME audio loop calls w3dme_poll in its
 *      idle branch; w3d_me_runner=2 = our own ME loop; 1 / the host = a thread).
 * The decisions are hle_w3d.c's / hle_w3d_lit.c's (atomic_meshes / lit_mesh after the lookup: chunk groups, bone
 * groups, lighting / emissive, UV base, alpha passes) written a second time for the ME; the host frames with w3d_me
 * 0 / 1 must be identical (the host tests compare them). */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "hle.h"
#include "hle_w3d_int.h"
#include "hle_w3d_me.h"
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)
#include "hle_me_jobs.h"                     /* the shared ME job interface */
#endif
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspgu.h>
#include "hle_ge.h"                                   /* hle_ge_mip_level */
#include <malloc.h>
#pragma GCC optimize("no-tree-loop-distribute-patterns", "no-jump-tables")
#define UNC(p) ((volatile uint32_t *)(0x40000000u | (uintptr_t)(p)))
#define UNCP(T, p) ((volatile T *)(0x40000000u | (uintptr_t)(p)))
#else
#include <pthread.h>
#include <unistd.h>
#define UNC(p) ((volatile uint32_t *)(uintptr_t)(p))
#define UNCP(T, p) ((volatile T *)(uintptr_t)(p))
#endif

/* the one word both cores read to find the control block: alone in its 64-byte line, always accessed uncached */
uint32_t w3dme_gaddr[16] __attribute__((aligned(64)));
/* the dither policy of the state encoder (psp/hle_ge.c ge_dither: 0 off, 1 opaque meshes only, 2 every
   draw but additive; 0 with ge_p32), set by hle_ge.c on the main core, sent to the ME in each batch's opts bits 24-25 */
int w3dme_dpol = 1;
#ifdef HLE_PSP
#define ME_CTL() ((volatile W3mCtl *)(uintptr_t)UNC(w3dme_gaddr)[0])
#define ME_PTR(c, f, T) ((T)(uintptr_t)(c)->f)            /* the ME reads the addresses from the control block */
#else
/* host: one process, 64-bit pointers - the test thread uses the main side's pointers */
static volatile W3mCtl *volatile g_host_ctl;
static void *g_host_stream, *g_host_parts, *g_host_arena, *g_host_priv;
#define ME_CTL() (g_host_ctl)
#define ME_PTR(c, f, T) ((T)g_host_##f)
#endif

#define W3M_GUARD_X 8.0f                    /* = hle_w3d.c GUARD_X / GUARD_Y / NEAR_SAFE */
#define W3M_GUARD_Y 9.5f
#define W3M_NEAR_SAFE 1.01f
#define W3M_CL_CULL 0
#define W3M_CL_HW 1
#define W3M_CL_CPU 2
#define W3M_CL_INSIDE 4
#define W3M_STATE_WORDS 18u                 /* sizeof(W3dState) / 4 */
_Static_assert(sizeof(W3dState) == 4u * W3M_STATE_WORDS, "W3dState words");
_Static_assert(sizeof(W3mTex) == 4u * W3M_TEX_WORDS, "W3mTex words");
_Static_assert(sizeof(W3mCtl) <= 256u, "W3mCtl");
_Static_assert(sizeof(LGroup) == 28u + 24u + 192u, "LGroup layout (hle_w3d_int.h)");

/* a camera of the batch: [0..15] projection (W3dCam.proj), 16 near, 17 far, 18 fog, 19 valid, [20..35] the lit
   projection (S x P), 36 has it */
#define CAMF 40

/* ------------------------------------------------------------------ 1. the builder (ME-safe) */
typedef struct {
    uint32_t batch, line, wpos, line_ok, finished, failed;
    uint32_t *stream;                       /* job lines (cached; each line invalidated before its first read) */
    volatile uint32_t *arena;               /* GE words / host operations (uncached on the PSP) */
    volatile W3mPart *parts;                /* (uncached on the PSP) */
    uint32_t apos, aend, nparts;            /* arena words */
    int seg_open, seg_cam, sent_ok, tex_ok, texon, world_ok, proj_custom, lit_setup;
    uint32_t seg_start, seg_rec, begun_rec;
    W3dState sent, st, rst;
    W3mTex tsent;
    uint32_t rec, rec_cam, rec_flags, rec_lit;
    float M[12], gm[12], wm[12];            /* classification matrix, the record's world matrix, the world to send */
    /* the lit / skinned record */
    float MV[12], W[12], peak;
    uint32_t nbones, bones;                 /* the palette (PSP: address; host: index into the bone pool) */
    float lamb[3], ldir[4][3], lcol[4][3];
    int ln;
    float cam[W3M_CAMS][CAMF];
    uint32_t opts, group, vlo, vhi;
    uint32_t gtmpl_n;
    uint32_t gtmpl[64];
    uint32_t item[W3M_ITEM_MAX + 1];
    uint32_t n_meshes, n_cull, n_main, n_segs, n_words, n_stale, n_ranges, n_twopass, n_units, n_lmeshes;
    uint32_t burst, sim, n_wb, astart;      /* w3d_me_bus BURST for this batch; sim; lines written back */
    uint8_t chcls[W3M_MAXCH];
    uint8_t gcls[W3M_MAXGROUPS];
} W3mPriv;

static inline void wcp(void *d, const void *s, uint32_t n)
{
    uint32_t *a = (uint32_t *)d;
    const uint32_t *b = (const uint32_t *)s;
    for (uint32_t i = 0; i < n; i++) a[i] = b[i];
}
static inline int wne(const void *d, const void *s, uint32_t n)
{
    const uint32_t *a = (const uint32_t *)d, *b = (const uint32_t *)s;
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 1;
    return 0;
}
static inline uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }
static inline float bitsf(uint32_t u) { union { float f; uint32_t u; } x; x.u = u; return x.f; }
static inline float me_fabs(float f) { return __builtin_fabsf(f); }

/* hle_w3d.c w3d_classify_box, the same arithmetic in the same order; m = M43 rows (right, up, at, pos) */
static int me_classify_box(const float *bmin, const float *bmax, const float *m, float near_z, float far_z, int gnear)
{
    float c[3], e[3][3];
    float cx = (bmin[0] + bmax[0]) * 0.5f, cy = (bmin[1] + bmax[1]) * 0.5f, cz = (bmin[2] + bmax[2]) * 0.5f;
    for (int k = 0; k < 3; k++) c[k] = cx * m[0 + k] + cy * m[3 + k] + cz * m[6 + k] + m[9 + k];
    for (int a = 0; a < 3; a++) {
        float h = (bmax[a] - bmin[a]) * 0.5f;
        for (int k = 0; k < 3; k++) e[a][k] = m[3 * a + k] * h;
    }
    const float X = c[0], Y = c[1], Z = c[2];
    const float x0 = e[0][0], y0 = e[0][1], z0 = e[0][2], x1 = e[1][0], y1 = e[1][1], z1 = e[1][2];
    const float x2 = e[2][0], y2 = e[2][1], z2 = e[2][2];
    const float rx = me_fabs(x0) + me_fabs(x1) + me_fabs(x2), ry = me_fabs(y0) + me_fabs(y1) + me_fabs(y2);
    const float rz = me_fabs(z0) + me_fabs(z1) + me_fabs(z2);
    float v, r;
    int fr_in = 1;
    v = Z - X; r = me_fabs(z0 - x0) + me_fabs(z1 - x1) + me_fabs(z2 - x2);
    if (v + r < 0.0f) return W3M_CL_CULL;
    fr_in &= v - r >= 0.0f;
    if (X + rx < 0.0f) return W3M_CL_CULL;
    fr_in &= X - rx >= 0.0f;
    if (Y + ry < 0.0f) return W3M_CL_CULL;
    fr_in &= Y - ry >= 0.0f;
    v = Z - Y; r = me_fabs(z0 - y0) + me_fabs(z1 - y1) + me_fabs(z2 - y2);
    if (v + r < 0.0f) return W3M_CL_CULL;
    fr_in &= v - r >= 0.0f;
    v = Z - near_z;
    if (v + rz < 0.0f) return W3M_CL_CULL;
    fr_in &= v - rz >= 0.0f;
    v = far_z - Z;
    if (v + rz < 0.0f) return W3M_CL_CULL;
    fr_in &= v - rz >= 0.0f;
    if (fr_in && gnear) return W3M_CL_HW | W3M_CL_INSIDE;
    const float gxp = W3M_GUARD_X + 1.0f, gxm = W3M_GUARD_X - 1.0f, gyp = W3M_GUARD_Y + 1.0f, gym = W3M_GUARD_Y - 1.0f;
    v = gxp * Z - 2.0f * X;
    r = me_fabs(gxp * z0 - 2.0f * x0) + me_fabs(gxp * z1 - 2.0f * x1) + me_fabs(gxp * z2 - 2.0f * x2);
    if (v - r < 0.0f) return W3M_CL_CPU;
    v = 2.0f * X + gxm * Z;
    r = me_fabs(2.0f * x0 + gxm * z0) + me_fabs(2.0f * x1 + gxm * z1) + me_fabs(2.0f * x2 + gxm * z2);
    if (v - r < 0.0f) return W3M_CL_CPU;
    v = 2.0f * Y + gym * Z;
    r = me_fabs(2.0f * y0 + gym * z0) + me_fabs(2.0f * y1 + gym * z1) + me_fabs(2.0f * y2 + gym * z2);
    if (v - r < 0.0f) return W3M_CL_CPU;
    v = gyp * Z - 2.0f * Y;
    r = me_fabs(gyp * z0 - 2.0f * y0) + me_fabs(gyp * z1 - 2.0f * y1) + me_fabs(gyp * z2 - 2.0f * y2);
    if (v - r < 0.0f) return W3M_CL_CPU;
    if (!gnear && Z - near_z * W3M_NEAR_SAFE - rz < 0.0f) return W3M_CL_CPU;
    return W3M_CL_HW;
}

static int me_classify_chunk(const int16_t *bmin, const int16_t *bmax, const float *m, float near_z, float far_z,
                             int gnear)
{
    float mn[3], mx[3];
    const float scale = 1.0f / 128.0f;
    for (int i = 0; i < 3; i++) { mn[i] = bmin[i] * scale; mx[i] = bmax[i] * scale; }
    return me_classify_box(mn, mx, m, near_z, far_z, gnear) & 3;
}

/* psp/hle_ge3d.c rgb8: a 0..1 colour as R | G << 8 | B << 16 */
static uint32_t me_rgb8(const float *c)
{
    uint32_t o = 0;
    for (int i = 0; i < 3; i++) {
        float v = c[i] * 255.0f + 0.5f;
        o |= (uint32_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int)v) << (8 * i);
    }
    return o;
}

/* ---- the encoder (PSP: GE words exactly as libpspgu / hle_ge3d.c / hle_ge.c send them) */
#ifdef HLE_PSP
#define GC(c, a) ((uint32_t)(c) << 24 | ((uint32_t)(a) & 0xFFFFFFu))
#define GF(c, f) ((uint32_t)(c) << 24 | fbits(f) >> 8)
#define GEN(c, on) GC(c, (on) ? 1 : 0)       /* sceGuEnable / sceGuDisable of a plain state */

uint32_t *w3dme_enc_proj(uint32_t *o, const float *p)
{
    *o++ = GF(0x3E, 0.0f);
    for (int i = 0; i < 16; i++) *o++ = GF(0x3F, p[i]);
    return o;
}

uint32_t *w3dme_enc_world(uint32_t *o, const float *gm)
{
    *o++ = GC(0x3A, 0);
    for (int i = 0; i < 12; i++) *o++ = GF(0x3B, gm[i]);
    return o;
}

/* hle_ge3d.c send_lights: per light sceGuLight(i, GU_DIRECTIONAL, GU_DIFFUSE, dir) + its diffuse colour + ambient 0 */
uint32_t *w3dme_enc_lights(uint32_t *o, const W3dLights *l)
{
    for (int i = 0; i < l->n && i < 4; i++) {
        *o++ = GF(0x63 + 3 * i, l->dir[i][0]);
        *o++ = GF(0x64 + 3 * i, l->dir[i][1]);
        *o++ = GF(0x65 + 3 * i, l->dir[i][2]);
        *o++ = GC(0x5F + i, (GU_DIRECTIONAL << 8) & 0x300);    /* type, GU_DIFFUSE = the plain diffuse model */
        *o++ = GC(0x90 + 3 * i, me_rgb8(l->col[i]));           /* sceGuLightColor(i, GU_DIFFUSE, ...) */
        *o++ = GC(0x8F + 3 * i, 0);                             /* sceGuLightColor(i, GU_AMBIENT, 0) */
    }
    return o;
}

/* hle_ge3d.c w3d_be_bones: one BOFS, then 12 BONE words per matrix */
uint32_t *w3dme_enc_bones(uint32_t *o, int n, const float (*m)[12])
{
    if (n > 8) n = 8;
    *o++ = GC(42, 0);
    for (int i = 0; i < n; i++)
        for (int q = 0; q < 12; q++) *o++ = GC(43, fbits(m[i][q]) >> 8);
    return o;
}

/* hle_ge.c hle_ge_3d_blend -> set_blend; dpol = the dither policy (hle_ge.c dith_want with ge3d_active) */
uint32_t *w3dme_enc_blend(uint32_t *o, int on, int sb, int db, int dpol)
{
    if (!on) {
        *o++ = GEN(0x21, 0);                                   /* sceGuDisable(GU_BLEND) */
        *o++ = GEN(0x20, dpol != 0);                           /* GU_DITHER (was always on) */
        return o;
    }
    int a = GU_FIX, b = GU_FIX;
    unsigned fa = 0, fb = 0;
    if (sb == 1) fa = 0;
    else if (sb == 2 || sb == 7) fa = 0xFFFFFF;
    else if (sb == 8) fa = 0;
    else if (sb == 5 || sb == 11) a = GU_SRC_ALPHA;
    else if (sb == 6) a = GU_ONE_MINUS_SRC_ALPHA;
    else if (sb == 9) a = GU_OTHER_COLOR;
    else if (sb == 10) a = GU_ONE_MINUS_OTHER_COLOR;
    else a = GU_SRC_ALPHA;
    if (db == 1) fb = 0;
    else if (db == 2 || db == 7) fb = 0xFFFFFF;
    else if (db == 8) fb = 0;
    else if (db == 3) b = GU_OTHER_COLOR;
    else if (db == 4) b = GU_ONE_MINUS_OTHER_COLOR;
    else if (db == 5 || db == 11) b = GU_SRC_ALPHA;
    else if (db == 6) b = GU_ONE_MINUS_SRC_ALPHA;
    else b = GU_ONE_MINUS_SRC_ALPHA;
    if (sb == 2 && db == 1) *o++ = GEN(0x21, 0);
    else {
        *o++ = GEN(0x21, 1);
        *o++ = GC(0xDF, (uint32_t)a | (uint32_t)b << 4 | (uint32_t)GU_ADD << 8);   /* sceGuBlendFunc */
        *o++ = GC(0xE0, fa);
        *o++ = GC(0xE1, fb);
    }
    *o++ = GEN(0x22, sb == 5 || sb == 11);                     /* alpha test */
    *o++ = GEN(0x20, dpol == 2 ? db != 2 : dpol == 1 && sb == 2 && db == 1);   /* dither (policy) */
    return o;
}

/* psp/hle_ge3d.c send_state; prev = what was sent (all = 1: nothing); amb = the game's ambient light of the lit pipelines'
   lights (sent with every lit state, as send_state does); front: bit 0 = w3d_front, bits 1-2 = the dither policy
   (w3dme_dpol) */
uint32_t *w3dme_enc_state(uint32_t *o, const W3dState *st, const W3dState *prev, int all, float far_z, float fog_z,
                          int front, const float *amb)
{
    const W3dState *sn = prev;
    if (!all && !wne(sn, st, W3M_STATE_WORDS)) return o;
    if (all || st->ztest != sn->ztest || st->zwrite != sn->zwrite) {
        if (st->ztest || st->zwrite) {
            *o++ = GEN(0x23, 1);
            *o++ = GC(0xDE, st->ztest == 2 ? GU_GREATER : st->ztest ? GU_GEQUAL : GU_ALWAYS);   /* 2: GREATER */
        } else
            *o++ = GEN(0x23, 0);
        *o++ = GC(0xE7, st->zwrite ? GU_FALSE : GU_TRUE);
    }
    if (all || st->cull != sn->cull) {
        if (st->cull) {
            int ccw_front = !(front & 1);
            if (st->cull == 2) ccw_front = !ccw_front;
            *o++ = GC(0x9B, (ccw_front ? GU_CCW : GU_CW) ? 0 : 1);     /* sceGuFrontFace */
            *o++ = GEN(0x1D, 1);
        } else
            *o++ = GEN(0x1D, 0);
    }
    if (all || st->blend != sn->blend || st->src_blend != sn->src_blend || st->dst_blend != sn->dst_blend ||
        st->atest != sn->atest) {
        o = w3dme_enc_blend(o, st->blend, st->src_blend, st->dst_blend, front >> 1 & 3);
        if (st->atest == 1) { *o++ = GEN(0x22, 1); *o++ = GC(0xDB, GU_GREATER | 0 << 8 | 0xFF << 16); }
        else if (st->atest == 2) { *o++ = GEN(0x22, 1); *o++ = GC(0xDB, GU_GEQUAL | 0x80 << 8 | 0xFF << 16); }
        else if (st->atest == 3) { *o++ = GEN(0x22, 1); *o++ = GC(0xDB, GU_LESS | 0x80 << 8 | 0xFF << 16); }
        else *o++ = GEN(0x22, 0);
    }
    int fog = st->fog && far_z > fog_z;
    if (all || fog != (sn->fog && far_z > fog_z) || (fog && st->fog_color != sn->fog_color)) {
        if (fog) {
            *o++ = GC(207, st->fog_color & 0xFFFFFF);
            *o++ = GF(205, -far_z);
            *o++ = GF(206, -1.0f / (far_z - fog_z));
            *o++ = GEN(0x1F, 1);
        } else
            *o++ = GEN(0x1F, 0);
    }
    if (all || st->uscale != sn->uscale) { *o++ = GF(0x48, st->uscale); *o++ = GF(0x49, st->uscale); }
    if (all || st->ubase != sn->ubase || st->vbase != sn->vbase) { *o++ = GF(0x4A, st->ubase); *o++ = GF(0x4B, st->vbase); }
    int lit = st->light || st->lights;
    if (all || lit != (sn->light || sn->lights)) *o++ = GEN(0x17, lit);
    int nl = st->lights ? st->lights - 1 : 0, snl = all ? -1 : sn->lights ? sn->lights - 1 : 0;
    if (nl != snl)
        for (int i = 0; i < 4; i++) *o++ = GEN(0x18 + i, i < nl);
    if (st->lights) {
        if (all || !sn->lights) *o++ = GC(0x53, 0);                    /* sceGuColorMaterial(0) */
        if (all || !sn->lights || st->lamb != sn->lamb || st->lemis != sn->lemis || st->lemit != sn->lemit) {
            uint32_t m = st->lamb, sa = st->lemis & 0xFF, sd = st->lemis >> 8 & 0xFF, mac = 0, mdc = 0;
            for (int i = 0; i < 3; i++) {
                uint32_t c = m >> (8 * i) & 0xFF;
                mac |= ((c * sa + 127u) / 255u) << (8 * i);
                mdc |= ((c * sd + 127u) / 255u) << (8 * i);
            }
            uint32_t ac = mac | (m & 0xFF000000u);
            *o++ = GC(0x55, ac);                                       /* sceGuAmbientColor: MAC + MAA */
            *o++ = GC(0x58, ac >> 24);
            *o++ = GC(86, mdc);                                        /* MDC */
            *o++ = GC(87, 0);                                          /* MSC */
            *o++ = GC(84, st->lemit & 0xFFFFFF);                       /* EMC */
        }
        uint32_t al = me_rgb8(amb) | 0xFF000000u;                      /* sceGuAmbient: AC + ALA (the game's ambient) */
        *o++ = GC(0x5C, al);
        *o++ = GC(0x5D, al >> 24);
    } else if (st->light) {
        if (all || !sn->light || sn->lights) *o++ = GC(0x53, GU_AMBIENT);
        if (all || !sn->light || sn->lights || st->lamb != sn->lamb) { *o++ = GC(0x5C, st->lamb); *o++ = GC(0x5D, st->lamb >> 24); }
        if (all || !sn->light || sn->lights || st->lemis != sn->lemis) *o++ = GC(84, st->lemis & 0xFFFFFF);
    }
    return o;
}

/* hle_ge.c apply_state's texture part (texon: the back-end's cur_texon: -1 unknown, 0 off, 1 on) */
static uint32_t *enc_tex(uint32_t *o, const W3mTex *t, const W3mTex *prev, int all, int *texon)
{
    if (!t->textured) {
        if (*texon != 0) { *o++ = GEN(0x1E, 0); *texon = 0; }
        return o;
    }
    if (*texon != 1) { *o++ = GEN(0x1E, 1); *texon = 1; }
    /* the swizzle byte carries the max mip level in bits 4-6, bufw_clut the level bias in bits 24-31 */
    uint32_t psm = t->psm_swz_exp & 0xFF, swz = t->psm_swz_exp >> 8 & 1u, mx = t->psm_swz_exp >> 12 & 7u,
             wexp = t->psm_swz_exp >> 16 & 0xFF, hexp = t->psm_swz_exp >> 24, bufw = t->bufw_clut & 0xFFFF,
             nclut = t->bufw_clut >> 16 & 0xFF, cpsm = t->cpsm_filt_wrap & 0xFF;
    if (all || t->tbp != prev->tbp || t->cbp != prev->cbp || t->psm_swz_exp != prev->psm_swz_exp ||
        t->bufw_clut != prev->bufw_clut || (t->cpsm_filt_wrap & 0xFF) != (prev->cpsm_filt_wrap & 0xFF)) {
        *o++ = GC(0xC2, swz | mx << 16);                         /* sceGuTexMode(psm, mx, 0, swz) */
        *o++ = GC(0xC3, psm);
        *o++ = GC(0xCB, 0);
        *o++ = GC(0xA0, t->tbp);                                 /* sceGuTexImage(0, w, h, bufw, tbp) */
        *o++ = GC(0xA8, ((t->tbp >> 8) & 0x0F0000) | bufw);
        *o++ = GC(0xB8, hexp << 8 | wexp);
        *o++ = GC(0xCB, 0);
        for (uint32_t k = 1; k <= mx; k++) {                     /* sceGuTexImage(k, ...) per level */
            uint32_t lw, lh, lb, a = t->tbp + hle_ge_mip_level(psm, swz, 1u << wexp, 1u << hexp, k, &lw, &lh, &lb);
            *o++ = GC(0xA0 + k, a);
            *o++ = GC(0xA8 + k, ((a >> 8) & 0x0F0000) | lb);
            *o++ = GC(0xB8 + k, (hexp > k ? hexp - k : 0u) << 8 | (wexp > k ? wexp - k : 0u));
            *o++ = GC(0xCB, 0);
        }
        if (mx) *o++ = GC(0xC8, (t->bufw_clut >> 24) << 16);    /* TLEVEL auto + bias */
        if (t->cbp) {
            *o++ = GC(0xC5, cpsm | 0u << 2 | 0xFFu << 8 | 0u << 16);   /* sceGuClutMode(cpsm, 0, 0xFF, 0) */
            *o++ = GC(0xB0, t->cbp);                             /* sceGuClutLoad(n, cbp) */
            *o++ = GC(0xB1, (t->cbp >> 8) & 0xF0000);
            *o++ = GC(0xC4, nclut);
        }
        *o++ = GC(0xCB, 0);                                      /* sceGuTexFlush */
    }
    uint32_t f = t->cpsm_filt_wrap >> 8 & 0xFF, wu = t->cpsm_filt_wrap >> 16 & 0xFF, wv = t->cpsm_filt_wrap >> 24;
    if (all || f != (prev->cpsm_filt_wrap >> 8 & 0xFF)) *o++ = GC(0xC6, f | (f & 1u) << 8);   /* mag = bit 0 */
    if (all || wu != (prev->cpsm_filt_wrap >> 16 & 0xFF) || wv != prev->cpsm_filt_wrap >> 24) *o++ = GC(0xC7, wu | wv << 8);
    return o;
}

uint32_t *w3dme_enc_tex(uint32_t *o, const W3mTex *t, const W3mTex *prev, int all)
{
    int texon = -1;
    return enc_tex(o, t, prev, all, &texon);
}
#endif

/* ---- output */
static int out_room(W3mPriv *p, uint32_t words) { return p->apos + words + 4u <= p->aend; }
/* w3d_me_bus BURST: the arena line holding word `word` leaves the ME's D-cache as one 64-byte write
   (hit writeback-invalidate: the line is not needed again in this batch). sim (a main-core thread / the host): the
   arena is written uncached / plain, only the line count is kept (the same accounting as on the ME). */
static inline void line_out(W3mPriv *p, uint32_t word)
{
#ifdef HLE_PSP
    if (!p->sim) __asm__ volatile("cache 0x1B, 0(%0)" : : "r"(&p->arena[word & ~15u]) : "memory");
#else
    (void)word;
#endif
    p->n_wb++;
}
static inline void out1(W3mPriv *p, uint32_t w)
{
    p->arena[p->apos++] = w;
    if (p->burst && !(p->apos & 15u)) line_out(p, p->apos - 1u);
}
static void outn(W3mPriv *p, const uint32_t *w, uint32_t n) { for (uint32_t i = 0; i < n; i++) out1(p, w[i]); }

static void part(W3mPriv *p, uint32_t type, uint32_t rec, uint32_t w1, uint32_t n)
{
    if (p->nparts >= W3M_PARTS) { if (!p->failed) p->failed = 3; return; }
    volatile W3mPart *q = &p->parts[p->nparts++];
    q->w0 = type << 24 | (rec & 0xFFFFFF);
    q->w1 = w1;
    q->n = n;
    q->pad = 0;
}

/* host: the back-end keeps a lit projection until it is told otherwise (hle_w3d_lit.c sends w3d_be_proj(NULL) after
   every lit record); on the PSP the next global state (a building record's camera) carries the camera's projection */
static void proj_restore(W3mPriv *p)
{
    if (!p->proj_custom) return;
#ifndef HLE_PSP
    out1(p, (uint32_t)W3M_OP_PROJ << 24);
#endif
    p->proj_custom = 0;
}

static void seg_close(W3mPriv *p)
{
    if (!p->seg_open) return;
    proj_restore(p);
#ifdef HLE_PSP
    out1(p, GC(0x0B, 0));                                         /* RET */
#else
    out1(p, (uint32_t)W3M_OP_RET << 24);
#endif
    part(p, W3M_P_SEG, p->seg_rec, p->seg_start * 4u, p->apos - p->seg_start);
    p->n_segs++;
    p->n_words += p->apos - p->seg_start;
    p->seg_open = 0;
}

/* the main core draws something itself: the GE state the next segment finds is unknown */
static void main_part(W3mPriv *p, uint32_t type, uint32_t rec, uint32_t mesh)
{
    seg_close(p);
    part(p, type, rec, mesh, 0);
    p->sent_ok = 0;
    p->tex_ok = 0;
    p->texon = -1;
    p->world_ok = 0;
    p->seg_cam = -1;
    p->lit_setup = 0;
    p->proj_custom = 0;
}

/* before the first GE word of a record's draws: open a segment (fresh state) / the camera's global state
   (hle_ge3d.c w3d_be_begin); host: the record's w3d_be_begin. (Nothing here writes a static variable: the ME's cache
   would write its line back over the main core's neighbours - all ME state is in W3mPriv.) */
static void seg_need(W3mPriv *p)
{
    int fresh = 0;
    if (!p->seg_open) {
        p->seg_open = 1;
        p->seg_start = p->apos;
        p->seg_rec = p->rec;
        p->seg_cam = -1;
        p->sent_ok = 0;
        p->tex_ok = 0;
        p->texon = -1;
        p->world_ok = 0;
        p->lit_setup = 0;
        fresh = 1;
    }
    if (p->seg_cam != (int)p->rec_cam) {
        if (!p->rec_lit) proj_restore(p);
#ifdef HLE_PSP
        uint32_t tmp[20], *e = w3dme_enc_proj(tmp, p->cam[p->rec_cam]);
        outn(p, tmp, (uint32_t)(e - tmp));
        outn(p, p->gtmpl, p->gtmpl_n);                            /* send_global without the projection (captured) */
        p->proj_custom = 0;
#endif
        p->seg_cam = (int)p->rec_cam;
        p->sent_ok = 0;
    }
#ifndef HLE_PSP
    if (fresh || p->begun_rec != p->rec + 1u) {
        if (!p->rec_lit) proj_restore(p);
        out1(p, (uint32_t)W3M_OP_BEGIN << 24 | 2);
        out1(p, p->rec);
        out1(p, p->rec_cam);
        p->begun_rec = p->rec + 1u;
    }
#else
    (void)fresh;
#endif
}

/* a lit record's projection (S x P) and lights, once per record and segment (hle_w3d_lit.c lit_begin) */
static void lit_need(W3mPriv *p)
{
    if (p->lit_setup) return;
    const float *pr = &p->cam[p->rec_cam][20];
    W3dLights L;
    for (int k = 0; k < 3; k++) L.amb[k] = p->lamb[k];
    L.n = p->ln;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) { L.dir[i][k] = p->ldir[i][k]; L.col[i][k] = p->lcol[i][k]; }
#ifdef HLE_PSP
    uint32_t tmp[64], *e = w3dme_enc_proj(tmp, pr);
    e = w3dme_enc_lights(e, &L);
    outn(p, tmp, (uint32_t)(e - tmp));
#else
    out1(p, (uint32_t)W3M_OP_PROJ << 24 | 16);
    for (int i = 0; i < 16; i++) out1(p, fbits(pr[i]));
    out1(p, (uint32_t)W3M_OP_LIGHTS << 24 | 28);
    const uint32_t *lw = (const uint32_t *)(const void *)&L;
    for (uint32_t i = 0; i < 28u; i++) out1(p, lw[i]);
#endif
    p->proj_custom = 1;
    p->sent_ok = 0;                                               /* (w3d_be_lights: the ambient goes with the state) */
    p->lit_setup = 1;
}

/* ---- the stream reader: words of published lines; a line's tag is checked when it is first touched */
static int line_check(W3mPriv *p, uint32_t line, int sim)
{
    if (p->line_ok == line + 1u) return 1;
    uint32_t *l = p->stream + 16u * line;
#ifdef HLE_PSP
    if (!sim) hle_me_dinval(l, 64);
#else
    (void)sim;
#endif
    uint32_t want = (p->batch & 0xFFFFu) << 16 | (line & 0xFFFFu);
    if (l[0] != want) {
#ifdef HLE_PSP
        if (!sim) { hle_me_dinval(l, 64); __asm__ volatile("sync" ::: "memory"); }
#endif
        p->n_stale++;
        if (l[0] != want) return 0;
    }
    p->line_ok = line + 1u;
    return 1;
}

/* the next item if it is complete in the published lines: 1 = in p->item (type in item[0] >> 24), 0 = not yet,
   -1 = broken stream */
static int next_item(W3mPriv *p, uint32_t published, int sim)
{
    for (;;) {
        if (p->wpos > W3M_PAY) { p->line++; p->wpos = 1; }
        if (p->line >= published) return 0;
        if (!line_check(p, p->line, sim)) return -1;
        uint32_t h = p->stream[16u * p->line + p->wpos];
        if (h == 0) { p->wpos = W3M_PAY + 1u; continue; }         /* padding: the rest of the line */
        uint32_t n = h & 0xFFFFFFu;
        if (n > W3M_ITEM_MAX) return -1;
        /* words left from here in the published lines */
        uint32_t avail = (published - p->line) * W3M_PAY - (p->wpos - 1u);
        if (avail < n + 1u) return 0;
        uint32_t ln = p->line, wp = p->wpos;
        for (uint32_t i = 0; i <= n; i++) {
            if (wp > W3M_PAY) { ln++; wp = 1; if (!line_check(p, ln, sim)) return -1; }
            p->item[i] = p->stream[16u * ln + wp];
            wp++;
        }
        p->line = ln;
        p->wpos = wp;
        return 1;
    }
}

/* ---- the items */
static void item_cam(W3mPriv *p)
{
    uint32_t idx = p->item[1], n = p->item[0] & 0xFFFFFFu;
    if (idx >= W3M_CAMS || (n != W3M_CAM_WORDS && n != W3M_LCAM_WORDS)) { if (!p->failed) p->failed = 4; return; }
    for (int i = 0; i < 19; i++) p->cam[idx][i] = bitsf(p->item[2 + i]);
    p->cam[idx][19] = 1.0f;
    p->cam[idx][36] = 0.0f;
    if (n == W3M_LCAM_WORDS) {
        for (int i = 0; i < 16; i++) p->cam[idx][20 + i] = bitsf(p->item[21 + i]);
        p->cam[idx][36] = 1.0f;
    }
}

static void st_unpack(W3dState *st, uint32_t s0, uint32_t s1)
{
    st->textured = 0; st->ubase = 0.0f; st->vbase = 0.0f; st->uscale = 0.0f;
    st->ztest = (s0 & 1) ? 1 + (int)(s0 >> 5 & 1) : 0; st->zwrite = (int)(s0 >> 1 & 1); st->cull = (int)(s0 >> 2 & 3);
    st->blend = 0; st->src_blend = (int)(s0 >> 8 & 0xF); st->dst_blend = (int)(s0 >> 12 & 0xF);
    st->atest = 0; st->fog = (int)(s0 >> 4 & 1); st->fog_color = s1;
    st->light = 0; st->lamb = 0; st->lemis = 0; st->lights = 0; st->lemit = 0;
}

static void item_rec(W3mPriv *p)
{
    const uint32_t *w = p->item + 1;
    p->rec = w[0];
    p->rec_cam = w[1];
    p->rec_flags = w[2];
    p->rec_lit = 0;
    if (p->rec_cam >= W3M_CAMS || p->cam[p->rec_cam][19] != 1.0f) { if (!p->failed) p->failed = 4; return; }
    st_unpack(&p->rst, w[3], w[4]);
    wcp(&p->st, &p->rst, W3M_STATE_WORDS);                        /* atomic_meshes: W3dState st = rd->st */
    for (int i = 0; i < 12; i++) { p->M[i] = bitsf(w[5 + i]); p->gm[i] = bitsf(w[17 + i]); p->wm[i] = p->gm[i]; }
    p->world_ok = 0;
}

static void emit_world(W3mPriv *p)
{
    if (p->world_ok) return;
#ifdef HLE_PSP
    uint32_t tmp[13], *e = w3dme_enc_world(tmp, p->wm);
    outn(p, tmp, (uint32_t)(e - tmp));
#else
    out1(p, (uint32_t)W3M_OP_WORLD << 24 | 12);
    for (int i = 0; i < 12; i++) out1(p, fbits(p->wm[i]));
#endif
    p->world_ok = 1;
}

static void emit_state(W3mPriv *p)
{
#ifdef HLE_PSP
    uint32_t tmp[96];
    float far_z = p->cam[p->rec_cam][17], fog_z = p->cam[p->rec_cam][18];
    int fr = (int)((p->opts >> 5 & 1) | (p->opts >> 24 & 3) << 1);   /* front + the dither policy */
    uint32_t *e = w3dme_enc_state(tmp, &p->st, &p->sent, !p->sent_ok, far_z, fog_z, fr, p->lamb);
    outn(p, tmp, (uint32_t)(e - tmp));
#else
    if (p->sent_ok && !wne(&p->sent, &p->st, W3M_STATE_WORDS)) return;
    out1(p, (uint32_t)W3M_OP_STATE << 24 | W3M_STATE_WORDS);
    const uint32_t *s = (const uint32_t *)(const void *)&p->st;
    for (uint32_t i = 0; i < W3M_STATE_WORDS; i++) out1(p, s[i]);
#endif
    wcp(&p->sent, &p->st, W3M_STATE_WORDS);
    p->sent_ok = 1;
}

static void emit_tex(W3mPriv *p, const W3mTex *t)
{
#ifdef HLE_PSP
    uint32_t tmp[64], *e = enc_tex(tmp, t, &p->tsent, !p->tex_ok, &p->texon);   /* up to 49 words */
    outn(p, tmp, (uint32_t)(e - tmp));
#else
    if (p->tex_ok && !wne(&p->tsent, t, W3M_TEX_WORDS)) return;
    out1(p, (uint32_t)W3M_OP_TEX << 24 | 4);
    out1(p, t->tbp);                                              /* host: raster, the driver's filter / addressing */
    out1(p, t->psm_swz_exp & 0xFF);
    out1(p, t->psm_swz_exp >> 8 & 0xFF);
    out1(p, t->psm_swz_exp >> 16 & 0xFF);
#endif
    wcp(&p->tsent, t, W3M_TEX_WORDS);
    p->tex_ok = 1;
}

/* hle_w3d.c w3d_alpha_passes */
static int me_alpha_passes(W3mPriv *p, W3dState *st, int acl, int valpha)
{
    int passes = 1;
    if (acl == 2 || valpha) {
        st->blend = 1;
        if (acl >= 1 && (p->opts >> 4 & 1)) { st->atest = 2; passes = 2; }
        else st->atest = acl >= 1 ? 1 : 0;
    } else if (acl == 1) { st->blend = 0; st->atest = 1; }
    else { st->blend = 0; st->atest = 0; }
    return passes;
}

/* the mesh item (hle_w3d.c me_prep_mesh): w[0] k | i << 16, [1] verts, [2] sub, [3] chunks, [4] nchunks | fmt << 16 |
   cl << 24, [5] nprims, [6] ubase, [7] vbase, [8] mc4, [9..17] W3mTex */
static void item_mesh(W3mPriv *p)
{
    const uint32_t *w = p->item + 1;
    uint32_t k = w[0] & 0xFFFF, i = w[0] >> 16, verts = w[1], sub = w[2], chunks = w[3];
    uint32_t nch = w[4] & 0xFFFF, fmt = w[4] >> 16 & 0xFF, cl = w[4] >> 24, nprims = w[5];
    float ubase = bitsf(w[6]), vbase = bitsf(w[7]);
    uint32_t mc4 = w[8];
    W3mTex t;
    wcp(&t, w + 9, W3M_TEX_WORDS);
    p->n_meshes++;
    if (p->rec_lit || verts < p->vlo || verts >= p->vhi || sub < p->vlo || sub >= p->vhi || chunks < p->vlo ||
        chunks + 24u * nch > p->vhi) { if (!p->failed) p->failed = 4; return; }
    float near_z = p->cam[p->rec_cam][16], far_z = p->cam[p->rec_cam][17];
    int gnear = (int)(p->opts & 1), clip = (int)(p->opts >> 1 & 1);
    int inside = (int)(cl & W3M_CL_INSIDE);
    cl &= 3;
#ifdef HLE_PSP
    const Chunk *chs = (const Chunk *)(uintptr_t)chunks;
#else
    const Chunk *chs = (const Chunk *)(w3d_arena_base() + chunks);   /* host: an offset (64-bit pointers) */
#endif
    int whole = cl == W3M_CL_HW && (nch <= 1 || inside);
    if (!whole) {
        if (nch > W3M_MAXCH) { p->n_cull++; return; }              /* (hle_w3d.c: st_m_fail) */
        uint32_t nhw = 0, ncpu = 0, group = p->group;
        for (uint32_t g = 0; g < nch; g += group) {
            uint32_t ge = g + group < nch ? g + group : nch;
            int gc = W3M_CL_CPU;
            if (ge - g > 1) {
                int16_t umn[3], umx[3];
                for (int a = 0; a < 3; a++) { umn[a] = chs[g].bmin[a]; umx[a] = chs[g].bmax[a]; }
                for (uint32_t q = g + 1; q < ge; q++)
                    for (int a = 0; a < 3; a++) {
                        if (chs[q].bmin[a] < umn[a]) umn[a] = chs[q].bmin[a];
                        if (chs[q].bmax[a] > umx[a]) umx[a] = chs[q].bmax[a];
                    }
                gc = me_classify_chunk(umn, umx, p->M, near_z, far_z, gnear);
            }
            for (uint32_t q = g; q < ge; q++) {
                int cc = gc != W3M_CL_CPU ? gc : me_classify_chunk(chs[q].bmin, chs[q].bmax, p->M, near_z, far_z, gnear);
                if (cc == W3M_CL_CPU && !clip) cc = W3M_CL_CULL;
                p->chcls[q] = (uint8_t)cc;
                if (cc == W3M_CL_HW) nhw++; else if (cc == W3M_CL_CPU) ncpu++;
            }
        }
        if (!nhw && !ncpu) { p->n_cull++; return; }
        if (ncpu) {                                               /* the CPU clipper is the main core's */
            main_part(p, W3M_P_MAINMESH, p->rec, i);
            p->n_main++;
            return;
        }
        whole = nhw == nch;
    }
    /* reserve: state + texture + world + the draws of two passes */
    if (!out_room(p, 256u + 2u * (16u + 4u * nprims))) { if (!p->failed) p->failed = 2; return; }
    /* hle_w3d.c atomic_meshes: the lighting / emissive of this mesh */
    uint8_t mat4[4] = {(uint8_t)mc4, (uint8_t)(mc4 >> 8), (uint8_t)(mc4 >> 16), (uint8_t)(mc4 >> 24)};
    int gmod = (int)(p->rec_flags >> 24 & 1);
    int modulate = gmod || mc4 != 0xFFFFFFFFu;
    uint8_t amb3[3] = {(uint8_t)p->rec_flags, (uint8_t)(p->rec_flags >> 8), (uint8_t)(p->rec_flags >> 16)};
    W3dState *st = &p->st;
    st->light = 0;
    if (p->opts >> 2 & 1) {
        uint32_t lm = modulate ? mc4 : 0xFFFFFFFFu, em = 0;
        for (int q = 0; q < 3; q++) {
            uint32_t m8 = modulate ? mat4[q] : 255u;
            em |= ((amb3[q] * m8 * 257u + 32768u) >> 16) << (8 * q);
        }
        if (lm != 0xFFFFFFFFu || em) { st->light = 1; st->lamb = lm; st->lemis = em; }
    }
    st->textured = (int)t.textured;
    int valpha = gmod && mat4[3] < 255;
    st->uscale = fmt == W3D_FMT_UV16 ? 8.0f : 1.0f;
    int tilebase = (int)(p->opts >> 3 & 1);
    st->ubase = fmt == W3D_FMT_UV16 && (!(t.wrap & 1) || tilebase) ? ubase : 0.0f;
    st->vbase = fmt == W3D_FMT_UV16 && (!(t.wrap & 2) || tilebase) ? vbase : 0.0f;
    int passes = me_alpha_passes(p, st, (int)t.acl, valpha);
    int zw = st->zwrite;
    seg_need(p);
    emit_tex(p, &t);
    uint32_t stride = fmt == W3D_FMT_UV16 ? 16u : 20u;
    for (int pass = 0; pass < passes; pass++) {
        if (pass == 1) { st->atest = 3; st->zwrite = 0; p->n_twopass++; }
        emit_state(p);
        emit_world(p);
        if (whole) {
#ifdef HLE_PSP
            out1(p, GC(16, (verts >> 8) & 0xF0000));
            out1(p, GC(1, verts));
            out1(p, GC(16, (sub >> 8) & 0xF0000));
            out1(p, GC(10, sub));
#else
            out1(p, (uint32_t)W3M_OP_MESH << 24 | 1);
            out1(p, k);
#endif
            continue;
        }
        for (uint32_t q = 0; q < nch;) {
            if (p->chcls[q] == W3M_CL_HW) {
                uint32_t r = q, npr = 0;
                while (r < nch && p->chcls[r] == W3M_CL_HW) npr += chs[r++].nprims;
                uint32_t fv = chs[q].first_vert, fp = chs[q].first_prim;
#ifdef HLE_PSP
                uint32_t v = verts + fv * stride;
                const uint32_t *prims = (const uint32_t *)(uintptr_t)sub + 1u + fp;
                out1(p, GC(16, (v >> 8) & 0xF0000));
                out1(p, GC(1, v));
                out1(p, GC(18, (fmt == W3D_FMT_UV16 ? GU_TEXTURE_16BIT : GU_TEXTURE_32BITF) | GU_COLOR_8888 |
                                   GU_VERTEX_16BIT | GU_TRANSFORM_3D));
                for (uint32_t x = 0; x < npr; x++) out1(p, prims[x]);
#else
                (void)stride;
                out1(p, (uint32_t)W3M_OP_RANGE << 24 | 3);
                out1(p, k);
                out1(p, fv);
                out1(p, fp | npr << 16);
#endif
                p->n_ranges++;
                q = r;
            } else
                q++;
        }
    }
    st->zwrite = zw;
}

/* ---- lit / skinned records (hle_w3d_lit.c lit_draw: lit_begin + lit_mesh) */
/* the lit record (hle_w3d_lit.c w3d_lit_me_push): w[0] rec, [1] camera (8 + lit camera), [2] bit 0 skinned, bit 1 the
   geometry's modulate flag, [3] [4] base state, [5] peak, [6] bones, [7] palette, [8..19] MV, [20..31] W, [32..43] the
   record's GE world matrix, [44..46] ambient, [47] lights, [48..59] directions, [60..71] colours */
static const float (*me_palette(W3mPriv *p))[12]
{
#ifdef HLE_PSP
    return (const float (*)[12])(uintptr_t)p->bones;
#else
    return w3d_lit_bone_base() + p->bones;
#endif
}

static void item_lrec(W3mPriv *p, int sim)
{
    const uint32_t *w = p->item + 1;
    p->rec = w[0];
    p->rec_cam = w[1];
    p->rec_flags = w[2];
    p->rec_lit = 1;
    if (p->rec_cam >= W3M_CAMS || p->cam[p->rec_cam][19] != 1.0f || p->cam[p->rec_cam][36] != 1.0f || w[6] > 128u ||
        w[47] > 4u) { if (!p->failed) p->failed = 4; return; }
    st_unpack(&p->rst, w[3], w[4]);
    wcp(&p->st, &p->rst, W3M_STATE_WORDS);                        /* lit_draw: W3dState st = rec->st */
    p->peak = bitsf(w[5]);
    p->nbones = w[6];
    p->bones = w[7];
    for (int i = 0; i < 12; i++) {
        p->MV[i] = bitsf(w[8 + i]);
        p->W[i] = bitsf(w[20 + i]);
        p->gm[i] = bitsf(w[32 + i]);
        p->wm[i] = p->gm[i];
    }
    for (int k = 0; k < 3; k++) p->lamb[k] = bitsf(w[44 + k]);
    p->ln = (int)w[47];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) { p->ldir[i][k] = bitsf(w[48 + 3 * i + k]); p->lcol[i][k] = bitsf(w[60 + 3 * i + k]); }
    p->world_ok = 0;
    p->lit_setup = 0;
#ifdef HLE_PSP
    if (!sim && p->nbones) {                                      /* the palette: main wrote it back before the record */
        uint32_t a = p->bones;
        if (a < 0x08000000u || a + 48u * p->nbones > 0x0C000000u) { if (!p->failed) p->failed = 4; return; }
        hle_me_dinval((const void *)(uintptr_t)a, 48u * p->nbones);
    }
#else
    (void)sim;
#endif
}

/* hle_w3d_lit.c pal_bones: the group's palette slots */
static void me_pal_bones(W3mPriv *p, const LGroup *g, float (*bm)[12])
{
    const float (*pal)[12] = me_palette(p);
    for (int b = 0; b < g->nb && b < 8; b++) {
        uint32_t s = g->bone[b] < p->nbones ? g->bone[b] : 0u;
        for (int q = 0; q < 12; q++) bm[b][q] = pal[s][q];
    }
}

/* hle_w3d_lit.c group_box */
static void me_group_box(const LGroup *g, const float (*bm)[12], int nb, float *mn, float *mx)
{
    if (!nb) {
        for (int a = 0; a < 3; a++) { mn[a] = g->bmin[a]; mx[a] = g->bmax[a]; }
        return;
    }
    float c[3], h[3];
    for (int a = 0; a < 3; a++) { mn[a] = 1e30f; mx[a] = -1e30f; }
    for (int b = 0; b < nb; b++) {
        if (g->bbmin[b][0] > g->bbmax[b][0]) continue;
        for (int a = 0; a < 3; a++) {
            c[a] = (g->bbmin[b][a] + g->bbmax[b][a]) * 0.5f;
            h[a] = (g->bbmax[b][a] - g->bbmin[b][a]) * 0.5f;
        }
        const float *B = bm[b];
        for (int k = 0; k < 3; k++) {
            float cc = c[0] * B[k] + c[1] * B[3 + k] + c[2] * B[6 + k] + B[9 + k];
            float r = me_fabs(h[0] * B[k]) + me_fabs(h[1] * B[3 + k]) + me_fabs(h[2] * B[6 + k]);
            if (cc - r < mn[k]) mn[k] = cc - r;
            if (cc + r > mx[k]) mx[k] = cc + r;
        }
    }
}

/* the lit mesh (hle_w3d_lit.c lit_prep_mesh): w[0] k | i << 16, [1] block, [2] groups, [3] groups count, [4] pscale,
   [5] ubase, [6] vbase, [7] material colour, [8] surface ambient, [9] diffuse, [10] reserved, [11..19] W3mTex */
static void item_lmesh(W3mPriv *p)
{
    const uint32_t *w = p->item + 1;
    uint32_t k = w[0] & 0xFFFF, i = w[0] >> 16, block = w[1], groups = w[2], nx = w[3];
    float pscale = bitsf(w[4]), ubase = bitsf(w[5]), vbase = bitsf(w[6]), sa = bitsf(w[8]), sd = bitsf(w[9]);
    uint32_t mc4 = w[7];
    W3mTex t;
    wcp(&t, w + 11, W3M_TEX_WORDS);
    p->n_meshes++;
    if (!p->rec_lit || block < p->vlo || block >= p->vhi || groups < p->vlo || nx > W3M_MAXGROUPS ||
        groups + (uint32_t)sizeof(LGroup) * nx > p->vhi) { if (!p->failed) p->failed = 4; return; }
#ifdef HLE_PSP
    const LGroup *gt = (const LGroup *)(uintptr_t)groups;
#else
    const LGroup *gt = (const LGroup *)(w3d_arena_base() + groups);
#endif
    float near_z = p->cam[p->rec_cam][16], far_z = p->cam[p->rec_cam][17];
    int gnear = (int)(p->opts & 1), skinned = (int)(p->rec_flags & 1);
    /* every group first: a group that leaves the guard band needs the main core's CPU path - then the whole mesh goes
       there (its draws must stay in order) */
    uint32_t nhw = 0;
    for (uint32_t g = 0; g < nx; g++) {
        float bm[8][12], mn[3], mx[3];
        int nb = gt[g].nb > 8 ? 8 : gt[g].nb;
        if (block + gt[g].voff >= p->vhi || block + gt[g].suboff >= p->vhi) { if (!p->failed) p->failed = 4; return; }
        if (nb) me_pal_bones(p, &gt[g], bm);
        me_group_box(&gt[g], (const float (*)[12])bm, nb, mn, mx);
        int gc = me_classify_box(mn, mx, p->MV, near_z, far_z, gnear) & 3;
        if (gc == W3M_CL_CPU) {
            main_part(p, W3M_P_MAINMESH, p->rec, i);
            p->n_main++;
            return;
        }
        p->gcls[g] = (uint8_t)gc;
        if (gc == W3M_CL_HW) nhw++;
    }
    if (!out_room(p, 256u + 2u * nx * 110u)) { if (!p->failed) p->failed = 2; return; }
    /* hle_w3d_lit.c lit_mesh: the material / surface state */
    W3dState *st = &p->st;
    if (!(sa >= 0.0f && sa <= 64.0f)) sa = 1.0f;
    if (!(sd >= 0.0f && sd <= 2.0f)) sd = 1.0f;
    st->textured = (int)t.textured;
    st->uscale = 8.0f;
    int tilebase = (int)(p->opts >> 3 & 1);
    st->ubase = !(t.wrap & 1) || tilebase ? ubase : 0.0f;
    st->vbase = !(t.wrap & 2) || tilebase ? vbase : 0.0f;
    st->light = 0;
    st->lights = 1 + p->ln;
    st->lamb = mc4;
    if ((mc4 & 0xFFFFFFu) != 0xFFFFFFu && p->peak > 1.0f) { sa /= p->peak; sd /= p->peak; }
    st->lemit = 0;
    if (sa > 1.0f) {
        for (int q = 0; q < 3; q++) {
            float e = sa * p->lamb[q];
            e = (e > 1.0f ? 1.0f : e) * (float)(mc4 >> (8 * q) & 0xFF);
            st->lemit |= (uint32_t)(e + 0.5f) << (8 * q);
        }
        sa = 0.0f;
    }
    if (sd > 1.0f) sd = 1.0f;
    st->lemis = (uint32_t)(sa * 255.0f + 0.5f) | (uint32_t)(sd * 255.0f + 0.5f) << 8;
    int valpha = (int)(p->rec_flags >> 1 & 1) && (mc4 >> 24) < 255;
    int passes = me_alpha_passes(p, st, (int)t.acl, valpha || (mc4 >> 24) < 255);
    int zw = st->zwrite;
    if (!nhw) { p->n_cull++; return; }                            /* (the main path binds and draws nothing) */
    float kk = 32768.0f * pscale;
    seg_need(p);
    lit_need(p);
    emit_tex(p, &t);
    if (!skinned) {                                               /* rigid: the position scale in the world matrix */
        for (int rr = 0; rr < 3; rr++)
            for (int q = 0; q < 3; q++) p->wm[3 * rr + q] = p->W[3 * rr + q] * kk;
        for (int q = 0; q < 3; q++) p->wm[9 + q] = p->W[9 + q];
        p->world_ok = 0;
    }
    for (int pass = 0; pass < passes; pass++) {
        if (pass == 1) { st->atest = 3; st->zwrite = 0; p->n_twopass++; }
        emit_state(p);
        emit_world(p);
        for (uint32_t g = 0; g < nx; g++) {
            if (p->gcls[g] != W3M_CL_HW) continue;
            int nb = gt[g].nb > 8 ? 8 : gt[g].nb;
            if (nb) {                                             /* the bones with the position scale on the 3x3 */
                float bm[8][12];
                me_pal_bones(p, &gt[g], bm);
                for (int b = 0; b < nb; b++)
                    for (int q = 0; q < 9; q++) bm[b][q] *= kk;
#ifdef HLE_PSP
                uint32_t tmp[100], *e = w3dme_enc_bones(tmp, nb, (const float (*)[12])bm);
                outn(p, tmp, (uint32_t)(e - tmp));
#else
                out1(p, (uint32_t)W3M_OP_BONES << 24 | (1u + 12u * (uint32_t)nb));
                out1(p, (uint32_t)nb);
                for (int b = 0; b < nb; b++)
                    for (int q = 0; q < 12; q++) out1(p, fbits(bm[b][q]));
#endif
            }
#ifdef HLE_PSP
            uint32_t v = block + gt[g].voff, s = block + gt[g].suboff;
            out1(p, GC(16, (v >> 8) & 0xF0000));
            out1(p, GC(1, v));
            out1(p, GC(16, (s >> 8) & 0xF0000));
            out1(p, GC(10, s));
#else
            out1(p, (uint32_t)W3M_OP_LMESH << 24 | 2);
            out1(p, k);
            out1(p, g);
#endif
        }
    }
    st->zwrite = zw;
    p->n_lmeshes++;
    (void)block;
}

static void batch_end(W3mPriv *p, volatile W3mCtl *c)
{
    seg_close(p);
    if (p->burst && (p->apos & 15u) && p->apos > p->astart) line_out(p, p->apos - 1u);   /* the last
                                                                     partial line (the sync below drains it) */
    c->nparts = p->nparts;
    c->arena_end = p->apos * 4u;
    c->fail = p->failed;
    c->st_meshes = p->n_meshes; c->st_cull = p->n_cull; c->st_main = p->n_main; c->st_segs = p->n_segs;
    c->st_words = p->n_words; c->st_stale = p->n_stale; c->st_ranges = p->n_ranges; c->st_twopass = p->n_twopass;
    c->st_units = p->n_units;
    c->st_lmeshes = p->n_lmeshes;
    c->st_wb = p->n_wb;
    c->st_awords = p->apos - p->astart;
    __asm__ volatile("" ::: "memory");
#ifdef HLE_PSP
    __asm__ volatile("sync" ::: "memory");
#else
    __sync_synchronize();
#endif
    c->done = p->batch;
    p->finished = 1;
}

static int poll_body(int sim)
{
    volatile W3mCtl *c = ME_CTL();
    if (!c) return 0;
    c->polls = c->polls + 1u;
    uint32_t b = c->batch;
    if (!b) return 0;
    W3mPriv *p = ME_PTR(c, priv, W3mPriv *);
    if (b != p->batch) {                                          /* a new batch */
#ifdef HLE_PSP
        if (!sim) hle_me_dwbinv_all();                            /* a fresh view of the stream, the vertex cache, the
                                                                     palettes, the global template (main rewrote them) */
#endif
        p->batch = b;
        p->line = 0;
        p->wpos = 1;
        p->line_ok = 0;
        p->finished = 0;
        p->failed = 0;
        p->stream = ME_PTR(c, stream, uint32_t *);
        p->arena = ME_PTR(c, arena, volatile uint32_t *);
        p->parts = ME_PTR(c, parts, volatile W3mPart *);
        p->apos = c->arena_start / 4u;
        p->aend = W3M_ARENA / 4u;
        p->nparts = 0;
        /* w3d_me_bus BURST: the ME writes the arena through its D-cache - the cached alias of the same
           words (the whole D-cache was just written back + invalidated: no stale arena line; a first store into a
           line fills it from RAM, so the words of the previous batch in that line are written back unchanged) */
        p->burst = (c->opts & W3M_OPT_BURST) ? 1u : 0u;
        p->sim = (uint32_t)sim;
        p->astart = p->apos;
        p->n_wb = 0;
#ifdef HLE_PSP
        if (p->burst && !sim) p->arena = (volatile uint32_t *)((uintptr_t)p->arena & ~0x40000000u);
#endif
        p->seg_open = 0;
        p->seg_cam = -1;
        p->proj_custom = 0;
        p->lit_setup = 0;
        p->rec_lit = 0;
        p->begun_rec = 0;
        p->opts = c->opts;
        p->vlo = c->pad0[3];
        p->vhi = c->pad0[4];
        p->group = p->opts >> 8 & 0xFF;
        if (p->group < 1) p->group = 1;
#ifdef HLE_PSP
        p->gtmpl_n = c->pad0[1];
        if (p->gtmpl_n > 64) p->gtmpl_n = 64;
        const uint32_t *gt = (const uint32_t *)(uintptr_t)c->pad0[0];
        for (uint32_t i = 0; i < p->gtmpl_n; i++) p->gtmpl[i] = gt[i];
#else
        p->gtmpl_n = 0;
#endif
        for (uint32_t i = 0; i < W3M_CAMS; i++) { p->cam[i][19] = 0.0f; p->cam[i][36] = 0.0f; }
        p->n_meshes = p->n_cull = p->n_main = p->n_segs = p->n_words = p->n_stale = p->n_ranges = p->n_twopass = 0;
        p->n_units = 0;
        p->n_lmeshes = 0;
    }
    if (p->finished) return 0;
    uint32_t published = c->lines;
    if (published > W3M_LINES) published = W3M_LINES;
    int r = next_item(p, published, sim);
    if (r == 0) return 0;
    p->n_units++;
    if (r < 0) {                                                  /* a stale / broken line: the main core draws all */
        p->failed = 1;
        batch_end(p, c);
        return 1;
    }
    uint32_t type = p->item[0] >> 24;
    if (type == W3M_I_END) { batch_end(p, c); return 1; }
    if (!p->failed) {
        if (type == W3M_I_CAM) item_cam(p);
        else if (type == W3M_I_REC) item_rec(p);
        else if (type == W3M_I_MESH) item_mesh(p);
        else if (type == W3M_I_LREC) item_lrec(p, sim);
        else if (type == W3M_I_LMESH) item_lmesh(p);
        else if (type == W3M_I_MAINMESH) { main_part(p, W3M_P_MAINMESH, p->item[1], p->item[2]); p->n_main++; }
        else if (type == W3M_I_MREC) main_part(p, W3M_P_MREC, p->item[1], 0);
        else p->failed = 4;
    }
    c->lines_done = p->line;
    return 1;
}

/* the ME audio loop (hle_me_audio.c, idle branch) - only while no runner of our own owns the stream */
int w3dme_poll(int sim)
{
    volatile W3mCtl *c = ME_CTL();
    if (!c || c->pad0[2]) return 0;
    return poll_body(sim);
}

#ifdef HLE_PSP
/* our own ME loop (w3d_me_runner=2, the ME not used by anybody else): hle_me.h's header in the first 64 bytes */
void w3dme_work(volatile HleMeHdr *hdr)
{
    hdr->boots = hdr->boots + 1u;
    hdr->state = 1;
    for (;;) {
        hdr->heartbeat = hdr->heartbeat + 1u;
        if (hdr->exit_req) break;
        if (poll_body(0)) continue;
        for (uint32_t i = 0; i < 4096u; i++) __asm__ volatile("nop");
    }
    hle_me_dwbinv_all();
    hdr->state = 2;
    hle_me_halt();
}
#endif

/* ------------------------------------------------------------------ 3. the main core */
static volatile W3mCtl *g_ctl;                /* uncached view */
static uint32_t *g_stream;                    /* cached */
static volatile W3mPart *g_parts;             /* uncached */
static volatile uint32_t *g_arena;            /* uncached */
static W3mPriv *g_priv;
static uint32_t g_batch, g_wline, g_wpos, g_pub, g_last_polls, g_polls_us, g_runner;
static uint32_t *g_tmpl;                      /* global-state template (64-byte aligned, 64 words) */
static uint64_t gs_wait_us, gs_wait_max, gs_timeouts, gs_nopoll, gs_full, gs_bb_spin;
static int g_dead, g_timeouts_row;
/* w3d_me_bus (hle_w3d_me.h), the ME's bus counters of the completed batches */
int w3d_opt_me_bus = 0;
static uint64_t gs_awords, gs_bwords, gs_wb, gs_parts, gs_units, gs_batches_done, gs_quiet_sets;
static uint32_t g_polls0, g_quiet;
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)
void hle_me_jobs_idle(uint32_t nops) __attribute__((weak));   /* psp/hle_me_audio.c (the ME loop): idle back-off */
#endif
static void bus_quiet(int on)                 /* QUIET: the ME's idle back-off x4 while no batch is open */
{
    uint32_t want = on ? 4u * 8192u : 0u;     /* 0 = the loop's default (8192 NOPs, ~75 us at 333 MHz) */
    if (want == g_quiet) return;
    g_quiet = want;
    gs_quiet_sets++;
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)
    if (hle_me_jobs_idle) hle_me_jobs_idle(want);
#endif
}

W3mCtl *w3dme_ctl(void) { return (W3mCtl *)(uintptr_t)g_ctl; }
uint32_t *w3dme_stream(void) { return g_stream; }
const W3mPart *w3dme_parts(void) { return (const W3mPart *)(uintptr_t)g_parts; }
const uint32_t *w3dme_arena(void) { return (const uint32_t *)(uintptr_t)g_arena; }

static void *alloc64(uint32_t bytes)
{
#ifdef HLE_PSP
    void *p = memalign(64, bytes);
#else
    void *p = NULL;
    if (posix_memalign(&p, 64, bytes)) p = NULL;
#endif
    if (p) memset(p, 0, bytes);
    return p;
}

#ifndef HLE_PSP
static void *host_thread(void *arg)
{
    (void)arg;
    for (;;)
        if (!poll_body(1)) usleep(20);
    return NULL;
}
#else
static int sim_thread(SceSize n, void *a)
{
    (void)n; (void)a;
    for (;;)
        if (!poll_body(1)) sceKernelDelayThread(100);
    return 0;
}
#endif

/* runner: 0 = the ME audio loop calls w3dme_poll, 1 = a thread (PSP: main core, emulator tests; host: always),
   2 = our own ME loop */
int w3dme_init(int runner, char *why, int why_n)
{
    if (g_ctl) return 1;
    void *ctl = alloc64(256), *pr = alloc64((uint32_t)sizeof(W3mPriv) + 64u);
    g_stream = (uint32_t *)alloc64(W3M_LINES * 64u);
    void *parts = alloc64(W3M_PARTS * (uint32_t)sizeof(W3mPart)), *arena = alloc64(W3M_ARENA);
    g_tmpl = (uint32_t *)alloc64(256);
    if (!ctl || !pr || !g_stream || !parts || !arena || !g_tmpl) {
        snprintf(why, (size_t)why_n, "out of memory (%u KB)", (unsigned)((W3M_LINES * 64u + W3M_ARENA) >> 10));
        return 0;
    }
#ifndef HLE_PSP
    runner = 1;
#endif
    g_priv = (W3mPriv *)pr;
    g_ctl = UNCP(W3mCtl, ctl);
    g_parts = UNCP(W3mPart, parts);
    g_arena = UNC(arena);
    g_runner = (uint32_t)runner;
#ifdef HLE_PSP
    sceKernelDcacheWritebackInvalidateAll();                      /* zeroed through the cache: nothing stale / dirty */
#endif
    g_ctl->stream = (uint32_t)(uintptr_t)g_stream;
    g_ctl->parts = (uint32_t)(uintptr_t)g_parts;
    g_ctl->arena = (uint32_t)(uintptr_t)g_arena;
    g_ctl->priv = (uint32_t)(uintptr_t)g_priv;
    g_ctl->pad0[0] = (uint32_t)(uintptr_t)g_tmpl;
    g_ctl->pad0[2] = runner == 1 || runner == 2;                   /* our own runner: the ME audio loop keeps out */
#ifdef HLE_PSP
    UNC(w3dme_gaddr)[0] = (uint32_t)(uintptr_t)g_ctl;
    __asm__ volatile("sync" ::: "memory");
    if (runner == 3) {                                            /* the shared jobs-only ME loop when no audio
                                                                     runs on the ME (hle_me_jobs.h); its loop polls us */
#ifdef HLE_HAVE_AUDIO_MODEL
        g_ctl->pad0[2] = 0;
        int hle_psp_is_emulator(void);
        if (!hle_me_jobs_run(1, hle_psp_is_emulator(), why, why_n)) return 0;
#else
        snprintf(why, (size_t)why_n, "no ME code in this build (AUDIO=0)");
        return 0;
#endif
    } else if (runner == 1) {
        SceUID th = sceKernelCreateThread("w3dme_sim", sim_thread, 0x20, 0x4000, PSP_THREAD_ATTR_USER, NULL);
        if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) { snprintf(why, (size_t)why_n, "thread %08x", (unsigned)th); return 0; }
    } else if (runner == 2) {
#ifdef HLE_HAVE_AUDIO_MODEL
        if (hle_me_running()) { snprintf(why, (size_t)why_n, "the ME is taken (audio_me?)"); return 0; }
        hle_me_power_init();                                      /* park on a real suspend (hle_me.h; idempotent) */
        volatile HleMeHdr *h = (volatile HleMeHdr *)(uintptr_t)g_ctl;
        h->magic = HLE_ME_MAGIC;
        static uint8_t *region;
        if (!region) region = (uint8_t *)alloc64(32768);
        sceKernelDcacheWritebackInvalidateAll();
        if (!region || !hle_me_start(w3dme_work, h, region, 32768, 16384, why, why_n)) return 0;
#else
        snprintf(why, (size_t)why_n, "no ME code in this build (AUDIO=0)");
        return 0;
#endif
    }
#else
    g_host_stream = g_stream;
    g_host_parts = (void *)(uintptr_t)g_parts;
    g_host_arena = (void *)(uintptr_t)g_arena;
    g_host_priv = g_priv;
    __sync_synchronize();
    g_host_ctl = g_ctl;
    pthread_t th;
    if (pthread_create(&th, NULL, host_thread, NULL)) { snprintf(why, (size_t)why_n, "pthread_create"); return 0; }
    pthread_detach(th);
#endif
    g_last_polls = g_ctl->polls;
    g_polls0 = g_ctl->polls;
    g_polls_us = w3d_plat_us() - 1000000u;                       /* no poller until the first poll is seen */
    if (runner != 3)
        snprintf(why, (size_t)why_n, "%s", runner == 0 ? "the shared ME loop polls" : runner == 1 ? "a thread" : "our own ME loop");
    return 1;
}

/* the global-state template (PSP: send_global minus the projection, captured by the self-test) */
void w3dme_set_template(const uint32_t *w, uint32_t n)
{
    if (!g_tmpl) return;
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) g_tmpl[i] = w[i];
#ifdef HLE_PSP
    sceKernelDcacheWritebackRange(g_tmpl, 256);
#endif
    g_ctl->pad0[1] = n;
}

/* is somebody polling? (the ME audio loop runs only once the ME took the audio; a dead / parked ME stops) */
static int poller_alive(void)
{
    uint32_t p = g_ctl->polls, now = w3d_plat_us();
    if (p != g_last_polls) { g_last_polls = p; g_polls_us = now; return 1; }
    return now - g_polls_us < 50000u;
}

/* a new batch: 0 = no live poller (draw on the main core) */
int w3dme_batch_begin(uint32_t arena_start, uint32_t opts, uint32_t vlo, uint32_t vhi)
{
    if (!g_ctl || g_dead) return 0;
    if (!poller_alive()) { gs_nopoll++; return 0; }
    if (++g_batch == 0) g_batch = 1;
    g_wline = 0;
    g_wpos = 1;
    g_pub = 0;
    g_ctl->lines = 0;
    g_ctl->arena_start = arena_start;
    g_ctl->opts = opts;
    g_ctl->fail = 0;
    g_ctl->pad0[3] = vlo;                                          /* the vertex cache: [vlo, vhi) */
    g_ctl->pad0[4] = vhi;
    if (w3d_opt_me_bus & W3M_BUS_BURST) g_ctl->opts = opts | W3M_OPT_BURST;
    bus_quiet(0);                                                  /* a batch is open - normal polling */
#ifdef HLE_PSP
    __asm__ volatile("sync" ::: "memory");
#else
    __sync_synchronize();
#endif
    g_ctl->batch = g_batch;
    g_stream[0] = (g_batch & 0xFFFFu) << 16;
    return 1;
}

/* room for an item of n payload words (+ a reserve for the markers and the end) */
int w3dme_room(uint32_t n)
{
    uint32_t used = g_wline * W3M_PAY + (g_wpos - 1u);
    return used + n + 1u + 3u * W3M_PAY <= W3M_LINES * W3M_PAY;
}

void w3dme_put(uint32_t type, const uint32_t *w, uint32_t n)
{
    uint32_t total = n + 1u;
    for (uint32_t i = 0; i < total; i++) {
        if (g_wpos > W3M_PAY) {
            g_wline++;
            g_wpos = 1;
            if (g_wline >= W3M_LINES) { g_wline = W3M_LINES - 1u; return; }
            g_stream[16u * g_wline] = (g_batch & 0xFFFFu) << 16 | (g_wline & 0xFFFFu);
        }
        g_stream[16u * g_wline + g_wpos++] = i == 0 ? type << 24 | n : w[i - 1];
    }
}

/* everything put so far becomes visible to the ME (the open line is padded: published lines never change) */
void w3dme_publish(void)
{
    if (g_wpos > 1u) {
        while (g_wpos <= W3M_PAY) g_stream[16u * g_wline + g_wpos++] = 0;
        g_wline++;
        g_wpos = 1;
        if (g_wline < W3M_LINES) g_stream[16u * g_wline] = (g_batch & 0xFFFFu) << 16 | (g_wline & 0xFFFFu);
    }
    if (g_wline == g_pub) return;
#ifdef HLE_PSP
    sceKernelDcacheWritebackRange(g_stream + 16u * g_pub, (g_wline - g_pub) * 64u);
    __asm__ volatile("sync" ::: "memory");
#else
    __sync_synchronize();
#endif
    g_pub = g_wline;
    g_ctl->lines = g_pub;
}

/* wait for the ME to finish the batch: 1 = done (parts ready), 0 = failed (code in fail) or no progress for stall_us
   (then the ME is given up for the session: the main core draws) */
int w3dme_wait(uint32_t stall_us, uint32_t *fail, uint32_t *nparts, uint32_t *arena_end)
{
    uint32_t t0 = w3d_plat_us(), tp = t0, last = g_ctl->lines_done, last_poll = g_ctl->polls;
#ifdef HLE_PSP
    /* W3D_BB_WAIT: spin without a clock system call per turn (one uncached read of done per ~1 us) for up
       to 2 ms; then the loop below (its clock, the stall check and DelayThread). A batch the ME finishes
       within the spin returns as soon as it is done instead of after a DelayThread's sleep. */
    if ((w3d_opt_bb & W3D_BB_WAIT) && g_runner != 1) {   /* (runner 1 = a main-core thread: it must get the CPU) */
        for (int turn = 0; g_ctl->done != g_batch; turn++) {
            for (int i = 0; i < 256; i++) __asm__ volatile("nop");
            if ((turn & 63) == 63 && w3d_plat_us() - t0 > 2000u) break;
        }
        gs_bb_spin++;
    }
#endif
    for (int spin = 0; g_ctl->done != g_batch; spin++) {
        uint32_t now = w3d_plat_us(), ld = g_ctl->lines_done, pl = g_ctl->polls;
        if (ld != last || pl != last_poll) { last = ld; last_poll = pl; tp = now; }
        if (now - tp > stall_us) {
            gs_timeouts++;
            if (++g_timeouts_row >= 3) g_dead = 1;
            *fail = 5;
            LOG("world3d ME: no progress for %u us in batch %u (lines %u of %u, polls %u) - the main core draws it%s\n",
                stall_us, g_batch, ld, g_pub, pl, g_dead ? "; third in a row: the main core draws from now on" : "");
            return 0;
        }
#ifdef HLE_PSP
        if (spin < 64) { for (int i = 0; i < 256; i++) __asm__ volatile("nop"); }
        else sceKernelDelayThread(g_runner == 1 ? 100 : 20);
#else
        if (spin > 64) usleep(10);
#endif
    }
#ifdef HLE_PSP
    __asm__ volatile("sync" ::: "memory");
#else
    __sync_synchronize();
#endif
    g_timeouts_row = 0;
    uint32_t dt = w3d_plat_us() - t0;
    gs_wait_us += dt;
    if (dt > gs_wait_max) gs_wait_max = dt;
    *fail = g_ctl->fail;
    *nparts = g_ctl->nparts;
    *arena_end = g_ctl->arena_end;
    /* this batch's ME output (counted whether or not BURST was on) */
    gs_batches_done++;
    gs_awords += g_ctl->st_awords;
    if (g_ctl->st_wb) { gs_bwords += g_ctl->st_awords; gs_wb += g_ctl->st_wb; }
    gs_parts += *nparts;
    gs_units += g_ctl->st_units;
    bus_quiet((w3d_opt_me_bus & W3M_BUS_QUIET) != 0);              /* no batch open until the next one */
    return *fail == 0;
}

int w3dme_dead(void) { return g_dead; }
int w3dme_done(void) { return g_ctl && g_ctl->done == g_batch; }
uint32_t w3dme_unpublished(void) { return g_wline - g_pub + (g_wpos > 1u ? 1u : 0u); }

void w3dme_report(void)
{
    if (!g_ctl) return;
    LOG("world3d ME: runner %s, %u batches sent, waits %.3f ms total (max %u us), %llu timeouts, %llu batches without a "
        "poller (main core), last batch: %u meshes (%u culled by chunks, %u to the main core), %u segments, %u words, %u "
        "ranges, %u two-pass, %u stale-line retries, %u units, fail %u\n",
        g_runner == 0 ? "the shared ME loop" : g_runner == 1 ? "thread (simulated ME)" : "own ME loop", g_batch,
        (double)gs_wait_us / 1000.0, (unsigned)gs_wait_max, (unsigned long long)gs_timeouts,
        (unsigned long long)gs_nopoll, g_ctl->st_meshes, g_ctl->st_cull, g_ctl->st_main, g_ctl->st_segs,
        g_ctl->st_words, g_ctl->st_ranges, g_ctl->st_twopass, g_ctl->st_stale, g_ctl->st_units, g_ctl->fail);
    (void)gs_full;
    if (gs_bb_spin) LOG("world3d ME (w3d_bb wait): %llu waits began with the 2 ms spin\n", (unsigned long long)gs_bb_spin);
}

/* what the ME's 3D job puts on the shared bus, per main-loop iteration. Uncached ME accesses of this job:
   arena words (unless BURST: then one 64-byte write-back per line), 4 words per part, and per poll ~5 (the control
   block's address, runner flag, polls read + write, batch; + lines while a batch is open); per unit (an item) + the
   ME loop's bookkeeping ~8 (hle_me_audio.c zma_job_step). The ME loop's idle round itself adds ~8 more per idle poll. */
void w3dme_report_bus(double it)
{
    if (!g_ctl) return;
    double n = it > 0 ? it : 1.0;
    uint32_t polls = g_ctl->polls - g_polls0;
    LOG("world3d ME bus (w3d_me_bus=%d): %llu batches; per iteration: %.0f arena words (%.0f through the ME's "
        "cache in %.0f line write-backs, %.0f uncached), %.0f part words, %.0f units, %.0f polls of the 3D job -> ~%.0f "
        "uncached ME accesses by the 3D job + its polls (~%.0f with BURST on every batch); quiet switches %llu\n",
        w3d_opt_me_bus, (unsigned long long)gs_batches_done, (double)gs_awords / n, (double)gs_bwords / n,
        (double)gs_wb / n, (double)(gs_awords - gs_bwords) / n, 4.0 * (double)gs_parts / n, (double)gs_units / n,
        (double)polls / n,
        ((double)(gs_awords - gs_bwords) + (double)gs_wb + 4.0 * (double)gs_parts + 8.0 * (double)gs_units +
         5.0 * (double)polls) / n,
        ((double)gs_awords / 16.0 + 4.0 * (double)gs_parts + 8.0 * (double)gs_units + 5.0 * (double)polls) / n,
        (unsigned long long)gs_quiet_sets);
}
