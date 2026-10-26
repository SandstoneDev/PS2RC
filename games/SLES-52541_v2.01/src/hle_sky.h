/* hle_sky.h - the sky gradient and the render buffer's world-space primitives, drawn on the PSP GE.
 *
 * The PS2 frame clears only Z; the colour buffer is covered every frame by the sky gradient that game_loop 0x246760
 * draws first: sky_polys 0x2EDE60 writes 5 world-space quads around the camera (the colour cycle's sky top / bottom
 * colours) into the immediate-mode 3D temp buffer and hands it to buffer_render 0x2F5DE0 (an indexed triangle list).
 * Our hook of buffer_render, when called inside sky_polys (hooked too, as a scope flag), transforms the buffer with
 * the camera's view matrix on the CPU, clips it (near plane + a guard band, in view space) and draws Gouraud
 * triangles in screen space with Z off (host: the 2D rasteriser hle_raster_tris; PSP: hle_ge_tris in hle_ge.c), then
 * runs the guest original (or, with sky_guest=0, only what it does to guest memory).
 * hle_sky_rbuf() (called by the batch_flush hook in hle_render.c) draws the render buffer's world-space primitives
 * with the flush's render state (host: hle_raster_tris3d with the 3D world's Z; PSP: hle_ge_tris_z / packed
 * triangles): type 1 sprites (coronas incl. the sun, low clouds), types 8 / 9 triangles, types 3 / 4 / 5 / 7 particles.
 * cfg (PSP cfg file) / environment HLE_<KEY> (host): sky=0|1 (default 1), sky_guest=0|1 (default 0), sky_sprites=0|1
 * (default 1), sky_tris=0|1 (default 1: render-buffer types 8 / 9 = world-space triangles: shadows, headlight light
 * pools, skid marks, glass; sky_split=N: perspective splits of big triangles, default 0), sky_fx=0|1 (default 1:
 * render-buffer types 3 / 4 / 5 / 7 = effect particles: fire, explosions, sparks, smoke, dust, splashes),
 * sky_clear_once=0|1 (default 1: only the frame start's Z clear fills the fog colour), sky_far=0|1 (also clip the sky
 * at the camera's far plane, default 0), sky_log=N (vertex dumps of the first N sky buffers, default 0 - on the PSP
 * every log line is a write-through to the memory stick; N >= 3 also logs 4N type-1 flushes from flip sky_log_flip
 * on). */
#ifndef HLE_SKY_H
#define HLE_SKY_H
#include <stdint.h>

extern int sky_opt_on, sky_opt_guest, sky_opt_log, sky_opt_far, sky_opt_sprites, sky_opt_log_flip, sky_opt_tris,
    sky_opt_split, sky_opt_fx, sky_opt_clear_once;
void hle_sky_cfg(const char *k, const char *v);   /* PSP cfg keys sky* (hle_ge.c read_cfg) */
void hle_sky_init(void);                           /* hle_render_init: options (host: environment) */
void hle_sky_report(void);                         /* hle_render_report */
/* hle_render.c batch_flush 0x3C50F0, before the guest: counts every type, draws types 1, 3 / 4 / 5 / 7 and 8 / 9 */
void hle_sky_rbuf(uint32_t buf, uint32_t n, int type);
/* called for every clear of the screen camera that includes Z (mode & 2, w3d_sky on): 1 = also fill the picture with
   the fog colour (the frame start's Z clear only); 0 = depth only (a mid-frame Z clear: the heat-haze mask pass
   0x517570 of fire / explosions - filling there would cover the finished picture with the fog colour, a white
   screen). cfg sky_clear_once=0 / host HLE_SKY_CLEAR_ONCE=0: every Z-only clear fills. */
int hle_sky_clear_fill(int mode);
/* hle_ce.c x_cbatch: the corona batch - on: type-1 sprites stay pending across flushes (same camera and
   render state); off: draws what is pending. hle_render.c drv_set_state calls hle_sky_spr_state() before a render
   state changes while hle_sky_spr_pend is set (the pending sprites are drawn with the state they were queued with). */
void hle_sky_spr_batch(int on);
void hle_sky_spr_state(void);
extern int hle_sky_spr_pend;
void hle_ce_report(void) __attribute__((weak));   /* night levers summary (hle_ce.c) */

#endif
