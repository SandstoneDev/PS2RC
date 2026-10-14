/* hle_w3d.c - the game's 3D world on the PSP GE: hook of the render driver's mesh node body, PS2 native geometry ->
 * GE vertex blocks, per-atomic transform, guard-band classification and a CPU clipper.
 *
 * The hook (HLE 0x391398, mesh_node_body(node, param)): the executing pipeline is at 0x82B440 +0, the object is the
 * param's data pointer. Handled here: atomics (type byte 1) of the game's static world pipelines (id 0x53F20080 at
 * pipeline +0x30, and 0x53F20082 with day/night colours) with one morph target and native geometry (geometry flags &
 * 0x01000000) whose mesh cache exists (geometry + *(gp 0x66CB68): {count, resource entry *[count]}, created at load by
 * the native-data reader 0x3904C0); the lit / skinned pipelines are in hle_w3d_lit.c. Everything else - the
 * render-buffer batches (non-atomic objects), other cameras (mirrors) - runs the translated original unchanged.
 * world3d=0 (PSP cfg) / HLE_WORLD3D=0 (host) turns the hook into a pure pass-through.
 * A handled atomic is drawn by the back-end (PSP: psp/hle_ge3d.c, host: host/hle_w3d_host.c); the translated original
 * then runs as well unless mesh_guest=0 (its only output is a VU1 DMA packet the HLE drops; the side effects it has on
 * a native building atomic - the reference / clear counters of the mesh resource entries, the driver's packet buffer,
 * the lights qwords - are read only by the driver's own PS2 back-end).
 *
 * Native geometry (per mesh: the resource entry +0x20 = DMA chain, built offline for VU1 and relocated at load): REF
 * tags whose second VIF code is an UNPACK carry one attribute each (VU slot = the UNPACK address low byte:
 * 0 position V4_16 (s16 /128, w != 0 = ADC: the triangle ending at this vertex is not drawn), 1 UV V2_16 (s16 /4096),
 * 2 colour V4_8 (prelight RGBA) or V4_16 (low byte = day, high byte = night: the day/night pipeline)); an ITOP in a
 * CNT / RET tag closes a batch of that many vertices; tri-strip batches after the first repeat the previous batch's
 * last two vertices.
 * Conversion (once per mesh, into a block of the w3d arena): the strip is split at the ADC flags into sub-strips (a
 * sub-strip starting at an odd stream index gets its first vertex twice, so the GE's strip parity - i.e. the winding
 * for culling - matches the PS2 stream's); vertex = UV u16 (raw - base, base = the floor tile; TexScale 8) or float
 * when the UV span is 16 tiles or more, RGBA8888 colour, s16 position (the PS2 value: world matrix x 256); after the
 * vertices a GE sub-list (VTYPE, one PRIM per sub-strip - the GE advances the vertex address after each PRIM - , RET)
 * that the main list CALLs, then a chunk table (consecutive sub-strips of >= w3d_chunk vertices with their own
 * bounding box).
 * Speed (measured on a PSP-3000: once 30 ms per 3D frame in this hook, most of it re-baking colours and re-converting
 * meshes): (1) the converter reads the guest arrays through host pointers with integer arithmetic only (16-bit
 * formats; the generic float path stays for anything else); (2) w3d_light=1: the per-atomic colour parameters
 * (material colour incl. the fading alpha, the building ambient) are GE state - lighting on with the vertex colour as the
 * material ambient, ambient light = material colour, emissive = ambient x material - so the vertices carry only the
 * prelight (day/night pipeline: day/night at the balance, quantised to w3d_dnq) and a fading atomic or a time-of-day
 * step no longer re-bakes anything; re-bakes left (the day/night balance moving) have a per-frame budget
 * (w3d_rebake_us); (3) w3d_gnear=1: the GE clips the near plane itself (PPSSPP and the PSP agree), so a mesh / chunk /
 * triangle only needs the CPU when it leaves the 4096-pixel guard band; (4) a microsecond clock instead of the
 * soft-float double hle_host_seconds() in the timing; (5) phase timers in the report.
 * Cache: keyed by the resource entry address, validated by the chain pointer, the geometry and a hash of the first
 * chain words; LRU eviction of blocks the open GE list does not use; the baked colour key changes -> re-bake in place,
 * or into list memory when the GE may still read the block.
 * Transform: world = the frame's world matrix (driver function 0x33FAC0) x the camera's view matrix (+0x20; it maps to
 * X/Z, Y/Z in 0..1 = the screen, Z = depth), computed in float on the CPU: the GE's 24-bit matrices then only carry
 * camera-relative values (no world-coordinate jitter far from the origin). Projection (view -> clip): x = 2X - Z,
 * y = Z - 2Y, z = A Z + B, w = Z with the camera's near / far planes. Classification of a bounding box (the mesh's, then for big or partly visible meshes every
 * chunk's): outside the frustum -> skipped; inside the guard band (|x| <= 8 w, |y| <= 9.5 w: the 4096-pixel window
 * and the 2730-pixel Y errata; with w3d_gnear=0 also in front of the near plane) -> the GE draws it (the whole mesh:
 * one CALL; runs of chunks: VADDR + their PRIM words); otherwise -> the CPU path: every vertex transformed once,
 * triangles outside the frustum dropped, triangles inside the guard band drawn by the GE from the cached block through
 * 16-bit indices, the rest clipped against the guard band (and the near plane with w3d_gnear=0) and drawn as view-space
 * floats (world = identity). */
#include "hle.h"
#include "hle_render.h"
#include "hle_w3d.h"
#include "hle_w3d_int.h"
#include "hle_w3d_me.h"                       /* the Media Engine split */
#include <stdlib.h>
#include <math.h>
#ifdef HLE_PSP
#include "hle_ge.h"
#endif

void GUEST_FN(mesh_node_body)(R5900Ctx *c);

#define G_RENDER_CTX 0x0082B440u            /* pipeline execution context: +0 the pipeline being executed */
#define DRV_ENGINE 0x00825C30u             /* the driver's globals: +0 the current camera */
#define GP_MESHCACHE_GEO 0x0066CB68u      /* offset of the mesh-cache pointer in a geometry (one morph target) */
#define GP_DN_BALANCE 0x0066CDE0u         /* day/night pipeline balance (float; written by 0x4CEFF0) */
#define GP_AMB_BUILDING 0x00668898u       /* building ambient RGB bytes (time-of-day cycle, written by 0x4C0740) */
#define GP_AMB_DN 0x006689F0u
#define GP_DEFAULT_MATCOL 0x00668358u     /* the node's colour when the geometry does not modulate */
#define FN_FRAME_MATRIX 0x0033FAC0u
#define PDS_BUILDING 0x53F20080u
#define PDS_BUILDING_DN 0x53F20082u

/* GE command words (gecmd: VTYPE 0x12, PRIM 0x04, RET 0x0B; vertex type bits as pspgu.h) */
#define GE_CMD(c, a) (((uint32_t)(c) << 24) | ((uint32_t)(a) & 0xFFFFFFu))
#define GE_VT_UV16 2u
#define GE_VT_UVF 3u
#define GE_VT_C8888 (7u << 2)
#define GE_VT_P16 (2u << 7)
#define GE_P_TRIANGLES 3u
#define GE_P_STRIP 4u

/* ------------------------------------------------------------------ options */
int w3d_opt_on = 1, w3d_opt_kb = 1792, w3d_opt_cull = 1, w3d_opt_fog = 1, w3d_opt_clip = 1, w3d_opt_front = 0,
    w3d_opt_afail = 1;
int w3d_opt_light = 1, w3d_opt_gnear = 1, w3d_opt_dnq = 2, w3d_opt_rebake_us = 2000, w3d_opt_chunk = 24,
    w3d_opt_chunk_group = 4, w3d_opt_tilebase = 0, w3d_opt_sky = 1, w3d_opt_near_mm = 0;
int w3d_opt_lodz = 0;                     /* the LOD pass tests GREATER (see lod_pass) */
int w3d_opt_bb = 0;                       /* W3D_BB_* bits (hle_w3d.h), 0 = the plain path */
#ifdef HLE_PSP
int w3d_opt_guest = 0;                    /* PSP: skip the translated node for the atomics the GE draws */
#else
int w3d_opt_guest = 1;                    /* host: keep the guest's timeline identical to the boot gate */
#endif
#define GUARD_X 8.0f                      /* |x_ndc| < 2048 / 240 = 8.53 */
#define GUARD_Y 9.5f                      /* |y_ndc| < 1365 / 136 = 10.04 (the GE's Y span limit 0xAAAA, measured) */
#define NEAR_SAFE 1.01f                   /* CPU near plane, a bit in front of the GE's (w3d_gnear=0) */

void w3d_cfg(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "world3d")) w3d_opt_on = x;
    else if (!strcmp(k, "mesh_guest")) w3d_opt_guest = x;
    else if (!strcmp(k, "w3d_kb")) w3d_opt_kb = x;
    else if (!strcmp(k, "w3d_cull")) w3d_opt_cull = x;
    else if (!strcmp(k, "w3d_fog")) w3d_opt_fog = x;
    else if (!strcmp(k, "w3d_clip")) w3d_opt_clip = x;
    else if (!strcmp(k, "w3d_front")) w3d_opt_front = x;
    else if (!strcmp(k, "w3d_afail")) w3d_opt_afail = x;
    else if (!strcmp(k, "w3d_light")) w3d_opt_light = x;
    else if (!strcmp(k, "w3d_gnear")) w3d_opt_gnear = x;
    else if (!strcmp(k, "w3d_dnq")) w3d_opt_dnq = x;
    else if (!strcmp(k, "w3d_rebake_us")) w3d_opt_rebake_us = x;
    else if (!strcmp(k, "w3d_chunk")) w3d_opt_chunk = x;
    else if (!strcmp(k, "w3d_chunk_group")) w3d_opt_chunk_group = x;
    else if (!strcmp(k, "w3d_tilebase")) w3d_opt_tilebase = x;
    else if (!strcmp(k, "w3d_sky")) w3d_opt_sky = x;
    else if (!strcmp(k, "w3d_near_mm")) w3d_opt_near_mm = x;
    else if (!strcmp(k, "w3d_im3d")) w3d_opt_im3d = x;
    else if (!strcmp(k, "w3d_defer")) w3d_opt_defer = x;
    else if (!strcmp(k, "w3d_gecmd")) w3d_opt_gecmd = x;
    else if (!strcmp(k, "w3d_kick")) w3d_opt_kick = x < 1 ? 1 : x;
    else if (!strcmp(k, "w3d_texskip")) w3d_opt_texskip = x;
    else if (!strcmp(k, "w3d_hwprof")) w3d_opt_hwprof = x;
    else if (!strcmp(k, "w3d_me")) w3d_opt_me = x;
    else if (!strcmp(k, "w3d_me_runner")) w3d_opt_me_runner = x;
    else if (!strcmp(k, "w3d_me_stall_ms")) w3d_opt_me_stall_ms = x < 10 ? 10 : x;
    else if (!strcmp(k, "w3d_me_hook")) w3d_opt_me_hook = x;
    else if (!strcmp(k, "w3d_me_lit")) w3d_opt_me_lit = x;
    else if (!strcmp(k, "w3d_vframe")) w3d_opt_vframe = x;
    else if (!strcmp(k, "w3d_lodz")) w3d_opt_lodz = x;
    else if (!strcmp(k, "w3d_bb")) w3d_opt_bb = (int)strtol(v, NULL, 0);
    else if (!strcmp(k, "w3d_me_bus")) w3d_opt_me_bus = (int)strtol(v, NULL, 0);   /* see hle_w3d_me.h */
    else w3d_lit_cfg(k, v);
}

/* ------------------------------------------------------------------ the PS2's LOD pass */
/* The game's scene render (0x26FC50) draws every visible entity first, then lowers the camera's Z shift (+0x90) by
   gp 0x666E30 (-100.0) for the visible LOD list and restores it afterwards: in the GS's 24-bit Z (GEQUAL) the HD
   model a LOD covers wins ties and near-ties. Our projection ignores the Z shift and the GE has a 16-bit Z (one step
   = 6 cm at 20 m, 1.5 m at 100 m with the near plane at 0.1): a LOD drawn after its HD model wins every tie, so the
   low-detail LOD texture shows on the HD buildings. A LOD-pass atomic = camera Z shift more than 50 below the highest
   value seen this frame (entities and roads are drawn before the LODs).
   w3d_lodz=1: those atomics test GREATER instead of GEQUAL (ties go to the HD model, as the PS2's bias makes them);
   default 0 = unchanged (the counters below are kept either way). */
static float lodz_ref = -1e30f;              /* highest Z shift of this frame (reset in w3d_frame_end) */
static uint64_t st_lod_atomics, st_lod_near[3], st_all_near[3], st_lodz_set;
static int lodz_logs = 4;
static const float lod_dist[3] = {20.0f, 50.0f, 100.0f};
static int lod_pass(uint32_t camera, const M43 *M)
{
    uint32_t w = gw(camera + 0x90);
    float zs, d2 = M->m[3][0] * M->m[3][0] + M->m[3][1] * M->m[3][1] + M->m[3][2] * M->m[3][2];
    memcpy(&zs, &w, 4);
    int lod = !(zs >= lodz_ref - 50.0f);
    if (!lod && zs > lodz_ref) lodz_ref = zs;
    for (int i = 0; i < 3; i++)
        if (d2 < lod_dist[i] * lod_dist[i]) {
            st_all_near[i]++;
            if (lod) st_lod_near[i]++;
        }
    if (lod) {
        st_lod_atomics++;
        if (lodz_logs > 0) {
            lodz_logs--;
            LOG("world3d: LOD pass: camera depth shift %.1f, this frame's highest %.1f (delta %.1f), atomic at "
                "%.1f m, flip %llu, w3d_lodz %d\n", (double)zs, (double)lodz_ref, (double)(zs - lodz_ref),
                (double)sqrtf(d2), (unsigned long long)hle_render_flip_count(), w3d_opt_lodz);
        }
    }
    return lod;
}

/* ------------------------------------------------------------------ statistics */
static uint64_t st_calls, st_atomics, st_skipped_guest, st_pass, st_meshes, st_m_hw, st_m_cpu, st_m_cull, st_m_fail,
    st_m_notex, st_m_twopass, st_conv, st_conv_bytes, st_conv_fail, st_rebake, st_rebake_defer, st_dyncopy, st_evict,
    st_tris_hw, st_tris_cpu_in, st_tris_cpu_idx, st_tris_cpu_out, st_ch_hw, st_ch_cpu, st_ch_cull, st_tris_cpu_clipped,
    st_cpu_verts, st_noroom, st_lookups, st_hits, st_conv_slow, st_lit_meshes, st_rebake_bytes, st_ch_groups, st_stale;
static uint64_t st_why[16];
enum { WHY_OFF, WHY_NOTATOMIC, WHY_PIPE, WHY_GEO, WHY_MORPH, WHY_NATIVE, WHY_CACHE, WHY_CAMERA, WHY_TARGET, WHY_N };
static const char *why_name[WHY_N] = {"off", "not an atomic", "other pipeline", "no geometry", "morph targets",
                                      "not native", "no mesh cache", "camera", "not the screen"};
static uint64_t pds_calls[8];
static uint32_t pds_ids[8];
static int npds;
/* microsecond phase clocks (w3d_plat_us): the whole hook, conversions, re-bakes, the CPU path, texture binds, the
   per-atomic setup (world matrix, matrices, render states) */
static uint64_t u_hook, u_conv, u_rebake, u_cpu, u_tex, u_setup, u_lit;
/* sampled phase clocks (1 mesh in 8, 1 atomic in 4: a clock read is a syscall), scaled at the report: lookup (cache /
   colour key), classification, texture bind, state + draw commands (without the CPU path) */
static uint64_t us_lookup, us_class, us_tex, us_draw, us_setup, n_samp_mesh, n_samp_atom;
static uint32_t samp_ctr;
static uint32_t conv_max_us, u_rebake_frame;
uint64_t w3d_u_lit_conv;                  /* hle_w3d_lit.c: conversion time of the lit / skinned meshes */
extern uint64_t hle_hitch_mesh_us __attribute__((alias("w3d_u_lit_conv")));   /* the core's hitch source "mesh" */

/* ------------------------------------------------------------------ the arena (first fit, coalescing, 64-byte units) */
static uint8_t *arena;
static uint32_t arena_bytes, arena_used, arena_peak;
#define W3D_MAXBLK 2048
static struct { uint32_t off, size; int16_t owner; uint8_t used; } blk[W3D_MAXBLK];   /* owner: Ent index */
static int nblk;
static uint64_t st_evict_windows;

static int arena_alloc(uint32_t size, uint32_t *off)
{
    size = (size + 63u) & ~63u;
    for (int i = 0; i < nblk; i++) {
        if (blk[i].used || blk[i].size < size) continue;
        if (blk[i].size > size) {
            if (nblk >= W3D_MAXBLK) continue;
            memmove(&blk[i + 2], &blk[i + 1], sizeof blk[0] * (size_t)(nblk - i - 1));
            blk[i + 1].off = blk[i].off + size;
            blk[i + 1].size = blk[i].size - size;
            blk[i + 1].used = 0;
            blk[i + 1].owner = -1;
            blk[i].size = size;
            nblk++;
        }
        blk[i].used = 1;
        *off = blk[i].off;
        arena_used += size;
        if (arena_used > arena_peak) arena_peak = arena_used;
        return 1;
    }
    return 0;
}

