/* hle_water.c - the water surfaces, drawn on the PSP GE (overview + cfg keys in hle_water.h).
 *
 * Hook water_batch 0x3C6950(entries, n, LTM (NULL), program 1..4 = type + 1); hook water_render 0x182850 only for
 * timing and the water_tp test aid.
 * Guest data (SLES-52541 v2.01):
 *   water buffer 0x83D610: +0 count, +0x10 entries of 96 bytes (at most 24), +0x910 type 0..3:
 *     type 0 flat triangle, 1 high-detail triangle, 2 flat rectangle, 3 high-detail rectangle.
 *   entry: +0 x0 y0 (float), +8 size dx dy (types 0 / 2; types 1 / 3: half the size = cells of 2 units, signed for
 *     triangles), +16 u0 v0, +24 du dv (types 0 / 2: over the whole polygon; 1 / 3: per cell), +32 RGBA bytes
 *     (0.577 x the time cycle's water colour, the layer's alpha), +48 corner heights; types 1 / 3 also the wave
 *     amplitudes per corner (+64.. small, +80.. big).
 *   flat rectangle: corners (x0, y0) z +48, (x0, y0+dy) z +52, (x0+dx, y0+dy) z +56, (x0+dx, y0) z +60, UV u0 + {0, du},
 *     v0 + {0, dv} likewise, drawn as the triangles (x0,y1)(x0,y0)(x1,y1) / (x1,y1)(x0,y0)(x1,y0).
 *   flat triangle: (x0, y0) z +48 uv (u0, v0); (x0+dx, y0) z +52 uv (u0+du, v0); (x0, y0+dy) z +56 uv (u0, v0+dv).
 *   high-detail rectangle: heights +48 (x0,y0), +52 (x1,y0), +56 (x0,y1), +60 (x1,y1). On the PS2 the high-detail
 *     types are a grid of 2-unit cells with waves; the wave formula is not reproduced here: water_waves=1 uses an
 *     approximation (two sines, see wave_h), default off.
 *   render states at the batch: texture = the water texture (gp+0x290 = 0x66BC00), fog on, SRCALPHA / INVSRCALPHA,
 *     Z write off, cull none.
 * Colours: the vertex bytes are used as they are (0..255 = 0..1, as hle_w3d_imm.c); the PS2 also multiplies them by a
 * material colour - not checked whether the product differs. */
#include "hle.h"
#include "hle_render.h"
#include "hle_w3d.h"
#include "hle_w3d_int.h"
#include "hle_water.h"
#include <stdlib.h>
#include <math.h>

#define DRV_ENGINE 0x00825C30u
#define ESZ 96u

void GUEST(003c6950)(R5900Ctx *c);
void GUEST(00182850)(R5900Ctx *c);

int water_opt_on = 0, water_opt_guest = 0, water_opt_hd = 1, water_opt_waves = 0, water_opt_grid = 8,
    water_opt_log = 0;

static uint64_t st_calls, st_guest, st_drawn, st_ent[4], st_tris, st_clip, st_cull, st_skip_cam, st_skip_target,
    st_skip_tex, st_skip_bad, st_skip_hd;
static uint64_t us_draw, st_rw, us_rw;
static uint32_t us_max, us_rw_max;
static int logged;

static int ram_ok(uint32_t a, uint32_t n) { return a >= 0x1000u && a < hle_ram_end && n <= hle_ram_end - a; }
static inline float gf(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }

/* water_tp=FLIP:x:y:z[,FLIP:x:y:z...] (test aid, PSP and host): the player ped's matrix position is set at those flips
   (as hle_w3d.c HLE_W3D_TELEPORT on the host), from the water_render hook - for frames / A/B runs at water spots */
static struct { unsigned long long flip; float p[3]; } tp[8];
static int ntp, tp_next;
static void tp_parse(const char *e)
{
    ntp = tp_next = 0;
    while (e && *e && ntp < 8) {
        char *end;
        unsigned long long f = strtoull(e, &end, 10);
        float p[3];
        int ok = *end == ':';
        for (int i = 0; ok && i < 3; i++) {
            p[i] = strtof(end + 1, &end);
            ok = i < 2 ? *end == ':' : 1;
        }
        if (ok) { tp[ntp].flip = f; memcpy(tp[ntp].p, p, sizeof p); ntp++; }
        e = strchr(e, ',');
        if (e) e++;
    }
}

