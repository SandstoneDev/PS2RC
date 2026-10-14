/* iop_kernel_test.c - the IOP runtime's kernel and library HLE seen from a module: a hand-written "module" whose
 * functions are plain C (what generated code looks like from the runtime's side). Prints one line per check;
 * tests/iop/test_iop_rt.py compares them. No game or SDK material. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "iop_rt.h"
#include "iop_rt_ucontext.h"

#define BASE 0x1000u
enum {
    I_CREATE_TH, I_START_TH, I_CREATE_SEMA, I_SIGNAL, I_WAIT, I_CREATE_EVF, I_SET_EVF, I_WAIT_EVF, I_REG_RPC,
    I_SET_QUEUE, I_RPC_LOOP, I_GET_TID, I_ALLOC_TIMER, I_SET_TIMER_HANDLER, I_START_TIMER, I_SD_SET_PARAM,
    I_SD_SET_SWITCH, I_SD_SET_ADDR, I_SD_SET_CORE_ATTR, I_SD_SPU2_HANDLER, I_SD_VOICE_TRANS, I_SD_TRANS_HANDLER,
    I_SPRINTF, I_ALLOC, I_FREE, I_ISIGNAL, I_VB_END, I_SLEEP, I_IWAKEUP, I_POLL, N_IMP
};
static const IopImportDesc imps[N_IMP] = {
    {"thbase", 4, 0}, {"thbase", 6, 0}, {"thsemap", 4, 0}, {"thsemap", 6, 0}, {"thsemap", 8, 0}, {"thevent", 4, 0},
    {"thevent", 6, 0}, {"thevent", 10, 0}, {"sifcmd", 17, 0}, {"sifcmd", 19, 0}, {"sifcmd", 22, 0},
    {"thbase", 20, 0}, {"timrman", 4, 0}, {"timrman", 20, 0}, {"timrman", 23, 0}, {"libsd", 5, 0}, {"libsd", 7, 0},
    {"libsd", 9, 0}, {"libsd", 11, 0}, {"libsd", 27, 0}, {"libsd", 17, 0}, {"libsd", 26, 0}, {"sysclib", 19, 0},
    {"sysmem", 4, 0}, {"sysmem", 5, 0}, {"thsemap", 7, 0}, {"vblank", 5, 0}, {"thbase", 24, 0}, {"thbase", 26, 0},
    {"thsemap", 9, 0},
};
static IopFn imp[N_IMP];
static char logbuf[4096];
static IopRt *rt;

static void logf_(const char *fmt, ...)
{
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (logbuf[0]) strcat(logbuf, " ");
    strcat(logbuf, b);
}

static uint32_t W32(uint32_t a, uint32_t v) { memcpy(iop_rt_mem(rt) + a, &v, 4); return a; }
static uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, iop_rt_mem(rt) + a, 4); return v; }

/* call import k with up to 6 arguments (o32: a0-a3, then the stack) */
static uint32_t call(IopCpu *c, int k, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    uint32_t save = c->r[29];
    c->r[29] -= 32;
    W32(c->r[29] + 16, a4);
    W32(c->r[29] + 20, a5);
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    imp[k](c);
    c->r[29] = save;
    return c->r[2];
}
#define C0(k) call(c, k, 0, 0, 0, 0, 0, 0)
#define C1(k, a) call(c, k, a, 0, 0, 0, 0, 0)
#define C2(k, a, b) call(c, k, a, b, 0, 0, 0, 0)
#define C3(k, a, b, d) call(c, k, a, b, d, 0, 0, 0)
#define C4(k, a, b, d, e) call(c, k, a, b, d, e, 0, 0)

/* guest memory scratch (module bss) */
#define G_PARAM  (BASE + 0x100)
#define G_SEMA   (BASE + 0x140)       /* sema ids */
#define G_RES    (BASE + 0x180)
#define G_RPCSD  (BASE + 0x200)
#define G_RPCQ   (BASE + 0x280)
#define G_RPCBUF (BASE + 0x300)
#define G_STR    (BASE + 0x400)
#define G_OUT    (BASE + 0x500)
#define G_TIMER  (BASE + 0x600)

static uint32_t mk_thread(IopCpu *c, uint32_t entry, uint32_t prio)
{
    W32(G_PARAM, 0x02000000); W32(G_PARAM + 4, 0); W32(G_PARAM + 8, entry); W32(G_PARAM + 12, 0x800);
    W32(G_PARAM + 16, prio);
    return C1(I_CREATE_TH, G_PARAM);
}
static uint32_t mk_sema(IopCpu *c, uint32_t init, uint32_t max, uint32_t attr)
{
    W32(G_PARAM, attr); W32(G_PARAM + 4, 0); W32(G_PARAM + 8, init); W32(G_PARAM + 12, max);
    return C1(I_CREATE_SEMA, G_PARAM);
}

