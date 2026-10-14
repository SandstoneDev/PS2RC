/* hle_postfx.c - the PS2 post-processing colour filter and glow, drawn on the PSP GE (overview + cfg keys in
 * hle_postfx.h).
 *
 * Hook postfx_colour 0x51A760(c1, c2): two RGBA byte colours (r, g, b, a in memory order). The post effects 0x514300
 * call it unconditionally every gameplay frame with the time-of-day colour cycle's two post-effect colours (floats
 * 0x7074B8..C4 / 0x7074C8..D4), both scaled by a lighting factor of up to 1.4 in the dark and set to 64 grey under
 * night / infrared vision.
 * What the PS2 draws (its GS packet, decoded at run time on the host with postfx_log: 266 A+D writes, 3 sprite passes
 * in 20 strips of 32 pixels): (A) the frame (0xA0000, PSMCT32) is copied into the Z buffer's memory (0x1B8000);
 * (B) the copy is drawn back modulated by c1 (GS MODULATE = texel x RGBAQ >> 7, clamped); (C) the copy, bilinear and
 * shifted by a sub-pixel offset, modulated by c2 is added with FIX = c2.alpha. Per channel that is
 *   frame' = min(255, min(255, F c1 / 128) + min(255, F c2 / 128) a2 / 128) ~= F x K, K = (c1 + c2 a2 / 128) / 128
 * (the intro passes c1 = c2 = 64 grey, a2 127: K = 0.996). The inner clamps only matter when the other term is tiny:
 * over every row of the colour cycle data (lighting factor 1.0 and 1.4) one multiply per channel is within 3.5 / 255
 * of the GS passes; the sub-pixel blur of the 2nd term is not drawn.
 * PSP: one untextured full-screen sprite per factor of 2 (K up to 2 per pass; the colour cycle data reaches K = 3.0),
 * blend Cd x src + Cd x FIX (GE: ((2c+1)(2f+1)) >> 10 per term, one clamp) - hle_ge_postfx_mul in hle_ge.c.
 * Host: hle_raster_postfx_mul (hle_raster.c), the same arithmetic on the 8-bit PNG model. */
#include "hle.h"
#include "hle_cfg.h"                 /* the shared cfg table */
#include "hle_render.h"
#include "hle_postfx.h"
#include <stdlib.h>
#ifdef HLE_PSP
#include <pspkernel.h>
#include <pspiofilemgr.h>
#else
#include <time.h>
#endif

void GUEST(0051a760)(R5900Ctx *c);

int pfx_opt_on = 1, pfx_opt_filter = 1, pfx_opt_guest = 0, pfx_opt_dither = 1, pfx_opt_log = 0;   /* guest 0: the original only builds dropped packets */
int pfx_opt_radiosity = 1, pfx_opt_rad_fence = 0, pfx_opt_rad_guest = 0, pfx_opt_ge_probe = 64;

/* ------------------------------------------------------------------ statistics */
static uint64_t st_calls, st_drawn, st_passes, st_identity, st_notdrawn, st_skipped_guest;
static uint64_t us_hle, us_guest, n_guest_timed, cyc_guest;
static uint32_t us_hle_max;
static float k_min[3] = {9, 9, 9}, k_max[3] = {0, 0, 0};
static uint32_t last_c1, last_c2;
static uint64_t ge_us_mul, n_ge_mul, ge_us_rad, n_ge_rad;      /* sampled GE time of the passes (PSP, postfx_ge_probe) */
static uint32_t ge_us_mul_max, ge_us_rad_max;

static inline uint32_t now_us(void)
{
#ifdef HLE_PSP
    return sceKernelGetSystemTimeLow();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
#endif
}

