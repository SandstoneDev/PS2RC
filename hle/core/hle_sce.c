/* hle_sce.c - HLE of Sony EE library functions linked into the game (bound by name in the pack's hooks.def, the
 * addresses from a signature scan). Each HLE_SCE(name) replaces the whole library function: the kernel/SIF/IOP
 * plumbing behind it (kernel patches, SIF DMA, IOP RPC servers, DECI2) does not exist on the host.
 *
 * Choice per subsystem: libkernl thread/timer wrappers that depend on kernel
 * internals -> HLE; SIF RPC -> HLE at sceSifBindRpc/sceSifCallRpc with per-server handlers (hle_iop.c); fileio,
 * libcdvd, libpad, libmc -> HLE at the API; libgraph -> only the functions that poll GS/VU state or touch the kernel;
 * libdma -> NOT hooked (works through the DMAC register model, hle_mmio.c); libc/libm/printf -> guest code. */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: PZ_SCOPE zones */
#include "hle_fast.h"                          /* x_pal50 */

#define TRACE_HOOK(name) hle_trace_call(name, 0, c)

/* run a guest function in interrupt context (RPC end functions, CD callbacks) */
uint32_t hle_call_intr(uint32_t fn, uint32_t a0, uint32_t gp)
{
    if (!fn) return 0;
    int was = hle_in_intr;
    R5900Ctx *ic = hle_intr_ctx;
    R5900Ctx save = *ic;                                      /* nested use (callback from a handler) */
    ic->ram = hle_ram;
    ic->ram_mask = HLE_RAM_SIZE - 1;
    ic->r[4].d[0] = (uint64_t)(int64_t)(int32_t)a0;
    ic->r[28].d[0] = (uint64_t)(int64_t)(int32_t)gp;
    /* sp: only the low word of the slot is defined (back-end v5 writes sp/gp as one word) */
    ic->r[29].d[0] = was ? (uint64_t)(int64_t)(int32_t)((uint32_t)save.r[29].d[0] - 0x100u) : HLE_INTR_STACK_TOP - 0x40;
    ic->r[31].d[0] = 0;
    hle_in_intr = 1;
    rc_dispatch(ic, fn);
    uint32_t r = (uint32_t)ic->r[2].d[0];
    hle_in_intr = was;
    if (was) *ic = save;
    return r;
}

/* ================================================================== libkernl */
HLE_SCE(_InitSys)                                                 /* _InitSys: kernel patches, alarms, timers, TLB */
{
    TRACE_HOOK("_InitSys");
    /* what remains observable of the original: InitThread lowers main to priority 1; tail clears a 512-byte table
       and two words (the pack's [symbols] initsys_table / initsys_words) */
    hle_change_priority(0, 1);
#ifdef HLE_HAVE_SYM_initsys_table                             /* the pack's [symbols] (libkernl data) */
    memset(gptr(HLE_SYM_initsys_table, 512), 0, 512);
#endif
#ifdef HLE_HAVE_SYM_initsys_words
    sw_(HLE_SYM_initsys_words, 0);
    sw_(HLE_SYM_initsys_words + 4, 0);
#endif
    hle_milestone("crt0: _InitSys (HLE)");
    RET(0);
}

HLE_SCE(InitThread) { TRACE_HOOK("InitThread"); hle_change_priority(0, 1); RET(0); }

HLE_SCE(DelayThread)                                                 /* DelayThread(usec) */
{
    uint32_t usec = ARG(0);
    hle_trace_call("DelayThread", HLE_ADDR(DelayThread), c);
    hle_safe_point(c, 200);
    if (hle_in_intr) { RET(-1); return; }
    HleThread *t = hle_cur;
    hle_alarm_add(hle_vtime + (uint64_t)usec * (EE_HZ / 1000000), 0, 0, 0, ALARM_THREAD_DELAY, t->id);
    hle_block(WK_DELAY, 0);
    RET(0);
}

HLE_SCE(StartThread)                                                 /* StartThread(tid, arg) (libkernl 3.0 wrapper) */
{
    hle_trace_call("StartThread", HLE_ADDR(StartThread), c);
    int r = hle_start_thread(SARG(0), ARG(1));
    LOG("StartThread(%d, %08x) = %d\n", SARG(0), ARG(1), r);
    RET(r);
    if (!hle_in_intr) hle_reschedule();
}

