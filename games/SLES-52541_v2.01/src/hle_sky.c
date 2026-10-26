/* hle_sky.c - the sky gradient and the render buffer's world-space primitives, drawn on the PSP GE (overview + cfg
 * keys in hle_sky.h).
 *
 * Hooks: sky_polys 0x2EDE60 (only marks its scope) and buffer_render 0x2F5DE0 (draws the temp buffer when called
 * inside sky_polys; other callers go to the guest only). hle_sky_rbuf is called from the batch_flush hook.
 * Guest data (SLES-52541 v2.01):
 *   sky_polys sets: no texture, Z test / write off, vertex alpha off, SRCALPHA / INVSRCALPHA, fog off, cull none, and
 *   writes 5 quads of 4 vertices (top colour on 0 / 1, bottom colour on 2 / 3, alpha 255, u = v = 0) + 6 indices
 *   (0 2 1, 1 2 3) each. buffer_render draws the buffer as an indexed triangle list when it holds indices (and counts
 *   it at gp+2040), then empties it.
 *   immediate-mode 3D temp buffer: vertices *(gp+4756 = 0x66CC04), stride 0x24 (x, y, z floats +0, RGBA bytes +0x0C,
 *   u +0x1C, v +0x20); u16 indices *(gp-13492 = 0x6684BC); index count gp+340 (0x66BAC4), vertex count gp+344
 *   (0x66BAC8).
 *   current camera = *0x825C30: projection +0x14 (1 = perspective), view matrix +0x20 (rows right / up / at / pos at
 *   +0x20 / +0x30 / +0x40 / +0x50; it maps world to (X, Y, Z) with X/Z, Y/Z = 0..1 across the camera raster, y down),
 *   frame buffer raster +0x60 (width +0x0C, height +0x10), near +0x80, far +0x84.
 * Drawing: every vertex -> view space (float), each triangle clipped in view space against Z >= near and a guard band
 * (X/Z, Y/Z in [-1, 2]: one raster size beyond every edge - linear planes, exact; the result stays far inside the GE's
 * 4096 window), projected to raster pixels, drawn as 2D Gouraud triangles without Z and without texture. The sky is the
 * first colour draw of the frame (the 3D world and the 2D overlays come after it), so no depth is needed: Z stays as
 * the frame's clear left it. */
#include "hle.h"
#include "hle_render.h"
#include "hle_sky.h"
#include "hle_w3d.h"                                 /* w3d_opt_near_mm: the GE near plane */
#include "hle_water.h"                               /* water keys of the in-run A/B */
#include <stdlib.h>
#include <math.h>
#ifdef HLE_PSP
#include <pspkernel.h>
#include "hle_ge.h"
#else
#include <time.h>
#endif

void GUEST(002f5de0)(R5900Ctx *c); void GUEST(002ede60)(R5900Ctx *c);

int sky_opt_on = 1, sky_opt_guest = 0 /* default 0; sky_guest=1 also runs the original */, sky_opt_log = 0, sky_opt_far = 0, sky_opt_sprites = 1, sky_opt_log_flip = 0, sky_opt_tris = 1, sky_opt_split = 0, sky_opt_fx = 1;
int sky_opt_clear_once = 1;          /* hle_sky_clear_fill (0 = every Z-only clear fills) */

#define TB_VERTS 0x0066CC04u          /* gp+4756: pointer to the temp buffer's vertices */
#define TB_IDX 0x006684BCu            /* gp-13492: pointer to its u16 indices */
#define TB_NIDX 0x0066BAC4u           /* gp+340: index count */
#define TB_NVERTS 0x0066BAC8u         /* gp+344: vertex count */
#define TB_STAT 0x0066C168u           /* gp+2040: the counter the guest bumps per non-empty buffer */
#define DRV_CURCAM 0x00825C30u        /* the current camera pointer */
#define MAXV 64                       /* temp buffer vertices handled (the sky uses 20, 30 indices) */
#define MAXI 192
#define MAXOUT 384                    /* output vertices (128 triangles after clipping; 12 KB) */
#define GB_LO (-1.0f)                 /* guard band in X/Z, Y/Z (0..1 = the raster) */
#define GB_HI 2.0f

/* ------------------------------------------------------------------ statistics */
static uint64_t st_calls, st_sky, st_sky_drawn, st_tris_in, st_tris_out, st_tris_culled, st_tris_clipped, st_bad,
    st_notarget, st_skipped_guest, st_other;
static uint64_t us_prep, us_emit, us_guest, cyc_guest, n_guest_timed;
static uint32_t us_prep_max, us_emit_max;

static inline uint32_t now_us(void)
{
#ifdef HLE_PSP
    return sceKernelGetSystemTimeLow();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
#endif
}

static inline float gf(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }
static inline int ram_ok(uint32_t a, uint32_t n) { return a >= 0x100000u && a < hle_ram_end && n <= hle_ram_end - a; }

/* ------------------------------------------------------------------ options */
void hle_sky_cfg(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "sky")) sky_opt_on = x;
    else if (!strcmp(k, "sky_guest")) sky_opt_guest = x;
    else if (!strcmp(k, "sky_log")) sky_opt_log = x;
    else if (!strcmp(k, "sky_far")) sky_opt_far = x;
    else if (!strcmp(k, "sky_sprites")) sky_opt_sprites = x;
    else if (!strcmp(k, "sky_log_flip")) sky_opt_log_flip = x;
    else if (!strcmp(k, "sky_tris")) sky_opt_tris = x;
    else if (!strcmp(k, "sky_split")) sky_opt_split = x;
    else if (!strcmp(k, "sky_fx")) sky_opt_fx = x;
    else if (!strcmp(k, "sky_clear_once")) sky_opt_clear_once = x;
    else if (!strcmp(k, "water") || !strncmp(k, "water_", 6)) hle_water_cfg(k, v);   /* the in-run A/B */
}

void hle_sky_init(void)
{
#ifndef HLE_PSP
    static const char *keys[][2] = {{"sky", "HLE_SKY"}, {"sky_guest", "HLE_SKY_GUEST"}, {"sky_log", "HLE_SKY_LOG"},
                                    {"sky_far", "HLE_SKY_FAR"}, {"sky_sprites", "HLE_SKY_SPRITES"},
                                    {"sky_log_flip", "HLE_SKY_LOG_FLIP"}, {"sky_tris", "HLE_SKY_TRIS"},
                                    {"sky_split", "HLE_SKY_SPLIT"}, {"sky_fx", "HLE_SKY_FX"},
                                    {"sky_clear_once", "HLE_SKY_CLEAR_ONCE"}};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *e = getenv(keys[i][1]);
        if (e) hle_sky_cfg(keys[i][0], e);
    }
#else
    if (!hle_ge_on) sky_opt_on = sky_opt_sprites = sky_opt_tris = sky_opt_fx = 0;
#endif
    LOG("sky: sky=%d sky_guest=%d sky_far=%d sky_sprites=%d sky_log=%d - sky_polys 0x2EDE60 via "
        "buffer_render 0x2F5DE0 and the render buffer's world-space sprites (type 1) drawn by the HLE; sky_tris=%d "
        "(render buffer types 8 / 9: shadows, skid marks, ...); sky_clear_once=%d (only the frame start's "
        "Z clear fills the fog colour)\n", sky_opt_on, sky_opt_guest, sky_opt_far, sky_opt_sprites, sky_opt_log,
        sky_opt_tris, sky_opt_clear_once);
}

/* ------------------------------------------------------------------ clipping in view space */
typedef struct { float X, Y, Z, r, g, b, a, u, v; } SV;

static inline SV sv_lerp(const SV *p, const SV *q, float t)
{
    SV o;
    o.X = p->X + (q->X - p->X) * t;
    o.Y = p->Y + (q->Y - p->Y) * t;
    o.Z = p->Z + (q->Z - p->Z) * t;
    o.r = p->r + (q->r - p->r) * t;
    o.g = p->g + (q->g - p->g) * t;
    o.b = p->b + (q->b - p->b) * t;
    o.a = p->a + (q->a - p->a) * t;
    o.u = p->u + (q->u - p->u) * t;
    o.v = p->v + (q->v - p->v) * t;
    return o;
}

/* plane value of a point: >= 0 inside. 0 near (Z - zn), 1 far (zf - Z), 2 left (X - lo Z), 3 right (hi Z - X),
   4 top (Y - lo Z), 5 bottom (hi Z - Y) */
