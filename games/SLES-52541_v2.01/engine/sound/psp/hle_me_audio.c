/* hle_me_audio.c - the audio worker on the Media Engine: see hle_me_audio.h.
 *
 * The ME runs its OWN copies of the SPU2-lite engine and of the output stretcher: spu2_lite.c and audio_stretch.c are
 * compiled a second time into this unit under zme_* names, with the code-generation rules the ME needs - no calls the
 * compiler invents (loop distribution into memset/memcpy is off) and no jump tables (a switch must not jump through a
 * table of user-segment addresses: ME code runs at its cached-kernel alias 0x8xxxxxxx and every branch must stay
 * there; J/JAL keep the upper address bits, table jumps would not). A build-time check of the linked ELF verifies
 * that every call reachable from zma_work stays inside zme_* / zma_* / libgcc arithmetic. Same C, same integer math:
 * the ME's samples equal the main core's bit for bit (shadow mode proves it on the hardware).
 */
#if defined(HLE_PSP) && defined(HLE_HAVE_AUDIO_MODEL)
#pragma GCC optimize("no-tree-loop-distribute-patterns", "no-jump-tables")

/* ------------------------------------------------------------------ 1. the ME's private engine + stretcher */
#define spu2_adpcm_block zme_spu2_adpcm_block
#define spu2_vol_fixed   zme_spu2_vol_fixed
#define spu2_env_tick    zme_spu2_env_tick
#define spu2_vol_tick    zme_spu2_vol_tick
#define spu2_init        zme_spu2_init
#define spu2_set_param   zme_spu2_set_param
#define spu2_set_addr    zme_spu2_set_addr
#define spu2_get_envx    zme_spu2_get_envx
#define spu2_get_nax     zme_spu2_get_nax
#define spu2_key_on      zme_spu2_key_on
#define spu2_key_off     zme_spu2_key_off
#define spu2_set_position zme_spu2_set_position
#define spu2_set_irq     zme_spu2_set_irq
#define spu2_irq_take    zme_spu2_irq_take
#define spu2_sounding    zme_spu2_sounding
#define spu2_render48    zme_spu2_render48
#define spu2_advance     zme_spu2_advance
#define spu2_render      zme_spu2_render
#define spu2_bind_ops    zme_spu2_bind_ops
#define as_resample48    zme_as_resample48
#define as_ring_init     zme_as_ring_init
#define as_ring_write    zme_as_ring_write
#define as_init          zme_as_init
#define as_hop           zme_as_hop
#define as_fill          zme_as_fill
#include "spu2_lite.c"
#define clamp16 as_clamp16                         /* both files have a static clamp16 */
#include "audio_stretch.c"
#undef clamp16
#include "spu2_split.h"                            /* spu2_cmd_apply, here on the zme_* engine */

static inline uint32_t zme_apply(Spu2 *b, uint32_t w0, uint32_t w1, int16_t *out48)
{
    return spu2_cmd_apply(b, w0, w1, out48);
}
static inline int zme_hop(AudioStretch *st, int16_t *out, uint32_t elapsed) { return as_hop(st, out, elapsed); }
static inline uint32_t zme_resample(AsResampler *rs, const int16_t *in, uint32_t n, int16_t *out)
{
    return as_resample48(rs, in, n, out);
}
static inline uint32_t zme_ring_write(AsRing *r, const int16_t *s, uint32_t n) { return as_ring_write(r, s, n); }

#undef spu2_adpcm_block
#undef spu2_vol_fixed
#undef spu2_env_tick
#undef spu2_vol_tick
#undef spu2_init
#undef spu2_set_param
#undef spu2_set_addr
#undef spu2_get_envx
#undef spu2_get_nax
#undef spu2_key_on
#undef spu2_key_off
#undef spu2_set_position
#undef spu2_set_irq
#undef spu2_irq_take
#undef spu2_sounding
#undef spu2_render48
#undef spu2_advance
#undef spu2_render
#undef spu2_bind_ops
#undef as_resample48
#undef as_ring_init
#undef as_ring_write
#undef as_init
#undef as_hop
#undef as_fill

