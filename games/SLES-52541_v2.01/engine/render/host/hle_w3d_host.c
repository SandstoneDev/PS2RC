/* hle_w3d_host.c - host back-end of the 3D world: a model of what the PSP GE does with
 * hle_w3d.c's draws, so the converter, the matrices, the strips / sub-lists, the guard-band classification and the CPU
 * clipper run (and are checked) on the host:
 *   - the sub-list of a cached mesh is executed like the GE does it (VTYPE, PRIM after PRIM with the vertex address
 *     advancing, RET); vertices: 16-bit UV / 32768 x texture scale + offset, 16-bit positions / 32768, world matrix,
 *     identity view, the projection; strip triangles with the GE's alternating winding;
 *   - the GE's discard rules are CHECKED (they must never trigger: the classification exists to avoid them): a vertex
 *     outside 0 <= x, y < 4096 after the viewport (centre 2048, 480 x 272), behind the near plane (the GE would clip),
 *     two vertices 0xAAAA (2730.625) or more apart in Y (a GE hardware restriction) - counted and reported;
 *   - with the host render capture on (--frames DIR) the triangles are rasterised into the same picture as the 2D
 *     (hle_raster.c: 16-bit Z like the GE, perspective-correct, fog, alpha test, blending, back-face culling).
 * Memory: the vertex cache is malloc'ed; the GE "epoch" advances after every atomic (the host has no GE latency).
 * Also modelled: the GE's near-plane clipping (w3d_gnear=1 sends triangles that cross the near plane; they are
 * clipped against Zc = -Wc, then the window rule applies to the clipped vertices), GE lighting with no light
 * sources (w3d_light=1: emissive + ambient light x vertex colour, alpha = vertex alpha x ambient alpha), directional
 * lights + normals and bone matrices for the lit / skinned pipelines (w3d_be_lights / w3d_be_bones). */
#ifndef HLE_PSP
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "hle.h"
#include "hle_render.h"
#include "hle_w3d.h"
#include "hle_w3d_me.h"

static W3dCam cam;
static float world[12];
static W3dState cur;
static uint32_t tex_raster;
static int tex_filter, tex_au, tex_av;
static uint32_t epoch = 1;
static uint64_t n_tris, n_drawn, n_culled, n_out_window, n_behind, n_yspan, n_degenerate, n_atomics, n_meshes, n_cpu_tris, n_idx_tris, n_ranges;
static uint64_t n_nearclip, n_lit_verts;
static uint8_t *scratch;
static uint32_t scratch_n;
static int cur_path;                         /* diagnostics: 0 mesh, 1 range, 2 indexed, 3 CPU floats, 4 lit */
static void copies_free(void);
static const char *path_name[] = {"mesh", "range", "indexed", "cpu-float", "lit"};

void *w3d_plat_arena(uint32_t bytes) { return calloc(1, bytes); }
void w3d_plat_flush(const void *p, uint32_t bytes) { (void)p; (void)bytes; }
uint32_t w3d_plat_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}

/* GE lighting without light sources (w3d_light): emissive + ambient light x vertex colour, alpha x ambient alpha */
static inline uint32_t mul255(uint32_t a, uint32_t b) { return (a * b + 127u) / 255u; }
static uint32_t light_c(uint32_t c)
{
    if (!cur.light) return c;
    uint32_t o = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t v = (cur.lemis >> (8 * i) & 0xFF) + mul255(c >> (8 * i) & 0xFF, cur.lamb >> (8 * i) & 0xFF);
        o |= (v > 255 ? 255 : v) << (8 * i);
    }
    return o | mul255(c >> 24, cur.lamb >> 24) << 24;
}

int w3d_be_target(void)
{
    uint32_t fb = hle_render_cur_fb(), scr = hle_render_screen_raster();
    return !(scr && fb && fb != scr);        /* a mirror / render-target camera: not the screen */
}

int w3d_be_begin(const W3dCam *c)
{
    if (!w3d_defer_flushing && !w3d_be_target()) return 0;   /* (deferred records were checked when taken) */
    copies_free();
    cam = *c;
    n_atomics++;
    return 1;
}

uint32_t w3d_be_epoch(void) { return epoch; }
void w3d_be_end(void) { epoch++; }
void w3d_be_kick(void) {}
void w3d_be_world(const float m[12]) { memcpy(world, m, sizeof world); }

