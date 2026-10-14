/* hle_raster.c - host only: a software 2D rasteriser for the draws captured at the render driver boundary
 * (hle_render.c) and a decoder of the PS2 native textures (the driver's PS2 rasters), writing PNG frames (zlib).
 *
 * Textures: a PS2 raster created by drv_texture_read keeps the GIF transfer packets of its texture dictionary in
 * memory ("new style" layout): per mip level GIFtag(A+D, NLOOP 3: TRXPOS, TRXREG, TRXDIR) + GIFtag(IMAGE) + data,
 * then the palette packet; the raster's pixel pointer (+0x04) points at the mip-0 image data (0x50 bytes after its
 * A+D tag) and its palette pointer (+0x08) at the palette data - both are checked against their IMAGE tags before use.
 * 4/8-bit textures are stored swizzled (uploaded as PSMCT16/PSMCT32 with halved dimensions); the closed-form address
 * mapping and the CSM1 palette order are adapted from an MIT-licensed library (see NOTICE.md) and were validated on
 * all 26k textures of the game's PS2 model archive. Alpha 0..0x80 is scaled to 0..255.
 * Shading: colour = texel * vertex / 255 (GS MODULATE with the VU's 255 -> 0x80 vertex scaling), alpha the same;
 * the driver's blend factors (ZERO..INVDESTCOLOR) on an RGBA frame buffer; pixel centres at +0.5; bilinear for the
 * linear filter modes, else nearest; wrap / mirror / clamp addressing. Z is not modelled (2D only). */
#ifndef HLE_PSP
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zlib.h>
#include "hle.h"
#include "hle_render.h"
#include "hle_postfx.h"                 /* hle_raster_postfx_mul */
#include "hle_w3d.h"                    /* w3d_defer_flush before every other draw */

#define FB_MAX_W 1024
#define FB_MAX_H 1024
static uint8_t fb[FB_MAX_W * FB_MAX_H * 4];
static uint16_t zbuf[FB_MAX_W * FB_MAX_H];       /* the 3D world's depth (GE model, 65535 = near) */
static int scr_w = 640, scr_h = 448, opened, every = 30, max_frames = 400, written;
static char out_dir[1024];
static FILE *index_f;
static uint64_t last_hash;
static uint64_t n_tris, n_lines, n_pixels, n_tex_decoded, n_tex_failed, n_skipped;

/* ------------------------------------------------------------------ textures */
typedef struct {
    uint32_t raster, pix, pal, opix;
    int w, h, ok;
    uint8_t *rgba;
    char name[33];
} Tex;
#define TEX_MAX 4096
static Tex texs[TEX_MAX];
static int ntex;
static size_t tex_bytes;
static int diag_left = 12;              /* texture decode diagnostics in the log */

static uint64_t gq(uint32_t a) { return (uint64_t)gw(a) | (uint64_t)gw(a + 4) << 32; }

/* bytes of IMAGE data announced by the GIF tag 16 bytes before p (0 if that is no IMAGE tag) */
static uint32_t image_bytes(uint32_t p)
{
    if (p < 0x100000u + 16 || p >= hle_ram_end) return 0;
    uint64_t tag = gq(p - 16);
    if (((tag >> 58) & 3) != 2) return 0;
    uint32_t nloop = (uint32_t)(tag & 0x7FFF);
    if (!nloop) nloop = 0x8000;
    return nloop * 16;
}

/* GS transfer address: unit index (bytes for PSMT8, nibbles for PSMT4) of texel (x, y), w = stored stride */
static uint32_t swz(int x, int y, int w)
{
    int xx = x ^ ((((y >> 1) ^ (y >> 2)) & 1) << 2);
    int nx = (xx & 7) | ((x >> 1) & ~7);
    int ny = (y & 1) | ((y >> 1) & ~1);
    int lane = ((y >> 1) & 1) | (((x >> 3) & 1) << 1);
    return (uint32_t)(lane | (nx << 2) | ny * 2 * w);
}

