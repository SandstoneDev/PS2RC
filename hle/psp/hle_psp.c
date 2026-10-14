/* hle_psp.c - PSP platform layer of the HLE runtime + the EBOOT entry point.
 *
 * Memory:
 *   - the guest RAM window: one block from partition 2, guest 0 .. the pack's ram_end_psp (the game heap must fit
 *     below it; a game's heap set-up may read hle_ram_end through a prepare_gen text patch of the pack);
 *   - only the game's DATA is loaded from its own ELF on the memory stick (the pack's [memory] code end .. data_end);
 *     the guest code region is never read as data, so its memory ("the hole") holds the HLE's big buffers: the
 *     register-model arrays, ... (hle_plat_big_alloc);
 *   - the scratchpad is guest RAM 0x0000-0x3FFF (0x70000000 masked), the HLE kernel area 0x80000-0xFFFFF.
 * Threads: main() only starts the game PSP thread (game_main, GAME_STACK) and sleeps. Every guest thread is a
 *   coroutine (hle_ctx.S) inside the game thread, on a host stack carved from the BOTTOM of the game thread's own
 *   stack (the pool; game_main's own frames use the top GAME_OWN_STACK). Coroutine stacks in a separate memory block
 *   (the hole) do not work: on the PSP-3000 the log stopped at the first sceIo call made from a coroutine and the
 *   console switched off, while PPSSPP ran it.
 *   The EE kernel rules (strict priority, FIFO per priority, switches only at safe points) live in hle_sched.c.
 *   A second PSP thread is the watchdog (no progress for stuck_seconds -> report and stop).
 * Time: deterministic virtual time as on the host; when every guest thread is blocked, the idle step waits until the
 *   real clock (sceKernelGetSystemTimeWide) reaches the next virtual event (hle_plat_pace) instead of jumping - the
 *   guest's VBlank runs at the real 50 Hz PAL rate while it idles. The PSP display VBlank (59.94 Hz) is not used: the
 *   game is PAL; the renderer will sync its flips to the display later.
 * Disc: the extracted disc files the boot needs live in <EBOOT dir>/disc/ (disc_files.txt lists them), addressed
 *   through discmap.txt + iso_meta.bin like on the host; files are opened on demand (sceIoOpen costs 17-34 ms) and
 *   kept open in a small LRU.
 * Output: <EBOOT dir>/session.log (header, the whole HLE log, milestones with virtual/real time, the end of the run)
 *   and the same milestones on screen.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspsysmem.h>
#include <psppower.h>
#include <psprtc.h>                             /* run info (PSP clock) */
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_prof.h"                           /* build 22: profiler */
#include "ps2rc_exc.h"                          /* the crash handler (src/psp_exc, ps2rc_exc.prx next to the EBOOT) */
#include "hle_hitch.h"                          /* hitch sources (disc, log) */
#include "hle_mem_psp.h"                        /* memory mode diagnostics, volatile partition, heap_extra_kb */
#include "hle_aw_psp.h"                         /* results block / results_<seq>.txt / shots, cpu_clock_mhz */
/* profiler, PSP part (psp/hle_prof_psp.c): hardware counters through ps2rc_exc.prx, off unless ps2rc.cfg prof_hw >= 1 */
int hle_psp_is_emulator(void);
Ps2rcPerfBlock *hle_psp_perf_block(void);
void hle_psp_perf_status(char *out, unsigned n);
int hle_psp_perf_shutdown(void);                /* -1 none, 1 left the module, 0 did not */
void hle_psp_perf_exit_info(char *out, unsigned n);
#ifdef HLE_HAVE_AUDIO_MODEL
int hle_me_running(void);                       /* hle/psp/hle_me.c (AUDIO=1 only): an ME loop runs */
int hle_me_stop(int timeout_ms);                /* ... exit request, wait, then held in reset (1 = acknowledged) */
int hle_me_power_shutdown(int *lock_held);      /* the ME power callback out at exit (az_exit=1) */
#include "hle_me.h"                             /* power-callback counters + the PRX's sleep counters */
#endif
int hle_ge_shutdown(void) __attribute__((weak));   /* the pack's GE back-end, if any */
#ifdef HLE_HAVE_AUDIO_MODEL
int hle_audio_psp_stop(void);                   /* the pack's PSP audio output */
#endif
#ifdef HLE_SPLASH
int hle_splash_show(const char *dir);           /* psp/hle_splash.c: community QR splash, build.sh SPLASH=1 */
#endif

PSP_MODULE_INFO(HLE_PACK_PSP_MODULE, 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
PSP_HEAP_SIZE_KB(768);

#ifndef HLE_BUILD_NUM
#define HLE_BUILD_NUM 21
#endif
#define GAME_OWN_STACK (256u * 1024u)           /* game_main + the HLE boot path (top of the game thread's stack) */
#define GAME_POOL (HLE_STACK_MAIN + 10u * HLE_STACK_THREAD)   /* coroutine stacks: thread 1 + 10 more guest threads */
#define GAME_STACK (GAME_OWN_STACK + GAME_POOL + 4096u)
#ifndef HLE_TRANSLATION
#define HLE_TRANSLATION "unknown"
#endif
#define WINDOW_SIZE HLE_PACK_RAM_END_PSP       /* guest RAM end (the pack's [memory] ram_end_psp) */
#define DATA_FNV_EXPECTED HLE_PACK_DATA_FNV     /* FNV-1a 64 of guest [code end, bss end) after loading ([verify]) */

void hle_ctx_swap_asm(uint32_t *save, const uint32_t *load);
void hle_ctx_trampoline(void);

static char dir[256];
/* run option disc_dir=<dir/> - where the game's data lives (the boot ELF, discmap.txt, iso_meta.bin, disc/), e.g. the
   main build's folder, so that A/B variant folders hold only EBOOT.PBP + the run options + ps2rc_exc.prx;
   empty (default) = the EBOOT's own folder. The run options, session.log, prof.bin, ps2rc_exc.prx stay in the EBOOT
   folder. */
static char disc_dir[256];
static const char *data_dir(void) { return disc_dir[0] ? disc_dir : dir; }
static char log_path[320];
static int log_ok;                          /* session.log could be created */
static char logbuf[32768];
static volatile unsigned loglen;
static volatile int log_busy;               /* the main thread is inside the log code (the watchdog does not commit) */
static int log_sync = 1;                    /* boot: every line is committed to the card at once */
static SceInt64 t0_us;
static int opt_pace = 1, opt_stuck = 120, opt_screen = 1;
static int opt_exit_auto;                   /* exit_auto=N - EMULATOR ONLY: exit N s after the end
                                               without X (PPSSPP checks of the exit sequence; ignored on the PSP) */
static int opt_log_sync = 1;                /* log_sync=0: buffer while the guest runs */
/* log_sync=2 = buffered while the main loop runs, committed by the watchdog thread every log_commit_ms (default
   500 ms; at once at boot, milestones, crash and exit): the game thread never waits for the memory stick (with
   log_sync=1 up to 107 ms per iteration in io.log were measured) */
static int opt_log_commit_ms = 500;
static uint64_t st_log_commits_game, st_log_commits_wd, st_log_game_us;
extern int hle_cdasync_opt __attribute__((weak));   /* the pack's async disc reader, if any: cfg disc_async=1 */
static uint8_t *hole_lo, *hole_cur, *hole_hi;
static uint8_t *pool_lo, *pool_cur, *pool_hi;   /* coroutine stacks: the bottom of the game thread's own stack */
static uint8_t *thr_lo, *thr_hi;                /* the game thread's whole stack */
static SceUID game_thid = -1;
static unsigned free_kb_start, free_kb_window;
static volatile int exiting;
/* cfg heap_extra_kb=N (-1 / "max" = as much as the reserve allows) - the guest
   RAM end (= the game heap's end: the pack's text patch) N KB above the pack's PSP RAM end, at most 32 MB - 64 KB
   (the PS2 has 32 MB); mem_volatile=1 - our big buffers into the volatile 4 MB (hle_mem_psp.c), the audio block into
   the hole; mem_reserve_kb=K - user memory kept free after the window (default: estimated from audio / movies /
   prof_capture); mem_fake_free_kb=K - TEST ONLY: pretend K KB free at start (checks the 24 MB mode message). Defaults 0:
   the window ends at the pack's PSP RAM end, nothing in the volatile partition. */
static int opt_heap_extra_kb, opt_mem_volatile, opt_mem_reserve_kb = -1, opt_mem_fake_free_kb;
static int cfg_movies = 1, cfg_audio = 1;          /* read here only for the reserve estimate (hle_movie / hle_audio_psp
                                                      parse their own keys) */
static uint32_t window_size = 0x01BC0000u;         /* = WINDOW_SIZE + heap extra */
static unsigned heap_extra_kb_got, mem_reserve_kb_used;
static int vol_lock_rc = 0x7FFFFFFF;
static unsigned big_in_vol_kb, big_in_part_kb;
static uint32_t main_sp, main_stack_lo, main_stack_hi;   /* the module's main thread (main()) */
/* ps2rc.cfg results=0|1 (default -1 = on for scripted runs, i.e.
   mainloop_iterations > 0): the end-of-run results block in session.log + results_<seq>.txt + shot_<seq>_results.bmp
   (the final screen) in the EBOOT folder; results_keep=N files of each kind kept (default 20, oldest deleted) */
static int opt_results = -1, opt_results_keep = 20;
static unsigned run_seq;                           /* `run: sequence N` of this run (0 = unknown) */
/* az_exit=1 - the exit without stop + unload of ps2rc_exc.prx (its exit
   mode: the ME sysevent handlers stay valid and only record into the exit marker until the kernel reboot), the ME power
   callback unregistered, the volatile partition given back but its scePowerLock kept until the reboot; az_marker=1 -
   at boot, read + log the previous exit's marker (ps2rc_exc.h "exit mode"). Both default 0 = unchanged. */
static int opt_az_exit, opt_az_marker;
static int az_crashed;                             /* a crash report / fatal error ran (cpu_auto) */
static char run_info[400];
static int results_written, results_shot_done;
static const char *results_tag = "results";

/* ------------------------------------------------------------------ logging
 * The memory-stick FAT driver updates a file's size only when the file is closed: an EBOOT that crashes with
 * session.log open leaves a 0-byte file (build 12 on the PSP-3000). So every commit opens session.log for append,
 * writes and closes it. Until the guest runs (log_sync) each line is committed at once; afterwards lines are
 * buffered and committed at every milestone, when the buffer fills, every 2 s from the watchdog thread and at exit. */
static SceUID log_sema = -1;               /* serialises commits of the main and the watchdog thread (line order) */
static char wdbuf[sizeof logbuf];

/* build 17: which sceIo call the game thread is in (shown by the watchdog's progress lines: builds 14-16 stopped after
   the same log line with no progress line at all - a game thread stuck inside a log commit would block the watchdog
   on log_sema, so the watchdog now never waits for it more than 0.5 s and always prints on screen) */
enum { IO_NONE, IO_LOG_OPEN, IO_LOG_WRITE, IO_LOG_CLOSE, IO_DISC_OPEN, IO_DISC_READ, IO_DISC_CLOSE };
static const char *const io_names[] = {"none", "log open", "log write", "log close", "disc open", "disc seek/read",
                                       "disc close"};
static volatile int io_state;
static volatile SceInt64 io_t0;
static void io_mark(int s)
{
    if (s) io_t0 = sceKernelGetSystemTimeWide();
    io_state = s;
}

/* build 18: on the PSP-3000 session.log stopped after the same line in builds 14-17 while the game ran on to the end
   (the screen showed it): the log's sceIoOpen failed once the game had its disc files open (PPSSPP allows more open
   files). A failed open now closes the least recently used disc file and retries; failures are counted. */
static int vf_close_lru(void);
static unsigned log_open_fails, log_open_recovered;
static int log_open_err;

static void log_commit_unlocked(const char *s, unsigned n)
{
    io_mark(IO_LOG_OPEN);
    SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    for (int tries = 0; fd < 0 && (uint32_t)fd != 0x80010002u && tries < 4; tries++) {   /* not ENOENT */
        log_open_fails++;
        log_open_err = fd;
        if (!vf_close_lru()) break;
        fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) log_open_recovered++;
    }
    if (fd >= 0) {
        io_mark(IO_LOG_WRITE);
        sceIoWrite(fd, s, n);
        io_mark(IO_LOG_CLOSE);
        sceIoClose(fd);
    }
    io_mark(IO_NONE);
}

