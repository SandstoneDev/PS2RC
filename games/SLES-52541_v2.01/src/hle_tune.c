/* hle_tune.c - less game work per frame through the game's own variables (draw distance, LOD distances,
 * pedestrian / traffic density). Keys and semantics: hle_tune.h.
 *
 * Guest addresses: SLES-52541 v2.01 main ELF (call sites and script command handlers observed in the translated code).
 * Only float / int32 arithmetic on values the host and the PSP compute identically (IEEE single precision), so a host
 * run with the same keys stays the PSP run's twin.
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared poc6.cfg table */
#include "hle_tune.h"
#ifdef HLE_PSP
#include <pspiofilemgr.h>
#include <pspkernel.h>
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------ the game's variables */
#define A_TC_FARCLIP    0x707490u   /* the current time-of-day set (0x707440) +0x50: far clip, rewritten every frame */
#define A_TC_FOGSTART   0x707494u   /* ... +0x54: fog start -> the camera's fog plane (game_loop 0x246998) */
#define A_LODDISTSCALE  0x666E1Cu   /* the LOD distance scale, 1.0 on the PS2, never written by the game */
#define A_PEDDENSITY    0x66592Cu   /* ped density multiplier (also set by script command 0x03DE) */
#define A_CARDENSITY    0x6659B4u   /* car density multiplier (also set by script command 0x01EB) */
#define A_MAXPEDS       0x665934u   /* ped limit (25) */
#define A_MAXPEDS_INT   0x665938u   /* ped limit in interiors (40) */
#define A_MAXCARS       0x6659B8u   /* car limit (12) */
/* read-only statistics */
#define A_VIS_ENT       0x66C220u   /* visible entities (the render-list build zeroes it, the world scan adds) */
#define A_VIS_LOD       0x66C224u   /* visible LODs */
#define A_INVIS_ENT     0x66C228u   /* invisible entities */
#define A_TOTALPEDS     0x66BCA0u   /* total peds (cops + emergency + civilians + gangs) */
static const uint32_t A_CARS[6] = {0x66BCD8u, 0x66BCDCu, 0x66BCE0u, 0x66BCE4u, 0x66BCECu, 0x66BCF0u};
                                    /* the car counters the game sums against the car limit (0x1AA6D8) */
/* streaming loader (read-only statistics) */
#define A_STR_CHANNEL   0x7FF3B0u   /* 2 read channels, 0x98 bytes each: model ids[16] +0, load status +0x80
                                       (1 reading, 2 big model), sector +0x88, sector count +0x8C (0x2C3680) */
#define A_STR_REQUESTED 0x66C688u   /* models requested (0x2C1620 increments, 0x2C3680 decrements) */
#define A_STR_MEMUSED   0x66C68Cu   /* streaming memory used (the streaming update 0x2BFED0: memory ids 7, 8, 11-13) */
#define A_STR_MEMAVAIL  0x667688u   /* streaming budget (settings file "memory" x 1024, read at init 0x895AE0) */
#define A_STR_VEHWANTED 0x66768Cu   /* wanted resident vehicle models (settings file "vehicles", 12) */
#define A_HEAP          0x66D500u   /* the main heap descriptor (init 0x100250): start +0, end +4,
                                       bytes used +0x40 (zeroed by the heap init 0x10CD50) */

static uint32_t tune_us(void)                  /* real time in us (integer: no soft-float on the PSP) */
{
#ifdef HLE_PSP
    return sceKernelGetSystemTimeLow();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
#endif
}

static inline float gf(uint32_t a) { uint32_t w = gw(a); float f; memcpy(&f, &w, 4); return f; }
static inline void sf(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); sw_(a, w); }

