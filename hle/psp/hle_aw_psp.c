/* hle_aw_psp.c - PSP only. Two independent parts:
 *
 * A. Automatic test results (no photos of the screen needed). Lines of the end-of-run report are collected while
 *    the run ends (the A/B module's screen lines through hle_plat_result_line, selected log lines - main-loop time,
 *    hitch summary, A/B report, clock - through a prefix tee in hle_psp.c's log writer). At the end hle_psp.c writes
 *    them as one block into session.log and into <EBOOT dir>results_<seq>.txt (seq = the shared `run: sequence`
 *    counter), and hle_aw_shot saves the displayed frame buffer as shot_<seq>_<tag>.bmp: the final text console
 *    ("results") and, with an A/B, one game frame per variant (the in-run A/B, ab_shots). The display buffer is read
 *    through the uncached VRAM alias: the end screen is the CPU-written console (hle_ge.c's buffer T, or the boot's
 *    8888 debug screen); a GE frame shown on the display is never a GE draw target (hle_ge.c draws into P and copies
 *    into a buffer that is neither displayed nor pending), so the read races nothing. (PPSSPP's hardware backends keep
 *    GE output on the host GPU: a GE frame read this way is stale or black there - booleans only.) Card space: at most
 *    results_keep (default 20) results_*.txt and as many shot_*.bmp per folder; the oldest sequence numbers go first.
 *
 * B. cpu_clock_mhz=N (at the user's own risk; absent = nothing, the default stays at 333). The method
 *    of the PLL multiplier numerator of
 *    0xBC1000FC, written from kernel mode (kubridge kuKernelCall) one step at a time with interrupts off, RAM-only, reset
 *    by any reboot. Safety rules:
 *      - only ONCE at boot, before the watchdog / audio / Media Engine / GE / disc threads exist, no card I/O near the
 *        write (the developer's engine died when a card write met a multiplier change; ARK's ramp, which does no I/O,
 *        never did): one log line before (committed), 300 ms quiet, the ramp (one unit per step, see below),
 *        300 ms quiet, then the log lines of what happened;
 *      - only on the measured board: family B (09g, PSP-3000: msb 0x0122, den 2) in its known idle state (index 5,
 *        1:1 domains, numerator 73 = 333 MHz); everything else = refused, 333 kept;
 *      - never above numerator 88 = 401 MHz, the developer's measured ceiling (452 MHz switched the console off);
 *      - the clock is checked after the ramp by a calibrated CPU loop (scePowerGetCpuClockFrequency cannot see a PLL
 *        change): outside +-8 % of the expected value, or the register not taking the value -> straight back to 333;
 *      - every cpu_clock_log_s seconds (default 10) one log line from the watchdog: the register's MHz, the loop's MHz,
 *        battery temperature + level, the display's vblank rate; battery temperature >= cpu_clock_maxtemp (default
 *        50 C) twice in a row -> back to 333 at run time (the one run-time change: an emergency, with the log
 *        semaphore held so no session.log write runs next to it);
 *      - the exit sequence ramps back to 333 before sceKernelExitGame (the XMB would otherwise keep the overclock).
 *    cpu_clock_mhz=333 = the same instrument without any register write (the control folders). The emulator has no
 *    PLL: refused there, the instrument lines still run (booleans only).
 *
 *    The first ramp (5 numerator units per kernel call back to back,
 *    a vblank wait before each, 100 ms pauses) switched the 3008 off on the boot ramp. The ramp is now in the shape of
 *    ARK-5's ladder, the only multiplier change with 0 deaths on that console (ARK-5's sources):
 *    ONE unit per kernel call, the current value written first, ~6 ms settle, 100 us pauses, no vblank wait. Two more
 *    keys: cpu_clock_method=ark (ARK-5's own ladder through sctrlHENSetSpeed(483) - it always ends at numerator 80 =
 *    365 MHz on this board, first dipping to 55 = 251 MHz; own = the default) and cpu_clock_restore=0|1 (default 1:
 *    walk back to 333 at exit; 0 = leave it, as ARK does).
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include "ps2rc_kcall.h"                 /* KUBridge kernel call (own import) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_aw_psp.h"
#include "hle_mem_psp.h"                  /* hle_mem_seconfig (is ARK there?) */

/* ================================================================== A. results */
static char res_buf[12288];
static unsigned res_len, res_lines, res_dropped;
static int res_frozen;

void hle_aw_result_add(const char *s, size_t n)
{
    if (res_frozen || !n) return;
    if (res_len + n + 2 > sizeof res_buf) { res_dropped++; return; }
    memcpy(res_buf + res_len, s, n);
    res_len += (unsigned)n;
    if (res_buf[res_len - 1] != '\n') res_buf[res_len++] = '\n';
    res_lines++;
}

void hle_plat_result_line(const char *line) { hle_aw_result_add(line, strlen(line)); }

/* the log lines that belong to the results block (whole lines from LOG / say; checked on the game thread only) */
static const char *const tee_prefix[] = {
    "main-loop real time", "hitch all", "hitch after", "AB on", "AB variant ", "AB report", "AB GE v", "AB median",
    "AB result", "AB floor", "AB REFUSED", "AB part ended", "AB shot", "x_fxskip", "ge_busy: ",
    "clock", "speed: summary", "result=", "CRASH:", "tune ("};
void hle_aw_result_tee(const char *s, size_t n)
{
    if (res_frozen || n < 6) return;
    for (unsigned i = 0; i < sizeof tee_prefix / sizeof tee_prefix[0]; i++) {
        size_t k = strlen(tee_prefix[i]);
        if (n >= k && !memcmp(s, tee_prefix[i], k)) { hle_aw_result_add(s, n); return; }
    }
}

const char *hle_aw_results(unsigned *len, unsigned *lines, unsigned *dropped)
{
    *len = res_len;
    *lines = res_lines;
    *dropped = res_dropped;
    return res_buf;
}

void hle_aw_results_freeze(void) { res_frozen = 1; }

