/* hle_settings_menu.c - this game's settings registry for the in-game menu (hle/psp/hle_menu.cpp; format
 * and rules: hle/core/hle_settings_reg.h). One line per setting. Friendly names come from hle_settings.c (range, words,
 * code default); "rec" = the value of the player template cfg/player_en.cfg (tests/cfg/test_cm_menu.py checks it).
 * Live apply = the run-time switches of the in-run A/B tester (per-frame consumers that re-read
 * their key), plus the controls, the fps counter, the log text, the volume and the frame cap; everything else applies
 * after a restart. The PSP-only setters are weak: the host build links without them. */
#include <stdlib.h>
#include <string.h>
#include "hle_cfg.h"
#include "hle_settings_reg.h"
#include "hle_fast.h"                /* hle_x_set */

int hle_tune_set(const char *k, const char *v) __attribute__((weak));        /* hle_tune.c */
int hle_ge_tex_cfg(const char *k, const char *v) __attribute__((weak));      /* psp/hle_ge.c */
void hle_sky_cfg(const char *k, const char *v) __attribute__((weak));        /* hle_sky.c (also forwards water) */
void hle_ge_show_fps(int on) __attribute__((weak));                          /* psp/hle_ge.c */
void hle_ge_text_rows(int rows) __attribute__((weak));                       /* psp/hle_ge.c */
void hle_pad_reconfig(void) __attribute__((weak));                           /* hle/psp/hle_pad.c */
void hle_audio_set_volume(int pct) __attribute__((weak));                    /* psp/hle_audio_psp.c */
int hle_ge_dither_cfg(const char *k, const char *v) __attribute__((weak));   /* psp/hle_ge.c */

/* ------------------------------------------------------------------ live apply (internal key, internal value) */
static int ap_x(const char *k, const char *v) { return !strncmp(k, "x_", 2) && hle_x_set(k + 2, (int)strtol(v, NULL, 0)); }
static int ap_tune(const char *k, const char *v) { return hle_tune_set && !strncmp(k, "tune_", 5) && hle_tune_set(k + 5, v); }
static int ap_tex(const char *k, const char *v) { if (!hle_ge_tex_cfg) return 0; hle_ge_tex_cfg(k, v); return 1; }
static int ap_sky(const char *k, const char *v) { if (!hle_sky_cfg) return 0; hle_sky_cfg(k, v); return 1; }
static int ap_fps(const char *k, const char *v) { (void)k; if (!hle_ge_show_fps) return 0; hle_ge_show_fps(atoi(v)); return 1; }
static int ap_text(const char *k, const char *v) { (void)k; if (!hle_ge_text_rows) return 0; hle_ge_text_rows(atoi(v)); return 1; }
static int ap_pad(const char *k, const char *v) { (void)k; (void)v; if (!hle_pad_reconfig) return 0; hle_pad_reconfig(); return 1; }
static int ap_dith(const char *k, const char *v) { if (!hle_ge_dither_cfg) return 0; hle_ge_dither_cfg(k, v); return 1; }
static int ap_vol(const char *k, const char *v) { (void)k; if (!hle_audio_set_volume) return 0; hle_audio_set_volume(atoi(v)); return 1; }

#define COMBO_L2 "select+l,l+r,select+square,r+square,select+cross,none"
#define COMBO_R2 "select+r,r+square,l+r,select+cross,select+square,none"
#define COMBO_L3 "select+triangle,l+triangle,r+triangle,select+square,none"
#define FACE "cross,circle,square,triangle,none"
#define COMBO_R3 "select+circle,r+circle,l+circle,select+cross,none"

