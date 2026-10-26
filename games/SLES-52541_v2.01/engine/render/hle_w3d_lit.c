/* hle_w3d_lit.c - the lit and skinned pipelines of the 3D world on the PSP GE. The core (hle_w3d.c: hook, vertex
 * cache, building pipelines) passes every atomic of another driver pipeline here; 0 = not handled (the translated mesh
 * node runs exactly as before).
 *
 * Pipelines (per pipeline cfg key, PSP cfg file / host HLE_<KEY>):
 *   w3d_peds: 0x53F20088 (the game's ped skin pipeline: gameplay peds incl. the player character) and 0x00011002 (the
 *             render driver's PS2 skin pipeline: cutscene actors and skinned props);
 *   w3d_cars: 0x53F20084 (the game's vehicle pipeline: vehicle bodies, wheels, bicycles);
 *   w3d_lit:  0x00005001 (the render driver's generic PS2 pipeline for lit props: small objects of the opening scene).
 * Native mesh formats (measured on host dumps of the intro and of gameplay): VIF UNPACKs per attribute slot -
 *   0x53F20088: 0 position V4_16 /1024 (w = ADC), 1 UV V2_16 /4096, 2 normal V4_8, 3 skin V4_32;
 *   0x00011002: 0 position V3_32, 1 UV V2_32, 2 prelight V4_8 (black, unused), 3 normal V3_8, 4 skin V4_32;
 *   0x53F20084: 0 position V4_16 /1024 (w = ADC), 1 UV V2_16, 3 normal V4_8 (2 prelight if present);
 *   0x00005001: 0 position V3_32, 1 UV V2_32 (V4_32: two sets, the first is used), 2 prelight V4_8, 3 normal V3_8.
 *   Skin V4_32 = 4 weights as floats whose low 10 mantissa bits hold the bone's VU matrix offset (4 qwords per bone:
 *   bone = ((bits & 0x3FF) >> 2) - 1 (the matrices sit at VU 720 + 4 x bone, the VU code reads 716 + offset; checked
 *   on the dumps).
 * Bone matrices: the guest's own skin-matrix function 0x3C2458 (atomic, skin data) - the matrices the PS2 pipeline
 * uploads: skin-to-bone x bone LTM x inverse atomic LTM = the atomic's space (skin plugin: skin data = geometry +
 * *(0x601948), bone hierarchy = atomic + *(0x601944)).
 * Conversion (once per mesh, cached in the core's arena; validated like the buildings): the stream is walked as strips
 * (ADC breaks; float formats: one strip with the driver's degenerate joins), every triangle's bone set is merged into
 * the open group's palette (<= w3d_bones bones, default 8 = the GE's limit); a triangle that does not fit closes the group (and
 * its sub-strip). Group = its palette, its vertices (N weights u8 /128, UV u16 + tile base, normal s8, position s16 in
 * 1/1024 units or a power of two that fits - the uniform scale goes into the bone / world matrices: the GE divides
 * N.L by |N|) and a GE sub-list
 * (VTYPE, PRIM per sub-strip, the 0xFF000000 dummy word the GE's skinning needs after a skinned PRIM, RET). Lit
 * meshes = one group without weights.
 * Drawing: GE world = LTM x C (C = the inverse of the camera's frame LTM: rigid, camera-relative, as GE lighting
 * needs), projection = S x P (S = camera LTM x the driver's view matrix: its view-window scale / offset, which
 * would distort the normals in the world matrix), lights = every light of the world's directional light list with
 * the "light atomics" flag (the game: the ambient light at gp+204, the main directional light at gp+208 and up to 6
 * extra directional lights), in camera space,
 * merged down to the GE's 4 lights (GE: AC = the ambient sum, lights 0..3 directional diffuse, material ambient /
 * diffuse = material colour x surface properties), bone matrices per group (BOFS + BONE words), then the group's
 * sub-list. Defaults: all three keys on (checked: host frames vs PCSX2, PPSSPP intro + gameplay). */
#include "hle.h"
#include "hle_cfg.h"                          /* w3d_ldirfix */
#include "hle_render.h"
#include "hle_w3d_int.h"
#include "hle_w3d_me.h"                       /* lit / skinned records on the ME */
#ifdef HLE_PSP
#include <pspkernel.h>
#endif
#include <stdlib.h>
#include <math.h>

#define G_RENDER_CTX 0x0082B440u
#define DRV_ENGINE 0x00825C30u             /* the render driver's globals: +0 current camera */
#define GP_MESHCACHE_GEO 0x0066CB68u      /* offset of the mesh-cache pointer in a geometry (one morph target) */
#define GP_MESHCACHE_ATOMIC 0x0066CB6Cu   /* ... in an atomic (several morph targets) */
#define GP_SKIN_GLOBALS 0x00601940u       /* skin plugin globals: +4 atomic offset (bone hierarchy *), +8 geometry
                                             offset (skin data *), +0xC the skin matrix buffer (fn 0x3C2458 fills it) */
#define FN_SKIN_MATRICES 0x003C2458u      /* the skin-matrix function (atomic, skin): bone matrices in atomic space */
#define FN_FRAME_MATRIX 0x0033FAC0u
#define GP_AMBIENT_LIGHT 0x0066BA3Cu      /* gp (0x66B970) + 204: the ambient light * (set by the time-of-day lights) */
#define GP_DIRECT_LIGHT 0x0066BA40u       /* gp + 208: the main directional light * */
#define PDS_PED 0x53F20088u
#define PDS_DRVSKIN 0x00011002u
#define PDS_CAR 0x53F20084u
#define PDS_DRVLIT 0x00005001u

int w3d_opt_peds = 1, w3d_opt_cars = 1, w3d_opt_lit = 1, w3d_opt_bones = 8;   /* on by default (checked on host + PPSSPP) */
int w3d_opt_carpaint = 1;         /* the vehicle pipeline bridges' per-draw paint / lights / paint-job / alpha rules
                                     (0 = the material as it is: placeholder green / magenta paint) */
int w3d_opt_lights = 4;           /* GE light slots for the game's directional lights (0 = only the ambient + the
                                     main directional light); the world's light list is merged down to this many */

void w3d_lit_cfg(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "w3d_peds")) w3d_opt_peds = x;
    else if (!strcmp(k, "w3d_cars")) w3d_opt_cars = x;
    else if (!strcmp(k, "w3d_lit")) w3d_opt_lit = x;
    else if (!strcmp(k, "w3d_bones")) w3d_opt_bones = x;
    else if (!strcmp(k, "w3d_lights")) w3d_opt_lights = x;
    else if (!strcmp(k, "w3d_carpaint")) w3d_opt_carpaint = x;
}

/* ------------------------------------------------------------------ statistics */
static uint64_t sl_atomics[4], sl_meshes, sl_groups, sl_tris, sl_conv, sl_conv_fail, sl_conv_bytes, sl_fail, sl_noskin,
    sl_bones_up, sl_cull, sl_gcull, sl_why[8];
enum { LK_PED = 0, LK_DRVSKIN = 1, LK_CAR = 2, LK_DRVLIT = 3 };
static const char *lk_name[4] = {"ped skin 53f20088", "driver skin 00011002", "vehicle 53f20084", "driver lit 00005001"};

/* attribute slot maps: slot -> 0 position, 1 UV, 2 prelight, 3 normal, 4 skin, -1 ignored */
static const int8_t map_ped[8] = {0, 1, 3, 4, -1, -1, -1, -1};
static const int8_t map_drvskin[8] = {0, 1, -1, 3, 4, -1, -1, -1};
static const int8_t map_car[8] = {0, 1, -1, 3, -1, -1, -1, -1};
static const int8_t map_drvlit[8] = {0, 1, -1, 3, -1, -1, -1, -1};

static inline uint32_t gwk(uint32_t a) { return w3d_gwk(a); }
static inline float gfk(uint32_t a) { uint32_t w = gwk(a); float f; memcpy(&f, &w, 4); return f; }
static int ram_ok(uint32_t a, uint32_t n) { return a >= 0x1000u && a < hle_ram_end && n <= hle_ram_end - a; }

/* ------------------------------------------------------------------ one stream vertex */
typedef struct {
    float p[3], uv[2];
    int8_t n[3];
    uint8_t adc, nb;
    uint8_t bone[4];
    float w[4];
} LV;

static void read_lv(const Batch *b, int j, LV *o, float pscale)
{
    uint8_t *ram = hle_ram;
    switch (b->pf) {
    case PF_V4_16: { const int16_t *q = (const int16_t *)(ram + b->pos) + 4 * j;
        o->p[0] = q[0] * pscale; o->p[1] = q[1] * pscale; o->p[2] = q[2] * pscale; o->adc = q[3] != 0; break; }
    case PF_V3_16: { const int16_t *q = (const int16_t *)(ram + b->pos) + 3 * j;
        o->p[0] = q[0] * pscale; o->p[1] = q[1] * pscale; o->p[2] = q[2] * pscale; o->adc = 0; break; }
    case PF_V4_32: { float f[4]; memcpy(f, ram + b->pos + 16u * (uint32_t)j, 16);
        o->p[0] = f[0]; o->p[1] = f[1]; o->p[2] = f[2]; uint32_t w; memcpy(&w, &f[3], 4); o->adc = w != 0; break; }
    default: { float f[3]; memcpy(f, ram + b->pos + 12u * (uint32_t)j, 12);
        o->p[0] = f[0]; o->p[1] = f[1]; o->p[2] = f[2]; o->adc = 0; break; }
    }
    o->uv[0] = o->uv[1] = 0.0f;
    switch (b->uf) {
    case UF_V2_16: { const int16_t *q = (const int16_t *)(ram + b->uv) + 2 * j;
        o->uv[0] = q[0] * (1.0f / 4096.0f); o->uv[1] = q[1] * (1.0f / 4096.0f); break; }
    case UF_V4_16: { const int16_t *q = (const int16_t *)(ram + b->uv) + 4 * j;
        o->uv[0] = q[0] * (1.0f / 4096.0f); o->uv[1] = q[1] * (1.0f / 4096.0f); break; }
    case UF_V2_32: memcpy(o->uv, ram + b->uv + 8u * (uint32_t)j, 8); break;
    case UF_V4_32: memcpy(o->uv, ram + b->uv + 16u * (uint32_t)j, 8); break;
    default: break;
    }
    o->n[0] = 0; o->n[1] = 0; o->n[2] = 127;
    if (b->nf == NF_V4_8) { const int8_t *q = (const int8_t *)(ram + b->nrm) + 4 * j; o->n[0] = q[0]; o->n[1] = q[1]; o->n[2] = q[2]; }
    else if (b->nf == NF_V3_8) { const int8_t *q = (const int8_t *)(ram + b->nrm) + 3 * j; o->n[0] = q[0]; o->n[1] = q[1]; o->n[2] = q[2]; }
    o->nb = 0;
    if (b->wf == WF_V4_32) {
        uint32_t w[4];
        memcpy(w, ram + b->wgt + 16u * (uint32_t)j, 16);
        for (int i = 0; i < 4; i++) {
            float f;
            memcpy(&f, &w[i], 4);
            if (!(f > 0.001f)) continue;
            /* the low bits are the VU offset of the bone's matrix rows from 716 (the VU code loads rows 716..719 +
               offset); the object setup (0x510530 -> 0x5102A0) unpacks the matrices to 720 + 4 x bone: bone =
               offset / 4 - 1 (checked on dumps: offsets / 4 run 1..numBones; with base 1 the images of a
               multi-bone vertex through its bones agree 4-5x better, and the last bone no longer falls off the
               palette - stretched "rods" from the player character's hands) */
            uint32_t ob = (w[i] & 0x3FFu) >> 2;
            o->bone[o->nb] = (uint8_t)(ob ? ob - 1u : 0u);
            o->w[o->nb++] = f;
        }
    }
}

/* ------------------------------------------------------------------ conversion */
/* GE vertex type bits (pspgu.h) */
#define VT_UV16 (2u)
#define VT_N8 (1u << 5)
#define VT_PF (3u << 7)
#define VT_P16 (2u << 7)
#define VT_W8 (1u << 9)
#define VT_WCOUNT(n) (((uint32_t)(n) - 1u) << 14)
#define GE_CMD(c, a) (((uint32_t)(c) << 24) | ((uint32_t)(a) & 0xFFFFFFu))

/* layout of a group's vertex: N weights u8, UV u16 x2, normal s8 x3, position s16 x3 in 1/pscale units (GE: every
   element aligned to its size, the vertex to its largest element: 2 bytes here). The position scale goes into the
   world / bone matrices as a uniform factor: the GE divides N.L by |N| (measured), so lighting is unaffected */