static void clut_entry(uint32_t pal, int fmt, int i, uint8_t *o)
{
    if (fmt == 1 || fmt == 0xA) {                         /* 1555 / 555: 16-bit entries */
        uint16_t v = gh(pal + 2u * (uint32_t)i);
        o[0] = (uint8_t)((v & 31) << 3);
        o[1] = (uint8_t)(((v >> 5) & 31) << 3);
        o[2] = (uint8_t)(((v >> 10) & 31) << 3);
        o[3] = fmt == 0xA || (v & 0x8000) ? 255 : 0;
    } else {                                              /* 8888 / 888: 32-bit entries */
        uint32_t v = gw(pal + 4u * (uint32_t)i);
        o[0] = (uint8_t)v;
        o[1] = (uint8_t)(v >> 8);
        o[2] = (uint8_t)(v >> 16);
        unsigned a = (v >> 24) * 2;
        o[3] = fmt == 6 ? 255 : (uint8_t)(a > 255 ? 255 : a);
    }
}

static int decode(Tex *t)
{
    uint32_t r = t->raster;
    int w = (int)gw(r + 0x0C), h = (int)gw(r + 0x10), depth = (int)gw(r + 0x14);
    int fmt = gb(r + 0x23) & 0xF, pal8 = gb(r + 0x23) & 0x20, pal4 = gb(r + 0x23) & 0x40;   /* raster format PAL8 0x2000, PAL4 0x4000 */
    uint32_t pix = gw(r + 0x04), pal = gw(r + 0x08);
    uint32_t pbytes = image_bytes(pix), cbytes = pal ? image_bytes(pal) : 0;
    if (diag_left > 0) {
        diag_left--;
        LOG("RENDER texture %08x \"%s\": %dx%d depth %d format %02x pixels %08x (IMAGE %u bytes) palette %08x "
            "(IMAGE %u bytes) original pixels %08x\n", r, t->name, w, h, depth, gb(r + 0x23), pix, pbytes, pal, cbytes,
            gw(r + 0x24));
    }
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || !pbytes) return 0;
    uint8_t *o = malloc((size_t)w * h * 4);
    if (!o) return 0;
    if (depth == 8 || depth == 4 || pal8 || pal4) {
        int n = depth == 4 || pal4 ? 16 : 256;
        uint8_t clut[256][4];
        if (cbytes < (uint32_t)n * (fmt == 1 || fmt == 0xA ? 2u : 4u)) { free(o); return 0; }
        for (int i = 0; i < n; i++) {
            int j = n == 256 ? (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1) : i;   /* CSM1 order */
            clut_entry(pal, fmt, j, clut[i]);
        }
        int ww = n == 256 ? (w > 16 ? w : 16) : (w > 32 ? w : 32);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t a = swz(x, y, ww);
                int idx;
                if (n == 256) idx = a < pbytes ? gb(pix + a) : 0;
                else {
                    uint32_t b = a >> 1;
                    idx = b < pbytes ? (a & 1 ? gb(pix + b) >> 4 : gb(pix + b) & 15) : 0;
                }
                memcpy(o + ((size_t)y * w + x) * 4, clut[idx], 4);
            }
    } else if (depth == 32 || depth == 24) {
        if (pbytes < (uint32_t)(w * h * 4)) { free(o); return 0; }
        for (int i = 0; i < w * h; i++) {
            uint32_t v = gw(pix + 4u * (uint32_t)i);
            unsigned a = (v >> 24) * 2;
            o[4 * i] = (uint8_t)v;
            o[4 * i + 1] = (uint8_t)(v >> 8);
            o[4 * i + 2] = (uint8_t)(v >> 16);
            o[4 * i + 3] = fmt == 6 ? 255 : (uint8_t)(a > 255 ? 255 : a);
        }
    } else if (depth == 16) {
        if (pbytes < (uint32_t)(w * h * 2)) { free(o); return 0; }
        for (int i = 0; i < w * h; i++) {
            uint16_t v = gh(pix + 2u * (uint32_t)i);
            o[4 * i] = (uint8_t)((v & 31) << 3);
            o[4 * i + 1] = (uint8_t)(((v >> 5) & 31) << 3);
            o[4 * i + 2] = (uint8_t)(((v >> 10) & 31) << 3);
            o[4 * i + 3] = fmt == 0xA || (v & 0x8000) ? 255 : 0;
        }
    } else {
        free(o);
        return 0;
    }
    t->w = w;
    t->h = h;
    t->rgba = o;
    tex_bytes += (size_t)w * h * 4;
    return 1;
}

