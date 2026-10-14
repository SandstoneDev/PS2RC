/* r5900_rt.h - runtime support for C code generated from R5900 (PS2 EE) machine code by ps2rc.
 *
 * A straightforward, correctness-first model. Semantics: the R5900 behaviour, checked instruction by instruction
 * against the PCSX2 interpreter (the oracle, tests/poc1); the deviations from PCSX2 are listed below.
 *
 * FPU model (PCSX2-interpreter equivalent with GameDB eeClampMode 2, "extra + preserve sign"):
 *   inputs with exponent 0 -> +-0, exponent 255 -> +-0x7F7FFFFF; arithmetic in IEEE single with the host FPU in
 *   round-toward-zero + flush-to-zero mode (r5900_host_fpu_init); results Inf -> +-0x7F7FFFFF, denormal -> +-0.
 *   Documented deviation from Sony hardware: exponent-255 values are ordinary numbers on the EE and Fmax is 0x7FFFFFFF.
 *   FCR31: only the C (condition) bit is modelled (no cause/sticky flags).
 * Shift-amount register: this runtime keeps it as a bit count (MTSAB stores ((rs ^ imm) & 15) * 8, QFSRV uses it as
 * is); PCSX2 keeps bytes, so MFSA reads 8x PCSX2's value - the one accepted difference of tests/poc1.
 */
#ifndef R5900_RT_H
#define R5900_RT_H

#include <stdint.h>
#include <string.h>

typedef union __attribute__((aligned(16))) {
    uint64_t d[2];
    int64_t sd[2];
    uint32_t w[4];
    int32_t sw[4];
    uint16_t h[8];
    int16_t sh[8];
    uint8_t b[16];
    int8_t sb[16];
} R5900Reg128;

/* VU0 state for COP2 macro mode (see the "VU0 macro mode" section below) */
typedef struct R5900VU0 {
    R5900Reg128 vf[32];          /* VF00-VF31. vf[0] is never read or written: VF00 = (0, 0, 0, 1.0) is a constant */
    R5900Reg128 acc;             /* ACC */
    uint32_t vi[32];             /* CCR[2,n]: VI00-VI15 (16-bit, VI00 = 0 constant), 16 status, 17 MAC, 18 clip, 20 R,
                                    21 I, 22 Q, 23 P, 26 TPC, 27 CMSAR0, 28 FBRST, 29 VPU-STAT, 31 CMSAR1 */
} R5900VU0;

typedef struct R5900Ctx {
    R5900Reg128 r[32];
    R5900Reg128 hi, lo;          /* d[0]: pipeline 0 HI/LO, d[1]: HI1/LO1 */
    uint32_t sa;                 /* shift amount in bits */
    union { float f; uint32_t u; } fpr[32], acc;
    uint32_t fcr31;
    uint8_t *ram;                /* guest memory window */
    uint32_t ram_mask;           /* window size - 1 (power of two) */
    R5900VU0 vu0;                /* VU0 registers (COP2 macro mode), appended 2026-10-03 */
} R5900Ctx;

/* ------------------------------------------------------------------ memory (little-endian guest == host)
   Halfword/word/doubleword accesses are naturally aligned: on the EE a misaligned LH/LW/LD/SH/SW/SD raises an
   address error, so correct guest code never issues one (the host traps the same way). The guest window must be
   allocated 16-byte aligned. */
#define RT_PTR(c, a) ((c)->ram + ((uint32_t)(a) & (c)->ram_mask))
#define RT_AL(c, a, n) __builtin_assume_aligned(RT_PTR(c, a), n)
static inline uint8_t  rt_lb(R5900Ctx *c, uint32_t a) { return *RT_PTR(c, a); }
static inline uint16_t rt_lh(R5900Ctx *c, uint32_t a) { uint16_t v; memcpy(&v, RT_AL(c, a, 2), 2); return v; }
static inline uint32_t rt_lw(R5900Ctx *c, uint32_t a) { uint32_t v; memcpy(&v, RT_AL(c, a, 4), 4); return v; }
static inline uint64_t rt_ld(R5900Ctx *c, uint32_t a) { uint64_t v; memcpy(&v, RT_AL(c, a, 8), 8); return v; }
static inline void rt_sb(R5900Ctx *c, uint32_t a, uint8_t v) { *RT_PTR(c, a) = v; }
static inline void rt_sh(R5900Ctx *c, uint32_t a, uint16_t v) { memcpy(RT_AL(c, a, 2), &v, 2); }
static inline void rt_sw(R5900Ctx *c, uint32_t a, uint32_t v) { memcpy(RT_AL(c, a, 4), &v, 4); }
static inline void rt_sd(R5900Ctx *c, uint32_t a, uint64_t v) { memcpy(RT_AL(c, a, 8), &v, 8); }
static inline void rt_lq(R5900Ctx *c, uint32_t a, R5900Reg128 *d) { memcpy(d, RT_AL(c, a & ~15u, 16), 16); }
static inline void rt_sq(R5900Ctx *c, uint32_t a, const R5900Reg128 *s) { memcpy(RT_AL(c, a & ~15u, 16), s, 16); }

