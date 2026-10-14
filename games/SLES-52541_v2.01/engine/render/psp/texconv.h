/* texconv.h - the render driver's PS2 native raster -> PSP GE texture.
 *
 * C99, no OS calls, no allocation, no globals written: the caller owns every buffer, so the library runs in the
 * host harnesses, in a PSP worker thread or inside the HLE render driver. Builds with host gcc and psp-gcc.
 *
 * INPUT - a PS2 raster exactly as the game's render driver keeps it after streaming it in ("new style" rasters,
 * stream raster version 2 = every one of the game's 32,525 textures, measured over all its texture dictionaries):
 *
 *   pixel block (stream pixelSize bytes), per mip level k = 0 .. MXL:
 *     +0x00 GIF tag  NLOOP 3, FLG PACKED, NREG 1, REGS A+D (0xE)
 *     +0x10 TRXPOS   (reg 0x51)          +0x20 TRXREG (reg 0x52: RRW bits 0-11, RRH bits 32-43)
 *     +0x30 TRXDIR   (reg 0x53)          +0x40 GIF tag FLG IMAGE, NLOOP = data bytes / 16
 *     +0x50 image data: align16(sw * sh * depth / 8) bytes, sw = max(w_k, minw), sh = max(h_k, minh)
 *   palette block (stream paletteSize bytes, palettised rasters only): the same 0x50 bytes, then the CLUT image
 *     (PAL8: 16x16 entries in GS CSM1 order; PAL4: 8x3 entries, the first 16 used), entries RGBA8888 with
 *     alpha 0..0x80 (0x80 = opaque) or 1555.
 *   PSMT8 levels are pre-swizzled for a PSMCT32 upload of (sw/2 x sh/2), PSMT4 levels for a PSMCT16 upload of
 *   (sw/2 x sh/2) (minw x minh = 16x4 / 32x4); the TRXREG of each level says which (both are handled).
 *   PSMCT32 levels are linear RGBA8888 (alpha 0..0x80).
 *
 *   In guest memory after the translated drv_texture_read (0x357BF0): the raster's pixel pointer (+0x04) points 0x50
 *   bytes into the level-0 upload (= pixel block + 0x50), its palette pointer (+0x08) 0x50 bytes into the palette
 *   upload; both blocks are byte-identical to the texture dictionary stream's (the reader copies them verbatim; both
 *   pointers were checked against their IMAGE tags at run time on a PSP). Width, height, depth at
 *   +0x0C/+0x10/+0x14, format byte (format >> 8) at +0x23. tc_ps2_from_packets() rebuilds the input from those
 *   pointers by walking and validating the uploads.
 *
 * OUTPUT - one buffer per texture, every part 16-byte aligned (GE: TBP/CBP in 16-byte units):
 *     [level 0 texels][level 1] ... [level n-1][CLUT]
 *   PAL4 -> GU_PSM_T4, PAL8 -> GU_PSM_T8 (indices kept, CSM1 order undone), CLUT GU_PSM_8888 (alpha 0..0x80 ->
 *   min(255, 2a)) or GU_PSM_5551 (PS2 1555 entries copied: same bit layout); RGBA32 -> GU_PSM_8888 (or 5650/5551/
 *   4444 by alpha class with TC_D32_16); 1555 -> GU_PSM_5551. Texels in the GE's Fast-mode ("swizzled") order -
 *   16-byte x 8-row blocks, height padded to 8 rows - whenever a row is at least 16 bytes (texture width == buffer
 *   width); narrower textures are linear with the row padded to 16 bytes. Draw with sceGuTexMode(psm, nlevels - 1,
 *   0, swizzled), sceGuTexImage(k, w, h, bufw, base + level[k].offset), sceGuClutMode(clut_psm, 0, 0xFF, 0),
 *   sceGuClutLoad(n, base + clut_offset) with n = clut_entries / 8 for 8888 CLUTs and clut_entries / 16 for 16-bit
 *   ones (the GE loads the CLUT in 32-byte units; PPSSPP and the PSP agree). Write the
 *   buffer back from the D-cache (sceKernelDcacheWritebackRange) before the GE reads it.
 */
