/* hle_menu_chord.h - the button combination that opens the in-game menu (pure C, no PSP headers: hle_menu_psp.c
 * uses it on the PSP, tests/cfg/test_cm_menu.py on the host).
 *
 * Default: L + R + Cross + D-pad Down (owner, 2026-10-20). The menu opens on the read where the LAST of its buttons
 * goes down (all held now, not all held before). From that read on the combination's buttons are kept from the game
 * while all of them are held, and every button still held when the menu closes stays hidden from the game until it is
 * released - so neither the opening press nor the release after the menu reaches the game. The buttons pressed on the
 * way to the full combination (e.g. L + R before Down) do reach the game until the last one arrives, as any partial
 * combination does.
 * Refused combinations (ps2rc_dev.cfg menu_chord=...; the default is kept): fewer than 2 or more than 4 buttons, an
 * unknown name, a superset of the port's L+R+SELECT (fps counter), and anything containing L+R+Down+START or
 * L+R+Down+SELECT: the ARK custom firmware's SystemControl hooks the controller reads and leaves the game when all four
 * of either are held (to its launcher / to the XMB; ARK-4 core/systemctrl/src/exitgame.c, ARK-5
 * SystemControl/src/controller.c, public sources).
 */
#ifndef HLE_MENU_CHORD_H
#define HLE_MENU_CHORD_H
#include <string.h>

/* PSP button bits (= pspctrl.h PSP_CTRL_*) */
#define MC_SELECT   0x0001u
#define MC_START    0x0008u
#define MC_UP       0x0010u
#define MC_RIGHT    0x0020u
#define MC_DOWN     0x0040u
#define MC_LEFT     0x0080u
#define MC_L        0x0100u
#define MC_R        0x0200u
#define MC_TRIANGLE 0x1000u
#define MC_CIRCLE   0x2000u
#define MC_CROSS    0x4000u
#define MC_SQUARE   0x8000u

#define MC_DEFAULT (MC_L | MC_R | MC_CROSS | MC_DOWN)
#define MC_ARK_LAUNCHER (MC_L | MC_R | MC_DOWN | MC_START)    /* ARK: exit to its launcher */
#define MC_ARK_XMB (MC_L | MC_R | MC_DOWN | MC_SELECT)        /* ARK: exit to the XMB */
#define MC_FPS (MC_L | MC_R | MC_SELECT)                      /* the port's fps counter toggle */

typedef struct {
    unsigned chord;              /* the combination */
    unsigned prev;               /* the raw buttons of the previous read */
    unsigned block;              /* held when the menu opened / closed: hidden from the game until released */
} MenuChord;

static inline int mc_popc(unsigned x) { int n = 0; while (x) { n += (int)(x & 1u); x >>= 1; } return n; }

/* "l+r+cross+down" -> bits; 0 = refused (*why says why) */
static inline unsigned mc_parse(const char *s, const char **why)
{
    static const struct { const char *n; unsigned b; } nm[] = {
        {"select", MC_SELECT}, {"start", MC_START}, {"l", MC_L}, {"r", MC_R}, {"up", MC_UP}, {"down", MC_DOWN},
        {"left", MC_LEFT}, {"right", MC_RIGHT}, {"triangle", MC_TRIANGLE}, {"circle", MC_CIRCLE},
        {"cross", MC_CROSS}, {"square", MC_SQUARE}};
    unsigned m = 0;
    int n = 0;
    *why = "";
    while (*s) {
        const char *e = strchr(s, '+');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        unsigned b = 0;
        for (unsigned i = 0; i < sizeof nm / sizeof nm[0]; i++) {
            size_t k = strlen(nm[i].n), j;
            if (k != len) continue;
            for (j = 0; j < k; j++) {
                char c = s[j];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != nm[i].n[j]) break;
            }
            if (j == k) b = nm[i].b;
        }
        if (!b) { *why = "unknown button name"; return 0; }
        m |= b;
        n++;
        if (!e) break;
        s = e + 1;
    }
    if (mc_popc(m) < 2 || n > 4) { *why = "needs two to four buttons"; return 0; }
    if ((m & MC_FPS) == MC_FPS) { *why = "contains L+R+SELECT (the fps counter)"; return 0; }
    if ((m & MC_ARK_LAUNCHER) == MC_ARK_LAUNCHER || (m & MC_ARK_XMB) == MC_ARK_XMB) {
        *why = "contains L+R+Down+START or L+R+Down+SELECT (the custom firmware leaves the game)";
        return 0;
    }
    return m;
}

/* one controller read: raw PSP buttons -> the buttons the game gets; *open = 1 on the read that completes the
   combination */
static inline unsigned mc_filter(MenuChord *mc, unsigned raw, int *open)
{
    unsigned b = raw;
    *open = 0;
    if (mc->block) { mc->block &= raw; b &= ~mc->block; }
    if (mc->chord && (raw & mc->chord) == mc->chord) {
        if ((mc->prev & mc->chord) != mc->chord) *open = 1;
        mc->block |= mc->chord;                  /* hidden from now on until each is released */
        b &= ~mc->chord;
    }
    mc->prev = raw;
    return b;
}

/* the menu closed with these buttons held: hidden from the game until released */
static inline void mc_closed(MenuChord *mc, unsigned held)
{
    mc->block = held;
    mc->prev = held;
}

#endif