static inline int64_t rt_lwl(R5900Ctx *c, uint32_t a, uint64_t old) {
    static const uint32_t mask[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    unsigned s = a & 3; uint32_t mem = rt_lw(c, a & ~3u);
    return (int32_t)(((uint32_t)old & mask[s]) | (mem << (24 - 8 * s)));
}
static inline uint64_t rt_lwr(R5900Ctx *c, uint32_t a, uint64_t old) {
    static const uint32_t mask[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    unsigned s = a & 3; uint32_t mem = rt_lw(c, a & ~3u);
    uint32_t v = ((uint32_t)old & mask[s]) | (mem >> (8 * s));
    if (s == 0) return (uint64_t)(int64_t)(int32_t)v;           /* whole word: sign-extend */
    return (old & 0xFFFFFFFF00000000ull) | v;                    /* partial: upper half preserved */
}
static inline uint64_t rt_ldl(R5900Ctx *c, uint32_t a, uint64_t old) {
    unsigned s = a & 7; uint64_t mem = rt_ld(c, a & ~7u);
    uint64_t keep = s == 7 ? 0 : (0x00ffffffffffffffull >> (8 * s));
    return (old & keep) | (mem << (56 - 8 * s));
}
static inline uint64_t rt_ldr(R5900Ctx *c, uint32_t a, uint64_t old) {
    unsigned s = a & 7; uint64_t mem = rt_ld(c, a & ~7u);
    uint64_t keep = s == 0 ? 0 : ~(0xffffffffffffffffull >> (8 * s));
    return (old & keep) | (mem >> (8 * s));
}
/* unaligned stores (little-endian): SWL/SDL write the register's high bytes ending at `a`, SWR/SDR its low bytes
   starting at `a`; the other bytes of the aligned word/doubleword are kept */
static inline void rt_swl(R5900Ctx *c, uint32_t a, uint64_t v) {
    unsigned s = a & 3; uint32_t mem = rt_lw(c, a & ~3u);
    uint32_t keep = s == 3 ? 0 : (0xffffff00u << (8 * s));
    rt_sw(c, a & ~3u, (mem & keep) | ((uint32_t)v >> (24 - 8 * s)));
}
static inline void rt_swr(R5900Ctx *c, uint32_t a, uint64_t v) {
    unsigned s = a & 3; uint32_t mem = rt_lw(c, a & ~3u);
    uint32_t keep = s == 0 ? 0 : (0xffffffffu >> (32 - 8 * s));
    rt_sw(c, a & ~3u, (mem & keep) | ((uint32_t)v << (8 * s)));
}
static inline void rt_sdl(R5900Ctx *c, uint32_t a, uint64_t v) {
    unsigned s = a & 7; uint64_t mem = rt_ld(c, a & ~7u);
    uint64_t keep = s == 7 ? 0 : (0xffffffffffffff00ull << (8 * s));
    rt_sd(c, a & ~7u, (mem & keep) | (v >> (56 - 8 * s)));
}
static inline void rt_sdr(R5900Ctx *c, uint32_t a, uint64_t v) {
    unsigned s = a & 7; uint64_t mem = rt_ld(c, a & ~7u);
    uint64_t keep = s == 0 ? 0 : (0xffffffffffffffffull >> (64 - 8 * s));
    rt_sd(c, a & ~7u, (mem & keep) | (v << (8 * s)));
}

/* the same accessors over a function-local copy of the window (back-end v1+): stores through the guest window cannot
   alias a local, so the compiler keeps base and mask in registers instead of reloading them from the context */
typedef struct { uint8_t *ram; uint32_t mask; } R5900Mem;
#define RM_PTR(m, a) ((m).ram + ((uint32_t)(a) & (m).mask))
#define RM_AL(m, a, n) __builtin_assume_aligned(RM_PTR(m, a), n)
static inline uint8_t  rm_lb(R5900Mem m, uint32_t a) { return *RM_PTR(m, a); }
static inline uint16_t rm_lh(R5900Mem m, uint32_t a) { uint16_t v; memcpy(&v, RM_AL(m, a, 2), 2); return v; }
static inline uint32_t rm_lw(R5900Mem m, uint32_t a) { uint32_t v; memcpy(&v, RM_AL(m, a, 4), 4); return v; }
static inline uint64_t rm_ld(R5900Mem m, uint32_t a) { uint64_t v; memcpy(&v, RM_AL(m, a, 8), 8); return v; }
static inline void rm_sb(R5900Mem m, uint32_t a, uint8_t v) { *RM_PTR(m, a) = v; }
static inline void rm_sh(R5900Mem m, uint32_t a, uint16_t v) { memcpy(RM_AL(m, a, 2), &v, 2); }
static inline void rm_sw(R5900Mem m, uint32_t a, uint32_t v) { memcpy(RM_AL(m, a, 4), &v, 4); }
static inline void rm_sd(R5900Mem m, uint32_t a, uint64_t v) { memcpy(RM_AL(m, a, 8), &v, 8); }
static inline int64_t rm_lwl(R5900Mem m, uint32_t a, uint64_t old) {
    static const uint32_t mask[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    unsigned s = a & 3; uint32_t mem = rm_lw(m, a & ~3u);
    return (int32_t)(((uint32_t)old & mask[s]) | (mem << (24 - 8 * s)));
}
static inline uint64_t rm_lwr(R5900Mem m, uint32_t a, uint64_t old) {
    static const uint32_t mask[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    unsigned s = a & 3; uint32_t mem = rm_lw(m, a & ~3u);
    uint32_t v = ((uint32_t)old & mask[s]) | (mem >> (8 * s));
    if (s == 0) return (uint64_t)(int64_t)(int32_t)v;
    return (old & 0xFFFFFFFF00000000ull) | v;
}
static inline uint64_t rm_ldl(R5900Mem m, uint32_t a, uint64_t old) {
    unsigned s = a & 7; uint64_t mem = rm_ld(m, a & ~7u);
    uint64_t keep = s == 7 ? 0 : (0x00ffffffffffffffull >> (8 * s));
    return (old & keep) | (mem << (56 - 8 * s));
}
static inline uint64_t rm_ldr(R5900Mem m, uint32_t a, uint64_t old) {
    unsigned s = a & 7; uint64_t mem = rm_ld(m, a & ~7u);
    uint64_t keep = s == 0 ? 0 : ~(0xffffffffffffffffull >> (8 * s));
    return (old & keep) | (mem >> (8 * s));
}
static inline void rm_swl(R5900Mem m, uint32_t a, uint64_t v) {
    unsigned s = a & 3; uint32_t mem = rm_lw(m, a & ~3u);
    uint32_t keep = s == 3 ? 0 : (0xffffff00u << (8 * s));
    rm_sw(m, a & ~3u, (mem & keep) | ((uint32_t)v >> (24 - 8 * s)));
}
static inline void rm_swr(R5900Mem m, uint32_t a, uint64_t v) {
    unsigned s = a & 3; uint32_t mem = rm_lw(m, a & ~3u);
    uint32_t keep = s == 0 ? 0 : (0xffffffffu >> (32 - 8 * s));
    rm_sw(m, a & ~3u, (mem & keep) | ((uint32_t)v << (8 * s)));
}
static inline void rm_sdl(R5900Mem m, uint32_t a, uint64_t v) {
    unsigned s = a & 7; uint64_t mem = rm_ld(m, a & ~7u);
    uint64_t keep = s == 7 ? 0 : (0xffffffffffffff00ull << (8 * s));
    rm_sd(m, a & ~7u, (mem & keep) | (v >> (56 - 8 * s)));
}
static inline void rm_sdr(R5900Mem m, uint32_t a, uint64_t v) {
    unsigned s = a & 7; uint64_t mem = rm_ld(m, a & ~7u);
    uint64_t keep = s == 0 ? 0 : (0xffffffffffffffffull >> (64 - 8 * s));
    rm_sd(m, a & ~7u, (mem & keep) | (v << (8 * s)));
}
/* 128-bit accesses of LQC2/SQC2 in the back-end v1+ (v0 uses rt_lq/rt_sq): the low 4 address bits are ignored like
   LQ/SQ (an unaligned LQC2/SQC2 is an address error on the EE; PCSX2's recompiler masks the address); the VU0 register
   side is 16-byte aligned too (without the hint GCC copies into it with unaligned stores). One line each:
   ps2rc/pipeline/patch_rt_header.py replaces these definitions by name. */
static inline void rm_lq(R5900Mem m, uint32_t a, R5900Reg128 *d) { memcpy(__builtin_assume_aligned(d, 16), RM_AL(m, a & ~15u, 16), 16); }
static inline void rm_sq(R5900Mem m, uint32_t a, const R5900Reg128 *s) { memcpy(RM_AL(m, a & ~15u, 16), __builtin_assume_aligned(s, 16), 16); }

/* back-end v5 lever maskbase (step S4): the window mask applies to the base register only and the 16-bit offset is
   added after it, so the compiler shares ram + (base & mask) between the accesses through one base. The address is
   the rm_* one whenever (base & mask) + offset stays inside the window; otherwise (an end pointer such as 0x02000000
   with a negative offset, a pointer just below a mirror, an access beyond the end of RAM) it lies up to 32 KB + 16
   outside: the harness must keep RMB_GUARD bytes of host memory before and after the window [ram, ram + mask].
   RMB_CHECK (test builds): every such access calls rmb_mismatch(base, offset) (provided by the harness) first and
   then takes the rm_* address. */
#define RMB_GUARD 0x10000u
#ifdef RMB_CHECK
void rmb_mismatch(uint32_t base, int32_t off);
static inline uint8_t *rmb_ptr(R5900Mem m, uint32_t b, int32_t o) {
    uint32_t lo = b & m.mask;
    if ((int32_t)lo + o < 0 || lo + (uint32_t)o > m.mask) { rmb_mismatch(b, o); return RM_PTR(m, b + (uint32_t)o); }
    return m.ram + lo + o;
}
#define RMB_PTR(m, b, o) rmb_ptr(m, (uint32_t)(b), (int32_t)(o))
#else
#define RMB_PTR(m, b, o) ((m).ram + ((uint32_t)(b) & (m).mask) + (int32_t)(o))
#endif
#define RMB_AL(m, b, o, n) __builtin_assume_aligned(RMB_PTR(m, b, o), n)
static inline uint8_t  rmb_lb(R5900Mem m, uint32_t b, int32_t o) { return *RMB_PTR(m, b, o); }
static inline uint16_t rmb_lh(R5900Mem m, uint32_t b, int32_t o) { uint16_t v; memcpy(&v, RMB_AL(m, b, o, 2), 2); return v; }
static inline uint32_t rmb_lw(R5900Mem m, uint32_t b, int32_t o) { uint32_t v; memcpy(&v, RMB_AL(m, b, o, 4), 4); return v; }
static inline uint64_t rmb_ld(R5900Mem m, uint32_t b, int32_t o) { uint64_t v; memcpy(&v, RMB_AL(m, b, o, 8), 8); return v; }
static inline void rmb_sb(R5900Mem m, uint32_t b, int32_t o, uint8_t v) { *RMB_PTR(m, b, o) = v; }
static inline void rmb_sh(R5900Mem m, uint32_t b, int32_t o, uint16_t v) { memcpy(RMB_AL(m, b, o, 2), &v, 2); }
static inline void rmb_sw(R5900Mem m, uint32_t b, int32_t o, uint32_t v) { memcpy(RMB_AL(m, b, o, 4), &v, 4); }
static inline void rmb_sd(R5900Mem m, uint32_t b, int32_t o, uint64_t v) { memcpy(RMB_AL(m, b, o, 8), &v, 8); }
/* the unaligned and 128-bit forms: the aligned word / doubleword / quadword around base + offset */
static inline int64_t rmb_lwl(R5900Mem m, uint32_t b, int32_t o, uint64_t old) {
    static const uint32_t mask[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    unsigned s = (b + (uint32_t)o) & 3; uint32_t mem = rmb_lw(m, b, o - (int32_t)s);
    return (int32_t)(((uint32_t)old & mask[s]) | (mem << (24 - 8 * s)));
}
static inline uint64_t rmb_lwr(R5900Mem m, uint32_t b, int32_t o, uint64_t old) {
    static const uint32_t mask[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    unsigned s = (b + (uint32_t)o) & 3; uint32_t mem = rmb_lw(m, b, o - (int32_t)s);
    uint32_t v = ((uint32_t)old & mask[s]) | (mem >> (8 * s));
    if (s == 0) return (uint64_t)(int64_t)(int32_t)v;
    return (old & 0xFFFFFFFF00000000ull) | v;
}
static inline uint64_t rmb_ldl(R5900Mem m, uint32_t b, int32_t o, uint64_t old) {
    unsigned s = (b + (uint32_t)o) & 7; uint64_t mem = rmb_ld(m, b, o - (int32_t)s);
    uint64_t keep = s == 7 ? 0 : (0x00ffffffffffffffull >> (8 * s));
    return (old & keep) | (mem << (56 - 8 * s));
}
static inline uint64_t rmb_ldr(R5900Mem m, uint32_t b, int32_t o, uint64_t old) {
    unsigned s = (b + (uint32_t)o) & 7; uint64_t mem = rmb_ld(m, b, o - (int32_t)s);
    uint64_t keep = s == 0 ? 0 : ~(0xffffffffffffffffull >> (8 * s));
    return (old & keep) | (mem >> (8 * s));
}
static inline void rmb_swl(R5900Mem m, uint32_t b, int32_t o, uint64_t v) {
    unsigned s = (b + (uint32_t)o) & 3; uint32_t mem = rmb_lw(m, b, o - (int32_t)s);
    uint32_t keep = s == 3 ? 0 : (0xffffff00u << (8 * s));
    rmb_sw(m, b, o - (int32_t)s, (mem & keep) | ((uint32_t)v >> (24 - 8 * s)));
}
static inline void rmb_swr(R5900Mem m, uint32_t b, int32_t o, uint64_t v) {
    unsigned s = (b + (uint32_t)o) & 3; uint32_t mem = rmb_lw(m, b, o - (int32_t)s);
    uint32_t keep = s == 0 ? 0 : (0xffffffffu >> (32 - 8 * s));
    rmb_sw(m, b, o - (int32_t)s, (mem & keep) | ((uint32_t)v << (8 * s)));
}
static inline void rmb_sdl(R5900Mem m, uint32_t b, int32_t o, uint64_t v) {
    unsigned s = (b + (uint32_t)o) & 7; uint64_t mem = rmb_ld(m, b, o - (int32_t)s);
    uint64_t keep = s == 7 ? 0 : (0xffffffffffffff00ull << (8 * s));
    rmb_sd(m, b, o - (int32_t)s, (mem & keep) | (v >> (56 - 8 * s)));
}
static inline void rmb_sdr(R5900Mem m, uint32_t b, int32_t o, uint64_t v) {
    unsigned s = (b + (uint32_t)o) & 7; uint64_t mem = rmb_ld(m, b, o - (int32_t)s);
    uint64_t keep = s == 0 ? 0 : (0xffffffffffffffffull >> (64 - 8 * s));
    rmb_sd(m, b, o - (int32_t)s, (mem & keep) | (v << (8 * s)));
}
static inline void rmb_lq(R5900Mem m, uint32_t b, int32_t o, R5900Reg128 *d) { memcpy(__builtin_assume_aligned(d, 16), RMB_AL(m, b, o - (int32_t)((b + (uint32_t)o) & 15), 16), 16); }
static inline void rmb_sq(R5900Mem m, uint32_t b, int32_t o, const R5900Reg128 *s) { memcpy(RMB_AL(m, b, o - (int32_t)((b + (uint32_t)o) & 15), 16), __builtin_assume_aligned(s, 16), 16); }

/* back-end v5 lever ufuse: an adjacent LDL/LDR (LWL/LWR, SDL/SDR, SWL/SWR) pair of
   one register and base covering the 8 (4) bytes at base + offset is one unaligned access (lwl/lwr, swl/swr on the
   Allegrex). Every byte has the address the two merges use; RMB_CHECK also checks the last byte. Only ABI-mode
   functions with maskbase use them (the rt_ / rm_ forms exist for the generic rewrite of the back-end). */
static inline uint64_t rt_uld(R5900Ctx *c, uint32_t a) { uint64_t v; memcpy(&v, RT_PTR(c, a), 8); return v; }
static inline uint32_t rt_ulw(R5900Ctx *c, uint32_t a) { uint32_t v; memcpy(&v, RT_PTR(c, a), 4); return v; }
static inline void rt_usd(R5900Ctx *c, uint32_t a, uint64_t v) { memcpy(RT_PTR(c, a), &v, 8); }
static inline void rt_usw(R5900Ctx *c, uint32_t a, uint64_t v) { uint32_t w = (uint32_t)v; memcpy(RT_PTR(c, a), &w, 4); }
static inline uint64_t rm_uld(R5900Mem m, uint32_t a) { uint64_t v; memcpy(&v, RM_PTR(m, a), 8); return v; }
static inline uint32_t rm_ulw(R5900Mem m, uint32_t a) { uint32_t v; memcpy(&v, RM_PTR(m, a), 4); return v; }
static inline void rm_usd(R5900Mem m, uint32_t a, uint64_t v) { memcpy(RM_PTR(m, a), &v, 8); }
static inline void rm_usw(R5900Mem m, uint32_t a, uint64_t v) { uint32_t w = (uint32_t)v; memcpy(RM_PTR(m, a), &w, 4); }
#ifdef RMB_CHECK
#define RMB_UCHK(m, b, o, n) ((void)RMB_PTR(m, b, (o) + (n) - 1))
#else
#define RMB_UCHK(m, b, o, n) ((void)0)
#endif
static inline uint64_t rmb_uld(R5900Mem m, uint32_t b, int32_t o) { uint64_t v; RMB_UCHK(m, b, o, 8); memcpy(&v, RMB_PTR(m, b, o), 8); return v; }
static inline uint32_t rmb_ulw(R5900Mem m, uint32_t b, int32_t o) { uint32_t v; RMB_UCHK(m, b, o, 4); memcpy(&v, RMB_PTR(m, b, o), 4); return v; }
static inline void rmb_usd(R5900Mem m, uint32_t b, int32_t o, uint64_t v) { RMB_UCHK(m, b, o, 8); memcpy(RMB_PTR(m, b, o), &v, 8); }
static inline void rmb_usw(R5900Mem m, uint32_t b, int32_t o, uint64_t v) { uint32_t w = (uint32_t)v; RMB_UCHK(m, b, o, 4); memcpy(RMB_PTR(m, b, o), &w, 4); }

/* ------------------------------------------------------------------ provided by the generated code / the harness */
void rc_dispatch(R5900Ctx *c, uint32_t guest_addr);        /* JALR / indirect jump: call the function at guest_addr */
typedef void (*rc_fn)(R5900Ctx *c);
typedef rc_fn (*rc_lookup_fn)(uint32_t guest_addr);
#define RC_MAX_OVERLAYS 16
/* overlays (r5900_rt.c): several translated overlays share the guest window [rc_overlay_lo, rc_overlay_hi); the
   dispatcher sends guest addresses inside it to the lookup of the overlay currently loaded (0 = none) */
extern int rc_overlay_current;
extern uint32_t rc_overlay_lo, rc_overlay_hi;
extern rc_lookup_fn rc_overlay_lookup[RC_MAX_OVERLAYS];
/* EE kernel calls (r5900_rt.c): SYSCALL with the number in v1 (negative: the interrupt-context variant, same slot)
   runs rt_syscall_table[|number|]; an empty slot is rt_fatal("syscall") */
typedef void (*rt_syscall_fn)(R5900Ctx *c);
#define RT_NSYSCALLS 256
extern rt_syscall_fn rt_syscall_table[RT_NSYSCALLS];
void rt_syscall(R5900Ctx *c, uint32_t pc);
/* unrecoverable guest event: what = "break", "syscall", "trap", "dispatch" (unknown target), "switch" (jump-table
   index out of range), "leave" (control left a function to a non-function address), COP0 mnemonics.
   The harness decides (abort, report, longjmp); generated code returns from the current function if it comes back. */
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value);
/* VU0 micro subroutines (r5900_rt.c): VCALLMS/VCALLMSR call rt_vu0_callms(c, pc of the instruction, start address in
   VU0 micro memory in bytes = imm15 * 8 resp. CMSAR0 * 8). Micro mode is not emulated: an HLE installs
   rt_vu0_callms_hook (it runs the subroutine to completion on c->vu0, so the .I interlock variants of QMFC2/CFC2 that
   follow see its results); without a hook it is rt_fatal("vcallms", pc, address). */
typedef void (*rt_vu0_callms_fn)(R5900Ctx *c, uint32_t pc, uint32_t addr);
extern rt_vu0_callms_fn rt_vu0_callms_hook;
void rt_vu0_callms(R5900Ctx *c, uint32_t pc, uint32_t addr);
/* COP0 condition CPCOND0 (BC0F/BC0T/BC0FL/BC0TL; Sony libs poll it for DMA completion: PCSX2 COP0.cpp CPCOND0 =
   ((D_STAT.CIS | ~D_PCR.CPC) & 0x3FF) == 0x3FF). DMA is HLE: rt_cpcond0_hook, default 1 = "transfers complete" so the
   polling loops terminate. */
typedef int (*rt_cpcond0_fn)(R5900Ctx *c);
extern rt_cpcond0_fn rt_cpcond0_hook;
int rt_cpcond0(R5900Ctx *c);

/* callee-saved context copies (s0-s7, fp: 128 bits; f20-f31) kept across a call by an exact function of an ABI-mode
   program (back-end abi_exempt): the ABI-mode callee keeps those registers in C locals and may leave other values in
   their context copies, while the exact caller's context must stay what the EE ABI preserves (added 2026-10-03) */
typedef struct { R5900Reg128 r[9]; uint32_t f[12]; } R5900CSave;
static inline void rt_cs_save(const R5900Ctx *c, R5900CSave *s) {
    memcpy(&s->r[0], &c->r[16], 8 * sizeof(R5900Reg128)); s->r[8] = c->r[30];
    for (int i = 0; i < 12; i++) s->f[i] = c->fpr[20 + i].u;
}
static inline void rt_cs_restore(R5900Ctx *c, const R5900CSave *s) {
    memcpy(&c->r[16], &s->r[0], 8 * sizeof(R5900Reg128)); c->r[30] = s->r[8];
    for (int i = 0; i < 12; i++) c->fpr[20 + i].u = s->f[i];
}

/* ------------------------------------------------------------------ integer helpers */
#define SX32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static inline void rt_mult(R5900Ctx *c, int p, int64_t a, int64_t b) {         /* p = pipeline 0/1 */
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo.d[p] = SX32(r); c->hi.d[p] = SX32(r >> 32);
}
static inline void rt_multu(R5900Ctx *c, int p, uint64_t a, uint64_t b) {
    uint64_t r = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    c->lo.d[p] = SX32(r); c->hi.d[p] = SX32(r >> 32);
}
static inline void rt_div(R5900Ctx *c, int p, uint64_t a, uint64_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if ((uint32_t)x == 0x80000000u && y == -1) { c->lo.d[p] = SX32(0x80000000u); c->hi.d[p] = 0; }
    else if (y != 0) { c->lo.d[p] = SX32(x / y); c->hi.d[p] = SX32(x % y); }
    else { c->lo.d[p] = x < 0 ? 1 : (uint64_t)-1; c->hi.d[p] = SX32(x); }
}
static inline void rt_divu(R5900Ctx *c, int p, uint64_t a, uint64_t b) {
    uint32_t x = (uint32_t)a, y = (uint32_t)b;
    if (y != 0) { c->lo.d[p] = SX32(x / y); c->hi.d[p] = SX32(x % y); }
    else { c->lo.d[p] = (uint64_t)-1; c->hi.d[p] = SX32(x); }
}
static inline void rt_madd(R5900Ctx *c, int p, uint64_t a, uint64_t b) {
    int64_t acc = (int64_t)((uint64_t)(uint32_t)c->lo.d[p] | ((uint64_t)(uint32_t)c->hi.d[p] << 32));
    int64_t r = acc + (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo.d[p] = SX32(r); c->hi.d[p] = SX32((uint64_t)r >> 32);
}
static inline void rt_maddu(R5900Ctx *c, int p, uint64_t a, uint64_t b) {
    uint64_t acc = (uint64_t)(uint32_t)c->lo.d[p] | ((uint64_t)(uint32_t)c->hi.d[p] << 32);
    uint64_t r = acc + (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    c->lo.d[p] = SX32(r); c->hi.d[p] = SX32(r >> 32);
}
/* back-end v5 (lever "hilo"): the same operations on HI/LO held in C locals (lo = LO of the pipeline, hi = HI) */
static inline void rl_mult(uint64_t *lo, uint64_t *hi, int64_t a, int64_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    *lo = SX32(r); *hi = SX32(r >> 32);
}
static inline void rl_multu(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b) {
    uint64_t r = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    *lo = SX32(r); *hi = SX32(r >> 32);
}
static inline void rl_div(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if ((uint32_t)x == 0x80000000u && y == -1) { *lo = SX32(0x80000000u); *hi = 0; }
    else if (y != 0) { *lo = SX32(x / y); *hi = SX32(x % y); }
    else { *lo = x < 0 ? 1 : (uint64_t)-1; *hi = SX32(x); }
}
static inline void rl_divu(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b) {
    uint32_t x = (uint32_t)a, y = (uint32_t)b;
    if (y != 0) { *lo = SX32(x / y); *hi = SX32(x % y); }
    else { *lo = (uint64_t)-1; *hi = SX32(x); }
}
static inline void rl_madd(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b) {
    int64_t acc = (int64_t)((uint64_t)(uint32_t)*lo | ((uint64_t)(uint32_t)*hi << 32));
    int64_t r = acc + (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    *lo = SX32(r); *hi = SX32((uint64_t)r >> 32);
}
static inline void rl_maddu(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b) {
    uint64_t acc = (uint64_t)(uint32_t)*lo | ((uint64_t)(uint32_t)*hi << 32);
    uint64_t r = acc + (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    *lo = SX32(r); *hi = SX32(r >> 32);
}

/* ------------------------------------------------------------------ FPU */
#define F_FMAX 0x7F7FFFFFu
static inline uint32_t f_in(uint32_t x) {
    uint32_t e = x & 0x7F800000u;
    if (e == 0) return x & 0x80000000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | F_FMAX;
    return x;
}
static inline uint32_t f_out(uint32_t x) {
    uint32_t e = x & 0x7F800000u;
    if (e == 0x7F800000u) return (x & 0x80000000u) | F_FMAX;
    if (e == 0) return x & 0x80000000u;
    return x;
}
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float F(uint32_t raw) { return u2f(f_in(raw)); }

static inline uint32_t fpu_add(uint32_t a, uint32_t b) { return f_out(f2u(F(a) + F(b))); }
static inline uint32_t fpu_sub(uint32_t a, uint32_t b) { return f_out(f2u(F(a) - F(b))); }
static inline uint32_t fpu_mul(uint32_t a, uint32_t b) { return f_out(f2u(F(a) * F(b))); }
static inline uint32_t fpu_div(uint32_t a, uint32_t b) {
    if ((b & 0x7F800000u) == 0) return ((a ^ b) & 0x80000000u) | F_FMAX;
    return f_out(f2u(F(a) / F(b)));
}

static inline uint32_t fpu_sqrt(uint32_t t) {
    if ((t & 0x7F800000u) == 0) return t & 0x80000000u;
    return f2u(__builtin_sqrtf(u2f(f_in(t) & 0x7FFFFFFFu)));
}
static inline uint32_t fpu_sqrt_c(uint32_t t) {     /* t known clean (finite, normal or zero) */
    if ((t & 0x7F800000u) == 0) return t & 0x80000000u;
    return f2u(__builtin_sqrtf(u2f(t & 0x7FFFFFFFu)));
}
static inline uint32_t fpu_rsqrt(uint32_t s, uint32_t t) {
    if ((t & 0x7F800000u) == 0) return (t & 0x80000000u) | F_FMAX;
    float r = __builtin_sqrtf(u2f(f_in(t) & 0x7FFFFFFFu));
    return f_out(f2u(F(s) / u2f(f_in(f2u(r)))));
}
static inline uint32_t fpu_max(uint32_t a, uint32_t b) {
    return ((int32_t)a < 0 && (int32_t)b < 0) ? ((int32_t)a < (int32_t)b ? a : b) : ((int32_t)a > (int32_t)b ? a : b);
}
static inline uint32_t fpu_min(uint32_t a, uint32_t b) {
    return ((int32_t)a < 0 && (int32_t)b < 0) ? ((int32_t)a > (int32_t)b ? a : b) : ((int32_t)a < (int32_t)b ? a : b);
}
static inline uint32_t fpu_madd(uint32_t acc, uint32_t a, uint32_t b) {
    uint32_t p = f2u(F(a) * F(b));
    return f_out(f2u(F(acc) + F(p)));
}
static inline uint32_t fpu_msub(uint32_t acc, uint32_t a, uint32_t b) {
    uint32_t p = f2u(F(a) * F(b));
    return f_out(f2u(F(acc) - F(p)));
}
static inline uint32_t fpu_madda(uint32_t acc, uint32_t a, uint32_t b) {   /* ACC += product (ACC unclamped) */
    return f_out(f2u(u2f(acc) + F(a) * F(b)));
}
static inline uint32_t fpu_msuba(uint32_t acc, uint32_t a, uint32_t b) {
    return f_out(f2u(u2f(acc) - F(a) * F(b)));
}
static inline uint32_t fpu_cvt_w(uint32_t a) {
    if ((a & 0x7F800000u) <= 0x4E800000u) return (uint32_t)(int32_t)u2f(a);
    return (a & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
}
static inline uint32_t fpu_cvt_s(uint32_t a) { return f2u((float)(int32_t)a); }
#define FCR31_C 0x00800000u
static inline void fpu_cmp(R5900Ctx *c, int cond) { if (cond) c->fcr31 |= FCR31_C; else c->fcr31 &= ~FCR31_C; }

/* back-end v5 (lever "clampmin2"): the same input clamp as f_in (exponent 0 -> +-0,
   exponent 255 -> +-0x7F7FFFFF, else unchanged), branchless: x + (min(|x|, FMAX) - |x|), with the magnitude 0 for
   exponent 0 (psp-gcc: ext, and, min, movz, subu, addu). Equal to f_in for all 2^32 inputs (tests/test_v5_clamp.py). */
static inline uint32_t f_in5(uint32_t x) {
    int32_t a = (int32_t)(x & 0x7FFFFFFFu);
    int32_t m = a < (int32_t)F_FMAX ? a : (int32_t)F_FMAX;
    m = ((x >> 23) & 0xFFu) ? m : 0;
    return x + (uint32_t)(m - a);
}
static inline float F5(uint32_t raw) { return u2f(f_in5(raw)); }
static inline uint32_t fpu_sqrt5(uint32_t t) {
    if ((t & 0x7F800000u) == 0) return t & 0x80000000u;
    return f2u(__builtin_sqrtf(u2f(f_in5(t) & 0x7FFFFFFFu)));
}
static inline uint32_t fpu_rsqrt5(uint32_t s, uint32_t t) {
    if ((t & 0x7F800000u) == 0) return (t & 0x80000000u) | F_FMAX;
    float r = __builtin_sqrtf(u2f(f_in5(t) & 0x7FFFFFFFu));
    return f_out(f2u(F5(s) / u2f(f_in5(f2u(r)))));
}
static inline uint32_t fpu_madda5(uint32_t acc, uint32_t a, uint32_t b) { return f_out(f2u(u2f(acc) + F5(a) * F5(b))); }
static inline uint32_t fpu_msuba5(uint32_t acc, uint32_t a, uint32_t b) { return f_out(f2u(u2f(acc) - F5(a) * F5(b))); }
static inline uint64_t fpu_cfc1(R5900Ctx *c, int fs) {
    if (fs == 31) return SX32(c->fcr31);
    if (fs == 0) return 0x2E00;
    return 0;
}

/* ------------------------------------------------------------------ MMI (128-bit, lanes) */
static inline R5900Reg128 R(const R5900Ctx *c, int n) { return c->r[n]; }
static inline uint32_t sat_u32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
static inline int32_t sat_s32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFLL - 1 ? (int32_t)0x80000000 : (int32_t)v; }

static inline R5900Reg128 mmi_paddw(R5900Reg128 a, R5900Reg128 b) { for (int i = 0; i < 4; i++) a.w[i] += b.w[i]; return a; }
static inline R5900Reg128 mmi_psubw(R5900Reg128 a, R5900Reg128 b) { for (int i = 0; i < 4; i++) a.w[i] -= b.w[i]; return a; }
static inline R5900Reg128 mmi_psubb(R5900Reg128 a, R5900Reg128 b) { for (int i = 0; i < 16; i++) a.b[i] -= b.b[i]; return a; }
static inline R5900Reg128 mmi_paddub(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 16; i++) { unsigned v = a.b[i] + b.b[i]; a.b[i] = v > 255 ? 255 : v; } return a;
}
static inline R5900Reg128 mmi_padduw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) a.w[i] = sat_u32((uint64_t)a.w[i] + b.w[i]);

    return a;
}
static inline R5900Reg128 mmi_psubsw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) a.sw[i] = sat_s32((int64_t)a.sw[i] - b.sw[i]);

    return a;
}
static inline R5900Reg128 mmi_pand(R5900Reg128 a, R5900Reg128 b) { a.d[0] &= b.d[0]; a.d[1] &= b.d[1]; return a; }
static inline R5900Reg128 mmi_por(R5900Reg128 a, R5900Reg128 b) { a.d[0] |= b.d[0]; a.d[1] |= b.d[1]; return a; }
static inline R5900Reg128 mmi_pxor(R5900Reg128 a, R5900Reg128 b) { a.d[0] ^= b.d[0]; a.d[1] ^= b.d[1]; return a; }
static inline R5900Reg128 mmi_pnor(R5900Reg128 a, R5900Reg128 b) { a.d[0] = ~(a.d[0] | b.d[0]); a.d[1] = ~(a.d[1] | b.d[1]); return a; }
static inline R5900Reg128 mmi_pcpyld(R5900Reg128 s, R5900Reg128 t) { R5900Reg128 r; r.d[0] = t.d[0]; r.d[1] = s.d[0]; return r; }
static inline R5900Reg128 mmi_pcpyud(R5900Reg128 s, R5900Reg128 t) { R5900Reg128 r; r.d[0] = s.d[1]; r.d[1] = t.d[1]; return r; }
static inline R5900Reg128 mmi_pextlw(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; r.w[0] = t.w[0]; r.w[1] = s.w[0]; r.w[2] = t.w[1]; r.w[3] = s.w[1]; return r;
}
static inline R5900Reg128 mmi_pextuw(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; r.w[0] = t.w[2]; r.w[1] = s.w[2]; r.w[2] = t.w[3]; r.w[3] = s.w[3]; return r;
}
static inline R5900Reg128 mmi_pextlh(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 4; i++) { r.h[2 * i] = t.h[i]; r.h[2 * i + 1] = s.h[i]; } return r;
}
static inline R5900Reg128 mmi_pextlb(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 8; i++) { r.b[2 * i] = t.b[i]; r.b[2 * i + 1] = s.b[i]; } return r;
}
static inline R5900Reg128 mmi_ppacw(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; r.w[0] = t.w[0]; r.w[1] = t.w[2]; r.w[2] = s.w[0]; r.w[3] = s.w[2]; return r;
}
static inline R5900Reg128 mmi_ppac5(R5900Reg128 t) {
    R5900Reg128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t v = t.w[i];
        r.w[i] = ((v >> 3) & 0x1F) | (((v >> 11) & 0x1F) << 5) | (((v >> 19) & 0x1F) << 10) | ((v >> 16) & 0x8000);
    }
    return r;
}
static inline R5900Reg128 mmi_pexew(R5900Reg128 t) { R5900Reg128 r = t; r.w[0] = t.w[2]; r.w[2] = t.w[0]; return r; }
static inline R5900Reg128 mmi_prot3w(R5900Reg128 t) { R5900Reg128 r = t; r.w[0] = t.w[1]; r.w[1] = t.w[2]; r.w[2] = t.w[0]; return r; }
static inline R5900Reg128 mmi_pcpyh(R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 4; i++) { r.h[i] = t.h[0]; r.h[4 + i] = t.h[4]; } return r;
}
static inline R5900Reg128 mmi_pinteh(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 4; i++) { r.h[2 * i] = t.h[2 * i]; r.h[2 * i + 1] = s.h[2 * i]; } return r;
}
static inline R5900Reg128 mmi_psllw(R5900Reg128 t, int sa) { for (int i = 0; i < 4; i++) t.w[i] <<= sa; return t; }
static inline R5900Reg128 mmi_psrlw(R5900Reg128 t, int sa) { for (int i = 0; i < 4; i++) t.w[i] >>= sa; return t; }
static inline R5900Reg128 mmi_pcgtw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) a.w[i] = a.sw[i] > b.sw[i] ? 0xFFFFFFFFu : 0;

    return a;
}
static inline R5900Reg128 mmi_pceqw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) a.w[i] = a.w[i] == b.w[i] ? 0xFFFFFFFFu : 0;

    return a;
}
static inline R5900Reg128 mmi_pmaxw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) if (b.sw[i] > a.sw[i]) a.sw[i] = b.sw[i];

    return a;
}
static inline R5900Reg128 mmi_pminw(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 4; i++) if (b.sw[i] < a.sw[i]) a.sw[i] = b.sw[i];

    return a;
}
static inline uint32_t plzcw1(int32_t v) {           /* leading bits equal to the sign bit, minus one */
    uint32_t x = (uint32_t)(v < 0 ? ~v : v);
    uint32_t n = 0;
    for (int i = 31; i >= 0 && !((x >> i) & 1); i--) n++;
    return n - 1;
}
static inline R5900Reg128 mmi_plzcw(R5900Reg128 old, R5900Reg128 s) {
    old.w[0] = plzcw1(s.sw[0]); old.w[1] = plzcw1(s.sw[1]); return old;
}
static inline R5900Reg128 mmi_qfsrv(R5900Reg128 s, R5900Reg128 t, uint32_t shift_bits) {
    /* 256-bit {s:t} >> sa, low 128 bits */
    unsigned n = shift_bits & 127;
    uint64_t q[4] = {t.d[0], t.d[1], s.d[0], s.d[1]};
    R5900Reg128 r;
    for (int i = 0; i < 2; i++) {
        unsigned bit = n + 64 * i, k = bit / 64, o = bit % 64;
        uint64_t lo = q[k], hi = k + 1 < 4 ? q[k + 1] : 0;
        r.d[i] = o ? (lo >> o) | (hi << (64 - o)) : lo;
    }
    return r;
}
/* halfword/byte MMI (as Sony's libipu uses them), PCSX2 MMI.cpp semantics: wrap-around adds, signed compares, shift
   count & 15 */