static void arena_free(uint32_t off)
{
    int lo = 0, hi = nblk - 1, i = -1;
    while (lo <= hi) {                    /* blocks are sorted by offset */
        int mid = (lo + hi) >> 1;
        if (blk[mid].off == off) { i = mid; break; }
        if (blk[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    if (i < 0 || !blk[i].used) return;
    blk[i].used = 0;
    arena_used -= blk[i].size;
    if (i + 1 < nblk && !blk[i + 1].used) {
        blk[i].size += blk[i + 1].size;
        memmove(&blk[i + 1], &blk[i + 2], sizeof blk[0] * (size_t)(nblk - i - 2));
        nblk--;
    }
    if (i > 0 && !blk[i - 1].used) {
        blk[i - 1].size += blk[i].size;
        memmove(&blk[i], &blk[i + 1], sizeof blk[0] * (size_t)(nblk - i - 1));
        nblk--;
    }
}

uint8_t *w3d_arena_base(void) { return arena; }

/* ------------------------------------------------------------------ the mesh cache */
#define W3D_MAXENT 1024
#define W3D_HASH 4096                     /* 4 x the entries: chains of ~1 (each step a cache miss on the PSP) */
static Ent ent[W3D_MAXENT];
static int16_t hash_head[W3D_HASH];
static uint32_t frame_no = 1;

static unsigned hslot(uint32_t re) { return (re >> 4) * 2654435761u >> 20; }

void w3d_ent_unlink(Ent *e)
{
    int idx = (int)(e - ent);
    int16_t *p = &hash_head[hslot(e->re)];
    while (*p >= 0 && *p != idx) p = &ent[*p].next;
    if (*p == idx) *p = e->next;
    e->next = -1;
}

void w3d_ent_free(Ent *e)
{
    if (!e->used) return;
    w3d_ent_unlink(e);
    if (e->bytes) arena_free(e->off);
    memset(e, 0, sizeof *e);
    e->next = -1;
}

/* the least recently used entry the GE cannot be reading any more */
static Ent *ent_victim(const Ent *except)
{
    uint32_t ep = w3d_be_epoch();
    Ent *best = NULL;
    for (int i = 0; i < W3D_MAXENT; i++) {
        Ent *e = &ent[i];
        if (!e->used || e->pin == ep || e == except || (w3d_me_hold && e->hold == w3d_me_hold)) continue;
        if (!best || e->last_use < best->last_use) best = e;
    }
    return best;
}

/* when the arena has no hole of the size, evict the cheapest WINDOW of address-adjacent blocks that makes one
   (evicting single LRU entries until a fit appears, with blocks of very different sizes, frees scattered holes,
   evicts far more than needed and thrashes - measured on the host in gameplay: 265k conversions in 512 frames at
   1.75 MB while the working set peaked at 1.2 MB). Cost of a window = the newest last_use among its blocks (free
   blocks: none); blocks the open GE list may read (pinned) end a window. */
static int evict_window(uint32_t need)
{
    uint32_t ep = w3d_be_epoch(), best_score = 0xFFFFFFFFu;
    int best_i = -1, best_j = -1, best_n = 0;
    need = (need + 63u) & ~63u;
    for (int i = 0; i < nblk; i++) {
        uint32_t sz = 0, score = 0;
        int nused = 0;
        for (int j = i; j < nblk; j++) {
            if (blk[j].used) {
                const Ent *e = blk[j].owner >= 0 ? &ent[blk[j].owner] : NULL;
                if (!e || e->pin == ep || (w3d_me_hold && e->hold == w3d_me_hold)) break;   /* pinned / held (or
                                                   not an entry): the window cannot cross it */
                if (e->last_use + 1u > score) score = e->last_use + 1u;
                nused++;
            }
            sz += blk[j].size;
            if (score > best_score) break;
            if (sz >= need) {
                if (score < best_score || (score == best_score && nused < best_n)) {
                    best_score = score; best_i = i; best_j = j; best_n = nused;
                }
                break;
            }
        }
    }
    if (best_i < 0) return 0;
    /* evict the used blocks of the window (owners looked up first: freeing merges / shifts the block list) */
    int16_t own[64];
    int n = 0;
    for (int j = best_i; j <= best_j && n < 64; j++)
        if (blk[j].used) own[n++] = blk[j].owner;
    for (int q = 0; q < n; q++) { w3d_ent_free(&ent[own[q]]); st_evict++; }
    st_evict_windows++;
    return 1;
}

static void arena_set_owner(uint32_t off, int16_t owner)
{
    int lo = 0, hi = nblk - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (blk[mid].off == off) { blk[mid].owner = owner; return; }
        if (blk[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
}

int w3d_arena_get_for(Ent *e, uint32_t bytes, uint32_t *off)
{
    if (bytes > arena_bytes) return 0;
    for (int tries = 0; tries < 8; tries++) {
        if (arena_alloc(bytes, off)) { arena_set_owner(*off, e ? (int16_t)(e - ent) : -1); return 1; }
        if (!evict_window(bytes)) {
            Ent *v = ent_victim(e);                     /* no window: evict the single LRU entry */
            if (!v) return 0;
            w3d_ent_free(v);
            st_evict++;
        }
    }
    return 0;
}

int w3d_arena_get(uint32_t bytes, uint32_t *off) { return w3d_arena_get_for(NULL, bytes, off); }

#ifndef HLE_PSP
/* host check: every entry with a block owns exactly that block, every used block's owner points back to it */
static void arena_check(const char *where)
{
    static int diag = 10;
    if (diag <= 0) return;
    for (int i = 0; i < nblk; i++) {
        if (i + 1 < nblk && blk[i].off + blk[i].size != blk[i + 1].off) {
            diag--; LOG("world3d ARENA CHECK (%s): block %d off %u size %u, next off %u\n", where, i, blk[i].off,
                        blk[i].size, blk[i + 1].off);
            return;
        }
        if (!blk[i].used) continue;
        int o = blk[i].owner;
        if (o < 0 || o >= W3D_MAXENT || !ent[o].used || ent[o].off != blk[i].off || ent[o].bytes == 0 ||
            ((ent[o].bytes + 63u) & ~63u) != blk[i].size) {
            diag--;
            LOG("world3d ARENA CHECK (%s): block %d off %u size %u owner %d (used %d off %u bytes %u re %08x)\n", where, i,
                blk[i].off, blk[i].size, o, o >= 0 && o < W3D_MAXENT ? ent[o].used : -1,
                o >= 0 && o < W3D_MAXENT ? ent[o].off : 0, o >= 0 && o < W3D_MAXENT ? ent[o].bytes : 0,
                o >= 0 && o < W3D_MAXENT ? ent[o].re : 0);
            return;
        }
    }
    for (int i = 0; i < W3D_MAXENT; i++) {
        if (!ent[i].used || !ent[i].bytes) continue;
        int found = 0, lo = 0, hi = nblk - 1;
        while (lo <= hi) {
            int j = (lo + hi) >> 1;
            if (blk[j].off == ent[i].off) { found = blk[j].used && blk[j].owner == i; break; }
            if (blk[j].off < ent[i].off) lo = j + 1; else hi = j - 1;
        }
        if (!found) {
            diag--;
            LOG("world3d ARENA CHECK (%s): entry %d re %08x off %u bytes %u has no block of its own\n", where, i, ent[i].re,
                ent[i].off, ent[i].bytes);
            return;
        }
    }
}
#else
#define arena_check(w) ((void)0)
#endif

Ent *w3d_ent_new(uint32_t re)
{
    Ent *e = NULL;
    for (int i = 0; i < W3D_MAXENT && !e; i++)
        if (!ent[i].used) e = &ent[i];
    if (!e) {
        e = ent_victim(NULL);
        if (!e) return NULL;
        w3d_ent_free(e);
        st_evict++;
    }
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->re = re;
    unsigned h = hslot(re);
    e->next = hash_head[h];
    hash_head[h] = (int16_t)(e - ent);
    return e;
}

Ent *w3d_ent_find(uint32_t re)
{
    for (int16_t i = hash_head[hslot(re)]; i >= 0; i = ent[i].next)
        if (ent[i].re == re) return &ent[i];
    return NULL;
}

uint32_t w3d_frame_no(void) { return frame_no; }
void w3d_stat_conv(uint32_t bytes) { st_conv++; st_conv_bytes += bytes; }
void w3d_stat_noroom(void) { st_noroom++; }

/* ------------------------------------------------------------------ the native geometry (VIF streams) */
Batch w3d_bat[MAXBATCH];
int w3d_nbat;
#define bat w3d_bat
#define nbat w3d_nbat
static const uint8_t pf_size[] = {0, 8, 6, 16, 12}, uf_size[] = {0, 4, 8, 8, 16}, cf_size[] = {0, 4, 8};

static int ram_ok(uint32_t a, uint32_t n) { return a >= 0x1000u && a < hle_ram_end && n <= hle_ram_end - a; }

/* one VIF UNPACK code -> (format family, element bytes); 0 = not an attribute we use */
static int unpack_fmt(uint32_t code, int *slot, int *vn, int *vl)
{
    uint32_t cmd = code >> 24 & 0x7F;
    if ((cmd & 0x60) != 0x60) return 0;
    *vn = (int)(cmd >> 2 & 3) + 1;
    *vl = (int)(cmd & 3);                 /* 0 = 32-bit, 1 = 16, 2 = 8, 3 = 5551 */
    *slot = (int)(code & 0xFF);
    return 1;
}

typedef struct { uint32_t addr, bytes; int slot, vn, vl; } Pend;

/* slot -> attribute for the building pipelines (0 position, 1 UV, 2 colour); the lit pipelines pass their own map
   (w3d_parse_chain_map) */
static const int8_t slot_map_building[8] = {0, 1, 2, -1, -1, -1, -1, -1};

static int attr_add(Batch *b, const Pend *p, int n, const int8_t *map)
{
    int s = p->slot < 8 ? map[p->slot] : -1, vn = p->vn, vl = p->vl;
    if (s == 0) {
        b->pf = vl == 1 && vn == 4 ? PF_V4_16 : vl == 1 && vn == 3 ? PF_V3_16 : vl == 0 && vn == 4 ? PF_V4_32
              : vl == 0 && vn == 3 ? PF_V3_32 : PF_NONE;
        b->pos = p->addr;
        return b->pf && p->bytes >= (uint32_t)n * pf_size[b->pf] && ram_ok(p->addr, (uint32_t)n * pf_size[b->pf]);
    }
    if (s == 1) {
        b->uf = vl == 1 && vn == 2 ? UF_V2_16 : vl == 0 && vn == 2 ? UF_V2_32 : vl == 1 && vn == 4 ? UF_V4_16
              : vl == 0 && vn == 4 ? UF_V4_32 : UF_NONE;
        b->uv = p->addr;
        return b->uf && p->bytes >= (uint32_t)n * uf_size[b->uf] && ram_ok(p->addr, (uint32_t)n * uf_size[b->uf]);
    }
    if (s == 2) {
        b->cf = vl == 2 && vn == 4 ? CF_V4_8 : vl == 1 && vn == 4 ? CF_V4_16 : CF_NONE;
        b->col = p->addr;
        return b->cf && p->bytes >= (uint32_t)n * cf_size[b->cf] && ram_ok(p->addr, (uint32_t)n * cf_size[b->cf]);
    }
    if (s == 3) {                         /* normals: V4_8 / V3_8 (s8 / 128) */
        b->nf = vl == 2 && vn == 4 ? NF_V4_8 : vl == 2 && vn == 3 ? NF_V3_8 : NF_NONE;
        b->nrm = p->addr;
        uint32_t es = b->nf == NF_V4_8 ? 4u : 3u;
        return b->nf && p->bytes >= (uint32_t)n * es && ram_ok(p->addr, (uint32_t)n * es);
    }
    if (s == 4) {                         /* skin: V4_32 (4 x (float weight with the bone index in the low bits)) */
        b->wf = vl == 0 && vn == 4 ? WF_V4_32 : WF_NONE;
        b->wgt = p->addr;
        return b->wf && p->bytes >= (uint32_t)n * 16u && ram_ok(p->addr, (uint32_t)n * 16u);
    }
    return 1;                             /* other slots are not used */
}

/* walk a mesh's DMA chain into the batch table; returns the number of stream vertices, or < 0 */
int w3d_parse_chain_map(uint32_t data, int strip, const int8_t *map)
{
    Pend pend[8];
    int npend = 0, first = 1, total = 0;
    nbat = 0;
    uint32_t p = data;
    for (int guard = 0; guard < 16384; guard++) {
        if (!ram_ok(p, 16)) return -1;
        uint32_t w0 = gw(p), w1 = gw(p + 4), id = w0 >> 28 & 7, qwc = w0 & 0xFFFF;
        /* the VIF codes: the tag's upper 64 bits, then (CNT / RET / END) the inline qwords */
        uint32_t cs = p + 8, ce = p + 16;
        if (id == 1 || id == 6 || id == 7) ce = p + 16 + qwc * 16u;
        else if (id != 3) return -2;      /* only CNT, REF, RET, END in these chains */
        if (!ram_ok(cs, ce - cs)) return -1;
        for (uint32_t a = cs; a < ce;) {
            uint32_t code = gw(a), cmd = code >> 24 & 0x7F;
            a += 4;
            int slot, vn, vl;
            if (cmd == 0x04) {            /* ITOP: a batch of that many vertices */
                int n = (int)(code & 0x3FF);
                if (n <= 0 || n > 256 || nbat >= MAXBATCH) return -3;
                Batch *b = &bat[nbat];
                memset(b, 0, sizeof *b);
                b->n = (uint16_t)n;
                b->skip = (uint16_t)(first || !strip ? 0 : 2);
                for (int i = 0; i < npend; i++)
                    if (!attr_add(b, &pend[i], n, map)) return -4;
                if (!b->pf) return -5;
                if (b->n > b->skip) { total += b->n - b->skip; nbat++; }
                npend = 0;
                first = 0;
                continue;
            }
            if (unpack_fmt(code, &slot, &vn, &vl)) {
                uint32_t num = code >> 16 & 0xFF;
                if (!num) num = 256;
                uint32_t esz = vl == 3 ? 2u : (uint32_t)vn * (4u >> vl), bytes = (num * esz + 3u) & ~3u;
                Pend *pp = npend < 8 ? &pend[npend++] : NULL;
                if (!pp) return -6;
                if (id == 3 && a == ce) {     /* the last code of a REF tag: its data is the referenced block */
                    pp->addr = w1 & 0x7FFFFFFFu;
                    pp->bytes = qwc * 16u;
                    if (!ram_ok(pp->addr, pp->bytes)) return -1;
                } else {                      /* inline data */
                    pp->addr = a;
                    pp->bytes = bytes;
                    a += bytes;
                }
                pp->slot = slot; pp->vn = vn; pp->vl = vl;
                continue;
            }
            switch (cmd) {
            case 0x20: a += 4; break;                         /* STMASK */
            case 0x30: case 0x31: a += 16; break;             /* STROW / STCOL */
            case 0x4A: { uint32_t n = code >> 16 & 0xFF; a += (n ? n : 256) * 8u; break; }   /* MPG */
            case 0x50: case 0x51: { uint32_t n = code & 0xFFFF; a += (n ? n : 65536) * 16u; break; }   /* DIRECT */
            default: break;                                   /* NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSCAL, FLUSH... */
            }
        }
        if (id == 6 || id == 7) return total;
        p = id == 3 ? p + 16 : p + 16 + qwc * 16u;
    }
    return -7;
}

static int parse_chain(uint32_t data, int strip) { return w3d_parse_chain_map(data, strip, slot_map_building); }

/* ------------------------------------------------------------------ the generic (float) stream walk: any position / UV
   format (the first converter, kept for meshes whose formats the fast walk does not take) */
typedef struct { float u, v; uint32_t c; int16_t x, y, z; uint8_t adc; } SV;

static inline uint32_t clamp255(uint32_t v) { return v > 255 ? 255 : v; }

static void read_sv(const Batch *b, int j, const ColP *cp, SV *o, int want_col)
{
    uint32_t a = b->pos + (uint32_t)j * pf_size[b->pf];
    switch (b->pf) {
    case PF_V4_16: o->x = (int16_t)gh(a); o->y = (int16_t)gh(a + 2); o->z = (int16_t)gh(a + 4); o->adc = gh(a + 6) != 0; break;
    case PF_V3_16: o->x = (int16_t)gh(a); o->y = (int16_t)gh(a + 2); o->z = (int16_t)gh(a + 4); o->adc = 0; break;
    default: {
        float f[4];
        uint32_t w[4];
        for (int i = 0; i < 4; i++) w[i] = gw(a + 4u * (uint32_t)i);
        memcpy(f, w, sizeof f);
        float s[3] = {f[0] * 128.0f, f[1] * 128.0f, f[2] * 128.0f};
        for (int i = 0; i < 3; i++) s[i] = s[i] > 32767.0f ? 32767.0f : s[i] < -32768.0f ? -32768.0f : s[i];
        o->x = (int16_t)s[0]; o->y = (int16_t)s[1]; o->z = (int16_t)s[2];
        o->adc = b->pf == PF_V4_32 && w[3] != 0;
    }
    }
    o->u = o->v = 0.0f;
    if (b->uf) {
        uint32_t ua = b->uv + (uint32_t)j * uf_size[b->uf];
        if (b->uf == UF_V2_32 || b->uf == UF_V4_32) {
            uint32_t w0 = gw(ua), w1 = gw(ua + 4);
            memcpy(&o->u, &w0, 4); memcpy(&o->v, &w1, 4);
        }
        else { o->u = (int16_t)gh(ua) * (1.0f / 4096.0f); o->v = (int16_t)gh(ua + 2) * (1.0f / 4096.0f); }
    }
    if (!want_col) return;
    uint32_t r = 255, g = 255, bl = 255, al = 255;
    if (b->cf == CF_V4_8) {
        uint32_t w = gw(b->col + 4u * (uint32_t)j);
        r = w & 0xFF; g = w >> 8 & 0xFF; bl = w >> 16 & 0xFF; al = w >> 24;
    } else if (b->cf == CF_V4_16) {         /* low byte day, high byte night */
        uint32_t ca = b->col + 8u * (uint32_t)j, c4[4];
        for (int i = 0; i < 4; i++) {
            uint32_t h = gh(ca + 2u * (uint32_t)i);
            c4[i] = ((h & 0xFF) * (256u - cp->b8) + (h >> 8) * cp->b8) >> 8;
        }
        r = c4[0]; g = c4[1]; bl = c4[2]; al = c4[3];
    }
    r = clamp255(r + cp->amb[0]); g = clamp255(g + cp->amb[1]); bl = clamp255(bl + cp->amb[2]);
    if (cp->modulate) {
        r = (r * cp->mat[0] * 257u + 32768u) >> 16; g = (g * cp->mat[1] * 257u + 32768u) >> 16;
        bl = (bl * cp->mat[2] * 257u + 32768u) >> 16; al = (al * cp->mat[3] * 257u + 32768u) >> 16;
    }
    o->c = r | g << 8 | bl << 16 | al << 24;
}

/* the stream walk: pass 1 (dst NULL) counts output vertices / prims / triangles / chunks and the bounds; pass 2 writes
   the vertices (fmt) and, if given, the sub-list and the chunk table. A chunk = consecutive sub-strips (never split)
   until it holds w3d_chunk vertices, with its own bounding box: big meshes are classified (culled / GE / CPU-clipped)
   per chunk, so only the parts near the camera go through the CPU and the off-screen parts never reach the GE. */
static uint32_t chunk_v = 24, chunk_group = 4;
typedef struct { uint32_t nverts, nprims, ntris, nchunks; int16_t pmin[3], pmax[3]; float umin, umax, vmin, vmax; } Count;

static void put_vertex(uint8_t *dst, uint32_t k, int fmt, const SV *s, float ub, float vb)
{
    if (fmt == W3D_FMT_UV16) {
        W3dV16 *o = (W3dV16 *)dst + k;
        float fu = (s->u - ub) * 4096.0f + 0.5f, fv = (s->v - vb) * 4096.0f + 0.5f;
        o->u = (uint16_t)(fu < 0.0f ? 0 : fu > 65535.0f ? 65535 : (int)fu);
        o->v = (uint16_t)(fv < 0.0f ? 0 : fv > 65535.0f ? 65535 : (int)fv);
        o->c = s->c; o->x = s->x; o->y = s->y; o->z = s->z; o->pad = 0;
    } else {
        W3dVF *o = (W3dVF *)dst + k;
        o->u = s->u; o->v = s->v; o->c = s->c; o->x = s->x; o->y = s->y; o->z = s->z; o->pad = 0;
    }
}

static void chunk_bound(int16_t *mn, int16_t *mx, const SV *s)
{
    if (s->x < mn[0]) mn[0] = s->x;
    if (s->y < mn[1]) mn[1] = s->y;
    if (s->z < mn[2]) mn[2] = s->z;
    if (s->x > mx[0]) mx[0] = s->x;
    if (s->y > mx[1]) mx[1] = s->y;
    if (s->z > mx[2]) mx[2] = s->z;
}

static int walk(int strip, uint32_t cap, const ColP *cp, Count *cn, uint8_t *dst, int fmt, float ub, float vb,
                uint32_t *sub, Chunk *ch)
{
    SV h1, h2, cur;                         /* the two previous stream vertices */
    memset(&h1, 0, sizeof h1);
    memset(&h2, 0, sizeof h2);
    uint32_t k = 0, nv = 0, np = 0, nt = 0, run = 0, in_run = 0, nch = 0, ch_p0 = 0, ch_v0 = 0, ch_t0 = 0;
    int16_t cmin[3] = {32767, 32767, 32767}, cmax[3] = {-32768, -32768, -32768};
    int want = dst != NULL;
    if (!want) {
        for (int i = 0; i < 3; i++) { cn->pmin[i] = 32767; cn->pmax[i] = -32768; }
        cn->umin = cn->vmin = 1e30f; cn->umax = cn->vmax = -1e30f;
    }
#define EMIT(s) do { if (want) { put_vertex(dst, nv, fmt, &(s), ub, vb); if (ch) chunk_bound(cmin, cmax, &(s)); } \
                     nv++; run++; } while (0)
#define CHUNK_CLOSE() do { if (np > ch_p0) { if (ch) { Chunk *c_ = &ch[nch]; c_->first_prim = (uint16_t)ch_p0; \
                               c_->nprims = (uint16_t)(np - ch_p0); c_->first_vert = ch_v0; \
                               c_->ntris = (uint16_t)(nt - ch_t0); c_->pad = 0; \
                               for (int q_ = 0; q_ < 3; q_++) { c_->bmin[q_] = cmin[q_]; c_->bmax[q_] = cmax[q_]; \
                                   cmin[q_] = 32767; cmax[q_] = -32768; } } \
                               nch++; ch_p0 = np; ch_v0 = nv; ch_t0 = nt; } } while (0)
#define CLOSE() do { if (in_run) { if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_STRIP << 16 | run); np++; in_run = 0; \
                                   if (nv - ch_v0 >= chunk_v) CHUNK_CLOSE(); } } while (0)
    for (int bi = 0; bi < nbat; bi++) {
        const Batch *b = &bat[bi];
        for (int j = b->skip; j < b->n && k < cap; j++, k++) {
            read_sv(b, j, cp, &cur, want);
            if (!want) {
                if (cur.x < cn->pmin[0]) cn->pmin[0] = cur.x;
                if (cur.y < cn->pmin[1]) cn->pmin[1] = cur.y;
                if (cur.z < cn->pmin[2]) cn->pmin[2] = cur.z;
                if (cur.x > cn->pmax[0]) cn->pmax[0] = cur.x;
                if (cur.y > cn->pmax[1]) cn->pmax[1] = cur.y;
                if (cur.z > cn->pmax[2]) cn->pmax[2] = cur.z;
                if (cur.u < cn->umin) cn->umin = cur.u;
                if (cur.u > cn->umax) cn->umax = cur.u;
                if (cur.v < cn->vmin) cn->vmin = cur.v;
                if (cur.v > cn->vmax) cn->vmax = cur.v;
            }
            if (strip) {
                if (k >= 2) {
                    int draw = !cur.adc;                /* the triangle (k-2, k-1, k) */
                    if (draw && !in_run) {
                        run = 0;
                        in_run = 1;
                        if ((k - 2) & 1) EMIT(h2);      /* odd start: repeat, so the GE parity = the stream's */
                        EMIT(h2);
                        EMIT(h1);
                    }
                    if (draw) { EMIT(cur); nt++; }
                    else CLOSE();
                }
                h2 = h1;
                h1 = cur;
            } else {                                /* triangle list: prims of chunk_v vertices, one chunk each */
                EMIT(cur);
                if (run == chunk_v) {
                    if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_TRIANGLES << 16 | run);
                    np++; nt += run / 3; run = 0;
                    CHUNK_CLOSE();
                }
            }
        }
    }
    if (strip) CLOSE();
    else {
        uint32_t n3 = run - run % 3;
        if (n3) { if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_TRIANGLES << 16 | n3); np++; nt += n3 / 3; }
    }
    CHUNK_CLOSE();
#undef EMIT
#undef CLOSE
#undef CHUNK_CLOSE
    cn->nverts = nv;
    cn->nprims = np;
    cn->ntris = nt;
    cn->nchunks = nch;
    return 0;
}

/* ------------------------------------------------------------------ the fast stream walk: 16-bit positions
   (V4_16 / V3_16), V2_16 UVs or none, V4_8 / V4_16 / no colours - every static building mesh. Host pointers straight
   into the guest arrays (ranges checked by parse_chain), integers only; the same output as the generic walk. Modes:
   COUNT (pass 1: counts, bounds, raw UV range), EMIT (pass 2: vertices, sub-list, chunks), COLOUR (re-bake: only the
   colour word of every output vertex, in the same order). */
static int batches_fast(void)
{
    for (int i = 0; i < nbat; i++) {
        const Batch *b = &bat[i];
        if ((b->pf != PF_V4_16 && b->pf != PF_V3_16) || (b->uf != UF_NONE && b->uf != UF_V2_16)) return 0;
    }
    return 1;
}

static const int16_t zero_uv[2];
static const uint8_t white_c[8] = {255, 255, 255, 255, 255, 255, 255, 255};

enum { FW_COUNT = 0, FW_EMIT = 1, FW_COLOUR = 2 };
typedef struct {
    int strip, fmt;
    uint32_t cap, maxv;                     /* maxv: never write more output vertices (EMIT / COLOUR) */
    int32_t ubi, vbi;                       /* UV16: the base in raw 1/4096 units (floor tile x 4096) */
    ColP cp;
    uint8_t *dst;
    uint32_t *sub;
    Chunk *ch;
    uint32_t nverts, nprims, ntris, nchunks;
    int16_t pmin[3], pmax[3];
    int32_t umin, umax, vmin, vmax;
} FW;
typedef struct { const int16_t *p, *t; const uint8_t *c; uint32_t cf; } VRef;

