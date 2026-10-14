/* texconv.c - the render driver's PS2 native raster -> PSP GE texture.
 * See texconv.h for the input/output layouts. Sources of the rules:
 *  - PS2 raster layout, transfer minimum sizes, the pre-swizzle closed form and the CSM1 palette order: adapted from
 *    an MIT-licensed library (see NOTICE.md), implemented here as the fast byte/nibble transposes below;
 *  - PSP Fast-mode (swizzled) order: (y/8)*(pitch*8) + (y%8)*16 + (xb/16)*128 + xb%16 for byte column xb, i.e.
 *    16-byte x 8-row blocks, height aligned to 8 (PPSSPP and the PSP agree). */
#include "texconv.h"

#include <string.h>

#if defined(__GNUC__)
typedef uint32_t tc_w32 __attribute__((may_alias, aligned(4)));
#define LDW(p) (*(const tc_w32 *)(const void *)(p))
#define STW(p, v) (*(tc_w32 *)(void *)(p) = (v))
#else
static uint32_t LDW(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
#define STW(p, v) do { uint32_t v_ = (v); memcpy((p), &v_, 4); } while (0)
#endif

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int is_pot(int32_t v) { return v > 0 && (v & (v - 1)) == 0; }
static int imax(int a, int b) { return a > b ? a : b; }
static uint32_t a16(uint32_t x) { return (x + 15u) & ~15u; }

/* ------------------------------------------------------------------------------------------------ inspect */
/* transfer minimum size: the smallest upload of a level */
static void transfer_min(int psm, int swizzled, int *minw, int *minh)
{
    *minh = 1;
    switch (psm) {
    case TC_GS_PSMCT32: *minw = 2; break;
    case TC_GS_PSMCT16: case TC_GS_PSMCT16S: *minw = 4; break;
    default: *minw = 8; break;
    }
    if (swizzled && psm == TC_GS_PSMT8) { *minw = 16; *minh = 4; }
    if (swizzled && psm == TC_GS_PSMT4) { *minw = 32; *minh = 4; }
}

/* One upload header at blk + off: GIF A+D (NLOOP 3) TRXPOS/TRXREG/TRXDIR + IMAGE tag. */
static int upload(const uint8_t *blk, uint32_t size, uint32_t off, uint32_t *rrw, uint32_t *rrh, uint32_t *data_off,
                  uint32_t *data_bytes)
{
    if (off > size || size - off < 0x50u) return -1;
    const uint8_t *p = blk + off;
    uint64_t tag = rd64(p), regs = rd64(p + 8);
    if ((tag & 0x7FFFu) != 3u || ((tag >> 58) & 3u) != 0u || ((tag >> 60) & 0xFu) != 1u || (regs & 0xFu) != 0xEu)
        return -1;
    if (p[0x18] != 0x51 || p[0x28] != 0x52 || p[0x38] != 0x53) return -1;
    uint64_t trxreg = rd64(p + 0x20), img = rd64(p + 0x40);
    if (((img >> 58) & 3u) != 2u) return -1;
    *rrw = (uint32_t)(trxreg & 0xFFFu);
    *rrh = (uint32_t)((trxreg >> 32) & 0xFFFu);
    *data_off = off + 0x50u;
    *data_bytes = (uint32_t)(img & 0x7FFFu) * 16u;
    if (*data_bytes > size - *data_off) return -1;
    return 0;
}

static int level_ok(const TcPs2Info *o, int k, uint32_t rrw, uint32_t rrh, uint32_t bytes, TcPs2Level *L)
{
    int lw = imax(o->width >> k, 1), lh = imax(o->height >> k, 1), minw, minh;
    if (o->psm == TC_GS_PSMT8 || o->psm == TC_GS_PSMT4) {      /* pre-swizzled first (halved TRXREG), then linear */
        for (int swz = 1; swz >= 0; swz--) {
            transfer_min(o->psm, swz, &minw, &minh);
            uint32_t sw = (uint32_t)imax(lw, minw), sh = (uint32_t)imax(lh, minh);
            uint32_t ew = swz ? sw / 2 : sw, eh = swz ? sh / 2 : sh;
            if (rrw == ew && rrh == eh && bytes == a16(sw * sh * (uint32_t)o->depth / 8u)) {
                L->w = (uint16_t)lw; L->h = (uint16_t)lh; L->sw = (uint16_t)sw; L->sh = (uint16_t)sh;
                L->swizzled = (uint8_t)swz;
                return 1;
            }
        }
        return 0;
    }
    transfer_min(o->psm, 0, &minw, &minh);
    uint32_t sw = (uint32_t)imax(lw, minw), sh = (uint32_t)imax(lh, minh);
    if (rrw != sw || rrh != sh || bytes != a16(sw * sh * (uint32_t)o->depth / 8u)) return 0;
    L->w = (uint16_t)lw; L->h = (uint16_t)lh; L->sw = (uint16_t)sw; L->sh = (uint16_t)sh; L->swizzled = 0;
    return 1;
}

int tc_ps2_inspect(const TcPs2Raster *r, TcPs2Info *o)
{
    if (!r || !o || !r->pix) return TC_ERR_ARG;
    memset(o, 0, sizeof *o);
    o->width = r->width; o->height = r->height; o->depth = r->depth; o->format = r->format;
    o->ps2_bytes = r->pix_size + (r->pal ? r->pal_size : 0u);
    if (r->version != 2) return TC_ERR_FORMAT;
    if (!is_pot(r->width) || !is_pot(r->height) || r->width > 1024 || r->height > 1024) return TC_ERR_FORMAT;
    uint32_t pix = r->format & TC_PF_PIXEL_MASK;
    if (r->format & (TC_PF_PAL8 | TC_PF_PAL4)) {
        int p8 = (r->format & TC_PF_PAL8) != 0;
        if ((r->format & TC_PF_PAL8) && (r->format & TC_PF_PAL4)) return TC_ERR_FORMAT;
        if (r->depth != (p8 ? 8 : 4)) return TC_ERR_FORMAT;
        if (pix != TC_PF_8888 && pix != TC_PF_1555) return TC_ERR_FORMAT;
        o->kind = p8 ? TC_KIND_PAL8 : TC_KIND_PAL4;
        o->psm = p8 ? TC_GS_PSMT8 : TC_GS_PSMT4;
        o->clut_bits = pix == TC_PF_8888 ? 32 : 16;
        o->clut_entries = p8 ? 256 : 16;
    } else if (r->depth == 32 && (pix == TC_PF_8888 || pix == TC_PF_888)) {
        o->kind = pix == TC_PF_8888 ? TC_KIND_RGBA32 : TC_KIND_RGB32;
        o->psm = TC_GS_PSMCT32;
    } else if (r->depth == 16 && (pix == TC_PF_1555 || pix == TC_PF_555)) {
        o->kind = TC_KIND_RGBA16;
        o->psm = pix == TC_PF_1555 ? TC_GS_PSMCT16S : TC_GS_PSMCT16;
    } else {
        return TC_ERR_FORMAT;
    }
    if (r->nlevels > TC_MAX_PS2_LEVELS) return TC_ERR_PACKET;
    uint32_t want = r->nlevels ? r->nlevels : TC_MAX_PS2_LEVELS, off = 0;
    for (uint32_t k = 0; k < want; k++) {
        uint32_t rrw, rrh, doff, dbytes;
        TcPs2Level *L = &o->level[k];
        if (upload(r->pix, r->pix_size, off, &rrw, &rrh, &doff, &dbytes) || !level_ok(o, (int)k, rrw, rrh, dbytes, L)) {
            if (r->nlevels || k == 0) return TC_ERR_PACKET;
            break;
        }
        L->data = r->pix + doff;
        L->bytes = dbytes;
        o->nlevels = (uint8_t)(k + 1);
        off = doff + dbytes;
        if ((o->width >> k) <= 1 && (o->height >> k) <= 1) break;
    }
    if (o->clut_entries) {
        uint32_t rrw, rrh, doff, dbytes, need = o->clut_entries * (o->clut_bits / 8u);
        if (!r->pal || upload(r->pal, r->pal_size, 0, &rrw, &rrh, &doff, &dbytes)) return TC_ERR_PACKET;
        if (o->kind == TC_KIND_PAL8 ? (rrw != 16 || rrh != 16) : (rrw != 8 || (rrh != 2 && rrh != 3)))
            return TC_ERR_PACKET;
        if (dbytes < need) return TC_ERR_PACKET;
        o->clut = r->pal + doff;
        o->clut_bytes = dbytes;
    }
    return TC_OK;
}

int tc_ps2_from_packets(const uint8_t *level0_upload, uint32_t avail, const uint8_t *palette_upload, int32_t width,
                        int32_t height, int32_t depth, uint32_t format, TcPs2Raster *out)
{
    if (!level0_upload || !out) return TC_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->width = width; out->height = height; out->depth = depth; out->format = format; out->version = 2;
    if (palette_upload > level0_upload && (uint32_t)(palette_upload - level0_upload) < avail)
        avail = (uint32_t)(palette_upload - level0_upload);   /* the palette upload ends the level walk */
    out->pix = level0_upload;
    out->pix_size = avail;
    out->nlevels = (format & TC_PF_MIPMAP) ? 0 : 1;
    if (palette_upload) {
        uint32_t rrw, rrh, doff, dbytes;
        if (upload(palette_upload, 0x50u + 0x400u, 0, &rrw, &rrh, &doff, &dbytes)) return TC_ERR_PACKET;
        out->pal = palette_upload;
        out->pal_size = doff + dbytes;
    }
    TcPs2Info info;
    int e = tc_ps2_inspect(out, &info);
    if (e) return e;
    const TcPs2Level *last = &info.level[info.nlevels - 1];
    out->pix_size = (uint32_t)(last->data + last->bytes - level0_upload);
    out->nlevels = info.nlevels;
    return TC_OK;
}

/* ------------------------------------------------------------------------------------------------ stream chunks */
static int chunk_at(const uint8_t *p, const uint8_t *end, uint32_t *type, uint32_t *size)
{
    if (p > end || end - p < 12) return -1;
    *type = rd32(p);
    *size = rd32(p + 4);
    if (*size > (uint32_t)(end - p - 12)) return -1;
    return 0;
}

static void copy_name(char *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n && i < 31 && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

int tc_parse_native(const uint8_t *chunk, uint32_t size, TcNative *o)
{
    uint32_t t, s, t2, s2;
    if (!chunk || !o) return TC_ERR_ARG;
    memset(o, 0, sizeof *o);
    const uint8_t *end = chunk + size;
    if (chunk_at(chunk, end, &t, &s) || t != 0x15u) return TC_ERR_CHUNK;
    const uint8_t *p = chunk + 12, *bend = p + s;
    /* STRUCT {'PS2\0', filter / addressing word} */
    if (chunk_at(p, bend, &t, &s) || t != 1u || s < 8 || rd32(p + 12) != 0x00325350u) return TC_ERR_CHUNK;
    o->filter_addressing = rd32(p + 16);
    p += 12 + s;
    if (chunk_at(p, bend, &t, &s) || t != 2u) return TC_ERR_CHUNK;
    copy_name(o->name, p + 12, s);
    p += 12 + s;
    if (chunk_at(p, bend, &t, &s) || t != 2u) return TC_ERR_CHUNK;
    copy_name(o->mask, p + 12, s);
    p += 12 + s;
    /* STRUCT { STRUCT(0x40 raster header), STRUCT(pixel block + palette block) } */
    if (chunk_at(p, bend, &t, &s) || t != 1u) return TC_ERR_CHUNK;
    const uint8_t *r = p + 12, *rend = r + s;
    if (chunk_at(r, rend, &t2, &s2) || t2 != 1u || s2 < 0x40u) return TC_ERR_CHUNK;
    const uint8_t *h = r + 12;
    TcPs2Raster *ras = &o->raster;
    ras->width = (int32_t)rd32(h);
    ras->height = (int32_t)rd32(h + 4);
    ras->depth = (int32_t)rd32(h + 8);
    ras->format = rd16(h + 0x0C);
    ras->version = (int16_t)rd16(h + 0x0E);
    ras->tex0 = rd64(h + 0x10);
    ras->nlevels = ((rd32(h + 0x1C) >> 2) & 7u) + 1u;
    uint32_t pixel_size = rd32(h + 0x30), palette_size = rd32(h + 0x34);
    ras->mipmap_kl = rd32(h + 0x3C);
    r = h + s2;
    if (chunk_at(r, rend, &t2, &s2) || t2 != 1u) return TC_ERR_CHUNK;
    if (pixel_size > s2 || palette_size > s2 - pixel_size) return TC_ERR_CHUNK;
    ras->pix = r + 12;
    ras->pix_size = pixel_size;
    ras->pal = palette_size ? r + 12 + pixel_size : NULL;
    ras->pal_size = palette_size;
    p += 12 + s;
    /* EXTENSION { mipmap K/L value 0x110 ... } (optional) */
    if (!chunk_at(p, bend, &t, &s) && t == 3u) {
        const uint8_t *e = p + 12, *eend = e + s;
        while (!chunk_at(e, eend, &t2, &s2)) {
            if (t2 == 0x110u && s2 >= 4) { o->drv_mipmap = rd32(e + 12); o->has_drv_mipmap = 1; }
            e += 12 + s2;
        }
    }
    return TC_OK;
}

int tc_dict_begin(const uint8_t *dict, uint32_t size, TcDictIter *it, uint32_t *count)
{
    uint32_t t, s, t2, s2;
    if (!dict || !it) return TC_ERR_ARG;
    const uint8_t *end = dict + size;
    if (chunk_at(dict, end, &t, &s) || t != 0x16u) return TC_ERR_CHUNK;
    const uint8_t *p = dict + 12;
    it->end = p + s;
    if (chunk_at(p, it->end, &t2, &s2) || t2 != 1u || s2 < 4) return TC_ERR_CHUNK;
    if (count) *count = rd16(p + 12);
    it->p = p + 12 + s2;
    return TC_OK;
}

int tc_dict_next(TcDictIter *it, const uint8_t **chunk, uint32_t *chunk_size)
{
    uint32_t t, s;
    while (it->p < it->end) {
        if (chunk_at(it->p, it->end, &t, &s)) return TC_ERR_CHUNK;
        const uint8_t *c = it->p;
        it->p += 12 + s;
        if (t == 0x15u) {
            *chunk = c;
            *chunk_size = 12 + s;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ policy / plan */
void tc_policy_default(TcPolicy *p)
{
    memset(p, 0, sizeof *p);
    p->max_dim = 512;
    p->max_levels = 8;
    p->direct32 = TC_D32_8888;
    p->cache_bytes = 8192;
    p->swizzle = 1;
    p->decimate = 0;
    p->alpha = TC_AC_CLUT;
    p->flags = 0;
}

static const uint8_t gu_bpp[6] = {16, 16, 16, 32, 4, 8};

static uint32_t row_bytes(int psm, int w) { return (uint32_t)w * gu_bpp[psm] / 8u; }
static int min_swizzle_w(int psm) { return 128 / gu_bpp[psm]; }

/* PSMT4 swizzled-transfer address (nibbles) / PSMT8 (bytes) of texel (x, y), stored stride ww texels:
 * the pre-swizzle closed form (see the file header) */
static uint32_t swz(uint32_t x, uint32_t y, uint32_t ww)
{
    uint32_t xx = x ^ ((((y >> 1) ^ (y >> 2)) & 1u) << 2);
    uint32_t nx = (xx & 7u) | ((x >> 1) & ~7u);
    uint32_t ny = (y & 1u) | ((y >> 1) & ~1u);
    uint32_t lane = ((y >> 1) & 1u) | (((x >> 3) & 1u) << 1);
    return lane | (nx << 2) | ny * 2u * ww;
}

/* texel (x, y) of a PS2 level: palette index, or RGBA8888 with PS2 alpha (0..0x80) for direct colour */
static uint32_t src_texel(const TcPs2Info *in, const TcPs2Level *L, uint32_t x, uint32_t y)
{
    switch (in->kind) {
    case TC_KIND_PAL8: {
        uint32_t a = L->swizzled ? swz(x, y, L->sw) : y * L->sw + x;
        return a < L->bytes ? L->data[a] : 0u;
    }
    case TC_KIND_PAL4: {
        uint32_t a = L->swizzled ? swz(x, y, L->sw) : y * L->sw + x, b = a >> 1;
        if (b >= L->bytes) return 0u;
        return (a & 1u) ? (uint32_t)(L->data[b] >> 4) : (uint32_t)(L->data[b] & 15u);
    }
    case TC_KIND_RGBA32: return rd32(L->data + (y * L->sw + x) * 4u);
    case TC_KIND_RGB32: return (rd32(L->data + (y * L->sw + x) * 4u) & 0x00FFFFFFu) | 0x80000000u;
    default: {                                                   /* RGBA16 1555 -> RGBA8888 (PS2 alpha) */
        uint32_t v = rd16(L->data + (y * L->sw + x) * 2u);
        return ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19) |
               ((in->psm == TC_GS_PSMCT16 || (v & 0x8000u)) ? 0x80000000u : 0u);
    }
    }
}

/* GS CSM1: texel index i of a 256-entry CLUT lives at CLUT buffer entry i with bits 3 and 4 swapped */
static uint32_t csm1(uint32_t i) { return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1); }

static int alpha_class_of(uint32_t lo, uint32_t mid)      /* lo: some alpha < 0x80; mid: some 0 < alpha < 0x80 */
{
    return mid ? TC_ALPHA_BLEND : lo ? TC_ALPHA_CUTOUT : TC_ALPHA_OPAQUE;
}

static uint32_t clut_alpha(const TcPs2Info *in, uint32_t i)
{
    if (in->clut_bits == 32) return in->clut[4u * i + 3u];
    return (rd16(in->clut + 2u * i) & 0x8000u) ? 0x80u : 0u;
}

/* alpha class of every CLUT entry / of every level-0 texel of a direct-colour raster */
static int alpha_class_all(const TcPs2Info *in)
{
    uint32_t lo = 0, mid = 0;
    if (in->clut_entries) {
        for (uint32_t i = 0; i < in->clut_entries; i++) {
            uint32_t a = clut_alpha(in, i);
            lo |= a < 0x80u;
            mid |= a && a < 0x80u;
        }
    } else if (in->kind == TC_KIND_RGBA32) {                  /* alpha = byte 3 of every stored texel */
        const TcPs2Level *L = &in->level[0];
        for (uint32_t y = 0; y < L->h; y++) {
            const uint8_t *row = L->data + (size_t)y * L->sw * 4u + 3u;
            for (uint32_t x = 0; x < L->w; x++) {
                uint32_t a = row[4u * x];
                lo |= a < 0x80u;
                mid |= (a - 1u) < 0x7Fu;                     /* 0 < a < 0x80 */
            }
        }
    } else {
        const TcPs2Level *L = &in->level[0];
        for (uint32_t y = 0; y < L->h; y++)
            for (uint32_t x = 0; x < L->w; x++) {
                uint32_t a = src_texel(in, L, x, y) >> 24;
                lo |= a < 0x80u;
                mid |= a && a < 0x80u;
            }
    }
    return alpha_class_of(lo, mid);
}

/* nib2mask[b] = bit (b & 15) | bit (b >> 4): the two PAL4 indices of a stored byte */
#define NM(b) (uint16_t)((1u << ((b) & 15u)) | (1u << ((b) >> 4)))
#define NM4(b) NM(b), NM((b) + 1u), NM((b) + 2u), NM((b) + 3u)
#define NM16(b) NM4(b), NM4((b) + 4u), NM4((b) + 8u), NM4((b) + 12u)
#define NM64(b) NM16(b), NM16((b) + 16u), NM16((b) + 32u), NM16((b) + 48u)
static const uint16_t nib2mask[256] = {NM64(0u), NM64(64u), NM64(128u), NM64(192u)};

/* alpha class of the CLUT entries level 0 uses. cat[texel index] = 0 opaque / 1 alpha 0 / 2 partial (texel index i
 * reads CLUT buffer entry csm1(i) for PAL8). The scan stops as soon as the class is decided: at the first partial
 * entry in use (BLEND), or at the first alpha-0 entry in use when the CLUT has no partial entry (CUTOUT); only an
 * opaque result needs every texel. Stored bytes are classified 4 per word through a 256-entry byte table (PAL8:
 * cat itself; PAL4: both nibbles' categories, built per texture when the image is large enough to repay it). */
static int alpha_class_used(const TcPs2Info *in)
{
    const TcPs2Level *L = &in->level[0];
    const uint32_t ne = in->clut_entries;
    uint8_t cat[256];
    uint32_t acc = 0, have = 0;
    for (uint32_t i = 0; i < ne; i++) {
        uint32_t a = clut_alpha(in, ne == 256 ? csm1(i) : i);
        cat[i] = (uint8_t)(a >= 0x80u ? 0u : a ? 2u : 1u);
        have |= cat[i];
    }
    const uint32_t decide = (have & 2u) ? 2u : 1u;   /* acc bits that settle the class */
    if (L->w == L->sw && L->h == L->sh) {        /* the stored image is exactly the texels (a permutation of them) */
        const uint32_t n = (uint32_t)L->sw * L->sh * (uint32_t)in->depth / 8u;
        const uint8_t *d = L->data;
        if (in->kind == TC_KIND_PAL4 && n < 2048u) {
            uint32_t zero = 0, mid = 0, m = 0;
            for (uint32_t i = 0; i < 16u; i++) {
                zero |= (uint32_t)(cat[i] == 1u) << i;
                mid |= (uint32_t)(cat[i] == 2u) << i;
            }
            for (uint32_t j = 0; j < n; j++) m |= nib2mask[d[j]];
            acc = ((m & mid) ? 2u : 0u) | ((m & zero) ? 1u : 0u);
        } else {
            uint8_t cat4[256];
            if (in->kind == TC_KIND_PAL4) {
                for (uint32_t b = 0; b < 256u; b++) cat4[b] = (uint8_t)(cat[b & 15u] | cat[b >> 4]);
                memcpy(cat, cat4, sizeof cat);          /* the scan below indexes the array itself (no pointer) */
            }
            if (((uintptr_t)d & 3u) == 0 && (n & 63u) == 0) {
                for (uint32_t i = 0; i < n && !(acc & decide); i += 64) {
                    uint32_t a = 0;
                    for (uint32_t j = i; j < i + 64u; j += 4) {
                        uint32_t v = LDW(d + j);
                        a |= (uint32_t)cat[v & 0xFFu] | cat[(v >> 8) & 0xFFu] | cat[(v >> 16) & 0xFFu] | cat[v >> 24];
                    }
                    acc |= a;
                }
            } else {
                for (uint32_t j = 0; j < n && !(acc & decide); j++) acc |= cat[d[j]];
            }
        }
    } else {
        for (uint32_t y = 0; y < L->h; y++)
            for (uint32_t x = 0; x < L->w; x++) acc |= cat[src_texel(in, L, x, y)];
    }
    return (acc & 2u) ? TC_ALPHA_BLEND : acc ? TC_ALPHA_CUTOUT : TC_ALPHA_OPAQUE;
}

int tc_plan(const TcPs2Info *in, const TcPolicy *pol, TcPspTex *o)
{
    if (!in || !pol || !o || !in->nlevels) return TC_ERR_ARG;
    memset(o, 0, sizeof *o);
    o->alpha_class = TC_ALPHA_UNKNOWN;
    o->clut_psm = TC_GU_NONE;
    switch (in->kind) {
    case TC_KIND_PAL4:
    case TC_KIND_PAL8:
        o->psm = in->kind == TC_KIND_PAL8 ? TC_GU_T8 : TC_GU_T4;
        o->clut_psm = in->clut_bits == 32 ? TC_GU_8888 : TC_GU_5551;
        o->clut_entries = in->clut_entries;
        break;
    case TC_KIND_RGBA16: o->psm = TC_GU_5551; break;
    case TC_KIND_RGB32: o->psm = pol->direct32 == TC_D32_16 ? TC_GU_5650 : TC_GU_8888; break;
    default:
        if (pol->direct32 == TC_D32_16) {
            int ac = alpha_class_all(in);
            o->psm = ac == TC_ALPHA_OPAQUE ? TC_GU_5650 : ac == TC_ALPHA_CUTOUT ? TC_GU_5551 : TC_GU_4444;
        } else {
            o->psm = TC_GU_8888;
        }
        break;
    }
    const int nps2 = in->nlevels, W = in->width, H = in->height;
    const int max_dim = pol->max_dim && pol->max_dim < 512 ? pol->max_dim : 512;
    int first = 0, shift = 0;
    while (imax(W >> first, H >> first) > max_dim && first + 1 < nps2) first++;
    while (imax(W >> (first + shift), H >> (first + shift)) > max_dim) {
        if (!pol->decimate) return TC_ERR_SIZE;
        shift++;
    }
    if (pol->cache_bytes)
        while (first + 1 < nps2 && !shift) {
            int lw = imax(W >> first, 1), lh = imax(H >> first, 1);
            if (row_bytes(o->psm, lw) * (uint32_t)lh <= pol->cache_bytes) break;
            first++;
        }
    const int w0 = imax(W >> (first + shift), 1);
    const int sw = pol->swizzle && w0 >= min_swizzle_w(o->psm);
    const int maxl = pol->max_levels < 1 ? 1 : pol->max_levels > TC_MAX_PSP_LEVELS ? TC_MAX_PSP_LEVELS : pol->max_levels;
    uint32_t off = 0;
    int n = 0;
    for (int k = first; k < nps2; k++) {
        int lw = imax(W >> (k + shift), 1), lh = imax(H >> (k + shift), 1);
        if (n >= maxl) break;
        if (sw && lw < min_swizzle_w(o->psm)) break;
        if (shift && k > first) break;
        uint32_t rb = row_bytes(o->psm, lw);
        if (rb < 16u) rb = 16u;
        uint32_t rows = sw ? (uint32_t)((lh + 7) & ~7) : (uint32_t)lh;
        TcPspLevel *pl = &o->level[n++];
        pl->offset = off;
        pl->bytes = rb * rows;
        pl->width = (uint16_t)lw;
        pl->height = (uint16_t)lh;
        pl->bufw = (uint16_t)(rb * 8u / gu_bpp[o->psm]);
        pl->rows = (uint16_t)rows;
        off = a16(off + pl->bytes);
    }
    if ((pol->flags & TC_F_GENMIP) && n > 0) {             /* generated levels below the kept ones */
        int ng = 0;
        while (n < maxl) {
            const TcPspLevel *pv = &o->level[n - 1];
            if (pv->width == 1 && pv->height == 1) break;
            int lw = imax(pv->width >> 1, 1), lh = imax(pv->height >> 1, 1);
            if (sw && (uint32_t)lw * gu_bpp[o->psm] < 128u) break;   /* the swizzle floor (16-byte rows) */
            uint32_t rb = row_bytes(o->psm, lw);
            if (rb < 16u) rb = 16u;
            uint32_t rows = sw ? ((uint32_t)lh + 7u) & ~7u : (uint32_t)lh;
            TcPspLevel *pl = &o->level[n++];
            pl->offset = off;
            pl->bytes = rb * rows;
            pl->width = (uint16_t)lw;
            pl->height = (uint16_t)lh;
            pl->bufw = (uint16_t)(rb * 8u / gu_bpp[o->psm]);
            pl->rows = (uint16_t)rows;
            off = a16(off + pl->bytes);
            ng++;
        }
        o->ngen = (uint8_t)ng;
    }
    o->nlevels = (uint8_t)n;
    o->first = (uint8_t)first;
    o->shift = (uint8_t)shift;
    o->swizzled = (uint8_t)sw;
    if (o->clut_psm != TC_GU_NONE) {
        o->clut_offset = off;
        o->clut_bytes = o->clut_entries * (o->clut_psm == TC_GU_8888 ? 4u : 2u);
        off = a16(off + o->clut_bytes);
    }
    o->total_bytes = off;
    return TC_OK;
}

/* ------------------------------------------------------------------------------------------------ fast paths */
/* 4x4 byte transpose of the words a, b, c, d: x[n] = byte n of a, b, c, d (in that byte order) */
#define TRANSPOSE4(a, b, c, d, x0, x1, x2, x3)                                                                       \
    do {                                                                                                             \
        uint32_t t0_ = ((a) & 0x00FF00FFu) | (((b) & 0x00FF00FFu) << 8);                                             \
        uint32_t t1_ = (((a) >> 8) & 0x00FF00FFu) | ((b) & 0xFF00FF00u);                                             \
        uint32_t t2_ = ((c) & 0x00FF00FFu) | (((d) & 0x00FF00FFu) << 8);                                             \
        uint32_t t3_ = (((c) >> 8) & 0x00FF00FFu) | ((d) & 0xFF00FF00u);                                             \
        x0 = (t0_ & 0xFFFFu) | (t2_ << 16);                                                                          \
        x1 = (t1_ & 0xFFFFu) | (t3_ << 16);                                                                          \
        x2 = (t0_ >> 16) | (t2_ & 0xFFFF0000u);                                                                      \
        x3 = (t1_ >> 16) | (t3_ & 0xFFFF0000u);                                                                      \
    } while (0)

/* PSMT8 pre-swizzled (stride w >= 16, h % 4 == 0) -> T8 Fast mode (row w bytes).
 * In a 4-row strip y0..y0+3 the 32 bytes at y0*w + p*2w + 32g hold texels 16g..16g+15 of rows y0+p (lanes 0/2) and
 * y0+p+2 (lanes 1/3) of words 0..7; rows whose ((y>>1)^(y>>2)) bit is set take words 4..7 first. A 16-texel group is
 * one 16-byte row of a PSP block. */
/* one 16-texel group: 32 source bytes at s -> 16 bytes of row A at da, 16 of row B at db; SRCA = row A's swap flag
 * (a literal, so each instantiation is straight-line code) */
#define T8_GROUP(s, da, db, SRCA)                                                                                      \
    do {                                                                                                             \
        uint32_t x0, x1, x2, x3, y0w, y1w, y2w, y3w;                                                                 \
        TRANSPOSE4(LDW(s), LDW((s) + 4), LDW((s) + 8), LDW((s) + 12), x0, x1, x2, x3);                               \
        TRANSPOSE4(LDW((s) + 16), LDW((s) + 20), LDW((s) + 24), LDW((s) + 28), y0w, y1w, y2w, y3w);                  \
        if (!(SRCA)) {                                                                                                 \
            STW(da, x0); STW((da) + 4, y0w); STW((da) + 8, x2); STW((da) + 12, y2w);                                 \
            STW(db, y1w); STW((db) + 4, x1); STW((db) + 8, y3w); STW((db) + 12, x3);                                 \
        } else {                                                                                                     \
            STW(da, y0w); STW((da) + 4, x0); STW((da) + 8, y2w); STW((da) + 12, x2);                                 \
            STW(db, x1); STW((db) + 4, y1w); STW((db) + 8, x3); STW((db) + 12, y3w);                                 \
        }                                                                                                            \
    } while (0)

static void fast_t8(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    const uint32_t ngroups = w >> 4, blockrow = w * 8u;
    for (uint32_t y0 = 0; y0 < h; y0 += 4) {
        const uint32_t sa = (y0 >> 2) & 1u;
        for (uint32_t p = 0; p < 2; p++) {
            const uint8_t *s = src + (size_t)y0 * w + (size_t)p * 2u * w;
            const uint32_t ya = y0 + p, yb = ya + 2;
            uint8_t *da = dst + (ya >> 3) * blockrow + (ya & 7u) * 16u;
            uint8_t *db = dst + (yb >> 3) * blockrow + (yb & 7u) * 16u;
            if (!sa)
                for (uint32_t g = 0; g < ngroups; g++, s += 32, da += 128, db += 128) T8_GROUP(s, da, db, 0);
            else
                for (uint32_t g = 0; g < ngroups; g++, s += 32, da += 128, db += 128) T8_GROUP(s, da, db, 1);
        }
    }
}

/* PSMT4 pre-swizzled (stride w >= 32 nibbles, h % 4 == 0) -> T4 Fast mode (row w/2 bytes).
 * Same structure in nibble units: the 16 bytes at y0*w/2 + p*w + 16g are 8 halfwords; row y0+p takes nibble lanes
 * 0/2, row y0+p+2 lanes 1/3. M = W & 0x0F0F0F0F, T = M | M >> 12 packs a word's two halfwords' lane-0 nibbles into
 * byte 0 and lane-2 nibbles into byte 1 (lanes 1/3 likewise from W >> 4): exactly the PSP's 2-texel bytes. */
/* lane-0/2 (t) and lane-1/3 (u) nibble pairs of a word, see fast_t4 */
#define NIB(wv, t, u)                                                                                                \
    do {                                                                                                             \
        uint32_t m_ = (wv) & 0x0F0F0F0Fu, n_ = ((wv) >> 4) & 0x0F0F0F0Fu;                                            \
        t = m_ | (m_ >> 12);                                                                                         \
        u = n_ | (n_ >> 12);                                                                                         \
    } while (0)
#define PACK_LO(a, b, c, d) (((a) & 0xFFu) | ((b) & 0xFFu) << 8 | ((c) & 0xFFu) << 16 | ((d) & 0xFFu) << 24)
#define PACK_HI(a, b, c, d) (((a) >> 8 & 0xFFu) | ((b) >> 8 & 0xFFu) << 8 | ((c) >> 8 & 0xFFu) << 16 | \
                             ((d) >> 8 & 0xFFu) << 24)
/* one 16-texel group: 16 source bytes at s -> 8 bytes of row A at da, 8 of row B at db; a swapped row reads
 * halfwords 4..7 (words 2, 3) first; row B's swap flag is !SRCA */
#define T4_GROUP(s, da, db, SRCA)                                                                                      \
    do {                                                                                                             \
        uint32_t t0, t1, t2, t3, u0, u1, u2, u3;                                                                     \
        NIB(LDW(s), t0, u0); NIB(LDW((s) + 4), t1, u1); NIB(LDW((s) + 8), t2, u2); NIB(LDW((s) + 12), t3, u3);       \
        if (!(SRCA)) {                                                                                                 \
            STW(da, PACK_LO(t0, t1, t2, t3)); STW((da) + 4, PACK_HI(t0, t1, t2, t3));                                \
            STW(db, PACK_LO(u2, u3, u0, u1)); STW((db) + 4, PACK_HI(u2, u3, u0, u1));                                \
        } else {                                                                                                     \
            STW(da, PACK_LO(t2, t3, t0, t1)); STW((da) + 4, PACK_HI(t2, t3, t0, t1));                                \
            STW(db, PACK_LO(u0, u1, u2, u3)); STW((db) + 4, PACK_HI(u0, u1, u2, u3));                                \
        }                                                                                                            \
    } while (0)

static void fast_t4(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    const uint32_t nbx = w >> 5, rb = w >> 1, blockrow = rb * 8u;     /* two 16-texel groups per PSP block */
    for (uint32_t y0 = 0; y0 < h; y0 += 4) {
        const uint32_t sa = (y0 >> 2) & 1u;
        for (uint32_t p = 0; p < 2; p++) {
            const uint8_t *s = src + (size_t)y0 * rb + (size_t)p * w;
            const uint32_t ya = y0 + p, yb = ya + 2;
            uint8_t *da = dst + (ya >> 3) * blockrow + (ya & 7u) * 16u;
            uint8_t *db = dst + (yb >> 3) * blockrow + (yb & 7u) * 16u;
            if (!sa)
                for (uint32_t b = 0; b < nbx; b++, s += 32, da += 128, db += 128) {
                    T4_GROUP(s, da, db, 0);
                    T4_GROUP(s + 16, da + 8, db + 8, 0);
                }
            else
                for (uint32_t b = 0; b < nbx; b++, s += 32, da += 128, db += 128) {
                    T4_GROUP(s, da, db, 1);
                    T4_GROUP(s + 16, da + 8, db + 8, 1);
                }
        }
    }
}

/* 2:1 point decimation (policy.decimate, plan shift 1) of a pre-swizzled
 * PSMT8 / PSMT4 level straight into the half-size Fast-mode texture - the same texels as the per-texel path (even
 * columns of even rows), byte-identical to it, at a fraction of its cost. In each 4-row strip y0 only the p = 0 half
 * of the source is read: its row A is y0 and its row B y0 + 2 (both even) -> output rows y0/2 and y0/2 + 1. A source
 * group (16 texels, 16 output bytes of a full-size row) gives 8 texels: output bytes (g/2)*128 + (g%2)*8 of the
 * output block row (T8; T4: 32-texel blocks b give 16 texels = 8 bytes at (b/2)*128 + (b%2)*8). */
#define EVEN8(a, b) (((a) & 0xFFu) | (((a) >> 8) & 0xFF00u) | (((b) & 0xFFu) << 16) | (((b) << 8) & 0xFF000000u))
static uint32_t even4(uint32_t v)                  /* the low nibbles (even texels) of 4 T4 bytes -> 2 bytes */
{
    uint32_t lo = v & 0x0F0F0F0Fu, h = lo | (lo >> 4);
    return (h & 0xFFu) | ((h >> 8) & 0xFF00u);
}

static void half_t8(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)   /* w, h: source size */
{
    const uint32_t ngroups = w >> 4, blockrow = (w >> 1) * 8u;
    uint32_t ta[4], tb[4];
    uint8_t *a8 = (uint8_t *)ta, *b8 = (uint8_t *)tb;
    for (uint32_t y0 = 0; y0 < h; y0 += 4) {
        const uint32_t sa = (y0 >> 2) & 1u, ya = y0 >> 1, yb = ya + 1u;
        const uint8_t *s = src + (size_t)y0 * w;
        uint8_t *da = dst + (ya >> 3) * blockrow + (ya & 7u) * 16u;
        uint8_t *db = dst + (yb >> 3) * blockrow + (yb & 7u) * 16u;
        for (uint32_t g = 0; g < ngroups; g++, s += 32) {
            if (!sa) T8_GROUP(s, a8, b8, 0); else T8_GROUP(s, a8, b8, 1);
            const uint32_t o = (g >> 1) * 128u + (g & 1u) * 8u;
            STW(da + o, EVEN8(ta[0], ta[1])); STW(da + o + 4, EVEN8(ta[2], ta[3]));
            STW(db + o, EVEN8(tb[0], tb[1])); STW(db + o + 4, EVEN8(tb[2], tb[3]));
        }
    }
}

static void half_t4(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)   /* w, h: source size */
{
    const uint32_t nbx = w >> 5, rb = w >> 1, blockrow = (w >> 2) * 8u;
    uint32_t ta[4], tb[4];
    uint8_t *a8 = (uint8_t *)ta, *b8 = (uint8_t *)tb;
    for (uint32_t y0 = 0; y0 < h; y0 += 4) {
        const uint32_t sa = (y0 >> 2) & 1u, ya = y0 >> 1, yb = ya + 1u;
        const uint8_t *s = src + (size_t)y0 * rb;
        uint8_t *da = dst + (ya >> 3) * blockrow + (ya & 7u) * 16u;
        uint8_t *db = dst + (yb >> 3) * blockrow + (yb & 7u) * 16u;
        for (uint32_t b = 0; b < nbx; b++, s += 32) {
            if (!sa) { T4_GROUP(s, a8, b8, 0); T4_GROUP(s + 16, a8 + 8, b8 + 8, 0); }
            else { T4_GROUP(s, a8, b8, 1); T4_GROUP(s + 16, a8 + 8, b8 + 8, 1); }
            const uint32_t o = (b >> 1) * 128u + (b & 1u) * 8u;
            STW(da + o, even4(ta[0]) | even4(ta[1]) << 16); STW(da + o + 4, even4(ta[2]) | even4(ta[3]) << 16);
            STW(db + o, even4(tb[0]) | even4(tb[1]) << 16); STW(db + o + 4, even4(tb[2]) | even4(tb[3]) << 16);
        }
    }
}

/* a generated mip level (TC_F_GENMIP) = 2:1 point decimation of the previous
 * PSP level as stored (Fast mode, w x h = half of it): output row y = source row 2y, output texel x = source texel 2x.
 * T8: the 32 source bytes of row 2y in the two source blocks 2b, 2b+1 give output block b's 16-byte row (even bytes);
 * T4: likewise in nibbles (even4). dw, dh: output size; the output row is >= 16 bytes (the swizzle floor). */
static void down_t8(const uint8_t *src, uint8_t *dst, uint32_t dw, uint32_t dh)
{
    const uint32_t srb = dw * 2u, drb = dw, nb = drb >> 4;
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t sy = 2u * y;
        const uint8_t *s = src + (sy >> 3) * srb * 8u + (sy & 7u) * 16u;
        uint8_t *d = dst + (y >> 3) * drb * 8u + (y & 7u) * 16u;
        for (uint32_t b = 0; b < nb; b++, s += 256, d += 128) {
            STW(d, EVEN8(LDW(s), LDW(s + 4)));
            STW(d + 4, EVEN8(LDW(s + 8), LDW(s + 12)));
            STW(d + 8, EVEN8(LDW(s + 128), LDW(s + 132)));
            STW(d + 12, EVEN8(LDW(s + 136), LDW(s + 140)));
        }
    }
}

static void down_t4(const uint8_t *src, uint8_t *dst, uint32_t dw, uint32_t dh)
{
    const uint32_t srb = dw, drb = dw >> 1, nb = drb >> 4;
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t sy = y << 1;
        const uint8_t *s = src + (sy >> 3) * srb * 8u + (sy & 7u) * 16u;
        uint8_t *d = dst + (y >> 3) * drb * 8u + (y & 7u) * 16u;
        for (uint32_t b = 0; b < nb; b++, s += 256, d += 128) {
            STW(d, even4(LDW(s)) | even4(LDW(s + 4)) << 16);
            STW(d + 4, even4(LDW(s + 8)) | even4(LDW(s + 12)) << 16);
            STW(d + 8, even4(LDW(s + 128)) | even4(LDW(s + 132)) << 16);
            STW(d + 12, even4(LDW(s + 136)) | even4(LDW(s + 140)) << 16);
        }
    }
}

