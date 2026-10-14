/* hle_w3d_imm.c - the render driver's immediate-mode 3D primitives that reach the mesh node 0x391398.
 * The driver's immediate-mode transform (0x36A500) and its indexed / non-indexed draw calls (indexed: 0x369E18) run
 * the driver's PS2 immediate-mode render pipelines (ids 6 and 7) through the same mesh node as the atomics. The object
 * the node gets is a block of the driver's immediate-mode globals (driver globals 0x825C30 + *(gp+0x1178), +0x4C):
 * -0xC u16 vertex count, -8 vertices (x y z floats, RGBA bytes +0xC, u +0x1C, v +0x20), -4 vertex stride (0x24),
 * +0 flags (1 all opaque, 2 no clip, 4 XYZ, 8 RGBA, 0x10 UV), +4 world matrix pointer (NULL = world space), +0x20 the
 * render pipeline, +0x24 primitive type (1 line list, 2 polyline, 3 triangle list, 4 strip, 5 fan, 6 points), +0x28
 * u16 indices (NULL: the non-indexed draw), +0x2C index count (or the vertex count). The translated node only builds a
 * VU1 packet the HLE drops, so these were never drawn on the PSP: the 17 callers of the transform include the pieces
 * of broken objects (lamp posts: object.dat collision effect 200 = breakable), plane trails, rain, the rainbow and the
 * game's temp-buffer flush outside the sky (the sky itself: hle_sky.c; with sky_guest=0, the default, the sky's
 * immediate-mode 3D never runs).
 * Triangles (list / strip / fan) are transformed to view space on the CPU (world matrix x the camera's view matrix),
 * clipped against the near plane and the GE guard band, and drawn as float vertices with the camera's projection, the
 * driver's render states of the moment (raster, filter, addressing, Z, blending, cull, fog) and the vertex colours as
 * they are. Lines / points stay with the guest (not drawn). cfg w3d_im3d (default 1; 0 = not drawn). */
#include "hle.h"
#include "hle_render.h"
#include "hle_w3d.h"
#include "hle_w3d_int.h"
#include <math.h>

#define DRV_ENGINE 0x00825C30u
int w3d_opt_im3d = 1;
static uint64_t si_calls, si_drawn, si_tris, si_clipped, si_skip_prim, si_skip_bad, si_skip_cam, si_skip_target,
    si_skip_tex;

static int ram_ok(uint32_t a, uint32_t n) { return a >= 0x1000u && a < hle_ram_end && n <= hle_ram_end - a; }

/* the GE's limits in clip space (see hle_w3d_lit.c GUARD_X / GUARD_Y): |x| <= 8 w, |y| <= 9.5 w; view space ->
   clip (w3d_camera_setup): xc = 2X - Z, yc = Z - 2Y, wc = Z */
typedef struct { float p[3], u, v, c[4]; } IV;
static float plane(const float *p, int k, float near)
{
    switch (k) {
    case 0: return p[2] - near;                          /* Z >= near */
    case 1: return 2.0f * p[0] + 7.0f * p[2];            /* xc >= -8 wc */
    case 2: return 9.0f * p[2] - 2.0f * p[0];            /* xc <= 8 wc */
    case 3: return 10.5f * p[2] - 2.0f * p[1];           /* yc >= -9.5 wc */
    default: return 8.5f * p[2] + 2.0f * p[1];           /* yc <= 9.5 wc */
    }
}

#define IOUT 384
static W3dVC iout[IOUT];
static int niout;

static void iflush(void)
{
    if (niout) { w3d_be_tris(iout, niout); niout = 0; }
}

static void iemit(const IV *a)
{
    W3dVC *o = &iout[niout++];
    o->u = a->u; o->v = a->v;
    o->x = a->p[0]; o->y = a->p[1]; o->z = a->p[2];
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        float f = a->c[i] + 0.5f;
        c |= (uint32_t)(f <= 0.0f ? 0 : f >= 255.0f ? 255 : (int)f) << (8 * i);
    }
    o->c = c;
}