static void tp_tick(void)
{
    if (tp_next >= ntp || hle_render_flip_count() < tp[tp_next].flip) return;
    uint32_t ped = gw(0x0070A260u), m = ped && ram_ok(ped, 0x20) ? gw(ped + 0x14) : 0;
    if (m && ram_ok(m, 0x40)) {
        for (int i = 0; i < 3; i++) { uint32_t w; memcpy(&w, &tp[tp_next].p[i], 4); sw_(m + 0x30 + 4u * (uint32_t)i, w); }
        LOG("water test teleport (water_tp) at flip %llu: player %08x -> %.1f %.1f %.1f\n",
            (unsigned long long)hle_render_flip_count(), ped, tp[tp_next].p[0], tp[tp_next].p[1], tp[tp_next].p[2]);
    }
    tp_next++;
}

void hle_water_cfg(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "water")) water_opt_on = x;
    else if (!strcmp(k, "water_guest")) water_opt_guest = x;
    else if (!strcmp(k, "water_hd")) water_opt_hd = x;
    else if (!strcmp(k, "water_waves")) water_opt_waves = x;
    else if (!strcmp(k, "water_grid")) water_opt_grid = x < 1 ? 1 : x > 16 ? 16 : x;
    else if (!strcmp(k, "water_log")) water_opt_log = x;
    else if (!strcmp(k, "water_tp")) tp_parse(v);
}

void hle_water_init(void)
{
#ifndef HLE_PSP
    static const char *keys[][2] = {{"water", "HLE_WATER"}, {"water_guest", "HLE_WATER_GUEST"},
                                    {"water_hd", "HLE_WATER_HD"}, {"water_waves", "HLE_WATER_WAVES"},
                                    {"water_grid", "HLE_WATER_GRID"}, {"water_log", "HLE_WATER_LOG"},
                                    {"water_tp", "HLE_WATER_TP"}};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *e = getenv(keys[i][1]);
        if (e) hle_water_cfg(keys[i][0], e);
    }
#endif
    LOG("water: water=%d water_guest=%d water_hd=%d water_waves=%d water_grid=%d - the water batch renderer "
        "0x3C6950 (VU1 water_0..3) drawn by the HLE through the 3D back-end\n", water_opt_on, water_opt_guest,
        water_opt_hd, water_opt_waves, water_opt_grid);
    for (int i = 0; i < ntp; i++)
        LOG("water test teleport %d (water_tp): flip %llu -> %.1f %.1f %.1f\n", i, tp[i].flip, tp[i].p[0], tp[i].p[1],
            tp[i].p[2]);
}

/* ------------------------------------------------------------------ view space + clipping (as hle_w3d_imm.c) */
typedef struct { float p[3], u, v, c[4]; } WV;

/* the GE's limits in clip space (hle_w3d_imm.c): |x| <= 8 w, |y| <= 9.5 w; view -> clip: xc = 2X - Z, yc = Z - 2Y */
static float plane(const float *p, int k, float near)
{
    switch (k) {
    case 0: return p[2] - near;
    case 1: return 2.0f * p[0] + 7.0f * p[2];
    case 2: return 9.0f * p[2] - 2.0f * p[0];
    case 3: return 10.5f * p[2] - 2.0f * p[1];
    default: return 8.5f * p[2] + 2.0f * p[1];
    }
}

#define WOUT 384
static W3dVC wout[WOUT];
static int nwout;

static void wflush(void)
{
    if (nwout) { w3d_be_tris(wout, nwout); nwout = 0; }
}

static void wemit(const WV *a)
{
    W3dVC *o = &wout[nwout++];
    o->u = a->u; o->v = a->v;
    o->x = a->p[0]; o->y = a->p[1]; o->z = a->p[2];
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        float f = a->c[i] + 0.5f;
        c |= (uint32_t)(f <= 0.0f ? 0 : f >= 255.0f ? 255 : (int)f) << (8 * i);
    }
    o->c = c;
}

