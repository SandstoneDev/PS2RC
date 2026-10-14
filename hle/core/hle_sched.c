/* hle_sched.c - EE kernel model of the PoC-6 HLE runtime: guest threads as host coroutines (hle_plat_ctx_*: ucontext
 * on the host, a hand-written Allegrex context switch on the PSP - one OS thread runs all guest code either way), a
 * strict priority scheduler (EE kernel semantics: 0 = highest, FIFO per priority, no time slicing), semaphores,
 * INTC/DMAC interrupt handlers, alarms, and deterministic virtual time.
 *
 * Determinism: the guest never observes host time. Virtual time (hle_vtime, EE cycles) advances only
 *   - by fixed charges at "safe points" (syscalls, hooked functions, hardware-register accesses, COP0 Count reads),
 *   - and, when every guest thread is blocked, by jumping straight to the next event (VBlank, alarm). Before that jump
 *     the platform may pace (hle_plat_pace): the PSP waits until real time has reached the event, so the guest never
 *     runs ahead of the real clock while it idles (VBlank at the real 50 Hz PAL rate); the host jumps at once.
 * Interrupts (VBlank start/end, DMA completion, timer alarms) are raised by those time steps and delivered only at
 * safe points when the running context has interrupts enabled (and always from the idle loop). Handlers run on the
 * interrupted thread's host stack with a separate register context (hle_intr_ctx) and a guest interrupt stack.
 * After handlers ran, a thread they made ready preempts the running one if it has a higher priority.
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: zones (thread switches, interrupts, pacing) */
#include "hle_bootpace.h"                      /* x_bootpace (loading screens at real time) */

HleThread *hle_cur;
int hle_in_intr;
static R5900Ctx intr_ctx __attribute__((aligned(16)));
R5900Ctx *hle_intr_ctx = &intr_ctx;
static HleThread threads[HLE_MAX_THREADS];
HleSema hle_semas[HLE_MAX_SEMAS];
static uint32_t main_entry;

uint64_t hle_vtime;
uint64_t hle_frame;
int hle_vblank_hz = 50;
static uint64_t next_vblank_start, next_vblank_end;
uint32_t hle_intc_stat, hle_intc_mask, hle_dmac_stat;
static uint64_t idle_since_vtime;      /* vtime when the system last went idle with no thread running */
static uint64_t idle_jumps;
uint64_t hle_stat_intr, hle_stat_switches;
static int in_idle;

/* ------------------------------------------------------------------ ready queues */
static HleThread *rq_head[128], *rq_tail[128];

static void rq_remove(HleThread *t)
{
    int p = t->prio;
    HleThread **pp = &rq_head[p], *prev = NULL;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (!*pp) return;
    *pp = t->next;
    if (rq_tail[p] == t) rq_tail[p] = prev;
    t->next = NULL;
}
static void rq_push_tail(HleThread *t)
{
    int p = t->prio;
    t->next = NULL;
    if (rq_tail[p]) rq_tail[p]->next = t; else rq_head[p] = t;
    rq_tail[p] = t;
}
static void rq_push_head(HleThread *t)
{
    int p = t->prio;
    t->next = rq_head[p];
    rq_head[p] = t;
    if (!rq_tail[p]) rq_tail[p] = t;
}
static HleThread *rq_best(void)
{
    for (int p = 0; p < 128; p++)
        for (HleThread *t = rq_head[p]; t; t = t->next)
            if (!t->suspended) return t;
    return NULL;
}

HleThread *hle_thread(int id)
{
    if (id <= 0 || id >= HLE_MAX_THREADS || threads[id].status == TS_FREE) return NULL;
    return &threads[id];
}

HleThread *hle_thread_of(R5900Ctx *c)
{
    if (c == hle_intr_ctx) return NULL;
    return (HleThread *)c;
}

