/* hle_bd.h - x_mirror (hle_bd.c); the GE serial probe / async flip live in the PSP GE module (hle_ge.h). */
#ifndef HLE_BD_H
#define HLE_BD_H
#include <stdint.h>

extern uint64_t hle_bd_mirror_active;  /* frames with a mirror type != 0 (gp+3900) so far */

void hle_bd_report(void);              /* end of run (called by hle_ab_report) */

#endif
