/* iop_rt.c - IOP runtime core: memory, modules, threads (coroutines + priority scheduler), semaphores, event flags,
 * sleep/wakeup/delay, alarms, hardware timers, interrupt context, sysmem, SIF RPC servers. The behaviour follows the
 * documented IOP kernel interfaces (thbase, thsemap, thevent, intrman, sysmem, timrman, sifman, sifcmd, loadcore).
 * See iop_rt.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iop_rt_int.h"

IopRt *iop_rt_current;

/* ------------------------------------------------------------------ logging */

void iop_rt_logv(IopRt *rt, int level, const char *fmt, va_list ap)
{
    if (!rt->cfg.log) return;
    char b[768];
    vsnprintf(b, sizeof b, fmt, ap);
    rt->cfg.log(rt->cfg.log_user, level, "%s", b);
}

void iop_rt_log(IopRt *rt, int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    iop_rt_logv(rt, level, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ memory */

static void *ralloc(IopRt *rt, size_t n)
{
    if (rt->cfg.alloc) return rt->cfg.alloc(rt->cfg.alloc_user, n);
    return calloc(1, n);
}

static void rfree(IopRt *rt, void *p)
{
    if (!p) return;
    if (rt->cfg.alloc) { if (rt->cfg.free) rt->cfg.free(rt->cfg.alloc_user, p); }
    else free(p);
}

#ifdef IOP_CHECK
static uint32_t check_size = IOP_MEM_MASK + 1u;
uint32_t iop_rt_check_addr(uint32_t a, unsigned size)
{
    uint32_t m = a & IOP_MEM_MASK;
    if (m + size > check_size || (a & (size - 1u))) {
        fprintf(stderr, "IOP_CHECK: access of %u bytes at 0x%08x outside the backed IOP RAM (0x%x) or unaligned\n",
                size, a, check_size);
        abort();
    }
    return m;
}
#endif

uint8_t *iop_rt_mem(IopRt *rt) { return rt->mem; }
uint32_t iop_rt_mem_size(IopRt *rt) { return rt->mem_size; }
const IopRtStats *iop_rt_stats(IopRt *rt) { return &rt->st; }

void *iop_ptr(IopCpu *c, uint32_t addr) { return addr ? c->mem + (addr & IOP_MEM_MASK) : 0; }

uint32_t iop_arg(IopCpu *c, int k)
{
    if (k < 4) return c->r[4 + k];
    return rd32(c->rt, c->r[29] + 4u * (uint32_t)k);
}

void iop_ret(IopCpu *c, uint32_t v) { c->r[2] = v; }

/* sysmem: first fit over [heap_lo, heap_hi) in 256-byte units; mode 0 low, 1 high, 2 at `addr` */
uint32_t iop_rt_alloc(IopRt *rt, uint32_t size, int mode, uint32_t addr)
{
    size = (size + 255u) & ~255u;
    if (!size) size = 256;
    if (rt->nblocks >= IOP_MAX_BLOCKS) return 0;
    uint32_t got = 0;
    int pos = 0;
    if (mode == 2) {
        addr &= ~255u;
        if (addr < rt->heap_lo || addr + size > rt->heap_hi) return 0;
        for (pos = 0; pos < rt->nblocks; pos++) {
            IopBlock *b = &rt->blocks[pos];
            if (b->addr + b->size <= addr) continue;
            if (b->addr >= addr + size) break;
            return 0;                                         /* overlaps */
        }
        got = addr;
    } else if (mode == 1) {
        uint32_t end = rt->heap_hi;
        for (pos = rt->nblocks; pos >= 0; pos--) {
            uint32_t start = pos > 0 ? rt->blocks[pos - 1].addr + rt->blocks[pos - 1].size : rt->heap_lo;
            if (end >= start && end - start >= size) { got = end - size; break; }
            if (pos > 0) end = rt->blocks[pos - 1].addr;
        }
        if (pos < 0) return 0;
    } else {
        uint32_t start = rt->heap_lo;
        for (pos = 0; pos <= rt->nblocks; pos++) {
            uint32_t end = pos < rt->nblocks ? rt->blocks[pos].addr : rt->heap_hi;
            if (end >= start && end - start >= size) { got = start; break; }
            if (pos < rt->nblocks) start = rt->blocks[pos].addr + rt->blocks[pos].size;
        }
        if (pos > rt->nblocks) return 0;
    }
    memmove(&rt->blocks[pos + 1], &rt->blocks[pos], (size_t)(rt->nblocks - pos) * sizeof rt->blocks[0]);
    rt->blocks[pos].addr = got;
    rt->blocks[pos].size = size;
    rt->nblocks++;
    rt->st.mem_used += size;
    if (rt->st.mem_used > rt->st.mem_peak) rt->st.mem_peak = rt->st.mem_used;
    return got;
}

int iop_rt_free(IopRt *rt, uint32_t addr)
{
    for (int i = 0; i < rt->nblocks; i++) {
        if (rt->blocks[i].addr != addr) continue;
        rt->st.mem_used -= rt->blocks[i].size;
        memmove(&rt->blocks[i], &rt->blocks[i + 1], (size_t)(rt->nblocks - i - 1) * sizeof rt->blocks[0]);
        rt->nblocks--;
        return KE_OK;
    }
    return KE_ERROR;
}

/* ------------------------------------------------------------------ create / destroy */

IopRt *iop_rt_create(const IopRtConfig *cfg)
{
    IopRt *rt = cfg->alloc ? (IopRt *)cfg->alloc(cfg->alloc_user, sizeof *rt) : (IopRt *)calloc(1, sizeof *rt);
    if (!rt) return 0;
    memset(rt, 0, sizeof *rt);
    rt->cfg = *cfg;
    if (!rt->cfg.host_stack) rt->cfg.host_stack = 64u * 1024u;
    rt->mem_size = cfg->mem_size ? cfg->mem_size : IOP_MEM_MASK + 1u;
    if (rt->mem_size > IOP_MEM_MASK + 1u) rt->mem_size = IOP_MEM_MASK + 1u;
    rt->mem_size &= ~255u;
    rt->mem = (uint8_t *)ralloc(rt, (size_t)rt->mem_size + 64u);
    if (!rt->mem || !cfg->coro.create || !cfg->coro.swap || !cfg->coro.main_ctx) {
        rfree(rt, rt->mem);
        if (cfg->alloc) { if (cfg->free) cfg->free(cfg->alloc_user, rt); } else free(rt);
        return 0;
    }
#ifdef IOP_CHECK
    check_size = rt->mem_size;
#endif
    rt->heap_lo = 0x1000u;                                    /* page 0 stays unused (NULL) */
    rt->heap_hi = rt->mem_size - IOP_INTR_LEVELS * IOP_INTR_STACK;
    rt->cur = -1;
    rt->main = cfg->coro.main_ctx(cfg->coro.user);
    iop_rt_libs_init(rt);
    return rt;
}

void iop_rt_destroy(IopRt *rt)
{
    if (!rt) return;
    rfree(rt, rt->mem);
    if (rt->cfg.alloc) { if (rt->cfg.free) rt->cfg.free(rt->cfg.alloc_user, rt); }
    else free(rt);
}

/* ------------------------------------------------------------------ modules, imports, calls */

int iop_rt_bind(IopRt *rt, const char *lib, unsigned num, IopFn fn)
{
    if (rt->nbinds >= IOP_MAX_BINDS) return -1;
    IopBind *b = &rt->binds[rt->nbinds++];
    strncpy(b->lib, lib, 8);
    b->lib[8] = 0;
    b->num = num;
    b->fn = fn;
    return 0;
}

static void hle_unimpl(IopCpu *c)
{
    IopRt *rt = c->rt;
    rt->st.unimpl_calls++;
    if (rt->unimpl_logged++ < 16)
        iop_rt_log(rt, 0, "IOP: call of an unimplemented import (caller ra 0x%08x) - returns 0\n", c->r[31]);
    c->r[2] = 0;
}

const IopModule *iop_rt_module(IopRt *rt, const char *name)
{
    for (int i = 0; i < rt->nmods; i++)
        if (!strcmp(rt->mods[i]->name, name)) return rt->mods[i];
    return 0;
}

IopFn iop_rt_fn(IopRt *rt, uint32_t addr)
{
    for (int i = 0; i < rt->nmods; i++) {
        IopFn f = rt->mods[i]->fn_at(addr);
        if (f) return f;
    }
    return 0;
}

void iop_rt_call(IopCpu *c, uint32_t addr)
{
    IopRt *rt = c->rt;
    IopFn f = iop_rt_fn(rt, addr);
    if (f) { f(c); return; }
    for (int i = 0; i < rt->nmods; i++) {
        const IopModule *m = rt->mods[i];
        for (uint32_t k = 0; k < m->nimports; k++)
            if (m->imports[k].stub == addr) { m->imp[k](c); return; }
    }
    iop_rt_log(rt, 0, "IOP: indirect call to 0x%08x: no translated function there - ignored\n", addr);
}

void iop_rt_trap(IopCpu *c, uint32_t pc, uint32_t word)
{
    iop_rt_log(c->rt, 0, "IOP: untranslatable instruction %08x at 0x%08x ignored\n", word, pc);
}

/* ------------------------------------------------------------------ threads and the scheduler */

static int thread_index(IopRt *rt, uint32_t id)
{
    if (id == 0) return rt->cur;
    int t = (int)id - 1;
    if (t < 0 || t >= IOP_MAX_THREADS || rt->th[t].state == TS_FREE) return -1;
    return t;
}

static void thread_entry(void)
{
    IopRt *rt = iop_rt_current;
    int t = rt->cur;
    IopThread *T = &rt->th[t];
    IopCpu *c = &T->cpu;
    memset(c, 0, sizeof *c);
    c->mem = rt->mem;
    c->rt = rt;
    c->r[4] = T->arg;
    c->r[5] = T->arg2;
    c->r[29] = T->stack + T->stack_size - 16u;
    c->r[28] = T->gp;
    IopFn f = iop_rt_fn(rt, T->entry);
    if (f) f(c);
    else iop_rt_log(rt, 0, "IOP: thread %d entry 0x%08x is not translated code\n", t + 1, T->entry);
    T->state = TS_DORMANT;                                    /* ExitThread by returning */
    T->coro_started = 2;                                      /* this coroutine has finished */
    for (;;) rt->cfg.coro.swap(rt->cfg.coro.user, T->coro, rt->main);
}

static int pick(IopRt *rt)
{
    int best = -1;
    for (int t = 0; t < IOP_MAX_THREADS; t++) {
        IopThread *T = &rt->th[t];
        if (T->state != TS_READY) continue;
        if (best < 0 || T->prio < rt->th[best].prio || (T->prio == rt->th[best].prio && T->seq < rt->th[best].seq))
            best = t;
    }
    return best;
}

void iop_rt_run(IopRt *rt)
{
    if (rt->cur >= 0) return;                                 /* called from an IOP thread: nothing to do */
    for (;;) {
        int t = pick(rt);
        if (t < 0) break;
        IopThread *T = &rt->th[t];
        if (!T->coro || T->coro_started == 2) {
            iop_rt_current = rt;
            T->coro = rt->cfg.coro.create(rt->cfg.coro.user, rt->cfg.host_stack, thread_entry);
            T->coro_started = 1;
            if (!T->coro) {
                iop_rt_log(rt, 0, "IOP: no host stack for thread %d - dropped\n", t + 1);
                T->state = TS_DORMANT;
                continue;
            }
        }
        T->state = TS_RUN;
        rt->cur = t;
        rt->st.switches++;
        iop_rt_current = rt;
        rt->cfg.coro.swap(rt->cfg.coro.user, rt->main, T->coro);
        rt->cur = -1;
    }
}

void iop_rt_ready(IopRt *rt, int t, int result)
{
    IopThread *T = &rt->th[t];
    T->state = TS_READY;
    T->wait = W_NONE;
    T->result = result;
    T->seq = ++rt->seq;
}

void iop_rt_wait(IopRt *rt, int kind)
{
    if (rt->cur < 0 || rt->intr_level) {
        iop_rt_log(rt, 0, "IOP: a blocking call outside a thread / in an interrupt handler - not waiting\n");
        return;
    }
    IopThread *T = &rt->th[rt->cur];
    T->state = TS_WAIT;
    T->wait = kind;
    T->seq = ++rt->seq;                                       /* wait order (FIFO wake-ups) */
    rt->cfg.coro.swap(rt->cfg.coro.user, T->coro, rt->main);
}

void iop_rt_preempt_check(IopRt *rt)
{
    if (rt->cur < 0 || rt->intr_level) return;
    IopThread *T = &rt->th[rt->cur];
    int b = pick(rt);
    if (b < 0 || rt->th[b].prio >= T->prio) return;
    T->state = TS_READY;                                      /* keeps its place at the head of its priority */
    rt->cfg.coro.swap(rt->cfg.coro.user, T->coro, rt->main);
}

/* ------------------------------------------------------------------ interrupt context */

uint32_t iop_rt_call_handler(IopRt *rt, uint32_t fn, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2)
{
    IopFn f = iop_rt_fn(rt, fn);
    if (!f) {
        iop_rt_log(rt, 0, "IOP: handler 0x%08x is not translated code\n", fn);
        return 0;
    }
    if (rt->intr_level >= IOP_INTR_LEVELS) {
        iop_rt_log(rt, 0, "IOP: interrupt nesting too deep - handler 0x%08x skipped\n", fn);
        return 0;
    }
    IopCpu c;
    memset(&c, 0, sizeof c);
    c.mem = rt->mem;
    c.rt = rt;
    c.r[4] = a0;
    c.r[5] = a1;
    c.r[6] = a2;
    c.r[28] = gp;
    c.r[29] = rt->heap_hi + (uint32_t)(rt->intr_level + 1) * IOP_INTR_STACK - 16u;
    rt->intr_level++;
    rt->st.intr_calls++;
    f(&c);
    rt->intr_level--;
    return c.r[2];
}

/* ------------------------------------------------------------------ thbase */

#define A(k) iop_arg(c, k)
#define RET(v) (c->r[2] = (uint32_t)(v))

static uint32_t mod_gp(IopCpu *c) { return c->r[28]; }

void hle_CreateThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t p = A(0);
    int t;
    for (t = 0; t < IOP_MAX_THREADS && rt->th[t].state != TS_FREE; t++) {}
    if (t >= IOP_MAX_THREADS) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[t];
    memset(T, 0, sizeof *T);
    T->attr = rd32(rt, p);
    T->option = rd32(rt, p + 4);
    T->entry = rd32(rt, p + 8);
    T->stack_size = (rd32(rt, p + 12) + 15u) & ~15u;
    T->prio = T->init_prio = (int)rd32(rt, p + 16);
    T->gp = mod_gp(c);
    T->stack = iop_rt_alloc(rt, T->stack_size, 1, 0);
    if (!T->stack) { RET(KE_NO_MEMORY); return; }
    T->state = TS_DORMANT;
    rt->st.threads++;
    iop_rt_log(rt, 2, "IOP: CreateThread %d entry 0x%08x prio %d stack 0x%x at 0x%08x\n", t + 1, T->entry, T->prio,
               T->stack_size, T->stack);
    RET(t + 1);
}

void hle_DeleteThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    int t = thread_index(rt, A(0));
    if (t < 0) { RET(KE_UNKNOWN_THID); return; }
    if (rt->th[t].state != TS_DORMANT) { RET(KE_NOT_DORMANT); return; }
    iop_rt_free(rt, rt->th[t].stack);
    rt->th[t].state = TS_FREE;
    RET(KE_OK);
}

