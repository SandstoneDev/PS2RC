/* hle_host.c - host platform layer (x86-64 Linux / WSL) of the HLE runtime + the host harness entry point:
 * loads the boot ELF into a 32 MB guest RAM, installs the HLE kernel and runs the recompiled program from its ELF
 * entry point on guest thread 1.
 *
 * usage: ps2rc_host --elf <boot ELF> --disc <extracted disc dir> --discmap discmap.txt [--meta iso_meta.bin]
 *                  [--log file] [--max-seconds N] [--max-frames N] [--verbose 0|1|2] [--stuck-seconds N]
 *                  [--vcallms-nohit | --vcallms strict|nohit|hle] [--mainloop-limit N]
 *                  [--audio model|silent]  the pack's IOP audio: its model (default when built with AUDIO=1; env
 *                                          HLE_AUDIO_WAV=<file> captures its 48 kHz mix) or the silent responder
 *                  [--frames DIR]          render capture (a pack with a renderer boundary): the 2D draws
 *                                          rasterised to DIR/frame_<flip>.png (+ frames.txt)
 *                  [--frame-every N]       ... every N-th flip (default 30; identical pictures are skipped)
 *                  [--frame-max N]         ... at most N PNGs (default 400)
 *                  [--render-log N]        log the first N calls of every renderer boundary hook (default 4)
 *                  [--render-trace N]      log every 2D draw of flip N (state, first vertices, caller)
 *                  [--psp-layout]          guest RAM ends at the pack's ram_end_psp and the code region (the pack's
 *                                          [memory] code) is not loaded (pattern-filled, checked at exit): the PSP
 *                  [--ram-end HEX]         other RAM end
 *                  [--paced]               idle waits for real time like the PSP
 *                  [--pad T0-T1:MASK,...]  scripted button presses by virtual time (PS2 bits, active high)
 *                  [--memcard DIR]         a PS2 memory card in slot 1 = the directory tree DIR (hle/core/hle_memcard.c;
 *                                          without it: no card)
 *                  [--poke T:ADDR=BYTE,...] at virtual time T (s) write BYTE to guest ADDR (hex), once - e.g. a
 *                                          game's menu flag, to reach a screen without scripting the pad
 *                  [--mmio-profile FILE]   which generated functions accessed hardware registers (trap set)
 *                  [--alias-report FILE]   check-mode builds: hardware accesses that went through plain accessors
 *                  [--disc-usage FILE]     disc files the run read
 *                  [--disc-override ISO_PATH=FILE]   serve one disc file from FILE (same size; e.g. a settings file)
 *                  [--sample N]            every N s of host time: log the call chain of the running guest code
 *                  [--sample-word [*]HEX]  ... and that guest word (with *: the 4 words it points to)
 *                  [--selftest]            at the pack's [runtime] selftest_at: the pack's hle_selftest_run
 *                  [--watch HEX]           log every change of that guest word (checked at safe points) + call chain
 *                  [--prof]                profiler (hle/core/hle_prof.c): zone tables in the log
 *                  [--prof-summary N]      ... a summary every N s of host time (default 10; --prof-summary-ms N)
 *                  [--prof-bin FILE]       ... per-iteration capture of up to 4000 main-loop iterations into FILE
 * Exit codes: see hle_main.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
/* backtrace() comes with <execinfo.h>: glibc / macOS have it, musl (Alpine) and MinGW do not - then no chain is
   printed (HLE_HAVE_EXECINFO below) */
#if defined(__has_include)
# if __has_include(<execinfo.h>)
#  include <execinfo.h>
#  define HLE_HAVE_EXECINFO 1
# endif
#endif
#ifndef HLE_HAVE_EXECINFO
# define HLE_HAVE_EXECINFO 0
#endif
#include <fcntl.h>
#include <fenv.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <xmmintrin.h>
#include "hle.h"
#include "hle_prof.h"
#include "hle_memcard.h"                                     /* --memcard */

static FILE *logf_;
static double t_start;
static int paced;
static const char *mmio_profile_path, *alias_report_path, *disc_usage_path;
static int psp_layout;

/* ------------------------------------------------------------------ time */
double hle_host_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9 - t_start;
}

/* (hle_bl.c): --paced-main = paced only from the first main-loop iteration (the boot runs unpaced; at the
   first main-loop pace the clock is aligned with virtual time, minus HLE_BL_SIMDEBT ms = the lag a slow PSP boot leaves:
   b52 HW ~30,000), so the pacing simulation reproduces the hardware's situation; pace_shift = the moved clock */
