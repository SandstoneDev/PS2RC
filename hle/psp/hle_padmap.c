/* hle_padmap.c - the player's control mapping: parse + check the ps2rc.cfg keys, then map one pad sample.
 * Rules and keys: hle_padmap.h. Pure C, no PSP headers, no allocation. */
#include <stdio.h>
#include <string.h>
#include "hle_padmap.h"

#define PS2_SELECT   0x0001u
#define PS2_L3       0x0002u
#define PS2_R3       0x0004u
#define PS2_START    0x0008u
#define PS2_UP       0x0010u
#define PS2_RIGHT    0x0020u
#define PS2_DOWN     0x0040u
#define PS2_LEFT     0x0080u
#define PS2_L2       0x0100u
#define PS2_R2       0x0200u
#define PS2_L1       0x0400u
#define PS2_R1       0x0800u
#define PS2_TRIANGLE 0x1000u
#define PS2_CIRCLE   0x2000u
#define PS2_CROSS    0x4000u
#define PS2_SQUARE   0x8000u

#define TOK_DPAD 0x0004u                                   /* parse only: the word "dpad" */
#define DEAD 40                                            /* PSP sticks rest anywhere in ~128 +- 30 */
#define FPS_COMBO (PM_L | PM_R | PM_SELECT)                /* the port's fps counter toggle */
#define SYS_COMBO (PM_L | PM_R | PM_DOWN | PM_START)       /* custom firmware's menu */
#define DPAD (PM_UP | PM_DOWN | PM_LEFT | PM_RIGHT)

static const struct { const char *n; uint32_t b; } names[] = {
    {"select", PM_SELECT}, {"start", PM_START}, {"l", PM_L}, {"r", PM_R}, {"up", PM_UP}, {"down", PM_DOWN},
    {"left", PM_LEFT}, {"right", PM_RIGHT}, {"triangle", PM_TRIANGLE}, {"circle", PM_CIRCLE}, {"cross", PM_CROSS},
    {"square", PM_SQUARE}, {"home", PM_HOME}, {"dpad", TOK_DPAD}, {"analog", PM_ASTICK},
};
#define N_NAMES (int)(sizeof names / sizeof names[0])

static const char *const key_of[PI_N] = {"pad_l2", "pad_r2", "pad_l3", "pad_r3", "pad_rstick", "pad_l1", "pad_r1",
                                          "pad_select", "pad_start"};
static const char *const friendly_of[PI_N] = {"map_l2", "map_r2", "map_l3", "map_r3", "map_rstick", "map_l1",
                                               "map_r1", "map_select", "map_start"};
static const uint16_t ps2_of[PI_N] = {PS2_L2, PS2_R2, PS2_L3, PS2_R3, 0, PS2_L1, PS2_R1, PS2_SELECT, PS2_START};
static const uint32_t mod_btn[4] = {PM_SELECT, PM_L, PM_R, 0};
static const char *const mod_name[4] = {"select", "l", "r", "none"};

static int popc(uint32_t v) { int n = 0; while (v) { n += (int)(v & 1u); v >>= 1; } return n; }

const char *padmap_chord_name(uint32_t c, char *buf, int n)
{
    int o = 0;
    buf[0] = 0;
    if (!c) { snprintf(buf, (size_t)n, "none"); return buf; }
    for (int i = 0; i < N_NAMES; i++)
        if (c & names[i].b && o < n - 1) {
            int r = snprintf(buf + o, (size_t)(n - o), "%s%s", o ? "+" : "", names[i].n);
            if (r > 0) o += r;
            if (o >= n) o = n - 1;
        }
    return buf;
}