/* ------------------------------------------------------------------ options */
static double o_far = 100, o_fog = 100, o_lod = 100, o_peds = 100, o_cars = 100, o_farmax = 0;
static int o_maxpeds = -1, o_maxcars = -1, o_stats = 0, o_strmem = 0, o_vehmodels = -1, o_strfree = 0;
static int o_strlog = 0;                       /* diagnostic: top-N loaded models of the span (not a cut) */
static double o_fogmax = 0;                    /* fog start at most P % of the applied far clip (0 = off) */
static double o_drivefar = 0, o_drivev = 8;    /* far clip x P % while the player drives faster than V m/s */
static float k_drive = 1.0f;                   /* the smoothed drive factor applied this frame */
static int drive_snap;                         /* set by hle_tune_set (A/B switch): jump, do not ease */
static uint64_t n_drive_frames;
static double drive_k_sum;
static int t_on, t_any;                        /* any key changes the game / statistics wanted */
static float k_far = 1.0f, k_fog = 1.0f, k_lod = 1.0f, k_peds = 1.0f, k_cars = 1.0f, f_farmax = 0.0f, k_fogmax = 0.0f;

static void opt_set(const char *k, const char *v)
{
    double x = strtod(v, NULL);
    if (!strcmp(k, "far")) o_far = x;
    else if (!strcmp(k, "farmax")) o_farmax = x;
    else if (!strcmp(k, "fogmax")) o_fogmax = x;
    else if (!strcmp(k, "drivefar")) o_drivefar = x;
    else if (!strcmp(k, "drivev")) o_drivev = x;
    else if (!strcmp(k, "fog")) o_fog = x;
    else if (!strcmp(k, "lod")) o_lod = x;
    else if (!strcmp(k, "peds")) o_peds = x;
    else if (!strcmp(k, "cars")) o_cars = x;
    else if (!strcmp(k, "maxpeds")) o_maxpeds = (int)x;
    else if (!strcmp(k, "maxcars")) o_maxcars = (int)x;
    else if (!strcmp(k, "strmem")) o_strmem = (int)x;
    else if (!strcmp(k, "vehmodels")) o_vehmodels = (int)x;
    else if (!strcmp(k, "strfree")) o_strfree = (int)x;
    else if (!strcmp(k, "strlog")) o_strlog = (int)x;
    else if (!strcmp(k, "stats")) o_stats = (int)x;
    else LOG("tune: unknown key tune_%s (ignored)\n", k);
}

static void opt_load(void)
{
#ifdef HLE_PSP
    for (int ci = 0; ci < hle_cfg_count(); ci++)     /* the shared table (hle_cfg.c), whole file */
        if (!strncmp(hle_cfg_key(ci), "tune_", 5)) opt_set(hle_cfg_key(ci) + 5, hle_cfg_val(ci));
#else
    static const char *keys[] = {"far", "farmax", "fog", "lod", "peds", "cars", "maxpeds", "maxcars", "stats",
                                 "strmem", "vehmodels", "strfree", "strlog", "fogmax", "drivefar", "drivev"};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char env[32] = "HLE_TUNE_";
        size_t n = 9;
        for (const char *p = keys[i]; *p && n < sizeof env - 1; p++) env[n++] = (char)(*p - 32);
        env[n] = 0;
        const char *e = getenv(env);
        if (e && *e) opt_set(keys[i], e);
    }
#endif
}

static void k_update(void)
{
    k_far = (float)(o_far / 100.0);
    f_farmax = (float)o_farmax;
    k_fog = (float)(o_fog / 100.0);
    k_lod = (float)(o_lod / 100.0);
    k_peds = (float)(o_peds / 100.0);
    k_cars = (float)(o_cars / 100.0);
    k_fogmax = o_fogmax > 0 ? (float)(o_fogmax / 100.0) : 0.0f;
}