typedef struct { uint32_t uv, n, p, size; } VLay;
static VLay vlay(int nw)
{
    VLay l;
    l.uv = ((uint32_t)nw + 1u) & ~1u;
    l.n = l.uv + 4u;
    l.p = (l.n + 3u + 1u) & ~1u;
    l.size = l.p + 6u;
    return l;
}

static void put_pos(uint8_t *o, const float *p, float scale)
{
    int16_t q[3];
    for (int i = 0; i < 3; i++) {
        float f = p[i] * scale;
        f = f + (f < 0.0f ? -0.5f : 0.5f);
        q[i] = (int16_t)(f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (int)f);
    }
    memcpy(o, q, 6);
}

static void get_pos(const uint8_t *o, float inv, float *p)
{
    int16_t q[3];
    memcpy(q, o, 6);
    for (int i = 0; i < 3; i++) p[i] = q[i] * inv;
}

/* a group of the converted mesh (stored after the vertices and sub-lists): LGroup, hle_w3d_int.h */

/* the vertex's weights at their palette slots, u8 /128, summing to exactly 128: the GE does not normalise (measured:
   sum of weight x bone, weights as given) - the largest weight takes the rounding remainder; bones not in the
   palette (a triangle with more bones than fit) are dropped and the rest renormalised */
static void put_weights(uint8_t *o, const LV *v, const uint8_t *pal, int npal)
{
    int slot[4], n = 0;
    float w[4], sum = 0.0f;
    for (int b = 0; b < v->nb; b++)
        for (int q = 0; q < npal; q++)
            if (pal[q] == v->bone[b]) { slot[n] = q; w[n] = v->w[b]; sum += v->w[b]; n++; break; }
    if (!n || !(sum > 0.0f)) { o[0] = 128; return; }              /* no weight: the palette's first bone */
    int q[4], tot = 0, big = 0;
    for (int i = 0; i < n; i++) {
        q[i] = (int)(w[i] / sum * 128.0f + 0.5f);
        tot += q[i];
        if (w[i] > w[big]) big = i;
    }
    q[big] += 128 - tot;
    for (int i = 0; i < n; i++) o[slot[i]] = (uint8_t)(q[i] < 0 ? 0 : q[i]);
}

#define LMAXG 128
#define LMAXP 2048
typedef struct {
    uint32_t vstart, vcount, pstart, ntris;
    uint8_t nb, bone[8];
} PGroup;
static PGroup pg[LMAXG];
static uint32_t pcount[LMAXP];            /* PRIM vertex counts (all groups, in order) */
static int npg, npc;

/* the stream walk: pass 1 (dst NULL) forms the groups (palettes, sub-strips, vertex / PRIM counts), pass 2 writes the
   vertices of every group with its layout. Returns 0 when a table overflows. */
static int lwalk(int strip, uint32_t cap, float pscale, int maxb, uint8_t *dst, const uint32_t *goff, int32_t ubi,
                 int32_t vbi, float *bmin, float *bmax, float *uvmin, float *uvmax, float oscale)
{
    LV h1, h2, cur;
    memset(&h1, 0, sizeof h1);
    memset(&h2, 0, sizeof h2);
    uint32_t k = 0, nv = 0, run = 0, in_run = 0;
    int g = -1;
    uint8_t pal[8];
    int npal = 0;
    VLay lay = vlay(maxb ? 1 : 0);
    int pass2 = dst != NULL;
    if (!pass2) { npg = 0; npc = 0; }
    int gi = -1;                                   /* pass 2: the group being written */
    uint32_t gv = 0;                               /* pass 2: vertex index inside the group */
#define EMIT(v_) do { \
        if (pass2) { \
            uint8_t *o_ = dst + goff[gi] + gv * lay.size; \
            memset(o_, 0, lay.size); \
            if (maxb) put_weights(o_, &(v_), pal, npal); \
            uint16_t u_[2]; \
            for (int a_ = 0; a_ < 2; a_++) { float f_ = (v_).uv[a_] * 4096.0f - (float)(a_ ? vbi : ubi) + 0.5f; \
                u_[a_] = (uint16_t)(f_ < 0.0f ? 0 : f_ > 65535.0f ? 65535 : (int)f_); } \
            memcpy(o_ + lay.uv, u_, 4); \
            memcpy(o_ + lay.n, (v_).n, 3); \
            put_pos(o_ + lay.p, (v_).p, oscale); \
            gv++; } \
        nv++; run++; } while (0)
#define CLOSE_STRIP() do { if (in_run) { if (!pass2) { if (npc >= LMAXP) return 0; pcount[npc++] = run; } in_run = 0; } } while (0)
#define CLOSE_GROUP() do { CLOSE_STRIP(); if (!pass2 && g >= 0) { if (maxb && !npal) { pal[0] = 0; npal = 1; }                                pg[g].nb = (uint8_t)npal; memcpy(pg[g].bone, pal, 8); \
                               pg[g].vcount = nv - pg[g].vstart; } g = -1; } while (0)
#define OPEN_GROUP() do { if (!pass2) { if (npg >= LMAXG) return 0; g = npg++; memset(&pg[g], 0, sizeof pg[g]); \
                              pg[g].vstart = nv; pg[g].pstart = (uint32_t)npc; } else { gi++; gv = 0; \
                              lay = vlay(maxb ? (pg[gi].nb ? pg[gi].nb : 1) : 0); memcpy(pal, pg[gi].bone, 8); \
                              npal = pg[gi].nb; } \
                          g = pass2 ? gi : g; } while (0)
    for (int i = 0; i < 3; i++) { bmin[i] = 1e30f; bmax[i] = -1e30f; }
    uvmin[0] = uvmin[1] = 1e30f; uvmax[0] = uvmax[1] = -1e30f;
    /* pass 2 follows pass 1's group boundaries: the triangle where each group starts */
    uint32_t tri_no = 0;
    int next_g = 0;
    for (int bi = 0; bi < w3d_nbat; bi++) {
        const Batch *b = &w3d_bat[bi];
        for (int j = b->skip; j < b->n && k < cap; j++, k++) {
            read_lv(b, j, &cur, pscale);
            for (int i = 0; i < 3; i++) {
                if (cur.p[i] < bmin[i]) bmin[i] = cur.p[i];
                if (cur.p[i] > bmax[i]) bmax[i] = cur.p[i];
            }
            for (int i = 0; i < 2; i++) {
                if (cur.uv[i] < uvmin[i]) uvmin[i] = cur.uv[i];
                if (cur.uv[i] > uvmax[i]) uvmax[i] = cur.uv[i];
            }
            /* the triangle (k-2, k-1, k) = (h2, h1, cur): every one of a strip, every third of a list */
            if (k < 2 || (!strip && (k % 3) != 2)) { h2 = h1; h1 = cur; continue; }
            int draw = strip ? !cur.adc : 1;
            if (draw) {
                const LV *tv[3] = {&h2, &h1, &cur};
                /* does the triangle fit the open group's palette? */
                int fits = g >= 0 || pass2;
                if (!pass2) {
                    uint8_t add[12];
                    int nadd = 0;
                    for (int pass_ = 0; pass_ < 2; pass_++) {   /* 2nd round: a new group needs all its bones */
                        nadd = 0;
                        if (maxb)
                            for (int t = 0; t < 3; t++)
                                for (int b_ = 0; b_ < tv[t]->nb; b_++) {
                                    uint8_t bn = tv[t]->bone[b_];
                                    int have = 0;
                                    for (int q = 0; q < npal && !have; q++) have = pal[q] == bn;
                                    for (int q = 0; q < nadd && !have; q++) have = add[q] == bn;
                                    if (!have && nadd < 12) add[nadd++] = bn;
                                }
                        if (g >= 0 && npal + nadd <= maxb) break;
                        if (g >= 0 && pass_ == 1) break;
                        if (g >= 0) CLOSE_GROUP();
                        OPEN_GROUP();
                        npal = 0;
                        fits = 0;
                    }
                    for (int q = 0; q < nadd && npal < 8; q++) pal[npal++] = add[q];
                    if (maxb && npal > maxb) npal = maxb;   /* (a triangle with > maxb bones: the rest dropped) */
                }
                if (pass2 && next_g < npg && tri_no == pg[next_g].ntris) {   /* ntris holds the group's first triangle */
                    CLOSE_STRIP();
                    OPEN_GROUP();
                    next_g++;
                    fits = 0;
                }
                if (!pass2 && !fits) pg[g].ntris = tri_no;   /* pass 1: remember where the group starts */
                if (!in_run || !fits) {
                    CLOSE_STRIP();
                    run = 0;
                    in_run = 1;
                    if (strip && (k - 2) & 1) EMIT(h2);     /* odd start: repeat, so the GE parity = the stream's */
                    EMIT(h2);
                    EMIT(h1);
                }
                EMIT(cur);
                tri_no++;
            } else CLOSE_STRIP();
            if (!strip) { if (in_run) { CLOSE_STRIP(); } }  /* lists: every triangle its own strip of 3 */
            h2 = h1;
            h1 = cur;
        }
    }
    CLOSE_GROUP();
#undef EMIT
#undef CLOSE_STRIP
#undef CLOSE_GROUP
#undef OPEN_GROUP
    return 1;
}

static int lit_convert(Ent *e, uint32_t cap, int strip, int kind, const int8_t *map)
{
    uint32_t t0 = w3d_plat_us();
    int n = w3d_parse_chain_map(e->data, strip, map);
    if (n <= 0) { sl_conv_fail++; return 0; }
    for (int i = 0; i < w3d_nbat; i++)
        if (!w3d_bat[i].pf) { sl_conv_fail++; return 0; }
    int maxb = kind == LK_PED || kind == LK_DRVSKIN ? w3d_opt_bones : 0;
    if (maxb > 8) maxb = 8;
    if (maxb && maxb < 1) maxb = 1;
    if (maxb)
        for (int i = 0; i < w3d_nbat; i++)
            if (!w3d_bat[i].wf) { sl_noskin++; maxb = 0; break; }      /* a skinned pipeline without skin data */
    float pscale = 1.0f / 1024.0f;
    float bmin[3], bmax[3], uvmin[2], uvmax[2];
    uint32_t capx = cap ? cap : 0xFFFFFFFFu;
    if (!lwalk(strip, capx, pscale, maxb, NULL, NULL, 0, 0, bmin, bmax, uvmin, uvmax, 1.0f)) { sl_conv_fail++; return 0; }
    if (!npg) { sl_conv_fail++; return 0; }
    /* positions: s16 x oscale - 1024 for the 16-bit sources (their own values), else the largest power of two that keeps
       the bind-pose box inside +-32000 */
    float oscale = 1024.0f, mxa = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(bmin[i]) > mxa) mxa = fabsf(bmin[i]);
        if (fabsf(bmax[i]) > mxa) mxa = fabsf(bmax[i]);
    }
    int fsrc = w3d_bat[0].pf == PF_V3_32 || w3d_bat[0].pf == PF_V4_32;
    if (fsrc || mxa * oscale > 32000.0f) {
        oscale = 32768.0f;
        while (oscale > 1.0f && mxa * oscale > 32000.0f) oscale *= 0.5f;
    }
    /* UV: u16 in 1/4096 units above the floor tile; a span of 16 tiles or more is clamped (not seen in the dumps) */
    int32_t ubi = (int32_t)floorf(uvmin[0]) * 4096, vbi = (int32_t)floorf(uvmin[1]) * 4096;
    /* layout: [group vertices][group sub-lists][group table] */
    uint32_t off = 0, goff[LMAXG], soff[LMAXG];
    for (int g = 0; g < npg; g++) {
        VLay l = vlay(maxb ? (pg[g].nb ? pg[g].nb : 1) : 0);
        goff[g] = off;
        off += (pg[g].vcount * l.size + 15u) & ~15u;
    }
    uint32_t vbytes = off;
    for (int g = 0; g < npg; g++) {
        soff[g] = off;
        uint32_t np = g + 1 < npg ? pg[g + 1].pstart - pg[g].pstart : (uint32_t)npc - pg[g].pstart;
        off += 4u * (np + 3u);                   /* VTYPE, PRIMs, dummy, RET */
    }
    uint32_t toff = (off + 15u) & ~15u, bytes = toff + (uint32_t)sizeof(LGroup) * (uint32_t)npg, aoff;
    if (!w3d_arena_get_for(e, bytes, &aoff)) { w3d_stat_noroom(); return 0; }
    uint8_t *blk = w3d_arena_base() + aoff;
    if (!lwalk(strip, capx, pscale, maxb, blk, goff, ubi, vbi, bmin, bmax, uvmin, uvmax, oscale)) { sl_conv_fail++; return 0; }
    LGroup *gt = (LGroup *)(blk + toff);
    uint32_t ntris = 0;
    for (int g = 0; g < npg; g++) {
        int nw = maxb ? (pg[g].nb ? pg[g].nb : 1) : 0;
        uint32_t vt = VT_UV16 | VT_N8 | VT_P16 | (nw ? VT_W8 | VT_WCOUNT(nw) : 0);
        uint32_t *sub = (uint32_t *)(blk + soff[g]);
        uint32_t np = g + 1 < npg ? pg[g + 1].pstart - pg[g].pstart : (uint32_t)npc - pg[g].pstart, s = 0, tris = 0;
        sub[s++] = GE_CMD(0x12, vt);
        for (uint32_t p = 0; p < np; p++) {
            uint32_t cnt = pcount[pg[g].pstart + p];
            sub[s++] = GE_CMD(0x04, 4u << 16 | cnt);      /* triangle strip */
            tris += cnt >= 2 ? cnt - 2 : 0;
        }
        sub[s++] = 0xFF000000u;                           /* skinning dummy word (harmless without skinning) */
        sub[s++] = GE_CMD(0x0B, 0);
        memset(&gt[g], 0, sizeof gt[g]);
        gt[g].voff = goff[g];
        gt[g].suboff = soff[g];
        gt[g].nverts = pg[g].vcount;
        gt[g].ntris = tris;
        gt[g].nprims = (uint16_t)np;
        gt[g].nb = (uint8_t)(maxb ? pg[g].nb : 0);
        memcpy(gt[g].bone, pg[g].bone, 8);
        VLay l = vlay(maxb ? (pg[g].nb ? pg[g].nb : 1) : 0);
        for (int a = 0; a < 3; a++) { gt[g].bmin[a] = 1e30f; gt[g].bmax[a] = -1e30f; }
        for (int b = 0; b < 8; b++)
            for (int a = 0; a < 3; a++) { gt[g].bbmin[b][a] = 1e30f; gt[g].bbmax[b][a] = -1e30f; }
        int nslot = gt[g].nb;
        for (uint32_t v = 0; v < pg[g].vcount; v++) {
            float pp[3];
            const uint8_t *vv = blk + goff[g] + v * l.size;
            get_pos(vv + l.p, 1.0f / oscale, pp);
            for (int a = 0; a < 3; a++) {
                if (pp[a] < gt[g].bmin[a]) gt[g].bmin[a] = pp[a];
                if (pp[a] > gt[g].bmax[a]) gt[g].bmax[a] = pp[a];
            }
            for (int b = 0; b < nslot; b++) {
                if (!vv[b]) continue;
                for (int a = 0; a < 3; a++) {
                    if (pp[a] < gt[g].bbmin[b][a]) gt[g].bbmin[b][a] = pp[a];
                    if (pp[a] > gt[g].bbmax[b][a]) gt[g].bbmax[b][a] = pp[a];
                }
            }
        }
        ntris += tris;
    }
    e->off = aoff;
    e->bytes = bytes;
    e->vbytes = vbytes;
    e->xoff = toff;
    e->nx = (uint16_t)npg;
    e->ntris = ntris;
    e->nstream = cap && cap < (uint32_t)n ? cap : (uint32_t)n;
    e->strip = (uint8_t)strip;
    e->ubase = (float)(ubi / 4096);
    e->vbase = (float)(vbi / 4096);
    e->stride = (uint8_t)maxb;
    e->pscale = 1.0f / oscale;
    for (int i = 0; i < 3; i++) { e->bmin[i] = bmin[i]; e->bmax[i] = bmax[i]; }
    w3d_plat_flush(blk, bytes);
    w3d_stat_conv(bytes);
    sl_conv++;
    sl_conv_bytes += bytes;
    w3d_u_lit_conv += w3d_plat_us() - t0;
    return 1;
}

