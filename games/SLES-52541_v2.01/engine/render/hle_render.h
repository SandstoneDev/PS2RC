/* hle_render.h - the render boundary of the game's render driver: trap-and-log hooks at the driver entry points
 * (hle_render.c, both targets) and, on the host, a capture of the per-frame 2D draws (2D primitives, the game's
 * render buffer type 0 = font glyphs) that a software rasteriser (host/hle_raster.c, host only) turns into PNG frames.
 *
 * Coordinates are the guest's screen coordinates (the camera frame-buffer raster, e.g. 640x448); colours are 0..255,
 * texture alpha is already scaled from the PS2 0..0x80 range to 0..255. */
#ifndef HLE_RENDER_H
#define HLE_RENDER_H
#include <stdint.h>

/* render-state ids of drv_set_state that the 2D rasteriser uses (the driver's render-state enum) */
enum {
    RS_TEXTURERASTER = 1, RS_TEXTUREADDRESS = 2, RS_TEXTUREADDRESSU = 3, RS_TEXTUREADDRESSV = 4,
    RS_ZTESTENABLE = 6, RS_SHADEMODE = 7, RS_ZWRITEENABLE = 8, RS_TEXTUREFILTER = 9, RS_SRCBLEND = 10,
    RS_DESTBLEND = 11, RS_VERTEXALPHAENABLE = 12, RS_FOGENABLE = 14, RS_FOGCOLOR = 15, RS_CULLMODE = 20,
    RS_COUNT = 32
};

typedef struct {
    float x, y;          /* screen position */
    float u, v;          /* texture coordinates (0..1 over the raster) */
    float r, g, b, a;    /* vertex colour 0..255 */
} RndVtx;

typedef struct {
    uint32_t raster;     /* guest address of the driver's raster (0 = untextured) */
    int src_blend, dst_blend, filter, addr_u, addr_v, vertex_alpha;
} RndState;

/* host only: 3D triangles of the 3D world (hle_w3d_host.c's GE model): normalized device coordinates (x, y in
   -1..1, y up, as after the GE projection), GE depth 0..65535 (bigger = nearer: sceGuDepthRange(65535, 0) + GEQUAL),
   q = 1 / w for perspective-correct attributes, fog factor (1 = none) */
typedef struct {
    float x, y, z, q;
    float u, v;
    float r, g, b, a;
    float fog;
} Rnd3Vtx;
typedef struct {
    RndState s;                  /* raster (0 = untextured), blend factors, filter, addressing */
    int blend;                   /* 0: no blending */
    int atest;                   /* 0 off, 1 alpha > 0, 2 alpha >= 0x80, 3 0 < alpha < 0x80 */
    int ztest, zwrite;
    uint32_t fog_color;          /* r, g, b, a bytes (r in the low byte) */
} Rnd3State;
void hle_raster_tris3d(const Rnd3Vtx *v, int nverts, const Rnd3State *st);   /* nverts / 3 triangles, Z buffered */
int hle_raster_alpha_class(uint32_t raster);   /* -1 not decodable, 0 opaque, 1 cutout, 2 blend (texconv's CLUT class) */
int hle_raster_is_open(void);

/* host rasteriser (hle_raster.c); every function is a no-op until hle_raster_open succeeded */
int hle_raster_open(const char *dir, int every, int max_frames);
void hle_raster_set_screen(int w, int h);
void hle_raster_clear(const uint8_t rgba[4], int mode);              /* mode: 1 = colour, 2 = Z (the driver's camera clear) */
void hle_raster_tris(const RndVtx *v, int nverts, const RndState *st); /* nverts / 3 independent triangles */
void hle_raster_lines(const RndVtx *v, int nverts, const RndState *st);/* nverts / 2 independent lines */
int hle_raster_frame_end(uint64_t flip, const char *tag);            /* returns 1 when a PNG was written */
void hle_raster_close(void);
void hle_raster_texture_name(uint32_t raster, const char *name);    /* for the texture log (drv_texture_read) */
void hle_raster_forget(uint32_t raster);                             /* drv_raster_destroy: drop the decoded copy */

/* options (host: --frames DIR, --frame-every N, --frame-max N, --render-log N) */
extern const char *hle_opt_frames_dir;
extern int hle_opt_frame_every, hle_opt_frame_max, hle_opt_render_log;
extern uint64_t hle_opt_render_trace;     /* host: log every 2D draw of this flip (--render-trace N) */
void hle_render_init(void);
void hle_render_report(void);

/* for the 3D world (hle_w3d.c): the render state as last set through drv_set_state, the flip count,
   the frame-buffer raster of the camera being updated and the raster of the last flip (the screen) */
uint32_t hle_render_rs(int s);
uint64_t hle_render_flip_count(void);
uint32_t hle_render_cur_fb(void);
uint32_t hle_render_screen_raster(void);
void hle_w3d_init(void);                 /* hle_w3d.c: options, memory (called by hle_render_init) */
void hle_w3d_report(void);

#endif