/* keep at most `keep` files <prefix><seq>... in dir: the lowest sequence numbers are deleted first */
#define CAP_MAX 256
int hle_aw_cap_files(const char *dir, const char *prefix, const char *suffix, int keep)
{
    static struct { char name[64]; unsigned seq; } f[CAP_MAX];
    int n = 0, removed = 0;
    size_t pl = strlen(prefix), sl = strlen(suffix);
    SceUID d = sceIoDopen(dir);
    if (d < 0) return -1;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);                                   /* d_private must be 0 (fatms) */
        if (sceIoDread(d, &e) <= 0) break;
        const char *nm = e.d_name;
        size_t l = strlen(nm);
        if (l <= pl + sl || l >= sizeof f[0].name || strncmp(nm, prefix, pl) || strcmp(nm + l - sl, suffix)) continue;
        if (nm[pl] < '0' || nm[pl] > '9') continue;
        if (n >= CAP_MAX) break;
        snprintf(f[n].name, sizeof f[n].name, "%s", nm);
        f[n].seq = (unsigned)strtoul(nm + pl, NULL, 10);
        n++;
    }
    sceIoDclose(d);
    while (n > keep && keep >= 0) {
        int o = 0;
        for (int i = 1; i < n; i++)
            if (f[i].seq < f[o].seq || (f[i].seq == f[o].seq && strcmp(f[i].name, f[o].name) < 0)) o = i;
        char path[320];
        snprintf(path, sizeof path, "%s%s", dir, f[o].name);
        if (sceIoRemove(path) >= 0) removed++;
        f[o] = f[n - 1];
        n--;
    }
    return removed;
}

/* the displayed frame buffer -> <dir>shot_<seq>_<tag>.bmp (24-bit, 480x272); 0 = written, < 0 = not (out = why) */
int hle_aw_shot(const char *dir, unsigned seq, const char *tag, int keep, char *out, unsigned outn)
{
    void *top = NULL;
    int bw = 0, pf = 0;
    out[0] = 0;
    if (sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top) {
        snprintf(out, outn, "no display buffer");
        return -1;
    }
    uint32_t phys = (uint32_t)(uintptr_t)top & 0x1FFFFFFFu;
    if (phys < 0x04000000u || phys >= 0x04400000u || bw < 480 || bw > 1024 || pf < 0 || pf > 3) {
        snprintf(out, outn, "display buffer %08x stride %d format %d not readable", (unsigned)(uintptr_t)top, bw, pf);
        return -2;
    }
    const int W = 480, H = 272, bpp = pf == 3 ? 4 : 2;
    if (phys + (uint32_t)(bw * H * bpp) > 0x04400000u) { snprintf(out, outn, "display buffer beyond VRAM"); return -2; }
    const volatile uint8_t *vram = (const volatile uint8_t *)(uintptr_t)(phys | 0x40000000u);   /* uncached */
    char name[96], path[320];
    snprintf(name, sizeof name, "shot_%u_%s.bmp", seq, tag);
    snprintf(path, sizeof path, "%s%s", dir, name);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { snprintf(out, outn, "cannot create %s (%08x)", name, (unsigned)fd); return -3; }
    uint8_t h[54];
    uint32_t img = (uint32_t)(W * 3 * H);
    memset(h, 0, sizeof h);
    h[0] = 'B';
    h[1] = 'M';
    uint32_t v[] = {54u + img, 0, 54, 40, (uint32_t)W, (uint32_t)H};
    memcpy(h + 2, &v[0], 4);
    memcpy(h + 10, &v[2], 4);
    memcpy(h + 14, &v[3], 4);
    memcpy(h + 18, &v[4], 4);
    memcpy(h + 22, &v[5], 4);
    h[26] = 1;
    h[28] = 24;
    memcpy(h + 34, &img, 4);
    int ok = sceIoWrite(fd, h, sizeof h) == (int)sizeof h;
    static uint8_t rows[16 * 480 * 3];
    for (int y0 = H - 1; ok && y0 >= 0; y0 -= 16) {                /* BMP rows bottom-up, 16 per write */
        int k = 0;
        for (int y = y0; y > y0 - 16 && y >= 0; y--) {
            const volatile uint8_t *r = vram + (size_t)y * (size_t)bw * (size_t)bpp;
            for (int x = 0; x < W; x++) {
                unsigned R, G, B;
                if (pf == 3) {
                    uint32_t p = *(const volatile uint32_t *)(r + 4 * x);   /* ABGR: R in the low byte */
                    R = p & 255u; G = (p >> 8) & 255u; B = (p >> 16) & 255u;
                } else {
                    unsigned p = *(const volatile uint16_t *)(r + 2 * x);
                    if (pf == 0) { R = (p & 31u) << 3; G = ((p >> 5) & 63u) << 2; B = ((p >> 11) & 31u) << 3; }
                    else if (pf == 1) { R = (p & 31u) << 3; G = ((p >> 5) & 31u) << 3; B = ((p >> 10) & 31u) << 3; }
                    else { R = (p & 15u) << 4; G = ((p >> 4) & 15u) << 4; B = ((p >> 8) & 15u) << 4; }
                    R |= R >> 5; G |= G >> 6; B |= B >> 5;
                }
                rows[k++] = (uint8_t)B;
                rows[k++] = (uint8_t)G;
                rows[k++] = (uint8_t)R;
            }
        }
        ok = sceIoWrite(fd, rows, k) == k;
    }
    sceIoClose(fd);
    if (!ok) { snprintf(out, outn, "%s: write failed (card full?)", name); return -4; }
    int rm = keep > 0 ? hle_aw_cap_files(dir, "shot_", ".bmp", keep) : 0;
    snprintf(out, outn, "%s (display %08x stride %d format %d)%s", name, (unsigned)(uintptr_t)top, bw, pf,
             rm > 0 ? ", older shots deleted" : "");
    return 0;
}

