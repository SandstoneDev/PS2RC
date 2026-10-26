// psp_imgui.cpp - Dear ImGui on the PSP: pool allocator, baked font, pad input, GE renderer, PSP widgets.
// API and life cycle: psp_imgui.h. Build: psp-g++ -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics
//   -DIMGUI_USER_CONFIG='"pimgui_imconfig.h"' (README.md). PIMGUI_HOST: the same file for a host (tests): no GE code,
//   pimgui_render() only finishes the ImGui frame (the test rasterises ImGui::GetDrawData() itself).
//
// GE renderer: ImGui's vertices (pos, uv, col) are converted into the GE through-mode layout (float u, v in TEXELS,
// u32 ABGR colour = ImGui's IM_COL32 packing, float x, y, z) in list memory (sceGuGetMemory), indices copied next to
// them, one indexed GU_TRIANGLES draw per ImDrawCmd with its clip rectangle as the GE scissor (raw SCISSOR1/2 commands:
// the pspsdk sceGuScissor arguments differ between SDK versions). Textures: the font (T4 + 16-entry CLUT) or any
// PimguiTex. Vertices of one command are never shared with another (ImGui starts a new command for a new texture /
// clip rectangle), so a command with a non-font texture gets its own vertex range rescaled to its size.
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "psp_imgui.h"
#include "pimgui_font13.h"
#ifndef PIMGUI_HOST
#include <pspkernel.h>
#include <pspgu.h>
#endif

