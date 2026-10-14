/* snd_glue.c - the API of snd_driver.h (sndd_init / sndd_rpc / sndd_service / sndd_timer_tick ...) implemented by the
 * game's sound driver module, translated on the player's machine from the player's own disc by the IOP static
 * recompiler (python -m ps2rc.iop gen <disc>/SYSTEM/<driver module> --sym sndd_irx) and run by the generic IOP runtime
 * (ps2rc/iop/rt). Link this file + the generated C + the runtime; callers (audio_host.c, hle_audio_psp.c, the audio
 * bench, spu2_lite.c's binding) only use snd_driver.h.
 *
 * What this glue adds to the generic runtime (all of it facts of this game's IOP set-up or of the RPC protocol):
 *   - the RPC server id of the driver and the fno 1 packet size (snd_driver.h);
 *   - the lockstep order of one fno 1: the SPU engine advances frame_ms in 64-sample chunks, after each chunk a
 *     pending SPU2 IRQ is delivered and the IOP threads run; then the RPC; then the threads again;
 *   - the 33 ms hardware timer of the driver is clocked by the engine (spu2_lite calls sndd_timer_tick every 1,584
 *     samples);
 *   - bindings for the game's disc streaming module, which the driver imports by number (#5 read, #7 sync,
 *     #8 drive position, #9, #11/#12 lock/unlock): synchronous sector reads of the disc. The drive position is
 *     reported as 0, so of the duplicated SFX pak copies the driver always picks the one with the lowest LSN (copy
 *     01) - the staged PSP disc only carries copy 01;
 *   - the disc as LSNs: the harness's disc map (hle_disc_*) when linked in, else a disc map file
 *     (SNDD_GEN_DISCMAP / sndd_gen_discmap()); the bytes are read through the platform's SnddIo by path (keeps the
 *     PSP's audio read cache);
 *   - the statistics of snd_driver.h (SnddStats) that the harness logs, counted from the driver's own messages;
 *     `state` = the driver's track state variable (module offset SNDD_GEN_STATE_OFF).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snd_driver.h"
#include "iop_rt.h"

#ifndef SNDD_GEN_MEM
#ifdef HLE_PSP
#define SNDD_GEN_MEM 0x100000u                /* 1 MB: peak use 1,010,176 B + page 0 + interrupt stacks; with
                                                 IOP_MEM_MASK 0xFFFFF every address stays inside (wild ones wrap) */
#else
#define SNDD_GEN_MEM 0x200000u                /* host: the whole 2 MB IOP address space */
#endif
#endif
#define SNDD_GEN_STATE_OFF 0xA574u            /* driver: track state (-6 stopped ... -3 playing), module-relative */
#define SNDD_GEN_LOAD_PRIO 8                  /* module_start runs above every driver thread */
#define SNDD_GEN_STACK     (32u * 1024u)      /* host stack per IOP thread */

extern const IopModule sndd_irx;            /* the generated module (build dir, never distributed) */

/* ------------------------------------------------------------------ coroutines: platform */
#ifdef HLE_PSP
#include "hle_plat.h"
typedef struct { hle_hostctx ctx; } GCoro;
static GCoro g_main;
static void *pc_create(void *u, size_t st, void (*entry)(void))
{
    (void)u;
    GCoro *g = (GCoro *)calloc(1, sizeof *g);
    void *s = hle_plat_stack_alloc(st);
    if (!g || !s) { free(g); return 0; }
    hle_plat_ctx_make(&g->ctx, s, st, entry);
    return g;
}
static void pc_swap(void *u, void *from, void *to) { (void)u; hle_plat_ctx_swap(&((GCoro *)from)->ctx, &((GCoro *)to)->ctx); }
static void *pc_main(void *u) { (void)u; return &g_main; }
static void coro_ops(IopCoroOps *o) { o->user = 0; o->create = pc_create; o->swap = pc_swap; o->main_ctx = pc_main; }
/* the runtime's memory (the IOP RAM image is the big part): one user-partition block per request, zeroed */
#include <pspsysmem.h>
static void *psp_alloc(void *u, size_t n)
{
    (void)u;
    SceUID id = sceKernelAllocPartitionMemory(2, "ps2rc_iop", PSP_SMEM_Low, (SceSize)n, NULL);
    if (id < 0) return 0;
    void *p = sceKernelGetBlockHeadAddr(id);
    memset(p, 0, n);
    return p;
}
#define SNDD_GEN_ALLOC psp_alloc
#else
#include "iop_rt_ucontext.h"
static void coro_ops(IopCoroOps *o) { iop_rt_coro_ucontext(o); }
#endif