/* ================================================================== B. CPU clock (cpu_clock_mhz) */
#define HW(a) (*((volatile unsigned int *)(a)))
#define CK_NUM_REST 73u                     /* family B (09g): numerator of 333 MHz */
#define CK_NUM_MAX 88u                      /* family B: 401 MHz, the developer's measured ceiling */
#define CK_NUM_ARK 80u                      /* ARK-5's ladder ends at num(483) = 483*2/12 = 80 = 365 MHz */
static int ck_req;                          /* cpu_clock_mhz (0 = key absent: nothing at all) */
static int ck_log_s = 10, ck_maxtemp = 50;
static int ck_ark;                          /* cpu_clock_method=ark - ARK-5's own ladder (sctrlHENSetSpeed) */
static int ck_restore = 1;                  /* cpu_clock_restore=0 leaves the PLL as it is at exit */
static int ck_ark_req = 483;                /* cpu_clock_ark_req - the value passed to sctrlHENSetSpeed */
static int ck_early;                        /* cpu_clock_early=1 - before the crash-handler module loads */
void at_sctrlHENSetSpeed(int cpu, int bus);  /* hle_mem_psp.c: SystemCtrlForUser import stub */
enum { CK_OFF, CK_STOCK, CK_OC, CK_REFUSED, CK_FALLBACK, CK_EXTERNAL };
static int ck_state = CK_OFF;
static const char *const ck_state_name[] = {"off", "333 (instrument only)", "overclocked", "refused (333 kept)",
                                            "fell back to 333", "changed outside the game"};
static int ck_emu, ck_kcall_ok, ck_board;        /* ck_board: family B in the known 333 MHz state at boot */
static unsigned ck_regs[4], ck_num_cur = CK_NUM_REST, ck_num_target;
static unsigned ck_ref_us;                  /* the CPU loop at 333 MHz (boot calibration) */
static int ck_boot_mhz, ck_min_mhz = 100000, ck_max_mhz, ck_checks, ck_temp0 = -1000, ck_temp_max = -1000, ck_temp_last;
static int ck_hot, ck_emerg_done;
static char ck_why[200];
static uint32_t ck_t0_us, ck_last_us, ck_last_vc;

void hle_aw_clock_cfg(const char *k, const char *v)
{
    if (!strcmp(k, "cpu_clock_mhz")) ck_req = atoi(v);
    else if (!strcmp(k, "cpu_clock_log_s")) ck_log_s = atoi(v) < 2 ? 2 : atoi(v);
    else if (!strcmp(k, "cpu_clock_maxtemp")) ck_maxtemp = atoi(v);
    else if (!strcmp(k, "cpu_clock_method")) ck_ark = !strcmp(v, "ark");
    else if (!strcmp(k, "cpu_clock_restore")) ck_restore = atoi(v) != 0;
    else if (!strcmp(k, "cpu_clock_ark_req")) ck_ark_req = atoi(v);
    else if (!strcmp(k, "cpu_clock_early")) ck_early = atoi(v) != 0;
}

int hle_aw_clock_on(void) { return ck_req > 0; }
int hle_aw_clock_early(void) { return ck_early; }

/* the numerator ARK-5's doOverclock ends at for sctrlHENSetSpeed(req) on this board (Compat/PSP/src/
   overclock.c: steps of 10 "MHz" from 333 while <= req, num = freq * 2 / 12 truncated): 454 -> 75 (342 MHz, the
   step an earlier in-game re-apply survived), 483 -> 80 (365 MHz); 0 = not a request ARK ladders (343..483) */
static unsigned ark_num(int req)
{
    if (req < 343 || req > 483) return 0u;
    unsigned theo = 333u + 10u * (unsigned)((req - 333) / 10);
    return theo * 2u / 12u;
}

/* KERNEL (kuKernelCall): the four PLL registers, read only */
static int k_pll_read(unsigned dst, unsigned z)
{
    (void)z;
    volatile unsigned *d = (volatile unsigned *)dst;
    d[0] = HW(0xbc100068);
    d[1] = HW(0xbc1000fc);
    d[2] = HW(0xbc200000);
    d[3] = HW(0xbc200004);
    __asm__ volatile("sync");
    return 0;
}

/* KERNEL: ONE numerator unit, in the shape of ARK-5's ladder (Compat/PSP/src/overclock.c:244-270, 0 deaths in ~28
   ladders on the developer's 3008): interrupts off; first the CURRENT value again (the code of the write + settle is then
   in the I-cache when the generator re-locks - ARK starts every step with num(defaultFreq), the value already there),
   then cur +- 1; settle 0x2ffff x 10 instructions (~6 ms at 333, ARK's number); interrupts back. The first ramp wrote up
   to 5 units back to back (73 -> 78 = +23 MHz at once) - the first step of the ramp that switched the console off.
   Only the numerator field of 0xBC1000FC is written; no syscall, no I/O, no FPU in here. */
static int k_pll_walk(unsigned from, unsigned to)
{
    unsigned int st;
    unsigned keep = HW(0xbc1000fc) & 0xffff00ffu;
    unsigned nx = to > from ? from + 1u : to < from ? from - 1u : from;   /* never more than one unit */
    __asm__ volatile(".set push\n.set noreorder\n"
                     "mfc0 %0, $12\nsync\n"
                     "li $t0, 0xfffffffe\nand $t0, %0, $t0\nmtc0 $t0, $12\nsync\nnop\nnop\nnop\n"
                     ".set pop\n" : "=r"(st) : : "$t0", "memory");
    HW(0xbc1000fc) = keep | (from << 8);
    __asm__ volatile("sync");
    HW(0xbc1000fc) = keep | (nx << 8);
    __asm__ volatile(".set push\n.set noreorder\n.set nomacro\n"
                     "sync\nlui $t0, 0x02\nori $t0, $t0, 0xffff\n"
                     "1: nop\nnop\nnop\nnop\nnop\nnop\nnop\naddiu $t0, $t0, -1\nbnez $t0, 1b\nnop\n"
                     ".set pop\n" : : : "$t0", "memory");
    __asm__ volatile(".set push\n.set noreorder\n"
                     "mtc0 %0, $12\nsync\nnop\nnop\nnop\n.set pop\n" : : "r"(st) : "memory");
    return (int)(HW(0xbc1000fc) & 0xffff);
}

static int kcall2(void *fn, unsigned a1, unsigned a2, unsigned *ret)
{
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    a.arg[0] = a1;
    a.arg[1] = a2;
    if (ps2rc_kcall(fn, &a) < 0) return 0;
    if (ret) *ret = a.ret[0];
    return 1;
}

static int is_emulator(void)
{
    int out = 0;
    return sceIoDevctl("kemulator:", 0x00000003, NULL, 0, &out, sizeof out) >= 0;
}

static int mhz_of(unsigned num) { return (int)((333u * num + 36u) / 73u); }