static void start_thread(IopCpu *c, int t, uint32_t arg)
{
    IopRt *rt = c->rt;
    IopThread *T = &rt->th[t];
    T->arg = arg;
    T->arg2 = 0;
    T->prio = T->init_prio;
    T->wakeups = 0;
    iop_rt_ready(rt, t, 0);
}

void hle_StartThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    int t = thread_index(rt, A(0));
    if (t < 0) { RET(KE_UNKNOWN_THID); return; }
    if (rt->th[t].state != TS_DORMANT) { RET(KE_NOT_DORMANT); return; }
    start_thread(c, t, A(1));
    RET(KE_OK);
    iop_rt_preempt_check(rt);
}

void hle_ExitThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    if (rt->cur < 0) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[rt->cur];
    T->state = TS_DORMANT;
    T->coro_started = 2;
    for (;;) rt->cfg.coro.swap(rt->cfg.coro.user, T->coro, rt->main);
}

void hle_GetThreadId(IopCpu *c) { RET(c->rt->cur >= 0 ? c->rt->cur + 1 : KE_ERROR); }

void hle_ChangeThreadPriority(IopCpu *c)
{
    IopRt *rt = c->rt;
    int t = thread_index(rt, A(0));
    if (t < 0) { RET(KE_UNKNOWN_THID); return; }
    int p = (int)A(1);
    rt->th[t].prio = p ? p : rt->th[t].init_prio;
    RET(KE_OK);
    iop_rt_preempt_check(rt);
}

