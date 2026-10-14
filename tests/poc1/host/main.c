/* PoC-1 host harness: runs the recompiled R5900 test functions over the same input vectors as the PS2 guest and
 * writes the output blocks in the guest's layout (test-major, 32 bytes per vector).
 * usage: poc1_host vectors.bin results.bin */
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>
#include "r5900_rt.h"
#include "poc1_table.h"

/* back-end v5s4 (maskbase): the window has RMB_GUARD bytes around it, and this host build (-DRMB_CHECK) counts
   every access whose base + offset would leave the window under maskbase (it then takes the rm_* address) */
static unsigned rmb_bad;
void rmb_mismatch(uint32_t base, int32_t off)
{
    if (rmb_bad++ < 10) fprintf(stderr, "rmb_mismatch base=%08x off=%d\n", (unsigned)base, (int)off);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s vectors.bin results.bin\n", argv[0]); return 2; }
    /* EE FPU model: round toward zero, flush denormal results (inputs are pre-clamped by the runtime) */
    fesetround(FE_TOWARDZERO);
    _mm_setcsr(_mm_getcsr() | 0x8040);                  /* FTZ | DAZ */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n_in = (size_t)POC1_NTESTS * POC1_NVEC * 64, n_out = (size_t)POC1_NTESTS * POC1_NVEC * 32;
    uint8_t *in = malloc(n_in), *out = calloc(1, n_out);
    if (fread(in, 1, n_in, f) != n_in) { fprintf(stderr, "short vectors file\n"); return 1; }
    fclose(f);
    static uint8_t ram_buf[RMB_GUARD + 0x10000 + RMB_GUARD];
    uint8_t *ram = ram_buf + RMB_GUARD;
    R5900Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ram = ram;
    ctx.ram_mask = 0x10000 - 1;
    for (int k = 0; k < POC1_NTESTS; k++) {
        for (int v = 0; v < POC1_NVEC; v++) {
            size_t i = (size_t)k * POC1_NVEC + v;
            memcpy(ram + 0x1000, in + i * 64, 64);
            memset(ram + 0x2000, 0, 32);
            ctx.r[4].d[0] = 0x1000;
            ctx.r[5].d[0] = 0x2000;
            poc1_tests[k].fn(&ctx);
            memcpy(out + i * 32, ram + 0x2000, 32);
        }
    }
    f = fopen(argv[2], "wb");
    fwrite(out, 1, n_out, f);
    fclose(f);
    printf("ran %d tests x %d vectors, rmb_mismatch %u\n", POC1_NTESTS, POC1_NVEC, rmb_bad);
    return rmb_bad ? 3 : 0;
}