static void log_commit(const char *s, unsigned n)
{
    if (!log_ok || !n) return;
    int gt = game_thid >= 0 && sceKernelGetThreadId() == game_thid;   /* hitch source "log" */
    SceInt64 t0 = gt ? sceKernelGetSystemTimeWide() : 0;
    if (log_sema >= 0) sceKernelWaitSema(log_sema, 1, NULL);
    log_commit_unlocked(s, n);
    if (log_sema >= 0) sceKernelSignalSema(log_sema, 1);
    if (gt) {
        uint64_t dt = (uint64_t)(sceKernelGetSystemTimeWide() - t0);
        hle_hitch_us[HITCH_LOG] += dt;
        st_log_game_us += dt;
        st_log_commits_game++;
    }
}

/* the watchdog's commit: gives up (returns -1) when log_sema stays taken for timeout_us */
static int log_commit_timed(const char *s, unsigned n, SceUInt timeout_us)
{
    if (!log_ok || !n) return 0;
    if (log_sema >= 0) {
        SceUInt t = timeout_us;
        if (sceKernelWaitSema(log_sema, 1, &t) < 0) return -1;
    }
    log_commit_unlocked(s, n);
    if (log_sema >= 0) sceKernelSignalSema(log_sema, 1);
    return 0;
}

static void log_flush_locked(void)
{
    log_busy = 1;
    unsigned n = loglen;
    if (n) log_commit(logbuf, n);
    loglen = 0;
    log_busy = 0;
}

void hle_plat_log_write(const char *s, size_t n)
{
    int pz = hle_pz_on && sceKernelGetThreadId() == game_thid;   /* profiler zone io.log (game thread only) */
    if (game_thid >= 0 && sceKernelGetThreadId() == game_thid) hle_aw_result_tee(s, n);   /* results lines */
    if (pz) hle_pz_enter(PZ_log);
    log_busy = 1;
    if (n >= sizeof logbuf) {                              /* oversized: write through */
        log_flush_locked();
        log_busy = 1;
        log_commit(s, (unsigned)n);
    } else {
        if (loglen + n > sizeof logbuf) { log_flush_locked(); log_busy = 1; }
        memcpy(logbuf + loglen, s, n);
        loglen += (unsigned)n;
        if (log_sync == 1) { log_flush_locked(); log_busy = 1; }   /* 2: the watchdog commits */
    }
    log_busy = 0;
    if (pz) hle_pz_leave(PZ_log);
}

void hle_plat_log_flush(void)
{
    int pz = hle_pz_on && sceKernelGetThreadId() == game_thid;
    if (pz) hle_pz_enter(PZ_log);
    log_flush_locked();
    if (pz) hle_pz_leave(PZ_log);
}

/* watchdog thread (higher priority: the main thread is preempted, never inside the copy), every 2 s: take the pending
   lines unless the main thread is in the log code right now, then commit them outside (the main thread may append) */
static void log_commit_async(void)
{
    if (log_busy || !loglen) return;
    unsigned n = loglen;
    memcpy(wdbuf, logbuf, n);
    loglen = 0;
    st_log_commits_wd++;
    if (log_commit_timed(wdbuf, n, 500000) < 0) pspDebugScreenPrintf("(log busy: %u bytes not written)\n", n);
}

void hle_plat_milestone(const char *line)
{
    if (!opt_screen) return;
    /* "MILESTONE what (vtime ...)" -> short screen line */
    pspDebugScreenPrintf("%.*s", 120, line + (strncmp(line, "MILESTONE ", 10) == 0 ? 10 : 0));
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = sizeof b - 1;
    hle_plat_log_write(b, (size_t)n);
    if (opt_screen) pspDebugScreenPrintf("%s", b);
}

/* ------------------------------------------------------------------ automatic test results
   The lines of the end-of-run report (A/B screen lines, A/B
   result / variant / floor lines, main-loop time, hitch summary, clock, result=) as one block in session.log ("| "
   prefixed: tools that parse the original lines do not see them twice) and in <EBOOT dir>results_<seq>.txt; the final
   screen as shot_<seq>_results.bmp (crash: _crash). Scripted runs only by default (mainloop_iterations > 0). */
static int results_on(void) { return opt_results > 0 || (opt_results < 0 && hle_opt_mainloop_limit > 0); }

static void say_line(const char *line) { say("%s", line); }

int hle_plat_shot(const char *tag, char *why, unsigned why_n)
{
    return hle_aw_shot(dir, run_seq, tag, opt_results_keep, why, why_n);
}

static void results_write(void)
{
    if (results_written || !results_on()) return;
    results_written = 1;
    unsigned len, lines, dropped;
    const char *b = hle_aw_results(&len, &lines, &dropped);
    hle_aw_results_freeze();
    static char blk[16384];
    int k = snprintf(blk, sizeof blk, "==== results block: run sequence %u, %u lines%s - also in "
                     "results_%u.txt, the final screen in shot_%u_%s.bmp ====\n", run_seq, lines,
                     dropped ? " (some DROPPED: block full)" : "", run_seq, run_seq, results_tag);
    for (unsigned i = 0; i < len && k < (int)sizeof blk - 200;) {
        unsigned j = i;
        while (j < len && b[j] != '\n') j++;
        k += snprintf(blk + k, sizeof blk - k, "| %.*s\n", (int)(j - i), b + i);
        i = j + 1;
    }
    if (k < (int)sizeof blk - 64) k += snprintf(blk + k, sizeof blk - k, "==== end of the results block ====\n");
    hle_plat_log_write(blk, (size_t)k);
    char path[320], hdr[768];
    snprintf(path, sizeof path, "%sresults_%u.txt", dir, run_seq);
    int hn = snprintf(hdr, sizeof hdr, "PS2RC results\nbuild=%d "
                      "translation=%s folder=%s\n%s---- %u lines%s ----\n", HLE_BUILD_NUM, HLE_TRANSLATION, dir,
                      run_info, lines, dropped ? " (some dropped: block full)" : "");
    char foot[200];
    int fn = snprintf(foot, sizeof foot, "---- end; final screen: shot_%u_%s.bmp; per-variant game frames: the 'AB "
                      "shot' lines ----\n", run_seq, results_tag);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int ok = 0;
    if (fd >= 0) {
        ok = sceIoWrite(fd, hdr, hn) == hn && sceIoWrite(fd, b, len) == (int)len && sceIoWrite(fd, foot, fn) == fn;
        sceIoClose(fd);
    }
    int rm = fd >= 0 ? hle_aw_cap_files(dir, "results_", ".txt", opt_results_keep) : 0;
    LOG("results: %s %s (%u bytes)%s\n", path, fd < 0 ? "NOT created" : ok ? "written" : "write FAILED",
        (unsigned)(hn + len + fn), rm > 0 ? "; older results files deleted" : "");
    log_flush_locked();
}

static void results_shot(void)
{
    if (results_shot_done || !results_on()) return;
    results_shot_done = 1;
    sceKernelDelayThread(100000);
    char why[256];
    int r = hle_aw_shot(dir, run_seq, results_tag, opt_results_keep, why, sizeof why);
    LOG("results: final screen %s: %s\n", r == 0 ? "saved" : "NOT saved", why);
    log_flush_locked();
}

/* ------------------------------------------------------------------ time */
double hle_host_seconds(void) { return (double)(sceKernelGetSystemTimeWide() - t0_us) / 1e6; }

/* psp/hle_movie.c: virtual time stands still while a movie plays inside the pack's movie hook (the host skips
   them: one virtual timeline); the movie's real time is left out of the pacing, so the boot after it is paced
   as without it (hle_host_seconds keeps counting it) */
static SceInt64 pace_shift_us;
void hle_plat_pace_shift(uint64_t us) { pace_shift_us += (SceInt64)us; }
/* (hle_bl.c): the pacing clock and moving it (x_pace lag forgiveness / alignment) */
void hle_plat_pace_adjust(int64_t us) { pace_shift_us += (SceInt64)us; }
uint64_t hle_plat_pace_now_us(void)
{
    SceInt64 n = sceKernelGetSystemTimeWide() - t0_us - pace_shift_us;
    return n > 0 ? (uint64_t)n : 0u;
}

/* ps2rc.cfg pace_boot=0 - no real-time pacing before the first main-loop
   iteration (test folders). The boot's guest work ends near virtual 34 s on the PSP-3000 and the game's loading screens
   then wait until virtual 40.1 s: 5-6.5 s of the ~40 s boot were idle.pace (hardware logs). Virtual time is
   deterministic, so the guest does the same; only the real wait goes. At the first main-loop pace call the clock is
   re-aligned (pace_shift_us) so the main loop is paced exactly as before. Default 1 = as before. */
static int opt_pace_boot = 1, pace_boot_aligned;
static SceInt64 pace_boot_saved_us;

void hle_plat_pace(uint64_t vtime)
{
    if (!opt_pace) return;
    uint64_t due_us = vtime / EE_HZ * 1000000ull + vtime % EE_HZ * 1000000ull / EE_HZ;   /* EE cycles -> us */
    if (!opt_pace_boot) {
        if (!hle_mainloop_iters) return;                   /* boot: virtual time may run ahead of the real clock */
        if (!pace_boot_aligned) {
            pace_boot_aligned = 1;
            SceInt64 now_us = sceKernelGetSystemTimeWide() - t0_us - pace_shift_us;
            if ((SceInt64)due_us > now_us) {               /* virtual ahead: count it, no wait for it */
                pace_boot_saved_us = (SceInt64)due_us - now_us;
                pace_shift_us -= pace_boot_saved_us;
            }
        }
    }
    for (;;) {
        uint64_t now_us = (uint64_t)(sceKernelGetSystemTimeWide() - t0_us - pace_shift_us);
        if (now_us >= due_us) return;
        uint64_t d = due_us - now_us;
        sceKernelDelayThread((SceUInt)(d > 1000000 ? 1000000 : d));
        hle_progress++;
    }
}