static inline float plane(const SV *p, int k, float zn, float zf)
{
    switch (k) {
    case 0: return p->Z - zn;
    case 1: return zf - p->Z;
    case 2: return p->X - GB_LO * p->Z;
    case 3: return GB_HI * p->Z - p->X;
    case 4: return p->Y - GB_LO * p->Z;
    default: return GB_HI * p->Z - p->Y;
    }
}

/* Sutherland-Hodgman against the near plane, (use_far) the far plane and the guard band; returns the vertex count */
static int clip_poly(SV *a, int n, SV *tmp, float zn, float zf, int use_far)
{
    SV *in = a, *out = tmp;
    for (int k = 0; k < 6 && n >= 3; k++) {
        if (k == 1 && !use_far) continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const SV *p = &in[i], *q = &in[i + 1 == n ? 0 : i + 1];
            float dp = plane(p, k, zn, zf), dq = plane(q, k, zn, zf);
            if (dp >= 0.0f) out[m++] = *p;
            if ((dp >= 0.0f) != (dq >= 0.0f)) out[m++] = sv_lerp(p, q, dp / (dp - dq));
        }
        SV *t = in; in = out; out = t;
        n = m;
    }
    if (in != a) memcpy(a, in, (size_t)n * sizeof(SV));
    return n;
}

/* ------------------------------------------------------------------ the camera of the current update */
typedef struct {
    uint32_t cam;
    float W, H;                  /* camera raster (guest screen pixels) */
    float m[4][3];               /* view matrix rows: world -> (X, Y, Z), X/Z and Y/Z = 0..1 across the raster */
    float zn, zf;                /* near / far */
    float da, db;                /* GE depth of a view-space Z: 32767.5 (1 - A - B / Z) = the 3D back-end's projection
                                    (hle_w3d.c w3d_camera_setup: A = (f+n)/(f-n), B = -2fn/(f-n), n = the GE near
                                    plane incl. w3d_near_mm) through sceGuDepthRange(65535, 0) */
} SkyCam;

/* 1 = a perspective camera that renders into the screen raster; 0 = anything else (st_bad / st_notarget counted by the
   caller) and -1 = not the screen */
static int cam_read(SkyCam *k)
{
    uint32_t cam = gw(DRV_CURCAM);
    if (!ram_ok(cam, 0xA0u) || gw(cam + 0x14) != 1) return 0;
    uint32_t fb = gw(cam + 0x60), scr = hle_render_screen_raster();
    if (!ram_ok(fb, 0x40u) || (scr && fb != scr)) return -1;   /* mirrors / render targets */
    k->cam = cam;
    k->W = (float)gw(fb + 0x0C);
    k->H = (float)gw(fb + 0x10);
    if (!(k->W > 0.0f && k->W <= 2048.0f && k->H > 0.0f && k->H <= 2048.0f)) return 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) k->m[r][c] = gf(cam + 0x20u + 0x10u * (uint32_t)r + 4u * (uint32_t)c);
    k->zn = gf(cam + 0x80);
    k->zf = gf(cam + 0x84);
    if (!(k->zn > 0.0f) || !(k->zf > k->zn)) return 0;
    float ng = (float)w3d_opt_near_mm * 0.001f;
    if (!(ng > k->zn) || ng >= k->zf * 0.5f) ng = k->zn;
    float A = (k->zf + ng) / (k->zf - ng), B = -2.0f * k->zf * ng / (k->zf - ng);
    k->da = 32767.5f * (1.0f - A);
    k->db = -32767.5f * B;
    return 1;
}

static inline void cam_xform(const SkyCam *k, float x, float y, float z, float *o)
{
    for (int c = 0; c < 3; c++) o[c] = x * k->m[0][c] + y * k->m[1][c] + z * k->m[2][c] + k->m[3][c];
}

/* ------------------------------------------------------------------ the sky draw */
static void cpu_line(const char *when);
static SV vv[MAXV];
static uint8_t vo[MAXV];                        /* outcode per vertex (plane k outside = bit k) */
static RndVtx outv[MAXOUT];

static void sky_draw(void)
{
    uint32_t t_prep = now_us();
    uint32_t ni = gw(TB_NIDX), nv = gw(TB_NVERTS), verts = gw(TB_VERTS), idx = gw(TB_IDX);
    if (!ni || !nv || nv > MAXV || ni > MAXI || !ram_ok(verts, nv * 0x24u) || !ram_ok(idx, ni * 2u)) {
        st_bad++;
        return;
    }
    SkyCam K;
    int ck = cam_read(&K);
    if (ck <= 0) {
        if (ck < 0) st_notarget++; else st_bad++;
        return;
    }
    float W = K.W, H = K.H, zn = K.zn, zf = K.zf;
    uint32_t cam = K.cam;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t p = verts + 0x24u * i;
        SV *o = &vv[i];
        float xyz[3];
        cam_xform(&K, gf(p), gf(p + 4), gf(p + 8), xyz);
        o->X = xyz[0];
        o->Y = xyz[1];
        o->Z = xyz[2];
        o->r = gb(p + 0x0C);
        o->g = gb(p + 0x0D);
        o->b = gb(p + 0x0E);
        o->a = gb(p + 0x0F);
        o->u = o->v = 0.0f;
        unsigned oc = 0;
        for (int pk = 0; pk < 6; pk++)
            if ((pk != 1 || sky_opt_far) && plane(o, pk, zn, zf) < 0.0f) oc |= 1u << pk;
        vo[i] = (uint8_t)oc;
    }
    if (st_sky <= (uint64_t)sky_opt_log) {
        LOG("SKY buffer %llu (flip %llu): %u vertices, %u indices, camera %08x raster %ux%u near %.3f far %.1f\n",
            (unsigned long long)st_sky, (unsigned long long)hle_render_flip_count(), nv, ni, cam, (unsigned)W,
            (unsigned)H, zn, zf);
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t p = verts + 0x24u * i;
            LOG("  v%-2u world %9.2f %9.2f %8.2f  view X %9.2f Y %9.2f Z %9.2f  screen %8.1f %8.1f  rgba %3.0f %3.0f %3.0f "
                "%3.0f\n", i, gf(p), gf(p + 4), gf(p + 8), vv[i].X, vv[i].Y, vv[i].Z,
                vv[i].Z > 0.0f ? vv[i].X / vv[i].Z * W : -1.0f, vv[i].Z > 0.0f ? vv[i].Y / vv[i].Z * H : -1.0f,
                vv[i].r, vv[i].g, vv[i].b, vv[i].a);
        }
    }
    int k = 0;
    for (uint32_t t = 0; t + 2 < ni; t += 3) {
        uint32_t i0 = gh(idx + 2u * t), i1 = gh(idx + 2u * t + 2u), i2 = gh(idx + 2u * t + 4u);
        if (i0 >= nv || i1 >= nv || i2 >= nv) { st_bad++; continue; }
        st_tris_in++;
        if (vo[i0] & vo[i1] & vo[i2]) { st_tris_culled++; continue; }   /* all outside one plane */
        SV poly[12], tmp[12];
        poly[0] = vv[i0]; poly[1] = vv[i1]; poly[2] = vv[i2];
        int n = 3;
        if (vo[i0] | vo[i1] | vo[i2]) {                                 /* straddles a plane: clip */
            n = clip_poly(poly, 3, tmp, zn, zf, sky_opt_far);
            st_tris_clipped++;
        }
        if (n < 3) { st_tris_culled++; continue; }
        RndVtx q[12];
        for (int j = 0; j < n; j++) {
            float iz = 1.0f / poly[j].Z;
            q[j].x = poly[j].X * iz * W;
            q[j].y = poly[j].Y * iz * H;
            q[j].u = q[j].v = 0.0f;
            q[j].r = poly[j].r; q[j].g = poly[j].g; q[j].b = poly[j].b; q[j].a = poly[j].a;
        }
        for (int j = 1; j + 1 < n && k + 3 <= MAXOUT; j++) {
            outv[k++] = q[0];
            outv[k++] = q[j];
            outv[k++] = q[j + 1];
            st_tris_out++;
        }
    }
    if (!k) return;
    RndState st;
    memset(&st, 0, sizeof st);
    st.raster = 0;                                   /* sky_polys: TEXTURERASTER NULL */
    st.src_blend = (int)hle_render_rs(RS_SRCBLEND);
    st.dst_blend = (int)hle_render_rs(RS_DESTBLEND);
    st.filter = (int)hle_render_rs(RS_TEXTUREFILTER);
    st.addr_u = (int)hle_render_rs(RS_TEXTUREADDRESSU);
    st.addr_v = (int)hle_render_rs(RS_TEXTUREADDRESSV);
    st.vertex_alpha = (int)hle_render_rs(RS_VERTEXALPHAENABLE);
    if (!st.vertex_alpha) { st.src_blend = 2; st.dst_blend = 1; }   /* no raster, vertex alpha off: opaque (ONE/ZERO) */
    uint32_t t_emit = now_us(), dt = t_emit - t_prep;
    us_prep += dt;
    if (dt > us_prep_max) us_prep_max = dt;