void hle_RotateThreadReadyQueue(IopCpu *c)
{
    IopRt *rt = c->rt;
    RET(KE_OK);
    if (rt->cur < 0) return;
    IopThread *T = &rt->th[rt->cur];
    int prio = (int)A(0) ? (int)A(0) : T->prio;
    if (prio != T->prio) return;
    for (int t = 0; t < IOP_MAX_THREADS; t++)
        if (rt->th[t].state == TS_READY && rt->th[t].prio == prio) {
            iop_rt_ready(rt, rt->cur, 0);                     /* to the tail of its priority */
            rt->cfg.coro.swap(rt->cfg.coro.user, T->coro, rt->main);
            return;
        }
}

void hle_SleepThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    if (rt->cur < 0) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[rt->cur];
    if (T->wakeups > 0) { T->wakeups--; RET(KE_OK); return; }
    iop_rt_wait(rt, W_SLEEP);
    RET(T->result);
}

static int wakeup(IopRt *rt, uint32_t id)
{
    int t = thread_index(rt, id);
    if (t < 0) return KE_UNKNOWN_THID;
    IopThread *T = &rt->th[t];
    if (T->state == TS_WAIT && T->wait == W_SLEEP) iop_rt_ready(rt, t, KE_OK);
    else T->wakeups++;
    return KE_OK;
}