/* ------------------------------------------------------------------ the hole: big buffers + stacks */
static void *hole_take(size_t size, size_t align)
{
    uintptr_t p = ((uintptr_t)hole_cur + align - 1) & ~(uintptr_t)(align - 1);
    if (!hole_cur || p + size > (uintptr_t)hole_hi) return NULL;
    hole_cur = (uint8_t *)(p + size);
    return (void *)p;
}

/* mem_volatile=1 (partition locked) - the volatile partition first while it keeps room for the intro movie
   (HLE_MEM_VOL_MOVIE_KEEP), then the hole, then a block of user memory (not the 768 KB newlib heap); otherwise as
   before (the hole, then memalign) */
void *hle_plat_big_alloc(size_t size)
{
    void *p = NULL;
    if (hle_psp_mem_volatile()) {
        p = hle_mem_vol_take(size, HLE_MEM_VOL_MOVIE_KEEP);
        if (p) { big_in_vol_kb += (unsigned)(size >> 10); return p; }      /* zeroed */
        p = hole_take(size, 64);
        if (!p) {
            SceUID id = sceKernelAllocPartitionMemory(2, "hle_big", PSP_SMEM_High, size + 64, NULL);
            if (id >= 0) {
                p = (void *)(((uintptr_t)sceKernelGetBlockHeadAddr(id) + 63u) & ~(uintptr_t)63u);
                big_in_part_kb += (unsigned)(size >> 10);
                LOG("mem: %u KB from user memory (volatile partition and hole full)\n",
                    (unsigned)(size >> 10));
            }
        }
    } else p = hole_take(size, 64);
    if (!p) p = memalign(64, size);
    if (!p) { LOG("FATAL: out of memory (%u bytes)\n", (unsigned)size); hle_set_exit_reason("out of memory");
              hle_exit(4); }
    memset(p, 0, size);
    return p;
}

#define STACK_PAINT 0x57AC57ACu
void *hle_plat_stack_alloc(size_t size)
{
    void *p = NULL;
    uintptr_t a = ((uintptr_t)pool_cur + 63) & ~(uintptr_t)63;
    if (pool_cur && a + size <= (uintptr_t)pool_hi) {      /* inside the game thread's own stack */
        p = (void *)a;
        pool_cur = (uint8_t *)(a + size);
        LOG("host stack of %u KB at %p (game thread stack pool, %u KB left)\n", (unsigned)(size >> 10), p,
            (unsigned)((uintptr_t)(pool_hi - pool_cur) >> 10));
    }
    if (!p) {
        p = hole_take(size, 64);
        if (p) LOG("WARNING: host stack of %u KB at %p in the hole, OUTSIDE the game thread's stack (the build 13 "
                   "layout that died on the PSP-3000)\n", (unsigned)(size >> 10), p);
    }
    if (!p) {                                              /* hole full: a block from the free user memory */
        SceUID id = sceKernelAllocPartitionMemory(2, "hle_stack", PSP_SMEM_Low, size, NULL);
        if (id < 0) return NULL;
        p = sceKernelGetBlockHeadAddr(id);
        LOG("host stack of %u KB outside the hole (free now %u KB)\n", (unsigned)(size >> 10),
            (unsigned)(sceKernelTotalFreeMemSize() >> 10));
    }
    uint32_t *w = p;
    for (size_t i = 0; i < size / 4; i++) w[i] = STACK_PAINT;
    return p;
}

/* the hole / reserve policy for psp/hle_audio_psp.c and psp/hle_movie.c (hle_mem_psp.h) */
int hle_psp_mem_volatile(void) { return opt_mem_volatile && hle_mem_vol_on(); }
void *hle_psp_hole_take(size_t size, size_t align) { return hole_take(size, align); }
unsigned hle_psp_hole_free_kb(void) { return hole_cur ? (unsigned)((uintptr_t)(hole_hi - hole_cur) >> 10) : 0; }

size_t hle_plat_stack_used(void *stack, size_t size)
{
    const uint32_t *w = stack;
    size_t i = 0;
    while (i < size / 4 && w[i] == STACK_PAINT) i++;
    return size - 4 * i;
}

void hle_plat_ctx_make(hle_hostctx *ctx, void *stack, size_t size, void (*fn)(void))
{
    memset(ctx, 0, sizeof *ctx);
    uint32_t gp;
    __asm__ volatile("move %0, $28" : "=r"(gp));
    ctx->w[0] = (uint32_t)(uintptr_t)fn;                                    /* s0: entry for the trampoline */
    ctx->w[9] = ((uint32_t)(uintptr_t)stack + (uint32_t)size - 64u) & ~15u; /* sp */
    ctx->w[10] = (uint32_t)(uintptr_t)hle_ctx_trampoline;                   /* ra */
    ctx->w[11] = gp;
}

void hle_plat_ctx_swap(hle_hostctx *save, hle_hostctx *load)
{
    static int logged;
    if (logged < 3) {                                      /* breadcrumbs: the first switches into guest threads */
        logged++;
        uint32_t sp = load->w[9];
        int inside = sp > (uint32_t)(uintptr_t)thr_lo && sp <= (uint32_t)(uintptr_t)thr_hi;
        LOG("ctx: switch #%d to host sp=%08x (%s)\n", logged, (unsigned)sp,
            inside ? "inside the game thread's stack" : "OUTSIDE the game thread's stack");
    }
    hle_ctx_swap_asm(save->w, load->w);
}

/* ------------------------------------------------------------------ files: virtual handles, LRU of open fds */
#define MAX_VFILES 512
#define MAX_OPEN 8                                         /* build 18: was 10 (see log_commit_unlocked) */
static struct { char *path; SceUID fd; int64_t pos, size; uint32_t used; } vf[MAX_VFILES];
static int nvf, nopen;
static uint32_t vf_tick;
static uint64_t io_opens, io_reads, io_bytes;
static double io_seconds;

/* the disc lock (recursive), created by the pack's asynchronous disc reader before its thread starts (disc_async=1);
   until then a no-op. hle_disc.c takes it around every file-table access; vf_close_lru takes it too,
   so no thread closes a file the reader is reading. Time the game thread waits for it = hitch source "disc-wait". */
static SceUID dl_sema = -1;
static volatile SceUID dl_owner;
static int dl_depth;
void hle_plat_disc_lock_create(void)
{
    if (dl_sema < 0) dl_sema = sceKernelCreateSema("hle_disc_lock", 0, 1, 1, NULL);
}
void hle_plat_disc_lock(void)
{
    if (dl_sema < 0) return;
    SceUID me = sceKernelGetThreadId();
    if (dl_owner == me) { dl_depth++; return; }
    if (me == game_thid && dl_owner) {                     /* the reader holds it: count the wait */
        SceInt64 t0 = sceKernelGetSystemTimeWide();
        sceKernelWaitSema(dl_sema, 1, NULL);
        hle_hitch_us[HITCH_DWAIT] += (uint64_t)(sceKernelGetSystemTimeWide() - t0);
    } else sceKernelWaitSema(dl_sema, 1, NULL);
    dl_owner = me;
    dl_depth = 1;
}
void hle_plat_disc_unlock(void)
{
    if (dl_sema < 0 || dl_owner != sceKernelGetThreadId()) return;
    if (--dl_depth == 0) {
        dl_owner = 0;
        sceKernelSignalSema(dl_sema, 1);
    }
}

/* closes the least recently used open disc file; 0 if none is open. Only on the game thread (the disc layer has no
   lock: the watchdog's log commits must not close a file the game thread is reading) - or the thread that holds the
   disc lock (the asynchronous disc reader) */
static int vf_close_lru_locked(void);
static int vf_close_lru(void)
{
    SceUID me = sceKernelGetThreadId();
    if (game_thid >= 0 && me != game_thid && !(dl_sema >= 0 && dl_owner == me)) return 0;
    hle_plat_disc_lock();
    int r = vf_close_lru_locked();
    hle_plat_disc_unlock();
    return r;
}
static int vf_close_lru_locked(void)
{
    int lru = -1;
    for (int i = 0; i < nvf; i++)
        if (vf[i].fd >= 0 && (lru < 0 || vf[i].used < vf[lru].used)) lru = i;
    if (lru < 0) return 0;
    int st = io_state, gt = sceKernelGetThreadId() == game_thid;
    if (gt) io_mark(IO_DISC_CLOSE);
    sceIoClose(vf[lru].fd);
    if (gt) io_mark(st);
    vf[lru].fd = -1;
    nopen--;
    return 1;
}

/* psp/hle_mcfs_psp.c: the memory card's file layer frees an fd the same way when its open hits the limit */
int hle_psp_disc_close_lru(void) { return vf_close_lru(); }

static unsigned disc_open_fails;
static int disc_open_err;

static int vf_ensure(int h)
{
    if (vf[h].fd >= 0) { vf[h].used = ++vf_tick; return 1; }
    if (nopen >= MAX_OPEN) vf_close_lru();                 /* close the least recently used */
    SceInt64 t = sceKernelGetSystemTimeWide();
    int gt = sceKernelGetThreadId() == game_thid;          /* only the game thread's state / time */
    if (gt) io_mark(IO_DISC_OPEN);
    SceUID fd = sceIoOpen(vf[h].path, PSP_O_RDONLY, 0);
    while (fd < 0 && (uint32_t)fd != 0x80010002u && vf_close_lru()) {   /* not ENOENT: the open-file limit */
        disc_open_fails++;
        disc_open_err = fd;
        fd = sceIoOpen(vf[h].path, PSP_O_RDONLY, 0);
    }
    if (gt) io_mark(IO_NONE);
    SceInt64 dt_open = sceKernelGetSystemTimeWide() - t;
    io_seconds += (double)dt_open / 1e6;
    if (gt) hle_hitch_us[HITCH_DISC] += (uint64_t)dt_open;
    io_opens++;
    if (fd < 0) return 0;
    vf[h].fd = fd;
    vf[h].pos = 0;
    vf[h].used = ++vf_tick;
    nopen++;
    if (vf[h].size < 0) {
        vf[h].size = sceIoLseek(fd, 0, PSP_SEEK_END);
        vf[h].pos = vf[h].size;
    }
    return 1;
}

int hle_plat_open(const char *path)
{
    for (int i = 0; i < nvf; i++) if (strcmp(vf[i].path, path) == 0) return vf_ensure(i) ? i : -1;
    if (nvf >= MAX_VFILES) return -1;
    int h = nvf;
    vf[h].path = strdup(path);
    vf[h].fd = -1;
    vf[h].size = -1;
    if (!vf[h].path || !vf_ensure(h)) { free(vf[h].path); return -1; }
    nvf++;
    return h;
}