// ------------------------------------------------------------------ the pool allocator
// First fit over the whole pool, blocks in address order with boundary tags (size of this block and of the previous
// one), coalescing on free. ImGui's pattern (vectors that grow by doubling, a few hundred live blocks) needs nothing
// smarter; everything goes with the pool at pimgui_destroy().
namespace {
struct Blk { uint32_t size, prev, used, magic; };      // 16 bytes: payloads stay 16-byte aligned
const uint32_t kMagic = 0x50494D47u;                   // "PIMG"
uint8_t *g_pool, *g_pool_end;
uint32_t g_pool_used, g_pool_peak, g_allocs, g_failed;

inline Blk *blk_next(Blk *b) { return (Blk *)((uint8_t *)b + b->size); }

void pool_init(void *mem, uint32_t bytes)
{
    g_pool = (uint8_t *)(((uintptr_t)mem + 15u) & ~(uintptr_t)15u);
    g_pool_end = g_pool + ((bytes - (uint32_t)(g_pool - (uint8_t *)mem)) & ~15u);
    Blk *b = (Blk *)g_pool;
    b->size = (uint32_t)(g_pool_end - g_pool);
    b->prev = 0;
    b->used = 0;
    b->magic = kMagic;
    g_pool_used = g_pool_peak = g_allocs = g_failed = 0;
}

void *pool_alloc(size_t sz, void *)
{
    uint32_t need = (uint32_t)((sz + 15u) & ~(size_t)15u) + (uint32_t)sizeof(Blk);
    for (Blk *b = (Blk *)g_pool; (uint8_t *)b < g_pool_end; b = blk_next(b)) {
        if (b->used || b->size < need) continue;
        if (b->size - need >= 64u) {                   // split: the rest stays free
            Blk *r = (Blk *)((uint8_t *)b + need);
            r->size = b->size - need;
            r->prev = need;
            r->used = 0;
            r->magic = kMagic;
            Blk *n = blk_next(r);
            if ((uint8_t *)n < g_pool_end) n->prev = r->size;
            b->size = need;
        }
        b->used = 1;
        g_pool_used += b->size;
        if (g_pool_used > g_pool_peak) g_pool_peak = g_pool_used;
        g_allocs++;
        return b + 1;
    }
    g_failed++;
    return NULL;
}

void pool_free(void *p, void *)
{
    if (!p) return;
    Blk *b = (Blk *)p - 1;
    if (b->magic != kMagic || !b->used) return;        // not ours / double free: ignored
    b->used = 0;
    g_pool_used -= b->size;
    Blk *n = blk_next(b);
    if ((uint8_t *)n < g_pool_end && !n->used) {       // merge the next block
        b->size += n->size;
        n->magic = 0;
        n = blk_next(b);
        if ((uint8_t *)n < g_pool_end) n->prev = b->size;
    }
    if (b->prev) {                                     // merge into the previous block
        Blk *pv = (Blk *)((uint8_t *)b - b->prev);
        if (!pv->used) {
            pv->size += b->size;
            b->magic = 0;
            n = blk_next(pv);
            if ((uint8_t *)n < g_pool_end) n->prev = pv->size;
        }
    }
}

// ------------------------------------------------------------------ state
ImGuiContext *g_ctx;
int g_nav, g_scr_w = 480, g_scr_h = 272;
PimguiStats g_last;
unsigned g_clut[16] __attribute__((aligned(64)));
PimguiTex g_font;

// pad: held now / before, presses this frame, auto-repeat fires, consumed bits
unsigned g_now, g_prev, g_fire, g_consumed, g_stale;   // g_stale: held since before the UI, ignored until released
bool g_blocked;
float g_hold[16], g_next[16];
const float kRepDelay = 0.35f, kRepPeriod = 0.08f, kRepFast = 0.03f, kRepFastAfter = 1.5f;

void font_setup(ImFontAtlas *a)
{
    a->Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.SizePixels = PIMGUI_FONT_SIZE;
    cfg.PixelSnapH = true;
    ImStrncpy(cfg.Name, "ProggyClean 13 px (baked)", IM_ARRAYSIZE(cfg.Name));
    a->Sources.push_back(cfg);
    ImFont *f = IM_NEW(ImFont);
    f->FontSize = PIMGUI_FONT_SIZE;
    f->Ascent = PIMGUI_FONT_ASCENT;
    f->Descent = PIMGUI_FONT_DESCENT;
    f->ContainerAtlas = a;
    f->Sources = &a->Sources[0];
    f->SourcesCount = 1;
    a->Fonts.push_back(f);
    a->TexWidth = PIMGUI_FONT_TEX_W;
    a->TexHeight = PIMGUI_FONT_TEX_H;
    a->TexUvScale = ImVec2(1.0f / PIMGUI_FONT_TEX_W, 1.0f / PIMGUI_FONT_TEX_H);
    a->TexUvWhitePixel = ImVec2(PIMGUI_FONT_WHITE_U, PIMGUI_FONT_WHITE_V);
    for (int i = 0; i < PIMGUI_FONT_GLYPHS; i++) {
        const float *g = pimgui_font_glyphs[i];
        f->AddGlyph(NULL, (ImWchar)g[0], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[1]);
    }
    f->BuildLookupTable();
    a->TexReady = true;
    for (int i = 0; i < 16; i++) g_clut[i] = 0x00FFFFFFu | (unsigned)(i * 17) << 24;   // white, 16 alpha levels
    g_font.data = pimgui_font_t4;
    g_font.w = g_font.bufw = PIMGUI_FONT_TEX_W;
    g_font.h = PIMGUI_FONT_TEX_H;
    g_font.psm = 4;                                    // GU_PSM_T4
    g_font.linear = 0;
    g_font.clut = g_clut;
    g_font.clut_entries = 16;
    a->SetTexID((ImTextureID)(uintptr_t)&g_font);
#ifndef PIMGUI_HOST
    sceKernelDcacheWritebackRange(g_clut, sizeof g_clut);
    sceKernelDcacheWritebackRange((void *)pimgui_font_t4, sizeof pimgui_font_t4);
#endif
}

// a dark, flat theme for a 480x272 LCD: no rounding, thin frames, small paddings, no anti-aliasing (crisp + fewer
// vertices)
void style_setup(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(4, 3);
    s.FramePadding = ImVec2(4, 1);
    s.ItemSpacing = ImVec2(4, 2);
    s.ItemInnerSpacing = ImVec2(3, 2);
    s.CellPadding = ImVec2(3, 1);
    s.IndentSpacing = 10;
    s.ScrollbarSize = 5;
    s.GrabMinSize = 6;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = s.ScrollbarRounding = 0;
    s.GrabRounding = s.TabRounding = 0;
    s.AntiAliasedLines = s.AntiAliasedLinesUseTex = s.AntiAliasedFill = false;
    s.DisplaySafeAreaPadding = ImVec2(0, 0);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.55f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.90f);
    c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.97f);
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.45f, 0.70f, 0.80f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.20f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.30f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.50f, 0.75f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.36f, 0.66f, 1.00f);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.44f, 0.80f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.26f, 0.44f, 1.00f);
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.44f, 0.80f, 1.00f);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.42f, 0.78f, 1.00f);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_TabSelectedOverline] = ImVec4(1.00f, 0.75f, 0.25f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.40f, 0.55f, 0.60f);
    c[ImGuiCol_NavCursor] = ImVec4(1.00f, 0.75f, 0.25f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
}