#ifdef HLE_PSP
    hle_ge_tris(outv, k, &st);
#else
    if (hle_raster_is_open()) hle_raster_tris(outv, k, &st);
#endif
    dt = now_us() - t_emit;
    us_emit += dt;
    if (dt > us_emit_max) us_emit_max = dt;
    st_sky_drawn++;
    if (st_sky_drawn == 256 || st_sky_drawn == 4096 || (st_sky_drawn & 16383) == 0) cpu_line("so far");
}

/* ------------------------------------------------------------------ render buffer type 1: world-space sprites
   batch_flush 0x3C50F0 passes type 1 entries in world space (no model matrix): coronas (sun, lamps) and the low
   clouds. Entry (40 bytes): x, y, z +0, half w +12, half h +16, u1 v1 u2 v2 s16 / 4096 +20, RGBA +28, sin / cos of
   the rotation +32 / +36 (0 / 1 = none). On the PS2 (VU1 program #35) the centre is the view-space position and the
   four corners are the centre +- the rotated half extents (hw cos - hh sin, ...) added BEFORE the divide by Z - so the
   half size on the screen is (hw, hh) / Z raster pixels; depth is the GS's 1/Z depth; a sprite with any corner
   outside the clip volume is dropped. Drawn here as two triangles per sprite with the render state of the flush
   (texture, blend, Z test against the 3D world's depth), near-plane culled, guard band clipped by the back-end.
   Type 2 and the screen-space type 0 (fonts, hle_render.c) are not handled here. */
static uint64_t st_rb_flush[10], st_rb_entries[10], st_spr_drawn, st_spr_culled, st_spr_flushes, st_spr_bad;
static uint64_t us_spr;
static int rb_logged;
#define MAXSPR 64                     /* sprites per draw batch (type-1 flushes hold 1..12 in gameplay) */
static RndVtx spr_v[MAXSPR * 6];
static float spr_z[MAXSPR * 6];
/* phase timers of the three render-buffer paths (PSP: the w3d batch flush
   that every FX draw forces first, the texture lookup, the GE emit; the rest = transform / clip) and the corona batch
   (hle_ce.c x_cbatch: inside the corona pass the type-1 sprites stay pending across flushes while the camera and the
   render state stay the same; a state change - hle_render.c drv_set_state -, the camera, a full batch or the end of
   the pass draws them) */
static uint64_t ce_us_spr_def, ce_us_spr_emit, ce_us_tri_def, ce_us_tri_tex, ce_us_tri_emit, ce_us_fx_def,
    ce_us_fx_tex, ce_us_fx_emit, ce_us_hv_emit;
static uint64_t ce_spr_held, ce_spr_state_draws;
static int spr_k, spr_batch;
static uint32_t spr_cam;
int hle_sky_spr_pend;

/* one batch of sprite triangles with the flush's render state */
static void spr_emit(int k, const SkyCam *K)
{
    RndState st;
    memset(&st, 0, sizeof st);
    st.raster = hle_render_rs(RS_TEXTURERASTER);
    st.src_blend = (int)hle_render_rs(RS_SRCBLEND);
    st.dst_blend = (int)hle_render_rs(RS_DESTBLEND);
    st.filter = (int)hle_render_rs(RS_TEXTUREFILTER);
    st.addr_u = (int)hle_render_rs(RS_TEXTUREADDRESSU);
    st.addr_v = (int)hle_render_rs(RS_TEXTUREADDRESSV);
    st.vertex_alpha = (int)hle_render_rs(RS_VERTEXALPHAENABLE);
#ifdef HLE_PSP
    (void)K;
    uint32_t te = now_us();                              /* phase timer (GE emit) */
    hle_ge_tris_z(spr_v, spr_z, k, &st);
    ce_us_spr_emit += now_us() - te;
#else
    if (hle_raster_is_open()) {
        static Rnd3Vtx v3[MAXSPR * 6];
        for (int j = 0; j < k; j++) {
            Rnd3Vtx *o = &v3[j];
            o->x = spr_v[j].x / K->W * 2.0f - 1.0f;
            o->y = 1.0f - spr_v[j].y / K->H * 2.0f;
            o->z = spr_z[j];
            o->q = 1.0f;
            o->u = spr_v[j].u;
            o->v = spr_v[j].v;
            o->r = spr_v[j].r; o->g = spr_v[j].g; o->b = spr_v[j].b; o->a = spr_v[j].a;
            o->fog = 1.0f;
        }
        Rnd3State s3;
        memset(&s3, 0, sizeof s3);
        s3.s = st;
        int sb = st.src_blend ? st.src_blend : 5, db = st.dst_blend ? st.dst_blend : 6;
        s3.s.src_blend = sb;
        s3.s.dst_blend = db;
        s3.blend = !(sb == 2 && db == 1);
        s3.atest = sb == 5 ? 1 : 0;                      /* as hle_ge.c: alpha > 0 with a SRCALPHA source factor */
        s3.ztest = hle_render_rs(RS_ZTESTENABLE) != 0;
        s3.zwrite = hle_render_rs(RS_ZWRITEENABLE) != 0;
        hle_raster_tris3d(v3, k, &s3);
    }
#endif
    st_spr_flushes++;
}

/* the corona batch (see above) */
static SkyCam spr_K;
static void spr_pending_draw(void)
{
    if (spr_k) spr_emit(spr_k, &spr_K);
    spr_k = 0;
    hle_sky_spr_pend = 0;
}
void hle_sky_spr_batch(int on)
{
    if (!on) spr_pending_draw();
    spr_batch = on;
}
void hle_sky_spr_state(void)
{
    ce_spr_state_draws++;
    spr_pending_draw();
}

static void tri_log(uint32_t buf, uint32_t n, int type);
static void tris_draw(uint32_t buf, uint32_t n);
static void fx_draw(uint32_t buf, uint32_t n, int type);