static inline R5900Reg128 mmi_paddh(R5900Reg128 a, R5900Reg128 b) { for (int i = 0; i < 8; i++) a.h[i] += b.h[i]; return a; }
static inline R5900Reg128 mmi_pcgth(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 8; i++) a.h[i] = a.sh[i] > b.sh[i] ? 0xFFFFu : 0;
    return a;
}
static inline R5900Reg128 mmi_pmaxh(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 8; i++) if (b.sh[i] >= a.sh[i]) a.sh[i] = b.sh[i];
    return a;
}
static inline R5900Reg128 mmi_pminh(R5900Reg128 a, R5900Reg128 b) {
    for (int i = 0; i < 8; i++) if (b.sh[i] <= a.sh[i]) a.sh[i] = b.sh[i];
    return a;
}
static inline R5900Reg128 mmi_ppacb(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 8; i++) { r.b[i] = t.b[2 * i]; r.b[8 + i] = s.b[2 * i]; } return r;
}
static inline R5900Reg128 mmi_pextub(R5900Reg128 s, R5900Reg128 t) {
    R5900Reg128 r; for (int i = 0; i < 8; i++) { r.b[2 * i] = t.b[8 + i]; r.b[2 * i + 1] = s.b[8 + i]; } return r;
}
static inline R5900Reg128 mmi_psllh(R5900Reg128 t, int sa) { for (int i = 0; i < 8; i++) t.h[i] = (uint16_t)(t.h[i] << (sa & 15)); return t; }
static inline R5900Reg128 mmi_psrlh(R5900Reg128 t, int sa) { for (int i = 0; i < 8; i++) t.h[i] = (uint16_t)(t.h[i] >> (sa & 15)); return t; }
static inline R5900Reg128 mmi_psrah(R5900Reg128 t, int sa) { for (int i = 0; i < 8; i++) t.sh[i] = (int16_t)(t.sh[i] >> (sa & 15)); return t; }

