/* hle_ge.h - PSP GE back-end of the render driver boundary.
 *
 * hle_render.c traps the render driver's entry points; on the PSP its hooks hand the 2D path to this module, which
 * draws it with the GE (sceGu, through mode): the 2D draw functions' primitives, the game's render buffer type 0 (font
 * glyphs), camera clears, and the flip at drv_raster_show - the same semantics as the host's software rasteriser
 * (hle_raster.c). Guest rasters become GE textures through texconv (cache keyed by the
 * raster address + an upload version bumped on create / texture read / unlock / destroy). The debug log keeps showing:
 * the screen text is recorded (pspDebugScreenPrintf is wrapped at link time) and drawn over the game picture; the
 * end of the run, a crash and a stall (no flip for 3 s) switch the screen back to the plain text console.
 * ps2rc.cfg: render=ge|text (default ge), ge_text=N overlay rows (12), ge_tex_kb=N texture arena (2048),
 * im2d_guest=0|1 (1: the translated original 2D draw functions still run), ge_clear=0|1, ge_dump=flip,flip,... (BMPs). */
#ifndef HLE_GE_H
#define HLE_GE_H
#include <stdint.h>
#include "hle_render.h"

extern int hle_ge_on;             /* render=ge and the GE is initialised */
extern int hle_ge_guest_im2d;     /* run the translated original 2D draw functions as well (default 0) */

int hle_ge_init(void);            /* at boot (hle_render_init): config, memory, GE; 1 = GE mode */
void hle_ge_camera_begin(uint32_t camera, uint32_t fb_raster);
void hle_ge_clear(uint32_t camera, const uint8_t rgba[4], int mode);
/* primitive type 1..6 (the driver's numbering) over 2D vertices (stride 0x40) at verts; indices: guest u16 array
   (gidx), host array (hidx) or none */
void hle_ge_im2d(int type, uint32_t verts, int nverts, uint32_t gidx, const uint16_t *hidx, int nidx,
                 const RndState *st);
void hle_ge_rbuf(uint32_t buf, uint32_t n, const RndState *st);   /* render buffer type 0: n 40-byte entries */
/* the sky (hle_sky.c): n / 3 triangles from host vertices in guest raster coordinates, through mode, no depth */
void hle_ge_tris(const RndVtx *v, int n, const RndState *st);
/* the same with a GE depth per vertex (0 far .. 65535 near) and the driver's Z test / write (world-space sprites) */
void hle_ge_tris_z(const RndVtx *v, const float *z, int n, const RndState *st);
/* n / 3 triangles already in the through-mode GeV layout (float u, v texels; u32 ABGR; float x, y screen,
   z GE depth; 24 bytes), copied into the list as they are; hle_ge_tex_size: the texel size of a raster's GE texture */
void hle_ge_tris_packed(const void *v, int n, const RndState *st, int depth);
int hle_ge_tex_size(uint32_t raster, int *w, int *h);
void hle_ge_flip(uint32_t raster);
void hle_ge_raster_changed(uint32_t raster);   /* create / texture read / unlock / destroy: the upload ends */
int hle_ge_tex_cfg(const char *k, const char *v);   /* tex_half / tex_half_fx / tex_mip* (boot + in-run A/B) */
void hle_ge_report(void);
int hle_ge_shutdown(void);         /* before sceKernelExitGame (finish / reset the GE, sceGuTerm) */
/* the 3D world (psp/hle_ge3d.c): the open frame / list / texture cache of hle_ge.c */
int hle_ge_3d_begin(void);                 /* 1 = the screen is the target and a frame list is open */
int hle_ge_3d_need_state(void);            /* 1 = the 3D state was dropped (list start / 2D draw): send it again */
void hle_ge_3d_room(uint32_t bytes);       /* list room (may finish + sync the list: a "resync") */
uint32_t hle_ge_3d_epoch(void);            /* GE work generation (advances at every list sync) */
int hle_ge_3d_texture(uint32_t raster, int filter, int addr_u, int addr_v, int *aclass);   /* bind; 0 = not drawable */
void hle_ge_3d_blend(int on, int src_blend, int dst_blend);
/* w3d_me: a texture bound later by GE words the Media Engine writes - looked up (may convert / resync)
   and held, not bound; hle_ge_3d_hold(1) starts holding, (0) pins the held ones to the current epoch; hle_ge_3d_forget:
   the GE state changed behind hle_ge.c's caches (a CALLed ME segment) */