/* ------------------------------------------------------------------ the camera: rigid part C and the driver's skew S */
static uint32_t lc_camera, lc_frame;
static M43 lc_C;                          /* world -> camera space (inverse of the camera frame LTM) */
static M43 lc_S;                          /* camera space -> the driver's view space (the guard band / frustum planes) */
static float lc_proj[16];                 /* S x P (row vectors) */
static float lc_rot[3][3];                /* world direction -> camera space */

static uint8_t lc_vkey[64];               /* the camera's view matrix the cached matrices were made from */
static uint32_t lc_gen;                   /* bumped whenever they are made again (light directions follow it) */
static float lc_pkey[16];                 /* the camera projection lc_proj was made from */
int w3d_re_owned(uint32_t re, uint32_t slot);   /* hle_w3d.c */
static int lit_camera(R5900Ctx *c, uint32_t camera)
{
    /* also keyed by the view matrix - the game renders mirrors (mirror_pass) with the SAME camera re-aimed at a
       512x256 raster earlier in the frame; keyed by camera + frame only, the main pass then drew every lit / skinned
       atomic with the mirror's matrices (the player character invisible indoors) */
    uint32_t fr = w3d_frame_no();
    const uint8_t *vk = hle_ram + ((camera + 0x20u) & (HLE_RAM_SIZE - 1));
    /* and by the projection lc_proj is made from (w3d_cam.proj = the camera's near / far planes): with the
       same camera, frame and view matrix but another near / far plane, the cached S x P gave the lit / skinned atomics
       the depth of the old planes while the buildings used the new ones */
    if (camera == lc_camera && fr == lc_frame && !memcmp(lc_vkey, vk, sizeof lc_vkey) &&
        !memcmp(lc_pkey, w3d_cam.proj, sizeof lc_pkey)) return 1;
    uint32_t cframe = gwk(camera + 4);
    uint32_t ltm = cframe ? w3d_frame_ltm(c, cframe) : 0;
    if (!ram_ok(ltm, 0x40)) return 0;
    M43 L, V, S;
    w3d_drv_mat(ltm, &L);
    w3d_drv_mat(camera + 0x20, &V);
    /* C = rigid inverse of L (rows right / up / at / pos): R^T, -pos R^T */
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) lc_C.m[i][k] = L.m[k][i];
    for (int k = 0; k < 3; k++)
        lc_C.m[3][k] = -(L.m[3][0] * L.m[k][0] + L.m[3][1] * L.m[k][1] + L.m[3][2] * L.m[k][2]);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) lc_rot[i][k] = lc_C.m[i][k];
    w3d_mat_mul(&L, &V, &S);              /* camera space -> the driver's view space */
    lc_S = S;
    /* S4 x P: P = w3d_cam.proj (rows = inputs X, Y, Z, 1) */
    const float *P = w3d_cam.proj;
    float S4[4][4] = {{S.m[0][0], S.m[0][1], S.m[0][2], 0}, {S.m[1][0], S.m[1][1], S.m[1][2], 0},
                      {S.m[2][0], S.m[2][1], S.m[2][2], 0}, {S.m[3][0], S.m[3][1], S.m[3][2], 1}};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            lc_proj[4 * r + k] = S4[r][0] * P[k] + S4[r][1] * P[4 + k] + S4[r][2] * P[8 + k] + S4[r][3] * P[12 + k];
    lc_camera = camera;
    lc_frame = fr;
    memcpy(lc_vkey, vk, sizeof lc_vkey);
    memcpy(lc_pkey, w3d_cam.proj, sizeof lc_pkey);
    lc_gen++;
    return 1;
}

/* ------------------------------------------------------------------ the game's lights (ambient / main directional) */
static void read_light_rgb(uint32_t light, float *rgb)
{
    for (int i = 0; i < 3; i++) {
        float v = gfk(light + 0x18 + 4u * (uint32_t)i);
        rgb[i] = !(v > 0.0f) ? 0.0f : v > 2.0f ? 2.0f : v;
    }
}

/* a directional light's direction in camera (C) space, once per light per frame (its frame LTM is a guest call)
   issue #2 "the light gets too dark, then back up": the game re-aims the SAME extra directional light
   objects for every entity it lights (point lights near the entity -> an extra directional light per entity, cutscene
   lights): a cache keyed by light + frame number gave every later entity of the frame the first entity's direction
   (lit from the wrong side, changing with the draw order from frame to frame). The cache now also keys on the light
   frame's modelling matrix "at" vector (light frame +0x10 modelling, +0x20 at: the light frames are root frames, so
   their LTM follows it); w3d_ldirfix=0 = the old cache (the stale hits are counted either way). */
#define LDIR_CACHE 16
static struct { uint32_t light, frame, gen, at[3]; float d[3]; } ldir_cache[LDIR_CACHE];
static int ldir_next;
static int ldir_fix = -1;                         /* w3d_ldirfix (default 1) */
static uint64_t sl_ldir_hits, sl_ldir_stale;
static int light_dir(R5900Ctx *c, uint32_t light, float *out)
{
    uint32_t fno = w3d_frame_no();
    if (ldir_fix < 0) {
#ifdef HLE_PSP
        const char *v = hle_cfg_get("w3d_ldirfix");
#else
        const char *v = getenv("HLE_W3D_LDIRFIX");
        if (!v) v = hle_cfg_get("w3d_ldirfix");
#endif
        ldir_fix = v ? atoi(v) != 0 : 1;
        LOG("world3d lights: w3d_ldirfix=%d (light direction cache keyed by the light frame's matrix)\n", ldir_fix);
    }
    uint32_t fr0 = gwk(light + 4), mat_at[3] = {0, 0, 0};
    if (fr0 && ram_ok(fr0, 0x50)) for (int k = 0; k < 3; k++) mat_at[k] = gwk(fr0 + 0x30 + 4u * (uint32_t)k);
    for (int i = 0; i < LDIR_CACHE; i++)
        if (ldir_cache[i].light == light && ldir_cache[i].frame == fno && ldir_cache[i].gen == lc_gen) {
            int same = !memcmp(ldir_cache[i].at, mat_at, sizeof mat_at);
            sl_ldir_hits++;
            if (!same) sl_ldir_stale++;
            if (same || !ldir_fix) {
                memcpy(out, ldir_cache[i].d, 12);
                return 1;
            }
            ldir_cache[i].light = 0;              /* re-aimed since: recompute below */
        }
    uint32_t fr = fr0, ltm = fr ? w3d_frame_ltm(c, fr) : 0;
    if (!ram_ok(ltm, 0x40)) return 0;
    float at[3], d[3];
    for (int i = 0; i < 3; i++) at[i] = gfk(ltm + 0x20 + 4u * (uint32_t)i);
    /* the driver: a directional light shines along its frame's at vector; the GE wants the direction towards the light */
    for (int k = 0; k < 3; k++) d[k] = -(at[0] * lc_rot[0][k] + at[1] * lc_rot[1][k] + at[2] * lc_rot[2][k]);
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-6f)) return 0;
    for (int k = 0; k < 3; k++) out[k] = d[k] / l;
    int s = ldir_next++ % LDIR_CACHE;
    ldir_cache[s].light = light; ldir_cache[s].frame = fno; ldir_cache[s].gen = lc_gen;
    memcpy(ldir_cache[s].at, mat_at, sizeof mat_at);
    memcpy(ldir_cache[s].d, out, 12);
    return 1;
}

static float lum3(const float *c) { return c[0] + c[1] + c[2]; }

/* the peak of the lit colour (ambient + sum of colour x max(N.L, 0), largest channel) over the normals pointing at a
   light or at the lights' weighted sum: the VU clamps that sum to 1 before it multiplies by the material colour, the GE
   clamps after - see lit_peak below */
static float light_peak(const W3dLights *L)
{
    float cand[5][3], peak = 0.0f, s[3] = {0, 0, 0};
    int nc = 0;
    for (int i = 0; i < L->n; i++) {
        memcpy(cand[nc++], L->dir[i], 12);
        for (int k = 0; k < 3; k++) s[k] += L->dir[i][k] * lum3(L->col[i]);
    }
    float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl > 1e-6f) { for (int k = 0; k < 3; k++) cand[nc][k] = s[k] / sl; nc++; }
    for (int q = 0; q < nc; q++)
        for (int ch = 0; ch < 3; ch++) {
            float v = L->amb[ch];
            for (int i = 0; i < L->n; i++) {
                float d = cand[q][0] * L->dir[i][0] + cand[q][1] * L->dir[i][1] + cand[q][2] * L->dir[i][2];
                if (d > 0.0f) v += L->col[i][ch] * d;
            }
            if (v > peak) peak = v;
        }
    if (!L->n) for (int ch = 0; ch < 3; ch++) if (L->amb[ch] > peak) peak = L->amb[ch];
    return peak;
}

static float lit_peak;            /* light_peak of the current atomic's lights */
static uint64_t sl_light_atomics, sl_light_dirs, sl_light_merged;