static void wtri(const WV *a, const WV *b, const WV *c, float near)
{
    const WV *t[3] = {a, b, c};
    unsigned all = 0x1F, any = 0;
    for (int i = 0; i < 3; i++) {
        const float *p = t[i]->p;
        float xc = 2.0f * p[0] - p[2], yc = p[2] - 2.0f * p[1], w = p[2];
        unsigned oc = (unsigned)(p[2] < near) | (unsigned)(xc < -w) << 1 | (unsigned)(xc > w) << 2 |
                      (unsigned)(yc < -w) << 3 | (unsigned)(yc > w) << 4;
        all &= oc;
        any |= oc;
    }
    if (all) { st_cull++; return; }
    WV poly[2][12];
    int n = 3, cur = 0, clipped = 0;
    for (int i = 0; i < 3; i++) poly[0][i] = *t[i];
    if (any) {
        for (int k = 0; k < 5; k++) {
            float d[12];
            int outside = 0;
            for (int i = 0; i < n; i++) { d[i] = plane(poly[cur][i].p, k, near); outside |= d[i] < 0.0f; }
            if (!outside) continue;
            clipped = 1;
            int m = 0;
            for (int i = 0; i < n && m < 11; i++) {
                const WV *A = &poly[cur][i], *B = &poly[cur][(i + 1) % n];
                float da = d[i], db = d[(i + 1) % n];
                if (da >= 0.0f) poly[cur ^ 1][m++] = *A;
                if ((da >= 0.0f) != (db >= 0.0f) && m < 11) {
                    float s = da / (da - db);
                    WV *o = &poly[cur ^ 1][m++];
                    for (int q = 0; q < 3; q++) o->p[q] = A->p[q] + (B->p[q] - A->p[q]) * s;
                    o->u = A->u + (B->u - A->u) * s;
                    o->v = A->v + (B->v - A->v) * s;
                    for (int q = 0; q < 4; q++) o->c[q] = A->c[q] + (B->c[q] - A->c[q]) * s;
                }
            }
            cur ^= 1;
            n = m;
            if (n < 3) { st_cull++; return; }
        }
    }
    if (clipped) st_clip++;
    for (int i = 1; i + 1 < n; i++) {
        if (nwout + 3 > WOUT) wflush();
        wemit(&poly[cur][0]);
        wemit(&poly[cur][i]);
        wemit(&poly[cur][i + 1]);
        st_tris++;
    }
}

/* ------------------------------------------------------------------ the entries */
static M43 V;                 /* the camera's view matrix (world -> view space) */
static float col[4];          /* the entry's colour */
static float near_z;

static void vtx(WV *o, float x, float y, float z, float u, float v)
{
    for (int q = 0; q < 3; q++) o->p[q] = x * V.m[0][q] + y * V.m[1][q] + z * V.m[2][q] + V.m[3][q];
    o->u = u;
    o->v = v;
    memcpy(o->c, col, sizeof col);
}

/* water_waves=1: an approximation of the high-detail wave height (the PS2's own formula is not reproduced) - two
   travelling sines scaled by the corner amplitudes (big, small), phase from the frame clock */
static float wave_t;
static inline float wave_h(float x, float y, float big, float small)
{
    return big * 0.5f * sinf(0.10f * x + 0.07f * y + wave_t) + small * 0.25f * sinf(0.45f * x - 0.33f * y + 2.3f * wave_t);
}

static void ent_flat_rect(uint32_t e)
{
    float x0 = gf(e), y0 = gf(e + 4), dx = gf(e + 8), dy = gf(e + 12);
    float u0 = gf(e + 16), v0 = gf(e + 20), du = gf(e + 24), dv = gf(e + 28);
    float z00 = gf(e + 48), z01 = gf(e + 52), z11 = gf(e + 56), z10 = gf(e + 60);
    WV a, b, c, d;                                    /* a (x0,y0) b (x0,y1) c (x1,y1) d (x1,y0) */
    vtx(&a, x0, y0, z00, u0, v0);
    vtx(&b, x0, y0 + dy, z01, u0, v0 + dv);
    vtx(&c, x0 + dx, y0 + dy, z11, u0 + du, v0 + dv);
    vtx(&d, x0 + dx, y0, z10, u0 + du, v0);
    wtri(&b, &a, &c, near_z);
    wtri(&c, &a, &d, near_z);
}