// ------------------------------------------------------------------ GE renderer
#ifndef PIMGUI_HOST
struct GeV { float u, v; uint32_t c; float x, y, z; };     // GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
const int kVType = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_2D;

inline void scissor(int x0, int y0, int x1, int y1)        // [x0, x1) x [y0, y1)
{
    sceGuSendCommandi(212, (y0 << 10) | x0);               // SCISSOR1: start
    sceGuSendCommandi(213, ((y1 - 1) << 10) | (x1 - 1));   // SCISSOR2: end (inclusive)
}

void bind(const PimguiTex *t)
{
    sceGuTexMode(t->psm, 0, 0, 0);
    if (t->clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad((int)(t->clut_entries / 8u), t->clut);
    }
    sceGuTexImage(0, t->w, t->h, t->bufw, t->data);
    sceGuTexFilter(t->linear ? GU_LINEAR : GU_NEAREST, t->linear ? GU_LINEAR : GU_NEAREST);
    sceGuTexFlush();
}

void set_state(void)
{
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);                               // no depth writes
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_ALPHA_TEST);                            // fully transparent texels write nothing
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuEnable(GU_SCISSOR_TEST);
}

inline void conv(GeV *o, const ImDrawVert *s, int n, float tw, float th)
{
    for (int i = 0; i < n; i++) {
        o[i].u = s[i].uv.x * tw;
        o[i].v = s[i].uv.y * th;
        o[i].c = s[i].col;
        o[i].x = s[i].pos.x;
        o[i].y = s[i].pos.y;
        o[i].z = 0.0f;
    }
}

void render_draw_data(ImDrawData *dd)
{
    memset(&g_last.vertices, 0, sizeof(unsigned) * 4);
    if (!dd || dd->CmdListsCount == 0) return;
    set_state();
    const PimguiTex *bound = NULL;
    for (int l = 0; l < dd->CmdListsCount; l++) {
        const ImDrawList *cl = dd->CmdLists[l];
        int nv = cl->VtxBuffer.Size, ni = cl->IdxBuffer.Size;
        if (!nv || !ni) continue;
        unsigned vb = (unsigned)nv * sizeof(GeV), ib = ((unsigned)ni * 2u + 15u) & ~15u;
        GeV *v = (GeV *)sceGuGetMemory((int)vb);
        uint16_t *ix = (uint16_t *)sceGuGetMemory((int)ib);
        conv(v, cl->VtxBuffer.Data, nv, PIMGUI_FONT_TEX_W, PIMGUI_FONT_TEX_H);
        memcpy(ix, cl->IdxBuffer.Data, (size_t)ni * 2u);
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {     // other textures: their own vertex range, their size
            const ImDrawCmd *cmd = &cl->CmdBuffer[c];
            const PimguiTex *t = (const PimguiTex *)(uintptr_t)cmd->GetTexID();
            if (cmd->UserCallback || !t || t == &g_font || !cmd->ElemCount) continue;
            unsigned lo = 0xFFFFu, hi = 0;
            for (unsigned k = 0; k < cmd->ElemCount; k++) {
                unsigned i = cl->IdxBuffer.Data[cmd->IdxOffset + k];
                if (i < lo) lo = i;
                if (i > hi) hi = i;
            }
            conv(v + cmd->VtxOffset + lo, cl->VtxBuffer.Data + cmd->VtxOffset + lo, (int)(hi - lo + 1), t->w, t->h);
        }
        if (!((uintptr_t)v & 0x40000000u)) {               // a cached list: the GE reads RAM
            sceKernelDcacheWritebackRange(v, vb);
            sceKernelDcacheWritebackRange(ix, ib);
        }
        g_last.vertices += (unsigned)nv;
        g_last.indices += (unsigned)ni;
        g_last.list_bytes += vb + ib;
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd *cmd = &cl->CmdBuffer[c];
            if (cmd->UserCallback) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) { set_state(); bound = NULL; }
                else cmd->UserCallback(cl, cmd);
                continue;
            }
            if (!cmd->ElemCount) continue;
            float fx0 = cmd->ClipRect.x - dd->DisplayPos.x, fy0 = cmd->ClipRect.y - dd->DisplayPos.y;
            float fx1 = cmd->ClipRect.z - dd->DisplayPos.x, fy1 = cmd->ClipRect.w - dd->DisplayPos.y;
            int x0 = fx0 < 0 ? 0 : (int)fx0, y0 = fy0 < 0 ? 0 : (int)fy0;
            int x1 = fx1 > g_scr_w ? g_scr_w : (int)(fx1 + 0.999f), y1 = fy1 > g_scr_h ? g_scr_h : (int)(fy1 + 0.999f);
            if (x1 <= x0 || y1 <= y0) continue;
            scissor(x0, y0, x1, y1);
            const PimguiTex *t = (const PimguiTex *)(uintptr_t)cmd->GetTexID();
            if (!t) t = &g_font;
            if (t != bound) { bind(t); bound = t; }
            sceGuDrawArray(GU_TRIANGLES, kVType, (int)cmd->ElemCount, ix + cmd->IdxOffset, v + cmd->VtxOffset);
            g_last.commands++;
        }
    }
    scissor(0, 0, g_scr_w, g_scr_h);
}
#endif

