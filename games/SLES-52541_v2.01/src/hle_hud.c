/* hle_hud.c - the HUD on the PSP screen: 4:3 proportions anchored to the screen corners, a round radar, a size and
 * an outline for legibility (overview and keys: hle_hud.h).
 *
 * How: sites.def routes the calls of the HUD draw (0x2B01A0, called once per frame from the 2D pass) to its element
 * functions through the handlers below. A handler
 *   1. draws the text the game has buffered so far (the game's own "draw the buffered text" function, 0x2A8820, which
 *      the game calls 11 times a frame anyway) - so earlier text is not transformed,
 *   2. sets the group's transform (hle_hud.h), calls the element function as the game would,
 *   3. draws the text that element buffered while the transform is still set, then clears it.
 * The register context is restored around each extra text call (the element call sees its arguments, the caller sees
 * the element's results). Only the moment the buffered text is drawn changes (the HUD text is drawn right after its
 * element instead of at the end of the 2D pass) - nothing the game computes.
 * Element groups (facts of the boot program: the call sites in 0x2B01A0, the element positions in guest units):
 *   top right    0x2AB500 clock / health / armour / money / weapon / ammo (x 497..608), 0x2AA4D0 wanted level
 *                                                              anchor (640, 0)
 *   bottom right 0x2AD660 vehicle name, 0x2ADB60 area name (x 608, y 344)   anchor (640, 448)
 *   radar        0x2AC960 radar (ring x 36..138, y 340..424), 0x2AC6F0 (the ring around the radar in some
 *                vehicles)                                     anchor (0, 448), radar factor
 *   bottom left  0x2ABD00 the player's stats panel shown instead of the radar (x 40..210, y 299..426)
 *                                                              anchor (0, 448)
 *   top left     0x2AE060 help text box (x 30..233)            anchor (0, 0)
 *   centre       0x2AED60 script texts (any position), 0x2AEF80 subtitles / dialogue line (x 320), 0x2AF890,
 *                0x2AF220, 0x2AF630 (big / success / odd-job messages, x 320)   anchor x 320 = the 4:3 picture in
 *                the middle of the screen (4:3 factor only: no size change, no outline)
 * The cross-hair (0x2AA9F0, full-screen sniper scope) and everything outside the HUD draw (menus, subtitles,
 * loading screens) keep the old stretch. */
#include "hle.h"
#include "hle_cfg.h"
#include "hle_hud.h"
#include <stdlib.h>
#include <string.h>

int hle_hud_on, hle_hud_outline;
float hle_hud_ax, hle_hud_kx = 1.0f, hle_hud_ay, hle_hud_ky = 1.0f;

#define HUD_FONT_DRAW 0x002A8820u          /* draws the buffered text and empties the buffer */
#define HUD_FONT_ITER 0x0066C478u          /* the text buffer's write pointer */
#define HUD_FONT_BUF0 0x007C3200u          /* ... its start (empty buffer) */

/* the screen: guest 640x448 (a 4:3 picture) shown at 480x272 */
#define GUEST_W 640.0f
#define GUEST_H 448.0f
#define PSP_W 480.0f
#define PSP_H 272.0f
/* the radar ring in guest units (0x2AC960's four quarter sprites): 102 x 84 */
#define RADAR_W 102.0f
#define RADAR_H 84.0f

enum { G_TR, G_BR, G_RADAR, G_BL, G_TL, G_C, G_N };
static const char *const g_name[G_N] = {"top-right", "bottom-right", "radar", "bottom-left", "top-left", "centre"};

static int ready, opt_fix, opt_scale = 100, opt_outline, opt_radar_ps2, depth;
static float k43, kradar;
static uint64_t st_calls[G_N], st_flush;

/* a key: the host's environment (HLE_HUD_ASPECT, ...) first, then the cfg table */
static const char *key(const char *k)
{
#ifndef HLE_PSP
    char e[40] = "HLE_";
    size_t n = strlen(k);
    for (size_t i = 0; i < n && i + 5 < sizeof e; i++) e[4 + i] = (char)(k[i] >= 'a' && k[i] <= 'z' ? k[i] - 32 : k[i]);
    const char *v = getenv(e);
    if (v) return v;
#endif
    return hle_cfg_get(k);
}

static int word(const char *k, const char *on, const char *off, int def)
{
    const char *v = key(k);
    if (!v) return def;
    if (!strcmp(v, on) || !strcmp(v, "1") || !strcmp(v, "on")) return 1;
    if (!strcmp(v, off) || !strcmp(v, "0") || !strcmp(v, "off")) return 0;
    return def;
}