static void ent_flat_tri(uint32_t e)
{
    float x0 = gf(e), y0 = gf(e + 4), dx = gf(e + 8), dy = gf(e + 12);
    float u0 = gf(e + 16), v0 = gf(e + 20), du = gf(e + 24), dv = gf(e + 28);
    WV a, b, c;
    vtx(&a, x0, y0, gf(e + 48), u0, v0);
    vtx(&b, x0 + dx, y0, gf(e + 52), u0 + du, v0);
    vtx(&c, x0, y0 + dy, gf(e + 56), u0, v0 + dv);
    wtri(&a, &b, &c, near_z);
}

/* type 3: hx, hy = cells (2 units each), du, dv per cell; corners z / small / big amplitudes p0 (x0,y0), p1 (x1,y0),
   p2 (x0,y1), p3 (x1,y1) */
static void ent_hd_rect(uint32_t e)
{
    float x0 = gf(e), y0 = gf(e + 4), hx = gf(e + 8), hy = gf(e + 12);
    float u0 = gf(e + 16), v0 = gf(e + 20), duc = gf(e + 24), dvc = gf(e + 28);
    float dx = 2.0f * hx, dy = 2.0f * hy, du = duc * hx, dv = dvc * hy;
    float z[4] = {gf(e + 48), gf(e + 52), gf(e + 56), gf(e + 60)};
    if (!water_opt_waves) {
        WV a, b, c, d;
        vtx(&a, x0, y0, z[0], u0, v0);
        vtx(&b, x0, y0 + dy, z[2], u0, v0 + dv);
        vtx(&c, x0 + dx, y0 + dy, z[3], u0 + du, v0 + dv);
        vtx(&d, x0 + dx, y0, z[1], u0 + du, v0);
        wtri(&b, &a, &c, near_z);
        wtri(&c, &a, &d, near_z);
        return;
    }
    float sm[4] = {gf(e + 64), gf(e + 68), gf(e + 72), gf(e + 76)}, bg[4] = {gf(e + 80), gf(e + 84), gf(e + 88), gf(e + 92)};
    int nx = (int)fabsf(hx), ny = (int)fabsf(hy);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    if (nx > water_opt_grid) nx = water_opt_grid;
    if (ny > water_opt_grid) ny = water_opt_grid;
    WV row[2][17];
    for (int j = 0; j <= ny; j++) {
        float t = (float)j / (float)ny;
        WV *r = row[j & 1];
        for (int i = 0; i <= nx; i++) {
            float s = (float)i / (float)nx;
            float w00 = (1 - s) * (1 - t), w10 = s * (1 - t), w01 = (1 - s) * t, w11 = s * t;
            float x = x0 + dx * s, y = y0 + dy * t;
            float h = z[0] * w00 + z[1] * w10 + z[2] * w01 + z[3] * w11;
            h += wave_h(x, y, bg[0] * w00 + bg[1] * w10 + bg[2] * w01 + bg[3] * w11,
                        sm[0] * w00 + sm[1] * w10 + sm[2] * w01 + sm[3] * w11);
            vtx(&r[i], x, y, h, u0 + du * s, v0 + dv * t);
        }
        if (!j) continue;
        WV *p = row[(j - 1) & 1];
        for (int i = 0; i < nx; i++) {
            wtri(&r[i], &p[i], &r[i + 1], near_z);
            wtri(&r[i + 1], &p[i], &p[i + 1], near_z);
        }
    }
}

/* type 1: origin, half sizes (signed), per-cell UV steps; heights / small / big amplitudes of v0 (x0,y0), v1 (x1,y0),
   v2 (x0,y1) */