// ------------------------------------------------------------------ rows state
struct Rows {
    ImGuiID id, last_id;
    int *focus;
    int count, last_count, prev_focus;
    double t0;
    bool changed;
};
Rows R;

unsigned col_u32(ImGuiCol c) { return ImGui::GetColorU32(c); }
const unsigned kAccent = IM_COL32(255, 190, 64, 255);
}   // namespace

// ------------------------------------------------------------------ C API: life cycle
extern "C" int pimgui_create(void *pool, unsigned pool_bytes, int nav_mode)
{
    if (g_ctx || !pool || pool_bytes < 64u * 1024u) return 0;
    pool_init(pool, pool_bytes);
    ImGui::SetAllocatorFunctions(pool_alloc, pool_free, NULL);
    g_ctx = ImGui::CreateContext();
    if (!g_ctx) return 0;
    g_nav = nav_mode;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.DisplaySize = ImVec2((float)g_scr_w, (float)g_scr_h);
    io.BackendPlatformName = io.BackendRendererName = "psp_imgui";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    if (nav_mode == PIMGUI_NAV_IMGUI) {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.MouseDrawCursor = false;
    font_setup(io.Fonts);
    style_setup();
    g_now = g_prev = g_fire = g_consumed = g_stale = 0;
    memset(&R, 0, sizeof R);
    R.prev_focus = -1;
    return 1;
}

extern "C" void pimgui_destroy(void)
{
    if (!g_ctx) return;
    ImGui::DestroyContext(g_ctx);
    g_ctx = NULL;
}

extern "C" int pimgui_exists(void) { return g_ctx != NULL; }
extern "C" void pimgui_set_screen(int w, int h)
{
    g_scr_w = w;
    g_scr_h = h;
    if (g_ctx) ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
}
extern "C" const PimguiTex *pimgui_font_tex(void) { return &g_font; }

extern "C" void pimgui_stats(PimguiStats *st)
{
    *st = g_last;
    st->pool_bytes = (unsigned)(g_pool_end - g_pool);
    st->pool_used = g_pool_used;
    st->pool_peak = g_pool_peak;
    st->allocs = g_allocs;
    st->failed = g_failed;
}

extern "C" void pimgui_new_frame(unsigned buttons, float dt)
{
    if (!g_ctx) return;
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
    g_prev = g_now;
    g_now = buttons & 0xFFFFu;
    g_stale &= g_now;
    g_consumed = 0;
    g_blocked = false;
    g_fire = 0;
    for (int i = 0; i < 16; i++) {
        unsigned b = 1u << i;
        if (!(g_now & b)) { g_hold[i] = 0; continue; }
        if (g_stale & b) continue;                        // held when the UI opened: no press, no repeat
        if (!(g_prev & b)) { g_hold[i] = 0; g_next[i] = kRepDelay; g_fire |= b; continue; }
        g_hold[i] += dt;
        if (g_hold[i] >= g_next[i]) {
            g_fire |= b;
            g_next[i] += g_hold[i] >= kRepFastAfter ? kRepFast : kRepPeriod;
            if (g_next[i] < g_hold[i]) g_next[i] = g_hold[i];   // a long frame: no burst
        }
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = dt;
    io.DisplaySize = ImVec2((float)g_scr_w, (float)g_scr_h);
    if (g_nav == PIMGUI_NAV_IMGUI) {
        static const struct { unsigned b; ImGuiKey k; } map[] = {
            {PIMGUI_UP, ImGuiKey_GamepadDpadUp}, {PIMGUI_DOWN, ImGuiKey_GamepadDpadDown},
            {PIMGUI_LEFT, ImGuiKey_GamepadDpadLeft}, {PIMGUI_RIGHT, ImGuiKey_GamepadDpadRight},
            {PIMGUI_CROSS, ImGuiKey_GamepadFaceDown}, {PIMGUI_CIRCLE, ImGuiKey_GamepadFaceRight},
            {PIMGUI_TRIANGLE, ImGuiKey_GamepadFaceUp}, {PIMGUI_SQUARE, ImGuiKey_GamepadFaceLeft},
            {PIMGUI_L, ImGuiKey_GamepadL1}, {PIMGUI_R, ImGuiKey_GamepadR1},
            {PIMGUI_START, ImGuiKey_GamepadStart}, {PIMGUI_SELECT, ImGuiKey_GamepadBack}};
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if ((g_now ^ g_prev) & map[i].b) io.AddKeyEvent(map[i].k, (g_now & map[i].b) != 0);
    }
    ImGui::NewFrame();
}

extern "C" void pimgui_render(void)
{
    if (!g_ctx) return;
    ImGui::Render();
#ifndef PIMGUI_HOST
    render_draw_data(ImGui::GetDrawData());
#else
    ImDrawData *dd = ImGui::GetDrawData();
    memset(&g_last.vertices, 0, sizeof(unsigned) * 4);
    for (int l = 0; dd && l < dd->CmdListsCount; l++) {
        g_last.vertices += (unsigned)dd->CmdLists[l]->VtxBuffer.Size;
        g_last.indices += (unsigned)dd->CmdLists[l]->IdxBuffer.Size;
        g_last.commands += (unsigned)dd->CmdLists[l]->CmdBuffer.Size;
        g_last.list_bytes += (unsigned)dd->CmdLists[l]->VtxBuffer.Size * 24u + (unsigned)dd->CmdLists[l]->IdxBuffer.Size * 2u;
    }
#endif
}

// ------------------------------------------------------------------ pad
extern "C" int pimgui_pressed(unsigned m) { return !g_blocked && (g_now & ~g_prev & m & ~g_consumed & ~g_stale) != 0; }
extern "C" int pimgui_repeat(unsigned m) { return !g_blocked && (g_fire & m & ~g_consumed) != 0; }
extern "C" void pimgui_block_input(int on) { g_blocked = on != 0; }
extern "C" int pimgui_held(unsigned m) { return (g_now & m) == m; }
extern "C" void pimgui_consume(unsigned m) { g_consumed |= m; }
extern "C" void pimgui_held_at_start(unsigned b) { g_now = g_stale = b & 0xFFFFu; }

// ------------------------------------------------------------------ minimal C layout
extern "C" void pimgui_window_begin(const char *id, float x, float y, float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                           (g_nav == PIMGUI_NAV_ROWS ? ImGuiWindowFlags_NoNav : 0));
}
extern "C" void pimgui_window_end(void) { ImGui::End(); }
extern "C" void pimgui_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}
extern "C" void pimgui_text_colored(unsigned color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
extern "C" void pimgui_separator(void) { ImGui::Separator(); }
extern "C" void pimgui_same_line(void) { ImGui::SameLine(); }

// ------------------------------------------------------------------ widgets: button icons
extern "C" void pimgui_icon(unsigned b)
{
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return;
    float lh = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = lh - 2.0f, cx = p.x + s * 0.5f + 1.0f, cy = p.y + lh * 0.5f;
    const char *txt = b == PIMGUI_L ? "L" : b == PIMGUI_R ? "R" : b == PIMGUI_START ? "START" : b == PIMGUI_SELECT ? "SELECT" : NULL;
    float wdt = s + 2.0f;
    if (txt) wdt = ImGui::CalcTextSize(txt).x + 6.0f;
    ImRect bb(p, ImVec2(p.x + wdt, p.y + lh));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    ImDrawList *dl = w->DrawList;
    float r = s * 0.5f;
    switch (b) {
    case PIMGUI_CROSS:
        dl->AddLine(ImVec2(cx - r + 1, cy - r + 1), ImVec2(cx + r - 1, cy + r - 1), IM_COL32(120, 170, 255, 255), 2.0f);
        dl->AddLine(ImVec2(cx - r + 1, cy + r - 1), ImVec2(cx + r - 1, cy - r + 1), IM_COL32(120, 170, 255, 255), 2.0f);
        break;
    case PIMGUI_CIRCLE: dl->AddCircle(ImVec2(cx, cy), r - 0.5f, IM_COL32(255, 105, 105, 255), 12, 2.0f); break;
    case PIMGUI_SQUARE:
        dl->AddRect(ImVec2(cx - r + 1, cy - r + 1), ImVec2(cx + r - 1, cy + r - 1), IM_COL32(240, 130, 220, 255), 0, 0, 2.0f);
        break;
    case PIMGUI_TRIANGLE:
        dl->AddTriangle(ImVec2(cx, cy - r + 1), ImVec2(cx + r, cy + r - 1), ImVec2(cx - r, cy + r - 1),
                        IM_COL32(90, 220, 170, 255), 2.0f);
        break;
    case PIMGUI_UP: case PIMGUI_DOWN: case PIMGUI_LEFT: case PIMGUI_RIGHT: case PIMGUI_UP | PIMGUI_DOWN:
    case PIMGUI_LEFT | PIMGUI_RIGHT: case PIMGUI_UP | PIMGUI_DOWN | PIMGUI_LEFT | PIMGUI_RIGHT: {
        unsigned dim = IM_COL32(110, 115, 125, 255), on = IM_COL32(240, 240, 240, 255);
        float a = r / 3.0f;
        dl->AddRectFilled(ImVec2(cx - a, cy - r), ImVec2(cx + a, cy + r), dim);
        dl->AddRectFilled(ImVec2(cx - r, cy - a), ImVec2(cx + r, cy + a), dim);
        if (b & PIMGUI_UP) dl->AddRectFilled(ImVec2(cx - a, cy - r), ImVec2(cx + a, cy - a), on);
        if (b & PIMGUI_DOWN) dl->AddRectFilled(ImVec2(cx - a, cy + a), ImVec2(cx + a, cy + r), on);
        if (b & PIMGUI_LEFT) dl->AddRectFilled(ImVec2(cx - r, cy - a), ImVec2(cx - a, cy + a), on);
        if (b & PIMGUI_RIGHT) dl->AddRectFilled(ImVec2(cx + a, cy - a), ImVec2(cx + r, cy + a), on);
        break;
    }
    default:
        if (txt) {
            dl->AddRect(ImVec2(p.x, p.y + 1), ImVec2(p.x + wdt - 1, p.y + lh - 1), IM_COL32(200, 200, 210, 255));
            dl->AddText(ImVec2(p.x + 3, p.y), IM_COL32(230, 230, 235, 255), txt);
        }
        break;
    }
}

extern "C" void pimgui_hint(unsigned b, const char *label)
{
    pimgui_icon(b);
    ImGui::SameLine(0, 2);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 9);
}