/* ---- threads (entry addresses 0x2000 + 16 k) */
static void th_high(IopCpu *c)
{
    logf_("H:wait");
    C1(I_WAIT, R32(G_SEMA));
    logf_("H:got");
}
static void th_low(IopCpu *c)
{
    logf_("L:sig");
    C1(I_SIGNAL, R32(G_SEMA));
    logf_("L:end");
}
static void th_fifo(IopCpu *c)
{
    char n = (char)('A' + c->r[4]);
    logf_("%c:wait", n);
    C1(I_WAIT, R32(G_SEMA + 4));
    logf_("%c:got", n);
}
static void th_evf(IopCpu *c)
{
    uint32_t r = C4(I_WAIT_EVF, R32(G_SEMA + 8), 3, 0x10, G_RES);
    logf_("E:got r=%d bits=%u", (int)r, R32(G_RES));
}
static void rpc_handler(IopCpu *c)                 /* (fno, buf, size) -> buf */
{
    uint32_t fno = c->r[4], buf = c->r[5];
    if (fno == 2) C0(I_VB_END);                    /* waits for a vertical blank inside the call */
    W32(buf, R32(buf) + fno);
    logf_("R:fno%u", fno);
    C1(I_SIGNAL, R32(G_SEMA + 12));                /* wakes the lower-priority worker: it runs after the reply */
    c->r[2] = buf;
}
static void th_server(IopCpu *c)
{
    C2(I_SET_QUEUE, G_RPCQ, C0(I_GET_TID));
    {                                              /* sceSifRegisterRpc has 7 arguments: the queue is the 7th */
        uint32_t save = c->r[29];
        c->r[29] -= 32;
        W32(c->r[29] + 16, 0); W32(c->r[29] + 20, 0); W32(c->r[29] + 24, G_RPCQ);
        c->r[4] = G_RPCSD; c->r[5] = 0x55667788u; c->r[6] = 0x2050; c->r[7] = G_RPCBUF;
        imp[I_REG_RPC](c);
        c->r[29] = save;
    }
    C1(I_RPC_LOOP, G_RPCQ);
}
static void th_worker(IopCpu *c)
{
    for (;;) {
        C1(I_WAIT, R32(G_SEMA + 12));
        W32(G_RPCBUF, 0xEEu);
        logf_("W:ran");
    }
}
static void timer_handler(IopCpu *c)
{
    static int n;
    logf_("T:%u", c->r[4]);
    c->r[2] = ++n < 2 ? 1000u : 0u;
}
static void intr_handler(IopCpu *c)                /* SPU2 IRQ: (core bits, data) */
{
    logf_("I:core%u", c->r[4]);
    C1(I_ISIGNAL, R32(c->r[5]));
    c->r[2] = 1;
}
static void trans_handler(IopCpu *c)
{
    logf_("D:ch%u data%u", c->r[4], c->r[5]);
    c->r[2] = 0;
}
static void th_irqwait(IopCpu *c)
{
    C1(I_WAIT, R32(G_SEMA + 16));
    logf_("Q:woke");
}
static void th_sleep(IopCpu *c)
{
    logf_("S:sleep");
    C0(I_SLEEP);
    logf_("S:awake");
}

static void mod_start(IopCpu *c)
{
    W32(G_SEMA, mk_sema(c, 0, 1, 0));
    W32(G_SEMA + 4, mk_sema(c, 0, 2, 0));
    W32(G_PARAM, 0); W32(G_PARAM + 4, 0); W32(G_PARAM + 8, 0);
    W32(G_SEMA + 8, C1(I_CREATE_EVF, G_PARAM));
    W32(G_SEMA + 12, mk_sema(c, 0, 1, 1));
    W32(G_SEMA + 16, mk_sema(c, 0, 1, 1));
    uint32_t h = mk_thread(c, 0x2000, 50), l = mk_thread(c, 0x2010, 60);
    C2(I_START_TH, l, 0);
    C2(I_START_TH, h, 0);
    c->r[2] = 0;
}