static void ent_hd_tri(uint32_t e)
{
    float x0 = gf(e), y0 = gf(e + 4), hx = gf(e + 8), hy = gf(e + 12);
    float u0 = gf(e + 16), v0 = gf(e + 20), duc = gf(e + 24), dvc = gf(e + 28);
    float dx = 2.0f * hx, dy = 2.0f * hy, du = duc * fabsf(hx), dv = dvc * fabsf(hy);
    float z[3] = {gf(e + 48), gf(e + 52), gf(e + 56)};
    if (water_opt_waves) {
        float sm[3] = {gf(e + 64), gf(e + 68), gf(e + 72)}, bg[3] = {gf(e + 80), gf(e + 84), gf(e + 88)};
        z[0] += wave_h(x0, y0, bg[0], sm[0]);
        z[1] += wave_h(x0 + dx, y0, bg[1], sm[1]);
        z[2] += wave_h(x0, y0 + dy, bg[2], sm[2]);
    }
    WV a, b, c;
    vtx(&a, x0, y0, z[0], u0, v0);
    vtx(&b, x0 + dx, y0, z[1], u0 + du, v0);
    vtx(&c, x0, y0 + dy, z[2], u0, v0 + dv);
    wtri(&a, &b, &c, near_z);
}

static void log_batch(uint32_t buf, uint32_t n, int type)
{
    uint32_t ras = hle_render_rs(RS_TEXTURERASTER);
    LOG("water batch (flip %llu): type %d, %u entries at %08x, raster %08x %ux%u, blend %u/%u, z test/write %u/%u, "
        "fog %u (colour %08x), cull %u, filter %u, addr %u/%u, vertex alpha %u\n",
        (unsigned long long)hle_render_flip_count(), type, n, buf, ras, ras ? gw(ras + 0x0C) : 0,
        ras ? gw(ras + 0x10) : 0, hle_render_rs(RS_SRCBLEND), hle_render_rs(RS_DESTBLEND),
        hle_render_rs(RS_ZTESTENABLE), hle_render_rs(RS_ZWRITEENABLE), hle_render_rs(RS_FOGENABLE),
        hle_render_rs(RS_FOGCOLOR), hle_render_rs(RS_CULLMODE), hle_render_rs(RS_TEXTUREFILTER),
        hle_render_rs(RS_TEXTUREADDRESSU), hle_render_rs(RS_TEXTUREADDRESSV), hle_render_rs(RS_VERTEXALPHAENABLE));
    for (uint32_t i = 0; i < n && i < 3; i++) {
        uint32_t e = buf + ESZ * i;
        LOG("  e%u: %.2f %.2f | %.3f %.3f | uv %.3f %.3f d %.4f %.4f | rgba %u %u %u %u | z %.2f %.2f %.2f %.2f | "
            "a %.3f %.3f %.3f %.3f | b %.3f %.3f %.3f %.3f\n", i, gf(e), gf(e + 4), gf(e + 8), gf(e + 12), gf(e + 16),
            gf(e + 20), gf(e + 24), gf(e + 28), gb(e + 32), gb(e + 33), gb(e + 34), gb(e + 35), gf(e + 48),
            gf(e + 52), gf(e + 56), gf(e + 60), gf(e + 64), gf(e + 68), gf(e + 72), gf(e + 76), gf(e + 80),
            gf(e + 84), gf(e + 88), gf(e + 92));
    }
}