/* ------------------------------------------------------------------ VU0 macro mode (COP2)
 * Semantics: PCSX2 v2.6.3's EE-interpreter model of the VU0 macro instructions (VUops.cpp: vuDouble operands,
 * VUflags.cpp VU_MAC_UPDATE results, clamp mode "normal" = vu0Overflow), which is bit-identical to PCSX2's recompiler
 * (microVU0 macro mode) for every operand whose exponent is not 255; where the two PCSX2 paths differ on ordinary
 * values (CFC2/CTC2 of VI01-VI15 = 16 bits, FBRST, VRNEXT to VF00) the model follows the recompiler:
 *   operands: exponent 0 -> +-0, exponent 255 -> +-0x7F7FFFFF (vu0_clamp = the FPU's f_in mapping);
 *   arithmetic: IEEE single precision with the host FPU in round-toward-zero + flush-to-zero (as the FPU model);
 *   MADD/MSUB: the product is rounded, then ACC +- product is rounded (no fused multiply-add);
 *   results: exponent 255 -> +-0x7F7FFFFF, exponent 0 -> +-0 (vu0_clamp; dead under round-toward-zero + flush-to-zero);
 *   lanes outside the dest field are untouched; all lanes are computed before any is written (fd == fs/ft is fine);
 *   VF00 reads as (0, 0, 0, 1.0), writes to VF00 are dropped; VI00 reads 0.
 * Not modelled: MAC/status/clip flags (CFC2 returns what CTC2 stored), FDIV/EFU timing,
 * micro mode (VCALLMS: rt_vu0_callms hook). Generated code passes constant arguments: the always_inline helpers fold
 * to straight-line code. */