#ifndef TEXCONV_H
#define TEXCONV_H

#include <stddef.h>
#include <stdint.h>

#define TC_VERSION 1

/* results */
#define TC_OK 0
#define TC_ERR_ARG (-1)      /* NULL pointer / buffer too small / misaligned destination */
#define TC_ERR_FORMAT (-2)   /* raster format not supported (stream version < 2, 24-bit, LUM8 ...) */
#define TC_ERR_PACKET (-3)   /* GIF uploads do not describe the raster (tags, TRXREG, sizes, bounds) */
#define TC_ERR_SIZE (-4)     /* larger than policy.max_dim and no PS2 level / decimation allowed */
#define TC_ERR_CHUNK (-5)    /* stream chunk layout (texture dictionary / native texture) not as expected */

/* the driver's raster format bits (the raster's format byte << 8) */
#define TC_PF_1555 0x0100u
#define TC_PF_565 0x0200u
#define TC_PF_4444 0x0300u
#define TC_PF_LUM8 0x0400u
#define TC_PF_8888 0x0500u
#define TC_PF_888 0x0600u
#define TC_PF_555 0x0A00u
#define TC_PF_PIXEL_MASK 0x0F00u
#define TC_PF_AUTOMIPMAP 0x1000u
#define TC_PF_PAL8 0x2000u
#define TC_PF_PAL4 0x4000u
#define TC_PF_MIPMAP 0x8000u

/* GS pixel storage modes */
#define TC_GS_PSMCT32 0x00
#define TC_GS_PSMCT16 0x02
#define TC_GS_PSMCT16S 0x0A
#define TC_GS_PSMT8 0x13
#define TC_GS_PSMT4 0x14

/* PSP GE formats (pspgu.h GU_PSM_*) */
#define TC_GU_5650 0
#define TC_GU_5551 1
#define TC_GU_4444 2
#define TC_GU_8888 3
#define TC_GU_T4 4
#define TC_GU_T8 5
#define TC_GU_NONE 0xFF

/* input kinds */
#define TC_KIND_PAL4 1
#define TC_KIND_PAL8 2
#define TC_KIND_RGBA32 3     /* 8888 (alpha 0..0x80) */
#define TC_KIND_RGB32 4      /* 888 stored as 32-bit, alpha ignored */
#define TC_KIND_RGBA16 5     /* 1555 / 555 */

/* alpha classes (of what level 0 uses, PS2 scale): every alpha >= 0x80 / only 0 or >= 0x80 / anything else */
#define TC_ALPHA_OPAQUE 0
#define TC_ALPHA_CUTOUT 1
#define TC_ALPHA_BLEND 2
#define TC_ALPHA_UNKNOWN 0xFF

/* policy.alpha: how the alpha class is found */
#define TC_AC_NONE 0         /* not computed (TC_ALPHA_UNKNOWN) */
#define TC_AC_CLUT 1         /* from every CLUT entry (or every texel of a direct-colour texture): cheap and
                                conservative - never reports a lower class than the texels use (default) */
#define TC_AC_USED 2         /* exact: from the CLUT entries level 0 really uses (an extra pass over the stored
                                indices when some CLUT entry is not opaque; measured over all the game's textures,
                                9,464 are opaque by use but not by CLUT) */

/* policy.direct32 */
#define TC_D32_8888 0        /* RGBA32 -> GU_PSM_8888 (exact apart from the alpha scaling) */
#define TC_D32_16 1          /* RGBA32 -> 5650 (opaque) / 5551 (cutout) / 4444 (blend), truncating */

/* policy.flags */
#define TC_F_GENERIC 0x01    /* force the per-texel path (tests: must give the same bytes as the fast paths) */
#define TC_F_GENMIP 0x02     /* after the kept levels, generate more down to the swizzle floor (or 1x1 when
                                not swizzled), each a 2:1 point decimation of the previous PSP level (even texels of
                                even rows; CLUT indices kept, no requantising), up to max_levels */