static void dump_texture(const Tex *t);

static Tex *tex_find(uint32_t raster, int create)
{
    for (int i = 0; i < ntex; i++)
        if (texs[i].raster == raster) return &texs[i];
    if (!create) return NULL;
    if (ntex == TEX_MAX || tex_bytes > (256u << 20)) {      /* simple full flush */
        for (int i = 0; i < ntex; i++) free(texs[i].rgba);
        ntex = 0;
        tex_bytes = 0;
    }
    Tex *t = &texs[ntex++];
    memset(t, 0, sizeof *t);
    t->raster = raster;
    return t;
}

static Tex *tex_get(uint32_t raster)
{
    if (!raster || raster >= hle_ram_end - 0x40) return NULL;
    Tex *t = tex_find(raster, 1);
    uint32_t pix = gw(raster + 0x04), pal = gw(raster + 0x08), opix = gw(raster + 0x24);
    if (t->ok && (t->pix != pix || t->pal != pal || t->opix != opix)) {   /* moved (defragmenter): decode again */
        free(t->rgba);
        t->rgba = NULL;
        t->ok = 0;
    }
    if (!t->ok) {
        t->pix = pix;
        t->pal = pal;
        t->opix = opix;
        if (decode(t)) { t->ok = 1; n_tex_decoded++; dump_texture(t); }
        else { t->ok = -1; n_tex_failed++; }
    }
    return t->ok > 0 ? t : NULL;
}

/* a raster was destroyed (its address may be reused by a different texture) or a texture was (re)read */
void hle_raster_forget(uint32_t raster)
{
    Tex *t = tex_find(raster, 0);
    if (!t) return;
    free(t->rgba);
    t->rgba = NULL;
    t->ok = 0;
    t->name[0] = 0;
}

void hle_raster_texture_name(uint32_t raster, const char *name)
{
    Tex *t = tex_find(raster, 1);
    free(t->rgba);
    t->rgba = NULL;
    t->ok = 0;
    snprintf(t->name, sizeof t->name, "%s", name);
}

static void sample(const Tex *t, float u, float v, int filter, int au, int av, float *o)
{
    if (filter == 2 || filter == 4 || filter == 6) {      /* LINEAR variants: bilinear */
        float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
        int x0 = (int)floorf(x), y0 = (int)floorf(y);
        float fx = x - x0, fy = y - y0;
        float s[4][4];
        for (int k = 0; k < 4; k++) {
            int xi = x0 + (k & 1), yi = y0 + (k >> 1);
#define ADDR(c, n, m) do { if ((m) == 3 || (m) == 4) c = c < 0 ? 0 : c >= (n) ? (n) - 1 : c; \
                           else if ((m) == 2) { int p = (c % (2 * (n)) + 2 * (n)) % (2 * (n)); c = p < (n) ? p : 2 * (n) - 1 - p; } \
                           else c = ((c % (n)) + (n)) % (n); } while (0)
            ADDR(xi, t->w, au);
            ADDR(yi, t->h, av);
            const uint8_t *p = t->rgba + ((size_t)yi * t->w + xi) * 4;
            for (int c = 0; c < 4; c++) s[k][c] = p[c];
        }
        for (int c = 0; c < 4; c++)
            o[c] = (s[0][c] * (1 - fx) + s[1][c] * fx) * (1 - fy) + (s[2][c] * (1 - fx) + s[3][c] * fx) * fy;
    } else {
        int xi = (int)floorf(u * t->w), yi = (int)floorf(v * t->h);
        ADDR(xi, t->w, au);
        ADDR(yi, t->h, av);
#undef ADDR
        const uint8_t *p = t->rgba + ((size_t)yi * t->w + xi) * 4;
        for (int c = 0; c < 4; c++) o[c] = p[c];
    }
}

