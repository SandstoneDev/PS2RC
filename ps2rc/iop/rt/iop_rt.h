/* iop_rt.h - runtime of IOP modules translated to C by the IOP static recompiler (ps2rc/iop), and a high-level
 * emulation (HLE) of the IOP kernel and of the standard IOP libraries those modules import.
 *
 * What it provides
 *   - the IOP RAM image the generated code reads and writes (`IopCpu.mem`, addresses masked with IOP_MEM_MASK);
 *   - module loading (copy of the relocated image, bss cleared, imports resolved by library name + function number,
 *     module_start run in a loader thread);
 *   - IOP threads as coroutines (the embedding supplies the context switch), strict priority scheduling with FIFO
 *     order inside a priority and preemption when a higher-priority thread becomes ready, as the IOP kernel does;
 *   - semaphores, event flags, sleep / wakeup, a virtual microsecond clock (DelayThread, alarms), hardware timers,
 *     interrupt handlers run in an interrupt context (no preemption until they return);
 *   - SIF RPC servers (the EE side calls iop_rt_sif_call);
 *   - sysmem, sysclib, stdio/Kprintf, intrman, loadcore, vblank, ioman (cdrom0:/host0: files of the disc), cdvdman
 *     (search / read on the disc) and libsd (the sound registers, mapped onto an IopSdOps back-end).
 * Library functions are identified by the library name and the export number of the documented IOP module interfaces
 * (the numbering used by open-source PS2 SDKs). Anything not implemented resolves to a stub that logs and returns 0;
 * the embedding can bind its own functions (e.g. a game's own IOP modules) with iop_rt_bind before loading.
 *
 * Determinism: nothing depends on real time. Time moves only when the embedding says so (iop_rt_advance_us, timer
 * expiries, SPU IRQs, vblanks). Single instance per thread of the embedding; not thread-safe.
 * C99 + `may_alias`; no OS calls (memory comes from the embedding's allocator).
 */
#ifndef IOP_RT_H
#define IOP_RT_H

#include <stddef.h>
#include <stdint.h>

#ifndef IOP_MEM_MASK
#define IOP_MEM_MASK 0x1FFFFFu               /* the IOP's 2 MB address space; the backed size is set at create time.
                                                Define it (for every unit) to the backed size - 1 (a power of two) to
                                                keep even wild addresses inside the image; IOP_CHECK aborts on them */
#endif

typedef struct IopRt IopRt;

typedef struct IopCpu {
    uint32_t r[32];
    uint32_t hi, lo;
    uint8_t *mem;                            /* IOP RAM image */
    IopRt *rt;
} IopCpu;

typedef void (*IopFn)(IopCpu *c);

typedef struct IopImportDesc {
    const char *lib;
    uint16_t num;
    uint32_t stub;                           /* address of the import stub in the module */
} IopImportDesc;

/* written by the generator (one per translated module) */
typedef struct IopModule {
    const char *name;
    uint32_t version;
    const char *md5;                         /* of the module file it was generated from */
    uint32_t base, image_size, file_size, entry, gp;
    const uint32_t *image;                   /* file_size bytes (text + data, relocated for base) */
    uint32_t nimports;
    const IopImportDesc *imports;
    IopFn *imp;                              /* nimports slots, filled by iop_rt_load */
    IopFn (*fn_at)(uint32_t addr);           /* translated function at an address (callbacks), 0 if none */
} IopModule;

/* ------------------------------------------------------------------ memory access of the generated code */
#ifdef IOP_CHECK
uint32_t iop_rt_check_addr(uint32_t a, unsigned size);   /* aborts the run outside the backed memory */
#define IOP_A(a, n) iop_rt_check_addr((uint32_t)(a), (n))
#else
#define IOP_A(a, n) ((uint32_t)(a) & IOP_MEM_MASK)
#endif
typedef uint32_t iop_u32a __attribute__((may_alias));
typedef uint16_t iop_u16a __attribute__((may_alias));
#define IOP_LW(M, a) (*(const iop_u32a *)(void *)((M) + IOP_A((a), 4)))
#define IOP_LH(M, a) ((uint32_t)*(const iop_u16a *)(void *)((M) + IOP_A((a), 2)))
#define IOP_LB(M, a) ((uint32_t) * (const uint8_t *)((M) + IOP_A((a), 1)))
#define IOP_SW(M, a, v) (*(iop_u32a *)(void *)((M) + IOP_A((a), 4)) = (uint32_t)(v))
#define IOP_SH(M, a, v) (*(iop_u16a *)(void *)((M) + IOP_A((a), 2)) = (uint16_t)(v))
#define IOP_SB(M, a, v) (*(uint8_t *)((M) + IOP_A((a), 1)) = (uint8_t)(v))