void hle_sky_rbuf(uint32_t buf, uint32_t n, int type)
{
    if (type < 0 || type > 9 || !n || n > 256 || !ram_ok(buf, 16u + 96u * n)) return;
    st_rb_flush[type]++;
    st_rb_entries[type] += n;
    if (type == 8 || type == 9) {
        tri_log(buf, n, type);
        tris_draw(buf, n);
        return;
    }
    if (type == 3 || type == 4 || type == 5 || type == 7) {
        fx_draw(buf, n, type);
        return;
    }
    if (type != 1) return;
    uint32_t t0 = now_us();
#ifdef HLE_PSP
    w3d_defer_flush();                                   /* done first by the GE emit anyway - timed here */
    ce_us_spr_def += now_us() - t0;
#endif
    SkyCam K;
    int logit = sky_opt_log >= 3 && rb_logged < sky_opt_log * 4 &&
                hle_render_flip_count() >= (uint64_t)sky_opt_log_flip;   /* detail log: sky_log >= 3 */
    if (cam_read(&K) <= 0) { st_spr_bad++; return; }
    uint32_t ras = hle_render_rs(RS_TEXTURERASTER);
    if (logit) {
        rb_logged++;
        LOG("SKY rbuf type %d (flip %llu): %u entries, raster %08x %ux%u, blend %u/%u, z test/write %u/%u, vertex "
            "alpha %u, fog %u, cull %u, filter %u, camera %08x %gx%g near %g far %g\n", type,
            (unsigned long long)hle_render_flip_count(), n, ras, ras ? gw(ras + 0x0C) : 0, ras ? gw(ras + 0x10) : 0,
            hle_render_rs(RS_SRCBLEND), hle_render_rs(RS_DESTBLEND), hle_render_rs(RS_ZTESTENABLE),
            hle_render_rs(RS_ZWRITEENABLE), hle_render_rs(RS_VERTEXALPHAENABLE), hle_render_rs(RS_FOGENABLE),
            hle_render_rs(RS_CULLMODE), hle_render_rs(RS_TEXTUREFILTER), K.cam, K.W, K.H, K.zn, K.zf);
    }
    if (!sky_opt_sprites) return;
    RndVtx *sv = spr_v;
    float *sz = spr_z;
    if (spr_k && K.cam != spr_cam) spr_pending_draw();  /* a batch is for one camera */
    int k = spr_k;
    spr_k = 0;
    hle_sky_spr_pend = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + 16u + 40u * i;
        float P[3];
        cam_xform(&K, gf(e), gf(e + 4), gf(e + 8), P);
        float hw = gf(e + 12), hh = gf(e + 16), sn = gf(e + 32), cs = gf(e + 36);
        if (logit && i < 6)
            LOG("  e%u pos %g %g %g half %g %g uv %d %d %d %d rgba %u %u %u %u rot %g %g | view %g %g %g -> screen %g %g "
                "half %g %g\n", i, gf(e), gf(e + 4), gf(e + 8), hw, hh, (int16_t)gh(e + 20), (int16_t)gh(e + 22),
                (int16_t)gh(e + 24), (int16_t)gh(e + 26), gb(e + 28), gb(e + 29), gb(e + 30), gb(e + 31), sn, cs, P[0],
                P[1], P[2], P[2] > 0.0f ? P[0] / P[2] * K.W : -1.0f, P[2] > 0.0f ? P[1] / P[2] * K.H : -1.0f,
                P[2] > 0.0f ? hw / P[2] : -1.0f, P[2] > 0.0f ? hh / P[2] : -1.0f);
        if (!(P[2] > K.zn) || !(hw == hw) || !(hh == hh) || !(sn == sn) || !(cs == cs)) { st_spr_culled++; continue; }
        float iz = 1.0f / P[2], cx = P[0] * iz * K.W, cy = P[1] * iz * K.H;
        float ax = (hw * cs) * iz, ay = (hw * sn) * iz, bx = (hh * sn) * iz, by = (hh * cs) * iz;
        /* corners: A = c + (-hw cos + hh sin, -hw sin - hh cos), B = c + (hw cos + hh sin, hw sin - hh cos),
           C = c + (hw cos - hh sin, hw sin + hh cos), D = c + (-hw cos - hh sin, -hw sin + hh cos) */
        float x[4] = {cx - ax + bx, cx + ax + bx, cx + ax - bx, cx - ax - bx};
        float y[4] = {cy - ay - by, cy + ay - by, cy + ay + by, cy - ay + by};
        float u1 = (int16_t)gh(e + 20) / 4096.0f, v1 = (int16_t)gh(e + 22) / 4096.0f;
        float u2 = (int16_t)gh(e + 24) / 4096.0f, v2 = (int16_t)gh(e + 26) / 4096.0f;
        float u[4] = {u1, u2, u2, u1}, v[4] = {v1, v1, v2, v2};
        /* the whole sprite off the screen: dropped (the GE would clip it anyway) */
        float xmin = fminf(fminf(x[0], x[1]), fminf(x[2], x[3])), xmax = fmaxf(fmaxf(x[0], x[1]), fmaxf(x[2], x[3]));
        float ymin = fminf(fminf(y[0], y[1]), fminf(y[2], y[3])), ymax = fmaxf(fmaxf(y[0], y[1]), fmaxf(y[2], y[3]));
        if (xmax < 0.0f || ymax < 0.0f || xmin > K.W || ymin > K.H) { st_spr_culled++; continue; }
        float d = K.da + K.db * iz;                      /* GE depth (bigger = nearer) */
        if (d < 0.0f) d = 0.0f;
        if (d > 65535.0f) d = 65535.0f;
        static const int tri[6] = {0, 1, 2, 0, 2, 3};
        if (k + 6 > MAXSPR * 6) { spr_emit(k, &K); k = 0; }
        for (int j = 0; j < 6; j++) {
            int c = tri[j];
            RndVtx *o = &sv[k];
            o->x = x[c];
            o->y = y[c];
            o->u = u[c];
            o->v = v[c];
            o->r = gb(e + 28);
            o->g = gb(e + 29);
            o->b = gb(e + 30);
            o->a = gb(e + 31);
            sz[k++] = d;
        }
        st_spr_drawn++;
    }
    if (k && spr_batch) {                                /* held for the next corona (x_cbatch) */
        spr_k = k;
        spr_K = K;
        spr_cam = K.cam;
        hle_sky_spr_pend = 1;
        ce_spr_held++;
    } else if (k) spr_emit(k, &K);
    us_spr += now_us() - t0;
}

/* ------------------------------------------------------------------ render buffer types 8 / 9: world-space triangles
   (log) 60-byte entries: 3 x xyz floats +0, 3 x (u, v) s16 = float x 4096 +36, 3 x RGBA +48 */
static int tri_logged;
static uint32_t tri_seen[64];                /* raster / type / blend signatures already logged */
static int tri_nseen;

static void tri_log(uint32_t buf, uint32_t n, int type)
{
    if (sky_opt_log < 3 || tri_logged >= sky_opt_log * 4 || hle_render_flip_count() < (uint64_t)sky_opt_log_flip) return;
    /* one log per new signature (raster, type, blend factors): every effect once, not the same shadow 40 times */
    uint32_t sig = hle_render_rs(RS_TEXTURERASTER) ^ (uint32_t)type << 28 ^ hle_render_rs(RS_SRCBLEND) << 20 ^
                   hle_render_rs(RS_DESTBLEND) << 24;
    for (int i = 0; i < tri_nseen; i++)
        if (tri_seen[i] == sig) return;
    if (tri_nseen < 64) tri_seen[tri_nseen++] = sig;
    tri_logged++;
    uint32_t ras = hle_render_rs(RS_TEXTURERASTER);
    LOG("SKY tri type %d (flip %llu): %u entries, raster %08x %ux%u, blend %u/%u, z test/write %u/%u, vertex alpha %u, "
        "fog %u, cull %u, filter %u, addr %u/%u\n", type, (unsigned long long)hle_render_flip_count(), n, ras,
        ras ? gw(ras + 0x0C) : 0, ras ? gw(ras + 0x10) : 0, hle_render_rs(RS_SRCBLEND), hle_render_rs(RS_DESTBLEND),
        hle_render_rs(RS_ZTESTENABLE), hle_render_rs(RS_ZWRITEENABLE), hle_render_rs(RS_VERTEXALPHAENABLE),
        hle_render_rs(RS_FOGENABLE), hle_render_rs(RS_CULLMODE), hle_render_rs(RS_TEXTUREFILTER),
        hle_render_rs(RS_TEXTUREADDRESSU), hle_render_rs(RS_TEXTUREADDRESSV));
    SkyCam K;
    int ok = cam_read(&K) > 0;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        uint32_t e = buf + 16u + 60u * i;
        for (int v = 0; v < 3; v++) {
            float P[3] = {0, 0, 0};
            if (ok) cam_xform(&K, gf(e + 12u * v), gf(e + 12u * v + 4), gf(e + 12u * v + 8), P);
            LOG("  e%u v%d pos %g %g %g uv %d %d rgba %u %u %u %u | view %g %g %g -> screen %g %g\n", i, v,
                gf(e + 12u * v), gf(e + 12u * v + 4), gf(e + 12u * v + 8), (int16_t)gh(e + 36 + 4u * v),
                (int16_t)gh(e + 38 + 4u * v), gb(e + 48 + 4u * v), gb(e + 49 + 4u * v), gb(e + 50 + 4u * v),
                gb(e + 51 + 4u * v), P[0], P[1], P[2], ok && P[2] > 0.0f ? P[0] / P[2] * K.W : -1.0f,
                ok && P[2] > 0.0f ? P[1] / P[2] * K.H : -1.0f);
        }
    }
}

/* ------------------------------------------------------------------ render buffer types 8 / 9: world-space triangles
   Used by the 3D scene and the effects for ped / vehicle shadows, headlight light pools, lamp shadows, blood pools
   (drawn with a shifted near plane, so their depth sits a little in front of the ground), skid marks, glass and a few
   others. On the PS2 every vertex is transformed by the view matrix with perspective-correct texturing, and the whole
   triangle is dropped when a vertex leaves the clip volume (no near-plane clipping). Here: view space, outcodes, near /
   guard-band clipping (better than the PS2's drop), big triangles whose depth ratio is > 1.5 split in four (sky_split
   times: the GE's through mode interpolates UV affinely), projection, the GE depth of each vertex, the flush's render
   state. */