/* ------------------------------------------------------------------ pixels */
static float factor(int f, const float *s, const float *d, int c)
{
    switch (f) {
    case 1: return 0.0f;                                  /* ZERO */
    case 2: return 1.0f;                                  /* ONE */
    case 3: return s[c] / 255.0f;                         /* SRCCOLOR */
    case 4: return 1.0f - s[c] / 255.0f;                  /* INVSRCCOLOR */
    case 5: return s[3] / 255.0f;                         /* SRCALPHA */
    case 6: return 1.0f - s[3] / 255.0f;                  /* INVSRCALPHA */
    case 7: return d[3] / 255.0f;                         /* DESTALPHA */
    case 8: return 1.0f - d[3] / 255.0f;                  /* INVDESTALPHA */
    case 9: return d[c] / 255.0f;                         /* DESTCOLOR */
    case 10: return 1.0f - d[c] / 255.0f;                 /* INVDESTCOLOR */
    case 11: { float a = s[3] / 255.0f, b = 1.0f - d[3] / 255.0f; return a < b ? a : b; }   /* SRCALPHASAT */
    default: return -1.0f;
    }
}

static void put(int x, int y, const float *src, const RndState *st)
{
    uint8_t *p = fb + ((size_t)y * scr_w + x) * 4;
    float d[4] = {p[0], p[1], p[2], p[3]}, o[4];
    int sb = st->src_blend ? st->src_blend : 5, db = st->dst_blend ? st->dst_blend : 6;
    if (src[3] <= 0.0f && sb == 5) return;                /* alpha test (the driver tests alpha > 0 on alpha formats) */
    for (int c = 0; c < 4; c++) {
        float fs = factor(sb, src, d, c), fd = factor(db, src, d, c);
        if (fs < 0) fs = 1;
        if (fd < 0) fd = 0;
        o[c] = src[c] * fs + d[c] * fd;
    }
    for (int c = 0; c < 4; c++) p[c] = (uint8_t)(o[c] < 0 ? 0 : o[c] > 255 ? 255 : o[c] + 0.5f);
    n_pixels++;
}

static void shade(const RndVtx *a, const Tex *t, const RndState *st, float u, float v, const float *vc, float *out)
{
    (void)a;
    if (t) {
        float s[4];
        sample(t, u, v, st->filter, st->addr_u, st->addr_v, s);
        for (int c = 0; c < 4; c++) out[c] = s[c] * vc[c] / 255.0f;
    } else
        for (int c = 0; c < 4; c++) out[c] = vc[c];
}

void hle_raster_tris(const RndVtx *v, int nverts, const RndState *st)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened) return;
    const Tex *t = tex_get(st->raster);
    if (st->raster && !t) {
        /* a raster without PS2 texture data: the camera frame buffer itself (motion blur / post-FX feedback) - not
           modelled, skipped instead of drawn untextured */
        n_skipped += (uint64_t)nverts / 3;
        return;
    }
    for (int i = 0; i + 2 < nverts; i += 3) {
        const RndVtx *a = &v[i], *b = &v[i + 1], *c = &v[i + 2];
        float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (fabsf(area) < 1e-6f || !isfinite(area)) continue;
        n_tris++;
        float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
        float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
        int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > scr_w) x1 = scr_w;
        if (y1 > scr_h) y1 = scr_h;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
                float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                float vc[4] = {w0 * a->r + w1 * b->r + w2 * c->r, w0 * a->g + w1 * b->g + w2 * c->g,
                               w0 * a->b + w1 * b->b + w2 * c->b, w0 * a->a + w1 * b->a + w2 * c->a};
                float s[4];
                shade(a, t, st, w0 * a->u + w1 * b->u + w2 * c->u, w0 * a->v + w1 * b->v + w2 * c->v, vc, s);
                put(x, y, s, st);
            }
    }
}

void hle_raster_lines(const RndVtx *v, int nverts, const RndState *st)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened) return;
    for (int i = 0; i + 1 < nverts; i += 2) {
        float dx = v[i + 1].x - v[i].x, dy = v[i + 1].y - v[i].y;
        int n = (int)fmaxf(fabsf(dx), fabsf(dy)) + 1;
        if (n > 4096) continue;
        n_lines++;
        for (int k = 0; k < n; k++) {
            float f = n > 1 ? (float)k / (n - 1) : 0;
            int x = (int)floorf(v[i].x + dx * f), y = (int)floorf(v[i].y + dy * f);
            if (x < 0 || y < 0 || x >= scr_w || y >= scr_h) continue;
            float s[4] = {v[i].r, v[i].g, v[i].b, v[i].a};
            put(x, y, s, st);
        }
    }
}

