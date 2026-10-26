/* hle_ge3d.c - PSP GE back-end of the 3D world (the core is hle_w3d.c).
 *
 * Draws into the 2D back-end's frame (hle_ge.c: the open GU_DIRECT list, the persistent picture P at VRAM 0xCC000, the
 * texture cache) with the 3D state on top of its 2D state:
 *   - projection = the camera's (hle_w3d.c: x = 2X - Z, y = Z - 2Y, z = A Z + B, w = Z), view = identity, world = the
 *     atomic's world (frame) matrix x view matrix (x 256 for the 16-bit positions) or identity for the CPU-clipped
 *     triangles;
 *   - Z: the 16-bit buffer at VRAM 0x110000 (reserved by hle_ge.c, stride 512), sceGuDepthRange(65535, 0) + GEQUAL
 *     (cleared to 0 = farthest by the frame's camera clear with Z, hle_ge.c), clip planes ON (the GE clips at the near
 *     plane and, with clipping on, clamps Z instead of discarding triangles that cross the far plane - PPSSPP and the
 *     PSP agree);
 *   - culling as the render driver asked (CULLMODE: back), front face = GU_CCW (negative area on the y-down screen =
 *     the PS2 stream's winding; w3d_front=1 flips it for an A/B on the device), fog (GE fog = (z_view + FOG1) x FOG2:
 *     FOG1 = -far, FOG2 = -1 / (far - fogPlane) gives the driver's linear fog from the fog plane to the far plane),
 *     blending with the driver's factors, the alpha test of the PS2 bridge (hle_w3d.c);
 *   - a cached mesh = BASE + VADDR (its vertex block) + CALL of its sub-list (VTYPE, PRIM..., RET; the GE advances the
 *     vertex address after every PRIM); the CPU path's unclipped triangles = an indexed draw into the same block
 *     (indices in list memory), its clipped ones = float vertices in list memory.
 * The 3D state is sent again whenever hle_ge.c dropped it (a list start - incl. a mid-frame resync - or a 2D draw:
 * hle_ge_3d_need_state), so a resync between two draws never leaves a mesh without its matrices / state.
 * Memory: the vertex cache comes from the guest code hole (hle_plat_big_alloc), written back from the dcache after every
 * conversion (w3d_plat_flush) before any list references it. */
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspgu.h>
#include "hle.h"
#include "hle_render.h"
#include "hle_ge.h"
#include "hle_w3d.h"
#include "hle_w3d_me.h"                       /* the Media Engine split */

static W3dCam cam;
static int cam_valid;
static float world[12];
static W3dState cur;
static int have_state;
static uint32_t tex_raster;
static int tex_filter, tex_au, tex_av;
static uint64_t st_atomics, st_meshes, st_indexed, st_float_tris, st_resends, st_state, st_texfail, st_ranges,
    st_range_prims;
static uint64_t st_list_bytes;

void *w3d_plat_arena(uint32_t bytes)
{
    uint8_t *p = hle_plat_big_alloc(bytes);           /* the hole (else the heap); zeroed */
    sceKernelDcacheWritebackInvalidateRange(p, bytes);
    return p;
}

void w3d_plat_flush(const void *p, uint32_t bytes)
{
    if ((uintptr_t)p & 0x40000000u) return;           /* list memory (uncached alias): nothing in the dcache */
    sceKernelDcacheWritebackRange(p, bytes);
}

uint32_t w3d_plat_us(void) { return (uint32_t)sceKernelGetSystemTimeLow(); }

static void mat4_of(const float m[12], ScePspFMatrix4 *o)
{
    o->x.x = m[0]; o->x.y = m[1]; o->x.z = m[2]; o->x.w = 0.0f;
    o->y.x = m[3]; o->y.y = m[4]; o->y.z = m[5]; o->y.w = 0.0f;
    o->z.x = m[6]; o->z.y = m[7]; o->z.z = m[8]; o->z.w = 0.0f;
    o->w.x = m[9]; o->w.y = m[10]; o->w.z = m[11]; o->w.w = 1.0f;
}

/* w3d_gecmd=1: the hot 3D commands written straight into the open display list (libpspgu's own list
   pointer, what sceGuSendCommandi does) instead of a library call each - fewer calls and less code on the per-mesh
   path */
typedef struct { unsigned int *start, *current; int parent_context; } GuListPriv;
extern GuListPriv *gu_list;
static inline void gcmd(uint32_t cmd, uint32_t arg) { *gu_list->current++ = cmd << 24 | (arg & 0xFFFFFFu); }
static inline void gcmdf(uint32_t cmd, float f) { uint32_t u; memcpy(&u, &f, 4); *gu_list->current++ = cmd << 24 | u >> 8; }
static uint64_t st_texskips, st_kicks;
/* libpspgu kicks the GE (sceGeListUpdateStallAddr, a system call) in every sceGuCallList / sceGuDrawArray: once per
   mesh. With w3d_gecmd the meshes' CALLs do not kick; the GE gets the new part of the list every w3d_kick meshes and
   at the end of every atomic / deferred batch (and hle_ge.c's 2D draws and the list finish kick anyway) */
extern int ge_list_executed[2];
extern int gu_curr_context, gu_object_stack_depth;
static int kick_pending;
static void ge_kick(void)
{
    if (!kick_pending) return;
    kick_pending = 0;
    if (gu_curr_context || gu_object_stack_depth) return;
    sceGeListUpdateStallAddr(ge_list_executed[0], gu_list->current);
    st_kicks++;
}
void w3d_be_kick(void) { ge_kick(); }

