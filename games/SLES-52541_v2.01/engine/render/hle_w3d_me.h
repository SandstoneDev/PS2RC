/* hle_w3d_me.h - the 3D world's display lists built
 * on the PSP's Media Engine (second MIPS core), w3d_me=1 (default 0 = everything on the main core).
 *
 * Split of a deferred batch (hle_w3d.c):
 *   main core (game thread): per atomic and mesh everything that needs the game's memory, the vertex cache or the GE
 *     texture cache - mesh lookup / conversion / re-bake, the mesh-box cull, the texture lookup (held, not bound), for
 *     lit / skinned atomics also the light list, the bone palette and the surface properties - and writes a JOB STREAM:
 *     64-byte lines, word 0 of every line a tag (batch << 16 | line) so that the ME can tell a stale cache line from
 *     this batch's; items: camera, building record, lit record, mesh, lit mesh, "main" markers (meshes / records the
 *     main core draws itself), batch end. Written back from the dcache, then the line count is published (uncached).
 *     With w3d_me_hook=1 it is written at HOOK time, record by record, so the ME builds while the game runs; with
 *     w3d_me_hook=0 the whole batch is written at the flush.
 *   Media Engine (or, in tests, a thread): reads the stream as it grows; per mesh the chunk / bone-group
 *     classification, the state (lighting / emissive / alpha passes as hle_w3d.c / hle_w3d_lit.c decide them) and the
 *     GE words: 3D global state, projection, lights, world matrix, bone matrices, texture bind, state, CALL of the
 *     cached sub-list or chunk ranges - into its ARENA (uncached stores); and a PART list for the main core: SEG (arena
 *     words ending in RET), MAINMESH (draw this mesh yourself: CPU clipper / re-bake copy), MREC (draw this record
 *     yourself). No syscalls, no libc, no VFPU, no jump tables on the ME.
 *   main core again, at the flush: END, waits for the ME, then walks the parts in order: SEG -> BASE + CALL in the
 *     GE back-end's open list (GE nesting: list -> segment -> mesh sub-list = 2 levels, the GE's maximum), MAINMESH /
 *     MREC -> the main-core draw code; one GE kick. Textures and vertex blocks the batch references are HELD from the lookup until the
 *     batch's CALLs are in the list (then pinned to the GE epoch as before), so a mid-frame resync cannot evict them.
 * Host: the same builder on a thread; instead of GE words it writes operations that the main thread replays through
 * the host back-end (hle_w3d_host.c), so the host frames show whether the split draws exactly what the main path does.
 */
#ifndef HLE_W3D_ME_H
#define HLE_W3D_ME_H
#include <stdint.h>
#include "hle_w3d.h"
#include "hle_w3d_int.h"

#define W3M_LINES      2048u          /* job stream: 64-byte lines per batch (128 KB) */
#define W3M_PAY        15u            /* payload words per line (word 0 = tag) */
#define W3M_PARTS      2048u          /* part list entries per batch */
#define W3M_ARENA      (256u * 1024u) /* GE words (bytes); reset when the GE has run everything that called into it */
#define W3M_MAXCH      4096u          /* chunks per mesh (hle_w3d.c MAXCHUNK) */
#define W3M_CAMS       16u            /* 0..7 the buildings' cameras (dcam), 8..15 the lit records' (lcam + 8) */
#define W3M_MAXGROUPS  256u           /* bone groups per lit mesh on the ME (more: the main core draws the mesh) */
#define W3M_ITEM_MAX   96u            /* payload words of the largest item */

/* job items: word 0 = type << 24 | payload words */
enum { W3M_I_CAM = 1, W3M_I_REC = 2, W3M_I_MESH = 3, W3M_I_MAINMESH = 4, W3M_I_MREC = 5, W3M_I_END = 6,
       W3M_I_LREC = 7, W3M_I_LMESH = 8 };
#define W3M_CAM_WORDS  20u            /* index, proj[16], near, far, fog */
#define W3M_LCAM_WORDS 36u            /* + the lit projection (S x P, 16) */
#define W3M_REC_WORDS  29u            /* rec index, cam, flags, st0, st1, M[12], gm[12] */
#define W3M_MESH_WORDS 18u            /* see hle_w3d_me.c item_mesh */
#define W3M_LREC_WORDS 72u            /* see hle_w3d_me.c item_lrec */
#define W3M_LMESH_WORDS 20u           /* see hle_w3d_me.c item_lmesh */

/* parts (the ME's output for the main core, uncached): w0 = type << 24 | record, w1 = arena offset (SEG) / mesh */
enum { W3M_P_SEG = 1, W3M_P_MAINMESH = 2, W3M_P_MREC = 3 };
typedef struct { uint32_t w0, w1, n, pad; } W3mPart;