void hle_raster_clear(const uint8_t rgba[4], int mode)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened) return;
    if (mode & 2) memset(zbuf, 0, sizeof zbuf);           /* Z (the 3D world; GE depth 0 = farthest) */
    if (!(mode & 1)) return;
    for (int i = 0; i < scr_w * scr_h; i++) memcpy(fb + 4 * (size_t)i, rgba, 4);
}

/* ------------------------------------------------------------------ 3D (the 3D world: hle_w3d_host.c)
   A model of what the PSP GE does with the 3D draws: 16-bit depth (65535 = near, GEQUAL), perspective-correct
   texture / colour (q = 1/w), texel x vertex colour / 255, fog after shading, alpha test, Z test, then blend. */
int hle_raster_is_open(void) { return opened; }

int hle_raster_alpha_class(uint32_t raster)
{
    Tex *t = tex_get(raster);
    if (!t) return -1;
    uint32_t r = raster;
    int fmt = gb(r + 0x23) & 0xF, pal8 = gb(r + 0x23) & 0x20, pal4 = gb(r + 0x23) & 0x40, depth = (int)gw(r + 0x14);
    int opaque = 1, cut = 1;
    if (depth == 8 || depth == 4 || pal8 || pal4) {       /* texconv TC_AC_CLUT: every CLUT entry */
        int n = depth == 4 || pal4 ? 16 : 256;
        uint32_t pal = gw(r + 0x08);
        for (int i = 0; i < n; i++) {
            uint8_t e[4];
            clut_entry(pal, fmt, i, e);
            if (e[3] != 255) opaque = 0;
            if (e[3] != 255 && e[3] != 0) cut = 0;
        }
    } else
        for (int i = 0; i < t->w * t->h; i++) {
            uint8_t a = t->rgba[4 * (size_t)i + 3];
            if (a != 255) opaque = 0;
            if (a != 255 && a != 0) cut = 0;
        }
    return opaque ? 0 : cut ? 1 : 2;
}

static void put3(int x, int y, const float *src, const Rnd3State *st, float z)
{
    uint8_t *p = fb + ((size_t)y * scr_w + x) * 4;
    uint16_t *zp = &zbuf[(size_t)y * scr_w + x];
    uint16_t zi = (uint16_t)(z <= 0.0f ? 0 : z >= 65535.0f ? 65535 : (int)(z + 0.5f));
    float a = src[3];
    switch (st->atest) {
    case 1: if (!(a > 0.5f)) return; break;
    case 2: if (!(a >= 127.5f)) return; break;
    case 3: if (!(a > 0.5f && a < 127.5f)) return; break;
    default: break;
    }
    if (st->ztest && (zi < *zp || (st->ztest == 2 && zi == *zp))) return;   /* 2: GREATER (w3d_lodz) */
    float d[4] = {p[0], p[1], p[2], p[3]}, o[4];
    if (st->blend) {
        for (int c = 0; c < 4; c++) {
            float fs = factor(st->s.src_blend, src, d, c), fd = factor(st->s.dst_blend, src, d, c);
            if (fs < 0) fs = 1;
            if (fd < 0) fd = 0;
            o[c] = src[c] * fs + d[c] * fd;
        }
    } else
        memcpy(o, src, sizeof o);
    for (int c = 0; c < 4; c++) p[c] = (uint8_t)(o[c] < 0 ? 0 : o[c] > 255 ? 255 : o[c] + 0.5f);
    if (st->zwrite) *zp = zi;
    n_pixels++;
}