/* ------------------------------------------------------------------ host contexts */
static void thread_entry(void)
{
    HleThread *t = hle_cur;
    R5900Ctx *c = &t->c;
    VLOG("thread %d starts at %08x (arg %08x, sp %08x, gp %08x, prio %d)\n", t->id, t->entry, t->arg,
         (uint32_t)c->r[29].d[0], (uint32_t)c->r[28].d[0], t->prio);
    rc_dispatch(c, t->entry);
    VLOG("thread %d returned from its entry function (v0=%08x)\n", t->id, (uint32_t)c->r[2].d[0]);
    if (t->id == 1) {
        LOG("main thread returned from crt0 (exit code %d)\n", (int)c->r[2].d[0]);
        hle_exit(0);
    }
    hle_exit_thread();
}

static void host_stack_alloc(HleThread *t, size_t size)
{
    if (t->host_stack) return;
    void *p = hle_plat_stack_alloc(size);
    if (!p) { LOG("FATAL: cannot allocate a %u-byte host stack for guest thread %d\n", (unsigned)size, t->id);
              hle_set_exit_reason("out of memory for a guest thread stack"); hle_exit(4); }
    t->host_stack = p;
    t->host_stack_size = size;
}

static void switch_to(HleThread *n)
{
    HleThread *o = hle_cur;
    if (n == o) { n->status = TS_RUN; return; }
    if (!n->host_started) {
        hle_plat_ctx_make(&n->hc, n->host_stack, n->host_stack_size, thread_entry);
        n->host_started = 1;
    }
    n->status = TS_RUN;
    n->run_count++;
    hle_cur = n;
    hle_stat_switches++;
    VVLOG("switch %d -> %d\n", o ? o->id : -1, n->id);
    hle_pz_switch(n->id);
    hle_plat_ctx_swap(&o->hc, &n->hc);
    /* resumed: hle_cur == o again */
}

/* pick the next thread after the current one stopped running (blocked, exited, or yielded): idles until one is
   ready. The current thread's status was already updated by the caller. */
static void schedule_next(void)
{
    for (;;) {
        HleThread *n = rq_best();
        if (n) {
            rq_remove(n);
            switch_to(n);
            return;
        }
        in_idle = 1;
        hle_idle_advance();
        in_idle = 0;
    }
}

/* ------------------------------------------------------------------ thread API */
int hle_create_thread(uint32_t entry, uint32_t stack, uint32_t stack_size, uint32_t gp, int prio, uint32_t attr,
                      uint32_t option)
{
    for (int id = 2; id < HLE_MAX_THREADS; id++) {
        HleThread *t = &threads[id];
        if (t->status != TS_FREE) continue;
        void *hs = t->host_stack;
        size_t hss = t->host_stack_size;
        memset(t, 0, sizeof(*t));
        t->host_stack = hs;
        t->host_stack_size = hss;
        t->id = id;
        t->status = TS_DORMANT;
        t->entry = entry;
        t->stack = stack;
        t->stack_size = stack_size;
        t->gp = gp;
        t->prio = t->init_prio = prio & 127;
        t->attr = attr;
        t->option = option;
        t->c.ram = hle_ram;
        t->c.ram_mask = HLE_RAM_SIZE - 1;
        host_stack_alloc(t, HLE_STACK_THREAD);
        return id;
    }
    return -1;
}

int hle_start_thread(int id, uint32_t arg)
{
    HleThread *t = hle_thread(id);
    if (!t || t->status != TS_DORMANT) return -1;
    R5900Ctx *c = &t->c;
    memset(c->r, 0, sizeof(c->r));
    c->hi.d[0] = c->hi.d[1] = c->lo.d[0] = c->lo.d[1] = 0;
    uint32_t top = t->stack + t->stack_size;
    c->r[4].d[0] = arg;                                     /* a0 */
    c->r[28].d[0] = t->gp;                                  /* gp */
    c->r[29].d[0] = (top - 0x20) & ~15u;                    /* sp (Play!: stack top - STACK_FRAME_RESERVE_SIZE) */
    c->r[30].d[0] = c->r[29].d[0];                          /* fp */
    c->r[31].d[0] = 0;                                      /* ra: returning ends the thread (thread_entry) */
    c->fcr31 = 0;
    t->arg = arg;
    t->prio = t->init_prio;
    t->wakeup_count = 0;
    t->suspended = 0;
    t->intr_disabled = 0;
    t->host_started = 0;
    t->status = TS_READY;
    hle_pz_thread_reset(id);
    rq_push_tail(t);
    return id;
}