static void send_world(void)
{
    if (w3d_opt_gecmd) {                      /* WMS 0, then the 4 x 3 elements (what sceGuSetMatrix(GU_MODEL) sends) */
        gcmd(0x3A, 0);
        for (int i = 0; i < 12; i++) gcmdf(0x3B, world[i]);
        return;
    }
    ScePspFMatrix4 m;
    mat4_of(world, &m);
    sceGuSetMatrix(GU_MODEL, &m);
}

/* the projection in use (the camera's, or the lit pipelines' S x P) and the driver's lights (hle_w3d_lit.c) */
static float proj_cur[16];
static int proj_custom;
static W3dLights lights_cur;
static int lights_valid;
static uint64_t st_bones, st_lightsets;
extern uint32_t hle_ge_tex_scans;                 /* hle_ge.c */

/* W3D_BB_GECMD: the projection / lights the GE holds (0 = unknown: a forgotten state, a CALLed segment),
   so a lit record does not send them again when they are already there; proj_lazy = the camera's projection is due
   (lit_end) but sent only before the next draw that is not a lit projection again */
static float proj_ge[16];
static int proj_ge_ok, proj_lazy, lights_ge_ok;
static uint64_t st_bb_proj_skip, st_bb_light_skip;

static void send_proj(void)
{
    ScePspFMatrix4 p;
    memcpy(&p, proj_custom ? proj_cur : cam.proj, sizeof p);
    sceGuSetMatrix(GU_PROJECTION, &p);
    memcpy(proj_ge, &p, sizeof proj_ge);
    proj_ge_ok = 1;
    proj_lazy = 0;
}

static uint32_t rgb8(const float *c)
{
    uint32_t o = 0;
    for (int i = 0; i < 3; i++) {
        float v = c[i] * 255.0f + 0.5f;
        o |= (uint32_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int)v) << (8 * i);
    }
    return o;
}

static void send_lights(void)
{
    lights_ge_ok = 1;
    for (int i = 0; i < lights_cur.n && i < 4; i++) {
        ScePspFVector3 d = {lights_cur.dir[i][0], lights_cur.dir[i][1], lights_cur.dir[i][2]};
        sceGuLight(i, GU_DIRECTIONAL, GU_DIFFUSE, &d);
        sceGuLightColor(i, GU_DIFFUSE, rgb8(lights_cur.col[i]));
        sceGuLightColor(i, GU_AMBIENT, 0);
    }
    st_lightsets++;
}

/* the state the GE holds (sent_ok = 0 after a list start / 2D draw: everything is sent again); a mesh only
   sends what differs from the previous one (hle_ge.c's blend calls are kept as they were: the 2D code shares them) */
static W3dState sent;
static int sent_ok;
static uint64_t st_state_skips;

static void send_state(void)
{
    const W3dState *st = &cur;
    int all = !sent_ok;
    if (!all && !memcmp(&sent, st, sizeof sent)) { st_state_skips++; return; }
    /* the GE writes depth only while its depth test is on: Z write without Z test = test ALWAYS (as the GS does) */
    if (all || st->ztest != sent.ztest || st->zwrite != sent.zwrite) {
        if (st->ztest || st->zwrite) {
            sceGuEnable(GU_DEPTH_TEST);
            /* ztest 2 (w3d_lodz): the PS2's LOD pass - strictly nearer, the HD model under it wins ties */
            sceGuDepthFunc(st->ztest == 2 ? GU_GREATER : st->ztest ? GU_GEQUAL : GU_ALWAYS);
        } else
            sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(st->zwrite ? GU_FALSE : GU_TRUE);       /* GU_TRUE = no depth writes */
    }
    if (all || st->cull != sent.cull) {
        if (st->cull) {
            int ccw_front = !w3d_opt_front;
            if (st->cull == 2) ccw_front = !ccw_front;          /* CULLMODE front: keep the other side */
            sceGuFrontFace(ccw_front ? GU_CCW : GU_CW);
            sceGuEnable(GU_CULL_FACE);
        } else
            sceGuDisable(GU_CULL_FACE);
    }
    /* blend and alpha test together: hle_ge.c's set_blend also touches the alpha test */
    if (all || st->blend != sent.blend || st->src_blend != sent.src_blend || st->dst_blend != sent.dst_blend ||
        st->atest != sent.atest) {
        hle_ge_3d_blend(st->blend, st->src_blend, st->dst_blend);
        switch (st->atest) {
        case 1: sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0, 0xFF); break;
        case 2: sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GEQUAL, 0x80, 0xFF); break;   /* PS2 AREF 0x40, doubled */
        case 3: sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_LESS, 0x80, 0xFF); break;
        default: sceGuDisable(GU_ALPHA_TEST); break;
        }
    }
    int fog = st->fog && cam.far_z > cam.fog_z;
    if (all || fog != (sent.fog && cam.far_z > cam.fog_z) || (fog && st->fog_color != sent.fog_color)) {
        if (fog) {
            sceGuSendCommandi(207, (int)(st->fog_color & 0xFFFFFF));    /* FOGCOLOR (R in the low byte, as the driver's RGBA) */
            sceGuSendCommandf(205, -cam.far_z);                         /* FOG1 */
            sceGuSendCommandf(206, -1.0f / (cam.far_z - cam.fog_z));    /* FOG2 */
            sceGuEnable(GU_FOG);
        } else
            sceGuDisable(GU_FOG);
    }
    if (all || st->uscale != sent.uscale) sceGuTexScale(st->uscale, st->uscale);
    if (all || st->ubase != sent.ubase || st->vbase != sent.vbase) sceGuTexOffset(st->ubase, st->vbase);
    /* GE lighting. light (buildings, w3d_light): no light source, the vertex colour is the material ambient (CMAT 1):
       colour = emissive + ambient light x vertex colour, alpha = vertex alpha x ambient alpha. lights (lit pipelines):
       CMAT 0, material ambient = material colour x surface ambient (+ its alpha), diffuse = material colour x surface
       diffuse, global ambient = the driver's ambient light, directional lights 0..n-1 (w3d_be_lights) */
    int lit = st->light || st->lights;
    if (all || lit != (sent.light || sent.lights)) { if (lit) sceGuEnable(GU_LIGHTING); else sceGuDisable(GU_LIGHTING); }
    int nl = st->lights ? st->lights - 1 : 0, snl = all ? -1 : sent.lights ? sent.lights - 1 : 0;
    if (nl != snl)
        for (int i = 0; i < 4; i++) { if (i < nl) sceGuEnable(GU_LIGHT0 + i); else sceGuDisable(GU_LIGHT0 + i); }
    if (st->lights) {
        if (all || !sent.lights) sceGuColorMaterial(0);
        if (all || !sent.lights || st->lamb != sent.lamb || st->lemis != sent.lemis || st->lemit != sent.lemit) {
            uint32_t m = st->lamb, sa = st->lemis & 0xFF, sd = st->lemis >> 8 & 0xFF, mac = 0, mdc = 0;
            for (int i = 0; i < 3; i++) {
                uint32_t c = m >> (8 * i) & 0xFF;
                mac |= ((c * sa + 127u) / 255u) << (8 * i);
                mdc |= ((c * sd + 127u) / 255u) << (8 * i);
            }
            sceGuAmbientColor(mac | (m & 0xFF000000u));                 /* MAC + MAA */
            sceGuSendCommandi(86, (int)mdc);                             /* MDC */
            sceGuSendCommandi(87, 0);                                    /* MSC */
            sceGuSendCommandi(84, (int)(st->lemit & 0xFFFFFF));          /* EMC (0 except lights-on materials) */
        }
        sceGuAmbient(rgb8(lights_cur.amb) | 0xFF000000u);                /* AC + ALA: the driver's ambient light */
    } else if (st->light) {
        if (all || !sent.light || sent.lights) sceGuColorMaterial(GU_AMBIENT);
        if (all || !sent.light || sent.lights || st->lamb != sent.lamb) sceGuAmbient(st->lamb);
        if (all || !sent.light || sent.lights || st->lemis != sent.lemis)
            sceGuSendCommandi(84, (int)(st->lemis & 0xFFFFFF));        /* EMC: material emissive colour */
    }
    sent = *st;
    sent_ok = 1;
    st_state++;
}