/* ------------------------------------------------------------------ options */
static void opt_set(const char *k, const char *v)
{
    int x = atoi(v);
    if (!strcmp(k, "postfx")) pfx_opt_on = x;
    else if (!strcmp(k, "postfx_filter"))
        pfx_opt_filter = !strcmp(v, "ps2") || !strcmp(v, "1") ? 1 : 0;   /* ps2 | off */
    else if (!strcmp(k, "postfx_guest")) pfx_opt_guest = x;
    else if (!strcmp(k, "postfx_dither")) pfx_opt_dither = x;
    else if (!strcmp(k, "postfx_log")) pfx_opt_log = x;
    else if (!strcmp(k, "postfx_radiosity")) pfx_opt_radiosity = x;
    else if (!strcmp(k, "postfx_rad_fence")) pfx_opt_rad_fence = x;
    else if (!strcmp(k, "postfx_rad_guest")) pfx_opt_rad_guest = x;
    else if (!strcmp(k, "postfx_ge_probe")) pfx_opt_ge_probe = x;
}

void hle_postfx_init(void)
{
#ifdef HLE_PSP
    /* the cfg file next to the EBOOT: the postfx* lines */
    for (int ci = 0; ci < hle_cfg_count(); ci++)     /* the shared table (hle_cfg.c), whole file */
        if (!strncmp(hle_cfg_key(ci), "postfx", 6)) opt_set(hle_cfg_key(ci), hle_cfg_val(ci));
#else
    static const char *keys[] = {"postfx", "postfx_filter", "postfx_guest", "postfx_dither", "postfx_log",
                                 "postfx_radiosity", "postfx_rad_fence", "postfx_rad_guest", "postfx_ge_probe"};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char env[40] = "HLE_";
        size_t k = 4;
        for (const char *p = keys[i]; *p && k < sizeof env - 1; p++)
            env[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        env[k] = 0;
        const char *e = getenv(env);
        if (e && *e) opt_set(keys[i], e);
    }
#endif
    LOG("postfx: postfx=%d postfx_filter=%s postfx_guest=%d postfx_dither=%d postfx_log=%d postfx_radiosity=%d "
        "postfx_rad_fence=%d postfx_rad_guest=%d postfx_ge_probe=%d - the PS2 colour filter 0x51A760 %s, the radiosity "
        "0x518280 %s\n", pfx_opt_on, pfx_opt_filter ? "ps2" : "off", pfx_opt_guest, pfx_opt_dither, pfx_opt_log,
        pfx_opt_radiosity, pfx_opt_rad_fence, pfx_opt_rad_guest, pfx_opt_ge_probe,
        pfx_opt_on && pfx_opt_filter ? "is drawn (frame x K before the HUD)" : "is NOT drawn (as before)",
        pfx_opt_on && pfx_opt_radiosity ? "is drawn" : "is NOT drawn");
}

/* ------------------------------------------------------------------ host: the guest's GS packet, decoded (postfx_log) */
#ifndef HLE_PSP
static uint64_t gq(uint32_t a) { return (uint64_t)gw(a) | (uint64_t)gw(a + 4) << 32; }

static const char *gs_reg_name(unsigned r)
{
    switch (r) {
    case 0x00: return "PRIM"; case 0x01: return "RGBAQ"; case 0x02: return "ST"; case 0x03: return "UV";
    case 0x04: return "XYZF2"; case 0x05: return "XYZ2"; case 0x06: return "TEX0_1"; case 0x08: return "CLAMP_1";
    case 0x0F: return "NOP"; case 0x14: return "TEX1_1"; case 0x18: return "XYOFFSET_1"; case 0x3F: return "TEXFLUSH";
    case 0x40: return "SCISSOR_1"; case 0x42: return "ALPHA_1"; case 0x45: return "DTHE"; case 0x46: return "COLCLAMP";
    case 0x47: return "TEST_1"; case 0x49: return "PABE"; case 0x4A: return "FBA_1"; case 0x4C: return "FRAME_1";
    case 0x4E: return "ZBUF_1";
    default: return "?";
    }
}