int w3d_be_texture(uint32_t raster, int filter, int au, int av, int *aclass)
{
    *aclass = 0;
    tex_raster = raster;
    tex_filter = filter;
    tex_au = au;
    tex_av = av;
    if (!raster) return 1;
    int a = hle_raster_alpha_class(raster);
    if (a < 0) return 0;
    *aclass = a;
    return 1;
}

void w3d_be_state(const W3dState *st) { cur = *st; }

void *w3d_be_alloc(uint32_t bytes)
{
    if (bytes > scratch_n) {
        free(scratch);
        scratch = malloc(bytes);
        scratch_n = scratch ? bytes : 0;
    }
    return scratch;
}

/* the lit pipelines' projection, the game's lights and bone matrices */
static float proj_cur[16];
static int proj_custom;
static W3dLights lights_cur;
static float bones[8][12];
static int nbones_cur;

void w3d_be_proj(const float *p)
{
    if (p) { memcpy(proj_cur, p, sizeof proj_cur); proj_custom = 1; }
    else proj_custom = 0;
}
void w3d_be_lights(const W3dLights *l) { lights_cur = *l; }
void w3d_be_bones(int n, const float (*m)[12])
{
    nbones_cur = n > 8 ? 8 : n;
    memcpy(bones, m, sizeof bones[0] * (size_t)nbones_cur);
}