static void itri(const IV *a, const IV *b, const IV *c, float near)
{
    /* trivially outside the screen frustum (x, y in [-w, w], Z >= near): dropped */
    const IV *t[3] = {a, b, c};
    unsigned all = 0x1F, any = 0;
    for (int i = 0; i < 3; i++) {
        const float *p = t[i]->p;
        float xc = 2.0f * p[0] - p[2], yc = p[2] - 2.0f * p[1], w = p[2];
        unsigned oc = (p[2] < near) | (xc < -w) << 1 | (xc > w) << 2 | (yc < -w) << 3 | (yc > w) << 4;
        all &= oc;
        any |= oc;
        (void)any;
    }
    if (all) return;
    IV poly[2][12];
    int n = 3, cur = 0;
    for (int i = 0; i < 3; i++) poly[0][i] = *t[i];
    int clipped = 0;
    for (int k = 0; k < 5; k++) {
        float d[12];
        int outside = 0;
        for (int i = 0; i < n; i++) { d[i] = plane(poly[cur][i].p, k, near); outside |= d[i] < 0.0f; }
        if (!outside) continue;
        clipped = 1;
        int m = 0;
        for (int i = 0; i < n && m < 11; i++) {
            const IV *A = &poly[cur][i], *B = &poly[cur][(i + 1) % n];
            float da = d[i], db = d[(i + 1) % n];
            if (da >= 0.0f) poly[cur ^ 1][m++] = *A;
            if ((da >= 0.0f) != (db >= 0.0f) && m < 11) {
                float s = da / (da - db);
                IV *o = &poly[cur ^ 1][m++];
                for (int q = 0; q < 3; q++) o->p[q] = A->p[q] + (B->p[q] - A->p[q]) * s;
                o->u = A->u + (B->u - A->u) * s;
                o->v = A->v + (B->v - A->v) * s;
                for (int q = 0; q < 4; q++) o->c[q] = A->c[q] + (B->c[q] - A->c[q]) * s;
            }
        }
        cur ^= 1;
        n = m;
        if (n < 3) return;
    }
    if (clipped) si_clipped++;
    for (int i = 1; i + 1 < n; i++) {
        if (niout + 3 > IOUT) iflush();
        iemit(&poly[cur][0]);
        iemit(&poly[cur][i]);
        iemit(&poly[cur][i + 1]);
        si_tris++;
    }
}

static void iload(uint32_t vp, uint32_t stride, uint32_t k, const M43 *M, IV *o)
{
    uint32_t a = vp + stride * k;
    float x[3];
    for (int i = 0; i < 3; i++) { uint32_t w = gw(a + 4u * (uint32_t)i); memcpy(&x[i], &w, 4); }
    for (int q = 0; q < 3; q++) o->p[q] = x[0] * M->m[0][q] + x[1] * M->m[1][q] + x[2] * M->m[2][q] + M->m[3][q];
    for (int q = 0; q < 4; q++) o->c[q] = (float)gb(a + 0x0C + (uint32_t)q);
    uint32_t wu = gw(a + 0x1C), wv = gw(a + 0x20);
    memcpy(&o->u, &wu, 4);
    memcpy(&o->v, &wv, 4);
}

