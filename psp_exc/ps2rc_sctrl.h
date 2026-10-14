/* ps2rc_sctrl.h - the CFW's function finder for the kernel PRX (ARK-4 and compatible CFWs).
 *
 * The CFW's SystemCtrlForKernel library exports sctrlHENFindFunction (NID 0x159AF5CC): the address of a function by
 * module name, library name and NID (old NIDs resolved by the CFW), 0 when not found. ps2rc_sctrl.S imports it under
 * the name ps2rc_find_function. Written for PS2RC from the library's interface (its name, import flags and the NID);
 * no CFW SDK header or stub library is used.
 */
#ifndef PS2RC_SCTRL_H
#define PS2RC_SCTRL_H

#include <stdint.h>

uint32_t ps2rc_find_function(const char *module, const char *library, uint32_t nid);

#endif