void hle_make_ready(HleThread *t)
{
    if (t->status == TS_READY || t->status == TS_RUN) return;
    t->status = TS_READY;
    t->wait_kind = WK_NONE;
    rq_push_tail(t);
}

void hle_reschedule(void)
{
    if (hle_in_intr || in_idle) return;
    HleThread *n = rq_best();
    if (!n || n->prio >= hle_cur->prio) return;
    HleThread *o = hle_cur;
    o->status = TS_READY;
    rq_push_head(o);                                        /* preempted: stays first of its priority */
    rq_remove(n);
    switch_to(n);
}

void hle_yield(void)
{
    if (hle_in_intr) return;
    HleThread *o = hle_cur;
    o->status = TS_READY;
    rq_push_tail(o);
    schedule_next();
}

void hle_block(int kind, int id)
{
    HleThread *t = hle_cur;
    if (hle_in_intr) { LOG("WARNING: blocking call (kind %d) from interrupt context ignored\n", kind); return; }
    t->status = TS_WAIT;
    t->wait_kind = kind;
    t->wait_id = id;
    schedule_next();
}

void hle_exit_thread(void)
{
    HleThread *t = hle_cur;
    VLOG("thread %d exits\n", t->id);
    hle_pz_thread_reset(t->id);
    t->status = TS_DORMANT;
    t->host_started = 0;
    schedule_next();
    LOG("FATAL: exited thread %d resumed\n", t->id);
    hle_exit(4);
}

int hle_change_priority(int id, int prio)
{
    HleThread *t = id == 0 ? hle_cur : hle_thread(id);
    if (!t) return -1;
    int old = t->prio;
    prio &= 127;
    if (t->status == TS_READY) { rq_remove(t); t->prio = prio; rq_push_tail(t); }
    else t->prio = prio;
    return old;
}

int hle_rotate_ready_queue(int prio)
{
    prio &= 127;
    if (!hle_in_intr && hle_cur->prio == prio) {
        hle_yield();
        return prio;
    }
    HleThread *h = rq_head[prio];
    if (h && h->next) { rq_remove(h); rq_push_tail(h); }
    return prio;
}

/* ------------------------------------------------------------------ semaphores */
int hle_sema_create(int init, int max, int attr, uint32_t option)
{
    for (int id = 1; id < HLE_MAX_SEMAS; id++) {
        HleSema *s = &hle_semas[id];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->count = init;
        s->init_count = init;
        s->max_count = max;
        s->attr = attr;
        s->option = option;
        return id;
    }
    return -1;
}

int hle_sema_delete(int id)
{
    if (id <= 0 || id >= HLE_MAX_SEMAS || !hle_semas[id].used) return -1;
    HleSema *s = &hle_semas[id];
    while (s->waiters) {
        HleThread *t = s->waiters;
        s->waiters = t->next;
        t->next = NULL;
        t->sema_result = -1;
        hle_make_ready(t);
    }
    s->used = 0;
    return id;
}

int hle_sema_signal(int id)
{
    if (id <= 0 || id >= HLE_MAX_SEMAS || !hle_semas[id].used) return -1;
    HleSema *s = &hle_semas[id];
    if (s->waiters) {
        HleThread *t = s->waiters;
        s->waiters = t->next;
        t->next = NULL;
        s->nwait--;
        t->sema_result = id;
        hle_make_ready(t);
    } else {
        s->count++;
    }
    return id;
}

int hle_sema_wait(int id)
{
    if (id <= 0 || id >= HLE_MAX_SEMAS || !hle_semas[id].used) return -1;
    HleSema *s = &hle_semas[id];
    if (s->count > 0) { s->count--; return id; }
    if (hle_in_intr) return -1;
    HleThread *t = hle_cur;
    t->next = NULL;
    HleThread **pp = &s->waiters;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    s->nwait++;
    t->sema_result = id;
    hle_block(WK_SEMA, id);
    return t->sema_result;
}

int hle_sema_poll(int id)
{
    if (id <= 0 || id >= HLE_MAX_SEMAS || !hle_semas[id].used) return -1;
    HleSema *s = &hle_semas[id];
    if (s->count > 0) { s->count--; return id; }
    return -1;
}

