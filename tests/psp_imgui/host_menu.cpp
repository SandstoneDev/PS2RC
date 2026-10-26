// host_menu.cpp - host harness for lib/psp_imgui + the in-game settings menu (hle/psp/hle_menu.cpp).
// Runs the real menu code (the real cfg reader HLE_CFG_DIR, the game pack's registry, the real save rules) with a
// scripted pad, rasterises ImGui's draw data in software into 480x272 PPM pictures (the font texture as the GE sees
// it: T4 + CLUT, nearest, through mode, scissor), so the layout can be checked without an emulator.
//   host_menu <out dir> <script> [pool KB]    -> <out>/shot_<n>.ppm, <out>/log.txt; cfg files rewritten in HLE_CFG_DIR
//   host_menu --marquee                        -> the marquee offset table
//   host_menu --pool                           -> allocator self-test
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "imgui.h"
#include "psp_imgui.h"
extern "C" {
#include "hle_cfg.h"
#include "hle_menu.h"
int hle_x_gen;
int hle_x_set(const char *name, int val) { (void)name; (void)val; hle_x_gen++; return 1; }   // the live x_ switches
int hle_tune_set(const char *k, const char *v) { (void)v; return !strcmp(k, "far") || !strcmp(k, "fog") || !strcmp(k, "fogmax") || !strcmp(k, "lod") || !strcmp(k, "drivefar"); }
}

static const char *g_out, *g_dir;
static unsigned char fb[272][480][3];
static int g_frames;
static FILE *g_log;

static void bg(void)                                  // a stand-in for the frozen game picture
{
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            fb[y][x][0] = (unsigned char)(40 + x / 6);
            fb[y][x][1] = (unsigned char)(70 + y / 4);
            fb[y][x][2] = (unsigned char)(((x / 32 + y / 32) & 1) ? 120 : 90);
        }
}

struct V { float x, y, u, v; unsigned c; };

static void tri(const V &a, const V &b, const V &c, const PimguiTex *t, int cx0, int cy0, int cx1, int cy1)
{
    float minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    float maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    float miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    float maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    int x0 = (int)minx, x1 = (int)maxx + 1, y0 = (int)miny, y1 = (int)maxy + 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float u = (w0 * a.u + w1 * b.u + w2 * c.u) * t->w, v = (w0 * a.v + w1 * b.v + w2 * c.v) * t->h;
            int tu = (int)u, tv = (int)v;
            if (tu < 0) tu = 0;
            if (tv < 0) tv = 0;
            if (tu >= t->w) tu = t->w - 1;
            if (tv >= t->h) tv = t->h - 1;
            unsigned texel = 0xFFFFFFFFu;
            if (t->psm == 4) {
                unsigned char byte = ((const unsigned char *)t->data)[(tv * t->bufw + tu) / 2];
                texel = t->clut[tu & 1 ? byte >> 4 : byte & 15];
            }
            float cr = 0, cg = 0, cb = 0, ca = 0;
            const V *vs[3] = {&a, &b, &c};
            float ws[3] = {w0, w1, w2};
            for (int k = 0; k < 3; k++) {
                cr += ws[k] * (vs[k]->c & 255);
                cg += ws[k] * (vs[k]->c >> 8 & 255);
                cb += ws[k] * (vs[k]->c >> 16 & 255);
                ca += ws[k] * (vs[k]->c >> 24);
            }
            float r = cr * (texel & 255) / 255.0f, g = cg * (texel >> 8 & 255) / 255.0f;
            float bl = cb * (texel >> 16 & 255) / 255.0f, al = ca * (texel >> 24) / 255.0f / 255.0f;
            if (al <= 0) continue;                    // the GE's alpha test GREATER 0
            unsigned char *d = fb[y][x];
            d[0] = (unsigned char)(r * al + d[0] * (1 - al));
            d[1] = (unsigned char)(g * al + d[1] * (1 - al));
            d[2] = (unsigned char)(bl * al + d[2] * (1 - al));
        }
}