void hle_WakeupThread(IopCpu *c) { RET(wakeup(c->rt, A(0))); iop_rt_preempt_check(c->rt); }
void hle_iWakeupThread(IopCpu *c) { RET(wakeup(c->rt, A(0))); }

void hle_CancelWakeupThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    int t = thread_index(rt, A(0));
    if (t < 0) { RET(KE_UNKNOWN_THID); return; }
    int n = rt->th[t].wakeups;
    rt->th[t].wakeups = 0;
    RET(n);
}

void hle_DelayThread(IopCpu *c)
{
    IopRt *rt = c->rt;
    if (rt->cur < 0) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[rt->cur];
    T->wait_until = rt->now_us + A(0);
    iop_rt_wait(rt, W_DELAY);
    RET(KE_OK);
}

/* the IOP system clock runs at 36.864 MHz */
static uint64_t clk_of_us(uint64_t us) { return us * 36864u / 1000u; }
static uint64_t us_of_clk(uint64_t clk) { return clk * 1000u / 36864u; }

void hle_GetSystemTime(IopCpu *c)
{
    uint64_t k = clk_of_us(c->rt->now_us);
    wr32(c->rt, A(0), (uint32_t)k);
    wr32(c->rt, A(0) + 4, (uint32_t)(k >> 32));
    RET(KE_OK);
}

void hle_USec2SysClock(IopCpu *c)
{
    uint64_t k = clk_of_us(A(0));
    wr32(c->rt, A(1), (uint32_t)k);
    wr32(c->rt, A(1) + 4, (uint32_t)(k >> 32));
    RET(KE_OK);
}

void hle_SysClock2USec(IopCpu *c)
{
    uint64_t k = (uint64_t)rd32(c->rt, A(0)) | (uint64_t)rd32(c->rt, A(0) + 4) << 32, us = us_of_clk(k);
    if (A(1)) wr32(c->rt, A(1), (uint32_t)(us / 1000000u));
    if (A(2)) wr32(c->rt, A(2), (uint32_t)(us % 1000000u));
    RET(KE_OK);
}

static void set_alarm(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint64_t k = (uint64_t)rd32(rt, A(0)) | (uint64_t)rd32(rt, A(0) + 4) << 32;
    for (int i = 0; i < IOP_MAX_ALARMS; i++) {
        IopAlarm *a = &rt->alarm[i];
        if (a->used) continue;
        a->used = 1;
        a->due = rt->now_us + us_of_clk(k);
        a->handler = A(1);
        a->common = A(2);
        a->gp = mod_gp(c);
        RET(KE_OK);
        return;
    }
    RET(KE_ERROR);
}
void hle_SetAlarm(IopCpu *c) { set_alarm(c); }
void hle_iSetAlarm(IopCpu *c) { set_alarm(c); }

void hle_CancelAlarm(IopCpu *c)
{
    IopRt *rt = c->rt;
    for (int i = 0; i < IOP_MAX_ALARMS; i++) {
        IopAlarm *a = &rt->alarm[i];
        if (a->used && a->handler == A(0) && a->common == A(1)) { a->used = 0; RET(KE_OK); return; }
    }
    RET(KE_ERROR);
}

void iop_rt_advance_us(IopRt *rt, uint64_t us)
{
    rt->now_us += us;
    for (int t = 0; t < IOP_MAX_THREADS; t++) {
        IopThread *T = &rt->th[t];
        if (T->state == TS_WAIT && T->wait == W_DELAY && T->wait_until <= rt->now_us) iop_rt_ready(rt, t, KE_OK);
    }
    for (int i = 0; i < IOP_MAX_ALARMS; i++) {
        IopAlarm *a = &rt->alarm[i];
        while (a->used && a->due <= rt->now_us) {
            uint32_t next = iop_rt_call_handler(rt, a->handler, a->gp, a->common, 0, 0);
            if (!next) { a->used = 0; break; }
            a->due += us_of_clk(next) ? us_of_clk(next) : 1;
        }
    }
}

/* ------------------------------------------------------------------ thsemap */

static IopSema *sema(IopRt *rt, uint32_t id)
{
    if (id == 0 || id > IOP_MAX_SEMAS || !rt->sem[id - 1].used) return 0;
    return &rt->sem[id - 1];
}

void hle_CreateSema(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t p = A(0);
    for (int i = 0; i < IOP_MAX_SEMAS; i++) {
        IopSema *s = &rt->sem[i];
        if (s->used) continue;
        s->used = 1;
        s->attr = rd32(rt, p);
        s->option = rd32(rt, p + 4);
        s->count = (int)rd32(rt, p + 8);
        s->max = (int)rd32(rt, p + 12);
        rt->st.semas++;
        RET(i + 1);
        return;
    }
    RET(KE_ERROR);
}