/* ------------------------------------------------------------------ interrupt handlers */
typedef struct { int used, cause, id; uint32_t fn, arg, gp; } IntHandler;
#define MAX_HANDLERS 64
static IntHandler intc_h[MAX_HANDLERS], dmac_h[MAX_HANDLERS];   /* list order = call order */
static int nintc, ndmac, next_hid = 1;

static int add_handler(IntHandler *h, int *n, int cause, uint32_t fn, int next, uint32_t arg, uint32_t gp)
{
    if (*n >= MAX_HANDLERS) return -1;
    IntHandler e = {1, cause, next_hid++, fn, arg, gp};
    /* EE kernel: next == 0 -> append after the existing handlers of this cause, next == -1 -> insert first */
    int pos = *n;
    if (next == -1) {
        for (int i = 0; i < *n; i++) if (h[i].cause == cause) { pos = i; break; }
    } else if (next > 0) {
        for (int i = 0; i < *n; i++) if (h[i].id == next) { pos = i; break; }
    }
    memmove(&h[pos + 1], &h[pos], (*n - pos) * sizeof(IntHandler));
    h[pos] = e;
    (*n)++;
    return e.id;
}
static int remove_handler(IntHandler *h, int *n, int cause, int id)
{
    for (int i = 0; i < *n; i++)
        if (h[i].id == id && h[i].cause == cause) {
            memmove(&h[i], &h[i + 1], (*n - i - 1) * sizeof(IntHandler));
            (*n)--;
            return 0;
        }
    return -1;
}
int hle_add_intc_handler(int cause, uint32_t fn, int next, uint32_t arg, uint32_t gp)
{ return add_handler(intc_h, &nintc, cause, fn, next, arg, gp); }
int hle_remove_intc_handler(int cause, int id) { return remove_handler(intc_h, &nintc, cause, id); }
int hle_add_dmac_handler(int ch, uint32_t fn, int next, uint32_t arg, uint32_t gp)
{ return add_handler(dmac_h, &ndmac, ch, fn, next, arg, gp); }
int hle_remove_dmac_handler(int ch, int id) { return remove_handler(dmac_h, &ndmac, ch, id); }

/* run one guest handler in interrupt context; returns v0 */
static uint32_t run_handler(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t gp)
{
    R5900Ctx *c = hle_intr_ctx;
    c->ram = hle_ram;
    c->ram_mask = HLE_RAM_SIZE - 1;
    c->r[4].d[0] = (uint64_t)(int64_t)(int32_t)a0;
    c->r[5].d[0] = (uint64_t)(int64_t)(int32_t)a1;
    c->r[6].d[0] = (uint64_t)(int64_t)(int32_t)a2;
    c->r[7].d[0] = 0;
    c->r[28].d[0] = (uint64_t)(int64_t)(int32_t)gp;
    c->r[29].d[0] = HLE_INTR_STACK_TOP - 0x40;
    c->r[31].d[0] = 0;
    hle_stat_intr++;
    rc_dispatch(c, fn);
    return (uint32_t)c->r[2].d[0];
}

/* ------------------------------------------------------------------ alarms */
typedef struct { int used, id, kind, thread; uint64_t when; uint32_t fn, arg, gp; int pending; } Alarm;
#define MAX_ALARMS 64
static Alarm alarms[MAX_ALARMS];
static int next_alarm_id = 1;
/* the number of used alarms with pending = 1 - pending_work() runs at every safe point
   (every MMIO access, syscall, back-edge period: ~1,000 per gameplay frame) and scanned all 64 alarms (3 KB) each time */
static int n_alarm_pending;
static uint64_t next_due;              /* earliest vblank / alarm time (0: recompute), see hle_time_charge */

int hle_alarm_add(uint64_t when, uint32_t fn, uint32_t arg, uint32_t gp, int kind, int thread_id)
{
    for (int i = 0; i < MAX_ALARMS; i++)
        if (!alarms[i].used) {
            alarms[i] = (Alarm){1, next_alarm_id++, kind, thread_id, when, fn, arg, gp, 0};
            next_due = 0;
            if (next_alarm_id > 0x7FFF) next_alarm_id = 1;
            return alarms[i].id;
        }
    LOG("WARNING: alarm table full\n");
    return -1;
}

