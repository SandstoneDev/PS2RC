/* hle_ge.c - PSP GE back-end of the render driver boundary: the 2D path.
 *
 * What it draws (hooks in hle_render.c, same semantics as the host's software rasteriser hle_raster.c):
 *   - the 2D draw functions' primitives (line list / polyline / triangle list / strip / fan, indexed or not; 2D vertex
 *     stride 0x40: x, y +0/+4, u, v +0x10/+0x14 (0..1), colour floats 0..255 +0x20..+0x2C) in through mode, guest
 *     screen coordinates (the camera raster, 640x448) scaled to 480x272;
 *   - the game's render buffer type 0 (font glyphs: centre, half extents, texture corners s16/4096, RGBA bytes) as
 *     GE sprites;
 *   - camera clears (colour), and drv_raster_show = the flip.
 *   Shading = texel x vertex colour (GU_TFX_MODULATE: vertex 255 = 1.0, the driver's VU programs' 255 -> 0x80 scaling
 *   into GS MODULATE); texture alpha 0..0x80 is doubled by texconv; the driver's blend factors -> GE blend; alpha test
 *   "> 0" when the source factor is SRCALPHA (the driver tests alpha on alpha formats); filter / U,V addressing from
 *   the render states.
 *
 * Display: 480x272, 16-bit 5650 (half the fill bandwidth of 8888 - the GE is fill-bound on hardware - and the 4
 *   buffers of 272 KB fit with room for the 3D Z buffer and a VRAM texture cache later), dithered. The guest draws
 *   into a persistent picture P (VRAM 0xCC000, never displayed) that keeps the previous frame like the PS2 / host
 *   frame buffer (the loading screens fade in over the previous picture without a colour clear); at the flip P is
 *   block-copied into one of three display buffers (VRAM 0 / 0x44000 / 0x88000; Z reserved at 0x110000, the text
 *   console buffer T at 0x154000; 1.6 MB of the 2 MB in all), the log text
 *   is drawn over that copy, and it is shown with sceDisplaySetFrameBuf(NEXTFRAME) - never a vblank wait: the buffer
 *   filled next is neither the displayed nor the pending one (tracked with the vblank count). The first frame goes
 *   to buffer 2 because the boot's 8888 debug screen occupies VRAM 0..0x88000.
 * Display lists: two lists of 256 KB in the guest code hole, written through their UNCACHED alias (commands and the
 *   vertices taken from them with sceGuGetMemory reach RAM without a dcache writeback); GU_DIRECT: the GE draws while
 *   the guest builds the frame; at the flip the list is finished and synced (sceGuSync), so a list / its vertices are
 *   never rewritten while the GE may read them (lists alternate as well). A list that would overflow, or a texture
 *   that only fits by evicting one the open list still uses, forces a mid-frame finish + sync ("resync").
 * Textures: guest rasters (the driver's PS2 rasters: GIF upload packets behind the pixel / palette pointers) -> texconv
 *   (PAL4 -> T4, PAL8 -> T8 + 8888 CLUT, swizzled, level 0 only) into a texture arena in the hole (first fit, LRU
 *   eviction); cache entry = one upload of a raster: raster address + the raster's pixel / palette / original-pixels
 *   pointers and size / format (a defragmenter move re-converts); raster create / texture read / unlock / destroy
 *   (hle_render.c) end the upload, so the next draw converts a new version.
 *   After converting, the arena range is written back from the dcache before any list references it. A raster
 *   without PS2 upload packets (the camera raster used as a texture by motion blur / post-FX) is not drawn (counted).
 * Screen text: pspDebugScreenPrintf is wrapped (-Wl,--wrap): every line also goes into a console ring; in GE mode the
 *   last ge_text rows are drawn over the picture (8x8 font from libpspdebug, half-transparent black row backgrounds).
 *   A "result=" / CRASH / FATAL / WATCHDOG / "Press X" line switches the screen back to the plain debug console for good
 *   (the console is redrawn into its own VRAM buffer T at 0x154000, which the GE never draws into); a stall (no flip for
 *   3 s while a line arrives, e.g. the watchdog's progress lines) shows the console until the next flip.
 * 2D depth: 2D draws honour the driver's Z test enable / Z write enable states on the Z buffer at 0x110000 (the
 *   round radar is a Z mask; the driver tests GEQUAL; the guest's Z clear is done lazily unless the 3D world cleared
 *   it), primitives leaving the guard band are clipped instead of clamped, L+R+SELECT+TRIANGLE toggles the log text
 *   (L+R+SELECT = fps counter), ps2rc.cfg ge_zmask=0 / ge_trace=<flip>.
 * Threads: every sceGu call runs on the game PSP thread (all guest threads are coroutines in it); the watchdog / crash
 *   handler only touch the display and the debug screen, serialised with the flip by a semaphore. */
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <psputils.h>
#include <stdlib.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_render.h"
#include "hle_ge.h"
#include "hle_w3d.h"
#include "hle_sky.h"                                  /* the sky (hle_sky.c) */
#include "hle_postfx.h"                               /* the PS2 colour filter (hle_postfx.c) */
#include "hle_water.h"                                /* water surfaces (hle_water.c) */
#include "texconv.h"
#include "hle_hitch.h"                                     /* hitch sources texconv / ge-sync */
#include "hle_fast.h"                                      /* x_aflip (hle_x_opt) */

int hle_ge_on;
int hle_ge_guest_im2d = 0;                 /* default 0 (ps2rc.cfg im2d_guest=1 = the old behaviour) */

/* ------------------------------------------------------------------ options (<EBOOT dir>/ps2rc.cfg) */
static int opt_render_ge = 1, opt_text_rows = 12, opt_tex_kb = 2048, opt_clear = 0;
static uint32_t opt_dump[16];
static uint32_t opt_trace;               /* ge_trace=<flip>: log every 2D draw of that flip (diagnostics) */
static int opt_zmask = 1;                /* ge_zmask=0: 2D draws without depth (the radar is then square) */
static int opt_ndump;

/* ------------------------------------------------------------------ display / VRAM */
#define SCR_W 480
#define SCR_H 272
#define FB_STRIDE 512
#define FB_BYTES (FB_STRIDE * SCR_H * 2)
#define P_OFF (3u * FB_BYTES)             /* the persistent picture the guest draws into (never displayed) */
#define ZB_OFF (4u * FB_BYTES)            /* reserved for the 3D Z buffer */
#define T_OFF (5u * FB_BYTES)             /* the debug console after a takeover (CPU-written, never a GE target) */
#define VRAM_UNCACHED 0x44000000u
static const uint32_t fb_off[3] = {0, FB_BYTES, 2u * FB_BYTES};
static int p_ready;                       /* P cleared once */
static int disp = -1, pend = -1;          /* displayed / pending (latched at the next vblank) buffer */
static unsigned pend_vc;                  /* vblank count read right after the pending buffer was set */
static int first_present = 1;
static int draw_buf = 2;

/* screen owner: the debug console (boot, end of run, stall) or the GE frames */
enum { SCR_TEXT = 0, SCR_GE, SCR_STALL, SCR_FINAL };
static volatile int scr = SCR_TEXT;
static volatile SceInt64 last_present_us;
#define STALL_US 3000000
static SceUID ge_sema = -1;

/* ------------------------------------------------------------------ display lists */
#define LIST_BYTES (256u * 1024u)
#define LIST_MARGIN 2048u
static void *list_u[2];                   /* uncached views of the two lists */
static int cur_list, frame_open;
static uint32_t epoch = 1;                /* GE work since the last sync; a texture bound in it is pinned */
static float sx = 0.75f, sy = 272.0f / 448.0f;   /* guest screen (camera raster) -> 480x272 */
static uint32_t screen_raster, cur_fb;

/* ------------------------------------------------------------------ statistics */
static uint64_t st_flips, st_presented, st_prims, st_verts, st_quads, st_clears, st_binds, st_conv, st_conv_bytes,
    st_conv_fail, st_tex_skip, st_offscreen, st_bad, st_resync, st_evict, st_clamped, st_dumps, st_takeovers,
    st_stalls, st_zclear, st_blend_approx, st_skyclears;
static uint32_t st_list_max, st_arena_peak, conv_us, conv_us_max;
static uint64_t sync_us, flip_us;                 /* CPU time waiting for the GE (sceGuSync), whole flips */
static uint32_t sync_us_max, flip_us_max;

/* ------------------------------------------------------------------ GE cost probe and async flip
   GE serial probe (ps2rc.cfg ge_ser=N, default 0 = off): on a probe frame the frame list is built in GU_SEND mode (the
   GE does not run while the CPU builds it) and every list end - the flip, a resync, and a CUT at each change of the
   drawing category (GS_*, marked by the entry points; the cut happens at the next list_room, the same safe point as
   an overflow resync) - is sent and waited for: the wait = the GE's own time for that part, with the main core asleep
   (no CPU bus traffic from the game thread; the ME may still run) = a lower bound of the GE cost per category. Probe
   frames: with the in-run A/B the last settle (not counted) iteration of every window (per variant); without it every
   N-th frame. The probe frame itself is slow (CPU and GE serial) - never in an A/B count.
   List-open time: first draw of a frame (frame_begin) -> its flip done, per frame; the A/B report divides it per variant
   (the sampler's "drawing" share may be this, not GE work: the hardware never reported "stalled").
   Async flip (ps2rc.cfg ge_aflip=1 starts the presenter thread; x_aflip=0|1 per flip, default 1 then; A/B key): the
   flip finishes the frame list WITHOUT waiting for the GE; a presenter thread (priority 0x1E) waits for that list
   (sceGeListSync) and shows the buffer; the next list start (or anything that needs the GE idle) first waits for the
   presenter ("settle") and only then advances the epoch, so nothing the pending list reads (textures, vertex blocks,
   the list itself) is reused early. The GE then overlaps the next frame's game logic instead of the CPU waiting. */
static int opt_ser, opt_aflip, ser_off;
static int ser_frame, ser_send, ser_cat, ser_want, ser_v;
#define SER_V 5                                   /* 4 A/B variants + "no A/B" */
static uint64_t ser_us[SER_V][GS_N], ser_marks[SER_V][GS_N], ser_frames[SER_V], ser_cuts[SER_V], ser_cpu_us[SER_V];
static uint32_t ser_us_max[SER_V];
static uint64_t ser_prev[GS_N], ser_mprev;          /* the probe frame's start values (its log line) */
static uint32_t ser_t0, ser_fr_us, open_t0;
static uint64_t open_us;
static const char *const gs_name[GS_N] = {"other", "clear", "2d", "w3d-me", "w3d-main", "fx", "postfx", "flip"};
int hle_ab_probe_due(int *v) __attribute__((weak));     /* the in-run A/B (developer module) */
extern int ge_list_executed[2];                          /* libpspgu */
static SceUID af_go = -1, af_done = -1;
static volatile int af_pending, af_buf, af_id, af_quit;
static int af_x = 1, af_gen = -1, af_broken;
static uint64_t st_aflips, st_af_settles, st_af_timeouts, af_wait_us;
static uint32_t af_wait_max;
static void present(int b);

void hle_ge_ser_mark(int cat) { ser_want = cat; if (ser_frame) ser_marks[ser_v][cat]++; }
uint64_t hle_ge_open_us(void) { return open_us; }
double hle_ge_ser_ms(int v)
{
    if (v < 0 || v >= SER_V || !ser_frames[v]) return -1.0;
    uint64_t t = 0;
    for (int c = 0; c < GS_N; c++) t += ser_us[v][c];
    return (double)t / (double)ser_frames[v] / 1000.0;
}

/* ------------------------------------------------------------------ small helpers */
static inline float gfl(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t cb(float f) { return !(f > 0.0f) ? 0u : f >= 255.0f ? 255u : (uint32_t)(f + 0.5f); }
static inline uint32_t abgr(float r, float g, float b, float a) { return cb(r) | cb(g) << 8 | cb(b) << 16 | cb(a) << 24; }
static inline float clampc(float v, float lo, float hi)
{
    if (!(v >= lo)) { st_clamped++; return lo; }        /* also NaN */
    if (v > hi) { st_clamped++; return hi; }
    return v;
}
static int ge_lock(SceUInt timeout_us)
{
    if (ge_sema < 0) return 0;
    SceUInt t = timeout_us;
    return sceKernelWaitSema(ge_sema, 1, timeout_us ? &t : NULL) >= 0;
}
static void ge_unlock(int locked) { if (locked) sceKernelSignalSema(ge_sema, 1); }

/* ------------------------------------------------------------------ the console ring (every screen line) */
#define CON_COLS 68                       /* the debug screen's 480 / 7 px */
#define CON_ROWS 64
static char con[CON_ROWS][CON_COLS];
static uint8_t con_len[CON_ROWS];
static volatile unsigned con_row;         /* rows started so far; the current one is con_row % CON_ROWS */

static void con_newline(void)
{
    unsigned r = con_row + 1;
    con_len[r % CON_ROWS] = 0;
    con_row = r;
}

static void con_add(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\n') { con_newline(); continue; }
        if (ch == '\r') continue;
        if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = ' ';
        unsigned r = con_row % CON_ROWS;
        if (con_len[r] >= CON_COLS) { con_newline(); r = con_row % CON_ROWS; }
        unsigned l = con_len[r];
        if (l < CON_COLS) { con[r][l] = ch; con_len[r] = (uint8_t)(l + 1); }
    }
}

/* the last `rows` rows (oldest first) into a snapshot; the current row counts when it is not empty (*partial = 1) */
static int con_snapshot(int rows, char (*out)[CON_COLS], uint8_t *len, int *partial)
{
    unsigned last = con_row;
    int part = con_len[last % CON_ROWS] != 0;
    if (!part && last > 0) last--;
    if (partial) *partial = part;
    int n = 0;
    for (int k = rows - 1; k >= 0; k--) {
        if ((unsigned)k > last) continue;
        unsigned r = (last - (unsigned)k) % CON_ROWS;
        unsigned l = con_len[r];
        if (l > CON_COLS) l = CON_COLS;
        memcpy(out[n], con[r], l);
        len[n] = (uint8_t)l;
        n++;
    }
    return n;
}

/* the screen goes back to the plain debug console, redrawn into its own buffer T (565 like the GE frames): the GE never
   renders into T, so the CPU-written text is safe from it on the PSP and PPSSPP shows T from memory (its hardware
   backends would keep showing their GPU copy of a buffer the GE had drawn) */
static void text_takeover(int final)
{
    int locked = ge_lock(200000);
    if (scr == SCR_GE) {
        pspDebugScreenInitEx((void *)(VRAM_UNCACHED + T_OFF), PSP_DISPLAY_PIXEL_FORMAT_565, 1);
        static char rows[33][CON_COLS];
        static uint8_t lens[33];
        int partial = 0, n = con_snapshot(33, rows, lens, &partial);
        for (int i = 0; i < n; i++) {
            pspDebugScreenPrintData(rows[i], lens[i]);
            if (i < n - 1 || !partial) pspDebugScreenPrintData("\n", 1);   /* the current row stays open */
        }
        scr = final ? SCR_FINAL : SCR_STALL;
        if (final) st_takeovers++; else st_stalls++;
    } else if (final && scr == SCR_STALL)
        scr = SCR_FINAL;
    ge_unlock(locked);
}

static int final_line(const char *b)
{
    return strstr(b, "result=") || strstr(b, "CRASH") || strstr(b, "FATAL") || strstr(b, "WATCHDOG") ||
           strstr(b, "Press X");
}