int64_t hle_plat_pread(int h, uint64_t off, void *dst, uint32_t n)
{
    if (h < 0 || h >= nvf || !vf_ensure(h)) return -1;
    SceInt64 t = sceKernelGetSystemTimeWide();
    int gt = sceKernelGetThreadId() == game_thid;          /* only the game thread's state / time */
    if (gt) io_mark(IO_DISC_READ);
    if (vf[h].pos != (int64_t)off) {
        if (sceIoLseek(vf[h].fd, (SceOff)off, PSP_SEEK_SET) != (SceOff)off) { if (gt) io_mark(IO_NONE); return -1; }
        vf[h].pos = (int64_t)off;
    }
    int r = sceIoRead(vf[h].fd, dst, n);
    if (gt) io_mark(IO_NONE);
    SceInt64 dt_rd = sceKernelGetSystemTimeWide() - t;
    io_seconds += (double)dt_rd / 1e6;
    if (gt) hle_hitch_us[HITCH_DISC] += (uint64_t)dt_rd;
    if (r < 0) { vf[h].pos = -1; return r; }
    vf[h].pos += r;
    io_reads++;
    io_bytes += (uint64_t)r;
    return r;
}

int64_t hle_plat_size(int h) { return h >= 0 && h < nvf ? vf[h].size : -1; }

/* build 22 (profiler): a whole file next to the EBOOT in one write, closed at once (FAT commits on close); the open
   gets the same open-file-limit recovery as session.log */
int hle_plat_write_file(const char *name, const void *data, size_t n)
{
    char path[320];
    snprintf(path, sizeof path, "%s%s", dir, name);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    for (int tries = 0; fd < 0 && (uint32_t)fd != 0x80010002u && tries < 4 && vf_close_lru(); tries++)
        fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int w = sceIoWrite(fd, data, n);
    sceIoClose(fd);
    return w == (int)n ? 0 : -1;
}
const char *hle_plat_disc_root(void) { return dir; }

/* ------------------------------------------------------------------ diagnostics */
void hle_plat_backtrace(void) { LOG("(no host backtrace on the PSP; see the call-trace ring)\n"); }
void hle_plat_note_mmio(void *ra, uint32_t addr) { (void)ra; (void)addr; }
const char *hle_plat_fn_name(void *pc) { (void)pc; return NULL; }

/* the one exit path (end of run + X, HOME, crash, the HOME callback's fallback). An unordered exit powered the
   console off after X (2 of 5 times before the exception handler was chained and released). Everything that can still run EBOOT or module code asynchronously is stopped
   first, in this order: the audio feeder thread + its sceAudio channel; the GE (an open GU_DIRECT frame list is
   finished, the queue reset if it does not get idle, sceGuInit's GE callbacks removed - a GE interrupt must not call
   into a freed EBOOT); the hardware-counter kernel thread (it must have LEFT ps2rc_exc.prx before the module is
   unloaded - if it did not, the module is only stopped); the crash-handler module (stop = default exception handler
   back, then unload); X released; then sceKernelExitGame. Every step writes one session.log line first, committed to
   the card at once (log_sync = 1): a console that still switches off shows the last step it reached. Only the first
   caller runs it (the HOME callback thread and the game thread may both get here). */
static volatile int exit_seq_owner;
static void exit_sequence(const char *who)
{
    int me = sceKernelGetThreadId();
    if (exit_seq_owner && exit_seq_owner != me) for (;;) sceKernelDelayThread(1000000);
    exit_seq_owner = me;
    exiting = 1;                                   /* the watchdog stops reporting */
    log_sync = 1;
    log_flush_locked();
    say("exit 1/6 (%s): stopping the audio feeder thread + sceAudio channel, then any Media Engine loop\n", who);
#ifdef HLE_HAVE_AUDIO_MODEL
    if (opt_az_exit || opt_az_marker) {            /* was a suspend asked for during the run? (power switch
                                                      or auto-sleep: with a scePowerLock held it waits for the unlock) */
        HleMePower pw;
        char si[320];
        hle_me_power_events(&pw);
        hle_me_sleep_info(si, sizeof si);
        say("exit 1/6: power callback this run: power-switch events %u, parks %u, resumes %u, last flags "
            "%08x; volatile scePowerLock %s, ME scePowerLock %s; PRX %s\n", (unsigned)pw.switches, (unsigned)pw.parks,
            (unsigned)pw.resumes, (unsigned)pw.last_flags, hle_mem_vol_on() ? "HELD" : "not held",
            hle_me_power_lock_rc() == 0 ? "HELD" : "not held", si);
    }
#endif
    int a = 1;
#ifdef HLE_HAVE_AUDIO_MODEL
    a = hle_audio_psp_stop();                      /* audio_me: its ME loop is stopped + held in reset here */
#endif
    int me_was = 0, me_ack = 0;
#ifdef HLE_HAVE_AUDIO_MODEL                        /* hle_me.c is compiled only with AUDIO=1 (build.sh) */
    me_was = hle_me_running();                     /* any other ME user */
    me_ack = me_was ? hle_me_stop(100) : 0;
#endif
    say("exit 2/6: audio %s; Media Engine %s; GE (open frame list, queue, GE callbacks)\n",
        a ? "stopped" : "feeder did NOT stop within 0.5 s (left running, channel kept)",
        !me_was ? "not running (never started, or already stopped + held in reset)"
                : me_ack ? "was running: stopped (acknowledged), held in reset"
                         : "was running: NO acknowledgement in 100 ms, held in reset");
    int g = hle_ge_shutdown ? hle_ge_shutdown() : 8;          /* 8: no GE back-end ("was off") */
    say("exit 3/6: GE %s%s%s; hardware-counter thread\n", g & 8 ? "was off" : g & 4 ? "NOT idle in 0.5 s, queue reset"
        : "idle", g & 1 ? " (an open frame list was finished)" : "", g & 8 ? "" : ", GE callbacks removed");
    int pr = hle_psp_perf_shutdown();
    char pinfo[256] = "";
    if (pr >= 0) hle_psp_perf_exit_info(pinfo, sizeof pinfo);
    say("exit 4/6: counter thread %s%s%s%s; crash-handler module: %s\n", pr < 0 ? "not running" : pr ?
        "left the module" : "did NOT leave the module within 1 s", pinfo[0] ? " (" : "", pinfo, pinfo[0] ? ")" : "",
        opt_az_exit ? "exit mode, stays loaded until the reboot" : pr == 0
        ? "stop only (no unload under a live kernel thread)" : "stop + unload");
    int st = 0, un = 0;
    int q0 = ps2rc_exc_query();                    /* the unload checked, not assumed */
    if (opt_az_exit) {                             /* no stop, no unload - see opt_az_exit */
        int em = ps2rc_exc_exitmode(run_seq);
        int held = 0, pcb = 0x7FFFFFFF;
#ifdef HLE_HAVE_AUDIO_MODEL
        pcb = hle_me_power_shutdown(&held);
#endif
        say("exit 5/6: module exit mode %08x (%s); ME power callback unregister %08x (7fffffff = none); "
            "ME scePowerLock %s; module query %08x; waiting for X to be released\n", (unsigned)em,
            ((uint32_t)em & 0xFFFF0000u) == 0x415A0000u ? ((em & 0x100) ? "done, exit marker armed" : "done, ME not "
            "in reset: no marker") : em == -2 ? "module older than version 2: left as it is, NOT stopped / unloaded"
            : em == 0x7FFFFFFF ? "no module" : "FAILED - module left as it is", (unsigned)pcb,
            held ? "held (kept until the reboot)" : "not held", (unsigned)q0);
    } else {
        ps2rc_exc_uninstall2(pr != 0, &st, &un);
        int q1 = ps2rc_exc_query();
        say("exit 5/6: module stop %08x unload %08x (7fffffff = not done; success = the uid); module query before %08x, "
            "after %08x (%s); waiting for X to be released\n", (unsigned)st, (unsigned)un, (unsigned)q0, (unsigned)q1,
            q0 == 0x7FFFFFFF ? "no module" : (uint32_t)q1 == 0x8002012Eu ? "gone: unknown module"
            : q1 == q0 ? "SAME answer as before: unload not confirmed" : "answer changed");
    }
    if (hle_mem_vol_on()) {                        /* GE + audio + ME stopped above: the partition goes back */
        int vr = hle_mem_vol_unlock2(opt_az_exit);
        if (opt_az_exit)
            say("exit 5/6: volatile partition unlocked (%08x), its scePowerLock KEPT until the reboot\n",
                (unsigned)vr);
        else
            say("exit 5/6: volatile partition unlocked (%08x), scePowerLock released (scePowerUnlock %08x)\n",
                (unsigned)vr, (unsigned)hle_mem_vol_power_unlock_rc());
    }
    if (hle_aw_clock_on()) {                       /* everything else is stopped - the PLL back to 333 */
        char cl[320];
        say("exit 5/6: clock back to 333 MHz if it was raised (no card I/O during the ramp)\n");
        if (hle_aw_clock_down("at exit", cl, sizeof cl) > 0) say("%s", cl);
    }
    SceCtrlData pad;
    for (int i = 0; i < 40; i++) {                 /* at most 2 s */
        sceCtrlPeekBufferPositive(&pad, 1);
        if (!(pad.Buttons & PSP_CTRL_CROSS)) break;
        sceKernelDelayThread(50000);
    }
    sceKernelDelayThread(100000);
    {                                              /* cpu_auto state at a clean exit / a crash */
        char al[320];
        int an = hle_aw_auto_exit(!az_crashed, al, sizeof al);
        if (an > 0) say("%s", al);
    }
    say("exit 6/6: sceKernelExitGame\n");
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}

static void wait_exit_button(void)
{
    if (opt_screen && !hle_exit_requested) pspDebugScreenPrintf("\nPress X to exit.\n");
    results_shot();                                /* the final screen -> shot_<seq>_results.bmp */
    SceCtrlData pad;
    int autox = opt_exit_auto > 0 && hle_psp_is_emulator();   /* never on the PSP (build 11: an unattended exit
                                                                  switched a console off) */
    SceInt64 t_end = sceKernelGetSystemTimeWide() + (SceInt64)opt_exit_auto * 1000000;
    for (;;) {
        if (hle_exit_requested) break;             /* build 30: HOME -> Exit needs no X */
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CROSS) break;
        if (autox && sceKernelGetSystemTimeWide() >= t_end) break;
        sceKernelDelayThread(50000);
    }
    exit_sequence(hle_exit_requested ? "HOME" : autox && !(pad.Buttons & PSP_CTRL_CROSS) ? "exit_auto (emulator)"
                                                                                         : "X pressed");
}