// ------------------------------------------------------------------ widgets: marquee
// a derma-style marquee as in an earlier PSP UI port (ping-pong, a pause at each end, linear, whole pixels): hold the
// start for PIMGUI_MARQUEE_HOLD_S, slide left until the end of the text is visible, hold, slide back, repeat
extern "C" float pimgui_marquee_offset(float text_w, float box_w, double t)
{
    float over = text_w - box_w;
    if (over <= 0.0f || t < 0.0) return 0.0f;
    double span = over / PIMGUI_MARQUEE_PX_S, cycle = 2.0 * (PIMGUI_MARQUEE_HOLD_S + span);
    double x = fmod(t, cycle);
    if (x < PIMGUI_MARQUEE_HOLD_S) return 0.0f;
    x -= PIMGUI_MARQUEE_HOLD_S;
    if (x < span) return (float)floor(x * PIMGUI_MARQUEE_PX_S + 1e-4);
    x -= span;
    if (x < PIMGUI_MARQUEE_HOLD_S) return over;
    x -= PIMGUI_MARQUEE_HOLD_S;
    return (float)ceil(over - x * PIMGUI_MARQUEE_PX_S - 1e-4);
}

static void marquee_at(ImDrawList *dl, ImVec2 p, float w, float lh, const char *text, int active, double t, unsigned col)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    if (ts.x <= w) { dl->AddText(p, col, text); return; }
    if (active) {
        float off = pimgui_marquee_offset(ts.x, w, t);
        dl->PushClipRect(p, ImVec2(p.x + w, p.y + lh), true);
        dl->AddText(ImVec2(p.x - off, p.y), col, text);
        dl->PopClipRect();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, p, ImVec2(p.x + w, p.y + lh), p.x + w, p.x + w, text, NULL, &ts);
    ImGui::PopStyleColor();
}