#define VU0_ACC 32                /* fmac destination: ACC */
#define VU0_IREG 32               /* fmac second operand: the I register */
#define VU0_QREG 33               /* fmac second operand: the Q register */
#define VU0_ADD 0
#define VU0_SUB 1
#define VU0_MUL 2
#define VU0_MADD 3
#define VU0_MSUB 4
#define VU0_INLINE static inline __attribute__((always_inline))

/* lane i (0 = x .. 3 = w) of VF[n] (VF00 constant), or the I/Q register */
VU0_INLINE uint32_t vu0_lane(const R5900Ctx *c, int n, int i)
{
    if (n == 0) return i == 3 ? 0x3F800000u : 0u;
    if (n == VU0_IREG) return c->vu0.vi[21];
    if (n == VU0_QREG) return c->vu0.vi[22];
    return c->vu0.vf[n].w[i];
}
VU0_INLINE R5900Reg128 vu0_vf(const R5900Ctx *c, int n)
{
    if (n == 0) { R5900Reg128 r; r.d[0] = 0; r.d[1] = 0x3F80000000000000ull; return r; }
    return c->vu0.vf[n];
}
/* operand / result clamp: exponent 255 -> +-0x7F7FFFFF, exponent 0 -> +-0 (= f_in = f_out), branchless on Allegrex
   (ext, min, sra, movz, ins, or): no branches for GCC to tail-duplicate */