/* R3000 division: by zero LO = -1 (dividend >= 0) / +1, HI = dividend; 0x80000000 / -1 = 0x80000000 rem 0 */
#define IOP_DIV(hi, lo, a, b) do { int32_t a_ = (int32_t)(a), b_ = (int32_t)(b); \
        if (b_ == 0) { (lo) = a_ >= 0 ? 0xFFFFFFFFu : 1u; (hi) = (uint32_t)a_; } \
        else if (a_ == (int32_t)0x80000000 && b_ == -1) { (lo) = 0x80000000u; (hi) = 0; } \
        else { (lo) = (uint32_t)(a_ / b_); (hi) = (uint32_t)(a_ % b_); } } while (0)
#define IOP_DIVU(hi, lo, a, b) do { uint32_t a_ = (a), b_ = (b); \
        if (b_ == 0) { (lo) = 0xFFFFFFFFu; (hi) = a_; } else { (lo) = a_ / b_; (hi) = a_ % b_; } } while (0)

/* unaligned word access, little-endian MIPS */
static inline uint32_t iop_lwl(const uint8_t *M, uint32_t a, uint32_t rt)
{
    uint32_t w = IOP_LW(M, a & ~3u), s = (3u - (a & 3u)) * 8u;
    return s ? (rt & ((1u << s) - 1u)) | (w << s) : w;
}
static inline uint32_t iop_lwr(const uint8_t *M, uint32_t a, uint32_t rt)
{
    uint32_t w = IOP_LW(M, a & ~3u), s = (a & 3u) * 8u;
    return s ? (rt & ~(0xFFFFFFFFu >> s)) | (w >> s) : w;
}
static inline void iop_swl(uint8_t *M, uint32_t a, uint32_t rt)
{
    uint32_t w = IOP_LW(M, a & ~3u), s = (3u - (a & 3u)) * 8u;
    IOP_SW(M, a & ~3u, s ? (w & ~(0xFFFFFFFFu >> s)) | (rt >> s) : rt);
}
static inline void iop_swr(uint8_t *M, uint32_t a, uint32_t rt)
{
    uint32_t w = IOP_LW(M, a & ~3u), s = (a & 3u) * 8u;
    IOP_SW(M, a & ~3u, s ? (w & ((1u << s) - 1u)) | (rt << s) : rt);
}

void iop_rt_call(IopCpu *c, uint32_t addr);             /* indirect call (jalr) of translated code */
void iop_rt_trap(IopCpu *c, uint32_t pc, uint32_t word); /* syscall / break / COP0: logged, ignored */

/* ------------------------------------------------------------------ embedding interfaces */

/* coroutines: one per IOP thread. create() returns a context that starts in entry() on its own stack (entry never
 * returns); swap(from, to) saves the running context into `from` and resumes `to`; main_ctx() is the storage of the
 * context that calls iop_rt_run (the scheduler). ps2rc/iop/rt/iop_rt_ucontext.c implements it with ucontext. */
typedef struct IopCoroOps {
    void *user;
    void *(*create)(void *user, size_t stack_bytes, void (*entry)(void));
    void (*swap)(void *user, void *from, void *to);
    void *(*main_ctx)(void *user);
} IopCoroOps;

/* the disc (cdvdman, ioman cdrom0:). Paths are ISO paths without the device, e.g. "\\DIR\\FILE.EXT;1" as the module
 * passes them (case and separators as given; the back-end normalises). */
typedef struct IopDiscOps {
    void *user;
    int (*lookup)(void *user, const char *path, uint32_t *lsn, uint32_t *size);   /* 0 = found */
    int (*read)(void *user, uint32_t lsn, uint32_t nsect, void *dst);             /* 0 = ok */
} IopDiscOps;

/* the sound processor under libsd: voice = core * 24 + voice number; byte addresses in SPU RAM */
enum { IOP_SD_VOLL = 0, IOP_SD_VOLR = 1, IOP_SD_PITCH = 2, IOP_SD_ADSR1 = 3, IOP_SD_ADSR2 = 4 };
enum { IOP_SD_SSA = 0, IOP_SD_LSAX = 1 };
typedef struct IopSdOps {
    void *user;
    uint8_t *ram;                            /* SPU RAM image */
    uint32_t ram_size;
    void (*set_param)(void *user, unsigned voice, unsigned param, uint16_t value);
    void (*set_addr)(void *user, unsigned voice, unsigned which, uint32_t addr);
    uint16_t (*get_envx)(void *user, unsigned voice);
    uint32_t (*get_nax)(void *user, unsigned voice);
    void (*key_on)(void *user, unsigned core, uint32_t mask24);
    void (*key_off)(void *user, unsigned core, uint32_t mask24);
    void (*set_irq)(void *user, uint32_t addr, int enable);
    void (*dma_begin)(void *user, uint32_t addr, uint32_t size);   /* optional: SPU RAM is about to be written */
    void (*dma_done)(void *user, uint32_t addr, uint32_t size);    /* optional: SPU RAM was written */
} IopSdOps;