void hle_tune_init(void)
{
    opt_load();
    k_update();
    t_on = o_far != 100 || o_farmax > 0 || o_fog != 100 || o_fogmax > 0 || o_drivefar > 0 || o_lod != 100 ||
           o_peds != 100 ||
           o_cars != 100 || o_maxpeds >= 0 || o_maxcars >= 0 || o_strmem > 0 || o_vehmodels >= 0;
    t_any = t_on || o_stats > 0 || o_strlog > 0;
    LOG("tune: tune_far=%g tune_farmax=%g tune_fog=%g tune_lod=%g tune_peds=%g tune_cars=%g "
        "tune_maxpeds=%d tune_maxcars=%d tune_strmem=%d tune_strfree=%d tune_vehmodels=%d tune_stats=%d - %s "
        "(0x278320 hook)\n",
        o_far, o_farmax, o_fog, o_lod, o_peds, o_cars, o_maxpeds, o_maxcars, o_strmem, o_strfree, o_vehmodels,
        o_stats,
        t_on ? "the game's draw distance / density variables are CHANGED" : "original game (nothing written)");
    if (o_fogmax > 0) LOG("tune: tune_fogmax=%g - the fog start is at most %g %% of the applied far clip\n",
                          o_fogmax, o_fogmax);
    if (o_drivefar > 0)
        LOG("tune: tune_drivefar=%g tune_drivev=%g - far clip (and fog start) x %g %% while the player drives "
            "faster than %g m/s (eased 1 %% per frame)\n", o_drivefar, o_drivev, o_drivefar, o_drivev);
}

/* run-time switch of the per-frame draw-distance keys for the in-run A/B (called through a weak symbol from a
   developer module, not in every checkout). key without "tune_"; returns 1 = applied from the next daycycle_update,
   0 = not a run-time key (density / streaming keys stay boot-time: spawned entities and loaded models outlive a
   window). After the first call the far / fog / LOD block is written every frame (t_rt): at 100 % that writes the
   game's own values back unchanged (far x 1.0, fog x (far / far) x 1.0, LOD scale 1.0 = the PS2's constant), so switching a key
   back to 100 really restores the original. Not called unless an A/B lists a tune_ key: default runs unchanged. */
static int t_rt;
int hle_tune_set(const char *key, const char *value)
{
    if (strcmp(key, "far") && strcmp(key, "farmax") && strcmp(key, "fog") && strcmp(key, "fogmax") && strcmp(key, "lod") &&
        strcmp(key, "drivefar") && strcmp(key, "drivev"))
        return 0;
    if (!strcmp(key, "drivefar") || !strcmp(key, "drivev")) drive_snap = 1;
    opt_set(key, value);
    k_update();
    t_rt = t_on = t_any = 1;
    return 1;
}

/* ------------------------------------------------------------------ statistics (read only) */
typedef struct {
    uint64_t frames;
    double vis, lod, invis, peds, cars, far_game, far_set, fog_set, be;
    uint32_t vis_max, lod_max, peds_max, cars_max;
} Acc;
static Acc w_acc, s_acc;                       /* current window, gameplay span 50-95 s */
typedef struct {
    uint64_t frames, busy, reads, sectors, models[5], spawn_peds, spawn_cars;
    double mem, req, heap_used;
    uint32_t mem_max, req_max, heap_free_min;
} SAcc;                                        /* streaming + population churn */
static SAcc w_sacc, s_sacc;
static const char *const mclass[5] = {"peds", "vehicles", "world/objects", "textures", "other"};
static uint32_t ch_status[2], ch_sector[2], last_peds, last_cars;
static int st_have_last;
#define STRLOG_IDS 32768
static uint16_t *strlog_hist;                  /* tune_strlog: loads per model id over the span */
#define A_STR_INFO      0x7C4900u              /* the streaming info per model, 12 bytes per id (see 0x2C1620):
                                                  next / prev s16 +0 / +2, state (low 3 bits) | flags +6 */
static double w_t0 = -1;
#define SPAN0 50.0
#define SPAN1 95.0