uint8_t padmap_stick(unsigned v)
{
    int d = (int)v - 128;
    if (d > -DEAD && d < DEAD) return 0x80;
    int s = d > 0 ? (d - DEAD) * 127 / (127 - DEAD) : (d + DEAD) * 128 / (128 - DEAD);   /* past the dead zone */
    s += 128;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* the built-in combination of an input for a modifier: modifier + L = L2, + R = R2, + Triangle = L3, + Circle = R3,
   + D-pad = right stick; a modifier that is L or R itself gets SELECT in its own place (modifier l: L2 = L+SELECT) */
static uint32_t def_chord(int mod, int i)
{
    uint32_t m = mod_btn[mod];
    switch (i) {
    case PI_L1: return PM_L;
    case PI_R1: return PM_R;
    case PI_SELECT: return PM_SELECT;
    case PI_START: return PM_START;
    }
    if (!m) return 0;
    switch (i) {
    case PI_L2: return m | (m == PM_L ? PM_SELECT : PM_L);
    case PI_R2: return m | (m == PM_R ? PM_SELECT : PM_R);
    case PI_L3: return m | PM_TRIANGLE;
    case PI_R3: return m | PM_CIRCLE;
    case PI_RSTICK: return m;
    }
    return 0;
}

static void lower_copy(const char *v, char *out, int n)
{
    int o = 0;
    while (*v == ' ' || *v == '\t') v++;
    for (; *v && o < n - 1; v++) {
        if (*v == ' ' || *v == '\t') continue;
        out[o++] = (char)(*v >= 'A' && *v <= 'Z' ? *v + 32 : *v);
    }
    out[o] = 0;
}

/* "a+b+c" -> bits; 0 = ok (out 0 = none), else *why */
static int parse_combo(const char *v, uint32_t *out, const char **why)
{
    char s[64];
    lower_copy(v, s, sizeof s);
    *out = 0;
    if (!strcmp(s, "none") || !strcmp(s, "off")) return 0;
    if (!*s) { *why = "empty"; return -1; }
    for (char *p = s; *p;) {
        char *e = strchr(p, '+');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        int k = -1;
        for (int i = 0; i < N_NAMES; i++)
            if (strlen(names[i].n) == l && !strncmp(names[i].n, p, l)) k = i;
        if (k < 0) { *why = "unknown button name"; return -1; }
        if (*out & names[k].b) { *why = "a button twice"; return -1; }
        *out |= names[k].b;
        if (!e) break;
        p = e + 1;
        if (!*p) { *why = "ends with +"; return -1; }
    }
    if (*out & PM_HOME) { *why = "HOME belongs to the system"; return -1; }
    return 0;
}

static int word(const char *v, const char *const *w, int n)
{
    char s[32];
    lower_copy(v, s, sizeof s);
    for (int i = 0; i < n; i++)
        if (!strcmp(s, w[i])) return i;
    return -1;
}

static void note(PadMap *pm, const char *key, const char *val, const char *why, const char *used)
{
    size_t o = strlen(pm->notes);
    if (o >= sizeof pm->notes - 1) return;
    if (!used) snprintf(pm->notes + o, sizeof pm->notes - o, "%s=%s: %s; ", key, val, why);
    else snprintf(pm->notes + o, sizeof pm->notes - o, "%s=%s: %s, %s used; ", key, val, why, used);
}

/* an input's combination as text (the right stick with its +dpad / +analog) */
static const char *input_name(int i, uint32_t c, int rs_mode, char *buf, int n)
{
    padmap_chord_name(c, buf, n);
    if (i == PI_RSTICK && c) {
        size_t l = strlen(buf);
        snprintf(buf + l, (size_t)n - l, "+%s", rs_mode == RS_DPAD ? "dpad" : "analog");
    }
    return buf;
}

/* the reason a combination may not be used (NULL = fine) */
static const char *reserved(int i, uint32_t c)
{
    if (!c) return NULL;
    if (i == PI_RSTICK) return NULL;                       /* <button>+dpad / +analog: 2 buttons, never these */
    if ((c & FPS_COMBO) == FPS_COMBO) return "contains L+R+SELECT (the fps counter)";
    if (popc(c) == 3 && !(c & ~SYS_COMBO)) return "three of L+R+Down+START (the system menu)";
    return NULL;
}

static void build_entries(PadMap *pm)
{
    static const struct { uint32_t c; uint16_t o; } fixed[] = {
        {PM_TRIANGLE, PS2_TRIANGLE}, {PM_CIRCLE, PS2_CIRCLE}, {PM_CROSS, PS2_CROSS}, {PM_SQUARE, PS2_SQUARE},
        {PM_UP, PS2_UP}, {PM_DOWN, PS2_DOWN}, {PM_LEFT, PS2_LEFT}, {PM_RIGHT, PS2_RIGHT},
    };
    int n = 0;
    for (int i = 0; i < PI_N; i++) {
        if (i == PI_RSTICK || !pm->chord[i]) continue;
        PadEnt *e = &pm->ent[n++];
        e->chord = pm->chord[i];
        e->out = ps2_of[i];
        e->dir = -1;
        e->owner = (int8_t)i;
        e->select_plain = i == PI_SELECT && pm->chord[i] == PM_SELECT;
    }
    for (unsigned j = 0; j < sizeof fixed / sizeof fixed[0]; j++) {
        PadEnt *e = &pm->ent[n++];
        e->chord = fixed[j].c;
        e->out = fixed[j].o;
        e->dir = -1;
        e->owner = -1;
        e->select_plain = 0;
    }
    uint32_t b = pm->chord[PI_RSTICK];
    if (pm->rs_mode == RS_DPAD && b) {
        static const uint32_t d[4] = {PM_LEFT, PM_RIGHT, PM_UP, PM_DOWN};   /* the order of the original code */
        for (int k = 0; k < 4; k++) {
            PadEnt *e = &pm->ent[n++];
            e->chord = b | d[k];
            e->out = k == 2 ? pm->sel_up_bit : 0;
            e->dir = (int8_t)(k == 2 && pm->sel_up_bit ? -1 : k);
            e->owner = PI_RSTICK;
            e->select_plain = 0;
        }
    } else if (pm->rs_mode == RS_ANALOG && b) {
        PadEnt *e = &pm->ent[n++];
        e->chord = b | PM_ASTICK;
        e->out = 0;
        e->dir = 4;
        e->owner = PI_RSTICK;
        e->select_plain = 0;
    }
    pm->n_ent = n;
}

/* rstick value -> mode + button; 0 = ok */
static int parse_rstick(const char *v, int mod, uint32_t *btn, int *mode, const char **why)
{
    uint32_t c;
    if (parse_combo(v, &c, why)) return -1;
    if (!c) { *btn = 0; *mode = RS_NONE; return 0; }
    if (c == PM_ASTICK) {                                  /* "analog": the modifier + the analog stick */
        if (!mod_btn[mod]) { *why = "analog needs a modifier (modifier=none)"; return -1; }
        *btn = mod_btn[mod];
        *mode = RS_ANALOG;
        return 0;
    }
    uint32_t how = c & (TOK_DPAD | PM_ASTICK), b = c & ~(TOK_DPAD | PM_ASTICK);
    if ((how != TOK_DPAD && how != PM_ASTICK) || popc(b) != 1 || (b & DPAD)) {
        *why = "use <button>+dpad, <button>+analog, analog or none";
        return -1;
    }
    *btn = b;
    *mode = how == TOK_DPAD ? RS_DPAD : RS_ANALOG;
    return 0;
}

void padmap_config(PadMap *pm, padmap_get_fn get)
{
    memset(pm, 0, sizeof *pm);
    static const char *const mods[] = {"select", "l", "r", "none"}, *const sg[] = {"always", "tap"};
    const char *vmod = get("pad_mod"), *vsg = get("pad_sel_game"), *su = get("pad_sel_up");
    int bad_mod = 0, bad_sg = 0;
    if (vmod) {
        int k = word(vmod, mods, 4);
        if (k < 0) { note(pm, "modifier", vmod, "not select, l, r or none", "select"); bad_mod = 1; }
        else pm->mod = k;
    }
    if (vsg) {
        int k = word(vsg, sg, 2);
        if (k < 0) { note(pm, "select_to_game", vsg, "not always or tap", "always"); bad_sg = 1; }
        else pm->sel_tap = k;
    }
    if (su) pm->sel_up_bit = !strncmp(su, "l3", 2) ? PS2_L3 : !strncmp(su, "stick", 5) ? 0 : PS2_R3;   /* as before */

    uint32_t def[PI_N];
    int user[PI_N], def_mode = mod_btn[pm->mod] ? RS_DPAD : RS_NONE;
    const char *val[PI_N];
    pm->rs_mode = def_mode;
    char nb[64];
    for (int i = 0; i < PI_N; i++) {
        def[i] = pm->chord[i] = def_chord(pm->mod, i);
        user[i] = 0;
        val[i] = get(key_of[i]);
        if (!val[i]) continue;
        const char *why = NULL;
        uint32_t c;
        int mode = RS_NONE;
        if (i == PI_RSTICK) {
            if (parse_rstick(val[i], pm->mod, &c, &mode, &why)) goto bad;
        } else {
            if (parse_combo(val[i], &c, &why)) goto bad;
            why = (c & (TOK_DPAD | PM_ASTICK)) ? "dpad / analog only for map_rstick"
                : c && popc(c) < (i <= PI_R3 ? 2 : 1) ? "needs two or three buttons"
                : popc(c) > 3 ? "more than three buttons" : reserved(i, c);
            if (why) goto bad;
        }
        pm->chord[i] = c;
        if (i == PI_RSTICK) pm->rs_mode = mode;
        user[i] = c != def[i] || (i == PI_RSTICK && mode != def_mode);
        continue;
    bad:
        note(pm, friendly_of[i], val[i], why, input_name(i, def[i], def_mode, nb, sizeof nb));
        pm->bad[i] = 1;
    }
    if (pm->rs_mode != RS_DPAD && pm->sel_up_bit)
        note(pm, "select_up", su, "applies only to a D-pad right stick, ignored", NULL);
    if (pm->sel_tap && pm->chord[PI_SELECT] != PM_SELECT)
        note(pm, "select_to_game", "tap", "applies only while SELECT gives SELECT, ignored", NULL);

    /* conflicts: two entries with the same combination -> the changed key (the later one if both are changed)
       takes its default; repeat (the defaults never conflict with each other, so this ends) */
    for (int guard = 0; guard < 4 * PI_N; guard++) {
        build_entries(pm);
        int victim = -1, other = -1;
        uint32_t same = 0;
        for (int a = 0; a < pm->n_ent && victim < 0; a++)
            for (int b = a + 1; b < pm->n_ent; b++) {
                int oa = pm->ent[a].owner, ob = pm->ent[b].owner;
                if (pm->ent[a].chord != pm->ent[b].chord || oa == ob) continue;
                if (ob >= 0 && user[ob]) victim = ob, other = oa;
                else if (oa >= 0 && user[oa]) victim = oa, other = ob;
                else continue;
                same = pm->ent[a].chord;
                break;
            }
        if (victim < 0) break;
        char why[96];
        if (other >= 0) snprintf(why, sizeof why, "same buttons as %s", friendly_of[other]);
        else snprintf(why, sizeof why, "takes the button %s", padmap_chord_name(same, nb, sizeof nb));
        note(pm, friendly_of[victim], val[victim], why, input_name(victim, def[victim], def_mode, nb, sizeof nb));
        pm->chord[victim] = def[victim];
        if (victim == PI_RSTICK) pm->rs_mode = def_mode;
        user[victim] = 0;
        pm->bad[victim] = 1;
    }
    build_entries(pm);

    /* the log line: '*' = not in the file (built-in default), '!' = the file's value was not used (notes) */
    int o = snprintf(pm->line, sizeof pm->line, "controls: modifier=%s%s select_to_game=%s%s", mod_name[pm->mod],
                     !vmod ? "*" : bad_mod ? "!" : "", pm->sel_tap ? "tap" : "always", !vsg ? "*" : bad_sg ? "!" : "");
    for (int i = 0; i < PI_N && o < (int)sizeof pm->line - 48; i++) {
        input_name(i, pm->chord[i], pm->rs_mode, nb, sizeof nb);
        if (i == PI_RSTICK && pm->rs_mode == RS_DPAD && pm->sel_up_bit && pm->chord[i])
            strcat(nb, pm->sel_up_bit == PS2_L3 ? "(up=l3)" : "(up=r3)");
        o += snprintf(pm->line + o, sizeof pm->line - (size_t)o, " %s=%s%s", friendly_of[i], nb,
                      !val[i] ? "*" : pm->bad[i] ? "!" : "");
    }
}

uint16_t padmap_apply(const PadMap *pm, PadTap *st, uint32_t buttons, unsigned lx, unsigned ly, uint64_t frame,
                      uint8_t sticks[4])
{
    uint32_t h = buttons & PM_PHYS;
    uint8_t ax = padmap_stick(lx), ay = padmap_stick(ly);
    if (ax != 0x80 || ay != 0x80) h |= PM_ASTICK;
    uint32_t act = 0;
    for (int e = 0; e < pm->n_ent; e++)
        if (!(pm->ent[e].chord & ~h)) act |= 1u << e;
    uint16_t m = 0;
    uint8_t rx = 0x80, ry = 0x80, lsx = ax, lsy = ay;
    int sel_used = 0;
    for (int e = 0; e < pm->n_ent; e++) {
        if (!(act & (1u << e))) continue;
        const PadEnt *p = &pm->ent[e];
        if (p->select_plain) { if (!pm->sel_tap) m |= p->out; continue; }   /* never silent (always) / tap below */
        int quiet = 0;
        for (int f = 0; f < pm->n_ent && !quiet; f++)
            if (f != e && (act & (1u << f)) && (p->chord & pm->ent[f].chord) == p->chord &&
                pm->ent[f].chord != p->chord)
                quiet = 1;                                 /* inside a larger held combination */
        if (quiet) continue;
        if (p->chord & PM_SELECT) sel_used = 1;
        switch (p->dir) {
        case -1: m |= p->out; break;
        case 0: rx = 0x00; break;
        case 1: rx = 0xFF; break;
        case 2: ry = 0x00; break;
        case 3: ry = 0xFF; break;
        case 4: rx = ax; ry = ay; lsx = lsy = 0x80; break;
        }
    }
    if (pm->sel_tap && pm->chord[PI_SELECT] == PM_SELECT) {   /* SELECT as a tap */
        if (buttons & PM_SELECT) {
            if (!st->sel_down) st->sel_down = 1, st->sel_alone = 1;
            if ((h & PM_PHYS & ~PM_SELECT) || sel_used) st->sel_alone = 0;
        } else if (st->sel_down) {
            st->sel_down = 0;
            if (st->sel_alone) st->pulse = 1, st->pulse_frame = frame;
        }
        if (st->pulse) {
            if (frame != st->pulse_frame) st->pulse_frame = frame, st->pulse++;
            if (st->pulse > PADMAP_TAP_FRAMES) st->pulse = 0;
            else m |= PS2_SELECT;
        }
    }
    sticks[0] = rx;
    sticks[1] = ry;
    sticks[2] = lsx;
    sticks[3] = lsy;
    return m;
}
