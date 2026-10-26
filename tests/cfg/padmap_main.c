/* padmap_main.c - host harness for hle/psp/hle_padmap.c (tests/cfg/test_padmap.py).
 *   padmap_t line  [key=value ...]   -> the "controls:" line and the notes
 *   padmap_t equiv [key=value ...]   -> compares the mapping with the original fixed code (below) for every button
 *                                       combination and a few stick positions: "equiv ok N" or the first mismatch
 *   padmap_t seq   [key=value ...]   -> stdin lines "FRAME BUTTONS(hex) LX LY [CTX]" -> "MASK RX RY LX LY" (hex;
 *                                       CTX 0 on foot, 1 in a vehicle)
 *   padmap_t car   [key=value ...]   -> the "controls in a vehicle:" line
 * The keys are the internal ones (pad_l2, pad_mod, ...), as hle_cfg.c stores them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle_padmap.h"

static int n_kv;
static char *kv_k[64], *kv_v[64];
static const char *get(const char *k)
{
    for (int i = n_kv - 1; i >= 0; i--)
        if (!strcmp(kv_k[i], k)) return kv_v[i];
    return NULL;
}

/* the original hle_pad.c mapping before the player's mapping existed (build 33 .. 0.1.1), PSP bits in, PS2 out */
static uint16_t old_map(uint32_t b, unsigned lx, unsigned ly, uint16_t sel_up_bit, uint8_t st[4])
{
    uint16_t m = 0;
    int mod = (b & PM_SELECT) != 0;
    if (b & PM_SELECT) m |= 0x0001;
    if (b & PM_START) m |= 0x0008;
    if (b & PM_CROSS) m |= 0x4000;
    if (b & PM_SQUARE) m |= 0x8000;
    if (mod) {
        if (b & PM_TRIANGLE) m |= 0x0002;
        if (b & PM_CIRCLE) m |= 0x0004;
        if (b & PM_L) m |= 0x0100;
        if (b & PM_R) m |= 0x0200;
    } else {
        if (b & PM_TRIANGLE) m |= 0x1000;
        if (b & PM_CIRCLE) m |= 0x2000;
        if (b & PM_L) m |= 0x0400;
        if (b & PM_R) m |= 0x0800;
    }
    uint8_t rx = 0x80, ry = 0x80;
    if (mod) {
        if (b & PM_LEFT) rx = 0x00;
        if (b & PM_RIGHT) rx = 0xFF;
        if (b & PM_UP) {
            if (sel_up_bit) m |= sel_up_bit;
            else ry = 0x00;
        }
        if (b & PM_DOWN) ry = 0xFF;
    } else {
        if (b & PM_UP) m |= 0x0010;
        if (b & PM_DOWN) m |= 0x0040;
        if (b & PM_LEFT) m |= 0x0080;
        if (b & PM_RIGHT) m |= 0x0020;
    }
    st[0] = rx;
    st[1] = ry;
    st[2] = padmap_stick(lx);
    st[3] = padmap_stick(ly);
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    for (int i = 2; i < argc && n_kv < 64; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = 0;
        kv_k[n_kv] = argv[i];
        kv_v[n_kv++] = eq + 1;
    }
    static PadMap pm;
    padmap_config(&pm, get);
    if (!strcmp(argv[1], "line")) {
        printf("%s\nnotes: %s\n", pm.line, pm.notes);
        return 0;
    }
    if (!strcmp(argv[1], "car")) {
        printf("%s\nnotes: %s\n", pm.car_line, pm.notes);
        return 0;
    }
    if (!strcmp(argv[1], "equiv")) {
        const char *su = get("pad_sel_up");
        uint16_t sub = su ? (!strncmp(su, "l3", 2) ? 0x0002 : !strncmp(su, "stick", 5) ? 0 : 0x0004) : 0;
        static const unsigned sv[] = {0x80, 0x00, 0xFF, 0x60, 0xB0, 0x20, 0xE8};
        long n = 0;
        for (uint32_t c = 0; c < 4096; c++) {
            /* spread the 12 bits onto the PSP button bits (+ HOME / unknown bits sometimes set: ignored) */
            static const uint32_t bit[12] = {PM_SELECT, PM_START, PM_UP, PM_RIGHT, PM_DOWN, PM_LEFT, PM_L, PM_R,
                                             PM_TRIANGLE, PM_CIRCLE, PM_CROSS, PM_SQUARE};
            uint32_t b = 0;
            for (int k = 0; k < 12; k++) if (c & (1u << k)) b |= bit[k];
            if (c & 1u) b |= 0x20000u;                       /* the HOLD switch bit: ignored by both */
            for (unsigned x = 0; x < sizeof sv / sizeof sv[0]; x++)
                for (unsigned y = 0; y < sizeof sv / sizeof sv[0]; y++) {
                    static PadTap tap;
                    memset(&tap, 0, sizeof tap);
                    uint8_t a[4], o[4], a1[4];
                    uint16_t ma = padmap_apply(&pm, &tap, b, sv[x], sv[y], 1, 0, a);
                    uint16_t mo = old_map(b, sv[x], sv[y], sub, o);
                    uint16_t m1 = padmap_apply(&pm, &tap, b, sv[x], sv[y], 1, 1, a1);   /* "in a vehicle" too */
                    n++;
                    if (ma != mo || memcmp(a, o, 4) || m1 != mo || memcmp(a1, o, 4)) {
                        printf("MISMATCH buttons %05x stick %02x,%02x: new %04x %02x %02x %02x %02x old %04x %02x %02x "
                               "%02x %02x\n", (unsigned)b, sv[x], sv[y], ma, a[0], a[1], a[2], a[3], mo, o[0], o[1],
                               o[2], o[3]);
                        return 1;
                    }
                }
        }
        printf("equiv ok %ld\n", n);
        return 0;
    }
    if (!strcmp(argv[1], "seq")) {
        static PadTap tap;
        memset(&tap, 0, sizeof tap);
        unsigned long long f;
        unsigned b, lx, ly;
        int ctx;
        char ln[128];
        while (fgets(ln, sizeof ln, stdin)) {
            ctx = 0;
            if (sscanf(ln, "%llu %x %x %x %d", &f, &b, &lx, &ly, &ctx) < 4) continue;
            uint8_t s[4];
            uint16_t m = padmap_apply(&pm, &tap, b, lx, ly, f, ctx, s);
            printf("%04x %02x %02x %02x %02x\n", m, s[0], s[1], s[2], s[3]);
        }
        return 0;
    }
    return 2;
}