/* ------------------------------------------------------------------ the disc as LSNs */
int hle_disc_lookup(const char *guest_path, uint32_t *lsn, uint32_t *size) __attribute__((weak));
const char *hle_disc_name_of_lsn(uint32_t lsn) __attribute__((weak));

typedef struct { uint32_t lsn, size; char iso[64]; } MapEnt;
static MapEnt *g_map;
static int g_nmap;
static char g_map_path[512];

void sndd_gen_discmap(const char *path)
{
    snprintf(g_map_path, sizeof g_map_path, "%s", path ? path : "");
}

static int map_cmp(const void *a, const void *b)
{
    uint32_t x = ((const MapEnt *)a)->lsn, y = ((const MapEnt *)b)->lsn;
    return x < y ? -1 : x > y;
}

static void map_load(void)
{
    if (g_map || (hle_disc_lookup && hle_disc_name_of_lsn)) return;
    const char *p = g_map_path[0] ? g_map_path : getenv("SNDD_GEN_DISCMAP");
    FILE *f = p ? fopen(p, "r") : 0;
    if (!f) return;
    int cap = 4096;
    g_map = (MapEnt *)calloc((size_t)cap, sizeof *g_map);
    char line[512];
    while (g_map && fgets(line, sizeof line, f)) {
        MapEnt e;
        memset(&e, 0, sizeof e);
        if (sscanf(line, "%u %u %63s", &e.lsn, &e.size, e.iso) != 3) continue;
        if (g_nmap == cap) {
            cap *= 2;
            MapEnt *n = (MapEnt *)realloc(g_map, (size_t)cap * sizeof *g_map);
            if (!n) break;
            g_map = n;
        }
        g_map[g_nmap++] = e;
    }
    fclose(f);
    if (g_map) qsort(g_map, (size_t)g_nmap, sizeof *g_map, map_cmp);
}

/* "\\AUDIO\\CONFIG\\BANKSLOT.DAT;1" -> "AUDIO/CONFIG/BANKSLOT.DAT" (disc map form) */
static void iso_key(const char *in, char *out, size_t n)
{
    while (*in == '\\' || *in == '/') in++;
    size_t k = 0;
    for (; *in && *in != ';' && k + 1 < n; in++)
        out[k++] = *in == '\\' ? '/' : (*in >= 'a' && *in <= 'z' ? (char)(*in - 32) : *in);
    out[k] = 0;
}

static int map_lookup(const char *iso, uint32_t *lsn, uint32_t *size)
{
    if (hle_disc_lookup && hle_disc_name_of_lsn) return hle_disc_lookup(iso, lsn, size) ? 0 : -1;
    map_load();
    char k[256];
    iso_key(iso, k, sizeof k);
    for (int i = 0; i < g_nmap; i++)
        if (!strcmp(g_map[i].iso, k)) { *lsn = g_map[i].lsn; *size = g_map[i].size; return 0; }
    return -1;
}

static const char *map_name_of_lsn(uint32_t lsn, uint32_t *start, uint32_t *size)
{
    if (hle_disc_lookup && hle_disc_name_of_lsn) {
        const char *n = hle_disc_name_of_lsn(lsn);
        if (!n || n[0] == '<' || !hle_disc_lookup(n, start, size)) return 0;
        return n;
    }
    map_load();
    int lo = 0, hi = g_nmap - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_map[mid].lsn <= lsn) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return 0;
    uint32_t ns = (g_map[best].size + 2047u) / 2048u;
    if (lsn >= g_map[best].lsn + (ns ? ns : 1)) return 0;
    *start = g_map[best].lsn;
    *size = g_map[best].size;
    return g_map[best].iso;
}

/* ------------------------------------------------------------------ one driver instance per SndDrv */
typedef struct Glue {
    SndDrv *a;
    IopRt *rt;
    int loaded;
} Glue;
static Glue g_glue[4];
static Glue *g_cur;                         /* the instance whose callbacks run (single-threaded harness) */

static Glue *glue_of(SndDrv *a)
{
    for (int i = 0; i < 4; i++) if (g_glue[i].a == a) return &g_glue[i];
    return 0;
}

/* the driver looks for up to 10 copies of each SFX pak and retries a missing one 10 times (a vblank apart): the disc
   map does not change, so the last miss is remembered */
static int d_lookup(void *u, const char *path, uint32_t *lsn, uint32_t *size)
{
    static char miss[160];
    (void)u;
    if (miss[0] && !strcmp(miss, path)) return -1;
    int r = map_lookup(path, lsn, size);
    if (r != 0) snprintf(miss, sizeof miss, "%s", path);
    return r;
}