/* PS2 RGBA8888 (alpha 0..0x80) -> PSP 8888: alpha min(255, 2a) (= tc_alpha_ps2_to_psp, written as a min so that
 * Allegrex gets srl/andi/min/ins) */
static uint32_t conv8888(uint32_t v)
{
    uint32_t a = (v >> 23) & 0x1FEu;
    a = a < 0xFFu ? a : 0xFFu;
    return (v & 0x00FFFFFFu) | a << 24;
}

/* PSMCT32 linear (stride w >= 4) -> 8888 Fast mode, alpha scaled */
static void fast_8888(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint32_t sstride)
{
    const uint32_t rb = w * 4u, blockrow = rb * 8u;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * sstride * 4u;
        uint8_t *d = dst + (y >> 3) * blockrow + (y & 7u) * 16u;
        for (uint32_t x = 0; x < w; x += 4, s += 16, d += 128) {
            STW(d, conv8888(LDW(s)));
            STW(d + 4, conv8888(LDW(s + 4)));
            STW(d + 8, conv8888(LDW(s + 8)));
            STW(d + 12, conv8888(LDW(s + 12)));
        }
    }
}

/* ------------------------------------------------------------------------------------------------ per-texel path */
static uint32_t psp_value(const TcPs2Info *in, int psm, uint32_t v)
{
    if (in->kind == TC_KIND_PAL4 || in->kind == TC_KIND_PAL8) return v;
    if (psm == TC_GU_8888) return conv8888(v);
    uint32_t r = v & 0xFFu, g = (v >> 8) & 0xFFu, b = (v >> 16) & 0xFFu, a = tc_alpha_ps2_to_psp(v >> 24);
    switch (psm) {
    case TC_GU_5650: return (r >> 3) | (g >> 2) << 5 | (b >> 3) << 11;
    case TC_GU_5551: return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (a >> 7) << 15;
    default: return (r >> 4) | (g >> 4) << 4 | (b >> 4) << 8 | (a >> 4) << 12;
    }
}