extern "C" void pimgui_marquee(const char *text, float w, int active, double t, unsigned color)
{
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return;
    float lh = ImGui::GetTextLineHeight();
    if (w <= 0.0f) w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImRect bb(p, ImVec2(p.x + w, p.y + lh));
    ImGui::ItemSize(bb.GetSize());
    if (!ImGui::ItemAdd(bb, 0)) return;
    marquee_at(win->DrawList, p, w, lh, text, active, t, color ? color : col_u32(ImGuiCol_Text));
}

// ------------------------------------------------------------------ widgets: row lists
extern "C" void pimgui_rows_begin(const char *id, int *focus, float height)
{
    ImGuiID gid = ImGui::GetID(id);
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    R.id = gid;
    R.focus = focus;
    R.count = 0;
    if (gid == R.last_id && R.last_count > 0) {        // Up / Down over last frame's rows
        int n = R.last_count;
        if (pimgui_repeat(PIMGUI_UP)) { *focus = (*focus - 1 + n) % n; pimgui_consume(PIMGUI_UP); }
        else if (pimgui_repeat(PIMGUI_DOWN)) { *focus = (*focus + 1) % n; pimgui_consume(PIMGUI_DOWN); }
    }
    R.changed = gid != R.last_id || *focus != R.prev_focus;
    if (R.changed) R.t0 = ImGui::GetTime();
    R.prev_focus = *focus;
}

