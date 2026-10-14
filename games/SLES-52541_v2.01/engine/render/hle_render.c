/* hle_render.c - trap-and-log hooks at the game's render driver boundary: camera begin/end/clear/flip, raster
 * create/destroy/lock/unlock, native texture read, texture cache upload, the four 2D draw device functions,
 * drv_set_state and the game's render-buffer flush.
 *
 * Every hook counts and logs its first calls (--render-log N, default 4; on the PSP the lines go to session.log) and
 * then runs the translated original (GUEST), so the game behaves exactly as before. On the host, --frames DIR
 * additionally captures the 2D draws (2D primitives with the current render state, render-buffer type 0 quads =
 * font glyphs) and hands them to the software rasteriser of hle_raster.c, which writes PNG frames at every n-th flip
 * (drv_raster_show); --render-trace N logs every 2D draw of flip N.
 * Signatures (the driver's standard device functions): bool f(void *pOut, void *pInOut, int32 nIn);
 * 2D draws: Line(verts, n, v1, v2), Triangle(verts, n, v1, v2, v3), Primitive(type, verts, n),
 * IndexedPrimitive(type, verts, n, u16 *indices, nIndices); vertex stride 0x40 (x, y, z +0/+4/+8, u, v +0x10/+0x14,
 * recipZ +0x18, colour floats 0..255 r, g, b, a +0x20..+0x2C - as written by the 2D sprite vertex setup 0x2B1770).
 * Render buffer 0x836DF0 (flush 0x3C50F0): count +0, entries of 40 bytes at +16, type byte +0x6810; type 0 entries
 * (written by 0x3C4930): centre x, y, z, half width, half height floats +0..+16 (screen), u1, v1, u2, v2 s16 / 4096
 * +20..+26 (the two texture corners), r, g, b, a bytes +28..+31, 0 +32, 1.0f +36 - centre/half-size fits the PCSX2
 * screenshot of the intro text (glyph positions and sizes), a corner/size reading draws the glyphs half as large.
 *
 * PSP: the same captures go to the GE back-end hle_ge.c (render=ge, the default): 2D primitives, render-buffer
 * type 0, colour clears and the flip are drawn with sceGu; raster create / texture read / unlock / destroy bump the GE
 * texture cache's upload version. Both targets measure what the translated driver originals cost (host time + virtual
 * cycles per hook, report at the end); im2d_guest=0 (PSP ps2rc.cfg) / HLE_IM2D_GUEST=0 (host environment) skips the 2D
 * draw originals (they only build PS2 DMA packets that the HLE drops). */
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_render.h"
#include "hle_w3d.h"
#include "hle_sky.h"                               /* the sky (hle_sky.c) */
#include "hle_xgame.h"                              /* speed switches (hle_fast.c) */
#include "hle_postfx.h"                            /* the PS2 colour filter (hle_postfx.c) */
#include "hle_water.h"                             /* water surfaces (hle_water.c) */
#ifdef HLE_PSP
#include "hle_ge.h"
#else
#include <stdlib.h>
#endif

/* the translated originals (renamed by the code generator) */
void GUEST_FN(drv_camera_begin)(R5900Ctx *c); void GUEST_FN(drv_camera_end)(R5900Ctx *c); void GUEST_FN(drv_camera_clear)(R5900Ctx *c);
void GUEST_FN(drv_raster_show)(R5900Ctx *c); void GUEST_FN(drv_raster_create)(R5900Ctx *c); void GUEST_FN(drv_raster_destroy)(R5900Ctx *c);
void GUEST_FN(drv_raster_lock)(R5900Ctx *c); void GUEST_FN(drv_raster_unlock)(R5900Ctx *c); void GUEST_FN(drv_texture_read)(R5900Ctx *c);
void GUEST_FN(drv_texcache_access)(R5900Ctx *c); void GUEST_FN(drv_2d_line)(R5900Ctx *c); void GUEST_FN(drv_2d_triangle)(R5900Ctx *c);
void GUEST_FN(drv_2d_prim)(R5900Ctx *c); void GUEST_FN(drv_2d_iprim)(R5900Ctx *c); void GUEST_FN(drv_set_state)(R5900Ctx *c);
void GUEST_FN(batch_flush)(R5900Ctx *c);
void GUEST_FN(drv_set_state_ext)(R5900Ctx *c);           /* the driver's extended render state */

const char *hle_opt_frames_dir;
int hle_opt_frame_every = 30, hle_opt_frame_max = 400, hle_opt_render_log = 4;
uint64_t hle_opt_render_trace;

