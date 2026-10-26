/* hle_padmap.h - the player's control mapping (ps2rc.cfg): which PSP controls give which PS2 pad inputs.
 * Pure C (no PSP headers): hle_pad.c uses it on the PSP, tests/cfg/test_padmap.py compiles it on the host.
 *
 * Model: every PS2 input (the 16 buttons, the right stick, the left stick) is produced by up to PADMAP_ALT
 * alternatives ("a/b"), each a combination = a set of 1-3 PSP buttons held together, optionally with the analog stick
 * ("analog" = out of its dead zone, "analog_left/right/up/down" = pushed that way). When the buttons of a larger
 * combination are all held, every smaller combination inside it is silent (SELECT + L gives L2, not L1); exception:
 * with select_to_game=always the PSP SELECT always reaches the game as SELECT (the original behaviour).
 * "+tap": the input comes only after its buttons were pressed and released with nothing else (no other button, no
 * stick), as a pulse in PADMAP_TAP_FRAMES distinct frames (vblank counts seen by the reads); "+hold": the same, or
 * while held alone for PADMAP_HOLD_FRAMES frames and longer. A tap / hold combination never silences others.
 * Two layers: on foot and in a vehicle (the game pack's hle_pack_pad_context(); the vehicle layer exists only when
 * the preset has one or a car_* key is in the file - otherwise one layer, the original behaviour).
 * Keys (internal; the friendly names are the game's alias table, map_<x> = both layers, car_<x> = in a vehicle):
 *   pad_preset   default | stories          the built-in layout the keys below change (default = the original one)
 *   pad_mod      select | l | r | none      default preset: the button the built-in combinations start with
 *   pad_sel_game always | tap               how the PSP SELECT reaches the game (default always)
 *   pad_<input>  pad_c_<input>              <input> = l2 r2 l3 r3 rstick l1 r1 select start cross circle square
 *                                           triangle up down left right lstick; a value "select+l", "left/l+left",
 *                                           "l+hold", "l+analog_left", "none"
 *   pad_rstick   [<buttons>+]dpad | <buttons>+analog | analog (= the modifier + analog) | none
 *   pad_lstick   analog | dpad | <button>+dpad | none
 *   pad_sel_up   r3 | l3 | stick            Up under the right-stick D-pad button gives R3 / L3 (older key)
 * Validation: a value that does not parse, a combination two inputs share (the changed key gives way; both changed:
 * the later key), one that contains the port's own L+R+SELECT (fps counter) or that is three of the system's
 * L+R+Down+START, or HOME, is noted and that key takes its default. Defaults never conflict with each other.
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
/* virtual: the analog stick pushed far that way (a combination holds them together with PM_ASTICK) */
#define PM_ALEFT    0x00100000u
#define PM_ARIGHT   0x00200000u
#define PM_AUP      0x00400000u
#define PM_ADOWN    0x00800000u
#define PM_ADIRS    0x00F00000u
#define PM_ANALOG   (PM_ASTICK | PM_ADIRS)

#define PADMAP_TAP_FRAMES 3            /* a tap reaches the game in this many distinct frames */
#define PADMAP_HOLD_FRAMES 8           /* "+hold": held alone this many distinct frames = held */
#define PADMAP_ALT 3                   /* alternatives per input */
#define PADMAP_MAX_ENT 64

/* the inputs, in key / log order (the first nine = the original keys) */
enum { PI_L2, PI_R2, PI_L3, PI_R3, PI_RSTICK, PI_L1, PI_R1, PI_SELECT, PI_START, PI_CROSS, PI_CIRCLE, PI_SQUARE,
       PI_TRIANGLE, PI_UP, PI_DOWN, PI_LEFT, PI_RIGHT, PI_LSTICK, PI_N };
enum { RS_NONE, RS_DPAD, RS_ANALOG };  /* stick modes (the left stick: RS_ANALOG = the analog stick itself) */
enum { PF_TAP = 1, PF_HOLD = 2 };      /* alternative flags */
enum { PRESET_DEFAULT, PRESET_STORIES, PRESET_N };

typedef struct {
    uint32_t chord;                    /* PSP bits, all held = active */
    uint16_t out;                      /* PS2 bits (0: a stick entry) */
    int8_t dir;                        /* -1 a button; right stick 0 left 1 right 2 up 3 down, 4 analog; left stick
                                          5 left 6 right 7 up 8 down */
    int8_t owner;                      /* PI_* of the key */
    uint8_t flags;                     /* PF_* */
    int8_t select_plain;               /* the PSP SELECT's own SELECT entry (select_to_game=always) */
} PadEnt;

typedef struct {
    uint32_t c[PADMAP_ALT];            /* combinations (n used) */
    uint8_t fl[PADMAP_ALT];            /* PF_* */
    int8_t n;                          /* 0 = none */
    int8_t mode;                       /* sticks: RS_* */
} PadIn;

typedef struct {
    PadIn in[PI_N];
    PadEnt ent[PADMAP_MAX_ENT];
    int n_ent;
} PadLayer;

typedef struct {
    int preset;                        /* PRESET_* */
    int mod;                           /* 0 select, 1 l, 2 r, 3 none */
    int sel_tap;                       /* select_to_game=tap */
    int has_car;                       /* a vehicle layer exists */
    uint16_t sel_up_bit;               /* 0 = right stick up; PS2 R3 / L3 bit */
    int cam_inv;                       /* camera_invert: bit 0 = X, bit 1 = Y of an analog-stick right stick */
    PadLayer lay[2];                   /* 0 on foot (the only one without has_car), 1 in a vehicle */
    uint8_t bad[2][PI_N];              /* the file's value was not used (see notes) */
    char line[900];                    /* "controls: ..." */
    char car_line[900];                /* "controls in a vehicle: ..." ("" without has_car) */
    char notes[1000];                  /* "" or "map_l2=...: why; ..." */
} PadMap;

typedef struct {                       /* one tap / hold combination's state */
    uint8_t down, alone, pulse, held, n_held;
    uint64_t hframe, pframe;           /* the frame counted last while held / while pulsing */
} PadTapE;

typedef struct {
    PadTapE e[2][PADMAP_MAX_ENT];
    int ctx;
} PadTap;

/* cfg value of an internal key, NULL = not in the file */
typedef const char *(*padmap_get_fn)(const char *key);

void padmap_config(PadMap *pm, padmap_get_fn get);
/* one sample: PSP buttons, raw analog stick (0..255), the frame counter, the context (0 on foot, 1 in a vehicle;
   ignored without a vehicle layer) -> PS2 buttons (active high), sticks[] = right X, right Y, left X, left Y */
uint16_t padmap_apply(const PadMap *pm, PadTap *st, uint32_t buttons, unsigned lx, unsigned ly, uint64_t frame,
                      int ctx, uint8_t sticks[4]);
uint8_t padmap_stick(unsigned v);      /* the dead-zone rescale of one analog axis */
const char *padmap_chord_name(uint32_t chord, char *buf, int n);

#endif