static void final_lines(int code)
{
    const char *why = hle_exit_reason();
    int ml = hle_milestone_reached(HLE_PACK_MAIN_LOOP_NAME);
    const char *end = hle_exit_requested ? "home" : code == 0 ? "limit" : code == 3 ? "blocked" : code == 5 ? "deadlock"
                                                                        : code == 7 ? "stuck" : "error";
    unsigned hole_used = (unsigned)((hole_cur - hole_lo) >> 10), hole_kb = (unsigned)((hole_hi - hole_lo) >> 10);
    LOG("memory: free_kb_start=%u free_kb_after_window=%u free_kb_end=%u max_free_block_kb_end=%u hole_kb=%u "
        "hole_used_kb=%u\n", free_kb_start, free_kb_window, (unsigned)(sceKernelTotalFreeMemSize() >> 10),
        (unsigned)(sceKernelMaxFreeMemSize() >> 10), hole_kb, hole_used);
    if (opt_heap_extra_kb || opt_mem_volatile) {
        char vi[320];
        hle_mem_vol_info(vi, sizeof vi);
        LOG("memory: heap_extra_kb=%u (asked %d, reserve %u KB), guest RAM end %08x; mem_volatile=%d, %s; "
            "big buffers: %u KB in the volatile partition, %u KB in user memory blocks\n", heap_extra_kb_got,
            opt_heap_extra_kb, mem_reserve_kb_used, (unsigned)window_size, opt_mem_volatile, vi, big_in_vol_kb,
            big_in_part_kb);
    }
    LOG("io: opens %llu, reads %llu, %llu bytes, %.1f s in sceIo; real time %.1f s, virtual %.3f s, frames %llu\n",
        (unsigned long long)io_opens, (unsigned long long)io_reads, (unsigned long long)io_bytes, io_seconds,
        hle_host_seconds(), (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame);
    uint32_t fcr31;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr31));
    LOG("fcr31_end=%08x main_loop_iterations=%llu\n", fcr31, (unsigned long long)hle_mainloop_iters);
    if (hle_pz_opt_hw >= 1) {                              /* build 22: a frozen step names the register that failed */
        char st[256];
        hle_psp_perf_status(st, sizeof st);
        LOG("hardware counters at the end: %s\n", st);
    }
    if (!opt_pace_boot)
        LOG("pace_boot=0: at the first main-loop pace the virtual clock was %lld ms ahead of the real one "
            "(not waited for; %s)\n", (long long)(pace_boot_saved_us / 1000), pace_boot_aligned ? "re-aligned there"
            : "the main loop never paced");
    LOG("log: log_sync=%d, %llu commits on the game thread (%llu ms in all), %llu by the watchdog\n",
        opt_log_sync, (unsigned long long)st_log_commits_game, (unsigned long long)(st_log_game_us / 1000u),
        (unsigned long long)st_log_commits_wd);
    LOG("files: log open failures %u (recovered %u, last error %08x), disc open retries %u (last error %08x), "
        "disc files open at the end %d\n", log_open_fails, log_open_recovered, (unsigned)log_open_err, disc_open_fails,
        (unsigned)disc_open_err, nopen);
    hle_aw_clock_report();                                 /* cpu_clock_mhz summary (nothing without it) */
    say("result=%s end=%s code=%d reason=%s\n", ml ? "main_loop" : "boot_incomplete", end, code, why);
    log_flush_locked();
    results_write();
}

void hle_plat_exit(int code)
{
    if (!exiting) {
        exiting = 1;
        log_sync = 1;                                      /* from here on every line goes to the card at once */
        final_lines(code);
        log_flush_locked();
    }
    wait_exit_button();
    for (;;) sceKernelDelayThread(1000000);
}

/* ------------------------------------------------------------------ crash report (ps2rc_exc.prx resumes here)
   User mode, on the faulting thread's own stack 4 KB below the fault sp. Writes the registers (map EPC to a function
   with the build's ps2rc.elf: build/phase6psp/b<N>/ps2rc.elf), the guest state and the trace ring, then waits for X. */
static void crash_report(uint32_t *r)
{
    az_crashed = 1;                                        /* cpu_auto counts it as a failure */
    exiting = 1;                                           /* the watchdog stops committing / reporting */
    hle_pz_on = 0;                                         /* build 22: the profiler stays out of the crash path */
    log_sema = -1;                                         /* the crash may have hit inside a commit */
    log_busy = 0;
    log_sync = 1;
    char b[1024];
    unsigned n = ps2rc_exc_format(r, b, sizeof b);
    log_flush_locked();
    log_commit(b, n);
    if (opt_screen) pspDebugScreenPrintf("\n%s", b);
    say("CRASH: host thread %s, guest thread %d%s, vtime %.3f s, frame %llu, real %.1f s\n",
        sceKernelGetThreadId() == game_thid ? "hle_game" : "other (watchdog?)", hle_cur ? hle_cur->id : -1,
        hle_in_intr ? " (interrupt context)" : "", (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame,
        hle_host_seconds());
    say("CRASH: last loop back-edge taken at guest %08x\n", (unsigned)hle_where);
    hle_backtrace_dump(hle_cur ? &hle_cur->c : NULL);
    hle_trace_ring_dump();
    hle_stack_report();
    say("result=crash end=error code=6 reason=exception %u (%s) at EPC %08x BadVAddr %08x\n",
        (unsigned)((r[PS2RC_EXC_CAUSE] >> 2) & 31), ps2rc_exc_name(r[PS2RC_EXC_CAUSE]), (unsigned)r[PS2RC_EXC_EPC],
        (unsigned)r[PS2RC_EXC_BADVADDR]);
    results_tag = "crash";
    results_write();
    wait_exit_button();
    for (;;) sceKernelDelayThread(1000000);
}

/* ------------------------------------------------------------------ watchdog thread */
/* sampler (build 15): every 10 ms the watchdog reads hle_where (guest address of the last loop back-edge, written by
   the generated code - RC_BACKEDGE_AT); every PROGRESS_TICKS it writes one "progress" line with virtual time, frame,
   back-edge count and the 4 most frequent sampled loop addresses (a hang inside guest loops shows up there). */
#define SAMPLE_US 10000
#define PROGRESS_TICKS 300
#define NSLOT 32
static struct { uint32_t addr, n; } smp[NSLOT];
static unsigned smp_total;

static void sample_where(void)
{
    uint32_t a = hle_where;
    smp_total++;
    int free_i = -1, min_i = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (smp[i].n && smp[i].addr == a) { smp[i].n++; return; }
        if (!smp[i].n && free_i < 0) free_i = i;
        if (smp[i].n < smp[min_i].n) min_i = i;
    }
    int k = free_i >= 0 ? free_i : min_i;                  /* full: replace the rarest */
    smp[k].addr = a;
    smp[k].n = 1;
}

static void progress_line(void)
{
    char b[640];
    int n = snprintf(b, sizeof b, "progress: real %.1f s, vtime %.3f s, frame %llu, backedges %llu, syscalls %llu, "
                     "hooks %llu, guest thread %d%s, hot loops:", hle_host_seconds(), (double)hle_vtime / EE_HZ,
                     (unsigned long long)hle_frame, (unsigned long long)(hle_stat_backedges + hle_backedge_count),
                     (unsigned long long)hle_stat_syscalls, (unsigned long long)hle_stat_hooks,
                     hle_cur ? hle_cur->id : -1, hle_in_intr ? " (interrupt)" : "");
    for (int t = 0; t < 4 && n > 0 && n < (int)sizeof b - 32; t++) {
        int best = -1;
        for (int i = 0; i < NSLOT; i++)
            if (smp[i].n && (best < 0 || smp[i].n > smp[best].n)) best = i;
        if (best < 0) break;
        n += snprintf(b + n, sizeof b - n, " %08x %u%%", (unsigned)smp[best].addr,
                      smp_total ? smp[best].n * 100u / smp_total : 0u);
        smp[best].n = 0;
    }
    int st = io_state;
    if (n > 0 && n < (int)sizeof b - 48)
        n += st ? snprintf(b + n, sizeof b - n, "; game thread in sceIo: %s for %.1f s", io_names[st],
                           (double)(sceKernelGetSystemTimeWide() - io_t0) / 1e6)
                : snprintf(b + n, sizeof b - n, "; not in sceIo");
    if (n > 0 && n < (int)sizeof b - 80)
        n += snprintf(b + n, sizeof b - n, "; disc files open %d; log open fails %u (recovered %u, last %08x)", nopen,
                      log_open_fails, log_open_recovered, (unsigned)log_open_err);
    if (n > 0 && n < (int)sizeof b - 1) b[n++] = '\n';
    if (n >= (int)sizeof b) n = sizeof b - 1;
    b[n > 0 ? n : 0] = 0;
    memset(smp, 0, sizeof smp);
    smp_total = 0;
    if (n <= 0) return;
    pspDebugScreenPrintf("%s", b);                         /* always: the screen needs no lock and no sceIo */
    if (log_commit_timed(b, (unsigned)n, 500000) < 0) pspDebugScreenPrintf("(log busy: line not written)\n");
}

static int watchdog(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint64_t last = hle_progress;
    int still = 0, tick = 0, sub = 0;
    for (;;) {
        sceKernelDelayThread(SAMPLE_US);
        if (exiting) continue;
        sample_where();
        if (log_sync == 2 && loglen && (sub % (unsigned)(opt_log_commit_ms >= 10 ? opt_log_commit_ms / 10 : 1)) == 0)
            log_commit_async();                            /* log_sync=2 */
        if (++sub % PROGRESS_TICKS == 0) progress_line();
        if (sub % (1000000 / SAMPLE_US)) continue;         /* the rest once per second */
        if (hle_aw_clock_on()) {                           /* cpu_clock_mhz check every cpu_clock_log_s */
            char cl[400];
            int em = 0, n = hle_aw_clock_tick(cl, sizeof cl, &em);
            if (n > 0) log_commit_timed(cl, (unsigned)n, 500000);
            if (em) {                                      /* battery too hot: back to 333, no log write meanwhile */
                SceUInt t = 2000000;
                int got = log_sema >= 0 && sceKernelWaitSema(log_sema, 1, &t) >= 0;
                n = hle_aw_clock_down("EMERGENCY (battery temperature)", cl, sizeof cl);
                if (got) sceKernelSignalSema(log_sema, 1);
                if (n > 0) { pspDebugScreenPrintf("%s", cl); log_commit_timed(cl, (unsigned)n, 500000); }
            }
        }
        {                                                  /* cpu_auto "ok" after 60 s of play */
            char al[200];
            int an = hle_aw_auto_tick(al, sizeof al);
            if (an > 0) log_commit_timed(al, (unsigned)an, 500000);
        }
        if (++tick % 2 == 0) log_commit_async();           /* pending log lines reach the card within ~2 s */
        if (hle_progress != last) { last = hle_progress; still = 0; continue; }
        if (++still < opt_stuck) continue;
        LOG("WATCHDOG: guest code stuck (%d s without a safe point) in guest thread %d%s\n", still,
            hle_cur ? hle_cur->id : -1, hle_in_intr ? " (interrupt context)" : "");
        hle_set_exit_reason("watchdog: guest code stuck");
        hle_exit(7);
    }
    return 0;
}