/* every pspDebugScreenPrintf of the EBOOT lands here (linked with -Wl,--wrap=pspDebugScreenPrintf) */
void __wrap_pspDebugScreenPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void __wrap_pspDebugScreenPrintf(const char *fmt, ...)
{
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof b) n = (int)sizeof b - 1;
    con_add(b, n);
    int s = scr;
    if (s == SCR_GE) {
        int fin = final_line(b);
        if (fin || sceKernelGetSystemTimeWide() - last_present_us > STALL_US) text_takeover(fin);
        return;                                            /* the takeover redrew the ring, b included */
    }
    if (s == SCR_STALL && final_line(b)) scr = SCR_FINAL;
    pspDebugScreenPrintData(b, n);
}

/* ------------------------------------------------------------------ texture arena (first fit, coalescing) */
static uint8_t *arena;
static uint32_t arena_bytes, arena_used;
#define MAXBLK 1024
static struct { uint32_t off, size; int used; } blk[MAXBLK];
static int nblk;

static int arena_alloc(uint32_t size, uint32_t *off)
{
    size = (size + 63u) & ~63u;
    for (int i = 0; i < nblk; i++) {
        if (blk[i].used || blk[i].size < size) continue;
        if (blk[i].size > size) {
            if (nblk >= MAXBLK) continue;
            memmove(&blk[i + 2], &blk[i + 1], sizeof blk[0] * (size_t)(nblk - i - 1));
            blk[i + 1].off = blk[i].off + size;
            blk[i + 1].size = blk[i].size - size;
            blk[i + 1].used = 0;
            blk[i].size = size;
            nblk++;
        }
        blk[i].used = 1;
        *off = blk[i].off;
        arena_used += size;
        if (arena_used > st_arena_peak) st_arena_peak = arena_used;
        return 1;
    }
    return 0;
}

static void arena_free(uint32_t off)
{
    int i = 0;
    while (i < nblk && blk[i].off != off) i++;
    if (i == nblk || !blk[i].used) return;
    blk[i].used = 0;
    arena_used -= blk[i].size;
    if (i + 1 < nblk && !blk[i + 1].used) {               /* merge with the next */
        blk[i].size += blk[i + 1].size;
        memmove(&blk[i + 1], &blk[i + 2], sizeof blk[0] * (size_t)(nblk - i - 2));
        nblk--;
    }
    if (i > 0 && !blk[i - 1].used) {                      /* and with the previous */
        blk[i - 1].size += blk[i].size;
        memmove(&blk[i], &blk[i + 1], sizeof blk[0] * (size_t)(nblk - i - 1));
        nblk--;
    }
}

/* ------------------------------------------------------------------ texture cache
   An entry is one upload of a raster: key = raster address + its pixel / palette / original-pixels pointers + size /
   format (a defragmenter move or a re-created raster at the same address re-converts); every event that changes the
   raster's texels (create, texture read, unlock, destroy: hle_ge_raster_changed) ends that upload, and the next draw
   converts a new one (ver = the global upload sequence number). */
static uint32_t upload_seq;
#define MAX_TEX 256
typedef struct {
    uint32_t raster, ver, pix, pal, opix, dims;   /* key + what validates it */
    uint32_t off, bytes;                          /* arena block (bytes 0 = none) */
    int ok;                                       /* 1 converted, -1 not convertible, 0 free slot */
    uint8_t psm, clut_psm, swz;
    uint8_t aclass;                               /* TC_ALPHA_* over the CLUT entries (3D blending) */
    uint16_t w, h, bufw, clut_n;
    uint32_t lvl_off, clut_off;
    uint64_t last_frame;
    uint32_t pin;                                 /* == epoch: used by the open list */
    uint32_t w3d_hold;                            /* == w3d_hold_gen: a w3d_me batch will bind it */
    uint64_t ws_flip;                             /* flip + 1 of its last count in the working set */
    uint8_t half;                                 /* converted at half width / height (tex_half) */
    uint8_t nlev;                                 /* levels converted (> 1 only with tex_mip) */
    uint32_t l0_bytes;                            /* level 0 bytes (+ CLUT): the working set without mips */
} GeTex;
static GeTex tex[MAX_TEX];
/* the frame's texture working set (distinct textures bound or described to the ME, their arena bytes) - what a VRAM
   texture cache would have to hold */
static uint64_t ws_bytes, ws_n, ws_tot_bytes, ws_tot_n, ws_frames, ws_max_bytes;
static uint64_t st_flips_ws;                      /* = st_flips (declared later) mirrored at every flip */
static uint64_t ws_l0_bytes;                      /* the same counting level 0 (+ CLUT) only */
static inline void ws_count(GeTex *t)
{
    if (t->ws_flip == st_flips_ws + 1u) return;
    t->ws_flip = st_flips_ws + 1u;
    ws_bytes += t->bytes;
    ws_l0_bytes += t->l0_bytes;
    ws_n++;
}
/* w3d_me: while a batch is described to the Media Engine its textures are looked up but bound only
   later (by the ME's GE words); held textures are neither evicted nor released, as if the open list used them */
static uint32_t w3d_hold_gen;
static inline int tex_busy(const GeTex *t) { return t->pin == epoch || (w3d_hold_gen && t->w3d_hold == w3d_hold_gen); }
static GeTex *cur_tex;                            /* bound in the open list */
/* Lower texture resolution for the 3D world.
   tex_half=N (ps2rc.cfg / A/B key, default 0 = off): a texture first converted for a 3D draw (hle_ge_3d_texture: peds,
   cars, props on the main core; hle_ge_3d_tex_info: buildings described to the ME) whose larger side is >= N px is
   converted at half width and height - the stored PS2 mip level 1 when the raster has one (exact), else 2:1 point
   decimation (texconv fast path, CLUT formats kept). UVs stay right: the 3D path's UVs are normalised (GE texture
   scale 1 / 8, independent of the size) and the 2D paths compute texel UVs from the entry's own w / h at draw time.
   tex_half_fx=1 (default 0) lets the FX lookups (sky, sprites, shadows, particles, water: hle_ge_tris_packed /
   hle_ge_tex_size / hle_ge_tris) convert at half too; otherwise they take whatever entry exists (new ones at full).
   2D draws and render-buffer draws (HUD, fonts, radar, menus, loading screens) always want full resolution: a halved entry
   they meet is converted again at full (and then stays full for the 3D lookups too - no ping-pong). A change of either
   key (A/B switch) drops every cached texture (busy ones become orphans, as hle_ge_raster_changed does), so the next
   draws convert under the new value inside the window's settle iterations. */
enum { TL_FULL, TL_ANY, TL_HALF };
static int opt_tex_half, opt_tex_half_fx;
static uint64_t st_half, st_half_mip, st_half_bytes_saved, st_promote, st_tex_flush;
#define BJ_V 5                                    /* 4 A/B variants + "no A/B" */
static uint64_t bj_conv[BJ_V], bj_conv_bytes[BJ_V], bj_conv_us[BJ_V], bj_half[BJ_V], bj_evict[BJ_V];
static uint64_t bj_ws_bytes[BJ_V], bj_ws_n[BJ_V], bj_ws_frames[BJ_V], bj_ws_max[BJ_V], bj_arena[BJ_V];
int hle_ab_cur(int *counted) __attribute__((weak));     /* in-run A/B: the variant in effect, -1 = no A/B part */
static inline int bj_v(void)                      /* the statistics row of this moment, -1 = not counted */
{
    int counted = 1, v = hle_ab_cur ? hle_ab_cur(&counted) : -1;
    if (v < 0) return BJ_V - 1;
    return counted && v < BJ_V - 1 ? v : -1;
}
static inline int fx_mode(void) { return opt_tex_half_fx ? TL_HALF : TL_ANY; }
/* Auto mipmaps for the 3D world. tex_mip=1 (ps2rc.cfg / A/B key, default 0 = off):
   a texture converted for a 3D lookup (TL_HALF: peds / cars / props on the main core, buildings described to the ME;
   tex_half applies first) keeps its stored PS2 levels and gets generated ones below them down to the swizzle floor
   (texconv TC_F_GENMIP: 2:1 point decimation of the previous level, CLUT indices kept), at most 8. A 3D bind (main core
   apply_state for hle_ge_3d_texture, the ME's words via hle_ge_3d_tex_info) sets TMODE's max level, the level
   registers TBP/TBW/TSIZE 1..n, TLEVEL auto mode + bias, and a mipmap minifying filter (tex_mip_filter 0:
   LINEAR_MIPMAP_NEAREST, 1: LINEAR_MIPMAP_LINEAR; nearest-filtered textures: NEAREST_MIPMAP_NEAREST / _LINEAR).
   2D / FX / render-buffer binds of the same entry use level 0 only (max level 0). tex_mip_bias / tex_mip_abias: level
   bias in 1/16 levels for opaque / alpha (cutout, blend) textures (default 0 / 0); tex_mip_alpha=0: alpha textures
   bound without levels (foliage / fences keep level 0 coverage). GE fact: with a max
   level >= 1 the 8 KB texture cache is split between even and odd levels. A change of tex_mip* drops the cache. */
static int opt_tex_mip, opt_tex_mip_bias, opt_tex_mip_abias, opt_tex_mip_alpha = 1, opt_tex_mip_filter;
static uint64_t st_mip_tex, st_mip_levels, st_mip_gen, st_mip_bytes, st_mip_bad;
static uint64_t bk_ws_l0[BJ_V], bk_mip_binds[BJ_V], bk_binds[BJ_V], bk_mip_conv[BJ_V];
static uint32_t cur_mx;                           /* max mip level of the bound texture (0: level 0 only) */
static int want_mip;                              /* apply_state called for a 3D bind (hle_ge_3d_texture) */
static inline uint32_t tex_mx(const GeTex *t)
{
    if (!opt_tex_mip || t->nlev < 2 || (!opt_tex_mip_alpha && t->aclass != TC_ALPHA_OPAQUE)) return 0;
    return (uint32_t)t->nlev - 1u;
}
static inline int tex_lbias(const GeTex *t) { return t->aclass == TC_ALPHA_OPAQUE ? opt_tex_mip_bias : opt_tex_mip_abias; }
static inline void bk_bind_count(uint32_t mx)
{
    int bv = bj_v();
    if (bv >= 0) { bk_binds[bv]++; if (mx) bk_mip_binds[bv]++; }
}
static int diag_conv = 8;

static void tex_release(GeTex *t)
{
    if (t->bytes) arena_free(t->off);
    if (t == cur_tex) cur_tex = NULL;
    memset(t, 0, sizeof *t);
}

static void resync(void);

/* the least recently used texture the open list does not use */
static GeTex *tex_lru(void)
{
    GeTex *best = NULL;
    for (int i = 0; i < MAX_TEX; i++) {
        GeTex *t = &tex[i];
        if (!t->ok || tex_busy(t)) continue;                /* + held (w3d_me) */
        if (!best || t->last_frame < best->last_frame) best = t;
    }
    return best;
}

static int arena_get(uint32_t bytes, uint32_t *off)
{
    if (bytes > arena_bytes) return 0;
    for (;;) {
        if (arena_alloc(bytes, off)) return 1;
        GeTex *v = tex_lru();
        if (v && v->bytes) {
            tex_release(v);
            st_evict++;
            int bv = bj_v();
            if (bv >= 0) bj_evict[bv]++;
            continue;
        }
        if (v) { tex_release(v); continue; }
        if (!frame_open) return 0;
        resync();                                          /* nothing pinned after a sync */
        if (!tex_lru()) return 0;
    }
}

static GeTex *tex_slot(void)
{
    for (int i = 0; i < MAX_TEX; i++)
        if (!tex[i].ok) return &tex[i];
    GeTex *v = tex_lru();
    if (!v && frame_open) { resync(); v = tex_lru(); }
    if (!v) return NULL;
    if (v->bytes) {
        st_evict++;
        int bv = bj_v();
        if (bv >= 0) bj_evict[bv]++;
    }
    tex_release(v);
    return v;
}

static int tex_convert(GeTex *t, uint32_t r, int half_min, int mip)
{
    int32_t w = (int32_t)gw(r + 0x0C), h = (int32_t)gw(r + 0x10), depth = (int32_t)gw(r + 0x14);
    uint32_t fmt = (uint32_t)gb(r + 0x23) << 8, pix = t->pix, pal = t->pal;
    int err = 0;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || pix < HLE_CODE_HI + 0x50u || pix >= hle_ram_end) err = 100;
    const uint8_t *palu = NULL;
    if (!err && (fmt & (TC_PF_PAL4 | TC_PF_PAL8))) {
        if (pal < HLE_CODE_HI + 0x50u || pal >= hle_ram_end - 0x500u) err = 101;
        else palu = hle_ram + (pal - 0x50u);
    }
    TcPs2Raster ps2;
    TcPs2Info info;
    TcPolicy pol;
    TcPspTex plan;
    if (!err) {
        uint32_t up = pix - 0x50u, avail = hle_ram_end - up;
        if (avail > 0x200000u) avail = 0x200000u;
        err = -tc_ps2_from_packets(hle_ram + up, avail, palu, w, h, depth, fmt, &ps2);
    }
    if (!err) err = -tc_ps2_inspect(&ps2, &info);
    if (!err) {
        tc_policy_default(&pol);
        pol.max_levels = 1;                                /* 2D: level 0 only, never a dropped PS2 level */
        pol.cache_bytes = 0;
        pol.alpha = TC_AC_CLUT;                            /* the alpha class (CLUT scan only) */
        int mx = w > h ? w : h;
        if (half_min > 0 && mx >= half_min && mx >= 32) {  /* half size (PS2 level 1 or 2:1 decimation) */
            pol.max_dim = (uint16_t)(mx / 2);
            pol.decimate = 1;
        }
        if (mip) {                                         /* stored + generated levels (tex_mip) */
            pol.max_levels = TC_MAX_PSP_LEVELS;
            pol.flags |= TC_F_GENMIP;
        }
        err = -tc_plan(&info, &pol, &plan);
    }
    uint32_t off = 0;
    if (!err && (plan.psm > TC_GU_T8 || plan.level[0].width > 512 || plan.level[0].height > 512)) err = 102;
    if (!err && !arena_get(plan.total_bytes, &off)) err = 103;
    if (!err) {
        t->off = off;
        t->bytes = plan.total_bytes;
        err = -tc_convert(&info, &pol, &plan, arena + off, plan.total_bytes);
        if (!err) sceKernelDcacheWritebackRange(arena + off, plan.total_bytes);   /* before any list uses it */
    }
    if (err) {
        st_conv_fail++;
        if (diag_conv > 0) {
            diag_conv--;
            LOG("GE texture: raster %08x %dx%d depth %d format %04x pixels %08x palette %08x not converted (%s %d)\n",
                r, (int)w, (int)h, (int)depth, (unsigned)fmt, pix, pal, err >= 100 ? "check" : "texconv", err);
        }
        return 0;
    }
    t->psm = plan.psm;
    t->aclass = plan.alpha_class;
    t->clut_psm = plan.clut_psm;
    t->swz = plan.swizzled;
    t->w = plan.level[0].width;
    t->h = plan.level[0].height;
    t->bufw = plan.level[0].bufw;
    t->lvl_off = plan.level[0].offset;
    t->clut_n = plan.clut_psm != TC_GU_NONE ? plan.clut_entries : 0;
    t->clut_off = plan.clut_offset;
    t->half = (uint8_t)(plan.first || plan.shift);
    t->nlev = 1;
    t->l0_bytes = plan.level[0].bytes + plan.clut_bytes;
    if (plan.nlevels > 1) {
        int good = 1;                                      /* the layout the binds compute (hle_ge_mip_level) */
        for (uint32_t k = 1; k < plan.nlevels; k++) {
            uint32_t lw, lh, lb, o = hle_ge_mip_level(plan.psm, plan.swizzled, plan.level[0].width, plan.level[0].height,
                                                      k, &lw, &lh, &lb);
            if (o != plan.level[k].offset - plan.level[0].offset || lw != plan.level[k].width ||
                lh != plan.level[k].height || lb != plan.level[k].bufw) good = 0;
        }
        if (good) {
            t->nlev = plan.nlevels;
            st_mip_tex++;
            st_mip_levels += plan.nlevels;
            st_mip_gen += plan.ngen;
            st_mip_bytes += plan.level[plan.nlevels - 1].offset + plan.level[plan.nlevels - 1].bytes - plan.level[1].offset;
            int bv = bj_v();
            if (bv >= 0) bk_mip_conv[bv]++;
        } else {
            st_mip_bad++;
        }
    }
    if (t->half) {
        st_half++;
        if (plan.first) st_half_mip++;
        st_half_bytes_saved += 3u * (uint64_t)plan.level[0].bytes;   /* level 0 at full size = 4 x */
    }
    st_conv++;
    st_conv_bytes += plan.total_bytes;
    return 1;
}