static void wake(int id)
{
    HleThread *t = hle_thread(id);
    if (!t) return;
    if (t->status == TS_WAIT && t->wait_kind == WK_SLEEP) hle_make_ready(t);
    else t->wakeup_count++;
}
HLE_SCE(iWakeupThread) { hle_trace_call("iWakeupThread", HLE_ADDR(iWakeupThread), c); wake(SARG(0)); RET(SARG(0)); }
HLE_SCE(iRotateThreadReadyQueue) { hle_trace_call("iRotateThreadReadyQueue", HLE_ADDR(iRotateThreadReadyQueue), c); RET(hle_rotate_ready_queue(SARG(0))); }
HLE_SCE(iSuspendThread)
{
    hle_trace_call("iSuspendThread", HLE_ADDR(iSuspendThread), c);
    HleThread *t = hle_thread(SARG(0));
    if (t && t != hle_cur) t->suspended++;
    RET(t ? SARG(0) : -1);
}

HLE_SCE(GetTimerSystemTime) { RET64(hle_vtime / 2); hle_time_charge(32); }   /* GetTimerSystemTime (BUSCLK) */
HLE_SCE(iGetTimerSystemTime) { RET64(hle_vtime / 2); }                          /* iGetTimerSystemTime */

static void set_timer_alarm(R5900Ctx *c, const char *name)
{
    uint64_t clk = ARG64(0);
    uint32_t fn = ARG(1), arg = ARG(2);
    int id = hle_alarm_add(hle_vtime + 2 * clk, fn, arg, (uint32_t)c->r[28].d[0], ALARM_TIMER, 0);
    VLOG("%s(%llu clocks, %08x, %08x) = %d\n", name, (unsigned long long)clk, fn, arg, id);
    RET(id);
}
HLE_SCE(SetTimerAlarm) { hle_trace_call("SetTimerAlarm", HLE_ADDR(SetTimerAlarm), c); set_timer_alarm(c, "SetTimerAlarm"); }
HLE_SCE(iSetTimerAlarm) { hle_trace_call("iSetTimerAlarm", HLE_ADDR(iSetTimerAlarm), c); set_timer_alarm(c, "iSetTimerAlarm"); }
HLE_SCE(ReleaseTimerAlarm) { RET(hle_alarm_cancel(SARG(0))); }
HLE_SCE(iReleaseTimerAlarm) { RET(hle_alarm_cancel(SARG(0))); }

static char tty_line[1024];
static int tty_len;
static void tty_put(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\n' || tty_len == (int)sizeof tty_line - 1) {
            tty_line[tty_len] = 0;
            LOG("TTY: %s\n", tty_line);
            tty_len = 0;
            if (ch == '\n') continue;
        }
        if (ch != '\r') tty_line[tty_len++] = ch;
    }
}

HLE_SCE(write)                                                 /* write(fd, buf, n): stdout/stderr -> log */
{
    uint32_t fd = ARG(0), buf = ARG(1), n = ARG(2);
    if (fd == 1 || fd == 2) {
        if (n > 0x100000) n = 0x100000;
        tty_put(gptr(buf, n), n);
        RET(n);
    } else {
        RET(-1);
    }
}
HLE_SCE(read) { RET(-1); }                                    /* read (tty) */

HLE_SCE(Exit)                                                 /* Exit(code) */
{
    LOG("guest Exit(%d)\n", SARG(0));
    hle_exit(0);
}

/* ================================================================== SIF / IOP */
static int nservers;
HLE_SCE(sceSifInitRpc) { hle_trace_call("sceSifInitRpc", HLE_ADDR(sceSifInitRpc), c); RET(0); }
HLE_SCE(sceSifInitCmd) { hle_trace_call("sceSifInitCmd", HLE_ADDR(sceSifInitCmd), c); RET(0); }
HLE_SCE(sceSifAddCmdHandler) { RET(0); }                                     /* sceSifAddCmdHandler */
HLE_SCE(sceSifSendCmd) { VLOG("sceSifSendCmd(%08x, ...)\n", ARG(0)); RET(1); }
HLE_SCE(isceSifSendCmd) { RET(1); }                                     /* isceSifSendCmd */

