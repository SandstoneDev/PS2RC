/* hle_syscall.c - EE kernel system calls, HLE.
 * Numbers per ps2sdk ee/kernel/include/syscallnr.h; negative numbers are the interrupt-context ("i") variants.
 * Semantics follow the EE kernel as reimplemented by Play! (Source/ee/PS2OS.cpp) where the details matter
 * (SetupThread stack placement, thread start registers, handler chaining). */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: PZ_SCOPE zones */

static uint32_t main_stack_base, main_stack_size, heap_end;
static uint32_t gs_imr = 0x7F00;
static uint32_t sif_reg[32];
static uint32_t custom_syscall[256];
static uint32_t vsync_flag1, vsync_flag2;

static const char *names[256] = {
    [2] = "SetGsCrt", [7] = "_ExecPS2", [13] = "SetVTLBRefillHandler", [14] = "SetVCommonHandler",
    [16] = "AddIntcHandler", [17] = "RemoveIntcHandler", [18] = "AddDmacHandler", [19] = "RemoveDmacHandler",
    [20] = "_EnableIntc", [21] = "_DisableIntc", [22] = "_EnableDmac", [23] = "_DisableDmac",
    [24] = "_SetAlarm", [25] = "_ReleaseAlarm", [26] = "_iEnableIntc", [27] = "_iDisableIntc",
    [28] = "_iEnableDmac", [29] = "_iDisableDmac", [32] = "CreateThread", [33] = "DeleteThread",
    [34] = "StartThread", [35] = "ExitThread", [36] = "ExitDeleteThread", [37] = "TerminateThread",
    [38] = "iTerminateThread", [41] = "ChangeThreadPriority", [42] = "iChangeThreadPriority",
    [43] = "RotateThreadReadyQueue", [44] = "_iRotateThreadReadyQueue", [45] = "ReleaseWaitThread",
    [46] = "iReleaseWaitThread", [47] = "GetThreadId", [48] = "ReferThreadStatus", [49] = "iReferThreadStatus",
    [50] = "SleepThread", [51] = "WakeupThread", [52] = "_iWakeupThread", [53] = "CancelWakeupThread",
    [54] = "iCancelWakeupThread", [55] = "SuspendThread", [56] = "_iSuspendThread", [57] = "ResumeThread",
    [58] = "iResumeThread", [60] = "SetupThread", [61] = "SetupHeap", [62] = "EndOfHeap", [64] = "CreateSema",
    [65] = "DeleteSema", [66] = "SignalSema", [67] = "iSignalSema", [68] = "WaitSema", [69] = "PollSema",
    [70] = "iPollSema", [71] = "ReferSemaStatus", [72] = "iReferSemaStatus", [73] = "iDeleteSema",
    [74] = "SetOsdConfigParam", [75] = "GetOsdConfigParam", [86] = "_SetTLBEntry", [90] = "Copy",
    [91] = "GetEntryAddress", [100] = "FlushCache", [104] = "iFlushCache", [107] = "sceSifStopDma",
    [111] = "GetOsdConfigParam2", [112] = "GsGetIMR", [113] = "GsPutIMR", [115] = "SetVSyncFlag",
    [116] = "SetSyscall", [117] = "_print", [118] = "sceSifDmaStat", [119] = "sceSifSetDma",
    [120] = "sceSifSetDChain", [121] = "sceSifSetReg", [122] = "sceSifGetReg", [124] = "Deci2Call",
    [127] = "GetMemorySize", [128] = "_GetGsDxDyOffset", [130] = "_InitTLB", [131] = "FindAddress",
};

const char *hle_syscall_name(int n)
{
    int a = n < 0 ? -n : n;
    return a < 256 && names[a] ? names[a] : "?";
}

static int thread_status_bits(HleThread *t)
{
    int s = 0;
    switch (t->status) {
    case TS_RUN: s = t == hle_cur ? 1 : 2; break;
    case TS_READY: s = 2; break;
    case TS_WAIT: s = 4; break;
    case TS_DORMANT: s = 16; break;
    default: s = 0;
    }
    if (t->suspended) s |= 8;
    return s;
}

static int vsync_hook_installed;
void hle_vsync_flag_update(void)
{
    if (vsync_flag1) sw_(vsync_flag1, 1);
    if (vsync_flag2) { sw_(vsync_flag2, hle_gs_csr); sw_(vsync_flag2 + 4, 0); }
}

