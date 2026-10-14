/* spu2_lite.c - see spu2_lite.h. Behaviour follows the documented SPU hardware (psx-spx "SPU ADPCM", "SPU Envelope",
 * "SPU Voice Flags") as an IOP sound driver uses it; PCSX2 (pcsx2/SPU2, GPL) was read as a behavioural reference only -
 * no code is taken from it. */
#include "spu2_lite.h"

#define RAM_MASK (SPU2_RAM_SIZE - 1u)
#define PARK 0x5000u

#define FLAG_LOOP_END 1u
#define FLAG_REPEAT 2u
#define FLAG_LOOP_START 4u

static int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static int32_t clamp16(int32_t x) { return clampi(x, -32768, 32767); }

/* ------------------------------------------------------------------ PS-ADPCM
 * One 16-byte block -> 28 samples, as psx-spx "SPU ADPCM" documents the format: byte 0 = shift (low nibble; 13-15
 * act as 9) and filter (high nibble; 0-4 - other values are taken as 0 here), byte 1 = the loop flags, bytes 2-15 =
 * 28 4-bit samples, the low nibble first. A sample = (nibble << 12, sign-extended) >> shift, plus the filter's
 * prediction from the two previous outputs ((old * f0 + older * f1 + 32) >> 6), clamped to 16 bits. */
