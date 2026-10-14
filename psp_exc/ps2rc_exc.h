/* ps2rc_exc.h - crash handler for the PS2RC PSP EBOOTs (build 15) + the ALLEGREX hardware counters (build 22).
 *
 * A user-mode EBOOT crashing on the PSP (address error, bus error, trap, ...) just freezes the console, which then
 * switches itself off: no log line says where. ps2rc_exc.prx is a tiny KERNEL module (needs a CFW such as ARK-4) that
 * registers a default exception handler (src/psp_exc/exc_asm.S). On an exception it copies the faulting thread's
 * registers + EPC / Cause / BadVAddr / Status into the EBOOT's block below, then resumes that thread in USER mode in
 * the EBOOT's report function (a0 = the block), on the thread's own stack 4 KB below its fault sp (the PSP kernel
 * kills a thread whose sp leaves its own stack - see tests/phase6psp/ctxtest), with the EBOOT's gp. The report
 * function writes session.log and waits for X. One shot: a second exception spins (the console then switches off).
 *
 * Block layout (32-bit words; 44-46 = install diagnostics): 0-31 GPR r0-r31 (v0/v1 as the PSP exception manager saved them in COP0 control
 * registers $4/$5), 32 EPC, 33 Cause, 34 BadVAddr, 35 Status, 36 LO, 37 HI, 40 PS2RC_EXC_HIT once it fired,
 * 47 = the sceKernelRegisterDefaultExceptionHandler result (written by module_start).
 *
 * Hardware counters - OFF unless the EBOOT passes a counter block
 * (Ps2rcExcArgs.perf != 0, run option prof_hw >= 1). The ALLEGREX profiler registers at 0xBC400000 are readable from
 * kernel mode only, and PPSSPP does not map them at all (any access crashes the emulator), so the EBOOT never passes
 * the block in PPSSPP and the module only takes it when the crash handler is armed (= a real CFW kernel). Design taken
 * from an earlier hardware-verified debug module of the same author: a kernel thread (priority 0x12, 4 KB stack)
 * wakes every period_us (50 ms), reads the 20 words 0x00-0x4C (0x50 killed the reading thread on a PSP-3000 - never
 * read), never writes a counter while counting (only PROFEN = 1 once if it was 0; at its quit it
 * puts the block back - PROFEN 0, words 1-19 cleared, read back into exit_*), and accumulates the 32-bit deltas into 64-bit sums
 * (a 32-bit counter at 333 MHz wraps in ~13 s; 50 ms windows never wrap). `step` is the breadcrumb of the last
 * register access (a kernel thread that dies is removed silently). The EBOOT reads the sums under `seq` (odd while
 * the thread updates them). `readfn` publishes a synchronous reader for kuKernelCall (kubridge), used only with
 * prof_hw=2 at a few zone boundaries (~65 us per call measured on the PSP-3000).
 * Word index = register offset / 4 (pspsdk pspdebug.h PspDebugProfilerRegs; checked on a PSP-3000 by a calibration run):
 *   0 enable, 1 system clock cycles (PLL units), 2 CPU geared cycles, 3 internal stall, 4 memory stall, 5 COPz (FPU)
 *   busy stall, 6 VFPU busy stall, 7 sleep cycles, 8 bus busy cycles (PLL units), 9 uncached loads, 10 uncached stores,
 *   11 cached loads, 12 cached stores, 13 I-cache misses, 14 D-cache misses, 15 D-cache writebacks, 16 COP0, 17 FPU,
 *   18 VFPU instructions, 19 local bus (pspsdk: local_bus; 0x4C, read on the PSP-3000 without harm).
 */
#ifndef PS2RC_EXC_H
#define PS2RC_EXC_H

#include <stdint.h>

#define PS2RC_EXC_WORDS 48
#define PS2RC_EXC_EPC 32
#define PS2RC_EXC_CAUSE 33
#define PS2RC_EXC_BADVADDR 34
#define PS2RC_EXC_STATUS 35
#define PS2RC_EXC_LO 36
#define PS2RC_EXC_HI 37
#define PS2RC_EXC_FLAG 40
#define PS2RC_EXC_REGRESULT 47
#define PS2RC_EXC_HIT 0xC4A5C4A5u
#define PS2RC_EXC_MAGIC 0x50533252u                 /* "PS2R" */