#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "hle_me_audio.h"

_Static_assert(sizeof(AsStats) <= 32u * 4u, "ZmaCtl.st_words too small");
_Static_assert((sizeof(ZmaCtl) & 3u) == 0, "ZmaCtl");

/* ------------------------------------------------------------------ 2. the worker (Media Engine)
 * One loop body, two entries: zma_work (the ME: cache maintenance, CP0 Count, HALT at the end) and zma_work_sim (a
 * main-core THREAD for the emulator - audio_me_sim=1: PPSSPP has no ME, but the whole hand-over / ring / hop / shadow
 * protocol of the game can run there; no cache ops, no CP0, yields while idle, may simulate a hang). `sim` is a
 * compile-time constant in each copy (always_inline), so the ME's copy contains none of the simulation code. */
void zma_sim_idle(void);                                   /* main-core helpers of the simulation (below) */
uint32_t zma_sim_count(void);
/* (hle_me_jobs.h) ONE unit of ONE job - round-robin from ZmaJobs.rr over the enabled slots, the first that has
   work (its poll returns 1) is measured and ends the step. The only indirect call the ME makes (allowed by the build-time check): the
   job's plain address with the kernel-cached alias, like the work function itself (hle_me.c). 1 = a unit ran. */
static inline __attribute__((always_inline)) int zma_job_step(volatile ZmaJobs *j, const int sim)
{
    uint32_t n = j->n;
    if (n == 0) return 0;
    if (n > HLE_ME_JOBS) n = HLE_ME_JOBS;
    uint32_t s = j->rr;
    for (uint32_t k = 0; k < n; k++, s++) {
        if (s >= n) s = 0;
        volatile ZmaJobSlot *js = &j->s[s];
        uint32_t fn = js->fn;
        if (!fn || !js->on) continue;
        int (*poll)(int) = (int (*)(int))(uintptr_t)(sim ? fn : (fn | 0x80000000u));
        uint32_t t0 = sim ? zma_sim_count() : hle_me_count();
        if (!poll(sim)) continue;
        uint32_t dt = (sim ? zma_sim_count() : hle_me_count()) - t0;
        uint32_t lo = js->busy_lo + dt;
        if (lo < dt) js->busy_hi = js->busy_hi + 1u;
        js->busy_lo = lo;
        js->units = js->units + 1u;
        if (dt > js->max) js->max = dt;
        uint32_t lim = sim ? 1000u : j->over_counts;              /* the simulation counts microseconds */
        if (lim && dt > lim) js->over = js->over + 1u;
        j->rr = s + 1u >= n ? 0u : s + 1u;
        return 1;
    }
    return 0;
}

