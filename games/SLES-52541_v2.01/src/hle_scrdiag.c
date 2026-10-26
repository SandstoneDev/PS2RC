/* hle_scrdiag.c - the game's script interpreter - command ring, invalid-command guard, crash report.
 *
 * Tester's blackjack session (converter 0.1.1, PSP): crash AdEL in the pointer-to-member thunk 0x556E50 called by
 * the script interpreter's command step 0x1FA8D0. The script at IP held the bytes 00 5F = command 0x5F00: handler group
 * 243 of 27 (0x5F00 / 100) -> ProcessOneCommand indexed the 12-byte member-pointer table 0x6FEC90 far past its end, read
 * floats as {delta, index} and loaded a "vtable" from this + 0x3EB2B8C3 -> unaligned address. On the PS2 the same
 * bytes would crash the same way: the script itself was already off its command stream (a desync or corrupted script
 * memory before that point); the registers do not say which script, where, or what it executed before.
 *
 * script_command (HOOK 0x1FA8D0, hooks.def):
 *  - keeps, per script slot, a ring of its last SCR_RING commands (offset in its code + command number), ~1 us/frame;
 *  - guard: a command number >= 2700 (27 handler groups) can never be dispatched; instead of the crash the HLE logs
 *    the script (name, base, IP, bytes around it, its last commands) and runs the game's own TERMINATE_THIS_SCRIPT
 *    (command 0x004E of handler group 0, 0x1FAF60) for it - that script stops, the game goes on. ps2rc.cfg
 *    x_scrguard=0: report only, then the original runs (and crashes as before).
 * hle_pack_crash_report (called by the core's crash dump): every active script with its slot, name, base / IP, the
 * offset inside its code (main script file offset for the main script's threads, offset inside the streamed script for
 * external ones), gosub depth / returns, the bytes around IP and its ring.
 *
 * Layout facts (measured on SLES-52541 v2.01 with the translation; the field offsets equal the well-known PC
 * script slot): list head gp+1048, next +0, name +8 (8 chars), base IP +16, IP +20, gosub stack +24 (8 words),
 * gosub depth +56 (16 bits), active +196, external +199, mission +220. Script space (the main script file image) at 0x6B2870,
 * script slots 0x6AD470 + n * 0xE0 (96, ending at the script space), handler table source 0x5F9A38 (27 groups). */
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "hle_fast.h"

#define SCR_GP           0x0066B970u
#define SCR_ACTIVE_HEAD  (SCR_GP + 1048u)
#define SCR_SPACE        0x006B2870u
#define SCR_SLOTS        0x006AD470u
#define SCR_SLOT_SIZE    0xE0u
#define SCR_SLOT_COUNT   96u
#define SCR_CMD_LIMIT    2700u           /* 27 handler groups of 100 commands */
#define SCR_TERMINATE    0x004Eu         /* TERMINATE_THIS_SCRIPT, handler group 0 */
#define SCR_GROUP0       0x001FAF60u     /* ProcessCommands0To99(this, command) */
#define SCR_RING         8u

static int scr_ram_ok(uint32_t a, uint32_t n) { return a >= 0x100000u && a < hle_ram_end && n <= hle_ram_end - a; }
static int scr_slot_of(uint32_t s)
{
    if (s < SCR_SLOTS || s >= SCR_SLOTS + SCR_SLOT_COUNT * SCR_SLOT_SIZE || (s - SCR_SLOTS) % SCR_SLOT_SIZE) return -1;
    return (int)((s - SCR_SLOTS) / SCR_SLOT_SIZE);
}

/* per slot: the last SCR_RING commands, bit 31 (used) | (offset in the script's code mod 512 KB) << 12 | command &
   0xFFF (a main script file offset above 512 KB is kept modulo 512 KB - with the IP of the report enough to find it) */
static uint32_t ring[SCR_SLOT_COUNT][SCR_RING];
static uint8_t ring_pos[SCR_SLOT_COUNT];
static uint64_t n_guarded;

static void scr_name(uint32_t s, char nm[9])
{
    for (int i = 0; i < 8; i++) { uint8_t b = gb(s + 8 + (uint32_t)i); nm[i] = b >= 32 && b < 127 ? (char)b : b ? '?' : 0; }
    nm[8] = 0;
}

static void scr_bytes(char *o, size_t cap, uint32_t a, unsigned n)
{
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    o[0] = 0;
    for (unsigned i = 0; i < n && k + 4 < cap; i++) {
        if (!scr_ram_ok(a + i, 1)) break;
        uint8_t b = gb(a + i);
        o[k++] = hx[b >> 4]; o[k++] = hx[b & 15]; o[k++] = ' ';
    }
    if (k) k--;
    o[k] = 0;
}

