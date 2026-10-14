/* hle_mem_psp.c - the PSP's memory mode and reserves (see hle_mem_psp.h).
 *
 * SE configuration: the CFW's SystemControl exports sctrlSEGetConfig (library SystemCtrlForUser),
 *   which copies the CFW's settings structure. The import stub is written here (the same layout as PSPSDK's
 *   IMPORT_START / IMPORT_FUNC: a .lib.stub entry + a two-word .sceStub.text slot the module loader links); no CFW
 *   header or library is used. ARK-5 byte offsets read by hle_psp.c (from the ARK-5 sources, hardware-checked by the
 *   author 2026-10-08): +8 iso_cache_partition, +27 msspeed, +34 high_memory_use. Not called on the emulator.
 *
 * Volatile partition: sceKernelVolatileMemTryLock hands the game the 4 MB the system otherwise keeps for its own
 *   dialogs / suspend. Rules (hardware findings): hold scePowerLock while it is ours (no suspend), unlock it before any
 *   sceUtility* dialog (PS2RC uses none: the memory card is files), touch the memory before trusting its size. The
 *   arena is a bump allocator: blocks stay for the whole run, except the last block, which can be given back (the
 *   intro movie's buffers).
 */
#include <pspkernel.h>
#include <pspsuspend.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "hle_mem_psp.h"

int hle_psp_is_emulator(void);                  /* psp/hle_prof_psp.c */

/* ------------------------------------------------------------------ import stub: SystemCtrlForUser */
int at_sctrlSEGetConfig(void *config);
void at_sctrlHENSetSpeed(int cpu, int bus);     /* ARK-5 overclockHandler (hle_aw_psp.c) */
__asm__(
    "    .set push\n"
    "    .pushsection .rodata.sceResident, \"a\"\n"
    "    .word 0\n"
    "at_sctrl_libname:\n"
    "    .asciz \"SystemCtrlForUser\"\n"
    "    .align 2\n"
    "    .popsection\n"
    "    .pushsection .lib.stub, \"a\", @progbits\n"
    "    .global at_sctrl_stub_entry\n"
    "at_sctrl_stub_entry:\n"
    "    .word at_sctrl_libname\n"
    "    .word 0x40090000\n"
    "    .word 0x5\n"
    "    .word __executable_start\n"
    "    .word __executable_start\n"
    "    .popsection\n"
    "    .set pop\n"
    "    .set push\n"
    "    .set noreorder\n"
    "    .pushsection .sceStub.text, \"ax\", @progbits\n"
    "    .globl at_sctrlSEGetConfig\n"
    "    .type at_sctrlSEGetConfig, @function\n"
    "    .ent at_sctrlSEGetConfig, 0\n"
    "at_sctrlSEGetConfig:\n"
    "    .word at_sctrl_stub_entry\n"
    "    .word 0x16C3B7EE\n"
    "    .end at_sctrlSEGetConfig\n"
    "    .size at_sctrlSEGetConfig, .-at_sctrlSEGetConfig\n"
    /* sctrlHENSetSpeed in the same entry = ARK-5's own PLL ladder (hle_aw_psp.c,
       cpu_clock_method=ark only; never called otherwise). Second stub after the first, second NID after the first:
       one entry, contiguous, same order. */
    "    .globl at_sctrlHENSetSpeed\n"
    "    .type at_sctrlHENSetSpeed, @function\n"
    "    .ent at_sctrlHENSetSpeed, 0\n"
    "at_sctrlHENSetSpeed:\n"
    "    .word at_sctrl_stub_entry\n"
    "    .word 0xCC9ADCF8\n"
    "    .end at_sctrlHENSetSpeed\n"
    "    .size at_sctrlHENSetSpeed, .-at_sctrlHENSetSpeed\n"
    "    .popsection\n"
    "    .pushsection .rodata.sceNid, \"a\"\n"
    "    .word 0x16C3B7EE\n"
    "    .word 0xCC9ADCF8\n"
    "    .popsection\n"
    "    .set pop\n");

int hle_mem_seconfig(uint8_t *buf, unsigned n)
{
    memset(buf, 0xA5, n);
    if (hle_psp_is_emulator()) return 0x7FFFFFFF;
    return at_sctrlSEGetConfig(buf);
}