/* the GE texture of a guest raster, converted on first use; NULL = not drawable */
static uint8_t tex_hint[4096];            /* raster -> last slot (the 256-entry scan per draw thrashed
                                             the dcache: ~170 3D lookups + every 2D draw per frame); 4096 hashed slots:
                                             with 256 a hint collision still scanned on most gameplay draws */
uint32_t hle_ge_tex_scans;                /* lookups that missed the hint and scanned the table (world3d report) */
static GeTex *tex_lookup(uint32_t r, int mode)       /* mode TL_FULL / TL_ANY / TL_HALF */
{
    if (r < HLE_CODE_HI || r >= hle_ram_end - 0x40u) return NULL;
    uint32_t pix = gw(r + 0x04), pal = gw(r + 0x08), opix = gw(r + 0x24);
    uint32_t dims = gw(r + 0x0C) ^ gw(r + 0x10) << 11 ^ gw(r + 0x14) << 22 ^ (uint32_t)gb(r + 0x23) << 24;
    GeTex *t = NULL;
    unsigned hs = (r >> 4) * 2654435761u >> 20;
    if (tex[tex_hint[hs]].ok && tex[tex_hint[hs]].raster == r) t = &tex[tex_hint[hs]];
    else {
        hle_ge_tex_scans++;
        for (int i = 0; i < MAX_TEX; i++)
            if (tex[i].ok && tex[i].raster == r) { t = &tex[i]; tex_hint[hs] = (uint8_t)i; break; }
    }
    if (t && t->pix == pix && t->pal == pal && t->opix == opix && t->dims == dims && !(mode == TL_FULL && t->half)) {
        t->last_frame = st_presented;
        return t->ok > 0 ? t : NULL;
    }
    if (t && mode == TL_FULL && t->half) st_promote++;     /* a 2D draw wants it at full size */
    if (t) {                                               /* stale: re-convert */
        if (tex_busy(t)) t->raster = 0;                   /* the open list still samples it: orphan until evicted */
        else tex_release(t);
    }
    t = tex_slot();
    if (!t) return NULL;
    t->raster = r;
    tex_hint[hs] = (uint8_t)(t - tex);
    t->ver = ++upload_seq;
    t->pix = pix;
    t->pal = pal;
    t->opix = opix;
    t->dims = dims;
    t->last_frame = st_presented;
    SceUInt t0 = sceKernelGetSystemTimeLow();
    t->ok = tex_convert(t, r, mode == TL_HALF ? opt_tex_half : 0, mode == TL_HALF && opt_tex_mip) ? 1 : -1;
    SceUInt dt = sceKernelGetSystemTimeLow() - t0;
    {                                                      /* per A/B variant (counted iterations) */
        int bv = bj_v();
        if (bv >= 0) {
            bj_conv[bv]++;
            bj_conv_us[bv] += dt;
            if (t->ok > 0) { bj_conv_bytes[bv] += t->bytes; bj_half[bv] += t->half; }
        }
    }
    conv_us += dt;
    hle_hitch_us[HITCH_TEXCONV] += dt;
    if (dt > conv_us_max) conv_us_max = dt;
    if (t->ok < 0 && t->bytes) { arena_free(t->off); t->bytes = 0; }
    return t->ok > 0 ? t : NULL;
}

/* drop every cached texture (busy ones become orphans until evicted) */
static void tex_flush(void)
{
    for (int i = 0; i < MAX_TEX; i++)
        if (tex[i].ok) {
            if (tex_busy(&tex[i])) tex[i].raster = 0;
            else tex_release(&tex[i]);
        }
    st_tex_flush++;
}

/* tex_half / tex_half_fx / tex_mip* from ps2rc.cfg (boot) and the in-run A/B (FAM_TEX, between two
   main-loop iterations); returns 1 for a known key */
int hle_ge_tex_cfg(const char *k, const char *v)
{
    int *o = !strcmp(k, "tex_half") ? &opt_tex_half : !strcmp(k, "tex_half_fx") ? &opt_tex_half_fx : NULL;
    if (!o) {                                              /* tex_mip family */
        o = !strcmp(k, "tex_mip") ? &opt_tex_mip : !strcmp(k, "tex_mip_bias") ? &opt_tex_mip_bias :
            !strcmp(k, "tex_mip_abias") ? &opt_tex_mip_abias : !strcmp(k, "tex_mip_alpha") ? &opt_tex_mip_alpha :
            !strcmp(k, "tex_mip_filter") ? &opt_tex_mip_filter : NULL;
        if (!o) return 0;
        int n = atoi(v);
        if (o == &opt_tex_mip_bias || o == &opt_tex_mip_abias) n = n < -128 ? -128 : n > 127 ? 127 : n;
        else n = n != 0;
        if (n != *o) {
            *o = n;
            if (hle_ge_on) tex_flush();
        }
        return 1;
    }
    int n = atoi(v);
    if (n < 0) n = 0;
    if (o == &opt_tex_half && n > 0 && n < 32) n = 32;
    if (n != *o) {
        *o = n;
        if (hle_ge_on) tex_flush();
    }
    return 1;
}

void hle_ge_raster_changed(uint32_t r)
{
    for (int i = 0; i < MAX_TEX; i++)
        if (tex[i].ok && tex[i].raster == r) {
            if (tex_busy(&tex[i])) tex[i].raster = 0;     /* still sampled by the open list (or held: w3d_me) */
            else tex_release(&tex[i]);
        }
}

/* ------------------------------------------------------------------ GE state */
static int cur_sb = -1, cur_db = -1, cur_filter = -1, cur_wrap = -1, cur_texon = -1;
static int ge3d_active;                   /* the GE carries the 3D world's state (psp/hle_ge3d.c) */
static int cur_zmode = -1;                /* 2D depth, bit 0 Z test (GEQUAL), bit 1 Z write */
static int zclear_pending = 1;            /* the guest cleared Z (camera clear mode 2): done before the next 2D Z draw */
static uint64_t st_zdraws, st_zclears_done;

static void state_reset(void)
{
    static const ScePspIMatrix4 dither = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    sceGuDrawBufferList(GU_PSM_5650, (void *)P_OFF, FB_STRIDE);
    sceGuDepthBuffer((void *)ZB_OFF, FB_STRIDE);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CULL_FACE);                            /* through mode still culls */
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuSetDither(&dither);
    sceGuEnable(GU_DITHER);
    sceGuShadeModel(GU_SMOOTH);
    sceGuPixelMask(0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    cur_tex = NULL;
    cur_sb = cur_db = cur_filter = cur_wrap = cur_texon = -1;
    ge3d_active = 0;
    cur_zmode = 0;                                         /* depth test off, no depth writes (above) */
}

/* back to the 2D state after 3D draws (depth, culling, fog, clipping, alpha test, texture scale) */
static void ge3d_leave(void)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    if (!ge3d_active) return;
    ge3d_active = 0;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_LIGHTING);                             /* the 3D path's GE lighting */
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    cur_sb = cur_db = cur_filter = cur_wrap = cur_texon = -1;
    cur_zmode = 0;                                         /* the 2D depth state is "off" again (above) */
}

/* The driver's blend factors (1 ZERO, 2 ONE, 3 SRCCOLOR, 4 INVSRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA, 7 DESTALPHA,
   8 INVDESTALPHA, 9 DESTCOLOR, 10 INVDESTCOLOR, 11 SRCALPHASAT) -> GE. The GE's source factor offers the destination colour, its
   destination factor the source colour (GU_OTHER_COLOR); a 5650 frame buffer has no destination alpha: DESTALPHA is
   taken as 1 (the PS2 frame buffer alpha of opaque pixels), as the host rasteriser's cleared buffer mostly has. */
static void set_blend(int sb, int db)
{
    int a = GU_FIX, b = GU_FIX;
    unsigned fa = 0, fb = 0;
    switch (sb) {
    case 1: fa = 0; break;
    case 2: case 7: fa = 0xFFFFFF; break;
    case 8: fa = 0; break;
    case 5: case 11: a = GU_SRC_ALPHA; break;
    case 6: a = GU_ONE_MINUS_SRC_ALPHA; break;
    case 9: a = GU_OTHER_COLOR; break;                     /* as the source operand: Cd */
    case 10: a = GU_ONE_MINUS_OTHER_COLOR; break;          /* 1 - Cd */
    default: a = GU_SRC_ALPHA; st_blend_approx++; break;   /* SRCCOLOR as source factor: not on the GE */
    }
    switch (db) {
    case 1: fb = 0; break;
    case 2: case 7: fb = 0xFFFFFF; break;
    case 8: fb = 0; break;
    case 3: b = GU_OTHER_COLOR; break;                     /* as the destination operand: Cs */
    case 4: b = GU_ONE_MINUS_OTHER_COLOR; break;
    case 5: case 11: b = GU_SRC_ALPHA; break;
    case 6: b = GU_ONE_MINUS_SRC_ALPHA; break;
    default: b = GU_ONE_MINUS_SRC_ALPHA; st_blend_approx++; break;   /* DESTCOLOR as destination factor */
    }
    if (sb == 2 && db == 1) sceGuDisable(GU_BLEND);       /* ONE / ZERO: opaque */
    else { sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD, a, b, fa, fb); }
    if (sb == 5 || sb == 11) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST);
    if (db == 2) sceGuDisable(GU_DITHER); else sceGuEnable(GU_DITHER);   /* additive: no dither speckles */
}

static void apply_state(const RndState *st, GeTex *t)
{
    int sb = st->src_blend ? st->src_blend : 5, db = st->dst_blend ? st->dst_blend : 6;
    if (sb != cur_sb || db != cur_db) { set_blend(sb, db); cur_sb = sb; cur_db = db; }
    if (!t) {
        if (cur_texon != 0) { sceGuDisable(GU_TEXTURE_2D); cur_texon = 0; }
        return;
    }
    if (cur_texon != 1) { sceGuEnable(GU_TEXTURE_2D); cur_texon = 1; }
    uint32_t mx = want_mip ? tex_mx(t) : 0;               /* mip levels for a 3D bind */
    if (t != cur_tex || t->pin != epoch || mx != cur_mx) {
        uint8_t *base = arena + t->off;
        sceGuTexMode(t->psm, (int)mx, 0, t->swz);
        sceGuTexImage(0, t->w, t->h, t->bufw, base + t->lvl_off);
        for (uint32_t k = 1; k <= mx; k++) {
            uint32_t lw, lh, lb, o = hle_ge_mip_level(t->psm, t->swz, t->w, t->h, k, &lw, &lh, &lb);
            sceGuTexImage((int)k, (int)lw, (int)lh, (int)lb, base + t->lvl_off + o);
        }
        if (mx) sceGuSendCommandi(200, (int)(((uint32_t)tex_lbias(t) & 0xFFu) << 16));   /* TLEVEL: auto + bias */
        cur_mx = mx;
        if (want_mip) bk_bind_count(mx);
        if (t->clut_n) {
            sceGuClutMode(t->clut_psm, 0, 0xFF, 0);
            sceGuClutLoad(t->clut_psm == GU_PSM_8888 ? t->clut_n / 8 : t->clut_n / 16, base + t->clut_off);
        }
        sceGuTexFlush();
        cur_tex = t;
        t->pin = epoch;
        st_binds++;
        ws_count(t);
    }
    int f = (int)hle_ge_min_filter(st->filter == 2 || st->filter == 4 || st->filter == 6, mx,
                                   opt_tex_mip_filter ? 7u : 5u);   /* = GU_LINEAR / GU_NEAREST without mips */
    if (f != cur_filter) { sceGuTexFilter(f, f & 1); cur_filter = f; }
    int wu = st->addr_u == 3 || st->addr_u == 4 ? GU_CLAMP : GU_REPEAT;
    int wv = st->addr_v == 3 || st->addr_v == 4 ? GU_CLAMP : GU_REPEAT;
    if ((wu | wv << 1) != cur_wrap) { sceGuTexWrap(wu, wv); cur_wrap = wu | wv << 1; }
}

static int af_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        if (sceKernelWaitSema(af_go, 1, NULL) < 0 || af_quit) break;
        sceGeListSync(af_id, 0);                  /* that list done (an id already retired returns at once) */
        present(af_buf);
        sceKernelSignalSema(af_done, 1);
    }
    return 0;
}

static int af_use(void)
{
    if (!opt_aflip || af_broken || af_go < 0) return 0;
    if (af_gen != hle_x_gen) { af_gen = hle_x_gen; af_x = hle_x_opt("aflip", 1); }
    return af_x;
}

/* the pending async frame: wait for its presenter, then the GE is idle for that list - advance the epoch */
static void af_settle(void)
{
    if (!af_pending) return;
    SceUInt t0 = sceKernelGetSystemTimeLow(), to = 2000000;
    if (sceKernelWaitSema(af_done, 1, &to) < 0) {         /* the presenter is stuck: never again; sync here */
        st_af_timeouts++;
        af_broken = 1;
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        LOG("GE async flip: the presenter did not answer in 2 s - async flip off for the rest of the run\n");
    }
    SceUInt dt = sceKernelGetSystemTimeLow() - t0;
    af_wait_us += dt;
    if (dt > af_wait_max) af_wait_max = dt;
    sync_us += dt;
    hle_hitch_us[HITCH_GESYNC] += dt;
    st_af_settles++;
    af_pending = 0;
    cur_list ^= 1;
    epoch++;
    cur_tex = NULL;
}

/* ------------------------------------------------------------------ frames */
static int next_draw_buffer(void)
{
    af_settle();                                           /* the async frame's buffer is presented first */
    int locked = ge_lock(0);
    if (pend >= 0 && sceDisplayGetVcount() != pend_vc) { disp = pend; pend = -1; }
    int b = 2;
    if (!first_present)
        for (int i = 0; i < 3; i++)
            if (i != disp && i != pend) { b = i; break; }
    ge_unlock(locked);
    return b;
}

static void clear_rect(uint32_t color, int flags)
{
    struct CV { uint32_t c; int16_t x, y, z, pad; } *v = sceGuGetMemory(2 * sizeof(struct CV));
    v[0].c = color; v[0].x = 0; v[0].y = 0; v[0].z = 0; v[0].pad = 0;
    v[1].c = color; v[1].x = SCR_W; v[1].y = SCR_H; v[1].z = 0; v[1].pad = 0;
    sceGuSendCommandi(211, ((flags & 7) << 8) | 1);       /* CMD_CLEAR on (what sceGuClear does) */
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
    sceGuSendCommandi(211, 0);
}