static uint64_t st_tri_in, st_tri_out, st_tri_culled, st_tri_clipped, st_tri_split, st_tri_flushes, st_tri_bad;
static uint64_t us_tri;

/* output vertices in the GE's through-mode layout (hle_ge.c GEV_FMT: float u, v in texels; ABGR colour; float x,
   y in PSP screen pixels, z = GE depth), written once and copied into the display list by hle_ge_tris_packed. On the
   host x, y stay in raster pixels and u, v normalised (hv_sx = hv_sy = hv_tu = hv_tv = 1) for the PNG model. */
typedef struct { float u, v; uint32_t c; float x, y, z; } HV;
#define MAXHV 384                     /* 128 triangles per draw (9 KB) */
static HV hv[MAXHV];
static int hv_n;
static float hv_sx, hv_sy, hv_tu, hv_tv;
static RndState hv_st;

static void hv_flush(const SkyCam *K)
{
    if (!hv_n) return;
#ifdef HLE_PSP
    (void)K;
    uint32_t te = now_us();                              /* phase timer (GE emit) */
    hle_ge_tris_packed(hv, hv_n, &hv_st, 1);
    ce_us_hv_emit += now_us() - te;
#else
    if (hle_raster_is_open()) {
        static Rnd3Vtx v3[MAXHV];
        for (int j = 0; j < hv_n; j++) {
            Rnd3Vtx *o = &v3[j];
            o->x = hv[j].x / K->W * 2.0f - 1.0f;
            o->y = 1.0f - hv[j].y / K->H * 2.0f;
            o->z = hv[j].z;
            o->q = 1.0f;
            o->u = hv[j].u;
            o->v = hv[j].v;
            o->r = (float)(hv[j].c & 255u);
            o->g = (float)(hv[j].c >> 8 & 255u);
            o->b = (float)(hv[j].c >> 16 & 255u);
            o->a = (float)(hv[j].c >> 24);
            o->fog = 1.0f;
        }
        Rnd3State s3;
        memset(&s3, 0, sizeof s3);
        s3.s = hv_st;
        int sb = hv_st.src_blend ? hv_st.src_blend : 5, db = hv_st.dst_blend ? hv_st.dst_blend : 6;
        s3.s.src_blend = sb;
        s3.s.dst_blend = db;
        s3.blend = !(sb == 2 && db == 1);
        s3.atest = sb == 5 ? 1 : 0;
        s3.ztest = hle_render_rs(RS_ZTESTENABLE) != 0;
        s3.zwrite = hle_render_rs(RS_ZWRITEENABLE) != 0;
        hle_raster_tris3d(v3, hv_n, &s3);
    }
#endif
    hv_n = 0;
}

static inline void hv_put(float X, float Y, float Z, float u, float v, uint32_t c, const SkyCam *K)
{
    float iz = 1.0f / Z, d = K->da + K->db * iz;
    HV *o = &hv[hv_n++];
    o->u = u * hv_tu;
    o->v = v * hv_tv;
    o->c = c;
    o->x = X * iz * K->W * hv_sx;
    o->y = Y * iz * K->H * hv_sy;
    o->z = d < 0.0f ? 0.0f : d > 65535.0f ? 65535.0f : d;
}

static inline uint32_t pack_c(const SV *s)
{
    uint32_t c = 0;
    float f[4] = {s->r, s->g, s->b, s->a};
    for (int k = 0; k < 4; k++) {
        float x = f[k];
        uint32_t q = !(x > 0.0f) ? 0u : x >= 255.0f ? 255u : (uint32_t)(x + 0.5f);
        c |= q << (8 * k);
    }
    return c;
}

/* the affine error of a through-mode triangle grows with its depth ratio and its size on the screen: with sky_split=N
   (default 0 - blurry shadow / skid textures hardly show it, and every split costs CPU) a triangle with Zmax > 1.5 Zmin
   and more than 40 raster pixels across is split in four, at most N times */
static int tri_wants_split(const SV *a, const SV *b, const SV *c, const SkyCam *K, int depth)
{
    if (depth >= sky_opt_split) return 0;
    float z0 = fminf(a->Z, fminf(b->Z, c->Z)), z1 = fmaxf(a->Z, fmaxf(b->Z, c->Z));
    if (!(z1 > 1.5f * z0)) return 0;
    float ia = 1.0f / a->Z, ib = 1.0f / b->Z, ic = 1.0f / c->Z;
    float xa = a->X * ia * K->W, xb = b->X * ib * K->W, xc = c->X * ic * K->W;
    float ya = a->Y * ia * K->H, yb = b->Y * ib * K->H, yc = c->Y * ic * K->H;
    float ex = fmaxf(xa, fmaxf(xb, xc)) - fminf(xa, fminf(xb, xc));
    float ey = fmaxf(ya, fmaxf(yb, yc)) - fminf(ya, fminf(yb, yc));
    return ex > 40.0f || ey > 40.0f;
}

static void tri_emit(const SV *a, const SV *b, const SV *c, const SkyCam *K, int depth)
{
    if (tri_wants_split(a, b, c, K, depth)) {             /* perspective: split at the edge midpoints */
        SV ab = sv_lerp(a, b, 0.5f), bc = sv_lerp(b, c, 0.5f), ca = sv_lerp(c, a, 0.5f);
        st_tri_split++;
        tri_emit(a, &ab, &ca, K, depth + 1);
        tri_emit(&ab, b, &bc, K, depth + 1);
        tri_emit(&ca, &bc, c, K, depth + 1);
        tri_emit(&ab, &bc, &ca, K, depth + 1);
        return;
    }
    if (hv_n + 3 > MAXHV) hv_flush(K);
    hv_put(a->X, a->Y, a->Z, a->u, a->v, pack_c(a), K);
    hv_put(b->X, b->Y, b->Z, b->u, b->v, pack_c(b), K);
    hv_put(c->X, c->Y, c->Z, c->u, c->v, pack_c(c), K);
    st_tri_out++;
}

/* outcode of the near plane and the guard band (the plane() numbering, no far plane); NaN counts as outside */
static inline unsigned oc_xyz(float X, float Y, float Z, float zn)
{
    unsigned o = 0;
    if (!(Z - zn >= 0.0f)) o |= 1u;
    if (!(X - GB_LO * Z >= 0.0f)) o |= 4u;
    if (!(GB_HI * Z - X >= 0.0f)) o |= 8u;
    if (!(Y - GB_LO * Z >= 0.0f)) o |= 16u;
    if (!(GB_HI * Z - Y >= 0.0f)) o |= 32u;
    return o;
}