static int rnd_on;                         /* host capture active */
static uint32_t cur_fb, screen_raster;     /* camera frame buffer being updated, raster of the last flip */
static uint32_t rs[RS_COUNT];              /* render state as last set through drv_set_state */
static uint64_t flips, frame_prims, frame_quads, frame_clears;

enum { H_CAM_BEGIN, H_CAM_END, H_CAM_CLEAR, H_SHOW, H_RAS_CREATE, H_RAS_DESTROY, H_RAS_LOCK, H_RAS_UNLOCK,
       H_TEX_READ, H_TEXCACHE, H_IM2D_LINE, H_IM2D_TRI, H_IM2D_PRIM, H_IM2D_IPRIM, H_RSTATE, H_RBUF_FLUSH, H_N };
static const struct { const char *name; uint32_t addr; } hooks[H_N] = {
    {"drv_camera_begin", HLE_ADDR(drv_camera_begin)}, {"drv_camera_end", HLE_ADDR(drv_camera_end)}, {"drv_camera_clear", HLE_ADDR(drv_camera_clear)},
    {"drv_raster_show", HLE_ADDR(drv_raster_show)}, {"drv_raster_create", HLE_ADDR(drv_raster_create)}, {"drv_raster_destroy", HLE_ADDR(drv_raster_destroy)},
    {"drv_raster_lock", HLE_ADDR(drv_raster_lock)}, {"drv_raster_unlock", HLE_ADDR(drv_raster_unlock)}, {"drv_texture_read", HLE_ADDR(drv_texture_read)},
    {"drv_texcache_access", HLE_ADDR(drv_texcache_access)}, {"drv_2d_line", HLE_ADDR(drv_2d_line)},
    {"drv_2d_triangle", HLE_ADDR(drv_2d_triangle)}, {"drv_2d_prim", HLE_ADDR(drv_2d_prim)},
    {"drv_2d_iprim", HLE_ADDR(drv_2d_iprim)}, {"drv_set_state", HLE_ADDR(drv_set_state)},
    {"batch_flush", HLE_ADDR(batch_flush)},
};
static uint64_t calls[H_N];
static int im2d_guest = 0;                 /* run the translated 2D draw originals (PSP: ps2rc.cfg im2d_guest, host: env);
                                              default 0 (their DMA is dropped), 1 = the old behaviour */

static float gf(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }
static uint32_t gw_ok(uint32_t a) { return a && a < hle_ram_end - 4 ? gw(a) : 0; }

/* count a call; nonzero for the first --render-log calls of the hook (those are logged) */
static int enter(int h) { return ++calls[h] <= (uint64_t)hle_opt_render_log; }

/* cost of the translated driver originals: host time (every call; 1 in 16 for the frequent render-state set and
   texture-cache access) and virtual cycles; a call during which the guest switched threads is not timed (its time
   would include another guest thread's work) */
static uint64_t g_us[H_N], g_cyc[H_N], g_timed[H_N], g_switched[H_N];
#define GUEST_CALL(h, addr)                                                                                         \
    do {                                                                                                            \
        int tm_ = ((h) != H_RSTATE && (h) != H_TEXCACHE) || ((uint32_t)calls[h] * 2654435761u) >> 28 == 0;          \
        uint64_t sw_ = hle_stat_switches, v0_ = hle_vtime;                                                          \
        uint32_t t0_ = tm_ ? w3d_plat_us() : 0;   /* integer us (PSP: no soft-float double per call) */          \
        GUEST_FN(addr)(c);   /* addr: the hook's name (hooks.def) */                                              \
        if (hle_stat_switches != sw_) g_switched[h]++;                                                              \
        else {                                                                                                      \
            g_cyc[h] += hle_vtime - v0_;                                                                            \
            if (tm_) { g_us[h] += (uint32_t)(w3d_plat_us() - t0_); g_timed[h]++; }                                  \
        }                                                                                                           \
    } while (0)

static void cur_state(RndState *st)
{
    st->raster = rs[RS_TEXTURERASTER];
    st->src_blend = (int)rs[RS_SRCBLEND];
    st->dst_blend = (int)rs[RS_DESTBLEND];
    st->filter = (int)rs[RS_TEXTUREFILTER];
    st->addr_u = (int)rs[RS_TEXTUREADDRESSU];
    st->addr_v = (int)rs[RS_TEXTUREADDRESSV];
    st->vertex_alpha = (int)rs[RS_VERTEXALPHAENABLE];
}