int hle_alarm_cancel(int id)
{
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && alarms[i].id == id) {
            if (alarms[i].pending) { alarms[i].pending = 0; n_alarm_pending--; }
            alarms[i].used = 0;
            return 0;
        }
    return -1;
}

/* ------------------------------------------------------------------ time and events */
static uint64_t frame_cycles(void) { return EE_HZ / (uint64_t)hle_vblank_hz; }

void hle_frame_event(void)
{
    for (int i = 1; i < HLE_MAX_THREADS; i++) {
        HleThread *t = &threads[i];
        if (t->status == TS_WAIT && t->wait_kind == WK_VSYNC) hle_make_ready(t);
    }
    if (hle_frame % 250 == 0)
        LOG("frame %llu (vtime %.2f s, host %.1f s, switches %llu, syscalls %llu, main-loop iterations %llu)\n",
            (unsigned long long)hle_frame, (double)hle_vtime / EE_HZ, hle_host_seconds(),
            (unsigned long long)hle_stat_switches, (unsigned long long)hle_stat_syscalls,
            (unsigned long long)hle_mainloop_iters);
}

void hle_wait_vsync(void)
{
    if (hle_in_intr) return;
    hle_block(WK_VSYNC, 0);
}

static void process_due_events(void)
{
    while (hle_vtime >= next_vblank_start || hle_vtime >= next_vblank_end) {
        if (next_vblank_start <= next_vblank_end) {
            hle_frame++;
            hle_gs_csr |= 0x8;                              /* CSR.VSINT */
            hle_gs_csr ^= 0x2000;                           /* CSR.FIELD toggles every field */
            hle_intc_raise(2);
            hle_vsync_flag_update();
            hle_frame_event();
            next_vblank_start += frame_cycles();
        } else {
            hle_intc_raise(3);
            next_vblank_end += frame_cycles();
        }
    }
    for (int i = 0; i < MAX_ALARMS; i++) {
        Alarm *a = &alarms[i];
        if (!a->used || a->pending || hle_vtime < a->when) continue;
        if (a->kind == ALARM_THREAD_DELAY) {
            HleThread *t = hle_thread(a->thread);
            a->used = 0;
            if (t && t->status == TS_WAIT && t->wait_kind == WK_DELAY) hle_make_ready(t);
        } else {
            a->pending = 1;                                 /* guest callback: runs in interrupt context */
            n_alarm_pending++;
        }
    }
}

/* build 21: hle_time_charge runs at every syscall / MMIO access / hook (~10 M times per 3,000 main-loop iterations);
   process_due_events scans all 64 alarms each time. next_due caches the earliest vblank / alarm time; every place that
   adds or re-arms an alarm resets it to 0 (recompute at the next charge). */
static uint64_t next_event_time(void);
static void due_events(void)
{
    process_due_events();
    next_due = next_event_time();
}

int hle_rtpace_mode;                            /* set by the pack's real-time pacing (hle.h) */
uint32_t hle_rtpace_ctr, hle_rtpace_per = 64;

void hle_time_charge(uint64_t cycles)
{
    hle_vtime += cycles;
    if (hle_rtpace_mode == 2 && ++hle_rtpace_ctr >= hle_rtpace_per) { hle_rtpace_ctr = 0; hle_pack_rt_sync(); }
    if (hle_bootpace_on) hle_bootpace_charge(cycles);         /* x_bootpace (boot only) */
    if (hle_vtime >= next_due) due_events();
}

static uint64_t next_event_time(void)
{
    uint64_t t = next_vblank_start < next_vblank_end ? next_vblank_start : next_vblank_end;
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && !alarms[i].pending && alarms[i].when < t) t = alarms[i].when;
    return t;
}

static int interrupts_allowed(void)
{
    if (hle_in_intr) return 0;
    if (in_idle) return 1;
    return !hle_cur->intr_disabled;
}