/* a register-only loop (fits in the I-cache, touches no memory): its time scales with the CPU clock alone.
   it is inline asm now - the C loop it replaces was a pure function of a constant, which gcc
   computed once outside the timed region, so every earlier clock log read "CPU loop 1 us" and the after-ramp check
   compared 333 with the target (a working 365 / 401 would have been judged FALLBACK and walked back by our own ramp
   before the next log line). */
static unsigned bench_loop(unsigned iters)
{
    __asm__ volatile(".set push\n.set noreorder\n"
                     "1: addiu %0, %0, -1\n"
                     "   bnez  %0, 1b\n"
                     "   nop\n"
                     ".set pop\n" : "+r"(iters) : : "memory");
    return iters;
}
static volatile unsigned bench_sink;
#define BENCH_ITERS 200000u                 /* ~1.8 ms at 333 MHz (az_oc measured 2 M iterations = 22.06 ms) */
static unsigned bench_us(int runs)          /* the fastest of `runs` (interrupts only make a run slower) */
{
    unsigned best = 0xFFFFFFFFu;
    for (int r = 0; r < runs; r++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        bench_sink = bench_loop(BENCH_ITERS);
        unsigned d = sceKernelGetSystemTimeLow() - t0;
        if (d && d < best) best = d;
    }
    return best;
}
static int bench_mhz(int runs)
{
    unsigned us = bench_us(runs);
    return ck_ref_us && us && us != 0xFFFFFFFFu ? (int)((333ull * ck_ref_us + us / 2u) / us) : 0;
}

static float vblank_hz(unsigned ms)
{
    unsigned v0 = sceDisplayGetVcount(), t0 = sceKernelGetSystemTimeLow();
    sceKernelDelayThread(ms * 1000u);
    unsigned v1 = sceDisplayGetVcount(), t1 = sceKernelGetSystemTimeLow();
    return t1 != t0 ? (float)(v1 - v0) * 1e6f / (float)(t1 - t0) : 0.0f;
}

static int read_regs(void)
{
    unsigned z = 0;
    if (ck_emu || !kcall2((void *)k_pll_read, (unsigned)(uintptr_t)ck_regs, 0, &z)) return 0;
    return 1;
}

/* family B (09g) in the state the developer measured: idle multiplier msb 0x0122 / den 2, ratio index 5, no apply pending,
   1:1 CPU / bus domains, numerator 73 (333 MHz) */
static int board_ok(char *why, unsigned n)
{
    unsigned ctl = ck_regs[0], mul = ck_regs[1];
    unsigned idx = ctl & 0x0fu, num = (mul >> 8) & 0xffu, den = mul & 0xffu, msb = (mul >> 16) & 0xffffu;
    if (den == 0u) { snprintf(why, n, "family C (den 0, PSP-E1000 street): no known PLL formula"); return 0; }
    if (msb != 0x0122u || den != 2u) {
        snprintf(why, n, "not the measured board (msb %04x den %u; family B / 09g PSP-3000 = msb 0122 den 2)", msb, den);
        return 0;
    }
    if ((ctl & 0x80u) || idx != 5u || ck_regs[2] != 0x01ff01ffu || ck_regs[3] != 0x01ff01ffu || num != CK_NUM_REST) {
        snprintf(why, n, "family B but not in the known 333 MHz state (index %u, apply %u, domains %08x/%08x, "
                 "numerator %u)", idx, (ctl >> 7) & 1u, ck_regs[2], ck_regs[3], num);
        return 0;
    }
    return 1;
}

/* ramp from ck_num_cur to `to` ONE unit (~4.6 MHz) per kernel call with 100 us pauses, like ARK-5's ladder (the first
   ramp: 5 units per call, a vblank wait before each, 100 ms pauses) - no I/O in here */
static int ramp_to(unsigned to, unsigned *got_num)
{
    unsigned got = 0;
    while (ck_num_cur != to) {
        unsigned nx = to > ck_num_cur ? ck_num_cur + 1u : ck_num_cur - 1u;
        if (!kcall2((void *)k_pll_walk, ck_num_cur, to, &got)) { *got_num = 0; return 0; }
        ck_num_cur = (got >> 8) & 0xffu;
        if (ck_num_cur != nx) { *got_num = ck_num_cur; return 0; }
        sceKernelDelayThread(100);                                 /* ARK: sceKernelDelayThread(100) between steps */
    }
    *got_num = ck_num_cur;
    return 1;
}


/* cpu_clock_at=main|thread - ARK's sctrlHENSetSpeed(
   cpu_clock_ark_req, req / 2) at the very first line of main() (before the debug screen, threads, log) or of the game
   thread (after its FCR31 setup, before the log rotation): the bisection between the tiny test EBOOT (342 MHz lived 3
   of 3) and the game EBOOT (AZD1 died). It reads ps2rc.cfg itself (the log is not open yet), leaves a breadcrumb file
   az_preclock.txt (open / write / close + sceIoSync) before the call and again after it, measures the spin before /
   after, and the game logs the result later (hle_aw_preclock_report). Emulator: no call. */