static inline uint32_t bake_col(const uint8_t *c, uint32_t cf, const ColP *cp)
{
    uint32_t r, g, b, a;
    if (cf == CF_V4_8) {
        uint32_t w;
        memcpy(&w, c, 4);
        if (cp->plain) return w;
        r = w & 0xFF; g = w >> 8 & 0xFF; b = w >> 16 & 0xFF; a = w >> 24;
    } else if (cf == CF_V4_16) {
        uint16_t h[4];
        memcpy(h, c, 8);
        uint32_t bb = cp->b8;
        if (bb == 0) { r = h[0] & 0xFFu; g = h[1] & 0xFFu; b = h[2] & 0xFFu; a = h[3] & 0xFFu; }
        else if (bb == 256) { r = (uint32_t)h[0] >> 8; g = (uint32_t)h[1] >> 8; b = (uint32_t)h[2] >> 8; a = (uint32_t)h[3] >> 8; }
        else {
            uint32_t ib = 256u - bb;
            r = ((h[0] & 0xFFu) * ib + ((uint32_t)h[0] >> 8) * bb) >> 8;
            g = ((h[1] & 0xFFu) * ib + ((uint32_t)h[1] >> 8) * bb) >> 8;
            b = ((h[2] & 0xFFu) * ib + ((uint32_t)h[2] >> 8) * bb) >> 8;
            a = ((h[3] & 0xFFu) * ib + ((uint32_t)h[3] >> 8) * bb) >> 8;
        }
        if (cp->plain) return r | g << 8 | b << 16 | a << 24;
    } else {
        if (cp->plain) return 0xFFFFFFFFu;
        r = g = b = a = 255;
    }
    r = clamp255(r + cp->amb[0]); g = clamp255(g + cp->amb[1]); b = clamp255(b + cp->amb[2]);
    if (cp->modulate) {
        r = (r * cp->mat[0] * 257u + 32768u) >> 16; g = (g * cp->mat[1] * 257u + 32768u) >> 16;
        b = (b * cp->mat[2] * 257u + 32768u) >> 16; a = (a * cp->mat[3] * 257u + 32768u) >> 16;
    }
    return r | g << 8 | b << 16 | a << 24;
}

static inline __attribute__((always_inline)) void fw_put(FW *w, const VRef *v, uint32_t nv, const int mode,
                                                         int16_t *cmin, int16_t *cmax)
{
    uint32_t col = bake_col(v->c, v->cf, &w->cp);
    if (w->fmt == W3D_FMT_UV16) {
        W3dV16 *o = (W3dV16 *)w->dst + nv;
        o->c = col;
        if (mode == FW_COLOUR) return;
        o->u = (uint16_t)(v->t[0] - w->ubi);
        o->v = (uint16_t)(v->t[1] - w->vbi);
        o->x = v->p[0]; o->y = v->p[1]; o->z = v->p[2]; o->pad = 0;
    } else {
        W3dVF *o = (W3dVF *)w->dst + nv;
        o->c = col;
        if (mode == FW_COLOUR) return;
        o->u = v->t[0] * (1.0f / 4096.0f);
        o->v = v->t[1] * (1.0f / 4096.0f);
        o->x = v->p[0]; o->y = v->p[1]; o->z = v->p[2]; o->pad = 0;
    }
    if (w->ch) {
        int16_t x = v->p[0], y = v->p[1], z = v->p[2];
        if (x < cmin[0]) cmin[0] = x;
        if (y < cmin[1]) cmin[1] = y;
        if (z < cmin[2]) cmin[2] = z;
        if (x > cmax[0]) cmax[0] = x;
        if (y > cmax[1]) cmax[1] = y;
        if (z > cmax[2]) cmax[2] = z;
    }
}

static inline __attribute__((always_inline)) void fw_walk(FW *w, const int mode)
{
    VRef h1, h2, cur;
    h1.p = h2.p = zero_uv; h1.t = h2.t = zero_uv; h1.c = h2.c = white_c; h1.cf = h2.cf = CF_NONE;
    uint32_t k = 0, nv = 0, np = 0, nt = 0, run = 0, in_run = 0, nch = 0, ch_p0 = 0, ch_v0 = 0, ch_t0 = 0;
    const uint32_t cv = chunk_v, cap = w->cap;
    int16_t cmin[3] = {32767, 32767, 32767}, cmax[3] = {-32768, -32768, -32768};
    int16_t pmn0 = 32767, pmn1 = 32767, pmn2 = 32767, pmx0 = -32768, pmx1 = -32768, pmx2 = -32768;
    int32_t umn = 0x7FFFFFFF, umx = -0x7FFFFFFF - 1, vmn = 0x7FFFFFFF, vmx = -0x7FFFFFFF - 1;
    uint32_t *sub = mode == FW_EMIT ? w->sub : NULL;
    Chunk *ch = mode == FW_EMIT ? w->ch : NULL;
#define PUT(v_) do { if (mode != FW_COUNT && nv < w->maxv) fw_put(w, &(v_), nv, mode, cmin, cmax); nv++; run++; } while (0)
#define CHUNK_CLOSE() do { if (np > ch_p0) { if (ch) { Chunk *c_ = &ch[nch]; c_->first_prim = (uint16_t)ch_p0; \
                               c_->nprims = (uint16_t)(np - ch_p0); c_->first_vert = ch_v0; \
                               c_->ntris = (uint16_t)(nt - ch_t0); c_->pad = 0; \
                               for (int q_ = 0; q_ < 3; q_++) { c_->bmin[q_] = cmin[q_]; c_->bmax[q_] = cmax[q_]; \
                                   cmin[q_] = 32767; cmax[q_] = -32768; } } \
                               nch++; ch_p0 = np; ch_v0 = nv; ch_t0 = nt; } } while (0)
#define CLOSE() do { if (in_run) { if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_STRIP << 16 | run); np++; in_run = 0; \
                                   if (nv - ch_v0 >= cv) CHUNK_CLOSE(); } } while (0)
    for (int bi = 0; bi < nbat && k < cap; bi++) {
        const Batch *b = &bat[bi];
        const uint32_t ps = b->pf == PF_V4_16 ? 4u : 3u, ts = b->uf ? 2u : 0u;
        const uint32_t cs = b->cf == CF_V4_16 ? 8u : b->cf == CF_V4_8 ? 4u : 0u;
        const int16_t *P = (const int16_t *)(hle_ram + b->pos) + (uint32_t)b->skip * ps;
        const int16_t *T = b->uf ? (const int16_t *)(hle_ram + b->uv) + 2u * b->skip : zero_uv;
        const uint8_t *C = b->cf ? hle_ram + b->col + cs * b->skip : white_c;
        cur.cf = b->cf;
        for (uint32_t j = b->skip; j < b->n && k < cap; j++, k++, P += ps, T += ts, C += cs) {
            cur.p = P; cur.t = T; cur.c = C;
            if (mode == FW_COUNT) {
                int16_t x = P[0], y = P[1], z = P[2];
                if (x < pmn0) pmn0 = x;
                if (x > pmx0) pmx0 = x;
                if (y < pmn1) pmn1 = y;
                if (y > pmx1) pmx1 = y;
                if (z < pmn2) pmn2 = z;
                if (z > pmx2) pmx2 = z;
                int32_t u = T[0], v = T[1];
                if (u < umn) umn = u;
                if (u > umx) umx = u;
                if (v < vmn) vmn = v;
                if (v > vmx) vmx = v;
            }
            if (w->strip) {
                if (k >= 2) {
                    int draw = ps == 3u || P[3] == 0;   /* ADC: the triangle (k-2, k-1, k) is not drawn */
                    if (draw && !in_run) {
                        run = 0;
                        in_run = 1;
                        if ((k - 2) & 1) PUT(h2);       /* odd start: repeat, so the GE parity = the stream's */
                        PUT(h2);
                        PUT(h1);
                    }
                    if (draw) { PUT(cur); nt++; }
                    else CLOSE();
                }
                h2 = h1;
                h1 = cur;
            } else {
                PUT(cur);
                if (run == cv) {
                    if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_TRIANGLES << 16 | run);
                    np++; nt += run / 3; run = 0;
                    CHUNK_CLOSE();
                }
            }
        }
    }
    if (w->strip) CLOSE();
    else {
        uint32_t n3 = run - run % 3;
        if (n3) { if (sub) sub[1 + np] = GE_CMD(0x04, GE_P_TRIANGLES << 16 | n3); np++; nt += n3 / 3; }
    }
    CHUNK_CLOSE();
#undef PUT
#undef CLOSE
#undef CHUNK_CLOSE
    w->nverts = nv; w->nprims = np; w->ntris = nt; w->nchunks = nch;
    if (mode == FW_COUNT) {
        w->pmin[0] = pmn0; w->pmin[1] = pmn1; w->pmin[2] = pmn2;
        w->pmax[0] = pmx0; w->pmax[1] = pmx1; w->pmax[2] = pmx2;
        w->umin = umn; w->umax = umx; w->vmin = vmn; w->vmax = vmx;
    }
}

static void __attribute__((noinline)) fw_count(FW *w) { fw_walk(w, FW_COUNT); }
static void __attribute__((noinline)) fw_emit(FW *w) { fw_walk(w, FW_EMIT); }
static void __attribute__((noinline)) fw_colour(FW *w) { fw_walk(w, FW_COLOUR); }

/* a hash of the chain's first 8 words (8, not 16: the first DMA tag and its VIF codes / REF address;
   one cache line of guest memory read per mesh and frame) */