/* module_start argument (sceKernelStartModule argp). Build 15 EBOOTs pass the first four words (16 bytes); a module
   older than build 22 ignores `perf`, so the EBOOT then sees no PS2RC_PERF_MAGIC in its block (reported, not guessed) */
typedef struct {
    uint32_t magic;          /* PS2RC_EXC_MAGIC */
    uint32_t regs;           /* user address of the PS2RC_EXC_WORDS block */
    uint32_t report;         /* user function void report(uint32_t *regs), never returns */
    uint32_t gp;             /* the EBOOT's gp */
    uint32_t perf;           /* build 22: user address of a Ps2rcPerfBlock (8-aligned), 0 = no hardware counters */
    uint32_t me;             /* user address of a Ps2rcMeBlock (64-aligned), 0 = no ME sleep support */
} Ps2rcExcArgs;

/* ---- Media Engine sleep support (psp_exc/exc_me.c).
 * Once the EBOOT has taken the Media Engine (src/hle/psp/hle_me.c: own reset stub at 0xBFC00000), the OS's ME driver
 * ("SceMeRpc" sysevent handler of sceMeCodecWrapper) would wait forever at suspend for its own ME program (uofw
 * me_wrapper.c: event 0x4005 posts "save + halt" and spins on 0xBFC00700). On request (hookfn, before the first
 * takeover) this module registers its own sysevent handler and replaces SceMeRpc's handler by a no-op for the rest of
 * the session (both undone in module_stop - the exit sequence's "exit 5/6"): at SUSPEND_PHASE0_5 (0x4005, where the OS
 * saves its ME) it asks the EBOOT's ME loop to stop (exit_req -> the ME writes its cache back, state 2, HALT) and
 * holds the ME in reset; at RESUME_PHASE0_5 (0x10005, where the OS restarts its ME) it re-copies the stub, marks a
 * COLD start (the ME's D-cache came back undefined: the stub then initialises both caches with Index Store Tag like the
 * OS's ME boot code, uofw me_wrapper/init.S, instead of writing them back) and releases the reset; the ME loop goes on
 * from its control block (commands, rings and stretcher are in main RAM, which sleep keeps). */
#define PS2RC_ME_MAGIC 0x5A4D5331u                  /* "ZMS1": the module took the block */
#define PS2RC_ME_EV_SUSPEND 0x00004005u             /* SCE_SYSTEM_SUSPEND_EVENT_PHASE0_5 */
#define PS2RC_ME_EV_RESUME 0x00010005u              /* SCE_SYSTEM_RESUME_EVENT_PHASE0_5 */
#define PS2RC_ME_BOOT_COLD 12                       /* word of the stub's boot block: 1 = cold start (Index Store Tag) */

typedef struct {
    uint32_t magic;                   /* kernel: PS2RC_ME_MAGIC once taken */
    uint32_t version;                 /* kernel: 1 */
    uint32_t hookfn;                  /* kernel: int ps2rc_me_hook(Ps2rcMeBlock *b, int on) for kuKernelCall */
    uint32_t pad0;
    /* user -> kernel (read by the kernel at the events) */
    volatile uint32_t ctl;            /* uncached address of the ME control block (HleMeHdr: [2] state, [3] exit_req) */
    volatile uint32_t live;           /* 1 while the EBOOT has the ME running (hle_me_start .. hle_me_stop / park) */
    uint32_t stub;                    /* the reset stub (EBOOT address) */
    uint32_t stub_words;
    uint32_t bootblk;                 /* the stub's boot block (EBOOT address; word PS2RC_ME_BOOT_COLD) */
    uint32_t exit_seq;                /* the run's sequence number, written into the exit marker (on = 2) */
    uint32_t pad1[6];
    /* kernel -> user */
    volatile int32_t hook_rc;         /* hook install: 0 ok, < 0 = which step failed (-1 finder, -2 functions not found,
                                         -3 register refused, -4 wrong block) */
    volatile uint32_t installed;      /* 1 while our sysevent handler is registered */
    volatile uint32_t mrpc_found;     /* 1 = the OS ME driver's "SceMeRpc" handler was found and replaced by a no-op */
    volatile uint32_t mrpc_orig;      /* its original handler (given back in module_stop) */
    volatile int32_t reg_rc;          /* sceKernelRegisterSysEventHandler result */
    volatile uint32_t clk_fn;         /* sceSysregMeBusClockEnable (called before a restart) or 0 = not found */
    volatile uint32_t events, last_event;
    volatile uint32_t suspends, resumes;   /* PHASE0_5 events handled (resumes = the EBOOT's wait epoch) */
    volatile uint32_t park_ack;       /* 1 = the ME acknowledged the last park (cache written back, halted) */
    volatile uint32_t park_us;        /* how long the last park took */
    volatile uint32_t restarts;       /* restarts done at resume */
    volatile uint32_t reset_reg;      /* 0xBC10004C as found at the last resume */
    volatile uint32_t ev_n;           /* events recorded in ev_ring */
    volatile uint32_t ev_ring[16];    /* the last 16 event ids (index ev_n % 16) */
    volatile uint32_t busclk_suspend; /* 0xBC100050 (bus clock enables) after the park */
    volatile uint32_t busclk_resume;  /* ... at the resume, before sceSysregMeBusClockEnable */
    volatile uint32_t busclk_after;   /* ... after it (= busclk_resume when the function was not found) */
    volatile uint32_t exitmode_rc;    /* exit-mode tag (high half) | what hookfn(block, 2) did (version >= 2 modules) */
} Ps2rcMeBlock;