/* the frame-wide 3D state: projection, view, depth, clipping, shading */
static void send_global(void)
{
    ScePspFMatrix4 id;
    static const float i12[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    mat4_of(i12, &id);
    send_proj();
    sceGuSetMatrix(GU_VIEW, &id);
    sceGuDepthRange(65535, 0);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_LIGHT0);                 /* lighting = ambient + emissive only (lit pipelines enable theirs) */
    sceGuDisable(GU_LIGHT1);
    sceGuDisable(GU_LIGHT2);
    sceGuDisable(GU_LIGHT3);
    sceGuColorMaterial(GU_AMBIENT);          /* CMAT: the vertex colour is the material ambient */
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sent_ok = 0;
    st_resends++;
}

/* before any 3D command: room in the list, then everything hle_ge.c may have dropped (a list start, a 2D draw) */
static int tx_valid, tx_acl, tx_ok;
static uint32_t tx_epoch;
static void ensure(uint32_t bytes)
{
    hle_ge_3d_room(bytes);
    if (!hle_ge_3d_need_state()) {
        if (proj_lazy) send_proj();               /* the camera's projection after a lit record */
        return;
    }
    tx_valid = 0;
    send_global();
    send_world();
    if (lights_valid) send_lights();
    if (have_state) {
        int acl;
        hle_ge_3d_texture(tex_raster, tex_filter, tex_au, tex_av, &acl);
        send_state();
    }
}

int w3d_be_target(void) { return hle_ge_on && hle_ge_3d_begin(); }

int w3d_be_begin(const W3dCam *c)
{
    hle_ge_ser_mark(GS_W3DM);                     /* main-core 3D (peds, cars, props, immediate-mode 3D, CPU path) */
    if (!hle_ge_on || !hle_ge_3d_begin()) return 0;
    int changed = !cam_valid || memcmp(&cam, c, sizeof cam) != 0;
    cam = *c;
    cam_valid = 1;
    have_state = 0;
    hle_ge_3d_room(2048);
    if (hle_ge_3d_need_state() || changed) send_global();
    st_atomics++;
    return 1;
}

uint32_t w3d_be_epoch(void) { return hle_ge_3d_epoch(); }

void w3d_be_proj(const float *p)
{
    if (w3d_opt_bb & W3D_BB_GECMD) {
        if (p) {
            memcpy(proj_cur, p, sizeof proj_cur);
            proj_custom = 1;
            proj_lazy = 0;                        /* (before ensure: it must not send the camera's first) */
            ensure(128);
            if (proj_ge_ok && !memcmp(proj_ge, proj_cur, sizeof proj_ge)) { st_bb_proj_skip++; return; }
            send_proj();
        } else if (proj_custom) {
            proj_custom = 0;
            if (proj_ge_ok && !memcmp(proj_ge, cam.proj, sizeof proj_ge)) return;
            proj_lazy = 1;                        /* sent by the next ensure() unless a lit projection comes first */
        }
        return;
    }
    if (p) { memcpy(proj_cur, p, sizeof proj_cur); proj_custom = 1; }
    else if (!proj_custom) return;
    else proj_custom = 0;
    ensure(128);
    send_proj();
}

