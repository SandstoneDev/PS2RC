# PoC-1 PSP EBOOT (pspdev gcc 15.2, built in WSL by build.sh)
ROOT     = ../../..
TARGET   = poc1
OBJS     = main.o poc1_rc.o
INCDIR   = $(ROOT)/runtime $(ROOT)/build/poc1/rc
CFLAGS   = -O2 -G0 -Wall -Wno-unused-label -Wno-misleading-indentation -std=gnu11 -fno-fast-math -fno-math-errno -frounding-math -ffp-contract=off
ASFLAGS  = $(CFLAGS)
LIBS     = -lm -lpsprtc
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = PS2RC PoC-1
PSP_LARGE_MEMORY = 1

vpath poc1_rc.c $(ROOT)/build/poc1/rc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