static uint32_t chain_sig(uint32_t data)
{
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)(hle_ram + (data & (HLE_RAM_SIZE - 1) & ~3u));
    for (int i = 0; i < 8; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

uint32_t w3d_chain_sig(uint32_t data) { return chain_sig(data); }

static uint32_t col_key(const ColP *cp)
{
    uint32_t h = cp->b8 * 2654435761u;
    h ^= (uint32_t)cp->amb[0] | (uint32_t)cp->amb[1] << 8 | (uint32_t)cp->amb[2] << 16;
    h = h * 16777619u ^ ((uint32_t)cp->mat[0] | (uint32_t)cp->mat[1] << 8 | (uint32_t)cp->mat[2] << 16 |
                         (uint32_t)cp->mat[3] << 24);
    return h * 2246822519u ^ (uint32_t)cp->modulate ^ (uint32_t)cp->dn << 1;
}

static uint32_t vtype_of(int fmt) { return (fmt == W3D_FMT_UV16 ? GE_VT_UV16 : GE_VT_UVF) | GE_VT_C8888 | GE_VT_P16; }

/* the generic conversion (the float walk) */
static int convert_slow(Ent *e, uint32_t cap, int strip, const ColP *cp, int n)
{
    Count cn;
    walk(strip, cap ? cap : 0xFFFFFFFFu, cp, &cn, NULL, 0, 0, 0, NULL, NULL);
    if (!cn.nverts || !cn.nprims) { st_conv_fail++; return 0; }
    float ub = 0.0f, vb = 0.0f;
    int fmt = W3D_FMT_UVF;
    if (cn.nverts > 60000) { st_conv_fail++; return 0; }      /* PRIM counts are 16-bit */
    if (cn.umin > -4096.0f && cn.umax < 4096.0f && cn.vmin > -4096.0f && cn.vmax < 4096.0f) {
        ub = (float)(int)cn.umin;                                /* floor (the tile the UVs start in) */
        if (ub > cn.umin) ub -= 1.0f;
        vb = (float)(int)cn.vmin;
        if (vb > cn.vmin) vb -= 1.0f;
        if (cn.umax - ub < 15.99f && cn.vmax - vb < 15.99f) fmt = W3D_FMT_UV16;
    }
    uint32_t stride = fmt == W3D_FMT_UV16 ? 16u : 20u, vbytes = (cn.nverts * stride + 15u) & ~15u;
    uint32_t ch_off = vbytes + 4u * (cn.nprims + 2u);
    uint32_t bytes = ch_off + (uint32_t)sizeof(Chunk) * cn.nchunks, off;
    if (!w3d_arena_get_for(e, bytes, &off)) { st_noroom++; return 0; }
    uint8_t *blkp = arena + off;
    uint32_t *sub = (uint32_t *)(blkp + vbytes);
    sub[0] = GE_CMD(0x12, vtype_of(fmt));
    Count c2;
    walk(strip, cap ? cap : 0xFFFFFFFFu, cp, &c2, blkp, fmt, ub, vb, sub, (Chunk *)(blkp + ch_off));
    sub[1 + c2.nprims] = GE_CMD(0x0B, 0);
    e->nchunks = c2.nchunks;
    e->ch_off = ch_off;
    e->off = off;
    e->bytes = bytes;
    e->vbytes = vbytes;
    e->nverts = cn.nverts;
    e->nprims = cn.nprims;
    e->ntris = cn.ntris;
    e->nstream = cap && cap < (uint32_t)n ? cap : (uint32_t)n;   /* the mesh's index count caps the stream */
    e->fmt = (uint8_t)fmt;
    e->strip = (uint8_t)strip;
    e->ubase = ub;
    e->vbase = vb;
    for (int i = 0; i < 3; i++) { e->bmin[i] = cn.pmin[i] * (1.0f / 128.0f); e->bmax[i] = cn.pmax[i] * (1.0f / 128.0f); }
    st_conv_slow++;
    return 1;
}

/* convert a mesh into a new block (e: key fields set) */
static int convert(Ent *e, uint32_t cap, int strip, const ColP *cp)
{
    uint32_t t0 = w3d_plat_us();
    int n = parse_chain(e->data, strip), ok;
    if (n <= 0) { st_conv_fail++; return 0; }
    e->fast = (uint8_t)batches_fast();
    if (!e->fast) ok = convert_slow(e, cap, strip, cp, n);
    else {
        FW w;
        memset(&w, 0, sizeof w);
        w.strip = strip;
        w.cap = cap ? cap : 0xFFFFFFFFu;
        w.cp = *cp;
        fw_count(&w);
        ok = 0;
        if (!w.nverts || !w.nprims || w.nverts > 60000) st_conv_fail++;      /* PRIM counts are 16-bit */
        else {
            int32_t ubi = w.umin >> 12 << 12, vbi = w.vmin >> 12 << 12;   /* floor tile (arithmetic shift) x 4096 */
            int fmt = w.umax - ubi <= 65495 && w.vmax - vbi <= 65495 ? W3D_FMT_UV16 : W3D_FMT_UVF;   /* < 15.99 tiles */
            uint32_t stride = fmt == W3D_FMT_UV16 ? 16u : 20u, vbytes = (w.nverts * stride + 15u) & ~15u;
            uint32_t ch_off = vbytes + 4u * (w.nprims + 2u);
            uint32_t bytes = ch_off + (uint32_t)sizeof(Chunk) * w.nchunks, off;
            if (!w3d_arena_get_for(e, bytes, &off)) st_noroom++;
            else {
                uint8_t *blkp = arena + off;
                uint32_t *sub = (uint32_t *)(blkp + vbytes);
                sub[0] = GE_CMD(0x12, vtype_of(fmt));
                uint32_t nv = w.nverts, np = w.nprims, nt = w.ntris;
                w.fmt = fmt;
                w.maxv = nv;
                w.ubi = ubi;
                w.vbi = vbi;
                w.dst = blkp;
                w.sub = sub;
                w.ch = (Chunk *)(blkp + ch_off);
                fw_emit(&w);
                sub[1 + w.nprims] = GE_CMD(0x0B, 0);
                e->nchunks = w.nchunks;
                e->ch_off = ch_off;
                e->off = off;
                e->bytes = bytes;
                e->vbytes = vbytes;
                e->nverts = nv;
                e->nprims = np;
                e->ntris = nt;
                e->nstream = cap && cap < (uint32_t)n ? cap : (uint32_t)n;
                e->fmt = (uint8_t)fmt;
                e->strip = (uint8_t)strip;
                e->ubase = (float)(ubi >> 12);
                e->vbase = (float)(vbi >> 12);
                for (int i = 0; i < 3; i++) {
                    e->bmin[i] = w.pmin[i] * (1.0f / 128.0f);
                    e->bmax[i] = w.pmax[i] * (1.0f / 128.0f);
                }
                ok = 1;
            }
        }
    }
    if (!ok) return 0;
    e->ckey = col_key(cp);
    arena_check("convert");
    w3d_plat_flush(arena + e->off, e->bytes);
    st_conv++;
    st_conv_bytes += e->bytes;
    uint32_t dt = w3d_plat_us() - t0;
    u_conv += dt;
    if (dt > conv_max_us) conv_max_us = dt;
    return 1;
}

/* re-bake the colours of a converted mesh into dst (its own block, or a copy of it in GE list memory); 0 = the chain
   no longer gives the converted vertex count (the mesh changed under the same addresses: the caller re-converts) */
static uint64_t st_rebake_mismatch;
static int rebake(Ent *e, const ColP *cp, uint8_t *dst)
{
    if (parse_chain(e->data, e->strip) <= 0) return 0;
    if (dst != arena + e->off) memcpy(dst, arena + e->off, e->vbytes);   /* the copy needs the positions / UVs too */
    uint32_t got;
    if (e->fast) {
        FW w;
        memset(&w, 0, sizeof w);
        w.strip = e->strip;
        w.fmt = e->fmt;
        w.cap = e->nstream;
        w.maxv = e->nverts;
        w.cp = *cp;
        w.dst = dst;
        fw_colour(&w);
        got = w.nverts;
    } else {
        Count c2;
        walk(e->strip, e->nstream, cp, &c2, NULL, 0, 0, 0, NULL, NULL);   /* count first (the generic walk has no limit) */
        got = c2.nverts;
        if (got == e->nverts) walk(e->strip, e->nstream, cp, &c2, dst, e->fmt, e->ubase, e->vbase, NULL, NULL);
    }
    st_rebake_bytes += e->vbytes;
    if (got != e->nverts) {
        static int diag = 6;
        st_rebake_mismatch++;
        if (diag > 0) {
            diag--;
            LOG("world3d: re-bake of mesh %08x: the chain gives %u vertices, the cache holds %u - re-converted\n", e->re,
                got, e->nverts);
        }
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ matrices (driver layout: right, up, at, pos rows;
   v' = v M) */
void w3d_drv_mat(uint32_t a, M43 *o)
{
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 3; k++) {
            uint32_t w = gw(a + 16u * (uint32_t)r + 4u * (uint32_t)k);
            memcpy(&o->m[r][k], &w, 4);
        }
}

void w3d_mat_mul(const M43 *a, const M43 *b, M43 *o)
{
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 3; k++)
            o->m[r][k] = a->m[r][0] * b->m[0][k] + a->m[r][1] * b->m[1][k] + a->m[r][2] * b->m[2][k] + (r == 3 ? b->m[3][k] : 0.0f);
}

static inline void xform(const M43 *m, float x, float y, float z, float *o)
{
    for (int k = 0; k < 3; k++) o[k] = x * m->m[0][k] + y * m->m[1][k] + z * m->m[2][k] + m->m[3][k];
}

/* ------------------------------------------------------------------ the camera */
W3dCam w3d_cam;
#define cam w3d_cam
static uint32_t cam_sig;
static float cn_min = 1e30f, cn_max = 0.0f, cf_min = 1e30f, cf_max = 0.0f;   /* near / far planes seen */

int w3d_camera_setup(uint32_t c)
{
    if (!ram_ok(c, 0xA0)) return 0;
    if (gw(c + 0x14) != 1) return 0;        /* perspective only (parallel cameras: shadows etc. stay on the guest) */
    float n, f, fog;
    uint32_t wn = gw(c + 0x80), wf = gw(c + 0x84), wg = gw(c + 0x88);
    memcpy(&n, &wn, 4); memcpy(&f, &wf, 4); memcpy(&fog, &wg, 4);
    if (!(n > 0.0f) || !(f > n)) return 0;
    /* statistics: the camera near / far planes the game uses */
    if (n < cn_min) cn_min = n;
    if (n > cn_max) cn_max = n;
    if (f < cf_min) cf_min = f;
    if (f > cf_max) cf_max = f;
    uint32_t sig = c ^ wn * 3u ^ wf * 7u ^ wg * 11u ^ (uint32_t)w3d_opt_near_mm * 13u;
    if (sig == cam_sig && cam.camera == c) return 1;
    cam_sig = sig;
    cam.camera = c;
    cam.near_z = n;
    cam.far_z = f;
    cam.fog_z = fog;
    /* the GE's near plane: the camera's, or a floor (w3d_near_mm / 1000 units; 16-bit Z and UV precision scale with it -
       geometry closer than the floor is clipped by the GE) */
    float ng = (float)w3d_opt_near_mm * 0.001f;
    if (!(ng > n) || ng >= f * 0.5f) ng = n;
    float A = (f + ng) / (f - ng), B = -2.0f * f * ng / (f - ng);
    float *p = cam.proj;                    /* columns x, y, z, w (each = outputs xc, yc, zc, wc) */
    memset(p, 0, sizeof cam.proj);
    p[0] = 2.0f;                            /* X: xc += 2X */
    p[5] = -2.0f;                           /* Y: yc -= 2Y */
    p[8] = -1.0f; p[9] = 1.0f; p[10] = A; p[11] = 1.0f;   /* Z: xc -= Z, yc += Z, zc += A Z, wc = Z */
    p[14] = B;                              /* 1: zc += B */
    return 1;
}

/* ------------------------------------------------------------------ classification and the CPU clipper */
/* the near plane the CPU path keeps (w3d_gnear=0: a bit in front of the GE's; 1: none - the GE clips it) */
static inline float zsafe(void) { return w3d_opt_gnear ? -1e30f : cam.near_z * NEAR_SAFE; }

/* outcodes of a view-space point: bits 0-4 guard planes (and the safe near plane), 8-13 the frustum */
static inline unsigned outcode(const float *p, float zn_safe)
{
    float X = p[0], Y = p[1], Z = p[2];
    unsigned o = 0;
    if ((GUARD_X + 1.0f) * Z - 2.0f * X < 0.0f) o |= 1;
    if (2.0f * X + (GUARD_X - 1.0f) * Z < 0.0f) o |= 2;
    if ((GUARD_Y - 1.0f) * Z + 2.0f * Y < 0.0f) o |= 4;
    if ((GUARD_Y + 1.0f) * Z - 2.0f * Y < 0.0f) o |= 8;
    if (Z < zn_safe) o |= 16;
    if (Z - X < 0.0f) o |= 0x100;
    if (X < 0.0f) o |= 0x200;
    if (Y < 0.0f) o |= 0x400;
    if (Z - Y < 0.0f) o |= 0x800;
    if (Z < cam.near_z) o |= 0x1000;
    if (Z > cam.far_z) o |= 0x2000;
    return o;
}

/* a box against the planes: each plane is linear in (X, Y, Z), so over the 8 corners its value is L(centre) +- the sum
   of |L(half extent vector)| - the extreme corners without transforming all 8 */
int w3d_classify_box(const float *bmin, const float *bmax, const M43 *m)
{
    float c[3], e[3][3];
    xform(m, (bmin[0] + bmax[0]) * 0.5f, (bmin[1] + bmax[1]) * 0.5f, (bmin[2] + bmax[2]) * 0.5f, c);
    for (int a = 0; a < 3; a++) {
        float h = (bmax[a] - bmin[a]) * 0.5f;
        for (int k = 0; k < 3; k++) e[a][k] = m->m[a][k] * h;
    }
    /* the planes written out (no multiplications by the zero coefficients: with strict IEEE math the compiler
       keeps them) - same values as a generic plane formula. For a plane value L = ax X + ay Y + az Z + k:
       centre v = L(c), radius r = sum over the 3 box axes of |ax e_a.x + ay e_a.y + az e_a.z| */
    const float X = c[0], Y = c[1], Z = c[2];
    const float x0 = e[0][0], y0 = e[0][1], z0 = e[0][2], x1 = e[1][0], y1 = e[1][1], z1 = e[1][2];
    const float x2 = e[2][0], y2 = e[2][1], z2 = e[2][2];
    const float rx = fabsf(x0) + fabsf(x1) + fabsf(x2), ry = fabsf(y0) + fabsf(y1) + fabsf(y2);
    const float rz = fabsf(z0) + fabsf(z1) + fabsf(z2);
    float v, r;
    int fr_in = 1;
    /* the frustum: every corner outside one plane -> culled */
    v = Z - X; r = fabsf(z0 - x0) + fabsf(z1 - x1) + fabsf(z2 - x2);
    if (v + r < 0.0f) return CL_CULL;
    fr_in &= v - r >= 0.0f;                                                   /* Z - X >= 0 */
    if (X + rx < 0.0f) return CL_CULL;
    fr_in &= X - rx >= 0.0f;                                                  /* X >= 0 */
    if (Y + ry < 0.0f) return CL_CULL;
    fr_in &= Y - ry >= 0.0f;                                                  /* Y >= 0 */
    v = Z - Y; r = fabsf(z0 - y0) + fabsf(z1 - y1) + fabsf(z2 - y2);
    if (v + r < 0.0f) return CL_CULL;
    fr_in &= v - r >= 0.0f;                                                   /* Z - Y >= 0 */
    v = Z - cam.near_z;
    if (v + rz < 0.0f) return CL_CULL;
    fr_in &= v - rz >= 0.0f;                                                  /* Z >= near */
    v = cam.far_z - Z;
    if (v + rz < 0.0f) return CL_CULL;
    fr_in &= v - rz >= 0.0f;                                                  /* Z <= far */
    if (fr_in && w3d_opt_gnear) return CL_HW | CL_INSIDE;   /* entirely visible (so inside the guard band too) */
    /* the guard band (and, w3d_gnear=0, the safe near plane): every corner inside all -> the GE draws it (the guard
       planes pass through the eye, so their inside is w >= 0: a corner behind the eye is never "inside") */
    const float gxp = GUARD_X + 1.0f, gxm = GUARD_X - 1.0f, gyp = GUARD_Y + 1.0f, gym = GUARD_Y - 1.0f;
    v = gxp * Z - 2.0f * X;
    r = fabsf(gxp * z0 - 2.0f * x0) + fabsf(gxp * z1 - 2.0f * x1) + fabsf(gxp * z2 - 2.0f * x2);
    if (v - r < 0.0f) return CL_CPU;
    v = 2.0f * X + gxm * Z;
    r = fabsf(2.0f * x0 + gxm * z0) + fabsf(2.0f * x1 + gxm * z1) + fabsf(2.0f * x2 + gxm * z2);
    if (v - r < 0.0f) return CL_CPU;
    v = 2.0f * Y + gym * Z;
    r = fabsf(2.0f * y0 + gym * z0) + fabsf(2.0f * y1 + gym * z1) + fabsf(2.0f * y2 + gym * z2);
    if (v - r < 0.0f) return CL_CPU;
    v = gyp * Z - 2.0f * Y;
    r = fabsf(gyp * z0 - 2.0f * y0) + fabsf(gyp * z1 - 2.0f * y1) + fabsf(gyp * z2 - 2.0f * y2);
    if (v - r < 0.0f) return CL_CPU;
    if (!w3d_opt_gnear && Z - cam.near_z * NEAR_SAFE - rz < 0.0f) return CL_CPU;
    return CL_HW;
}

int w3d_classify_chunk(const Chunk *c, const M43 *m, float scale)
{
    float mn[3], mx[3];
    for (int i = 0; i < 3; i++) { mn[i] = c->bmin[i] * scale; mx[i] = c->bmax[i] * scale; }
    return w3d_classify_box(mn, mx, m) & 3;
}

/* the world matrix the GE holds now (1 = the atomic's, 2 = identity, 0 = unknown) */
static int world_cur;
static const float *world_gm;
static const float w3d_ident[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
static void set_world(int which)
{
    if (world_cur == which) return;
    w3d_be_world(which == 1 ? world_gm : w3d_ident);
    world_cur = which;
}
void w3d_world_reset(const float *gm) { world_cur = 0; world_gm = gm; }
void w3d_world_set(int which) { set_world(which); }

#define MAXCHUNK 4096                       /* 60000 vertices / w3d_chunk 24 = 2500 chunks at most */
static uint8_t chcls[MAXCHUNK];
/* a vertex of the CPU clipper: view-space position (the plane tests), model-space position (what the
   GE draws - s16 / 32768 units with the atomic's own world matrix, so an unclipped vertex goes through exactly the
   arithmetic of the GE path and shared edges stay watertight), UV, colour */
typedef struct { float p[3], mp[3], u, v, c[4]; } CV;
#define CPU_OUT 768
static W3dVC cpu_out[CPU_OUT];
static int ncpu_out;
#define CPU_IDX 6144
static uint16_t cpu_idx[CPU_IDX];
static int ncpu_idx;
static const W3dMeshView *cpu_mv;           /* the mesh the CPU path is working on */
static const uint8_t *cpu_vover;
/* the CPU path's draws of the first alpha pass, drawn again by the second (no second CPU pass) */
#define MAXREC 64
static struct { const void *copy; int n, kind; } rec[MAXREC];
static int nrec, rec_on, rec_overflow;
static uint64_t st_cpu_replays;

static void cpu_flush(void)
{
    if (ncpu_idx) {                         /* unclipped triangles: indices into the cached block */
        set_world(1);
        const void *cp = w3d_be_indexed(cpu_mv, cpu_vover, cpu_idx, ncpu_idx);
        if (rec_on) { if (nrec < MAXREC && cp) { rec[nrec].copy = cp; rec[nrec].n = ncpu_idx; rec[nrec++].kind = 0; } else rec_overflow = 1; }
        st_tris_cpu_idx += (uint64_t)ncpu_idx / 3;
        ncpu_idx = 0;
    }
    if (ncpu_out) {                         /* clipped triangles: model-space floats, the atomic's world matrix */
        set_world(1);
        const void *cp = w3d_be_tris(cpu_out, ncpu_out);
        if (rec_on) { if (nrec < MAXREC && cp) { rec[nrec].copy = cp; rec[nrec].n = ncpu_out; rec[nrec++].kind = 1; } else rec_overflow = 1; }
        st_tris_cpu_out += (uint64_t)ncpu_out / 3;
        ncpu_out = 0;
    }
}

static void out_idx(uint32_t a, uint32_t b, uint32_t c)
{
    if (ncpu_idx + 3 > CPU_IDX) cpu_flush();
    cpu_idx[ncpu_idx++] = (uint16_t)a;
    cpu_idx[ncpu_idx++] = (uint16_t)b;
    cpu_idx[ncpu_idx++] = (uint16_t)c;
}

static uint32_t pack_c(const float *c)
{
    uint32_t r[4];
    for (int i = 0; i < 4; i++) { float v = c[i] + 0.5f; r[i] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint32_t)v; }
    return r[0] | r[1] << 8 | r[2] << 16 | r[3] << 24;
}

static void out_tri(const CV *a, const CV *b, const CV *c)
{
    if (ncpu_out + 3 > CPU_OUT) cpu_flush();
    const CV *t[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        W3dVC *o = &cpu_out[ncpu_out++];
        o->u = t[i]->u; o->v = t[i]->v; o->c = pack_c(t[i]->c);
        o->x = t[i]->mp[0]; o->y = t[i]->mp[1]; o->z = t[i]->mp[2];
    }
}

static float plane_d(int pl, const float *p, float zs)
{
    float X = p[0], Y = p[1], Z = p[2];
    switch (pl) {
    case 0: return (GUARD_X + 1.0f) * Z - 2.0f * X;
    case 1: return 2.0f * X + (GUARD_X - 1.0f) * Z;
    case 2: return (GUARD_Y - 1.0f) * Z + 2.0f * Y;
    case 3: return (GUARD_Y + 1.0f) * Z - 2.0f * Y;
    default: return Z - zs;
    }
}

static void clip_tri(const CV *a, const CV *b, const CV *c, unsigned planes, float zs)
{
    CV buf[2][12];
    int n = 3, cur = 0;
    buf[0][0] = *a; buf[0][1] = *b; buf[0][2] = *c;
    for (int pl = 0; pl < 5; pl++) {
        if (!(planes & 1u << pl)) continue;
        const CV *in = buf[cur];
        CV *out = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const CV *p = &in[i], *q = &in[(i + 1) % n];
            float dp = plane_d(pl, p->p, zs), dq = plane_d(pl, q->p, zs);
            if (dp >= 0.0f && m < 12) out[m++] = *p;
            if ((dp >= 0.0f) != (dq >= 0.0f) && m < 12) {
                /* the intersection from a canonical endpoint (lexicographic view position), so that two triangles
                   sharing the edge compute the same point bit for bit, whatever their winding */
                const CV *a = p, *b = q;
                float da = dp, db = dq;
                if (q->p[0] < p->p[0] || (q->p[0] == p->p[0] && (q->p[1] < p->p[1] ||
                                                                 (q->p[1] == p->p[1] && q->p[2] < p->p[2])))) {
                    a = q; b = p; da = dq; db = dp;
                }
                float t = da / (da - db);
                CV *r = &out[m++];
                for (int k = 0; k < 3; k++) {
                    r->p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
                    r->mp[k] = a->mp[k] + (b->mp[k] - a->mp[k]) * t;
                }
                r->u = a->u + (b->u - a->u) * t;
                r->v = a->v + (b->v - a->v) * t;
                for (int k = 0; k < 4; k++) r->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
            }
        }
        n = m;
        cur ^= 1;
        if (n < 3) return;
    }
    st_tris_cpu_clipped++;
    for (int i = 1; i + 1 < n; i++) out_tri(&buf[cur][0], &buf[cur][i], &buf[cur][i + 1]);
}

/* full attributes of a block vertex (only for triangles that are really clipped) */
static void load_cv(const Ent *e, const uint8_t *verts, uint32_t k, const float *xyz, CV *o)
{
    uint32_t c;
    const int16_t *q;
    if (e->fmt == W3D_FMT_UV16) {
        const W3dV16 *v = (const W3dV16 *)verts + k;
        c = v->c;
        q = &v->x;
        o->u = v->u * (1.0f / 32768.0f);    /* GE units: x TexScale 8 + TexOffset base gives the UV */
        o->v = v->v * (1.0f / 32768.0f);
    } else {
        const W3dVF *v = (const W3dVF *)verts + k;
        c = v->c;
        q = &v->x;
        o->u = v->u; o->v = v->v;
    }
    o->p[0] = xyz[0]; o->p[1] = xyz[1]; o->p[2] = xyz[2];
    for (int i = 0; i < 3; i++) o->mp[i] = q[i] * (1.0f / 32768.0f);   /* exactly what the GE reads from s16 */
    for (int i = 0; i < 4; i++) o->c[i] = (float)(c >> (8 * i) & 0xFF);
}

/* a transformed vertex of the CPU path: view-space position, outcode, block index */
typedef struct { float p[3]; unsigned oc; uint32_t k; } TV;

/* branchless outcodes (the sign bit of each plane value; the Allegrex has no branch prediction and every FPU
   compare is a branch here) - same planes as outcode() (a value of -0.0 counts as outside: conservative) */
static inline uint32_t sgn(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 31; }

typedef struct { float m00, m01, m02, m10, m11, m12, m20, m21, m22, m30, m31, m32, zs, zn, zf; } XM;

static inline __attribute__((always_inline)) void load_tv(const XM *x, const uint8_t *verts, uint32_t stride,
                                                          uint32_t poff, uint32_t k, TV *o)
{
    const int16_t *q = (const int16_t *)(verts + k * stride + poff);
    float px = q[0], py = q[1], pz = q[2];
    float X = px * x->m00 + py * x->m10 + pz * x->m20 + x->m30;
    float Y = px * x->m01 + py * x->m11 + pz * x->m21 + x->m31;
    float Z = px * x->m02 + py * x->m12 + pz * x->m22 + x->m32;
    o->p[0] = X; o->p[1] = Y; o->p[2] = Z;
    float X2 = X + X, Y2 = Y + Y;
    o->oc = sgn((GUARD_X + 1.0f) * Z - X2) | sgn(X2 + (GUARD_X - 1.0f) * Z) << 1 |
            sgn((GUARD_Y - 1.0f) * Z + Y2) << 2 | sgn((GUARD_Y + 1.0f) * Z - Y2) << 3 | sgn(Z - x->zs) << 4 |
            sgn(Z - X) << 8 | sgn(X) << 9 | sgn(Y) << 10 | sgn(Z - Y) << 11 | sgn(Z - x->zn) << 12 | sgn(x->zf - Z) << 13;
    o->k = k;
}

static inline __attribute__((always_inline)) void cpu_tri(const Ent *e, const uint8_t *verts, const TV *a,
                                                          const TV *b, const TV *c, float zs)
{
    if (a->oc & b->oc & c->oc & 0x3F00) return;   /* outside one frustum plane */
    unsigned any = (a->oc | b->oc | c->oc) & 0x1F;
#ifndef HLE_PSP
    if (!any) {                                   /* host check: an accepted triangle lies in the guard pyramid */
        static int diag_acc = 6;
        const TV *t3[3] = {a, b, c};
        for (int i = 0; i < 3 && diag_acc > 0; i++)
            if (!(t3[i]->p[2] > 0.0f) || fabsf(2.0f * t3[i]->p[0] - t3[i]->p[2]) > 8.6f * t3[i]->p[2]) {
                diag_acc--;
                LOG("world3d: CPU PATH ACCEPT CHECK: mesh %08x vertex k %u view %.3f %.3f %.3f oc %03x (a k %u oc %03x, "
                    "b k %u oc %03x, c k %u oc %03x)\n", e->re, t3[i]->k, t3[i]->p[0], t3[i]->p[1], t3[i]->p[2],
                    t3[i]->oc, a->k, a->oc, b->k, b->oc, c->k, c->oc);
                break;
            }
    }
#endif
    if (!any) { out_idx(a->k, b->k, c->k); return; }   /* inside the guard band: the GE transforms the cached vertices */
    CV va, vb, vc;
    load_cv(e, verts, a->k, a->p, &va);
    load_cv(e, verts, b->k, b->p, &vb);
    load_cv(e, verts, c->k, c->p, &vc);
    clip_tri(&va, &vb, &vc, any, zs);
}

/* the CPU path of a range of PRIMs (a chunk) of a mesh: transform + outcode every vertex once, accept / reject / clip
   every triangle (the GE's strip winding: odd triangles flipped) */
static void cpu_range(const Ent *e, const uint8_t *verts, const M43 *m, const W3dMeshView *mv, uint32_t p0,
                      uint32_t np, uint32_t k0)
{
    cpu_mv = mv;
    cpu_vover = verts;
    const float s = 1.0f / 128.0f;              /* the mesh matrix pre-scaled for the raw s16 positions */
    XM x = {m->m[0][0] * s, m->m[0][1] * s, m->m[0][2] * s, m->m[1][0] * s, m->m[1][1] * s, m->m[1][2] * s,
            m->m[2][0] * s, m->m[2][1] * s, m->m[2][2] * s, m->m[3][0], m->m[3][1], m->m[3][2], zsafe(), cam.near_z,
            cam.far_z};
    const uint32_t *sub = (const uint32_t *)(arena + e->off + e->vbytes);
    const uint32_t stride = e->fmt == W3D_FMT_UV16 ? 16u : 20u, poff = e->fmt == W3D_FMT_UV16 ? 8u : 12u;
    float zs = x.zs;
    uint32_t k = k0, nv = 0, ntri = 0;
#ifndef HLE_PSP
    {
        static int diag_k = 6;
        uint32_t span = 0;
        for (uint32_t p = p0; p < p0 + np; p++) span += sub[1 + p] & 0xFFFF;
        if ((k0 + span > e->nverts || p0 + np > e->nprims) && diag_k > 0) {
            diag_k--;
            LOG("world3d: CPU RANGE CHECK: mesh %08x off %u bytes %u nverts %u nprims %u nchunks %u ch_off %u strip %d: "
                "prims %u+%u first vertex %u span %u (mv nverts %u)\n", e->re, e->off, e->bytes, e->nverts, e->nprims,
                e->nchunks, e->ch_off, e->strip, p0, np, k0, span, mv->nverts);
        }
    }
#endif
    for (uint32_t p = p0; p < p0 + np; p++) {
        uint32_t w = sub[1 + p], cnt = w & 0xFFFF, prim = w >> 16 & 7;
        TV v[3];
        if (prim == GE_P_TRIANGLES) {
            for (uint32_t i = 0; i + 2 < cnt; i += 3) {
                load_tv(&x, verts, stride, poff, k + i, &v[0]);
                load_tv(&x, verts, stride, poff, k + i + 1, &v[1]);
                load_tv(&x, verts, stride, poff, k + i + 2, &v[2]);
                cpu_tri(e, verts, &v[0], &v[1], &v[2], zs);
            }
            nv += cnt; ntri += cnt / 3;
        } else {
            TV *a = &v[0], *b = &v[1], *c = &v[2];
            for (uint32_t i = 0; i < cnt; i++) {
                load_tv(&x, verts, stride, poff, k + i, c);
                if (i >= 2) {
                    if ((i - 2) & 1) cpu_tri(e, verts, b, a, c, zs);    /* odd: the GE flips it */
                    else cpu_tri(e, verts, a, b, c, zs);
                }
                TV *t = a; a = b; b = c; c = t;     /* rotate: a, b = the two previous vertices */
            }
            nv += cnt; ntri += cnt > 2 ? cnt - 2 : 0;
        }
        k += cnt;
    }
    st_cpu_verts += nv;
    st_tris_cpu_in += ntri;
}

/* ------------------------------------------------------------------ one atomic */
static uint32_t gwk(uint32_t a) { return ram_ok(a, 4) ? gw(a) : 0; }
static float gfk(uint32_t a) { uint32_t w = gwk(a); float f; memcpy(&f, &w, 4); return f; }
uint32_t w3d_gwk(uint32_t a) { return gwk(a); }

/* W3D_BB_LTM: the driver's frame world-matrix getter 0x33FAC0 returns the frame's matrix at +0x50, after re-syncing
   the hierarchy (0x344378) when the root frame (+0xA0) has its dirty bit (+3, bit 0) set. The in-sync case natively:
   no guest call (dispatch + translated code + its I-cache lines per atomic); the other case, or any address outside
   RAM, runs the guest function. The clean path writes no guest memory and no register the hook
   keeps (v0 is set by the hook's return; a0 / a1 are restored by the hook wrapper). */
static uint64_t st_bb_ltm_native, st_bb_ltm_guest;
uint32_t w3d_frame_ltm(R5900Ctx *c, uint32_t frame)
{
    if ((w3d_opt_bb & W3D_BB_LTM) && ram_ok(frame, 0xA4)) {
        uint32_t root = gw(frame + 0xA0);
        if (ram_ok(root, 4) && !(gb(root + 3) & 1)) { st_bb_ltm_native++; return frame + 0x50u; }
    }
    st_bb_ltm_guest++;
    return hle_call(c, FN_FRAME_MATRIX, 1, frame);
}

static void count_pds(uint32_t pds)
{
    int i = 0;
    while (i < npds && pds_ids[i] != pds) i++;
    if (i == npds && npds < 8) pds_ids[npds++] = pds;
    if (i < 8) pds_calls[i]++;
}

/* the driver's render states every atomic starts from (drv_set_state, tracked by hle_render.c) */
void w3d_base_state(W3dState *st)
{
    memset(st, 0, sizeof *st);
    st->ztest = hle_render_rs(RS_ZTESTENABLE) != 0;
    st->zwrite = hle_render_rs(RS_ZWRITEENABLE) != 0;
    uint32_t cm = hle_render_rs(RS_CULLMODE);
    st->cull = !w3d_opt_cull ? 0 : cm == 2 ? 1 : cm == 3 ? 2 : 0;
    st->src_blend = (int)hle_render_rs(RS_SRCBLEND);
    st->dst_blend = (int)hle_render_rs(RS_DESTBLEND);
    if (!st->src_blend) st->src_blend = 5;
    if (!st->dst_blend) st->dst_blend = 6;
    st->fog = w3d_opt_fog && hle_render_rs(RS_FOGENABLE) && cam.fog_z < cam.far_z;
    st->fog_color = hle_render_rs(RS_FOGCOLOR);
}

/* PS2 alpha (the PS2 blend / test rules + texconv class) -> blend / alpha test / passes */
int w3d_alpha_passes(W3dState *st, int acl, int valpha)
{
    int passes = 1;
    if (acl == 2 || valpha) {
        st->blend = 1;
        if (acl >= 1 && w3d_opt_afail) { st->atest = 2; passes = 2; }
        else st->atest = acl >= 1 ? 1 : 0;
    } else if (acl == 1) { st->blend = 0; st->atest = 1; }
    else { st->blend = 0; st->atest = 0; }
    return passes;
}

/* the texture of a material (filter / addressing from the texture's +0x50); 0 = not drawable */
int w3d_bind_material_texture(uint32_t mat, uint32_t *ras_out, int *acl, int *wrap)
{
    return w3d_bind_texture(mat ? gwk(mat) : 0, ras_out, acl, wrap);
}

/* a texture chosen per draw (the vehicle draw paths swap paint-job / lights-on textures) */
int w3d_bind_texture(uint32_t tex, uint32_t *ras_out, int *acl, int *wrap)
{
    uint32_t ras = tex ? gwk(tex) : 0, fa = tex ? gwk(tex + 0x50) : 0;
    *acl = 0;
    *ras_out = ras;
    uint32_t au = fa >> 8 & 0xF, av = fa >> 12 & 0xF;    /* addressing: 3 CLAMP, 4 BORDER, else repeating */
    *wrap = (au != 3 && au != 4 ? 1 : 0) | (av != 3 && av != 4 ? 2 : 0);
    int ok = w3d_be_texture(ras, (int)(fa & 0xFF), (int)(fa >> 8 & 0xF), (int)(fa >> 12 & 0xF), acl);
    if (!ok) st_m_notex++;
    return ok;
}

/* draw one converted mesh with its classification (whole / GE chunk ranges / CPU path); st already set.
   pass: 0 = the only pass, 1 = the first of two (the CPU path's draws are recorded), 2 = the second (they are drawn
   again from the first pass's list copies when the GE epoch did not change; else the CPU path runs again) */
static uint32_t rec_epoch;
static void draw_classified(Ent *e, const M43 *M, const uint8_t *vover, int whole, uint32_t nch, uint32_t ncpu,
                            int pass)
{
    W3dMeshView mv;
    mv.verts = arena + e->off;
    mv.sub = (const uint32_t *)(arena + e->off + e->vbytes);
    mv.nverts = e->nverts; mv.nprims = e->nprims; mv.nsub = e->nprims + 2; mv.ntris = e->ntris;
    mv.fmt = e->fmt; mv.prim = e->strip ? W3D_PRIM_STRIP : W3D_PRIM_LIST;
    mv.stride = e->fmt == W3D_FMT_UV16 ? 16 : 20;
    mv.ubase = e->ubase; mv.vbase = e->vbase;
    if (whole) {                    /* the whole mesh on the GE: one CALL of its sub-list */
        set_world(1);
        w3d_be_mesh(&mv, vover);
        st_tris_hw += e->ntris;
        return;
    }
    const Chunk *chs = (const Chunk *)(arena + e->off + e->ch_off);
    const uint8_t *vb = vover ? vover : arena + e->off;
    uint32_t tc0 = ncpu ? w3d_plat_us() : 0;
    int replay = pass == 2 && ncpu && !rec_overflow && rec_epoch == w3d_be_epoch();
    if (pass == 1 && ncpu) { nrec = 0; rec_overflow = 0; rec_on = 1; rec_epoch = w3d_be_epoch(); }
    for (uint32_t q = 0; q < nch;) {    /* per chunk: runs of GE chunks as one range, the others via the CPU */
        if (chcls[q] == CL_HW) {
            uint32_t r = q, npr = 0;
            while (r < nch && chcls[r] == CL_HW) { st_tris_hw += chs[r].ntris; npr += chs[r++].nprims; }
#ifndef HLE_PSP
            {   /* host check: the run's PRIMs consume exactly its chunks' vertices, all inside their boxes */
                static int diag_run = 8;
                uint32_t vend = r < nch ? chs[r].first_vert : e->nverts, sum = 0;
                for (uint32_t p = 0; p < npr; p++) sum += mv.sub[1 + chs[q].first_prim + p] & 0xFFFF;
                int bad = sum != vend - chs[q].first_vert;
                for (uint32_t c2 = q; c2 < r && !bad; c2++) {
                    uint32_t v0 = chs[c2].first_vert, v1 = c2 + 1 < nch ? chs[c2 + 1].first_vert : e->nverts;
                    for (uint32_t k = v0; k < v1 && !bad; k++) {
                        const int16_t *pp = e->fmt == W3D_FMT_UV16 ? &((const W3dV16 *)vb)[k].x : &((const W3dVF *)vb)[k].x;
                        for (int a = 0; a < 3; a++)
                            if (pp[a] < chs[c2].bmin[a] || pp[a] > chs[c2].bmax[a]) bad = 2;
                    }
                }
                if (bad && diag_run > 0) {
                    diag_run--;
                    LOG("world3d: CHUNK RUN CHECK FAILED (%s): mesh %08x chunks %u-%u of %u, prims %u (sum %u, vertices %u)"
                        " strip %d nverts %u\n", bad == 1 ? "vertex count" : "box", e->re, q, r - 1, nch, npr, sum,
                        vend - chs[q].first_vert, e->strip, e->nverts);
                }
            }
#endif
            set_world(1);
            w3d_be_range(&mv, vover, chs[q].first_vert, mv.sub + 1 + chs[q].first_prim, npr);
            q = r;
        } else {
            if (chcls[q] == CL_CPU && !replay) cpu_range(e, vb, M, &mv, chs[q].first_prim, chs[q].nprims, chs[q].first_vert);
            q++;
        }
    }
    cpu_flush();
    rec_on = 0;
    if (pass == 1 && ncpu && rec_epoch != w3d_be_epoch()) rec_overflow = 1;   /* a sync inside: recompute */
    if (replay) {
        for (int i = 0; i < nrec; i++) {
            if (rec[i].kind == 0) { set_world(1); w3d_be_indexed_again(&mv, vover, rec[i].copy, rec[i].n); }
            else { set_world(1); w3d_be_tris_again(rec[i].copy, rec[i].n); }
        }
        st_cpu_replays++;
    }
    if (ncpu) u_cpu += w3d_plat_us() - tc0;
}

/* the ALLEGREX counters around our own work (w3d_hwprof=N: 1 in N immediate atomics or deferred batches;
   needs poc6.cfg prof_hw=2 on the PSP - the profiler's kernel door, ~65 us a read, so only sampled). Words (layout
   in ps2rc_exc.h): 2 CPU cycles, 3 internal stall, 4 memory stall, 5 FPU stall, 6 VFPU stall, 7 sleep,
   9 / 10 uncached loads / stores, 11 / 12 cached loads / stores, 13 I-cache misses, 14 D-cache misses, 15 D-cache
   writebacks. The cost of the door itself (two reads back to back, 8 times) is subtracted. */
int w3d_opt_hwprof = 0;
/* three kinds of sample: 0 = a whole building atomic drawn immediately (w3d_defer=0, from the hook's entry to its
   end), 1 = the hook's part of a deferred atomic (what is taken), 2 = a deferred batch drawn at a flush */
static const char *hw_kind_name[4] = {"immediate atomic (whole)", "deferred atomic: the hook's part",
                                      "deferred batch drawn", "lit / skinned atomic (whole)"};
static uint32_t hw_a[20], hw_t0;
static int hw_live = -1, hw_ncal;
static uint64_t hw_ctr[4], hw_acc[4][20], hw_cal[20], hw_n_at[4], hw_n_mesh[4], hw_samples[4], hw_us[4];
static void hw_begin(int kind)
{
    hw_live = -1;
    if (w3d_opt_hwprof <= 0 || (hw_ctr[kind]++ % (uint64_t)w3d_opt_hwprof) != 0) return;
    if (!hw_ncal) {
        uint32_t x[20], y[20];
        for (int k = 0; k < 8; k++) {
            if (!hle_plat_hw_door(x) || !hle_plat_hw_door(y)) { hw_ncal = -1; return; }
            for (int i = 0; i < 20; i++) hw_cal[i] += (uint32_t)(y[i] - x[i]);
        }
        hw_ncal = 8;
    }
    if (hw_ncal < 0) return;
    if (hle_plat_hw_door(hw_a)) { hw_live = kind; hw_t0 = w3d_plat_us(); }
}
static void hw_end(int kind, uint32_t nat, uint32_t nmesh)
{
    if (hw_live != kind) return;
    hw_live = -1;
    uint32_t b[20], t1 = w3d_plat_us();
    if (!hle_plat_hw_door(b)) return;
    for (int i = 0; i < 20; i++) hw_acc[kind][i] += (uint32_t)(b[i] - hw_a[i]);
    hw_n_at[kind] += nat;
    hw_n_mesh[kind] += nmesh;
    hw_us[kind] += t1 - hw_t0;
    hw_samples[kind]++;
}
void w3d_hwp_begin(int kind) { hw_begin(kind); }
void w3d_hwp_end(int kind, uint32_t nat, uint32_t nmesh) { hw_end(kind, nat, nmesh); }
static void w3d_hw_report(void)
{
    if (!w3d_opt_hwprof) return;
    if (hw_ncal <= 0) {
        LOG("world3d hw (w3d_hwprof %d): no samples (needs prof_hw=2 and the kernel door)\n", w3d_opt_hwprof);
        return;
    }
    for (int k = 0; k < 4; k++) {
        if (!hw_samples[k]) continue;
        double n = (double)hw_samples[k], c[20];
        for (int i = 0; i < 20; i++) {
            double v = (double)hw_acc[k][i] - n * (double)hw_cal[i] / (double)hw_ncal;
            c[i] = v > 0 ? v : 0;
        }
        double stall = c[3] + c[4] + c[5] + c[6], inst = c[2] - stall - c[7];
        double na = hw_n_at[k] ? (double)hw_n_at[k] : 1.0, nm = hw_n_mesh[k] ? (double)hw_n_mesh[k] : 1.0;
        LOG("world3d hw (w3d_hwprof %d) %s: %llu samples, %.0f atomics, %.0f meshes; per atomic %.1f us (clock), %.0f "
            "cycles, %.0f instructions (CPI %.2f), memory stall %.0f%% of the cycles, I-misses %.1f, D-misses %.1f, "
            "D-writebacks %.1f, uncached loads %.1f / stores %.1f, cached loads %.0f / stores %.0f; per mesh %.1f us, "
            "%.0f cycles, I-misses %.1f, D-misses %.1f; door cost %.0f cycles\n", w3d_opt_hwprof, hw_kind_name[k],
            (unsigned long long)hw_samples[k], na, (double)hw_n_mesh[k], (double)hw_us[k] / na, c[2] / na, inst / na,
            inst > 0 ? (c[2] - c[7]) / inst : 0.0, c[2] > 0 ? 100.0 * c[4] / c[2] : 0.0, c[13] / na, c[14] / na,
            c[15] / na, c[9] / na, c[10] / na, c[11] / na, c[12] / na, (double)hw_us[k] / nm, c[2] / nm, c[13] / nm,
            c[14] / nm, (double)hw_cal[2] / (double)hw_ncal);
    }
}

/* deferred building atomics (w3d_defer=1). On the PSP every atomic entered the hook with cold caches (the
   game's own code ran in between): ~120 cold starts of the whole 3D path per frame (measured on a PSP: ~55 us per
   mesh, 4x PPSSPP; whole CPU memory-bound, I-cache misses ahead). With w3d_defer the hook only takes what can change
   before the frame ends (world x view, render states, day/night balance / ambient, each mesh's material colour and
   texture) and returns; the records are drawn in one loop at the next point that needs the GE in order
   (psp/hle_ge.c ge3d_leave = any 2D / sky / effect / post-FX draw, a camera begin, the flip; host: hle_raster.c's
   entry points; a lit / skinned atomic or an immediate-mode 3D primitive of ours). Same draws in the same order, just
   later and batched. */
typedef struct {
    uint32_t obj, pds, geo, mh, mc, nmesh, mesh0;
    int dn, gmod, strip, cam;
    int lit;                                /* w3d_defer=2: >= 0 = a lit record of hle_w3d_lit.c, drawn there */
    M43 M;
    float gm[12];
    ColP cp;
    uint8_t amb3[3];
    W3dState st;
} DRec;
typedef struct { uint32_t mat, mc4, tex; } DMesh;
#define DREC_MAX 384
#define DMESH_MAX 3072
#define DCAM_MAX 8
static DRec drec[DREC_MAX];
static DMesh dmesh[DMESH_MAX], dmesh_now[512];
static W3dCam dcam[DCAM_MAX];
static int ndrec, ndmesh, ndcam, dflushing, in_hook;
int w3d_opt_defer = 2;                      /* 0 off, 1 building atomics, 2 + the lit / skinned atomics */
int w3d_opt_gecmd = 1, w3d_opt_kick = 16, w3d_opt_texskip = 1;
/* hle_w3d_me.h: w3d_me=1 is the default (measured on a PSP in an A/B run: -3.2 ms per frame, hook -5.1 ms, 0
   failures); w3d_me_hook / w3d_me_lit / w3d_vframe stay off until verified on hardware */
int w3d_opt_me = 1, w3d_opt_me_runner = 0, w3d_opt_me_stall_ms = 200;
int w3d_opt_me_hook = 0, w3d_opt_me_lit = 0, w3d_opt_vframe = 0;
int w3d_defer_flushing;                     /* the backends' target check is skipped while the records are drawn */
static uint64_t st_def_atomics, st_def_batches, st_def_lost, st_def_full, u_flush, st_def_lit;
static void atomic_meshes(DRec *r, const DMesh *ms);
static void me_hook_record(int i);           /* w3d_me_hook (hle_w3d_me.h) */
static int me_prepping;
static uint64_t st_me_flush_nested, st_vframe_hits;

int w3d_defer_room(void) { return ndrec < DREC_MAX; }
void w3d_defer_push_lit(int idx)
{
    if (ndrec >= DREC_MAX) return;            /* (the caller checked w3d_defer_room) */
    drec[ndrec].lit = idx;
    ndrec++;
    me_hook_record(ndrec - 1);                /* its phase 1 for the ME now (w3d_me_hook) */
}

static int dcam_index(void)
{
    if (ndcam && !memcmp(&dcam[ndcam - 1], &cam, sizeof cam)) return ndcam - 1;
    if (ndcam >= DCAM_MAX) return -1;
    dcam[ndcam] = cam;
    return ndcam++;
}

static int me_ready(void);
static void me_flush(void);
void w3d_defer_flush(void)
{
    if (!ndrec || dflushing) return;
    if (me_prepping) { st_me_flush_nested++; return; }   /* not inside a record's phase 1 (it flushes later) */
    if (me_ready()) { me_flush(); return; }     /* the Media Engine builds the batch's GE words */
    dflushing = 1;
    w3d_defer_flushing = 1;
    uint32_t t0 = w3d_plat_us();
    W3dCam keep = cam;
    int curcam = -1;
    hw_begin(2);
    for (int i = 0; i < ndrec; i++) {
        DRec *r = &drec[i];
        if (r->lit >= 0) { w3d_lit_draw_deferred(r->lit); st_def_lit++; curcam = -1; continue; }
        if (r->cam != curcam) { cam = dcam[r->cam]; curcam = r->cam; }
        if (!w3d_be_begin(&cam)) { st_def_lost++; continue; }
        w3d_world_reset(r->gm);
        atomic_meshes(r, &dmesh[r->mesh0]);
        w3d_be_end();
    }
    w3d_be_kick();
    hw_end(2, (uint32_t)ndrec, (uint32_t)ndmesh);
    cam = keep;
    st_def_batches++;
    st_def_atomics += (uint64_t)ndrec;
    ndrec = ndmesh = ndcam = 0;
    w3d_lit_defer_reset();
    uint32_t dt = w3d_plat_us() - t0;
    u_flush += dt;
    if (!in_hook) u_hook += dt;              /* inside the hook (lit / immediate 3D) the hook's clock already runs */
    w3d_defer_flushing = 0;
    dflushing = 0;
}

static int w3d_atomic(R5900Ctx *c)
{
    uint32_t pipe = gwk(G_RENDER_CTX), obj = gwk(ARG(1));
    if (!pipe || !obj || gb(obj) != 1) {
        st_why[WHY_NOTATOMIC]++;
        static int dg = 8;
        static uint32_t dg_pds[8];
        uint32_t pds0 = pipe ? gwk(pipe + 0x30) : 0;
        if ((pds0 == 6 || pds0 == 7) && obj && w3d_im3d(c, obj)) return 1;   /* immediate-mode 3D (hle_w3d_imm.c) */
        int seen = 0;
        for (int q = 0; q < 8; q++) seen |= dg_pds[q] == (pds0 ^ (obj ? gb(obj) : 0xEE) << 24);
        if (dg > 0 && !seen) {             /* diagnostic: what else goes through the mesh node */
            dg_pds[--dg] = pds0 ^ (obj ? gb(obj) : 0xEE) << 24;
            LOG("world3d: mesh node call for object %08x type %u (not an atomic), pipeline %08x pds %08x, flip %llu\n",
                obj, obj ? gb(obj) : 0, pipe, pds0, (unsigned long long)hle_render_flip_count());
        }
#ifndef HLE_PSP
        if ((pds0 == 6 || pds0 == 7) && obj && ram_ok(obj - 0x0C, 0x3C)) {   /* the immediate-mode block (hle_w3d_imm.c) */
            static int di = 40;
            static uint32_t di_key[40];
            uint32_t fl = gw(obj), ltm = gw(obj + 4), nv = gh(obj - 0x0C), vp = gw(obj - 8), ip = gw(obj + 0x28),
                     ni = gw(obj + 0x2C), pt = gw(obj + 0x24), ras = hle_render_rs(RS_TEXTURERASTER);
            uint32_t key = fl ^ pt << 8 ^ (ltm != 0) << 16 ^ ras * 2654435761u;
            int sn = 0;
            for (int q = 0; q < 40; q++) sn |= di_key[q] == key;
            if (di > 0 && !sn) {
                di_key[--di] = key;
                float v0[3] = {0, 0, 0};
                if (vp && ram_ok(vp, 12)) for (int q = 0; q < 3; q++) { uint32_t w = gw(vp + 4u * (uint32_t)q); memcpy(&v0[q], &w, 4); }
                LOG("world3d: immediate 3D through the node: pds %u flags %02x ltm %08x verts %u at %08x (first %.1f %.1f %.1f) "
                    "indices %u at %08x prim %u raster %08x (%ux%u) blend %u/%u ztest %u zwrite %u, flip %llu\n", pds0,
                    fl, ltm, nv, vp, v0[0], v0[1], v0[2], ni, ip, pt, ras, ras ? gw(ras + 0x0C) : 0,
                    ras ? gw(ras + 0x10) : 0, hle_render_rs(RS_SRCBLEND), hle_render_rs(RS_DESTBLEND),
                    hle_render_rs(RS_ZTESTENABLE), hle_render_rs(RS_ZWRITEENABLE),
                    (unsigned long long)hle_render_flip_count());
            }
        }
#endif
        return 0;
    }
    uint32_t pds = gwk(pipe + 0x30);
    count_pds(pds);
    int dn = pds == PDS_BUILDING_DN;
    if (pds != PDS_BUILDING && !dn) {
        int r = w3d_lit_atomic(c, pds, obj);      /* vehicles / peds / 0x5001 (hle_w3d_lit.c); 0 = not handled */
        if (r > 0) { st_lit_meshes++; return 1; }
        st_why[WHY_PIPE]++;
        return 0;
    }
    hw_begin(w3d_opt_defer && !dflushing ? 1 : 0);
    int tsamp = (++samp_ctr & 3) == 0;
    uint32_t ts = tsamp ? w3d_plat_us() : 0;
    uint32_t geo = gwk(obj + 0x18);
    if (!geo || !ram_ok(geo, 0x60)) { st_why[WHY_GEO]++; return 0; }
    if (gw(geo + 0x18) != 1) { st_why[WHY_MORPH]++; return 0; }
    uint32_t gflags = gw(geo + 8), mh = gw(geo + 0x54);
    if (!(gflags & 0x01000000u) || !ram_ok(mh, 0x10)) {
        st_why[WHY_NATIVE]++;
        static int dg = 12;
        static uint32_t dg_geo[12];
        int seen = 0;
        for (int q = 0; q < 12; q++) seen |= dg_geo[q] == geo;
        if (dg > 0 && !seen) {             /* diagnostic: which building atomics are not PS2-native (runtime-instanced) */
            dg_geo[--dg] = geo;
            LOG("world3d: atomic %08x pds %08x geometry %08x not native (flags %08x, %u vertices, %u triangles, mesh "
                "header %08x, mesh caches geo %08x atomic %08x), flip %llu\n", obj, pds, geo, gflags, gw(geo + 0x14),
                gw(geo + 0x10), mh, gwk(geo + gw(GP_MESHCACHE_GEO)), gwk(obj + gw(0x0066CB6Cu)),
                (unsigned long long)hle_render_flip_count());
        }
        return 0;
    }
    uint32_t nmesh = gh(mh + 4), hflags = gw(mh), mc = gwk(geo + gw(GP_MESHCACHE_GEO));
    if (!nmesh || !mc || gw(mc) != nmesh || !ram_ok(mc, 4 + 4 * nmesh)) { st_why[WHY_CACHE]++; return 0; }
    uint32_t camera = gwk(DRV_ENGINE);
    if (!camera || !w3d_camera_setup(camera)) { st_why[WHY_CAMERA]++; return 0; }
    if (!w3d_be_target()) { st_why[WHY_TARGET]++; return 0; }
    int defer = w3d_opt_defer && !dflushing;
    if (defer && (ndrec >= DREC_MAX || ndmesh + (int)nmesh > DMESH_MAX || ndcam >= DCAM_MAX)) {
        st_def_full++;
        w3d_defer_flush();
    }
    if (!defer && nmesh > 512) { st_why[WHY_CACHE]++; return 0; }
    if (defer && (int)nmesh > DMESH_MAX) { st_why[WHY_CACHE]++; return 0; }
    st_atomics++;
    DRec rnow, *rd = defer ? &drec[ndrec] : &rnow;
    DMesh *ms = defer ? &dmesh[ndmesh] : dmesh_now;
    /* world = the frame's world matrix x view matrix */
    uint32_t frame = gwk(obj + 4), ltm = frame ? w3d_frame_ltm(c, frame) : 0;
    M43 L, V;
    if (!ram_ok(ltm, 0x40)) return 0;
    w3d_drv_mat(ltm, &L);
    w3d_drv_mat(camera + 0x20, &V);
    w3d_mat_mul(&L, &V, &rd->M);
    float *gm = rd->gm;                     /* the GE world matrix for s16 positions (x 32768 / 128) */
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) gm[3 * r + k] = rd->M.m[r][k] * 256.0f;
    for (int k = 0; k < 3; k++) gm[9 + k] = rd->M.m[3][k];
    /* the colour parameters of this atomic: balance (day/night), building ambient, material colour per mesh */
    ColP cp;
    memset(&cp, 0, sizeof cp);
    float bal = dn ? gfk(GP_DN_BALANCE) : 0.0f;
    cp.b8 = !(bal > 0.0f) ? 0u : bal >= 1.0f ? 256u : (uint32_t)(bal * 256.0f + 0.5f);
    if (w3d_opt_dnq > 0 && w3d_opt_dnq < 8) {     /* quantised balance (fewer re-bakes while it moves) */
        uint32_t q = (uint32_t)w3d_opt_dnq;
        cp.b8 = (cp.b8 + (1u << (q - 1))) >> q << q;
        if (cp.b8 > 256) cp.b8 = 256;
    }
    uint32_t amb = gwk(dn ? GP_AMB_DN : GP_AMB_BUILDING);
    uint8_t amb3[3] = {(uint8_t)amb, (uint8_t)(amb >> 8), (uint8_t)(amb >> 16)};
    cp.dn = dn;
    int gmod = (gflags & 0x40) != 0;
    /* render states as the game set them (drv_set_state) */
    w3d_base_state(&rd->st);
    if (lod_pass(camera, &rd->M) && w3d_opt_lodz && rd->st.ztest) { rd->st.ztest = 2; st_lodz_set++; }   /* the LOD pass */
    rd->obj = obj; rd->pds = pds; rd->geo = geo; rd->mh = mh; rd->mc = mc; rd->nmesh = nmesh;
    rd->dn = dn; rd->gmod = gmod; rd->strip = (hflags & 1) != 0;
    rd->cp = cp;
    memcpy(rd->amb3, amb3, 3);
    /* what may change before the records are drawn: each mesh's material colour (fading entities set the alpha only
       around their render call) and texture */
    uint32_t mtab = mh + 0x10 + gw(mh + 0x0C), dmc = gmod ? 0 : gwk(GP_DEFAULT_MATCOL);
    for (uint32_t i = 0; i < nmesh; i++) {
        uint32_t mat = gwk(mtab + 12u * i + 8);
        ms[i].mat = mat;
        ms[i].mc4 = !mat ? 0 : gmod ? gwk(mat + 4) : dmc;
        ms[i].tex = mat ? gwk(mat) : 0;
    }
    if (tsamp) { us_setup += w3d_plat_us() - ts; n_samp_atom++; }
    if (defer) {
        int ci = dcam_index();
        if (ci < 0) { st_why[WHY_CAMERA]++; return 0; }
        rd->cam = ci;
        rd->lit = -1;
        rd->mesh0 = (uint32_t)ndmesh;
        ndmesh += (int)nmesh;
        ndrec++;
        me_hook_record(ndrec - 1);            /* its phase 1 for the ME now (w3d_me_hook) */
        hw_end(1, 1, nmesh);
        return 1;
    }
    if (!w3d_be_begin(&cam)) { st_why[WHY_TARGET]++; return 0; }
    w3d_world_reset(rd->gm);
    atomic_meshes(rd, ms);
    w3d_be_end();
    hw_end(0, 1, nmesh);
    return 1;
}