static void generic_level(const TcPs2Info *in, const TcPspTex *pl, int j, uint8_t *lvl)
{
    const TcPspLevel *P = &pl->level[j];
    const TcPs2Level *L = &in->level[pl->first + j];
    const uint32_t rb = P->bytes / P->rows, step = 1u << pl->shift, bpp = gu_bpp[pl->psm];
    for (uint32_t y = 0; y < P->height; y++)
        for (uint32_t x = 0; x < P->width; x++) {
            uint32_t v = psp_value(in, pl->psm, src_texel(in, L, x * step, y * step));
            uint32_t xb = x * bpp / 8u;
            uint32_t a = pl->swizzled ? (y >> 3) * rb * 8u + (xb >> 4) * 128u + (y & 7u) * 16u + (xb & 15u)
                                      : y * rb + xb;
            switch (bpp) {
            case 4: lvl[a] = (uint8_t)((x & 1u) ? (lvl[a] & 0x0Fu) | (v << 4) : (lvl[a] & 0xF0u) | (v & 15u)); break;
            case 8: lvl[a] = (uint8_t)v; break;
            case 16: wr16(lvl + a, v); break;
            default: wr32(lvl + a, v); break;
            }
        }
}

/* per-texel generated level j from level j-1 (any format / layout): texel (x, y) = previous (sx x, sy y),
 * sx / sy = 2 while the previous side is larger, else 1 (a side already at 1) */