#define TC_MAX_PS2_LEVELS 7
#define TC_MAX_PSP_LEVELS 8

/* ------------------------------------------------------------------------------------------------ input */
typedef struct TcPs2Raster {
    int32_t width, height, depth;  /* raster width / height / depth (4, 8, 16, 32) */
    uint32_t format;               /* raster format: TC_PF_* bits (| raster type in bits 0-2) */
    int32_t version;               /* stream raster version; 2 = GIF uploads (the only layout supported) */
    uint32_t nlevels;              /* stored mip levels (TEX1 MXL + 1); 0 = as many as the uploads validate */
    uint64_t tex0;                 /* GS TEX0 as stored (informative; PSM/CPSM are derived from depth/format) */
    uint32_t mipmap_kl;            /* K/L (TEX1 bits), informative */
    const uint8_t *pix;            /* pixel block (see above) */
    uint32_t pix_size;
    const uint8_t *pal;            /* palette block, NULL when not palettised */
    uint32_t pal_size;
} TcPs2Raster;

typedef struct TcPs2Level {
    const uint8_t *data;           /* image data of the upload */
    uint32_t bytes;                /* image data bytes (IMAGE NLOOP x 16) */
    uint16_t w, h;                 /* level size in texels */
    uint16_t sw, sh;               /* stored size (min-transfer clamped), texels */
    uint8_t swizzled;              /* stored pre-swizzled (TRXREG halved) */
    uint8_t pad_[3];
} TcPs2Level;

typedef struct TcPs2Info {
    int32_t width, height, depth;
    uint32_t format;
    uint8_t kind;                  /* TC_KIND_* */
    uint8_t psm;                   /* TC_GS_* */
    uint8_t nlevels;
    uint8_t clut_bits;             /* 32 / 16 / 0 */
    uint16_t clut_entries;         /* 256 / 16 / 0 */
    uint16_t pad_;
    const uint8_t *clut;           /* CLUT image data (PS2 order) */
    uint32_t clut_bytes;
    uint32_t ps2_bytes;            /* pixel block + palette block bytes (what the driver keeps per raster, without
                                      its DMA-chain header) */
    TcPs2Level level[TC_MAX_PS2_LEVELS];
} TcPs2Info;

/* Validate a raster and describe its levels / CLUT. Every read stays inside pix/pal + their sizes. */
int tc_ps2_inspect(const TcPs2Raster *r, TcPs2Info *out);

/* Rebuild a TcPs2Raster from guest memory left by the translated drv_texture_read:
 * level0_upload = the raster's pixel pointer (+0x04) - 0x50, avail = readable bytes from there, palette_upload = its
 * palette pointer (+0x08) - 0x50 (NULL if not palettised; its size is derived from the format). Levels are walked while their uploads match
 * the expected TRXREG/IMAGE sizes (at most 7, stopping at palette_upload). */
int tc_ps2_from_packets(const uint8_t *level0_upload, uint32_t avail, const uint8_t *palette_upload, int32_t width,
                        int32_t height, int32_t depth, uint32_t format, TcPs2Raster *out);

/* ------------------------------------------------------------------------------------------------ stream chunks */
typedef struct TcNative {
    char name[32], mask[32];
    uint32_t filter_addressing;    /* the texture's filter / addressing word: filter bits 0-7, U addressing 8-11,
                                      V 12-15 */
    uint32_t drv_mipmap;           /* mipmap K/L value extension (0x110) */
    uint8_t has_drv_mipmap;
    uint8_t pad_[3];
    TcPs2Raster raster;            /* points into the chunk */
} TcNative;

/* Parse one native texture chunk (type 0x15, starting at its 12-byte chunk header). */
int tc_parse_native(const uint8_t *chunk, uint32_t size, TcNative *out);

typedef struct TcDictIter {
    const uint8_t *p, *end;
} TcDictIter;
/* Start iterating a texture dictionary (0x16) blob; *count = the dictionary's texture count. */
int tc_dict_begin(const uint8_t *dict, uint32_t size, TcDictIter *it, uint32_t *count);
/* Next native texture chunk: 1 = found, 0 = end, < 0 = error. */
int tc_dict_next(TcDictIter *it, const uint8_t **chunk, uint32_t *chunk_size);