static void tris_draw(uint32_t buf, uint32_t n)
{
    if (!sky_opt_tris || !ram_ok(buf, 16u + 60u * n)) return;
    uint32_t t0 = now_us();
#ifdef HLE_PSP
    w3d_defer_flush();                                   /* done first by the GE emit anyway - timed here */
    ce_us_tri_def += now_us() - t0;
    uint64_t hv0 = ce_us_hv_emit;
#endif
    SkyCam K;
    if (cam_read(&K) <= 0) { st_tri_bad++; return; }
    memset(&hv_st, 0, sizeof hv_st);
    hv_st.raster = hle_render_rs(RS_TEXTURERASTER);
    hv_st.src_blend = (int)hle_render_rs(RS_SRCBLEND);
    hv_st.dst_blend = (int)hle_render_rs(RS_DESTBLEND);
    hv_st.filter = (int)hle_render_rs(RS_TEXTUREFILTER);
    hv_st.addr_u = (int)hle_render_rs(RS_TEXTUREADDRESSU);
    hv_st.addr_v = (int)hle_render_rs(RS_TEXTUREADDRESSV);
    hv_st.vertex_alpha = (int)hle_render_rs(RS_VERTEXALPHAENABLE);
#ifdef HLE_PSP
    hv_sx = 480.0f / K.W;
    hv_sy = 272.0f / K.H;
    int tw = 0, th = 0;
    uint32_t tt = now_us();
    if (hv_st.raster && !hle_ge_tex_size(hv_st.raster, &tw, &th)) { st_tri_bad++; return; }   /* not drawable */
    ce_us_tri_tex += now_us() - tt;
    hv_tu = (float)tw;
    hv_tv = (float)th;
#else
    hv_sx = hv_sy = hv_tu = hv_tv = 1.0f;
#endif
    hv_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + 16u + 60u * i;
        float P[3][3], u[3], v[3];
        uint32_t c[3];
        unsigned oc_and = 0x3F, oc_or = 0;
        for (int j = 0; j < 3; j++) {
            cam_xform(&K, gf(e + 12u * j), gf(e + 12u * j + 4), gf(e + 12u * j + 8), P[j]);
            unsigned oc = oc_xyz(P[j][0], P[j][1], P[j][2], K.zn);
            u[j] = (int16_t)gh(e + 36u + 4u * j) * (1.0f / 4096.0f);
            v[j] = (int16_t)gh(e + 38u + 4u * j) * (1.0f / 4096.0f);
            c[j] = gw(e + 48u + 4u * j);              /* R, G, B, A bytes = the GE's ABGR word */
            oc_and &= oc;
            oc_or |= oc;
        }
        st_tri_in++;
        if (oc_and) { st_tri_culled++; continue; }
        if (!oc_or && !sky_opt_split) {                  /* fast path: inside, no split - straight into the batch */
            if (hv_n + 3 > MAXHV) hv_flush(&K);
            for (int j = 0; j < 3; j++) hv_put(P[j][0], P[j][1], P[j][2], u[j], v[j], c[j], &K);
            st_tri_out++;
            continue;
        }
        SV poly[12], tmp[12];
        for (int j = 0; j < 3; j++) {
            SV *s = &poly[j];
            s->X = P[j][0];
            s->Y = P[j][1];
            s->Z = P[j][2];
            s->u = u[j];
            s->v = v[j];
            s->r = (float)(c[j] & 255u);
            s->g = (float)(c[j] >> 8 & 255u);
            s->b = (float)(c[j] >> 16 & 255u);
            s->a = (float)(c[j] >> 24);
        }
        int m = 3;
        if (oc_or) {
            m = clip_poly(poly, 3, tmp, K.zn, K.zf, 0);
            st_tri_clipped++;
            if (m < 3) { st_tri_culled++; continue; }
        }
        for (int j = 1; j + 1 < m; j++) tri_emit(&poly[0], &poly[j], &poly[j + 1], &K, 0);
    }
    hv_flush(&K);
    st_tri_flushes++;
    us_tri += now_us() - t0;
#ifdef HLE_PSP
    ce_us_tri_emit += ce_us_hv_emit - hv0;
#endif
}

/* ------------------------------------------------------------------ render buffer types 3 / 4 / 5 / 7: FX particles
   The effect system (fire, explosions, sparks, smoke, dust, blood, splashes, exhaust; called from render_effects
   0x246FD0) queues camera-facing quads in the render buffer: type 5 / 7 = 96-byte entries with RGBA +80, type 3 / 4 =
   80-byte entries with RGBA +64 (type 4 = with fog). Entry: position xyz +0, axis A xyz +12, axis B xyz +24, four
   extents +48 (s0, s1 along A, s2, s3 along B). The corners are pos + A s0 + B s2, pos + A s1 + B s2,
   pos + A s1 + B s3, pos + A s0 + B s3 with UV (0,0) (1,0) (1,1) (0,1); on the PS2 a quad with a corner outside the
   clip volume is dropped. x_rbuf=1 skips the PS2 batch renderers - only this draws them. Drawn here: pos x the view
   matrix, A / B x its 3x3, corners as linear combinations in view space (3 transforms per particle instead of 4),
   clipped like the type 8/9 triangles, the flush's render state (texture, blend, Z test / write). */
static uint64_t st_fx_in, st_fx_out, st_fx_culled, st_fx_clipped, st_fx_flushes, st_fx_bad;
static uint64_t us_fx;
static int fx_logged;

/* sky_log >= 1: which textures / blends the particles used (report: "sky: FX texture"; host: + the texture name) */
#define FX_NTEX 48
static struct { uint32_t ras, col; int sb, db; uint64_t flushes, n, flip0, flip1; } fx_tex[FX_NTEX];
static int fx_ntex;

static void fx_tex_note(uint32_t n, uint32_t col)
{
    int i;
    for (i = 0; i < fx_ntex; i++)
        if (fx_tex[i].ras == hv_st.raster && fx_tex[i].sb == hv_st.src_blend && fx_tex[i].db == hv_st.dst_blend) break;
    if (i == fx_ntex) {
        if (fx_ntex == FX_NTEX) return;
        fx_ntex++;
        fx_tex[i].ras = hv_st.raster;
        fx_tex[i].sb = hv_st.src_blend;
        fx_tex[i].db = hv_st.dst_blend;
        fx_tex[i].col = col;
        fx_tex[i].flip0 = hle_render_flip_count();
    }
    fx_tex[i].flip1 = hle_render_flip_count();
    fx_tex[i].flushes++;
    fx_tex[i].n += n;
}

/* host only: the texture object (raster pointer +0, name char[32] +0x10) that holds a raster - a scan of guest RAM */
static const char *fx_tex_name(uint32_t ras, char *out)
{
    strcpy(out, "Unknown");
    (void)ras;
#ifndef HLE_PSP
    for (uint32_t a = 0x100000u; ras && a + 0x30u < hle_ram_end; a += 4u) {
        if (gw(a) != ras) continue;
        char s[33];
        int k, ok = 1;
        for (k = 0; k < 32; k++) {
            uint32_t w = gw(a + 0x10u + (uint32_t)(k & ~3));
            char ch = (char)(w >> (8 * (k & 3)) & 255u);
            s[k] = ch;
            if (!ch) break;
            if (ch < 0x20 || ch > 0x7E) { ok = 0; break; }
        }
        if (ok && k >= 2 && k < 32) { s[k] = 0; strcpy(out, s); break; }
    }
#endif
    return out;
}