HLE_SCE(sceSifBindRpc)                                                 /* sceSifBindRpc(cd, rpc_id, mode) */
{
    uint32_t cd = ARG(0), id = ARG(1);
    uint32_t srv = 0;
    for (int i = 0; i < nservers; i++) if (gw(HLE_FAKE_SERVER + 64 * i) == id) srv = HLE_FAKE_SERVER + 64 * i;
    if (!srv) {
        srv = HLE_FAKE_SERVER + 64 * nservers++;
        memset(gptr(srv, 64), 0, 64);
        sw_(srv, id);
    }
    sw_(cd + 4, id);
    sw_(cd + 0x14, srv + 16);                                 /* buff */
    sw_(cd + 0x18, srv + 32);                                 /* cbuff */
    sw_(cd + 0x24, srv);                                      /* server: non-zero = bound */
    LOG("sceSifBindRpc(cd %08x, rpc %08x) -> server %08x\n", cd, id, srv);
    hle_trace_call("sceSifBindRpc", HLE_ADDR(sceSifBindRpc), c);
    RET(0);
}

HLE_SCE(sceSifCallRpc)              /* sceSifCallRpc(cd, fno, mode, send, ssize, recv, rsize, endfunc, efarg) */
{
    uint32_t cd = ARG(0), fno = ARG(1), mode = ARG(2), send = ARG(3), ssize = ARG(4), recv = ARG(5), rsize = ARG(6);
    uint32_t endf = ARG(7), efarg = STACKARG(0);
    uint32_t srv = gw(cd + 0x24);
    uint32_t id = srv ? gw(srv) : 0;
    if (!hle_rpc_registered(id)) hle_trace_call("sceSifCallRpc", HLE_ADDR(sceSifCallRpc), c);  /* not the pack's servers */
    hle_safe_point(c, 2000);
    int r = hle_rpc_call(c, id, fno, mode, send, ssize, recv, rsize);
    if (r != 0) hle_die(c, "unhandled SIF RPC: server %08x fno %u (send %08x/%u recv %08x/%u)", id, fno, send, ssize,
                        recv, rsize);
    if (endf) hle_call_intr(endf, efarg, (uint32_t)c->r[28].d[0]);
    RET(0);
}
HLE_SCE(sceSifCheckStatRpc) { RET(0); }                                     /* sceSifCheckStatRpc: never busy */
HLE_SCE(sceSifRebootIop) { LOG("sceSifRebootIop(%s)\n", gstr(ARG(0))); hle_milestone("IOP reboot (HLE)"); RET(1); }
HLE_SCE(sceSifSyncIop) { RET(1); }                                     /* sceSifSyncIop */
HLE_SCE(sceSifResetIop) { RET(1); }                                     /* sceSifResetIop */
HLE_SCE(sceSifIsAliveIop) { RET(1); }                                     /* sceSifIsAliveIop */
HLE_SCE(sceSifLoadFileReset) { RET(0); }                                     /* sceSifLoadFileReset */
static int next_module_id = 1;
HLE_SCE(_sceSifLoadModule)                                    /* _sceSifLoadModule(path, argc, argv, int *modres, mode) */
{
    LOG("IOP module load: %s (HLE: services provided by hle_iop.c)\n", gstr(ARG(0)));
    if (ARG(3)) sw_(ARG(3), 0);
    RET(next_module_id++);
}
HLE_SCE(_sceSifLoadModuleBuffer)                                    /* _sceSifLoadModuleBuffer(ptr, argc, argv, modres*, ...) */
{
    LOG("IOP module load from buffer %08x (HLE)\n", ARG(0));
    if (ARG(3)) sw_(ARG(3), 0);
    RET(next_module_id++);
}
static uint32_t iop_heap = 0x00100000;
HLE_SCE(sceSifInitIopHeap) { RET(0); }                                     /* sceSifInitIopHeap */
HLE_SCE(sceSifAllocIopHeap)                                                 /* sceSifAllocIopHeap(size): fake IOP address */
{
    uint32_t a = iop_heap;
    iop_heap += (ARG(0) + 255) & ~255u;
    VLOG("sceSifAllocIopHeap(%x) = %08x\n", ARG(0), a);
    RET(a);
}
HLE_SCE(sceSifLoadIopHeap) { LOG("sceSifLoadIopHeap(%s, %08x)\n", gstr(ARG(0)), ARG(1)); RET(0); }