static uint32_t psp_off(const TcPspTex *pl, uint32_t rb, uint32_t xb, uint32_t y)
{
    return pl->swizzled ? (y >> 3) * rb * 8u + ((xb >> 4) << 7) + ((y & 7u) << 4) + (xb & 15u) : y * rb + xb;
}

static void gen_level(const TcPspTex *pl, int j, const uint8_t *src, uint8_t *lvl)
{
    const TcPspLevel *P = &pl->level[j], *Q = &pl->level[j - 1];
    const uint32_t rb = P->bytes / P->rows, qrb = Q->bytes / Q->rows, bpp = gu_bpp[pl->psm];
    const uint32_t sx = Q->width > P->width ? 2u : 1u, sy = Q->height > P->height ? 2u : 1u;
    for (uint32_t y = 0; y < P->height; y++)
        for (uint32_t x = 0; x < P->width; x++) {
            uint32_t qx = x * sx, qxb = qx * bpp / 8u, xb = x * bpp / 8u, v;
            const uint8_t *s = src + psp_off(pl, qrb, qxb, y * sy);
            switch (bpp) {
            case 4: v = (qx & 1u) ? (uint32_t)(*s >> 4) : (uint32_t)(*s & 15u); break;
            case 8: v = *s; break;
            case 16: v = rd16(s); break;
            default: v = rd32(s); break;
            }
            uint32_t a = psp_off(pl, rb, xb, y);
            switch (bpp) {
            case 4: lvl[a] = (uint8_t)(x & 1u ? (uint32_t)(lvl[a] & 15u) | v << 4 : (uint32_t)(lvl[a] & 0xF0u) | v); break;
            case 8: lvl[a] = (uint8_t)v; break;
            case 16: wr16(lvl + a, v); break;
            default: wr32(lvl + a, v); break;
            }
        }
}