static void list_begin(void)
{
    af_settle();                                           /* the async frame's list is done first */
    if (ser_frame) {                                       /* probe frame - built, then sent and waited for */
        sceGuStart(GU_SEND, list_u[cur_list]);
        ser_send = 1;
        ser_cat = ser_want;
    } else {
        sceGuStart(GU_DIRECT, list_u[cur_list]);
        ser_send = 0;
    }
    state_reset();
}

static void list_sync(void)
{
    uint32_t used = (uint32_t)sceGuCheckList();
    if (used > st_list_max) st_list_max = used;
    if (ser_send) {                                        /* the GE's own time for this part */
        sceGuFinish();
        SceUInt t0 = sceKernelGetSystemTimeLow();
        int id = sceGuSendList(GU_TAIL, list_u[cur_list], NULL);
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        SceUInt dt = sceKernelGetSystemTimeLow() - t0;
        if (id < 0 && !ser_off) { ser_off = 1; LOG("GE serial probe: sceGuSendList %08x - probe off\n", (unsigned)id); }
        ser_us[ser_v][ser_cat] += dt;
        ser_fr_us += dt;
        ser_cuts[ser_v]++;
        ser_send = 0;
        cur_list ^= 1;
        epoch++;
        cur_tex = NULL;
        return;
    }
    sceGuFinish();
    SceUInt t0 = sceKernelGetSystemTimeLow();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    SceUInt dt = sceKernelGetSystemTimeLow() - t0;
    sync_us += dt;
    hle_hitch_us[HITCH_GESYNC] += dt;
    if (dt > sync_us_max) sync_us_max = dt;
    cur_list ^= 1;
    epoch++;                                               /* nothing the GE reads is pinned any more */
    cur_tex = NULL;
}

static void resync(void)
{
    if (!frame_open) return;
    list_sync();
    list_begin();
    st_resync++;
}

/* a guest frame is drawn into P, which keeps the previous frame like the PS2 / host frame buffer (the loading screens
   fade in over the previous picture without a colour clear); ge_clear=1 clears it to black at every frame start */
static void frame_begin(void)
{
    if (frame_open) return;
    af_settle();                                           /* before the probe decision / the open clock */
    if (opt_ser > 0 && !ser_off) {                         /* GE serial probe frame? */
        int v = -1, due = hle_ab_probe_due ? hle_ab_probe_due(&v) : -1;
        if (due < 0) { due = st_flips % (uint64_t)opt_ser == 0; v = SER_V - 1; }
        if (due && v >= 0 && v < SER_V) {
            ser_frame = 1;
            ser_v = v;
            memcpy(ser_prev, ser_us[v], sizeof ser_prev);
            ser_mprev = ser_marks[v][GS_W3DM];
            ser_fr_us = 0;
            ser_t0 = sceKernelGetSystemTimeLow();
        }
    }
    open_t0 = sceKernelGetSystemTimeLow();
    list_begin();
    if (opt_clear || !p_ready) clear_rect(0xFF000000u, GU_COLOR_BUFFER_BIT);
    p_ready = 1;
    frame_open = 1;
}

static void list_room(uint32_t bytes)
{
    if (ser_send && ser_want != ser_cat && frame_open) {   /* probe cut at a category change */
        list_sync();
        list_begin();
    }
    if ((uint32_t)sceGuCheckList() + bytes + LIST_MARGIN > LIST_BYTES) resync();
}

static int draw_target(void)
{
    if (!hle_ge_on || scr == SCR_FINAL) return 0;
    if (screen_raster && cur_fb && cur_fb != screen_raster) { st_offscreen++; return 0; }
    return 1;
}

/* ------------------------------------------------------------------ the hooks' entry points */
void hle_ge_camera_begin(uint32_t camera, uint32_t fb)
{
    w3d_defer_flush();                                     /* deferred 3D atomics first */
    (void)camera;
    cur_fb = fb;
    if (fb && (!screen_raster || fb == screen_raster) && fb < hle_ram_end - 0x40u) {
        int w = (int)gw(fb + 0x0C), h = (int)gw(fb + 0x10);
        if (w > 0 && h > 0 && w <= 2048 && h <= 2048) { sx = (float)SCR_W / (float)w; sy = (float)SCR_H / (float)h; }
    }
}

void hle_ge_clear(uint32_t camera, const uint8_t rgba[4], int mode)
{
    if (!hle_ge_on || scr == SCR_FINAL) return;
    hle_ge_ser_mark(GS_CLEAR);
    uint32_t fb = camera && camera < hle_ram_end - 0x80u ? gw(camera + 0x60) : 0;
    if (screen_raster && fb && fb != screen_raster) { st_offscreen++; return; }
    int zc = (mode & 2) && w3d_opt_on;                     /* the 3D world's Z buffer (VRAM 0x110000) */
    if (mode & 2) { st_zclear++; zclear_pending = !zc; }   /* else done before the next 2D Z draw */
    if (!(mode & 1) && !zc) return;
    frame_begin();
    ge3d_leave();
    list_room(64);
    uint32_t col = (uint32_t)rgba[0] | (uint32_t)rgba[1] << 8 | (uint32_t)rgba[2] << 16 | (uint32_t)rgba[3] << 24;
    if (zc && w3d_opt_sky && hle_sky_clear_fill(mode)) {   /* w3d_sky: the 3D frame's Z clear also */
                                                           /* (only the frame start's, hle_sky.c - not the */
                                                           /* heat-haze pass's mid-frame one: the white screen) */
        col = hle_render_rs(RS_FOGCOLOR) | 0xFF000000u;     /* clears the picture to the fog / horizon colour - the */
        mode |= 1;                                          /* sky pass (3D) is not drawn yet: no stale picture */
        st_skyclears++;
    }
    clear_rect(col, (mode & 1 ? GU_COLOR_BUFFER_BIT : 0) | (zc ? GU_DEPTH_BUFFER_BIT : 0));   /* depth 0 = farthest */
    if (mode & 1) st_clears++;
}

typedef struct { float u, v; uint32_t c; float x, y, z; } GeV;
#define GEV_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define GE_MAX_VERTS 8192

/* ---- 2D depth. The driver's 2D screen Z is the GS depth (0 = far .. 2^24-1 = near) and the driver tests GEQUAL
   (drv_set_state 0x34FFE8, Z test enable: TEST_1 ZTE + ZTST 2). The round radar is a Z mask: a Z-only rectangle at
   z 0, four Z-only fans outside the circle at z 2^24-1, then the tile at z 2^24-2 with the Z test on (observed in a
   host trace). The GE's through-mode depth is 16-bit: the mapping keeps the order and the top 128 values distinct
   (2^24-1 and 2^24-2 must not collapse - without depth the radar is drawn square). */
static uint64_t st_clipped, st_clip_dropped;

static inline float zmap(float z)
{
    if (!(z > 0.0f)) return 0.0f;
    if (z >= 16777215.0f) return 65535.0f;
    if (z >= 16777088.0f) return 65408.0f + (z - 16777088.0f);
    return z * (65407.0f / 16777088.0f);
}

/* the depth state of a 2D draw from the driver's Z test / Z write enable states; the GE writes depth only while its
   depth test is on, so a Z-write-only draw (the radar mask) tests ALWAYS. The guest's Z clear (camera clear mode 2, every frame) is done
   here, lazily: a frame without a Z draw costs no depth fill. use = 0 forces depth off (render-buffer sprites). */
static void apply_depth(int use)
{
    int m = use && opt_zmask ? (hle_render_rs(RS_ZTESTENABLE) != 0) | (hle_render_rs(RS_ZWRITEENABLE) != 0) << 1 : 0;
    if (m && zclear_pending) {
        clear_rect(0, GU_DEPTH_BUFFER_BIT);                /* depth 0 = the farthest */
        zclear_pending = 0;
        st_zclears_done++;
    }
    if (m != cur_zmode) {
        cur_zmode = m;
        if (!m) {
            sceGuDisable(GU_DEPTH_TEST);
            sceGuDepthMask(GU_TRUE);
        } else {
            sceGuEnable(GU_DEPTH_TEST);
            sceGuDepthFunc(m & 1 ? GU_GEQUAL : GU_ALWAYS);
            sceGuDepthMask(m & 2 ? GU_FALSE : GU_TRUE);
        }
    }
    if (m) st_zdraws++;
}

/* ---- guard band: primitives reaching outside [-1024, 1536] are clipped (UV, colour, Z interpolated), not clamped -
   the pause-menu map scrolled / zoomed beyond the screen (clamping its ~9,500 coordinates squashed the tiles).
   Through mode + the 2048 - 240 / 136 drawing offset keeps the band inside the GE's 0..4095 window. */
#define GB_LO (-1024.0f)
#define GB_HI 1536.0f

static inline int gb_out(const GeV *v) { return !(v->x >= GB_LO && v->x <= GB_HI && v->y >= GB_LO && v->y <= GB_HI); }

static GeV lerp_v(const GeV *a, const GeV *b, float t)
{
    GeV o;
    o.u = a->u + (b->u - a->u) * t;
    o.v = a->v + (b->v - a->v) * t;
    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t;
    uint32_t c = 0;
    for (int k = 0; k < 32; k += 8) {
        float ca = (float)(a->c >> k & 255u), cc = (float)(b->c >> k & 255u);
        c |= (uint32_t)(ca + (cc - ca) * t + 0.5f) << k;
    }
    o.c = c;
    return o;
}

/* Sutherland-Hodgman: keep the part with coord (x: axis 0, y: axis 1) >= lim (ge = 1) or <= lim (ge = 0) */
static int clip_plane(const GeV *in, int n, GeV *out, int axis, float lim, int ge)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const GeV *a = &in[i], *b = &in[i + 1 == n ? 0 : i + 1];
        float da = (axis ? a->y : a->x) - lim, db = (axis ? b->y : b->x) - lim;
        if (!ge) { da = -da; db = -db; }
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) out[m++] = lerp_v(a, b, da / (da - db));
    }
    return m;
}

/* one triangle clipped to the guard band as a fan into out (at most 15 vertices); returns the vertex count */
static int clip_tri(const GeV *a, const GeV *b, const GeV *c, GeV *out)
{
    GeV p0[8], p1[8];
    p0[0] = *a; p0[1] = *b; p0[2] = *c;
    int n = clip_plane(p0, 3, p1, 0, GB_LO, 1);
    n = clip_plane(p1, n, p0, 0, GB_HI, 0);
    n = clip_plane(p0, n, p1, 1, GB_LO, 1);
    n = clip_plane(p1, n, p0, 1, GB_HI, 0);
    int k = 0;
    for (int i = 1; i + 1 < n; i++) { out[k++] = p0[0]; out[k++] = p0[i]; out[k++] = p0[i + 1]; }
    return k;
}

void hle_ge_im2d(int type, uint32_t verts, int nv, uint32_t gidx, const uint16_t *hidx, int ni, const RndState *st)
{
    w3d_defer_flush();                                     /* before any texture lookup here */
    static const int8_t prim_of[7] = {-1, GU_LINES, GU_LINE_STRIP, GU_TRIANGLES, GU_TRIANGLE_STRIP, GU_TRIANGLE_FAN,
                                      GU_POINTS};
    if (!draw_target()) return;
    hle_ge_ser_mark(GS_2D);
    int prim = type >= 1 && type <= 6 ? prim_of[type] : -1;
    int n = gidx || hidx ? ni : nv;
    if (prim < 0 || n <= 0 || n > GE_MAX_VERTS || nv <= 0 || nv > 65536 || verts < HLE_CODE_HI ||
        verts + 0x40u * (uint32_t)nv > hle_ram_end || (gidx && (gidx < HLE_CODE_HI || gidx + 2u * (uint32_t)n > hle_ram_end))) {
        st_bad++;
        return;
    }
    GeTex *t = NULL;
    if (st->raster) {
        t = tex_lookup(st->raster, TL_FULL);               /* 2D always at full size */
        if (!t) { st_tex_skip++; return; }                 /* e.g. the camera raster as a texture (post-FX) */
    }
    frame_begin();
    ge3d_leave();
    list_room((uint32_t)n * sizeof(GeV) + 256u);
    float tw = t ? (float)t->w : 0.0f, th = t ? (float)t->h : 0.0f;
    GeV *v = sceGuGetMemory(n * (int)sizeof(GeV));
    int outside = 0;
    for (int i = 0; i < n; i++) {
        uint32_t j = gidx ? gh(gidx + 2u * (uint32_t)i) : hidx ? hidx[i] : (uint32_t)i;
        if (j >= (uint32_t)nv) j = 0;
        uint32_t p = verts + 0x40u * j;
        GeV o;
        o.u = gfl(p + 0x10) * tw;
        o.v = gfl(p + 0x14) * th;
        o.c = abgr(gfl(p + 0x20), gfl(p + 0x24), gfl(p + 0x28), gfl(p + 0x2C));
        o.x = gfl(p) * sx;
        o.y = gfl(p + 4) * sy;
        o.z = zmap(gfl(p + 8));
        outside |= gb_out(&o);
        v[i] = o;
    }
    int ntri = prim == GU_TRIANGLES ? n / 3 : prim == GU_TRIANGLE_STRIP || prim == GU_TRIANGLE_FAN ? n - 2 : 0;
    if (outside && ntri > 0 && ntri <= 256) {
        /* clip to the guard band: each triangle -> a clipped fan -> a triangle list (v is read back from the uncached
           list; a resync in list_room finishes that list but leaves its bytes untouched until the list after next) */
        list_room((uint32_t)ntri * 15u * sizeof(GeV) + 256u);
        GeV *w = sceGuGetMemory(ntri * 15 * (int)sizeof(GeV));
        int k = 0;
        for (int i = 0; i < ntri; i++) {
            const GeV *a, *b, *c;
            if (prim == GU_TRIANGLES) { a = &v[3 * i]; b = &v[3 * i + 1]; c = &v[3 * i + 2]; }
            else if (prim == GU_TRIANGLE_FAN) { a = &v[0]; b = &v[i + 1]; c = &v[i + 2]; }
            else { a = &v[i]; b = &v[i + 1]; c = &v[i + 2]; }
            if (gb_out(a) || gb_out(b) || gb_out(c)) k += clip_tri(a, b, c, w + k);
            else { w[k++] = *a; w[k++] = *b; w[k++] = *c; }
        }
        st_clipped++;
        apply_state(st, t);
        apply_depth(1);
        if (k) sceGuDrawArray(GU_TRIANGLES, GEV_FMT, k, NULL, w);
        v = w;
        n = k;
    } else {
        if (outside) {                                     /* lines / points / huge primitives: clamp as before */
            st_clip_dropped++;
            for (int i = 0; i < n; i++) { v[i].x = clampc(v[i].x, GB_LO, GB_HI); v[i].y = clampc(v[i].y, GB_LO, GB_HI); }
        }
        apply_state(st, t);
        apply_depth(1);
        sceGuDrawArray(prim, GEV_FMT, n, NULL, v);
    }
    if (opt_trace && st_flips + 1 == opt_trace && n > 0)
        LOG("GE trace: im2d type %d n %d raster %08x -> %s psm %d %ux%u off +%06x, v0 uv %.1f %.1f xyz %.1f %.1f %.0f c "
            "%08x, blend %d/%d filter %d, z test/write %u/%u%s\n", type, n, st->raster, t ? "tex" : "untextured",
            t ? t->psm : -1, t ? t->w : 0, t ? t->h : 0, t ? t->off : 0, v[0].u, v[0].v, v[0].x, v[0].y, v[0].z,
            v[0].c, st->src_blend, st->dst_blend, st->filter, hle_render_rs(RS_ZTESTENABLE),
            hle_render_rs(RS_ZWRITEENABLE), outside ? " (clipped)" : "");
    st_prims++;
    st_verts += (uint64_t)n;
    if (t) t->last_frame = st_presented;
}