static void lit_lights(R5900Ctx *c, W3dLights *L)
{
    /* the render driver lights the atomic with every light in the world's directional (global) light list that has
       the "light atomics" flag (flags bit 0): the ambient lights add up, each directional light adds colour x
       max(N.L, 0) (the driver's light setup + the VU code: sum clamped to 1, then x material colour). The game keeps
       the ambient light, the main directional light and the extra directional lights (cutscenes, nearby point lights:
       up to 6 more) in that list and switches them per entity, so the list is read for every atomic; the directions
       once per frame. */
    float dirs[24][3], cols[24][3];
    int nd = 0;
    memset(L, 0, sizeof *L);
    uint32_t amb = gwk(GP_AMBIENT_LIGHT), dir = gwk(GP_DIRECT_LIGHT);
    uint32_t start = dir && ram_ok(dir, 0x40) ? dir : amb && ram_ok(amb, 0x40) ? amb : 0;
    uint32_t first = start ? gwk(start + 0x34) : 0;      /* a light's in-world list link (+0x34) */
    if (w3d_opt_lights > 0 && first && ram_ok(first, 8)) {
        uint32_t n = start + 0x34;
        for (int q = 0; q < 32 && n && ram_ok(n, 8); q++) {
            uint32_t lt = n - 0x34;
            /* the list head lives in the world object: everything that is not a light (object type 3) is skipped */
            if (ram_ok(lt, 0x40) && gb(lt) == 3 && (gb(lt + 2) & 1)) {
                uint32_t type = gb(lt + 1);
                float col[3];
                read_light_rgb(lt, col);
                if (type == 2) {                                   /* ambient light */
                    for (int k = 0; k < 3; k++) L->amb[k] += col[k];
                } else if (type == 1 && nd < 24 && lum3(col) > 0.0f && light_dir(c, lt, dirs[nd])) {
                    memcpy(cols[nd], col, 12);                     /* directional light */
                    nd++;
                }
            }
            n = gwk(n);
            if (n == start + 0x34) break;
        }
    } else {
        if (amb && ram_ok(amb, 0x40) && (gb(amb + 2) & 1)) read_light_rgb(amb, L->amb);
        if (dir && ram_ok(dir, 0x40) && (gb(dir + 2) & 1) && light_dir(c, dir, dirs[0])) {
            read_light_rgb(dir, cols[0]);
            nd = 1;
        }
    }
    /* more directional lights than GE slots: merge the two closest in direction (their colour-weighted vector sum:
       exact for every normal that faces both) until they fit */
    int slots = w3d_opt_lights < 1 ? 1 : w3d_opt_lights > 4 ? 4 : w3d_opt_lights;
    sl_light_atomics++;
    sl_light_dirs += (uint64_t)nd;
    while (nd > slots) {
        int bi = 0, bj = 1;
        float best = -2.0f;
        for (int i = 0; i < nd; i++)
            for (int j = i + 1; j < nd; j++) {
                float d = dirs[i][0] * dirs[j][0] + dirs[i][1] * dirs[j][1] + dirs[i][2] * dirs[j][2];
                if (d > best) { best = d; bi = i; bj = j; }
            }
        float wi = lum3(cols[bi]), wj = lum3(cols[bj]), s[3];
        for (int k = 0; k < 3; k++) s[k] = dirs[bi][k] * wi + dirs[bj][k] * wj;
        float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), f = wi + wj > 0.0f ? sl / (wi + wj) : 0.0f;
        if (sl > 1e-6f) for (int k = 0; k < 3; k++) dirs[bi][k] = s[k] / sl;
        for (int k = 0; k < 3; k++) cols[bi][k] = (cols[bi][k] + cols[bj][k]) * f;
        memmove(dirs[bj], dirs[bj + 1], sizeof dirs[0] * (size_t)(nd - bj - 1));
        memmove(cols[bj], cols[bj + 1], sizeof cols[0] * (size_t)(nd - bj - 1));
        nd--;
        sl_light_merged++;
    }
    for (int i = 0; i < nd; i++) {
        memcpy(L->dir[i], dirs[i], 12);
        for (int k = 0; k < 3; k++) L->col[i][k] = cols[i][k] > 2.0f ? 2.0f : cols[i][k];
    }
    for (int k = 0; k < 3; k++) if (L->amb[k] > 2.0f) L->amb[k] = 2.0f;
    L->n = nd;
    lit_peak = light_peak(L);
    static int diag = 6;
    if (diag > 0 && (w3d_frame_no() % 97) == 0) {
        diag--;
        LOG("world3d lit lights (frame %u): ambient %.3f %.3f %.3f, %d directional light(s) for the GE, first %.3f %.3f "
            "%.3f dir %.2f %.2f %.2f, peak %.2f\n", w3d_frame_no(), L->amb[0], L->amb[1], L->amb[2], L->n,
            L->col[0][0], L->col[0][1], L->col[0][2], L->dir[0][0], L->dir[0][1], L->dir[0][2], lit_peak);
    }
}

/* ------------------------------------------------------------------ the CPU path of a lit / skinned group
   A group whose posed box leaves the GE guard band (an actor or a car right at the camera) would lose whole triangles on
   the GE (any vertex outside the 4096 window discards the primitive). Its vertices are skinned and moved to camera
   space on the CPU instead, triangles outside the frustum dropped, the rest clipped against the guard band in the
   driver's view space and drawn as camera-space floats (UV, normal, position) with world = identity: the GE still
   lights them. */
typedef struct { float u, v, n[3], p[3]; } LVC;           /* GE: UV float, normal float, position float */
#define LVC_VTYPE (3u | 3u << 5 | 3u << 7)
typedef struct { float cp[3], vp[3], n[3], u, v; unsigned oc; } LCV;
#define LOUT 384
static LVC lout[LOUT];
static int nlout;
static uint64_t sl_cpu_groups, sl_cpu_tris, sl_cpu_clipped;
#define GUARD_X 8.0f
#define GUARD_Y 9.5f

static void lout_flush(void)
{
    if (!nlout) return;
    w3d_world_set(2);
    w3d_be_prims(LVC_VTYPE, lout, nlout);
    sl_cpu_tris += (uint64_t)nlout / 3;
    nlout = 0;
}

static void lout_tri(const LCV *a, const LCV *b, const LCV *c)
{
    if (nlout + 3 > LOUT) lout_flush();
    const LCV *t[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        LVC *o = &lout[nlout++];
        o->u = t[i]->u; o->v = t[i]->v;
        memcpy(o->n, t[i]->n, 12);
        memcpy(o->p, t[i]->cp, 12);
    }
}

static inline uint32_t sgnb(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 31; }

static unsigned lcv_outcode(const float *v)
{
    float X = v[0], Y = v[1], Z = v[2];
    return sgnb((GUARD_X + 1.0f) * Z - 2.0f * X) | sgnb(2.0f * X + (GUARD_X - 1.0f) * Z) << 1 |
           sgnb((GUARD_Y - 1.0f) * Z + 2.0f * Y) << 2 | sgnb((GUARD_Y + 1.0f) * Z - 2.0f * Y) << 3 |
           sgnb(Z - X) << 8 | sgnb(X) << 9 | sgnb(Y) << 10 | sgnb(Z - Y) << 11 | sgnb(Z - w3d_cam.near_z) << 12 |
           sgnb(w3d_cam.far_z - Z) << 13;
}

static float lplane(int pl, const float *v)
{
    float X = v[0], Y = v[1], Z = v[2];
    switch (pl) {
    case 0: return (GUARD_X + 1.0f) * Z - 2.0f * X;
    case 1: return 2.0f * X + (GUARD_X - 1.0f) * Z;
    case 2: return (GUARD_Y - 1.0f) * Z + 2.0f * Y;
    default: return (GUARD_Y + 1.0f) * Z - 2.0f * Y;
    }
}

static void lclip(const LCV *a, const LCV *b, const LCV *c, unsigned planes)
{
    LCV buf[2][10];
    int n = 3, cur = 0;
    buf[0][0] = *a; buf[0][1] = *b; buf[0][2] = *c;
    for (int pl = 0; pl < 4; pl++) {
        if (!(planes & 1u << pl)) continue;
        const LCV *in = buf[cur];
        LCV *out = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const LCV *p = &in[i], *q = &in[(i + 1) % n];
            float dp = lplane(pl, p->vp), dq = lplane(pl, q->vp);
            if (dp >= 0.0f && m < 10) out[m++] = *p;
            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 10) {
                const LCV *x = p, *y = q;            /* canonical endpoint order (watertight shared edges) */
                float dx = dp, dy = dq;
                if (q->vp[0] < p->vp[0] || (q->vp[0] == p->vp[0] && (q->vp[1] < p->vp[1] ||
                                                                     (q->vp[1] == p->vp[1] && q->vp[2] < p->vp[2])))) {
                    x = q; y = p; dx = dq; dy = dp;
                }
                float t = dx / (dx - dy);
                LCV *r = &out[m++];
                for (int k = 0; k < 3; k++) {
                    r->cp[k] = x->cp[k] + (y->cp[k] - x->cp[k]) * t;
                    r->vp[k] = x->vp[k] + (y->vp[k] - x->vp[k]) * t;
                    r->n[k] = x->n[k] + (y->n[k] - x->n[k]) * t;
                }
                r->u = x->u + (y->u - x->u) * t;
                r->v = x->v + (y->v - x->v) * t;
            }
        }
        n = m;
        cur ^= 1;
        if (n < 3) return;
    }
    sl_cpu_clipped++;
    for (int i = 1; i + 1 < n; i++) lout_tri(&buf[cur][0], &buf[cur][i], &buf[cur][i + 1]);
}

static void ltri(const LCV *a, const LCV *b, const LCV *c)
{
    if (a->oc & b->oc & c->oc & 0x3F00) return;          /* outside one frustum plane */
    unsigned any = (a->oc | b->oc | c->oc) & 0x0F;
    if (!any) lout_tri(a, b, c);
    else lclip(a, b, c, any);
}

/* one vertex of a converted group: skin (bm: the group's bone matrices), camera space (W), the driver's view space
   (lc_S) */
static float lcv_inv = 1.0f / 1024.0f;          /* the group's position unit (Ent pscale) */
static void lcv_load(const uint8_t *v, const VLay *l, int nw, const float (*bm)[12], const M43 *W, LCV *o)
{
    float p[3], n[3];
    get_pos(v + l->p, lcv_inv, p);
    const int8_t *nn = (const int8_t *)(v + l->n);
    for (int i = 0; i < 3; i++) n[i] = nn[i] * (1.0f / 127.0f);
    uint16_t uv[2];
    memcpy(uv, v + l->uv, 4);
    o->u = uv[0] * (1.0f / 32768.0f);                     /* GE units: the state's TexScale 8 + TexOffset apply */
    o->v = uv[1] * (1.0f / 32768.0f);
    if (nw) {
        float ps[3] = {0, 0, 0}, ns[3] = {0, 0, 0};
        for (int b = 0; b < nw; b++) {
            float w = v[b] * (1.0f / 128.0f);
            if (w == 0.0f) continue;
            const float *B = bm[b];
            for (int k = 0; k < 3; k++) {
                ps[k] += w * (p[0] * B[k] + p[1] * B[3 + k] + p[2] * B[6 + k] + B[9 + k]);
                ns[k] += w * (n[0] * B[k] + n[1] * B[3 + k] + n[2] * B[6 + k]);
            }
        }
        memcpy(p, ps, 12);
        memcpy(n, ns, 12);
    }
    for (int k = 0; k < 3; k++) {
        o->cp[k] = p[0] * W->m[0][k] + p[1] * W->m[1][k] + p[2] * W->m[2][k] + W->m[3][k];
        o->n[k] = n[0] * W->m[0][k] + n[1] * W->m[1][k] + n[2] * W->m[2][k];
    }
    for (int k = 0; k < 3; k++)
        o->vp[k] = o->cp[0] * lc_S.m[0][k] + o->cp[1] * lc_S.m[1][k] + o->cp[2] * lc_S.m[2][k] + lc_S.m[3][k];
    o->oc = lcv_outcode(o->vp);
}

static void lit_cpu_group(const uint8_t *verts, const uint32_t *sub, int nw, const float (*bm)[12], const M43 *W)
{
    VLay l = vlay(nw);
    uint32_t k = 0;
    sl_cpu_groups++;
    for (int i = 1; (sub[i] >> 24) == 0x04; i++) {        /* the PRIM words of the sub-list */
        uint32_t cnt = sub[i] & 0xFFFF;
        LCV v3[3];
        LCV *a = &v3[0], *b = &v3[1], *c = &v3[2];
        for (uint32_t j = 0; j < cnt; j++) {
            lcv_load(verts + (size_t)(k + j) * l.size, &l, nw, bm, W, c);
            if (j >= 2) {
                if ((j - 2) & 1) ltri(b, a, c);              /* the GE strip winding: odd triangles flipped */
                else ltri(a, b, c);
            }
            LCV *t = a; a = b; b = c; c = t;
        }
        k += cnt;
    }
    lout_flush();
}