static void sys_entry(R5900Ctx *c)
{
    PZ_SCOPE(syscall);
    int32_t n = (int32_t)c->r[3].d[0];
    uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3);
    hle_stat_syscalls++;
    if (!hle_in_intr) hle_safe_point(c, 500);
    if (hle_verbose >= 1 && n != 100 && n != -104)
        LOG("[%d] syscall %d %s(%08x, %08x, %08x, %08x)\n", hle_cur ? hle_cur->id : 0, n, hle_syscall_name(n), a0,
            a1, a2, a3);
    int32_t r = 0;
    switch (n) {
    case 2:                                                  /* SetGsCrt(interlace, mode, field) */
        LOG("SetGsCrt(interlace %u, mode %u, field %u)\n", a0, a1, a2);
        break;
    case 7:
        hle_die(c, "_ExecPS2(%08x) - the game tried to reboot/execute another program", a0);
    case 13: case 14:
        r = 0;
        break;
    case 16:                                                 /* AddIntcHandler(cause, handler, next, arg) */
        r = hle_add_intc_handler((int)a0, a1, (int)a2, a3, (uint32_t)c->r[28].d[0]);
        LOG("AddIntcHandler(cause %u, %08x, next %d, arg %08x) = %d\n", a0, a1, (int)a2, a3, r);
        break;
    case 17:
        r = hle_remove_intc_handler((int)a0, (int)a1);
        break;
    case 18:
        r = hle_add_dmac_handler((int)a0, a1, (int)a2, a3, (uint32_t)c->r[28].d[0]);
        LOG("AddDmacHandler(ch %u, %08x, next %d, arg %08x) = %d\n", a0, a1, (int)a2, a3, r);
        break;
    case 19:
        r = hle_remove_dmac_handler((int)a0, (int)a1);
        break;
    case 20: case -26: {                                     /* _EnableIntc / _iEnableIntc */
        uint32_t b = 1u << (a0 & 31);
        r = !(hle_intc_mask & b);
        hle_intc_mask |= b;
        break;
    }
    case 21: case -27: {
        uint32_t b = 1u << (a0 & 31);
        r = !!(hle_intc_mask & b);
        hle_intc_mask &= ~b;
        break;
    }
    case 22: case -28: {                                     /* _EnableDmac: D_STAT.CIM bit */
        uint32_t b = 1u << (16 + (a0 & 15));
        r = !(hle_dmac_stat & b);
        hle_dmac_stat |= b;
        break;
    }
    case 23: case -29: {
        uint32_t b = 1u << (16 + (a0 & 15));
        r = !!(hle_dmac_stat & b);
        hle_dmac_stat &= ~b;
        break;
    }
    case 32: {                                               /* CreateThread(ee_thread_t *) */
        uint32_t p = a0;
        r = hle_create_thread(gw(p + 4), gw(p + 8), gw(p + 12), gw(p + 16), (int)gw(p + 20), gw(p + 28),
                              gw(p + 32));
        LOG("CreateThread(entry %08x, stack %08x+%x, gp %08x, prio %d) = %d\n", gw(p + 4), gw(p + 8), gw(p + 12),
            gw(p + 16), (int)gw(p + 20), r);
        break;
    }
    case 33: {
        HleThread *t = hle_thread((int)a0);
        if (!t || t == hle_cur || t->status != TS_DORMANT) { r = -1; break; }
        t->status = TS_FREE;
        r = (int)a0;
        break;
    }
    case 34:                                                 /* StartThread(tid, arg) */
        r = hle_start_thread((int)a0, a1);
        LOG("StartThread(%d, arg %08x) = %d\n", (int)a0, a1, r);
        hle_reschedule();
        break;
    case 35:
        hle_exit_thread();
    case 36:
        hle_cur->status = TS_FREE;
        hle_exit_thread();
    case 37: case -38: {                                     /* TerminateThread(tid) */
        HleThread *t = hle_thread((int)a0);
        if (!t || t == hle_cur) { r = -1; break; }
        LOG("TerminateThread(%d) - only allowed on waiting/ready threads\n", (int)a0);
        r = -1;
        break;
    }
    case 41: case -42:                                       /* ChangeThreadPriority(tid, prio) */
        r = hle_change_priority((int)a0, (int)a1);
        if (n > 0) hle_reschedule();
        break;
    case 43: case -44:
        r = hle_rotate_ready_queue((int)a0);
        break;
    case 45: case -46: {                                     /* ReleaseWaitThread */
        HleThread *t = hle_thread((int)a0);
        if (!t || t->status != TS_WAIT) { r = -1; break; }
        if (t->wait_kind == WK_SEMA) {                       /* unlink from the semaphore queue */
            HleSema *s = &hle_semas[t->wait_id];
            HleThread **pp = &s->waiters;
            while (*pp && *pp != t) pp = &(*pp)->next;
            if (*pp) { *pp = t->next; s->nwait--; }
            t->sema_result = -1;
        }
        hle_make_ready(t);
        r = (int)a0;
        if (n > 0) hle_reschedule();
        break;
    }
    case 47: case -47:
        r = hle_cur ? hle_cur->id : 1;
        break;
    case 48: case -49: {                                     /* ReferThreadStatus(tid, ee_thread_status_t *) */
        HleThread *t = a0 == 0 ? hle_cur : hle_thread((int)a0);
        if (!t) { r = -1; break; }
        r = thread_status_bits(t);
        if (a1) {
            uint32_t p = a1;
            sw_(p + 0, (uint32_t)r);
            sw_(p + 4, t->entry);
            sw_(p + 8, t->stack);
            sw_(p + 12, t->stack_size);
            sw_(p + 16, t->gp);
            sw_(p + 20, (uint32_t)t->init_prio);
            sw_(p + 24, (uint32_t)t->prio);
            sw_(p + 28, t->attr);
            sw_(p + 32, t->option);
            sw_(p + 36, t->status == TS_WAIT ? (t->wait_kind == WK_SEMA ? 2 : 1) : 0);
            sw_(p + 40, t->status == TS_WAIT ? (uint32_t)t->wait_id : 0);
            sw_(p + 44, (uint32_t)t->wakeup_count);
        }
        break;
    }
    case 50:                                                 /* SleepThread */
        if (hle_cur->wakeup_count > 0) { hle_cur->wakeup_count--; r = hle_cur->id; break; }
        hle_block(WK_SLEEP, 0);
        r = hle_cur->id;
        break;
    case 51: case -52: {                                     /* WakeupThread / iWakeupThread */
        HleThread *t = hle_thread((int)a0);
        if (!t) { r = -1; break; }
        if (t->status == TS_WAIT && t->wait_kind == WK_SLEEP) hle_make_ready(t);
        else t->wakeup_count++;
        r = (int)a0;
        if (n > 0) hle_reschedule();
        break;
    }
    case 53: case -54: {
        HleThread *t = a0 ? hle_thread((int)a0) : hle_cur;
        if (!t) { r = -1; break; }
        r = t->wakeup_count;
        t->wakeup_count = 0;
        break;
    }
    case 55: case -56: {                                     /* SuspendThread */
        HleThread *t = hle_thread((int)a0);
        if (!t || t == hle_cur) { r = -1; break; }
        t->suspended++;
        r = (int)a0;
        break;
    }
    case 57: case -58: {
        HleThread *t = hle_thread((int)a0);
        if (!t || !t->suspended) { r = -1; break; }
        t->suspended--;
        r = (int)a0;
        if (n > 0) hle_reschedule();
        break;
    }
    case 60: {                                               /* SetupThread(gp, stack, size, args, root) */
        uint32_t top = a1 == 0xFFFFFFFFu ? hle_ram_end - 0x1000 : a1 + a2;   /* top of the RAM present */
        main_stack_size = a2;
        main_stack_base = top - a2;
        HleThread *t = hle_cur;
        t->stack = main_stack_base;
        t->stack_size = main_stack_size;
        t->gp = a0;
        if (a3) {                                            /* argc / argv for main() */
            const char *arg0 = HLE_PACK_BOOT_ARG0;
            sw_(a3, 1);
            sw_(a3 + 4, a3 + 12);
            sw_(a3 + 8, 0);
            gcopy_out(a3 + 12, arg0, (uint32_t)strlen(arg0) + 1);
        }
        r = (int32_t)(top - 0x2A0);
        LOG("SetupThread(gp %08x, stack %08x, size %x, args %08x, root %08x) -> sp %08x\n", a0, a1, a2, a3,
            ARG(4), (uint32_t)r);
        hle_milestone("crt0: SetupThread");
        break;
    }
    case 61:                                                 /* SetupHeap(start, size) */
        heap_end = a1 == 0xFFFFFFFFu ? main_stack_base : a0 + a1;
        r = (int32_t)heap_end;
        LOG("SetupHeap(%08x, %08x) -> heap end %08x\n", a0, a1, heap_end);
        break;
    case 62:
        r = (int32_t)heap_end;
        break;
    case 64: {                                               /* CreateSema(ee_sema_t *) */
        uint32_t p = a0;
        r = hle_sema_create((int)gw(p + 8), (int)gw(p + 4), (int)gw(p + 16), gw(p + 20));
        VLOG("CreateSema(init %d, max %d) = %d\n", (int)gw(p + 8), (int)gw(p + 4), r);
        break;
    }
    case 65: case -73:
        r = hle_sema_delete((int)a0);
        if (n > 0) hle_reschedule();
        break;
    case 66:
        r = hle_sema_signal((int)a0);
        hle_reschedule();
        break;
    case -67:
        r = hle_sema_signal((int)a0);
        break;
    case 68:
        r = hle_sema_wait((int)a0);
        break;
    case 69: case -70:
        r = hle_sema_poll((int)a0);
        break;
    case 71: case -72: {                                     /* ReferSemaStatus(sid, ee_sema_t *) */
        if (a0 == 0 || a0 >= HLE_MAX_SEMAS || !hle_semas[a0].used) { r = -1; break; }
        HleSema *s = &hle_semas[a0];
        sw_(a1 + 0, (uint32_t)s->count);
        sw_(a1 + 4, (uint32_t)s->max_count);
        sw_(a1 + 8, (uint32_t)s->init_count);
        sw_(a1 + 12, (uint32_t)s->nwait);
        sw_(a1 + 16, (uint32_t)s->attr);
        sw_(a1 + 20, s->option);
        r = (int)a0;
        break;
    }
    case 74:
        r = 0;
        break;
    case 75:                                                 /* GetOsdConfigParam: English, 4:3, RGB, version 1 */
        sw_(a0, (1u << 16) | (1u << 13));
        r = 0;
        break;
    case 86: case 130:
        r = 0;
        break;
    case 90:                                                 /* Copy(dst, src, n) (kernel memcpy) */
        if (a0 < hle_ram_end && a1 < hle_ram_end && a2 <= hle_ram_end - a0 && a2 <= hle_ram_end - a1)
            memmove(hle_ram + a0, hle_ram + a1, a2);
        else LOG("Copy(%08x, %08x, %x) outside RAM ignored\n", a0, a1, a2);
        r = 0;
        break;
    case 91: case 131:
        r = 0;
        break;
    case 100: case -104:
        r = 0;
        break;
    case 107:
        r = 0;
        break;
    case 111:
        if (a0 && a1 < 0x100) memset(gptr(a0, a1), 0, a1);
        r = 0;
        break;
    case 112: case -112:
        r = (int32_t)gs_imr;
        break;
    case 113: case -113:
        r = (int32_t)gs_imr;
        gs_imr = a0;
        break;
    case 115:
        vsync_flag1 = a0;
        vsync_flag2 = a1;
        vsync_hook_installed = 1;
        r = 0;
        break;
    case 116:
        LOG("SetSyscall(%u, %08x)\n", a0, a1);
        custom_syscall[a0 & 255] = a1;
        r = 0;
        break;
    case 117:
        LOG("kernel print: %s", gstr(a0));
        break;
    case 118: case -118:
        r = -1;                                              /* sceSifDmaStat: transfer already complete */
        break;
    case 119: case -119: {                                   /* sceSifSetDma(SifDmaTransfer_t *, n) */
        static int dma_id;
        for (uint32_t i = 0; i < a1; i++)
            VLOG("  SIF DMA %u: src %08x -> IOP %08x size %x attr %x\n", i, gw(a0 + 16 * i), gw(a0 + 16 * i + 4),
                 gw(a0 + 16 * i + 8), gw(a0 + 16 * i + 12));
        r = ++dma_id;
        break;
    }
    case 120: case -120:
        r = 0;
        break;
    case 121:
        sif_reg[a0 & 31] = a1;
        r = 0;
        break;
    case 122:
        r = (int32_t)sif_reg[a0 & 31];
        if ((a0 & 31) == 4) r |= 0x70000;                    /* SMFLG: IOP-side SIF/RPC initialised */
        break;
    case 124:
        r = -1;
        break;
    case 127:
        r = (int32_t)hle_ram_end;
        break;
    case 128:
        r = 0;
        break;
    default: {
        uint32_t idx = (uint32_t)(n < 0 ? -n : n) & 255;
        if (custom_syscall[idx]) {
            /* the EE kernel saves the caller's registers around a SetSyscall handler and returns its v0/v1. The
               context copies matter: an exact (non-ABI) caller keeps its registers there, and an ABI-mode handler
               of back-end v5 does not restore the context copies of s0-s7/fp/ra */
            LOG("custom syscall %d -> guest handler %08x\n", n, custom_syscall[idx]);
            R5900Reg128 gpr[32];
            memcpy(gpr, c->r, sizeof gpr);
            rc_dispatch(c, custom_syscall[idx]);
            gpr[2] = c->r[2];
            gpr[3] = c->r[3];
            memcpy(c->r, gpr, sizeof gpr);
            return;
        }
        hle_die(c, "unimplemented syscall %d (%s) a0=%08x a1=%08x", n, hle_syscall_name(n), a0, a1);
    }
    }
    c->r[2].d[0] = (uint64_t)(int64_t)r;
}

void hle_syscalls_install(void)
{
    for (int i = 0; i < RT_NSYSCALLS; i++) rt_syscall_table[i] = sys_entry;
}