/* ================================================================== fileio (sceOpen ...) */
#define MAX_FD 32
static struct { int used, disc; uint32_t size; uint64_t pos; char name[96]; } fds[MAX_FD];

HLE_SCE(sceFsReset) { RET(0); }                                     /* sceFsReset */
HLE_SCE(sceFsInit) { RET(0); }                                     /* sceFsInit */

HLE_SCE(sceOpen)                                                 /* sceOpen(name, flags) */
{
    const char *name = gstr(ARG(0));
    uint32_t flags = ARG(1), size = 0;
    int idx = -1;
    if ((flags & 3) == 1 || !(flags & 2)) idx = hle_disc_open(name, &size);
    int fd = -1;
    if (idx >= 0)
        for (int i = 3; i < MAX_FD; i++)
            if (!fds[i].used) {
                fds[i].used = 1;
                fds[i].disc = idx;
                fds[i].size = size;
                fds[i].pos = 0;
                snprintf(fds[i].name, sizeof fds[i].name, "%s", name);
                fd = i;
                break;
            }
    LOG("sceOpen(\"%s\", %x) = %d%s\n", name, flags, fd, idx < 0 ? " (not found)" : "");
    hle_trace_call("sceOpen", HLE_ADDR(sceOpen), c);
    RET(fd >= 0 ? fd : -2);                                   /* -ENOENT */
}

static int fd_ok(int fd) { return fd >= 0 && fd < MAX_FD && fds[fd].used; }

HLE_SCE(sceClose) { int fd = SARG(0), ok = fd_ok(fd); if (ok) fds[fd].used = 0; RET(ok ? 0 : -9); }   /* sceClose */
HLE_SCE(sceLseek)                                                 /* sceLseek(fd, offset, whence) */
{
    int fd = SARG(0);
    if (!fd_ok(fd)) { RET(-9); return; }
    int64_t off = SARG(1);
    int wh = SARG(2);
    int64_t base = wh == 0 ? 0 : wh == 1 ? (int64_t)fds[fd].pos : (int64_t)fds[fd].size;
    int64_t np = base + off;
    if (np < 0) np = 0;
    fds[fd].pos = (uint64_t)np;
    RET((int32_t)np);
}
HLE_SCE(sceLseek64)                                                 /* sceLseek64(fd, int64 off, whence) */
{
    int fd = SARG(0);
    if (!fd_ok(fd)) { RET64((uint64_t)-9); return; }
    int64_t off = (int64_t)ARG64(1);
    int wh = SARG(2);
    int64_t base = wh == 0 ? 0 : wh == 1 ? (int64_t)fds[fd].pos : (int64_t)fds[fd].size;
    fds[fd].pos = (uint64_t)(base + off);
    RET64(fds[fd].pos);
}
HLE_SCE(sceRead)                                                 /* sceRead(fd, buf, n) */
{
    int fd = SARG(0);
    uint32_t buf = ARG(1), n = ARG(2);
    if (!fd_ok(fd)) { RET(-9); return; }
    hle_safe_point(c, 1000 + n / 8);
    int64_t r = hle_disc_read_fd(fds[fd].disc, fds[fd].pos, gptr(buf, n), n);
    if (r > 0) fds[fd].pos += (uint64_t)r;
    VLOG("sceRead(%d \"%s\", %08x, %u) = %lld\n", fd, fds[fd].name, buf, n, (long long)r);
    RET((int32_t)r);
}
HLE_SCE(sceWrite)                                                 /* sceWrite(fd, buf, n) */
{
    int fd = SARG(0);
    uint32_t n = ARG(2);
    if (fd == 1 || fd == 2) { tty_put(gptr(ARG(1), n), n); RET(n); return; }
    RET(-5);
}
HLE_SCE(sceIoctl) { VLOG("sceIoctl(%d, %x)\n", SARG(0), ARG(1)); RET(0); }
HLE_SCE(sceGetstat)                                                 /* sceGetstat(name, sceIoStat *) */
{
    uint32_t lsn, size;
    if (!hle_disc_lookup(gstr(ARG(0)), &lsn, &size)) { RET(-2); return; }
    memset(gptr(ARG(1), 64), 0, 64);
    sw_(ARG(1) + 0, 0x2124);                                  /* mode: regular file, readable */
    sw_(ARG(1) + 4, 0);
    sw_(ARG(1) + 8, size);
    RET(0);
}
HLE_SCE(sceDopen) { RET(-2); }                                    /* sceDopen */