/* GE model: object position -> clip (x, y, z, w) and the view-space z (fog) */
static void ge_xform(float x, float y, float z, float *clip, float *vz)
{
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = x * world[k] + y * world[3 + k] + z * world[6 + k] + world[9 + k];
    *vz = p[2];
    const float *m = proj_custom ? proj_cur : cam.proj;
    for (int k = 0; k < 4; k++) clip[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
}

/* a GE vertex of any type (weights / UV / normal / position; colour not used by the lit converter): the GE layout rule
   (every element aligned to its size, the vertex to its largest element; PPSSPP and the PSP agree) */
typedef struct { int nw, wsz, uvf, nf, pf; uint32_t woff, uvoff, noff, poff, size; } HLay;
static HLay hlay(uint32_t vt)
{
    static const int esz[4] = {0, 1, 2, 4};
    HLay l;
    memset(&l, 0, sizeof l);
    int wf = (int)(vt >> 9 & 3), tf = (int)(vt & 3), nf = (int)(vt >> 5 & 3), pf = (int)(vt >> 7 & 3);
    l.nw = wf ? (int)(vt >> 14 & 7) + 1 : 0;
    l.wsz = esz[wf]; l.uvf = tf; l.nf = nf; l.pf = pf;
    uint32_t o = 0, big = 1;
#define ALIGN(sz) do { uint32_t s_ = (uint32_t)(sz); if (s_ > big) big = s_; o = (o + s_ - 1) & ~(s_ - 1); } while (0)
    if (l.nw) { ALIGN(esz[wf]); l.woff = o; o += (uint32_t)(l.nw * esz[wf]); }
    if (tf) { ALIGN(esz[tf]); l.uvoff = o; o += 2u * (uint32_t)esz[tf]; }
    if (nf) { ALIGN(esz[nf]); l.noff = o; o += 3u * (uint32_t)esz[nf]; }
    if (pf) { ALIGN(esz[pf]); l.poff = o; o += 3u * (uint32_t)esz[pf]; }
#undef ALIGN
    l.size = (o + big - 1) & ~(big - 1);
    return l;
}

static float rd_comp(const uint8_t *p, int fmt, int i, int is_uv)
{
    switch (fmt) {
    case 1: return is_uv ? p[i] / 128.0f : ((const int8_t *)p)[i] / 127.0f;
    case 2: return is_uv ? ((const uint16_t *)p)[i] / 32768.0f : ((const int16_t *)p)[i] / 32768.0f;
    default: { float f; memcpy(&f, p + 4 * i, 4); return f; }
    }
}

/* the GE's lighting with the game's lights (state lights): colour = MAC x AC + sum(MDC x light x max(N.L, 0) / |N|) */
static uint32_t lit_colour(const float *nrm)
{
    uint32_t m = cur.lamb, sa = cur.lemis & 0xFF, sd = cur.lemis >> 8 & 0xFF;
    float n_len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    uint32_t o = 0;
    for (int ch = 0; ch < 3; ch++) {
        float mc = (float)(m >> (8 * ch) & 0xFF) / 255.0f;
        float v = (float)(cur.lemit >> (8 * ch) & 0xFF) / 255.0f + mc * sa / 255.0f * lights_cur.amb[ch];
        for (int i = 0; i < lights_cur.n && i < cur.lights - 1; i++) {
            float d = n_len > 0.0f ? (nrm[0] * lights_cur.dir[i][0] + nrm[1] * lights_cur.dir[i][1] +
                                      nrm[2] * lights_cur.dir[i][2]) / n_len : 0.0f;
            if (d > 0.0f) v += mc * sd / 255.0f * lights_cur.col[i][ch] * d;
        }
        float c = v * 255.0f + 0.5f;
        o |= (uint32_t)(c <= 0.0f ? 0 : c >= 255.0f ? 255 : (int)c) << (8 * ch);
    }
    return o | (m & 0xFF000000u);
}

typedef struct { float clip[4], vz, u, v; uint32_t c; } GV;

static void tri_post(const GV *a, const GV *b, const GV *c)
{
    const GV *t[3] = {a, b, c};
    float xs[3], ys[3];
    for (int i = 0; i < 3; i++) {
        float w = t[i]->clip[3];
        if (!(w > 0.0f) || t[i]->clip[2] < -w * 1.0001f - 1e-6f) { n_behind++; return; }   /* not near-clipped */
        xs[i] = 2048.0f + t[i]->clip[0] / w * 240.0f;
        ys[i] = 2048.0f - t[i]->clip[1] / w * 136.0f;
        if (!(xs[i] >= 0.0f && xs[i] < 4096.0f && ys[i] >= 0.0f && ys[i] < 4096.0f)) {
            if (++n_out_window <= 8)
                LOG("world3d host GE model: window violation #%llu (%s): vertex %d clip %.3f %.3f %.3f %.3f -> screen "
                    "%.1f %.1f\n", (unsigned long long)n_out_window, path_name[cur_path], i, t[i]->clip[0],
                    t[i]->clip[1], t[i]->clip[2], w, xs[i], ys[i]);
            return;
        }
    }
    float ymin = fminf(ys[0], fminf(ys[1], ys[2])), ymax = fmaxf(ys[0], fmaxf(ys[1], ys[2]));
    if (ymax - ymin >= 2730.625f) { n_yspan++; return; }
    float area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0]);
    if (area == 0.0f) { n_degenerate++; return; }
    /* front faces: negative area on the y-down screen (= the GE's CCW, sceGuFrontFace(GU_CCW)) */
    int front = w3d_opt_front ? area > 0.0f : area < 0.0f;
    if ((cur.cull == 1 && !front) || (cur.cull == 2 && front)) { n_culled++; return; }
    n_drawn++;
    if (!hle_raster_is_open()) return;
    Rnd3Vtx v[3];
    float fog1 = -cam.far_z, fog2 = cam.far_z > cam.fog_z ? -1.0f / (cam.far_z - cam.fog_z) : 0.0f;
    for (int i = 0; i < 3; i++) {
        float w = t[i]->clip[3];
        v[i].x = t[i]->clip[0] / w;
        v[i].y = t[i]->clip[1] / w;
        float zn = t[i]->clip[2] / w;
        v[i].z = 32767.5f - 32767.5f * zn;
        v[i].q = 1.0f / w;
        v[i].u = t[i]->u;
        v[i].v = t[i]->v;
        uint32_t c = t[i]->c;
        v[i].r = (float)(c & 0xFF); v[i].g = (float)(c >> 8 & 0xFF); v[i].b = (float)(c >> 16 & 0xFF);
        v[i].a = (float)(c >> 24);
        float f = (t[i]->vz + fog1) * fog2;
        v[i].fog = cur.fog ? (f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f) : 1.0f;
    }
    Rnd3State rs;
    memset(&rs, 0, sizeof rs);
    rs.s.raster = cur.textured ? tex_raster : 0;
    rs.s.filter = tex_filter;
    rs.s.addr_u = tex_au;
    rs.s.addr_v = tex_av;
    rs.s.src_blend = cur.src_blend;
    rs.s.dst_blend = cur.dst_blend;
    rs.blend = cur.blend;
    rs.atest = cur.atest;
    rs.ztest = cur.ztest;                       /* (a Z write without Z test: the PSP back-end tests ALWAYS) */
    rs.zwrite = cur.zwrite;
    rs.fog_color = cur.fog_color;
    hle_raster_tris3d(v, 3, &rs);
}