static IopFn fn_at(uint32_t a)
{
    switch (a) {
    case 0x1000: return mod_start;
    case 0x2000: return th_high;
    case 0x2010: return th_low;
    case 0x2020: return th_fifo;
    case 0x2030: return th_evf;
    case 0x2040: return th_server;
    case 0x2050: return rpc_handler;
    case 0x2060: return th_worker;
    case 0x2070: return timer_handler;
    case 0x2080: return intr_handler;
    case 0x2090: return trans_handler;
    case 0x20A0: return th_irqwait;
    case 0x20B0: return th_sleep;
    default: return 0;
    }
}

static const uint32_t image[4] = {0};
static const IopModule mod = {"rtkernel", 0x101, "-", BASE, 0x2000, 16, BASE, BASE + 0x7FF0, image, N_IMP, imps, imp,
                              fn_at};

/* sound back-end recorder */
static char sdlog[2048];
static void sdlogf(const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (sdlog[0]) strcat(sdlog, " ");
    strcat(sdlog, b);
}
static uint8_t spuram[0x200000];
static void s_param(void *u, unsigned v, unsigned p, uint16_t x) { (void)u; sdlogf("P%u.%u=%x", v, p, x); }
static void s_addr(void *u, unsigned v, unsigned w, uint32_t a) { (void)u; sdlogf("A%u.%u=%x", v, w, a); }
static void s_kon(void *u, unsigned core, uint32_t m) { (void)u; sdlogf("KON%u=%x", core, m); }
static void s_koff(void *u, unsigned core, uint32_t m) { (void)u; sdlogf("KOFF%u=%x", core, m); }
static void s_irq(void *u, uint32_t a, int e) { (void)u; sdlogf("IRQ%x,%d", a, e); }
static void s_dma(void *u, uint32_t a, uint32_t n) { (void)u; sdlogf("DMA%x+%u", a, n); }