static int d_read(void *u, uint32_t lsn, uint32_t nsect, void *dst)
{
    Glue *g = (Glue *)u;
    SnddIo *io = &g->a->cfg.io;
    uint8_t *out = (uint8_t *)dst;
    while (nsect) {
        uint32_t start, size;
        const char *iso = map_name_of_lsn(lsn, &start, &size);
        if (!iso || !io->read) { memset(out, 0, (size_t)nsect * 2048u); return -1; }
        char path[160];
        size_t k = 0;
        for (const char *s = iso; *s && k + 1 < sizeof path; s++) path[k++] = *s == '/' ? '\\' : *s;
        path[k] = 0;
        uint32_t fsect = (size + 2047u) / 2048u, rel = lsn - start, n = fsect - rel;
        if (n > nsect) n = nsect;
        uint32_t off = rel * 2048u, bytes = n * 2048u, have = size > off ? size - off : 0;
        if (have > bytes) have = bytes;
        int r;
        while ((r = io->read(io->user, path, off, out, have)) == SNDD_IO_PENDING) {}
        if (r != SNDD_IO_OK) { memset(out, 0, bytes); return -1; }
        if (have < bytes) memset(out + have, 0, bytes - have);
        out += bytes;
        lsn += n;
        nsect -= n;
    }
    return 0;
}

/* the driver's messages: errors at level 0, progress at 1; counted into SnddStats */
static void g_log(void *u, int level, const char *fmt, ...)
{
    Glue *g = (Glue *)u;
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (level == 1 && g && g->a) {
        SnddStats *s = &g->a->st;
        if (strstr(b, "Error")) {                        /* refusals are chatty (2), other errors 0 */
            if (strstr(b, "as slot is being played")) { level = 2; s->loads_refused_playing++; }
            else if (strstr(b, "as slot is being loaded")) { level = 2; s->loads_refused_loading++; }
            else if (strstr(b, "Cannot play sound")) { level = 2; s->starts_failed++; }
            else level = 0;
        }
        if (strstr(b, "Load complete")) s->loads_ok++;
        else if (strstr(b, "Failed to load sound")) s->loads_failed++;
        else if (strstr(b, "Starting track")) s->tracks_started++;
        else if (strstr(b, "Finished track")) s->track_ends++;
        else if (strstr(b, "Changing from track")) s->track_chains++;
    }
    if (g && g->a && g->a->cfg.log) g->a->cfg.log(g->a->cfg.log_user, level, "%s", b);
#ifndef HLE_PSP
    static int dbg = -2;                                 /* host debugging: SNDD_GEN_LOG=<level> -> stderr */
    if (dbg == -2) { const char *e = getenv("SNDD_GEN_LOG"); dbg = e && *e ? atoi(e) : -1; }
    if (level <= dbg) fprintf(stderr, "[iop%d] %s", level, b);
#endif
}

/* SnddStats: sound starts = voices keyed on; track fills = SPU DMA transfers of the stream ring (libsd channel 1
   in this driver - counted per sub-stream, not per ring fill) */
static void (*g_key_on)(void *user, unsigned core, uint32_t mask24);
static void cnt_key_on(void *user, unsigned core, uint32_t mask24)
{
    if (g_cur && g_cur->a) g_cur->a->st.starts += (uint32_t)__builtin_popcount(mask24);
    if (g_key_on) g_key_on(user, core, mask24);
}
static IopFn g_voice_trans;
static void cnt_voice_trans(IopCpu *c)
{
    if (iop_arg(c, 0) == 1 && g_cur && g_cur->a) g_cur->a->st.track_fills++;
    g_voice_trans(c);
}

/* ------------------------------------------------------------------ the game's disc streaming module */
static void cds_read(IopCpu *c)                          /* #5 (channel, iop_buf, lsn, sectors) -> 1 accepted */
{
    uint32_t buf = iop_arg(c, 1), lsn = iop_arg(c, 2), n = iop_arg(c, 3);
    int r = iop_rt_disc_read(c->rt, lsn, n, iop_ptr(c, buf));
    iop_ret(c, r == 0 ? 1u : 0u);
}
static void cds_zero(IopCpu *c) { iop_ret(c, 0); }       /* #7 sync (0 = done, no error), #9, #11, #12 */
static void cds_pos(IopCpu *c) { iop_ret(c, 0); }        /* #8 drive position: 0 -> the lowest-LSN copy */