/* a generated level the down_t8 / down_t4 fast path can take */
static int fast_down(const TcPspTex *pl, int j)
{
    const TcPspLevel *P = &pl->level[j], *Q = &pl->level[j - 1];
    if (!pl->swizzled || Q->width != 2u * P->width || Q->height != 2u * P->height) return 0;
    if (pl->psm == TC_GU_T8) return P->width >= 16 && Q->bytes == (uint32_t)Q->width * Q->rows;
    if (pl->psm == TC_GU_T4) return P->width >= 32 && 2u * Q->bytes == (uint32_t)Q->width * Q->rows;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ convert */
static int fast_level(const TcPs2Info *in, const TcPspTex *pl, int j)
{
    const TcPspLevel *P = &pl->level[j];
    const TcPs2Level *L = &in->level[pl->first + j];
    if (pl->shift || !pl->swizzled || ((uintptr_t)L->data & 3u) || (P->height & 3u)) return 0;
    switch (in->kind) {
    case TC_KIND_PAL8: return L->swizzled && P->width >= 16 && L->sw == P->width;
    case TC_KIND_PAL4: return L->swizzled && P->width >= 32 && L->sw == P->width;
    case TC_KIND_RGBA32: return pl->psm == TC_GU_8888 && P->width >= 4 && L->sw == P->width;
    default: return 0;
    }
}

/* a level the 2:1 decimation fast path can take (half_t8 / half_t4) */
static int fast_half(const TcPs2Info *in, const TcPspTex *pl, int j)
{
    const TcPspLevel *P = &pl->level[j];
    const TcPs2Level *L = &in->level[pl->first + j];
    if (pl->shift != 1 || j != 0 || !pl->swizzled || !L->swizzled || ((uintptr_t)L->data & 3u)) return 0;
    if ((L->h & 3u) || L->sh != L->h || L->sw != L->w || 2u * P->width != L->w || 2u * P->height != L->h) return 0;
    switch (in->kind) {
    case TC_KIND_PAL8: return pl->psm == TC_GU_T8 && L->w >= 32;
    case TC_KIND_PAL4: return pl->psm == TC_GU_T4 && L->w >= 64;
    default: return 0;
    }
}

int tc_convert(const TcPs2Info *in, const TcPolicy *pol, TcPspTex *pl, void *dst, uint32_t dst_size)
{
    if (!in || !pol || !pl || !dst || dst_size < pl->total_bytes || ((uintptr_t)dst & 15u)) return TC_ERR_ARG;
    if (pl->ngen >= pl->nlevels || pl->first + pl->nlevels - pl->ngen > in->nlevels || !pl->nlevels) return TC_ERR_ARG;
    uint8_t *d = (uint8_t *)dst;
    int generic_used = 0;
    uint32_t done = 0;                                 /* bytes of dst written so far (gaps zeroed) */
    for (int j = 0; j < pl->nlevels; j++) {
        const TcPspLevel *P = &pl->level[j];
        uint8_t *lvl = d + P->offset;
        if (P->offset > done) memset(d + done, 0, P->offset - done);
        if (j >= pl->nlevels - pl->ngen) {             /* generated from level j - 1 (already written) */
            const uint8_t *src = d + pl->level[j - 1].offset;
            int fd = !(pol->flags & TC_F_GENERIC) && fast_down(pl, j);
            if (!fd || P->rows != P->height) memset(lvl, 0, P->bytes);
            if (fd) {
                if (pl->psm == TC_GU_T8) down_t8(src, lvl, P->width, P->height);
                else down_t4(src, lvl, P->width, P->height);
            } else {
                gen_level(pl, j, src, lvl);
                generic_used = 1;
            }
            done = P->offset + P->bytes;
            continue;
        }
        int fast = !(pol->flags & TC_F_GENERIC) && fast_level(in, pl, j);
        int half = !fast && !(pol->flags & TC_F_GENERIC) && fast_half(in, pl, j);
        if ((!fast && !half) || P->rows != P->height || row_bytes(pl->psm, P->width) != P->bytes / P->rows)
            memset(lvl, 0, P->bytes);                  /* padding rows / bytes, or nibble-wise writes */
        if (half) {
            const TcPs2Level *L = &in->level[pl->first + j];
            if (in->kind == TC_KIND_PAL8) half_t8(L->data, lvl, L->w, L->h);
            else half_t4(L->data, lvl, L->w, L->h);
        } else if (fast) {
            const TcPs2Level *L = &in->level[pl->first + j];
            switch (in->kind) {
            case TC_KIND_PAL8: fast_t8(L->data, lvl, P->width, P->height); break;
            case TC_KIND_PAL4: fast_t4(L->data, lvl, P->width, P->height); break;
            default: fast_8888(L->data, lvl, P->width, P->height, L->sw); break;
            }
        } else {
            generic_level(in, pl, j, lvl);
            generic_used = 1;
        }
        done = P->offset + P->bytes;
    }
    if (pl->clut_psm != TC_GU_NONE) {
        if (pl->clut_offset > done) memset(d + done, 0, pl->clut_offset - done);
        uint8_t *c = d + pl->clut_offset;
        const uint32_t n = pl->clut_entries;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t s = n == 256 ? csm1(i) : i;
            if (pl->clut_psm == TC_GU_8888) wr32(c + 4u * i, conv8888(rd32(in->clut + 4u * s)));
            else wr16(c + 2u * i, rd16(in->clut + 2u * s));
        }
        done = pl->clut_offset + pl->clut_bytes;
    }
    if (pl->total_bytes > done) memset(d + done, 0, pl->total_bytes - done);
    pl->path = generic_used ? 2 : 1;
    if (pol->alpha == TC_AC_NONE) {
        pl->alpha_class = TC_ALPHA_UNKNOWN;
    } else {
        int ac = alpha_class_all(in);
        if (pol->alpha == TC_AC_USED && in->clut_entries && ac != TC_ALPHA_OPAQUE) ac = alpha_class_used(in);
        pl->alpha_class = (uint8_t)ac;
    }
    return TC_OK;
}

/* ------------------------------------------------------------------------------------------------ cost */
void tc_cost(const TcPs2Info *in, const TcPspTex *pl, const TcPolicy *pol, TcCost *o)
{
    memset(o, 0, sizeof *o);
    int fast = 1;
    for (int j = 0; j < pl->nlevels; j++) {
        if (j >= pl->nlevels - pl->ngen) {             /* generated from the previous PSP level */
            o->texels += (uint32_t)pl->level[j].width * pl->level[j].height;
            if ((pol->flags & TC_F_GENERIC) || !fast_down(pl, j)) fast = 0;
            continue;
        }
        const TcPs2Level *L = &in->level[pl->first + j];
        o->src_bytes += L->bytes;
        o->texels += (uint32_t)pl->level[j].width * pl->level[j].height;
        if ((pol->flags & TC_F_GENERIC) || !(fast_level(in, pl, j) || fast_half(in, pl, j))) fast = 0;
    }
    o->src_bytes += pl->clut_entries ? (uint32_t)pl->clut_entries * in->clut_bits / 8u : 0u;
    o->dst_bytes = pl->total_bytes;
    o->ps2_bytes = in->ps2_bytes;
    o->path = (uint8_t)(fast ? 1 : 2);
}