VU0_INLINE uint32_t vu0_clamp(uint32_t x)
{
    int32_t m = (int32_t)(x & 0x7FFFFFFFu);
    m = m < (int32_t)F_FMAX ? m : (int32_t)F_FMAX;
    m = (m >> 23) == 0 ? 0 : m;
    return (x & 0x80000000u) | (uint32_t)m;
}
/* one FMAC lane: s op t (MADD/MSUB: ACC lane a +- s * t). cl: operands known clean (finite, not denormal: no input
   clamp; bit 0 s, bit 1 t, bit 2 a - back-end v3+); fast: no output clamp (v3+) */
VU0_INLINE uint32_t vu0_fmac1(int op, uint32_t a, uint32_t s, uint32_t t, unsigned cl, int fast)
{
    float x = u2f((cl & 1) ? s : vu0_clamp(s)), y = u2f((cl & 2) ? t : vu0_clamp(t)), r;
    if (op == VU0_ADD) r = x + y;
    else if (op == VU0_SUB) r = x - y;
    else if (op == VU0_MUL) r = x * y;
    else {
        float p = x * y, z = u2f((cl & 4) ? a : vu0_clamp(a));
        r = op == VU0_MADD ? z + p : z - p;
    }
    return fast ? f2u(r) : vu0_clamp(f2u(r));
}
/* FMAC instruction: d = VF index or VU0_ACC; dest = field mask (8 x, 4 y, 2 z, 1 w); s = VF[fs]; t = VF[ft],
   VU0_IREG or VU0_QREG; bc = broadcast field of t (0..3) or -1 (lane-wise) */