static double host_pace_shift;
static int host_pace_aligned;
uint64_t hle_plat_pace_now_us(void)
{
    double n = hle_host_seconds() - host_pace_shift;
    return n > 0 ? (uint64_t)(n * 1e6) : 0u;
}
void hle_plat_pace_adjust(int64_t us) { host_pace_shift += (double)us / 1e6; }

void hle_plat_pace(uint64_t vtime)
{
    if (!paced) return;
    if (paced == 2) {
        if (!hle_mainloop_iters) return;
        if (!host_pace_aligned) {
            host_pace_aligned = 1;
            const char *e = getenv("HLE_BL_SIMDEBT");
            host_pace_shift = hle_host_seconds() - (double)vtime / EE_HZ - (e ? atof(e) / 1000.0 : 0.0);
            LOG("host --paced-main: pacing from the main loop on, real clock aligned to virtual %.3f s %s\n",
                (double)vtime / EE_HZ, e ? "+ HLE_BL_SIMDEBT" : "");
        }
    }
    double due = (double)vtime / EE_HZ, now = hle_host_seconds() - host_pace_shift;
    if (due > now) {
        struct timespec ts = {(time_t)(due - now), (long)(((due - now) - (time_t)(due - now)) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

/* ------------------------------------------------------------------ logging */
void hle_plat_log_write(const char *s, size_t n) { fwrite(s, 1, n, logf_ ? logf_ : stderr); }
void hle_plat_log_flush(void) { fflush(logf_); }
void hle_plat_milestone(const char *line) { (void)line; }
void hle_plat_result_line(const char *line) { (void)line; }             /* the log has the lines */
int hle_plat_shot(const char *tag, char *why, unsigned why_n)
{
    (void)tag;
    if (why && why_n) snprintf(why, why_n, "host: no screen");
    return -1;
}

/* profiler platform part (build 22): no hardware counters on the host; prof.bin goes to --prof-bin */
static const char *prof_bin_path;
int hle_plat_hw_totals(uint64_t tot[20], uint64_t *us) { (void)tot; (void)us; return 0; }
/* host input: nothing pressed, unless --pad "T0-T1:MASK,..." scripts presses by virtual time (seconds; MASK = PS2
   button bits, active high, as hle/psp/hle_pad.c returns them, e.g. 4000 = Cross, 0008 = START) - deterministic.
   "T0-T1:MASK@XX,YY" also holds the LEFT stick at XX,YY (hex 00-ff, 80 = centre; 80,00 = full forward) for that
   interval, e.g. 60-90:0@80,00 pushes the stick forward for 30 s. */
#define MAX_PAD_EV 32
static struct { double t0, t1; uint16_t m; int lx, ly; } pad_ev[MAX_PAD_EV];
static int n_pad_ev;
static void pad_script(const char *s)
{
    while (*s && n_pad_ev < MAX_PAD_EV) {
        double a, b;
        unsigned m, x, y;
        int n = 0;
        if (sscanf(s, "%lf-%lf:%x%n", &a, &b, &m, &n) != 3) break;
        pad_ev[n_pad_ev].t0 = a;
        pad_ev[n_pad_ev].t1 = b;
        pad_ev[n_pad_ev].m = (uint16_t)m;
        pad_ev[n_pad_ev].lx = pad_ev[n_pad_ev].ly = -1;
        s += n;
        if (*s == '@' && sscanf(s, "@%x,%x%n", &x, &y, &n) == 2) {
            pad_ev[n_pad_ev].lx = (int)(x & 0xFF);
            pad_ev[n_pad_ev].ly = (int)(y & 0xFF);
            s += n;
        }
        n_pad_ev++;
        if (*s == ',') s++;
    }
}
/* --poke "T:ADDR=BYTE,..." - one guest byte written once when the virtual time reaches T (checked at every pad read
   = once per game frame) */
#define MAX_POKES 32
static struct { double t; uint32_t a; uint8_t v, done; } pokes[MAX_POKES];
static int n_pokes;
static void poke_script(const char *s)
{
    while (*s && n_pokes < MAX_POKES) {
        double t;
        unsigned a, v;
        int n = 0;
        if (sscanf(s, "%lf:%x=%x%n", &t, &a, &v, &n) != 3) break;
        pokes[n_pokes].t = t;
        pokes[n_pokes].a = a;
        pokes[n_pokes].v = (uint8_t)v;
        n_pokes++;
        s += n;
        if (*s == ',') s++;
    }
}
static void pokes_apply(double t)
{
    for (int i = 0; i < n_pokes; i++)
        if (!pokes[i].done && t >= pokes[i].t) {
            pokes[i].done = 1;
            LOG("poke (script): guest byte %08x = %02x (was %02x) at vtime %.3f s, frame %llu\n", pokes[i].a, pokes[i].v,
                gb(pokes[i].a), t, (unsigned long long)hle_frame);
            sb_(pokes[i].a, pokes[i].v);
        }
}

uint16_t hle_plat_pad_read(uint8_t sticks[4])
{
    double t = (double)hle_vtime / EE_HZ;
    if (n_pokes) pokes_apply(t);
    uint16_t m = 0;
    for (int i = 0; i < n_pad_ev; i++)
        if (t >= pad_ev[i].t0 && t < pad_ev[i].t1) {
            m |= pad_ev[i].m;
            if (pad_ev[i].lx >= 0) { sticks[2] = (uint8_t)pad_ev[i].lx; sticks[3] = (uint8_t)pad_ev[i].ly; }
        }
    static uint16_t last;
    static uint8_t last_lx = 0x80, last_ly = 0x80;
    if (m != last || sticks[2] != last_lx || sticks[3] != last_ly)
        LOG("pad (script): buttons %04x left stick %02x,%02x at vtime %.3f s, frame %llu\n", m, sticks[2], sticks[3],
            t, (unsigned long long)hle_frame);
    last = m;
    last_lx = sticks[2];
    last_ly = sticks[3];
    return m;
}
int hle_plat_hw_door(uint32_t raw[20]) { (void)raw; return 0; }
void *hle_plat_prof_alloc(size_t n) { return malloc(n); }
int hle_plat_write_file(const char *name, const void *data, size_t n)
{
    FILE *f = fopen(prof_bin_path && !strcmp(name, "prof.bin") ? prof_bin_path : name, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, n, f);
    return fclose(f) == 0 && w == n ? 0 : -1;
}

/* ------------------------------------------------------------------ stacks / coroutines */
void *hle_plat_stack_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    mprotect(p, 65536, PROT_NONE);                          /* guard: host stack overflow -> SIGSEGV */
    return p;
}

/* high-water mark: the lowest resident page of the (never released) NORESERVE mapping */
size_t hle_plat_stack_used(void *stack, size_t size)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), n = size / pg;
    unsigned char *vec = malloc(n);
    if (!vec || mincore(stack, size, vec) != 0) { free(vec); return 0; }
    size_t lowest = n;
    for (size_t i = 65536 / pg; i < n; i++) if (vec[i] & 1) { lowest = i; break; }
    free(vec);
    return lowest < n ? size - lowest * pg : 0;
}

void hle_plat_ctx_make(hle_hostctx *ctx, void *stack, size_t size, void (*fn)(void))
{
    getcontext(ctx);
    ctx->uc_stack.ss_sp = stack;
    ctx->uc_stack.ss_size = size;
    ctx->uc_link = NULL;
    makecontext(ctx, fn, 0);
}

void hle_plat_ctx_swap(hle_hostctx *save, hle_hostctx *load) { swapcontext(save, load); }

void *hle_plat_big_alloc(size_t size)
{
    void *p = calloc(1, size);
    if (!p) { LOG("FATAL: out of memory (%zu bytes)\n", size); hle_exit(4); }
    return p;
}

/* ------------------------------------------------------------------ files */
#define MAX_FILES 1024
static int fds[MAX_FILES];
static int nfds;

int hle_plat_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0 || nfds >= MAX_FILES) return -1;
    fds[nfds] = fd;
    return nfds++;
}

