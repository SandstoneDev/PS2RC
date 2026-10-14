# VU0 oracle PSP EBOOT (pspdev gcc 15.2, built in WSL by build.sh from a WSL-native copy)
TARGET   = vu0
OBJS     = main.o vu0_rc.o r5900_rt.o
INCDIR   = .
CFLAGS   = -O2 -G0 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-misleading-indentation -std=gnu11 -fno-fast-math -fno-math-errno -frounding-math -ffp-contract=off
ASFLAGS  = $(CFLAGS)
LIBS     = -lm -lpsprtc -lpsppower
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = PS2RC VU0
PSP_LARGE_MEMORY = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