static void fx_draw(uint32_t buf, uint32_t n, int type)
{
    uint32_t stride = type == 5 || type == 7 ? 96u : 80u, coff = type == 5 || type == 7 ? 80u : 64u;
    if (!sky_opt_fx || !ram_ok(buf, 16u + stride * n)) return;
    uint32_t t0 = now_us();
#ifdef HLE_PSP
    w3d_defer_flush();                                   /* done first by the GE emit anyway - timed here */
    ce_us_fx_def += now_us() - t0;
    uint64_t hv0 = ce_us_hv_emit;
#endif
    SkyCam K;
    if (cam_read(&K) <= 0) { st_fx_bad++; return; }
    memset(&hv_st, 0, sizeof hv_st);
    hv_st.raster = hle_render_rs(RS_TEXTURERASTER);
    hv_st.src_blend = (int)hle_render_rs(RS_SRCBLEND);
    hv_st.dst_blend = (int)hle_render_rs(RS_DESTBLEND);
    hv_st.filter = (int)hle_render_rs(RS_TEXTUREFILTER);
    hv_st.addr_u = (int)hle_render_rs(RS_TEXTUREADDRESSU);
    hv_st.addr_v = (int)hle_render_rs(RS_TEXTUREADDRESSV);
    hv_st.vertex_alpha = (int)hle_render_rs(RS_VERTEXALPHAENABLE);
    if (sky_opt_log && n) fx_tex_note(n, gw(buf + 16u + coff));
    int logit = sky_opt_log >= 3 && fx_logged < sky_opt_log * 4 &&
                hle_render_flip_count() >= (uint64_t)sky_opt_log_flip;
    if (logit) {
        fx_logged++;
        uint32_t ras = hv_st.raster;
        LOG("SKY fx type %d (flip %llu): %u entries, raster %08x %ux%u, blend %d/%d, z test/write %u/%u, vertex alpha %d, "
            "fog %u, cull %u\n", type, (unsigned long long)hle_render_flip_count(), n, ras, ras ? gw(ras + 0x0C) : 0,
            ras ? gw(ras + 0x10) : 0, hv_st.src_blend, hv_st.dst_blend, hle_render_rs(RS_ZTESTENABLE),
            hle_render_rs(RS_ZWRITEENABLE), hv_st.vertex_alpha, hle_render_rs(RS_FOGENABLE), hle_render_rs(RS_CULLMODE));
        for (uint32_t i = 0; i < n && i < 3; i++) {
            uint32_t e = buf + 16u + stride * i;
            LOG("  e%u pos %g %g %g A %g %g %g B %g %g %g s %g %g %g %g rgba %08x (+36 %08x, +64 %08x)\n", i, gf(e),
                gf(e + 4), gf(e + 8), gf(e + 12), gf(e + 16), gf(e + 20), gf(e + 24), gf(e + 28), gf(e + 32), gf(e + 48),
                gf(e + 52), gf(e + 56), gf(e + 60), gw(e + coff), gw(e + 36), gw(e + 64));
        }
    }
#ifdef HLE_PSP
    hv_sx = 480.0f / K.W;
    hv_sy = 272.0f / K.H;
    int tw = 0, th = 0;
    uint32_t tt = now_us();
    if (hv_st.raster && !hle_ge_tex_size(hv_st.raster, &tw, &th)) { st_fx_bad++; return; }   /* not drawable */
    ce_us_fx_tex += now_us() - tt;
    hv_tu = (float)tw;
    hv_tv = (float)th;
#else
    hv_sx = hv_sy = hv_tu = hv_tv = 1.0f;
#endif
    hv_n = 0;
    static const float cu[4] = {0.0f, 1.0f, 1.0f, 0.0f}, cv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + 16u + stride * i;
        float P[3], A[3], B[3];
        cam_xform(&K, gf(e), gf(e + 4), gf(e + 8), P);
        float ax = gf(e + 12), ay = gf(e + 16), az = gf(e + 20), bx = gf(e + 24), by = gf(e + 28), bz = gf(e + 32);
        for (int c = 0; c < 3; c++) {
            A[c] = ax * K.m[0][c] + ay * K.m[1][c] + az * K.m[2][c];
            B[c] = bx * K.m[0][c] + by * K.m[1][c] + bz * K.m[2][c];
        }
        float s0 = gf(e + 48), s1 = gf(e + 52), s2 = gf(e + 56), s3 = gf(e + 60);
        float sa[4] = {s0, s1, s1, s0}, sb[4] = {s2, s2, s3, s3};
        uint32_t col = gw(e + coff);
        float Q[4][3];
        unsigned oc_and = 0x3F, oc_or = 0;
        for (int k = 0; k < 4; k++) {
            for (int c = 0; c < 3; c++) Q[k][c] = P[c] + A[c] * sa[k] + B[c] * sb[k];
            unsigned oc = oc_xyz(Q[k][0], Q[k][1], Q[k][2], K.zn);
            oc_and &= oc;
            oc_or |= oc;
        }
        st_fx_in++;
        if (oc_and) { st_fx_culled++; continue; }
        if (!oc_or) {                                    /* fast path: the quad as two triangles (0 1 2, 0 2 3) */
            if (hv_n + 6 > MAXHV) hv_flush(&K);
            HV *o = &hv[hv_n];
            for (int k = 0; k < 4; k++) hv_put(Q[k][0], Q[k][1], Q[k][2], cu[k], cv[k], col, &K);   /* o[0..3] */
            o[5] = o[3];
            o[4] = o[2];
            o[3] = o[0];
            hv_n += 2;
            st_fx_out++;
            continue;
        }
        SV poly[12], tmp[12];
        for (int k = 0; k < 4; k++) {
            SV *s = &poly[k];
            s->X = Q[k][0];
            s->Y = Q[k][1];
            s->Z = Q[k][2];
            s->u = cu[k];
            s->v = cv[k];
            s->r = (float)(col & 255u);
            s->g = (float)(col >> 8 & 255u);
            s->b = (float)(col >> 16 & 255u);
            s->a = (float)(col >> 24);
        }
        int m = clip_poly(poly, 4, tmp, K.zn, K.zf, 0);
        st_fx_clipped++;
        if (m < 3) { st_fx_culled++; continue; }
        for (int j = 1; j + 1 < m; j++) tri_emit(&poly[0], &poly[j], &poly[j + 1], &K, 0);
        st_fx_out++;
    }
    hv_flush(&K);
    st_fx_flushes++;
    us_fx += now_us() - t0;
#ifdef HLE_PSP
    ce_us_fx_emit += ce_us_hv_emit - hv0;
#endif
}

/* ------------------------------------------------------------------ the hooks
   The translated callers keep ra in a C local (v5 ABI calls), so buffer_render cannot tell its caller from
   c->r[31]: sky_polys itself is hooked and marks the scope. */
static int in_skypolys;
static uint64_t st_skypolys;

HLE(002ede60)
{
    st_skypolys++;
    in_skypolys = 1;
    GUEST(002ede60)(c);
    in_skypolys = 0;
}

HLE(002f5de0)
{
    st_calls++;
    if (!in_skypolys) {                              /* the other temp-buffer users: guest only */
        st_other++;
        GUEST(002f5de0)(c);
        return;
    }
    st_sky++;
    if (sky_opt_on) sky_draw();
    if (sky_opt_guest || !sky_opt_on) {
        uint64_t sw = hle_stat_switches, v0 = hle_vtime;
        uint32_t t0 = now_us();
        GUEST(002f5de0)(c);
        if (hle_stat_switches == sw) {
            us_guest += now_us() - t0;
            cyc_guest += hle_vtime - v0;
            n_guest_timed++;
        }
    } else {
        /* sky_guest=0: what the original leaves in guest memory (the counter, the emptied buffer), without its
           transform / draw (it only builds VU1 DMA packets that the HLE drops; the driver's state ends as before) */
        if (gw(TB_NIDX)) sw_(TB_STAT, gw(TB_STAT) + 1u);
        sw_(TB_NVERTS, 0);
        sw_(TB_NIDX, 0);
        st_skipped_guest++;
    }
}

/* ------------------------------------------------------------------ the fog-colour fill of a Z-only clear
   w3d_sky (hle_ge.c hle_ge_clear, host: hle_render.c's clear hook) turns the frame-start Z-only clear of the screen
   camera (in game_loop 0x246760) into a fog-colour fill of the picture as well. The game clears Z a second time in a
   frame while fire / explosion particles with heat haze are alive: the heat-haze mask pass 0x517570, called by the post
   effects 0x514300, clears Z, draws a black additive full-screen quad and the heat-haze particles (then GS packets the
   HLE drops). That clear comes AFTER the 3D world, the effects and the colour filter, so filling there would cover the
   finished picture with the fog colour - a flat light (white-blue / white-grey) screen with only the HUD on it for as
   long as a burning wreck has heat haze. The PS2's Z-only clear never touches the colour.
   Rule (sky_clear_once=1, default): only the first Z-only clear of a flip fills (the frame start); a later one in the
   same flip, or one after a colour clear of the screen in the same flip, clears depth only. sky_clear_once=0: every
   Z-only clear of the screen fills. Called for every clear of the screen camera that includes Z (mode & 2); returns 1
   when the caller should fill the picture with the fog colour too. */
static uint64_t zc_flip = ~0ull, st_zc_first, st_zc_mid, st_zc_mid_filled, st_zc_mid_flips, zc_mid_flip0, zc_mid_flip1;
static int zc_n, zc_colour, zc_mid, zc_mid_logged;

int hle_sky_clear_fill(int mode)
{
    uint64_t f = hle_render_flip_count();
    if (f != zc_flip) { zc_flip = f; zc_n = zc_colour = zc_mid = 0; }
    if (mode & 1) { zc_colour = 1; return 0; }        /* a colour clear: the guest's own colour, the frame is fresh */
    if (!(mode & 2)) return 0;
    int first = !zc_n && !zc_colour;
    zc_n++;
    if (first) { st_zc_first++; return 1; }
    st_zc_mid++;                                       /* a mid-frame Z clear (the heat-haze mask pass) */
    if (!zc_mid) {
        zc_mid = 1;
        st_zc_mid_flips++;
        if (!zc_mid_flip0) zc_mid_flip0 = f;
        zc_mid_flip1 = f;
    }
    if (zc_mid_logged < 4) {
        zc_mid_logged++;
        LOG("sky: flip %llu: Z-only clear #%d of the screen in one frame (the heat-haze mask pass 0x517570 of a "
            "fire / explosion) - %s\n", (unsigned long long)f, zc_n,
            sky_opt_clear_once ? "depth only (sky_clear_once=1)" : "the picture FILLED with the fog colour (sky_clear_once=0, "
            "the white screen)");
    }
    if (sky_opt_clear_once) return 0;
    st_zc_mid_filled++;
    return 1;
}