/* ------------------------------------------------------------------ configuration (<EBOOT dir>/ps2rc.cfg) */
static void read_config(void)
{
    if (!hle_cfg_found()) { say("config=none (defaults)\n"); return; }
    for (int ci = 0; ci < hle_cfg_count(); ci++) {    /* the shared table (hle_cfg.c), whole file */
        {
            const char *k = hle_cfg_key(ci), *v = hle_cfg_val(ci);
            if (!strcmp(k, "vcallms"))
                hle_opt_vcallms = !strcmp(v, "nohit") ? VCALLMS_NOHIT : !strcmp(v, "hle") ? VCALLMS_HLE : VCALLMS_STRICT;
            else if (!strcmp(k, "max_seconds")) hle_max_seconds = atoi(v);
            else if (!strcmp(k, "mainloop_iterations")) hle_opt_mainloop_limit = strtoull(v, 0, 10);
            else if (!strcmp(k, "max_frames")) hle_max_frames = strtoull(v, 0, 10);
            else if (!strcmp(k, "verbose")) hle_verbose = atoi(v);
            else if (!strcmp(k, "pace")) opt_pace = atoi(v);
            else if (!strcmp(k, "pace_boot")) opt_pace_boot = atoi(v);
            else if (!strcmp(k, "stuck_seconds")) opt_stuck = atoi(v);
            else if (!strcmp(k, "screen")) opt_screen = atoi(v);
            else if (!strcmp(k, "exit_auto")) opt_exit_auto = atoi(v);
            else if (!strcmp(k, "log_sync")) opt_log_sync = atoi(v);
            else if (!strcmp(k, "log_commit_ms")) opt_log_commit_ms = atoi(v);
            else if (!strcmp(k, "disc_async")) { if (&hle_cdasync_opt) hle_cdasync_opt = atoi(v); }
            else if (!strncmp(k, "hitch_", 6)) hle_hitch_cfg(k, v);
            else if (!strcmp(k, "idle_skip")) hle_opt_idle_skip = atoi(v);
            else if (!strcmp(k, "prof")) hle_pz_opt_on = atoi(v);                 /* profiler */
            else if (!strcmp(k, "prof_hw")) hle_pz_opt_hw = atoi(v);
            else if (!strcmp(k, "prof_summary")) hle_pz_opt_summary_s = atoi(v);
            else if (!strcmp(k, "prof_capture")) hle_pz_opt_capture = atoi(v);
            else if (!strcmp(k, "heap_extra_kb")) opt_heap_extra_kb = !strcmp(v, "max") ? -1 : atoi(v);
            else if (!strcmp(k, "mem_volatile")) opt_mem_volatile = atoi(v);
            else if (!strcmp(k, "mem_reserve_kb")) opt_mem_reserve_kb = atoi(v);
            else if (!strcmp(k, "mem_fake_free_kb")) opt_mem_fake_free_kb = atoi(v);
            else if (!strcmp(k, "movies")) cfg_movies = atoi(v);
            else if (!strcmp(k, "audio")) cfg_audio = atoi(v);
            else if (!strcmp(k, "results")) opt_results = atoi(v);
            else if (!strcmp(k, "results_keep")) opt_results_keep = atoi(v);
            else if (!strncmp(k, "cpu_clock_", 10)) hle_aw_clock_cfg(k, v);
            else if (!strcmp(k, "az_exit")) opt_az_exit = atoi(v);
            else if (!strcmp(k, "az_marker")) opt_az_marker = atoi(v);
            else if (!strcmp(k, "disc_dir") && *v && strlen(v) < sizeof disc_dir - 1)   /* build 25 */
                snprintf(disc_dir, sizeof disc_dir, "%s%s", v, v[strlen(v) - 1] == '/' ? "" : "/");
        }
    }
    if (disc_dir[0]) say("config: disc_dir=%s (game data: " HLE_PACK_BOOT_ELF ", discmap.txt, iso_meta.bin, disc/)\n", disc_dir);
    say("config: vcallms=%s max_seconds=%d mainloop_iterations=%llu verbose=%d pace=%d stuck_seconds=%d log_sync=%d\n",
        hle_opt_vcallms == VCALLMS_NOHIT ? "nohit" : hle_opt_vcallms == VCALLMS_HLE ? "hle" : "strict",
        hle_max_seconds, (unsigned long long)hle_opt_mainloop_limit, hle_verbose, opt_pace, opt_stuck, opt_log_sync);
    say("config: prof=%d prof_hw=%d prof_summary=%d prof_capture=%d\n", hle_pz_opt_on, hle_pz_opt_hw,
        hle_pz_opt_summary_s, hle_pz_opt_capture);
    say("config: log_sync=%d log_commit_ms=%d disc_async=%d hitch_window=%d\n", opt_log_sync,
        opt_log_commit_ms, &hle_cdasync_opt ? hle_cdasync_opt : 0, hle_hitch_opt_window);
    if (opt_heap_extra_kb || opt_mem_volatile || opt_mem_reserve_kb >= 0 || opt_mem_fake_free_kb)
        say("config: heap_extra_kb=%d mem_volatile=%d mem_reserve_kb=%d mem_fake_free_kb=%d\n",
            opt_heap_extra_kb, opt_mem_volatile, opt_mem_reserve_kb, opt_mem_fake_free_kb);
    if (!opt_pace_boot)
        say("config: pace_boot=0 - the boot is not paced to the real clock (main loop paced as before)\n");
    say("config: results=%d (%s) results_keep=%d%s\n", opt_results, results_on() ? "results block + "
        "results_<seq>.txt + final screen shot" : "off", opt_results_keep, hle_aw_clock_on() ? "; cpu_clock_mhz set "
        "(clock lines below)" : "");
    if (opt_az_exit || opt_az_marker || hle_aw_clock_early())
        say("config: az_exit=%d (%s) az_marker=%d cpu_clock_early=%d\n", opt_az_exit, opt_az_exit ?
            "exit mode: module stays loaded, ME power callback out, scePowerLock kept until the reboot" :
            "the exit as before: stop + unload, scePowerUnlock", opt_az_marker, hle_aw_clock_early());
}

/* the previous exit's marker (ps2rc_exc.h "exit mode"), read-only */
static void az_marker_report(void)
{
    uint32_t m[PS2RC_AZ_MARK_WORDS];
    if (!ps2rc_exc_marker_read(m)) { say("exit marker: not read (emulator or kuKernelCall refused)\n"); return; }
    uint32_t x = PS2RC_AZ_MARK_XOR;
    for (int i = 0; i < 15; i++) x ^= m[i];
    if (m[0] != PS2RC_AZ_MARK_MAGIC) {
        say("exit marker: none (word 0 %08x) - the last exit did not arm one (az_exit=0 / older module), or "
            "the power went off since, or the OS's ME boot rewrote that RAM\n", (unsigned)m[0]);
        return;
    }
    say("exit marker: previous exit = run sequence %u%s; state %x (armed %u, our ME handler registered %u, "
        "SceMeRpc on our no-op %u); sysevents after exit mode: our handler %u (last %08x), SceMeRpc %u (last %08x); "
        "first ids %08x %08x %08x %08x %08x %08x %08x %08x (bit 31 = SceMeRpc's)%s\n", (unsigned)m[1],
        m[1] + 1u == run_seq ? " (the run just before this one)" : " (NOT the run just before this one)",
        (unsigned)m[2], (unsigned)(m[2] & 1u), (unsigned)((m[2] >> 1) & 1u), (unsigned)((m[2] >> 2) & 1u),
        (unsigned)m[3], (unsigned)m[5], (unsigned)m[4], (unsigned)m[6], (unsigned)m[7], (unsigned)m[8], (unsigned)m[9],
        (unsigned)m[10], (unsigned)m[11], (unsigned)m[12], (unsigned)m[13], (unsigned)m[14],
        x == m[15] ? "" : " - CHECK WORD WRONG (the RAM was partly rewritten)");
}

