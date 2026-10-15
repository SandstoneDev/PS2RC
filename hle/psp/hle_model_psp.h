/* hle_model_psp.h - which PSP this is (PSP build only): the 32 MB PSP-1000 or a 64 MB model (PSP-2000, PSP-3000,
 * PSP Go, ...). The game needs the 64 MB models: the PSP-1000 has 24 MB of user memory, the port needs ~50 MB
 * (translated code ~13 MB + the PS2 RAM window ~28 MB + buffers). hle_psp.c shows a "not supported" screen on a
 * PSP-1000 and keeps the "turn on ARK's extra memory" advice for 64 MB models that run in 24 MB mode.
 *
 * Detection, most direct first (all read only, kernel calls only through the CFW's KUBridge; nothing on the emulator):
 *   1. the RAM size register 0xBC100040 (bits 0-1: 16 << n MB), read in kernel mode through kuKernelCall;
 *   2. kuKernelGetModel() (0 = 01g = PSP-1000, 4 = 05g = PSP Go, ...) - only trusted when the kernel call worked
 *      (without the KUBridge library its import is not linked), for the log and when the register says nothing;
 *   3. nothing known (emulator, no KUBridge): ram_mb 0 - hle_psp.c then names both possible causes.
 */
#ifndef HLE_MODEL_PSP_H
#define HLE_MODEL_PSP_H

typedef struct HleModel {
    int ram_mb;           /* 32 or 64 (or 16 / 128 from the register); 0 = Unknown */
    int gen;              /* 1..11 = 01g..11g (kuKernelGetModel() + 1); 0 = Unknown */
    unsigned reg;         /* raw 0xBC100040 (0 = not read) */
    int kcall_rc;         /* kuKernelCall result of the register read (0x7FFFFFFF = not called: emulator) */
    int fake;             /* 1 = ps2rc.cfg mem_fake_model (TEST ONLY) replaced the answer */
    const char *source;   /* "register", "model", "emulator", "no kernel bridge", "TEST mem_fake_model" */
} HleModel;

/* detects once (later calls return the same answer); never NULL */
const HleModel *hle_model_get(void);
/* TEST ONLY (ps2rc.cfg mem_fake_model=N): pretend kuKernelGetModel() = N and a matching RAM register (N = 0: 32 MB) */
void hle_model_fake(int model);
/* 1 = a 32 MB console (PSP-1000), 0 = 64 MB or more, -1 = Unknown */
int hle_model_is_32mb(void);
/* one line for session.log, no newline: "PSP-1000 (32 MB, 01g; register 0x...)" */
void hle_model_describe(char *out, unsigned n);

#endif