/* one mesh of an atomic record (the body of the main core's record loop, also drawn alone for the Media Engine's
   MAINMESH parts); st = the record's running state, carried from mesh to mesh */
static void mesh_draw(DRec *rd, const DMesh *ms, uint32_t i, W3dState *stp)
{
    const uint32_t obj = rd->obj, pds = rd->pds, geo = rd->geo, mh = rd->mh, mc = rd->mc;
    const int dn = rd->dn, gmod = rd->gmod, strip = rd->strip;
    const M43 *Mp = &rd->M;
    const ColP *cpp = &rd->cp;
    const uint8_t *amb3 = rd->amb3;
#define st (*stp)
    (void)gmod;
    int msamp = (++samp_ctr & 7) == 0;
    uint32_t tm0 = msamp ? w3d_plat_us() : 0, tm1 = 0, tm2 = 0, tm3 = 0;
    uint64_t cpu0 = u_cpu, conv0 = u_conv + u_rebake;
    uint32_t mesh = mh + 0x10 + gw(mh + 0x0C) + 12u * i, mat = ms[i].mat, re = gwk(mc + 4 + 4 * i);
    st_meshes++;
    /* w3d_vframe: an entry validated this frame is trusted without the game's memory again */
    Ent *ev = w3d_opt_vframe && re ? w3d_ent_find(re) : NULL;
    int vhit = ev && ev->vframe == frame_no && ev->geo == geo && ev->kind == EK_BUILDING;
    uint32_t data = vhit ? ev->data : re ? gwk(re + 0x20) : 0;
    if (!mat || !data || !ram_ok(data, 64)) { st_m_fail++; return; }
    uint32_t mc4 = ms[i].mc4;
    uint8_t mat4[4] = {(uint8_t)mc4, (uint8_t)(mc4 >> 8), (uint8_t)(mc4 >> 16), (uint8_t)(mc4 >> 24)};
    int modulate = gmod || mc4 != 0xFFFFFFFFu;   /* the default colour (white) needs no multiply */
    /* what goes into the vertices (bake) and what the GE applies (light) */
    ColP bake = *cpp;
    st.light = 0;
    if (w3d_opt_light) {
        bake.amb[0] = bake.amb[1] = bake.amb[2] = 0;
        bake.modulate = 0;
        bake.mat[0] = bake.mat[1] = bake.mat[2] = bake.mat[3] = 255;
        uint32_t lm = modulate ? mc4 : 0xFFFFFFFFu, em = 0;
        for (int q = 0; q < 3; q++) {
            uint32_t m8 = modulate ? mat4[q] : 255u;
            em |= ((amb3[q] * m8 * 257u + 32768u) >> 16) << (8 * q);
        }
        if (lm != 0xFFFFFFFFu || em) { st.light = 1; st.lamb = lm; st.lemis = em; }
    } else {
        memcpy(bake.amb, amb3, 3);
        memcpy(bake.mat, mat4, 4);
        bake.modulate = modulate;
    }
    bake.plain = !bake.amb[0] && !bake.amb[1] && !bake.amb[2] && !bake.modulate;
    /* the converted mesh */
    st_lookups++;
    Ent *e = vhit ? ev : w3d_ent_find(re);
    uint32_t sig = 0, rsz = 0, nidx = 0;
    if (vhit) st_vframe_hits++;
    else {
        sig = chain_sig(data); rsz = gwk(re + 0x08); nidx = gwk(mesh + 4);
        if (e && (e->data != data || e->geo != geo || e->sig != sig || e->kind != EK_BUILDING || e->rsz != rsz ||
                  e->nidx != nidx)) {
            st_stale++;
            if (w3d_ent_busy(e)) { e->re = 0; w3d_ent_unlink(e); e = NULL; }   /* orphan (in use or held) */
            else { w3d_ent_free(e); e = NULL; }
        }
    }
    uint32_t ck = col_key(&bake);
    const uint8_t *vover = NULL;
    if (!e) {
        e = w3d_ent_new(re);
        if (!e) { st_m_fail++; st_noroom++; return; }
        e->data = data; e->geo = geo; e->sig = sig; e->dn = (uint8_t)dn; e->kind = EK_BUILDING;
        e->rsz = rsz; e->nidx = nidx;
        uint32_t cap = gwk(mesh + 4);
        if (!convert(e, cap, strip, &bake)) { w3d_ent_free(e); st_m_fail++; return; }
        e->vframe = frame_no;
    } else {
        if (!vhit) e->vframe = frame_no;
        st_hits++;
        if (e->ckey != ck) {
            static int diag_rb = 12;
            if (diag_rb > 0) {
                diag_rb--;
                LOG("world3d: colour change mesh %08x atomic %08x pds %08x: balance %u/256 ambient %02x%02x%02x material "
                    "%02x%02x%02x%02x modulate %d light %d flip %llu\n", re, obj, pds, bake.b8, bake.amb[0],
                    bake.amb[1], bake.amb[2], bake.mat[0], bake.mat[1], bake.mat[2], bake.mat[3], bake.modulate,
                    st.light, (unsigned long long)hle_render_flip_count());
            }
            if (w3d_opt_rebake_us > 0 && u_rebake_frame >= (uint32_t)w3d_opt_rebake_us) st_rebake_defer++;
            else {
                uint32_t tr = w3d_plat_us();
                int okb = 1;
                if (!w3d_ent_busy(e)) {   /* the GE is not reading it (nor a ME batch): re-bake in place */
                    okb = rebake(e, &bake, arena + e->off);
                    w3d_plat_flush(arena + e->off, e->vbytes);
                    e->ckey = ck;
                    st_rebake++;
                } else {                      /* it is: a copy in this frame's GE memory */
                    uint8_t *cpy = w3d_be_alloc(e->vbytes);
                    if (cpy) {
                        okb = rebake(e, &bake, cpy);
                        w3d_plat_flush(cpy, e->vbytes);
                        vover = cpy;
                        st_dyncopy++;
                    }
                }
                if (!okb) {                   /* stale entry: drop it (orphan if the GE may read it), skip the
                                                 mesh this frame; the next frame converts it again */
                    if (w3d_ent_busy(e)) { e->re = 0; w3d_ent_unlink(e); }
                    else w3d_ent_free(e);
                    st_m_fail++;
                    u_rebake += w3d_plat_us() - tr;
                    return;
                }
                uint32_t dt = w3d_plat_us() - tr;
                u_rebake += dt;
                u_rebake_frame += dt;
            }
        }
    }
    e->last_use = frame_no;
    if (msamp) tm1 = w3d_plat_us();
    /* where does it go: the mesh box, then (big or CPU meshes) every chunk */
    int cl = w3d_classify_box(e->bmin, e->bmax, Mp), inside = cl & CL_INSIDE;
    cl &= 3;
    if (cl == CL_CULL) { st_m_cull++; return; }
    const Chunk *chs = (const Chunk *)(arena + e->off + e->ch_off);
    uint32_t nch = e->nchunks, nhw = 0, ncpu = 0;
    int whole = cl == CL_HW && (nch <= 1 || inside);
    if (!whole) {
        if (nch > MAXCHUNK) { st_m_fail++; return; }
        /* chunks in groups of w3d_chunk_group: the group's box first (the union of its chunks'), its
           chunks one by one only when the group straddles a plane (small chunks for the CPU path, few box tests) */
        for (uint32_t g = 0; g < nch; g += chunk_group) {
            uint32_t ge = g + chunk_group < nch ? g + chunk_group : nch;
            int gc = CL_CPU;
            if (ge - g > 1) {
                Chunk u = chs[g];
                for (uint32_t q = g + 1; q < ge; q++)
                    for (int a = 0; a < 3; a++) {
                        if (chs[q].bmin[a] < u.bmin[a]) u.bmin[a] = chs[q].bmin[a];
                        if (chs[q].bmax[a] > u.bmax[a]) u.bmax[a] = chs[q].bmax[a];
                    }
                gc = w3d_classify_chunk(&u, Mp, 1.0f / 128.0f);
                st_ch_groups++;
            }
            for (uint32_t q = g; q < ge; q++) {
                int cc = gc != CL_CPU ? gc : w3d_classify_chunk(&chs[q], Mp, 1.0f / 128.0f);
                if (cc == CL_CPU && !w3d_opt_clip) cc = CL_CULL;
                chcls[q] = (uint8_t)cc;
                if (cc == CL_HW) nhw++; else if (cc == CL_CPU) ncpu++;
            }
        }
        st_ch_hw += nhw; st_ch_cpu += ncpu; st_ch_cull += nch - nhw - ncpu;
        if (!nhw && !ncpu) { st_m_cull++; return; }
        whole = nhw == nch;
    }
    /* texture and states */
    uint32_t ras;
    int acl, wrap;
    if (msamp) tm2 = w3d_plat_us();
    if (!w3d_bind_texture(ms[i].tex, &ras, &acl, &wrap)) return;
    if (msamp) tm3 = w3d_plat_us();
    st.textured = ras != 0;
    int valpha = gmod && mat4[3] < 255;          /* the geometry's modulate flag */
    st.uscale = e->fmt == W3D_FMT_UV16 ? 8.0f : 1.0f;        /* block UVs; the CPU path writes the same units */
    /* a whole-tile texture offset is invisible under REPEAT and costs UV-plane precision on the GE: only CLAMP
       axes get the base back (w3d_tilebase=1: always) */
    st.ubase = e->fmt == W3D_FMT_UV16 && (!(wrap & 1) || w3d_opt_tilebase) ? e->ubase : 0.0f;
    st.vbase = e->fmt == W3D_FMT_UV16 && (!(wrap & 2) || w3d_opt_tilebase) ? e->vbase : 0.0f;
    /* PS2: alpha blending when the raster has alpha or the material alpha < 255; alpha test GEQUAL 0x40 with
       AFAIL = frame buffer only (gp 0x6681B0 TEST_1 shadow 0x5140B): emulated as two passes for textures with
       partial alpha (the second one draws the low-alpha texels without Z) */
    int passes = w3d_alpha_passes(&st, acl, valpha);
    int zw = st.zwrite;
    for (int pass = 0; pass < passes; pass++) {
        if (pass == 1) { st.atest = 3; st.zwrite = 0; st_m_twopass++; }
        w3d_be_state(&st);
        draw_classified(e, Mp, vover, whole, nch, ncpu, passes == 1 ? 0 : pass + 1);
    }
    st.zwrite = zw;
    e->pin = w3d_be_epoch();            /* after the draws: a mid-frame list sync may have advanced it */
    if (whole) st_m_hw++; else st_m_cpu++;
    if (msamp) {                        /* (meshes culled or failing above are not sampled) */
        uint32_t tm4 = w3d_plat_us();
        uint64_t conv = u_conv + u_rebake - conv0, cpu = u_cpu - cpu0;
        us_lookup += (uint64_t)(tm1 - tm0) > conv ? (tm1 - tm0) - conv : 0;
        us_class += tm2 - tm1;
        us_tex += tm3 - tm2;
        us_draw += (uint64_t)(tm4 - tm3) > cpu ? (tm4 - tm3) - cpu : 0;
        n_samp_mesh++;
    }
#undef st
}