/* the A+D list behind the GIF tag at t (NREG 1, REGS = A+D); XYZ2 / UV pairs are summarised */
static void decode_packet(uint32_t t, uint32_t end)
{
    uint64_t tag = gq(t);
    unsigned nloop = (unsigned)(tag & 0x7FFF), nreg = (unsigned)(tag >> 60), flg = (unsigned)(tag >> 58) & 3;
    LOG("postfx packet at %08x: GIF tag %016llx %016llx: NLOOP %u EOP %u FLG %u NREG %u\n", t, (unsigned long long)tag,
        (unsigned long long)gq(t + 8), nloop, (unsigned)(tag >> 15) & 1, flg, nreg ? nreg : 16);
    unsigned nuv = 0, nxyz = 0;
    for (unsigned i = 0; i < nloop && t + 16u * (i + 2) <= end + 16u; i++) {
        uint32_t q = t + 16u * (i + 1);
        uint64_t d = gq(q);
        unsigned r = (unsigned)(gq(q + 8) & 0xFF);
        char x[160] = "";
        switch (r) {
        case 0x00: snprintf(x, sizeof x, "type %u IIP %u TME %u FGE %u ABE %u FST %u", (unsigned)(d & 7),
                            (unsigned)(d >> 3) & 1, (unsigned)(d >> 4) & 1, (unsigned)(d >> 5) & 1,
                            (unsigned)(d >> 6) & 1, (unsigned)(d >> 8) & 1); break;
        case 0x01: snprintf(x, sizeof x, "R %u G %u B %u A %u", (unsigned)(d & 255), (unsigned)(d >> 8) & 255,
                            (unsigned)(d >> 16) & 255, (unsigned)(d >> 24) & 255); break;
        case 0x03:
            if (nuv++ < 4 || i + 3 >= nloop)
                snprintf(x, sizeof x, "U %.4f V %.4f", (double)(d & 0x3FFF) / 16.0, (double)((d >> 16) & 0x3FFF) / 16.0);
            break;
        case 0x05:
            if (nxyz++ < 4 || i + 3 >= nloop)
                snprintf(x, sizeof x, "X %.4f Y %.4f Z %u", (double)(d & 0xFFFF) / 16.0, (double)((d >> 16) & 0xFFFF) / 16.0,
                         (unsigned)(d >> 32));
            break;
        case 0x06: snprintf(x, sizeof x, "TBP0 %05x (byte %08x) TBW %u PSM %02x TW %u TH %u TCC %u TFX %u",
                            (unsigned)(d & 0x3FFF), (unsigned)(d & 0x3FFF) * 256u, (unsigned)(d >> 14) & 63,
                            (unsigned)(d >> 20) & 63, (unsigned)(d >> 26) & 15, (unsigned)(d >> 30) & 15,
                            (unsigned)(d >> 34) & 1, (unsigned)(d >> 35) & 3); break;
        case 0x14: snprintf(x, sizeof x, "LCM %u MXL %u MMAG %u MMIN %u", (unsigned)(d & 1), (unsigned)(d >> 2) & 7,
                            (unsigned)(d >> 5) & 1, (unsigned)(d >> 6) & 7); break;
        case 0x40: snprintf(x, sizeof x, "x %u..%u y %u..%u", (unsigned)(d & 0x7FF), (unsigned)(d >> 16) & 0x7FF,
                            (unsigned)(d >> 32) & 0x7FF, (unsigned)(d >> 48) & 0x7FF); break;
        case 0x42: snprintf(x, sizeof x, "(A %u - B %u) x C %u >> 7 + D %u, FIX %u (0 Cs 1 Cd 2 zero / As Ad FIX)",
                            (unsigned)(d & 3), (unsigned)(d >> 2) & 3, (unsigned)(d >> 4) & 3, (unsigned)(d >> 6) & 3,
                            (unsigned)(d >> 32) & 255); break;
        case 0x47: snprintf(x, sizeof x, "ATE %u ATST %u AREF %u DATE %u ZTE %u ZTST %u", (unsigned)(d & 1),
                            (unsigned)(d >> 1) & 7, (unsigned)(d >> 4) & 255, (unsigned)(d >> 14) & 1,
                            (unsigned)(d >> 16) & 1, (unsigned)(d >> 17) & 3); break;
        case 0x4C: snprintf(x, sizeof x, "FBP %03x (byte %08x) FBW %u PSM %02x FBMSK %08x", (unsigned)(d & 0x1FF),
                            (unsigned)(d & 0x1FF) * 8192u, (unsigned)(d >> 16) & 63, (unsigned)(d >> 24) & 63,
                            (unsigned)(d >> 32)); break;
        case 0x4E: snprintf(x, sizeof x, "ZBP %03x PSM %02x ZMSK %u", (unsigned)(d & 0x1FF), (unsigned)(d >> 24) & 15,
                            (unsigned)(d >> 32) & 1); break;
        default: break;
        }
        if ((r == 0x03 || r == 0x05) && !x[0]) continue;
        LOG("  [%3u] %-10s %016llx %s\n", i, gs_reg_name(r), (unsigned long long)d, x);
    }
    LOG("postfx packet: %u UV + %u XYZ2 writes (sprite corners)\n", nuv, nxyz);
}