/* the posed box of a group (atomic space): its bind-pose box through each of its bones, merged - skinned vertices are
   convex combinations of those images, so the box holds them */
static void group_box(const LGroup *g, const float (*bm)[12], int nb, float *mn, float *mx)
{
    if (!nb) { memcpy(mn, g->bmin, 12); memcpy(mx, g->bmax, 12); return; }
    float c[3], h[3];
    for (int a = 0; a < 3; a++) { mn[a] = 1e30f; mx[a] = -1e30f; }
    for (int b = 0; b < nb; b++) {
        if (g->bbmin[b][0] > g->bbmax[b][0]) continue;    /* the slot weights no vertex */
        for (int a = 0; a < 3; a++) {
            c[a] = (g->bbmin[b][a] + g->bbmax[b][a]) * 0.5f;
            h[a] = (g->bbmax[b][a] - g->bbmin[b][a]) * 0.5f;
        }
        const float *B = bm[b];
        for (int k = 0; k < 3; k++) {
            float cc = c[0] * B[k] + c[1] * B[3 + k] + c[2] * B[6 + k] + B[9 + k];
            float r = fabsf(h[0] * B[k]) + fabsf(h[1] * B[3 + k]) + fabsf(h[2] * B[6 + k]);
            if (cc - r < mn[k]) mn[k] = cc - r;
            if (cc + r > mx[k]) mx[k] = cc + r;
        }
    }
}

/* ------------------------------------------------------------------ vehicle paint
   The game never writes a vehicle's colours into its materials: the vehicle pipeline's bridges (0x500400,
   0x5074B0 the env-map variant, 0x3AA7E0) rewrite their per-mesh copy of the material colour / texture / surface
   properties for every draw, from globals the vehicle sets before it renders (the model's current colours):
     - texture whose name starts with '#' and a paint job is set (gp+0x9E8): the paint-job texture;
     - texture == the vehicle lights texture (gp+0x9CC): colour white; the four light placeholder colours
       (255,175,0 / 0,255,200 / 185,255,0 / 255,60,0 = front left / right, rear left / right) with that light on
       (u8 gp+0x9F0[i]) -> the lights-on texture (gp+0x9D0) and surface properties 0x5FC470 (ambient 16);
     - else the paint placeholders 60,255,0 / 255,0,175 / 0,255,255 / 255,0,255 -> colour table[u8 gp+0x9E4..0x9E7]
       (RGB at 0x7B7FB0 + 4 x index, alpha kept);
     - 0x500400 / 0x5074B0 first: an atomic whose visibility plugin id (u16 at atomic + *(gp-0x44E0) + 2) has 0x1000
       draws no material with alpha < 255 (the game draws those in its later alpha pass). */
#define GP_CAR_COLOUR_IDX 0x0066C354u     /* gp + 0x9E4: u8[4] primary .. quaternary colour table index */
#define GP_CAR_PAINT_TEX 0x0066C358u   /* gp + 0x9E8: texture * (0 = none) */
#define GP_CAR_LIGHTS_TEX 0x0066C33Cu     /* gp + 0x9CC: texture * vehicle lights (off) */
#define GP_CAR_LIGHTSON_TEX 0x0066C340u   /* gp + 0x9D0: texture * vehicle lights (on) */
#define GP_CAR_LIGHT_ON 0x0066C360u       /* gp + 0x9F0: u8[4] */
#define GP_ATOMIC_ID_OFFSET 0x00667490u   /* gp - 0x44E0: the visibility plugin's atomic offset */
#define VEH_COLOUR_RGBA 0x007B7FB0u
#define SURF_LIGHT_ON 0x005FC470u
static uint64_t sl_paint, sl_painttex, sl_lights_on, sl_lights_off, sl_alpha_skip;

static void car_paint(uint32_t *tex, uint32_t *col, uint32_t *surf)
{
    uint32_t c = *col & 0xFFFFFFu, pj = gwk(GP_CAR_PAINT_TEX);
    if (pj && *tex && ram_ok(*tex, 0x54) && gb(*tex + 0x10) == '#') { *tex = pj; sl_painttex++; }
    if (*tex && *tex == gwk(GP_CAR_LIGHTS_TEX)) {
        int i = c == 0x00AFFFu ? 0 : c == 0xC8FF00u ? 1 : c == 0x00FFB9u ? 2 : c == 0x003CFFu ? 3 : -1;
        *col |= 0xFFFFFFu;
        if (i >= 0 && gb(GP_CAR_LIGHT_ON + (uint32_t)i)) {
            *tex = gwk(GP_CAR_LIGHTSON_TEX);
            *surf = SURF_LIGHT_ON;
            sl_lights_on++;
        } else sl_lights_off++;
        return;
    }
    int k = c == 0x00FF3Cu ? 0 : c == 0xAF00FFu ? 1 : c == 0xFFFF00u ? 2 : c == 0xFF00FFu ? 3 : -1;
    if (k < 0) return;
    uint32_t a = VEH_COLOUR_RGBA + 4u * gb(GP_CAR_COLOUR_IDX + (uint32_t)k);
    *col = (*col & 0xFF000000u) | (uint32_t)gb(a) | (uint32_t)gb(a + 1) << 8 | (uint32_t)gb(a + 2) << 16;
    sl_paint++;
}

/* ------------------------------------------------------------------ one atomic */
static uint64_t lit_frames_seen;
/* per atomic: the guest's bone matrices as floats (read once, not per group and pass); per mesh: each group's
   classification and scaled bone matrices from the first pass, reused by the second alpha pass */
static float palc[128][12];
#define LG_CACHE 96
static uint8_t gcls[LG_CACHE];
static float gbs[LG_CACHE][8][12];
static float (*palp)[12] = palc;          /* the palette being drawn (palc, or a deferred record's copy) */
static void pal_bones(const LGroup *g, uint32_t nbones, float (*bm)[12])
{
    for (int b = 0; b < g->nb; b++) memcpy(bm[b], palp[g->bone[b] < nbones ? g->bone[b] : 0], 48);
}
static uint64_t lt_total, lt_skin, lt_lights;   /* us: the lit path, the guest's bone matrices, the light list */
static uint64_t lt_box, lt_cpu, lt_bones, lt_draw;   /* us: group boxes + classification, CPU path, bones, draw calls */
static uint64_t lt_count, lt_total_s;                 /* the split is sampled (every 8th atomic): the timer is a syscall */
static int lt_on;
#define LT_NOW() (lt_on ? w3d_plat_us() : 0u)
/* w3d_defer=2: deferred lit / skinned atomics - what lit_draw needs, taken at hook time (camera snapshot incl.
   the lit projection, the lights, the bone palette, each mesh's paint / texture / surface after the vehicle bridge
   rules); hle_w3d.c keeps one queue for buildings and these, in the game's order */
typedef struct {
    uint32_t obj, pds, geo, mh, mc, nmesh, gflags, nbones;
    int kind, skinned, strip, cam, mesh0;
    const int8_t *map;
    M43 W, MV;
    float gm[12];
    W3dLights lights;
    float peak;
    int samp;                             /* this record's timer sample flag (lt_on at the hook) */
    W3dState st;
    float (*bones)[12];
} LRec;
typedef struct { uint32_t mat, mc4, tex, surf; uint8_t skip; } LMesh;
typedef struct { W3dCam cam; M43 S; float proj[16]; } LCam;
#define LREC_MAX 128
#define LMESH_MAX 1024
#define LBONE_MAX 2048
#define LCAM_MAX 8
static LRec lrec[LREC_MAX];
static LMesh lmesh[LMESH_MAX], lmesh_now[64];
static float lbone[LBONE_MAX][12] __attribute__((aligned(64)));   /* a record's palette on its own cache lines */
static LCam lcam[LCAM_MAX], lcam_now;
static int nlrec, nlmesh, nlbone, nlcam;
/* W3D_BB_LITPREP: per deferred lit mesh (index into lmesh) what lit_mesh's first part found (w3d_lit_prep) */
typedef struct { Ent *e; uint32_t re, serial; uint16_t g0, ng; uint8_t cull; } LPrep;
#define LPG_MAX 2048
static LPrep lprep[LMESH_MAX];
static uint8_t lpg[LPG_MAX];              /* the groups' classes (CL_CULL / CL_HW / CL_CPU) */
static int nlpg;
static uint32_t lprep_serial = 1;
static uint64_t sl_bb_prep_meshes, sl_bb_prep_used, lt_prep;
static int lit_draw(const LRec *rec, const LMesh *lm);

static int lcam_index(void)
{
    if (nlcam && !memcmp(&lcam[nlcam - 1].cam, &w3d_cam, sizeof w3d_cam) &&
        !memcmp(&lcam[nlcam - 1].S, &lc_S, sizeof lc_S) && !memcmp(lcam[nlcam - 1].proj, lc_proj, sizeof lc_proj))
        return nlcam - 1;
    if (nlcam >= LCAM_MAX) return -1;
    lcam[nlcam].cam = w3d_cam;
    lcam[nlcam].S = lc_S;
    memcpy(lcam[nlcam].proj, lc_proj, sizeof lc_proj);
    return nlcam++;
}

/* hle_w3d.c's flush: draw deferred lit record i; reset after the batch */
void w3d_lit_draw_deferred(int i)
{
    if (i < 0 || i >= nlrec) return;
    uint32_t t0 = w3d_plat_us();
    int keep = lt_on;
    lt_on = lrec[i].samp;                 /* the split timers sample the same atomics as the hook's part (not the last
                                             hooked atomic's flag) */
    lit_draw(&lrec[i], &lmesh[lrec[i].mesh0]);
    uint32_t tt = w3d_plat_us() - t0;
    lt_total += tt;
    if (lt_on) lt_total_s += tt;
    lt_on = keep;
    w3d_lit_time(tt);
}
void w3d_lit_defer_reset(void)
{
    nlrec = nlmesh = nlbone = nlcam = 0;
    if (++lprep_serial == 0) lprep_serial = 1;   /* the batch's prepared meshes are void now */
    nlpg = 0;
}