/* ------------------------------------------------------------------ the volatile partition */
static uint8_t *vol_lo, *vol_cur, *vol_hi;
static int vol_rc = 0x7FFFFFFF, vol_power_rc = 0x7FFFFFFF, vol_size, vol_locked, vol_touch_bad;
static int vol_unpower_rc = 0x7FFFFFFF;          /* the scePowerUnlock result at the unlock (exit log) */
static unsigned vol_blocks, vol_given_back;

int hle_mem_vol_lock(void)
{
    void *p = NULL;
    int size = 0;
    vol_rc = sceKernelVolatileMemTryLock(0, &p, &size);
    if (vol_rc < 0 || !p || size < (1 << 20)) {
        if (vol_rc >= 0) sceKernelVolatileMemUnlock(0);
        vol_size = size;
        return vol_rc < 0 ? vol_rc : -1;
    }
    vol_locked = 1;
    vol_size = size;
    vol_power_rc = scePowerLock(0);                /* no suspend while the partition holds our buffers */
    /* touch every 4 KB page: write a pattern, read it back, then clear (a size we did not touch is not a size) */
    volatile uint32_t *w = p;
    unsigned nw = (unsigned)size / 4u;
    for (unsigned i = 0; i < nw; i += 1024u) w[i] = 0x5A5A0000u ^ i;
    w[nw - 1] = 0xC0DEC0DEu;
    for (unsigned i = 0; i < nw; i += 1024u) if (w[i] != (0x5A5A0000u ^ i)) vol_touch_bad++;
    if (w[nw - 1] != 0xC0DEC0DEu) vol_touch_bad++;
    memset(p, 0, (size_t)size);
    sceKernelDcacheWritebackInvalidateRange(p, (unsigned)size);
    vol_lo = vol_cur = p;
    vol_hi = vol_lo + size;
    if (vol_touch_bad) { hle_mem_vol_unlock(); return -2; }
    return 0;
}

int hle_mem_vol_on(void) { return vol_locked; }

void hle_mem_vol_info(char *out, unsigned n)
{
    snprintf(out, n, "volatile partition: TryLock %08x at %p size %d KB, scePowerLock %08x, touch errors %u, %s; "
             "%u blocks taken, %u KB used, %u KB free, %u given back", (unsigned)vol_rc, vol_lo, vol_size >> 10,
             (unsigned)vol_power_rc, vol_touch_bad, vol_locked ? "LOCKED (ours)" : "not ours", vol_blocks,
             (unsigned)((vol_cur - vol_lo) >> 10), (unsigned)(hle_mem_vol_free() >> 10), vol_given_back);
}

size_t hle_mem_vol_free(void) { return vol_locked ? (size_t)(vol_hi - vol_cur) : 0; }

void *hle_mem_vol_take(size_t size, size_t keep)
{
    if (!vol_locked || !size) return NULL;
    uintptr_t a = ((uintptr_t)vol_cur + 63u) & ~(uintptr_t)63u;
    if (a + size + keep > (uintptr_t)vol_hi) return NULL;
    vol_cur = (uint8_t *)(a + size);
    vol_blocks++;
    memset((void *)a, 0, size);
    return (void *)a;
}

int hle_mem_vol_give_back(void *p, size_t size)
{
    if (!vol_locked || !p || (uint8_t *)p + size != vol_cur) return 0;
    vol_cur = p;
    vol_blocks--;
    vol_given_back++;
    return 1;
}

int hle_mem_vol_unlock(void) { return hle_mem_vol_unlock2(0); }

/* keep_power = 1 (az_exit=1) gives the partition back but keeps the scePowerLock until the kernel reboot of
   sceKernelExitGame: a power switch / auto-sleep held off during the run would otherwise fire right here, in the
   middle of the exit, with the Media Engine taken */
int hle_mem_vol_unlock2(int keep_power)
{
    if (!vol_locked) return 0x7FFFFFFF;
    vol_locked = 0;
    int r = sceKernelVolatileMemUnlock(0);
    if (vol_power_rc >= 0 && !keep_power) vol_unpower_rc = scePowerUnlock(0);
    return r;
}

int hle_mem_vol_power_unlock_rc(void) { return vol_unpower_rc; }   /* 0x7FFFFFFF = not called */