extern "C" int pimgui_rows_focus_changed(void) { return R.changed; }
extern "C" double pimgui_rows_focus_time(void) { return ImGui::GetTime() - R.t0; }

extern "C" int pimgui_row(const char *label, const char *value, float frac, int steppable, const char *badge, int dim)
{
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    int idx = R.count++;
    bool focused = R.focus && idx == *R.focus;
    float lh = ImGui::GetTextLineHeight(), h = lh + 5.0f;      // text + a 2-pixel value bar under it
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImRect bb(p, ImVec2(p.x + w, p.y + h));
    ImGui::ItemSize(bb.GetSize());
    bool vis = ImGui::ItemAdd(bb, 0);
    int act = PIMGUI_ROW_NONE;
    if (focused) {
        if (R.changed || bb.Min.y < win->ClipRect.Min.y || bb.Max.y > win->ClipRect.Max.y) {   // keep it in view
            if (bb.Min.y < win->ClipRect.Min.y + h) ImGui::SetScrollHereY(0.0f);
            else if (bb.Max.y > win->ClipRect.Max.y - h) ImGui::SetScrollHereY(1.0f);
        }
        if (!dim) {
            if (pimgui_repeat(PIMGUI_LEFT)) act = PIMGUI_ROW_LEFT;
            else if (pimgui_repeat(PIMGUI_RIGHT)) act = PIMGUI_ROW_RIGHT;
            else if (pimgui_pressed(PIMGUI_CROSS)) act = PIMGUI_ROW_ACTIVATE;
            pimgui_consume(PIMGUI_LEFT | PIMGUI_RIGHT | PIMGUI_CROSS);
        }
    }
    if (!vis) return act;
    ImDrawList *dl = win->DrawList;
    unsigned tcol = dim ? col_u32(ImGuiCol_TextDisabled) : col_u32(ImGuiCol_Text);
    if (focused) {
        dl->AddRectFilled(bb.Min, bb.Max, col_u32(ImGuiCol_Header));
        dl->AddRectFilled(bb.Min, ImVec2(bb.Min.x + 2, bb.Max.y), kAccent);
    } else if (idx & 1)
        dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(255, 255, 255, 10));
    float ty = p.y + 1.0f, x = p.x + 5.0f;
    float bw = 12.0f;
    if (badge && *badge) {
        ImVec2 bs = ImGui::CalcTextSize(badge);
        if (bs.x + 2 > bw) bw = bs.x + 2;
        dl->AddText(ImVec2(x, ty), kAccent, badge);
    }
    x += bw;
    float vw = 0.0f;
    if (value) {
        vw = w * 0.36f;
        if (vw < 110.0f) vw = 110.0f;
        if (vw > 180.0f) vw = 180.0f;
    }
    float lw = p.x + w - vw - 6.0f - x;
    marquee_at(dl, ImVec2(x, ty), lw, lh, label, focused, focused ? ImGui::GetTime() - R.t0 : 0.0, tcol);
    if (value) {
        float vx0 = p.x + w - vw - 2.0f, vx1 = p.x + w - 4.0f;
        if (frac >= 0.0f) {
            float f = frac > 1.0f ? 1.0f : frac;
            dl->AddRectFilled(ImVec2(vx0 + 9, bb.Max.y - 2), ImVec2(vx1 - 9, bb.Max.y), IM_COL32(255, 255, 255, 40));
            dl->AddRectFilled(ImVec2(vx0 + 9, bb.Max.y - 2), ImVec2(vx0 + 9 + (vx1 - vx0 - 18) * f, bb.Max.y),
                              focused ? kAccent : IM_COL32(110, 150, 220, 255));
        }
        if (steppable) {
            unsigned ac = focused ? kAccent : IM_COL32(150, 155, 165, 160);
            dl->AddText(ImVec2(vx0, ty), ac, "<");
            dl->AddText(ImVec2(vx1 - 7, ty), ac, ">");
        }
        float inner = vx1 - vx0 - 22.0f;
        ImVec2 vs = ImGui::CalcTextSize(value);
        float vx = vs.x < inner ? vx0 + 11.0f + (inner - vs.x) * 0.5f : vx0 + 11.0f;
        marquee_at(dl, ImVec2(ImFloor(vx), ty), inner, lh, value, focused, focused ? ImGui::GetTime() - R.t0 : 0.0, tcol);
    }
    return act;
}