int w3d_lit_atomic(R5900Ctx *c, uint32_t pds, uint32_t obj)
{
    int kind;
    const int8_t *map;
    if (pds == PDS_PED && w3d_opt_peds) { kind = LK_PED; map = map_ped; }
    else if (pds == PDS_DRVSKIN && w3d_opt_peds) { kind = LK_DRVSKIN; map = map_drvskin; }
    else if (pds == PDS_CAR && w3d_opt_cars) { kind = LK_CAR; map = map_car; }
    else if (pds == PDS_DRVLIT && w3d_opt_lit) { kind = LK_DRVLIT; map = map_drvlit; }
    else return 0;
    if (w3d_opt_defer < 2) w3d_defer_flush();   /* the deferred buildings first (draw order as the game's) */
    w3d_hwp_begin(3);
    uint32_t t0 = w3d_plat_us();
    lt_on = (lt_count++ & 7) == 0;
    int r = 0;
    uint32_t geo = gwk(obj + 0x18);
    do {
        if (!geo || !ram_ok(geo, 0x60)) { sl_why[0]++; break; }
        uint32_t gflags = gw(geo + 8), mh = gw(geo + 0x54);
        if (!(gflags & 0x01000000u) || !ram_ok(mh, 0x10)) { sl_why[1]++; break; }
        uint32_t nmesh = gh(mh + 4), hflags = gw(mh);
        uint32_t mcp = gw(geo + 0x18) == 1 ? geo + gwk(GP_MESHCACHE_GEO) : obj + gwk(GP_MESHCACHE_ATOMIC), mc = gwk(mcp);
        if (!nmesh || !mc || gw(mc) != nmesh || !ram_ok(mc, 4 + 4 * nmesh)) { sl_why[2]++; break; }
        uint32_t camera = gwk(DRV_ENGINE);
        if (!camera || !w3d_camera_setup(camera)) { sl_why[3]++; break; }
        if (!lit_camera(c, camera)) { sl_why[3]++; break; }
        /* skinned: the guest's bone matrices (atomic space) */
        int skinned = kind == LK_PED || kind == LK_DRVSKIN;
        uint32_t pal = 0, nbones = 0;
        if (skinned) {
            uint32_t skin = gwk(geo + gwk(GP_SKIN_GLOBALS + 8)), hier = gwk(obj + gwk(GP_SKIN_GLOBALS + 4));
            if (!skin || !hier || !ram_ok(skin, 0x20)) { sl_why[4]++; break; }
            nbones = gw(skin);
            if (!nbones || nbones > 128) { sl_why[4]++; break; }
            uint32_t ts = LT_NOW();
            pal = hle_call(c, FN_SKIN_MATRICES, 2, obj, skin);
            lt_skin += LT_NOW() - ts;
            if (!ram_ok(pal, nbones * 64u)) { sl_why[4]++; break; }
        }
        if (!w3d_be_target()) {
            sl_why[5]++;
            static int dg = 16;
            static uint32_t dg_flip;
            if (dg > 0 && hle_render_flip_count() != dg_flip) {   /* diagnostic: who renders lit atomics off the screen */
                uint32_t fb = hle_render_cur_fb(), scr = hle_render_screen_raster();
                dg--;
                dg_flip = (uint32_t)hle_render_flip_count();
                LOG("world3d lit: atomic %08x pds %08x not drawn - camera %08x renders to raster %08x (%ux%u), the "
                    "screen is %08x (%ux%u), flip %u\n", obj, pds, camera, fb, fb ? gw(fb + 0x0C) : 0,
                    fb ? gw(fb + 0x10) : 0, scr, scr ? gw(scr + 0x0C) : 0, scr ? gw(scr + 0x10) : 0, dg_flip);
            }
            break;
        }
        uint32_t frame = gwk(obj + 4), ltm = frame ? w3d_frame_ltm(c, frame) : 0;
        if (!ram_ok(ltm, 0x40)) { sl_why[6]++; break; }
        M43 Lt, W;
        w3d_drv_mat(ltm, &Lt);
        w3d_mat_mul(&Lt, &lc_C, &W);      /* object -> camera space (rigid) */
        float gm[12];
        for (int rr = 0; rr < 4; rr++)
            for (int kk = 0; kk < 3; kk++) gm[3 * rr + kk] = W.m[rr][kk];
        /* visibility: the converted meshes' boxes are checked per mesh below (in the driver's view space) */
        M43 V, MV;
        w3d_drv_mat(w3d_cam.camera + 0x20, &V);
        w3d_mat_mul(&Lt, &V, &MV);
        W3dLights lights;
        uint32_t tl = LT_NOW();
        lit_lights(c, &lights);
        lt_lights += LT_NOW() - tl;
#ifndef HLE_PSP
        {   /* diagnostic: every light linked with the direct light in the world's light list (type, flags, colour) */
            static int wdiag = 12;
            uint32_t dl = gwk(GP_DIRECT_LIGHT);
            if (skinned && wdiag > 0 && dl && w3d_frame_no() >= 1500 && (w3d_frame_no() % 31) == 0) {
                wdiag--;
                LOG("world3d lit light list (frame %u, kind %d, atomic %08x): amb %.3f dir %.3f\n", w3d_frame_no(), kind,
                    obj, lights.amb[0], lights.col[0][0]);
                uint32_t n = gwk(dl + 0x34);
                for (int q = 0; q < 16 && n && n != dl + 0x34; q++) {
                    uint32_t L = n - 0x34;
                    if (ram_ok(L, 0x40))
                        LOG("  node %08x: obj type %u sub %u flags %02x colour %.3f %.3f %.3f radius %.2f\n", n, gb(L),
                            gb(L + 1), gb(L + 2), gfk(L + 0x18), gfk(L + 0x1C), gfk(L + 0x20), gfk(L + 0x14));
                    n = gwk(n);
                }
            }
        }
#endif
        uint32_t idoff = kind == LK_CAR && w3d_opt_carpaint ? gwk(GP_ATOMIC_ID_OFFSET) : 0xFFFFFFFFu;
        int no_alpha = idoff < 0x10000u && (gh(obj + idoff + 2) & 0x1000) != 0;
        /* everything the drawing needs that can change before the frame ends - deferred (w3d_defer=2) or
           drawn at once (lit_draw) */
        int defer = w3d_opt_defer >= 2 && !w3d_defer_flushing;
        if (defer && (nlrec >= LREC_MAX || nlmesh + (int)nmesh > LMESH_MAX || nlbone + (int)nbones > LBONE_MAX ||
                      nlcam >= LCAM_MAX || !w3d_defer_room())) w3d_defer_flush();
        if (defer && (nmesh > LMESH_MAX || nbones > LBONE_MAX)) defer = 0;
        LRec rnow, *rec = defer ? &lrec[nlrec] : &rnow;
        LMesh *lm = defer ? &lmesh[nlmesh] : lmesh_now;
        if (!defer && nmesh > 64) { sl_why[2]++; break; }
        float (*bones)[12] = defer ? &lbone[nlbone] : palc;
        for (uint32_t b = 0; b < nbones; b++)              /* driver matrix: right, up, at, pos (16 bytes each) */
            for (uint32_t rr = 0; rr < 4; rr++) memcpy(&bones[b][3 * rr], hle_ram + pal + 64u * b + 16u * rr, 12);
        rec->obj = obj; rec->pds = pds; rec->geo = geo; rec->mh = mh; rec->mc = mc; rec->nmesh = nmesh;
        rec->gflags = gflags; rec->nbones = nbones; rec->kind = kind; rec->map = map; rec->skinned = skinned;
        rec->strip = (hflags & 1) != 0;
        rec->W = W; rec->MV = MV;
        memcpy(rec->gm, gm, sizeof gm);
        rec->lights = lights;
        rec->peak = lit_peak;
        rec->samp = lt_on;
        w3d_base_state(&rec->st);
        int ci = -1;                      /* (immediate: the record's own camera slot lcam_now) */
        if (defer) {
            ci = lcam_index();
            if (ci < 0) { w3d_defer_flush(); w3d_lit_defer_reset(); ci = lcam_index(); }
            if (ci < 0) defer = 0;
        }
        if (!defer) {
            lcam_now.cam = w3d_cam;
            lcam_now.S = lc_S;
            memcpy(lcam_now.proj, lc_proj, sizeof lc_proj);
        }
        rec->cam = ci;
        sl_atomics[kind]++;
        for (uint32_t i = 0; i < nmesh; i++) {
            uint32_t mat = gwk(mh + 0x10 + gw(mh + 0x0C) + 12u * i + 8);
            uint32_t mc4 = mat ? gwk(mat + 4) : 0, tex = mat ? gwk(mat) : 0, surf = mat + 0x0C;
            lm[i].skip = 0;
            if (mat && kind == LK_CAR && w3d_opt_carpaint) {
                if (no_alpha && (mc4 >> 24) < 255) { sl_alpha_skip++; lm[i].skip = 1; }
                else car_paint(&tex, &mc4, &surf);
            }
            lm[i].mat = mat; lm[i].mc4 = mc4; lm[i].tex = tex; lm[i].surf = surf;
        }
        if (defer) {
            rec->bones = bones;
            rec->mesh0 = nlmesh;
            nlrec++;
            nlmesh += (int)nmesh;
            nlbone += (int)nbones;
            if (w3d_opt_me && w3d_opt_me_lit) nlbone = (nlbone + 3) & ~3;   /* the next palette 64-byte aligned
                                                                               (the ME reads it by whole cache lines) */
            w3d_defer_push_lit(nlrec - 1);
            r = 1;
            break;
        }
        rec->bones = bones;               /* (palc, or the pool slot when the queue was full) */
        w3d_defer_flush();                /* (not deferred: whatever is queued goes first) */
        r = lit_draw(rec, lm);
    } while (0);
    uint32_t tt = w3d_plat_us() - t0;
    w3d_hwp_end(3, 1, 0);
    lt_total += tt;
    if (lt_on) lt_total_s += tt;
    w3d_lit_time(tt);
    (void)lit_frames_seen;
    return r;
}

/* the lookup of a lit / skinned mesh (lit_mesh, lit_prep_mesh): validated / converted entry, NULL = nothing
   to draw; w3d_vframe: an entry validated this frame is trusted without reading the game's memory again */
static uint64_t sl_vframe;
static Ent *lit_lookup(const LRec *rec, uint32_t i, uint32_t mat)
{
    const uint32_t geo = rec->geo, mh = rec->mh, mc = rec->mc;
    const int kind = rec->kind, skinned = rec->skinned, strip = rec->strip;
    uint32_t mesh = mh + 0x10 + gw(mh + 0x0C) + 12u * i, re = gwk(mc + 4 + 4 * i);
    Ent *e = w3d_opt_vframe && re ? w3d_ent_find(re) : NULL;
    if (e && e->vframe == w3d_frame_no() && e->geo == geo && e->kind == (skinned ? EK_SKIN : EK_LIT) &&
        e->dn == (uint8_t)kind && mat) {
        sl_vframe++;
        e->last_use = w3d_frame_no();
        return e;
    }
    uint32_t data = re ? gwk(re + 0x20) : 0;
    if (re && !w3d_re_owned(re, mc + 4 + 4 * i)) data = 0;   /* not this mesh's entry */
    if (!mat || !data || !ram_ok(data, 64)) { sl_fail++; return NULL; }
    uint32_t sig = w3d_chain_sig(data), rsz = gwk(re + 0x08), nidx = gwk(mesh + 4);
    e = w3d_ent_find(re);
    if (e && (e->data != data || e->geo != geo || e->sig != sig || e->rsz != rsz || e->nidx != nidx ||
              e->kind != (skinned ? EK_SKIN : EK_LIT) || e->dn != (uint8_t)kind)) {
        if (w3d_ent_busy(e)) { e->re = 0; w3d_ent_unlink(e); }   /* also when held (w3d_me) */
        else w3d_ent_free(e);
        e = NULL;
    }
    if (!e) {
        e = w3d_ent_new(re);
        if (!e) { sl_fail++; return NULL; }
        e->data = data; e->geo = geo; e->sig = sig; e->rsz = rsz; e->nidx = nidx;
        e->kind = (uint8_t)(skinned ? EK_SKIN : EK_LIT);
        e->dn = (uint8_t)kind;
        if (!lit_convert(e, nidx, strip, kind, rec->map)) { w3d_ent_free(e); sl_fail++; return NULL; }
    }
    e->vframe = w3d_frame_no();
    e->last_use = w3d_frame_no();
    return e;
}

/* W3D_BB_LITPREP: per deferred lit mesh (index into lmesh) what lit_mesh's first part found, made by
   w3d_lit_prep while the main core waits for the ME (hle_w3d.c me_flush); valid for the batch it was made in */
static const LPrep *lprep_of(const LMesh *lm, uint32_t i)
{
    if (lm < lmesh || lm >= lmesh + LMESH_MAX) return NULL;        /* an immediate record (lmesh_now) */
    uint32_t k = (uint32_t)(lm - lmesh) + i;
    return k < LMESH_MAX && lprep[k].serial == lprep_serial ? &lprep[k] : NULL;
}

void w3d_lit_prep(int lidx)
{
    if (lidx < 0 || lidx >= nlrec) return;
    const LRec *rec = &lrec[lidx];
    const LMesh *lm = &lmesh[rec->mesh0];
    uint32_t t0 = w3d_plat_us();
    int keep = lt_on;
    lt_on = rec->samp;
    float (*keep_pal)[12] = palp;
    palp = rec->bones;                    /* (pal_bones reads the record's palette, as lit_begin sets it) */
    for (uint32_t i = 0; i < rec->nmesh && rec->mesh0 + (int)i < LMESH_MAX; i++) {
        LPrep *p = &lprep[rec->mesh0 + (int)i];
        p->serial = 0;
        if (lm[i].skip) continue;
        Ent *e = lit_lookup(rec, i, lm[i].mat);
        p->e = e;
        p->re = e ? e->re : 0;
        p->cull = 0;
        p->ng = 0xFFFFu;
        if (e) {
            e->hold = w3d_me_hold;                    /* not evicted before the replay draws it (the batch's end) */
            if (!rec->skinned && (w3d_classify_box(e->bmin, e->bmax, &rec->MV) & 3) == CL_CULL) p->cull = 1;
            else if (nlpg + (int)e->nx <= LPG_MAX) {
                const LGroup *gt = (const LGroup *)(w3d_arena_base() + e->off + e->xoff);
                p->g0 = (uint16_t)nlpg;
                for (int g = 0; g < e->nx; g++) {
                    float bm[8][12], gmn[3], gmx[3];
                    pal_bones(&gt[g], rec->nbones, bm);
                    group_box(&gt[g], (const float (*)[12])bm, gt[g].nb, gmn, gmx);
                    lpg[nlpg + g] = (uint8_t)(w3d_classify_box(gmn, gmx, &rec->MV) & 3);
                }
                nlpg += e->nx;
                p->ng = e->nx;
            }
        }
        p->serial = lprep_serial;
        sl_bb_prep_meshes++;
    }
    palp = keep_pal;
    uint32_t tt = w3d_plat_us() - t0;
    if (lt_on) lt_prep += tt;
    lt_on = keep;
    lt_total += tt;
    if (rec->samp) lt_total_s += tt;
    w3d_lit_time(tt);
}