/* the texture of a mesh as the ME needs it (filled on the main core by the back-end: w3d_be_tex_desc) */
typedef struct {
    uint32_t ok;                      /* 0 = not drawable (the mesh is skipped, as w3d_bind_texture's 0) */
    uint32_t textured;                /* raster != 0 */
    uint32_t acl;                     /* alpha class 0 opaque, 1 cutout, 2 blend */
    uint32_t wrap;                    /* the driver's addressing: bit 0 U repeats, bit 1 V repeats (UV base rule) */
    /* PSP: what the GE back-end's apply_state binds (libpspgu argument values); host: raster / filter / addressing */
    uint32_t tbp, cbp;                /* texture / CLUT address (cbp 0 = no CLUT) */
    uint32_t psm_swz_exp;             /* psm | swizzle << 8 | log2 w << 16 | log2 h << 24 | max mip level << 12 */
    uint32_t bufw_clut;               /* buffer width | CLUT blocks << 16 | level bias (1/16, s8) << 24 */
    uint32_t cpsm_filt_wrap;          /* CLUT psm | GU filter << 8 | GU wrap u << 16 | wrap v << 24 */
} W3mTex;
#define W3M_TEX_WORDS 9u

/* the control block (uncached on the PSP; 64-byte aligned; the first 64 bytes are hle_me.h's HleMeHdr when the
   ME runs our own loop - w3d_me_runner=2) */
typedef struct {
    volatile uint32_t hdr[16];        /* HleMeHdr (own loop only) */
    /* main -> ME */
    volatile uint32_t batch;          /* current batch id (>= 1) */
    volatile uint32_t lines;          /* job lines published in this batch */
    volatile uint32_t arena_start;    /* arena byte offset where this batch's GE words begin */
    volatile uint32_t stream, parts, arena, priv;   /* addresses (PSP: stream cached, parts / arena uncached) */
    volatile uint32_t opts;           /* bit 0 gnear, 1 clip, 2 light, 3 tilebase, 4 afail, 5 front; chunk group << 8 */
    volatile uint32_t pad0[8];        /* [0] global template, [1] its words, [2] own runner, [3] [4] vertex cache */
    /* ME -> main */
    volatile uint32_t polls;          /* +1 per poll (a live poller) */
    volatile uint32_t done;           /* = batch when the batch is complete (parts + arena written) */
    volatile uint32_t nparts;         /* parts of the completed batch */
    volatile uint32_t arena_end;      /* arena byte offset after the batch */
    volatile uint32_t fail;           /* != 0: the batch failed on the ME (main draws it): 1 stale line, 2 arena full,
                                         3 parts full, 4 bad item */
    volatile uint32_t lines_done;     /* progress (main's stall check) */
    volatile uint32_t st_meshes, st_cull, st_main, st_segs, st_words, st_stale, st_ranges, st_twopass;
    volatile uint32_t st_units;       /* work units done (each poll that worked) */
    volatile uint32_t st_lmeshes;     /* lit / skinned meshes drawn by the ME */
    volatile uint32_t st_wb;          /* arena lines written back from the ME's D-cache (w3d_me_bus bit 0) */
    volatile uint32_t st_awords;      /* arena words written in this batch (cached or uncached) */
} W3mCtl;

/* cfg (hle_w3d.c): w3d_me 0 | 1, w3d_me_runner 0 the ME audio loop | 1 thread | 2 own ME loop | 3 the shared ME
   jobs-only loop when no audio runs on the ME (hle_me_jobs.h), w3d_me_stall_ms;
   w3d_me_hook (1: the stream is written at hook time), w3d_me_lit (1: lit / skinned atomics on the ME too),
   w3d_vframe (1: a mesh validated this frame is not validated again - also without the ME) */
extern int w3d_opt_me, w3d_opt_me_runner, w3d_opt_me_stall_ms, w3d_opt_me_hook, w3d_opt_me_lit, w3d_opt_vframe;
/* w3d_me_bus bits, default 0 = both off; switchable between two frames (in-run
   A/B): read per batch (bit 0, sent in the batch's opts) / per batch begin and end (bit 1).
   bit 0 BURST: the ME writes its GE words through its D-cache (cached alias of the arena) and writes every complete
     64-byte line back with one cache op (hit writeback-invalidate; the batch's last partial line at its end, then
     sync, then `done`) - one 64-byte burst instead of 16 single-word uncached stores;
   bit 1 QUIET: while no batch is open, the ME audio / jobs loop backs off 4x longer between idle polls (its jobs table's
     idle_nops; reset to the default when a batch opens) - fewer uncached polls of the ME on the shared bus. */
extern int w3d_opt_me_bus;
#define W3M_BUS_BURST 1
#define W3M_BUS_QUIET 2
#define W3M_OPT_BURST 64u                 /* opts bit (W3mCtl.opts) of a batch built with BURST */