static inline __attribute__((always_inline)) void zma_loop(volatile ZmaCtl *c, const int sim)
{
    ZmaRegion *r = (ZmaRegion *)(uintptr_t)c->region;
    volatile uint32_t *cmds = (volatile uint32_t *)(uintptr_t)c->cmds;
    uint32_t rd = c->cmd_rd, hop_done = c->hop_done, mode = c->mode;
    uint32_t sh_h = c->sh_h, sh_n = c->sh_n;
    c->h.boots = c->h.boots + 1u;
    if (!sim) __asm__ volatile("mtc0 $zero, $9\n\tsync");   /* CP0 Count read a constant on the PSP-3000's ME
                                                                (measured): public ME code writes it before use */
    c->h.state = 1;
    for (;;) {
        uint32_t t0 = sim ? zma_sim_count() : hle_me_count();
        c->h.count = t0;
        c->h.heartbeat = c->h.heartbeat + 1u;
        if (c->h.exit_req) break;
        if (sim && c->sim_hang) { zma_sim_idle(); continue; }     /* simulated dead ME: no progress at all */
        if (mode == ZMA_LIVE && c->hop_req != hop_done) {        /* real-time work first: one output hop */
            uint32_t req = c->hop_req, el = c->hop_elapsed, bi = c->hop_buf % ZMA_HOPS;
            int snd = zme_hop(&r->st, r->hop[bi], el);
            if (!sim) hle_me_dwb(r->hop[bi], sizeof r->hop[bi]);   /* the samples to RAM (sceAudio's DMA reads RAM) */
            const uint32_t *sw = (const uint32_t *)(const void *)&r->st.st;
            for (uint32_t i = 0; i < sizeof(AsStats) / 4u; i++) c->st_words[i] = sw[i];
            c->hop_snd = (uint32_t)snd;
            c->hops = c->hops + 1u;
            c->busy_hop = c->busy_hop + ((sim ? zma_sim_count() : hle_me_count()) - t0);
            hle_me_sync();
            c->hop_done = req;
            hop_done = req;
            continue;
        }
        if (rd == c->cmd_wr) {                                    /* idle: back off ~90 us between polls - every */
            volatile ZmaJobs *jt = (volatile ZmaJobs *)(uintptr_t)c->jobs;   /* one job unit, then the */
            if (jt && zma_job_step(jt, sim)) continue;            /* exit / hop / command checks again */
            if (sim) { zma_sim_idle(); continue; }                /* poll is uncached bus traffic the (memory-bound) */
            uint32_t nops = jt ? jt->idle_nops : 0u;              /* main core would pay for; the jobs */
            if (!nops) nops = 8192u;                              /* table may ask for a longer back-off (0 = 8192) */
            for (uint32_t i = 0; i < nops; i++) __asm__ volatile("nop");
            continue;
        }
        uint32_t k = rd & (ZMA_CMDS - 1u);
        uint32_t w0 = cmds[2u * k], w1 = cmds[2u * k + 1u];
        uint32_t op = SPU2C_OP(w0);
        if (op == SPU2C_RAMW) {                                   /* the main core rewrote SPU RAM [w1, w1+size) */
            uint32_t size = w0 >> 8;
            if (sim) { }
            else if (size <= 16384u) hle_me_dinval(r->b.ram + (w1 & (SPU2_RAM_SIZE - 1u)), size);
            else { hle_me_dwbinv_all(); c->ramw_all = c->ramw_all + 1u; }
            c->ramw = c->ramw + 1u;
        } else if (op >= SPU2C_PARAM && op <= SPU2C_RENDER) {
            uint32_t n = zme_apply(&r->b, w0, w1, r->out48);
            if (n) {
                if (mode == ZMA_SHADOW) {
                    sh_h = zma_hash(sh_h, r->out48, n);
                    sh_n++;
                    c->sh_h = sh_h;
                    hle_me_sync();
                    c->sh_n = sh_n;
                } else {                                          /* 48 -> 44.1 kHz into the output ring */
                    const int16_t *s = r->out48;
                    uint32_t left = n;
                    while (left) {
                        uint32_t m = left < 64u ? left : 64u;
                        uint32_t o = zme_resample(&r->rs, s, m, r->tmp44);
                        if (o) { zme_ring_write(&r->ring, r->tmp44, o); c->produced44 = c->produced44 + o; }
                        s += 2u * m;
                        left -= m;
                    }
                    c->ring_lost = r->ring.lost;
                }
                c->renders = c->renders + 1u;
                c->frames48 = c->frames48 + n;
                c->busy_render = c->busy_render + ((sim ? zma_sim_count() : hle_me_count()) - t0);
            } else {
                c->busy_cmd = c->busy_cmd + ((sim ? zma_sim_count() : hle_me_count()) - t0);
            }
        } else if (op != SPU2C_NOP && op != SPU2C_SYNC) {
            c->bad_cmds = c->bad_cmds + 1u;
            c->h.fault = 1u;
            c->h.fault_arg = w0;
        }
        rd++;
        c->cmd_rd = rd;
    }
}

void zma_work(volatile HleMeHdr *hdr)
{
    zma_loop((volatile ZmaCtl *)hdr, 0);
    hle_me_dwbinv_all();                                          /* nothing of ours stays dirty in the ME's cache */
    hdr->state = 2;
    hle_me_halt();
}