int64_t hle_plat_pread(int h, uint64_t off, void *dst, uint32_t n)
{
    if (h < 0 || h >= nfds) return -1;
    return pread(fds[h], dst, n, (off_t)off);
}

int64_t hle_plat_size(int h)
{
    struct stat st;
    if (h < 0 || h >= nfds || fstat(fds[h], &st) != 0) return -1;
    return st.st_size;
}

const char *hle_plat_disc_root(void) { return ""; }

/* ------------------------------------------------------------------ diagnostics */
const char *hle_plat_fn_name(void *pc)
{
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname) return di.dli_sname;
    return NULL;
}

void hle_plat_backtrace(void)
{
#if HLE_HAVE_EXECINFO
    void *frames[64];
    int n = backtrace(frames, 64);
    char **syms = backtrace_symbols(frames, n);
    LOG("host backtrace (guest functions are fn_<addr> / ov<N>_fn_<addr>):\n");
    for (int i = 0; i < n; i++) {
        const char *s = syms ? syms[i] : "?";
        const char *p = strchr(s, '(');
        LOG("  %s\n", p ? p : s);
    }
#else
    LOG("host backtrace: not available on this C library\n");
#endif
}

/* trap-set profiling: generated functions that reached the register model (open addressing on the return address) */
#define PROF_SLOTS 8192
static struct { void *ra; uint32_t regions; uint64_t count; uint32_t first; } prof[PROF_SLOTS];
static int prof_n;