/* ================================================================== libcdvd */
static uint32_t cd_callback, cd_poff_cb, cd_poff_arg;
HLE_SCE(sceCdInit) { hle_trace_call("sceCdInit", HLE_ADDR(sceCdInit), c); RET(1); }
HLE_SCE(sceCdDiskReady) { RET(2); }                                     /* sceCdDiskReady: SCECdComplete */
HLE_SCE(sceCdDiskReady_old) { RET(2); }
HLE_SCE(sceCdNcmdDiskReady) { RET(2); }
HLE_SCE(sceCdLayerSearchFile)                                                 /* sceCdLayerSearchFile(sceCdlFILE *, name, layer) */
{
    uint32_t fp = ARG(0), lsn, size;
    const char *name = gstr(ARG(1));
    if (!hle_disc_lookup(name, &lsn, &size)) {
        LOG("sceCdLayerSearchFile(\"%s\") not found\n", name);
        RET(0);
        return;
    }
    sw_(fp + 0, lsn);
    sw_(fp + 4, size);
    memset(gptr(fp + 8, 24), 0, 24);
    VLOG("sceCdLayerSearchFile(\"%s\") = lsn %u size %u\n", name, lsn, size);
    RET(1);
}
HLE_SCE(sceCdSync) { RET(0); }                                     /* sceCdSync */
HLE_SCE(sceCdSyncS) { RET(0); }                                     /* sceCdSyncS */
HLE_SCE(sceCdMmode) { RET(1); }                                     /* sceCdMmode */
HLE_SCE(sceCdStandby) { RET(1); }                                     /* sceCdStandby */
HLE_SCE(sceCdBreak) { RET(1); }                                     /* sceCdBreak */
HLE_SCE(sceCdReadDvdDualInfo)                                                 /* sceCdReadDvdDualInfo(int *on_dual, u32 *layer1) */
{
    if (ARG(0)) sw_(ARG(0), 0);
    if (ARG(1)) sw_(ARG(1), 0);
    RET(1);
}
HLE_SCE(sceCdReadClock)                                                 /* sceCdReadClock: fixed date 2005-10-26 12:00:00 */
{
    uint8_t clk[8] = {0, 0x00, 0x00, 0x12, 0, 0x26, 0x10, 0x05};
    gcopy_out(ARG(0), clk, 8);
    RET(1);
}
HLE_SCE(sceCdPowerOff) { RET(1); }                                     /* sceCdPowerOff */
HLE_SCE(sceCdCallback) { uint32_t old = cd_callback; cd_callback = ARG(0); RET(old); }
HLE_SCE(sceCdPOffCallback) { cd_poff_cb = ARG(0); cd_poff_arg = ARG(1); RET(0); }
HLE_SCE(_sceCdSetTimeout) { RET(0); }                                     /* _sceCdSetTimeout */

/* ================================================================== libpad: one DualShock 2 on port 0 (build 25: the
   player's controls through hle_plat_pad_read - PSP buttons/stick, host nothing pressed) */