/* the meshes of one atomic record (immediately or from w3d_defer_flush) */
static void atomic_meshes(DRec *rd, const DMesh *ms)
{
    W3dState st = rd->st;
    for (uint32_t i = 0; i < rd->nmesh; i++) mesh_draw(rd, ms, i, &st);
}

/* ------------------------------------------------------------------ the Media Engine (hle_w3d_me.h)
   w3d_me=1 (with w3d_defer >= 1): a deferred batch is described to the ME (or the test thread) - per mesh the main core
   does what needs the game's memory / the vertex cache / the GE back-end's texture cache (me_prep_mesh: mesh_draw's
   lookup / conversion / re-bake, the mesh-box cull, the texture lookup without binding; lit / skinned: hle_w3d_lit.c
   w3d_lit_me_push), the ME the chunk / bone-group classification, the state and the GE words; then the main core walks
   the ME's parts in order (CALL the segments, draw the MAINMESH / MREC parts itself with mesh_draw) and kicks
   the GE once. w3d_me_hook=1: the records are written when the hook takes them, so the ME builds while the
   game runs and the flush only adds END, waits for what is left and CALLs. A batch without a live poller, a failed one
   (stale line, arena / part list full) or one the ME does not finish in w3d_me_stall_ms is drawn by the main core's
   record loop (three stalls in a row give the ME up for the session). */
uint32_t w3d_me_hold;
static uint32_t me_hold_ctr, me_arena_end, me_call_epoch = 0xFFFFFFFFu;
static int me_state;                            /* 0 not tried, 1 on, -1 unavailable */
#define ME_MAXJOB 2048
static Ent *me_ent[ME_MAXJOB];                  /* job mesh index -> entry (pins, host replay) */
static uint32_t me_nk;                          /* job meshes of the open batch */
static int me_open;                             /* the open batch is the ME's (its records go into the stream) */
static int me_nsent;                            /* records [0, me_nsent) are in the stream */
static int me_full;                             /* the stream had no room: the later records are the main core's */
static uint32_t me_camsent;                     /* cameras in the stream: bit i = dcam i, bit 8 + i = lit camera i */
static uint64_t st_me_batches, st_me_main_batches, st_me_meshes, st_me_mainmesh, st_me_mrec, st_me_segs, st_me_words,
    st_me_rest, st_me_fail[8], st_me_texskip, st_me_lrec;
static uint64_t u_me_prep, u_me_wait, u_me_replay, u_me_hookprep;
static uint64_t u_bb_litprep, st_bb_litprep;   /* W3D_BB_LITPREP */

int w3d_ent_busy(const Ent *e) { return e->pin == w3d_be_epoch() || (w3d_me_hold && e->hold == w3d_me_hold); }

static int me_ready(void)
{
    if (!w3d_opt_me || !w3d_opt_defer) return 0;
    if (me_state == 0) {
        /* our own ME loop takes the ME's reset vector: not before the main loop runs (the intro movies' sceMpeg decodes
           on the ME) */
        if ((w3d_opt_me_runner == 2 || w3d_opt_me_runner == 3) && !hle_mainloop_iters) return 0;
        char why[128], how[96];
        me_state = -1;
        if (!w3d_be_me_selftest(why, (int)sizeof why)) {
            LOG("world3d ME: w3d_me=%d but the encoder self-test FAILED (%s) - the main core draws everything\n",
                w3d_opt_me, why);
            return 0;
        }
        if (!w3dme_init(w3d_opt_me_runner, how, (int)sizeof how)) {
            LOG("world3d ME: w3d_me=%d not available (%s) - the main core draws everything\n", w3d_opt_me, how);
            return 0;
        }
        int nt = 0;
        const uint32_t *tm = w3d_be_global_template(&nt);
        if (tm) w3dme_set_template(tm, (uint32_t)nt);
        me_state = 1;
        LOG("world3d ME: on (w3d_me=%d, runner %d: %s, w3d_me_hook=%d w3d_me_lit=%d w3d_vframe=%d); encoder self-test %s; "
            "stall limit %d ms\n", w3d_opt_me, w3d_opt_me_runner, how, w3d_opt_me_hook, w3d_opt_me_lit, w3d_opt_vframe,
            why, w3d_opt_me_stall_ms);
    }
    return me_state > 0 && !w3dme_dead();
}

