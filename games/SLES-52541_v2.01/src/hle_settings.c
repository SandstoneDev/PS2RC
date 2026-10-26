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
 *          as given (pad_sel_up, cpu_auto_reset); with "!word=n,..." words only those words (lower case)
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
    /* psp/hle_ge.c dither policy (opaque = no dither on semi-transparent draws) and the 8888 picture */
    {"dither",         "ge_dither",      0,    2,   1, 0, "!off=0,opaque=1,on=2,0=0,1=1,2=2"},
    {"color_depth",    "ge_p32",        16,   32,   1, 0, "!16=0,32=1"},
    /* night lights (hle_ce.c): distance limits of the entity lights and light pools, 0 = off */
    {"lamp_distance",  "x_lamp",         0, 1000,   0, 0, "off=0"},          /* lamps / coronas only up to D m */
    {"light_pools",    "x_lpool",        0,   99,   0, 0, "off=0"},          /* light pools to P % of 40 m */
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
    /* the player's control mapping (hle/psp/hle_padmap.h checks the combinations: "controls:" log line) */
    {"modifier",       "pad_mod",        0,    0,   0, 0, "!select=0,l=1,r=2,none=3"},
    {"select_to_game", "pad_sel_game",   0,    0,   0, 0, "!always=0,tap=1"},
    {"map_l2",         "pad_l2",         0,    0,   0, 0, NULL},             /* a combination: select+l */
    {"map_r2",         "pad_r2",         0,    0,   0, 0, NULL},
    {"map_l3",         "pad_l3",         0,    0,   0, 0, NULL},
    {"map_r3",         "pad_r3",         0,    0,   0, 0, NULL},
    {"map_rstick",     "pad_rstick",     0,    0,   0, 0, NULL},             /* select+dpad | r+analog | none */
    {"map_l1",         "pad_l1",         0,    0,   0, 0, NULL},
    {"map_r1",         "pad_r1",         0,    0,   0, 0, NULL},
    {"map_select",     "pad_select",     0,    0,   0, 0, NULL},
    {"map_start",      "pad_start",      0,    0,   0, 0, NULL},
    /* full remap + presets (inv -1 = logged in the "controls:" lines, not in "settings:") */
    {"controls_preset","pad_preset",     0,    0,   0, -1, "!default=0,stories=1,vcs=1"},
    {"camera_invert",  "pad_cam_inv",    0,    0,   0, -1, "!off=0,x=1,y=2,xy=3"},   /* analog-stick camera axes */
    {"map_cross",      "pad_cross",      0,    0,   0, -1, NULL},
    {"map_circle",     "pad_circle",     0,    0,   0, -1, NULL},
    {"map_square",     "pad_square",     0,    0,   0, -1, NULL},
    {"map_triangle",   "pad_triangle",   0,    0,   0, -1, NULL},
    {"map_up",         "pad_up",         0,    0,   0, -1, NULL},
    {"map_down",       "pad_down",       0,    0,   0, -1, NULL},
    {"map_left",       "pad_left",       0,    0,   0, -1, NULL},
    {"map_right",      "pad_right",      0,    0,   0, -1, NULL},
    {"map_lstick",     "pad_lstick",     0,    0,   0, -1, NULL},     /* analog | dpad | none */
    /* in a vehicle only (unset = the map_* value / the preset's vehicle layer) */
    {"car_l2",         "pad_c_l2",       0,    0,   0, -1, NULL},
    {"car_r2",         "pad_c_r2",       0,    0,   0, -1, NULL},
    {"car_l3",         "pad_c_l3",       0,    0,   0, -1, NULL},
    {"car_r3",         "pad_c_r3",       0,    0,   0, -1, NULL},
    {"car_rstick",     "pad_c_rstick",   0,    0,   0, -1, NULL},
    {"car_l1",         "pad_c_l1",       0,    0,   0, -1, NULL},
    {"car_r1",         "pad_c_r1",       0,    0,   0, -1, NULL},
    {"car_select",     "pad_c_select",   0,    0,   0, -1, NULL},
    {"car_start",      "pad_c_start",    0,    0,   0, -1, NULL},
    {"car_cross",      "pad_c_cross",    0,    0,   0, -1, NULL},
    {"car_circle",     "pad_c_circle",   0,    0,   0, -1, NULL},
    {"car_square",     "pad_c_square",   0,    0,   0, -1, NULL},
    {"car_triangle",   "pad_c_triangle", 0,    0,   0, -1, NULL},
    {"car_up",         "pad_c_up",       0,    0,   0, -1, NULL},
    {"car_down",       "pad_c_down",     0,    0,   0, -1, NULL},
    {"car_left",       "pad_c_left",     0,    0,   0, -1, NULL},
    {"car_right",      "pad_c_right",    0,    0,   0, -1, NULL},
    {"car_lstick",     "pad_c_lstick",   0,    0,   0, -1, NULL},
    {"sound",          "audio",          0,    1,   1, 0, "off=0,on=1"},
    {"volume",         "audio_vol",      0,  100, 100, 0, NULL},
    {"intro_movies",   "movies",         0,    1,   1, 0, "off=0,on=1"},
    /* HUD on the PSP screen (hle_hud.c) */
    {"hud_aspect",     "hud_aspect",     0,    0,   0, 0, "!fix=1,stretch=0"},   /* 4:3 HUD + round radar */
    {"hud_scale",      "hud_scale",     50,  200, 100, 0, NULL},             /* % size of the corner HUD */
    {"hud_outline",    "hud_outline",    0,    1,   0, 0, "off=0,on=1"},     /* black outline of the HUD text */
    {"hud_radar",      "hud_radar",      0,    0,   0, 0, "!round=0,ps2=1"}, /* radar shape */
};

const HleCfgAlias *hle_cfg_alias_table(int *n)
{
    *n = (int)(sizeof aliases / sizeof aliases[0]);
    return aliases;
}
