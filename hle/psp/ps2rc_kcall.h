/* ps2rc_kcall.h - a kernel-mode call from the EBOOT through the CFW's KUBridge library (ARK-4 and compatible CFWs).
 *
 * The CFW's KUBridge library exports kuKernelCall (NID 0x9060F69D): it runs a function of the caller in kernel mode
 * with up to 12 word arguments and stores the function's two-word result. ps2rc_kcall.S imports it under the name
 * ps2rc_kcall (the EBOOT still loads where the library is missing - the call then fails, < 0).
 * Written for PS2RC from the library's interface (its name, import flags, the NID and the argument block layout);
 * no CFW SDK header or stub library is used.
 */
#ifndef PS2RC_KCALL_H
#define PS2RC_KCALL_H

#include <stdint.h>

typedef struct Ps2rcKCall {
    uint32_t arg[12];                    /* the function's 12 word arguments, in order */
    uint32_t ret[2];                     /* its result (two words) */
} Ps2rcKCall;

/* fn(arg[0], ..., arg[11]) in kernel mode; < 0 = refused (no KUBridge library, or it declined) */
int ps2rc_kcall(void *fn, Ps2rcKCall *args);

#endif