static int pending_work(void)
{
    if (hle_intc_stat & hle_intc_mask) return 1;
    if ((hle_dmac_stat & 0x3FF) & (hle_dmac_stat >> 16)) return 1;
#ifndef HLE_PSP
    {   /* host: the counter must agree with the table (checked at every call; a mismatch is logged once) */
        int n = 0;
        static int warned;
        for (int i = 0; i < MAX_ALARMS; i++) n += alarms[i].used && alarms[i].pending;
        if (n != n_alarm_pending && !warned++) LOG("ALARM COUNT MISMATCH: table %d counter %d\n", n, n_alarm_pending);
    }
#endif
    return n_alarm_pending > 0;
}

/* a raised interrupt makes the NEXT loop back-edge a safe point (it used to wait up to RC_BACKEDGE_PERIOD
   back-edges). A renderer's DMA queue that kicks one packet per DMAC interrupt and spins on RAM counters in between
   otherwise spends ~1024 empty back-edges per packet (measured on a PSP-3000: more than half of the main loop's
   samples in that spin). Virtual time is still charged per back-edge actually executed (be_skipped). */
static uint32_t be_skipped;
static void backedge_soon(void)
{
    if (hle_backedge_count < RC_BACKEDGE_PERIOD - 1) {
        be_skipped += RC_BACKEDGE_PERIOD - 1 - hle_backedge_count;
        hle_backedge_count = RC_BACKEDGE_PERIOD - 1;
    }
}
void hle_intc_raise(int cause) { hle_intc_stat |= 1u << cause; backedge_soon(); }
void hle_dmac_raise(int ch) { hle_dmac_stat |= 1u << ch; backedge_soon(); }

static void run_interrupts(void)
{
    PZ_SCOPE(intr);
    hle_in_intr = 1;
    for (int guard = 0; guard < 16 && pending_work(); guard++) {
        uint32_t pend = hle_intc_stat & hle_intc_mask;
        for (int cause = 0; cause < 15; cause++) {
            if (!(pend & (1u << cause))) continue;
            hle_intc_stat &= ~(1u << cause);
            for (int i = 0; i < nintc; i++) {
                IntHandler h = intc_h[i];
                if (h.cause != cause) continue;
                VVLOG("INTC %d -> handler %08x\n", cause, h.fn);
                int32_t r = (int32_t)run_handler(h.fn, cause, h.arg, 0, h.gp);
                if (r < 0) break;
            }
        }
        uint32_t dpend = (hle_dmac_stat & 0x3FF) & (hle_dmac_stat >> 16);
        for (int ch = 9; ch >= 0; ch--) {
            if (!(dpend & (1u << ch))) continue;
            hle_dmac_stat &= ~(1u << ch);
            for (int i = 0; i < ndmac; i++) {
                IntHandler h = dmac_h[i];
                if (h.cause != ch) continue;
                VVLOG("DMAC %d -> handler %08x\n", ch, h.fn);
                int32_t r = (int32_t)run_handler(h.fn, ch, h.arg, 0, h.gp);
                if (r < 0) break;
            }
        }
        for (int i = 0; i < MAX_ALARMS; i++) {
            Alarm *a = &alarms[i];
            if (!a->used || !a->pending) continue;
            a->pending = 0;
            n_alarm_pending--;
            int id = a->id;
            /* libkernl 3.0 timer alarm handler (BUSCLK units):
               u64 handler(s32 id, u64 scheduled_time, u64 actual_time, void *arg, void *pc_value) */
            R5900Ctx *ic = hle_intr_ctx;
            ic->ram = hle_ram;
            ic->ram_mask = HLE_RAM_SIZE - 1;
            ic->r[4].d[0] = (uint64_t)id;
            ic->r[5].d[0] = a->when / 2;
            ic->r[6].d[0] = hle_vtime / 2;
            ic->r[7].d[0] = (uint64_t)(int64_t)(int32_t)a->arg;
            ic->r[8].d[0] = 0;
            ic->r[28].d[0] = (uint64_t)(int64_t)(int32_t)a->gp;
            ic->r[29].d[0] = HLE_INTR_STACK_TOP - 0x40;
            ic->r[31].d[0] = 0;
            VVLOG("ALARM %d -> handler %08x\n", id, a->fn);
            hle_stat_intr++;
            rc_dispatch(ic, a->fn);
            uint64_t again = ic->r[2].d[0];
            if (a->used && a->id == id) {
                if (again) { a->when = hle_vtime + 2 * again; next_due = 0; }   /* re-arm (handler returned a new delay) */
                else a->used = 0;
            }
        }
    }
    hle_in_intr = 0;
}