void w3d_be_lights(const W3dLights *l)
{
    /* W3D_BB_GECMD: the same lights as the GE holds - nothing to send, and the ambient colour (AC) the next
       state would send again is the same too, so the state cache stays valid */
    int same = (w3d_opt_bb & W3D_BB_GECMD) && lights_valid && lights_ge_ok && !memcmp(&lights_cur, l, sizeof *l);
    lights_cur = *l;
    lights_valid = 1;
    ensure(256);
    if (same && lights_ge_ok) { st_bb_light_skip++; return; }
    send_lights();
    sent_ok = 0;                                      /* the ambient light colour (AC) goes with the next state */
}

void w3d_be_prims(uint32_t vtype, const void *verts, int n)
{
    if (n < 3) return;
    uint32_t vsize = 32u;                             /* the lit CPU path's vertex: UV, normal, position floats */
    if (vtype != (3u | 3u << 5 | 3u << 7)) return;
    uint32_t bytes = (uint32_t)n * vsize;
    ensure(bytes + 256);
    void *o = sceGuGetMemory((int)bytes);
    memcpy(o, verts, bytes);
    sceGuDrawArray(GU_TRIANGLES, (int)vtype | GU_TRANSFORM_3D, n, NULL, o);
    st_float_tris += (uint64_t)n / 3;
    st_list_bytes += bytes;
}

void w3d_be_bones(int n, const float (*m)[12])
{
    if (n > 8) n = 8;
    ensure(64 + 52u * (uint32_t)n);
    if (w3d_opt_bb & W3D_BB_GECMD) {              /* the same words, written directly (no call per word) */
        unsigned int *o = gu_list->current;
        *o++ = 42u << 24;
        for (int i = 0; i < n; i++)
            for (int q = 0; q < 12; q++) {
                uint32_t u;
                memcpy(&u, &m[i][q], 4);
                *o++ = 43u << 24 | u >> 8;
            }
        gu_list->current = o;
        st_bones += (uint64_t)n;
        return;
    }
    /* what sceGuBoneMatrix sends (BOFS, then the 4 x 3 elements as BONE words, float24), with one BOFS for all bones:
       the GE advances the bone offset with every BONE word */
    sceGuSendCommandi(42, 0);
    for (int i = 0; i < n; i++)
        for (int q = 0; q < 12; q++) {
            uint32_t u;
            memcpy(&u, &m[i][q], 4);
            sceGuSendCommandi(43, (int)(u >> 8));
        }
    st_bones += (uint64_t)n;
}
void w3d_be_end(void)
{
    have_state = 0;
    if (!w3d_defer_flushing) ge_kick();    /* a deferred batch kicks every w3d_kick meshes and at its end */
}

void w3d_be_world(const float m[12])
{
    memcpy(world, m, sizeof world);
    ensure(128);
    send_world();
}

int w3d_be_texture(uint32_t raster, int filter, int addr_u, int addr_v, int *aclass)
{
    ensure(512);
    /* w3d_texskip=1: the same texture as the previous 3D bind in the same list and 3D state: nothing to do
       (hle_ge.c's lookup reads the guest raster every time; ensure() and a list sync invalidate this) */
    if (w3d_opt_texskip && tx_valid && raster == tex_raster && filter == tex_filter && addr_u == tex_au &&
        addr_v == tex_av && tx_epoch == hle_ge_3d_epoch()) {
        *aclass = tx_acl;
        st_texskips++;
        return tx_ok;
    }
    tex_raster = raster;
    tex_filter = filter;
    tex_au = addr_u;
    tex_av = addr_v;
    int ok = hle_ge_3d_texture(raster, filter, addr_u, addr_v, aclass);   /* may convert (and resync) */
    if (!ok) st_texfail++;
    tx_valid = 1;
    tx_acl = *aclass;
    tx_ok = ok;
    tx_epoch = hle_ge_3d_epoch();
    return ok;
}

void w3d_be_state(const W3dState *st)
{
    cur = *st;
    have_state = 1;
    ensure(512);
    send_state();
}

void *w3d_be_alloc(uint32_t bytes)
{
    ensure(bytes + 256);
    st_list_bytes += bytes;
    return sceGuGetMemory((int)((bytes + 15u) & ~15u));
}

static uint32_t vtype_of(const W3dMeshView *m)
{
    return (m->fmt == W3D_FMT_UV16 ? GU_TEXTURE_16BIT : GU_TEXTURE_32BITF) | GU_COLOR_8888 | GU_VERTEX_16BIT |
           GU_TRANSFORM_3D;
}

void w3d_be_mesh(const W3dMeshView *m, const uint8_t *override)
{
    const uint8_t *v = override ? override : m->verts;
    ensure(64);
    if (w3d_opt_gecmd) {                                            /* what the calls below send */
        uintptr_t s = (uintptr_t)m->sub;
        gcmd(16, (uint32_t)(((uintptr_t)v >> 8) & 0xF0000));
        gcmd(1, (uint32_t)((uintptr_t)v & 0xFFFFFF));
        gcmd(16, (uint32_t)((s >> 8) & 0xF0000));
        gcmd(10, (uint32_t)(s & 0xFFFFFF));
        st_meshes++;
        if (++kick_pending >= w3d_opt_kick) ge_kick();
        return;
    }
    sceGuSendCommandi(16, (int)(((uintptr_t)v >> 8) & 0xF0000));   /* BASE */
    sceGuSendCommandi(1, (int)((uintptr_t)v & 0xFFFFFF));           /* VADDR */
    sceGuCallList(m->sub);                                          /* VTYPE, PRIM..., RET */
    st_meshes++;
}