static void cpu_line(const char *when)
{
    if (st_sky_drawn)
        LOG("sky CPU (%s): HLE prep (transform, clip, project) %.1f us per sky (max %u), back-end emit %.1f us per "
            "sky (max %u; host: the PNG rasteriser, PSP: the GE list), %llu skies; guest original %.1f us per sky host "
            "time / %.0f virtual cycles (%llu timed)\n", when, (double)us_prep / (double)st_sky_drawn, us_prep_max,
            (double)us_emit / (double)st_sky_drawn, us_emit_max, (unsigned long long)st_sky_drawn,
            n_guest_timed ? (double)us_guest / (double)n_guest_timed : 0.0,
            n_guest_timed ? (double)cyc_guest / (double)n_guest_timed : 0.0, (unsigned long long)n_guest_timed);
}

void hle_sky_report(void)
{
    LOG("sky: buffer_render 0x2F5DE0 %llu calls, %llu from sky_polys, %llu skies drawn (%llu "
        "triangles in, %llu clipped, %llu out, %llu clipped away), %llu bad buffers, %llu not the screen, guest "
        "original %s (%llu skipped)\n", (unsigned long long)st_calls, (unsigned long long)st_sky,
        (unsigned long long)st_sky_drawn, (unsigned long long)st_tris_in, (unsigned long long)st_tris_clipped,
        (unsigned long long)st_tris_out, (unsigned long long)st_tris_culled,
        (unsigned long long)st_bad, (unsigned long long)st_notarget, sky_opt_guest ? "run" : "skipped",
        (unsigned long long)st_skipped_guest);
    LOG("  sky_polys 0x2EDE60 %llu calls; buffer_render outside it %llu calls (guest only, not drawn by the "
        "HLE)\n", (unsigned long long)st_skypolys, (unsigned long long)st_other);
    LOG("sky: render buffer flushes by type 4..9: %llu/%llu/%llu/%llu/%llu/%llu (entries %llu/%llu/%llu/%llu/%llu/%llu)\n",
        (unsigned long long)st_rb_flush[4], (unsigned long long)st_rb_flush[5], (unsigned long long)st_rb_flush[6],
        (unsigned long long)st_rb_flush[7], (unsigned long long)st_rb_flush[8], (unsigned long long)st_rb_flush[9],
        (unsigned long long)st_rb_entries[4], (unsigned long long)st_rb_entries[5], (unsigned long long)st_rb_entries[6],
        (unsigned long long)st_rb_entries[7], (unsigned long long)st_rb_entries[8], (unsigned long long)st_rb_entries[9]);
    LOG("sky: render buffer flushes by type 0/1/2/3: %llu/%llu/%llu/%llu (entries %llu/%llu/%llu/%llu); world-space "
        "sprites (type 1, sky_sprites=%d): %llu drawn in %llu flushes, %llu culled (behind the near plane / off the "
        "screen), %llu flushes without a screen camera, %.1f us per type-1 flush\n",
        (unsigned long long)st_rb_flush[0], (unsigned long long)st_rb_flush[1], (unsigned long long)st_rb_flush[2],
        (unsigned long long)st_rb_flush[3], (unsigned long long)st_rb_entries[0], (unsigned long long)st_rb_entries[1],
        (unsigned long long)st_rb_entries[2], (unsigned long long)st_rb_entries[3], sky_opt_sprites,
        (unsigned long long)st_spr_drawn, (unsigned long long)st_spr_flushes, (unsigned long long)st_spr_culled,
        (unsigned long long)st_spr_bad, st_rb_flush[1] ? (double)us_spr / (double)st_rb_flush[1] : 0.0);
    LOG("sky: FX particles (types 3 / 4 / 5 / 7, sky_fx=%d): %llu in, %llu drawn, %llu clipped, %llu culled, in %llu "
        "flushes (%llu without a screen camera / texture), %.1f us per flush\n", sky_opt_fx, (unsigned long long)st_fx_in,
        (unsigned long long)st_fx_out, (unsigned long long)st_fx_clipped, (unsigned long long)st_fx_culled,
        (unsigned long long)st_fx_flushes, (unsigned long long)st_fx_bad,
        st_fx_flushes ? (double)us_fx / (double)st_fx_flushes : 0.0);
    for (int i = 0; i < fx_ntex; i++) {
        char nm[40];
        uint32_t r = fx_tex[i].ras;
        LOG("sky: FX texture %08x %ux%u \"%s\" blend %d/%d: %llu flushes, %llu particles, first rgba %08x, flips "
            "%llu-%llu\n", r,
            r && ram_ok(r, 0x14u) ? gw(r + 0x0Cu) : 0u, r && ram_ok(r, 0x14u) ? gw(r + 0x10u) : 0u, fx_tex_name(r, nm),
            fx_tex[i].sb, fx_tex[i].db, (unsigned long long)fx_tex[i].flushes, (unsigned long long)fx_tex[i].n,
            fx_tex[i].col, (unsigned long long)fx_tex[i].flip0, (unsigned long long)fx_tex[i].flip1);
    }
    LOG("sky: world-space triangles (types 8 / 9, sky_tris=%d): %llu in, %llu drawn (%llu splits for perspective), "
        "%llu clipped, %llu culled, in %llu flushes (%llu without a screen camera), %.1f us per flush\n", sky_opt_tris,
        (unsigned long long)st_tri_in, (unsigned long long)st_tri_out, (unsigned long long)st_tri_split,
        (unsigned long long)st_tri_clipped, (unsigned long long)st_tri_culled, (unsigned long long)st_tri_flushes,
        (unsigned long long)st_tri_bad, st_tri_flushes ? (double)us_tri / (double)st_tri_flushes : 0.0);
    LOG("sky clears (sky_clear_once=%d): %llu frame-start Z clears of the screen filled with the fog colour "
        "(w3d_sky); %llu mid-frame Z-only clears (heat-haze mask pass 0x517570) in %llu flips (flips %llu-%llu): %llu of "
        "them filled the finished picture with the fog colour, %llu depth only\n", sky_opt_clear_once,
        (unsigned long long)st_zc_first, (unsigned long long)st_zc_mid, (unsigned long long)st_zc_mid_flips,
        (unsigned long long)zc_mid_flip0, (unsigned long long)zc_mid_flip1, (unsigned long long)st_zc_mid_filled,
        (unsigned long long)(st_zc_mid - st_zc_mid_filled));
    /* phase split per flush (PSP; host: 0) and the corona batch (x_cbatch) */
    LOG("sky phases, us per flush: sprites %llu flushes: w3d flush %.1f, GE emit %.1f (%llu GE draws, %llu "
        "held for the corona batch, %llu drawn at a state change); triangles %llu: w3d flush %.1f, texture %.1f, GE emit "
        "%.1f, transform %.1f; FX %llu: w3d flush %.1f, texture %.1f, GE emit %.1f, transform %.1f\n",
        (unsigned long long)st_rb_flush[1], st_rb_flush[1] ? (double)ce_us_spr_def / (double)st_rb_flush[1] : 0.0,
        st_rb_flush[1] ? (double)ce_us_spr_emit / (double)st_rb_flush[1] : 0.0, (unsigned long long)st_spr_flushes,
        (unsigned long long)ce_spr_held, (unsigned long long)ce_spr_state_draws, (unsigned long long)st_tri_flushes,
        st_tri_flushes ? (double)ce_us_tri_def / (double)st_tri_flushes : 0.0,
        st_tri_flushes ? (double)ce_us_tri_tex / (double)st_tri_flushes : 0.0,
        st_tri_flushes ? (double)ce_us_tri_emit / (double)st_tri_flushes : 0.0,
        st_tri_flushes ? (double)(us_tri - ce_us_tri_def - ce_us_tri_tex - ce_us_tri_emit) / (double)st_tri_flushes : 0.0,
        (unsigned long long)st_fx_flushes, st_fx_flushes ? (double)ce_us_fx_def / (double)st_fx_flushes : 0.0,
        st_fx_flushes ? (double)ce_us_fx_tex / (double)st_fx_flushes : 0.0,
        st_fx_flushes ? (double)ce_us_fx_emit / (double)st_fx_flushes : 0.0,
        st_fx_flushes ? (double)(us_fx - ce_us_fx_def - ce_us_fx_tex - ce_us_fx_emit) / (double)st_fx_flushes : 0.0);
    cpu_line("total");
}