/* 1 = drawn (the guest node may be skipped), 0 = left to the guest */
int w3d_im3d(R5900Ctx *c, uint32_t stash)
{
    (void)c;
    if (!w3d_opt_im3d) return 0;
    si_calls++;
    w3d_defer_flush();                    /* the deferred buildings first */
    if (!ram_ok(stash - 0x0C, 0x3C)) { si_skip_bad++; return 0; }
    uint32_t nv = gh(stash - 0x0C), vp = gw(stash - 8), stride = gw(stash - 4), ltm = gw(stash + 4),
             pt = gw(stash + 0x24), ip = gw(stash + 0x28), ni = gw(stash + 0x2C);
    if (pt < 3 || pt > 5) { si_skip_prim++; return 0; }
    uint32_t nidx = ni;
    if (!nv || stride < 0x24 || stride > 0x100 || !ram_ok(vp, nv * stride) || (ip && (ni > 200000 || !ram_ok(ip, ni * 2u))) ||
        nidx < 3 || (!ip && nidx > nv)) {
        si_skip_bad++;
        return 0;
    }
    uint32_t camera = w3d_gwk(DRV_ENGINE);
    if (!camera || !w3d_camera_setup(camera)) { si_skip_cam++; return 0; }
    if (!w3d_be_begin(&w3d_cam)) { si_skip_target++; return 0; }
    M43 V, L, M;
    w3d_drv_mat(camera + 0x20, &V);
    if (ltm && ram_ok(ltm, 0x40)) { w3d_drv_mat(ltm, &L); w3d_mat_mul(&L, &V, &M); }
    else M = V;
    W3dState st;
    w3d_base_state(&st);
    uint32_t ras = hle_render_rs(RS_TEXTURERASTER), filt = hle_render_rs(RS_TEXTUREFILTER);
    uint32_t au = hle_render_rs(RS_TEXTUREADDRESSU), av = hle_render_rs(RS_TEXTUREADDRESSV);
    int acl = 0;
    if (!w3d_be_texture(ras, (int)filt, (int)au, (int)av, &acl)) { w3d_be_end(); si_skip_tex++; return 0; }
    st.textured = ras != 0;
    st.uscale = 1.0f;
    st.ubase = st.vbase = 0.0f;
    st.light = 0;
    st.lights = 0;
    int valpha = hle_render_rs(RS_VERTEXALPHAENABLE) != 0;
    int passes = w3d_alpha_passes(&st, acl, valpha);
#ifndef HLE_PSP
    {   /* diagnostic: the distinct immediate-mode draws (raster, primitive, flags) */
        static int dg = 24;
        static uint32_t dk[24];
        uint32_t key = ras * 2654435761u ^ pt << 3 ^ gw(stash) << 8 ^ (ltm != 0);
        int sn = 0;
        for (int q = 0; q < 24; q++) sn |= dk[q] == key;
        if (dg > 0 && !sn) {
            dk[--dg] = key;
            float x[3];
            for (int q = 0; q < 3; q++) { uint32_t w = gw(vp + 4u * (uint32_t)q); memcpy(&x[q], &w, 4); }
            LOG("world3d immediate 3D: flags %02x ltm %08x %u verts (stride %u, first %.1f %.1f %.1f rgba %08x) %u %s prim %u, "
                "raster %08x %ux%u, blend %u/%u, z %u/%u, flip %llu\n", gw(stash), ltm, nv, stride, x[0], x[1], x[2],
                gw(vp + 0x0C), nidx, ip ? "indices" : "verts", pt, ras, ras ? gw(ras + 0x0C) : 0,
                ras ? gw(ras + 0x10) : 0, hle_render_rs(RS_SRCBLEND), hle_render_rs(RS_DESTBLEND),
                hle_render_rs(RS_ZTESTENABLE), hle_render_rs(RS_ZWRITEENABLE),
                (unsigned long long)hle_render_flip_count());
        }
    }
#endif
    float near = w3d_cam.near_z;
    int zw = st.zwrite;
    for (int pass = 0; pass < passes; pass++) {
        if (pass == 1) { st.atest = 3; st.zwrite = 0; }
        w3d_be_state(&st);
        w3d_world_set(2);                     /* identity: the vertices are in view space already */
        IV a, b, cc;
        for (uint32_t i = 0; i + 2 < nidx;) {
            uint32_t k0, k1, k2;
            if (pt == 3) { k0 = i; k1 = i + 1; k2 = i + 2; i += 3; }
            else if (pt == 4) { k0 = i; k1 = i + 1; k2 = i + 2; if (i & 1) { k0 = i + 1; k1 = i; } i++; }
            else { k0 = 0; k1 = i + 1; k2 = i + 2; i++; }
            if (ip) { k0 = gh(ip + 2u * k0); k1 = gh(ip + 2u * k1); k2 = gh(ip + 2u * k2); }
            if (k0 >= nv || k1 >= nv || k2 >= nv) continue;
            iload(vp, stride, k0, &M, &a);
            iload(vp, stride, k1, &M, &b);
            iload(vp, stride, k2, &M, &cc);
            itri(&a, &b, &cc, near);
        }
        iflush();
    }
    st.zwrite = zw;
    w3d_be_end();
    si_drawn++;
    return 1;
}

void w3d_im3d_report(void)
{
    LOG("world3d immediate 3D (w3d_im3d %d): %llu node calls, %llu drawn (%llu triangles, %llu clipped); left to the guest: %llu "
        "lines / points, %llu bad, %llu camera, %llu not the screen, %llu texture\n", w3d_opt_im3d,
        (unsigned long long)si_calls, (unsigned long long)si_drawn, (unsigned long long)si_tris,
        (unsigned long long)si_clipped, (unsigned long long)si_skip_prim, (unsigned long long)si_skip_bad,
        (unsigned long long)si_skip_cam, (unsigned long long)si_skip_target, (unsigned long long)si_skip_tex);
}
