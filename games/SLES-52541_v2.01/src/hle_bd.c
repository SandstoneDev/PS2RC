/* hle_bd.c - interiors.
 *
 * x_mirror=0|1 (poc6.cfg; host HLE_X_MIRROR; default 1 = the guest original; run-time switchable, in-run A/B key):
 *   0 skips mirror_pass 0x2FA580 (called with no argument from game_loop 0x246760 right after the entity pre-render;
 *   a void function: its call site flushes no argument). With a mirror in the camera's cull zone (gp+3900, the mirror
 *   type: bathrooms, barber shops, restaurants) it re-aims the scene camera at the 512x256 mirror raster and runs
 *   render_scene 0x246CF0 a second time: every building / ped of the interior goes through the TRANSLATED PS2 pipeline
 *   (the HLE 3D path passes "not the screen" atomics to the guest), building VIF / GIF DMA that the HLE drops - the GE
 *   never draws the mirror raster (mirrors are not drawn; hle_ge.c skips rasters without PS2 upload packets). Measured
 *   on a PSP in two interiors with mirrors: mesh_node 0x391398 run by the guest 13-19 ms per frame (19-62 calls),
 *   entity_render self 13-23 ms, against 0.4 ms / 3 ms outdoors. Player loses: nothing on the PSP (the reflection was
 *   never shown).
 *   Guest effect: the mirror buffer keeps its old texels; the camera raster / matrix are restored by the original's
 *   own epilogue, which is skipped together with its prologue (the camera is never re-aimed).
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_fast.h"
#include "hle_bd.h"

void GUEST(002fa580)(R5900Ctx *c);
static int mir_on = 1, mir_gen = -1;
static uint64_t mir_calls, mir_active, mir_skipped;
uint64_t hle_bd_mirror_active;                     /* frames with a mirror in the cull zone (read by others) */

HLE(002fa580)                                      /* mirror_pass() */

{
    if (mir_gen != hle_x_gen) { mir_gen = hle_x_gen; mir_on = hle_x_opt("mirror", 1); }
    mir_calls++;
    if (gw(c->r[28].w[0] + 3900u)) { mir_active++; hle_bd_mirror_active++; }
    else { GUEST(002fa580)(c); return; }           /* no mirror: the original returns at once */
    if (!mir_on) { mir_skipped++; return; }
    GUEST(002fa580)(c);
}

void hle_bd_report(void)
{
    LOG("x_mirror: mirror pass %llu calls, %llu with a mirror in the zone, %llu mirror passes "
        "skipped (x_mirror=0)\n", (unsigned long long)mir_calls, (unsigned long long)mir_active,
        (unsigned long long)mir_skipped);
}
