/* hle_w3d.h - the game's 3D world drawn on the PSP GE.
 *
 * hle_w3d.c hooks the render driver's mesh node body (mesh_node_body, 0x391398) that every model pipeline of the game
 * runs. For the static world pipelines (ids 0x53F20080 and 0x53F20082, the latter with day/night colours) it converts
 * the atomic's PS2 native geometry (the VIF unpack streams the model file carried for VU1: V4_16 positions /128 with
 * the ADC strip-break flag in w, V2_16 UVs /4096, V4_8 prelight or the V4_16 day/night pair) once into GE vertex
 * blocks (cache keyed by the mesh's resource entry, budget-limited, LRU), and draws them every frame with the atomic's
 * world matrix x camera view matrix, the camera projection, Z, culling, fog and blending as the driver asked. Meshes
 * whose bounding box leaves the GE guard band (the GE has no X/Y clipping) go through a CPU clipper. The draws go to a
 * back-end:
 *   PSP:  psp/hle_ge3d.c (sceGu, into the shared display list and picture);
 *   host: host/hle_w3d_host.c (emulates the GE transform and feeds hle_raster.c's 3D triangles: PNG frames).
 */
#ifndef HLE_W3D_H
#define HLE_W3D_H
#include <stdint.h>

/* GE vertex formats of a converted mesh (16-bit positions = the PS2 s16 values; the world matrix is scaled by 256 =
   32768 / 128 because the GE reads 16-bit positions as value / 32768) */
typedef struct { uint16_t u, v; uint32_t c; int16_t x, y, z, pad; } W3dV16;       /* UV raw - base, 1/4096 units */
typedef struct { float u, v; uint32_t c; int16_t x, y, z, pad; } W3dVF;           /* UV span >= 16 tiles */
/* CPU-clipped vertices: model space in s16 / 32768 units, drawn with the atomic's world matrix */
typedef struct { float u, v; uint32_t c; float x, y, z; } W3dVC;

enum { W3D_FMT_UV16 = 0, W3D_FMT_UVF = 1, W3D_FMT_LIT = 2 };   /* LIT: any vertex type, described by vtype */
enum { W3D_PRIM_STRIP = 0, W3D_PRIM_LIST = 1 };

/* one converted mesh as the back-ends see it: block = [vertices][sub-list words: VTYPE, PRIM..., RET] */
typedef struct {
    const uint8_t *verts;          /* host pointer of the vertex block (16-byte aligned) */
    const uint32_t *sub;           /* GE sub-list: VTYPE, PRIM x nprims, RET (no VADDR: the caller sets it) */
    uint32_t nverts, nprims, nsub, ntris;
    uint8_t fmt, prim, stride, pad;
    float ubase, vbase;            /* UV16: texture offset in tiles (scale 8 = 32768 / 4096) */
    uint32_t vtype;                /* W3D_FMT_LIT: the GE vertex type (weights / UV / normal / position) */
} W3dMeshView;

/* per-draw state (the texture is bound separately: w3d_be_texture) */
typedef struct {
    int textured;
    float ubase, vbase, uscale;    /* GE texture offset / scale (UV16: scale 8, the base in tiles; float UV: 1, 0) */
    int ztest, zwrite, cull;       /* cull: 0 none, 1 back, 2 front; ztest 2 = GREATER (the LOD pass) */
    int blend, src_blend, dst_blend;   /* blend: 0 off, 1 on (the driver's blend factors) */
    int atest;                     /* 0 off, 1 alpha > 0, 2 alpha >= 0x80 (PS2 AREF 0x40), 3 alpha < 0x80 (and > 0) */
    int fog;
    uint32_t fog_color;            /* RGBA as u32 (R in the low byte) */
    /* GE lighting carries the per-atomic colour parameters instead of baking them into the vertices
       (w3d_light=1): colour = emissive + ambient light x vertex colour (rgb), alpha = vertex alpha x ambient alpha
       (GE: CMAT ambient from the vertex, no light sources). light 0 = the vertex colour as it is. */
    int light;
    uint32_t lamb;                 /* light: ambient light colour RGBA (R low byte) = the material colour;
                                      lights: the material colour RGBA */
    uint32_t lemis;                /* light: emissive RGB = building ambient x material colour; lights: surface
                                      properties ambient (byte 0) and diffuse (byte 1) x 255 */
    /* lit pipelines (vehicles / peds / 0x5001): the driver's lighting on the GE - the lights set by w3d_be_lights
       (ambient = AC, directional lights 0..n-1), material ambient / diffuse = material colour x surface properties
       (registers, no vertex colour) */
    int lights;                    /* 0 off, else 1 + the number of directional lights */
    uint32_t lemit;                /* lights: emissive RGB (a surface ambient > 1, the vehicle lights-on material,
                                      ambient 16: material x min(1, ambient x ambient light)) */
} W3dState;