static int pc_stage = -1, pc_req, pc_done, pc_t_call, pc_t_back;
static unsigned pc_spin0, pc_spin1;
static unsigned pc_spin(void)
{
    unsigned best = 0xFFFFFFFFu;
    for (int i = 0; i < 3; i++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        bench_sink = bench_loop(2000000u);
        unsigned d = sceKernelGetSystemTimeLow() - t0;
        if (d && d < best) best = d;
    }
    return best;
}
static void pc_mark(const char *dir, const char *txt)
{
    char path[320];
    snprintf(path, sizeof path, "%saz_preclock.txt", dir);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, txt, strlen(txt)); sceIoClose(fd); }
    sceIoSync("ms0:", 0);
}
static int au_cfg = -1;                    /* cpu_auto (below); > 0 = the pre-clock stays off */
void hle_aw_preclock(const char *dir, int stage, unsigned t0_us)
{
    static const char *const names[] = {"main", "thread"};
    if (pc_done || au_cfg > 0) return;
    if (pc_stage < 0) {                                            /* the first caller reads the two keys */
        pc_stage = 99;                                             /* the shared table (hle_cfg.c) */
        (void)dir;                                                 /* = hle_plat_disc_root() (hle_psp.c) */
        const char *a = hle_cfg_get("cpu_clock_at");
        if (a) pc_stage = !strncmp(a, "main", 4) ? 0 : !strncmp(a, "thread", 6) ? 1 : 99;
        const char *r = hle_cfg_get("cpu_clock_ark_req");
        pc_req = r ? atoi(r) : 0;
        if (pc_req < 343 || pc_req > 483) pc_stage = 99;
    }
    if (pc_stage != stage) return;
    pc_done = 1;
    if (is_emulator()) { pc_done = 2; return; }
    char l[200];
    pc_spin0 = pc_spin();
    pc_t_call = (int)((sceKernelGetSystemTimeLow() - t0_us) / 1000u);
    snprintf(l, sizeof l, "pre-clock at %s: calling sctrlHENSetSpeed(%d, %d) at %d ms, spin %u us - if this is the last "
             "line, the call switched the console off\n", names[stage], pc_req, pc_req / 2, pc_t_call, pc_spin0);
    pc_mark(dir, l);
    at_sctrlHENSetSpeed(pc_req, pc_req / 2);
    pc_t_back = (int)((sceKernelGetSystemTimeLow() - t0_us) / 1000u);
    pc_spin1 = pc_spin();
    snprintf(l, sizeof l, "pre-clock at %s: returned at %d ms, spin %u us = %u MHz\n", names[stage], pc_t_back, pc_spin1,
             pc_spin1 ? (333u * pc_spin0 + pc_spin1 / 2u) / pc_spin1 : 0u);
    pc_mark(dir, l);
}
void hle_aw_preclock_report(void (*logf)(const char *line))
{
    char l[400];
    if (!pc_done) return;
    if (pc_done == 2) { logf("clock pre-clock: REFUSED - emulator\n"); return; }
    unsigned r[4] = {0, 0, 0, 0};
    int rd = kcall2((void *)k_pll_read, (unsigned)(uintptr_t)r, 0, NULL);
    snprintf(l, sizeof l, "clock pre-clock at %s: sctrlHENSetSpeed(%d, %d) called at %d ms, returned at %d ms; "
             "spin %u -> %u us = CPU %u MHz; PLL %s mul %08x (numerator %u = %d MHz on 09g), ctl %08x, domains %08x %08x\n",
             pc_stage == 0 ? "main" : "thread", pc_req, pc_req / 2, pc_t_call, pc_t_back, pc_spin0, pc_spin1,
             pc_spin1 ? (333u * pc_spin0 + pc_spin1 / 2u) / pc_spin1 : 0u, rd ? "read" : "NOT read", r[1],
             (r[1] >> 8) & 0xffu, mhz_of((r[1] >> 8) & 0xffu), r[0], r[2], r[3]);
    logf(l);
}

/* ================================================================== cpu_auto
 * "the overclock switches itself on where it can, and off by itself if the game crashes because of it".
 * ps2rc.cfg:  cpu_auto=0 (off, the default) | 342 | 365;  cpu_auto_reset=N (any number: changing it re-arms after an
 * automatic switch-off).  At the first line of main() (where ARK's call lived in every measured
 * run) and only when ALL hold: not the emulator; ARK present (the sctrlSEGetConfig block's magic); the PLL in
 * family B's shape (09g-like: msb 0x0122, den 2, ratio index 5, 1:1 domains, numerator 73..80) - the only board
 * family where ARK's ladder was measured (342 = sctrlHENSetSpeed(454) -> numerator 75, 365 = 483 -> 80); our own
 * register writes are never used (they switched the console off every time).
 * Crash-safe: <data dir>oc_state.txt ("state=... mhz=... cfg=... fails=... reset=...") is written + sceIoSync BEFORE the
 * call as "pending"; it becomes "ok" after 60 s of main loop, or at a clean exit (X / HOME). A boot that finds
 * "pending" (the previous run crashed or lost power while overclocked, before 60 s / a clean exit) does NOT overclock,
 * counts a failure and writes "failed"; the next boot tries again, one step lower (365 -> 342); the second failure
 * writes "disabled" (no overclock until cpu_auto changes or cpu_auto_reset changes). A power-off AFTER the clean exit
 * (exit 6/6 - it happens at 333 too, console-intrinsic) finds "ok" and is ignored. A crash report (exception) at the
 * overclock before "ok" leaves "pending" = a failure. The PLL is left as ARK leaves it at exit (ARK's next boot resets). */
static int au_reset, au_active, au_target, au_fails, au_ran, au_t_call, au_t_back;
static unsigned au_num, au_spin0, au_spin1, au_main_t0, au_ok_done, au_regs[4];
static char au_path[320], au_prev[24], au_why[260];

static int au_key(const char *key, int def)       /* the shared table (hle_cfg.c) */
{
    const char *k = hle_cfg_get(key);
    return k ? atoi(k) : def;
}

static void au_write(const char *state)
{
    char b[160];
    int n = snprintf(b, sizeof b, "state=%s mhz=%d cfg=%d fails=%d reset=%d\n", state, au_target, au_cfg, au_fails,
                     au_reset);
    SceUID fd = sceIoOpen(au_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, b, n); sceIoClose(fd); }
    sceIoSync("ms0:", 0);
}