/* ---- exit mode + the exit marker (module version 2).
 * hookfn(block, 2) = "exit mode", the EBOOT's replacement for stop + unload before sceKernelExitGame (poc6.cfg az_exit=1):
 * the default exception handler is given back and the counter thread told to quit (as module_stop), but the ME
 * sysevent handlers stay as they are - ours registered, the OS ME driver's "SceMeRpc" record still pointing at our no-op
 * - and the module is NOT unloaded, so nothing the kernel can still call (the sysevent list, SceMeRpc's handler word)
 * points into freed memory or at the OS ME driver's handler, which would wait forever for its own ME program (gone since
 * our takeover; uofw me_wrapper). From then on both handlers only RECORD what reaches them into the exit marker: 16 words
 * in the Media Engine's reset-vector RAM (KSEG1, written only while the ME is held in reset - the EBOOT stopped it at
 * exit 1/6), which a soft reboot does not clear unless the OS's ME boot rewrites it. The next PS2RC run reads it back
 * at boot (poc6.cfg az_marker=1): "the previous exit dispatched N sysevents to the ME handlers". A power cut erases it. */
#define PS2RC_AZ_MARK_ADDR 0xBFC00400u              /* after our ME stub (<= 128 words), before the 0x600 mailboxes */
#define PS2RC_AZ_MARK_MAGIC 0x415A5831u             /* "AZX1" */
#define PS2RC_AZ_MARK_WORDS 16
/* words: 0 magic, 1 run sequence, 2 state (bit0 armed, bit1 our handler registered, bit2 SceMeRpc swapped to our
   no-op), 3 calls of our handler, 4 calls of the no-op (SceMeRpc), 5 last id (ours), 6 last id (no-op), 7-14 the first
   8 ids in call order (bit 31 = the no-op), 15 check = xor of words 0-14 ^ PS2RC_AZ_MARK_XOR */
#define PS2RC_AZ_MARK_XOR 0xA5A5A5A5u

/* ---- hardware counters (see the header comment) */
#define PS2RC_PERF_BASE 0xBC400000u
#define PS2RC_PERF_N 20                             /* words 0x00-0x4C, hardware-verified; 0x50 is never read */
#define PS2RC_PERF_MAGIC 0x50524631u                /* "PRF1": the module took the block and started its thread */
/* step breadcrumbs (written BEFORE each register access): */
#define PS2RC_PERF_STEP_STARTED 0x0F0u              /* thread running, no register touched yet */
#define PS2RC_PERF_STEP_READ_EN 0x0F1u              /* reading word 0 (enable) */
#define PS2RC_PERF_STEP_WRITE_EN 0x0F2u             /* writing PROFEN = 1 */
#define PS2RC_PERF_STEP_ARM 0x100u                  /* + i: arming snapshot, reading word i */
#define PS2RC_PERF_STEP_ARMED 0x120u                /* armed: counting, first snapshot taken */
#define PS2RC_PERF_STEP_WINDOW 0x200u               /* + i: window, reading word i */
#define PS2RC_PERF_STEP_DONE 0x300u                 /* last window complete */
#define PS2RC_PERF_STEP_QUIT 0x400u                 /* quit acknowledged: PROFEN restored, thread deleted */
#define PS2RC_PERF_STEP_RESTORE 0x410u              /* + i (1..19): at the quit, clearing word i */
/* exit_done (the counter registers put back before the EBOOT exits; 0 = a module older than that) */
#define PS2RC_PERF_EXIT_RESTORED 1u                 /* PROFEN 0 and words 1..19 cleared by the thread at its quit */
#define PS2RC_PERF_EXIT_FOUND_ON 2u                 /* PROFEN was already 1 before us: nothing written (not ours) */
#define PS2RC_PERF_EXIT_STOP_ONLY 3u                /* the thread did not quit: module_stop wrote PROFEN 0 only */