static void log_call(int h, R5900Ctx *c, const char *extra)
{
    LOG("RENDER %s #%llu (%08x): a0 %08x a1 %08x a2 %08x a3 %08x%s%s vtime %.3f thread %d\n", hooks[h].name,
        (unsigned long long)calls[h], hooks[h].addr, ARG(0), ARG(1), ARG(2), ARG(3), extra ? " " : "",
        extra ? extra : "", (double)hle_vtime / EE_HZ, hle_cur ? hle_cur->id : -1);
}

static void log_ret(int h, R5900Ctx *c)
{
    LOG("RENDER %s #%llu -> %08x\n", hooks[h].name, (unsigned long long)calls[h], (uint32_t)c->r[2].d[0]);
}

/* the common shape: count, log the first calls, run the original */
#define PASS(h, addr)                                                                                               \
    do {                                                                                                            \
        int lg_ = enter(h);                                                                                         \
        if (lg_) log_call(h, c, NULL);                                                                              \
        GUEST_CALL(h, addr);                                                                                        \
        if (lg_) log_ret(h, c);                                                                                     \
    } while (0)

#ifndef HLE_PSP
static void im2d_vertex(uint32_t base, uint32_t i, RndVtx *o)
{
    uint32_t p = base + 0x40u * i;
    o->x = gf(p);
    o->y = gf(p + 4);
    o->u = gf(p + 0x10);
    o->v = gf(p + 0x14);
    o->r = gf(p + 0x20);
    o->g = gf(p + 0x24);
    o->b = gf(p + 0x28);
    o->a = gf(p + 0x2C);
}

#define MAXV 3072
static RndVtx tv[MAXV];

/* --render-trace N: every 2D draw of flip N with its state and first vertices */
static int tracing(void) { return hle_opt_render_trace && flips + 1 == hle_opt_render_trace; }

static void trace_prim(const char *what, int type, uint32_t verts, int nv, int ni, uint32_t ra)
{
    LOG("RENDER trace flip %llu %s: type %d, %d vertices, %d indices, raster %08x (%ux%u), blend %u/%u, vertex alpha "
        "%u, filter %u, z test/write %u/%u\n", (unsigned long long)hle_opt_render_trace, what, type, nv, ni, rs[RS_TEXTURERASTER],
        gw_ok(rs[RS_TEXTURERASTER] + 0x0C), gw_ok(rs[RS_TEXTURERASTER] + 0x10), rs[RS_SRCBLEND], rs[RS_DESTBLEND],
        rs[RS_VERTEXALPHAENABLE], rs[RS_TEXTUREFILTER], rs[RS_ZTESTENABLE], rs[RS_ZWRITEENABLE]);
    for (int i = 0; i < nv && i < 4 && verts < hle_ram_end - 0x100; i++) {
        uint32_t p = verts + 0x40u * (uint32_t)i;
        LOG("    v%d: xyz %g %g %g uv %g %g rgba %g %g %g %g (caller ra %08x)\n", i, gf(p), gf(p + 4), gf(p + 8),
            gf(p + 0x10), gf(p + 0x14), gf(p + 0x20), gf(p + 0x24), gf(p + 0x28), gf(p + 0x2C), ra);
    }
}

/* a 2D primitive (primitive type 1 line list, 2 polyline, 3 tri list, 4 strip, 5 fan) as independent tris/lines */
static void im2d_emit(int type, uint32_t verts, int nv, uint32_t idx, int ni)
{
    int n = idx ? ni : nv, k = 0;
    if (n <= 0 || n > 65536 || nv <= 0 || verts >= hle_ram_end) return;
#define VI(j) (idx ? (uint32_t)gh(idx + 2u * (uint32_t)(j)) : (uint32_t)(j))
    RndState st;
    cur_state(&st);
    if (type == 1 || type == 2) {
        int step = type == 1 ? 2 : 1;
        for (int j = 0; j + 1 < n; j += step) {
            if (k + 2 > MAXV) { hle_raster_lines(tv, k, &st); k = 0; }
            im2d_vertex(verts, VI(j), &tv[k++]);
            im2d_vertex(verts, VI(j + 1), &tv[k++]);
        }
        if (k) hle_raster_lines(tv, k, &st);
        return;
    }
    for (int j = 0; j + 2 < n; j += type == 3 ? 3 : 1) {
        uint32_t a, b, d;
        if (type == 3 || type == 4) { a = VI(j); b = VI(j + 1); d = VI(j + 2); }
        else if (type == 5) { a = VI(0); b = VI(j + 1); d = VI(j + 2); }
        else return;
        if (k + 3 > MAXV) { hle_raster_tris(tv, k, &st); k = 0; }
        im2d_vertex(verts, a, &tv[k++]);
        im2d_vertex(verts, b, &tv[k++]);
        im2d_vertex(verts, d, &tv[k++]);
    }
    if (k) hle_raster_tris(tv, k, &st);
#undef VI
}