static uint32_t region_bit(uint32_t a)
{
    uint32_t pa = (a >> 28) >= 0xA ? a & 0x1FFFFFFFu : a;
    if (pa >= 0x10000000u && pa < 0x10010000u) return 1;      /* EE registers */
    if (pa >= 0x11000000u && pa < 0x11010000u) return 2;      /* VU memory */
    if (pa >= 0x12000000u && pa < 0x12002000u) return 4;      /* GS privileged */
    if (pa >= 0x1FC00000u && pa < 0x20000000u) return 8;      /* BIOS */
    return 16;                                                /* other (unmapped / above the RAM end) */
}

static void prof_add(void *ra, uint32_t a)
{
    size_t h = ((size_t)ra >> 2) * 2654435761u % PROF_SLOTS;
    for (int i = 0; i < PROF_SLOTS; i++, h = (h + 1) % PROF_SLOTS) {
        if (prof[h].ra == ra) { prof[h].count++; prof[h].regions |= region_bit(a); return; }
        if (!prof[h].ra) {
            if (prof_n >= PROF_SLOTS * 3 / 4) return;
            prof[h].ra = ra;
            prof[h].count = 1;
            prof[h].regions = region_bit(a);
            prof[h].first = a;
            prof_n++;
            return;
        }
    }
}

void hle_plat_note_mmio(void *ra, uint32_t a)
{
    if (mmio_profile_path) prof_add(ra, a);
}

/* check-mode header (ps2rc/pipeline/patch_rt_header.py --mode check): a plain (masked) accessor saw an address that is
   neither RAM, a RAM mirror nor the scratchpad - on the PSP that access would alias into low RAM */
static struct { void *ra; uint32_t a; uint64_t count; } alias_tab[1024];
static int alias_n;
__attribute__((noinline)) void hle_alias_check(uint32_t a)
{
    uint32_t top = a >> 28;
    if (top == 2 || top == 3 || ((top >= 8 && top <= 0xB) && (a & 0x1FFFFFFFu) < HLE_RAM_SIZE)) return;
    if (a >= 0x70000000u && a < 0x70004000u) return;
    void *ra = __builtin_return_address(0);
    for (int i = 0; i < alias_n; i++) if (alias_tab[i].ra == ra) { alias_tab[i].count++; return; }
    if (alias_n < 1024) {
        alias_tab[alias_n].ra = ra;
        alias_tab[alias_n].a = a;
        alias_tab[alias_n].count = 1;
        alias_n++;
        if (alias_n <= 20) {
            const char *s = hle_plat_fn_name(ra);
            LOG("ALIAS: plain accessor in %s reached %08x (would alias into low RAM on the PSP)\n", s ? s : "?", a);
        }
    }
}

/* symbol of a host pc, else "?+0x<offset in the executable>" (static functions: resolve with addr2line -f -e) */
static const char *pc_name(void *pc, char *buf, size_t n)
{
    Dl_info di;
    if (dladdr(pc, &di)) {
        if (di.dli_sname) return di.dli_sname;
        snprintf(buf, n, "?+0x%lx", (unsigned long)((char *)pc - (char *)di.dli_fbase));
        return buf;
    }
    return "?";
}

