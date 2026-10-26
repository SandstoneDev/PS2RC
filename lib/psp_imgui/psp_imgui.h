/* psp_imgui.h - Dear ImGui on the PSP: GE (sceGu) renderer, PSP pad input, a pool allocator, a baked font, and
 * PSP-friendly widgets (row lists driven by the D-pad, marquee text, button icons). C API; C++ code may also call
 * ImGui:: directly between pimgui_new_frame() and pimgui_render().
 *
 * Life cycle (all on one thread, the one that owns the GE list):
 *     static char pool[256 * 1024] __attribute__((aligned(16)));   (or a partition block: see README)
 *     pimgui_create(pool, sizeof pool, PIMGUI_NAV_ROWS);
 *     each frame:  sceGuStart(...) + your draw buffer;  pimgui_new_frame(pad.Buttons, 1/60.f);
 *                  ... widgets ...;  pimgui_render();  sceGuFinish(); sceGuSync(0, 0); swap
 *     pimgui_destroy();          (the pool can be freed / reused afterwards: nothing else is kept)
 * Nothing runs and nothing is allocated while no context exists: an application pays only the code size.
 *
 * Text that does not fit is never scaled down (unreadable at 480x272): pimgui_marquee() scrolls it.
 * Licence: the psp_imgui files - see LICENSE.md; Dear ImGui (third_party/imgui/) - MIT, (c) Omar Cornut.
 */
#ifndef PSP_IMGUI_H
#define PSP_IMGUI_H