/* render buffer type 0 (batch renderer 0x3C4490 with a3 = 1): screen-space quads, 40-byte entries */
static void rbuf_emit(uint32_t buf, uint32_t n)
{
    RndState st;
    cur_state(&st);
    int k = 0;
    static int diag = 2;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + 16 + 40 * i;
        /* centre (+0, +4) and half extents (+12, +16), like the corona sprites this type is also used for */
        float x1 = gf(e) - gf(e + 12), y1 = gf(e + 4) - gf(e + 16), x2 = gf(e) + gf(e + 12), y2 = gf(e + 4) + gf(e + 16);
        float u1 = (int16_t)gh(e + 20) / 4096.0f, v1 = (int16_t)gh(e + 22) / 4096.0f;
        float u2 = (int16_t)gh(e + 24) / 4096.0f, v2 = (int16_t)gh(e + 26) / 4096.0f;
        float r = gb(e + 28), g = gb(e + 29), b = gb(e + 30), a = gb(e + 31);
        if (diag > 0 && i < 2) {
            if (i == 1) diag--;
            LOG("RENDER buffer entry %u/%u: (%g, %g)-(%g, %g) z %g uv (%g, %g)-(%g, %g) rgba %g %g %g %g raster %08x "
                "blend %u/%u filter %u\n", i, n, x1, y1, x2, y2, gf(e + 8), u1, v1, u2, v2, r, g, b, a, st.raster,
                rs[RS_SRCBLEND], rs[RS_DESTBLEND], rs[RS_TEXTUREFILTER]);
        }
        RndVtx q[4] = {{x1, y1, u1, v1, r, g, b, a}, {x2, y1, u2, v1, r, g, b, a}, {x2, y2, u2, v2, r, g, b, a},
                       {x1, y2, u1, v2, r, g, b, a}};
        static const int tri[6] = {0, 1, 2, 0, 2, 3};
        if (k + 6 > MAXV) { hle_raster_tris(tv, k, &st); k = 0; }
        for (int j = 0; j < 6; j++) tv[k++] = q[tri[j]];
    }
    if (k) hle_raster_tris(tv, k, &st);
}
#endif

/* ---- camera: begin/end update, clear, flip */
HLE_FN(drv_camera_begin)
{
    uint32_t cam = ARG(1), fb = gw_ok(cam + 0x60);
    cur_fb = fb;
    int lg = enter(H_CAM_BEGIN);
    if (lg) {
        char x[96];
        snprintf(x, sizeof x, "(camera %08x, frame buffer %08x %ux%u)", cam, fb, gw_ok(fb + 0x0C), gw_ok(fb + 0x10));
        log_call(H_CAM_BEGIN, c, x);
    }
#ifndef HLE_PSP
    if (rnd_on && fb) hle_raster_set_screen((int)gw_ok(fb + 0x0C), (int)gw_ok(fb + 0x10));
#else
    hle_ge_camera_begin(cam, fb);
#endif
    GUEST_CALL(H_CAM_BEGIN, drv_camera_begin);
    if (lg) log_ret(H_CAM_BEGIN, c);
}

HLE_FN(drv_camera_end) { PASS(H_CAM_END, drv_camera_end); }

HLE_FN(drv_camera_clear)
{
    uint32_t col = ARG(1);
    uint8_t rgba[4] = {0, 0, 0, 0};
    if (col && col < hle_ram_end - 4)
        for (int i = 0; i < 4; i++) rgba[i] = gb(col + (uint32_t)i);
    int lg = enter(H_CAM_CLEAR);
    if (lg) {
        char x[64];
        snprintf(x, sizeof x, "(rgba %02x%02x%02x%02x mode %d)", rgba[0], rgba[1], rgba[2], rgba[3], SARG(2));
        log_call(H_CAM_CLEAR, c, x);
    }
    frame_clears++;
#ifndef HLE_PSP
    if (rnd_on) {
        int md = SARG(2);
        if ((md & 2) && w3d_opt_on && w3d_opt_sky && hle_render_screen_raster() &&     /* + clear_fill */
            (!hle_render_cur_fb() || hle_render_cur_fb() == hle_render_screen_raster()) && hle_sky_clear_fill(md)) {
            uint32_t fc = hle_render_rs(RS_FOGCOLOR);           /* as psp/hle_ge.c (w3d_sky) */
            for (int i = 0; i < 4; i++) rgba[i] = (uint8_t)(fc >> (8 * i));
            rgba[3] = 0xFF;
            md |= 1;
        }
        hle_raster_clear(rgba, md);
    }
#else
    hle_ge_clear(ARG(0), rgba, SARG(2));
#endif
    GUEST_CALL(H_CAM_CLEAR, drv_camera_clear);
    if (lg) log_ret(H_CAM_CLEAR, c);
}

