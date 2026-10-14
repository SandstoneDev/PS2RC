/* hle_plat.h - what the PoC-6 HLE core (src/hle/hle_*.c) needs from the platform it runs on.
 *
 * Two implementations of the same interface:
 *   host (x86-64 Linux, WSL): src/hle/hle_host.c - ucontext coroutines on mmap'ed stacks, POSIX files, SIGALRM watchdog
 *   PSP  (Allegrex, pspsdk):   src/hle/psp/hle_psp.c + psp/hle_ctx.S - a hand-written context switch on stacks carved
 *                              from the game PSP thread's own stack (build 14; build 13 put them in the guest code hole
 *                              and the PSP-3000 died at the first sceIo call from a coroutine), sceIo files, a watchdog
 *                              thread, session.log
 * The core never calls an OS function directly; everything platform-specific goes through these functions.
 */
#ifndef HLE_PLAT_H
#define HLE_PLAT_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ host contexts (guest threads = coroutines) */
#ifdef HLE_PSP
/* Allegrex callee-saved state: s0-s7, fp, sp, ra, gp (12 words) + f20-f31 (12 words); see psp/hle_ctx.S */
typedef struct { uint32_t w[24]; } hle_hostctx;
/* host stacks come from the game PSP thread's stack pool (psp/hle_psp.c); PPSSPP high-water marks to the main loop:
   thread 1 5 KB, the others <= 3 KB */
#define HLE_STACK_MAIN   (512u * 1024u)       /* guest thread 1 (crt0/main) */
#define HLE_STACK_THREAD (128u * 1024u)       /* every other guest thread */
#else
#include <ucontext.h>
typedef ucontext_t hle_hostctx;
#define HLE_STACK_MAIN   (256u << 20)         /* MAP_NORESERVE: only touched pages cost memory */
#define HLE_STACK_THREAD (64u << 20)
#endif

void *hle_plat_stack_alloc(size_t size);                 /* host stack for a guest thread (kept for reuse) */
void hle_plat_ctx_make(hle_hostctx *ctx, void *stack, size_t size, void (*fn)(void));
void hle_plat_ctx_swap(hle_hostctx *save, hle_hostctx *load);
size_t hle_plat_stack_used(void *stack, size_t size);    /* high-water mark of a painted stack (0: unknown) */

/* ------------------------------------------------------------------ memory for big runtime buffers */
void *hle_plat_big_alloc(size_t size);                   /* zeroed; PSP: the hole; dies when out of memory */

/* ------------------------------------------------------------------ time */
double hle_host_seconds(void);                           /* real seconds since start */
/* idle with nothing to run: the next guest event is due at virtual time `vtime` (EE cycles). Paced platforms wait
   until real time reaches it (the guest never runs ahead of real time while idle); unpaced ones return at once. */
void hle_plat_pace(uint64_t vtime);
/* (hle_bl.c): the clock hle_plat_pace waits for, in us (virtual time `v` is due when this reaches
   v / EE_HZ s), and moving it: +us = the clock goes back by us (lag forgiven), -us = forward */
uint64_t hle_plat_pace_now_us(void);
void hle_plat_pace_adjust(int64_t us);

/* ------------------------------------------------------------------ logging backend */
void hle_plat_log_write(const char *s, size_t n);
void hle_plat_log_flush(void);
void hle_plat_milestone(const char *line);               /* a milestone line (PSP: also on screen) */
/* a line of the end-of-run results block (PSP: session.log block +
   results_<seq>.txt; host: nothing - the log has it); a screenshot of the displayed frame (PSP: shot_<seq>_<tag>.bmp in
   the EBOOT folder, 0 = saved; host: -1). why = one line for the log. */
void hle_plat_result_line(const char *line);
int hle_plat_shot(const char *tag, char *why, unsigned why_n);

/* ------------------------------------------------------------------ read-only files (disc, maps) */
int hle_plat_open(const char *path);                     /* handle >= 0 (PSP: virtual, opened on demand) or -1 */
int64_t hle_plat_pread(int h, uint64_t off, void *dst, uint32_t n);
int64_t hle_plat_size(int h);
const char *hle_plat_disc_root(void);                    /* directory of the disc files ("" = use the map path) */

/* ------------------------------------------------------------------ input (build 25) */
/* the player's controls as a DualShock 2 on port 0: PS2 button bits, active HIGH (bit 0 SELECT ... bit 15 SQUARE);
   sticks[] = right X, right Y, left X, left Y (0x80 = centre; left as they are when nothing moves).
   PSP: src/hle/psp/hle_pad.c; host: nothing pressed (deterministic runs) */
uint16_t hle_plat_pad_read(uint8_t sticks[4]);

/* ------------------------------------------------------------------ diagnostics / end of run */
void hle_plat_backtrace(void);                           /* host call chain of the running guest code, if any */
void hle_plat_note_mmio(void *ra, uint32_t addr);        /* trap-set profiling (host): who accessed hardware */
void hle_plat_exit(int code) __attribute__((noreturn));  /* after the core's final report */
const char *hle_plat_fn_name(void *host_pc);             /* symbol of generated code at a host pc, or NULL */

/* ------------------------------------------------------------------ profiler (build 22, src/hle/hle_prof.c) */
/* ALLEGREX hardware counters (PSP with prof_hw >= 1; the host has none): the 64-bit sums of every counter word since
   the counter thread armed (word index = register offset / 4, see src/psp_exc/ps2rc_exc.h) and the microseconds they
   cover; 0 = no counters */
int hle_plat_hw_totals(uint64_t tot[20], uint64_t *us);
/* the free-running counter registers right now (PSP prof_hw = 2: one kernel call, ~65 us); 0 = no door */
int hle_plat_hw_door(uint32_t raw[20]);
/* memory for the profiler's capture (PSP: a partition block, NOT the small newlib heap); NULL = none */
void *hle_plat_prof_alloc(size_t n);
/* writes a whole file next to the log (PSP: the EBOOT directory; host: --prof-bin decides the name); 0 = ok */
int hle_plat_write_file(const char *name, const void *data, size_t n);

#endif
