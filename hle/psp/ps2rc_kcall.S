/* ps2rc_kcall.S - the import of the CFW's KUBridge kuKernelCall as ps2rc_kcall (see ps2rc_kcall.h), with PSPSDK's
   own import macros: library KUBridge, attribute / version word 0x40090000, function NID 0x9060F69D. */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "KUBridge",0x40090000
	IMPORT_FUNC  "KUBridge",0x9060F69D,ps2rc_kcall
