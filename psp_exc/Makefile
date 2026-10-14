# ps2rc_exc.prx - the kernel half of the PS2RC crash handler (src/psp_exc/ps2rc_exc.h); built by build.sh
TARGET   = ps2rc_exc
OBJS     = exc_main.o exc_asm.o exc_me.o ps2rc_sctrl.o
INCDIR   = .
CFLAGS   = -O2 -G0 -Wall
ASFLAGS  = $(CFLAGS)
BUILD_PRX       = 1
USE_KERNEL_LIBS = 1
USE_KERNEL_LIBC = 1
PSP_FW_VERSION  = 660
# exports.exp: module_start, module_stop, module_info (module_stop runs only when it is exported)
PRX_EXPORTS     = exports.exp
# the CFW's sctrlHENFindFunction: ps2rc_sctrl.S (own import)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
