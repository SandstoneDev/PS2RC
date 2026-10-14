/* hle_hitch.c - frame-time histogram + the slowest main-loop iterations with a breakdown (see
 * hle_hitch.h). Integer arithmetic only on the game thread (it runs with the guest FPU mode). */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_hitch.h"

#ifdef HLE_PSP
#include <pspkernel.h>
uint64_t hle_hitch_now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
#else
#include <time.h>
uint64_t hle_hitch_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#endif

extern uint64_t hle_hitch_mesh_us __attribute__((weak));   /* the pack's mesh conversion time, us (if any) */

uint64_t hle_hitch_us[HITCH_N];
int hle_hitch_opt_window = 30;
static int opt_skip = 150;                         /* the first iterations: the mission start (the game's own loads) */
static int opt_top = 12;
int hle_hitch_opt_logstream = -1;                  /* hitch_logstream (the pack's streaming hook reads it) */

static const char *const src_name[HITCH_N] = {"disc", "disc-wait", "log", "texconv", "ge-sync", "stream", "mesh"};

#define NB 512                                     /* 1 ms buckets; the last one = 511 ms and more */
typedef struct {
    uint32_t h[NB];
    uint64_t n, sum_us, max_us;
    uint64_t src[HITCH_N];                         /* per-source sums */
} Hist;
static Hist h_all, h_play, h_win;

typedef struct { uint64_t it, us, t_us; uint32_t d[HITCH_N]; } Slow;
#define MAXTOP 24
static Slow top[MAXTOP];
static int ntop;
static Slow win_max;

uint64_t hle_hitch_it_n;                           /* main-loop iterations seen (the pack's hooks log it) */
#define it_n hle_hitch_it_n
static uint64_t last_us, t0_us, win_t0_us, prev[HITCH_N];
static int started;
static uint64_t thr_us = ~0ull;                    /* spike threshold: 2 x the gameplay median (updated every 64) */
static uint64_t spike_n, spike_us, spike_excess_us, spike_src[HITCH_N];

void hle_hitch_cfg(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "hitch_window")) hle_hitch_opt_window = x;
    else if (!strcmp(k, "hitch_skip")) opt_skip = x < 0 ? 0 : x;
    else if (!strcmp(k, "hitch_top")) opt_top = x < 1 ? 1 : x > MAXTOP ? MAXTOP : x;
    else if (!strcmp(k, "hitch_logstream")) hle_hitch_opt_logstream = x;
}

static void hist_add(Hist *h, uint64_t us, const uint32_t *d)
{
    uint64_t ms = us / 1000u;
    h->h[ms < NB ? ms : NB - 1]++;
    h->n++;
    h->sum_us += us;
    if (us > h->max_us) h->max_us = us;
    for (int k = 0; k < HITCH_N; k++) h->src[k] += d[k];
}

/* p-th per mille iteration time, ms (1 ms resolution; NB - 1 = "511 or more") */
static unsigned hist_pm(const Hist *h, unsigned pm)
{
    if (!h->n) return 0;
    uint64_t want = (h->n * pm + 999u) / 1000u, acc = 0;
    if (!want) want = 1;
    for (unsigned i = 0; i < NB; i++) {
        acc += h->h[i];
        if (acc >= want) return i;
    }
    return NB - 1;
}

static void top_add(uint64_t it, uint64_t us, uint64_t t_us, const uint32_t *d)
{
    int lim = opt_top;
    if (ntop == lim && us <= top[ntop - 1].us) return;
    int i = ntop < lim ? ntop++ : lim - 1;
    while (i > 0 && top[i - 1].us < us) { top[i] = top[i - 1]; i--; }
    top[i].it = it;
    top[i].us = us;
    top[i].t_us = t_us;
    memcpy(top[i].d, d, sizeof top[i].d);
}