static int first_blocked(IopRt *rt, int kind, int id, int by_prio)
{
    int best = -1;
    for (int t = 0; t < IOP_MAX_THREADS; t++) {
        IopThread *T = &rt->th[t];
        if (T->state != TS_WAIT || T->wait != kind || T->wait_id != id) continue;
        if (best < 0 || (by_prio && T->prio < rt->th[best].prio) ||
            ((!by_prio || T->prio == rt->th[best].prio) && T->seq < rt->th[best].seq))
            best = t;
    }
    return best;
}

void hle_DeleteSema(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopSema *s = sema(rt, A(0));
    if (!s) { RET(KE_UNKNOWN_SEMID); return; }
    int t;
    while ((t = first_blocked(rt, W_SEMA, (int)A(0), 0)) >= 0) iop_rt_ready(rt, t, -425 /* KE_WAIT_DELETE */);
    s->used = 0;
    RET(KE_OK);
    iop_rt_preempt_check(rt);
}

static int signal_sema(IopRt *rt, uint32_t id)
{
    IopSema *s = sema(rt, id);
    if (!s) return KE_UNKNOWN_SEMID;
    int t = first_blocked(rt, W_SEMA, (int)id, (int)(s->attr & 1u));
    if (t >= 0) { iop_rt_ready(rt, t, KE_OK); return KE_OK; }
    if (s->count >= s->max) return KE_SEMA_OVF;
    s->count++;
    return KE_OK;
}

void hle_SignalSema(IopCpu *c) { RET(signal_sema(c->rt, A(0))); iop_rt_preempt_check(c->rt); }
void hle_iSignalSema(IopCpu *c) { RET(signal_sema(c->rt, A(0))); }

void hle_WaitSema(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopSema *s = sema(rt, A(0));
    if (!s) { RET(KE_UNKNOWN_SEMID); return; }
    if (s->count > 0) { s->count--; RET(KE_OK); return; }
    if (rt->cur < 0 || rt->intr_level) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[rt->cur];
    T->wait_id = (int)A(0);
    iop_rt_wait(rt, W_SEMA);
    RET(T->result);
}

void hle_PollSema(IopCpu *c)
{
    IopSema *s = sema(c->rt, A(0));
    if (!s) { RET(KE_UNKNOWN_SEMID); return; }
    if (s->count > 0) { s->count--; RET(KE_OK); return; }
    RET(KE_SEMA_ZERO);
}

void hle_ReferSemaStatus(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopSema *s = sema(rt, A(0));
    if (!s) { RET(KE_UNKNOWN_SEMID); return; }
    uint32_t p = A(1);
    int n = 0;
    for (int t = 0; t < IOP_MAX_THREADS; t++)
        if (rt->th[t].state == TS_WAIT && rt->th[t].wait == W_SEMA && rt->th[t].wait_id == (int)A(0)) n++;
    wr32(rt, p, s->attr);
    wr32(rt, p + 4, s->option);
    wr32(rt, p + 8, 0);
    wr32(rt, p + 12, (uint32_t)s->max);
    wr32(rt, p + 16, (uint32_t)s->count);
    wr32(rt, p + 20, (uint32_t)n);
    RET(KE_OK);
}

/* ------------------------------------------------------------------ thevent */

#define WEF_OR        1u
#define WEF_CLEAR     0x10u
#define WEF_CLEAR_ALL 0x20u

static IopEvf *evf(IopRt *rt, uint32_t id)
{
    if (id == 0 || id > IOP_MAX_EVFLAGS || !rt->evf[id - 1].used) return 0;
    return &rt->evf[id - 1];
}

static int evf_match(uint32_t flags, uint32_t bits, uint32_t mode)
{
    return (mode & WEF_OR) ? (flags & bits) != 0 : (flags & bits) == bits;
}

static void evf_take(IopRt *rt, IopEvf *e, uint32_t bits, uint32_t mode, uint32_t res)
{
    if (res) wr32(rt, res, e->bits);
    if (mode & WEF_CLEAR_ALL) e->bits = 0;
    else if (mode & WEF_CLEAR) e->bits &= ~bits;
}

void hle_CreateEventFlag(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t p = A(0);
    for (int i = 0; i < IOP_MAX_EVFLAGS; i++) {
        IopEvf *e = &rt->evf[i];
        if (e->used) continue;
        e->used = 1;
        e->attr = rd32(rt, p);
        e->option = rd32(rt, p + 4);
        e->bits = rd32(rt, p + 8);
        rt->st.evflags++;
        RET(i + 1);
        return;
    }
    RET(KE_ERROR);
}

void hle_DeleteEventFlag(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopEvf *e = evf(rt, A(0));
    if (!e) { RET(KE_UNKNOWN_EVFID); return; }
    int t;
    while ((t = first_blocked(rt, W_EVF, (int)A(0), 0)) >= 0) iop_rt_ready(rt, t, -425);
    e->used = 0;
    RET(KE_OK);
    iop_rt_preempt_check(rt);
}

static int set_evf(IopRt *rt, uint32_t id, uint32_t bits)
{
    IopEvf *e = evf(rt, id);
    if (!e) return KE_UNKNOWN_EVFID;
    e->bits |= bits;
    for (;;) {                                                /* waiters in wait order */
        int best = -1;
        for (int t = 0; t < IOP_MAX_THREADS; t++) {
            IopThread *T = &rt->th[t];
            if (T->state != TS_WAIT || T->wait != W_EVF || T->wait_id != (int)id) continue;
            if (!evf_match(e->bits, T->wait_bits, T->wait_mode)) continue;
            if (best < 0 || T->seq < rt->th[best].seq) best = t;
        }
        if (best < 0) break;
        IopThread *T = &rt->th[best];
        evf_take(rt, e, T->wait_bits, T->wait_mode, T->wait_res);
        iop_rt_ready(rt, best, KE_OK);
    }
    return KE_OK;
}

