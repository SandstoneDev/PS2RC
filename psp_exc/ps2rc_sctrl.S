/* ps2rc_sctrl.S - the import of the CFW's SystemCtrlForKernel sctrlHENFindFunction as ps2rc_find_function (see
   ps2rc_sctrl.h), with PSPSDK's own import macros: library SystemCtrlForKernel, attribute / version word 0x00090000,
   function NID 0x159AF5CC. */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "SystemCtrlForKernel",0x00090000
	IMPORT_FUNC  "SystemCtrlForKernel",0x159AF5CC,ps2rc_find_function