static int scr_is_reg(R5900Ctx *c, uint32_t s)
{
    if (!c) return 0;
    for (int r = 1; r < 32; r++)
        if ((uint32_t)c->r[r].d[0] == s) return r;
    return 0;
}

/* one script: header line, bytes around IP, gosub returns, its last commands */
static void scr_report(R5900Ctx *c, uint32_t s, const char *tag)
{
    int slot = scr_slot_of(s);
    char nm[9], before[64], at[112], rs[12] = "";
    scr_name(s, nm);
    uint32_t base = gw(s + 16), ip = gw(s + 20), org = base ? base : SCR_SPACE, off = ip - org;
    unsigned sp = gh(s + 56);
    int reg = scr_is_reg(c, s);
    scr_bytes(before, sizeof before, ip - 16, 16);
    scr_bytes(at, sizeof at, ip, 32);
    if (reg) snprintf(rs, sizeof rs, " <- r%d", reg);
    LOG("%s slot %2d %08x %-8s base %08x ip %08x ip+%06x ext %u mis %u gosub %u%s\n", tag, slot, s, nm, base, ip, off,
        gb(s + 199), gb(s + 220), sp, rs);
    LOG("     ip-16: %s | ip: %s\n", before, at);
    if (sp && sp <= 8) {
        char st[96];
        size_t n = 0;
        for (unsigned i = 0; i < sp && n + 9 < sizeof st; i++)
            n += (size_t)snprintf(st + n, sizeof st - n, "%s%06x", i ? " " : "", gw(s + 24 + 4 * i) - org);
        st[n] = 0;
        LOG("     gosub returns (offsets): %s\n", st);
    }
    if (slot >= 0) {
        char rl[160];
        size_t n = 0;
        for (unsigned i = 0; i < SCR_RING && n + 16 < sizeof rl; i++) {
            uint32_t e = ring[slot][(ring_pos[slot] + i) % SCR_RING];
            if (!e) continue;
            n += (size_t)snprintf(rl + n, sizeof rl - n, "%s%05x:%03x", n ? " " : "", (e >> 12) & 0x7FFFFu, e & 0xFFFu);
        }
        rl[n] = 0;
        LOG("     last commands (offset:command, oldest first): %s\n", n ? rl : "(none)");
    }
}

void GUEST(001fa8d0)(R5900Ctx *c);
HLE(001fa8d0)                                     /* the script interpreter's command step(this) -> 1 = stop this frame */
{
    uint32_t s = ARG(0), ip = gw(s + 20);
    uint32_t cmd = (uint32_t)gh(ip) & 0x7FFFu;
    int slot = scr_slot_of(s);
    if (slot >= 0) {
        uint32_t base = gw(s + 16);
        ring[slot][ring_pos[slot]] = 0x80000000u | (((ip - (base ? base : SCR_SPACE)) & 0x7FFFFu) << 12) | (cmd & 0xFFFu);
        ring_pos[slot] = (uint8_t)((ring_pos[slot] + 1) % SCR_RING);
    }
    if (__builtin_expect(cmd >= SCR_CMD_LIMIT, 0)) {
        static int guard = -1;
        if (guard < 0) guard = hle_x_opt("scrguard", 1) != 0;
        n_guarded++;
        if (n_guarded <= 16) {
            LOG("SCRIPT GUARD: command %04x is no command (handler group %u of 27) at vtime %.3f s, frame %llu - %s\n",
                cmd, cmd / 100u, (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame,
                guard ? "this script is terminated (TERMINATE_THIS_SCRIPT), the game goes on"
                      : "x_scrguard=0: the original runs (crash)");
            scr_report(c, s, "SCRIPT GUARD:");
        }
        if (guard) {
            RET(hle_call(c, SCR_GROUP0, 2, s, SCR_TERMINATE));
            return;
        }
    }
    GUEST(001fa8d0)(c);
}

void hle_scrdiag_report(void)
{
    if (n_guarded) LOG("script guard: %llu invalid commands stopped (see SCRIPT GUARD lines)\n",
                       (unsigned long long)n_guarded);
}

void hle_pack_crash_report(R5900Ctx *c)
{
    uint32_t s = scr_ram_ok(SCR_ACTIVE_HEAD, 4) ? gw(SCR_ACTIVE_HEAD) : 0;
    LOG("scripts at the crash (active list gp+1048; ip+ = offset in the main script file for base 0, else in the streamed script; "
        "<- rN = a guest register holds the slot):\n");
    for (unsigned k = 0; s && k < SCR_SLOT_COUNT; k++, s = gw(s)) {
        if (!scr_ram_ok(s, SCR_SLOT_SIZE) || scr_slot_of(s) < 0) {
            LOG("  (list entry %08x is not a script slot - list ends)\n", s);
            break;
        }
        scr_report(c, s, " ");
    }
}