/* the packet is written at the render driver's packet pointer (gp+4224 = 0x66C9F0); the open may flush and restart
   the buffer, so the GIF tag is searched backwards from the end: EOP, NREG 1, REGS = A+D,
   NLOOP = (raster width / 32) x 4 + 186 */
static void dump_packet(uint32_t before, uint32_t after)
{
    unsigned want = 0;
    uint32_t fb = gw(0x825C30) ? gw(gw(0x825C30) + 0x60) : 0;
    if (fb && fb < hle_ram_end) want = (unsigned)(((int32_t)gw(fb + 0x0C) >> 5) * 4 + 186);
    LOG("postfx packet pointer %08x -> %08x (frame raster %08x %ux%u, expected NLOOP %u)\n", before, after, fb,
        fb ? gw(fb + 0x0C) : 0, fb ? gw(fb + 0x10) : 0, want);
    if ((after & 15) || after < 0x100) return;
    for (uint32_t t = after - 16; t + 0x8000u > after && t >= 16; t -= 16) {
        uint64_t tag = gq(t);
        if ((tag >> 60) == 1 && (tag >> 15 & 1) && (gq(t + 8) & 15) == 0xE && (!want || (tag & 0x7FFF) == want)) {
            decode_packet(t, after);
            return;
        }
    }
    LOG("postfx packet: GIF tag not found\n");
}

/* every A+D GIF packet written between two packet pointers (same buffer: forward scan; else the last one only) */
static void dump_packets(uint32_t before, uint32_t after)
{
    if (before >= after || after - before > 0x10000u || (before & 15)) { dump_packet(before, after); return; }
    LOG("postfx packets %08x -> %08x\n", before, after);
    for (uint32_t t = before; t + 16 <= after;) {
        uint64_t tag = gq(t);
        unsigned nloop = (unsigned)(tag & 0x7FFF);
        if ((tag >> 60) == 1 && (gq(t + 8) & 15) == 0xE && nloop && t + 16u * (nloop + 1) <= after) {
            decode_packet(t, t + 16u * (nloop + 1));
            t += 16u * (nloop + 1);
        } else
            t += 16;
    }
}
#endif

/* ------------------------------------------------------------------ the hook */
/* K per channel from the two RGBA colours (bytes r, g, b, a in memory order = the low byte first) */
static void filter_k(uint32_t c1, uint32_t c2, float k[3])
{
    float a2 = (float)(c2 >> 24 & 255u);
    for (int i = 0; i < 3; i++) {
        float v1 = (float)(c1 >> (8 * i) & 255u), v2 = (float)(c2 >> (8 * i) & 255u);
        k[i] = (v1 + v2 * a2 * (1.0f / 128.0f)) * (1.0f / 128.0f);
    }
}

