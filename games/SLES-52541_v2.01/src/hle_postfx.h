/* hle_postfx.h - the PS2 post-processing colour filter and glow, drawn on the PSP GE.
 *
 * Hook postfx_colour 0x51A760(c1, c2): called once per gameplay frame by the post effects 0x514300 (the last call of
 * render_effects 0x246FD0, after the 3D scene and before the HUD). c1 / c2 are two RGBA byte colours (the
 * time-of-day colour cycle's two post-effect colours, floats at 0x7074B8 / 0x7074C8, scaled by the game's lighting
 * factor and forced to 64 grey under night / infrared vision). On the PS2 the function builds GS packets that copy the
 * frame and blend it back twice; the net result is the frame multiplied per channel by
 *   K = (c1 + c2 x a2 / 128) / 128, clamped at 255
 * (within 3.5 / 255 of the GS arithmetic for every row of the colour cycle data).
 * Our code takes c1 / c2 from the arguments and multiplies the picture by K at that point of the frame (PSP:
 * hle_ge_postfx_mul, a full-screen untextured sprite with blend Cd x src + Cd x FIX, one pass per factor 2; host:
 * hle_raster_postfx_mul, the same GE blend arithmetic on the PNG model), so the 3D world, sky and effects are graded
 * and the HUD / menus drawn later are not (movies and loading screens never reach it).
 * cfg (PSP cfg file) / environment (host): postfx=0|1 (HLE_POSTFX, default 1: 0 = no filter),
 * postfx_filter=ps2|off (HLE_POSTFX_FILTER), postfx_guest=0|1 (HLE_POSTFX_GUEST, default 0; 1 = also run the
 * translated original, which only builds GS packets the HLE drops - a guest timeline change), postfx_dither=0|1
 * (HLE_POSTFX_DITHER, PSP: dither the multiply pass, default 1), postfx_log=N (HLE_POSTFX_LOG: log the first N calls;
 * host: also decode the guest's GS packets of those calls).
 * Hook postfx_glow 0x518280(limit, filter passes, render passes, intensity): the PS2's soft glow of bright areas (on
 * by default on the PS2) is drawn as well: postfx_radiosity=0|1 (HLE_POSTFX_RADIOSITY, default 1; PSP: 3 small
 * render-to-texture passes in VRAM 0x198000..0x1AD500 + one full-screen additive pass), postfx_rad_fence=0|1 (default
 * 0: in-list TEXFLUSH; 1 = finish and sync the GE list before each render target is sampled, a fallback),
 * postfx_rad_guest=0|1 (default 0; 1 = also run the translated original).
 * postfx_ge_probe=N (PSP, default 64): every N-th call of each effect is bracketed by two GE list syncs and the GE time
 * of the pass is measured (log line "postfx GE time"); 0 = never. */
#ifndef HLE_POSTFX_H
#define HLE_POSTFX_H
#include <stdint.h>

extern int pfx_opt_on, pfx_opt_filter, pfx_opt_guest, pfx_opt_dither, pfx_opt_log;
extern int pfx_opt_radiosity, pfx_opt_rad_fence, pfx_opt_rad_guest, pfx_opt_ge_probe;
void hle_postfx_init(void);                    /* hle_render_init: options (PSP: the cfg file, host: environment) */
void hle_postfx_report(void);                  /* hle_render_report */

/* the drawing back-ends: the picture multiplied per channel by k[0..2] (R, G, B; 0..8, clamped at 255); returns the
   number of full-screen passes drawn (0 = nothing: identity, not the screen, GE off) */
int hle_ge_postfx_mul(const float k[3], int dither);    /* PSP, hle_ge.c */
int hle_raster_postfx_mul(const float k[3]);            /* host model, hle_raster.c */
/* the PS2 glow 0x518280(limit, filter passes, render passes, intensity); returns the composite passes drawn */
int hle_ge_postfx_radiosity(int limit, int filter_passes, int render_passes, int intensity, int fence);   /* PSP */
int hle_raster_postfx_radiosity(int limit, int filter_passes, int render_passes, int intensity);         /* host */
uint32_t hle_ge_postfx_probe(void);   /* PSP: finish + sync the open list, returns the wait in us */

#endif