void hle_SetEventFlag(IopCpu *c) { RET(set_evf(c->rt, A(0), A(1))); iop_rt_preempt_check(c->rt); }
void hle_iSetEventFlag(IopCpu *c) { RET(set_evf(c->rt, A(0), A(1))); }

static void clear_evf(IopCpu *c)
{
    IopEvf *e = evf(c->rt, A(0));
    if (!e) { RET(KE_UNKNOWN_EVFID); return; }
    e->bits &= A(1);
    RET(KE_OK);
}
void hle_ClearEventFlag(IopCpu *c) { clear_evf(c); }
void hle_iClearEventFlag(IopCpu *c) { clear_evf(c); }

void hle_WaitEventFlag(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopEvf *e = evf(rt, A(0));
    if (!e) { RET(KE_UNKNOWN_EVFID); return; }
    uint32_t bits = A(1), mode = A(2), res = A(3);
    if (!bits) { RET(KE_EVF_ILPAT); return; }
    if (evf_match(e->bits, bits, mode)) { evf_take(rt, e, bits, mode, res); RET(KE_OK); return; }
    if (rt->cur < 0 || rt->intr_level) { RET(KE_ERROR); return; }
    IopThread *T = &rt->th[rt->cur];
    T->wait_id = (int)A(0);
    T->wait_bits = bits;
    T->wait_mode = mode;
    T->wait_res = res;
    iop_rt_wait(rt, W_EVF);
    RET(T->result);
}

void hle_PollEventFlag(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopEvf *e = evf(rt, A(0));
    if (!e) { RET(KE_UNKNOWN_EVFID); return; }
    if (!evf_match(e->bits, A(1), A(2))) { RET(KE_EVF_COND); return; }
    evf_take(rt, e, A(1), A(2), A(3));
    RET(KE_OK);
}

/* ------------------------------------------------------------------ intrman */

void hle_CpuSuspendIntr(IopCpu *c) { if (A(0)) wr32(c->rt, A(0), 0); RET(KE_OK); }
void hle_CpuResumeIntr(IopCpu *c) { RET(KE_OK); }
void hle_CpuEnableIntr(IopCpu *c) { RET(KE_OK); }
void hle_CpuDisableIntr(IopCpu *c) { RET(KE_OK); }
void hle_QueryIntrContext(IopCpu *c) { RET(c->rt->intr_level > 0); }

/* ------------------------------------------------------------------ sysmem */

void hle_AllocSysMemory(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t a = iop_rt_alloc(rt, A(1), (int)A(0), A(2));
    iop_rt_log(rt, 2, "IOP: AllocSysMemory(%u, 0x%x) -> 0x%08x (heap in use %u)\n", A(0), A(1), a, rt->st.mem_used);
    if (!a) iop_rt_log(rt, 0, "IOP: AllocSysMemory of 0x%x bytes failed (IOP RAM 0x%x, in use 0x%x)\n", A(1),
                       rt->mem_size, rt->st.mem_used);
    RET(a);
}

void hle_FreeSysMemory(IopCpu *c) { RET(iop_rt_free(c->rt, A(0))); }

static uint32_t free_total(IopRt *rt, int max_only)
{
    uint32_t tot = 0, mx = 0, start = rt->heap_lo;
    for (int i = 0; i <= rt->nblocks; i++) {
        uint32_t end = i < rt->nblocks ? rt->blocks[i].addr : rt->heap_hi;
        uint32_t g = end > start ? end - start : 0;
        tot += g;
        if (g > mx) mx = g;
        if (i < rt->nblocks) start = rt->blocks[i].addr + rt->blocks[i].size;
    }
    return max_only ? mx : tot;
}
void hle_QueryMemSize(IopCpu *c) { RET(c->rt->mem_size); }
void hle_QueryMaxFreeMemSize(IopCpu *c) { RET(free_total(c->rt, 1)); }
void hle_QueryTotalFreeMemSize(IopCpu *c) { RET(free_total(c->rt, 0)); }

void hle_Kprintf(IopCpu *c)
{
    char b[512];
    int n = iop_rt_format(c, A(0), 1, b, sizeof b);
    iop_rt_log(c->rt, 1, "%s", b);
    RET(n);
}

/* ------------------------------------------------------------------ timrman (hardware timers) */

static IopTimer *timer(IopRt *rt, uint32_t id)
{
    if (id == 0 || id > IOP_MAX_TIMERS || !rt->tim[id - 1].used) return 0;
    return &rt->tim[id - 1];
}

void hle_AllocHardTimer(IopCpu *c)
{
    IopRt *rt = c->rt;
    for (int i = 0; i < IOP_MAX_TIMERS; i++) {
        IopTimer *t = &rt->tim[i];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->source = A(0);
        t->size = A(1);
        t->prescale = A(2) ? A(2) : 1;
        RET(i + 1);
        return;
    }
    RET(KE_ERROR);
}

void hle_FreeHardTimer(IopCpu *c)
{
    IopTimer *t = timer(c->rt, A(0));
    if (!t) { RET(KE_ERROR); return; }
    t->used = 0;
    RET(KE_OK);
}

void hle_SetTimerHandler(IopCpu *c)
{
    IopTimer *t = timer(c->rt, A(0));
    if (!t) { RET(KE_ERROR); return; }
    t->compare = A(1);
    t->handler = A(2);
    t->common = A(3);
    t->gp = mod_gp(c);
    RET(KE_OK);
}

void hle_SetupHardTimer(IopCpu *c)
{
    IopTimer *t = timer(c->rt, A(0));
    if (!t) { RET(KE_ERROR); return; }
    t->source = A(1);
    t->mode = A(2);
    t->prescale = A(3) ? A(3) : 1;
    RET(KE_OK);
}