void hle_ge_rbuf(uint32_t buf, uint32_t n, const RndState *st)
{
    w3d_defer_flush();                                     /* before any texture lookup here */
    if (!draw_target()) return;
    hle_ge_ser_mark(GS_2D);                                /* render buffer type 0 = font glyphs (HUD) */
    if (!n || n > 256 || buf < HLE_CODE_HI || buf + 16u + 40u * n > hle_ram_end) { st_bad++; return; }
    GeTex *t = NULL;
    if (st->raster) {
        t = tex_lookup(st->raster, TL_FULL);         /* render buffer (fonts): full */
        if (!t) { st_tex_skip++; return; }
    }
    frame_begin();
    ge3d_leave();
    list_room(n * 2u * sizeof(GeV) + 256u);
    apply_state(st, t);
    apply_depth(0);                                        /* the glyph quads: no depth (as before) */
    float tw = t ? (float)t->w : 0.0f, th = t ? (float)t->h : 0.0f;
    GeV *v = sceGuGetMemory((int)(n * 2u * sizeof(GeV)));
    int k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = buf + 16u + 40u * i;
        float cx = gfl(e), cy = gfl(e + 4), hw = gfl(e + 12), hh = gfl(e + 16);
        uint32_t c = gb(e + 28) | (uint32_t)gb(e + 29) << 8 | (uint32_t)gb(e + 30) << 16 | (uint32_t)gb(e + 31) << 24;
        GeV a, b;
        a.u = (int16_t)gh(e + 20) / 4096.0f * tw;
        a.v = (int16_t)gh(e + 22) / 4096.0f * th;
        b.u = (int16_t)gh(e + 24) / 4096.0f * tw;
        b.v = (int16_t)gh(e + 26) / 4096.0f * th;
        a.c = b.c = c;                                     /* a sprite takes its colour from the second vertex */
        a.x = (cx - hw) * sx;
        a.y = (cy - hh) * sy;
        b.x = (cx + hw) * sx;
        b.y = (cy + hh) * sy;
        a.z = b.z = 0.0f;
        if (gb_out(&a) || gb_out(&b)) {                    /* guard band: clip the rectangle, UV follows */
            float *pa[2] = {&a.x, &a.y}, *pb[2] = {&b.x, &b.y}, *ta[2] = {&a.u, &a.v}, *tb[2] = {&b.u, &b.v};
            int gone = 0;
            for (int ax = 0; ax < 2 && !gone; ax++) {
                float p0 = *pa[ax], p1 = *pb[ax], t0 = *ta[ax], t1 = *tb[ax];
                if (!(p0 == p0) || !(p1 == p1) || (p0 < GB_LO && p1 < GB_LO) || (p0 > GB_HI && p1 > GB_HI)) { gone = 1; break; }
                float lo0 = p0 < GB_LO ? GB_LO : p0 > GB_HI ? GB_HI : p0, lo1 = p1 < GB_LO ? GB_LO : p1 > GB_HI ? GB_HI : p1;
                if (p1 != p0) {
                    *ta[ax] = t0 + (t1 - t0) * (lo0 - p0) / (p1 - p0);
                    *tb[ax] = t0 + (t1 - t0) * (lo1 - p0) / (p1 - p0);
                }
                *pa[ax] = lo0;
                *pb[ax] = lo1;
            }
            st_clipped++;
            if (gone) continue;
        }
        v[k++] = a;
        v[k++] = b;
    }
    if (k) sceGuDrawArray(GU_SPRITES, GEV_FMT, k, NULL, v);
    if (opt_trace && st_flips + 1 == opt_trace)
        LOG("GE trace: rbuf n %u raster %08x -> %s psm %d %ux%u\n", n, st->raster, t ? "tex" : "untextured",
            t ? t->psm : -1, t ? t->w : 0, t ? t->h : 0);
    st_quads += n;
    if (t) t->last_frame = st_presented;
}

/* ------------------------------------------------------------------ host-side triangles (the sky)
   n / 3 independent triangles in guest raster coordinates (RndVtx: x, y; u, v 0..1 over the raster; colour 0..255),
   through mode like the 2D path, untextured unless st->raster; vertices outside the guard band are clipped
   (hle_sky.c already clips the sky in view space, so normally none). hle_ge_tris: no depth (the sky is the
   frame's first colour draw); hle_ge_tris_z: per-vertex GE depth (0 = far .. 65535 = near, the 3D world's convention)
   with the Z test / write of the driver's render state (the render buffer's world-space sprites). */
static uint64_t st_w_draws, st_w_tris;

static void w_tris(const RndVtx *tv, const float *zv, int n, const RndState *st)
{
    w3d_defer_flush();                                     /* before any texture lookup here */
    if (!draw_target()) return;
    hle_ge_ser_mark(GS_FX);                                /* sky, sprites, shadows, particles, water */
    n -= n % 3;
    if (n <= 0 || n > GE_MAX_VERTS) { st_bad++; return; }
    GeTex *t = NULL;
    if (st->raster) {
        t = tex_lookup(st->raster, fx_mode());
        if (!t) { st_tex_skip++; return; }
    }
    frame_begin();
    ge3d_leave();
    list_room((uint32_t)n * sizeof(GeV) + 256u);
    float tw = t ? (float)t->w : 0.0f, th = t ? (float)t->h : 0.0f;
    GeV *v = sceGuGetMemory(n * (int)sizeof(GeV));
    int outside = 0;
    for (int i = 0; i < n; i++) {
        GeV o;
        o.u = tv[i].u * tw;
        o.v = tv[i].v * th;
        o.c = abgr(tv[i].r, tv[i].g, tv[i].b, tv[i].a);
        o.x = tv[i].x * sx;
        o.y = tv[i].y * sy;
        o.z = zv ? zv[i] : 0.0f;
        outside |= gb_out(&o);
        v[i] = o;
    }
    if (outside) {
        int ntri = n / 3;
        list_room((uint32_t)ntri * 15u * sizeof(GeV) + 256u);
        GeV *w = sceGuGetMemory(ntri * 15 * (int)sizeof(GeV));
        int k = 0;
        for (int i = 0; i < ntri; i++) {
            const GeV *a = &v[3 * i], *b = &v[3 * i + 1], *c = &v[3 * i + 2];
            if (gb_out(a) || gb_out(b) || gb_out(c)) k += clip_tri(a, b, c, w + k);
            else { w[k++] = *a; w[k++] = *b; w[k++] = *c; }
        }
        st_clipped++;
        v = w;
        n = k;
    }
    apply_state(st, t);
    apply_depth(zv != NULL);
    if (n) sceGuDrawArray(GU_TRIANGLES, GEV_FMT, n, NULL, v);
    st_w_draws++;
    st_w_tris += (uint64_t)n / 3;
    st_prims++;
    st_verts += (uint64_t)n;
    if (t) t->last_frame = st_presented;
}

void hle_ge_tris(const RndVtx *tv, int n, const RndState *st) { w_tris(tv, NULL, n, st); }
void hle_ge_tris_z(const RndVtx *tv, const float *z, int n, const RndState *st) { w_tris(tv, z, n, st); }

/* (the render buffer's world-space triangles: shadows, skid marks): vertices already in GeV layout
   (u, v in texels of the raster's GE texture - hle_ge_tex_size; ABGR colour; x, y in screen pixels inside the guard
   band; z = GE depth) are copied into the list as they are: one memcpy instead of a conversion per vertex. depth != 0:
   the driver's Z test / write of the current render state. */
_Static_assert(sizeof(GeV) == 24, "the sky's packed vertices (hle_sky.c HV) mirror GeV");

int hle_ge_tex_size(uint32_t raster, int *w, int *h)
{
    *w = *h = 0;
    if (!hle_ge_on || !raster) return 0;
    GeTex *t = tex_lookup(raster, fx_mode());          /* = hle_ge_tris_packed */
    if (!t) return 0;
    *w = t->w;
    *h = t->h;
    return 1;
}

void hle_ge_tris_packed(const void *pv, int n, const RndState *st, int depth)
{
    w3d_defer_flush();                                     /* before any texture lookup here */
    if (!draw_target()) return;
    hle_ge_ser_mark(GS_FX);                                /* sky, sprites, shadows, particles, water */
    n -= n % 3;
    if (n <= 0 || n > GE_MAX_VERTS) { st_bad++; return; }
    GeTex *t = NULL;
    if (st->raster) {
        t = tex_lookup(st->raster, fx_mode());
        if (!t) { st_tex_skip++; return; }
    }
    frame_begin();
    ge3d_leave();
    list_room((uint32_t)n * sizeof(GeV) + 256u);
    GeV *v = sceGuGetMemory(n * (int)sizeof(GeV));
    memcpy(v, pv, (size_t)n * sizeof(GeV));
    apply_state(st, t);
    apply_depth(depth != 0);
    sceGuDrawArray(GU_TRIANGLES, GEV_FMT, n, NULL, v);
    st_w_draws++;
    st_w_tris += (uint64_t)n / 3;
    st_prims++;
    st_verts += (uint64_t)n;
    if (t) t->last_frame = st_presented;
}

/* ------------------------------------------------------------------ the PS2 colour filter
   (hle_postfx.c): P x k per channel, drawn where the guest calls its filter (after the
   3D world / sky / effects, before the HUD). One untextured full-screen sprite per factor of 2, blend Cd x src + Cd x FIX
   (the GE: ((2c+1)(2f+1)) >> 10 per term, one clamp; FIX 0xFF is an exact x1, FIX 0 adds 0): m <= 1: src = m, FIX 0;
   1 < m <= 2: src = m - 1, FIX 255; a channel already done is x1 (src 0, FIX 255). Z, texture and alpha test off. */
int hle_ge_postfx_mul(const float k[3], int dither)
{
    if (!draw_target()) return -1;
    hle_ge_ser_mark(GS_PFX);
    float rem[3] = {k[0], k[1], k[2]};
    int np = 0;
    for (int pass = 0; pass < 3; pass++) {
        uint32_t src = 0, fix = 0;
        int ident = 1;
        float m[3];
        for (int i = 0; i < 3; i++) {
            float r = rem[i] >= 0.0f ? rem[i] : 0.0f;                 /* also NaN */
            m[i] = r > 2.0f ? 2.0f : r;
            int s, f = 0;
            if (m[i] > 1.0f) { f = 255; s = (int)((m[i] - 1.0f) * 256.0f); }   /* factor (2s+1)/512 */
            else s = (int)(m[i] * 256.0f);
            if (s > 255) s = 255;
            if (!(f == 255 && s == 0) && !(f == 0 && s == 255)) ident = 0;   /* x1 within the GE's 1/256 steps */
            src |= (uint32_t)s << (8 * i);
            fix |= (uint32_t)f << (8 * i);
        }
        if (ident) break;
        if (!np) {
            frame_begin();
            ge3d_leave();
            list_room(512);
            if (cur_texon != 0) { sceGuDisable(GU_TEXTURE_2D); cur_texon = 0; }
            apply_depth(0);
            sceGuDisable(GU_ALPHA_TEST);
            sceGuEnable(GU_BLEND);
            if (dither) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER);
        }
        sceGuBlendFunc(GU_ADD, GU_OTHER_COLOR, GU_FIX, 0, fix);         /* Cd x src + Cd x FIX */
        struct PV { uint32_t c; int16_t x, y, z, pad; } *v = sceGuGetMemory(2 * sizeof(struct PV));
        v[0].c = v[1].c = 0xFF000000u | src;                            /* a sprite: the second vertex's colour */
        v[0].x = 0; v[0].y = 0; v[0].z = 0; v[0].pad = 0;
        v[1].x = SCR_W; v[1].y = SCR_H; v[1].z = 0; v[1].pad = 0;
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        np++;
        for (int i = 0; i < 3; i++) rem[i] = m[i] > 0.0f ? rem[i] / m[i] : 1.0f;
    }
    if (np) cur_sb = cur_db = -1;                               /* the next 2D draw sets its blend / dither again */
    return np;
}

/* the PS2 radiosity 0x518280 (hle_postfx.c): P downsampled twice
   (bilinear 2:1 = a 2x2 box each) into R1 240x136 / R2 120x68 (5650 render targets in the free VRAM above the debug
   console T, 0x198000..0x1AD500), bright pass R2 = max(0, D - limit / 2) (R2 cleared to limit / 2, then the downsample
   drawn with GU_SUBTRACT: Cs - Cd), composite P += min(255, 2 R2) x intensity / 256 (GU_FRAGMENT_2X doubles the
   texel: the PS2's clamp(2D - limit) exactly; bilinear 4x upscale, GU_ADD FIX / FIX 0xFF) render_passes times.
   Every render target is sampled after TEXFLUSH + TEXSYNC in the same list (the pspsdk rendertarget sample's pattern;
   later primitives see earlier ones' pixels after a TEXFLUSH); fence = 1 (ps2rc.cfg postfx_rad_fence=1, a fallback)
   finishes and syncs the list (resync) before each target is sampled - PPSSPP gave pixel-identical frames both ways.
   filter_passes 0 / 1 (the darkness
   filter's call) are drawn with the two levels. Returns the number of composite passes (0 = nothing, -1 = not the
   screen). */
#define RAD1_OFF 0x198000u                                  /* 240x136, stride 256 */
#define RAD2_OFF (RAD1_OFF + 256u * 136u * 2u)              /* 120x68 (+ a zero row / columns), stride 128 */

static void rad_strips(int dw, int dh, float sw, float sh)
{
    struct RV { float u, v; float x, y, z; } *v = sceGuGetMemory(2 * 16 * (int)sizeof(struct RV));
    int n = 0;
    for (int x = 0; x < dw; x += 32) {                     /* 32-pixel strips: texture-cache friendly */
        int x1 = x + 32 < dw ? x + 32 : dw;
        v[n].u = (float)x * sw / (float)dw; v[n].v = 0.0f; v[n].x = (float)x; v[n].y = 0.0f; v[n].z = 0.0f; n++;
        v[n].u = (float)x1 * sw / (float)dw; v[n].v = sh; v[n].x = (float)x1; v[n].y = (float)dh; v[n].z = 0.0f; n++;
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, n, NULL, v);
}

static void rad_target(uint32_t off, int stride, int w, int h, uint32_t tex_off, int tw, int tbw)
{
    sceGuDrawBufferList(GU_PSM_5650, (void *)off, stride);
    sceGuScissor(0, 0, w, h);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DITHER);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, tw, tw, tbw, (void *)((uintptr_t)sceGeEdramGetAddr() + tex_off));
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuTexSync();
}

/* GE time probe - finish + sync the open frame list (if any) and return the microseconds the CPU waited for
   the GE to drain it (0 when no frame is open). hle_postfx.c brackets a sampled post-effect pass with two probes: the
   first drains the earlier work, the second measures the pass (+ the list restart, a few us). */
uint32_t hle_ge_postfx_probe(void)
{
    if (!hle_ge_on || !frame_open) return 0;
    SceUInt t0 = sceKernelGetSystemTimeLow();
    resync();
    return (uint32_t)(sceKernelGetSystemTimeLow() - t0);
}

