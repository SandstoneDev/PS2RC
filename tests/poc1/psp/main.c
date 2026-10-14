/* PoC-1 PSP harness: runs the recompiled R5900 test functions on the PSP (Allegrex) over the PS2 input vectors and
 * compares every output block with PCSX2's (expected.bin). Files next to the EBOOT: vectors.bin, expected.bin.
 * Writes poc1_psp_results.bin (all outputs) and poc1_psp_report.txt; shows a summary on screen.
 * The only accepted difference is the documented shift-amount register representation (MFSA = 8 x PCSX2), checked
 * like the host test. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r5900_rt.h"
#include "poc1_table.h"

PSP_MODULE_INFO("PS2RC_POC1", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);

#define printf pspDebugScreenPrintf
#define BUILD_NUM 12
#ifndef POC1_VARIANT                 /* back-end variant of poc1_rc.c (tests/poc1/build_rc.py writes it to the table) */
#define POC1_VARIANT "?"
#endif

static char dir[256];

static void *load(const char *name, size_t want)
{
    char path[320];
    snprintf(path, sizeof path, "%s%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return NULL; }
    void *p = malloc(want);
    if (!p) { printf("out of memory for %s (%u)\n", name, (unsigned)want); fclose(f); return NULL; }
    size_t got = fread(p, 1, want, f);
    fclose(f);
    if (got != want) { printf("%s: short read %u/%u\n", name, (unsigned)got, (unsigned)want); return NULL; }
    return p;
}

static int qfsrv_rule(const uint8_t *x, const uint8_t *y)
{
    uint64_t a0, a1, b0, b1;
    if (memcmp(x, y, 16)) return 0;
    memcpy(&a0, x + 16, 8); memcpy(&a1, x + 24, 8); memcpy(&b0, y + 16, 8); memcpy(&b1, y + 24, 8);
    return a0 == a1 && b0 == b1 && a0 == b0 * 8;
}

int main(int argc, char **argv)
{
    pspDebugScreenInit();
    printf("ps2rc PoC-1 (build %d, back-end %s): recompiled R5900 code on Allegrex\n\n", BUILD_NUM, POC1_VARIANT);
    strncpy(dir, argc > 0 ? argv[0] : "", sizeof dir - 1);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    /* EE FPU model: round toward zero (RM=1), flush denormal results (FS, bit 24) */
    __asm__ volatile("ctc1 %0, $31" :: "r"(0x01000001));

    size_t nvec = (size_t)POC1_NTESTS * POC1_NVEC;
    uint8_t *in = load("vectors.bin", nvec * 64);
    uint8_t *exp = load("expected.bin", nvec * 32);
    uint8_t *out = malloc(POC1_NVEC * 32);
    if (!in || !exp || !out) goto wait;

    static uint8_t ram_buf[RMB_GUARD + 0x10000 + RMB_GUARD] __attribute__((aligned(64)));   /* v5s4: guards */
    uint8_t *ram = ram_buf + RMB_GUARD;
    static R5900Ctx ctx;
    ctx.ram = ram;
    ctx.ram_mask = 0x10000 - 1;
    char rpath[320];
    snprintf(rpath, sizeof rpath, "%spoc1_psp_results.bin", dir);
    FILE *rf = fopen(rpath, "wb");
    int total_bad = 0;
    char report[8192];
    int rlen = snprintf(report, sizeof report, "ps2rc poc1 build=%d variant=%s compiler=gcc-%s\n", BUILD_NUM,
                        POC1_VARIANT, __VERSION__);
    u64 t0, t1;
    sceRtcGetCurrentTick(&t0);
    for (int k = 0; k < POC1_NTESTS; k++) {
        int bad = 0;
        for (int v = 0; v < POC1_NVEC; v++) {
            size_t i = (size_t)k * POC1_NVEC + v;
            memcpy(ram + 0x1000, in + i * 64, 64);
            memset(ram + 0x2000, 0, 32);
            ctx.r[4].d[0] = 0x1000;
            ctx.r[5].d[0] = 0x2000;
            poc1_tests[k].fn(&ctx);
            memcpy(out + (size_t)v * 32, ram + 0x2000, 32);
            const uint8_t *x = ram + 0x2000, *y = exp + i * 32;
            if (memcmp(x, y, 32) && !(strcmp(poc1_tests[k].name, "mmi_qfsrv") == 0 && qfsrv_rule(x, y)))
                bad++;
        }
        if (rf) fwrite(out, 32, POC1_NVEC, rf);
        total_bad += bad;
        rlen += snprintf(report + rlen, sizeof report - rlen, "%-14s %s %d\n", poc1_tests[k].name, bad ? "FAIL" : "ok", bad);
    }
    sceRtcGetCurrentTick(&t1);
    if (rf) fclose(rf);
    rlen += snprintf(report + rlen, sizeof report - rlen, "RESULT %s: %d mismatching vectors of %u; %.1f ms\n",
                     total_bad ? "FAIL" : "PASS", total_bad, (unsigned)nvec, (double)(t1 - t0) / 1000.0);
    printf("%s", report);
    snprintf(rpath, sizeof rpath, "%spoc1_psp_report.txt", dir);
    FILE *f = fopen(rpath, "w");
    if (f) { fwrite(report, 1, rlen, f); fclose(f); }
wait:
    printf("\nPress X to exit.\n");
    SceCtrlData pad;
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CROSS) break;
        sceKernelDelayThread(50000);
    }
    sceKernelExitGame();
    return 0;
}