void w3d_be_range(const W3dMeshView *m, const uint8_t *override, uint32_t first_vert, const uint32_t *prims,
                  uint32_t nprims)
{
    const uint8_t *v = (override ? override : m->verts) + first_vert * m->stride;
    ensure(64 + 4u * nprims);
    if (w3d_opt_gecmd) {
        gcmd(16, (uint32_t)(((uintptr_t)v >> 8) & 0xF0000));
        gcmd(1, (uint32_t)((uintptr_t)v & 0xFFFFFF));
        gcmd(18, vtype_of(m));
        for (uint32_t i = 0; i < nprims; i++) gcmd(4, prims[i]);
        if (++kick_pending >= w3d_opt_kick) ge_kick();
    } else {
    sceGuSendCommandi(16, (int)(((uintptr_t)v >> 8) & 0xF0000));   /* BASE */
    sceGuSendCommandi(1, (int)((uintptr_t)v & 0xFFFFFF));           /* VADDR */
    sceGuSendCommandi(18, (int)vtype_of(m));                        /* VTYPE */
    for (uint32_t i = 0; i < nprims; i++) sceGuSendCommandi(4, (int)(prims[i] & 0xFFFFFF));   /* PRIM (advances VADDR) */
    }
    st_ranges++;
    st_range_prims += nprims;
}

const void *w3d_be_indexed(const W3dMeshView *m, const uint8_t *override, const uint16_t *idx, int n)
{
    if (n < 3) return NULL;
    const uint8_t *v = override ? override : m->verts;
    uint32_t bytes = (uint32_t)n * 2u;
    ensure(bytes + 256);
    uint16_t *ix = sceGuGetMemory((int)((bytes + 15u) & ~15u));
    memcpy(ix, idx, bytes);
    sceGuDrawArray(GU_TRIANGLES, (int)(vtype_of(m) | GU_INDEX_16BIT), n, ix, v);
    st_indexed += (uint64_t)n / 3;
    st_list_bytes += bytes;
    return ix;
}

const void *w3d_be_tris(const W3dVC *v, int n)
{
    if (n < 3) return NULL;
    uint32_t bytes = (uint32_t)n * sizeof(W3dVC);
    ensure(bytes + 256);
    W3dVC *o = sceGuGetMemory((int)bytes);
    memcpy(o, v, bytes);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, NULL, o);
    st_float_tris += (uint64_t)n / 3;
    st_list_bytes += bytes;
    return o;
}

/* the second alpha pass draws the first pass's list copies again (the caller checked the GE epoch: the copies
   are in the open list, or in the previous one after one mid-frame sync - intact until the next) */
static uint64_t st_again;
void w3d_be_indexed_again(const W3dMeshView *m, const uint8_t *override, const void *copy, int n)
{
    if (n < 3 || !copy) return;
    ensure(256);
    sceGuDrawArray(GU_TRIANGLES, (int)(vtype_of(m) | GU_INDEX_16BIT), n, copy, override ? override : m->verts);
    st_indexed += (uint64_t)n / 3;
    st_again++;
}

void w3d_be_tris_again(const void *copy, int n)
{
    if (n < 3 || !copy) return;
    ensure(256);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, NULL, copy);
    st_float_tris += (uint64_t)n / 3;
    st_again++;
}

/* ------------------------------------------------------------------ the Media Engine path (w3d_me, hle_w3d_me.h) */
static int getexp(int val)                        /* libpspgu's sceGuTexImage getExp */
{
    unsigned int i;
    for (i = 9; (i > 0) && !((val >> i) & 1); --i)
        ;
    return (int)i;
}

int w3d_be_tex_desc(uint32_t raster, int filter, int addr_u, int addr_v, W3mTex *d)
{
    memset(d, 0, sizeof *d);
    HleGeTexInfo ti;
    if (!hle_ge_3d_tex_info(raster, &ti)) { st_texfail++; return 0; }
    d->ok = 1;
    d->textured = raster != 0;
    d->acl = ti.aclass;
    if (!raster) return 1;
    uint32_t f = hle_ge_min_filter(filter == 2 || filter == 4 || filter == 6, ti.mx, ti.mipf);   /* as hle_ge.c apply_state */
    uint32_t wu = addr_u == 3 || addr_u == 4 ? GU_CLAMP : GU_REPEAT, wv = addr_v == 3 || addr_v == 4 ? GU_CLAMP : GU_REPEAT;
    d->tbp = ti.tbp;
    d->cbp = ti.cbp;
    d->psm_swz_exp = (ti.psm & 0xFF) | ((ti.swz & 1u) | (ti.mx & 7u) << 4) << 8 | (uint32_t)getexp((int)ti.w) << 16 |
                     (uint32_t)getexp((int)ti.h) << 24;                 /* max mip level in bits 12-14 */
    d->bufw_clut = (ti.bufw & 0xFFFF) | ti.clut_blocks << 16 | (ti.lbias & 0xFFu) << 24;   /* level bias */
    d->cpsm_filt_wrap = (ti.clut_psm & 0xFF) | f << 8 | wu << 16 | wv << 24;
    return 1;
}

void w3d_be_hold(int on) { hle_ge_3d_hold(on); }

/* after a CALLed ME segment: what this file and hle_ge.c believe the GE holds is unknown - send everything again */
void w3d_be_forget(void)
{
    hle_ge_3d_forget();
    proj_ge_ok = 0;                               /* a segment set its own projection / lights */
    lights_ge_ok = 0;
    proj_lazy = 0;
    sent_ok = 0;
    tx_valid = 0;
    cam_valid = 0;
    have_state = 0;
}

int w3d_be_seg(const uint32_t *seg)
{
    hle_ge_ser_mark(GS_W3D);                      /* a Media Engine segment (buildings) */
    if (!hle_ge_on || !hle_ge_3d_begin()) return 0;
    hle_ge_3d_room(64);                           /* (may resync: the segment is complete, the GE may run it at once) */
    hle_ge_3d_need_state();                       /* the 3D state is on the GE now: a 2D draw cleans it up first */
    uintptr_t s = (uintptr_t)seg;
    gcmd(16, (uint32_t)((s >> 8) & 0xF0000));     /* what sceGuCallList sends (without its kick) */
    gcmd(10, (uint32_t)(s & 0xFFFFFF));
    kick_pending++;
    return 1;
}