static const HleSetting reg[] = {
/*  key               cat            label                     help                                                                                 rec      choices          lo hi step def unit  apply   flags */
    /* graphics */
    {"draw_distance", "Graphics", "Draw distance", "How far the world is drawn, in percent of the original. Higher: you see further, but the game is slower.", "40", NULL, 0, 0, 5, NULL, " %", ap_tune, 0},
    {"fog", "Graphics", "Fog start", "Where the fog starts, in percent. Higher: the fog is further away and thinner, so a short draw distance is no grey wall.", "250", NULL, 0, 0, 10, NULL, " %", ap_tune, 0},
    {"fog_limit", "Graphics", "Fog limit", "The fog starts no further than this share of the draw distance (0 = off): the far edge fades softly into the fog.", "80", NULL, 0, 0, 5, NULL, " %", ap_tune, 0},
    {"detail", "Graphics", "Model detail distance", "Distance at which models switch to simpler ones, in percent. Lower: faster, but close objects get blurry.", "100", NULL, 0, 0, 10, NULL, " %", ap_tune, 0},
    {"drive_distance", "Graphics", "Draw distance while driving", "Draw distance while driving fast, in percent of the draw distance (0 = off). Faster in a fast car.", "75", NULL, 0, 0, 5, NULL, " %", ap_tune, 0},
    {"textures", "Graphics", "Texture sharpness", "full = as the original, half = half size (faster, softer close up), low = softer still.", "full", "full,half,low", 0, 0, 0, NULL, NULL, ap_tex, 0},
    {"mipmaps", "Graphics", "Texture smoothing (mipmaps)", "Smooths distant textures so they do not shimmer; same speed.", "1", NULL, 0, 0, 0, NULL, NULL, ap_tex, 0},
    {"mirrors", "Graphics", "Mirrors in interiors", "Interior mirrors. Off: interiors with mirrors run much faster; outdoors there is no difference.", "0", NULL, 0, 0, 0, NULL, NULL, ap_x, 0},
    {"water", "Graphics", "Water surfaces", "Draws the water surfaces (barely slower).", "1", NULL, 0, 0, 0, NULL, NULL, ap_sky, 0},
    {"shadows", "Graphics", "Shadows and trails", "Shadows of cars and people, plane trails. Off: a little faster.", "1", NULL, 0, 0, 0, NULL, NULL, ap_x, 0},
    {"dither", "Graphics", "Dither", "The fine dot pattern that hides colour steps of a 16-bit picture: opaque = only on solid surfaces, on = everywhere, off = nowhere.", "opaque", NULL, 0, 0, 0, NULL, NULL, ap_dith, 0},
    {"color_depth", "Graphics", "Colour depth", "16 or 32 bits while the picture is drawn. 32: see-through effects blend exactly, no dot pattern.", "32", NULL, 0, 0, 0, NULL, " bit", NULL, 0},
    {"lamp_distance", "Graphics", "Street light distance", "Lamps, traffic lights and neon signs (glow, light pool, light) only up to this many metres (0 = off, as the original). Faster at night; 60 is a fast value.", NULL, NULL, 0, 0, 10, NULL, " m", ap_x, 0},
    {"light_pools", "Graphics", "Light pool distance", "Pools of light on the ground up to this share of the original distance (0 = off, as the original). Faster at night; 50 is a fast value.", NULL, NULL, 0, 0, 5, NULL, " %", ap_x, 0},
    {"hud_aspect", "Graphics", "HUD proportions", "fix = the HUD keeps its 4:3 proportions in the screen corners (round radar), stretch = stretched over the wide screen.", "fix", NULL, 0, 0, 0, NULL, NULL, NULL, 0},
    {"hud_scale", "Graphics", "HUD size", "Size of the corner HUD (money, clock, health, weapon, radar), in percent.", "125", NULL, 0, 0, 5, NULL, " %", NULL, 0},
    {"hud_outline", "Graphics", "HUD text outline", "A thin black outline around the HUD text.", "1", NULL, 0, 0, 0, NULL, NULL, NULL, 0},
    {"hud_radar", "Graphics", "Radar shape", "With fixed HUD proportions: round = round on the PSP screen, ps2 = as the original.", "round", NULL, 0, 0, 0, NULL, NULL, NULL, 0},
    /* population / far world */
    {"peds", "World", "Pedestrians", "How many pedestrians, in percent of the original. Higher: a livelier city, but clearly slower.", "25", NULL, 0, 0, 5, NULL, " %", NULL, 0},
    {"traffic", "World", "Traffic", "How many cars on the roads, in percent of the original. Higher: more cars, but slower.", "25", NULL, 0, 0, 5, NULL, " %", NULL, 0},
    {"far_ai", "World", "Far AI every 2nd frame", "People and objects further than this many metres think every second frame (0 = off). Faster, not visible.", "20", NULL, 0, 0, 5, NULL, " m", ap_x, 0},
    {"far_freeze", "World", "Freeze far objects", "Objects further than this many metres stand still until you come closer (0 = off).", "300", NULL, 0, 0, 50, NULL, " m", ap_x, 0},
    {"far_collision", "World", "Far collisions every 2nd frame", "Collisions of distant people and cars are checked every second frame beyond this many metres (0 = off).", "20", NULL, 0, 0, 5, NULL, " m", ap_x, 0},
    {"camera_reuse", "World", "Camera wall check reuse", "The camera keeps its wall check while it moved less than this many centimetres (0 = off). May lag one frame.", "25", NULL, 0, 0, 5, NULL, " cm", ap_x, 0},
    /* performance */
    {"show_fps", "Performance", "Frame counter", "Frames per second in the top-left corner. In game: hold L+R and press SELECT.", "0", NULL, 0, 0, 0, NULL, NULL, ap_fps, 0},
    {"fps_cap", "Performance", "Frame cap", "Frames per second limit: 30 (as the original), 25, 20, 15 or 60 (experimental); off = no limit. 25 is exact PAL timing only after a restart.", "30", "off,15,20,25,30,60", 0, 0, 0, NULL, " fps", ap_x, 0},
    {"game_fps", "Performance", "Game frame limit", "The game's own limit, 25 or 30 frames per second.", NULL, NULL, 0, 0, 0, NULL, " fps", NULL, 0},
    {"game_timing", "Performance", "Game clock", "old / cap / realtime: how the game clock follows the real clock (experimental).", NULL, "old,cap,realtime", 0, 0, 0, NULL, NULL, NULL, 0},
    {"overclock", "Performance", "CPU overclock", "Automatic overclock (ARK only): off = 333 MHz, 342 or 365 MHz. At your own risk; it steps down by itself after a crash.", "0", NULL, 0, 0, 0, NULL, " MHz", NULL, 0},
    {"overclock_reset", "Performance", "Overclock reset counter", "Change this number to allow the overclock again after it switched itself off.", NULL, "0,1,2,3,4,5,6,7,8,9", 0, 0, 0, NULL, NULL, NULL, 0},
    /* controls (hle/psp/hle_padmap.h checks every combination: a clash keeps the built-in value) */
    {"controls_preset", "Controls", "Layout", "default = SELECT combinations; stories = the handheld layout (hold L and move the analog stick to turn the camera).", NULL, "default,stories", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"modifier", "Controls", "Modifier button", "The button you hold for the built-in combinations: select, l, r or none.", NULL, NULL, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"select_to_game", "Controls", "SELECT reaches the game", "always = whenever SELECT is held; tap = only a lone press and release (the camera view no longer changes on combinations).", NULL, NULL, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_l2", "Controls", "L2 button", "The PS2 L2 button (on foot: previous weapon or target; in a car: look left).", NULL, COMBO_L2, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_r2", "Controls", "R2 button", "The PS2 R2 button (on foot: next weapon or target; in a car: look right).", NULL, COMBO_R2, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_l3", "Controls", "L3 button", "The PS2 L3 button, a press on the left stick (on foot: crouch; in a car: horn).", NULL, COMBO_L3, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_r3", "Controls", "R3 button", "The PS2 R3 button, a press on the right stick (on foot: look behind; in a car: side missions).", NULL, COMBO_R3, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_rstick", "Controls", "Right stick", "How you move the right stick (camera, hydraulics): a button + the D-pad, a button + the analog stick, or none.", NULL, "select+dpad,l+dpad,r+dpad,r+analog,analog,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"select_up", "Controls", "Up on the right-stick D-pad", "With the right stick on the D-pad, what Up does: stick = right stick up, r3 = the R3 button, l3 = the L3 button.", NULL, "stick,r3,l3", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"camera_invert", "Controls", "Invert camera", "The camera on the analog stick (stories: hold L and move the stick): x = left / right reversed, y = up / down reversed, xy = both.", NULL, NULL, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_l1", "Controls", "L1 button", "The PS2 L1 button.", NULL, "l,select+l,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_r1", "Controls", "R1 button", "The PS2 R1 button.", NULL, "r,select+r,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_select", "Controls", "SELECT button", "The PS2 SELECT button. To swap SELECT and START, set both.", NULL, "select,start,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_start", "Controls", "START button", "The PS2 START button. To swap SELECT and START, set both.", NULL, "start,select,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_cross", "Controls", "Cross button", "The PS2 Cross button (sprint, accelerate).", NULL, FACE, 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_circle", "Controls", "Circle button", "The PS2 Circle button (attack, car weapon).", NULL, "circle,cross,square,triangle,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_square", "Controls", "Square button", "The PS2 Square button (jump, brake).", NULL, "square,cross,circle,triangle,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_triangle", "Controls", "Triangle button", "The PS2 Triangle button (enter or leave a vehicle).", NULL, "triangle,circle,cross,square,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_up", "Controls", "D-pad up", "The PS2 D-pad up (recruit; in a car: radio station).", NULL, "up,l+up,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_down", "Controls", "D-pad down", "The PS2 D-pad down (gang: hold position; in a car: radio station).", NULL, "down,l+down,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_left", "Controls", "D-pad left", "The PS2 D-pad left (answer no).", NULL, "left,l+left,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_right", "Controls", "D-pad right", "The PS2 D-pad right (answer yes; skip a trip).", NULL, "right,l+right,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    {"map_lstick", "Controls", "Left stick", "The PS2 left stick (walk, steer): the analog stick, the D-pad, or none.", NULL, "analog,dpad,none", 0, 0, 0, NULL, NULL, ap_pad, 0},
    /* in a vehicle only: "foot" (first) = not set, the on-foot value / the layout's vehicle layer applies (HS_UNSET) */
    {"car_l2", "Controls", "In a vehicle: L2", "The PS2 L2 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot," COMBO_L2, 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_r2", "Controls", "In a vehicle: R2", "The PS2 R2 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot," COMBO_R2, 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_l3", "Controls", "In a vehicle: L3", "The PS2 L3 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot," COMBO_L3, 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_r3", "Controls", "In a vehicle: R3", "The PS2 R3 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot," COMBO_R3, 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_rstick", "Controls", "In a vehicle: right stick", "The PS2 right stick while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,select+dpad,l+dpad,r+dpad,r+analog,analog,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_l1", "Controls", "In a vehicle: L1", "The PS2 L1 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,l,select+l,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_r1", "Controls", "In a vehicle: R1", "The PS2 R1 while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,r,select+r,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_select", "Controls", "In a vehicle: SELECT", "The PS2 SELECT while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,select,start,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_start", "Controls", "In a vehicle: START", "The PS2 START while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,start,select,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_cross", "Controls", "In a vehicle: Cross", "The PS2 Cross while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot," FACE, 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_circle", "Controls", "In a vehicle: Circle", "The PS2 Circle while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,circle,cross,square,triangle,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_square", "Controls", "In a vehicle: Square", "The PS2 Square while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,square,cross,circle,triangle,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_triangle", "Controls", "In a vehicle: Triangle", "The PS2 Triangle while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,triangle,circle,cross,square,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_up", "Controls", "In a vehicle: D-pad up", "The PS2 D-pad up while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,up,l+up,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_down", "Controls", "In a vehicle: D-pad down", "The PS2 D-pad down while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,down,l+down,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_left", "Controls", "In a vehicle: D-pad left", "The PS2 D-pad left while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,left,l+left,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_right", "Controls", "In a vehicle: D-pad right", "The PS2 D-pad right while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,right,l+right,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    {"car_lstick", "Controls", "In a vehicle: left stick", "The PS2 left stick while you are in a vehicle. foot = the same as on foot (or the layout's vehicle value); the other values only in a vehicle.", NULL, "foot,analog,dpad,none", 0, 0, 0, NULL, NULL, ap_pad, HS_UNSET},
    /* sound and video */
    {"sound", "Audio", "Sound", "Game sound on or off.", "1", NULL, 0, 0, 0, NULL, NULL, NULL, 0},
    {"volume", "Audio", "Volume", "Sound volume, 0 to 100.", "100", NULL, 0, 0, 5, NULL, " %", ap_vol, 0},
    {"intro_movies", "Audio", "Intro movies", "Movies at start: on = play, off = skip. X skips a movie while it plays.", "1", NULL, 0, 0, 0, NULL, NULL, NULL, 0},
    /* developer / run options (ps2rc_dev.cfg) */
    {"ge_text", "Developer", "Log text rows on screen", "Rows of the log shown over the picture (0 = hidden; in game L+R+SELECT+Triangle).", NULL, NULL, 0, 34, 1, "12", NULL, ap_text, HS_DEV},
    {"x_aflip", "Developer", "Async flip", "Flip without waiting for the GE (needs ge_aflip=1 at boot).", NULL, NULL, 0, 1, 1, "1", NULL, ap_x, HS_DEV},
    {"sky", "Developer", "HLE sky", "The HLE sky (1) or the original sky code (0).", NULL, NULL, 0, 1, 1, "1", NULL, ap_sky, HS_DEV},
    {"vcallms", "Developer", "VU0 micro-programs", "hle = the HLE versions, nohit / strict = diagnostics.", NULL, "hle,nohit,strict", 0, 0, 0, "strict", NULL, NULL, HS_DEV},
    {"mainloop_iterations", "Developer", "Main-loop iteration limit", "Test runs stop after this many main-loop iterations (0 = no limit): change it in the file.", NULL, NULL, 0, 0, 0, "0", NULL, NULL, HS_DEV | HS_RO},
    {"screen", "Developer", "Debug screen", "1 = the boot's debug text screen.", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"verbose", "Developer", "Verbose log", "More log lines (0 = normal).", NULL, NULL, 0, 3, 1, "0", NULL, NULL, HS_DEV},
    {"pace", "Developer", "Real-time pacing", "1 = the game never runs ahead of real time.", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"x_pal50", "Developer", "PAL 50 Hz game limit", "1 = 25 fps game limit (PAL), 0 = 30.", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"x_pace", "Developer", "Game clock mode", "0 old, 1 cap, 2 real time.", NULL, NULL, 0, 2, 1, "0", NULL, NULL, HS_DEV},
    {"x_bootpace", "Developer", "Loading screens at real time", "2 = real-time pacing during the boot (loading music speed).", NULL, NULL, 0, 2, 1, "0", NULL, NULL, HS_DEV},
    {"max_seconds", "Developer", "Run time limit", "The run stops after this many seconds.", NULL, NULL, 60, 36000, 600, "600", " s", NULL, HS_DEV},
    {"stuck_seconds", "Developer", "Watchdog", "Seconds without progress before the watchdog stops the run.", NULL, NULL, 10, 600, 10, "120", " s", NULL, HS_DEV},
    {"audio_stats", "Developer", "Audio statistics period", "Seconds between audio statistics lines (0 = only at the end).", NULL, NULL, 0, 600, 10, "60", " s", NULL, HS_DEV},
    {"audio_log", "Developer", "Audio driver log", "1 = the sound driver's progress lines in session.log.", NULL, NULL, 0, 1, 1, "0", NULL, NULL, HS_DEV},
    {"log_sync", "Developer", "Log commits", "1 = every line at once, 2 = buffered (committed by the watchdog).", NULL, NULL, 0, 2, 1, "1", NULL, NULL, HS_DEV},
    {"log_commit_ms", "Developer", "Log commit period", "Milliseconds between buffered log commits.", NULL, NULL, 10, 5000, 50, "500", " ms", NULL, HS_DEV},
    {"disc_async", "Developer", "Asynchronous disc reads", "1 = disc reads on a helper thread.", NULL, NULL, 0, 1, 1, "0", NULL, NULL, HS_DEV},
    {"az_exit", "Developer", "Exit mode", "The exit / power-off handling mode.", NULL, NULL, 0, 2, 1, "0", NULL, NULL, HS_DEV},
    {"menu_pool_kb", "Developer", "Menu memory", "Kilobytes the in-game menu takes while it is open (from free memory).", NULL, NULL, 192, 1024, 32, "320", " KB", NULL, HS_DEV},
    {"cpu_clock_base", "Developer", "CPU clock at start", "auto = leave a CFW clock alone (333 only where the system left less), 333 = always set 333 (old builds), off = never set a clock.", NULL, "auto,333,off", 0, 0, 0, "auto", NULL, NULL, HS_DEV},
    {"clock_log", "Developer", "Clock log", "1 = the clock samples in session.log.", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"x_cbatch", "Developer", "Corona batch", "1 = the coronas' sprites in one GE draw per render state (same picture).", NULL, NULL, 0, 1, 1, "1", NULL, ap_x, HS_DEV},
    {"x_clock", "Developer", "Hold the game clock", "Test aid: the in-game clock is held at HHMM (0 = off).", NULL, NULL, 0, 2400, 100, "0", NULL, ap_x, HS_DEV},
    {"x_farmis", "Developer", "Far policy spares missions", "1 = mission entities are never frozen or thinned by the far_ settings.", NULL, NULL, 0, 1, 1, "1", NULL, ap_x, HS_DEV},
    {"x_scrguard", "Developer", "Script guard", "1 = a script with an invalid command is stopped (logged) instead of crashing the game.", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"w3d_ldirfix", "Developer", "Light direction per draw", "1 = each light direction is read per draw (0 = once per frame, the old flicker).", NULL, NULL, 0, 1, 1, "1", NULL, NULL, HS_DEV},
    {"ge_dither_mx", "Developer", "Weak dither matrix", "1 = a weaker dither matrix (with dither on).", NULL, NULL, 0, 1, 1, "0", NULL, ap_dith, HS_DEV},
    {"light_log", "Developer", "Light log", "Lighting inputs in session.log every N main-loop iterations (0 = off).", NULL, NULL, 0, 600, 1, "0", NULL, NULL, HS_DEV},
    {"tune_strmem", "Developer", "Streaming budget", "Kilobytes of the game heap for streaming (memory layout: change it in the file).", NULL, NULL, 0, 0, 0, NULL, " KB", NULL, HS_DEV | HS_RO},
    {"heap_extra_kb", "Developer", "Extra game heap", "Memory layout key: change it in the file.", NULL, NULL, 0, 0, 0, NULL, " KB", NULL, HS_DEV | HS_RO},
    {"mem_volatile", "Developer", "Volatile memory", "Memory layout key: change it in the file.", NULL, NULL, 0, 0, 0, NULL, NULL, NULL, HS_DEV | HS_RO},
    {"render", "Developer", "Renderer", "ge = the GE picture, text = the debug console only.", NULL, "ge,text", 0, 0, 0, "ge", NULL, NULL, HS_DEV},
};

const HleSetting *hle_settings_table(int *n)
{
    *n = (int)(sizeof reg / sizeof reg[0]);
    return reg;
}