void hle_deliver_interrupts(void)
{
    if (!interrupts_allowed() || !pending_work()) return;
    run_interrupts();
    hle_reschedule();
}

/* loop back-edges (RC_BACKEDGE in the harness runtime header): every RC_BACKEDGE_PERIOD backward branches of guest
   code are a safe point that also accounts the work as virtual time (8 EE cycles per back-edge, a rough loop-body
   cost) - deterministic, and it lets RAM polling loops observe interrupts (VBlank counters, DMA flags). */
uint32_t hle_backedge_count;
uint64_t hle_stat_backedges;
volatile uint32_t hle_where;            /* guest address of the last loop back-edge (RC_BACKEDGE_AT) */
/* known pure wait loops (the pack's [runtime] wait_loops) - they only re-read RAM that interrupt handlers / other
   threads change. When two consecutive back-edge periods end in one of them, virtual time jumps to the next scheduled
   event (as the idle step does) instead of being ground out 8 EE cycles per back-edge: a PSP back-edge costs more
   real time than the 8 EE cycles it stands for, so spinning made every guest wait longer than on the PS2 (e.g. a
   renderer waiting for its DMA queue / the previous frame's flip). */
int hle_opt_idle_skip = 1;
uint64_t hle_stat_idle_skips, hle_stat_idle_skipped_cycles;
static uint32_t last_slow_where;
static int is_wait_loop(uint32_t a) { return HLE_PACK_IS_WAIT_LOOP(a); }   /* the pack's [runtime] wait_loops */

void hle_backedge_slow(R5900Ctx *c)
{
    uint32_t done = RC_BACKEDGE_PERIOD - be_skipped;    /* back-edges really executed since the last slow path */
    be_skipped = 0;
    hle_backedge_count = 0;
    hle_stat_backedges += done;
    if (!c || hle_in_intr) { hle_time_charge(8ull * done); return; }
    hle_safe_point(c, 8ull * done);
    uint32_t w = hle_where;
    int skip = hle_opt_idle_skip && w == last_slow_where && is_wait_loop(w);
    last_slow_where = w;
    if (!skip) return;
    uint64_t t = next_event_time();
    if (t > hle_vtime) {
        { PZ_SCOPE(pace); hle_bootpace_pace(t); if (hle_pack_pace) hle_pack_pace(t); hle_plat_pace(t); }   /* never ahead of real time (PAL frame pacing) */
        hle_stat_idle_skipped_cycles += t - hle_vtime;
        hle_vtime = t;
    }
    hle_stat_idle_skips++;
    due_events();
    hle_check_limits();
    hle_deliver_interrupts();
}

void hle_safe_point(R5900Ctx *c, uint64_t cost)
{
    (void)c;
    hle_time_charge(cost);
    hle_check_limits();
    hle_deliver_interrupts();
}

void hle_idle_advance(void)
{
    uint64_t t = next_event_time();
    if (t > hle_vtime) {
        { PZ_SCOPE(pace); hle_bootpace_pace(t); if (hle_pack_pace) hle_pack_pace(t); hle_plat_pace(t); }   /* PSP: real time catches up with the next event */
        hle_vtime = t;
    }
    idle_jumps++;
    due_events();
    hle_check_limits();
    if (pending_work()) run_interrupts();
    if (!rq_best()) {
        if (hle_vtime - idle_since_vtime > (uint64_t)120 * EE_HZ && idle_since_vtime) {
            LOG("DEADLOCK: no guest thread became ready during 120 s of virtual time\n");
            hle_exit(5);
        }
        if (!idle_since_vtime) idle_since_vtime = hle_vtime;
    } else {
        idle_since_vtime = 0;
    }
}