void zma_work_sim(volatile HleMeHdr *hdr)                         /* main-core thread body (emulator test) */
{
    zma_loop((volatile ZmaCtl *)hdr, 1);
    hdr->state = 2;
}

/* ------------------------------------------------------------------ 3. main-core side */
static uint32_t us_now(void) { return sceKernelGetSystemTimeLow(); }
void zma_sim_idle(void) { sceKernelDelayThread(500); }
uint32_t zma_sim_count(void) { return sceKernelGetSystemTimeLow(); }   /* the simulation counts microseconds */

void zma_host_init(ZmaHost *z, volatile ZmaCtl *c, volatile uint32_t *cmds, ZmaRegion *r, int mode)
{
    uint32_t keep_wr = 0;
    (void)keep_wr;
    z->c = c;
    z->cmds = cmds;
    z->r = r;
    z->wr = 0;
    if (!z->stall_ms) z->stall_ms = 500;
    z->epoch = hle_me_resume_epoch();
    volatile uint32_t *w = (volatile uint32_t *)c;
    for (uint32_t i = 0; i < sizeof(ZmaCtl) / 4u; i++) w[i] = 0;
    c->mode = (uint32_t)mode;
    c->region = (uint32_t)(uintptr_t)r;
    c->cmds = (uint32_t)(uintptr_t)cmds;
    c->sh_h = ZMA_HASH0;
}

/* wait until cond() holds; 0 = no progress of cmd_rd / hop_done and no heartbeat for stall_ms */
static int wait_progress(ZmaHost *z, int which, uint32_t target, uint64_t *acc)
{
    volatile ZmaCtl *c = z->c;
    uint32_t t0 = us_now(), last_rd = c->cmd_rd, last_hb = c->h.heartbeat, t_prog = t0, ep = *z->epoch;
    for (int spin = 0;; spin++) {
        if (*z->epoch != ep) {                                      /* the PSP slept in this wait: fresh deadlines */
            ep = *z->epoch;
            t0 = t_prog = us_now();
            last_hb = c->h.heartbeat;
        }
        int done = which == 0 ? (z->wr - c->cmd_rd) < ZMA_CMDS     /* room in the ring */
                              : c->cmd_rd == target;                /* barrier: everything consumed */
        if (done) break;
        uint32_t now = us_now();
        if (c->cmd_rd != last_rd) { last_rd = c->cmd_rd; t_prog = now; }
        if (now - t_prog > (uint32_t)z->stall_ms * 1000u) return 0;
        if ((spin & 15) == 15) {                                    /* the ME's loop must keep counting */
            uint32_t hb = c->h.heartbeat;
            if (hb == last_hb && now - t0 > 100000u) return 0;
            last_hb = hb;
        }
        if (spin >= 8) sceKernelDelayThread(100);
    }
    uint32_t dt = us_now() - t0;
    z->waits_us += dt;
    *acc += dt;
    if (dt > z->wait_max_us) z->wait_max_us = dt;
    return 1;
}

int zma_push(ZmaHost *z, uint32_t w0, uint32_t w1)
{
    if (z->wr - z->c->cmd_rd >= ZMA_CMDS) {
        z->fulls++;
        if (!wait_progress(z, 0, 0, &z->full_us)) return 0;
    }
    uint32_t k = z->wr & (ZMA_CMDS - 1u);
    z->cmds[2u * k] = w0;
    z->cmds[2u * k + 1u] = w1;
    __asm__ volatile("sync" ::: "memory");                       /* the entry before the index (uncached, in order) */
    z->c->cmd_wr = ++z->wr;
    return 1;
}

int zma_barrier(ZmaHost *z)
{
    if (z->c->cmd_rd == z->wr) return 1;
    z->barriers++;
    return wait_progress(z, 1, z->wr, &z->barrier_us);
}