static inline uint32_t fword(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* a texture's descriptor for the ME (w3d_bind_texture without binding: looked up and held); the same raster
   / filter / addressing as the previous mesh of this batch: its descriptor again (w3d_texskip's rule - held, so it
   cannot have moved) */
/* W3D_BB_TEXC: at the flush (no game code runs until its end, the looked-up texture slots are held) a
   raster's descriptor cannot change: 16 slots by raster, valid for this flush only (bb_tex_serial) */
static uint32_t bb_tex_serial;
static struct { uint32_t ras, fa, serial, hold; W3mTex td; } bb_tex[16];
static uint64_t st_bb_texc_hits, st_bb_texc_miss;
int w3d_me_tex(uint32_t tex, W3mTex *td)
{
    static W3mTex td_last;
    static uint32_t td_ras, td_fa, td_hold;
    uint32_t ras = tex ? gwk(tex) : 0, fa = tex ? gwk(tex + 0x50) : 0;
    uint32_t au = fa >> 8 & 0xF, av = fa >> 12 & 0xF;
    if (w3d_opt_texskip && td_hold == w3d_me_hold && ras == td_ras && fa == td_fa) {
        *td = td_last;
        st_me_texskip++;
        return 1;
    }
    int bbc = (w3d_opt_bb & W3D_BB_TEXC) && dflushing && ras;
    unsigned bs = (ras >> 4) * 2654435761u >> 28;
    if (bbc && bb_tex[bs].serial == bb_tex_serial && bb_tex[bs].hold == w3d_me_hold && bb_tex[bs].ras == ras &&
        bb_tex[bs].fa == fa) {
        *td = bb_tex[bs].td;
        td_last = *td;
        td_ras = ras;
        td_fa = fa;
        td_hold = w3d_me_hold;
        st_bb_texc_hits++;
        return 1;
    }
    if (!w3d_be_tex_desc(ras, (int)(fa & 0xFF), (int)au, (int)av, td)) { st_m_notex++; td_hold = 0; return 0; }
    td->wrap = (au != 3 && au != 4 ? 1u : 0u) | (av != 3 && av != 4 ? 2u : 0u);
    td_last = *td;
    td_ras = ras;
    td_fa = fa;
    td_hold = w3d_me_hold;
    if (bbc) {
        bb_tex[bs].ras = ras; bb_tex[bs].fa = fa; bb_tex[bs].serial = bb_tex_serial; bb_tex[bs].hold = w3d_me_hold;
        bb_tex[bs].td = *td;
        st_bb_texc_miss++;
    }
    return 1;
}

int w3d_me_ent_add(Ent *e)
{
    if (me_nk >= ME_MAXJOB) return -1;
    e->hold = w3d_me_hold;
    me_ent[me_nk] = e;
    return (int)me_nk++;
}

/* phase 1 of one building mesh on the main core: 0 = nothing to draw, 1 = the ME draws it (w[1..17] filled), 2 = the
   main core draws it at the replay (a re-bake copy in GE memory) */
static int me_prep_mesh(DRec *rd, const DMesh *ms, uint32_t i, uint32_t *w, Ent **pe)
{
    const uint32_t obj = rd->obj, pds = rd->pds, geo = rd->geo, mh = rd->mh, mc = rd->mc;
    const int dn = rd->dn, gmod = rd->gmod, strip = rd->strip;
    const uint8_t *amb3 = rd->amb3;
    uint32_t mesh = mh + 0x10 + gw(mh + 0x0C) + 12u * i, mat = ms[i].mat, re = gwk(mc + 4 + 4 * i);
    st_meshes++;
    Ent *e = re ? w3d_ent_find(re) : NULL;
    int vhit = w3d_opt_vframe && e && e->vframe == frame_no && e->geo == geo && e->kind == EK_BUILDING;
    uint32_t data = vhit ? e->data : re ? gwk(re + 0x20) : 0;
    if (!mat || !data || !ram_ok(data, 64)) { st_m_fail++; return 0; }
    uint32_t mc4 = ms[i].mc4;
    uint8_t mat4[4] = {(uint8_t)mc4, (uint8_t)(mc4 >> 8), (uint8_t)(mc4 >> 16), (uint8_t)(mc4 >> 24)};
    int modulate = gmod || mc4 != 0xFFFFFFFFu;
    ColP bake = rd->cp;
    if (w3d_opt_light) {
        bake.amb[0] = bake.amb[1] = bake.amb[2] = 0;
        bake.modulate = 0;
        bake.mat[0] = bake.mat[1] = bake.mat[2] = bake.mat[3] = 255;
    } else {
        memcpy(bake.amb, amb3, 3);
        memcpy(bake.mat, mat4, 4);
        bake.modulate = modulate;
    }
    bake.plain = !bake.amb[0] && !bake.amb[1] && !bake.amb[2] && !bake.modulate;
    st_lookups++;
    int fresh = 0;
    if (vhit) st_vframe_hits++;
    else {
        uint32_t sig = chain_sig(data), rsz = gwk(re + 0x08), nidx = gwk(mesh + 4);
        if (e && (e->data != data || e->geo != geo || e->sig != sig || e->kind != EK_BUILDING || e->rsz != rsz ||
                  e->nidx != nidx)) {
            st_stale++;
            if (w3d_ent_busy(e)) { e->re = 0; w3d_ent_unlink(e); e = NULL; }
            else { w3d_ent_free(e); e = NULL; }
        }
        if (!e) {
            e = w3d_ent_new(re);
            if (!e) { st_m_fail++; st_noroom++; return 0; }
            e->data = data; e->geo = geo; e->sig = sig; e->dn = (uint8_t)dn; e->kind = EK_BUILDING;
            e->rsz = rsz; e->nidx = nidx;
            if (!convert(e, gwk(mesh + 4), strip, &bake)) { w3d_ent_free(e); st_m_fail++; return 0; }
            fresh = 1;
        }
        e->vframe = frame_no;
    }
    uint32_t ck = col_key(&bake);
    if (!fresh) st_hits++;
    if (!fresh && e->ckey != ck) {
        static int diag_rb = 12;
        if (diag_rb > 0) {
            diag_rb--;
            LOG("world3d: colour change mesh %08x atomic %08x pds %08x: balance %u/256 ambient %02x%02x%02x material "
                "%02x%02x%02x%02x modulate %d (w3d_me) flip %llu\n", re, obj, pds, bake.b8, bake.amb[0],
                bake.amb[1], bake.amb[2], bake.mat[0], bake.mat[1], bake.mat[2], bake.mat[3], bake.modulate,
                (unsigned long long)hle_render_flip_count());
        }
        if (w3d_opt_rebake_us > 0 && u_rebake_frame >= (uint32_t)w3d_opt_rebake_us) st_rebake_defer++;
        else if (w3d_ent_busy(e)) return 2;          /* a copy in GE memory: mesh_draw at the replay */
        else {
            uint32_t tr = w3d_plat_us();
            int okb = rebake(e, &bake, arena + e->off);
            w3d_plat_flush(arena + e->off, e->vbytes);
            e->ckey = ck;
            st_rebake++;
            if (!okb) {
                if (w3d_ent_busy(e)) { e->re = 0; w3d_ent_unlink(e); }
                else w3d_ent_free(e);
                st_m_fail++;
                u_rebake += w3d_plat_us() - tr;
                return 0;
            }
            uint32_t dt = w3d_plat_us() - tr;
            u_rebake += dt;
            u_rebake_frame += dt;
        }
    }
    e->last_use = frame_no;
    e->hold = w3d_me_hold;
    int cl = w3d_classify_box(e->bmin, e->bmax, &rd->M);
    if ((cl & 3) == CL_CULL) { st_m_cull++; return 0; }
    W3mTex td;
    if (!w3d_me_tex(ms[i].tex, &td)) return 0;
#ifdef HLE_PSP
    w[1] = (uint32_t)(uintptr_t)(arena + e->off);
    w[2] = (uint32_t)(uintptr_t)(arena + e->off + e->vbytes);
    w[3] = (uint32_t)(uintptr_t)(arena + e->off + e->ch_off);
#else
    w[1] = e->off;                                  /* host: offsets (64-bit pointers); the ME adds the base */
    w[2] = e->off + e->vbytes;
    w[3] = e->off + e->ch_off;
#endif
    w[4] = (e->nchunks & 0xFFFFu) | (uint32_t)e->fmt << 16 | (uint32_t)cl << 24;
    w[5] = e->nprims;
    w[6] = fword(e->ubase);
    w[7] = fword(e->vbase);
    w[8] = mc4;
    memcpy(&w[9], &td, sizeof td);
    *pe = e;
    return 1;
}

#ifndef HLE_PSP
static void me_view(const Ent *e, W3dMeshView *mv)
{
    memset(mv, 0, sizeof *mv);
    mv->verts = arena + e->off;
    mv->sub = (const uint32_t *)(arena + e->off + e->vbytes);
    mv->nverts = e->nverts; mv->nprims = e->nprims; mv->nsub = e->nprims + 2; mv->ntris = e->ntris;
    mv->fmt = e->fmt; mv->prim = e->strip ? W3D_PRIM_STRIP : W3D_PRIM_LIST;
    mv->stride = e->fmt == W3D_FMT_UV16 ? 16 : 20;
    mv->ubase = e->ubase; mv->vbase = e->vbase;
}

/* the host's "GE": a segment of operations replayed through the host back-end */
static void me_host_ops(const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        uint32_t op = w[i] >> 24, len = w[i] & 0xFFFFFFu;
        const uint32_t *a = w + i + 1;
        i += 1u + len;
        if (op == W3M_OP_RET) return;
        if (op == W3M_OP_BEGIN) {
            if (a[1] < 8) cam = dcam[a[1]];
            else cam = *w3d_lit_cam((int)a[1] - 8);
            w3d_be_begin(&cam);
        } else if (op == W3M_OP_WORLD) { float gm[12]; memcpy(gm, a, sizeof gm); w3d_be_world(gm); }
        else if (op == W3M_OP_TEX) { int acl; w3d_be_texture(a[0], (int)a[1], (int)a[2], (int)a[3], &acl); }
        else if (op == W3M_OP_STATE) { W3dState st; memcpy(&st, a, sizeof st); w3d_be_state(&st); }
        else if (op == W3M_OP_MESH) {
            W3dMeshView mv;
            me_view(me_ent[a[0]], &mv);
            w3d_be_mesh(&mv, NULL);
            st_tris_hw += me_ent[a[0]]->ntris;
        } else if (op == W3M_OP_RANGE) {
            W3dMeshView mv;
            me_view(me_ent[a[0]], &mv);
            w3d_be_range(&mv, NULL, a[1], mv.sub + 1 + (a[2] & 0xFFFFu), a[2] >> 16);
        } else if (op == W3M_OP_PROJ) {
            if (len >= 16) { float pr[16]; memcpy(pr, a, sizeof pr); w3d_be_proj(pr); }
            else w3d_be_proj(NULL);
        } else if (op == W3M_OP_LIGHTS) { W3dLights L; memcpy(&L, a, sizeof L); w3d_be_lights(&L); }
        else if (op == W3M_OP_BONES) {
            float bm[8][12];
            int nb = (int)a[0] > 8 ? 8 : (int)a[0];
            memcpy(bm, a + 1, sizeof bm[0] * (size_t)nb);
            w3d_be_bones(nb, (const float (*)[12])bm);
        } else if (op == W3M_OP_LMESH) {
            W3dMeshView mv;
            w3d_lit_group_view(me_ent[a[0]], (int)a[1], &mv);
            w3d_be_mesh(&mv, NULL);
        }
    }
}
#endif

/* the main core's record loop for records [from, ndrec) (also the whole batch when the ME is not used for it) */
static void me_main_records(int from)
{
    int curcam = -1;
    for (int i = from; i < ndrec; i++) {
        DRec *r = &drec[i];
        if (r->lit >= 0) { w3d_lit_draw_deferred(r->lit); st_def_lit++; curcam = -1; continue; }
        if (r->cam != curcam) { cam = dcam[r->cam]; curcam = r->cam; }
        if (!w3d_be_begin(&cam)) { st_def_lost++; continue; }
        w3d_world_reset(r->gm);
        atomic_meshes(r, &dmesh[r->mesh0]);
        w3d_be_end();
    }
}

/* a new batch for the ME (the first record of a batch at hook time, or the flush with w3d_me_hook=0) */
static void me_open_batch(void)
{
    me_open = 0;
    me_nsent = 0;
    me_full = 0;
    me_nk = 0;
    me_camsent = 0;
    if (!me_ready()) return;
    uint32_t ep = w3d_be_epoch();
    uint32_t opts = (w3d_opt_gnear ? 1u : 0u) | (w3d_opt_clip ? 2u : 0u) | (w3d_opt_light ? 4u : 0u) |
                    (w3d_opt_tilebase ? 8u : 0u) | (w3d_opt_afail ? 16u : 0u) | (w3d_opt_front ? 32u : 0u) |
                    chunk_group << 8;
#ifdef HLE_PSP
    uint32_t vlo = (uint32_t)(uintptr_t)arena, vhi = vlo + arena_bytes;
    uint32_t astart = ep == me_call_epoch ? me_arena_end : 0u;   /* from 0 once the GE ran everything that called in */
#else
    uint32_t vlo = 0, vhi = arena_bytes;                     /* host: offsets */
    uint32_t astart = 0;                                     /* host: the replay has consumed every segment */
    (void)ep;
#endif
    if (!w3dme_batch_begin(astart, opts, vlo, vhi)) return;
    if (++me_hold_ctr == 0) me_hold_ctr = 1;
    w3d_me_hold = me_hold_ctr;
    w3d_be_hold(1);
    me_open = 1;
}

/* phase 1 of record i (the next one in the stream) on the main core: its items, published at once */
static void me_push_record(int i)
{
    if (!me_open || me_full || i != me_nsent) return;
    DRec *r = &drec[i];
    uint32_t w[W3M_ITEM_MAX];
    int lit = r->lit >= 0;
    uint32_t nmesh = lit ? w3d_lit_nmesh(r->lit) : r->nmesh;
    if (!w3dme_room(W3M_LCAM_WORDS + W3M_LREC_WORDS + 8u + nmesh * (W3M_LMESH_WORDS + 1u)) ||
        me_nk + nmesh > ME_MAXJOB) { me_full = 1; st_me_rest++; return; }
    /* W3D_BB_LEAN: at the flush the whole phase 1 is timed once by me_flush (no clock per record) */
    int lean_flush = (w3d_opt_bb & W3D_BB_LEAN) && dflushing;
    uint32_t t0 = lean_flush ? 0 : w3d_plat_us();
    me_prepping = 1;
    if (lit) {
        if (w3d_opt_me_lit) { w3d_lit_me_push(r->lit, (uint32_t)i, &me_camsent); st_me_lrec++; }
        else { w[0] = (uint32_t)i; w3dme_put(W3M_I_MREC, w, 1); st_me_mrec++; }
    } else {
        if (!(me_camsent >> r->cam & 1u)) {
            const W3dCam *k = &dcam[r->cam];
            w[0] = (uint32_t)r->cam;
            for (int q = 0; q < 16; q++) w[1 + q] = fword(k->proj[q]);
            w[17] = fword(k->near_z); w[18] = fword(k->far_z); w[19] = fword(k->fog_z);
            w3dme_put(W3M_I_CAM, w, W3M_CAM_WORDS);
            me_camsent |= 1u << r->cam;
        }
        w[0] = (uint32_t)i;
        w[1] = (uint32_t)r->cam;
        w[2] = (uint32_t)r->amb3[0] | (uint32_t)r->amb3[1] << 8 | (uint32_t)r->amb3[2] << 16 | (uint32_t)(r->gmod != 0) << 24;
        w[3] = (uint32_t)(r->st.ztest != 0) | (uint32_t)(r->st.zwrite & 1) << 1 | (uint32_t)(r->st.cull & 3) << 2 |
               (uint32_t)(r->st.fog & 1) << 4 | (uint32_t)(r->st.ztest == 2) << 5 | (uint32_t)(r->st.src_blend & 0xF) << 8 | (uint32_t)(r->st.dst_blend & 0xF) << 12;
        w[4] = r->st.fog_color;
        for (int q = 0; q < 12; q++) { w[5 + q] = fword(r->M.m[q / 3][q % 3]); w[17 + q] = fword(r->gm[q]); }
        w3dme_put(W3M_I_REC, w, W3M_REC_WORDS);
        const DMesh *ms = &dmesh[r->mesh0];
        for (uint32_t m = 0; m < r->nmesh; m++) {
            Ent *e = NULL;
            int k = me_prep_mesh(r, ms, m, w, &e);
            if (k == 1) {
                int kk = w3d_me_ent_add(e);
                w[0] = (uint32_t)kk | m << 16;
                w3dme_put(W3M_I_MESH, w, W3M_MESH_WORDS);
                st_me_meshes++;
            } else if (k == 2) {
                w[0] = (uint32_t)i;
                w[1] = m;
                w3dme_put(W3M_I_MAINMESH, w, 2);
                st_me_mainmesh++;
            }
        }
    }
    /* W3D_BB_LEAN: at the flush the stream is published every 32 lines (one cache write-back system call
       per ~4 records instead of per record; me_flush publishes the rest with END) */
    if (!lean_flush || w3dme_unpublished() >= 32u) w3dme_publish();
    me_prepping = 0;
    me_nsent = i + 1;
    if (lean_flush) return;
    uint32_t dt = w3d_plat_us() - t0;
    if (dflushing) u_me_prep += dt; else u_me_hookprep += dt;
}

/* w3d_me_hook=1: a record the hook just took (building: w3d_atomic, lit: w3d_defer_push_lit) */
static void me_hook_record(int i)
{
    if (!w3d_opt_me || !w3d_opt_me_hook || dflushing) return;
    if (i == 0) me_open_batch();
    me_push_record(i);
}

static void me_flush(void)
{
    dflushing = 1;
    w3d_defer_flushing = 1;
    uint32_t t0 = w3d_plat_us();
    W3dCam keep = cam;
    hw_begin(2);
    int ok = 0;
    bb_tex_serial++;                              /* W3D_BB_TEXC: a new flush, an empty descriptor cache */
    if (!bb_tex_serial) bb_tex_serial = 1;
    if (!w3d_opt_me_hook) {                       /* w3d_me_hook=0: the whole batch now */
        me_open_batch();
        int lean = (w3d_opt_bb & W3D_BB_LEAN) != 0;
        uint32_t tl = lean ? w3d_plat_us() : 0;
        for (int i = 0; i < ndrec && me_open && !me_full; i++) me_push_record(i);
        if (lean) u_me_prep += w3d_plat_us() - tl;
    }
    if (!me_open) {
        st_me_main_batches++;
        me_main_records(0);
        goto done;
    }
    w3dme_put(W3M_I_END, NULL, 0);
    w3dme_publish();
    uint32_t t1 = w3d_plat_us(), fail = 0, nparts = 0, aend = 0;
    /* W3D_BB_LITPREP: while the ME builds, the main core does the lit / skinned records' lookups and bone-group
       classification (pure CPU, no GE words; the replay below uses them) until the ME is done */
    if ((w3d_opt_bb & W3D_BB_LITPREP) && !w3d_opt_me_lit) {
        for (int i = 0; i < ndrec && !w3dme_done(); i++)
            if (drec[i].lit >= 0) { w3d_lit_prep(drec[i].lit); st_bb_litprep++; }
        uint32_t tp = w3d_plat_us();
        u_bb_litprep += tp - t1;
        t1 = tp;
    }
    ok = w3dme_wait((uint32_t)w3d_opt_me_stall_ms * 1000u, &fail, &nparts, &aend);
    uint32_t t2 = w3d_plat_us();
    u_me_wait += t2 - t1;
    if (!ok) {
        st_me_fail[fail & 7]++;
        st_me_main_batches++;
        w3d_be_forget();
        me_main_records(0);
    } else {
        st_me_batches++;
        const W3mPart *parts = w3dme_parts();
        const uint32_t *ar = w3dme_arena();
        int dirty = 0;
        for (uint32_t pi = 0; pi < nparts; pi++) {
            uint32_t type = parts[pi].w0 >> 24, rec = parts[pi].w0 & 0xFFFFFFu, w1 = parts[pi].w1, n = parts[pi].n;
            if (type == W3M_P_SEG) {
#ifdef HLE_PSP
                if (!w3d_be_seg(ar + w1 / 4u)) st_def_lost++;
                (void)n;
#else
                me_host_ops(ar + w1 / 4u, n);
#endif
                st_me_segs++;
                st_me_words += n;
                dirty = 1;
                continue;
            }
            if ((int)rec >= ndrec) continue;
            DRec *r = &drec[rec];
            if (dirty) { w3d_be_forget(); world_cur = 0; dirty = 0; }   /* the segments set the GE's world
                                                                  matrix behind set_world's cache */
            if (type == W3M_P_MREC) {
                if (r->lit >= 0) { w3d_lit_draw_deferred(r->lit); st_def_lit++; }
            } else if (type == W3M_P_MAINMESH && r->lit >= 0) {
                w3d_lit_draw_one(r->lit, w1);
            } else if (type == W3M_P_MAINMESH && w1 < r->nmesh) {
                cam = dcam[r->cam];
                if (!w3d_be_begin(&cam)) { st_def_lost++; continue; }
                w3d_world_reset(r->gm);
                W3dState st = r->st;
                mesh_draw(r, &dmesh[r->mesh0], w1, &st);
                w3d_be_end();
            }
        }
        if (dirty) { w3d_be_forget(); world_cur = 0; }   /* a SEG-only batch left the segments' world matrix
                                   on the GE / host back-end; water / im3d's w3d_world_set(2) must send identity again */
        if (me_nsent < ndrec) me_main_records(me_nsent);   /* the stream was full: the rest on the main core */
        me_arena_end = aend;
    }
    for (uint32_t k = 0; k < me_nk; k++) me_ent[k]->pin = w3d_be_epoch();   /* now the open list calls them */
    w3d_me_hold = 0;
    w3d_be_hold(0);
    if (ok) me_call_epoch = w3d_be_epoch();      /* (a failed batch called nothing: the arena stays as it was) */
    u_me_replay += w3d_plat_us() - t2;
done:
    me_open = 0;
    me_nk = 0;
    w3d_be_kick();
    hw_end(2, (uint32_t)ndrec, (uint32_t)ndmesh);
    cam = keep;
    st_def_batches++;
    st_def_atomics += (uint64_t)ndrec;
    ndrec = ndmesh = ndcam = 0;
    w3d_lit_defer_reset();
    uint32_t dt = w3d_plat_us() - t0;
    u_flush += dt;
    if (!in_hook) u_hook += dt;
    w3d_defer_flushing = 0;
    dflushing = 0;
}

static void me_report(uint64_t frames)
{
    if (w3d_opt_vframe)
        LOG("world3d vframe (w3d_vframe %d): %llu lookups of a mesh already validated this frame skipped the game's "
            "memory (of %llu)\n", w3d_opt_vframe, (unsigned long long)st_vframe_hits, (unsigned long long)st_lookups);
    if (!w3d_opt_me) return;
    double n = frames ? (double)frames : 1.0;
    LOG("world3d ME (w3d_me %d, runner %d, hook %d, lit %d): %llu batches built by the ME, %llu drawn by the main core (no "
        "poller / failed: stale line %llu, arena full %llu, parts full %llu, bad item %llu, stall %llu), %llu batches "
        "with a full stream; meshes: %llu buildings to the ME, %llu lit / skinned records to the ME, %llu re-bake / "
        "CPU-path meshes + %llu lit records drawn by the main core; %llu segments, %llu KB of GE words, %llu texture "
        "lookups skipped (same as the previous mesh), %llu flushes asked for inside a record's phase 1; per 3D frame: "
        "phase 1 at the hook %.2f ms, at the flush %.2f ms, wait for the ME %.2f ms, replay %.2f ms\n",
        w3d_opt_me, w3d_opt_me_runner, w3d_opt_me_hook, w3d_opt_me_lit, (unsigned long long)st_me_batches,
        (unsigned long long)st_me_main_batches, (unsigned long long)st_me_fail[1], (unsigned long long)st_me_fail[2],
        (unsigned long long)st_me_fail[3], (unsigned long long)st_me_fail[4], (unsigned long long)st_me_fail[5],
        (unsigned long long)st_me_rest, (unsigned long long)st_me_meshes, (unsigned long long)st_me_lrec,
        (unsigned long long)st_me_mainmesh, (unsigned long long)st_me_mrec, (unsigned long long)st_me_segs,
        (unsigned long long)(st_me_words * 4u >> 10), (unsigned long long)st_me_texskip,
        (unsigned long long)st_me_flush_nested, (double)u_me_hookprep / 1e3 / n, (double)u_me_prep / 1e3 / n,
        (double)u_me_wait / 1e3 / n, (double)u_me_replay / 1e3 / n);
    w3dme_report();
    w3dme_report_bus(n);                          /* the ME's bus counters per iteration */
}

/* ------------------------------------------------------------------ host: the exploration dump (HLE_W3D_DUMP=file,
   HLE_W3D_FLIPS=n,n,...): every atomic of those flips with its camera, world matrix, geometry, meshes and DMA chains */
#ifndef HLE_PSP
static FILE *dump_f;
static uint32_t dump_flips[16];
static int ndump_flips;
static uint64_t dump_cur_flip = ~0ull;

static void dput(uint32_t tag, const void *p, uint32_t n)
{
    fwrite(&tag, 4, 1, dump_f);
    fwrite(&n, 4, 1, dump_f);
    if (n) fwrite(p, 1, n, dump_f);
}