static uint32_t pad_buttons = 0xFFFF;                         /* active low */
HLE_SCE(scePadInit) { hle_trace_call("scePadInit", HLE_ADDR(scePadInit), c); RET(1); }
HLE_SCE(scePadInit2) { RET(1); }                                     /* scePadInit2 */
HLE_SCE(scePadEnd) { RET(1); }                                     /* scePadEnd */
HLE_SCE(scePadPortOpen) { LOG("scePadPortOpen(%d, %d, %08x)\n", SARG(0), SARG(1), ARG(2)); RET(1); }
HLE_SCE(scePadPortClose) { RET(1); }                                     /* scePadPortClose */
HLE_SCE(scePadGetState) { RET(ARG(0) == 0 && ARG(1) == 0 ? 6 : 0); }    /* scePadGetState: 6 = stable / 0 = disconnected */
HLE_SCE(scePadRead)                                                 /* scePadRead(port, slot, rdata) */
{
    uint8_t d[32];
    memset(d, 0, sizeof d);
    if (ARG(0) != 0 || ARG(1) != 0) { RET(0); return; }
    uint8_t st[4] = {0x80, 0x80, 0x80, 0x80};
    pad_buttons = ~(uint32_t)hle_plat_pad_read(st) & 0xFFFFu;
    d[0] = 0;
    d[1] = 0x73;
    d[2] = (uint8_t)pad_buttons;
    d[3] = (uint8_t)(pad_buttons >> 8);
    d[4] = st[0];
    d[5] = st[1];
    d[6] = st[2];
    d[7] = st[3];
    gcopy_out(ARG(2), d, 32);
    RET(32);
}
HLE_SCE(scePadGetFrameCount) { RET((uint32_t)hle_frame); }                   /* scePadGetFrameCount */
HLE_SCE(scePadSetReqState) { RET(1); }                                     /* scePadSetReqState */
HLE_SCE(scePadGetReqState) { RET(0); }                                     /* scePadGetReqState: complete */
HLE_SCE(scePadInfoAct) { RET(0); }                                     /* scePadInfoAct: no actuators */
HLE_SCE(scePadInfoComb) { RET(0); }                                     /* scePadInfoComb */
HLE_SCE(scePadInfoMode)                                                 /* scePadInfoMode(port, slot, term, offs) */
{
    int term = SARG(2), offs = SARG(3);
    int r = 0;
    switch (term) {
    case 1: r = 7; break;                                     /* PAD_MODECURID: analog (DualShock) */
    case 2: r = 7; break;                                     /* PAD_MODECUREXID */
    case 3: r = 1; break;                                     /* PAD_MODECUROFFS */
    case 4: r = offs == -1 ? 2 : offs == 0 ? 4 : offs == 1 ? 7 : 0; break;   /* PAD_MODETABLE */
    }
    RET(r);
}
HLE_SCE(scePadSetMainMode) { RET(1); }                                     /* scePadSetMainMode */
HLE_SCE(scePadSetActDirect) { RET(1); }                                     /* scePadSetActDirect */
HLE_SCE(scePadSetActAlign) { RET(1); }                                     /* scePadSetActAlign */
HLE_SCE(scePadInfoPressMode) { RET(0); }                                     /* scePadInfoPressMode: no pressure mode */
HLE_SCE(scePadEnterPressMode) { RET(1); }
HLE_SCE(scePadExitPressMode) { RET(1); }
HLE_SCE(scePadGetButtonMask) { RET(-1); }                                    /* scePadGetButtonMask */
HLE_SCE(scePadSetButtonInfo) { RET(1); }                                     /* scePadSetButtonInfo */
HLE_SCE(scePadGetPortMax) { RET(2); }                                     /* scePadGetPortMax */
HLE_SCE(scePadGetSlotMax) { RET(1); }                                     /* scePadGetSlotMax */
HLE_SCE(scePadGetModVersion) { RET(0x0400); }                                /* scePadGetModVersion */
HLE_SCE(scePadSetWarningLevel) { RET(0); }                                     /* scePadSetWarningLevel */
HLE_SCE(scePadSetVrefParam) { RET(1); }                                     /* scePadSetVrefParam */
HLE_SCE(scePadSetDmaCallback) { RET(0); }                                     /* scePadSetDmaCallback */

/* ================================================================== libmc: hle_memcard.c (a memory card in slot 1
   backed by files - host --memcard DIR, PSP run option memcard=1; without a card the "no memory card" behaviour,
   result -10) */