/* the self-test: libpspgu's words (and this file's / hle_ge.c's) for the same inputs, captured by pointing libpspgu's
   list pointer at a scratch buffer, against the encoder the ME runs. Nothing captured reaches the GE. */
static uint32_t cap_buf[768] __attribute__((aligned(16)));
static unsigned int *cap_save;
static uint32_t g_tmpl[64];
static int g_tmpl_n;
static void cap_on(void) { cap_save = gu_list->current; gu_list->current = (unsigned int *)cap_buf; }
static int cap_off(void) { int n = (int)(gu_list->current - (unsigned int *)cap_buf); gu_list->current = cap_save; return n; }
static int cap_cmp(const uint32_t *enc, int ne, int n, const char *what, int k, char *why, int why_n)
{
    if (n == ne && !memcmp(cap_buf, enc, (size_t)n * 4u)) return 1;
    int i = 0;
    while (i < n && i < ne && cap_buf[i] == enc[i]) i++;
    snprintf(why, (size_t)why_n, "%s %d: libpspgu %d words, encoder %d, first difference at word %d (%08x / %08x)", what,
             k, n, ne, i, i < n ? (unsigned)cap_buf[i] : 0u, i < ne ? (unsigned)enc[i] : 0u);
    return 0;
}

const uint32_t *w3d_be_global_template(int *n) { *n = g_tmpl_n; return g_tmpl_n ? g_tmpl : NULL; }