static void write_profiles(void)
{
    char nb[64];
    if (mmio_profile_path) {
        FILE *f = fopen(mmio_profile_path, "w");
        if (f) {
            for (int i = 0; i < PROF_SLOTS; i++) {
                if (!prof[i].ra) continue;
                fprintf(f, "%s %x %llu %08x\n", pc_name(prof[i].ra, nb, sizeof nb), prof[i].regions,
                        (unsigned long long)prof[i].count, prof[i].first);
            }
            fclose(f);
        }
    }
    if (alias_report_path) {
        FILE *f = fopen(alias_report_path, "w");
        if (f) {
            for (int i = 0; i < alias_n; i++)
                fprintf(f, "%s %08x %llu\n", pc_name(alias_tab[i].ra, nb, sizeof nb), alias_tab[i].a,
                        (unsigned long long)alias_tab[i].count);
            fclose(f);
        }
        LOG("alias check: %d plain-accessor sites reached hardware addresses\n", alias_n);
    }
    {   /* back-end v5s4 host builds are compiled with -DRMB_CHECK (mk/build.sh host, hle/host/rmb_check.c):
           base+offset accesses that would leave the guest window */
        extern unsigned long hle_rmb_mismatches __attribute__((weak));
        extern const int hle_rmb_checked __attribute__((weak));
        if (&hle_rmb_checked) LOG("maskbase check: %lu base+offset accesses outside the guest window\n", hle_rmb_mismatches);
    }
    hle_disc_usage_dump(disc_usage_path);
}

/* --psp-layout: the code region is not loaded (like the PSP); it holds a pattern that must survive the run */
static void hole_check(void)
{
    if (!psp_layout) return;
    uint32_t bad = 0, first = 0;
    for (uint32_t a = HLE_CODE_LO; a < HLE_CODE_HI; a += 4)
        if (gw(a) != 0xA5A5A5A5u) { if (!bad++) first = a; }
    LOG("code-region hole check: %u words changed%s", bad, bad ? "" : "\n");
    if (bad) LOG(" (first at %08x: the game wrote its code region as data)\n", first);
}

void hle_plat_exit(int code)
{
    hole_check();
    write_profiles();
    fflush(NULL);
#ifdef HLE_GPROF
    { extern void _mcleanup(void); _mcleanup(); }      /* gprof build: write gmon.out (_exit skips it) */
#endif
    _exit(code);
}

/* ------------------------------------------------------------------ watchdog (SIGALRM, 1 Hz): guest code that runs
   without reaching a safe point (no syscall, hook, hardware access or loop back-edge) for stuck_seconds, or a run far
   past --max-seconds, is reported with the host backtrace of the stuck guest thread (the handler runs on its stack) */
static int stuck_seconds = 60;
static int sample_seconds;                       /* --sample N: log the running guest call chain every N s */
static uint32_t sample_words[8];                 /* --sample-word [*]HEX: guest word (or the 4 words it points to) */
static int sample_deref[8], n_sample_words;
static void sample_dump(void)
{
    for (int i = 0; i < n_sample_words; i++) {
        uint32_t a = sample_words[i], v = gw(a);
        if (!sample_deref[i]) { LOG("  word %08x = %08x\n", a, v); continue; }
        if (v < hle_ram_end - 16)
            LOG("  *%08x = %08x -> %08x %08x %08x %08x\n", a, v, gw(v), gw(v + 4), gw(v + 8), gw(v + 12));
        else
            LOG("  *%08x = %08x (not RAM)\n", a, v);
    }
}
static void on_alarm(int sig)
{
    (void)sig;
    static uint64_t last;
    static double last_change, next_sample;
    double now = hle_host_seconds();
    if (sample_seconds && now >= next_sample) {
        if (next_sample > 0) {
            LOG("SAMPLE at host %.0f s, vtime %.3f s, frame %llu, guest thread %d%s, syscalls %llu:\n", now,
                (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame, hle_cur ? hle_cur->id : -1,
                hle_in_intr ? " (interrupt context)" : "", (unsigned long long)hle_stat_syscalls);
            sample_dump();
            hle_plat_backtrace();
        }
        next_sample = now + sample_seconds;
    }
    if (hle_progress != last) { last = hle_progress; last_change = now; }
    int stuck = now - last_change >= stuck_seconds;
    if (!stuck && now < hle_max_seconds + 30) return;
    LOG("WATCHDOG: %s (%.0f s without a safe point)\n", stuck ? "guest code stuck" : "run far past the time limit",
        now - last_change);
    hle_set_exit_reason(stuck ? "watchdog: guest code stuck" : "watchdog: run far past the time limit");
    hle_backtrace_dump(hle_cur ? &hle_cur->c : NULL);
    hle_exit(7);
}

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    LOG("HOST SIGNAL %d (%s) at address %p in guest thread %d%s\n", sig, strsignal(sig), si->si_addr,
        hle_cur ? hle_cur->id : -1, hle_in_intr ? " (interrupt context)" : "");
    hle_set_exit_reason("host signal");
    hle_backtrace_dump(hle_cur ? &hle_cur->c : NULL);
    hle_exit(6);
}