static uint32_t lerp_c(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        float x = (float)(a >> (8 * i) & 0xFF), y = (float)(b >> (8 * i) & 0xFF), v = x + (y - x) * t + 0.5f;
        o |= (uint32_t)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)v) << (8 * i);
    }
    return o;
}

/* the GE: a triangle with every vertex outside the near clip surface (Zc < -Wc) is discarded, one that crosses it is
   clipped (clipped first, then divided) and the window rule applies to the result */
static void tri(const GV *a, const GV *b, const GV *c)
{
    const GV *t[3] = {a, b, c};
    n_tris++;
    float d[3];
    int nin = 0;
    for (int i = 0; i < 3; i++) { d[i] = t[i]->clip[2] + t[i]->clip[3]; nin += d[i] >= 0.0f; }
    if (nin == 3) { tri_post(a, b, c); return; }
    if (nin == 0) return;                       /* all outside one clipping surface: discarded */
    n_nearclip++;
    static int diag_nc = 6;                     /* a near-clipped triangle must lie inside the guard pyramid */
    for (int i = 0; i < 3 && diag_nc > 0; i++) {
        float w = t[i]->clip[3];
        if (!(w > 0.0f) || fabsf(t[i]->clip[0]) > 8.5f * w || fabsf(t[i]->clip[1]) > 10.0f * w) {
            diag_nc--;
            LOG("world3d host GE model: near-clipped triangle (%s) with a vertex outside the guard pyramid: "
                "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", path_name[cur_path],
                a->clip[0], a->clip[1], a->clip[2], a->clip[3], b->clip[0], b->clip[1], b->clip[2], b->clip[3],
                c->clip[0], c->clip[1], c->clip[2], c->clip[3]);
            break;
        }
    }
    GV poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const GV *p = t[i], *q = t[(i + 1) % 3];
        float dp = d[i], dq = d[(i + 1) % 3];
        if (dp >= 0.0f) poly[n++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            float s = dp / (dp - dq);
            GV *r = &poly[n++];
            for (int k = 0; k < 4; k++) r->clip[k] = p->clip[k] + (q->clip[k] - p->clip[k]) * s;
            r->clip[2] = -r->clip[3];               /* on the near clip surface (no rounding drift) */
            r->vz = p->vz + (q->vz - p->vz) * s;
            r->u = p->u + (q->u - p->u) * s;
            r->v = p->v + (q->v - p->v) * s;
            r->c = lerp_c(p->c, q->c, s);
        }
    }
    for (int i = 1; i + 1 < n; i++) tri_post(&poly[0], &poly[i], &poly[i + 1]);
}

static void load(const W3dMeshView *m, const uint8_t *verts, uint32_t k, GV *o)
{
    int16_t x, y, z;
    if (m->fmt == W3D_FMT_LIT) {                 /* lit / skinned vertex (any vtype), GE skinning + lighting */
        HLay l = hlay(m->vtype);
        const uint8_t *v = verts + (size_t)k * l.size;
        float p[3], n[3] = {0, 0, 1};
        for (int i = 0; i < 3; i++) p[i] = rd_comp(v + l.poff, l.pf, i, 0);
        if (l.nf) for (int i = 0; i < 3; i++) n[i] = rd_comp(v + l.noff, l.nf, i, 0);
        o->u = l.uvf ? rd_comp(v + l.uvoff, l.uvf, 0, 1) * cur.uscale + cur.ubase : 0.0f;
        o->v = l.uvf ? rd_comp(v + l.uvoff, l.uvf, 1, 1) * cur.uscale + cur.vbase : 0.0f;
        if (l.nw) {                               /* sum of weight x (vertex x bone matrix); weights not normalised */
            float ps[3] = {0, 0, 0}, ns[3] = {0, 0, 0};
            for (int b = 0; b < l.nw && b < nbones_cur; b++) {
                float w = l.wsz == 1 ? v[l.woff + (uint32_t)b] / 128.0f
                        : l.wsz == 2 ? ((const uint16_t *)(v + l.woff))[b] / 32768.0f
                        : rd_comp(v + l.woff, 3, b, 0);
                if (w == 0.0f) continue;
                const float *B = bones[b];
                for (int c = 0; c < 3; c++) {
                    ps[c] += w * (p[0] * B[c] + p[1] * B[3 + c] + p[2] * B[6 + c] + B[9 + c]);
                    ns[c] += w * (n[0] * B[c] + n[1] * B[3 + c] + n[2] * B[6 + c]);
                }
            }
            memcpy(p, ps, sizeof p);
            memcpy(n, ns, sizeof n);
        }
        float nw[3];                              /* the normal through the world matrix itself (GE) */
        for (int c = 0; c < 3; c++) nw[c] = n[0] * world[c] + n[1] * world[3 + c] + n[2] * world[6 + c];
        o->c = cur.lights ? lit_colour(nw) : 0xFFFFFFFFu;
        n_lit_verts++;
        ge_xform(p[0], p[1], p[2], o->clip, &o->vz);
        return;
    }
    if (m->fmt == W3D_FMT_UV16) {
        const W3dV16 *v = (const W3dV16 *)verts + k;
        x = v->x; y = v->y; z = v->z; o->c = light_c(v->c);
        o->u = v->u / 32768.0f * cur.uscale + cur.ubase;
        o->v = v->v / 32768.0f * cur.uscale + cur.vbase;
    } else {
        const W3dVF *v = (const W3dVF *)verts + k;
        x = v->x; y = v->y; z = v->z; o->c = light_c(v->c);
        o->u = v->u * cur.uscale + cur.ubase;
        o->v = v->v * cur.uscale + cur.vbase;
    }
    ge_xform(x / 32768.0f, y / 32768.0f, z / 32768.0f, o->clip, &o->vz);
}