/* ================================================================== libgraph (only what polls GS/VU state) */
HLE_SCE(sceGsResetGraph)                                                 /* sceGsResetGraph(mode, inter, omode, ffmode) */
{
    LOG("sceGsResetGraph(%d, inter %d, omode %d, ffmode %d)\n", SARG(0), SARG(1), SARG(2), SARG(3));
    hle_trace_call("sceGsResetGraph", HLE_ADDR(sceGsResetGraph), c);
    /* the omode values are SetGsCrt's - 2 = NTSC (59.94 Hz), 3 = PAL (50 Hz) (PCSX2 R5900OpcodeImpl.cpp SetGsCrt,
       gsKit GS_MODE_NTSC 2 / GS_MODE_PAL 3); a PAL game passes 3. The old mapping had the two swapped and ran a PAL
       game at 60 Hz; x_pal50=1 / HLE_X_PAL50=1 selects the right mapping (it moves the whole guest timeline). */
    if (hle_pack_vblank_pal50 ? hle_pack_vblank_pal50() : hle_x_opt("pal50", 1)) {   /* default on; a pack frame cap decides */
        if (ARG(2) == 3) hle_vblank_hz = 50;                  /* SCE_GS_PAL */
        else if (ARG(2) == 2) hle_vblank_hz = 60;             /* SCE_GS_NTSC */
        LOG("sceGsResetGraph: omode %d -> VBlank %d Hz\n", SARG(2), hle_vblank_hz);
    } else {
        if (ARG(2) == 2) hle_vblank_hz = 50;                  /* (the old, swapped mapping of builds <= 30) */
        else if (ARG(2) == 3) hle_vblank_hz = 60;
    }
    RET(0);
}
HLE_SCE(sceGsResetPath) { RET(0); }                                     /* sceGsResetPath */
HLE_SCE(sceGsSyncV)                                                 /* sceGsSyncV(mode): wait for the next VBlank */
{
    hle_safe_point(c, 100);
    hle_wait_vsync();
    RET((hle_gs_csr >> 13) & 1);
}
HLE_SCE(sceGsSyncPath) { RET(0); }                                     /* sceGsSyncPath(mode, timeout): paths idle */
HLE_SCE(sceGsExecStoreImage)                                                 /* sceGsExecStoreImage(img, dst): VRAM read-back */
{
    static unsigned n;
    if (n++ < 8) LOG("sceGsExecStoreImage(%08x -> %08x): no GS, destination left unchanged\n", ARG(0), ARG(1));
    RET(0);
}

/* ------------------------------------------------------------------ speed hooks (build 20, hooks.def "speed")
   _sceSDC / _sceIDC: the per-line D-cache writeback / invalidate loops behind SyncDCache / InvalidDCache - no-ops
   (registers untouched: both are void). memcpy / memset: host copies when every byte is plain guest RAM (or a KSEG /
   uncached mirror, or the scratchpad) and memcpy's ranges do not overlap; anything else runs the translated original.
   Virtual time is charged like the EE word loops (about one cycle per byte copied / 4). */
static uint8_t *plain_ram(uint32_t a, uint32_t n)
{
    if (a < hle_ram_end && n <= hle_ram_end - a) return hle_ram + a;
    uint32_t seg = a >> 28;
    if (seg == 2 || seg == 3 || seg == 8 || seg == 9 || seg == 0xA) {
        uint32_t p = a & 0x0FFFFFFFu;
        if (p < hle_ram_end && n <= hle_ram_end - p) return hle_ram + p;
    }
    if (a >= 0x70000000u && a - 0x70000000u < HLE_SPR_SIZE && n <= HLE_SPR_SIZE - (a - 0x70000000u))
        return hle_spr + (a - 0x70000000u);
    return NULL;
}

uint64_t hle_stat_fast_memcpy, hle_stat_fast_memset;
void GUEST_SCE(memcpy)(R5900Ctx *c);
void GUEST_SCE(memset)(R5900Ctx *c);

HLE_SCE(_sceSDC) { hle_time_charge(32); }                     /* _sceSDC (SyncDCache's line loop) */
HLE_SCE(_sceIDC) { hle_time_charge(32); }                     /* _sceIDC (InvalidDCache's line loop) */

HLE_SCE(memcpy)                                              /* memcpy(dst, src, n) -> dst */
{
    PZ_SCOPE(memcpy);
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    if (!n) { RET(d); return; }
    uint8_t *hd = plain_ram(d, n), *hs = plain_ram(s, n);
    if (!hd || !hs || (hd < hs + n && hs < hd + n)) { GUEST_SCE(memcpy)(c); return; }
    memcpy(hd, hs, n);
    hle_stat_fast_memcpy++;
    hle_time_charge(16 + n / 4);
    RET(d);
}

HLE_SCE(memset)                                              /* memset(dst, c, n) -> dst */
{
    PZ_SCOPE(memset);
    uint32_t d = ARG(0), v = ARG(1), n = ARG(2);
    if (!n) { RET(d); return; }
    uint8_t *hd = plain_ram(d, n);
    if (!hd) { GUEST_SCE(memset)(c); return; }
    memset(hd, (int)(v & 0xFF), n);
    hle_stat_fast_memset++;
    hle_time_charge(16 + n / 8);
    RET(d);
}
