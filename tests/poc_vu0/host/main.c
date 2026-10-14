/* VU0 oracle host harness: runs the recompiled test functions over the guest's input vectors (same order, one
 * R5900Ctx for the whole run like the guest's single EE) and writes the output blocks in the guest's layout.
 * usage: vu0_host vectors.bin results.bin */
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>
#include "r5900_rt.h"
#include "vu0_table.h"

void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value)
{
    (void)c;
    fprintf(stderr, "rt_fatal %s pc=%08x value=%08x\n", what, pc, value);
    exit(3);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s vectors.bin results.bin\n", argv[0]); return 2; }
    /* EE FPU / VU model: round toward zero, flush denormal results (inputs are pre-clamped by the runtime) */
    fesetround(FE_TOWARDZERO);
    _mm_setcsr(_mm_getcsr() | 0x8040);                  /* FTZ | DAZ */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n_in = (size_t)VU0_NTESTS * VU0_NVEC * VU0_IN, n_out = (size_t)VU0_NTESTS * VU0_NVEC * VU0_OUT;
    uint8_t *in = malloc(n_in), *out = calloc(1, n_out);
    if (fread(in, 1, n_in, f) != n_in) { fprintf(stderr, "short vectors file\n"); return 1; }
    fclose(f);
    static uint8_t ram[0x10000] __attribute__((aligned(16)));
    static R5900Ctx ctx;
    ctx.ram = ram;
    ctx.ram_mask = sizeof ram - 1;
    for (int k = 0; k < VU0_NTESTS; k++) {
        for (int v = 0; v < VU0_NVEC; v++) {
            size_t i = (size_t)k * VU0_NVEC + v;
            memcpy(ram + 0x1000, in + i * VU0_IN, VU0_IN);
            memset(ram + 0x2000, 0, VU0_OUT);
            ctx.r[4].d[0] = 0x1000;
            ctx.r[5].d[0] = 0x2000;
            ctx.r[29].d[0] = 0x8000;
            vu0_tests[k].fn(&ctx);
            memcpy(out + i * VU0_OUT, ram + 0x2000, VU0_OUT);
        }
    }
    f = fopen(argv[2], "wb");
    fwrite(out, 1, n_out, f);
    fclose(f);
    printf("ran %d tests x %d vectors\n", VU0_NTESTS, VU0_NVEC);
    return 0;
}