typedef struct IopRtConfig {
    uint32_t mem_size;                       /* backed IOP RAM (<= IOP_MEM_MASK + 1); 0 = 2 MB */
    size_t host_stack;                       /* native stack per IOP thread; 0 = 64 KB */
    IopCoroOps coro;
    IopDiscOps disc;
    IopSdOps sd;
    int sd_dma_round64;                      /* 1 = SPU DMA writes whole 64-byte blocks (hardware); 0 = exact size */
    /* level 0 errors, 1 module output (Kprintf/printf), 2 runtime progress, 3 every call (debug) */
    void (*log)(void *user, int level, const char *fmt, ...);
    void *log_user;
    void *(*alloc)(void *user, size_t n);    /* runtime memory (zeroed); 0 = calloc */
    void (*free)(void *user, void *p);
    void *alloc_user;
} IopRtConfig;

IopRt *iop_rt_create(const IopRtConfig *cfg);
void iop_rt_destroy(IopRt *rt);

/* bind an import (before iop_rt_load): the module's calls of lib/num go to fn (overrides the built-in HLE) */
int iop_rt_bind(IopRt *rt, const char *lib, unsigned num, IopFn fn);
/* the built-in HLE of lib/num (0 if none; *name = its interface name) - e.g. to wrap it in a binding */
IopFn iop_rt_builtin(const char *lib, unsigned num, const char **name);

/* load a translated module and run its start routine (argv[0] = name) in a loader thread at priority `prio`;
 * returns the start routine's result (v0) or -1 */
int iop_rt_load(IopRt *rt, const IopModule *m, int argc, const char *const *argv, int prio);

/* run the IOP threads until every one of them waits */
void iop_rt_run(IopRt *rt);

/* EE -> IOP SIF RPC: send `ssize` bytes to server `sid` function `fno`, receive `rsize` bytes of the reply. Runs the
 * IOP until the server thread has answered and every thread waits. 0 = answered, -1 = no such server / not answered */
int iop_rt_sif_call(IopRt *rt, uint32_t sid, uint32_t fno, const void *send, uint32_t ssize, void *recv,
                    uint32_t rsize);

/* events from the outside (interrupt context; call iop_rt_run afterwards to let woken threads run) */
void iop_rt_spu_irq(IopRt *rt, unsigned core_bits);     /* SPU2 IRQ -> libsd's SPU2 interrupt handler */
int iop_rt_timer_count(IopRt *rt);                      /* hardware timers allocated so far */
int iop_rt_timer_active(IopRt *rt, int k);              /* started and has a handler */
uint64_t iop_rt_timer_period_ns(IopRt *rt, int k);      /* compare value as time (0 = unknown source) */
void iop_rt_timer_expire(IopRt *rt, int k);             /* timer k reached its compare value: run its handler */
void iop_rt_vblank(IopRt *rt);                          /* a vertical blank: releases WaitVblank* waiters */
void iop_rt_advance_us(IopRt *rt, uint64_t us);         /* virtual time: DelayThread wake-ups and alarms */

/* access for embeddings and bindings */
int iop_rt_disc_read(IopRt *rt, uint32_t lsn, uint32_t nsect, void *dst);   /* 0 = ok (IopDiscOps.read) */
uint8_t *iop_rt_mem(IopRt *rt);
uint32_t iop_rt_mem_size(IopRt *rt);
const IopModule *iop_rt_module(IopRt *rt, const char *name);
uint32_t iop_arg(IopCpu *c, int k);                     /* argument k of the current call (o32: a0-a3, then stack) */
void iop_ret(IopCpu *c, uint32_t v);                    /* v0 */
void *iop_ptr(IopCpu *c, uint32_t addr);                /* host pointer of an IOP address (0 for NULL) */
void iop_rt_log(IopRt *rt, int level, const char *fmt, ...);

typedef struct IopRtStats {
    uint32_t switches, threads, semas, evflags, sif_calls, imports_unresolved, unimpl_calls, intr_calls;
    uint32_t mem_used, mem_peak;             /* sysmem heap bytes */
    uint32_t sd_dma_bytes, sd_dmas;
} IopRtStats;
const IopRtStats *iop_rt_stats(IopRt *rt);

#endif /* IOP_RT_H */