HLE_FN(drv_raster_show)
{
    int lg = enter(H_SHOW);
    if (lg) log_call(H_SHOW, c, NULL);
    flips++;
    if (ARG(0)) screen_raster = ARG(0);
    int wrote = 0;
#ifndef HLE_PSP
    if (rnd_on) {
        char tag[48];
        snprintf(tag, sizeof tag, "thread %d", hle_cur ? hle_cur->id : -1);
        wrote = hle_raster_frame_end(flips, tag);
    }
#else
    hle_ge_flip(ARG(0));
#endif
    if (flips <= 4 || wrote || (flips & 1023) == 0)
        LOG("RENDER flip %llu vtime %.3f thread %d: %llu 2D primitives, %llu render-buffer quads, %llu clears%s\n",
            (unsigned long long)flips, (double)hle_vtime / EE_HZ, hle_cur ? hle_cur->id : -1,
            (unsigned long long)frame_prims, (unsigned long long)frame_quads, (unsigned long long)frame_clears,
            wrote ? " (PNG written)" : "");
    frame_prims = frame_quads = frame_clears = 0;
    w3d_frame_end();                                       /* the 3D vertex cache's LRU frame */
    GUEST_CALL(H_SHOW, drv_raster_show);
    if (lg) log_ret(H_SHOW, c);
}

/* ---- rasters and textures */
HLE_FN(drv_raster_create)
{
    uint32_t r = ARG(1);
    int lg = enter(H_RAS_CREATE);
    if (lg) {
        char x[96];
        snprintf(x, sizeof x, "(raster %08x %ux%u depth %u flags %08x)", r, gw_ok(r + 0x0C), gw_ok(r + 0x10),
                 gw_ok(r + 0x14), ARG(2));
        log_call(H_RAS_CREATE, c, x);
    }
    GUEST_CALL(H_RAS_CREATE, drv_raster_create);
#ifdef HLE_PSP
    hle_ge_raster_changed(r);
#endif
    if (lg) log_ret(H_RAS_CREATE, c);
}

HLE_FN(drv_raster_destroy)
{
#ifndef HLE_PSP
    if (rnd_on) hle_raster_forget(ARG(1));
#else
    hle_ge_raster_changed(ARG(1));
#endif
    PASS(H_RAS_DESTROY, drv_raster_destroy);
}

HLE_FN(drv_raster_lock) { PASS(H_RAS_LOCK, drv_raster_lock); }

HLE_FN(drv_raster_unlock)
{
    uint32_t r = ARG(1);
    PASS(H_RAS_UNLOCK, drv_raster_unlock);
#ifdef HLE_PSP
    hle_ge_raster_changed(r);                              /* the game may have rewritten the texels */
#else
    (void)r;
#endif
}

HLE_FN(drv_texture_read)
{
    uint32_t out = ARG(1);
    int lg = enter(H_TEX_READ);
    if (lg) log_call(H_TEX_READ, c, NULL);
    GUEST_CALL(H_TEX_READ, drv_texture_read);
    uint32_t tex = gw_ok(out), ras = gw_ok(tex);
#ifdef HLE_PSP
    if (ras) hle_ge_raster_changed(ras);
#endif
    if (tex) {
        char name[33];
        for (int i = 0; i < 32; i++) name[i] = (char)gb(tex + 0x10 + (uint32_t)i);
        name[32] = 0;
        if (lg)
            LOG("RENDER drv_texture_read #%llu -> %08x: texture %08x \"%s\" raster %08x %ux%u depth %u format %02x\n",
                (unsigned long long)calls[H_TEX_READ], (uint32_t)c->r[2].d[0], tex, name, ras, gw_ok(ras + 0x0C),
                gw_ok(ras + 0x10), gw_ok(ras + 0x14), ras ? gb(ras + 0x23) : 0);
#ifndef HLE_PSP
        if (rnd_on && ras) hle_raster_texture_name(ras, name);
#endif
    } else if (lg)
        log_ret(H_TEX_READ, c);
}