int hle_ge_postfx_radiosity(int limit, int filter_passes, int render_passes, int intensity, int fence)
{
    if (!draw_target()) return -1;
    hle_ge_ser_mark(GS_PFX);
    int fix = intensity / 2;                               /* the PS2's ALPHA FIX (x / 128) */
    if (fix <= 0 || render_passes <= 0) return 0;
    if (render_passes > 4) render_passes = 4;
    if (limit < 0) limit = 0;
    if (limit > 255) limit = 255;
    (void)filter_passes;
    frame_begin();
    ge3d_leave();
    if (fence) resync();                                   /* P complete in VRAM before it is sampled */
    list_room(4096);
    /* 1. P 480x272 -> R1 240x136 */
    rad_target(RAD1_OFF, 256, 240, 136, P_OFF, 512, FB_STRIDE);
    sceGuDisable(GU_BLEND);
    rad_strips(240, 136, (float)SCR_W, (float)SCR_H);
    if (fence) resync();
    /* 2. R2 = limit / 2, then R1 -> R2 120x68 with Cs - Cd (clamped at 0) */
    rad_target(RAD2_OFF, 128, 128, 69, RAD1_OFF, 256, 256);
    clear_rect(0xFF000000u, GU_COLOR_BUFFER_BIT);          /* the texels the bilinear upscale reads past 120x68 */
    sceGuScissor(0, 0, 120, 68);
    uint32_t h = (uint32_t)limit / 2u;
    clear_rect(0xFF000000u | h << 16 | h << 8 | h, GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
    rad_strips(120, 68, 240.0f, 136.0f);
    if (fence) resync();
    /* 3. P += min(255, 2 R2) x fix / 128 (GE: ((2c+1)(2f+1)) >> 10, f = 2 fix ~ fix / 128) */
    rad_target(P_OFF, FB_STRIDE, SCR_W, SCR_H, RAD2_OFF, 128, 128);
    sceGuEnable(GU_FRAGMENT_2X);
    unsigned f = (unsigned)(2 * fix) > 255u ? 255u : (unsigned)(2 * fix);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, f | f << 8 | f << 16, 0xFFFFFF);
    for (int i = 0; i < render_passes; i++) rad_strips(SCR_W, SCR_H, 120.0f, 68.0f);
    sceGuDisable(GU_FRAGMENT_2X);
    /* back to hle_ge.c's 2D state (state_reset's texture function, everything re-sent by the next draw) */
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuEnable(GU_DITHER);
    cur_tex = NULL;
    cur_sb = cur_db = cur_filter = cur_wrap = -1;
    cur_texon = 1;
    cur_zmode = 0;
    return render_passes;
}

/* ------------------------------------------------------------------ the 3D world (psp/hle_ge3d.c)
   The 3D draws go into the same list and picture P as the 2D path: these functions give hle_ge3d.c the frame / list /
   texture cache of this file. A list start (state_reset) or a 2D draw (ge3d_leave) drops the 3D state; hle_ge3d.c asks
   hle_ge_3d_need_state() before drawing and sends it again. */
int hle_ge_3d_begin(void)
{
    if (!draw_target()) return 0;
    frame_begin();
    return 1;
}

int hle_ge_3d_need_state(void)
{
    if (ge3d_active) return 0;
    ge3d_active = 1;
    cur_sb = cur_db = -1;                                  /* the 3D path sets blend / alpha test itself */
    return 1;
}

void hle_ge_3d_room(uint32_t bytes) { list_room(bytes); }
uint32_t hle_ge_3d_epoch(void) { return epoch; }

int hle_ge_3d_texture(uint32_t raster, int filter, int addr_u, int addr_v, int *aclass)
{
    *aclass = 0;
    if (!raster) {
        if (cur_texon != 0) { sceGuDisable(GU_TEXTURE_2D); cur_texon = 0; }
        return 1;
    }
    GeTex *t = tex_lookup(raster, TL_HALF);            /* 3D (main core) */
    if (!t) { st_tex_skip++; return 0; }
    RndState st;
    memset(&st, 0, sizeof st);
    st.filter = filter;
    st.addr_u = addr_u;
    st.addr_v = addr_v;
    st.src_blend = cur_sb > 0 ? cur_sb : 5;               /* apply_state leaves the blend as it is */
    st.dst_blend = cur_db > 0 ? cur_db : 6;
    want_mip = 1;
    apply_state(&st, t);
    want_mip = 0;
    t->last_frame = st_presented;
    *aclass = t->aclass == TC_ALPHA_OPAQUE ? 0 : t->aclass == TC_ALPHA_CUTOUT ? 1 : 2;
    return 1;
}

/* w3d_me (hle_w3d_me.h): the texture of a mesh whose GE words the Media Engine writes - the
   lookup of hle_ge_3d_texture (may convert, may resync) without binding anything; the slot is held until
   hle_ge_3d_hold(0), which pins the held ones to the epoch whose list now calls the ME's words */
int hle_ge_3d_tex_info(uint32_t raster, HleGeTexInfo *ti)
{
    memset(ti, 0, sizeof *ti);
    if (!raster) { ti->ok = 1; return 1; }
    GeTex *t = tex_lookup(raster, TL_HALF);            /* 3D (ME batches) */
    if (!t) { st_tex_skip++; return 0; }
    if (w3d_hold_gen) t->w3d_hold = w3d_hold_gen;
    ws_count(t);
    uint8_t *base = arena + t->off;
    ti->ok = 1;
    ti->aclass = t->aclass == TC_ALPHA_OPAQUE ? 0 : t->aclass == TC_ALPHA_CUTOUT ? 1 : 2;
    ti->tbp = (uint32_t)(uintptr_t)(base + t->lvl_off);
    ti->psm = t->psm;
    ti->swz = t->swz;
    ti->w = t->w;
    ti->h = t->h;
    ti->bufw = t->bufw;
    ti->mx = tex_mx(t);
    ti->lbias = (uint32_t)tex_lbias(t);
    ti->mipf = opt_tex_mip_filter ? 7u : 5u;
    bk_bind_count(ti->mx);
    if (t->clut_n) {
        ti->cbp = (uint32_t)(uintptr_t)(base + t->clut_off);
        ti->clut_psm = t->clut_psm;
        ti->clut_blocks = t->clut_psm == GU_PSM_8888 ? t->clut_n / 8u : t->clut_n / 16u;
    }
    return 1;
}

void hle_ge_3d_hold(int on)
{
    static uint32_t ctr;
    if (on) {
        if (++ctr == 0) ctr = 1;
        w3d_hold_gen = ctr;
        return;
    }
    if (!w3d_hold_gen) return;
    for (int i = 0; i < MAX_TEX; i++)
        if (tex[i].w3d_hold == w3d_hold_gen) { tex[i].pin = epoch; tex[i].w3d_hold = 0; }
    w3d_hold_gen = 0;
}

void hle_ge_3d_forget(void)
{
    cur_tex = NULL;
    cur_sb = cur_db = cur_filter = cur_wrap = cur_texon = -1;
}

void hle_ge_3d_blend(int on, int sb, int db)
{
    if (!on) {
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_DITHER);
        cur_sb = 2;                                        /* ONE / ZERO = what apply_state would call opaque */
        cur_db = 1;
        return;
    }
    set_blend(sb, db);                                     /* (also sets an alpha test: hle_ge3d.c sets its own after) */
    cur_sb = sb;
    cur_db = db;
}

/* ------------------------------------------------------------------ the text overlay */
static uint8_t font_tex[128 * 128 / 2] __attribute__((aligned(64)));   /* T4, 16 x 16 glyphs of 8x8 */
static uint32_t font_clut[16] __attribute__((aligned(64)));
extern unsigned char msx[];                                  /* libpspdebug's 8x8 font (the debug screen's) */

static void font_build(void)
{
    memset(font_tex, 0, sizeof font_tex);
    for (int ch = 0; ch < 256; ch++)
        for (int y = 0; y < 8; y++) {
            unsigned bits = msx[ch * 8 + y];
            for (int x = 0; x < 8; x++)
                if (bits & (0x80u >> x)) {
                    int px = (ch & 15) * 8 + x, py = (ch >> 4) * 8 + y;
                    font_tex[py * 64 + px / 2] |= (uint8_t)(px & 1 ? 0x10 : 0x01);
                }
        }
    font_clut[0] = 0;
    for (int i = 1; i < 16; i++) font_clut[i] = 0xFFFFFFFFu;
    sceKernelDcacheWritebackRange(font_tex, sizeof font_tex);
    sceKernelDcacheWritebackRange(font_clut, sizeof font_clut);
}

/* hold L + R + SELECT and press TRIANGLE to hide / show the log text over the picture (ps2rc.cfg ge_text = the rows
   shown and the start state; ge_text=0 starts hidden and shows 12 rows when switched on). The game sees those
   buttons too: with SELECT held hle_pad.c turns L / R into L2 / R2, so it gets SELECT + L2 + R2 for that moment. */
static int text_on = -1, text_rows_on;
static uint32_t text_prev_buttons;

static void text_toggle_poll(void)
{
    if (text_on < 0) {
        text_on = opt_text_rows > 0;
        text_rows_on = opt_text_rows > 0 ? opt_text_rows : 12;
    }
    SceCtrlData p;
    if (sceCtrlPeekBufferPositive(&p, 1) <= 0) return;
    uint32_t b = p.Buttons;
    /* L + R + SELECT is the fps counter (fps_toggle_poll); the log text toggle is L + R + SELECT held, then TRIANGLE
       (the game sees SELECT + L2 + R2 + L3 for that moment). */
    uint32_t lrs = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
    if ((b & PSP_CTRL_TRIANGLE) && !(text_prev_buttons & PSP_CTRL_TRIANGLE) && (b & lrs) == lrs) {
        text_on = !text_on;
        LOG("GE: log text over the picture %s (L+R+SELECT+TRIANGLE, flip %llu)\n", text_on ? "on" : "off",
            (unsigned long long)st_flips);
    }
    text_prev_buttons = b;
}

static void overlay(void)
{
    text_toggle_poll();
    int rows = text_on ? (text_rows_on > 34 ? 34 : text_rows_on) : 0;
    if (rows <= 0) return;
    static char snap[34][CON_COLS];
    static uint8_t len[34];
    int n = con_snapshot(rows, snap, len, NULL), ng = 0, nb = 0;
    for (int r = 0; r < n; r++) {
        if (len[r]) nb++;
        for (int i = 0; i < len[r]; i++) ng += snap[r][i] != ' ';
    }
    if (!nb) return;
    list_room((uint32_t)nb * 12u + (uint32_t)ng * 20u + 512u);
    sceGuDrawBufferList(GU_PSM_5650, (void *)fb_off[draw_buf], FB_STRIDE);   /* a resync above restarts on P */
    int y0 = SCR_H - n * 8;
    ge3d_leave();                                          /* no 3D / 2D depth state under the log text */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    cur_zmode = 0;
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DITHER);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(0xA0000000u);                               /* row backgrounds: black, 63 % */
    struct BV { int16_t x, y, z; } *bv = sceGuGetMemory(nb * 2 * (int)sizeof(struct BV) + 4);
    int k = 0;
    for (int r = 0; r < n; r++) {
        if (!len[r]) continue;
        bv[k].x = 0; bv[k].y = (int16_t)(y0 + r * 8); bv[k].z = 0; k++;
        bv[k].x = (int16_t)(len[r] * 7 + 2); bv[k].y = (int16_t)(y0 + r * 8 + 8); bv[k].z = 0; k++;
    }
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, k, NULL, bv);
    if (ng) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_T4, 0, 0, 0);
        sceGuTexImage(0, 128, 128, 128, font_tex);
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(2, font_clut);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
        sceGuColor(0xFFFFFFFFu);
        struct TV { uint16_t u, v; int16_t x, y, z; } *tv = sceGuGetMemory(ng * 2 * (int)sizeof(struct TV) + 4);
        k = 0;
        for (int r = 0; r < n; r++)
            for (int i = 0; i < len[r]; i++) {
                unsigned ch = (unsigned char)snap[r][i];
                if (ch == ' ' || k + 2 > ng * 2) continue;
                uint16_t u = (uint16_t)((ch & 15) * 8), v = (uint16_t)((ch >> 4) * 8);
                int16_t x = (int16_t)(1 + i * 7), y = (int16_t)(y0 + r * 8);
                tv[k].u = u; tv[k].v = v; tv[k].x = x; tv[k].y = y; tv[k].z = 0; k++;
                tv[k].u = (uint16_t)(u + 7); tv[k].v = (uint16_t)(v + 8); tv[k].x = (int16_t)(x + 7);
                tv[k].y = (int16_t)(y + 8); tv[k].z = 0; k++;
            }
        if (k) sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k, NULL, tv);
    }
    cur_tex = NULL;
    cur_sb = cur_db = cur_filter = cur_wrap = cur_texon = -1;
}

/* ps2rc.cfg fps=1 shows the frame rate in the top-left corner (default 0 = off): frames shown
   per real second over the last half second and their mean time, e.g. "14.7 FPS  68 ms". Same font and state as the
   log text above; drawn after it, so it stays on top. Hold L + R and press SELECT to switch it on / off at run time
   (L + R + Down + START would open the CFW menu). The game sees SELECT + L2 + R2 for that moment (hle_pad.c: SELECT
   is the modifier), as with the log-text toggle. */
static int opt_fps;
static uint64_t fps_t0, fps_flips0;
static char fps_txt[32];
static int fps_len;
static uint32_t fps_prev_buttons;

static void fps_toggle_poll(void)
{
    SceCtrlData p;
    if (sceCtrlPeekBufferPositive(&p, 1) <= 0) return;
    uint32_t b = p.Buttons, lr = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
    if ((b & PSP_CTRL_SELECT) && !(fps_prev_buttons & PSP_CTRL_SELECT) && (b & lr) == lr) {
        opt_fps = !opt_fps;
        fps_t0 = 0;
        fps_len = 0;
        LOG("GE: fps counter %s (L+R+SELECT, flip %llu)\n", opt_fps ? "on" : "off", (unsigned long long)st_flips);
    }
    fps_prev_buttons = b;
}

static void fps_draw(void)
{
    fps_toggle_poll();
    if (!opt_fps) return;
    uint64_t now = sceKernelGetSystemTimeWide();
    if (!fps_t0) { fps_t0 = now; fps_flips0 = st_flips; }
    else if (now - fps_t0 >= 500000u) {
        uint64_t n = st_flips - fps_flips0, dt = now - fps_t0;
        fps_len = snprintf(fps_txt, sizeof fps_txt, "%4.1f FPS %3.0f ms", (double)n * 1e6 / (double)dt,
                           n ? (double)dt / 1000.0 / (double)n : 0.0);
        if (fps_len < 0 || fps_len >= (int)sizeof fps_txt) fps_len = 0;
        fps_t0 = now;
        fps_flips0 = st_flips;
    }
    if (fps_len <= 0) return;
    int ng = 0;
    for (int i = 0; i < fps_len; i++) ng += fps_txt[i] != ' ';
    list_room((uint32_t)ng * 20u + 512u);
    sceGuDrawBufferList(GU_PSM_5650, (void *)fb_off[draw_buf], FB_STRIDE);   /* a resync above restarts on P */
    ge3d_leave();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    cur_zmode = 0;
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DITHER);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(0xA0000000u);
    struct BV { int16_t x, y, z; } *bv = sceGuGetMemory(2 * (int)sizeof(struct BV) + 4);
    bv[0].x = 0; bv[0].y = 0; bv[0].z = 0;
    bv[1].x = (int16_t)(fps_len * 7 + 3); bv[1].y = 10; bv[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, bv);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, 128, 128, 128, font_tex);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(2, font_clut);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuColor(0xFF40FF40u);                               /* green text */
    struct TV { uint16_t u, v; int16_t x, y, z; } *tv = sceGuGetMemory(ng * 2 * (int)sizeof(struct TV) + 4);
    int k = 0;
    for (int i = 0; i < fps_len; i++) {
        unsigned ch = (unsigned char)fps_txt[i];
        if (ch == ' ') continue;
        uint16_t u = (uint16_t)((ch & 15) * 8), v = (uint16_t)((ch >> 4) * 8);
        int16_t x = (int16_t)(2 + i * 7), y = 1;
        tv[k].u = u; tv[k].v = v; tv[k].x = x; tv[k].y = y; tv[k].z = 0; k++;
        tv[k].u = (uint16_t)(u + 7); tv[k].v = (uint16_t)(v + 8); tv[k].x = (int16_t)(x + 7);
        tv[k].y = (int16_t)(y + 8); tv[k].z = 0; k++;
    }
    if (k) sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, k, NULL, tv);
    cur_tex = NULL;
    cur_sb = cur_db = cur_filter = cur_wrap = cur_texon = -1;
}