/* 1 = drawn */
static int water_draw(uint32_t buf, uint32_t n, int type)
{
    if (type < 0 || type > 3 || !n || n > 64 || !ram_ok(buf, n * ESZ)) { st_skip_bad++; return 0; }
    if ((type == 1 || type == 3) && !water_opt_hd) { st_skip_hd++; return 1; }
    w3d_defer_flush();                                 /* the 3D back-end's deferred buildings first (draw order) */
    uint32_t camera = w3d_gwk(DRV_ENGINE);
    if (!camera || !w3d_camera_setup(camera)) { st_skip_cam++; return 0; }
    if (!w3d_be_begin(&w3d_cam)) { st_skip_target++; return 0; }
    w3d_drv_mat(camera + 0x20, &V);
    W3dState st;
    w3d_base_state(&st);
    uint32_t ras = hle_render_rs(RS_TEXTURERASTER), filt = hle_render_rs(RS_TEXTUREFILTER);
    uint32_t au = hle_render_rs(RS_TEXTUREADDRESSU), av = hle_render_rs(RS_TEXTUREADDRESSV);
    int acl = 0;
    if (!w3d_be_texture(ras, (int)filt, (int)au, (int)av, &acl)) { w3d_be_end(); st_skip_tex++; return 0; }
    st.textured = ras != 0;
    st.uscale = 1.0f;
    st.ubase = st.vbase = 0.0f;
    st.light = 0;
    st.lights = 0;
    st.blend = 1;                                     /* the vertex alpha is the layer's transparency */
    st.atest = 0;
    st.cull = 0;
    w3d_be_state(&st);
    w3d_world_set(2);                                 /* identity: the vertices are in view space */
    near_z = w3d_cam.near_z;
    if (water_opt_waves) wave_t = (float)(hle_render_flip_count() % 100000u) * 0.06f;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + ESZ * i;
        for (int q = 0; q < 4; q++) col[q] = (float)gb(e + 32 + (uint32_t)q);
        switch (type) {
        case 0: ent_flat_tri(e); break;
        case 1: ent_hd_tri(e); break;
        case 2: ent_flat_rect(e); break;
        default: ent_hd_rect(e); break;
        }
    }
    wflush();
    w3d_be_end();
    st_ent[type] += n;
    return 1;
}

HLE(003c6950)                                         /* water batch renderer (entries, n, LTM, program 1..4) */
{
    st_calls++;
    uint32_t buf = ARG(0), n = ARG(1), prog = ARG(3);
    int type = (int)prog - 1;
    if (water_opt_on && buf) {
        uint32_t t0 = w3d_plat_us();
        if (water_opt_log > logged) { logged++; log_batch(buf, n, type); }
        int ok = water_draw(buf, n, type);
        uint32_t dt = w3d_plat_us() - t0;
        us_draw += dt;
        if (dt > us_max) us_max = dt;
        if (ok) {
            st_drawn++;
            if (!water_opt_guest) { RET(1); return; }
        }
    }
    st_guest++;
    GUEST(003c6950)(c);
}

HLE(00182850)                                         /* water_render (timed; the water_tp test aid) */
{
    st_rw++;
    tp_tick();
    uint32_t t0 = w3d_plat_us();
    GUEST(00182850)(c);
    uint32_t dt = w3d_plat_us() - t0;
    us_rw += dt;
    if (dt > us_rw_max) us_rw_max = dt;
}

void hle_water_report(void)
{
    LOG("water (water=%d): %llu batches, %llu drawn (entries: flat tri %llu, hd tri %llu, flat rect %llu, hd "
        "rect %llu; %llu triangles, %llu clipped, %llu culled), %llu to the guest; skipped: %llu camera, %llu not the "
        "screen, %llu texture, %llu bad, %llu hd; CPU %.1f ms total, %.1f us per batch, max %u us\n", water_opt_on,
        (unsigned long long)st_calls, (unsigned long long)st_drawn, (unsigned long long)st_ent[0],
        (unsigned long long)st_ent[1], (unsigned long long)st_ent[2], (unsigned long long)st_ent[3],
        (unsigned long long)st_tris, (unsigned long long)st_clip, (unsigned long long)st_cull,
        (unsigned long long)st_guest, (unsigned long long)st_skip_cam, (unsigned long long)st_skip_target,
        (unsigned long long)st_skip_tex, (unsigned long long)st_skip_bad, (unsigned long long)st_skip_hd,
        (double)us_draw / 1000.0, st_calls ? (double)us_draw / (double)st_calls : 0.0, (unsigned)us_max);
    LOG("water: water_render 0x182850 %llu calls, %.1f ms total (guest + HLE draw), %.1f us per call, max %u us\n",
        (unsigned long long)st_rw, (double)us_rw / 1000.0, st_rw ? (double)us_rw / (double)st_rw : 0.0,
        (unsigned)us_rw_max);
}