static void acc_add(Acc *a, uint32_t vis, uint32_t lod, uint32_t invis, uint32_t peds, uint32_t cars, float fg,
                    float fs, float og, uint64_t be)
{
    a->frames++;
    a->be += (double)be;
    a->vis += vis;
    a->lod += lod;
    a->invis += invis;
    a->peds += peds;
    a->cars += cars;
    a->far_game += fg;
    a->far_set += fs;
    a->fog_set += og;
    if (vis > a->vis_max) a->vis_max = vis;
    if (lod > a->lod_max) a->lod_max = lod;
    if (peds > a->peds_max) a->peds_max = peds;
    if (cars > a->cars_max) a->cars_max = cars;
}

static void sacc_log(const char *what, const SAcc *a)
{
    if (!a->frames) return;
    double n = (double)a->frames;
    LOG("tune streaming %s: %llu frames, channel busy %.0f %%, reads %.2f / frame (%.1f KB / frame), models loaded / "
        "100 frames: %s %.1f, %s %.1f, %s %.1f, %s %.1f, %s %.1f; requested %.1f (max %u); memory used %.0f KB "
        "(max %u) of %u KB; heap %u KB, used %.0f KB, free min %u KB; spawned / 100 frames: peds %.1f, cars %.1f\n",
        what, (unsigned long long)a->frames,
        100.0 * (double)a->busy / n, (double)a->reads / n, (double)a->sectors * 2.0 / n, mclass[0],
        100.0 * (double)a->models[0] / n, mclass[1], 100.0 * (double)a->models[1] / n, mclass[2],
        100.0 * (double)a->models[2] / n, mclass[3], 100.0 * (double)a->models[3] / n, mclass[4],
        100.0 * (double)a->models[4] / n, a->req / n, a->req_max, a->mem / n / 1024.0, a->mem_max >> 10,
        gw(A_STR_MEMAVAIL) >> 10, (gw(A_HEAP + 4) - gw(A_HEAP)) >> 10, a->heap_used / n / 1024.0,
        a->heap_free_min >> 10, 100.0 * (double)a->spawn_peds / n, 100.0 * (double)a->spawn_cars / n);
}

static void acc_log(const char *what, const Acc *a)
{
    if (!a->frames) return;
    double n = (double)a->frames;
    LOG("tune stats %s: %llu frames, visible entities %.1f (max %u) + LODs %.1f (max %u), invisible %.1f, peds %.1f "
        "(max %u), cars %.1f (max %u), far clip game %.0f -> %.0f, fog start %.0f, guest loop back-edges %.1f "
        "k/frame\n",
        what, (unsigned long long)a->frames, a->vis / n, a->vis_max, a->lod / n, a->lod_max, a->invis / n, a->peds / n,
        a->peds_max, a->cars / n, a->cars_max, a->far_game / n, a->far_set / n, a->fog_set / n, a->be / n / 1000.0);
}

/* ------------------------------------------------------------------ apply */
/* a game value the HLE scales by k. When the guest wrote something else than our last write, that is its new base -
   unless it is one of our own earlier writes coming back: the cutscene overlay saves the density multipliers, sets 0
   and restores the saved (= our scaled) value at the end (host run: 0.25 restored at vtime 44.2 s with k = 0.25), which
   must not be scaled a second time. A script value equal to one of our recent writes is taken as that restore too
   (the error can only leave more of the game's own value, never less). */
typedef struct {
    float base, set;
    float hist_set[4], hist_base[4];               /* our recent writes and the base each one stood for */
    int nhist;
    uint64_t changes;
    const char *name;
} Scaled;
static Scaled ped_d = {1.0f, -1.0f, {0}, {0}, 0, 0, "ped density"};
static Scaled car_d = {1.0f, -1.0f, {0}, {0}, 0, 0, "car density"};
static int32_t pint_base = 40, pint_set = -1;
static uint64_t n_frames, be_last, n_budget_changes;
static uint32_t ini_budget, last_budget;
extern uint32_t hle_backedge_count;                /* hle_sched.c: back-edges of the current period */