#ifndef HLE_PSP
static void stats_at_exit(void)                         /* host: SNDD_GEN_STATS=1 */
{
    for (int i = 0; i < 4; i++) {
        if (!g_glue[i].rt) continue;
        const IopRtStats *s = iop_rt_stats(g_glue[i].rt);
        fprintf(stderr, "iop runtime: switches %u, threads %u, semas %u, event flags %u, SIF calls %u, interrupts %u, "
                "unimplemented imports %u (calls %u), IOP heap in use %u / peak %u of %u bytes, SPU DMA %u x / %u bytes\n",
                s->switches, s->threads, s->semas, s->evflags, s->sif_calls, s->intr_calls, s->imports_unresolved,
                s->unimpl_calls, s->mem_used, s->mem_peak, iop_rt_mem_size(g_glue[i].rt), s->sd_dmas, s->sd_dma_bytes);
    }
}
#endif

/* ------------------------------------------------------------------ API of snd_driver.h */
int sndd_init(SndDrv *a, const SnddConfig *cfg)
{
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    a->state = SNDD_TRK_STOPPED;
    Glue *g = glue_of(a);
    if (!g) g = glue_of(0);
    if (!g) return 0;
    memset(g, 0, sizeof *g);
    g->a = a;
    g_cur = g;
    IopRtConfig rc;
    memset(&rc, 0, sizeof rc);
    rc.mem_size = SNDD_GEN_MEM;
    rc.host_stack = SNDD_GEN_STACK;
#ifdef SNDD_GEN_ALLOC
    rc.alloc = SNDD_GEN_ALLOC;                 /* never freed (the runtime lives for the whole run) */
#endif
    coro_ops(&rc.coro);
    rc.disc.user = g;
    rc.disc.lookup = d_lookup;
    rc.disc.read = d_read;
    rc.sd.user = cfg->spu.user;
    rc.sd.ram = cfg->spu.ram;
    rc.sd.ram_size = SNDD_SPU_RAM_SIZE;
    rc.sd.set_param = cfg->spu.set_param;
    rc.sd.set_addr = cfg->spu.set_addr;
    rc.sd.get_envx = cfg->spu.get_envx;
    rc.sd.get_nax = cfg->spu.get_nax;
    g_key_on = cfg->spu.key_on;
    rc.sd.key_on = cnt_key_on;
    rc.sd.key_off = cfg->spu.key_off;
    rc.sd.set_irq = cfg->spu.set_irq;
    rc.sd.dma_begin = cfg->spu.dma_begin;
    rc.sd.dma_done = cfg->spu.dma_done;
    rc.log = g_log;
    rc.log_user = g;
    g->rt = iop_rt_create(&rc);
    if (!g->rt) { g->a = 0; return 0; }
    g_voice_trans = iop_rt_builtin("libsd", 17, 0);
    if (g_voice_trans) iop_rt_bind(g->rt, "libsd", 17, cnt_voice_trans);
    iop_rt_bind(g->rt, "cdstream", 5, cds_read);
    iop_rt_bind(g->rt, "cdstream", 7, cds_zero);
    iop_rt_bind(g->rt, "cdstream", 8, cds_pos);
    iop_rt_bind(g->rt, "cdstream", 9, cds_zero);
    iop_rt_bind(g->rt, "cdstream", 11, cds_zero);
    iop_rt_bind(g->rt, "cdstream", 12, cds_zero);
#ifndef HLE_PSP
    if (getenv("SNDD_GEN_STATS")) atexit(stats_at_exit);
#endif
    static const char *const argv[] = {"IOPAUDIO.IRX"};
    int r = iop_rt_load(g->rt, &sndd_irx, 1, argv, SNDD_GEN_LOAD_PRIO);
    g->loaded = r == 0;
    if (!g->loaded) g_log(g, 0, "Sound driver (generated): module start failed (%d)\n", r);
    return 0;
}

static void g_service(Glue *g)
{
    SnddSpu *s = &g->a->cfg.spu;
    if (s->irq_take && s->irq_take(s->user)) {
        iop_rt_spu_irq(g->rt, 1);
        g->a->st.track_irqs++;
    }
    iop_rt_run(g->rt);
}