/* ------------------------------------------------------------------ boot */
void hle_sched_init(uint32_t entry)
{
    main_entry = entry;
    HleThread *t = &threads[1];
    memset(t, 0, sizeof(*t));
    t->id = 1;
    t->status = TS_RUN;
    t->prio = t->init_prio = 0;
    t->entry = entry;
    t->intr_disabled = 1;                                   /* crt0 runs EI itself after kernel setup */
    t->c.ram = hle_ram;
    t->c.ram_mask = HLE_RAM_SIZE - 1;
    host_stack_alloc(t, HLE_STACK_MAIN);
    intr_ctx.ram = hle_ram;
    intr_ctx.ram_mask = HLE_RAM_SIZE - 1;
    next_vblank_start = frame_cycles();
    next_vblank_end = next_vblank_start + frame_cycles() / 12;
    next_due = 0;
}

static hle_hostctx boot_hc;
void hle_sched_run(void)
{
    HleThread *t = &threads[1];
    hle_plat_ctx_make(&t->hc, t->host_stack, t->host_stack_size, thread_entry);
    t->host_started = 1;
    hle_cur = t;
    hle_pz_switch(1);
    hle_plat_ctx_swap(&boot_hc, &t->hc);
}

/* host stack high-water marks (platforms that paint their stacks; 0 = unknown) */
void hle_stack_report(void)
{
    LOG("host stacks (guest threads):");
    for (int i = 1; i < HLE_MAX_THREADS; i++) {
        HleThread *t = &threads[i];
        if (!t->host_stack) continue;
        size_t used = hle_plat_stack_used(t->host_stack, t->host_stack_size);
        LOG(" t%d %u/%u KB", i, (unsigned)((used + 1023) >> 10), (unsigned)(t->host_stack_size >> 10));
    }
    LOG("\n");
}

/* ------------------------------------------------------------------ diagnostics */
void hle_sched_dump(void)
{
    static const char *st[] = {"free", "dormant", "ready", "run", "wait"};
    static const char *wk[] = {"-", "sleep", "sema", "delay", "vsync"};
    LOG("threads (current %d, in_intr %d):\n", hle_cur ? hle_cur->id : -1, hle_in_intr);
    for (int i = 1; i < HLE_MAX_THREADS; i++) {
        HleThread *t = &threads[i];
        if (t->status == TS_FREE) continue;
        LOG("  thread %2d %-7s %-5s(%d) prio %3d entry %08x stack %08x+%x runs %llu pc-ra %08x sp %08x%s\n", t->id,
            st[t->status], wk[t->wait_kind], t->wait_id, t->prio, t->entry, t->stack, t->stack_size,
            (unsigned long long)t->run_count, (uint32_t)t->c.r[31].d[0], (uint32_t)t->c.r[29].d[0],
            t->suspended ? " SUSPENDED" : "");
    }
    for (int i = 1; i < HLE_MAX_SEMAS; i++) {
        HleSema *s = &hle_semas[i];
        if (!s->used || !s->waiters) continue;
        LOG("  sema %d count %d max %d waiters:", i, s->count, s->max_count);
        for (HleThread *t = s->waiters; t; t = t->next) LOG(" %d", t->id);
        LOG("\n");
    }
    LOG("  INTC stat %08x mask %08x  D_STAT %08x  handlers intc %d dmac %d\n", hle_intc_stat, hle_intc_mask,
        hle_dmac_stat, nintc, ndmac);
    for (int i = 0; i < nintc; i++) LOG("    intc cause %d -> %08x (arg %08x)\n", intc_h[i].cause, intc_h[i].fn, intc_h[i].arg);
    for (int i = 0; i < ndmac; i++) LOG("    dmac ch %d -> %08x (arg %08x)\n", dmac_h[i].cause, dmac_h[i].fn, dmac_h[i].arg);
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used) LOG("    alarm %d kind %d at %llu fn %08x thread %d\n", alarms[i].id, alarms[i].kind,
                                (unsigned long long)alarms[i].when, alarms[i].fn, alarms[i].thread);
    LOG("  vtime %llu cycles (%.3f s), frames %llu, idle jumps %llu\n", (unsigned long long)hle_vtime,
        (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame, (unsigned long long)idle_jumps);
}
