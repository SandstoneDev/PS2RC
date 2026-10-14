/* snd_unit.c - the game's sound driver module, translated from the player's own disc (ps2rc.iop, generator snddrv:
 * snddrv_gen.c), on the generic IOP runtime (ps2rc/iop/rt) with its glue snd_glue.c; snd_driver.h is the interface.
 * One unit, so the IOP runtime gets this build's address-space size: PSP 1 MB (the driver's peak use is
 * 1,010,176 B; every address wraps inside it). */
#ifdef HLE_PSP
#define IOP_MEM_MASK 0xFFFFFu
#endif
#include "iop_rt.c"
#include "iop_rt_libs.c"
#ifndef HLE_PSP
#include "iop_rt_ucontext.c"
#endif
#include "snddrv_gen.c"
#include "snd_glue.c"