HLE(0051a760)
{
    uint32_t c1 = ARG(0), c2 = ARG(1);
    st_calls++;
    last_c1 = c1;
    last_c2 = c2;
    int lg = st_calls <= (uint64_t)pfx_opt_log;
    if (pfx_opt_on && pfx_opt_filter) {
        uint32_t t0 = now_us();
        float k[3];
        filter_k(c1, c2, k);
        for (int i = 0; i < 3; i++) {
            if (k[i] < k_min[i]) k_min[i] = k[i];
            if (k[i] > k_max[i]) k_max[i] = k[i];
        }
#ifdef HLE_PSP
        int probe = pfx_opt_ge_probe > 0 && st_calls % (uint64_t)pfx_opt_ge_probe == 1;
        if (probe) hle_ge_postfx_probe();                 /* drain the frame's earlier GE work */
        int np = hle_ge_postfx_mul(k, pfx_opt_dither);
        if (probe && np > 0) {
            uint32_t g = hle_ge_postfx_probe();
            ge_us_mul += g;
            n_ge_mul++;
            if (g > ge_us_mul_max) ge_us_mul_max = g;
        }
#else
        int np = hle_raster_is_open() ? hle_raster_postfx_mul(k) : 0;
#endif
        if (np > 0) { st_drawn++; st_passes += (uint64_t)np; }
        else if (np == 0 && k[0] > 0.996f && k[0] < 1.004f && k[1] > 0.996f && k[1] < 1.004f && k[2] > 0.996f &&
                 k[2] < 1.004f) st_identity++;
        else st_notdrawn++;
        uint32_t dt = now_us() - t0;
        us_hle += dt;
        if (dt > us_hle_max) us_hle_max = dt;
        if (lg)
            LOG("postfx #%llu flip %llu vtime %.3f: c1 %08x c2 %08x -> K %.3f %.3f %.3f, %d passes\n",
                (unsigned long long)st_calls, (unsigned long long)hle_render_flip_count(), (double)hle_vtime / EE_HZ,
                c1, c2, (double)k[0], (double)k[1], (double)k[2], np);
    } else if (lg)
        LOG("postfx #%llu: c1 %08x c2 %08x (filter off)\n", (unsigned long long)st_calls, c1, c2);
    if (!pfx_opt_guest) {                      /* the original only builds a GS packet the HLE drops */
        st_skipped_guest++;
        return;
    }
#ifndef HLE_PSP
    uint32_t before = gw(0x66C9F0);
#endif
    uint64_t sw0 = hle_stat_switches, v0 = hle_vtime;
    uint32_t t1 = now_us();
    GUEST(0051a760)(c);
    if (hle_stat_switches == sw0) {
        us_guest += now_us() - t1;
        cyc_guest += hle_vtime - v0;
        n_guest_timed++;
    }
#ifndef HLE_PSP
    if (lg) dump_packet(before, gw(0x66C9F0));
#endif
}

/* ---- hook postfx_glow 0x518280(intensity limit, filter passes, render passes, intensity): the post effects call it
   when the glow is on (gp+5720) with (the colour cycle's limit 0x7074DC or 220, 2, 1, 35), and the darkness filter
   with (45, 0, 2, 255). What the PS2 draws (packets decoded with postfx_log): the frame copied into the Z buffer's
   memory, two in-place bilinear 2:1 downsamples there, a bright pass (Cd - Cs) x 128 >> 7 + Cd with RGBAQ = limit =
   clamp(2 D - limit), then frame += the 160x112 result upscaled x FIX (intensity / 2) >> 7 in 32-pixel strips. Drawn by
   hle_ge_postfx_radiosity / hle_raster_postfx_radiosity (postfx_radiosity), then the translated original only with
   postfx_rad_guest=1. */
void GUEST(00518280)(R5900Ctx *c);
static uint64_t st_rad_calls, st_rad_drawn, st_rad_other, st_rad_skipped;
static uint64_t us_rad, us_rad_guest, n_rad_guest;
static uint32_t us_rad_max;
static uint32_t rad_last[4];

