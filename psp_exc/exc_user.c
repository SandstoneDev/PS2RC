/* exc_user.c - user half of the PS2RC crash handler (see ps2rc_exc.h): loads ps2rc_exc.prx, formats the report. */
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include "ps2rc_kcall.h"                 /* KUBridge kernel call (own import) */
#include <stdio.h>
#include <string.h>
#include "ps2rc_exc.h"

uint32_t ps2rc_crash_regs[PS2RC_EXC_WORDS] __attribute__((aligned(64)));
/* the Media Engine sleep block (hle/psp/hle_me.c uses it once the module has taken it) */
Ps2rcMeBlock ps2rc_me_block __attribute__((aligned(64)));

static SceUID exc_mod = -1;
static SceUID exc_mod_last = -1;                  /* the uid after the unload, for the check */

/* sceKernelQueryModuleInfo on the module (the loaded one, else the last one) - the exit log compares
   the answer before the stop with the one after the unload (a gone uid answers "unknown module", 0x8002012E) */
int ps2rc_exc_query(void)
{
    SceUID id = exc_mod >= 0 ? exc_mod : exc_mod_last;
    if (id < 0) return 0x7FFFFFFF;
    SceKernelModuleInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    return sceKernelQueryModuleInfo(id, &info);
}

/* build 26: before sceKernelExitGame - module_stop disarms and gives the kernel's default exception handler back, then
   the module is unloaded (stop the counter thread first: hle_psp_perf_shutdown) */
void ps2rc_exc_uninstall(void) { ps2rc_exc_uninstall2(1, NULL, NULL); }

/* stop (disarm + default exception handler back) and, when `unload`, unload; the two results for
   the exit log (*stop_rc / *unload_rc = 0x7FFFFFFF when the step did not run). A module whose counter thread did not
   leave it is stopped but stays loaded (the kernel reboot of sceKernelExitGame removes it). */
void ps2rc_exc_uninstall2(int unload, int *stop_rc, int *unload_rc)
{
    if (stop_rc) *stop_rc = 0x7FFFFFFF;
    if (unload_rc) *unload_rc = 0x7FFFFFFF;
    if (exc_mod < 0) return;
    int st = 0;
    int r = sceKernelStopModule(exc_mod, 0, NULL, &st, NULL);
    if (stop_rc) *stop_rc = r;
    if (unload) {
        r = sceKernelUnloadModule(exc_mod);        /* success = the module's uid (PSP-3000 6.61 and PPSSPP) */
        if (unload_rc) *unload_rc = r;
    }
    exc_mod_last = exc_mod;
    exc_mod = -1;
}

/* exit mode through the module's hookfn (on = 2), instead of
   ps2rc_exc_uninstall2 - see ps2rc_exc.h "exit mode". Only for a version >= 2 module that took the ME block. */
int ps2rc_exc_exitmode(uint32_t seq)
{
    volatile Ps2rcMeBlock *b = &ps2rc_me_block;
    if (exc_mod < 0) return 0x7FFFFFFF;
    if (b->magic != PS2RC_ME_MAGIC || !b->hookfn || b->version < 2u) return -2;
    b->exit_seq = seq;
    b->exitmode_rc = 0;
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    a.arg[0] = (uint32_t)(uintptr_t)b;
    a.arg[1] = 2;
    int r = ps2rc_kcall((void *)(uintptr_t)b->hookfn, &a);
    if (r < 0) return r;
    if ((int)a.ret[0] < 0) return (int)a.ret[0];
    return (int)b->exitmode_rc;
}

/* KERNEL (kuKernelCall): the 16 marker words, read only */
static int k_mark_read(uint32_t dst, uint32_t z)
{
    (void)z;
    volatile uint32_t *m = (volatile uint32_t *)PS2RC_AZ_MARK_ADDR;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < PS2RC_AZ_MARK_WORDS; i++) d[i] = m[i];
    return 0;
}

int ps2rc_exc_marker_read(uint32_t *w16)
{
    int out = 0;
    memset(w16, 0, PS2RC_AZ_MARK_WORDS * 4);
    if (sceIoDevctl("kemulator:", 0x00000003, NULL, 0, &out, sizeof out) >= 0) return 0;   /* PPSSPP */
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    a.arg[0] = (uint32_t)(uintptr_t)w16;
    if (ps2rc_kcall((void *)k_mark_read, &a) < 0) return 0;
    return 1;
}