/* lit_draw split - lit_begin (camera, palette, projection, lights), lit_mesh (one mesh), lit_end */
static int lit_begin(const LRec *rec, M43 *keep_S)
{
    *keep_S = lc_S;                       /* lit_camera's cache stays consistent with its key */
    const LCam *lcm = rec->cam >= 0 ? &lcam[rec->cam] : &lcam_now;
    w3d_cam = lcm->cam;
    lc_S = lcm->S;
    if (!w3d_be_begin(&w3d_cam)) { sl_why[5]++; lc_S = *keep_S; return 0; }
    palp = rec->bones;
    lit_peak = rec->peak;
    w3d_be_proj(lcm->proj);
    w3d_be_lights(&rec->lights);
    w3d_world_reset(rec->gm);
    return 1;
}

static void lit_end(const M43 *keep_S)
{
    w3d_be_proj(NULL);
    w3d_be_end();
    palp = palc;
    lc_S = *keep_S;
}

static void lit_mesh(const LRec *rec, const LMesh *lm, uint32_t i, W3dState *stp)
{
    const uint32_t obj = rec->obj, pds = rec->pds, gflags = rec->gflags, nbones = rec->nbones;
    const int kind = rec->kind, skinned = rec->skinned;
    const M43 *Wp = &rec->W, *MVp = &rec->MV;
    const W3dLights *lights = &rec->lights;
    (void)obj; (void)pds; (void)kind;
#define st (*stp)
    uint32_t mat = lm[i].mat;
    sl_meshes++;
    if (lm[i].skip) return;
    /* W3D_BB_LITPREP: the lookup, the rigid cull and the groups' classes done while waiting for the ME
       (w3d_lit_prep, same inputs, nothing in between changes them); an entry that is gone falls back to the lookup */
    const LPrep *pp = lprep_of(lm, i);
    const uint8_t *pg = NULL;
    Ent *e;
    if (pp) {
        e = pp->e;
        if (!e) return;                                   /* (the prepared lookup found nothing) */
        if (!e->used || e->re != pp->re) pp = NULL;       /* gone since: the plain lookup path */
        else {
            sl_bb_prep_used++;
            if (pp->cull) { sl_cull++; return; }
            if (pp->ng != 0xFFFFu) pg = &lpg[pp->g0];
        }
    }
    if (!pp) {
        e = lit_lookup(rec, i, mat);
        if (!e) return;
        /* bind-pose box: skinned meshes move, so only rigid meshes are culled here */
        if (!skinned && (w3d_classify_box(e->bmin, e->bmax, MVp) & 3) == CL_CULL) { sl_cull++; return; }
    }
    uint32_t ras;
    int acl, wrap;
    uint32_t mc4 = lm[i].mc4, tex = lm[i].tex, surf = lm[i].surf;
    int bound = w3d_bind_texture(tex, &ras, &acl, &wrap);
#ifndef HLE_PSP
    if (kind == LK_CAR && surf == 0x005FC470u) {   /* diagnostic: lights-on draws */
        static int dg = 10;
        if (dg > 0) {
            dg--;
            LOG("world3d lit: lights-on mesh atomic %08x tex %08x '%.16s' raster %08x bound %d colour %08x "
                "material alpha %u, ambient light %.3f %.3f %.3f\n", obj, tex, tex ? (const char *)hle_ram +
                ((tex + 0x10) & (HLE_RAM_SIZE - 1)) : "", ras, bound, mc4, mc4 >> 24, lights->amb[0],
                lights->amb[1], lights->amb[2]);
        }
    }
#endif
    if (!bound) return;
    float sa = gfk(surf), sd = gfk(surf + 8);
    if (!(sa >= 0.0f && sa <= 64.0f)) sa = 1.0f;     /* (16: the lights-on surface properties) */
    if (!(sd >= 0.0f && sd <= 2.0f)) sd = 1.0f;
    st.textured = ras != 0;
    st.uscale = 8.0f;
    st.ubase = !(wrap & 1) || w3d_opt_tilebase ? e->ubase : 0.0f;   /* UV base rule (hle_w3d.c) */
    st.vbase = !(wrap & 2) || w3d_opt_tilebase ? e->vbase : 0.0f;
    st.light = 0;
    st.lights = 1 + lights->n;
    st.lamb = mc4;                /* material colour (RGBA bytes) */
    /* the VU clamps the light sum to 1 and then multiplies by the material colour, the GE multiplies first:
       the same for white materials; a coloured one (car paint) under lights that add up past 1 would wash
       out to white on the GE, so its lights are scaled to peak at 1 there */
    if ((mc4 & 0xFFFFFFu) != 0xFFFFFFu && lit_peak > 1.0f) { sa /= lit_peak; sd /= lit_peak; }
    st.lemit = 0;
    if (sa > 1.0f) {                 /* VU: min(ambient x light + ..., 1) x material: the part above 1 as
                                        emissive (the lights-on material: ambient 16) */
        for (int q = 0; q < 3; q++) {
            float e = sa * lights->amb[q];
            e = (e > 1.0f ? 1.0f : e) * (float)(mc4 >> (8 * q) & 0xFF);
            st.lemit |= (uint32_t)(e + 0.5f) << (8 * q);
        }
        sa = 0.0f;
    }
    if (sd > 1.0f) sd = 1.0f;
    st.lemis = (uint32_t)(sa * 255.0f + 0.5f) | (uint32_t)(sd * 255.0f + 0.5f) << 8;   /* surface props */
    int valpha = (gflags & 0x40) && (mc4 >> 24) < 255;
    int passes = w3d_alpha_passes(&st, acl, valpha || (mc4 >> 24) < 255);
    int zw = st.zwrite;
    const LGroup *gt = (const LGroup *)(w3d_arena_base() + e->off + e->xoff);
    /* the world matrix of this mesh: rigid meshes carry the s16 position scale in it (skinned ones in the bones) */
    float gmm[12];
    if (!skinned) {
        float k = 32768.0f * e->pscale;
        for (int rr = 0; rr < 3; rr++)
            for (int kk = 0; kk < 3; kk++) gmm[3 * rr + kk] = Wp->m[rr][kk] * k;
        for (int kk = 0; kk < 3; kk++) gmm[9 + kk] = Wp->m[3][kk];
        w3d_world_reset(gmm);
    }
    for (int pass = 0; pass < passes; pass++) {
        if (pass == 1) { st.atest = 3; st.zwrite = 0; }
        w3d_be_state(&st);
        w3d_world_set(1);
        for (int g = 0; g < e->nx; g++) {
            uint32_t tg0 = LT_NOW();
            float bm[8][12], bsl[8][12];
            int cached = pass > 0 && g < LG_CACHE, gc;
            float (*bs)[12] = g < LG_CACHE ? gbs[g] : bsl;
            if (cached) gc = gcls[g];
            else {
                pal_bones(&gt[g], nbones, bm);
                /* where does the group go: culled / the GE / the CPU path (it leaves the guard band) */
                if (pg) gc = pg[g];                       /* classified while waiting for the ME */
                else {
                    float gmn[3], gmx[3];
                    group_box(&gt[g], (const float (*)[12])bm, gt[g].nb, gmn, gmx);
                    gc = w3d_classify_box(gmn, gmx, MVp) & 3;
                }
                if (g < LG_CACHE) gcls[g] = (uint8_t)gc;
                if (gc == CL_HW && gt[g].nb) {   /* the position scale on the bones' 3x3 (translation in units) */
                    float k = 32768.0f * e->pscale;
                    for (int b = 0; b < gt[g].nb; b++) {
                        for (int q = 0; q < 9; q++) bs[b][q] = bm[b][q] * k;
                        for (int q = 9; q < 12; q++) bs[b][q] = bm[b][q];
                    }
                }
            }
            uint32_t tg1 = LT_NOW();
            lt_box += tg1 - tg0;
            if (gc == CL_CULL) { sl_gcull++; continue; }
            if (gc == CL_CPU) {
                int nw = gt[g].nb ? gt[g].nb : (e->stride ? 1 : 0);
                if (cached) pal_bones(&gt[g], nbones, bm);
                lcv_inv = e->pscale;
                lit_cpu_group(w3d_arena_base() + e->off + gt[g].voff,
                              (const uint32_t *)(w3d_arena_base() + e->off + gt[g].suboff), nw,
                              (const float (*)[12])bm, Wp);
                w3d_world_set(1);
                lt_cpu += LT_NOW() - tg1;
                continue;
            }
            if (gt[g].nb) {
                w3d_be_bones(gt[g].nb, (const float (*)[12])bs);
                sl_bones_up += gt[g].nb;
            }
            uint32_t tg2 = LT_NOW();
            lt_bones += tg2 - tg1;
            W3dMeshView mv;
            memset(&mv, 0, sizeof mv);
            mv.verts = w3d_arena_base() + e->off + gt[g].voff;
            mv.sub = (const uint32_t *)(w3d_arena_base() + e->off + gt[g].suboff);
            mv.nverts = gt[g].nverts;
            mv.nprims = gt[g].nprims;
            mv.nsub = gt[g].nprims + 3;
            mv.ntris = gt[g].ntris;
            mv.fmt = W3D_FMT_LIT;
            mv.stride = (uint8_t)vlay(gt[g].nb ? gt[g].nb : (e->stride ? 1 : 0)).size;
            mv.ubase = e->ubase;
            mv.vbase = e->vbase;
            mv.vtype = mv.sub[0] & 0xFFFFFFu;
            w3d_be_mesh(&mv, NULL);
            lt_draw += LT_NOW() - tg2;
            sl_groups++;
            sl_tris += gt[g].ntris;
        }
    }
    st.zwrite = zw;
    e->pin = w3d_be_epoch();
#undef st
}

/* the meshes of one lit / skinned atomic record (now, or from the deferred queue) */
static int lit_draw(const LRec *rec, const LMesh *lm)
{
    M43 keep_S;
    if (!lit_begin(rec, &keep_S)) return 0;
    W3dState st = rec->st;
    for (uint32_t i = 0; i < rec->nmesh; i++) lit_mesh(rec, lm, i, &st);
    lit_end(&keep_S);
    return 1;
}

/* ------------------------------------------------------------------ lit / skinned records on the Media Engine
   (hle_w3d_me.h, w3d_me=1 w3d_me_lit=1): at hook time the main core writes the record (camera with the lit projection,
   lights, matrices, the bone palette's address) and per mesh what lit_mesh's first part finds (lookup / conversion,
   the rigid cull, the texture lookup without binding, the surface properties); the ME classifies the bone groups,
   decides the state and writes projection, lights, world / bone matrices and the groups' CALLs. A mesh with a group in
   the CPU path is drawn by the main core (w3d_lit_draw_one) in its place. */