static void raster(void)
{
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd) return;
    for (int l = 0; l < dd->CmdListsCount; l++) {
        const ImDrawList *cl = dd->CmdLists[l];
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd &cmd = cl->CmdBuffer[c];
            if (cmd.UserCallback) continue;
            const PimguiTex *t = (const PimguiTex *)(uintptr_t)cmd.GetTexID();
            if (!t) t = pimgui_font_tex();
            int cx0 = (int)cmd.ClipRect.x, cy0 = (int)cmd.ClipRect.y;
            int cx1 = (int)(cmd.ClipRect.z + 0.999f), cy1 = (int)(cmd.ClipRect.w + 0.999f);
            if (cx0 < 0) cx0 = 0;
            if (cy0 < 0) cy0 = 0;
            if (cx1 > 480) cx1 = 480;
            if (cy1 > 272) cy1 = 272;
            for (unsigned k = 0; k + 2 < cmd.ElemCount; k += 3) {
                V v[3];
                for (int j = 0; j < 3; j++) {
                    const ImDrawVert &s = cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + k + j]];
                    v[j].x = s.pos.x; v[j].y = s.pos.y; v[j].u = s.uv.x; v[j].v = s.uv.y; v[j].c = s.col;
                }
                tri(v[0], v[1], v[2], t, cx0, cy0, cx1, cy1);
            }
        }
    }
}

static unsigned h_buttons(void) { return 0; }
static int h_begin(void) { bg(); return 1; }
static void h_end(int shot)
{
    g_frames++;
    if (shot <= 0) return;
    raster();
    char p[512];
    snprintf(p, sizeof p, "%s/shot_%02d.ppm", g_out, shot);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6\n480 272\n255\n");
    fwrite(fb, 1, sizeof fb, f);
    fclose(f);
}
static double h_seconds(void) { return g_frames / 60.0; }
static void path_of(int dev, char *p, size_t n) { snprintf(p, n, "%s/%s", g_dir, hle_cfg_file(dev)); }
static int h_read(int dev, char *buf, int cap)
{
    char p[512];
    path_of(dev, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}
static int h_write(int dev, const char *buf, int n)
{
    char p[512];
    path_of(dev, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    return 0;
}
static void h_log(const char *s) { fprintf(g_log, "%s\n", s); }
static void h_idle(void) {}
static void *h_tmp_alloc(unsigned n) { return malloc(n); }
static void h_tmp_free(void *p) { free(p); }

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--marquee")) {
        const double ts[] = {0, 0.5, 0.8, 1.0, 1.8, 2.3, 2.6, 3.0, 3.6, 4.0, 4.9, 5.2};
        for (double t : ts) printf("%.1f %.0f\n", t, pimgui_marquee_offset(130.0f, 100.0f, t));
        printf("fit %.0f\n", pimgui_marquee_offset(90.0f, 100.0f, 3.0));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--pool")) {
        // the allocator through ImGui's own alloc / free: random sizes, all freed again -> nothing in use
        static char pool[256 * 1024];
        if (!pimgui_create(pool, sizeof pool, PIMGUI_NAV_ROWS)) { printf("create failed\n"); return 1; }
        PimguiStats s0;
        pimgui_stats(&s0);
        std::vector<void *> p;
        unsigned seed = 1;
        for (int i = 0; i < 4000; i++) {
            seed = seed * 1103515245u + 12345u;
            if (p.size() < 40 || ((seed >> 16) % 3 && p.size() < 70)) {
                void *q = ImGui::MemAlloc(1 + (seed >> 8) % 3000);
                if (q) { memset(q, 0xAB, 1); p.push_back(q); }
            } else {
                size_t k = (seed >> 4) % p.size();
                ImGui::MemFree(p[k]);
                p.erase(p.begin() + (long)k);
            }
        }
        for (void *q : p) ImGui::MemFree(q);
        PimguiStats s1;
        pimgui_stats(&s1);
        printf("pool used before %u after %u peak %u failed %u\n", s0.pool_used, s1.pool_used, s1.pool_peak, s1.failed);
        pimgui_destroy();
        return 0;
    }
    if (argc < 3) return 2;
    g_out = argv[1];
    g_dir = getenv("HLE_CFG_DIR") ? getenv("HLE_CFG_DIR") : ".";
    unsigned kb = argc > 3 ? (unsigned)atoi(argv[3]) : 320;
    char lp[512];
    snprintf(lp, sizeof lp, "%s/log.txt", g_out);
    g_log = fopen(lp, "w");
    std::vector<char> pool(kb * 1024);
    static const HleMenuHost host = {h_buttons, h_begin, h_end, h_seconds, h_read, h_write, h_log, h_idle, h_tmp_alloc,
                                     h_tmp_free};
    HleMenuResult r;
    int rc = hle_menu_ui(&host, pool.data(), (unsigned)pool.size(), argv[2], &r);
    fprintf(g_log, "result rc=%d frames=%d changes=%d live=%d restart=%d saves=%d pool_peak=%u pool=%u failed=%u "
            "max_vertices=%u max_list=%u\n", rc, r.frames, r.changes, r.live, r.restart, r.saves, r.pool_peak,
            r.pool_bytes, r.failed_allocs, r.max_vertices, r.max_list_bytes);
    fclose(g_log);
    return rc < 0;
}