static void dmem(uint32_t addr, uint32_t n)
{
    if (!ram_ok(addr, n)) return;
    uint32_t tag = 0x204D454Du, len = n + 4;
    fwrite(&tag, 4, 1, dump_f);
    fwrite(&len, 4, 1, dump_f);
    fwrite(&addr, 4, 1, dump_f);
    fwrite(hle_ram + addr, 1, n, dump_f);
}

static int dumping(void)
{
    if (!dump_f) return 0;
    uint64_t f = hle_render_flip_count() + 1;
    for (int i = 0; i < ndump_flips; i++)
        if (dump_flips[i] == f) {
            if (f != dump_cur_flip) { dump_cur_flip = f; uint32_t fl = (uint32_t)f; dput(0x50494C46u, &fl, 4); }
            return 1;
        }
    return 0;
}

static void dump_chain(uint32_t data)
{
    uint32_t p = data;
    for (int n = 0; n < 8192 && ram_ok(p, 16); n++) {
        uint32_t w0 = gw(p), w1 = gw(p + 4), id = (w0 >> 28) & 7, qwc = w0 & 0xFFFF;
        if (id == 3) { dmem(p, 16); dmem(w1 & 0x7FFFFFF0u, qwc * 16); p += 16; continue; }
        if (id == 1) { dmem(p, 16 + qwc * 16); p += 16 + qwc * 16; continue; }
        dmem(p, 16 + (id == 6 || id == 7 ? qwc * 16 : 0));
        break;
    }
}

static void dump_atomic(R5900Ctx *c)
{
    uint32_t pipe = gwk(G_RENDER_CTX), obj = gwk(ARG(1)), pds = pipe ? gwk(pipe + 0x30) : 0;
    if (!obj || gb(obj) != 1 || !dumping()) return;
    uint32_t a0 = ARG(0), a1 = ARG(1), geo = gwk(obj + 0x18), mh = geo ? gwk(geo + 0x54) : 0;
    uint32_t nmesh = mh ? gh(mh + 4) : 0, frame = gwk(obj + 4);
    uint32_t ltm = frame ? hle_call(c, FN_FRAME_MATRIX, 1, frame) : 0;
    c->r[4].d[0] = a0; c->r[5].d[0] = a1;
    uint32_t camera = gwk(DRV_ENGINE), camframe = camera ? gwk(camera + 4) : 0;
    uint32_t hdr[16] = {pipe, pds, obj, geo, ltm, camera, camframe ? camframe + 0x50 : 0, nmesh, mh};
    uint32_t rsv[32];
    for (int i = 0; i < 32; i++) rsv[i] = hle_render_rs(i);
    dput(0x45444F4Eu, hdr, sizeof hdr);
    dput(0x20205352u, rsv, sizeof rsv);
    dmem(obj, 0x70);
    if (ltm) dmem(ltm, 0x40);
    if (camera) dmem(camera, 0xC0);
    if (camframe) dmem(camframe, 0x90);
    if (geo) dmem(geo, 0x70);
    dmem(GP_DN_BALANCE, 4); dmem(0x66C5BCu, 4); dmem(GP_AMB_DN, 4); dmem(GP_AMB_BUILDING, 4); dmem(0x6681B0u, 0x40);
    dmem(0x668240u, 0x10); dmem(GP_MESHCACHE_GEO, 8);
    w3d_lit_dump_extra(c, pds, obj, dmem);
    if (!mh || !geo) return;
    dmem(mh, 0x10 + gw(mh + 0x0C) + 12u * nmesh);
    uint32_t mc_ptr = gw(geo + 0x18) == 1 ? geo + gw(GP_MESHCACHE_GEO) : obj + gw(0x66CB6Cu), mc = gwk(mc_ptr);
    uint32_t mrec[2] = {mc_ptr, mc};
    dput(0x4843434Du, mrec, 8);
    if (mc) dmem(mc, 4 + 4u * nmesh);
    for (uint32_t i = 0; i < nmesh && mc; i++) {
        uint32_t mesh = mh + 0x10 + gw(mh + 0x0C) + 12u * i, mat = gwk(mesh + 8), tex = mat ? gwk(mat) : 0;
        uint32_t ras = tex ? gwk(tex) : 0, re = gwk(mc + 4 + 4u * i);
        uint32_t mrec2[8] = {i, mesh, mat, tex, ras, re, re ? gwk(re + 0x20) : 0, 0};
        dput(0x4853454Du, mrec2, sizeof mrec2);
        if (mat) dmem(mat, 0x20);
        if (tex) dmem(tex, 0x58);
        if (ras) dmem(ras, 0x40);
        if (re) dmem(re, 0x40);
        if (re && gwk(re + 0x20)) dump_chain(gw(re + 0x20));
    }
}
#endif

/* ------------------------------------------------------------------ the hook */
HLE_FN(mesh_node_body)
{
    st_calls++;
#ifndef HLE_PSP
    if (dump_f) dump_atomic(c);
#endif
    if (!w3d_opt_on) { st_why[WHY_OFF]++; GUEST_FN(mesh_node_body)(c); return; }
    /* W3D_BB_LEAN: the hook's clock (a system call per read) on 1 call in 8, scaled x 8 */
    static uint32_t bb_hctr;
    int lean = (w3d_opt_bb & W3D_BB_LEAN) != 0, timed = !lean || (++bb_hctr & 7u) == 0;
    uint32_t t0 = timed ? w3d_plat_us() : 0;
    uint64_t a0 = c->r[4].d[0], a1 = c->r[5].d[0];
    in_hook = 1;
    int handled = w3d_atomic(c);
    in_hook = 0;
    c->r[4].d[0] = a0;                      /* hle_call (the frame matrix getter) used the argument registers */
    c->r[5].d[0] = a1;
    if (timed) u_hook += (uint64_t)(w3d_plat_us() - t0) * (lean ? 8u : 1u);
    if (handled && !w3d_opt_guest) { st_skipped_guest++; RET(1); return; }
    if (!handled) st_pass++;
    GUEST_FN(mesh_node_body)(c);
}

/* a new flip: a new frame for the LRU */
static uint64_t f3d_frames, f3d_last_atomics, f3d_tris0, f3d_cpu0, f3d_hook0;

#ifndef HLE_PSP
/* host test aid (vehicle paint check): HLE_W3D_TELEPORT=flip:x:y:z[,flip:x:y:z...] moves the player character (its
   matrix position, the same write that was checked in PCSX2) at those flips - for frames of places the pad
   script never reaches (city traffic). Changes the game only when set; never on the PSP. */
static void w3d_teleport(void)
{
    static int init, n, next;
    static struct { unsigned long long flip; float p[3]; } tp[8];
    if (!init) {
        init = 1;
        const char *e = getenv("HLE_W3D_TELEPORT");
        while (e && *e && n < 8) {
            if (sscanf(e, "%llu:%f:%f:%f", &tp[n].flip, &tp[n].p[0], &tp[n].p[1], &tp[n].p[2]) == 4) n++;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    if (next >= n || hle_render_flip_count() < tp[next].flip) return;
    uint32_t ped = gwk(0x0070A260u), m = ped ? gwk(ped + 0x14) : 0;
    if (m && ram_ok(m, 0x40)) {
        for (int i = 0; i < 3; i++) { uint32_t w; memcpy(&w, &tp[next].p[i], 4); sw_(m + 0x30 + 4u * (uint32_t)i, w); }
        LOG("world3d test teleport (HLE_W3D_TELEPORT) at flip %llu: player %08x -> %.1f %.1f %.1f\n",
            (unsigned long long)hle_render_flip_count(), ped, tp[next].p[0], tp[next].p[1], tp[next].p[2]);
    }
    next++;
}
#endif

void w3d_frame_end(void)
{
    w3d_defer_flush();                      /* normally empty: the flip's ge3d_leave / hle_raster flushed them */
    frame_no++;
    lodz_ref = -1e30f;                      /* the LOD-pass reference starts again with the next frame */
    u_rebake_frame = 0;
#ifndef HLE_PSP
    w3d_teleport();
#endif
    if (st_atomics == f3d_last_atomics) return;     /* no 3D in this frame */
    f3d_last_atomics = st_atomics;
    f3d_frames++;
    if (f3d_frames == 1 || f3d_frames % 512 == 0) {
        double n = f3d_frames == 1 ? 1.0 : 512.0;
        LOG("world3d frame %llu (flip %llu): %llu atomics so far; last %llu 3D frames: %.0f GE triangles, %.0f CPU-path "
            "triangles, %.2f ms in the hook per frame; vertex cache %u KB (peak %u), %llu conversions, %llu evictions, "
            "%llu re-bakes\n",
            (unsigned long long)f3d_frames, (unsigned long long)hle_render_flip_count(), (unsigned long long)st_atomics,
            (unsigned long long)(f3d_frames == 1 ? 1 : 512), (double)(st_tris_hw - f3d_tris0) / n,
            (double)(st_tris_cpu_in - f3d_cpu0) / n, (double)(u_hook - f3d_hook0) / 1e3 / n, arena_used >> 10,
            arena_peak >> 10, (unsigned long long)st_conv, (unsigned long long)st_evict,
            (unsigned long long)st_rebake);
        f3d_tris0 = st_tris_hw;
        f3d_cpu0 = st_tris_cpu_in;
        f3d_hook0 = u_hook;
        LOG("world3d camera planes so far: near %.3f..%.3f far %.1f..%.1f (GE near floor %d mm)\n", cn_min, cn_max, cf_min,
            cf_max, w3d_opt_near_mm);
    }
}

void hle_w3d_init(void)
{
    memset(hash_head, 0xFF, sizeof hash_head);
    for (int i = 0; i < W3D_MAXENT; i++) ent[i].next = -1;
#ifndef HLE_PSP
    static const char *keys[] = {"world3d", "mesh_guest", "w3d_kb", "w3d_cull", "w3d_clip", "w3d_fog", "w3d_afail",
                                 "w3d_front", "w3d_light", "w3d_gnear", "w3d_dnq", "w3d_rebake_us", "w3d_chunk",
                                 "w3d_chunk_group", "w3d_tilebase", "w3d_sky", "w3d_near_mm",
                                 "w3d_peds", "w3d_cars", "w3d_lit", "w3d_bones", "w3d_lights", "w3d_carpaint", "w3d_im3d", "w3d_defer", "w3d_hwprof", "w3d_gecmd", "w3d_kick", "w3d_texskip",
                                 "w3d_me", "w3d_me_runner", "w3d_me_stall_ms", "w3d_me_hook", "w3d_me_lit", "w3d_vframe",
                                 "w3d_lodz", "w3d_bb", "w3d_me_bus", NULL};
    for (int i = 0; keys[i]; i++) {               /* host: HLE_<KEY> (HLE_WORLD3D, HLE_MESH_GUEST, HLE_W3D_KB, ...) */
        char env[48] = "HLE_";
        size_t n = strlen(keys[i]);
        for (size_t j = 0; j < n && j < 40; j++) env[4 + j] = (char)(keys[i][j] >= 'a' && keys[i][j] <= 'z' ? keys[i][j] - 32 : keys[i][j]);
        env[4 + (n < 40 ? n : 40)] = 0;
        const char *e = getenv(env);
        if (e) w3d_cfg(keys[i], e);
    }
    const char *p = getenv("HLE_W3D_DUMP"), *f = getenv("HLE_W3D_FLIPS");
    if (p && f) {
        dump_f = fopen(p, "wb");
        for (const char *s = f; *s && ndump_flips < 16;) {
            char *end;
            unsigned long v = strtoul(s, &end, 10);
            if (end == s) break;
            dump_flips[ndump_flips++] = (uint32_t)v;
            s = *end ? end + 1 : end;
        }
        LOG("W3D dump to %s: %d flips\n", p, ndump_flips);
    }
#endif
#ifdef HLE_PSP
    if (!hle_ge_on && w3d_opt_on) { w3d_opt_on = 0; LOG("world3d: off (render=text: no GE)\n"); }
#endif
    if (w3d_opt_kb < 128) w3d_opt_kb = 128;
    if (w3d_opt_kb > 4096) w3d_opt_kb = 4096;
    if (w3d_opt_chunk < 24) w3d_opt_chunk = 24;
    if (w3d_opt_chunk > 960) w3d_opt_chunk = 960;
    w3d_opt_chunk -= w3d_opt_chunk % 3;          /* triangle-list prims of whole triangles */
    chunk_v = (uint32_t)w3d_opt_chunk;
    if (w3d_opt_chunk_group < 1) w3d_opt_chunk_group = 1;
    if (w3d_opt_chunk_group > 16) w3d_opt_chunk_group = 16;
    chunk_group = (uint32_t)w3d_opt_chunk_group;
    if (w3d_opt_on) {
        arena_bytes = (uint32_t)w3d_opt_kb << 10;
        arena = w3d_plat_arena(arena_bytes);
        if (!arena) { w3d_opt_on = 0; LOG("world3d: no memory for the vertex cache (%u KB): 3D off\n", arena_bytes >> 10); }
        else {
            blk[0].off = 0; blk[0].size = arena_bytes; blk[0].used = 0; blk[0].owner = -1; nblk = 1;
        }
    }
    LOG("world3d=%d (mesh node 0x391398 hook: building atomics on the %s), mesh_guest=%d, "
        "vertex cache %d KB, cull=%d fog=%d clip=%d front=%d afail=%d\n", w3d_opt_on,
#ifdef HLE_PSP
        "GE",
#else
        "host GE model",
#endif
        w3d_opt_guest, w3d_opt_kb, w3d_opt_cull, w3d_opt_fog, w3d_opt_clip, w3d_opt_front, w3d_opt_afail);
    LOG("world3d: light=%d (material / ambient as GE lighting) gnear=%d (GE near clipping) dnq=%d "
        "rebake_us=%d chunk=%d group=%d\n", w3d_opt_light, w3d_opt_gnear, w3d_opt_dnq, w3d_opt_rebake_us,
        w3d_opt_chunk, w3d_opt_chunk_group);
    w3d_lit_init();
}

void hle_w3d_report(void)
{
    LOG("world3d: %llu mesh node calls: %llu atomics drawn by the 3D path (translated node skipped for %llu), %llu "
        "passed to the translated node\n", (unsigned long long)st_calls, (unsigned long long)st_atomics,
        (unsigned long long)st_skipped_guest, (unsigned long long)st_pass);
    for (int i = 0; i < WHY_N; i++)
        if (st_why[i]) LOG("world3d:   passed: %-16s %llu\n", why_name[i], (unsigned long long)st_why[i]);
    for (int i = 0; i < npds; i++)
        LOG("world3d:   atomics of pipeline %08x: %llu\n", pds_ids[i], (unsigned long long)pds_calls[i]);
    LOG("world3d: meshes %llu: GE %llu, CPU-clipped %llu, culled %llu, failed %llu, texture not drawable %llu, two-pass "
        "(alpha) %llu; triangles GE %llu, CPU in %llu -> indexed %llu + clipped out %llu (%llu clipped), CPU vertices "
        "%llu\n",
        (unsigned long long)st_meshes, (unsigned long long)st_m_hw, (unsigned long long)st_m_cpu,
        (unsigned long long)st_m_cull, (unsigned long long)st_m_fail, (unsigned long long)st_m_notex,
        (unsigned long long)st_m_twopass, (unsigned long long)st_tris_hw, (unsigned long long)st_tris_cpu_in,
        (unsigned long long)st_tris_cpu_idx, (unsigned long long)st_tris_cpu_out, (unsigned long long)st_tris_cpu_clipped, (unsigned long long)st_cpu_verts);
    LOG("world3d: chunks of partly visible / big meshes: %llu GE, %llu CPU, %llu culled (%llu group boxes); %llu second "
        "alpha passes drew the CPU path's list copies again\n", (unsigned long long)st_ch_hw,
        (unsigned long long)st_ch_cpu, (unsigned long long)st_ch_cull, (unsigned long long)st_ch_groups,
        (unsigned long long)st_cpu_replays);
    LOG("world3d: vertex cache: %llu conversions (%llu KB, %.1f ms, max %u us; %llu by the generic walk), %llu failed, "
        "%llu no room, %llu evictions (%llu windows), %llu stale entries replaced, %llu re-bakes (%llu KB, %llu deferred "
        "by the budget, %llu found the mesh changed), %llu copies, lookups %llu (hits %llu), arena %u/%u KB (peak %u, %d "
        "blocks)\n",
        (unsigned long long)st_conv, (unsigned long long)(st_conv_bytes >> 10), (double)u_conv / 1e3, conv_max_us,
        (unsigned long long)st_conv_slow, (unsigned long long)st_conv_fail, (unsigned long long)st_noroom,
        (unsigned long long)st_evict, (unsigned long long)st_evict_windows, (unsigned long long)st_stale,
        (unsigned long long)st_rebake, (unsigned long long)(st_rebake_bytes >> 10), (unsigned long long)st_rebake_defer,
        (unsigned long long)st_rebake_mismatch, (unsigned long long)st_dyncopy, (unsigned long long)st_lookups,
        (unsigned long long)st_hits, arena_used >> 10, arena_bytes >> 10, arena_peak >> 10, nblk);
    if (f3d_frames) {
        double n = (double)f3d_frames;
        LOG("world3d: per 3D frame (%llu frames): %.1f atomics, %.1f meshes, %.0f GE triangles (cached meshes + chunk "
            "ranges), %.0f CPU-path triangles (%.0f indexed, %.0f clipped out), %.2f ms in the hook\n",
            (unsigned long long)f3d_frames, (double)st_atomics / n, (double)st_meshes / n, (double)st_tris_hw / n,
            (double)st_tris_cpu_in / n, (double)st_tris_cpu_idx / n, (double)st_tris_cpu_out / n,
            (double)u_hook / 1e3 / n);
        /* sampled phases scaled: per drawn mesh (lookup + classification also for the culled ones), per atomic */
        double drawn = (double)(st_m_hw + st_m_cpu), allm = drawn + (double)st_m_cull;
        double sm = n_samp_mesh ? 1.0 / (double)n_samp_mesh : 0.0, sa = n_samp_atom ? 1.0 / (double)n_samp_atom : 0.0;
        double e_look = (double)us_lookup * sm * allm, e_class = (double)us_class * sm * allm;
        double e_tex = (double)us_tex * sm * drawn, e_draw = (double)us_draw * sm * drawn;
        double e_setup = (double)us_setup * sa * (double)st_atomics;
        double known = (double)(u_conv + u_rebake + u_cpu + u_lit) + e_look + e_class + e_tex + e_draw + e_setup;
        LOG("world3d: hook ms per 3D frame: total %.2f = conversion %.2f + re-bake %.2f + CPU path %.2f + lit / skinned "
            "atomics %.2f (their conversion %.2f) + sampled: atomic setup %.2f, mesh lookup %.2f, classification %.2f, "
            "texture %.2f, state + draw commands %.2f; unaccounted %.2f\n", (double)u_hook / 1e3 / n,
            (double)u_conv / 1e3 / n, (double)u_rebake / 1e3 / n, (double)u_cpu / 1e3 / n, (double)u_lit / 1e3 / n,
            (double)w3d_u_lit_conv / 1e3 / n, e_setup / 1e3 / n, e_look / 1e3 / n, e_class / 1e3 / n, e_tex / 1e3 / n,
            e_draw / 1e3 / n, ((double)u_hook - known) / 1e3 / n);
    }
    LOG("world3d: CPU time in the hook %.1f ms (conversion %.1f ms, re-bake %.1f ms, CPU clipping %.1f ms), %.1f us per "
        "handled atomic\n", (double)u_hook / 1e3, (double)u_conv / 1e3, (double)u_rebake / 1e3, (double)u_cpu / 1e3,
        st_atomics ? (double)u_hook / (double)st_atomics : 0.0);
    if (f3d_frames) {                       /* the PS2's LOD pass (lod_pass) */
        double n = (double)f3d_frames;
        LOG("world3d LOD pass (w3d_lodz %d): per 3D frame %.1f LOD atomics of %.1f (%.1f / %.1f / %.1f of "
            "them within 20 / 50 / 100 m; all atomics %.1f / %.1f / %.1f); %llu drawn with GREATER\n", w3d_opt_lodz,
            (double)st_lod_atomics / n, (double)st_atomics / n, (double)st_lod_near[0] / n, (double)st_lod_near[1] / n,
            (double)st_lod_near[2] / n, (double)st_all_near[0] / n, (double)st_all_near[1] / n,
            (double)st_all_near[2] / n, (unsigned long long)st_lodz_set);
    }
    w3d_lit_report();
    w3d_im3d_report();
    LOG("world3d defer (w3d_defer %d): %llu atomics (%llu of them lit / skinned) drawn from %llu batches (%.1f per "
        "batch), %llu lost (no target at the flush), %llu flushes because the buffer was full; %.2f ms in the flushes "
        "per 3D frame\n", w3d_opt_defer, (unsigned long long)st_def_atomics, (unsigned long long)st_def_lit,
        (unsigned long long)st_def_batches,
        st_def_batches ? (double)st_def_atomics / (double)st_def_batches : 0.0, (unsigned long long)st_def_lost,
        (unsigned long long)st_def_full, f3d_frames ? (double)u_flush / 1000.0 / (double)f3d_frames : 0.0);
    w3d_hw_report();
    LOG("world3d (w3d_bb %d): frame matrix native %llu / guest %llu; ME texture descriptors from the flush "
        "cache %llu (looked up %llu); lit records prepared while waiting for the ME %llu (%.2f ms per 3D frame); the "
        "hook clock %s\n", w3d_opt_bb, (unsigned long long)st_bb_ltm_native, (unsigned long long)st_bb_ltm_guest,
        (unsigned long long)st_bb_texc_hits, (unsigned long long)st_bb_texc_miss, (unsigned long long)st_bb_litprep,
        f3d_frames ? (double)u_bb_litprep / 1e3 / (double)f3d_frames : 0.0,
        (w3d_opt_bb & W3D_BB_LEAN) ? "sampled 1 in 8 (hook ms are estimates)" : "on every call");
    me_report(f3d_frames);
    w3d_be_report();
#ifndef HLE_PSP
    if (dump_f) fclose(dump_f);
#endif
}

void w3d_lit_time(uint32_t us) { u_lit += us; }