void hle_aw_auto_boot(const char *dir, unsigned t0_us)
{
    char ddir[256];
    SceUID fd;
    au_cfg = 0;
    au_cfg = au_key("cpu_auto", 0);                /* the shared table (hle_cfg.c; dir = its folder) */
    au_reset = au_key("cpu_auto_reset", 0);
    if (au_cfg == 0) return;
    au_ran = 1;
    if (au_cfg != 342 && au_cfg != 365) {
        snprintf(au_why, sizeof au_why, "cpu_auto=%d is not a supported value (0, 342, 365): off", au_cfg);
        return;
    }
    snprintf(ddir, sizeof ddir, "%s", dir);
    const char *dd = hle_cfg_get("disc_dir");
    if (dd) {
        int i = 0;
        for (; *dd && *dd != '\n' && *dd != '\r' && i < (int)sizeof ddir - 2; dd++) ddir[i++] = *dd;
        if (i && ddir[i - 1] != '/') ddir[i++] = '/';
        ddir[i] = 0;
    }
    snprintf(au_path, sizeof au_path, "%soc_state.txt", ddir);
    char st[160] = "", stname[24] = "none";
    int smhz = 0, scfg = 0, sfails = 0, sreset = 0;
    fd = sceIoOpen(au_path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int m = sceIoRead(fd, st, sizeof st - 1);
        sceIoClose(fd);
        st[m > 0 ? m : 0] = 0;
        if (sscanf(st, "state=%23s mhz=%d cfg=%d fails=%d reset=%d", stname, &smhz, &scfg, &sfails, &sreset) < 5)
            snprintf(stname, sizeof stname, "unreadable");
    }
    snprintf(au_prev, sizeof au_prev, "%s", stname);
    au_target = au_cfg;
    if (scfg != au_cfg || sreset != au_reset || !strcmp(stname, "none") || !strcmp(stname, "unreadable")) {
        au_fails = 0;                                              /* new value / reset number / no file: armed */
        if (strcmp(stname, "none")) snprintf(au_prev, sizeof au_prev, "re-armed (was %s)", stname);
    } else {
        au_fails = sfails;
        if (!strcmp(stname, "pending")) {                          /* the previous overclocked run never got to ok */
            au_fails++;
            au_target = smhz;
            au_write(au_fails >= 2 ? "disabled" : "failed");
            snprintf(au_why, sizeof au_why, "OC skipped after a failed run (the previous run at %d MHz crashed or "
                     "lost power before 60 s of play / a clean exit): failure %d of 2%s", smhz, au_fails,
                     au_fails >= 2 ? " - overclock now DISABLED (change cpu_auto or cpu_auto_reset to retry)"
                                   : " - the next run tries again one step lower");
            return;
        }
        if (!strcmp(stname, "disabled")) {
            snprintf(au_why, sizeof au_why, "overclock DISABLED after 2 failed runs (change cpu_auto or set "
                     "cpu_auto_reset to another number to retry)");
            return;
        }
    }
    if (is_emulator()) { snprintf(au_why, sizeof au_why, "REFUSED - emulator (no PLL, no ARK)"); return; }
    uint8_t cfgb[64];
    int sr = hle_mem_seconfig(cfgb, sizeof cfgb);
    uint32_t magic = (uint32_t)cfgb[0] | (uint32_t)cfgb[1] << 8 | (uint32_t)cfgb[2] << 16 | (uint32_t)cfgb[3] << 24;
    if (sr < 0 || magic != 0xB00B1E55u) {
        snprintf(au_why, sizeof au_why, "REFUSED - ARK not detected (sctrlSEGetConfig %08x, magic %08x)", (unsigned)sr,
                 (unsigned)magic);
        return;
    }
    if (!kcall2((void *)k_pll_read, (unsigned)(uintptr_t)au_regs, 0, NULL)) {
        snprintf(au_why, sizeof au_why, "REFUSED - kernel call refused (kubridge)");
        return;
    }
    unsigned mul = au_regs[1], num0 = (mul >> 8) & 0xffu;
    if ((mul >> 16) != 0x0122u || (mul & 0xffu) != 2u || (au_regs[0] & 0x8fu) != 5u || au_regs[2] != 0x01ff01ffu ||
        au_regs[3] != 0x01ff01ffu || num0 < 73u || num0 > 80u) {
        snprintf(au_why, sizeof au_why, "REFUSED - not a tested board (PLL ctl %08x mul %08x domains %08x %08x; tested: "
                 "family B / 09g PSP-3000, mul 0122nn02 with numerator 73..80)", au_regs[0], mul, au_regs[2], au_regs[3]);
        return;
    }
    if (au_fails >= 1 && au_target == 365) au_target = 342;        /* one failure: one step lower */
    int req = au_target == 365 ? 483 : 454;
    unsigned want = au_target == 365 ? 80u : 75u;
    au_write("pending");
    au_spin0 = pc_spin();
    au_t_call = (int)((sceKernelGetSystemTimeLow() - t0_us) / 1000u);
    at_sctrlHENSetSpeed(req, req / 2);
    au_t_back = (int)((sceKernelGetSystemTimeLow() - t0_us) / 1000u);
    au_spin1 = pc_spin();
    kcall2((void *)k_pll_read, (unsigned)(uintptr_t)au_regs, 0, NULL);
    au_num = (au_regs[1] >> 8) & 0xffu;
    if (au_num == want) { au_active = 1; return; }
    au_write("ok");                                                /* nothing raised: nothing to protect */
    snprintf(au_why, sizeof au_why, "ARK's ladder did not reach numerator %u (register %08x): the game runs at %d MHz",
             want, au_regs[1], mhz_of(au_num));
}

int hle_aw_auto_active(void) { return au_active ? au_target : 0; }

void hle_aw_auto_report(void (*logf)(const char *line))
{
    char l[600];
    if (!au_ran) return;
    if (au_active)
        snprintf(l, sizeof l, "clock cpu_auto=%d: ACTIVE %d MHz (%s) - sctrlHENSetSpeed(%d) at %d ms, back at "
                 "%d ms, spin %u -> %u us = CPU %u MHz, PLL numerator %u; state file %s: pending until 60 s of play or a "
                 "clean exit (previous state: %s, failures %d of 2, cpu_auto_reset=%d)\n", au_cfg, au_target,
                 au_target != au_cfg ? "one step lower after a failure" : "as asked", au_target == 365 ? 483 : 454,
                 au_t_call, au_t_back, au_spin0, au_spin1, au_spin1 ? (333u * au_spin0 + au_spin1 / 2u) / au_spin1 : 0u,
                 au_num, au_path, au_prev, au_fails, au_reset);
    else
        snprintf(l, sizeof l, "clock cpu_auto=%d: OFF this run, 333 MHz - %s (state file %s, previous state "
                 "%s, failures %d, cpu_auto_reset=%d)\n", au_cfg, au_why[0] ? au_why : "-", au_path[0] ? au_path : "-",
                 au_prev[0] ? au_prev : "-", au_fails, au_reset);
    logf(l);
}

/* watchdog, once per second: "ok" after 60 s of main loop at the overclock; returns a line length (0 = nothing) */
int hle_aw_auto_tick(char *out, unsigned n)
{
    if (!au_active || au_ok_done || !hle_mainloop_iters) return 0;
    unsigned now = sceKernelGetSystemTimeLow();
    if (!au_main_t0) { au_main_t0 = now | 1u; return 0; }
    if (now - au_main_t0 < 60000000u) return 0;
    au_ok_done = 1;
    au_write("ok");
    return snprintf(out, n, "clock cpu_auto: %d MHz stable for 60 s of play - state file: ok\n", au_target);
}