int w3d_be_me_selftest(char *why, int why_n)
{
    if (!hle_ge_on || !gu_list) { snprintf(why, (size_t)why_n, "the GE is off"); return 0; }
    if (!hle_ge_3d_begin()) { snprintf(why, (size_t)why_n, "no frame list open"); return 0; }
    static uint32_t enc[768];
    int ok = 1, n, ne, checks = 0;
    W3dCam kc = cam;
    int kpc = proj_custom;
    float kw[12];
    memcpy(kw, world, sizeof kw);
    W3dState kcur = cur;
    /* 1. send_global = the projection (encoder) + the rest (the template the ME copies) */
    for (int i = 0; i < 16; i++) cam.proj[i] = 0.37f * (float)(i + 1) - 2.1f;
    cam.near_z = 0.5f;
    cam.far_z = 812.5f;
    cam.fog_z = 140.25f;
    proj_custom = 0;
    cap_on();
    send_global();
    n = cap_off();
    ne = (int)(w3dme_enc_proj(enc, cam.proj) - enc);
    checks++;
    if (n < ne || n - ne > 64) { snprintf(why, (size_t)why_n, "send_global: %d words", n); ok = 0; }
    else if (!cap_cmp(enc, ne, ne, "projection", 0, why, why_n)) ok = 0;
    else { g_tmpl_n = n - ne; memcpy(g_tmpl, cap_buf + ne, (size_t)g_tmpl_n * 4u); }
    /* 2. the world matrix (gecmd=0's sceGuSetMatrix) */
    if (ok) {
        static const float tw[12] = {256.0f, -0.5f, 3.25f, 1.0f, 255.75f, -2.0f, 0.125f, 0.0f, -256.0f, 1234.5f, -77.25f, 9.0f};
        ScePspFMatrix4 m;
        mat4_of(tw, &m);
        cap_on();
        sceGuSetMatrix(GU_MODEL, &m);
        n = cap_off();
        ne = (int)(w3dme_enc_world(enc, tw) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "world", 0, why, why_n)) ok = 0;
    }
    /* 3. send_state: every test state from nothing, then each from the previous one */
    static const W3dState T[] = {
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5, .dst_blend = 6, .fog = 1,
         .fog_color = 0x00405060u},
        {.textured = 1, .ubase = 2.0f, .vbase = -3.0f, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5,
         .dst_blend = 6, .atest = 1, .fog = 1, .fog_color = 0x00405060u, .light = 1, .lamb = 0xFF808080u, .lemis = 0x102030u},
        {.textured = 1, .uscale = 1.0f, .ztest = 1, .cull = 2, .blend = 1, .src_blend = 5, .dst_blend = 6, .atest = 2,
         .fog_color = 0x00405060u, .light = 1, .lamb = 0x80FFFFFFu},
        {.textured = 1, .uscale = 1.0f, .ztest = 1, .blend = 1, .src_blend = 5, .dst_blend = 2, .atest = 3, .fog = 1,
         .fog_color = 0x00112233u, .lamb = 0x80FFFFFFu},
        {.uscale = 8.0f, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 2, .dst_blend = 1, .light = 1,
         .lamb = 0xFFFFFFFFu, .lemis = 0x050505u},
        {.textured = 1, .ubase = 0.5f, .vbase = 0.25f, .uscale = 8.0f, .blend = 1, .src_blend = 9, .dst_blend = 3,
         .atest = 1, .fog = 1, .fog_color = 0x00FF00FFu},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 1, .dst_blend = 7,
         .atest = 2, .fog = 1, .fog_color = 0x00405060u, .light = 1, .lamb = 0x40C0C0C0u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 6, .dst_blend = 5,
         .fog = 1, .fog_color = 0x00405060u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 10, .dst_blend = 4,
         .atest = 1, .fog = 1, .fog_color = 0x00405060u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 8, .dst_blend = 8,
         .fog = 1, .fog_color = 0x00405060u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 11, .dst_blend = 11,
         .atest = 3, .fog = 1, .fog_color = 0x00405060u, .light = 1, .lamb = 0xFF808080u, .lemis = 0x102030u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5, .dst_blend = 6, .fog = 1,
         .fog_color = 0x00405060u},
    };
    const int nt = (int)(sizeof T / sizeof T[0]);
    for (int i = 0; ok && i < nt; i++) {
        sent_ok = 0;
        cur = T[i];
        cap_on();
        send_state();
        n = cap_off();
        ne = (int)(w3dme_enc_state(enc, &T[i], &T[i], 1, cam.far_z, cam.fog_z, w3d_opt_front | w3dme_dpol << 1, lights_cur.amb) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "state (all)", i, why, why_n)) ok = 0;
    }
    for (int i = 1; ok && i < nt; i++) {
        sent = T[i - 1];
        sent_ok = 1;
        cur = T[i];
        cap_on();
        send_state();
        n = cap_off();
        ne = (int)(w3dme_enc_state(enc, &T[i], &T[i - 1], 0, cam.far_z, cam.fog_z, w3d_opt_front | w3dme_dpol << 1, lights_cur.amb) -
                   enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "state (change)", i, why, why_n)) ok = 0;
    }
    /* the lit pipelines - a custom projection (send_proj), the lights (send_lights), lit states */
    W3dLights kl = lights_cur;
    int klv = lights_valid;
    if (ok) {
        for (int i = 0; i < 16; i++) proj_cur[i] = 1.5f - 0.21f * (float)i;
        proj_custom = 1;
        cap_on();
        send_proj();
        n = cap_off();
        ne = (int)(w3dme_enc_proj(enc, proj_cur) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "lit projection", 0, why, why_n)) ok = 0;
        proj_custom = kpc;
    }
    for (int nl = 0; ok && nl <= 4; nl++) {
        memset(&lights_cur, 0, sizeof lights_cur);
        lights_cur.n = nl;
        for (int k = 0; k < 3; k++) lights_cur.amb[k] = 0.1f + 0.37f * (float)k + 0.05f * (float)nl;
        for (int q = 0; q < 4; q++)
            for (int k = 0; k < 3; k++) {
                lights_cur.dir[q][k] = (float)(q + 1) * 0.25f - 0.6f * (float)k;
                lights_cur.col[q][k] = 0.15f * (float)(q + k) + (q == 1 ? 1.2f : 0.0f);   /* incl. one > 1 (clamped) */
            }
        cap_on();
        send_lights();
        n = cap_off();
        ne = (int)(w3dme_enc_lights(enc, &lights_cur) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "lights", nl, why, why_n)) ok = 0;
    }
    static const W3dState L[] = {
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5, .dst_blend = 6, .fog = 1,
         .fog_color = 0x00405060u, .lights = 3, .lamb = 0xFF2080C0u, .lemis = 0xFF40u},
        {.textured = 1, .uscale = 8.0f, .ubase = 1.0f, .ztest = 1, .zwrite = 1, .cull = 1, .blend = 1, .src_blend = 5,
         .dst_blend = 6, .atest = 2, .fog = 1, .fog_color = 0x00405060u, .lights = 1, .lamb = 0x80FFFFFFu,
         .lemis = 0xC0FFu, .lemit = 0x203040u},
        {.uscale = 8.0f, .ztest = 1, .cull = 2, .src_blend = 5, .dst_blend = 6, .lights = 5, .lamb = 0xFF000000u,
         .lemis = 0x0101u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5, .dst_blend = 6, .fog = 1,
         .fog_color = 0x00405060u, .light = 1, .lamb = 0xFF808080u, .lemis = 0x102030u},
        {.textured = 1, .uscale = 8.0f, .ztest = 1, .zwrite = 1, .cull = 1, .src_blend = 5, .dst_blend = 6, .fog = 1,
         .fog_color = 0x00405060u, .lights = 2, .lamb = 0xFF2080C0u, .lemis = 0xFF40u},
    };
    const int nlt = (int)(sizeof L / sizeof L[0]);
    for (int i = 0; ok && i < nlt; i++) {
        sent_ok = 0;
        cur = L[i];
        cap_on();
        send_state();
        n = cap_off();
        ne = (int)(w3dme_enc_state(enc, &L[i], &L[i], 1, cam.far_z, cam.fog_z, w3d_opt_front | w3dme_dpol << 1, lights_cur.amb) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "lit state (all)", i, why, why_n)) ok = 0;
        if (ok && i) {
            sent = L[i - 1];
            sent_ok = 1;
            cur = L[i];
            cap_on();
            send_state();
            n = cap_off();
            ne = (int)(w3dme_enc_state(enc, &L[i], &L[i - 1], 0, cam.far_z, cam.fog_z, w3d_opt_front | w3dme_dpol << 1, lights_cur.amb) -
                       enc);
            checks++;
            if (!cap_cmp(enc, ne, n, "lit state (change)", i, why, why_n)) ok = 0;
        }
    }
    lights_cur = kl;
    lights_valid = klv;
    /* 4. a texture bind as hle_ge.c apply_state makes it (CLUT and direct colour; every filter / wrap) */
    for (int i = 0; ok && i < 4; i++) {
        W3mTex t;
        memset(&t, 0, sizeof t);
        t.ok = 1;
        t.textured = 1;
        uint32_t psm = i & 1 ? GU_PSM_5650 : GU_PSM_T8, swz = (uint32_t)(i < 2), w = 64u << (i & 1), h = 32u << (i >> 1);
        uint32_t bufw = w, cpsm = GU_PSM_8888, blocks = i & 1 ? 0 : 32, f = (uint32_t)(i & 1), wu = (uint32_t)(i >> 1),
                 wv = (uint32_t)((i ^ 1) & 1);
        t.tbp = 0x08912340u + 0x1000u * (uint32_t)i;
        t.cbp = blocks ? 0x08A00040u : 0;
        t.psm_swz_exp = psm | swz << 8 | (uint32_t)getexp((int)w) << 16 | (uint32_t)getexp((int)h) << 24;
        t.bufw_clut = bufw | blocks << 16;
        t.cpsm_filt_wrap = cpsm | f << 8 | wu << 16 | wv << 24;
        cap_on();
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode((int)psm, 0, 0, (int)swz);
        sceGuTexImage(0, (int)w, (int)h, (int)bufw, (void *)(uintptr_t)t.tbp);
        if (t.cbp) {
            sceGuClutMode(cpsm, 0, 0xFF, 0);
            sceGuClutLoad((int)blocks, (void *)(uintptr_t)t.cbp);
        }
        sceGuTexFlush();
        sceGuTexFilter((int)f, (int)f);
        sceGuTexWrap((int)wu, (int)wv);
        n = cap_off();
        ne = (int)(w3dme_enc_tex(enc, &t, &t, 1) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "texture", i, why, why_n)) ok = 0;
    }
    /* a bind with mip levels (TMODE max level, TBP/TBW/TSIZE 1..mx, TLEVEL bias, mipmap filter) */
    for (int i = 0; ok && i < 2; i++) {
        W3mTex t;
        memset(&t, 0, sizeof t);
        t.ok = 1;
        t.textured = 1;
        uint32_t psm = i ? GU_PSM_T8 : GU_PSM_T4, swz = 1, w = i ? 64u : 128u, h = i ? 64u : 32u, mx = 2;
        uint32_t bufw = w, cpsm = GU_PSM_8888, blocks = i ? 32u : 2u, f = hle_ge_min_filter(1, mx, i ? 7u : 5u);
        int bias = i ? 16 : -8;
        t.tbp = 0x08912340u + 0x2000u * (uint32_t)i;
        t.cbp = 0x08A00040u;
        t.psm_swz_exp = psm | (swz | mx << 4) << 8 | (uint32_t)getexp((int)w) << 16 | (uint32_t)getexp((int)h) << 24;
        t.bufw_clut = bufw | blocks << 16 | ((uint32_t)bias & 0xFFu) << 24;
        t.cpsm_filt_wrap = cpsm | f << 8;
        cap_on();
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode((int)psm, (int)mx, 0, (int)swz);
        sceGuTexImage(0, (int)w, (int)h, (int)bufw, (void *)(uintptr_t)t.tbp);
        for (uint32_t k = 1; k <= mx; k++) {
            uint32_t lw, lh, lb, o = hle_ge_mip_level(psm, swz, w, h, k, &lw, &lh, &lb);
            sceGuTexImage((int)k, (int)lw, (int)lh, (int)lb, (void *)(uintptr_t)(t.tbp + o));
        }
        sceGuTexLevelMode(GU_TEXTURE_AUTO, (float)bias / 16.0f);
        sceGuClutMode(cpsm, 0, 0xFF, 0);
        sceGuClutLoad((int)blocks, (void *)(uintptr_t)t.cbp);
        sceGuTexFlush();
        sceGuTexFilter((int)f, (int)(f & 1u));
        sceGuTexWrap(0, 0);
        n = cap_off();
        ne = (int)(w3dme_enc_tex(enc, &t, &t, 1) - enc);
        checks++;
        if (!cap_cmp(enc, ne, n, "texture mip", i, why, why_n)) ok = 0;
    }
    /* nothing of the above reached the GE; the caches go back to "unknown" */
    cam = kc;
    cam_valid = 0;
    proj_custom = kpc;
    memcpy(world, kw, sizeof world);
    cur = kcur;
    sent_ok = 0;
    tx_valid = 0;
    have_state = 0;
    proj_ge_ok = lights_ge_ok = 0;               /* the captured words never reached the GE; a projection */
    proj_lazy = (w3d_opt_bb & W3D_BB_GECMD) != 0;   /* due before the self-test is sent before the next draw */
    hle_ge_3d_forget();
    if (ok) snprintf(why, (size_t)why_n, "PASS (%d word sequences identical to libpspgu's, global template %d words)",
                     checks, g_tmpl_n);
    else g_tmpl_n = 0;
    return ok;
}