/* the driver's lights for the lit pipelines: ambient + up to 4 directional lights in view space (camera-relative, the
   GE view matrix is identity), colours 0..1 */
typedef struct {
    float amb[3];
    int n;
    float dir[4][3];               /* direction towards the light, view space, normalised */
    float col[4][3];
} W3dLights;

typedef struct {
    float proj[16];                /* GE projection: view space -> clip */
    float near_z, far_z, fog_z;    /* camera near / far / fog planes */
    uint32_t camera;
} W3dCam;

/* back-end (PSP: psp/hle_ge3d.c, host: host/hle_w3d_host.c) */
int w3d_be_begin(const W3dCam *cam);       /* before an atomic: 0 = the 3D cannot be drawn now (GE off, other target) */
int w3d_be_target(void);                   /* the same check without starting anything (deferred atomics) */
/* w3d_defer: draw the deferred building atomics now - called before anything else is drawn (psp/hle_ge.c
   ge3d_leave / camera begin, host hle_raster.c entry points, our lit / immediate-mode 3D paths, the frame end) */
void w3d_defer_flush(void);
extern int w3d_defer_flushing, w3d_opt_defer, w3d_opt_hwprof;
/* w3d_gecmd = the hot GE commands written directly (no library call, no kick per mesh), w3d_kick = meshes
   between two GE kicks then, w3d_texskip = no texture lookup for the same texture as the previous bind */
extern int w3d_opt_gecmd, w3d_opt_kick, w3d_opt_texskip;
/* main-core cuts of the 3D path, cfg w3d_bb = a bit mask, default 0 = the plain path; every bit draws the same GE
   words / pixels, switchable between two frames (the in-run A/B, FAM_W3D) */
extern int w3d_opt_bb;
#define W3D_BB_LEAN 1u      /* fewer clock syscalls (hook clock sampled 1 in 8, phase 1 timed per batch), publish the
                               ME stream every 32 lines at the flush instead of after every record */
#define W3D_BB_LTM 2u       /* the driver's frame world-matrix getter's clean path natively (no guest call when the
                               hierarchy is in sync) */
#define W3D_BB_WAIT 4u      /* ME wait: spin (no clock syscall per turn) up to 2 ms before yielding with DelayThread */
#define W3D_BB_GECMD 8u     /* PSP: bone words written directly; lights / lit projection not sent again when the GE
                               holds them; the camera projection after a lit record restored lazily */
#define W3D_BB_TEXC 16u     /* phase 1: texture descriptors cached for the rest of the flush (16 slots) */
#define W3D_BB_LITPREP 32u  /* lit / skinned records' lookups + bone-group classification done while waiting for the ME */
void w3d_be_kick(void);
uint32_t w3d_be_epoch(void);               /* GE work generation: a block drawn in this epoch may still be read */
void w3d_be_world(const float m[12]);      /* GE world matrix (x, y, z axes, translation) */
/* bind the texture of a guest driver raster (0 = untextured); 0 = not drawable; *alpha_class 0 opaque, 1 cutout, 2 blend
   (texconv's class over the CLUT entries; untextured: 0) */