#ifdef __cplusplus
extern "C" {
#endif

/* PSP buttons: the pspctrl.h PSP_CTRL_* bits */
#define PIMGUI_SELECT   0x0001u
#define PIMGUI_START    0x0008u
#define PIMGUI_UP       0x0010u
#define PIMGUI_RIGHT    0x0020u
#define PIMGUI_DOWN     0x0040u
#define PIMGUI_LEFT     0x0080u
#define PIMGUI_L        0x0100u
#define PIMGUI_R        0x0200u
#define PIMGUI_TRIANGLE 0x1000u
#define PIMGUI_CIRCLE   0x2000u
#define PIMGUI_CROSS    0x4000u
#define PIMGUI_SQUARE   0x8000u

/* navigation: ROWS = the pad belongs to the pimgui_row* lists and the application (ImGui's own keyboard / gamepad
   navigation stays off); IMGUI = the pad drives ImGui's gamepad navigation (Cross = activate, Circle = cancel,
   Triangle = menu, Square = windowing, D-pad = move, L / R = tweak slow / fast) for ordinary ImGui widgets */
enum { PIMGUI_NAV_ROWS = 0, PIMGUI_NAV_IMGUI = 1 };

typedef struct {
    unsigned pool_bytes, pool_used, pool_peak;   /* the allocator (bytes) */
    unsigned allocs, failed;                     /* allocations so far, refused ones (pool full) */
    unsigned vertices, indices, commands;        /* the last pimgui_render() */
    unsigned list_bytes;                         /* GE list memory the last frame used (vertices + indices) */
} PimguiStats;

/* a texture for ImGui::Image / ImDrawList (ImTextureID = (ImTextureID)(uintptr_t)&tex): GE pixel format (GU_PSM_*),
   size (powers of two), buffer width, 16-byte aligned data in RAM or VRAM; T4 / T8 also need a CLUT (8888, 16-byte
   aligned, entries = 16 / 256); data must be written back from the dcache by the caller */
typedef struct {
    const void *data;
    unsigned short w, h, bufw;
    unsigned char psm, linear;                   /* linear: 1 = GU_LINEAR filter */
    const unsigned *clut;
    unsigned clut_entries;
} PimguiTex;

int  pimgui_create(void *pool, unsigned pool_bytes, int nav_mode);   /* 1 = ok; 0 = pool too small / already made */
void pimgui_destroy(void);
int  pimgui_exists(void);
/* buttons = PSP buttons held now (PSP_CTRL bits); dt = seconds since the previous frame */
void pimgui_new_frame(unsigned buttons, float dt);
/* ImGui::Render() + GE commands appended to the list being built (sceGuStart done, draw buffer / offset set by the
   caller, through mode). Leaves: scissor = the full screen, blending on, texture on, depth test off. */
void pimgui_render(void);
void pimgui_stats(PimguiStats *st);
const PimguiTex *pimgui_font_tex(void);
void pimgui_set_screen(int w, int h);            /* default 480 x 272 */

/* ------------------------------------------------------------------ pad (valid after pimgui_new_frame) */
int  pimgui_pressed(unsigned mask);   /* any of mask went down this frame */
int  pimgui_repeat(unsigned mask);    /* went down, or held: repeats after 0.35 s every 0.08 s (0.03 s after 1.5 s) */
int  pimgui_held(unsigned mask);      /* all of mask held */
void pimgui_consume(unsigned mask);   /* pressed / repeat answer 0 for these for the rest of the frame */
void pimgui_held_at_start(unsigned buttons);   /* held when the UI opens (its opening combination): no press / repeat until released */
void pimgui_block_input(int on);      /* 1: pressed / repeat answer 0 until 0 (a popup drawn later gets the keys) */

/* ------------------------------------------------------------------ minimal C layout (C++ code uses ImGui:: directly) */
void pimgui_window_begin(const char *id, float x, float y, float w, float h);   /* a fixed window without decorations */
void pimgui_window_end(void);
void pimgui_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pimgui_text_colored(unsigned color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pimgui_separator(void);
void pimgui_same_line(void);
#define PIMGUI_RGBA(r, g, b, a) ((unsigned)(r) | (unsigned)(g) << 8 | (unsigned)(b) << 16 | (unsigned)(a) << 24)

/* ------------------------------------------------------------------ widgets */
/* the PSP button symbol (Cross / Circle / Square / Triangle / L / R / START / SELECT / D-pad) drawn inline, one text
   line high; pimgui_hint = symbol + label, on the same line as the previous hint */
void pimgui_icon(unsigned button);
void pimgui_hint(unsigned button, const char *label);

/* text in a box w wide (0 = to the end of the line). Fits: drawn as is. Too long: left-aligned, never scaled down;
   active = a marquee (ping-pong: pause at the start, slide at PIMGUI_MARQUEE_PX_S until the end shows, pause, slide
   back - the derma panels of an earlier PSP UI port), not active = cut with "...". t = seconds since the text became
   active (restart the marquee when the focus moves). */
void  pimgui_marquee(const char *text, float w, int active, double t, unsigned color);
/* the scroll offset in pixels at time t for a text text_w wide in a box box_w wide (pure function; tests) */
float pimgui_marquee_offset(float text_w, float box_w, double t);
#define PIMGUI_MARQUEE_PX_S   30.0
#define PIMGUI_MARQUEE_HOLD_S 0.8

/* row lists: a scrolling list with one focused row. D-pad Up / Down moves the focus (wraps), Left / Right return
   PIMGUI_ROW_LEFT / RIGHT on the focused row (auto-repeat), Cross returns PIMGUI_ROW_ACTIVATE. The focused row is kept
   in view; its label scrolls as a marquee when too long. */
enum { PIMGUI_ROW_NONE = 0, PIMGUI_ROW_LEFT, PIMGUI_ROW_RIGHT, PIMGUI_ROW_ACTIVATE };
void   pimgui_rows_begin(const char *id, int *focus, float height);   /* height 0 = to the bottom of the window */
/* one row: label left, value right (arrows "<  >" when steppable), frac 0..1 draws a bar under the value (< 0 none),
   badge = short accent text before the label ("*", "R", NULL); dim = greyed (not available) */
int    pimgui_row(const char *label, const char *value, float frac, int steppable, const char *badge, int dim);
int    pimgui_row_int(const char *label, int *v, int lo, int hi, int step, const char *fmt);   /* 1 = changed */
int    pimgui_row_choice(const char *label, int *idx, const char *const *items, int n);         /* 1 = changed */
int    pimgui_row_bool(const char *label, int *v);                                             /* 1 = changed */
int    pimgui_row_button(const char *label);                                                   /* 1 = activated */
int    pimgui_rows_focus_changed(void);   /* 1 = the focus moved this frame (or the list is new) */
double pimgui_rows_focus_time(void);      /* seconds since the focus last moved */
void   pimgui_rows_end(void);

#ifdef __cplusplus
}
#endif
#endif
