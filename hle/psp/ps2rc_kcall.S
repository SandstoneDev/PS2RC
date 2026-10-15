/* ps2rc_kcall.S - the import of the CFW's KUBridge kuKernelCall as ps2rc_kcall (see ps2rc_kcall.h), with PSPSDK's
   own import macros: library KUBridge, attribute / version word 0x40090000, function NIDs 0x9060F69D (kuKernelCall) and
   0x24331850 (kuKernelGetModel, as ps2rc_kgetmodel). */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "KUBridge",0x40090000
	IMPORT_FUNC  "KUBridge",0x9060F69D,ps2rc_kcall
	IMPORT_FUNC  "KUBridge",0x24331850,ps2rc_kgetmodel
