/* hle_ge_prof.c - profiler zones around the render boundary's GE back-end (kept out of the core hle_prof_psp.c).
 * The game pack's [build] psp_ldflags link with -Wl,--wrap=hle_ge_<fn>. */
#include "hle.h"
#include "hle_prof.h"
#include "hle_ge.h"

/* zones around the GE back-end (hle_ge.c, called from hle_render.c) without touching either file: the link uses
   -Wl,--wrap=hle_ge_<fn>, so every call from hle_render.o reaches these wrappers. ge.flip holds the list finish +
   sceGuSync = the time the CPU waits for the GE to finish drawing. */
void __real_hle_ge_flip(uint32_t raster);
void __real_hle_ge_im2d(int type, uint32_t verts, int nverts, uint32_t gidx, const uint16_t *hidx, int nidx,
                        const RndState *st);
void __real_hle_ge_rbuf(uint32_t buf, uint32_t n, const RndState *st);
void __real_hle_ge_clear(uint32_t camera, const uint8_t rgba[4], int mode);
void __real_hle_ge_camera_begin(uint32_t camera, uint32_t fb_raster);
void __wrap_hle_ge_flip(uint32_t raster) { PZ_SCOPE(ge_flip); __real_hle_ge_flip(raster); }
void __wrap_hle_ge_im2d(int type, uint32_t verts, int nverts, uint32_t gidx, const uint16_t *hidx, int nidx,
                        const RndState *st)
{
    PZ_SCOPE(ge_draw);
    __real_hle_ge_im2d(type, verts, nverts, gidx, hidx, nidx, st);
}
void __wrap_hle_ge_rbuf(uint32_t buf, uint32_t n, const RndState *st) { PZ_SCOPE(ge_draw); __real_hle_ge_rbuf(buf, n, st); }
void __wrap_hle_ge_clear(uint32_t camera, const uint8_t rgba[4], int mode)
{
    PZ_SCOPE(ge_draw);
    __real_hle_ge_clear(camera, rgba, mode);
}
void __wrap_hle_ge_camera_begin(uint32_t camera, uint32_t fb_raster)
{
    PZ_SCOPE(ge_draw);
    __real_hle_ge_camera_begin(camera, fb_raster);
}
