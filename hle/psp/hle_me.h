/* hle_me.h - the PSP's Media Engine (second MIPS core) for PS2RC.
 *
 * Bring-up = a recipe proven on a PSP-3000 in the author's earlier homebrew project: kubridge kuKernelCall (ARK-4)
 * runs a kernel-mode helper of this EBOOT on the main core that copies a reset stub to the ME reset vector 0xBFC00000
 * and pulses the ME reset bit (0xBC10004C bit 2). The stub (assembly, no stack) unlocks 64 MB, enables the bus clocks,
 * sets CP0 Status (FPU on, interrupts off), invalidates the I-cache, DISCARDS the ME's cached lines of its private
 * region (a previous run held in reset may have left dirty lines there - they must not be written back over memory
 * the main core has re-initialised since), writes back + invalidates the rest of its D-cache, then loads $sp, $gp and
 * jumps to the work function at its cached-kernel alias (0x80000000 | fn) with the uncached control block in $a0.
 *
 * ME code rules: no syscalls, no libc, no function pointers / jump tables (stay in kseg0; the one exception is the
 * job dispatch of hle_me_audio.c, which calls 0x80000000 | fn - hle_me_jobs.h), shared words uncached
 * (0x40000000 | addr), private memory only inside the region given to hle_me_start. Everything about the ME is
 * device-only: PPSSPP does not emulate it (hle_me_start refuses in the emulator).
 */
#ifndef HLE_ME_H
#define HLE_ME_H

#include <stdint.h>

#define HLE_ME_MAGIC 0x5A4D4531u      /* "ZME1" */

/* the generic head of every control block (uncached, 64-byte aligned, first in the block) */
typedef struct HleMeHdr {
    volatile uint32_t magic;          /* main: HLE_ME_MAGIC */
    volatile uint32_t heartbeat;      /* ME: +1 per loop */
    volatile uint32_t state;          /* ME: 1 running, 2 left the loop (cache written back, halting) */
    volatile uint32_t exit_req;       /* main: 1 = leave the loop */
    volatile uint32_t count;          /* ME: CP0 Count at the last loop (clock calibration) */
    volatile uint32_t boots;          /* ME: +1 at each start */
    volatile uint32_t fault;          /* ME: != 0 = the worker refused something (worker-defined code) */
    volatile uint32_t fault_arg;
    volatile uint32_t pad[8];         /* 64 bytes */
} HleMeHdr;

/* Start `work(ctl)` on the ME. ctl = UNCACHED pointer to a control block starting with HleMeHdr (main zeroes it except
 * magic before the call); region/region_bytes = the ME-private memory (cached by the ME; contains its stack at the
 * top: stack_bytes); work must be the plain function address (this file adds the kseg0 alias).
 * Returns 1 when the heartbeat ticks within ~100 ms, 0 otherwise (emulator, kubridge missing, ME dead) - `why` says
 * which (for the log). The ME is held in reset again after a failed start. */
int hle_me_start(void (*work)(volatile HleMeHdr *ctl), volatile HleMeHdr *ctl, void *region, uint32_t region_bytes,
                 uint32_t stack_bytes, char *why, int why_n);
/* exit request, wait (<= timeout_ms) for state 2, then hold the ME in reset. Returns 1 = the ME acknowledged. */
int hle_me_stop(int timeout_ms);
/* hold the ME in reset now (power callback, a stalled ME): no handshake. Returns the kuKernelCall result. */
int hle_me_park(void);
int hle_me_running(void);             /* started and neither stopped nor parked */
volatile HleMeHdr *hle_me_owner(void); /* round 6: the running loop's control block (0 = none) */
uint32_t hle_me_reset_reg_seen(void); /* 0xBC10004C as read before the first start (diagnostics) */
/* the stub's writes to 0xBC100040 / 0xBC100050: 0 (default) = 2 / 6 (proven on a PSP-3000),
 * 1 = OR into the values found; and the values the stub found at the last start (0xFFFFFFFF = not yet) */
void hle_me_set_regs(int or_mode);
void hle_me_regs_found(uint32_t *r40, uint32_t *r50);
/* 1 = the ME took an exception since the last start (its handler recorded Cause / EPC / BadVAddr and halted) */
int hle_me_exception(uint32_t *cause, uint32_t *epc, uint32_t *badvaddr);

