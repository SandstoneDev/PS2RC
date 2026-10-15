/* hle_padmap.h - the player's control mapping (ps2rc.cfg): which PSP buttons give the PS2 inputs the PSP lacks.
 * Pure C (no PSP headers): hle_pad.c uses it on the PSP, tests/cfg/test_padmap.py compiles it on the host.
 *
 * Model: every PS2 input that can be moved (L2 R2 L3 R3 L1 R1 SELECT START, the right stick) is produced by a
 * combination = a set of 1-3 PSP buttons held together. Cross / Circle / Square / Triangle / D-pad are fixed (each
 * gives itself). When the buttons of a larger combination are all held, every smaller combination inside it is
 * silent (SELECT + L gives L2, not L1); exception: with select_to_game=always the PSP SELECT always reaches the game
 * as SELECT (the original behaviour). select_to_game=tap: SELECT reaches the game only after it was pressed and
 * released with no other button, as a pulse that lasts PADMAP_TAP_FRAMES frames (vblank counts seen by the reads).
 * Keys (internal; the friendly names are the game's alias table):
 *   pad_mod      select | l | r | none        the button the built-in combinations start with (default select)
 *   pad_sel_game always | tap                 how the PSP SELECT reaches the game (default always)
 *   pad_l2 pad_r2 pad_l3 pad_r3               a combination of 2-3 buttons ("select+l", "l+r") or "none"
 *   pad_l1 pad_r1 pad_select pad_start        a combination of 1-3 buttons or "none"
 *   pad_rstick   <button>+dpad | <button>+analog | analog | none
 *   pad_sel_up   r3 | l3 | stick              Up under the right-stick D-pad button gives R3 / L3 (older key)
 * Validation: a value that does not parse, a combination two inputs share, one that contains the port's own
 * L+R+SELECT (fps counter) or that is three of the system's L+R+Down+START, or HOME, is noted and that key takes
 * its default. The defaults (today's layout for pad_mod=select) never conflict with each other.
 */
#ifndef HLE_PADMAP_H
#define HLE_PADMAP_H
#include <stdint.h>

/* PSP button bits = pspctrl.h PSP_CTRL_* (hle_pad.c checks that they agree) */
#define PM_SELECT   0x0001u
#define PM_ASTICK   0x0002u            /* virtual: the analog stick is out of its dead zone */
#define PM_START    0x0008u
#define PM_UP       0x0010u
#define PM_RIGHT    0x0020u
#define PM_DOWN     0x0040u
#define PM_LEFT     0x0080u
#define PM_L        0x0100u
#define PM_R        0x0200u
#define PM_TRIANGLE 0x1000u
#define PM_CIRCLE   0x2000u
#define PM_CROSS    0x4000u
#define PM_SQUARE   0x8000u
#define PM_HOME     0x10000u
#define PM_PHYS     0xF3F9u            /* the 12 buttons a game may use */

#define PADMAP_TAP_FRAMES 3            /* a SELECT tap reaches the game in this many distinct frames */
#define PADMAP_MAX_ENT 24

/* the moveable inputs, in key / log order */
enum { PI_L2, PI_R2, PI_L3, PI_R3, PI_RSTICK, PI_L1, PI_R1, PI_SELECT, PI_START, PI_N };
enum { RS_NONE, RS_DPAD, RS_ANALOG };

typedef struct {
    uint32_t chord;                    /* PSP bits, all held = active */
    uint16_t out;                      /* PS2 bits (0: a right-stick entry) */
    int8_t dir;                        /* right stick: 0 left 1 right 2 up 3 down, 4 = analog, -1 = a button */
    int8_t owner;                      /* PI_* of the key, -1 = a fixed button (Cross ... D-pad) */
    int8_t select_plain;               /* the PSP SELECT's own SELECT entry (select_to_game) */
} PadEnt;

typedef struct {
    int mod;                           /* 0 select, 1 l, 2 r, 3 none */
    int sel_tap;                       /* select_to_game=tap */
    uint32_t chord[PI_N];              /* combination per input (rstick: its button) */
    int rs_mode;                       /* RS_* */
    uint16_t sel_up_bit;               /* 0 = right stick up; PS2 R3 / L3 bit */
    uint8_t bad[PI_N];                 /* the file's value was not used (see notes) */
    PadEnt ent[PADMAP_MAX_ENT];
    int n_ent;
    char line[400];                    /* "controls: ..." */
    char notes[600];                   /* "" or "map_l2=...: why; ..." */
} PadMap;

typedef struct {
    int sel_down, sel_alone;           /* SELECT held / nothing else since it went down */
    int pulse;                         /* frames the pulse has been seen in (0 = no pulse) */
    uint64_t pulse_frame;              /* the frame value counted last */
} PadTap;

/* cfg value of an internal key, NULL = not in the file */
typedef const char *(*padmap_get_fn)(const char *key);

void padmap_config(PadMap *pm, padmap_get_fn get);
/* one sample: PSP buttons, raw analog stick (0..255), the frame counter -> PS2 buttons (active high),
   sticks[] = right X, right Y, left X, left Y */
uint16_t padmap_apply(const PadMap *pm, PadTap *st, uint32_t buttons, unsigned lx, unsigned ly, uint64_t frame,
                      uint8_t sticks[4]);
uint8_t padmap_stick(unsigned v);      /* the dead-zone rescale of one analog axis */
const char *padmap_chord_name(uint32_t chord, char *buf, int n);

#endif