/* ------------------------------------------------------------------------------------------------ policy */
typedef struct TcPolicy {
    uint16_t max_dim;              /* GE limit 512 */
    uint8_t max_levels;            /* PSP levels kept, 1..8 */
    uint8_t direct32;              /* TC_D32_* */
    uint32_t cache_bytes;          /* drop PS2 top mip levels (no resampling) while level 0 > this; 0 = off */
    uint8_t swizzle;               /* 1: Fast mode whenever a row is >= 16 bytes */
    uint8_t decimate;              /* 1: 2:1 point decimation when no stored level is <= max_dim */
    uint8_t alpha;                 /* TC_AC_* */
    uint8_t flags;                 /* TC_F_* */
} TcPolicy;

/* max_dim 512, max_levels 8, cache_bytes 8192 (the GE texture cache), Fast mode, TC_D32_8888, TC_AC_CLUT */
void tc_policy_default(TcPolicy *p);

/* ------------------------------------------------------------------------------------------------ output */
typedef struct TcPspLevel {
    uint32_t offset, bytes;        /* from the buffer start */
    uint16_t width, height;        /* texels (sceGuTexImage w, h) */
    uint16_t bufw;                 /* buffer width in texels (sceGuTexImage tbw) */
    uint16_t rows;                 /* stored rows (height padded to 8 when swizzled) */
} TcPspLevel;

typedef struct TcPspTex {
    uint8_t psm;                   /* TC_GU_* texel format */
    uint8_t clut_psm;              /* TC_GU_8888 / TC_GU_5551 / TC_GU_NONE */
    uint8_t swizzled;              /* sceGuTexMode swizzle argument */
    uint8_t nlevels;               /* sceGuTexMode maxmips = nlevels - 1 */
    uint8_t first;                 /* PS2 level that became PSP level 0 (cache / size policy) */
    uint8_t shift;                 /* extra 2:1 decimations (policy.decimate) */
    uint8_t alpha_class;           /* TC_ALPHA_* (filled by tc_convert) */
    uint8_t path;                  /* 1 = fast path, 2 = per-texel path (filled by tc_convert) */
    uint16_t clut_entries;
    uint8_t ngen;                  /* the last ngen levels are generated (TC_F_GENMIP), not PS2 levels */
    uint8_t pad_;
    uint32_t clut_offset, clut_bytes;
    uint32_t total_bytes;          /* buffer size, multiple of 16 */
    TcPspLevel level[TC_MAX_PSP_LEVELS];
} TcPspTex;

/* Decide format, levels and layout (no texel is touched, except the alpha scan TC_D32_16 needs). */
int tc_plan(const TcPs2Info *in, const TcPolicy *pol, TcPspTex *out);

/* Fill dst (total_bytes, 16-byte aligned, not overlapping the source) as planned; sets alpha_class and path.
 * Padding bytes (row / block / alignment padding) are written as zero: the output is deterministic. */
int tc_convert(const TcPs2Info *in, const TcPolicy *pol, TcPspTex *plan, void *dst, uint32_t dst_size);

/* ------------------------------------------------------------------------------------------------ cost model */
typedef struct TcCost {
    uint32_t src_bytes;            /* PS2 image + CLUT bytes read */
    uint32_t dst_bytes;            /* PSP bytes written */
    uint32_t texels;               /* PSP texels produced */
    uint32_t ps2_bytes;            /* PS2 raster bytes (pixel + palette blocks) */
    uint8_t path;                  /* 1 fast / 2 per-texel (predicted) */
    uint8_t pad_[3];
} TcCost;
void tc_cost(const TcPs2Info *in, const TcPspTex *plan, const TcPolicy *pol, TcCost *out);

/* PS2 alpha (0..0x80 = opaque) -> 0..255 */
static inline uint8_t tc_alpha_ps2_to_psp(uint32_t a) { return (uint8_t)(a >= 0x80u ? 0xFFu : a << 1); }

#endif
