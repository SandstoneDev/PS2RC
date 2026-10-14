/* hle_w3d_int.h - internals shared by the 3D world's core (hle_w3d.c: hook, building pipelines, cache, classification,
 * CPU clipper) and its lit / skinned pipelines (hle_w3d_lit.c: vehicles, peds, the driver's skinning, 0x5001).
 * Not part of the back-end interface (hle_w3d.h). */
#ifndef HLE_W3D_INT_H
#define HLE_W3D_INT_H
#include <stdint.h>
#include "hle.h"
#include "hle_w3d.h"

/* one VIF batch of a mesh's DMA chain: guest addresses of its attribute arrays (checked by the parser) */
typedef struct {
    uint32_t pos, uv, col, nrm, wgt;
    uint16_t n, skip;                     /* vertices in the batch, leading repeats (strip continuation) */
    uint8_t pf, uf, cf, nf, wf;           /* formats, see below */
} Batch;
enum { PF_NONE, PF_V4_16, PF_V3_16, PF_V4_32, PF_V3_32 };
enum { UF_NONE, UF_V2_16, UF_V2_32, UF_V4_16, UF_V4_32 };
enum { CF_NONE, CF_V4_8, CF_V4_16 };
enum { NF_NONE, NF_V4_8, NF_V3_8 };
enum { WF_NONE, WF_V4_32 };
#define MAXBATCH 512
extern Batch w3d_bat[MAXBATCH];
extern int w3d_nbat;
/* parse a mesh chain; map[slot] = attribute (0 position, 1 UV, 2 colour, 3 normal, 4 skin weights, -1 ignored) */
int w3d_parse_chain_map(uint32_t data, int strip, const int8_t *map);

/* chunk of a converted mesh: consecutive PRIMs with their bounding box (raw s16 positions) */
typedef struct { uint16_t first_prim, nprims; uint32_t first_vert; int16_t bmin[3], bmax[3]; uint16_t ntris, pad; } Chunk;

/* colour parameters baked into building vertices (plain = no ambient / modulation: the source colour as it is) */
typedef struct { uint32_t b8; uint8_t amb[3]; uint8_t mat[4]; int dn, modulate, plain; } ColP;

enum { EK_BUILDING = 0, EK_LIT = 1, EK_SKIN = 2 };
/* a converted mesh in the vertex cache */
typedef struct {
    uint32_t re;                          /* key (the mesh's resource entry) */
    int16_t next;                         /* hash chain (next to the key: one cache line per chain step) */
    uint16_t nx;                          /* lit / skinned: number of extra records (sub-draws) */
    uint32_t data, geo, sig;              /* what validates it */
    uint32_t rsz, nidx;                   /* + resource entry size (+0x08) and the mesh's index count: a different mesh
                                             can come back at the same addresses with the same chain head */
    uint32_t off, bytes, vbytes;          /* arena block: vertices, then the sub-list (then chunks / palettes) */
    uint32_t nverts, nprims, ntris, nstream, nchunks, ch_off;
    uint32_t ckey;                        /* colour parameters baked into the block */
    uint32_t last_use, pin;               /* frame of the last use, GE epoch of the last draw */
    float bmin[3], bmax[3];               /* object-space bounding box */
    float ubase, vbase;
    float pscale;                         /* lit / skinned: position units (1 / 128, 1 / 1024 ...) */
    uint32_t vtype;                       /* lit / skinned: the GE vertex type */
    uint32_t xoff;                        /* lit / skinned: offset of the per-kind extra data in the block */
    uint8_t used, fmt, prim, dn, strip, fast, kind, stride;
    uint32_t hold;                        /* == w3d_me_hold: a batch described to the Media Engine draws it */
    uint32_t vframe;                      /* w3d_vframe: the frame in which it was last validated */
} Ent;
/* w3d_me: the GE may read the entry (drawn in the open list) or a batch the ME builds will (held): neither
   evicted nor re-baked in place */
extern uint32_t w3d_me_hold;
int w3d_ent_busy(const Ent *e);

typedef struct { float m[4][3]; } M43;
/* a bone group of a converted lit / skinned mesh (hle_w3d_lit.c, stored after its vertices and sub-lists; declared here
   because the Media Engine's builder reads it too) */