VU0_INLINE void vu0_fmac(R5900Ctx *c, int op, int d, unsigned dest, int s, int t, int bc, unsigned cl, int fast)
{
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    const uint32_t *a = c->vu0.acc.w;
    if (dest & 8) r0 = vu0_fmac1(op, a[0], vu0_lane(c, s, 0), vu0_lane(c, t, bc < 0 ? 0 : bc), cl, fast);
    if (dest & 4) r1 = vu0_fmac1(op, a[1], vu0_lane(c, s, 1), vu0_lane(c, t, bc < 0 ? 1 : bc), cl, fast);
    if (dest & 2) r2 = vu0_fmac1(op, a[2], vu0_lane(c, s, 2), vu0_lane(c, t, bc < 0 ? 2 : bc), cl, fast);
    if (dest & 1) r3 = vu0_fmac1(op, a[3], vu0_lane(c, s, 3), vu0_lane(c, t, bc < 0 ? 3 : bc), cl, fast);
    if (d == 0) return;
    uint32_t *w = d == VU0_ACC ? c->vu0.acc.w : c->vu0.vf[d].w;
    if (dest & 8) w[0] = r0;
    if (dest & 4) w[1] = r1;
    if (dest & 2) w[2] = r2;
    if (dest & 1) w[3] = r3;
}
/* VMOVE (rot = 0) / VMR32 (rot = 1): VF[t].i = VF[s].(i + rot) & 3, raw bits; VABS (abs = 1): sign cleared */
VU0_INLINE void vu0_move(R5900Ctx *c, unsigned dest, int t, int s, int rot, int abs)
{
    uint32_t m = abs ? 0x7FFFFFFFu : 0xFFFFFFFFu;
    uint32_t r0 = vu0_lane(c, s, rot) & m, r1 = vu0_lane(c, s, (1 + rot) & 3) & m;
    uint32_t r2 = vu0_lane(c, s, (2 + rot) & 3) & m, r3 = vu0_lane(c, s, (3 + rot) & 3) & m;
    if (t == 0) return;
    if (dest & 8) c->vu0.vf[t].w[0] = r0;
    if (dest & 4) c->vu0.vf[t].w[1] = r1;
    if (dest & 2) c->vu0.vf[t].w[2] = r2;
    if (dest & 1) c->vu0.vf[t].w[3] = r3;
}
/* VRGET/VRNEXT destination: the dest lanes of VF[t] = v */
VU0_INLINE void vu0_fill(R5900Ctx *c, unsigned dest, int t, uint32_t v)
{
    if (t == 0) return;
    if (dest & 8) c->vu0.vf[t].w[0] = v;
    if (dest & 4) c->vu0.vf[t].w[1] = v;
    if (dest & 2) c->vu0.vf[t].w[2] = v;
    if (dest & 1) c->vu0.vf[t].w[3] = v;
}
/* R register (CCR[2,20]): 23-bit LFSR, held as 1.0 + mantissa; PCSX2 VUops.cpp AdvanceLFSR / microVU mVU_RNEXT */
VU0_INLINE void vu0_rinit(R5900Ctx *c, uint32_t v) { c->vu0.vi[20] = 0x3F800000u | (v & 0x7FFFFFu); }
VU0_INLINE void vu0_rxor(R5900Ctx *c, uint32_t v) { c->vu0.vi[20] = 0x3F800000u | ((c->vu0.vi[20] ^ v) & 0x7FFFFFu); }
VU0_INLINE uint32_t vu0_rnext(R5900Ctx *c)
{
    uint32_t r = c->vu0.vi[20];
    r = (((r << 1) ^ ((r >> 4) & 1) ^ ((r >> 22) & 1)) & 0x7FFFFFu) | 0x3F800000u;
    c->vu0.vi[20] = r;
    return r;
}
/* CFC2 (PCSX2 microVU_Macro.inl recCFC2): VI01-VI15 zero-extended 16 bits, R its 23 bits, the
   other control registers sign-extended 32 bits */