static void run_prims(const W3dMeshView *m, const uint8_t *verts, uint32_t k, const uint32_t *words, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = words[i], cmd = w >> 24;
        if (cmd == 0x0B) break;
        if (cmd != 0x04) continue;
        uint32_t cnt = w & 0xFFFF, prim = w >> 16 & 7;
        GV v[3];
        if (prim == 3) {
            for (uint32_t j = 0; j + 2 < cnt; j += 3) {
                for (int q = 0; q < 3; q++) load(m, verts, k + j + (uint32_t)q, &v[q]);
                tri(&v[0], &v[1], &v[2]);
            }
        } else if (prim == 4) {
            for (uint32_t j = 0; j < cnt; j++) {
                load(m, verts, k + j, &v[j % 3]);
                if (j < 2) continue;
                const GV *a = &v[(j - 2) % 3], *b = &v[(j - 1) % 3], *c = &v[j % 3];
                if ((j - 2) & 1) tri(b, a, c); else tri(a, b, c);
            }
        }
        k += cnt;                              /* the GE advances the vertex address after each PRIM */
    }
}

void w3d_be_mesh(const W3dMeshView *m, const uint8_t *override)
{
    n_meshes++;
    cur_path = 0;
    run_prims(m, override ? override : m->verts, 0, m->sub + 1, m->nsub - 1);   /* the sub-list: VTYPE, PRIM..., RET */
}

void w3d_be_range(const W3dMeshView *m, const uint8_t *override, uint32_t first_vert, const uint32_t *prims,
                  uint32_t nprims)
{
    n_ranges++;
    cur_path = 1;
    run_prims(m, override ? override : m->verts, first_vert, prims, nprims);
}

/* the host's "list memory" for the copies the second alpha pass draws again: kept until the next atomic */
static void *copies[256];
static int ncopies;
static const void *keep(const void *p, size_t bytes)
{
    if (ncopies >= 256) return NULL;
    void *c = malloc(bytes);
    if (c) { memcpy(c, p, bytes); copies[ncopies++] = c; }
    return c;
}
static void copies_free(void) { while (ncopies) free(copies[--ncopies]); }

static int in_again;
static void draw_indexed(const W3dMeshView *m, const uint8_t *override, const uint16_t *idx, int n)
{
    const uint8_t *verts = override ? override : m->verts;
    cur_path = 2;
    for (int i = 0; i + 2 < n; i += 3) {
        GV g[3];
        for (int q = 0; q < 3; q++) load(m, verts, idx[i + q], &g[q]);
        static int diag_ix = 6;
        for (int q = 0; q < 3 && diag_ix > 0; q++)
            if (!(g[q].clip[3] > 0.0f) || fabsf(g[q].clip[0]) > 8.6f * g[q].clip[3]) {
                diag_ix--;
                LOG("world3d host GE model: indexed draw%s: triangle %d of %d, indices %u %u %u, vertex %d clip %.3f %.3f "
                    "%.3f %.3f, verts %p nverts %u\n", in_again ? " (again)" : "", i / 3, n / 3, idx[i], idx[i + 1],
                    idx[i + 2], q, g[q].clip[0], g[q].clip[1], g[q].clip[2], g[q].clip[3], (const void *)verts,
                    m->nverts);
                break;
            }
        n_idx_tris++;
        tri(&g[0], &g[1], &g[2]);
    }
}