static void install_signals(void)
{
    static char altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_sigaction = on_signal;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
    signal(SIGINT, SIG_DFL);
}

static void on_term(int sig)
{
    LOG("terminated by signal %d\n", sig);
    hle_set_exit_reason("terminated");
    hle_backtrace_dump(hle_cur ? &hle_cur->c : NULL);
    hle_exit(0);
}

static void install_watchdog(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_handler = on_alarm;
    act.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &act, NULL);
    struct itimerval it = {{1, 0}, {1, 0}};
    setitimer(ITIMER_REAL, &it, NULL);
}

/* ------------------------------------------------------------------ ELF loading */
static int load_elf(const char *path, uint32_t *entry)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    uint8_t eh[52];
    if (fread(eh, 1, 52, f) != 52 || memcmp(eh, "\x7f" "ELF", 4) != 0) { fclose(f); return 0; }
    uint32_t phoff, e;
    uint16_t phentsize, phnum;
    memcpy(&e, eh + 24, 4);
    memcpy(&phoff, eh + 28, 4);
    memcpy(&phentsize, eh + 42, 2);
    memcpy(&phnum, eh + 44, 2);
    for (int i = 0; i < phnum; i++) {
        uint32_t ph[8];
        fseek(f, phoff + i * phentsize, SEEK_SET);
        if (fread(ph, 4, 8, f) != 8) { fclose(f); return 0; }
        if (ph[0] != 1 || ph[4] == 0) continue;               /* PT_LOAD with file data (overlay slots have none) */
        if (ph[2] + ph[5] > hle_ram_end) { fclose(f); return 0; }
        uint32_t skip = 0;
        if (psp_layout && ph[2] <= HLE_CODE_LO && ph[2] + ph[4] > HLE_CODE_HI)
            skip = HLE_CODE_HI - ph[2];                       /* the PSP loads only the data part */
        fseek(f, ph[1] + skip, SEEK_SET);
        if (fread(hle_ram + ph[2] + skip, 1, ph[4] - skip, f) != ph[4] - skip) { fclose(f); return 0; }
        LOG("loaded segment %08x filesz %x memsz %x%s\n", ph[2], ph[4], ph[5],
            skip ? " (code region skipped: PSP layout)" : "");
    }
    fclose(f);
    *entry = e;
    return 1;
}