int w3d_be_texture(uint32_t raster, int filter, int addr_u, int addr_v, int *alpha_class);
void w3d_be_state(const W3dState *st);
void w3d_be_mesh(const W3dMeshView *m, const uint8_t *verts_override);   /* draw a cached mesh (or a copy) */
/* draw nprims consecutive PRIM words of a cached mesh's sub-list starting at its vertex first_vert (GE chunks) */
void w3d_be_range(const W3dMeshView *m, const uint8_t *verts_override, uint32_t first_vert, const uint32_t *prims,
                  uint32_t nprims);
void *w3d_be_alloc(uint32_t bytes);        /* per-frame memory the GE reads (list memory on the PSP) */
/* n / 3 CPU-clipped triangles in view space (world = identity); returns the copy the GE reads (list memory) */
const void *w3d_be_tris(const W3dVC *v, int n);
/* n / 3 triangles of a cached mesh by 16-bit indices into its vertex block (the CPU path's unclipped triangles);
   returns the GE's copy of the indices */
const void *w3d_be_indexed(const W3dMeshView *m, const uint8_t *verts_override, const uint16_t *idx, int n);
/* draw again from those copies (the second alpha pass of a CPU-path mesh, same GE epoch) */
void w3d_be_tris_again(const void *copy, int n);
void w3d_be_indexed_again(const W3dMeshView *m, const uint8_t *verts_override, const void *copy, int n);
/* lit / skinned pipelines (hle_w3d_lit.c): the projection for the following draws (16 floats, rows = inputs
   x, y, z, 1; NULL = the camera's own w3d_cam.proj), the driver's lights (camera space), the bone matrices of the next
   draws (n <= 8, 12 floats each: right, up, at, pos rows as w3d_be_world) */
void w3d_be_proj(const float *proj16);
void w3d_be_lights(const W3dLights *l);
void w3d_be_bones(int n, const float (*m)[12]);
/* n / 3 triangles of vertex type vtype (no weights, no index) copied into GE memory and drawn: the lit CPU path's
   camera-space floats (UV, normal, position; world = identity) */
void w3d_be_prims(uint32_t vtype, const void *verts, int n);
void w3d_be_end(void);                     /* after an atomic */
void w3d_be_report(void);

/* configuration (PSP: ps2rc.cfg keys world3d, mesh_guest, w3d_kb, w3d_cull,
   w3d_fog, w3d_clip, w3d_front, w3d_afail, w3d_light, w3d_gnear, w3d_dnq, w3d_rebake_us, w3d_chunk; host: environment
   HLE_WORLD3D, HLE_MESH_GUEST, HLE_W3D_KB, ... = the key in upper case with HLE_ in front) */
extern int w3d_opt_on, w3d_opt_guest, w3d_opt_kb, w3d_opt_cull, w3d_opt_fog, w3d_opt_clip, w3d_opt_front, w3d_opt_afail;
extern int w3d_opt_light, w3d_opt_gnear, w3d_opt_dnq, w3d_opt_rebake_us, w3d_opt_chunk, w3d_opt_chunk_group;
extern int w3d_opt_sky, w3d_opt_near_mm;   /* fog-colour clear of 3D frames; GE near-plane floor (1/1000) */
void w3d_cfg(const char *key, const char *value);   /* one cfg key (hle_ge.c read_cfg forwards the unknown ones) */
void w3d_frame_end(void);                  /* every flip (hle_render.c): the LRU's frame counter */

/* platform (in the back-end files): the vertex-cache memory (PSP: the guest code hole, written back before the GE reads
   it), the cache write-back of a block the CPU just wrote, a microsecond clock (PSP: sceKernelGetSystemTimeLow - no
   soft-float double in the timing) */
void *w3d_plat_arena(uint32_t bytes);
void w3d_plat_flush(const void *p, uint32_t bytes);
uint32_t w3d_plat_us(void);

#endif
