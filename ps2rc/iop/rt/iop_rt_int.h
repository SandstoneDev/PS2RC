/* iop_rt_int.h - internals shared by the IOP runtime's files (not for embeddings). */
#ifndef IOP_RT_INT_H
#define IOP_RT_INT_H

#include <stdarg.h>
#include "iop_rt.h"

#define IOP_MAX_THREADS  32
#define IOP_MAX_SEMAS    512
#define IOP_MAX_EVFLAGS  32
#define IOP_MAX_TIMERS   4
#define IOP_MAX_ALARMS   32
#define IOP_MAX_SERVERS  16
#define IOP_MAX_QUEUES   8
#define IOP_MAX_MODULES  8
#define IOP_MAX_BINDS    64
#define IOP_MAX_BLOCKS   256
#define IOP_MAX_FDS      16
#define IOP_INTR_STACK   0x800u               /* guest stack of one interrupt nesting level */
#define IOP_INTR_LEVELS  4

/* IOP kernel error codes (documented values) */
#define KE_OK              0
#define KE_ERROR          (-1)
#define KE_ILLEGAL_THID   (-406)
#define KE_UNKNOWN_THID   (-407)
#define KE_UNKNOWN_SEMID  (-408)
#define KE_UNKNOWN_EVFID  (-409)
#define KE_DORMANT        (-413)
#define KE_NOT_DORMANT    (-414)
#define KE_SEMA_ZERO      (-419)
#define KE_SEMA_OVF       (-420)
#define KE_EVF_COND       (-421)
#define KE_EVF_MULTI      (-422)
#define KE_EVF_ILPAT      (-423)
#define KE_NO_MEMORY      (-400)

enum { TS_FREE = 0, TS_DORMANT, TS_READY, TS_RUN, TS_WAIT };
enum { W_NONE = 0, W_SEMA, W_EVF, W_SLEEP, W_DELAY, W_RPC, W_VBLANK, W_VBLANK_END };

typedef struct IopThread {
    int state, prio, init_prio;
    uint64_t seq;                            /* ready order inside a priority */
    uint32_t attr, option, entry, stack_size, stack, gp, arg, arg2;
    void *coro;
    int coro_started;
    IopCpu cpu;
    int wait, wait_id;
    uint32_t wait_bits, wait_mode, wait_res;
    uint64_t wait_until;                     /* W_DELAY: virtual us */
    int wakeups;
    int result;                              /* return value of the wait */
    uint32_t rpc_queue;                      /* W_RPC: queue address */
} IopThread;

typedef struct IopSema {
    int used;
    uint32_t attr, option;
    int count, max;
} IopSema;

typedef struct IopEvf {
    int used;
    uint32_t attr, option, bits;
} IopEvf;

typedef struct IopTimer {
    int used, started;
    uint32_t source, size, prescale, mode, compare, handler, common;
    uint32_t gp;
} IopTimer;

typedef struct IopAlarm {
    int used;
    uint64_t due;
    uint32_t handler, common, gp;
} IopAlarm;

typedef struct IopServer {
    int used;
    uint32_t sd, sid, func, buf, cfunc, cbuf, queue;
} IopServer;

typedef struct IopRpcReq {
    int pending, done;
    uint32_t sid, fno, ssize, rsize;
    const void *send;
    void *recv;
} IopRpcReq;

typedef struct IopQueue {
    int used;
    uint32_t addr;
    int thread;                              /* thread index serving it */
    IopRpcReq req;
} IopQueue;

typedef struct IopBlock { uint32_t addr, size; } IopBlock;

typedef struct IopFd { int used; uint32_t lsn, size, pos; } IopFd;

typedef struct IopBind { char lib[9]; unsigned num; IopFn fn; } IopBind;

struct IopRt {
    IopRtConfig cfg;
    uint8_t *mem;
    uint32_t mem_size, heap_lo, heap_hi;
    IopBlock blocks[IOP_MAX_BLOCKS];         /* allocated sysmem blocks, sorted by address */
    int nblocks;
    IopThread th[IOP_MAX_THREADS];
    int cur;                                 /* running thread index, -1 = none (scheduler / embedding) */
    uint64_t seq;
    void *main;
    IopSema sem[IOP_MAX_SEMAS];
    IopEvf evf[IOP_MAX_EVFLAGS];
    IopTimer tim[IOP_MAX_TIMERS];
    IopAlarm alarm[IOP_MAX_ALARMS];
    IopServer srv[IOP_MAX_SERVERS];
    IopQueue q[IOP_MAX_QUEUES];
    IopFd fd[IOP_MAX_FDS];
    const IopModule *mods[IOP_MAX_MODULES];
    int nmods;
    IopBind binds[IOP_MAX_BINDS];
    int nbinds;
    int intr_level;
    uint64_t now_us;
    /* libsd shadow */
    struct {
        uint32_t irqa[2];
        int irq_enable;
        uint32_t trans_func[2], trans_data[2], trans_gp[2];
        uint32_t spu2_func, spu2_data, spu2_gp;
        uint32_t irq_cb, irq_cb_gp;
        uint16_t vparam[2][24][8];
        uint16_t cparam[2][32];
        uint32_t sw[2][32];
        uint32_t addr[2][64];
        uint32_t cattr[2][16];
    } sd;
    IopRtStats st;
    int unimpl_logged;
};

extern IopRt *iop_rt_current;               /* the instance whose thread is starting (coroutine entry) */

void iop_rt_logv(IopRt *rt, int level, const char *fmt, va_list ap);
IopFn iop_rt_fn(IopRt *rt, uint32_t addr);
uint32_t iop_rt_call_handler(IopRt *rt, uint32_t fn, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2);
void iop_rt_preempt_check(IopRt *rt);
void iop_rt_wait(IopRt *rt, int kind);      /* current thread waits (state set here); returns when woken */
void iop_rt_ready(IopRt *rt, int t, int result);
uint32_t iop_rt_alloc(IopRt *rt, uint32_t size, int mode, uint32_t addr);
int iop_rt_free(IopRt *rt, uint32_t addr);
int iop_rt_format(IopCpu *c, uint32_t fmt, int argk, char *out, size_t cap);
void iop_rt_libs_init(IopRt *rt);

static inline IopRt *RT(IopCpu *c) { return c->rt; }
static inline uint8_t *MEMP(IopRt *rt, uint32_t a) { return rt->mem + (a & IOP_MEM_MASK); }
static inline uint32_t rd32(IopRt *rt, uint32_t a)
{
    const uint8_t *p = MEMP(rt, a);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void wr32(IopRt *rt, uint32_t a, uint32_t v)
{
    uint8_t *p = MEMP(rt, a);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#endif