void hle_raster_tris3d(const Rnd3Vtx *v, int nverts, const Rnd3State *st)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened) return;
    const Tex *t = st->s.raster ? tex_get(st->s.raster) : NULL;
    if (st->s.raster && !t) { n_skipped += (uint64_t)nverts / 3; return; }
    float fogc[3] = {(float)(st->fog_color & 0xFF), (float)(st->fog_color >> 8 & 0xFF), (float)(st->fog_color >> 16 & 0xFF)};
    for (int i = 0; i + 2 < nverts; i += 3) {
        float sx[3], sy[3];
        const Rnd3Vtx *tv[3] = {&v[i], &v[i + 1], &v[i + 2]};
        for (int k = 0; k < 3; k++) {
            sx[k] = (tv[k]->x + 1.0f) * 0.5f * (float)scr_w;
            sy[k] = (1.0f - tv[k]->y) * 0.5f * (float)scr_h;
        }
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (fabsf(area) < 1e-6f || !isfinite(area)) continue;
        n_tris++;
        int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
        int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > scr_w) x1 = scr_w;
        if (y1 > scr_h) y1 = scr_h;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                float b0 = w0 * tv[0]->q, b1 = w1 * tv[1]->q, b2 = w2 * tv[2]->q, qs = b0 + b1 + b2;
                if (!(qs > 0.0f)) continue;
                b0 /= qs; b1 /= qs; b2 /= qs;
                float z = w0 * tv[0]->z + w1 * tv[1]->z + w2 * tv[2]->z;
                float vc[4] = {b0 * tv[0]->r + b1 * tv[1]->r + b2 * tv[2]->r, b0 * tv[0]->g + b1 * tv[1]->g + b2 * tv[2]->g,
                               b0 * tv[0]->b + b1 * tv[1]->b + b2 * tv[2]->b, b0 * tv[0]->a + b1 * tv[1]->a + b2 * tv[2]->a};
                float s[4];
                if (t) {
                    float tx[4];
                    sample(t, b0 * tv[0]->u + b1 * tv[1]->u + b2 * tv[2]->u, b0 * tv[0]->v + b1 * tv[1]->v + b2 * tv[2]->v,
                           st->s.filter, st->s.addr_u, st->s.addr_v, tx);
                    for (int c = 0; c < 4; c++) s[c] = tx[c] * vc[c] / 255.0f;
                } else
                    memcpy(s, vc, sizeof s);
                float f = b0 * tv[0]->fog + b1 * tv[1]->fog + b2 * tv[2]->fog;
                if (f < 1.0f) {
                    if (f < 0.0f) f = 0.0f;
                    for (int c = 0; c < 3; c++) s[c] = s[c] * f + fogc[c] * (1.0f - f);
                }
                put3(x, y, s, st, z);
            }
    }
}


/* the PS2 colour filter (hle_postfx.c) = the picture x k per channel, modelled as
   psp/hle_ge.c's hle_ge_postfx_mul draws it: one pass per factor of 2, GE blend Cd x src + Cd x FIX with the GE's
   ((2c+1)(2f+1)) >> 10 per term and one clamp (the PSP's 5650 rounding / dither are not modelled) */
int hle_raster_postfx_mul(const float k[3])
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened) return 0;
    float rem[3] = {k[0], k[1], k[2]};
    int np = 0;
    for (int pass = 0; pass < 3; pass++) {
        int src[3], fix[3], ident = 1;
        float m[3];
        for (int i = 0; i < 3; i++) {
            float r = rem[i] >= 0.0f ? rem[i] : 0.0f;
            m[i] = r > 2.0f ? 2.0f : r;
            fix[i] = 0;
            if (m[i] > 1.0f) { fix[i] = 255; src[i] = (int)((m[i] - 1.0f) * 256.0f); }
            else src[i] = (int)(m[i] * 256.0f);
            if (src[i] > 255) src[i] = 255;
            if (!(fix[i] == 255 && src[i] == 0) && !(fix[i] == 0 && src[i] == 255)) ident = 0;
        }
        if (ident) break;
        uint8_t lut[3][256];
        for (int i = 0; i < 3; i++)
            for (int d = 0; d < 256; d++) {
                int o = ((2 * src[i] + 1) * (2 * d + 1) >> 10) + ((2 * d + 1) * (2 * fix[i] + 1) >> 10);
                lut[i][d] = (uint8_t)(o > 255 ? 255 : o);
            }
        uint8_t *p = fb;
        for (int n = scr_w * scr_h; n > 0; n--, p += 4) {
            p[0] = lut[0][p[0]];
            p[1] = lut[1][p[1]];
            p[2] = lut[2][p[2]];
        }
        np++;
        for (int i = 0; i < 3; i++) rem[i] = m[i] > 0.0f ? rem[i] / m[i] : 1.0f;
    }
    return np;
}