HLE_FN(drv_texcache_access) { PASS(H_TEXCACHE, drv_texcache_access); }

/* ---- 2D draws: drawn by the capture (host) / the GE (PSP), then the translated original unless im2d_guest is 0 (it
   returns TRUE; it builds VU1 DMA packets the HLE drops) */
#define IM2D_GUEST(h, addr)                                                                                         \
    do {                                                                                                            \
        if (im2d_guest) PASS(h, addr);                                                                              \
        else { enter(h); RET(1); }                                                                                  \
    } while (0)

HLE_FN(drv_2d_line)
{
    frame_prims++;
#ifndef HLE_PSP
    if (rnd_on) {
        RndState st;
        cur_state(&st);
        RndVtx l[2];
        im2d_vertex(ARG(0), ARG(2), &l[0]);
        im2d_vertex(ARG(0), ARG(3), &l[1]);
        hle_raster_lines(l, 2, &st);
    }
#else
    if (hle_ge_on) {
        RndState st;
        cur_state(&st);
        uint16_t ix[2] = {(uint16_t)ARG(2), (uint16_t)ARG(3)};
        hle_ge_im2d(1, ARG(0), SARG(1), 0, ix, 2, &st);
    }
#endif
    IM2D_GUEST(H_IM2D_LINE, drv_2d_line);
}

HLE_FN(drv_2d_triangle)
{
    frame_prims++;
#ifndef HLE_PSP
    if (rnd_on) {
        RndState st;
        cur_state(&st);
        RndVtx t[3];
        im2d_vertex(ARG(0), ARG(2), &t[0]);
        im2d_vertex(ARG(0), ARG(3), &t[1]);
        im2d_vertex(ARG(0), ARG(4), &t[2]);
        hle_raster_tris(t, 3, &st);
    }
#else
    if (hle_ge_on) {
        RndState st;
        cur_state(&st);
        uint16_t ix[3] = {(uint16_t)ARG(2), (uint16_t)ARG(3), (uint16_t)ARG(4)};
        hle_ge_im2d(3, ARG(0), SARG(1), 0, ix, 3, &st);
    }
#endif
    IM2D_GUEST(H_IM2D_TRI, drv_2d_triangle);
}

HLE_FN(drv_2d_prim)
{
    int lg = enter(H_IM2D_PRIM);
    if (lg) {
        char x[64];
        snprintf(x, sizeof x, "(type %d, %d vertices, raster %08x)", SARG(0), SARG(2), rs[RS_TEXTURERASTER]);
        log_call(H_IM2D_PRIM, c, x);
    }
    frame_prims++;
#ifndef HLE_PSP
    if (tracing()) trace_prim("2D primitive", SARG(0), ARG(1), SARG(2), 0, (uint32_t)c->r[31].d[0]);
    if (rnd_on) im2d_emit(SARG(0), ARG(1), SARG(2), 0, 0);
#else
    if (hle_ge_on) {
        RndState st;
        cur_state(&st);
        hle_ge_im2d(SARG(0), ARG(1), SARG(2), 0, NULL, 0, &st);
    }
#endif
    if (im2d_guest) GUEST_CALL(H_IM2D_PRIM, drv_2d_prim);
    else RET(1);
    if (lg) log_ret(H_IM2D_PRIM, c);
}

HLE_FN(drv_2d_iprim)
{
    int lg = enter(H_IM2D_IPRIM);
    if (lg) {
        char x[80];
        snprintf(x, sizeof x, "(type %d, %d vertices, %d indices, raster %08x)", SARG(0), SARG(2), SARG(4),
                 rs[RS_TEXTURERASTER]);
        log_call(H_IM2D_IPRIM, c, x);
    }
    frame_prims++;
#ifndef HLE_PSP
    if (tracing()) trace_prim("2D indexed", SARG(0), ARG(1), SARG(2), SARG(4), (uint32_t)c->r[31].d[0]);
    if (rnd_on) im2d_emit(SARG(0), ARG(1), SARG(2), ARG(3), SARG(4));
#else
    if (hle_ge_on) {
        RndState st;
        cur_state(&st);
        hle_ge_im2d(SARG(0), ARG(1), SARG(2), ARG(3), NULL, SARG(4), &st);
    }
#endif
    if (im2d_guest) GUEST_CALL(H_IM2D_IPRIM, drv_2d_iprim);
    else RET(1);
    if (lg) log_ret(H_IM2D_IPRIM, c);
}