int zma_hop(ZmaHost *z, uint32_t elapsed, uint32_t buf, uint32_t timeout_us, uint32_t *waited_us)
{
    volatile ZmaCtl *c = z->c;
    uint32_t req = c->hop_req + 1u, t0 = us_now(), ep = *z->epoch;
    c->hop_elapsed = elapsed;
    c->hop_buf = buf;
    __asm__ volatile("sync" ::: "memory");
    c->hop_req = req;
    for (int spin = 0; c->hop_done != req; spin++) {
        if (*z->epoch != ep) { ep = *z->epoch; t0 = us_now(); }   /* a sleep in this wait: a fresh deadline */
        if (us_now() - t0 > timeout_us) { *waited_us = us_now() - t0; return -1; }
        if (spin >= 4) sceKernelDelayThread(100);
    }
    *waited_us = us_now() - t0;
    return (int)c->hop_snd;
}

/* ------------------------------------------------------------------ 4. the job interface (hle_me_jobs.h) */
/* the 3D builder (hle_w3d_me.c). Weak: NULL in a program without it (the heartbeat). Registered as a job the first
   time a loop binds the table, so the builder needs no change for the audio loop; it returns 0
   at once while the game's w3d_me is off (or its own runner owns the stream). */
int w3dme_poll(int sim) __attribute__((weak));

#define UNC_ADDR(p) ((uintptr_t)(p) | 0x40000000u)
static ZmaJobs g_jobs_mem __attribute__((aligned(64)));     /* accessed through the uncached alias only */
static volatile ZmaJobs *g_jobs;
static const char *g_job_name[HLE_ME_JOBS];
static volatile ZmaCtl *g_poll;                             /* the control block of the loop that polls the table */
static int g_poll_sim;
static struct {                                             /* the jobs-only loop (ZMA_JOBS) */
    volatile ZmaCtl *ctl;                                   /* uncached */
    uint8_t *region;                                        /* its stack (nothing else: no audio state) */
    int want, sim, running;
    SceUID th;
    uint32_t starts, fails;
    char why[120];
} J = {0, 0, 0, 0, 0, -1, 0, 0, ""};
static struct { volatile HleMeHdr *h; uint32_t boots, count, t_us; double mhz; } g_cal;   /* Count calibration */
static int g_jobs_sleep = 1;                                /* the game's audio_me_sleep (zma_jobs_sleep_allowed) */
void zma_jobs_sleep_allowed(int on) { g_jobs_sleep = on; }

static void jobs_init(void)
{
    if (g_jobs) return;
    sceKernelDcacheWritebackInvalidateRange(&g_jobs_mem, sizeof g_jobs_mem);
    g_jobs = (volatile ZmaJobs *)UNC_ADDR(&g_jobs_mem);
    volatile uint32_t *w = (volatile uint32_t *)g_jobs;
    for (uint32_t i = 0; i < sizeof(ZmaJobs) / 4u; i++) w[i] = 0;
    if (w3dme_poll) hle_me_job_add(w3dme_poll, "w3d");
}

int hle_me_job_add(HleMeJobFn poll, const char *name)
{
    jobs_init();
    if (!poll) return -1;
    uint32_t n = g_jobs->n;
    for (uint32_t i = 0; i < n; i++)
        if (g_jobs->s[i].fn == (uint32_t)(uintptr_t)poll) return (int)i;
    if (n >= HLE_ME_JOBS) return -1;
    volatile ZmaJobSlot *js = &g_jobs->s[n];
    js->units = js->busy_lo = js->busy_hi = js->max = js->over = 0;
    js->on = 1;
    js->fn = (uint32_t)(uintptr_t)poll;
    g_job_name[n] = name ? name : "job";
    __asm__ volatile("sync" ::: "memory");
    g_jobs->n = n + 1u;                                     /* last: the slot is complete */
    return (int)n;
}

void hle_me_job_enable(int slot, int on)
{
    jobs_init();
    if (slot >= 0 && slot < (int)g_jobs->n) g_jobs->s[slot].on = on ? 1u : 0u;
}

int zma_jobs_count(void) { return g_jobs ? (int)g_jobs->n : 0; }

/* the idle back-off of every loop that polls the table (0 = the default 8192 NOPs,
   ~75 us at 333 MHz; capped at 65536). Read by the ME once per idle round (uncached). */