int main(int argc, char **argv)
{
    const char *disc = NULL, *discmap = NULL, *logpath = NULL, *meta = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--elf") && i + 1 < argc) hle_elf_path = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--discmap") && i + 1 < argc) discmap = argv[++i];
        else if (!strcmp(argv[i], "--meta") && i + 1 < argc) meta = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) hle_max_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) hle_max_frames = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--mainloop-limit") && i + 1 < argc) hle_opt_mainloop_limit = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--verbose") && i + 1 < argc) hle_verbose = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stuck-seconds") && i + 1 < argc) stuck_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) hle_selftest_pending = 1;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && hle_watch_n < 4)
            hle_watch_addr[hle_watch_n++] = (uint32_t)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--sample-word") && i + 1 < argc && n_sample_words < 8) {
            const char *s = argv[++i];
            sample_deref[n_sample_words] = s[0] == '*';
            sample_words[n_sample_words++] = (uint32_t)strtoul(s + (s[0] == '*'), 0, 16);
        }
        else if (!strcmp(argv[i], "--vcallms-nohit")) hle_opt_vcallms = VCALLMS_NOHIT;
        else if (!strcmp(argv[i], "--vcallms") && i + 1 < argc) {
            const char *m = argv[++i];
            hle_opt_vcallms = !strcmp(m, "nohit") ? VCALLMS_NOHIT : !strcmp(m, "hle") ? VCALLMS_HLE : VCALLMS_STRICT;
        }
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) hle_opt_audio_model = !strcmp(argv[++i], "model");
        else if (!strcmp(argv[i], "--psp-layout")) { psp_layout = 1; hle_ram_end = HLE_PACK_RAM_END_PSP; }
        else if (!strcmp(argv[i], "--ram-end") && i + 1 < argc) hle_ram_end = (uint32_t)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--paced")) paced = 1;
        else if (!strcmp(argv[i], "--paced-main")) paced = 2;
        else if (!strcmp(argv[i], "--no-idle-skip")) hle_opt_idle_skip = 0;
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) pad_script(argv[++i]);
        else if (!strcmp(argv[i], "--memcard") && i + 1 < argc) hle_mc_host_dir = argv[++i];
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) poke_script(argv[++i]);
        else if (!strcmp(argv[i], "--prof")) hle_pz_opt_on = 1;
        else if (!strcmp(argv[i], "--prof-summary") && i + 1 < argc) hle_pz_opt_summary_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof-summary-ms") && i + 1 < argc) hle_pz_opt_summary_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof-bin") && i + 1 < argc) { prof_bin_path = argv[++i]; hle_pz_opt_capture = 4000; }
        else if (!strcmp(argv[i], "--mmio-profile") && i + 1 < argc) mmio_profile_path = argv[++i];
        else if (!strcmp(argv[i], "--alias-report") && i + 1 < argc) alias_report_path = argv[++i];
        else if (!strcmp(argv[i], "--disc-usage") && i + 1 < argc) disc_usage_path = argv[++i];
        else if (!strcmp(argv[i], "--disc-override") && i + 1 < argc) {
            char *spec = argv[++i], *eq = strchr(spec, '=');
            if (!eq) { fprintf(stderr, "--disc-override ISO_PATH=FILE\n"); return 2; }
            *eq = 0;
            hle_disc_add_override(spec, eq + 1);
        }
        else if (hle_pack_host_arg && hle_pack_host_arg(argc, argv, &i)) ;   /* the game pack's options */
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!hle_elf_path || !disc || !discmap) {
        fprintf(stderr, "usage: %s --elf " HLE_PACK_BOOT_ELF " --disc DIR --discmap FILE [--log FILE] [--max-seconds N] "
                "[--max-frames N] [--verbose N] [--psp-layout] ...\n", argv[0]);
        return 2;
    }
    t_start = 0;
    t_start = hle_host_seconds();
    logf_ = logpath ? fopen(logpath, "w") : stdout;
    if (!logf_) { perror(logpath); return 4; }
    setvbuf(logf_, NULL, _IOFBF, 1 << 20);
    install_signals();
    signal(SIGTERM, on_term);
    /* R5900 FPU model: round toward zero, flush denormals to zero (as tests/poc2/host) */
    fesetround(FE_TOWARDZERO);
    _mm_setcsr(_mm_getcsr() | 0x8040);
    void *ram = NULL;
    if (posix_memalign(&ram, 4096, HLE_RAM_SIZE) != 0) { LOG("cannot allocate guest RAM\n"); return 4; }
    hle_ram = ram;
    memset(hle_ram, 0, HLE_RAM_SIZE);
    if (psp_layout) memset(hle_ram + HLE_CODE_LO, 0xA5, HLE_CODE_HI - HLE_CODE_LO);
    uint32_t entry = 0;
    if (!load_elf(hle_elf_path, &entry)) { LOG("cannot load ELF %s\n", hle_elf_path); return 4; }
    LOG("PS2RC host harness: ELF %s entry %08x, disc %s, guest RAM end %08x%s (host %.1f s)\n", hle_elf_path, entry,
        disc, hle_ram_end, psp_layout ? " (PSP layout)" : "", hle_host_seconds());
    char mpath[1024];
    if (!meta) {                                             /* iso_meta.bin next to the disc map */
        snprintf(mpath, sizeof mpath, "%s", discmap);
        char *slash = strrchr(mpath, '/');
        snprintf(slash ? slash + 1 : mpath, sizeof mpath - (slash ? (size_t)(slash + 1 - mpath) : 0), "iso_meta.bin");
        meta = mpath;
    }
    if (!hle_disc_init(disc, discmap, meta)) { LOG("cannot initialise the disc map\n"); return 4; }
    LOG("disc map ready (host %.1f s)\n", hle_host_seconds());
    hle_set_vcallms_mode(hle_opt_vcallms);
    hle_pz_init();                                           /* --prof: zone timing from here */
    hle_boot_init(entry);
    hle_plat_log_flush();
    install_watchdog();
    hle_sched_run();
    LOG("scheduler returned\n");
    hle_exit(0);
}