typedef struct {
    uint32_t voff, suboff, nverts, ntris;
    uint16_t nprims;
    uint8_t nb, pad;
    uint8_t bone[8];
    float bmin[3], bmax[3];               /* bind-pose box of the group's vertices (atomic space) */
    float bbmin[8][3], bbmax[8][3];       /* per palette slot: the bind-pose box of the vertices it weights (empty:
                                             min > max) - the posed group lies in the union of their images */
} LGroup;   /* the driver's matrix rows: right, up, at, pos; v' = v M */
enum { CL_CULL = 0, CL_HW = 1, CL_CPU = 2, CL_INSIDE = 4 };   /* w3d_classify_box: CL_HW | CL_INSIDE = all visible */

extern W3dCam w3d_cam;
int w3d_camera_setup(uint32_t camera);
void w3d_drv_mat(uint32_t a, M43 *o);
void w3d_mat_mul(const M43 *a, const M43 *b, M43 *o);
int w3d_classify_box(const float *bmin, const float *bmax, const M43 *m);
int w3d_classify_chunk(const Chunk *c, const M43 *m, float scale);
void w3d_world_reset(const float *gm);    /* a new atomic: the GE world matrix gm (12 floats) is "not sent yet" */
void w3d_world_set(int which);            /* 1 the atomic's matrix, 2 identity */
void w3d_base_state(W3dState *st);
int w3d_alpha_passes(W3dState *st, int acl, int valpha);
int w3d_bind_material_texture(uint32_t mat, uint32_t *ras_out, int *acl, int *wrap);   /* wrap: bit 0 U, bit 1 V repeat */
int w3d_bind_texture(uint32_t tex, uint32_t *ras_out, int *acl, int *wrap);            /* the same for a texture */
/* the driver's immediate-mode 3D primitives through the mesh node (hle_w3d_imm.c); 1 = drawn */
extern int w3d_opt_im3d;
/* sampled ALLEGREX counters (hle_w3d.c, w3d_hwprof); kind 3 = a lit / skinned atomic */
void w3d_hwp_begin(int kind);
void w3d_hwp_end(int kind, uint32_t natomics, uint32_t nmeshes);
/* w3d_defer=2: the lit path's records in hle_w3d.c's queue */
void w3d_defer_push_lit(int idx);
int w3d_defer_room(void);
void w3d_lit_draw_deferred(int idx);
void w3d_lit_defer_reset(void);
int w3d_im3d(R5900Ctx *c, uint32_t stash);
void w3d_im3d_report(void);
extern int w3d_opt_tilebase;
uint32_t w3d_gwk(uint32_t a);
uint32_t w3d_chain_sig(uint32_t data);
uint32_t w3d_frame_no(void);
uint32_t w3d_frame_ltm(R5900Ctx *c, uint32_t frame);   /* a frame's world matrix (W3D_BB_LTM: natively when in sync) */
void w3d_lit_prep(int lidx);              /* W3D_BB_LITPREP: a deferred lit record's lookups + group classes */
uint8_t *w3d_arena_base(void);
int w3d_arena_get(uint32_t bytes, uint32_t *off);
int w3d_arena_get_for(Ent *e, uint32_t bytes, uint32_t *off);   /* a block owned by e (evictable later) */
Ent *w3d_ent_new(uint32_t re);
Ent *w3d_ent_find(uint32_t re);
void w3d_ent_free(Ent *e);
void w3d_ent_unlink(Ent *e);
void w3d_stat_conv(uint32_t bytes);
void w3d_stat_noroom(void);
void w3d_lit_time(uint32_t us);
extern uint64_t w3d_u_lit_conv;

/* hle_w3d_lit.c */
void w3d_lit_cfg(const char *key, const char *value);
void w3d_lit_init(void);
void w3d_lit_report(void);
int w3d_lit_atomic(R5900Ctx *c, uint32_t pds, uint32_t obj);   /* 1 drawn, 0 not handled (the guest node runs) */
#ifndef HLE_PSP
void w3d_lit_dump_extra(R5900Ctx *c, uint32_t pds, uint32_t obj, void (*dmem)(uint32_t addr, uint32_t n));
#endif

#endif