extern "C" int pimgui_row_int(const char *label, int *v, int lo, int hi, int step, const char *fmt)
{
    char buf[32];
    ImFormatString(buf, sizeof buf, fmt ? fmt : "%d", *v);
    float f = hi > lo ? (float)(*v - lo) / (float)(hi - lo) : 0.0f;
    int a = pimgui_row(label, buf, f, 1, NULL, 0), old = *v;
    if (a == PIMGUI_ROW_LEFT) *v = *v - step < lo ? lo : *v - step;
    else if (a == PIMGUI_ROW_RIGHT) *v = *v + step > hi ? hi : *v + step;
    return *v != old;
}

extern "C" int pimgui_row_choice(const char *label, int *idx, const char *const *items, int n)
{
    int old = *idx;
    int a = pimgui_row(label, *idx >= 0 && *idx < n ? items[*idx] : "?", -1.0f, 1, NULL, 0);
    if (n <= 0) return 0;
    if (a == PIMGUI_ROW_LEFT) *idx = (*idx - 1 + n) % n;
    else if (a == PIMGUI_ROW_RIGHT || a == PIMGUI_ROW_ACTIVATE) *idx = (*idx + 1) % n;
    return *idx != old;
}

extern "C" int pimgui_row_bool(const char *label, int *v)
{
    int a = pimgui_row(label, *v ? "On" : "Off", -1.0f, 1, NULL, 0);
    if (a != PIMGUI_ROW_NONE) { *v = !*v; return 1; }
    return 0;
}

extern "C" int pimgui_row_button(const char *label)
{
    return pimgui_row(label, NULL, -1.0f, 0, NULL, 0) == PIMGUI_ROW_ACTIVATE;
}

extern "C" void pimgui_rows_end(void)
{
    R.last_id = R.id;
    R.last_count = R.count;
    if (R.focus && R.count > 0 && *R.focus >= R.count) *R.focus = R.count - 1;
    if (R.focus && *R.focus < 0) *R.focus = 0;
    ImGui::EndChild();
}