/* ------------------------------------------------------------------ frame dumps (ps2rc.cfg ge_dump=..., PPSSPP checks)
   The frame is read back with a GE block transfer into RAM (a temporary arena block): PPSSPP's hardware backends keep
   GE output on the host GPU and only write it back to emulated VRAM for such transfers, so a CPU read of VRAM sees
   stale bytes there; on the PSP the transfer is equally correct. */
static void dump_frame(uint32_t src_off, const char *name, uint64_t flip)
{
    char path[320];
    uint32_t off;
    af_settle();
    if (frame_open || !arena_get(FB_BYTES, &off)) { LOG("GE dump: flip %llu skipped (no room)\n", (unsigned long long)flip); return; }
    uint8_t *ram = arena + off;
    sceKernelDcacheWritebackInvalidateRange(ram, FB_BYTES);
    sceGuStart(GU_DIRECT, list_u[cur_list]);
    sceGuCopyImage(GU_PSM_5650, 0, 0, SCR_W, SCR_H, FB_STRIDE, (void *)((uintptr_t)sceGeEdramGetAddr() + src_off),
                   0, 0, FB_STRIDE, ram);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    sceKernelDcacheInvalidateRange(ram, FB_BYTES);
    snprintf(path, sizeof path, "%s%s_%06llu.bmp", hle_plat_disc_root(), name, (unsigned long long)flip);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { LOG("GE dump: cannot create %s (%08x)\n", path, (unsigned)fd); arena_free(off); return; }
    uint8_t h[54];
    uint32_t img = SCR_W * 3 * SCR_H;
    memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M';
    uint32_t v4[] = {54 + img, 0, 54, 40, SCR_W, SCR_H};
    memcpy(h + 2, &v4[0], 4); memcpy(h + 10, &v4[2], 4); memcpy(h + 14, &v4[3], 4);
    memcpy(h + 18, &v4[4], 4); memcpy(h + 22, &v4[5], 4);
    h[26] = 1; h[28] = 24;
    memcpy(h + 34, &img, 4);
    sceIoWrite(fd, h, sizeof h);
    static uint8_t rows[16 * SCR_W * 3];
    const uint16_t *fb = (const uint16_t *)ram;
    for (int y0 = SCR_H - 1; y0 >= 0; y0 -= 16) {
        int k = 0;
        for (int y = y0; y > y0 - 16 && y >= 0; y--)
            for (int x = 0; x < SCR_W; x++) {
                unsigned p = fb[y * FB_STRIDE + x];
                rows[k++] = (uint8_t)((p >> 11) << 3 | (p >> 13));            /* B */
                rows[k++] = (uint8_t)(((p >> 5) & 63) << 2 | ((p >> 9) & 3));  /* G */
                rows[k++] = (uint8_t)((p & 31) << 3 | ((p >> 2) & 7));         /* R */
            }
        sceIoWrite(fd, rows, k);
    }
    sceIoClose(fd);
    arena_free(off);
    st_dumps++;
    LOG("GE dump: flip %llu -> %s\n", (unsigned long long)flip, path);
}

/* ------------------------------------------------------------------ flip */
static void present(int b)
{
    int locked = ge_lock(0);
    if (scr != SCR_FINAL) {
        void *addr = (void *)((uintptr_t)sceGeEdramGetAddr() + fb_off[b]);
        if (first_present) {
            sceDisplaySetMode(0, SCR_W, SCR_H);
            sceDisplaySetFrameBuf(addr, FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
            sceDisplayWaitVblankStart();                   /* once: the 8888 boot screen leaves VRAM 0..0x88000 */
            disp = b;
            pend = -1;
            first_present = 0;
        } else {
            unsigned vb = sceDisplayGetVcount();
            sceDisplaySetFrameBuf(addr, FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
            unsigned va = sceDisplayGetVcount();
            if (pend >= 0 && (vb != pend_vc || va != vb)) disp = pend;   /* the old pending one was (maybe) shown */
            pend = b;
            pend_vc = va;
        }
        if (scr != SCR_GE) scr = SCR_GE;                   /* the boot console / a stall view ends here */
        last_present_us = sceKernelGetSystemTimeWide();
        st_presented++;
    }
    ge_unlock(locked);
}

void hle_ge_flip(uint32_t raster)
{
    if (raster && raster < hle_ram_end - 0x40u) {
        if (screen_raster != raster) {
            int w = (int)gw(raster + 0x0C), h = (int)gw(raster + 0x10);
            if (w > 0 && h > 0 && w <= 2048 && h <= 2048) { sx = (float)SCR_W / (float)w; sy = (float)SCR_H / (float)h; }
            if (hle_ge_on) LOG("GE screen raster %08x %dx%d -> %dx%d\n", raster, w, h, SCR_W, SCR_H);
        }
        screen_raster = raster;
    }
    if (!hle_ge_on) return;
    st_flips++;
    if (ws_n) {                                            /* the frame's texture working set */
        ws_tot_bytes += ws_bytes;
        ws_tot_n += ws_n;
        ws_frames++;
        if (ws_bytes > ws_max_bytes) ws_max_bytes = ws_bytes;
        int bv = bj_v();                                   /* per A/B variant, counted iterations */
        if (bv >= 0) {
            bj_ws_bytes[bv] += ws_bytes;
            bj_ws_n[bv] += ws_n;
            bj_ws_frames[bv]++;
            if (ws_bytes > bj_ws_max[bv]) bj_ws_max[bv] = ws_bytes;
            bj_arena[bv] += arena_used;
            bk_ws_l0[bv] += ws_l0_bytes;
        }
    }
    ws_bytes = ws_n = ws_l0_bytes = 0;
    st_flips_ws = st_flips;
    if (scr == SCR_FINAL) {
        if (frame_open) { list_sync(); frame_open = 0; }
        ser_frame = 0;
        af_settle();
        return;
    }
    SceUInt f0 = sceKernelGetSystemTimeLow();
    hle_ge_ser_mark(GS_FLIP);                              /* the copy P -> display buffer + the text */
    frame_begin();                                         /* a frame without draws: P as it was + the text */
    ge3d_leave();
    /* P -> the display buffer drawn next (neither displayed nor pending), the log text over it, present */
    draw_buf = next_draw_buffer();
    list_room(1024);
    uintptr_t vram = (uintptr_t)sceGeEdramGetAddr();
    sceGuCopyImage(GU_PSM_5650, 0, 0, SCR_W, SCR_H, FB_STRIDE, (void *)(vram + P_OFF), 0, 0, FB_STRIDE,
                   (void *)(vram + fb_off[draw_buf]));
    sceGuTexSync();
    sceGuDrawBufferList(GU_PSM_5650, (void *)fb_off[draw_buf], FB_STRIDE);
    overlay();
    fps_draw();
    if (!ser_frame && !first_present && opt_ndump == 0 && af_use()) {   /* async flip - no GE wait */
        uint32_t used = (uint32_t)sceGuCheckList();
        if (used > st_list_max) st_list_max = used;
        sceGuFinish();                                     /* (kicks the GE to the list end) */
        af_id = ge_list_executed[0];
        af_buf = draw_buf;
        af_pending = 1;
        frame_open = 0;
        open_us += sceKernelGetSystemTimeLow() - open_t0;
        sceKernelSignalSema(af_go, 1);
        st_aflips++;
        SceUInt fdt = sceKernelGetSystemTimeLow() - f0;
        flip_us += fdt;
        if (fdt > flip_us_max) flip_us_max = fdt;
        return;
    }
    list_sync();
    frame_open = 0;
    open_us += sceKernelGetSystemTimeLow() - open_t0;
    if (ser_frame) {                                       /* the probe frame is complete */
        ser_frames[ser_v]++;
        ser_cpu_us[ser_v] += sceKernelGetSystemTimeLow() - ser_t0;
        if (ser_fr_us > ser_us_max[ser_v]) ser_us_max[ser_v] = ser_fr_us;
        ser_frame = 0;
        /* one line per probe frame (a log script can split them by scene): us per category */
        LOG("GE ser it %llu v%d: %u us = %u %u %u %u %u %u %u %u (other clear 2d w3d-me w3d-main fx postfx flip), "
            "w3d-main marks %llu\n", (unsigned long long)hle_mainloop_iters, ser_v, (unsigned)ser_fr_us,
            (unsigned)(ser_us[ser_v][0] - ser_prev[0]), (unsigned)(ser_us[ser_v][1] - ser_prev[1]),
            (unsigned)(ser_us[ser_v][2] - ser_prev[2]), (unsigned)(ser_us[ser_v][3] - ser_prev[3]),
            (unsigned)(ser_us[ser_v][4] - ser_prev[4]), (unsigned)(ser_us[ser_v][5] - ser_prev[5]),
            (unsigned)(ser_us[ser_v][6] - ser_prev[6]), (unsigned)(ser_us[ser_v][7] - ser_prev[7]),
            (unsigned long long)(ser_marks[ser_v][GS_W3DM] - ser_mprev));
    }
    for (int i = 0; i < opt_ndump; i++)
        if (opt_dump[i] == st_flips) {
            dump_frame(P_OFF, "ge_frame", st_flips);               /* the game picture */
            dump_frame(fb_off[draw_buf], "ge_disp", st_flips);     /* what the screen shows (+ the log text) */
        }
    present(draw_buf);
    SceUInt fdt = sceKernelGetSystemTimeLow() - f0;
    flip_us += fdt;
    if (fdt > flip_us_max) flip_us_max = fdt;
    if (st_flips <= 4 || (st_flips & 1023) == 0) {
        LOG("GE time after %llu flips: waiting for the GE %.3f ms/flip (max %u us, incl. mid-frame resyncs), flip "
            "%.3f ms/flip (max %u us: copy to the display buffer, log text, sync, present)\n",
            (unsigned long long)st_flips, (double)sync_us / 1000.0 / (double)st_flips, sync_us_max,
            (double)flip_us / 1000.0 / (double)st_flips, flip_us_max);
        LOG("GE frame %llu: %llu prims (%llu vertices), %llu quads, %llu clears, %llu binds, textures %llu converted "
            "(%llu KB, %u ms) %llu failed %llu skipped, arena %u/%u KB (peak %u), list max %u KB, resyncs %llu, "
            "evictions %llu, off-screen %llu, bad %llu\n", (unsigned long long)st_flips, (unsigned long long)st_prims,
            (unsigned long long)st_verts, (unsigned long long)st_quads, (unsigned long long)st_clears,
            (unsigned long long)st_binds, (unsigned long long)st_conv, (unsigned long long)(st_conv_bytes >> 10),
            conv_us / 1000u, (unsigned long long)st_conv_fail, (unsigned long long)st_tex_skip, arena_used >> 10,
            arena_bytes >> 10, st_arena_peak >> 10, st_list_max >> 10, (unsigned long long)st_resync,
            (unsigned long long)st_evict, (unsigned long long)st_offscreen, (unsigned long long)st_bad);
    }
}

/* ------------------------------------------------------------------ FMV pictures (psp/hle_movie.c)
   While the movie player plays a movie the game thread is inside its hook, so no guest frame is open or
   drawn. Each movie picture (decoded by the Media Engine into RAM: 480x272 in a 512-pixel-wide buffer) goes into the
   display buffer drawn next and is shown by the same present() as a guest frame: one displayed / pending bookkeeping,
   and the first present after the 8888 boot screen waits its vblank as before. RGBA 8888 pictures are drawn as a
   texture in 64-pixel strips (the 8 KB texture cache) into the 5650 buffer, dithered; RGB 565 ones are block-copied.
   No log text over the movie (the console ring keeps recording; P is not touched). pic NULL = a black frame;
   dump != 0: the display buffer is also written to movie_disp_<dump>.bmp (dump_frame). */
static uint64_t st_movie_frames;

int hle_ge_movie_begin(void)
{
    if (!hle_ge_on || scr == SCR_FINAL) return 0;
    af_settle();
    if (frame_open) { list_sync(); frame_open = 0; }
    return 1;
}

void hle_ge_movie_frame(const void *pic, int rgba8888, uint32_t dump)
{
    if (!hle_ge_on || scr == SCR_FINAL) return;
    af_settle();
    if (frame_open) { list_sync(); frame_open = 0; }
    int b = next_draw_buffer();
    list_begin();
    sceGuDrawBufferList(GU_PSM_5650, (void *)fb_off[b], FB_STRIDE);
    if (!pic) {
        clear_rect(0xFF000000u, GU_COLOR_BUFFER_BIT);
    } else if (!rgba8888) {
        sceGuCopyImage(GU_PSM_5650, 0, 0, SCR_W, SCR_H, FB_STRIDE, (void *)pic, 0, 0, FB_STRIDE,
                       (void *)((uintptr_t)sceGeEdramGetAddr() + fb_off[b]));
        sceGuTexSync();
    } else {
        struct MV { int16_t u, v, x, y, z; } *v = sceGuGetMemory(16 * sizeof(struct MV));
        int n = 0;
        for (int x = 0; x < SCR_W; x += 64) {
            int x1 = x + 64 > SCR_W ? SCR_W : x + 64;
            v[n].u = (int16_t)x; v[n].v = 0; v[n].x = (int16_t)x; v[n].y = 0; v[n].z = 0; n++;
            v[n].u = (int16_t)x1; v[n].v = SCR_H; v[n].x = (int16_t)x1; v[n].y = SCR_H; v[n].z = 0; n++;
        }
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 512, 512, FB_STRIDE, pic);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, n, NULL, v);
    }
    list_sync();                                           /* the GE has read the picture before the next decode */
    if (dump) dump_frame(fb_off[b], "movie_disp", dump);   /* ps2rc.cfg movie_dump=... (PPSSPP checks only) */
    present(b);
    st_movie_frames++;
}

void hle_ge_movie_end(void) { hle_ge_movie_frame(NULL, 0, 0); }   /* black until the guest's first frame */