void hle_StartHardTimer(IopCpu *c)
{
    IopTimer *t = timer(c->rt, A(0));
    if (!t) { RET(KE_ERROR); return; }
    t->started = 1;
    iop_rt_log(c->rt, 2, "IOP: hardware timer %u started: compare %u, prescale %u, source %u\n", A(0), t->compare,
               t->prescale, t->source);
    RET(KE_OK);
}

void hle_StopHardTimer(IopCpu *c)
{
    IopTimer *t = timer(c->rt, A(0));
    if (!t) { RET(KE_ERROR); return; }
    t->started = 0;
    RET(KE_OK);
}

int iop_rt_timer_count(IopRt *rt)
{
    int n = 0;
    for (int i = 0; i < IOP_MAX_TIMERS; i++) if (rt->tim[i].used) n = i + 1;
    return n;
}

int iop_rt_timer_active(IopRt *rt, int k)
{
    if (k < 0 || k >= IOP_MAX_TIMERS) return 0;
    IopTimer *t = &rt->tim[k];
    return t->used && t->started && t->handler;
}

uint64_t iop_rt_timer_period_ns(IopRt *rt, int k)
{
    if (k < 0 || k >= IOP_MAX_TIMERS || !rt->tim[k].used) return 0;
    IopTimer *t = &rt->tim[k];
    if (!(t->source & 1u)) return 0;                          /* only the system clock (36.864 MHz) is known */
    return (uint64_t)t->compare * t->prescale * 1000000u / 36864u;
}

void iop_rt_timer_expire(IopRt *rt, int k)
{
    if (!iop_rt_timer_active(rt, k)) return;
    IopTimer *t = &rt->tim[k];
    uint32_t next = iop_rt_call_handler(rt, t->handler, t->gp, t->common, 0, 0);
    if (!next) t->started = 0;
    else t->compare = next;
}

/* ------------------------------------------------------------------ sifman / sifcmd (RPC servers) */

void hle_ok(IopCpu *c) { RET(KE_OK); }
void hle_one(IopCpu *c) { RET(1); }

static IopQueue *queue(IopRt *rt, uint32_t addr, int create)
{
    for (int i = 0; i < IOP_MAX_QUEUES; i++)
        if (rt->q[i].used && rt->q[i].addr == addr) return &rt->q[i];
    if (!create) return 0;
    for (int i = 0; i < IOP_MAX_QUEUES; i++)
        if (!rt->q[i].used) {
            memset(&rt->q[i], 0, sizeof rt->q[i]);
            rt->q[i].used = 1;
            rt->q[i].addr = addr;
            rt->q[i].thread = -1;
            return &rt->q[i];
        }
    return 0;
}

void hle_sceSifSetRpcQueue(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopQueue *q = queue(rt, A(0), 1);
    if (q) q->thread = thread_index(rt, A(1));
    RET(KE_OK);
}

void hle_sceSifRegisterRpc(IopCpu *c)
{
    IopRt *rt = c->rt;
    for (int i = 0; i < IOP_MAX_SERVERS; i++) {
        IopServer *s = &rt->srv[i];
        if (s->used) continue;
        s->used = 1;
        s->sd = A(0);
        s->sid = A(1);
        s->func = A(2);
        s->buf = A(3);
        s->cfunc = A(4);
        s->cbuf = A(5);
        s->queue = A(6);
        queue(rt, s->queue, 1);
        iop_rt_log(rt, 2, "IOP: RPC server 0x%08x registered (function 0x%08x, buffer 0x%08x)\n", s->sid, s->func,
                   s->buf);
        break;
    }
    RET(KE_OK);
}

static IopServer *server(IopRt *rt, uint32_t sid)
{
    for (int i = 0; i < IOP_MAX_SERVERS; i++)
        if (rt->srv[i].used && rt->srv[i].sid == sid) return &rt->srv[i];
    return 0;
}

static void serve(IopCpu *c, IopQueue *q)
{
    IopRt *rt = c->rt;
    IopRpcReq *r = &q->req;
    IopServer *s = server(rt, r->sid);
    if (s) {
        if (r->send && r->ssize) memmove(rt->mem + (s->buf & IOP_MEM_MASK), r->send, r->ssize);
        uint32_t save[32], hi = c->hi, lo = c->lo;
        memcpy(save, c->r, sizeof save);
        c->r[4] = r->fno;
        c->r[5] = s->buf;
        c->r[6] = r->ssize;
        c->r[31] = 0;
        iop_rt_call(c, s->func);
        uint32_t rep = c->r[2];
        memcpy(c->r, save, sizeof save);
        c->hi = hi;
        c->lo = lo;
        if (r->recv && r->rsize) {
            if (rep) memmove(r->recv, rt->mem + (rep & IOP_MEM_MASK), r->rsize);
            else memset(r->recv, 0, r->rsize);
        }
    }
    r->pending = 0;
    r->done = 1;
}

void hle_sceSifRpcLoop(IopCpu *c)
{
    IopRt *rt = c->rt;
    IopQueue *q = queue(rt, A(0), 1);
    if (!q || rt->cur < 0) { RET(KE_ERROR); return; }
    q->thread = rt->cur;
    for (;;) {
        if (!q->req.pending) {
            rt->th[rt->cur].rpc_queue = q->addr;
            iop_rt_wait(rt, W_RPC);
            continue;
        }
        serve(c, q);
    }
}

void hle_sceSifGetNextRequest(IopCpu *c)
{
    IopQueue *q = queue(c->rt, A(0), 0);
    RET(q && q->req.pending ? q->addr : 0);                   /* the queue address stands for the request */
}