static void rescale(uint32_t a, float k, Scaled *v)
{
    float cur = gf(a);
    if (cur != v->set) {
        float base = cur;
        int restored = 0;
        for (int i = 0; i < v->nhist; i++)
            if (v->hist_set[i] == cur) { base = v->hist_base[i]; restored = 1; break; }
        if (v->set >= 0.0f || cur != v->base) {
            v->changes++;
            if (v->changes <= 64)
                LOG("tune: %s set by the game to %.3f (was base %.3f)%s at vtime %.3f s -> %.3f applied\n", v->name,
                    cur, v->base, restored ? " = our own earlier write restored" : "", (double)hle_vtime / EE_HZ,
                    base * k);
        }
        v->base = base;
    }
    float set = v->base * k;
    if (set != v->set) {                           /* remember the write and the base it stands for */
        int i;
        for (i = 0; i < v->nhist && v->hist_set[i] != set; i++) {}
        if (i == v->nhist) {
            if (v->nhist < 4) v->nhist++;
            for (i = v->nhist - 1; i > 0; i--) {
                v->hist_set[i] = v->hist_set[i - 1];
                v->hist_base[i] = v->hist_base[i - 1];
            }
            v->hist_set[0] = set;
            v->hist_base[0] = v->base;
        }
    }
    v->set = set;
    sf(a, set);
}

/* one sample of the streaming / population state per frame (read only): a channel read is new when the channel is
   reading (1) or started a big model (2) at another sector than at the last sample */
static void stream_sample(int in_span)
{
    SAcc f;
    memset(&f, 0, sizeof f);
    if (o_strlog > 0 && !strlog_hist) strlog_hist = calloc(STRLOG_IDS, sizeof *strlog_hist);
    f.frames = 1;
    for (int c = 0; c < 2; c++) {
        uint32_t ch = A_STR_CHANNEL + 0x98u * (uint32_t)c;
        uint32_t st = gw(ch + 0x80), sec = gw(ch + 0x88);
        if (st) f.busy = 1;
        if ((st == 1 || st == 2) && (ch_status[c] != st || ch_sector[c] != sec)) {
            f.reads++;
            f.sectors += gw(ch + 0x8C);
            for (int i = 0; i < 16; i++) {
                int32_t id = (int32_t)gw(ch + 4u * (uint32_t)i);
                if (id < 0) continue;
                if (strlog_hist && in_span && id < STRLOG_IDS && strlog_hist[id] < 0xFFFF) strlog_hist[id]++;
                f.models[id < 300 ? 0 : (id >= 400 && id < 612) ? 1 : id < 15080 ? 2 : id < 18690 ? 3 : 4]++;
            }
        }
        ch_status[c] = st;
        ch_sector[c] = sec;
    }
    uint32_t mem = gw(A_STR_MEMUSED), req = gw(A_STR_REQUESTED), peds = gw(A_TOTALPEDS), cars = 0;
    for (int i = 0; i < 6; i++) cars += gw(A_CARS[i]);
    if (st_have_last) {
        if (peds > last_peds) f.spawn_peds = peds - last_peds;
        if (cars > last_cars) f.spawn_cars = cars - last_cars;
    }
    st_have_last = 1;
    last_peds = peds;
    last_cars = cars;
    f.mem = mem;
    f.req = (double)(int32_t)req;
    f.mem_max = mem;
    uint32_t heap_size = gw(A_HEAP + 4) - gw(A_HEAP), heap_used = gw(A_HEAP + 0x40);
    f.heap_used = heap_used;
    f.heap_free_min = heap_used < heap_size ? heap_size - heap_used : 0;
    f.req_max = req;
    SAcc *dst[2] = {in_span ? &s_sacc : NULL, o_stats > 0 ? &w_sacc : NULL};
    for (int k = 0; k < 2; k++) {
        SAcc *a = dst[k];
        if (!a) continue;
        a->frames++;
        a->busy += f.busy;
        a->reads += f.reads;
        a->sectors += f.sectors;
        for (int i = 0; i < 5; i++) a->models[i] += f.models[i];
        a->spawn_peds += f.spawn_peds;
        a->spawn_cars += f.spawn_cars;
        a->mem += f.mem;
        a->req += f.req;
        if (f.mem_max > a->mem_max) a->mem_max = f.mem_max;
        a->heap_used += f.heap_used;
        if (a->frames == 1 || f.heap_free_min < a->heap_free_min) a->heap_free_min = f.heap_free_min;
        if (f.req_max > a->req_max && (int32_t)f.req_max >= 0) a->req_max = f.req_max;
    }
}