void hle_me_jobs_idle(uint32_t nops)
{
    jobs_init();
    g_jobs->idle_nops = nops > 65536u ? 65536u : nops;
}

static void cal_reset(volatile HleMeHdr *h)
{
    g_cal.h = h;
    g_cal.boots = h ? h->boots : 0;
    g_cal.count = h ? h->count : 0;
    g_cal.t_us = us_now();
}

/* the ME's Count rate from a 0.2 .. 5 s window of the polling loop (Count wraps in ~9.5 s at 454 MHz) */
void zma_jobs_tick(void)
{
    volatile HleMeHdr *h = g_poll ? &g_poll->h : 0;
    if (h != g_cal.h || !h) { cal_reset(h); return; }
    uint32_t now = us_now(), dt = now - g_cal.t_us;
    if (dt < 200000u) return;
    uint32_t b = h->boots, cnt = h->count;
    if (b == g_cal.boots && dt <= 5000000u && h->state == 1) {
        double mhz = (double)(cnt - g_cal.count) / (double)dt;
        if (mhz >= 1.0 || g_poll_sim) {                     /* (the simulation counts microseconds: 1.0) */
            g_cal.mhz = mhz;
            if (g_jobs && !g_poll_sim) g_jobs->over_counts = (uint32_t)(mhz * 1000.0);
        }
    }
    g_cal.boots = b;
    g_cal.count = cnt;
    g_cal.t_us = now;
}

double hle_me_count_mhz(void)
{
    zma_jobs_tick();
    return g_cal.mhz;
}

int hle_me_jobs_polled(void)
{
    volatile ZmaCtl *c = g_poll;
    if (!c) return 0;
    if (g_poll_sim) return c->h.state == 1;
    return hle_me_owner() == &c->h;
}

void zma_jobs_bind(volatile ZmaCtl *c, int sim)
{
    jobs_init();
    c->jobs = (uint32_t)(uintptr_t)g_jobs;
    g_poll = c;
    g_poll_sim = sim;
    cal_reset(&c->h);
}

static int jobs_sim_thread(SceSize n, void *a) { (void)n; (void)a; zma_work_sim(&J.ctl->h); return 0; }

static int jobs_start(char *why, int why_n)
{
    if (!J.ctl) {
        void *c = memalign(64, 256);
        J.region = (uint8_t *)memalign(64, 32768);
        if (!c || !J.region) { snprintf(why, (size_t)why_n, "no memory"); J.fails++; return 0; }
        memset(c, 0, 256);
        sceKernelDcacheWritebackInvalidateRange(c, 256);
        J.ctl = (volatile ZmaCtl *)UNC_ADDR(c);
    }
    volatile uint32_t *w = (volatile uint32_t *)J.ctl;
    for (uint32_t i = 0; i < sizeof(ZmaCtl) / 4u; i++) w[i] = 0;
    J.ctl->mode = ZMA_JOBS;
    J.ctl->h.magic = HLE_ME_MAGIC;
    zma_jobs_bind(J.ctl, J.sim);
    int ok;
    if (J.sim) {
        J.th = sceKernelCreateThread("zme_jobs", jobs_sim_thread, 0x18, 0x8000, THREAD_ATTR_USER, NULL);
        ok = J.th >= 0 && sceKernelStartThread(J.th, 0, NULL) >= 0;
        for (int i = 0; ok && i < 100 && J.ctl->h.state != 1; i++) sceKernelDelayThread(1000);
        ok = ok && J.ctl->h.state == 1;
        if (!ok) snprintf(why, (size_t)why_n, "simulation thread failed (%08x)", (unsigned)J.th);
    } else {
        hle_me_power_init();                                /* idempotent: the power callback (hle_me.h) */
        if (g_jobs_sleep && !hle_me_sleep_hooked()) {       /* as the audio does (audio_me_sleep): the PRX's hook, */
            char w[200];                                    /* or scePowerLock in hle_me_start when it is refused */
            hle_me_sleep_enable(w, sizeof w);
        }
        ok = hle_me_start(zma_work, &J.ctl->h, J.region, 32768, ZMA_STACK, why, why_n);
    }
    if (!ok) {
        if (g_poll == J.ctl) g_poll = 0;
        J.fails++;
        snprintf(J.why, sizeof J.why, "%s", why);
        return 0;
    }
    J.running = 1;
    J.starts++;
    snprintf(J.why, sizeof J.why, "running");
    snprintf(why, (size_t)why_n, "started %s", J.sim ? "on a simulation thread" : "on the ME");
    return 1;
}