/* the PS2 radiosity 0x518280 as the GS draws it (hle_postfx.c), on the
   8-bit model at guest resolution: D = the picture box-downsampled 2:1 filter_passes times (bilinear at the texel
   corners), B = clamp(2 D - limit, 0, 255), picture += bilinear 2^n x upscale of B x (intensity / 2) / 128 (GS ALPHA
   FIX), render_passes times */
int hle_raster_postfx_radiosity(int limit, int filter_passes, int render_passes, int intensity)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened || render_passes <= 0 || intensity / 2 <= 0) return 0;
    int lv = filter_passes < 0 ? 0 : filter_passes > 4 ? 4 : filter_passes;
    int w = scr_w, h = scr_h;
    float *d = malloc((size_t)w * h * 3 * sizeof(float));
    if (!d) return 0;
    for (int i = 0; i < w * h; i++)
        for (int c = 0; c < 3; c++) d[3 * i + c] = fb[4 * (size_t)i + c];
    for (int k = 0; k < lv && w >= 2 && h >= 2; k++) {     /* in place: (x, y) reads (2x.., 2y..) >= (x, y) */
        int nw = w / 2, nh = h / 2;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 3; c++) {
                    const float *p = d + 3 * ((size_t)(2 * y) * scr_w + 2 * x) + c;
                    d[3 * ((size_t)y * scr_w + x) + c] = (p[0] + p[3] + p[3 * scr_w] + p[3 * scr_w + 3]) * 0.25f;
                }
        w = nw;
        h = nh;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                float *p = d + 3 * ((size_t)y * scr_w + x) + c;
                float b = 2.0f * floorf(*p) - (float)limit;
                *p = b < 0.0f ? 0.0f : b > 255.0f ? 255.0f : b;
            }
    float sx = (float)w / (float)scr_w, sy = (float)h / (float)scr_h, f = (float)(intensity / 2) / 128.0f;
    for (int pass = 0; pass < render_passes; pass++)
        for (int y = 0; y < scr_h; y++) {
            float v = (y + 0.5f) * sy - 0.5f;
            int y0 = (int)floorf(v);
            float fy = v - (float)y0;
            int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
            for (int x = 0; x < scr_w; x++) {
                float u = (x + 0.5f) * sx - 0.5f;
                int x0 = (int)floorf(u);
                float fx = u - (float)x0;
                int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
                uint8_t *o = fb + 4 * ((size_t)y * scr_w + x);
                for (int c = 0; c < 3; c++) {
                    float t = (d[3 * ((size_t)ya * scr_w + xa) + c] * (1 - fx) + d[3 * ((size_t)ya * scr_w + xb) + c] * fx) * (1 - fy) +
                              (d[3 * ((size_t)yb * scr_w + xa) + c] * (1 - fx) + d[3 * ((size_t)yb * scr_w + xb) + c] * fx) * fy;
                    int r = o[c] + (int)(t * f);
                    o[c] = (uint8_t)(r > 255 ? 255 : r);
                }
            }
        }
    free(d);
    return render_passes;
}

void hle_raster_set_screen(int w, int h)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (w > 0 && h > 0 && w <= FB_MAX_W && h <= FB_MAX_H && (w != scr_w || h != scr_h)) {
        LOG("RENDER screen %dx%d (camera frame buffer)\n", w, h);
        scr_w = w;
        scr_h = h;
    }
}

/* ------------------------------------------------------------------ PNG */
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = (uint32_t)crc32(0, h + 4, 4);
    crc = (uint32_t)crc32(crc, data, n);
    put32(h, crc);
    fwrite(h, 1, 4, f);
}