/* tune_drivefar - the player's vehicle speed (player ped 0x70A260, ped +0x4A5 bit 0 in a vehicle, +0x5CC the
   vehicle, vehicle +0x44 move speed in m per 1/50 s) decides the target factor
   (P % above tune_drivev m/s, else 100 %); the applied factor eases toward it by 1 % per frame (no fog pop) */
static float drive_k(void)
{
    float target = 1.0f;
    if (o_drivefar > 0) {
        uint32_t ped = gw(0x0070A260u);
        if (ped >= 0x100000u && ped < hle_ram_end - 0x600u && (gb(ped + 0x4A5) & 1)) {
            uint32_t v = gw(ped + 0x5CC);
            if (v >= 0x100000u && v < hle_ram_end - 0x100u) {
                float sx = gf(v + 0x44) * 50.0f, sy = gf(v + 0x48) * 50.0f;
                if (sx * sx + sy * sy > (float)(o_drivev * o_drivev)) target = (float)(o_drivefar / 100.0);
            }
        }
    }
    if (drive_snap) { k_drive = target; drive_snap = 0; }
    if (k_drive < target) k_drive = k_drive + 0.01f < target ? k_drive + 0.01f : target;
    else if (k_drive > target) k_drive = k_drive - 0.01f > target ? k_drive - 0.01f : target;
    if (k_drive != 1.0f) { n_drive_frames++; drive_k_sum += k_drive; }
    return k_drive;
}