static void jobs_stop(void)
{
    if (!J.running) return;
    if (J.sim) {
        J.ctl->h.exit_req = 1;
        for (int i = 0; i < 300 && J.ctl->h.state != 2; i++) sceKernelDelayThread(1000);
        if (J.ctl->h.state == 2 && J.th >= 0) sceKernelDeleteThread(J.th);
        J.th = -1;
    } else if (hle_me_owner() == &J.ctl->h) {
        hle_me_stop(100);                                   /* handshake: the jobs' cached state is written back */
    }
    if (g_poll == J.ctl) g_poll = 0;
    J.running = 0;
}

void zma_jobs_yield(void) { jobs_stop(); }

void zma_jobs_unbind(volatile ZmaCtl *c, int restart_ok)
{
    if (g_poll == c) g_poll = 0;
    if (c == J.ctl) J.running = 0;
    if (restart_ok && J.want && zma_jobs_count() > 0 && !J.running) {
        char why[120];
        jobs_start(why, sizeof why);
    }
}

int hle_me_jobs_run(int on, int sim, char *why, int why_n)
{
    jobs_init();
    J.want = on;
    if (!on) { jobs_stop(); snprintf(why, (size_t)why_n, "off"); return 0; }
    if (hle_me_jobs_polled()) {
        snprintf(why, (size_t)why_n, "%s", g_poll == J.ctl ? "the jobs-only loop runs" : "the audio loop polls the jobs");
        return 1;
    }
    J.sim = sim;
    if (J.running) jobs_stop();                             /* stale (stopped behind our back) */
    return jobs_start(why, why_n);
}

void hle_me_jobs_info(char *out, int n)
{
    jobs_init();
    double mhz = hle_me_count_mhz();
    int o = snprintf(out, (size_t)n, "ME jobs: %s, Count %.1f MHz%s",
                     !hle_me_jobs_polled() ? "nobody polls" : g_poll == J.ctl ? "jobs-only loop" : "audio loop", mhz,
                     g_poll_sim ? " (simulation: us)" : "");
    uint32_t cnt = g_jobs->n > HLE_ME_JOBS ? HLE_ME_JOBS : g_jobs->n;
    for (uint32_t i = 0; i < cnt && o < n - 8; i++) {
        volatile ZmaJobSlot *js = &g_jobs->s[i];
        double busy = (double)js->busy_hi * 4294967296.0 + (double)js->busy_lo;
        double k = mhz > 0 ? 1.0 / mhz : 0.0;               /* Count units -> us */
        o += snprintf(out + o, (size_t)(n - o), "; [%u] %s%s: %u units, %.1f ms, avg %.1f us, longest %.1f us, %u over "
                      "1 ms", (unsigned)i, g_job_name[i] ? g_job_name[i] : "?", js->on ? "" : " (off)",
                      (unsigned)js->units, busy * k / 1000.0, js->units ? busy * k / js->units : 0.0, js->max * k,
                      (unsigned)js->over);
    }
    if (J.starts || J.fails)
        snprintf(out + o, (size_t)(n > o ? n - o : 0), "; jobs-only loop: %u start(s), %u failure(s) (%s)",
                 (unsigned)J.starts, (unsigned)J.fails, J.why);
}

void zma_stats(const ZmaHost *z, AsStats *out)
{
    uint32_t *w = (uint32_t *)(void *)out;
    for (uint32_t i = 0; i < sizeof(AsStats) / 4u; i++) w[i] = z->c->st_words[i];
}

#else
typedef int hle_me_audio_unused;
#endif
