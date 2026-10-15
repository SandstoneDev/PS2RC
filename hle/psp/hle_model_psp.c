/* hle_model_psp.c - which PSP this is: the 32 MB PSP-1000 or a 64 MB model (see hle_model_psp.h). */
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include "hle_model_psp.h"
#include "ps2rc_kcall.h"                 /* KUBridge kuKernelCall + kuKernelGetModel (own import, ps2rc_kcall.S) */

int hle_psp_is_emulator(void);                  /* psp/hle_prof_psp.c */

static HleModel g_m;
static int g_done;

/* KERNEL (kuKernelCall): the system register that holds the RAM size, read only - no syscall, no FPU */
static int k_ram_reg(unsigned dst, unsigned z)
{
    (void)z;
    volatile unsigned *d = (volatile unsigned *)dst;
    d[0] = *(volatile unsigned *)0xBC100040u;
    __asm__ volatile("sync");
    return 0;
}

static void detect(void)
{
    memset(&g_m, 0, sizeof g_m);
    g_m.kcall_rc = 0x7FFFFFFF;
    if (hle_psp_is_emulator()) { g_m.source = "emulator"; return; }
    volatile unsigned v = 0;
    Ps2rcKCall a;
    memset(&a, 0, sizeof a);
    a.arg[0] = (unsigned)&v;
    g_m.kcall_rc = ps2rc_kcall((void *)k_ram_reg, &a);
    if (g_m.kcall_rc < 0) { g_m.source = "no kernel bridge"; return; }   /* KUBridge missing: GetModel not linked */
    int md = ps2rc_kgetmodel();
    if (md >= 0 && md <= 10) g_m.gen = md + 1;
    g_m.reg = v;
    if (v) { g_m.ram_mb = 16 << (v & 3u); g_m.source = "register"; }
    else if (g_m.gen) { g_m.ram_mb = g_m.gen == 1 ? 32 : 64; g_m.source = "model"; }   /* sysmem: only 01g has 32 MB */
    else g_m.source = "register and model unknown";
}

const HleModel *hle_model_get(void)
{
    if (!g_done) { g_done = 1; detect(); }
    return &g_m;
}

void hle_model_fake(int model)
{
    hle_model_get();
    g_m.gen = model + 1;
    g_m.ram_mb = model == 0 ? 32 : 64;
    g_m.reg = model == 0 ? 1u : 2u;
    g_m.fake = 1;
    g_m.source = "TEST mem_fake_model";
}

int hle_model_is_32mb(void)
{
    const HleModel *m = hle_model_get();
    return m->ram_mb == 0 ? -1 : m->ram_mb <= 32;
}

static const char *series(int gen)
{
    switch (gen) {
    case 1: return "PSP-1000";
    case 2: return "PSP-2000";
    case 3: case 4: case 7: case 9: return "PSP-3000";
    case 5: return "PSP Go";
    case 11: return "PSP-E1000";
    default: return gen ? "PSP" : "PSP (model Unknown)";
    }
}

void hle_model_describe(char *out, unsigned n)
{
    const HleModel *m = hle_model_get();
    char g[8] = "?";
    if (m->gen) snprintf(g, sizeof g, "%02dg", m->gen);
    if (!m->ram_mb)
        snprintf(out, n, "Unknown (%s; kuKernelCall %08x) - a 24 MB start can be a PSP-1000 or ARK's extra memory off",
                 m->source, (unsigned)m->kcall_rc);
    else
        snprintf(out, n, "%s (%d MB, %s; from the %s, register 0xBC100040=%08x, kuKernelCall %08x)%s", series(m->gen),
                 m->ram_mb, g, m->source, m->reg, (unsigned)m->kcall_rc,
                 m->gen && (m->gen == 1) != (m->ram_mb <= 32) ? " - MODEL AND REGISTER DISAGREE, the register wins" : "");
}