static void tune_frame(void)
{
    float far_game = gf(A_TC_FARCLIP), far_set = far_game, fog_set = gf(A_TC_FOGSTART);
    if (t_on) {
        if (o_far != 100 || o_farmax > 0 || o_fog != 100 || o_fogmax > 0 || o_drivefar > 0 || k_drive != 1.0f ||
            t_rt) {
            far_set = far_game * k_far;
            if (f_farmax > 0.0f && far_set > f_farmax) far_set = f_farmax;
            far_set *= drive_k();                  /* 1.0 unless tune_drivefar is on and the player drives */
            fog_set = far_game > 0.0f ? fog_set * (far_set / far_game) * k_fog : fog_set * k_fog;
            /* tune_fogmax: with tune_fog > 100 the fog start can stay where the game put it while the far
               clip comes in - capped below the far clip so the fog still completes there (fog start >= far turns
               the GE fog off: hle_w3d.c w3d_base_state) */
            if (k_fogmax > 0.0f && fog_set > far_set * k_fogmax) fog_set = far_set * k_fogmax;
            sf(A_TC_FARCLIP, far_set);
            sf(A_TC_FOGSTART, fog_set);
        }
        if (o_lod != 100 || t_rt) sf(A_LODDISTSCALE, k_lod);
        if (o_peds != 100) rescale(A_PEDDENSITY, k_peds, &ped_d);
        if (o_cars != 100) rescale(A_CARDENSITY, k_cars, &car_d);
        if (o_maxpeds >= 0) {
            sw_(A_MAXPEDS, (uint32_t)o_maxpeds);
            int32_t cur = (int32_t)gw(A_MAXPEDS_INT);
            if (cur != pint_set) pint_base = cur;          /* the game's interior cap (script 0x4BDC80 writes it) */
            pint_set = (int32_t)((int64_t)pint_base * o_maxpeds / 25);
            sw_(A_MAXPEDS_INT, (uint32_t)pint_set);
        }
        if (o_maxcars >= 0) sw_(A_MAXCARS, (uint32_t)o_maxcars);
        if (o_strmem > 0) {                        /* the game writes the budget only at init (settings file) */
            if (!ini_budget) ini_budget = gw(A_STR_MEMAVAIL);
            uint32_t budget = (uint32_t)o_strmem << 10;
            if (o_strfree > 0) {                   /* keep tune_strfree KB of the heap free for the game's own
                                                      allocations (cutscenes, missions): the budget shrinks to what
                                                      the heap can hold, never below the file's value */
                uint32_t size = gw(A_HEAP + 4) - gw(A_HEAP), used = gw(A_HEAP + 0x40), mem = gw(A_STR_MEMUSED);
                uint32_t room = mem + (used < size ? size - used : 0), reserve = (uint32_t)o_strfree << 10;
                uint32_t cap = room > reserve ? room - reserve : 0;
                if (cap < budget) budget = cap;
                if (budget < ini_budget) budget = ini_budget;
                if (budget != last_budget) n_budget_changes++;
                last_budget = budget;
            }
            sw_(A_STR_MEMAVAIL, budget);
        }
        if (o_vehmodels >= 0) sw_(A_STR_VEHWANTED, (uint32_t)o_vehmodels);
        if (n_frames == 0)
            LOG("tune: first frame at vtime %.3f s: far clip %.1f -> %.1f, fog start -> %.1f, LOD distance scale %.3f, "
                "ped density %.3f, car density %.3f, ped limit %d (interior %d), "
                "car limit %d\n", (double)hle_vtime / EE_HZ, far_game, far_set, fog_set,
                gf(A_LODDISTSCALE), gf(A_PEDDENSITY), gf(A_CARDENSITY), (int)gw(A_MAXPEDS), (int)gw(A_MAXPEDS_INT),
                (int)gw(A_MAXCARS));
        if (n_frames == 0 && (o_strmem > 0 || o_vehmodels >= 0))
            LOG("tune: streaming budget %u KB, vehicle models wanted %d (heap %u KB, used %u "
                "KB)\n",
                gw(A_STR_MEMAVAIL) >> 10, (int)gw(A_STR_VEHWANTED), (gw(A_HEAP + 4) - gw(A_HEAP)) >> 10,
                gw(A_HEAP + 0x40) >> 10);
    }
    n_frames++;
    /* statistics: the render-list counters hold the previous frame's render-list build result */
    uint32_t cars = 0;
    for (int i = 0; i < 6; i++) cars += gw(A_CARS[i]);
    uint32_t vis = gw(A_VIS_ENT), lod = gw(A_VIS_LOD), invis = gw(A_INVIS_ENT), peds = gw(A_TOTALPEDS);
    uint64_t be_now = hle_stat_backedges + hle_backedge_count, be = be_last ? be_now - be_last : 0;
    be_last = be_now;                              /* guest loop back-edges since the last frame (deterministic work) */
    double t = (double)hle_vtime / EE_HZ;
    if (t >= SPAN0 && t < SPAN1) acc_add(&s_acc, vis, lod, invis, peds, cars, far_game, far_set, fog_set, be);
    if (o_stats > 0) {
        if (w_t0 < 0) w_t0 = t;
        if (t - w_t0 >= o_stats) {
            char what[64];
            snprintf(what, sizeof what, "vtime %.0f-%.0f s", w_t0, t);
            acc_log(what, &w_acc);
            sacc_log(what, &w_sacc);
            memset(&w_acc, 0, sizeof w_acc);
            memset(&w_sacc, 0, sizeof w_sacc);
            w_t0 = t;
        }
        acc_add(&w_acc, vis, lod, invis, peds, cars, far_game, far_set, fog_set, be);
    }
    stream_sample(t >= SPAN0 && t < SPAN1);        /* after the window switch: the frame counts in the new window */
}

