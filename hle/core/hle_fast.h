/* hle_fast.h - speed switches and native fast paths.
 *
 * Every switch is a key "x_<name>" of the PSP run options file (next to the EBOOT, so thin A/B folders differ only in
 * that file) and an environment variable "HLE_X_<NAME>" on the host (same defaults on both). The game pack may define
 * more (its own header); the core's:
 *   x_pal50   1: sceGsResetGraph's omode follows the PS2 libgraph / SetGsCrt values (2 NTSC 60 Hz, 3 PAL 50 Hz) - a
 *                PAL game then runs its 50 Hz VBlank instead of 60 Hz (0 = the old, swapped constants).
 *                CHANGES THE GUEST TIMELINE (correctly).
 *   x_dma     1: scratchpad DMA (fromSPR / toSPR) copies contiguous runs with one memcpy instead of one call per
 *                quadword (same bytes, same order of effects; guest-identical)
 */
#ifndef HLE_FAST_H
#define HLE_FAST_H

#include <stdint.h>

int hle_x_opt(const char *name, int def);      /* "pal50" -> cfg x_pal50 / env HLE_X_PAL50; read once */
/* Run-time overrides (an in-run interleaved A/B): hle_x_set("name", 25) makes hle_x_opt("name", ..) answer 25 and
   bumps hle_x_gen; a consumer that caches a key switchable at run time re-reads it when hle_x_gen changed. Without
   any hle_x_set call nothing changes (hle_x_gen stays 0). 1 = ok, 0 = table full / name too long. */
extern int hle_x_gen;
int hle_x_set(const char *name, int val);


extern int hle_x_dma;                          /* cached x_dma (hle_mmio.c hot path) */

#endif