int main(void)
{
    IopRtConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    iop_rt_coro_ucontext(&cfg.coro);
    cfg.mem_size = 0x100000;
    cfg.sd.ram = spuram; cfg.sd.ram_size = sizeof spuram;
    cfg.sd.set_param = s_param; cfg.sd.set_addr = s_addr; cfg.sd.key_on = s_kon; cfg.sd.key_off = s_koff;
    cfg.sd.set_irq = s_irq; cfg.sd.dma_done = s_dma;
    rt = iop_rt_create(&cfg);
    int r = iop_rt_load(rt, &mod, 0, 0, 8);
    printf("K1 %d %s\n", r, logbuf);                        /* preemption by SignalSema */

    /* an outside caller (no thread running): import calls through a scratch cpu */
    IopCpu x;
    memset(&x, 0, sizeof x);
    x.mem = iop_rt_mem(rt); x.rt = rt; x.r[29] = 0xF0000; x.r[28] = BASE + 0x7FF0;
    IopCpu *c = &x;

    logbuf[0] = 0;                                           /* FIFO inside a priority */
    uint32_t a = mk_thread(c, 0x2020, 70), b = mk_thread(c, 0x2020, 70);
    C2(I_START_TH, a, 0);
    C2(I_START_TH, b, 1);
    iop_rt_run(rt);
    C1(I_SIGNAL, R32(G_SEMA + 4));
    C1(I_SIGNAL, R32(G_SEMA + 4));
    iop_rt_run(rt);
    printf("K2 %s\n", logbuf);

    logbuf[0] = 0;                                           /* event flag AND + clear */
    C2(I_START_TH, mk_thread(c, 0x2030, 70), 0);
    iop_rt_run(rt);
    C2(I_SET_EVF, R32(G_SEMA + 8), 1);
    iop_rt_run(rt);
    logf_("after1");
    C2(I_SET_EVF, R32(G_SEMA + 8), 6);
    iop_rt_run(rt);
    printf("K3 %s\n", logbuf);

    logbuf[0] = 0;                                           /* SIF RPC: the reply leaves before the worker runs */
    C2(I_START_TH, mk_thread(c, 0x2060, 45), 0);
    C2(I_START_TH, mk_thread(c, 0x2040, 40), 0);
    iop_rt_run(rt);
    uint32_t send[4] = {100, 1, 2, 3}, recv[4] = {0};
    int s1 = iop_rt_sif_call(rt, 0x55667788u, 5, send, 16, recv, 16);
    printf("K4 %d %u %u %s buf=%u\n", s1, recv[0], recv[1], logbuf, R32(G_RPCBUF));
    logbuf[0] = 0;
    s1 = iop_rt_sif_call(rt, 0x55667788u, 2, send, 16, recv, 16);   /* waits for a vblank inside the handler */
    int s2 = iop_rt_sif_call(rt, 0x99999999u, 1, send, 16, recv, 16);
    printf("K5 %d %u %d %s\n", s1, recv[0], s2, logbuf);

    logbuf[0] = 0;                                           /* hardware timer */
    uint32_t t = C3(I_ALLOC_TIMER, 1, 32, 1);
    C4(I_SET_TIMER_HANDLER, t, 36864, 0x2070, 77);
    C1(I_START_TIMER, t);
    printf("K6 n=%d act=%d ns=%llu", iop_rt_timer_count(rt), iop_rt_timer_active(rt, (int)t - 1),
           (unsigned long long)iop_rt_timer_period_ns(rt, (int)t - 1));
    iop_rt_timer_expire(rt, (int)t - 1);
    iop_rt_timer_expire(rt, (int)t - 1);
    printf(" act=%d %s\n", iop_rt_timer_active(rt, (int)t - 1), logbuf);

    logbuf[0] = 0;                                           /* libsd: registers, IRQ handler, DMA */
    C2(I_START_TH, mk_thread(c, 0x20A0, 30), 0);
    iop_rt_run(rt);
    C2(I_SD_SPU2_HANDLER, 0x2080, G_SEMA + 16);
    C3(I_SD_TRANS_HANDLER, 1, 0x2090, 42);
    C2(I_SD_SET_PARAM, 0x0300 | (5 << 1) | 1, 0x1EFF);
    C2(I_SD_SET_PARAM, 0x0980, 0x3FFF);                      /* core parameter: not a voice register */
    C2(I_SD_SET_SWITCH, 0x1500 | 1, 5);
    C2(I_SD_SET_SWITCH, 0x1600, (1u << 23) | 1u);
    C2(I_SD_SET_ADDR, 0x2140 | (3 << 1), 0x5000);
    C2(I_SD_SET_ADDR, 0x1F00, 0x1230);
    C2(I_SD_SET_CORE_ATTR, 4, 1);
    memcpy(iop_rt_mem(rt) + G_OUT, "abcdefgh", 8);
    uint32_t n = call(c, I_SD_VOICE_TRANS, 1, 0, G_OUT, 0x7000, 8, 0);
    iop_rt_spu_irq(rt, 1);
    iop_rt_run(rt);
    printf("K7 n=%u ram=%.8s sd=[%s] %s\n", n, spuram + 0x7000, sdlog, logbuf);

    logbuf[0] = 0;                                           /* sprintf / sysmem / sleep + iWakeup / PollSema */
    memcpy(iop_rt_mem(rt) + G_STR, "%s-%d-%04x-%c-%5s|", 19);
    memcpy(iop_rt_mem(rt) + G_STR + 0x40, "ab", 3);
    memcpy(iop_rt_mem(rt) + G_STR + 0x48, "q", 2);
    uint32_t len;
    {                                                        /* 6 arguments: 3 of them on the stack */
        uint32_t save = c->r[29];
        c->r[29] -= 32;
        W32(c->r[29] + 16, 0xBEEF); W32(c->r[29] + 20, 'z'); W32(c->r[29] + 24, G_STR + 0x48);
        c->r[4] = G_OUT; c->r[5] = G_STR; c->r[6] = G_STR + 0x40; c->r[7] = (uint32_t)-7;
        imp[I_SPRINTF](c);
        len = c->r[2];
        c->r[29] = save;
    }
    uint32_t m1 = C3(I_ALLOC, 0, 0x100, 0), m2 = C3(I_ALLOC, 1, 0x100, 0);
    C1(I_FREE, m1);
    uint32_t m3 = C3(I_ALLOC, 0, 0x80, 0);
    uint32_t sl = mk_thread(c, 0x20B0, 20);
    C2(I_START_TH, sl, 0);
    iop_rt_run(rt);
    C1(I_IWAKEUP, sl);
    iop_rt_run(rt);
    uint32_t sp = mk_sema(c, 1, 1, 0);
    int p1 = (int)C1(I_POLL, sp), p2 = (int)C1(I_POLL, sp), ov1 = (int)C1(I_SIGNAL, sp), ov2 = (int)C1(I_SIGNAL, sp);
    printf("K8 %u [%s] low=%d high=%d reuse=%d %s poll=%d,%d sig=%d,%d\n", len, (char *)iop_rt_mem(rt) + G_OUT,
           m1 != 0, m2 > m1, m3 == m1, logbuf, p1, p2, ov1, ov2);
    const IopRtStats *st = iop_rt_stats(rt);
    printf("K9 unresolved=%u unimpl=%u\n", st->imports_unresolved, st->unimpl_calls);
    return 0;
}