static void hud_init(void)
{
    ready = 1;
#ifdef HLE_PSP
    opt_fix = word("hud_aspect", "fix", "stretch", 1);
#else
    opt_fix = word("hud_aspect", "fix", "stretch", 0);         /* host default: the old picture (guest-identical runs) */
#endif
    const char *v = key("hud_scale");
    if (v) opt_scale = atoi(v);
    if (opt_scale < 50) opt_scale = 50;
    if (opt_scale > 200) opt_scale = 200;
    opt_outline = word("hud_outline", "on", "off", 0);
    opt_radar_ps2 = word("hud_radar", "ps2", "round", 0);
    /* 4:3 picture -> the PSP's 480x272: a guest unit is (4/3)/640 of the picture height wide on a TV and
       (PSP_W/GUEST_W) / (PSP_H/GUEST_H) times its height on the PSP */
    float tv = (4.0f / 3.0f) * GUEST_H / GUEST_W, psp = (PSP_W / GUEST_W) / (PSP_H / GUEST_H);
    k43 = tv / psp;                                            /* 0.756 */
    kradar = (RADAR_H * PSP_H / GUEST_H) / (RADAR_W * PSP_W / GUEST_W);   /* 0.667: the ring as wide as high */
    if (opt_radar_ps2) kradar = k43;
    LOG("hud: hud_aspect=%s hud_scale=%d hud_outline=%s hud_radar=%s (4:3 factor %.4f, radar factor %.4f)\n",
        opt_fix ? "fix" : "stretch", opt_scale, opt_outline ? "on" : "off", opt_radar_ps2 ? "ps2" : "round", k43, kradar);
}

/* draw the buffered text now, the register context unchanged for the caller */
static void font_flush(R5900Ctx *c)
{
    static R5900Ctx save;
    if (gw(HUD_FONT_ITER) == HUD_FONT_BUF0) return;            /* nothing buffered */
    memcpy(&save, c, sizeof save);
    rc_dispatch(c, HUD_FONT_DRAW);
    memcpy(c, &save, sizeof save);
    st_flush++;
}

static void set_group(int g)
{
    float s = (float)opt_scale / 100.0f;
    hle_hud_outline = opt_outline;
    switch (g) {
    case G_TR: hle_hud_ax = GUEST_W; hle_hud_ay = 0.0f; hle_hud_kx = k43 * s; hle_hud_ky = s; break;
    case G_BR: hle_hud_ax = GUEST_W; hle_hud_ay = GUEST_H; hle_hud_kx = k43 * s; hle_hud_ky = s; break;
    case G_RADAR: hle_hud_ax = 0.0f; hle_hud_ay = GUEST_H; hle_hud_kx = kradar * s; hle_hud_ky = s; break;
    case G_BL: hle_hud_ax = 0.0f; hle_hud_ay = GUEST_H; hle_hud_kx = k43 * s; hle_hud_ky = s; break;
    case G_TL: hle_hud_ax = 0.0f; hle_hud_ay = 0.0f; hle_hud_kx = k43 * s; hle_hud_ky = s; break;
    default: hle_hud_ax = GUEST_W / 2; hle_hud_ay = 0.0f; hle_hud_kx = k43; hle_hud_ky = 1.0f; hle_hud_outline = 0; break;
    }
    hle_hud_on = 1;
}

static void hud_call(R5900Ctx *c, uint32_t target, int g)
{
    if (!ready) hud_init();
    if (!opt_fix || depth) { rc_dispatch(c, target); return; }
    depth++;
    st_calls[g]++;
    font_flush(c);                                             /* earlier text: as before */
    set_group(g);
    rc_dispatch(c, target);
    font_flush(c);                                             /* this element's text: transformed */
    hle_hud_on = 0;
    hle_hud_outline = 0;
    depth--;
}

/* the handlers named in sites.def */
void hud_site_tr(R5900Ctx *c, uint32_t target);
void hud_site_br(R5900Ctx *c, uint32_t target);
void hud_site_radar(R5900Ctx *c, uint32_t target);
void hud_site_bl(R5900Ctx *c, uint32_t target);
void hud_site_tl(R5900Ctx *c, uint32_t target);
void hud_site_c(R5900Ctx *c, uint32_t target);
void hud_site_tr(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_TR); }
void hud_site_br(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_BR); }
void hud_site_radar(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_RADAR); }
void hud_site_bl(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_BL); }
void hud_site_tl(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_TL); }
void hud_site_c(R5900Ctx *c, uint32_t target) { hud_call(c, target, G_C); }

void hle_hud_report(void)
{
    if (!ready) return;
    LOG("hud: %s, scale %d %%, outline %s; element calls", opt_fix ? "fix" : "stretch", opt_scale,
        opt_outline ? "on" : "off");
    for (int g = 0; g < G_N; g++) LOG(" %s %llu", g_name[g], (unsigned long long)st_calls[g]);
    LOG(", extra text draws %llu\n", (unsigned long long)st_flush);
}