/* ---- render state */
/* optional native render-state paths (x_rs / x_tex): a separate module that is not part of every build; without
 * it every render-state set runs the translated original */
int hle_rsdev_state(R5900Ctx *c, uint32_t s, uint32_t v, void (*orig)(R5900Ctx *)) __attribute__((weak));
int hle_rsdev_state_ext(R5900Ctx *c, void (*orig)(R5900Ctx *)) __attribute__((weak));
void hle_rsdev_report(void) __attribute__((weak));
static void rs_orig(R5900Ctx *c) { PASS(H_RSTATE, drv_set_state); }
/* the public native render-state module (hle_rstate.c, cfg x_rsn); runs when the optional module did not handle */
int hle_rstate_set(R5900Ctx *c, uint32_t s, uint32_t v, void (*orig)(R5900Ctx *));
int hle_rstate_set_ext(R5900Ctx *c, void (*orig)(R5900Ctx *));
void hle_rstate_report(void);

HLE_FN(drv_set_state)
{
    uint32_t s = ARG(0), v = ARG(1);
    if (s < RS_COUNT) {
        rs[s] = v;
        if (s == RS_TEXTUREADDRESS) rs[RS_TEXTUREADDRESSU] = rs[RS_TEXTUREADDRESSV] = v;
    }
    int h = hle_rsdev_state ? hle_rsdev_state(c, s, v, rs_orig) : 0;
    if (h == 0) h = hle_rstate_set(c, s, v, rs_orig);
    if (h == 1) { enter(H_RSTATE); return; }
    if (h == 0) PASS(H_RSTATE, drv_set_state);
}

static void rse_orig(R5900Ctx *c) { GUEST_FN(drv_set_state_ext)(c); }

HLE_FN(drv_set_state_ext)
{
    if (!(hle_rsdev_state_ext && hle_rsdev_state_ext(c, rse_orig)) && !hle_rstate_set_ext(c, rse_orig))
        GUEST_FN(drv_set_state_ext)(c);
}

/* ---- the game's render buffer (batched primitives): capture type 0 before the original flushes and empties it.
   Types 0 and 1 share the batch renderer 0x3C4490 and the entry size; type 0 passes a3 = 1 (screen space: the font
   glyphs), type 1 a3 = 0 (clouds 0x2ED464, coronas 0x297A78: positions the renderer transforms - drawing them as
   screen quads with their ONE/ONE blend whitened the intro cutscene), so only type 0 is drawn. */
HLE_FN(batch_flush)
{
    uint32_t buf = ARG(0), n = gw_ok(buf);
    int type = buf && buf < hle_ram_end - 0x6810 ? gb(buf + 0x6810) : -1;
    int lg = enter(H_RBUF_FLUSH);
    if (lg) {
        char x[48];
        snprintf(x, sizeof x, "(type %d, %u entries)", type, n);
        log_call(H_RBUF_FLUSH, c, x);
    }
#ifndef HLE_PSP
    if (tracing())
        LOG("RENDER trace flip %llu render buffer: type %d, %u entries, raster %08x, blend %u/%u (caller ra %08x)\n",
            (unsigned long long)hle_opt_render_trace, type, n, rs[RS_TEXTURERASTER], rs[RS_SRCBLEND],
            rs[RS_DESTBLEND], (uint32_t)c->r[31].d[0]);
#endif
    hle_sky_rbuf(buf, n, type);                            /* render buffer types 1 / 2 (world-space sprites) */
    if (type == 0 && n && n <= 256) {
        frame_quads += n;
#ifndef HLE_PSP
        if (rnd_on) rbuf_emit(buf, n);
#else
        if (hle_ge_on) {
            RndState st;
            cur_state(&st);
            hle_ge_rbuf(buf, n, &st);
        }
#endif
    }
    /* ps2rc.cfg x_rbuf=1 / HLE_X_RBUF=1: the original only runs one of 10 batch renderers (0x3C4490 / 0x58E9B0 /
       0x4ADCE0 / 0x58DDD0 / 0x4AD0E0 / 0x4B1B20 / 0x5A28C0: each executes a VU1 pipeline whose packets the HLE drops;
       the render state they touch is saved and restored inside) and then empties the buffer: count (+0) = 0 when it
       was > 0, v0 = the old count. Skipped, only that remains - the GE draws what it draws above either way. Guest
       effect: no VU1 packets / scratchpad DMA from the flush (the virtual time and the driver's texture-cache LRU
       differ slightly - a timeline change like skipping the mesh node's original). */
    static int x_rbuf = -1;
    if (x_rbuf < 0) x_rbuf = hle_x_opt("rbuf", 1) != 0;          /* default on */
    if (x_rbuf && buf && buf < hle_ram_end - 4) {
        if ((int32_t)n > 0) sw_(buf, 0);
        RET(n);
        if (lg) log_ret(H_RBUF_FLUSH, c);
        return;
    }
    GUEST_CALL(H_RBUF_FLUSH, batch_flush);
    if (lg) log_ret(H_RBUF_FLUSH, c);
}