typedef struct {
    uint32_t magic;                   /* kernel: PS2RC_PERF_MAGIC once taken */
    uint32_t version;                 /* kernel: 1 */
    uint32_t period_us;               /* user: window period (0 or out of 1000..1000000 -> 50000) */
    volatile uint32_t quit;           /* user: 1 = stop counting and end the thread (before the EBOOT exits) */
    volatile uint32_t step;           /* kernel: breadcrumb PS2RC_PERF_STEP_* */
    volatile uint32_t seq;            /* kernel: odd while us/tot are being updated */
    volatile uint32_t windows;        /* kernel: windows accumulated */
    uint32_t thread_rc;               /* kernel: sceKernelCreateThread result */
    uint32_t start_rc;                /* kernel: sceKernelStartThread result */
    uint32_t readfn;                  /* kernel: int ps2rc_perf_read(uint32_t *dst, int n) for kuKernelCall */
    uint32_t en_before;               /* kernel: word 0 (PROFEN) found when arming */
    volatile uint32_t exit_done;      /* kernel: PS2RC_PERF_EXIT_*, 0 = not done / older module */
    volatile uint32_t exit_en;        /* kernel: word 0 read back after the restore */
    volatile uint32_t exit_nz;        /* kernel: bit i = word i (1..19) still non-zero after the clear (0 = all 0) */
    uint32_t reserved[2];
    volatile uint64_t us;             /* kernel: sum of the window lengths (sceKernelGetSystemTimeLow, us) */
    volatile uint64_t tot[PS2RC_PERF_N];   /* kernel: sum of the per-window deltas of every word since arming */
} Ps2rcPerfBlock;

#ifndef PS2RC_EXC_KERNEL
/* user side (src/psp_exc/exc_user.c) */
extern uint32_t ps2rc_crash_regs[PS2RC_EXC_WORDS];
extern Ps2rcMeBlock ps2rc_me_block;               /* passed to every module start (Ps2rcExcArgs.me) */
/* loads + starts <prx_path>; returns the sceKernelRegisterDefaultExceptionHandler result (>= 0 = armed) or the
   load/start error; *detail gets a one-line description for the log */
int ps2rc_exc_install(const char *prx_path, void (*report)(uint32_t *regs), char *detail, unsigned detail_size);
/* build 26: stop + unload ps2rc_exc.prx before sceKernelExitGame (module_stop releases the default exception handler;
   block word 43 = the default handler that was active before ours, the one exc_asm.S chains to when not armed) */
void ps2rc_exc_uninstall(void);
void ps2rc_exc_uninstall2(int unload, int *stop_rc, int *unload_rc);   /* exit log */
int ps2rc_exc_query(void);   /* sceKernelQueryModuleInfo of the (last) module, 0x7FFFFFFF = none */
/* exit mode instead of stop + unload (see "exit mode" above); returns the module's answer (block
   exitmode_rc), < 0 = not done: 0x7FFFFFFF no module, -2 module older than version 2, else the kuKernelCall error */
int ps2rc_exc_exitmode(uint32_t seq);
/* the 16 marker words at boot (kernel read through kubridge); 1 = read, 0 = not (emulator / refused) */
int ps2rc_exc_marker_read(uint32_t *w16);
/* the same + an optional counter block (NULL = no hardware counters; see Ps2rcPerfBlock) */
int ps2rc_exc_install2(const char *prx_path, void (*report)(uint32_t *regs), Ps2rcPerfBlock *perf, char *detail,
                       unsigned detail_size);
const char *ps2rc_exc_name(uint32_t cause);       /* "AdEL (address error, load or fetch)", ... */
/* formats the register block into `out` as 6 lines ("CRASH ..."); returns the length */
unsigned ps2rc_exc_format(const uint32_t *r, char *out, unsigned size);
#endif

#endif