static inline uint32_t lfw(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

uint32_t w3d_lit_nmesh(int lidx) { return lidx >= 0 && lidx < nlrec ? lrec[lidx].nmesh : 0; }
const W3dCam *w3d_lit_cam(int c) { return &lcam[c >= 0 && c < LCAM_MAX ? c : 0].cam; }
const float (*w3d_lit_bone_base(void))[12] { return (const float (*)[12])lbone; }

void w3d_lit_group_view(const Ent *e, int g, W3dMeshView *mv)
{
    const LGroup *gt = (const LGroup *)(w3d_arena_base() + e->off + e->xoff);
    memset(mv, 0, sizeof *mv);
    mv->verts = w3d_arena_base() + e->off + gt[g].voff;
    mv->sub = (const uint32_t *)(w3d_arena_base() + e->off + gt[g].suboff);
    mv->nverts = gt[g].nverts;
    mv->nprims = gt[g].nprims;
    mv->nsub = gt[g].nprims + 3;
    mv->ntris = gt[g].ntris;
    mv->fmt = W3D_FMT_LIT;
    mv->stride = (uint8_t)vlay(gt[g].nb ? gt[g].nb : (e->stride ? 1 : 0)).size;
    mv->ubase = e->ubase;
    mv->vbase = e->vbase;
    mv->vtype = mv->sub[0] & 0xFFFFFFu;
}

/* a MAINMESH part of a lit record: that one mesh drawn on the main core (its record's camera, palette, lights first) */
int w3d_lit_draw_one(int lidx, uint32_t i)
{
    if (lidx < 0 || lidx >= nlrec || i >= lrec[lidx].nmesh) return 0;
    const LRec *rec = &lrec[lidx];
    uint32_t t0 = w3d_plat_us();
    int keep = lt_on, r = 0;
    lt_on = rec->samp;
    M43 keep_S;
    if (lit_begin(rec, &keep_S)) {
        W3dState st = rec->st;
        lit_mesh(rec, &lmesh[rec->mesh0], i, &st);
        lit_end(&keep_S);
        r = 1;
    }
    lt_on = keep;
    uint32_t tt = w3d_plat_us() - t0;
    lt_total += tt;
    w3d_lit_time(tt);
    return r;
}

/* lit_mesh's part before the drawing, for the ME: 0 = nothing to draw, 1 = the ME draws it (w[1..19]), 2 = the main
   core draws it (more bone groups than the ME takes) */
static uint64_t sl_me_meshes, sl_me_main;
static int lit_prep_mesh(const LRec *rec, const LMesh *lm, uint32_t i, uint32_t *w, Ent **pe)
{
    uint32_t mat = lm[i].mat;
    sl_meshes++;
    if (lm[i].skip) return 0;
    Ent *e = lit_lookup(rec, i, mat);
    if (!e) return 0;
    if (!rec->skinned && (w3d_classify_box(e->bmin, e->bmax, &rec->MV) & 3) == CL_CULL) { sl_cull++; return 0; }
    W3mTex td;
    if (!w3d_me_tex(lm[i].tex, &td)) return 0;
    uint32_t surf = lm[i].surf;
    float sa = gfk(surf), sd = gfk(surf + 8);
    if (e->nx > W3M_MAXGROUPS) return 2;
#ifdef HLE_PSP
    w[1] = (uint32_t)(uintptr_t)(w3d_arena_base() + e->off);
    w[2] = (uint32_t)(uintptr_t)(w3d_arena_base() + e->off + e->xoff);
#else
    w[1] = e->off;                                   /* host: offsets (64-bit pointers) */
    w[2] = e->off + e->xoff;
#endif
    w[3] = e->nx;
    w[4] = lfw(e->pscale);
    w[5] = lfw(e->ubase);
    w[6] = lfw(e->vbase);
    w[7] = lm[i].mc4;
    w[8] = lfw(sa);
    w[9] = lfw(sd);
    w[10] = 0;
    memcpy(&w[11], &td, sizeof td);
    *pe = e;
    return 1;
}

void w3d_lit_me_push(int lidx, uint32_t ri, uint32_t *camsent)
{
    uint32_t w[W3M_ITEM_MAX];
    if (lidx < 0 || lidx >= nlrec || lrec[lidx].cam < 0 || lrec[lidx].cam >= LCAM_MAX) {
        w[0] = ri;
        w3dme_put(W3M_I_MREC, w, 1);
        return;
    }
    const LRec *rec = &lrec[lidx];
    uint32_t cidx = 8u + (uint32_t)rec->cam;
    if (!(*camsent >> cidx & 1u)) {
        const LCam *lc = &lcam[rec->cam];
        w[0] = cidx;
        for (int q = 0; q < 16; q++) w[1 + q] = lfw(lc->cam.proj[q]);
        w[17] = lfw(lc->cam.near_z); w[18] = lfw(lc->cam.far_z); w[19] = lfw(lc->cam.fog_z);
        for (int q = 0; q < 16; q++) w[20 + q] = lfw(lc->proj[q]);
        w3dme_put(W3M_I_CAM, w, W3M_LCAM_WORDS);
        *camsent |= 1u << cidx;
    }
#ifdef HLE_PSP
    if (rec->nbones) sceKernelDcacheWritebackRange(rec->bones, (rec->nbones * 48u + 63u) & ~63u);   /* the ME reads it */
#endif
    const W3dState *b = &rec->st;
    w[0] = ri;
    w[1] = cidx;
    w[2] = (rec->skinned ? 1u : 0u) | ((rec->gflags & 0x40) ? 2u : 0u);
    w[3] = (uint32_t)(b->ztest & 1) | (uint32_t)(b->zwrite & 1) << 1 | (uint32_t)(b->cull & 3) << 2 |
           (uint32_t)(b->fog & 1) << 4 | (uint32_t)(b->src_blend & 0xF) << 8 | (uint32_t)(b->dst_blend & 0xF) << 12;
    w[4] = b->fog_color;
    w[5] = lfw(rec->peak);
    w[6] = rec->nbones;
#ifdef HLE_PSP
    w[7] = (uint32_t)(uintptr_t)rec->bones;
#else
    w[7] = (uint32_t)(rec->bones - lbone);           /* host: an index into the pool */
#endif
    for (int q = 0; q < 12; q++) {
        w[8 + q] = lfw(rec->MV.m[q / 3][q % 3]);
        w[20 + q] = lfw(rec->W.m[q / 3][q % 3]);
        w[32 + q] = lfw(rec->gm[q]);
    }
    for (int k = 0; k < 3; k++) w[44 + k] = lfw(rec->lights.amb[k]);
    w[47] = (uint32_t)rec->lights.n;
    for (int q = 0; q < 4; q++)
        for (int k = 0; k < 3; k++) { w[48 + 3 * q + k] = lfw(rec->lights.dir[q][k]); w[60 + 3 * q + k] = lfw(rec->lights.col[q][k]); }
    w3dme_put(W3M_I_LREC, w, W3M_LREC_WORDS);
    const LMesh *lm = &lmesh[rec->mesh0];
    for (uint32_t i = 0; i < rec->nmesh; i++) {
        Ent *e = NULL;
        int k = lit_prep_mesh(rec, lm, i, w, &e);
        int kk = k == 1 ? w3d_me_ent_add(e) : -1;
        if (k == 1 && kk >= 0) {
            w[0] = (uint32_t)kk | i << 16;
            w3dme_put(W3M_I_LMESH, w, W3M_LMESH_WORDS);
            sl_me_meshes++;
        } else if (k) {
            w[0] = ri;
            w[1] = i;
            w3dme_put(W3M_I_MAINMESH, w, 2);
            sl_me_main++;
        }
    }
}

void w3d_lit_init(void)
{
    if (w3d_opt_bones < 1) w3d_opt_bones = 1;
    if (w3d_opt_bones > 8) w3d_opt_bones = 8;
    LOG("world3d lit / skinned pipelines: peds=%d (53f20088, 00011002) cars=%d (53f20084) lit=%d (00005001) bones=%d "
        "lights=%d\n", w3d_opt_peds, w3d_opt_cars, w3d_opt_lit, w3d_opt_bones, w3d_opt_lights);
}

void w3d_lit_report(void)
{
    LOG("world3d lights: w3d_ldirfix=%d, %llu cached light directions used, %llu of them re-aimed since (stale)\n",
        ldir_fix, (unsigned long long)sl_ldir_hits, (unsigned long long)sl_ldir_stale);
    if (sl_bb_prep_meshes)
        LOG("world3d lit (w3d_bb litprep): %llu meshes prepared while waiting for the ME, %llu of them used "
            "by the replay; %.0f%% of the sampled lit time\n", (unsigned long long)sl_bb_prep_meshes,
            (unsigned long long)sl_bb_prep_used, lt_total_s ? 100.0 * (double)lt_prep / (double)lt_total_s : 0.0);
    if (w3d_opt_vframe) LOG("world3d lit vframe: %llu lookups trusted (validated earlier this frame)\n", (unsigned long long)sl_vframe);
    LOG("world3d lit: atomics drawn %llu %s, %llu %s, %llu %s, %llu %s; meshes %llu (%llu failed, %llu culled), %llu GE "
        "groups, %llu triangles, %llu bone matrices sent; conversions %llu (%llu KB, %llu failed, %llu skinned meshes "
        "without skin data); passed to the guest: %llu %llu %llu %llu %llu %llu %llu; groups culled %llu, CPU path: %llu "
        "groups, %llu triangles (%llu clipped); lights: %.2f directional per atomic, %llu merges (w3d_lights %d); time: "
        "%.0f%% the guest's bone matrices, %.0f%% lights, %.0f%% meshes / groups (boxes %.0f%%, CPU path %.0f%%, bones "
        "%.0f%%, draw %.0f%%); vehicle bridges (w3d_carpaint %d): %llu paint colours, %llu paint-job textures, %llu "
        "lights on, %llu lights off, %llu alpha materials skipped (atomic id 0x1000)\n",
        (unsigned long long)sl_atomics[0], lk_name[0], (unsigned long long)sl_atomics[1], lk_name[1],
        (unsigned long long)sl_atomics[2], lk_name[2], (unsigned long long)sl_atomics[3], lk_name[3],
        (unsigned long long)sl_meshes, (unsigned long long)sl_fail, (unsigned long long)sl_cull,
        (unsigned long long)sl_groups, (unsigned long long)sl_tris, (unsigned long long)sl_bones_up,
        (unsigned long long)sl_conv, (unsigned long long)(sl_conv_bytes >> 10), (unsigned long long)sl_conv_fail,
        (unsigned long long)sl_noskin, (unsigned long long)sl_why[0], (unsigned long long)sl_why[1],
        (unsigned long long)sl_why[2], (unsigned long long)sl_why[3], (unsigned long long)sl_why[4],
        (unsigned long long)sl_why[5], (unsigned long long)sl_why[6], (unsigned long long)sl_gcull,
        (unsigned long long)sl_cpu_groups, (unsigned long long)sl_cpu_tris, (unsigned long long)sl_cpu_clipped,
        sl_light_atomics ? (double)sl_light_dirs / (double)sl_light_atomics : 0.0, (unsigned long long)sl_light_merged,
        w3d_opt_lights, lt_total_s ? 100.0 * (double)lt_skin / (double)lt_total_s : 0.0,
        lt_total_s ? 100.0 * (double)lt_lights / (double)lt_total_s : 0.0,
        lt_total_s ? 100.0 * (double)(lt_total_s - lt_skin - lt_lights) / (double)lt_total_s : 0.0,
        lt_total_s ? 100.0 * (double)lt_box / (double)lt_total_s : 0.0, lt_total_s ? 100.0 * (double)lt_cpu / (double)lt_total_s : 0.0,
        lt_total_s ? 100.0 * (double)lt_bones / (double)lt_total_s : 0.0,
        lt_total_s ? 100.0 * (double)lt_draw / (double)lt_total_s : 0.0, w3d_opt_carpaint,
        (unsigned long long)sl_paint, (unsigned long long)sl_painttex, (unsigned long long)sl_lights_on,
        (unsigned long long)sl_lights_off, (unsigned long long)sl_alpha_skip);
}

#ifndef HLE_PSP
/* host exploration dump (HLE_W3D_DUMP): for skinned atomics also the skin data, the bone hierarchy and the bone matrices
   the PS2 pipeline would upload (computed by the guest's own function); for every lit atomic the game's two main
   lights */
void w3d_lit_dump_extra(R5900Ctx *c, uint32_t pds, uint32_t obj, void (*dmem)(uint32_t addr, uint32_t n))
{
    if (pds != PDS_PED && pds != PDS_DRVSKIN && pds != PDS_CAR && pds != PDS_DRVLIT) return;
    uint32_t geo = w3d_gwk(obj + 0x18);
    if (!geo) return;
    uint32_t amb = gwk(GP_AMBIENT_LIGHT), dir = gwk(GP_DIRECT_LIGHT);
    dmem(GP_AMBIENT_LIGHT, 8);
    if (amb) dmem(amb, 0x40);
    if (dir) {
        dmem(dir, 0x40);
        uint32_t fr = gwk(dir + 4);
        if (fr) dmem(fr, 0xA0);
    }
    if (geo) { uint32_t mt = gwk(geo + 0x5C); if (mt) dmem(mt, 0x20); }
    if (pds != PDS_PED && pds != PDS_DRVSKIN) return;
    dmem(GP_SKIN_GLOBALS, 0x20);
    uint32_t skin = w3d_gwk(geo + w3d_gwk(GP_SKIN_GLOBALS + 8)), hier = w3d_gwk(obj + w3d_gwk(GP_SKIN_GLOBALS + 4));
    if (skin) {
        dmem(skin, 0x60);
        uint32_t nb = w3d_gwk(skin), ids = w3d_gwk(skin + 8), mats = w3d_gwk(skin + 12);
        if (nb && nb < 128) {
            if (ids) dmem(ids, nb);
            if (mats) dmem(mats, nb * 64u);
        }
    }
    if (hier) {
        dmem(hier, 0x30);
        uint32_t nn = w3d_gwk(hier + 4), mats = w3d_gwk(hier + 8), info = w3d_gwk(hier + 16);
        if (nn && nn < 128) {
            if (mats) dmem(mats, nn * 64u);
            if (info) dmem(info, nn * 16u);
        }
    }
    if (skin && hier) {
        uint64_t a0 = c->r[4].d[0], a1 = c->r[5].d[0];
        uint32_t pal = hle_call(c, FN_SKIN_MATRICES, 2, obj, skin);
        c->r[4].d[0] = a0;
        c->r[5].d[0] = a1;
        uint32_t nb = w3d_gwk(skin);
        if (pal && nb && nb < 128) dmem(pal, nb * 64u);
        LOG("W3D dump skin: atomic %08x pds %08x skin %08x (%u bones) hier %08x (%u nodes, flags %08x) palette %08x\n", obj,
            pds, skin, nb, hier, hier ? w3d_gwk(hier + 4) : 0, hier ? w3d_gwk(hier) : 0, pal);
    }
}
#endif