uint32_t hle_render_rs(int s) { return s >= 0 && s < RS_COUNT ? rs[s] : 0; }
uint64_t hle_render_flip_count(void) { return flips; }
uint32_t hle_render_cur_fb(void) { return cur_fb; }
uint32_t hle_render_screen_raster(void) { return screen_raster; }

void hle_render_init(void)
{
#ifndef HLE_PSP
    if (hle_opt_frames_dir) {
        rnd_on = hle_raster_open(hle_opt_frames_dir, hle_opt_frame_every, hle_opt_frame_max);
        LOG("render capture: %s (frames to %s, every %d flips, at most %d)\n", rnd_on ? "on" : "FAILED to start",
            hle_opt_frames_dir, hle_opt_frame_every, hle_opt_frame_max);
    }
    const char *e = getenv("HLE_IM2D_GUEST");
    if (e) im2d_guest = atoi(e) != 0;
#else
    hle_ge_init();                                         /* ps2rc.cfg render=ge|text, ge_*, im2d_guest */
    im2d_guest = hle_ge_guest_im2d;
#endif
    if (!im2d_guest) LOG("render: the translated 2D draw originals are SKIPPED (im2d_guest=0)\n");
    hle_w3d_init();                                        /* the 3D world (mesh_node_body hook, hle_w3d.c) */
    hle_sky_init();                                        /* the sky (buffer_render hook, hle_sky.c) */
    hle_postfx_init();                                     /* the PS2 colour filter (hle_postfx.c) */
    hle_water_init();                                      /* water surfaces (hle_water.c) */
    hle_x_report();                                        /* the x_* speed switches (hle_fast.c) */
    hle_cfg_report();                                      /* "settings: ..." + the cfg key table */
}

void hle_render_report(void)
{
    hle_w3d_report();
    hle_sky_report();
    hle_postfx_report();
    hle_water_report();
    if (hle_rsdev_report) hle_rsdev_report();
    hle_rstate_report();                                   /* x_rsn native render state (hle_rstate.c) */
    if (hle_farcol_report) hle_farcol_report();            /* x_farcol (a developer module, when present) */
    hle_ag_report();                                       /* x_ag* game-logic cuts (hle_ag.c) */
    hle_far_report();                                      /* far-entity policy (hle_far.c) */
    LOG("render boundary (driver hooks): %llu flips\n", (unsigned long long)flips);
    for (int h = 0; h < H_N; h++)
        if (calls[h]) LOG("  %-30s %08x %12llu calls\n", hooks[h].name, hooks[h].addr, (unsigned long long)calls[h]);
    /* what the translated originals cost (the HLE drops their DMA packets): host time, sampled 1/16 for the render
       state and the texture cache, scaled up; virtual cycles of every call without a guest thread switch inside */
    double tot_ms = 0;
    uint64_t tot_cyc = 0;
    for (int h = 0; h < H_N; h++) {
        if (!g_timed[h] && !g_cyc[h]) continue;
        uint64_t ncalls = g_timed[h] ? calls[h] - g_switched[h] : 0;
        double ms = g_timed[h] ? (double)g_us[h] / 1000.0 * (double)ncalls / (double)g_timed[h] : 0.0;
        tot_ms += ms;
        tot_cyc += g_cyc[h];
        LOG("  guest original %-30s %10.1f ms host (%.2f us/call, %llu timed), %8.3f s virtual, %llu calls with a "
            "thread switch inside\n", hooks[h].name, ms, g_timed[h] ? (double)g_us[h] / (double)g_timed[h] : 0.0,
            (unsigned long long)g_timed[h], (double)g_cyc[h] / EE_HZ, (unsigned long long)g_switched[h]);
    }
    LOG("render boundary: the translated driver originals took %.1f ms host time and %.3f s virtual time in total "
        "(im2d_guest=%d)\n", tot_ms, (double)tot_cyc / EE_HZ, im2d_guest);
#ifndef HLE_PSP
    if (rnd_on) hle_raster_close();
#else
    hle_ge_report();
#endif
}
