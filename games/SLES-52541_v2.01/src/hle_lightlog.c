/* hle_lightlog.c - diagnostics: the game's lighting inputs once per main-loop iteration (issue #2 "the light
 * gets too dark, then back up").
 *   light_log = N       0 = off (default); N = one line every N iterations (1 = every frame)
 *   light_log_max = M   at most M lines (default 20000)
 *   co_shots = I1,I2,.. screen shots shot_<seq>_co<I>.bmp at these main-loop iterations (up to 8; HW / PPSSPP sets)
 * One line: iteration, vtime, the game clock, the time-of-day colour set's ambient / object ambient / directional
 * colour (the first floats of the current colour set 0x707440), its two post-effect colours (0x7074B8 / 0x7074C8,
 * bytes as the colour filter gets them before the auto-exposure factor), the auto-exposure lighting basis (gp+5764),
 * the far clip / fog start of the colour set (+0x50 / +0x54) and of the camera object (+0x84 / +0x88), the player ped's
 * ground lighting (+300), the radiosity limit (0x7074DC), the day/night balance (0x66CDE0), the building ambients (0x668898 / 0x6689F0), the
 * colours of the ambient / main directional light objects (gp+204 / gp+208, RGB floats at +0x18) and the camera
 * position. Reads guest memory only. */
#include "hle.h"
#include "hle_cfg.h"
#include <stdlib.h>
#include <string.h>

void hle_lightlog_tick(void);

static int ready, every, maxl = 20000;
static uint32_t it, lines;
static uint32_t shot_it[8];
static int nshots;

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

static float gff(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }
static unsigned b255(float v) { return !(v > 0.0f) ? 0u : v >= 255.0f ? 255u : (unsigned)v; }

void hle_lightlog_tick(void)
{
    if (!ready) {
        ready = 1;
        const char *v = key("light_log");
        every = v ? atoi(v) : 0;
        v = key("light_log_max");
        if (v) maxl = atoi(v);
        if (every > 0) LOG("light_log: one line every %d iterations, at most %d lines\n", every, maxl);
        v = key("co_shots");
        while (v && *v && nshots < 8) {
            char *e;
            unsigned long x = strtoul(v, &e, 10);
            if (e == v) break;
            if (x) shot_it[nshots++] = (uint32_t)x;
            v = *e == ',' ? e + 1 : e;
        }
        if (nshots) LOG("co_shots: %d screen shots planned\n", nshots);
    }
    it++;
    for (int i = 0; i < nshots; i++)
        if (shot_it[i] == it) {
            char tag[24], why[96] = "";
            snprintf(tag, sizeof tag, "co%u", (unsigned)it);
            int r = hle_plat_shot(tag, why, sizeof why);
            LOG("co_shots: iteration %u -> %s (%s)\n", (unsigned)it, r == 0 ? "saved" : "NOT saved", why);
        }
    if (every <= 0 || it % (uint32_t)every || lines >= (uint32_t)maxl) return;
    lines++;
    const uint32_t cs = 0x00707440u, gp = 0x0066B970u;
    uint32_t la = gw(gp + 204), ld = gw(gp + 208);
    float a[3] = {0, 0, 0}, d[3] = {0, 0, 0};
    if (la >= 0x100000u && la < hle_ram_end - 0x40) for (int i = 0; i < 3; i++) a[i] = gff(la + 0x18 + 4u * (uint32_t)i);
    if (ld >= 0x100000u && ld < hle_ram_end - 0x40) for (int i = 0; i < 3; i++) d[i] = gff(ld + 0x18 + 4u * (uint32_t)i);
    uint32_t rw = gw(0x006FFAE4u);                  /* the camera object: far plane +0x84, fog plane +0x88 */
    float rwf = rw >= 0x100000u && rw < hle_ram_end - 0x100 ? gff(rw + 0x84) : -1.0f;
    float rwg = rw >= 0x100000u && rw < hle_ram_end - 0x100 ? gff(rw + 0x88) : -1.0f;
    uint32_t ped = gw(0x0070A260u);                 /* the player ped: +300 its lighting from the ground */
    float pl = ped >= 0x100000u && ped < hle_ram_end - 0x200 ? gff(ped + 300) : -1.0f;
    uint32_t ab = gw(0x00668898u), adn = gw(0x006689F0u);
    LOG("LL %u %.3f %02u:%02u:%02u cs %.0f %.0f %.0f | %.0f %.0f %.0f | %.0f %.0f %.0f pfx %02x%02x%02x%02x %02x%02x%02x%02x "
        "basis %.3f pl %.3f hl %.0f dn %.3f ab %06x adn %06x La %.3f %.3f %.3f Ld %.3f %.3f %.3f cam %.1f %.1f %.1f far %.0f fog %.0f rw %.0f %.0f\n",
        it, (double)hle_vtime / EE_HZ, gb(0x0066C02Cu), gb(0x0066C030u), gb(0x0066C034u),
        (double)gff(cs), (double)gff(cs + 4), (double)gff(cs + 8), (double)gff(cs + 12), (double)gff(cs + 16),
        (double)gff(cs + 20), (double)gff(cs + 24), (double)gff(cs + 28), (double)gff(cs + 32),
        b255(gff(cs + 0x78)), b255(gff(cs + 0x7C)), b255(gff(cs + 0x80)), b255(gff(cs + 0x84)),
        b255(gff(cs + 0x88)), b255(gff(cs + 0x8C)), b255(gff(cs + 0x90)), b255(gff(cs + 0x94)),
        (double)gff(gp + 5764), (double)pl, (double)gff(cs + 0x9C), (double)gff(0x0066CDE0u), ab & 0xFFFFFFu, adn & 0xFFFFFFu,
        (double)a[0], (double)a[1], (double)a[2], (double)d[0], (double)d[1], (double)d[2],
        (double)gff(0x006FF1D4u), (double)gff(0x006FF1D8u), (double)gff(0x006FF1DCu), (double)gff(cs + 0x50),
        (double)gff(cs + 0x54), (double)rwf, (double)rwg);
}