/* daycycle_update: computes the current time-of-day colour set for the camera position - once per game_process */
void GUEST(00278320)(R5900Ctx *c);
HLE(00278320)
{
    GUEST(00278320)(c);
    if (t_any) tune_frame();
    if (hle_bc_tick) hle_bc_tick();                                 /* developer module (not in every checkout): position log / test aids */
}

/* the memory manager's emergency paths (the allocator 0x1005E0 when the heap is full; the streaming space maker
   0x2C5890 when no streamed model is removable): counted and timed always (one compare per call), logged with
   tune_stats */
static uint64_t n_tidy, n_delbehind, us_tidy;
static uint32_t tidy_max_us;
void GUEST(00244590)(R5900Ctx *c);
void GUEST(002c5e40)(R5900Ctx *c);
HLE(00244590)                                      /* heap_defrag(a0, a1): defragments the heap */
{
    uint32_t a0 = ARG(0), a1 = ARG(1), t0 = tune_us();
    GUEST(00244590)(c);
    uint32_t dt = tune_us() - t0;
    n_tidy++;
    us_tidy += dt;
    if (dt > tidy_max_us) tidy_max_us = dt;
    if (t_any && n_tidy <= 32)
        LOG("tune: heap_defrag(%u, %u) #%llu at vtime %.3f s: %u us real (heap used %u KB of %u KB, streaming "
            "%u KB of %u KB)\n", a0, a1, (unsigned long long)n_tidy, (double)hle_vtime / EE_HZ, dt,
            gw(A_HEAP + 0x40) >> 10, (gw(A_HEAP + 4) - gw(A_HEAP)) >> 10, gw(A_STR_MEMUSED) >> 10,
            gw(A_STR_MEMAVAIL) >> 10);
}

HLE(002c5e40)                                      /* stream_free_behind(memory to free) */
{
    GUEST(002c5e40)(c);
    n_delbehind++;
}

void hle_tune_report(void)
{
    if (n_tidy || n_delbehind)
        LOG("tune: memory emergency paths: heap_defrag %llu calls (%.1f ms real in total, max %u us), "
            "stream_free_behind %llu calls\n", (unsigned long long)n_tidy, (double)us_tidy / 1000.0,
            tidy_max_us, (unsigned long long)n_delbehind);
    if (hle_bc_report) hle_bc_report();                               /* developer module (not in every checkout): drive aid counters */

    if (o_drivefar > 0 || n_drive_frames)
        LOG("tune: tune_drivefar=%g: %llu frames with a drive factor below 100 %% (mean %.1f %%)\n", o_drivefar,
            (unsigned long long)n_drive_frames, n_drive_frames ? drive_k_sum * 100.0 / (double)n_drive_frames : 100.0);
    if (!t_any) return;
    LOG("tune: %llu frames tuned; density multipliers rewritten by the game: peds %llu, cars %llu times\n",
        (unsigned long long)n_frames, (unsigned long long)ped_d.changes, (unsigned long long)car_d.changes);
    if (o_strmem > 0)
        LOG("tune: streaming budget at the end %u KB (INI %u KB, tune_strmem %d, tune_strfree %d: %llu changes)\n",
            gw(A_STR_MEMAVAIL) >> 10, ini_budget >> 10, o_strmem, o_strfree, (unsigned long long)n_budget_changes);
    acc_log("gameplay span vtime 50-95 s", &s_acc);
    sacc_log("gameplay span vtime 50-95 s", &s_sacc);
    if (strlog_hist) {                             /* the most reloaded models (selection by repeated maximum) */
        for (int k = 0; k < o_strlog; k++) {
            int best = -1;
            for (int i = 0; i < STRLOG_IDS; i++)
                if (strlog_hist[i] && (best < 0 || strlog_hist[i] > strlog_hist[best])) best = i;
            if (best < 0) break;
            LOG("tune strlog: model %5d loaded %4u times in the span, info byte +6 now %02x\n", best,
                strlog_hist[best], best < 26000 ? gb(A_STR_INFO + 12u * (uint32_t)best + 6) : 0);
            strlog_hist[best] = 0;
        }
    }
}
