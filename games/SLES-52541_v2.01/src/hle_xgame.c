/* hle_xgame.c - this game's speed switches (kept out of the core's hle_fast.c): the switch report line and the grass
 * hooks (x_grass). The switch reader hle_x_opt is the core's (hle_fast.c). */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_xgame.h"

void hle_x_report(void)
{
    LOG("speed switches: x_pal50=%d x_dma=%d x_rbuf=%d x_vu0=%d x_rs=%d x_tex=%d x_grass=%d x_farcol=%d "
        "\n", hle_x_opt("pal50", 1), hle_x_opt("dma", 1), hle_x_opt("rbuf", 1),
        hle_x_opt("vu0", 0), hle_x_opt("rs", 1), hle_x_opt("tex", 1), hle_x_opt("grass", 1), hle_x_opt("farcol", 0));
}

/* ------------------------------------------------------------------ the grass (x_grass)
   plants_update 0x489690 (once per game_process: scans the collision triangles near the camera for plant locations)
   and plants_render 0x489960 (builds the VU1 grass packets of the game's grass renderer 0x48CB80, which the HLE
   drops - no grass is drawn on the PSP). x_grass=1 runs neither. Guest effect: the plant lists stay as they are
   and the grass code's rand() calls (0x48B580 -> rand 0x550A48) no longer happen, so the global random sequence -
   ped / car spawns - differs from x_grass=0 (a different, equally valid timeline); nothing outside the grass code reads
   the plant lists (Unknown for the PS2 build's collision helpers: the host runs to the end with it). */
void GUEST(00489690)(R5900Ctx *c);
void GUEST(00489960)(R5900Ctx *c);
static int x_grass = -1, x_grass_gen = -1;          /* re-read when hle_x_gen changes (run-time A/B) */
static uint64_t x_grass_skipped;
HLE(00489690)
{
    if (x_grass_gen != hle_x_gen) { x_grass_gen = hle_x_gen; x_grass = hle_x_opt("grass", 1) != 0; }   /* default on */
    if (x_grass) { x_grass_skipped++; return; }
    GUEST(00489690)(c);
}
HLE(00489960)
{
    if (x_grass_gen != hle_x_gen) { x_grass_gen = hle_x_gen; x_grass = hle_x_opt("grass", 1) != 0; }   /* default on */
    if (x_grass) { x_grass_skipped++; return; }
    GUEST(00489960)(c);

}