static void g_state(Glue *g)
{
    const uint8_t *p = iop_rt_mem(g->rt) + ((sndd_irx.base + SNDD_GEN_STATE_OFF) & IOP_MEM_MASK);
    g->a->state = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

void sndd_service(SndDrv *a)
{
    Glue *g = glue_of(a);
    if (!g || !g->loaded || !a->initialised) return;
    g_cur = g;
    g_service(g);
}

int sndd_rpc(SndDrv *a, uint32_t fno, const uint8_t *send, uint32_t ssize, uint8_t *recv, uint32_t rsize,
             uint32_t frame_ms)
{
    Glue *g = glue_of(a);
    if (!g || !g->loaded) {                              /* no driver: the "not initialised" replies */
        if (recv && rsize) {
            uint32_t n = ssize < rsize ? ssize : rsize;
            if (send && n && recv != send) memmove(recv, send, n);
            if (n < rsize) memset(recv + n, 0, rsize - n);
            if (rsize >= 4 && fno != 1) memset(recv, 0, 4);
        }
        return 0;
    }
    g_cur = g;
    if (fno == 1 && send && recv && ssize >= SNDD_PACKET_SIZE && rsize >= SNDD_PACKET_SIZE) {
        if (!a->initialised) {                           /* no driver call before fno 0 */
            if (recv != send) memmove(recv, send, SNDD_PACKET_SIZE);
            return 0;
        }
        SnddSpu *s = &a->cfg.spu;
        if (s->advance && frame_ms) {
            uint32_t left = frame_ms * 48u;
            while (left) {
                uint32_t n = left < 64u ? left : 64u;
                s->advance(s->user, n);
                left -= n;
                g_service(g);
            }
        }
        a->st.frames++;
    }
    if (fno == 0) a->st.inits++;
    int r = iop_rt_sif_call(g->rt, SNDD_RPC_ID, fno, send, ssize, recv, rsize);
    if (r != 0 && recv && rsize) memset(recv, 0, rsize);
    if (fno == 0 && r == 0 && recv && rsize >= 4) {
        uint32_t w;
        memcpy(&w, recv, 4);
        a->initialised = w == 1;
        a->host_flag = send && ssize >= 4 ? (uint32_t)(send[0] | send[1] << 8 | send[2] << 16 | (uint32_t)send[3] << 24) : 0;
        const IopRtStats *s = iop_rt_stats(g->rt);
        g_log(g, 0, "Sound driver: generated driver (ps2rc.iop, module %s md5 %s): init -> %u; IOP threads %u, heap %u of "
              "%u KB\n", sndd_irx.name, sndd_irx.md5, w, s->threads, s->mem_used >> 10, iop_rt_mem_size(g->rt) >> 10);
    }
    g_service(g);
    g_state(g);
    return 0;
}

int32_t sndd_rpc_init(SndDrv *a, uint8_t *buf)
{
    uint8_t tmp[SNDD_INIT_PACKET_SIZE];
    memset(tmp, 0, sizeof tmp);
    if (buf) memcpy(tmp, buf, sizeof tmp);
    sndd_rpc(a, 0, tmp, sizeof tmp, tmp, sizeof tmp, 0);
    int32_t r;
    memcpy(&r, tmp, 4);
    if (buf) memcpy(buf, tmp, 4);
    return r;
}

void sndd_rpc_frame(SndDrv *a, const uint8_t *send, uint8_t *reply, uint32_t frame_ms)
{
    sndd_rpc(a, 1, send, SNDD_PACKET_SIZE, reply, SNDD_PACKET_SIZE, frame_ms);
}

void sndd_timer_tick(SndDrv *a)
{
    Glue *g = glue_of(a);
    if (!g || !g->loaded) return;
    static int checked;
    for (int k = 0; k < iop_rt_timer_count(g->rt); k++) {
        if (!iop_rt_timer_active(g->rt, k)) continue;
        if (!checked) {
            checked = 1;
            uint64_t ns = iop_rt_timer_period_ns(g->rt, k);
            if (ns != (uint64_t)SNDD_TIMER_US * 1000u)
                g_log(g, 0, "Sound driver (generated): hardware timer period %llu ns, the engine ticks every %u us\n",
                      (unsigned long long)ns, SNDD_TIMER_US);
        }
        a->timer_ticks++;
        iop_rt_timer_expire(g->rt, k);
    }
}

/* helpers of snd_driver.h that the platforms do not need from a translated driver */
const SnddSlot *sndd_slot(const SndDrv *a, int s) { (void)a; (void)s; return 0; }
const char *sndd_pak_name(const SndDrv *a, int pak) { (void)a; (void)pak; return ""; }
const char *sndd_strm_name(const SndDrv *a, int pak) { (void)a; (void)pak; return ""; }

static SndDrv g_iopa;
int snd_driver_init(const SnddConfig *cfg) { return sndd_init(&g_iopa, cfg); }
void snd_driver_frame(const uint8_t *send, uint8_t *reply, uint32_t frame_ms) { sndd_rpc_frame(&g_iopa, send, reply, frame_ms); }
SndDrv *snd_driver_instance(void) { return &g_iopa; }

/* the runtime of the driver (statistics, memory) for reports */
IopRt *sndd_gen_runtime(SndDrv *a)
{
    Glue *g = glue_of(a);
    return g ? g->rt : 0;
}
