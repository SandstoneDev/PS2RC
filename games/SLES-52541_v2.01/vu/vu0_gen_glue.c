/* vu0_gen_glue.c - the VU0 micro program #0, translated on the player's machine by ps2rc.vu.micro_gen (--sym vu0p,
 * output {work}/gen/vu0_prog0_gen.c on the include path), under the names the HLE calls (vu0_micro.h). */
#include "vu0_micro.h"
#include "vu0_prog0_gen.c"

int rt_vu0_callms_hle(R5900Ctx *c, uint32_t addr) { return vu0p(c, addr); }
int vu0_micro_is_prog0(const void *code, uint32_t nbytes) { return vu0p_is_code(code, nbytes); }
