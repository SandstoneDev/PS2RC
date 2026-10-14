/* hle_settings.c - the game's friendly cfg setting names (the player's
 * settings), mapped to the internal keys of the modules (tune_*, x_*, tex_half, water, fps, cpu_auto, ...). The generic
 * reader is hle_cfg.c; the commented template the converter writes: cfg_template.cfg in this pack.
 * Measured costs (PSP-3000) are in the template. */
#include <stddef.h>
#include "hle_cfg.h"

/* ------------------------------------------------------------------ the friendly names (player settings)
 * name     friendly key (what the shipped cfg template uses)
 * key      the internal key it is stored as (the modules read only internal keys)
 * lo / hi  numeric range of the FRIENDLY value: outside = clamped (noted in the log); lo = hi = 0: a word key, stored
 *          as given (pad_sel_up, cpu_auto_reset)
 * def      the code default of the INTERNAL key (applies when the file sets neither name; shown with '*')
 * inv      1 = internal = lo + hi - friendly (shadows=1 is x_fxskip=0)
 * words    "word=number,..." friendly words (textures=half); "!" first = ONLY these values (overclock)
 * The table is game-specific (the pack's module keys); the reader is generic. */
static const HleCfgAlias aliases[] = {
    /* graphics */
    {"draw_distance",  "tune_far",      10,  100, 100, 0, NULL},             /* % of the far clip */
    {"fog",            "tune_fog",      10,  400, 100, 0, NULL},             /* % fog start */
    {"fog_limit",      "tune_fogmax",    0,  100,   0, 0, "off=0"},          /* fog start <= P % of the far clip */
    {"detail",         "tune_lod",      30,  200, 100, 0, NULL},             /* % LOD distances */
    {"drive_distance", "tune_drivefar",  0,  100,   0, 0, "off=0"},          /* % far clip while driving fast */
    {"textures",       "tex_half",       0, 1024,   0, 0, "full=0,half=128,low=64"},   /* half size from N px */
    {"mipmaps",        "tex_mip",        0,    1,   0, 0, "off=0,on=1"},     /* texture mipmaps */
    {"mirrors",        "x_mirror",       0,    1,   1, 0, "off=0,on=1"},     /* interior mirror pass */
    {"water",          "water",          0,    1,   0, 0, "off=0,on=1"},     /* water surfaces */
    {"shadows",        "x_fxskip",       0,    1,   0, 1, "off=0,on=1"},     /* 0 = skip the shadow passes */
    /* population / far world logic */
    {"peds",           "tune_peds",      0,  100, 100, 0, NULL},             /* % pedestrian density */
    {"traffic",        "tune_cars",      0,  100, 100, 0, NULL},             /* % traffic density */
    {"far_ai",         "x_agfar",        0,  400,   0, 0, "off=0"},          /* far AI every 2nd frame beyond D m */
    {"far_freeze",     "x_agfrz",        0, 2000,   0, 0, "off=0"},          /* entities beyond F m frozen */
    {"far_collision",  "x_farcol",      -1,  400,   0, 0, "off=0"},          /* far collision every 2nd frame */
    {"camera_reuse",   "x_agcam",        0,  400,   0, 0, "off=0"},          /* camera cone cast reuse radius cm */
    /* performance / timing */
    {"show_fps",       "fps",            0,    1,   0, 0, "off=0,on=1"},     /* fps counter (L+R+SELECT in game) */
    {"game_fps",       "x_pal50",       25,   30,   1, 0, "!25=1,30=0"},     /* the game's own limit 25 | 30 */
    {"game_timing",    "x_pace",         0,    2,   0, 0, "old=0,cap=1,realtime=2"},   /* hle_bl.c (experimental) */
    {"fps_cap",        "x_fpscap",      15,   60,   0, 0, "off=0,0=0"},      /* 60/30/25/20/15 (down to the grid;
                                                                                 sets VBlank rate + game limit; 60 experimental) */
    /* overclock */

    {"overclock",      "cpu_auto",       0,  365,   0, 0, "!off=0,0=0,342=342,365=365"},
    {"overclock_reset","cpu_auto_reset", 0,    0,   0, 0, NULL},
    /* controls / sound */
    {"select_up",      "pad_sel_up",     0,    0,   0, 0, NULL},             /* r3 | l3 | stick */
    {"sound",          "audio",          0,    1,   1, 0, "off=0,on=1"},
    {"volume",         "audio_vol",      0,  100, 100, 0, NULL},
    {"intro_movies",   "movies",         0,    1,   1, 0, "off=0,on=1"},
};

const HleCfgAlias *hle_cfg_alias_table(int *n)
{
    *n = (int)(sizeof aliases / sizeof aliases[0]);
    return aliases;
}