HLE(00518280)
{
    st_rad_calls++;
    for (int i = 0; i < 4; i++) rad_last[i] = ARG(i);
    int lg = st_rad_calls <= (uint64_t)pfx_opt_log;
    if (lg)
        LOG("postfx radiosity #%llu flip %llu: limit %d filter passes %d render passes %d intensity %d\n",
            (unsigned long long)st_rad_calls, (unsigned long long)hle_render_flip_count(), SARG(0), SARG(1), SARG(2),
            SARG(3));
    if (SARG(1) != 2 || SARG(2) != 1) st_rad_other++;     /* not the standard (limit, 2, 1, 35) call */
    if (pfx_opt_on && pfx_opt_radiosity) {
        uint32_t t0 = now_us();
#ifdef HLE_PSP
        int probe = pfx_opt_ge_probe > 0 && st_rad_calls % (uint64_t)pfx_opt_ge_probe == 2;
        if (probe) hle_ge_postfx_probe();
        uint32_t tr = now_us();
        int np = hle_ge_postfx_radiosity(SARG(0), SARG(1), SARG(2), SARG(3), pfx_opt_rad_fence || probe);
        if (probe && np > 0) {
            uint32_t g = (now_us() - tr) + hle_ge_postfx_probe();   /* the internal syncs + the composite */
            ge_us_rad += g;
            n_ge_rad++;
            if (g > ge_us_rad_max) ge_us_rad_max = g;
        }
#else
        int np = hle_raster_is_open() ? hle_raster_postfx_radiosity(SARG(0), SARG(1), SARG(2), SARG(3)) : 0;
#endif
        if (np > 0) st_rad_drawn++;
        uint32_t dt = now_us() - t0;
        us_rad += dt;
        if (dt > us_rad_max) us_rad_max = dt;
    }
    if (!pfx_opt_rad_guest) { st_rad_skipped++; return; }
#ifndef HLE_PSP
    uint32_t before = gw(0x66C9F0);
#endif
    uint64_t sw0 = hle_stat_switches;
    uint32_t t1 = now_us();
    GUEST(00518280)(c);
    if (hle_stat_switches == sw0) { us_rad_guest += now_us() - t1; n_rad_guest++; }
#ifndef HLE_PSP
    if (lg) dump_packets(before, gw(0x66C9F0));
#endif
}

void hle_postfx_report(void)
{
    if (!st_calls) { LOG("postfx: the colour filter 0x51A760 was never called\n"); return; }
    LOG("postfx: colour filter 0x51A760 called %llu times, drawn %llu (%llu full-screen passes), identity %llu, "
        "not drawn %llu (not the screen / GE off); K range R %.3f..%.3f G %.3f..%.3f B %.3f..%.3f, last c1 %08x c2 %08x\n",
        (unsigned long long)st_calls, (unsigned long long)st_drawn, (unsigned long long)st_passes,
        (unsigned long long)st_identity, (unsigned long long)st_notdrawn, (double)k_min[0], (double)k_max[0],
        (double)k_min[1], (double)k_max[1], (double)k_min[2], (double)k_max[2], last_c1, last_c2);
    LOG("postfx CPU: HLE %.2f us per call (max %u us, incl. the GE list writes), guest original %.2f us per call "
        "(%llu timed, %.1f virtual us), %llu originals skipped (postfx_guest=0)\n",
        (double)us_hle / (double)st_calls, us_hle_max, n_guest_timed ? (double)us_guest / (double)n_guest_timed : 0.0,
        (unsigned long long)n_guest_timed, n_guest_timed ? (double)cyc_guest / (double)n_guest_timed * 1e6 / EE_HZ : 0.0,
        (unsigned long long)st_skipped_guest);
    LOG("postfx radiosity 0x518280: %llu calls, drawn %llu, %llu with other than 2 filter / 1 render passes, last args "
        "%d %d %d %d; HLE %.2f us per call (max %u us, incl. the GE list writes and syncs), guest original %.2f us per call "
        "(%llu timed), %llu originals skipped (postfx_rad_guest=0)\n", (unsigned long long)st_rad_calls,
        (unsigned long long)st_rad_drawn, (unsigned long long)st_rad_other, (int)rad_last[0], (int)rad_last[1],
        (int)rad_last[2], (int)rad_last[3], st_rad_calls ? (double)us_rad / (double)st_rad_calls : 0.0, us_rad_max,
        n_rad_guest ? (double)us_rad_guest / (double)n_rad_guest : 0.0, (unsigned long long)n_rad_guest,
        (unsigned long long)st_rad_skipped);
#ifdef HLE_PSP
    LOG("postfx GE time (sampled every %d-th call, list sync waits): colour filter %.1f us per pass (max %u us, %llu "
        "samples), radiosity %.1f us (max %u us, %llu samples; all 4 passes incl. its own syncs and list writes)\n",
        pfx_opt_ge_probe, n_ge_mul ? (double)ge_us_mul / (double)n_ge_mul : 0.0, ge_us_mul_max,
        (unsigned long long)n_ge_mul, n_ge_rad ? (double)ge_us_rad / (double)n_ge_rad : 0.0, ge_us_rad_max,
        (unsigned long long)n_ge_rad);
#endif
}