static int fmt_src(char *b, int n, const Slow *s)
{
    int o = 0;
    uint64_t sum = 0;
    for (int k = 0; k < HITCH_N && o < n - 40; k++) {
        sum += s->d[k];
        if (s->d[k] >= 500u)
            o += snprintf(b + o, (size_t)(n - o), " %s %u.%u", src_name[k], (unsigned)(s->d[k] / 1000u),
                          (unsigned)(s->d[k] / 100u % 10u));
    }
    uint64_t other = s->us > sum ? s->us - sum : 0;
    if (o < n - 40)
        o += snprintf(b + o, (size_t)(n - o), " other %u.%u", (unsigned)(other / 1000u), (unsigned)(other / 100u % 10u));
    return o;
}

static void hist_line(const char *label, const Hist *h)
{
    if (!h->n) return;
    char b[640];
    int o = snprintf(b, sizeof b, "%s: %llu iterations, mean %llu.%llu ms, p50 %u p90 %u p99 %u p99.9 %u ms, max %llu.%llu ms; "
                     "histogram (ms: count)", label, (unsigned long long)h->n,
                     (unsigned long long)(h->sum_us / h->n / 1000u), (unsigned long long)(h->sum_us / h->n / 100u % 10u),
                     hist_pm(h, 500), hist_pm(h, 900), hist_pm(h, 990), hist_pm(h, 999),
                     (unsigned long long)(h->max_us / 1000u), (unsigned long long)(h->max_us / 100u % 10u));
    static const unsigned edge[] = {0, 33, 50, 67, 83, 100, 133, 167, 200, 300, 400, NB};
    for (unsigned e = 0; e + 1 < sizeof edge / sizeof edge[0] && o < (int)sizeof b - 24; e++) {
        uint64_t c = 0;
        for (unsigned i = edge[e]; i < edge[e + 1]; i++) c += h->h[i];
        if (!c) continue;
        if (edge[e + 1] == NB) o += snprintf(b + o, sizeof b - o, " >=%u:%llu", edge[e], (unsigned long long)c);
        else o += snprintf(b + o, sizeof b - o, " %u-%u:%llu", edge[e], edge[e + 1], (unsigned long long)c);
    }
    LOG("%s\n", b);
    o = snprintf(b, sizeof b, "%s: mean per iteration by source (ms):", label);
    uint64_t sum = 0;
    for (int k = 0; k < HITCH_N && o < (int)sizeof b - 32; k++) {
        sum += h->src[k];
        o += snprintf(b + o, sizeof b - o, " %s %llu.%02llu", src_name[k], (unsigned long long)(h->src[k] / h->n / 1000u),
                      (unsigned long long)(h->src[k] / h->n / 10u % 100u));
    }
    uint64_t other = h->sum_us > sum ? h->sum_us - sum : 0;
    LOG("%s other %llu.%02llu\n", b, (unsigned long long)(other / h->n / 1000u),
        (unsigned long long)(other / h->n / 10u % 100u));
}