void hle_sceSifExecRequest(IopCpu *c)
{
    IopQueue *q = queue(c->rt, A(0), 0);
    if (q && q->req.pending) serve(c, q);
    RET(KE_OK);
}

int iop_rt_sif_call(IopRt *rt, uint32_t sid, uint32_t fno, const void *send, uint32_t ssize, void *recv,
                    uint32_t rsize)
{
    IopServer *s = server(rt, sid);
    if (!s) return -1;
    IopQueue *q = queue(rt, s->queue, 0);
    if (!q) return -1;
    rt->st.sif_calls++;
    q->req.pending = 1;
    q->req.done = 0;
    q->req.sid = sid;
    q->req.fno = fno;
    q->req.send = send;
    q->req.ssize = ssize;
    q->req.recv = recv;
    q->req.rsize = rsize;
    if (q->thread >= 0) {
        IopThread *T = &rt->th[q->thread];
        if (T->state == TS_WAIT && T->wait == W_RPC) iop_rt_ready(rt, q->thread, KE_OK);
    }
    iop_rt_run(rt);
    /* the EE waits for the answer: while the server is not done and threads only wait for time (vblank, delay),
       IOP time passes (the vblank / virtual clock moves to the next event; the embedding's sound clock does not) */
    for (int guard = 0; !q->req.done && guard < 100000; guard++) {
        int vb = 0;
        uint64_t due = UINT64_MAX;
        for (int t = 0; t < IOP_MAX_THREADS; t++) {
            IopThread *T = &rt->th[t];
            if (T->state != TS_WAIT) continue;
            if (T->wait == W_VBLANK || T->wait == W_VBLANK_END) vb = 1;
            if (T->wait == W_DELAY && T->wait_until < due) due = T->wait_until;
        }
        if (vb) iop_rt_vblank(rt);
        else if (due != UINT64_MAX) iop_rt_advance_us(rt, due - rt->now_us);
        else break;
        iop_rt_run(rt);
    }
    if (!q->req.done) {
        iop_rt_log(rt, 0, "IOP: RPC 0x%08x fno %u was not answered (server thread busy or blocked)\n", sid, fno);
        q->req.pending = 0;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ loadcore */

void hle_RegisterLibraryEntries(IopCpu *c) { RET(KE_OK); }

/* ------------------------------------------------------------------ module loading */

int iop_rt_load(IopRt *rt, const IopModule *m, int argc, const char *const *argv, int prio)
{
    if (rt->nmods >= IOP_MAX_MODULES) return -1;
    if (m->base + m->image_size > rt->heap_hi || iop_rt_alloc(rt, m->image_size, 2, m->base) != (m->base & ~255u)) {
        iop_rt_log(rt, 0, "IOP: module %s does not fit at 0x%08x (0x%x bytes)\n", m->name, m->base, m->image_size);
        return -1;
    }
    memcpy(rt->mem + m->base, m->image, m->file_size);
    memset(rt->mem + m->base + m->file_size, 0, m->image_size - m->file_size);
    int unresolved = 0;
    for (uint32_t k = 0; k < m->nimports; k++) {
        const IopImportDesc *d = &m->imports[k];
        IopFn fn = 0;
        for (int b = rt->nbinds - 1; b >= 0 && !fn; b--)
            if (rt->binds[b].num == d->num && !strncmp(rt->binds[b].lib, d->lib, 8)) fn = rt->binds[b].fn;
        const char *nm = 0;
        if (!fn) fn = iop_rt_builtin(d->lib, d->num, &nm);
        if (!fn) {
            fn = hle_unimpl;
            unresolved++;
            iop_rt_log(rt, 0, "IOP: module %s imports %s #%u: not implemented (calls return 0)\n", m->name, d->lib,
                       d->num);
        }
        m->imp[k] = fn;
    }
    rt->st.imports_unresolved += (uint32_t)unresolved;
    rt->mods[rt->nmods++] = m;
    /* argv in IOP memory */
    uint32_t args = 0;
    if (argc > 0 && argv) {
        uint32_t need = 4u * (uint32_t)argc + 4u;
        for (int i = 0; i < argc; i++) need += (uint32_t)strlen(argv[i]) + 1u;
        args = iop_rt_alloc(rt, need, 0, 0);
        if (args) {
            uint32_t s = args + 4u * (uint32_t)argc + 4u;
            for (int i = 0; i < argc; i++) {
                wr32(rt, args + 4u * (uint32_t)i, s);
                size_t n = strlen(argv[i]) + 1u;
                memcpy(rt->mem + s, argv[i], n);
                s += (uint32_t)n;
            }
            wr32(rt, args + 4u * (uint32_t)argc, 0);
        }
    }
    /* module_start in a loader thread */
    int t;
    for (t = 0; t < IOP_MAX_THREADS && rt->th[t].state != TS_FREE; t++) {}
    if (t >= IOP_MAX_THREADS) return -1;
    IopThread *T = &rt->th[t];
    memset(T, 0, sizeof *T);
    T->entry = m->entry;
    T->stack_size = 0x1000;
    T->prio = T->init_prio = prio;
    T->gp = m->gp;
    T->stack = iop_rt_alloc(rt, T->stack_size, 1, 0);
    if (!T->stack) return -1;
    T->state = TS_DORMANT;
    T->arg = (uint32_t)argc;
    T->arg2 = args;
    iop_rt_ready(rt, t, 0);
    iop_rt_log(rt, 2, "IOP: loading %s at 0x%08x (0x%x bytes), %u imports (%d not implemented)\n", m->name, m->base,
               m->image_size, m->nimports, unresolved);
    iop_rt_run(rt);
    int r = (T->state == TS_DORMANT) ? (int)T->cpu.r[2] : -1;
    if (T->state == TS_DORMANT) {                             /* the loader thread is done: give its memory back */
        iop_rt_free(rt, T->stack);
        T->state = TS_FREE;
    }
    return r;
}