static uint64_t fnv1a64(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

static void mem_step(const char *after)
{
    say("mem after %s: free_kb=%u max_free_block_kb=%u (real %.2f s)\n", after,
        (unsigned)(sceKernelTotalFreeMemSize() >> 10), (unsigned)(sceKernelMaxFreeMemSize() >> 10), hle_host_seconds());
}

static void fail(const char *what)
{
    az_crashed = 1;                                        /* cpu_auto counts it as a failure */
    say("FATAL: %s\n", what);
    hle_set_exit_reason(what);
    hle_plat_exit(4);
}

/* ------------------------------------------------------------------ memory mode, reserves, heap size
   Before the guest window:
   A. one diagnostics block: the main thread's stack (64 MB mode: near 0x0B9xxxxx-0x0BBxxxxx; 24 MB mode: just below
      0x0A000000), free / largest free user memory, the CFW's SE config bytes (ARK-5: +34 high_memory_use 0 off / 2 auto
      / 3 force16 / 4 forcemax, +27 msspeed, +8 iso_cache_partition). The emulator's numbers are not the console's
      (PPSSPP shows ~5 MB more free): only hardware logs count.
   B. 24 MB mode (ARK did not grant the PSP-2000/3000 memory: free <= 24 MB) or too little memory for the window: a
      clear message on screen + in session.log, then the usual clean exit (X) instead of a failing window allocation.
   E. mem_volatile=1: lock the volatile partition (our big buffers go there, hle_plat_big_alloc); heap_extra_kb: the
      window grows by what the reserve allows. */
#define WINDOW_MAX 0x01FF0000u                     /* RAM end cap: 64 KB below the EE's 32 MB (maskbase end pointers) */
static void mem_mode_check(void)
{
    unsigned tot_kb = (unsigned)(sceKernelTotalFreeMemSize() >> 10), max_kb = (unsigned)(sceKernelMaxFreeMemSize() >> 10);
    int emu = hle_psp_is_emulator();
    say("mem diag: main thread sp=%08x stack %08x-%08x (%u KB) -> %s; free %u KB, largest block %u KB%s\n",
        (unsigned)main_sp, (unsigned)main_stack_lo, (unsigned)main_stack_hi, (main_stack_hi - main_stack_lo) >> 10,
        main_stack_hi > 0x0A000000u ? "64 MB layout (user memory above 0x0A000000)"
                                    : main_stack_hi ? "24 MB layout (user memory ends at 0x0A000000)" : "stack unknown",
        tot_kb, max_kb, emu ? " - EMULATOR: PPSSPP overstates free memory (~5 MB), take numbers only from the PSP" : "");
    uint8_t sc[256];
    int scr = hle_mem_seconfig(sc, sizeof sc);
    uint32_t magic;
    memcpy(&magic, sc, 4);
    char hex[3 * 40 + 1];
    for (int i = 0; i < 40; i++) snprintf(hex + 3 * i, 4, "%02x ", sc[i]);
    hex[3 * 40 - 1] = 0;
    if (scr == 0x7FFFFFFF) say("ark seconfig: not read (emulator: no SystemControl)\n");
    else
        say("ark seconfig: sctrlSEGetConfig=%08x magic=%08x (%s) high_memory_use(+34)=%u (0 off, 2 auto, 3 force16, "
            "4 forcemax) msspeed(+27)=%u iso_cache_partition(+8)=%u (2 = high memory applied); bytes 0-39: %s\n",
            (unsigned)scr, (unsigned)magic, magic == 0xB00B1E55u ? "ARK" : sc[0] == 0xA5 && sc[1] == 0xA5 ?
            "buffer untouched: no SE config" : "not ARK's magic - offsets meaningless", sc[34], sc[27], sc[8], hex);
    /* B: the memory mode */
    unsigned free_kb = opt_mem_fake_free_kb > 0 ? (unsigned)opt_mem_fake_free_kb : tot_kb;
    unsigned big_kb = opt_mem_fake_free_kb > 0 ? (unsigned)opt_mem_fake_free_kb : max_kb;
    unsigned base_kb = (WINDOW_SIZE + 2 * RMB_GUARD) >> 10;
    if (opt_mem_fake_free_kb > 0) say("mem diag: mem_fake_free_kb=%d - TEST: the check below uses %u KB\n",
                                      opt_mem_fake_free_kb, free_kb);
    if (free_kb <= 24576 || big_kb < base_kb + 1024) {
        int mode24 = free_kb <= 24576;
        pspDebugScreenClear();
        pspDebugScreenSetXY(0, 2);
        say("\n**********************************************************************\n");
        if (mode24) {
            say("ARK: extra memory off - this port needs the PSP-2000/3000 64 MB mode.\n");
            say("Enable 'Use Extra Memory' (ARK settings / SETTINGS.TXT: always, highmem, on)\n");
            say("or check the memory stick (a slow / damaged card makes ARK fall back to\n");
            say("the settings in flash1). Free memory now %u KB (24 MB mode).\n", free_kb);
        } else {
            say("Not enough free memory for the game: largest block %u KB, needs %u KB.\n", big_kb, base_kb + 1024);
            say("Disable game plugins (SEPLUGINS) and start it again.\n");
        }
        say("**********************************************************************\n");
        hle_set_exit_reason(mode24 ? "memory: 24 MB mode (ARK extra memory off)" : "memory: not enough free memory");
        hle_plat_exit(4);                          /* final lines, "Press X to exit", the ordered exit */
    }
}

static void mem_layout_setup(void)
{
    unsigned max_kb = (unsigned)(sceKernelMaxFreeMemSize() >> 10), base_kb = (WINDOW_SIZE + 2 * RMB_GUARD) >> 10;
    /* E: the volatile partition first (it does not come from the user partition) */
    if (opt_mem_volatile) {
        vol_lock_rc = hle_mem_vol_lock();
        char vi[320];
        hle_mem_vol_info(vi, sizeof vi);
        say("mem: mem_volatile=1: %s%s\n", vi, vol_lock_rc ? " - NOT used, the build-46 layout" : "");
    }
    if (!opt_heap_extra_kb) return;
    /* the user memory that must stay free after the window (KB): runtime + system modules + margin, the audio
       block (unless the hole takes it), the intro movie (unless the volatile partition takes it), the profiler's
       capture block */
    unsigned reserve = 512;
    int vol = hle_psp_mem_volatile();
    if (cfg_audio && !vol) reserve += 2944;
    if (cfg_movies && !vol) reserve += 1408;
    if (hle_pz_opt_capture > 0) reserve += (unsigned)(hle_pz_opt_capture > 4000 ? 4000 : hle_pz_opt_capture) / 2 + 64;
    if (opt_mem_reserve_kb >= 0) reserve = (unsigned)opt_mem_reserve_kb;
    mem_reserve_kb_used = reserve;
    unsigned cap_kb = (WINDOW_MAX - WINDOW_SIZE) >> 10;
    unsigned want = opt_heap_extra_kb < 0 ? cap_kb : (unsigned)opt_heap_extra_kb;
    if (want > cap_kb) want = cap_kb;
    unsigned room = max_kb > base_kb + reserve ? max_kb - base_kb - reserve : 0;
    unsigned got = (want < room ? want : room) & ~63u;     /* whole 64 KB */
    heap_extra_kb_got = got;
    window_size = WINDOW_SIZE + (got << 10);
    say("mem: heap_extra_kb asked %d (%u KB, cap %u), largest block %u KB - window %u + guards %u - reserve "
        "%u KB = room %u KB -> heap +%u KB: guest RAM end %08x, game heap %u KB (build 46: 19538)%s\n",
        opt_heap_extra_kb, want, cap_kb, max_kb, WINDOW_SIZE >> 10, (2 * RMB_GUARD) >> 10, reserve, room, got,
        (unsigned)window_size, (unsigned)((window_size - 0x8A5D80u - 0x5800u) >> 10),
        got < want ? " (REDUCED: not enough user memory for the asked size)" : "");
}

/* ------------------------------------------------------------------ the guest's data from its own ELF */
static uint32_t load_elf_data(uint8_t *base)
{
    char path[320];
    snprintf(path, sizeof path, "%s" HLE_PACK_BOOT_ELF, data_dir());   /* build 25: cfg disc_dir */
    SceInt64 t = sceKernelGetSystemTimeWide();
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { say("elf_open=FAIL err=%08x path=%s\n", (unsigned)fd, path); fail("cannot open " HLE_PACK_BOOT_ELF); }
    uint8_t hdr[0x34], ph[32];
    uint32_t entry, phoff, p_off, p_vaddr, p_filesz, p_memsz;
    sceIoRead(fd, hdr, sizeof hdr);
    memcpy(&entry, hdr + 0x18, 4);
    memcpy(&phoff, hdr + 0x1C, 4);
    sceIoLseek(fd, phoff, PSP_SEEK_SET);
    sceIoRead(fd, ph, sizeof ph);
    memcpy(&p_off, ph + 4, 4);
    memcpy(&p_vaddr, ph + 8, 4);
    memcpy(&p_filesz, ph + 16, 4);
    memcpy(&p_memsz, ph + 20, 4);
    if (memcmp(hdr, "\x7f" "ELF", 4) || p_vaddr != HLE_CODE_LO || p_vaddr + p_filesz != HLE_DATA_END) {
        say("elf_check=FAIL vaddr=%08x filesz=%08x\n", p_vaddr, p_filesz);
        fail("unexpected ELF layout (not the program of game pack " HLE_PACK_NAME ")");
    }
    uint32_t len = HLE_DATA_END - HLE_CODE_HI;
    sceIoLseek(fd, p_off + (HLE_CODE_HI - p_vaddr), PSP_SEEK_SET);
    int got = sceIoRead(fd, base + HLE_CODE_HI, len);
    sceIoClose(fd);
    SceInt64 t1 = sceKernelGetSystemTimeWide();
    memset(base, 0, HLE_CODE_LO);                                           /* kernel area + scratchpad alias */
    memset(base + HLE_DATA_END, 0, window_size - HLE_DATA_END);             /* BSS, overlay buffer, heap, stack */
    SceInt64 t2 = sceKernelGetSystemTimeWide();
    uint64_t h = fnv1a64(base + HLE_CODE_HI, HLE_BSS_END - HLE_CODE_HI);
    say("elf_data_bytes=%d of %u read_ms=%.1f clear_ms=%.1f data_fnv=%016llx data_ok=%d entry=%08x\n", got, len,
        (double)(t1 - t) / 1000.0, (double)(t2 - t1) / 1000.0, (unsigned long long)h,
        !HLE_PACK_HAVE_DATA_FNV || h == DATA_FNV_EXPECTED, entry);
    if (got != (int)len || (HLE_PACK_HAVE_DATA_FNV && h != DATA_FNV_EXPECTED)) fail("game data read/check failed");
    return entry;
}

/* ------------------------------------------------------------------ HOME -> Exit
   Without a registered exit callback the HOME menu's Exit does nothing (the game could not be left).
   The callback asks the game thread to end the run at its next safe point (hle_check_limits: final report, counter
   thread stopped, crash handler released, sceKernelExitGame - the order the console needs, build 26). A game thread
   that does not react within 3 s (a movie, a long load without safe points) is not waited for; one that is already
   writing its final report gets 15 s. */
static int exit_cb_status = 1;                     /* 1 = callback thread not started */
static int exit_cb(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    hle_exit_requested = 1;
    for (int i = 0; i < 300; i++) {
        sceKernelDelayThread(50000);
        if (i >= 60 && !exiting) break;
    }
    say("HOME: the game thread did not end the run within %s - exiting directly\n", exiting ? "15 s" : "3 s");
    exit_sequence("HOME callback");                /* the same ordered, logged exit */
    return 0;
}

static int exit_cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("hle_exit_cb", exit_cb, NULL);
    exit_cb_status = cb < 0 ? cb : sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static int game_main(SceSize args, void *argp);

int main(int argc, char **argv)
{
    t0_us = sceKernelGetSystemTimeWide();
    {                                              /* the main thread's stack tells the memory mode */
        __asm__ volatile("move %0, $29" : "=r"(main_sp));
        SceKernelThreadInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &mi) >= 0) {
            main_stack_lo = (uint32_t)(uintptr_t)mi.stack;
            main_stack_hi = main_stack_lo + (uint32_t)mi.stackSize;
        }
    }
    strncpy(dir, argc > 0 ? argv[0] : "", sizeof dir - 1);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    hle_aw_auto_boot(dir, (unsigned)t0_us);        /* cpu_auto (nothing without the key / at 0) */
    hle_aw_preclock(dir, 0, (unsigned)t0_us);      /* cpu_clock_at=main (nothing without the key) */
    pspDebugScreenInit();
    scePowerSetClockFrequency(333, 333, 166);
    /* build 30: the exit callback thread first (priority above the game thread, so HOME is served while it runs) */
    SceUID cbt = sceKernelCreateThread("hle_exit_cb", exit_cb_thread, 0x11, 0x4000, THREAD_ATTR_USER, NULL);
    if (cbt < 0 || sceKernelStartThread(cbt, 0, NULL) < 0) exit_cb_status = cbt < 0 ? cbt : -1;
    /* the game runs in its own PSP thread: its stack holds the coroutine stacks of the guest threads */
    SceUID gt = sceKernelCreateThread("hle_game", game_main, 0x20, GAME_STACK, THREAD_ATTR_USER, NULL);
    if (gt < 0 || sceKernelStartThread(gt, 0, NULL) < 0) {
        pspDebugScreenPrintf("FATAL: cannot start the game thread (%08x)\n", (unsigned)gt);
        wait_exit_button();
    }
    for (;;) sceKernelSleepThread();
    return 0;
}

/* build 36: keep the previous run's files - <name> -> <prev> at start, the one before that is
   deleted - so running the same folder twice (e.g. a drift-check repeat of an A/B control) does not lose a log */
static int rotated_log, rotated_prof;
static int rotate_file(const char *name, const char *prev)
{
    char cur[320], old[320];
    snprintf(cur, sizeof cur, "%s%s", dir, name);
    snprintf(old, sizeof old, "%s%s", dir, prev);
    SceIoStat st;
    if (sceIoGetstat(cur, &st) < 0) return 0;              /* nothing to keep */
    sceIoRemove(old);                                      /* the run before the previous one goes */
    return sceIoRename(cur, old) >= 0 ? 1 : -1;
}

/* run info: every run of any folder that shares the data folder gets a sequence number from
   <data dir>run_counter.txt, and the log records the PSP clock and the battery, so an A/B analysis can put runs in
   their real order and see drift (warm-up, battery) whatever order the folders were run in. */