static const int16_t ADPCM_FILTER[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

void spu2_adpcm_block(const uint8_t *src, int16_t *dst, int32_t *h1p, int32_t *h2p)
{
    unsigned shift = src[0] & 15u, filter = src[0] >> 4;
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 0;
    const int32_t f0 = ADPCM_FILTER[filter][0], f1 = ADPCM_FILTER[filter][1];
    int32_t old = *h1p, older = *h2p;
    for (int n = 0; n < 28; n++) {
        unsigned nib = (src[2 + (n >> 1)] >> ((n & 1) * 4)) & 15u;
        int32_t s = (int32_t)(int16_t)(uint16_t)(nib << 12) >> shift;
        s = clamp16(s + ((old * f0 + older * f1 + 32) >> 6));
        dst[n] = (int16_t)s;
        older = old;
        old = s;
    }
    *h1p = old;
    *h2p = older;
}

/* ------------------------------------------------------------------ envelope (ADSR) */

typedef struct { int shift, step, exp, decr, target; } EnvParams;

static void env_params(const Spu2Voice *v, EnvParams *p)
{
    uint16_t a1 = v->adsr1, a2 = v->adsr2;
    switch (v->phase) {
    case SPU2_ENV_ATTACK:
        p->exp = a1 >> 15;
        p->shift = (a1 >> 10) & 0x1F;
        p->step = 7 - ((a1 >> 8) & 3);
        p->decr = 0;
        p->target = 0x7FFF;
        break;
    case SPU2_ENV_DECAY:
        p->exp = 1;
        p->decr = 1;
        p->shift = (a1 >> 4) & 0xF;
        p->step = -8;
        p->target = ((a1 & 0xF) + 1) << 11;
        break;
    case SPU2_ENV_SUSTAIN:
        p->exp = a2 >> 15;
        p->decr = (a2 >> 14) & 1;
        p->shift = (a2 >> 8) & 0x1F;
        p->step = 7 - ((a2 >> 6) & 3);
        if (p->decr) p->step = ~p->step;
        p->target = 0;
        break;
    default:                                                  /* release */
        p->exp = (a2 >> 5) & 1;
        p->decr = 1;
        p->shift = a2 & 0x1F;
        p->step = -8;
        p->target = 0;
        break;
    }
}

static void voice_stop(Spu2Voice *v)
{
    v->phase = SPU2_ENV_STOPPED;
    v->env = 0;
}

void spu2_env_tick(Spu2Voice *v)
{
    if (v->phase >= SPU2_ENV_STOPPED) { v->env = 0; return; }
    EnvParams p;
    env_params(v, &p);
    uint32_t cinc = 0x8000u >> (p.shift > 11 ? p.shift - 11 : 0);
    int32_t linc = p.step * (1 << (p.shift < 11 ? 11 - p.shift : 0));
    if (p.exp) {
        if (!p.decr && v->env > 0x6000) cinc >>= 2;
        if (p.decr) linc = (int16_t)((linc * v->env) >> 15);
    }
    if (cinc == 0) cinc = 1;
    v->env_counter += cinc;
    if (v->env_counter >= 0x8000u) {
        v->env_counter = 0;
        v->env = clampi(v->env + linc, 0, 0x7FFF);
    }
    if (v->phase == SPU2_ENV_SUSTAIN) {                       /* sustain lasts until key off or silence */
        if (v->env == 0) voice_stop(v);
        return;
    }
    if ((!p.decr && v->env >= p.target) || (p.decr && v->env <= p.target)) v->phase++;
    if (v->phase >= SPU2_ENV_STOPPED) voice_stop(v);
}

/* ------------------------------------------------------------------ volume: fixed level or sweep */

int32_t spu2_vol_fixed(uint16_t reg) { return (int16_t)(uint16_t)(reg << 1); }

void spu2_vol_tick(Spu2Vol *vol)
{
    uint16_t r = vol->reg;
    if (!(r & 0x8000)) return;
    int exp = (r >> 14) & 1, decr = (r >> 13) & 1, ph = (r >> 12) & 1, shift = (r >> 2) & 0x1F, stepf = r & 3;
    int32_t step = 7 - stepf;
    if (decr) step = ~step;
    uint32_t cinc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    int32_t linc = step * (1 << (shift < 11 ? 11 - shift : 0));
    if (exp) {
        if (!decr && vol->value > 0x6000) cinc >>= 2;
        if (decr) linc = (int16_t)((linc * vol->value) >> 15);
    }
    if (!(shift == 0x1F && stepf == 3) && cinc == 0) cinc = 1;  /* rate 0x7F = no change */
    vol->counter += cinc;
    if (!(exp && decr)) linc = ph ? -linc : linc;
    if (vol->counter >= 0x8000u) {
        vol->counter = 0;
        if (!decr) {
            vol->value = clamp16(vol->value + linc);
        } else {
            int32_t lo = ph ? -32768 : 0, hi = ph ? 0 : 32767;
            if (exp) { lo = 0; hi = 32767; }
            vol->value = clampi(vol->value + linc, lo, hi);
        }
    }
}

/* ------------------------------------------------------------------ sample stream of a voice */

static void enter_block(Spu2 *s, Spu2Voice *v, uint32_t addr)
{
    v->blk = addr & RAM_MASK & ~15u;
    v->flags = s->ram[v->blk + 1];
    if ((v->flags & FLAG_LOOP_START) && !v->lsa_written) v->lsa = v->blk;
    if (s->irq_enable && (s->irqa & ~15u) == v->blk) {        /* IRQ when any voice reads the IRQ address */
        s->irq_latched = 1;
        s->irq_enable = 0;                                     /* acknowledged by libsd; re-armed by the IOP */
    }
    if (v->phase < SPU2_ENV_STOPPED && !s->x_state_only) {
        spu2_adpcm_block(s->ram + v->blk, v->dec, &v->h1, &v->h2);
        v->decoded = 1;
    } else {
        v->decoded = 0;
    }
    /* a start / loop address 8 bytes into a block (the IOP aligns starts to 8 bytes) begins at sample 16, as the
     * hardware word counter does in PCSX2 (real-hardware behaviour unknown) */
    v->idx = (uint8_t)((addr & 15u) * 2u);
    if (v->idx > 27) v->idx = 27;
}

static void end_of_block(Spu2 *s, Spu2Voice *v, unsigned vi)
{
    uint32_t next;
    if (v->flags & FLAG_LOOP_END) {
        s->endx |= (uint64_t)1 << vi;
        next = v->lsa;
        if (!(v->flags & FLAG_REPEAT)) voice_stop(v);
    } else {
        next = v->blk + 16;
    }
    enter_block(s, v, next);
}

static void consume(Spu2 *s, Spu2Voice *v, unsigned vi)
{
    v->s_prev = v->s_cur;
    v->s_cur = v->decoded ? v->dec[v->idx] : 0;
    if (s->tap && v->decoded) s->tap(s->tap_user, vi, (int16_t)v->s_cur);
    if (++v->idx >= 28) end_of_block(s, v, vi);
}

/* advance a silent (stopped) voice by n samples without decoding: positions, loop flags and the IRQ still run */
static void skip(Spu2 *s, Spu2Voice *v, unsigned vi, uint32_t n)
{
    while (n) {
        uint32_t k = 28u - v->idx;
        if (k > n) k = n;
        v->idx = (uint8_t)(v->idx + k);
        n -= k;
        if (v->idx >= 28) end_of_block(s, v, vi);
        if (v->blk == PARK && v->flags == (FLAG_LOOP_END | FLAG_REPEAT | FLAG_LOOP_START)) break; /* parked */
    }
    v->s_prev = v->s_cur = 0;
}

static uint32_t fast_env_cinc(const Spu2Voice *v);
static void env_counter_advance(Spu2Voice *v, uint32_t cinc, uint32_t n);
static void x_quiet_ticks(Spu2Voice *v, uint32_t n);

/* Spu2.mix_limit (the PSP): a sounding voice that is not mixed in this block. Everything the IOP model can
   observe evolves exactly as in the per-tick path - envelope (ENVX), volumes, position (NAX), loop flags, ENDX, LSA, the
   IRQ - and every block it enters is still decoded (the ADPCM history stays exact, so it can be mixed again from any
   block on); only its samples are not interpolated, scaled and summed. No parked-block shortcut (unlike skip): NAX of a
   sounding voice is observable. */
static void quiet_advance(Spu2 *s, Spu2Voice *v, unsigned vi, uint32_t n, uint32_t pitch)
{
    uint32_t cinc = fast_env_cinc(v);
    if (cinc) {
        env_counter_advance(v, cinc, n);
    } else if (!s->x_ref) {
        x_quiet_ticks(v, n);                                   /* the same ticks, state in locals */
    } else {
        for (uint32_t k = 0; k < n; k++) {
            spu2_vol_tick(&v->vl);
            spu2_vol_tick(&v->vr);
            if (v->phase < SPU2_ENV_STOPPED) spu2_env_tick(v);
        }
    }
    uint32_t total = v->counter + pitch * n;
    v->counter = total & 0xFFFu;
    for (uint32_t m = total >> 12; m;) {
        uint32_t k = 28u - v->idx;
        if (k > m) k = m;
        v->idx = (uint8_t)(v->idx + k);
        m -= k;
        if (v->idx >= 28) end_of_block(s, v, vi);
    }
    if (v->decoded && v->idx >= 2) {                           /* interpolation can resume where the voice is */
        v->s_prev = v->dec[v->idx - 2];
        v->s_cur = v->dec[v->idx - 1];
    } else {
        v->s_prev = v->s_cur = 0;
    }
}

/* the voices beyond the mix_limit loudest (envelope x |left| + |right|) of the sounding ones */
static void select_quiet(const Spu2 *s, uint8_t *quiet)
{
    uint8_t idx[SPU2_VOICES];
    uint32_t loud[SPU2_VOICES];
    unsigned n = 0;
    for (unsigned vi = 0; vi < SPU2_VOICES; vi++) {
        const Spu2Voice *v = &s->v[vi];
        quiet[vi] = 0;
        if (v->phase >= SPU2_ENV_STOPPED) continue;
        uint32_t l = (uint32_t)(v->vl.value < 0 ? -v->vl.value : v->vl.value);
        uint32_t r = (uint32_t)(v->vr.value < 0 ? -v->vr.value : v->vr.value);
        uint32_t lv = (uint32_t)v->env * (l + r);
        unsigned j = n++;
        while (j > 0 && loud[j - 1] < lv) { loud[j] = loud[j - 1]; idx[j] = idx[j - 1]; j--; }
        loud[j] = lv;
        idx[j] = (uint8_t)vi;
    }
    for (unsigned k = s->mix_limit; k < n; k++) quiet[idx[k]] = 1;
}
/* ------------------------------------------------------------------ registers */

void spu2_init(Spu2 *s, uint8_t *ram)
{
    uint8_t *p = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    s->ram = ram;
    s->mvol = spu2_vol_fixed(0x3FFF);
    s->interp = SPU2_INTERP_LINEAR;
    s->rs_frac = 0x10000u;
    /* the silent looping block LSAX is parked on (flags loop end | repeat | loop start) */
    for (unsigned i = 0; i < 16; i++) ram[PARK + i] = 0;
    ram[PARK + 1] = FLAG_LOOP_END | FLAG_REPEAT | FLAG_LOOP_START;
    for (unsigned vi = 0; vi < SPU2_VOICES; vi++) {
        Spu2Voice *v = &s->v[vi];
        v->pitch = 0x3FFF;
        v->ssa = v->lsa = PARK;
        v->phase = SPU2_ENV_STOPPED;
        enter_block(s, v, PARK);
    }
    s->endx = 0xFFFFFFFFFFFFull;
}

void spu2_set_param(Spu2 *s, unsigned vi, unsigned param, uint16_t value)
{
    if (vi >= SPU2_VOICES) return;
    Spu2Voice *v = &s->v[vi];
    switch (param) {
    case 0: v->vl.reg = value; if (!(value & 0x8000)) v->vl.value = spu2_vol_fixed(value); break;
    case 1: v->vr.reg = value; if (!(value & 0x8000)) v->vr.value = spu2_vol_fixed(value); break;
    case 2: v->pitch = value; break;
    case 3: v->adsr1 = value; break;
    case 4: v->adsr2 = value; break;
    default: break;
    }
}

void spu2_set_addr(Spu2 *s, unsigned vi, unsigned which, uint32_t addr)
{
    if (vi >= SPU2_VOICES) return;
    if (which == 0) {
        s->v[vi].ssa = addr & RAM_MASK;
    } else {
        s->v[vi].lsa = addr & RAM_MASK;
        s->v[vi].lsa_written = 1;
    }
}

uint16_t spu2_get_envx(const Spu2 *s, unsigned vi) { return vi < SPU2_VOICES ? (uint16_t)s->v[vi].env : 0; }

uint32_t spu2_get_nax(const Spu2 *s, unsigned vi)
{
    if (vi >= SPU2_VOICES) return 0;
    const Spu2Voice *v = &s->v[vi];
    return v->blk + 2u + 2u * (v->idx / 4u);                   /* address of the word holding the next sample */
}

void spu2_key_on(Spu2 *s, unsigned core, uint32_t mask)
{
    for (unsigned n = 0; n < 24; n++) {
        if (!(mask >> n & 1)) continue;
        unsigned vi = core * 24 + n;
        if (vi >= SPU2_VOICES) continue;
        Spu2Voice *v = &s->v[vi];
        v->phase = SPU2_ENV_ATTACK;
        v->env = 0;
        v->env_counter = 0;
        v->lsa_written = 0;
        v->counter = 0;
        v->s_prev = v->s_cur = 0;
        v->h1 = v->h2 = 0;
        s->endx &= ~((uint64_t)1 << vi);
        enter_block(s, v, v->ssa);
    }
}

void spu2_key_off(Spu2 *s, unsigned core, uint32_t mask)
{
    for (unsigned n = 0; n < 24; n++) {
        if (!(mask >> n & 1)) continue;
        unsigned vi = core * 24 + n;
        if (vi >= SPU2_VOICES) continue;
        Spu2Voice *v = &s->v[vi];
        if (v->phase < SPU2_ENV_STOPPED) {
            v->phase = SPU2_ENV_RELEASE;
            v->env_counter = 0;
        }
    }
}

void spu2_set_position(Spu2 *s, unsigned vi, uint32_t nax)
{
    if (vi >= SPU2_VOICES) return;
    Spu2Voice *v = &s->v[vi];
    uint32_t word = (nax & 15u) >> 1;                          /* 1..7 = data words of the block */
    enter_block(s, v, nax & ~15u);
    v->idx = (uint8_t)(word ? (word - 1u) * 4u : 0u);
}

void spu2_set_irq(Spu2 *s, uint32_t addr, int enable)
{
    s->irqa = addr & RAM_MASK;
    s->irq_enable = enable ? 1 : 0;
}

int spu2_irq_take(Spu2 *s)
{
    int r = s->irq_latched;
    s->irq_latched = 0;
    return r;
}

uint32_t spu2_sounding(const Spu2 *s)
{
    uint32_t n = 0;
    for (unsigned vi = 0; vi < SPU2_VOICES; vi++) n += s->v[vi].env > 0;
    return n;
}

/* ------------------------------------------------------------------ mixing */

/* Fast path: in sustain at the maximum level with an increasing sustain rate (the IOP's ADSR2 0x1FC9 / 0x1FC0 hold)
 * and fixed volumes, the envelope level and both volumes cannot change: only the envelope counter moves, and that is
 * advanced in closed form. Output is bit-identical to ticking spu2_env_tick / spu2_vol_tick every sample. */
static uint32_t fast_env_cinc(const Spu2Voice *v)
{
    if (v->phase != SPU2_ENV_SUSTAIN || v->env != 0x7FFF || ((v->vl.reg | v->vr.reg) & 0x8000)) return 0;
    if ((v->adsr2 >> 14) & 1) return 0;                          /* decreasing sustain */
    int shift = (v->adsr2 >> 8) & 0x1F;
    uint32_t cinc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    if ((v->adsr2 >> 15) && v->env > 0x6000) cinc >>= 2;
    return cinc ? cinc : 1;
}

static void env_counter_advance(Spu2Voice *v, uint32_t cinc, uint32_t n)
{
    uint32_t c = v->env_counter;                                 /* always < 0x8000 between ticks */
    uint32_t k = (0x8000u - c + cinc - 1) / cinc;                /* ticks until the first reset */
    if (n < k) { v->env_counter = c + n * cinc; return; }
    uint32_t period = (0x8000u + cinc - 1) / cinc;
    v->env_counter = ((n - k) % period) * cinc;
}

/* the fast-path sample loop with the voice state in locals; returns the index of the first tick not rendered.
 * Kept out of line so the compiler gives its few live values registers (inlined, psp-gcc spills them). */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint32_t fast_span(Spu2 *s, Spu2Voice *v, unsigned vi, int32_t *al, int32_t *ar, uint32_t i, uint32_t n,
                          uint32_t pitch, int nearest)
{
    const int32_t gl = v->vl.value, gr = v->vr.value;
    uint32_t counter = v->counter, idx = v->idx;
    int32_t sp = v->s_prev, sc = v->s_cur;
    const int16_t *dec = v->dec;
    void (*const tap)(void *, unsigned, int16_t) = s->tap;
    const int mute = gl == 0 && gr == 0;                         /* inaudible: decode and advance only */
    for (; i < n; i++) {
        if (!mute) {
            int32_t smp = nearest ? sc : sp + (((sc - sp) * (int32_t)(counter & 0xFFF)) >> 12);
            int32_t o = (smp * 0x7FFF) >> 15;
            al[i] += (o * gl) >> 15;
            ar[i] += (o * gr) >> 15;
        }
        counter += pitch;
        while (counter >= 0x1000u) {
            counter -= 0x1000u;
            sp = sc;
            sc = dec[idx];
            if (tap) tap(s->tap_user, vi, (int16_t)sc);
            if (++idx >= 28) {
                v->s_prev = sp;
                v->s_cur = sc;
                v->idx = (uint8_t)idx;
                end_of_block(s, v, vi);
                if (v->phase != SPU2_ENV_SUSTAIN) {             /* stopped by an end flag: finish as consume() would */
                    v->counter = counter;
                    while (v->counter >= 0x1000u) {
                        v->counter -= 0x1000u;
                        consume(s, v, vi);
                    }
                    return i + 1;
                }
                idx = v->idx;
            }
        }
    }
    v->counter = counter;
    v->idx = (uint8_t)idx;
    v->s_prev = sp;
    v->s_cur = sc;
    return i;
}

/* ------------------------------------------------------------------ the per-tick path with the state in locals.
 * Bit-identical to the reference loop below (spu2_vol_tick x2, interpolation, spu2_env_tick,
 * mix, consume per 48 kHz tick; s->x_ref = 1 selects the reference): the volume sweep and envelope rates are decoded
 * once per span / phase instead of every tick, and nothing is called per tick except at a block end. On a host bench
 * ~45 % of the mixed voice ticks of scripted gameplay took this path (volume sweeps). */
typedef struct { int sweep, exp, decr, ph, nochg; uint32_t cinc; int32_t linc; } XVolP;
typedef struct { int exp, decr; uint32_t cinc; int32_t linc, target; } XEnvP;

static inline void xvol_init(XVolP *p, uint16_t r)
{
    p->sweep = (r & 0x8000) != 0;
    p->exp = (r >> 14) & 1;
    p->decr = (r >> 13) & 1;
    p->ph = (r >> 12) & 1;
    int shift = (r >> 2) & 0x1F, stepf = r & 3;
    int32_t step = 7 - stepf;
    if (p->decr) step = ~step;
    p->cinc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    p->linc = step * (1 << (shift < 11 ? 11 - shift : 0));
    p->nochg = shift == 0x1F && stepf == 3;
}

/* = spu2_vol_tick for a sweeping register */
static inline void xvol_tick(const XVolP *p, int32_t *value, uint32_t *counter)
{
    uint32_t cinc = p->cinc;
    int32_t linc = p->linc, val = *value;
    if (p->exp) {
        if (!p->decr && val > 0x6000) cinc >>= 2;
        if (p->decr) linc = (int16_t)((linc * val) >> 15);
    }
    if (!p->nochg && cinc == 0) cinc = 1;
    uint32_t cnt = *counter + cinc;
    if (!(p->exp && p->decr)) linc = p->ph ? -linc : linc;
    if (cnt >= 0x8000u) {
        cnt = 0;
        if (!p->decr) {
            *value = clamp16(val + linc);
        } else {
            int32_t lo = p->ph ? -32768 : 0, hi = p->ph ? 0 : 32767;
            if (p->exp) { lo = 0; hi = 32767; }
            *value = clampi(val + linc, lo, hi);
        }
    }
    *counter = cnt;
}

/* the counter increment xvol_tick / xenv_tick would use at this level (0: the counter never moves) */
static inline uint32_t xvol_cinc(const XVolP *p, int32_t val)
{
    uint32_t cinc = p->cinc;
    if (p->exp && !p->decr && val > 0x6000) cinc >>= 2;
    if (!p->nochg && cinc == 0) cinc = 1;
    return cinc;
}

static inline uint32_t xenv_cinc(const XEnvP *e, int32_t env)
{
    uint32_t cinc = e->cinc;
    if (e->exp && !e->decr && env > 0x6000) cinc >>= 2;
    return cinc ? cinc : 1;
}

static inline void xenv_init(XEnvP *e, const Spu2Voice *v)
{
    EnvParams p;
    env_params(v, &p);
    e->exp = p.exp;
    e->decr = p.decr;
    e->target = p.target;
    e->cinc = 0x8000u >> (p.shift > 11 ? p.shift - 11 : 0);
    e->linc = p.step * (1 << (p.shift < 11 ? 11 - p.shift : 0));
}

/* = spu2_env_tick for phase < STOPPED; returns the new phase (env 0 when stopped) */
static inline int xenv_tick(const XEnvP *e, int phase, int32_t *env, uint32_t *ecnt)
{
    uint32_t cinc = e->cinc;
    int32_t linc = e->linc, ev = *env;
    if (e->exp) {
        if (!e->decr && ev > 0x6000) cinc >>= 2;
        if (e->decr) linc = (int16_t)((linc * ev) >> 15);
    }
    if (cinc == 0) cinc = 1;
    uint32_t c = *ecnt + cinc;
    if (c >= 0x8000u) {
        c = 0;
        ev = clampi(ev + linc, 0, 0x7FFF);
    }
    *ecnt = c;
    if (phase == SPU2_ENV_SUSTAIN) {
        if (ev == 0) { *env = 0; return SPU2_ENV_STOPPED; }
        *env = ev;
        return phase;
    }
    if ((!e->decr && ev >= e->target) || (e->decr && ev <= e->target)) phase++;
    if (phase >= SPU2_ENV_STOPPED) { *env = 0; return SPU2_ENV_STOPPED; }
    *env = ev;
    return phase;
}

/* ticks [i, n) of a sounding voice on the per-tick path (mix = 0: advance only, as quiet_advance's per-tick branch -
   NOT used for that: quiet voices keep ticking volumes after a stop); returns the first tick not done: n, the tick after
   the one that stopped the voice, or the first tick on which the fast path (fast_env_cinc) applies */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint32_t x_slow_span(Spu2 *s, Spu2Voice *v, unsigned vi, int32_t *al, int32_t *ar, uint32_t i, uint32_t n,
                            uint32_t pitch, int nearest)
{
    XVolP pl, pr;
    XEnvP pe;
    xvol_init(&pl, v->vl.reg);
    xvol_init(&pr, v->vr.reg);
    int phase = v->phase;
    xenv_init(&pe, v);
    int32_t vl = v->vl.value, vr = v->vr.value, env = v->env;
    uint32_t cl = v->vl.counter, cr = v->vr.counter, ecnt = v->env_counter;
    uint32_t counter = v->counter, idx = v->idx;
    int32_t sp = v->s_prev, sc = v->s_cur;
    int decoded = v->decoded;
    const int16_t *dec = v->dec;
    void (*const tap)(void *, unsigned, int16_t) = s->tap;
    /* the fast path's entry condition minus the envelope level (fast_env_cinc) */
    const int fast_ok = !pl.sweep && !pr.sweep && !((v->adsr2 >> 14) & 1);
    while (i < n) {
        if (fast_ok && phase == SPU2_ENV_SUSTAIN && env == 0x7FFF) break;
        if (pl.sweep) xvol_tick(&pl, &vl, &cl);
        if (pr.sweep) xvol_tick(&pr, &vr, &cr);
        int32_t smp = nearest ? sc : sp + (((sc - sp) * (int32_t)(counter & 0xFFF)) >> 12);
        int nph = xenv_tick(&pe, phase, &env, &ecnt);
        int32_t o = (smp * env) >> 15;
        al[i] += (o * vl) >> 15;
        ar[i] += (o * vr) >> 15;
        counter += pitch;
        if (nph != phase) {                                    /* next phase (or stopped): rates of the new phase */
            phase = nph;
            v->phase = (uint8_t)phase;
            if (phase < SPU2_ENV_STOPPED) xenv_init(&pe, v);
        }
        while (counter >= 0x1000u) {                           /* consume() */
            counter -= 0x1000u;
            sp = sc;
            sc = decoded ? dec[idx] : 0;
            if (tap && decoded) tap(s->tap_user, vi, (int16_t)sc);
            if (++idx >= 28) {
                v->idx = (uint8_t)idx;
                v->phase = (uint8_t)phase;
                v->env = env;
                end_of_block(s, v, vi);                         /* may stop the voice (end flag without repeat) */
                idx = v->idx;
                decoded = v->decoded;
                if (v->phase != phase) {
                    phase = v->phase;
                    env = v->env;
                    if (phase < SPU2_ENV_STOPPED) xenv_init(&pe, v);
                }
            }
        }
        i++;
        if (phase >= SPU2_ENV_STOPPED) break;
        /* constant run: no volume / envelope counter reaches 0x8000 within the next L ticks, so both volumes and the
           envelope level keep their values, and so does the phase (its compare sees the same level every tick) -
           mixed like fast_span, the counters advanced by L increments at the end */
        if (i >= n || (fast_ok && phase == SPU2_ENV_SUSTAIN && env == 0x7FFF)) continue;
        if (phase == SPU2_ENV_SUSTAIN ? env == 0
                                      : ((!pe.decr && env >= pe.target) || (pe.decr && env <= pe.target))) continue;
        uint32_t L = n - i, kl = 0, kr = 0, ke;
        if (pl.sweep && (kl = xvol_cinc(&pl, vl)) != 0 && (0x7FFFu - cl) / kl < L) L = (0x7FFFu - cl) / kl;
        if (pr.sweep && (kr = xvol_cinc(&pr, vr)) != 0 && (0x7FFFu - cr) / kr < L) L = (0x7FFFu - cr) / kr;
        ke = xenv_cinc(&pe, env);
        if ((0x7FFFu - ecnt) / ke < L) L = (0x7FFFu - ecnt) / ke;
        if (L < 2) continue;
        const int32_t gl = vl, gr = vr, ge = env;
        uint32_t t = 0, stopped = 0;
        for (; t < L && !stopped; t++, i++) {
            int32_t smp2 = nearest ? sc : sp + (((sc - sp) * (int32_t)(counter & 0xFFF)) >> 12);
            int32_t o2 = (smp2 * ge) >> 15;
            al[i] += (o2 * gl) >> 15;
            ar[i] += (o2 * gr) >> 15;
            counter += pitch;
            while (counter >= 0x1000u) {
                counter -= 0x1000u;
                sp = sc;
                sc = decoded ? dec[idx] : 0;
                if (tap && decoded) tap(s->tap_user, vi, (int16_t)sc);
                if (++idx >= 28) {
                    v->idx = (uint8_t)idx;
                    v->phase = (uint8_t)phase;
                    v->env = env;
                    end_of_block(s, v, vi);
                    idx = v->idx;
                    decoded = v->decoded;
                    if (v->phase != phase) {                    /* stopped by an end flag: finish this tick */
                        phase = v->phase;
                        env = v->env;
                        stopped = 1;
                        if (phase < SPU2_ENV_STOPPED) xenv_init(&pe, v);
                    }
                }
            }
        }
        if (pl.sweep) cl += t * kl;                              /* t ticks done (< the first crossing) */
        if (pr.sweep) cr += t * kr;
        ecnt += t * ke;
        if (phase >= SPU2_ENV_STOPPED) break;
    }
    v->vl.value = vl; v->vl.counter = cl;
    v->vr.value = vr; v->vr.counter = cr;
    v->env = env; v->env_counter = ecnt; v->phase = (uint8_t)phase;
    v->counter = counter; v->idx = (uint8_t)idx;
    v->s_prev = sp; v->s_cur = sc;
    return i;
}

/* quiet_advance's per-tick branch: n x (volume ticks; envelope tick while the voice sounds) */
static void x_quiet_ticks(Spu2Voice *v, uint32_t n)
{
    XVolP pl, pr;
    XEnvP pe;
    xvol_init(&pl, v->vl.reg);
    xvol_init(&pr, v->vr.reg);
    int phase = v->phase;
    if (phase < SPU2_ENV_STOPPED) xenv_init(&pe, v);
    int32_t vl = v->vl.value, vr = v->vr.value, env = v->env;
    uint32_t cl = v->vl.counter, cr = v->vr.counter, ecnt = v->env_counter;
    for (uint32_t k = 0; k < n;) {
        if (pl.sweep) xvol_tick(&pl, &vl, &cl);
        if (pr.sweep) xvol_tick(&pr, &vr, &cr);
        if (phase < SPU2_ENV_STOPPED) {
            int nph = xenv_tick(&pe, phase, &env, &ecnt);
            if (nph != phase) {
                phase = nph;
                v->phase = (uint8_t)phase;
                if (phase < SPU2_ENV_STOPPED) xenv_init(&pe, v);
            }
        }
        k++;
        /* constant run (as in x_slow_span): no counter reaches 0x8000 within L ticks -> levels and phase unchanged */
        uint32_t L = n - k, kl = 0, kr = 0, ke = 0;
        if (!L) break;
        if (phase < SPU2_ENV_STOPPED) {
            if (phase == SPU2_ENV_SUSTAIN ? env == 0
                                          : ((!pe.decr && env >= pe.target) || (pe.decr && env <= pe.target))) continue;
            ke = xenv_cinc(&pe, env);
            if ((0x7FFFu - ecnt) / ke < L) L = (0x7FFFu - ecnt) / ke;
        }
        if (pl.sweep && (kl = xvol_cinc(&pl, vl)) != 0 && (0x7FFFu - cl) / kl < L) L = (0x7FFFu - cl) / kl;
        if (pr.sweep && (kr = xvol_cinc(&pr, vr)) != 0 && (0x7FFFu - cr) / kr < L) L = (0x7FFFu - cr) / kr;
        if (!L) continue;
        if (pl.sweep) cl += L * kl;
        if (pr.sweep) cr += L * kr;
        if (phase < SPU2_ENV_STOPPED) ecnt += L * ke;
        k += L;
    }
    v->vl.value = vl; v->vl.counter = cl;
    v->vr.value = vr; v->vr.counter = cr;
    v->env = env; v->env_counter = ecnt; v->phase = (uint8_t)phase;
}

static void render_block(Spu2 *s, int16_t *out, uint32_t n)
{
    int32_t acc[2][2][SPU2_BLOCK_FRAMES];                       /* [core][L/R][frame] */
    for (unsigned c = 0; c < 2; c++)
        for (unsigned ch = 0; ch < 2; ch++)
            for (uint32_t i = 0; i < n; i++) acc[c][ch][i] = 0;
    uint32_t sounding = 0;
    const int nearest = s->interp == SPU2_INTERP_NEAREST;
    uint8_t quiet[SPU2_VOICES];
    const int culling = s->mix_limit && s->mix_limit < SPU2_VOICES && !s->tap;
    if (culling) select_quiet(s, quiet);
    for (unsigned vi = 0; vi < SPU2_VOICES; vi++) {
        Spu2Voice *v = &s->v[vi];
        int32_t *al = acc[vi / 24][0], *ar = acc[vi / 24][1];
        uint32_t pitch = v->pitch > 0x3FFF ? 0x3FFF : v->pitch;
        uint32_t i = 0;
        if (v->phase < SPU2_ENV_STOPPED) sounding++;
        if ((s->x_state_only || (culling && quiet[vi])) && v->phase < SPU2_ENV_STOPPED) {   /* not mixed */
            quiet_advance(s, v, vi, n, pitch);
            s->ticks_quiet += n;
            continue;
        }
        if (v->phase < SPU2_ENV_STOPPED) s->ticks_mixed += n;
        while (i < n && v->phase < SPU2_ENV_STOPPED) {
            uint32_t cinc = fast_env_cinc(v);
            if (cinc) {                                          /* constant envelope and volumes */
                uint32_t i0 = i;
                i = fast_span(s, v, vi, al, ar, i, n, pitch, nearest);
                if (v->phase == SPU2_ENV_SUSTAIN) env_counter_advance(v, cinc, i - i0);
                continue;
            }
            if (!s->x_ref) {                                     /* same ticks, state in locals */
                i = x_slow_span(s, v, vi, al, ar, i, n, pitch, nearest);
                continue;
            }
            spu2_vol_tick(&v->vl);
            spu2_vol_tick(&v->vr);
            int32_t smp = nearest ? v->s_cur
                                  : v->s_prev + (((v->s_cur - v->s_prev) * (int32_t)(v->counter & 0xFFF)) >> 12);
            spu2_env_tick(v);
            int32_t o = (smp * v->env) >> 15;
            al[i] += (o * v->vl.value) >> 15;
            ar[i] += (o * v->vr.value) >> 15;
            v->counter += pitch;
            while (v->counter >= 0x1000u) {
                v->counter -= 0x1000u;
                consume(s, v, vi);
            }
            i++;
        }
        if (i < n) {                                            /* silent for the rest of the block */
            uint32_t m = n - i;
            if ((v->vl.reg | v->vr.reg) & 0x8000)
                for (uint32_t k = 0; k < m; k++) { spu2_vol_tick(&v->vl); spu2_vol_tick(&v->vr); }
            uint32_t total = v->counter + pitch * m;
            v->counter = total & 0xFFFu;
            skip(s, v, vi, total >> 12);
        }
    }
    if (sounding > s->peak_voices) s->peak_voices = sounding;
    s->clock += n;
    if (!out) return;
    for (uint32_t i = 0; i < n; i++) {
        for (unsigned ch = 0; ch < 2; ch++) {
            int32_t m = clamp16(acc[0][ch][i]) + clamp16(acc[1][ch][i]);
            out[2 * i + ch] = (int16_t)clamp16((clamp16(m) * s->mvol) >> 15);
        }
    }
}

static void render_span(Spu2 *s, int16_t *out, uint32_t frames, int to_sink)
{
    while (frames) {
        if (s->timer_cb && s->timer_period) {
            if (s->timer_count >= s->timer_period) {
                s->timer_count = 0;
                s->timer_cb(s->timer_user);
            }
        }
        uint32_t n = frames < SPU2_BLOCK_FRAMES ? frames : SPU2_BLOCK_FRAMES;
        if (s->timer_cb && s->timer_period && n > s->timer_period - s->timer_count)
            n = s->timer_period - s->timer_count;
        int16_t *dst = out ? out : (to_sink && s->sink ? s->sink48 : 0);
        render_block(s, dst, n);
        if (to_sink && s->sink && dst) s->sink(s->sink_user, dst, n);
        s->timer_count += n;
        frames -= n;
        if (out) out += 2 * n;
    }
}

void spu2_render48(Spu2 *s, int16_t *out, uint32_t frames) { render_span(s, out, frames, 0); }

void spu2_advance(Spu2 *s, uint32_t frames48) { render_span(s, 0, frames48, 1); }

void spu2_render(Spu2 *s, int16_t *out, uint32_t frames, uint32_t out_rate)
{
    if (out_rate == SPU2_RATE || out_rate == 0) { render_span(s, out, frames, 0); return; }
    uint32_t step = (uint32_t)(((uint64_t)SPU2_RATE << 16) / out_rate);
    for (uint32_t i = 0; i < frames; i++) {
        while (s->rs_frac >= 0x10000u) {
            if (s->blk48_pos >= s->blk48_len) {
                render_span(s, s->blk48, SPU2_BLOCK_FRAMES, 0);
                s->blk48_len = SPU2_BLOCK_FRAMES;
                s->blk48_pos = 0;
            }
            s->rs_prev[0] = s->rs_cur[0];
            s->rs_prev[1] = s->rs_cur[1];
            s->rs_cur[0] = s->blk48[2 * s->blk48_pos];
            s->rs_cur[1] = s->blk48[2 * s->blk48_pos + 1];
            s->blk48_pos++;
            s->rs_frac -= 0x10000u;
        }
        int32_t f = (int32_t)(s->rs_frac >> 1);                   /* 15-bit fraction */
        out[2 * i] = (int16_t)(s->rs_prev[0] + (((s->rs_cur[0] - s->rs_prev[0]) * f) >> 15));
        out[2 * i + 1] = (int16_t)(s->rs_prev[1] + (((s->rs_cur[1] - s->rs_prev[1]) * f) >> 15));
        s->rs_frac += step;
    }
}

/* ------------------------------------------------------------------ IOP model binding */

static void op_set_param(void *u, unsigned v, unsigned p, uint16_t val) { spu2_set_param((Spu2 *)u, v, p, val); }
static void op_set_addr(void *u, unsigned v, unsigned w, uint32_t a) { spu2_set_addr((Spu2 *)u, v, w, a); }
static uint16_t op_get_envx(void *u, unsigned v) { return spu2_get_envx((Spu2 *)u, v); }
static uint32_t op_get_nax(void *u, unsigned v) { return spu2_get_nax((Spu2 *)u, v); }
static void op_key_on(void *u, unsigned c, uint32_t m) { spu2_key_on((Spu2 *)u, c, m); }
static void op_key_off(void *u, unsigned c, uint32_t m) { spu2_key_off((Spu2 *)u, c, m); }
static void op_set_irq(void *u, uint32_t a, int e) { spu2_set_irq((Spu2 *)u, a, e); }
static int op_irq_take(void *u) { return spu2_irq_take((Spu2 *)u); }
static void op_advance(void *u, uint32_t n) { spu2_advance((Spu2 *)u, n); }
void spu2_bind_ops(Spu2 *s, Spu2Ops *ops, void (*timer_cb)(void *), void *timer_user, int lockstep)
{
    ops->user = s;
    ops->ram = s->ram;
    ops->set_param = op_set_param;
    ops->set_addr = op_set_addr;
    ops->get_envx = op_get_envx;
    ops->get_nax = op_get_nax;
    ops->key_on = op_key_on;
    ops->key_off = op_key_off;
    ops->set_irq = op_set_irq;
    ops->irq_take = op_irq_take;
    ops->dma_done = 0;
    ops->advance = lockstep ? op_advance : 0;
    s->timer_cb = timer_user ? timer_cb : 0;
    s->timer_user = timer_user;
    s->timer_period = SPU2_TIMER_TICKS;
    s->timer_count = 0;
}