int ps2rc_exc_install(const char *prx_path, void (*report)(uint32_t *regs), char *detail, unsigned detail_size)
{
    return ps2rc_exc_install2(prx_path, report, NULL, detail, detail_size);
}

int ps2rc_exc_install2(const char *prx_path, void (*report)(uint32_t *regs), Ps2rcPerfBlock *perf, char *detail,
                       unsigned detail_size)
{
    memset(ps2rc_crash_regs, 0, sizeof ps2rc_crash_regs);
    memset(&ps2rc_me_block, 0, sizeof ps2rc_me_block);
    if (perf) memset(perf, 0, sizeof *perf);       /* period_us 0 = the hardware-verified 50 ms, quit 0 */
    ps2rc_crash_regs[PS2RC_EXC_REGRESULT] = 0xFFFFFFFFu;
    SceUID mod = sceKernelLoadModule(prx_path, 0, NULL);
    if (mod < 0) {
        snprintf(detail, detail_size, "load %s failed %08x", prx_path, (unsigned)mod);
        return mod;
    }
    exc_mod = mod;
    uint32_t gp;
    __asm__ volatile("move %0, $28" : "=r"(gp));
    Ps2rcExcArgs a = {PS2RC_EXC_MAGIC, (uint32_t)(uintptr_t)ps2rc_crash_regs, (uint32_t)(uintptr_t)report, gp,
                      (uint32_t)(uintptr_t)perf, (uint32_t)(uintptr_t)&ps2rc_me_block};
    int status = -1;
    int r = sceKernelStartModule(mod, sizeof a, &a, &status, NULL);
    int reg = (int)ps2rc_crash_regs[PS2RC_EXC_REGRESULT];
    snprintf(detail, detail_size, "module %08x start %08x status %d register %08x via %s; stubs %08x %08x",
             (unsigned)mod, (unsigned)r, status, (unsigned)reg, ps2rc_crash_regs[46] == 1 ? "sctrlHENFindFunction"
                                    : ps2rc_crash_regs[46] == 2 ? "ExceptionManagerForKernel import"
                                    : ps2rc_crash_regs[46] == 3 ? "none: both imports unlinked" : "-",
             (unsigned)ps2rc_crash_regs[44], (unsigned)ps2rc_crash_regs[45]);
    if (r < 0) return r;
    return reg;
}

const char *ps2rc_exc_name(uint32_t cause)
{
    static const char *const names[32] = {
        "Int (interrupt)", "Mod (TLB modified)", "TLBL", "TLBS", "AdEL (address error, load or fetch)",
        "AdES (address error, store)", "IBE (bus error, fetch)", "DBE (bus error, data)", "Sys (syscall)",
        "Bp (break)", "RI (reserved instruction)", "CpU (coprocessor unusable)", "Ov (integer overflow)",
        "Tr (trap: teq/tne... e.g. integer division by zero)", "14", "FPE (floating point)", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31"};
    return names[(cause >> 2) & 31];
}

unsigned ps2rc_exc_format(const uint32_t *r, char *out, unsigned size)
{
    static const char *const rn[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
                                       "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9",
                                       "k0", "k1", "gp", "sp", "fp", "ra"};
    int n = snprintf(out, size, "CRASH: exception %u = %s\nCRASH: EPC=%08x BadVAddr=%08x Cause=%08x Status=%08x "
                     "ra=%08x sp=%08x%s\n",
                     (unsigned)((r[PS2RC_EXC_CAUSE] >> 2) & 31), ps2rc_exc_name(r[PS2RC_EXC_CAUSE]),
                     (unsigned)r[PS2RC_EXC_EPC], (unsigned)r[PS2RC_EXC_BADVADDR], (unsigned)r[PS2RC_EXC_CAUSE],
                     (unsigned)r[PS2RC_EXC_STATUS], (unsigned)r[31], (unsigned)r[29],
                     (r[PS2RC_EXC_CAUSE] & 0x80000000u) ? " (in a branch delay slot: the branch is at EPC)" : "");
    for (int row = 0; row < 4 && n > 0 && (unsigned)n < size; row++) {
        n += snprintf(out + n, size - n, "CRASH:");
        for (int i = row * 8; i < row * 8 + 8 && (unsigned)n < size; i++)
            n += snprintf(out + n, size - n, " %s=%08x", rn[i], (unsigned)r[i]);
        if ((unsigned)n < size) n += snprintf(out + n, size - n, "\n");
    }
    if (n < 0) return 0;
    return (unsigned)n < size ? (unsigned)n : size - 1;
}