/* ------------------------------------------------------------------ init / report */
static void read_cfg(void)
{
    for (int ci = 0; ci < hle_cfg_count(); ci++) {    /* the shared table (hle_cfg.c), whole file */
        {
            const char *k = hle_cfg_key(ci), *v = hle_cfg_val(ci);
            if (!strcmp(k, "render")) opt_render_ge = strcmp(v, "text") != 0;
            else if (!strcmp(k, "ge_text")) opt_text_rows = atoi(v);
            else if (!strcmp(k, "fps")) opt_fps = atoi(v);
            else if (!strcmp(k, "ge_tex_kb")) opt_tex_kb = atoi(v);
            else if (!strcmp(k, "ge_clear")) opt_clear = atoi(v);
            else if (!strcmp(k, "im2d_guest")) hle_ge_guest_im2d = atoi(v);
            else if (!strcmp(k, "ge_trace")) opt_trace = (uint32_t)strtoul(v, NULL, 10);
            else if (!strcmp(k, "ge_zmask")) opt_zmask = atoi(v);
            else if (!strcmp(k, "ge_ser")) opt_ser = atoi(v);          /* GE serial probe */
            else if (!strcmp(k, "ge_aflip")) opt_aflip = atoi(v);      /* async flip machinery */
            else if (!strncmp(k, "tex_half", 8) || !strncmp(k, "tex_mip", 7)) hle_ge_tex_cfg(k, v);   /* texture resolution / mipmaps */
            else if (!strncmp(k, "w3d_", 4) || !strcmp(k, "world3d") || !strcmp(k, "mesh_guest")) w3d_cfg(k, v);
            else if (!strcmp(k, "sky") || !strncmp(k, "sky_", 4)) hle_sky_cfg(k, v);   /* the sky */
            else if (!strcmp(k, "water") || !strncmp(k, "water_", 6)) hle_water_cfg(k, v);   /* water surfaces */
            else if (!strcmp(k, "ge_dump"))
                for (const char *p = v; *p && opt_ndump < 16;) {
                    unsigned long f = strtoul(p, (char **)&p, 10);
                    if (f) opt_dump[opt_ndump++] = (uint32_t)f;
                    while (*p && (*p < '0' || *p > '9')) p++;
                }
        }
    }
}

int hle_ge_init(void)
{
    read_cfg();
    if (opt_tex_kb < 512) opt_tex_kb = 512;
    if (opt_tex_kb > 3072) opt_tex_kb = 3072;
    if (!opt_render_ge) {
        LOG("render: text (ps2rc.cfg render=text): the GE stays off, the screen shows the debug console\n");
        return 0;
    }
    /* two display lists + the texture arena from the guest code hole */
    arena_bytes = (uint32_t)opt_tex_kb << 10;
    uint8_t *mem = hle_plat_big_alloc(2u * LIST_BYTES + arena_bytes);
    sceKernelDcacheWritebackInvalidateRange(mem, 2u * LIST_BYTES + arena_bytes);   /* no dirty line may follow */
    list_u[0] = (void *)((uintptr_t)mem | 0x40000000u);                           /* uncached views */
    list_u[1] = (void *)(((uintptr_t)mem + LIST_BYTES) | 0x40000000u);
    arena = mem + 2u * LIST_BYTES;
    blk[0].off = 0;
    blk[0].size = arena_bytes;
    blk[0].used = 0;
    nblk = 1;
    ge_sema = sceKernelCreateSema("hle_ge", 0, 1, 1, NULL);
    font_build();
    sceGuInit();
    sceGuStart(GU_DIRECT, list_u[0]);
    sceGuDrawBuffer(GU_PSM_5650, (void *)P_OFF, FB_STRIDE);
    state_reset();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    cur_list = 1;
    hle_ge_on = 1;
    if (opt_aflip) {                                       /* the async-flip presenter */
        af_go = sceKernelCreateSema("bd_af_go", 0, 0, 1, NULL);
        af_done = sceKernelCreateSema("bd_af_done", 0, 0, 1, NULL);
        SceUID th = af_go >= 0 && af_done >= 0 ? sceKernelCreateThread("bd_present", af_thread, 0x1E, 0x1000, 0, NULL) : -1;
        if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
            LOG("GE async flip: presenter NOT started (%08x) - flips wait for the GE as before\n", (unsigned)th);
            af_go = -1;
        } else
            LOG("GE async flip: presenter thread on (priority 0x1E); x_aflip (default 1, A/B key) decides "
                "per flip\n");
    }
    if (opt_ser > 0)
        LOG("GE serial probe: ge_ser=%d - %s\n", opt_ser,
            "the last settle iteration of every A/B window (or every N-th frame without an A/B) is built in GU_SEND mode "
            "and its GE time measured per category");
    LOG("render: ge - 480x272 5650 dithered, 3 display buffers (VRAM %06x/%06x/%06x) + the persistent picture %06x "
        "(Z reserved %06x), lists 2 x %u KB + texture arena %u KB in the hole at %p (uncached lists), text overlay %d "
        "rows, im2d_guest=%d, ge_clear=%d, %d frame dumps, fps counter %s\n", fb_off[0], fb_off[1], fb_off[2], P_OFF,
        ZB_OFF, LIST_BYTES >> 10, arena_bytes >> 10, mem, opt_text_rows, hle_ge_guest_im2d, opt_clear, opt_ndump,
        opt_fps ? "on (fps=1)" : "off (fps=0)");
    return 1;
}

static void bd_report(void)
{
    if (opt_aflip)
        LOG("GE async flip: %llu async flips, %llu settles, waited %.3f ms in all (%.3f ms per settle, max %u "
            "us), %llu presenter timeouts%s\n", (unsigned long long)st_aflips, (unsigned long long)st_af_settles,
            (double)af_wait_us / 1000.0, st_af_settles ? (double)af_wait_us / 1000.0 / (double)st_af_settles : 0.0,
            af_wait_max, (unsigned long long)st_af_timeouts, af_broken ? " (switched off after a timeout)" : "");
    LOG("GE list open: %.2f ms per flip (first draw -> flip done) over %llu flips\n",
        st_flips ? (double)open_us / 1000.0 / (double)st_flips : 0.0, (unsigned long long)st_flips);
    LOG("GE texture working set: %.1f distinct textures = %.0f KB per frame (max %llu KB) over %llu frames; "
        "eDRAM %u KB (sceGeEdramGetSize), framebuffers + P + Z + console %u KB -> %u KB free for a VRAM texture cache "
        "(minus the radiosity targets)\n", ws_frames ? (double)ws_tot_n / (double)ws_frames : 0.0,
        ws_frames ? (double)ws_tot_bytes / 1024.0 / (double)ws_frames : 0.0, (unsigned long long)(ws_max_bytes >> 10),
        (unsigned long long)ws_frames, (unsigned)(sceGeEdramGetSize() >> 10), (unsigned)((T_OFF + FB_BYTES) >> 10),
        (unsigned)((sceGeEdramGetSize() - (T_OFF + FB_BYTES)) >> 10));
    if (opt_ser <= 0) return;
    for (int v = 0; v < SER_V; v++) {
        if (!ser_frames[v]) continue;
        double n = (double)ser_frames[v], tot = 0;
        char line[512];
        int k = 0;
        for (int c = 0; c < GS_N; c++) {
            double ms = (double)ser_us[v][c] / n / 1000.0;
            tot += ms;
            k += snprintf(line + k, sizeof line - (size_t)k, " %s %.2f (%.1f)", gs_name[c], ms,
                          (double)ser_marks[v][c] / n);
        }
        LOG("GE serial %s%d: %llu probe frames, GE %.2f ms per frame (max %.2f) = ms (marks per frame):%s; "
            "%.1f cuts per frame; probe frame CPU %.1f ms\n", v < SER_V - 1 ? "v" : "no-AB ", v < SER_V - 1 ? v : 0,
            (unsigned long long)ser_frames[v], tot, (double)ser_us_max[v] / 1000.0, line,
            (double)ser_cuts[v] / n, (double)ser_cpu_us[v] / n / 1000.0);
    }
}

/* mip statistics (whole run + per A/B variant) */
static void bk_report(void)
{
    LOG("GE tex_mip: tex_mip=%d bias %d abias %d alpha %d filter %d at the end; %llu textures converted with "
        "levels (%.2f levels each, %llu generated levels in all), %llu KB of levels >= 1, %llu layout mismatches\n",
        opt_tex_mip, opt_tex_mip_bias, opt_tex_mip_abias, opt_tex_mip_alpha, opt_tex_mip_filter,
        (unsigned long long)st_mip_tex, st_mip_tex ? (double)st_mip_levels / (double)st_mip_tex : 0.0,
        (unsigned long long)st_mip_gen, (unsigned long long)(st_mip_bytes >> 10), (unsigned long long)st_mip_bad);
    for (int v = 0; v < BJ_V; v++) {
        if (!bj_ws_frames[v] && !bk_binds[v]) continue;
        double f = bj_ws_frames[v] ? (double)bj_ws_frames[v] : 1.0;
        LOG("GE tex_mip %s%d: %llu frames: working set level 0 %.0f KB per frame (all levels %.0f KB); 3D "
            "binds %.1f per frame, %.1f with levels; %llu conversions with levels\n", v < BJ_V - 1 ? "v" : "no-AB ",
            v < BJ_V - 1 ? v : 0, (unsigned long long)bj_ws_frames[v], (double)bk_ws_l0[v] / 1024.0 / f,
            (double)bj_ws_bytes[v] / 1024.0 / f, (double)bk_binds[v] / f, (double)bk_mip_binds[v] / f,
            (unsigned long long)bk_mip_conv[v]);
    }
}

/* texture resolution statistics, per A/B variant (counted iterations) or for the whole run */
static void bj_report(void)
{
    LOG("GE tex_half: tex_half=%d tex_half_fx=%d at the end; %llu textures converted at half size (%llu from "
        "the stored PS2 mip level 1, the rest 2:1 decimated), ~%llu KB of arena saved by them, %llu re-converted at full "
        "for a 2D draw, %llu cache flushes (key changes)\n", opt_tex_half, opt_tex_half_fx, (unsigned long long)st_half,
        (unsigned long long)st_half_mip, (unsigned long long)(st_half_bytes_saved >> 10),
        (unsigned long long)st_promote, (unsigned long long)st_tex_flush);
    for (int v = 0; v < BJ_V; v++) {
        if (!bj_ws_frames[v] && !bj_conv[v]) continue;
        double f = bj_ws_frames[v] ? (double)bj_ws_frames[v] : 1.0;
        LOG("GE tex %s%d: %llu frames: working set %.1f textures = %.0f KB per frame (max %llu KB), arena "
            "%.0f KB in use (mean at the flip); %llu conversions (%llu at half) = %.0f KB, %.3f ms in all = %.3f ms per "
            "frame (%.1f us each), %llu evictions = %.3f per frame\n", v < BJ_V - 1 ? "v" : "no-AB ",
            v < BJ_V - 1 ? v : 0, (unsigned long long)bj_ws_frames[v], (double)bj_ws_n[v] / f,
            (double)bj_ws_bytes[v] / 1024.0 / f, (unsigned long long)(bj_ws_max[v] >> 10),
            (double)bj_arena[v] / 1024.0 / f, (unsigned long long)bj_conv[v], (unsigned long long)bj_half[v],
            (double)bj_conv_bytes[v] / 1024.0, (double)bj_conv_us[v] / 1000.0, (double)bj_conv_us[v] / 1000.0 / f,
            bj_conv[v] ? (double)bj_conv_us[v] / (double)bj_conv[v] : 0.0, (unsigned long long)bj_evict[v],
            (double)bj_evict[v] / f);
    }
}

void hle_ge_report(void)
{
    if (!hle_ge_on) { LOG("render: text mode (no GE)\n"); return; }
    bd_report();
    bj_report();
    bk_report();
    LOG("GE: %llu flips, %llu presented, %llu 2D prims (%llu vertices), %llu render-buffer quads, %llu colour clears "
        "(%llu Z clears %s; %llu with the fog-colour sky clear, w3d_sky), %llu texture binds\n",
        (unsigned long long)st_flips, (unsigned long long)st_presented, (unsigned long long)st_prims,
        (unsigned long long)st_verts, (unsigned long long)st_quads, (unsigned long long)st_clears,
        (unsigned long long)st_zclear, w3d_opt_on ? "drawn: the 3D world's Z buffer" : "ignored",
        (unsigned long long)st_skyclears, (unsigned long long)st_binds);
    LOG("GE textures: %llu converted (%llu KB, %u ms, max %u us), %llu not convertible, %llu draws skipped for their "
        "texture, arena %u KB used of %u (peak %u, %d blocks), %llu evictions\n", (unsigned long long)st_conv,
        (unsigned long long)(st_conv_bytes >> 10), conv_us / 1000u, conv_us_max, (unsigned long long)st_conv_fail,
        (unsigned long long)st_tex_skip, arena_used >> 10, arena_bytes >> 10, st_arena_peak >> 10, nblk,
        (unsigned long long)st_evict);
    if (st_flips)
        LOG("GE time: waiting for the GE %.3f ms/flip (max %u us), flip %.3f ms/flip (max %u us), texture conversion "
            "%u ms in all\n", (double)sync_us / 1000.0 / (double)st_flips, sync_us_max,
            (double)flip_us / 1000.0 / (double)st_flips, flip_us_max, conv_us / 1000u);
    LOG("GE lists: max %u KB of %u, %llu mid-frame resyncs; %llu draws off-screen (other camera raster), %llu bad, "
        "%llu coordinates clamped, %llu approximated blends; screen: %llu console takeovers, %llu stall views, %llu "
        "dumps\n", st_list_max >> 10, LIST_BYTES >> 10, (unsigned long long)st_resync, (unsigned long long)st_offscreen,
        (unsigned long long)st_bad, (unsigned long long)st_clamped, (unsigned long long)st_blend_approx,
        (unsigned long long)st_takeovers, (unsigned long long)st_stalls, (unsigned long long)st_dumps);
    LOG("GE sky (hle_ge_tris): %llu draws, %llu triangles\n", (unsigned long long)st_w_draws,
        (unsigned long long)st_w_tris);
    LOG("GE 2D depth / guard band: %llu draws with Z test or Z write (radar mask), %llu depth clears done of %llu guest "
        "Z clears, %llu primitives clipped, %llu clamped (lines / points / > 256 triangles)\n",
        (unsigned long long)st_zdraws, (unsigned long long)st_zclears_done, (unsigned long long)st_zclear,
        (unsigned long long)st_clipped, (unsigned long long)st_clip_dropped);
    if (st_movie_frames) LOG("GE: %llu movie pictures presented (hle_movie.c)\n", (unsigned long long)st_movie_frames);
}

/* before sceKernelExitGame - nothing of the GE may still point into the
   EBOOT when it goes away: an open GU_DIRECT frame list (enqueued, stalled at its end while the guest builds the frame;
   open whenever an exit comes in the middle of a frame - HOME, crash) is finished and waited for, the queue is reset
   if the GE does not get idle within 0.5 s (sceGeBreak 1), and sceGuTerm takes sceGuInit's GE callbacks (signal /
   finish handlers in EBOOT code) out of the GE driver. Returns a bit set for the exit log: 1 = a frame list was open,
   2 = the GE was idle in time, 4 = the queue had to be reset, 8 = the GE was never on. */
int hle_ge_shutdown(void)
{
    if (!hle_ge_on) return 8;
    int r = 0, locked = ge_lock(200000);
    if (frame_open) {
        r |= 1;
        sceGuFinish();                                     /* closes the list; the GE runs it to the end */
        frame_open = 0;
    }
    for (int i = 0; i < 100; i++) {
        if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) { r |= 2; break; }
        sceKernelDelayThread(5000);
    }
    if (!(r & 2)) { sceGeBreak(1, NULL); r |= 4; }
    sceGuTerm();
    hle_ge_on = 0;
    ge_unlock(locked);
    return r;
}
#endif
