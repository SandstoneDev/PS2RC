/* spu2_split.c - the main core's half of the two-core engine split: see spu2_split.h. */
#include "spu2_split.h"

#define RAM_MASK (SPU2_RAM_SIZE - 1u)

static void push(Spu2Rec *r, uint32_t w0, uint32_t w1)
{
    r->cmds++;
    r->push(r->user, w0, w1);
}

/* the write cannot change engine state: A's fields equal B's for everything a register write touches */
static int param_noop(const Spu2Voice *v, unsigned p, uint16_t val)
{
    switch (p) {
    case 0: return v->vl.reg == val && ((val & 0x8000u) || v->vl.value == spu2_vol_fixed(val));
    case 1: return v->vr.reg == val && ((val & 0x8000u) || v->vr.value == spu2_vol_fixed(val));
    case 2: return v->pitch == val;
    case 3: return v->adsr1 == val;
    case 4: return v->adsr2 == val;
    default: return 1;                                         /* spu2_set_param ignores it */
    }
}

static void r_set_param(void *u, unsigned vi, unsigned p, uint16_t val)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (r->mode && vi < SPU2_VOICES) {
        if (param_noop(&r->a->v[vi], p, val)) r->dropped++;
        else push(r, SPU2C_PARAM | vi << 8 | p << 16, val);
    }
    spu2_set_param(r->a, vi, p, val);
}

static void r_set_addr(void *u, unsigned vi, unsigned which, uint32_t addr)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (r->mode && vi < SPU2_VOICES) {
        const Spu2Voice *v = &r->a->v[vi];
        uint32_t m = addr & RAM_MASK;
        int noop = which == 0 ? v->ssa == m : (v->lsa == m && v->lsa_written);
        if (noop) r->dropped++;
        else push(r, SPU2C_ADDR | vi << 8 | (which ? 1u : 0u) << 16, addr);
    }
    spu2_set_addr(r->a, vi, which, addr);
}

static void r_key_on(void *u, unsigned core, uint32_t mask)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (r->mode && core < 2 && (mask & 0xFFFFFFu)) push(r, SPU2C_KEYON | core << 8, mask & 0xFFFFFFu);
    spu2_key_on(r->a, core, mask);
}

static void r_key_off(void *u, unsigned core, uint32_t mask)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (r->mode && core < 2) {
        int any = 0;                                           /* only sounding voices react */
        for (unsigned n = 0; n < 24; n++)
            if ((mask >> n & 1u) && r->a->v[core * 24u + n].phase < SPU2_ENV_STOPPED) any = 1;
        if (any) push(r, SPU2C_KEYOFF | core << 8, mask & 0xFFFFFFu);
        else if (mask & 0xFFFFFFu) r->dropped++;
    }
    spu2_key_off(r->a, core, mask);
}

static void r_set_irq(void *u, uint32_t addr, int enable)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (r->mode) {
        uint32_t en = enable ? 1u : 0u;
        if (r->a->irqa == (addr & RAM_MASK) && r->a->irq_enable == en) r->dropped++;
        else push(r, SPU2C_IRQ | en << 8, addr);
    }
    spu2_set_irq(r->a, addr, enable);
}

static uint16_t r_get_envx(void *u, unsigned vi) { return spu2_get_envx(((Spu2Rec *)u)->a, vi); }
static uint32_t r_get_nax(void *u, unsigned vi) { return spu2_get_nax(((Spu2Rec *)u)->a, vi); }
static int r_irq_take(void *u) { return spu2_irq_take(((Spu2Rec *)u)->a); }

/* SPU RAM writes: barrier before (B has consumed every command, so it decodes nothing at the old bytes any more),
 * RAMW after (the platform's cache maintenance). A write the model starts but does not finish (a failed read) is
 * announced at the next advance - the bytes in the image may have changed either way. */
static void flush_write(Spu2Rec *r, uint32_t addr, uint32_t size)
{
    if (r->ram_written) r->ram_written(r->user, addr, size);
    r->ramw++;
    r->ramw_bytes += size;
    push(r, SPU2C_RAMW | (size & 0xFFFFFFu) << 8, addr);
}

static void r_dma_begin(void *u, uint32_t addr, uint32_t size)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (!r->mode) return;
    if (r->pend) { flush_write(r, r->pend_addr, r->pend_size); r->pend = 0; }
    r->barriers++;
    if (r->barrier) r->barrier(r->user);
    r->pend_addr = addr;
    r->pend_size = size;
    r->pend = 1;
}

static void r_dma_done(void *u, uint32_t addr, uint32_t size)
{
    Spu2Rec *r = (Spu2Rec *)u;
    if (!r->mode) return;
    if (r->pend && (r->pend_addr != addr || r->pend_size != size)) flush_write(r, r->pend_addr, r->pend_size);
    r->pend = 0;
    flush_write(r, addr, size);
}

static void r_advance(void *u, uint32_t n)
{
    Spu2Rec *r = (Spu2Rec *)u;
    Spu2 *a = r->a;
    if (!r->mode) { spu2_advance(a, n); return; }
    if (r->pend) { flush_write(r, r->pend_addr, r->pend_size); r->pend = 0; }
    while (n) {
        /* the block spu2_advance would render next: up to the IOP timer (which fires first if it is due), at most
           SPU2_BLOCK_FRAMES - so spu2_render48 / spu2_advance below render exactly one engine block of k frames */
        uint32_t k = n < SPU2_BLOCK_FRAMES ? n : SPU2_BLOCK_FRAMES;
        if (a->timer_cb && a->timer_period) {
            uint32_t left = a->timer_count >= a->timer_period ? a->timer_period : a->timer_period - a->timer_count;
            if (k > left) k = left;
        }
        if (r->mode == 1) spu2_render48(a, 0, k);             /* state only: the timer's writes are recorded inside */
        else spu2_advance(a, k);                              /* shadow: the full engine + the sink, as before */
        r->renders++;
        r->frames += k;
        push(r, SPU2C_RENDER, k);
        n -= k;
    }
}

void spu2_rec_bind_ops(Spu2Rec *r, Spu2 *a, Spu2Ops *ops, void (*timer_cb)(void *), void *timer_user)
{
    spu2_bind_ops(a, ops, timer_cb, timer_user, 1);            /* timer, ram, lockstep - then the wrappers */
    r->a = a;
    r->mode = 0;
    ops->user = r;
    ops->set_param = r_set_param;
    ops->set_addr = r_set_addr;
    ops->get_envx = r_get_envx;
    ops->get_nax = r_get_nax;
    ops->key_on = r_key_on;
    ops->key_off = r_key_off;
    ops->set_irq = r_set_irq;
    ops->irq_take = r_irq_take;
    ops->dma_done = r_dma_done;
    ops->dma_begin = r_dma_begin;
    ops->advance = r_advance;
    r->pend = 0;
}

void spu2_rec_set_mode(Spu2Rec *r, int mode)
{
    if (r->pend && r->mode) { flush_write(r, r->pend_addr, r->pend_size); }
    r->pend = 0;
    r->mode = mode;
    r->a->x_state_only = mode == 1;
}