/* mipmaps: mx = the highest mip level to bind (0 = level 0 only), lbias = the texture level
   bias in 1/16 levels (signed), mipf = GU minifying filter for a linear texture (GU_LINEAR_MIPMAP_NEAREST / _LINEAR) */
typedef struct { uint32_t ok, aclass, tbp, cbp, psm, swz, w, h, bufw, clut_psm, clut_blocks, mx, lbias, mipf; } HleGeTexInfo;
/* level k of a GE texture laid out by texconv (levels back to back, each 16-byte aligned, rows >= 16
   bytes, rows padded to 8 when swizzled; = tc_plan): byte offset from level 0, texel size and buffer width */
static inline uint32_t hle_ge_mip_level(uint32_t psm, uint32_t swz, uint32_t w0, uint32_t h0, uint32_t k, uint32_t *w,
                                        uint32_t *h, uint32_t *bufw)
{
    uint32_t bpp = psm == 4u ? 4u : psm == 5u ? 8u : psm == 3u ? 32u : 16u, off = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t lw = (w0 >> i) ? (w0 >> i) : 1u, lh = (h0 >> i) ? (h0 >> i) : 1u, rb = lw * bpp / 8u;
        if (rb < 16u) rb = 16u;
        if (i == k) { *w = lw; *h = lh; *bufw = rb * 8u / bpp; return off; }
        off = (off + rb * (swz ? (lh + 7u) & ~7u : lh) + 15u) & ~15u;
    }
}
/* GE minifying filter: the driver's linear (GU_LINEAR) / nearest; with mip levels bound (mx > 0) the mipmap variant
   (linear: mipf, nearest: GU_NEAREST_MIPMAP_NEAREST / _LINEAR); the magnifying filter is bit 0 of it */
static inline uint32_t hle_ge_min_filter(int linear, uint32_t mx, uint32_t mipf)
{
    if (!mx) return linear ? 1u : 0u;
    return linear ? (mipf == 7u ? 7u : 5u) : (mipf == 7u ? 6u : 4u);
}
int hle_ge_3d_tex_info(uint32_t raster, HleGeTexInfo *ti);
void hle_ge_3d_hold(int on);
void hle_ge_3d_forget(void);
/* FMV pictures (psp/hle_movie.c): a movie owns the screen between begin (1 = GE display available) and end
   (a black frame); each picture = 480x272 in a 512-wide RAM buffer (RGBA 8888 or RGB 565), shown at once */
int hle_ge_movie_begin(void);
void hle_ge_movie_frame(const void *pic, int rgba8888, uint32_t dump);   /* dump: BMP tag, 0 = none */
void hle_ge_movie_end(void);
/* GE serial probe categories (ps2rc.cfg ge_ser) - the entry points mark what they
   draw; on a probe frame a change of category cuts the list (sent + waited for: the GE time of that part) */
enum { GS_OTHER, GS_CLEAR, GS_2D, GS_W3D, GS_W3DM, GS_FX, GS_PFX, GS_FLIP, GS_N };
void hle_ge_ser_mark(int cat);
uint64_t hle_ge_open_us(void);             /* frame list open time so far (first draw -> flip done), us */
double hle_ge_ser_ms(int v);               /* GE serial ms per probe frame of A/B variant v (4 = no A/B); -1 none */

#endif
