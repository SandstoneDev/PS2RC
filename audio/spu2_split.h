/* spu2_split.h - the SPU2-lite engine split across two cores.
 *
 * The IOP sound-driver model must see exactly the host's voice state (ENVX, NAX, ENDX, the IRQ): the game reads it back
 * in every driver reply. That state does not depend on the samples, so the main core keeps the model and an engine "A"
 * in state-only mode (Spu2.x_state_only = 1: no ADPCM decode, no mix - ~40 % of the full engine's work was measured),
 * and every input of A is recorded as a command for a second engine "B" that does the expensive part - decode,
 * interpolation, volumes, mix - on the Media Engine. A and B start from the same state and receive the same register
 * writes in the same order between the same render blocks, so B's samples are the samples A would have rendered
 * (bit for bit; checked on recorded gameplay).
 *
 *   main core (game thread)                                      Media Engine
 *   driver RPC -> Spu2Ops (wrapped here)                          spu2_cmd_apply(B, cmd):
 *     set_param/addr/key on/off/irq -> A + command                  register writes -> B
 *     advance(n) -> A renders block by block (state only),          RENDER k -> spu2_render48(B, out, k) -> output
 *                   one RENDER k command per block                  RAMW -> invalidate the ME's cached SPU RAM lines
 *     SPU RAM "DMA": dma_begin -> barrier (B consumed every command so far), the write, dma_done -> RAMW command
 *
 * Block boundaries matter (the mix_limit voice culling is decided per block), so A renders exactly the blocks
 * spu2_advance would render (split at the IOP timer and at SPU2_BLOCK_FRAMES) and B renders the same k's.
 * Register writes that cannot change the engine (same value; A's state = B's state for every field a write touches)
 * are not recorded.
 *
 * The SPU RAM image is shared: B reads it where A's model writes it. B must never decode a block after the model has
 * overwritten it for a later point in time, hence the barrier before every write (the platform waits until B has
 * consumed every command pushed so far) and the RAMW command after it (the platform's cache maintenance).
 *
 * Portable C99, no libc, no OS: the platform supplies the command sink (a ring) and the barrier.
 */
#ifndef SPU2_SPLIT_H
#define SPU2_SPLIT_H

#include <stdint.h>
#include "spu2_lite.h"

enum {
    SPU2C_NOP = 0,
    SPU2C_PARAM = 1,    /* w0: voice << 8 | param << 16      w1: value */
    SPU2C_ADDR = 2,     /* w0: voice << 8 | which << 16      w1: byte address */
    SPU2C_KEYON = 3,    /* w0: core << 8                     w1: mask24 */
    SPU2C_KEYOFF = 4,   /* w0: core << 8                     w1: mask24 */
    SPU2C_IRQ = 5,      /* w0: enable << 8                   w1: byte address */
    SPU2C_RENDER = 6,   /*                                   w1: 48 kHz frames (one engine block, <= 256) */
    SPU2C_RAMW = 7,     /* w0: size << 8 (< 16 MB)           w1: SPU RAM byte address (the model wrote it) */
    SPU2C_SYNC = 8      /* w0: token << 8                    platform marker (the ME echoes the token) */
};

#define SPU2C_OP(w0)   ((w0) & 0xFFu)

typedef struct Spu2Rec Spu2Rec;
struct Spu2Rec {
    Spu2 *a;                                  /* the main core's engine */
    /* mode: 0 = off (A runs as before: full engine + sink, nothing recorded);
     *       1 = split (A state-only, no sink; B renders: commands recorded);
     *       2 = shadow (A full engine + sink as before AND commands recorded - B's output is compared, not played) */
    int mode;
    void (*push)(void *user, uint32_t w0, uint32_t w1);   /* append one command (the platform waits when full) */
    void (*barrier)(void *user);              /* wait until every command pushed so far is consumed */
    void (*ram_written)(void *user, uint32_t addr, uint32_t size);  /* before the RAMW command: e.g. cache writeback */
    void *user;
    uint32_t pend_addr, pend_size;            /* a write the model started (dma_begin) and has not announced yet */
    int pend;
    /* statistics */
    uint64_t cmds, renders, frames, dropped, ramw, ramw_bytes, barriers;
};

/* Fill `ops` (the driver model's operation table, before it starts) with recording wrappers around engine `a`
 * (lockstep: advance is set). timer_cb(timer_user) = the driver's hardware timer (A calls it; it writes through these
 * ops). mode starts at 0. */
void spu2_rec_bind_ops(Spu2Rec *r, Spu2 *a, Spu2Ops *ops, void (*timer_cb)(void *), void *timer_user);

/* switch the mode (between RPCs only) - sets A's x_state_only (1 in split mode) */
void spu2_rec_set_mode(Spu2Rec *r, int mode);

/* B side: apply one command to engine `b` (no timer, no sink, no tap); RENDER k renders k frames into out48 (stereo
 * s16, at least 2 * SPU2_BLOCK_FRAMES values) and returns k; RAMW / SYNC / NOP return 0 (the platform's business).
 * Header-only so that the Media Engine's own copy of the engine (compiled for the ME) can use it unchanged. */
static inline uint32_t spu2_cmd_apply(Spu2 *b, uint32_t w0, uint32_t w1, int16_t *out48)
{
    uint32_t op = SPU2C_OP(w0), x = (w0 >> 8) & 0xFFu, y = w0 >> 16;
    if (op == SPU2C_RENDER) {
        spu2_render48(b, out48, w1);
        return w1;
    }
    if (op == SPU2C_PARAM) spu2_set_param(b, x, y, (uint16_t)w1);
    else if (op == SPU2C_ADDR) spu2_set_addr(b, x, y, w1);
    else if (op == SPU2C_KEYON) spu2_key_on(b, x, w1);
    else if (op == SPU2C_KEYOFF) spu2_key_off(b, x, w1);
    else if (op == SPU2C_IRQ) spu2_set_irq(b, w1, (int)(x & 1u));
    return 0;
}

#endif /* SPU2_SPLIT_H */