static void log_run_info(void)
{
    char path[320], buf[32];
    unsigned seq = 0;
    snprintf(path, sizeof path, "%srun_counter.txt", data_dir());
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, buf, sizeof buf - 1);
        sceIoClose(fd);
        if (n > 0) { buf[n] = 0; seq = (unsigned)strtoul(buf, NULL, 10); }
    }
    seq++;
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int wrote = 0;
    if (fd >= 0) {
        int n = snprintf(buf, sizeof buf, "%u\n", seq);
        wrote = sceIoWrite(fd, buf, n) == n;
        sceIoClose(fd);
    }
    ScePspDateTime t;
    memset(&t, 0, sizeof t);
    int rtc = sceRtcGetCurrentClockLocalTime(&t);
    run_seq = seq;                                         /* results_<seq>.txt, shot_<seq>_*.bmp */
    snprintf(run_info, sizeof run_info, "run: sequence %u%s (shared counter %s), PSP clock %04u-%02u-%02u "
             "%02u:%02u:%02u%s, battery %d %% %s%s, battery temp %d C, folder %s\n", seq, wrote ? "" :
             " (counter NOT saved)", path, (unsigned)t.year, (unsigned)t.month, (unsigned)t.day, (unsigned)t.hour,
             (unsigned)t.minute, (unsigned)t.second, rtc < 0 ? " (clock read failed)" : "",
             scePowerGetBatteryLifePercent(), scePowerIsBatteryCharging() > 0 ? "charging" : "not charging",
             scePowerIsPowerOnline() > 0 ? ", AC power" : "", scePowerGetBatteryTemp(), dir);
    say("%s", run_info);
}

static int game_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    __asm__ volatile("ctc1 %0, $31" :: "r"(0x01000001));   /* EE FPU model: round toward zero, flush denormals
                                                              (FCR31 is per PSP thread) */
    hle_aw_preclock(dir, 1, (unsigned)t0_us);      /* cpu_clock_at=thread (nothing without the key) */
    rotated_log = rotate_file("session.log", "session_prev.log");
    rotated_prof = rotate_file("prof.bin", "prof_prev.bin");
    snprintf(log_path, sizeof log_path, "%ssession.log", dir);
    SceUID lf = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);   /* empty it */
    if (lf >= 0) { sceIoClose(lf); log_ok = 1; }
    log_sema = sceKernelCreateSema("hle_log", 0, 1, 1, NULL);
    unsigned fw = (unsigned)sceKernelDevkitVersion();
    free_kb_start = (unsigned)(sceKernelTotalFreeMemSize() >> 10);
    uint32_t fcr31;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr31));
    say("==== PS2RC PSP session ====\n");
    say("build=%d target=psp model=%s fw=%x.%x%x cpu_mhz=%d bus_mhz=%d compiler=gcc-%s\n", HLE_BUILD_NUM,
        free_kb_start > 24576 ? "PSP-2000/3000" : "PSP-1000", (fw >> 24) & 0xFF, (fw >> 16) & 0xFF, (fw >> 8) & 0xFF,
        scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), __VERSION__);
    say("translation=%s fcr31=%08x log=write-through (every line; ps2rc.cfg log_sync=0: buffered once the guest runs)\n",
        HLE_TRANSLATION, fcr31);
    say("free_kb_start=%u max_free_block_kb=%u window_kb=%u\n", free_kb_start,
        (unsigned)(sceKernelMaxFreeMemSize() >> 10), WINDOW_SIZE >> 10);
    say("previous run: session.log -> session_prev.log %s, prof.bin -> prof_prev.bin %s (older ones deleted)\n",
        rotated_log > 0 ? "kept" : rotated_log < 0 ? "RENAME FAILED" : "(none)",
        rotated_prof > 0 ? "kept" : rotated_prof < 0 ? "RENAME FAILED" : "(none)");
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    int tr = sceKernelReferThreadStatus(sceKernelGetThreadId(), &ti);
    if (tr < 0 || ti.stackSize < GAME_STACK) {
        say("game thread stack: refer=%08x size=%u\n", (unsigned)tr, (unsigned)ti.stackSize);
        fail("cannot locate the game thread's stack");
    }
    thr_lo = ti.stack;
    thr_hi = thr_lo + ti.stackSize;
    pool_lo = pool_cur = thr_lo + 4096;                    /* the kernel's words at the stack bottom stay untouched */
    pool_hi = pool_lo + GAME_POOL;
    uint32_t sp_now;
    __asm__ volatile("move %0, $29" : "=r"(sp_now));
    say("game thread: stack %p-%p (%u KB), coroutine stack pool %p-%p (%u KB), sp=%08x\n", thr_lo, thr_hi,
        (unsigned)(ti.stackSize >> 10), pool_lo, pool_hi, GAME_POOL >> 10, (unsigned)sp_now);
    game_thid = sceKernelGetThreadId();
    read_config();                                 /* build 22: before the crash handler (prof_hw decides its args) */
    if (hle_aw_clock_early())                      /* cpu_clock_early=1 - the clock change before the crash-
                                                      handler module, the run counter's card I/O, memory, audio, ME */
        hle_aw_clock_boot(say_line);
    log_run_info();
    hle_aw_preclock_report(say_line);              /* what cpu_clock_at did (nothing without it) */
    hle_aw_auto_report(say_line);                  /* what cpu_auto did (nothing without it) */
    if (opt_az_marker) az_marker_report();
    {
        char prx[320], detail[256];
        snprintf(prx, sizeof prx, "%sps2rc_exc.prx", dir);
        Ps2rcPerfBlock *perf = NULL;
        if (hle_pz_opt_hw >= 1 && hle_psp_is_emulator()) {
            say("prof_hw=%d refused: emulator detected (PPSSPP does not map the counters at 0xBC400000 and crashes "
                "on any access)\n", hle_pz_opt_hw);
            hle_pz_opt_hw = 0;
        }
        if (hle_pz_opt_hw >= 1) perf = hle_psp_perf_block();
        int er = ps2rc_exc_install2(prx, crash_report, perf, detail, sizeof detail);
        say("crash handler: %s (%s)\n", er >= 0 ? "armed" : "NOT armed", detail);
        say("exit callback (HOME -> Exit): %s (%08x)\n", exit_cb_status == 0 ? "registered" : "NOT registered",
            (unsigned)exit_cb_status);
        if (perf) {
            char st[256];
            hle_psp_perf_status(st, sizeof st);
            say("hardware counters (prof_hw=%d): %s\n", hle_pz_opt_hw, st);
        }
    }
    if (!hle_aw_clock_early())
        hle_aw_clock_boot(say_line);               /* cpu_clock_mhz (nothing without the key); before any
                                                      other thread, audio, GE or disc work starts */
    mem_step("config");
    mem_mode_check();                              /* diagnostics A, clean stop in 24 MB mode (B) */
#ifdef HLE_SPLASH
    /* the community QR splash of the main build (build.sh SPLASH=1; no run option) -
       before anything else uses the screen; waits for a button, HOME -> Exit ends the run here */
    if (hle_splash_show(dir)) {
        hle_set_exit_reason("HOME: exit requested during the splash");
        hle_plat_exit(0);
    }
    mem_step("splash");
#endif

    /* 1. the guest RAM window (RAM end = the pack's ram_end_psp), with a guard of
       RMB_GUARD (64 KB) of host memory on both sides, filled with 0xEE - back-end v5s4 (maskbase) addresses
       ram + (base & mask) + offset, which lies up to 32 KB outside the window when (base & mask) + offset crosses 0 or
       the end (runtime/r5900_rt.h; the host check build counts such accesses: RMB_CHECK, 0 expected) */
#ifndef RMB_GUARD
#define RMB_GUARD 0x10000u
#endif
    mem_layout_setup();                            /* volatile partition (mem_volatile), heap_extra_kb */
    say("step 1: allocate the guest RAM window (%u KiB + 2 x %u KiB guard, partition 2)\n", window_size >> 10,
        RMB_GUARD >> 10);
    SceUID uid = sceKernelAllocPartitionMemory(2, "guest_ram", PSP_SMEM_Low, window_size + 2 * RMB_GUARD, NULL);
    if (uid < 0) { say("window_alloc=FAIL err=%08x\n", (unsigned)uid); fail("cannot allocate the guest RAM window"); }
    uint8_t *base = (uint8_t *)sceKernelGetBlockHeadAddr(uid) + RMB_GUARD;
    memset(base - RMB_GUARD, 0xEE, RMB_GUARD);
    memset(base + window_size, 0xEE, RMB_GUARD);
    free_kb_window = (unsigned)(sceKernelTotalFreeMemSize() >> 10);
    say("window_alloc=ok host_base=%p free_kb_after_window=%u max_free_block_kb=%u\n", base, free_kb_window,
        (unsigned)(sceKernelMaxFreeMemSize() >> 10));
    if ((uintptr_t)base & 63) fail("guest window not 64-byte aligned");
    hle_ram = base;
    hle_ram_end = window_size;                     /* above the pack's RAM end with heap_extra_kb (the game's heap end) */

    /* 2. the game's data (the code region stays host memory: the hole) */
    say("step 2: load the game's data from " HLE_PACK_BOOT_ELF "\n");
    uint32_t entry = load_elf_data(base);
    hole_lo = hole_cur = base + HLE_CODE_LO;
    hole_hi = base + HLE_CODE_HI;
    say("hole_kb=%u (guest 0x%06x-0x%06x as host memory)\n", (unsigned)((hole_hi - hole_lo) >> 10), HLE_CODE_LO,
        HLE_CODE_HI);
    mem_step("data loaded");

    /* 3. disc map + ISO metadata + the disc files (<dir>disc/; build 25: <disc_dir>disc/ with ps2rc.cfg disc_dir) */
    say("step 3: disc map, ISO metadata\n");
    char disc[320], map[320], meta[320];
    snprintf(disc, sizeof disc, "%sdisc", data_dir());
    snprintf(map, sizeof map, "%sdiscmap.txt", data_dir());
    snprintf(meta, sizeof meta, "%siso_meta.bin", data_dir());
    if (!hle_disc_init(disc, map, meta)) fail("cannot read discmap.txt");
    hle_elf_path = HLE_PACK_BOOT_ELF;
    mem_step("disc map read");

    /* 4. HLE kernel, then run guest thread 1 from the ELF entry */
    say("step 4: HLE kernel (COP0, MMIO model, syscalls, IOP, render hooks, overlays, scheduler)\n");
    hle_set_vcallms_mode(hle_opt_vcallms);
    hle_pz_init();                                 /* build 22: ps2rc.cfg prof=1 - zone timing from here */
    hle_boot_init(entry);
    mem_step("HLE kernel ready");
    say("step 5: watchdog thread\n");
    SceUID wd = sceKernelCreateThread("hle_watchdog", watchdog, 0x11, 0x4000, 0, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
    else say("watchdog thread: FAIL err=%08x (running without it)\n", (unsigned)wd);
    mem_step("watchdog started");
    say("boot: hole_used_kb=%u free_kb=%u - running the game (real %.2f s)\n",
        (unsigned)((hole_cur - hole_lo) >> 10), (unsigned)(sceKernelTotalFreeMemSize() >> 10), hle_host_seconds());
    log_sync = opt_log_sync;                               /* 0: buffered, committed per milestone / 2 s */
    hle_sched_run();
    LOG("scheduler returned\n");
    hle_exit(0);
    return 0;
}