/* main-side API (hle_w3d.c) */
/* allocate + start the runner (0 the ME audio loop polls, 1 a thread, 2 our own ME loop); 0 = not available (why) */
int w3dme_init(int runner, char *why, int why_n);
void w3dme_set_template(const uint32_t *w, uint32_t n);   /* PSP: send_global without the projection */
/* 0 = no live poller: the main core draws; [vlo, vhi) = the vertex cache (the ME checks every address it uses) */
int w3dme_batch_begin(uint32_t arena_start, uint32_t opts, uint32_t vlo, uint32_t vhi);
int w3dme_room(uint32_t payload_words);
void w3dme_put(uint32_t type, const uint32_t *w, uint32_t n);
void w3dme_publish(void);
int w3dme_wait(uint32_t stall_us, uint32_t *fail, uint32_t *nparts, uint32_t *arena_end);
int w3dme_dead(void);
int w3dme_done(void);                         /* the ME finished the open batch (one uncached read) */
uint32_t w3dme_unpublished(void);             /* stream lines written but not yet published */
W3mCtl *w3dme_ctl(void);                            /* the uncached control block */
uint32_t *w3dme_stream(void);                       /* cached job lines */
const W3mPart *w3dme_parts(void);                   /* uncached part list */
const uint32_t *w3dme_arena(void);                  /* GE words / host operations */
void w3dme_report(void);
void w3dme_report_bus(double iterations);     /* the ME's bus counters per main-loop iteration */
/* the ME's step: 1 = did a unit of work (call again soon), 0 = idle. sim = 1 on a main-core thread / the host (no
   cache operations). Called by hle_me_audio.c's loop (idle branch) or by our own loop / thread. */
int w3dme_poll(int sim);
#ifdef HLE_PSP
#include "hle_me.h"
void w3dme_work(volatile HleMeHdr *hdr);            /* our own ME loop (w3d_me_runner=2, nobody else on the ME) */
/* the encoder for the self-test (psp/hle_ge3d.c): the same functions the ME runs, on the main core; amb = the lights'
   ambient colour (the lit pipelines' state sends it) */
uint32_t *w3dme_enc_state(uint32_t *o, const W3dState *st, const W3dState *prev, int all, float far_z, float fog_z,
                          int front, const float *amb);
uint32_t *w3dme_enc_tex(uint32_t *o, const W3mTex *t, const W3mTex *prev, int all);
uint32_t *w3dme_enc_proj(uint32_t *o, const float *p16);
uint32_t *w3dme_enc_blend(uint32_t *o, int on, int sb, int db);
uint32_t *w3dme_enc_world(uint32_t *o, const float *gm);
uint32_t *w3dme_enc_lights(uint32_t *o, const W3dLights *l);
uint32_t *w3dme_enc_bones(uint32_t *o, int n, const float (*m)[12]);
#endif

/* back-end (psp/hle_ge3d.c, hle_w3d_host.c) */
int w3d_be_tex_desc(uint32_t raster, int filter, int addr_u, int addr_v, W3mTex *d);   /* looked up + held, not bound */
void w3d_be_hold(int on);                     /* the GE back-end's texture holds (PSP) */
void w3d_be_forget(void);                     /* the GE state changed behind the back-end's caches (a CALLed segment) */
int w3d_be_seg(const uint32_t *seg);          /* PSP: BASE + CALL of a finished segment into the open list; 0 = lost */
int w3d_be_me_selftest(char *why, int why_n); /* PSP: the encoder against libpspgu + the global template; host: 1 */
const uint32_t *w3d_be_global_template(int *n);   /* PSP: send_global without the projection (self-test capture) */

/* hle_w3d.c <-> hle_w3d_lit.c */
int w3d_me_tex(uint32_t tex, W3mTex *td);     /* a driver texture's descriptor (held; the previous one if it is the same) */
int w3d_me_ent_add(Ent *e);                   /* a job mesh's entry (pins / host replay): its index, -1 = full */
/* the ME draws lit record lidx (drec index ri): its camera / record / meshes into the stream (room checked by the
   caller); *camsent: the cameras already in this batch's stream (bit 8 + lit camera) */
void w3d_lit_me_push(int lidx, uint32_t ri, uint32_t *camsent);
int w3d_lit_draw_one(int lidx, uint32_t mesh);   /* one mesh of a deferred lit record (a MAINMESH part) */
uint32_t w3d_lit_nmesh(int lidx);
const W3dCam *w3d_lit_cam(int lcam_idx);      /* host replay */
void w3d_lit_group_view(const Ent *e, int g, W3dMeshView *mv);   /* host replay: one bone group of a lit mesh */
const float (*w3d_lit_bone_base(void))[12];   /* host: the bone pool (the ME's palettes are offsets into it) */

/* host operations (the arena words of a host SEG): op << 24 | words */
enum { W3M_OP_BEGIN = 1, W3M_OP_WORLD = 2, W3M_OP_TEX = 3, W3M_OP_STATE = 4, W3M_OP_MESH = 5, W3M_OP_RANGE = 6,
       W3M_OP_END = 7, W3M_OP_RET = 8, W3M_OP_PROJ = 9, W3M_OP_LIGHTS = 10, W3M_OP_BONES = 11, W3M_OP_LMESH = 12 };

#endif