void hle_hitch_tick(void)
{
    uint64_t now = hle_hitch_now_us();
    uint64_t cur[HITCH_N];
    memcpy(cur, hle_hitch_us, sizeof cur);
    cur[HITCH_MESH] = &hle_hitch_mesh_us ? hle_hitch_mesh_us : 0;
    if (!started) {
        started = 1;
#ifndef HLE_PSP
        const char *e = getenv("HLE_HITCH_WINDOW");      /* host: the same keys from the environment */
        if (e && *e) hle_hitch_cfg("hitch_window", e);
        e = getenv("HLE_HITCH_SKIP");
        if (e && *e) hle_hitch_cfg("hitch_skip", e);
#endif
        last_us = t0_us = win_t0_us = now;
        memcpy(prev, cur, sizeof prev);
        return;
    }
    uint64_t us = now - last_us;
    last_us = now;
    uint32_t d[HITCH_N];
    for (int k = 0; k < HITCH_N; k++) {
        uint64_t x = cur[k] - prev[k];
        d[k] = x > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)x;
        prev[k] = cur[k];
    }
    uint64_t it = it_n++;
    hist_add(&h_all, us, d);
    if (it >= (uint64_t)opt_skip) hist_add(&h_play, us, d);
    hist_add(&h_win, us, d);
    top_add(it, us, now - t0_us, d);
    if (us > win_max.us) {
        win_max.it = it;
        win_max.us = us;
        win_max.t_us = now - t0_us;
        memcpy(win_max.d, d, sizeof d);
    }
    if ((it & 63) == 63) {
        const Hist *ref = h_play.n >= 64 ? &h_play : &h_all;
        thr_us = 2000ull * hist_pm(ref, 500) + 1000u;
    }
    if (it >= (uint64_t)opt_skip && us > thr_us) {
        uint64_t med = (thr_us - 1000u) / 2u;
        spike_n++;
        spike_us += us;
        spike_excess_us += us > med ? us - med : 0;
        for (int k = 0; k < HITCH_N; k++) spike_src[k] += d[k];
    }
    if (hle_hitch_opt_window > 0 && now - win_t0_us >= (uint64_t)hle_hitch_opt_window * 1000000u) {
        char b[400];
        fmt_src(b, sizeof b, &win_max);
        LOG("hitch window real %llu-%llu s: %llu iterations, mean %llu.%llu ms, p50 %u p90 %u p99 %u ms, max %llu.%llu "
            "ms (iteration %llu:%s)\n", (unsigned long long)((win_t0_us - t0_us) / 1000000u),
            (unsigned long long)((now - t0_us) / 1000000u), (unsigned long long)h_win.n,
            (unsigned long long)(h_win.n ? h_win.sum_us / h_win.n / 1000u : 0),
            (unsigned long long)(h_win.n ? h_win.sum_us / h_win.n / 100u % 10u : 0), hist_pm(&h_win, 500),
            hist_pm(&h_win, 900), hist_pm(&h_win, 990), (unsigned long long)(win_max.us / 1000u),
            (unsigned long long)(win_max.us / 100u % 10u), (unsigned long long)win_max.it, b);
        memset(&h_win, 0, sizeof h_win);
        memset(&win_max, 0, sizeof win_max);
        win_t0_us = now;
    }
}

void hle_hitch_report(void)
{
    if (hle_pack_hitch_report) hle_pack_hitch_report();    /* the pack's timed hooks */
    if (!h_all.n) return;
    LOG("hitch: main-loop iteration times, real clock, iteration = main-loop function entry to entry; sources "
        "summed on the game thread, other = the rest (the game's code, ...)\n");
    hist_line("hitch all", &h_all);
    char lab[64];
    snprintf(lab, sizeof lab, "hitch after %d", opt_skip);
    hist_line(lab, &h_play);
    if (h_play.n) {
        uint64_t med = (thr_us == ~0ull ? 0 : (thr_us - 1000u) / 2u);
        char b[400];
        int o = 0;
        for (int k = 0; k < HITCH_N && o < (int)sizeof b - 40; k++)
            o += snprintf(b + o, sizeof b - o, " %s %llu", src_name[k], (unsigned long long)(spike_src[k] / 1000u));
        uint64_t s = 0;
        for (int k = 0; k < HITCH_N; k++) s += spike_src[k];
        LOG("hitch spikes after %d (iterations > 2 x the median %llu ms): %llu iterations, %llu ms in them, %llu ms "
            "above the median (%llu.%llu %% of the time); sources in them (ms):%s other %llu\n", opt_skip,
            (unsigned long long)(med / 1000u), (unsigned long long)spike_n, (unsigned long long)(spike_us / 1000u),
            (unsigned long long)(spike_excess_us / 1000u),
            (unsigned long long)(h_play.sum_us ? spike_excess_us * 100u / h_play.sum_us : 0),
            (unsigned long long)(h_play.sum_us ? spike_excess_us * 1000u / h_play.sum_us % 10u : 0), b,
            (unsigned long long)((spike_us > s ? spike_us - s : 0) / 1000u));
    }
    for (int i = 0; i < ntop; i++) {
        char b[400];
        fmt_src(b, sizeof b, &top[i]);
        LOG("hitch slowest %2d: iteration %llu%s at real +%llu.%llu s: %llu.%llu ms =%s\n", i + 1,
            (unsigned long long)top[i].it, top[i].it < (uint64_t)opt_skip ? " (start)" : "",
            (unsigned long long)(top[i].t_us / 1000000u), (unsigned long long)(top[i].t_us / 100000u % 10u),
            (unsigned long long)(top[i].us / 1000u), (unsigned long long)(top[i].us / 100u % 10u), b);
    }
}