/* Sleep / power switch. Once the reset vector is ours, the OS's ME program is gone for the session - and the OS's ME
 * driver (sceMeCodecWrapper, loaded for a movie player's sceMpeg and CANT_STOP) has a suspend handler that posts a
 * "save and halt" request to that program and spins with NO timeout until it answers (uofw src/kd/me_wrapper/
 * me_wrapper.c, sysevent 0x4005: `while (*0xbfc00700 != -2);`): a suspend would hang the kernel - a power-off seen
 * on hardware. Parking the ME does not answer that request. So hle_me_start takes scePowerLock(0) before
 * the FIRST takeover and never releases it in this session (sleep requests are held off; the kernel reboot of
 * sceKernelExitGame clears the lock). The power callback (own thread, priority 0x11) still parks the ME on a real
 * SUSPENDING / STANDBY (a power-down that gets through anyway) and on POWER_SWITCH when the lock was refused, and
 * counts the events; the user polls hle_me_power_events(). Returns the scePowerRegisterCallback result. Idempotent. */
int hle_me_power_init(void);
typedef struct HleMePower { uint32_t parks, resumes, switches, last_flags; } HleMePower;
void hle_me_power_events(HleMePower *out);
/* scePowerLock's result of the first takeover: 1 = no takeover yet (sleep is safe), 0 = sleep held off, < 0 = the
   lock was refused (a suspend would hang: see above) */
int hle_me_power_lock_rc(void);
int hle_me_power_shutdown(int *lock_held);   /* exit - power callback out (the scePowerLock stays) */

/* Sleep WITH the ME taken over. hle_me_sleep_enable (before the first
 * hle_me_start) asks ps2rc_exc.prx (src/psp_exc/exc_me.c, through its block ps2rc_me_block and kuKernelCall) to
 * register its own sysevent handler and to replace the OS ME driver's for the session: at SUSPEND_PHASE0_5 it stops our
 * ME loop cleanly (cache written back, halted) and holds the ME in reset, at RESUME_PHASE0_5 it re-copies the stub and
 * restarts the ME cold (Index Store Tag cache init); the loop goes on from its control block. Then hle_me_start takes no
 * scePowerLock and the power callback parks nothing. Returns 1 when the hook is installed; `why` says what happened.
 * Undone by the module's stop in the exit sequence. */
int hle_me_sleep_enable(char *why, int why_n);
int hle_me_sleep_hooked(void);
volatile uint32_t *hle_me_resume_epoch(void);   /* +1 per resume handled by the PRX: waits restart their deadlines */
void hle_me_sleep_info(char *out, int n);       /* one line of the PRX's sleep counters */
void hle_me_set_cold(int on);                   /* next starts: 1 = cold cache init (Index Store Tag) - heartbeat test */

/* ME-side helpers (call only on the ME, from the work function) */
static inline uint32_t hle_me_count(void)
{
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
static inline void hle_me_sync(void) { __asm__ volatile("sync" ::: "memory"); }
/* D-cache maintenance by address on the CURRENT core (64-byte lines): 0x19 hit invalidate, 0x1A hit writeback */
static inline void hle_me_dinval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63u, e = ((uintptr_t)p + n + 63u) & ~63u;
    for (; a < e; a += 64) __asm__ volatile("cache 0x19, 0(%0)" : : "r"(a) : "memory");
}
static inline void hle_me_dwb(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63u, e = ((uintptr_t)p + n + 63u) & ~63u;
    for (; a < e; a += 64) __asm__ volatile("cache 0x1A, 0(%0)" : : "r"(a) : "memory");
    __asm__ volatile("sync" ::: "memory");
}
/* whole D-cache: index writeback-invalidate (16 KB 2-way: 8 KB of indexes, twice per index) */
static inline void hle_me_dwbinv_all(void)
{
    __asm__ volatile("sync" ::: "memory");
    for (uint32_t i = 0; i < 8192u; i += 64u) {
        __asm__ volatile("cache 0x14, 0(%0)" : : "r"(i) : "memory");
        __asm__ volatile("cache 0x14, 0(%0)" : : "r"(i) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}
static inline void hle_me_halt(void) { for (;;) __asm__ volatile(".word 0x70000000"); }   /* Allegrex HALT */

#endif