/* the exit sequence (clean = X / HOME, not a crash report / fatal error) */
int hle_aw_auto_exit(int clean, char *out, unsigned n)
{
    if (!au_active || au_ok_done) return 0;
    au_ok_done = 1;
    if (clean) {
        au_write("ok");
        return snprintf(out, n, "clock cpu_auto: clean exit at %d MHz before 60 s of play - state file: ok\n",
                        au_target);
    }
    return snprintf(out, n, "clock cpu_auto: the run ends with a crash / fatal error at %d MHz before 60 s of "
                    "play - state file left 'pending': the next boot skips the overclock and counts a failure\n",
                    au_target);
}

/* logf: hle_psp.c's say() (session.log line committed at once at boot + the screen) */
void hle_aw_clock_boot(void (*logf)(const char *line))
{
    char l[560];
    if (ck_req <= 0) return;
    ck_t0_us = ck_last_us = sceKernelGetSystemTimeLow();
    ck_last_vc = sceDisplayGetVcount();
    ck_emu = is_emulator();
    ck_temp0 = ck_temp_max = ck_temp_last = scePowerGetBatteryTemp();
    bench_us(3);                                                   /* warm the I-cache */
    ck_ref_us = bench_us(9);
    ck_boot_mhz = 333;
    float vb0 = vblank_hz(500);
    ck_kcall_ok = read_regs();
    snprintf(l, sizeof l, "clock: cpu_clock_mhz=%d cpu_clock_method=%s cpu_clock_restore=%d cpu_clock_log_s=%d "
             "cpu_clock_maxtemp=%d; now: scePower "
             "cpu %d / bus %d MHz, CPU loop %u us (= the 333 MHz reference), vblank %.2f Hz, battery %d C %d %%; PLL "
             "registers %s ctl %08x mul %08x cpu %08x bus %08x\n", ck_req, ck_ark ? "ark" : "own", ck_restore,
             ck_log_s, ck_maxtemp,
             scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), ck_ref_us, (double)vb0, ck_temp0,
             scePowerGetBatteryLifePercent(), ck_emu ? "not read (emulator)" : ck_kcall_ok ? "read:" : "NOT readable "
             "(kuKernelCall refused - kubridge missing?)", ck_regs[0], ck_regs[1], ck_regs[2], ck_regs[3]);
    logf(l);
    char bw[200] = "";
    ck_board = !ck_emu && ck_kcall_ok && board_ok(bw, sizeof bw);
    if (ck_req <= 333) {
        ck_state = CK_STOCK;
        snprintf(l, sizeof l, "clock: 333 MHz kept (control: instrument only, no register written); board: "
                 "%s\n", ck_board ? "family B (09g) in the known 333 MHz state" : bw[0] ? bw : "not checked");
        logf(l);
        return;
    }
    ck_state = CK_REFUSED;
    if (ck_emu) snprintf(ck_why, sizeof ck_why, "emulator (PPSSPP has no PLL)");
    else if (!ck_kcall_ok) snprintf(ck_why, sizeof ck_why, "kernel call refused");
    else if (!ck_board) snprintf(ck_why, sizeof ck_why, "%s", bw);
    else if (ck_ark) {                                             /* ARK's ladder ends at 80 whatever we ask */
        if (ck_req <= 333) snprintf(ck_why, sizeof ck_why, "%d MHz is not above 333", ck_req);
        else if (!ark_num(ck_ark_req) || ark_num(ck_ark_req) > CK_NUM_ARK)
            snprintf(ck_why, sizeof ck_why, "cpu_clock_ark_req=%d is not a request ARK-5 ladders (343..483)", ck_ark_req);
        else ck_num_target = ark_num(ck_ark_req);                  /* 483 -> 80, 454 -> 75 */
    } else {
        unsigned num = ((unsigned)ck_req * 73u + 166u) / 333u;
        if (num > CK_NUM_MAX || ck_req > 401)
            snprintf(ck_why, sizeof ck_why, "%d MHz is above 401 MHz (numerator 88), the ceiling measured on this "
                     "board (452 MHz switched it off)", ck_req);
        else if (num <= CK_NUM_REST) snprintf(ck_why, sizeof ck_why, "%d MHz is not above 333", ck_req);
        else ck_num_target = num;
    }
    if (!ck_num_target) {
        snprintf(l, sizeof l, "clock: REFUSED - %s; the game runs at 333 MHz\n", ck_why);
        logf(l);
        return;
    }
    if (ck_ark)
        snprintf(l, sizeof l, "clock: ARK-5 ladder: sctrlHENSetSpeed(%d, %d) -> ARK writes numerator 55 "
                 "(251 MHz), then climbs to %u = %d MHz in its own steps (the ladder with 0 deaths on this 3008); %s; no "
                 "card I/O until it is done - if this is the last line, the ladder switched the console off\n",
                 ck_ark_req, ck_ark_req / 2, ck_num_target, mhz_of(ck_num_target), ck_early
                 ? "EARLY - before the crash-handler module, the volatile partition, audio, ME, GE"
                 : "after the crash-handler module");
    else
        snprintf(l, sizeof l, "clock: ramp 333 -> %d MHz (numerator %u -> %u, ONE unit per step in ARK's "
                 "ladder shape, ~6 ms settle + 100 us pause each, no card I/O until it is done) - if this is the last "
                 "line, the ramp switched the console off\n", mhz_of(ck_num_target), CK_NUM_REST, ck_num_target);
    logf(l);
    sceKernelDelayThread(300000);                                  /* the card driver's background work ends */
    unsigned got = 0;
    int ok;
    if (ck_ark) {
        at_sctrlHENSetSpeed(ck_ark_req, ck_ark_req / 2);           /* ARK's overclockHandler: currFreq < req <= 483 */
        ok = read_regs();
        got = ck_num_cur = ok ? (ck_regs[1] >> 8) & 0xffu : 0u;
        if (ok && (ck_regs[1] >> 16) != 0x0122u) ok = 0, ck_num_cur = 0;  /* not ours to walk back */
    } else
        ok = ramp_to(ck_num_target, &got);
    sceKernelDelayThread(300000);
    int mhz = bench_mhz(9), want = mhz_of(ck_num_target);
    float vb1 = vblank_hz(500);
    int dev = want ? (mhz - want) * 1000 / want : 0;              /* per mille */
    int rd = read_regs();
    unsigned reg_num = (ck_regs[1] >> 8) & 0xffu;
    ck_boot_mhz = mhz;
    if (ok && rd && reg_num == ck_num_target && dev > -80 && dev < 80) {
        ck_state = CK_OC;
        snprintf(l, sizeof l, "clock: OVERCLOCKED - register numerator %u = %d MHz, CPU loop %d MHz (%+d.%d %% "
                 "vs expected), vblank %.2f Hz, scePower still says %d MHz (it cannot see the PLL)\n", reg_num, want, mhz,
                 dev / 10, (dev < 0 ? -dev : dev) % 10, (double)vb1, scePowerGetCpuClockFrequencyInt());
        logf(l);
        return;
    }
    snprintf(ck_why, sizeof ck_why, "%s: ramp %s (numerator now %u, register %s %u), CPU loop %d MHz vs %d expected",
             !ok ? "the register did not take a step" : !rd ? "register not readable after the ramp"
             : reg_num != ck_num_target ? "register mismatch" : "CPU loop outside +-8 %",
             ok ? "done" : "STOPPED", got, rd ? "reads" : "unread", reg_num, mhz, want);
    unsigned g2 = 0;
    if (ck_num_cur != CK_NUM_REST && ck_num_cur) ramp_to(CK_NUM_REST, &g2);
    ck_state = CK_FALLBACK;
    ck_boot_mhz = bench_mhz(9);
    snprintf(l, sizeof l, "clock: FALLBACK to 333 - %s; numerator back to %u, CPU loop %d MHz\n", ck_why,
             ck_num_cur, ck_boot_mhz);
    logf(l);
}