void w3d_be_report(void)
{
    LOG("world3d GE: %llu chunk ranges (%llu PRIM commands), %llu CPU-path draws repeated for the second alpha pass, "
        "%llu state sets skipped as unchanged, %llu bone matrices, %llu light sets, %u texture lookups that missed the "
        "hint (table scans)\n", (unsigned long long)st_ranges,
        (unsigned long long)st_range_prims, (unsigned long long)st_again, (unsigned long long)st_state_skips,
        (unsigned long long)st_bones, (unsigned long long)st_lightsets, (unsigned)hle_ge_tex_scans);
    LOG("world3d GE: w3d_gecmd %d (GE kicks %llu, every %d meshes), w3d_texskip %d (%llu texture binds "
        "skipped)\n", w3d_opt_gecmd, (unsigned long long)st_kicks, w3d_opt_kick, w3d_opt_texskip,
        (unsigned long long)st_texskips);
    LOG("world3d GE: %llu atomics, %llu cached-mesh calls, %llu indexed + %llu float triangles (CPU path), %llu KB of "
        "list memory, %llu state sets, %llu 3D state re-sends, %llu textures not drawable\n",
        (unsigned long long)st_atomics, (unsigned long long)st_meshes, (unsigned long long)st_indexed,
        (unsigned long long)st_float_tris, (unsigned long long)(st_list_bytes >> 10), (unsigned long long)st_state,
        (unsigned long long)st_resends, (unsigned long long)st_texfail);
}
#endif