static void draw_tris(const W3dVC *v, int n)
{
    cur_path = 3;
    for (int i = 0; i + 2 < n; i += 3) {
        GV g[3];
        for (int q = 0; q < 3; q++) {
            const W3dVC *s = &v[i + q];
            g[q].u = s->u * cur.uscale + cur.ubase;
            g[q].v = s->v * cur.uscale + cur.vbase;
            g[q].c = light_c(s->c);
            ge_xform(s->x, s->y, s->z, g[q].clip, &g[q].vz);
        }
        n_cpu_tris++;
        tri(&g[0], &g[1], &g[2]);
    }
}

const void *w3d_be_indexed(const W3dMeshView *m, const uint8_t *override, const uint16_t *idx, int n)
{
    draw_indexed(m, override, idx, n);
    return keep(idx, (size_t)n * 2u);
}

const void *w3d_be_tris(const W3dVC *v, int n)
{
    draw_tris(v, n);
    return keep(v, (size_t)n * sizeof *v);
}

void w3d_be_prims(uint32_t vtype, const void *verts, int n)
{
    W3dMeshView mv;
    memset(&mv, 0, sizeof mv);
    mv.verts = verts;
    mv.fmt = W3D_FMT_LIT;
    mv.vtype = vtype;
    cur_path = 4;
    for (int i = 0; i + 2 < n; i += 3) {
        GV g[3];
        for (int q = 0; q < 3; q++) load(&mv, verts, (uint32_t)(i + q), &g[q]);
        n_cpu_tris++;
        tri(&g[0], &g[1], &g[2]);
    }
}

void w3d_be_indexed_again(const W3dMeshView *m, const uint8_t *override, const void *copy, int n)
{
    in_again = 1;
    if (copy) draw_indexed(m, override, copy, n);
    in_again = 0;
}

void w3d_be_tris_again(const void *copy, int n)
{
    if (copy) draw_tris(copy, n);
}

/* w3d_me: the host's ME writes operations (hle_w3d.c replays them through this back-end); a texture is
   its raster + the driver's filter / addressing, its alpha class the one w3d_be_texture reports */
int w3d_be_tex_desc(uint32_t raster, int filter, int au, int av, W3mTex *d)
{
    memset(d, 0, sizeof *d);
    d->textured = raster != 0;
    if (raster) {
        int a = hle_raster_alpha_class(raster);
        if (a < 0) return 0;
        d->acl = (uint32_t)a;
    }
    d->ok = 1;
    d->tbp = raster;
    d->psm_swz_exp = ((uint32_t)filter & 0xFF) | ((uint32_t)au & 0xFF) << 8 | ((uint32_t)av & 0xFF) << 16;
    return 1;
}
void w3d_be_hold(int on) { (void)on; }
void w3d_be_forget(void) {}
int w3d_be_seg(const uint32_t *seg) { (void)seg; return 1; }
int w3d_be_me_selftest(char *why, int why_n)
{
    snprintf(why, (size_t)why_n, "not needed on the host (operations, no GE words)");
    return 1;
}
const uint32_t *w3d_be_global_template(int *n) { *n = 0; return NULL; }

void w3d_be_report(void)
{
    LOG("world3d host GE model: %llu chunk ranges, %llu triangles near-clipped by the GE (w3d_gnear), %llu lit vertices\n",
        (unsigned long long)n_ranges, (unsigned long long)n_nearclip, (unsigned long long)n_lit_verts);
    LOG("world3d host GE model: %llu atomics, %llu cached meshes drawn, %llu indexed + %llu clipped CPU triangles; %llu "
        "triangles: %llu drawn, "
        "%llu culled (back faces), %llu degenerate; GE rule violations (must be 0): %llu outside the 4096 window, %llu "
        "behind the near plane, %llu Y span >= 2730.625\n", (unsigned long long)n_atomics, (unsigned long long)n_meshes,
        (unsigned long long)n_idx_tris, (unsigned long long)n_cpu_tris, (unsigned long long)n_tris, (unsigned long long)n_drawn,
        (unsigned long long)n_culled, (unsigned long long)n_degenerate, (unsigned long long)n_out_window,
        (unsigned long long)n_behind, (unsigned long long)n_yspan);
}
#endif