/* watchdog thread, once per second. Returns the length of a line for session.log (0 = nothing); *emergency = 1 asks
   the caller to run hle_aw_clock_down with the log semaphore held */
int hle_aw_clock_tick(char *out, unsigned n, int *emergency)
{
    *emergency = 0;
    if (ck_req <= 0) return 0;
    uint32_t now = sceKernelGetSystemTimeLow();
    if (now - ck_last_us < (uint32_t)ck_log_s * 1000000u) return 0;
    unsigned vc = sceDisplayGetVcount();
    float vb = now != ck_last_us ? (float)(vc - ck_last_vc) * 1e6f / (float)(now - ck_last_us) : 0.0f;
    ck_last_us = now;
    ck_last_vc = vc;
    int mhz = bench_mhz(3), temp = scePowerGetBatteryTemp(), pct = scePowerGetBatteryLifePercent();
    int rd = ck_board && read_regs();                             /* the measured board only (read-only call) */
    unsigned num = rd ? (ck_regs[1] >> 8) & 0xffu : 0u;
    ck_checks++;
    if (mhz > 0 && mhz < ck_min_mhz) ck_min_mhz = mhz;
    if (mhz > ck_max_mhz) ck_max_mhz = mhz;
    if (temp > ck_temp_max) ck_temp_max = temp;
    ck_temp_last = temp;
    const char *note = "";
    if (rd && num != ck_num_cur && ck_state != CK_EXTERNAL) {
        ck_state = CK_EXTERNAL;                                    /* e.g. sleep / resume reprogrammed the PLL */
        note = " - the PLL numerator CHANGED outside the game (sleep / resume?): not re-applied";
    }
    if (ck_state == CK_OC && temp >= ck_maxtemp) {
        if (++ck_hot >= 2 && !ck_emerg_done) { *emergency = 1; note = " - battery too hot twice: back to 333 now"; }
    } else ck_hot = 0;
    return snprintf(out, n, "clock t=%u s: register %s%u = %d MHz, CPU loop %d MHz, battery %d C %d %%, "
                    "vblank %.2f Hz, state %s%s\n", (unsigned)((now - ck_t0_us) / 1000000u), rd ? "numerator " : "",
                    num, rd ? mhz_of(num) : 0, mhz, temp, pct, (double)vb, ck_state_name[ck_state], note);
}

/* back to 333 (emergency at run time, or the exit sequence): returns a line for the log */
int hle_aw_clock_down(const char *who, char *out, unsigned n)
{
    if (ck_req <= 0) return 0;
    int rd = ck_kcall_ok && !ck_emu && read_regs();
    unsigned num = rd ? (ck_regs[1] >> 8) & 0xffu : ck_num_cur;
    if (!rd || num == CK_NUM_REST || (ck_regs[1] >> 16) != 0x0122u)
        return snprintf(out, n, "clock %s: nothing to restore (%s, numerator %u)\n", who,
                        rd ? "register at 333 or not the measured board" : "register not read", num);
    if (!ck_restore && !strcmp(who, "at exit"))                     /* ARK itself never walks back */
        return snprintf(out, n, "clock %s: numerator %u left as it is (cpu_clock_restore=0): the XMB keeps "
                        "%d MHz until sleep / power off / the next game\n", who, num, mhz_of(num));
    ck_num_cur = num;
    sceKernelDelayThread(200000);
    unsigned got = 0;
    int ok = ramp_to(CK_NUM_REST, &got);
    sceKernelDelayThread(200000);
    int mhz = bench_mhz(5);
    ck_emerg_done = 1;
    if (ok && ck_state == CK_OC) ck_state = CK_FALLBACK;
    return snprintf(out, n, "clock %s: numerator %u -> %u (%s), CPU loop now %d MHz\n", who, num, got,
                    ok ? "ok" : "a step did NOT take", mhz);
}

void hle_aw_clock_report(void)
{
    if (ck_req <= 0) return;
    LOG("clock: summary - asked %d MHz, state %s%s%s; after the boot ramp the CPU loop read %d MHz; %d "
        "checks during the run: CPU loop min %d / max %d MHz; battery %d C at the start, max %d, last %d\n", ck_req,
        ck_state_name[ck_state], ck_why[0] ? " - " : "", ck_why, ck_boot_mhz, ck_checks,
        ck_checks ? ck_min_mhz : 0, ck_max_mhz, ck_temp0, ck_temp_max, ck_temp_last);
}
