/* VU0 oracle PSP harness: runs the recompiled VU0 test functions (tests/poc_vu0, back-end v4) on the PSP (Allegrex)
 * over the PS2 input vectors and compares every 128-byte output block with PCSX2's (expected.bin: PCSX2 interpreter
 * for the arithmetic tests, PCSX2 recompiler for the control-register tests - see vu0_compare.write_expected).
 * Files next to the EBOOT: vectors.bin, expected.bin. Writes vu0_psp_results.bin (all outputs) and vu0_psp_report.txt;
 * shows a summary on screen. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <psppower.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r5900_rt.h"
#include "vu0_table.h"

PSP_MODULE_INFO("PS2RC_VU0", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);

#define printf pspDebugScreenPrintf
#define BUILD_NUM 10

static char dir[256];

void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value)
{
    (void)c;
    printf("rt_fatal %s pc=%08x value=%08x\n", what, (unsigned)pc, (unsigned)value);
    sceKernelDelayThread(10 * 1000 * 1000);
    sceKernelExitGame();
}

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

int main(int argc, char **argv)
{
    pspDebugScreenInit();
    printf("ps2rc VU0 macro mode (build %d): recompiled COP2 code on Allegrex\n\n", BUILD_NUM);
    strncpy(dir, argc > 0 ? argv[0] : "", sizeof dir - 1);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    /* EE FPU / VU model: round toward zero (RM=1), flush denormal results (FS, bit 24) */
    __asm__ volatile("ctc1 %0, $31" :: "r"(0x01000001));
    scePowerSetClockFrequency(333, 333, 166);

    size_t nvec = (size_t)VU0_NTESTS * VU0_NVEC;
    uint8_t *in = load("vectors.bin", nvec * VU0_IN);
    uint8_t *exp = load("expected.bin", nvec * VU0_OUT);
    uint8_t *out = malloc((size_t)VU0_NVEC * VU0_OUT);
    if (!in || !exp || !out) goto wait;

    static uint8_t ram[0x10000] __attribute__((aligned(64)));
    static R5900Ctx ctx;
    ctx.ram = ram;
    ctx.ram_mask = sizeof ram - 1;
    char rpath[320];
    snprintf(rpath, sizeof rpath, "%svu0_psp_results.bin", dir);
    FILE *rf = fopen(rpath, "wb");
    int total_bad = 0;
    char report[8192];
    unsigned fw = (unsigned)sceKernelDevkitVersion();
    int rlen = snprintf(report, sizeof report, "==== ps2rc vu0 session ====\nbuild=%d compiler=gcc-%s fw=%x.%x%x "
                        "cpu_mhz=%d\n", BUILD_NUM, __VERSION__, (fw >> 24) & 0xFF, (fw >> 16) & 0xFF,
                        (fw >> 8) & 0xFF, scePowerGetCpuClockFrequencyInt());
    u64 t0, t1;
    sceRtcGetCurrentTick(&t0);
    for (int k = 0; k < VU0_NTESTS; k++) {
        int bad = 0;
        for (int v = 0; v < VU0_NVEC; v++) {
            size_t i = (size_t)k * VU0_NVEC + v;
            memcpy(ram + 0x1000, in + i * VU0_IN, VU0_IN);
            memset(ram + 0x2000, 0, VU0_OUT);
            ctx.r[4].d[0] = 0x1000;
            ctx.r[5].d[0] = 0x2000;
            ctx.r[29].d[0] = 0x8000;
            vu0_tests[k].fn(&ctx);
            memcpy(out + (size_t)v * VU0_OUT, ram + 0x2000, VU0_OUT);
            if (memcmp(ram + 0x2000, exp + i * VU0_OUT, VU0_OUT))
                bad++;
        }
        if (rf) fwrite(out, VU0_OUT, VU0_NVEC, rf);
        total_bad += bad;
        rlen += snprintf(report + rlen, sizeof report - rlen, "%-15s %s %d\n", vu0_tests[k].name, bad ? "FAIL" : "ok", bad);
    }
    sceRtcGetCurrentTick(&t1);
    if (rf) fclose(rf);
    rlen += snprintf(report + rlen, sizeof report - rlen, "RESULT %s: %d mismatching vectors of %u; %.1f ms\n",
                     total_bad ? "FAIL" : "PASS", total_bad, (unsigned)nvec, (double)(t1 - t0) / 1000.0);
    printf("%s", report);
    snprintf(rpath, sizeof rpath, "%svu0_psp_report.txt", dir);
    FILE *f = fopen(rpath, "w");
    if (f) { fwrite(report, 1, rlen, f); fclose(f); }
    snprintf(rpath, sizeof rpath, "%ssession.log", dir);     /* project convention: session.log next to the EBOOT */
    f = fopen(rpath, "w");
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