VU0_INLINE uint64_t vu0_cfc2(const R5900Ctx *c, int n)
{
    if (n == 0) return 0;
    if (n < 16) return c->vu0.vi[n] & 0xFFFFu;
    if (n == 20) return c->vu0.vi[20] & 0x7FFFFFu;
    return SX32(c->vu0.vi[n]);
}
/* CTC2 (recCTC2): VI00, MAC, TPC, VPU-STAT read-only; VI01-VI15 take 16 bits; status only its sticky bits 6-11;
   R = 1.0 + 23 bits; FBRST keeps bits 0x0C0C, RS0 (bit 1) / RS1 (bit 9) stop VU0 / VU1 (VPU-STAT bits 0-7 / 8-15).
   CMSAR1 (starts a VU1 micro program) is not handled here (the back-end rejects it). */
VU0_INLINE void vu0_ctc2(R5900Ctx *c, int n, uint32_t v)
{
    if (n == 0 || n == 17 || n == 26 || n == 29) return;
    if (n < 16) c->vu0.vi[n] = v & 0xFFFFu;
    else if (n == 16) c->vu0.vi[16] = (v & 0xFC0u) | (c->vu0.vi[16] & 0x3Fu);
    else if (n == 20) c->vu0.vi[20] = 0x3F800000u | (v & 0x7FFFFFu);
    else if (n == 28) {
        if (v & 2) c->vu0.vi[29] &= ~0xFFu;
        if (v & 0x200) c->vu0.vi[29] &= ~0xFF00u;
        c->vu0.vi[28] = v & 0x0C0Cu;
    } else c->vu0.vi[n] = v;
}
#endif