/* PNG of an RGBA8 image: channels 3 = RGB (alpha dropped), 4 = RGBA */
static int write_png_img(const char *path, const uint8_t *img, int w, int h, int channels)
{
    size_t row = (size_t)w * channels + 1, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    uLongf zn = compressBound((uLong)raw_n);
    uint8_t *z = malloc(zn);
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < w; x++)
            memcpy(raw + y * row + 1 + (size_t)channels * x, img + ((size_t)y * w + x) * 4, (size_t)channels);
    }
    int ok = compress2(z, &zn, raw, (uLong)raw_n, 6) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        uint8_t ih[13];
        put32(ih, (uint32_t)w);
        put32(ih + 4, (uint32_t)h);
        ih[8] = 8; ih[9] = channels == 4 ? 6 : 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;   /* 8-bit RGB(A) */
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ih, 13);
        chunk(f, "IDAT", z, (uint32_t)zn);
        chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    } else
        ok = 0;
    free(raw);
    free(z);
    return ok;
}

static int write_png(const char *path) { return write_png_img(path, fb, scr_w, scr_h, 3); }

/* every decoded texture once, as DIR/tex/<name>_<raster>.png (RGBA) - for checking the decoder */
static void dump_texture(const Tex *t)
{
    static int dumped;
    if (!opened || dumped >= 200) return;
    char p[1200], d[1100];
    snprintf(d, sizeof d, "%s/tex", out_dir);
    if (mkdir(d, 0777) != 0 && errno != EEXIST) return;
    snprintf(p, sizeof p, "%s/%s_%08x.png", d, t->name[0] ? t->name : "unnamed", t->raster);
    for (char *q = p + strlen(d) + 1; *q; q++)
        if (*q == '/' || *q == ' ') *q = '_';
    if (write_png_img(p, t->rgba, t->w, t->h, 4)) dumped++;
}

/* ------------------------------------------------------------------ frames */
int hle_raster_open(const char *dir, int every_n, int max_n)
{
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return 0;
    snprintf(out_dir, sizeof out_dir, "%s", dir);
    char p[1100];
    snprintf(p, sizeof p, "%s/frames.txt", dir);
    index_f = fopen(p, "w");
    if (!index_f) return 0;
    fprintf(index_f, "# host render capture: flip vtime_s thread file\n");
    every = every_n > 0 ? every_n : 1;
    max_frames = max_n;
    memset(fb, 0, sizeof fb);
    opened = 1;
    return 1;
}

int hle_raster_frame_end(uint64_t flip, const char *tag)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!opened || written >= max_frames || (flip != 1 && flip % (uint64_t)every)) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)scr_w * scr_h * 4; i++) h = (h ^ fb[i]) * 1099511628211ull;
    if (h == last_hash) return 0;                         /* same picture as the last PNG */
    last_hash = h;
    char p[1100];
    snprintf(p, sizeof p, "%s/frame_%06llu.png", out_dir, (unsigned long long)flip);
    if (!write_png(p)) return 0;
    written++;
    fprintf(index_f, "%llu %.3f %s frame_%06llu.png\n", (unsigned long long)flip, (double)hle_vtime / EE_HZ, tag,
            (unsigned long long)flip);
    fflush(index_f);
    return 1;
}

void hle_raster_close(void)
{
    if (!opened) return;
    LOG("render capture: %d PNG frames in %s, %llu triangles, %llu lines, %llu pixels, textures decoded %llu, "
        "failed %llu (cache %d rasters, %zu KB), %llu triangles skipped (texture = a frame-buffer raster)\n", written,
        out_dir, (unsigned long long)n_tris, (unsigned long long)n_lines, (unsigned long long)n_pixels,
        (unsigned long long)n_tex_decoded, (unsigned long long)n_tex_failed, ntex, tex_bytes >> 10,
        (unsigned long long)n_skipped);
    for (int i = 0, k = 0; i < ntex && k < 40; i++)
        if (texs[i].ok) {
            LOG("  texture %08x %-24s %s %dx%d\n", texs[i].raster, texs[i].name[0] ? texs[i].name : "(unnamed)",
                texs[i].ok > 0 ? "decoded" : "FAILED", texs[i].w, texs[i].h);
            k++;
        }
    if (index_f) fclose(index_f);
    index_f = NULL;
    opened = 0;
}
#endif
