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
#define TOK_TAP  0x01000000u                               /* parse only: "tap" */
#define TOK_HOLD 0x02000000u                               /* parse only: "hold" */
#define DEAD 40                                            /* PSP sticks rest anywhere in ~128 +- 30 */
#define FAR_LO 0x40                                        /* analog_left / _up: rescaled value below this */
#define FAR_HI 0xC0                                        /* analog_right / _down: above this */
#define FPS_COMBO (PM_L | PM_R | PM_SELECT)                /* the port's fps counter toggle */
#define SYS_COMBO (PM_L | PM_R | PM_DOWN | PM_START)       /* custom firmware's menu */
#define DPAD (PM_UP | PM_DOWN | PM_LEFT | PM_RIGHT)

static const struct { const char *n; uint32_t b; } names[] = {
    {"select", PM_SELECT}, {"start", PM_START}, {"l", PM_L}, {"r", PM_R}, {"up", PM_UP}, {"down", PM_DOWN},
    {"left", PM_LEFT}, {"right", PM_RIGHT}, {"triangle", PM_TRIANGLE}, {"circle", PM_CIRCLE}, {"cross", PM_CROSS},
    {"square", PM_SQUARE}, {"home", PM_HOME}, {"dpad", TOK_DPAD}, {"analog", PM_ASTICK},
    {"analog_left", PM_ALEFT}, {"analog_right", PM_ARIGHT}, {"analog_up", PM_AUP}, {"analog_down", PM_ADOWN},
    {"tap", TOK_TAP}, {"hold", TOK_HOLD},
};
#define N_NAMES (int)(sizeof names / sizeof names[0])

static const char *const in_name[PI_N] = {"l2", "r2", "l3", "r3", "rstick", "l1", "r1", "select", "start", "cross",
                                          "circle", "square", "triangle", "up", "down", "left", "right", "lstick"};
static const uint16_t ps2_of[PI_N] = {PS2_L2, PS2_R2, PS2_L3, PS2_R3, 0, PS2_L1, PS2_R1, PS2_SELECT, PS2_START,
                                      PS2_CROSS, PS2_CIRCLE, PS2_SQUARE, PS2_TRIANGLE, PS2_UP, PS2_DOWN, PS2_LEFT,
                                      PS2_RIGHT, 0};
static const uint32_t mod_btn[4] = {PM_SELECT, PM_L, PM_R, 0};
static const char *const mod_name[4] = {"select", "l", "r", "none"};
static const char *const preset_name[PRESET_N] = {"default", "stories"};

/* the stories preset (the classic handheld open-world layout: L + analog = camera, D-pad = weapons /
   radio), on foot and in a vehicle; order = PI_* */
static const char *const stories_def[2][PI_N] = {
    {"left", "right", "down", "l+r+hold", "l+analog", "l+hold", "r", "select", "start", "cross", "circle", "square",
     "triangle", "up", "l+down", "l+left", "l+right", "analog"},
    {"l+analog_left/l+analog_down", "l+analog_right/l+analog_down", "down", "up", "l+dpad", "l+hold", "r", "select",
     "start", "cross", "circle", "square", "triangle", "right", "left", "l+r+left", "l+r+right", "analog"},
};

static int popc(uint32_t v) { int n = 0; while (v) { n += (int)(v & 1u); v >>= 1; } return n; }

const char *padmap_chord_name(uint32_t c, char *buf, int n)
{
    int o = 0;
    buf[0] = 0;
    if (!c) { snprintf(buf, (size_t)n, "none"); return buf; }
    if (c & PM_ADIRS) c &= ~PM_ASTICK;                     /* analog_left already says "analog" */
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

/* the built-in value of an input as text (parsed like a file value) */
static const char *def_text(int preset, int mod, int layer, int i, char *buf, int n)
{
    if (preset == PRESET_STORIES) return stories_def[layer][i];
    const char *m = mod_name[mod];
    switch (i) {
    case PI_L2: if (mod == 3) return "none"; snprintf(buf, (size_t)n, mod == 1 ? "l+select" : "%s+l", m); return buf;
    case PI_R2: if (mod == 3) return "none"; snprintf(buf, (size_t)n, mod == 2 ? "r+select" : "%s+r", m); return buf;
    case PI_L3: if (mod == 3) return "none"; snprintf(buf, (size_t)n, "%s+triangle", m); return buf;
    case PI_R3: if (mod == 3) return "none"; snprintf(buf, (size_t)n, "%s+circle", m); return buf;
    case PI_RSTICK: if (mod == 3) return "none"; snprintf(buf, (size_t)n, "%s+dpad", m); return buf;
    case PI_L1: return "l";
    case PI_R1: return "r";
    case PI_LSTICK: return "analog";
    }
    return in_name[i];                                     /* select start cross ... right: itself */
}

/* lower case, spaces and tabs removed */
static void lower_copy(const char *v, char *out, int n)
{
    int o = 0;
    for (; *v && o < n - 1; v++) {
        if (*v == ' ' || *v == '\t') continue;
        out[o++] = (char)(*v >= 'A' && *v <= 'Z' ? *v + 32 : *v);
    }
    out[o] = 0;
}

/* one combination "a+b+c" (lower case, length l) -> bits incl. the parse-only tokens; 0 = ok, else *why */
static int parse_combo(const char *s, size_t len, uint32_t *out, const char **why)
{
    *out = 0;
    if (!len) { *why = "empty"; return -1; }
    for (const char *p = s, *end = s + len; p < end;) {
        const char *e = memchr(p, '+', (size_t)(end - p));
        size_t l = e ? (size_t)(e - p) : (size_t)(end - p);
        int k = -1;
        for (int i = 0; i < N_NAMES; i++)
            if (strlen(names[i].n) == l && !strncmp(names[i].n, p, l)) k = i;
        if (k < 0) { *why = "unknown button name"; return -1; }
        if (*out & names[k].b) { *why = "a button twice"; return -1; }
        *out |= names[k].b;
        if (!e) break;
        p = e + 1;
        if (p >= end) { *why = "ends with +"; return -1; }
    }
    if (*out & PM_HOME) { *why = "HOME belongs to the system"; return -1; }
    if (*out & PM_ADIRS) *out |= PM_ASTICK;
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

static void note(PadMap *pm, const char *pre, const char *key, const char *val, const char *why, const char *used)
{
    size_t o = strlen(pm->notes);
    if (o >= sizeof pm->notes - 1) return;
    if (!used) snprintf(pm->notes + o, sizeof pm->notes - o, "%s%s=%s: %s; ", pre, key, val, why);
    else snprintf(pm->notes + o, sizeof pm->notes - o, "%s%s=%s: %s, %s used; ", pre, key, val, why, used);
}

/* the reason a combination of PSP buttons may not be used (NULL = fine) */
static const char *reserved(uint32_t c)
{
    c &= PM_PHYS;
    if ((c & FPS_COMBO) == FPS_COMBO) return "contains L+R+SELECT (the fps counter)";
    if (popc(c) == 3 && !(c & ~SYS_COMBO)) return "three of L+R+Down+START (the system menu)";
    return NULL;
}

/* a stick value -> mode + its button(s); 0 = ok */
static int parse_stick(int i, const char *s, int mod, PadIn *in, const char **why)
{
    static const uint32_t d[4] = {PM_LEFT, PM_RIGHT, PM_UP, PM_DOWN};
    uint32_t c;
    if (strchr(s, '/')) { *why = "one value only for a stick"; return -1; }
    if (parse_combo(s, strlen(s), &c, why)) return -1;
    if (c & (TOK_TAP | TOK_HOLD)) { *why = "tap / hold not for a stick"; return -1; }
    uint32_t how = c & (TOK_DPAD | PM_ANALOG), b = c & PM_PHYS;
    in->n = 1;
    if (i == PI_LSTICK) {
        if (c == PM_ASTICK) { in->mode = RS_ANALOG; in->c[0] = 0; return 0; }
        if (how != TOK_DPAD || popc(b) > 1 || (b & DPAD)) { *why = "use analog, dpad, <button>+dpad or none"; return -1; }
        in->mode = RS_DPAD;
        in->c[0] = b;
        return 0;
    }
    if (c == PM_ASTICK) {                                  /* "analog": the modifier + the analog stick */
        if (!mod_btn[mod]) { *why = "analog needs a modifier (modifier=none)"; return -1; }
        in->mode = RS_ANALOG;
        in->c[0] = mod_btn[mod];
        return 0;
    }
    if ((how != TOK_DPAD && how != PM_ASTICK) || (b & DPAD) || popc(b) > 2 || (how == PM_ASTICK && !b)) {
        *why = "use <button>+dpad, <button>+analog, analog or none";
        return -1;
    }
    if (how == TOK_DPAD) {
        for (int k = 0; k < 4; k++)
            if (reserved(b | d[k])) { *why = reserved(b | d[k]); return -1; }
    } else if (reserved(b)) { *why = reserved(b); return -1; }
    in->mode = how == TOK_DPAD ? RS_DPAD : RS_ANALOG;
    in->c[0] = b;
    return 0;
}

/* a value of input i -> *in; 0 = ok, else *why */
static int parse_input(int i, const char *v, int mod, PadIn *in, const char **why)
{
    char s[128];
    lower_copy(v, s, sizeof s);
    memset(in, 0, sizeof *in);
    if (!strcmp(s, "none") || !strcmp(s, "off")) return 0;
    if (!*s) { *why = "empty"; return -1; }
    if (i == PI_RSTICK || i == PI_LSTICK) return parse_stick(i, s, mod, in, why);
    for (char *p = s;;) {
        char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        uint32_t c;
        if (in->n >= PADMAP_ALT) { *why = "more than three alternatives"; return -1; }
        if (parse_combo(p, l, &c, why)) return -1;
        uint32_t phys = c & PM_PHYS;
        if (c & TOK_DPAD) { *why = "dpad only for map_rstick / map_lstick"; return -1; }
        if ((c & TOK_TAP) && (c & TOK_HOLD)) { *why = "tap and hold together"; return -1; }
        if (!phys) { *why = "needs a button"; return -1; }
        if (popc(phys) > 3) { *why = "more than three buttons"; return -1; }
        if (reserved(phys)) { *why = reserved(phys); return -1; }
        uint32_t ch = c & ~(TOK_TAP | TOK_HOLD);
        for (int k = 0; k < in->n; k++)
            if (in->c[k] == ch) { *why = "the same combination twice"; return -1; }
        in->c[in->n] = ch;
        in->fl[in->n] = (uint8_t)((c & TOK_TAP ? PF_TAP : 0) | (c & TOK_HOLD ? PF_HOLD : 0));
        in->n++;
        if (!e) break;
        p = e + 1;
        if (!*p) { *why = "ends with /"; return -1; }
    }
    return 0;
}

static int same_in(const PadIn *a, const PadIn *b)
{
    if (a->n != b->n || a->mode != b->mode) return 0;
    for (int k = 0; k < a->n; k++)
        if (a->c[k] != b->c[k] || a->fl[k] != b->fl[k]) return 0;
    return 1;
}

/* an input as text: "select+l", "left/l+left", "l+hold", "select+dpad", "none" */
static const char *input_text(int i, const PadIn *in, char *buf, int n)
{
    char t[64];
    buf[0] = 0;
    if (i == PI_RSTICK || i == PI_LSTICK) {
        if (in->mode == RS_NONE) snprintf(buf, (size_t)n, "none");
        else if (in->mode == RS_ANALOG && !in->c[0]) snprintf(buf, (size_t)n, "analog");
        else if (!in->c[0]) snprintf(buf, (size_t)n, "dpad");
        else snprintf(buf, (size_t)n, "%s+%s", padmap_chord_name(in->c[0], t, sizeof t),
                      in->mode == RS_DPAD ? "dpad" : "analog");
        return buf;
    }
    if (!in->n) { snprintf(buf, (size_t)n, "none"); return buf; }
    int o = 0;
    for (int k = 0; k < in->n && o < n - 1; k++) {
        int r = snprintf(buf + o, (size_t)(n - o), "%s%s%s", k ? "/" : "", padmap_chord_name(in->c[k], t, sizeof t),
                         in->fl[k] & PF_TAP ? "+tap" : in->fl[k] & PF_HOLD ? "+hold" : "");
        if (r > 0) o += r;
        if (o >= n) o = n - 1;
    }
    return buf;
}

static void build_entries(PadLayer *ly, uint16_t sel_up_bit)
{
    static const uint32_t d[4] = {PM_LEFT, PM_RIGHT, PM_UP, PM_DOWN};   /* the order of the original code */
    int n = 0;
    for (int i = 0; i < PI_N; i++) {
        if (i == PI_RSTICK || i == PI_LSTICK) continue;
        const PadIn *in = &ly->in[i];
        for (int k = 0; k < in->n && n < PADMAP_MAX_ENT; k++) {
            PadEnt *e = &ly->ent[n++];
            e->chord = in->c[k];
            e->out = ps2_of[i];
            e->dir = -1;
            e->owner = (int8_t)i;
            e->flags = in->fl[k];
            e->select_plain = i == PI_SELECT && in->c[k] == PM_SELECT && !in->fl[k];
        }
    }
    for (int s = 0; s < 2; s++) {
        const PadIn *in = &ly->in[s ? PI_LSTICK : PI_RSTICK];
        uint32_t b = in->c[0];
        if (in->mode == RS_DPAD) {
            for (int k = 0; k < 4 && n < PADMAP_MAX_ENT; k++) {
                PadEnt *e = &ly->ent[n++];
                e->chord = b | d[k];
                e->out = !s && k == 2 ? sel_up_bit : 0;
                e->dir = (int8_t)(s ? 5 + k : k == 2 && sel_up_bit ? -1 : k);
                e->owner = (int8_t)(s ? PI_LSTICK : PI_RSTICK);
                e->flags = 0;
                e->select_plain = 0;
            }
        } else if (!s && in->mode == RS_ANALOG && n < PADMAP_MAX_ENT) {
            PadEnt *e = &ly->ent[n++];
            e->chord = b | PM_ASTICK;
            e->out = 0;
            e->dir = 4;
            e->owner = PI_RSTICK;
            e->flags = 0;
            e->select_plain = 0;
        }
    }
    ly->n_ent = n;
}

/* a vehicle-layer key: "foot" (any case) or an empty value = not set, the on-foot value applies (the in-game menu's
   first choice; the converter leaves the key out instead) */
static const char *car_get(padmap_get_fn get, const char *key)
{
    const char *v = get(key);
    if (!v) return NULL;
    while (*v == ' ' || *v == '	') v++;
    if (!*v) return NULL;
    static const char foot[] = "foot";
    int i = 0;
    for (; foot[i]; i++) if ((v[i] | 0x20) != foot[i]) return v;
    while (v[i] == ' ' || v[i] == '	') i++;
    return v[i] ? v : NULL;
}

void padmap_config(PadMap *pm, padmap_get_fn get)
{
    memset(pm, 0, sizeof *pm);
    static const char *const mods[] = {"select", "l", "r", "none"}, *const sg[] = {"always", "tap"};
    const char *vpre = get("pad_preset"), *vmod = get("pad_mod"), *vsg = get("pad_sel_game"), *su = get("pad_sel_up");
    const char *vci = get("pad_cam_inv");
    int bad_pre = 0, bad_mod = 0, bad_sg = 0;
    if (vpre) {
        static const char *const old_names[] = {"vcs"};   /* an earlier name of the stories preset, still accepted */
        int k = word(vpre, preset_name, PRESET_N);
        if (k < 0 && word(vpre, old_names, 1) == 0) k = PRESET_STORIES;
        if (k < 0) { note(pm, "", "controls_preset", vpre, "not default or stories", "default"); bad_pre = 1; }
        else pm->preset = k;
    }
    if (pm->preset == PRESET_STORIES) {
        pm->mod = 1;                                       /* "analog" = L + analog in this preset */
        if (vmod) { note(pm, "", "modifier", vmod, "applies only to controls_preset=default, ignored", NULL); bad_mod = 1; }
    } else if (vmod) {
        int k = word(vmod, mods, 4);
        if (k < 0) { note(pm, "", "modifier", vmod, "not select, l, r or none", "select"); bad_mod = 1; }
        else pm->mod = k;
    }
    if (vsg) {
        int k = word(vsg, sg, 2);
        if (k < 0) { note(pm, "", "select_to_game", vsg, "not always or tap", "always"); bad_sg = 1; }
        else pm->sel_tap = k;
    }
    if (vci) {                                             /* camera_invert=off|x|y|xy (or 0..3) */
        static const char *const ci[] = {"off", "x", "y", "xy"};
        int k = word(vci, ci, 4);
        if (k < 0 && vci[0] >= '0' && vci[0] <= '3' && !vci[1]) k = vci[0] - '0';
        if (k < 0) note(pm, "", "camera_invert", vci, "not off, x, y or xy", "off");
        else pm->cam_inv = k;
    }
    if (su) pm->sel_up_bit = !strncmp(su, "l3", 2) ? PS2_L3 : !strncmp(su, "stick", 5) ? 0 : PS2_R3;   /* as before */

    char key[24], fk[24], nb[128], db[64];
    pm->has_car = pm->preset == PRESET_STORIES;
    for (int i = 0; i < PI_N && !pm->has_car; i++) {
        snprintf(key, sizeof key, "pad_c_%s", in_name[i]);
        if (car_get(get, key)) pm->has_car = 1;
    }
    const char *val[2][PI_N];
    int from_car[2][PI_N];
    int any_dpad_rs = 0, any_plain_sel = 0;
    for (int L = 0; L <= pm->has_car; L++) {
        PadLayer *ly = &pm->lay[L];
        PadIn def[PI_N];
        int user[PI_N];
        const char *pre = L ? "in a vehicle: " : "";
        for (int i = 0; i < PI_N; i++) {
            const char *dt = def_text(pm->preset, pm->mod, L, i, db, sizeof db), *why = NULL;
            if (parse_input(i, dt, pm->mod, &def[i], &why)) memset(&def[i], 0, sizeof def[i]);   /* (never) */
            ly->in[i] = def[i];
            user[i] = 0;
            snprintf(key, sizeof key, "pad_c_%s", in_name[i]);
            const char *v = L ? car_get(get, key) : NULL;
            from_car[L][i] = v != NULL;
            if (!v) {
                snprintf(key, sizeof key, "pad_%s", in_name[i]);
                v = get(key);
            }
            val[L][i] = v;
            if (!v) continue;
            PadIn in;
            snprintf(fk, sizeof fk, "%s_%s", from_car[L][i] ? "car" : "map", in_name[i]);
            if (parse_input(i, v, pm->mod, &in, &why)) {
                if (!L || from_car[L][i]) note(pm, "", fk, v, why, input_text(i, &def[i], nb, sizeof nb));
                pm->bad[L][i] = 1;
                continue;
            }
            ly->in[i] = in;
            user[i] = !same_in(&in, &def[i]);
        }
        /* select_to_game=tap: the SELECT that gives SELECT comes as a tap */
        PadIn *sel = &ly->in[PI_SELECT];
        if (sel->n == 1 && sel->c[0] == PM_SELECT && !sel->fl[0]) {
            any_plain_sel = 1;
            if (pm->sel_tap) sel->fl[0] = PF_TAP;
        }
        if (ly->in[PI_RSTICK].mode == RS_DPAD) any_dpad_rs = 1;

        /* conflicts: two entries with the same combination -> the changed key (the later one if both are changed)
           takes its default; repeat (the defaults never conflict with each other, so this ends) */
        for (int guard = 0; guard < 4 * PI_N; guard++) {
            build_entries(ly, pm->sel_up_bit);
            int victim = -1, other = -1;
            for (int a = 0; a < ly->n_ent && victim < 0; a++)
                for (int b = a + 1; b < ly->n_ent; b++) {
                    int oa = ly->ent[a].owner, ob = ly->ent[b].owner;
                    if (ly->ent[a].chord != ly->ent[b].chord || oa == ob) continue;
                    if (user[ob]) victim = ob, other = oa;
                    else if (user[oa]) victim = oa, other = ob;
                    else continue;
                    break;
                }
            if (victim < 0) break;
            char why[96];
            snprintf(why, sizeof why, "same buttons as %s_%s", L ? "car" : "map", in_name[other]);
            snprintf(fk, sizeof fk, "%s_%s", from_car[L][victim] ? "car" : "map", in_name[victim]);
            note(pm, pre, fk, val[L][victim], why, input_text(victim, &def[victim], nb, sizeof nb));
            ly->in[victim] = def[victim];
            if (victim == PI_SELECT && pm->sel_tap && def[victim].n == 1 && def[victim].c[0] == PM_SELECT &&
                !def[victim].fl[0])
                ly->in[victim].fl[0] = PF_TAP;
            user[victim] = 0;
            pm->bad[L][victim] = 1;
        }
        build_entries(ly, pm->sel_up_bit);
    }
    if (!any_dpad_rs && pm->sel_up_bit)
        note(pm, "", "select_up", su, "applies only to a D-pad right stick, ignored", NULL);
    if (pm->sel_tap && !any_plain_sel)
        note(pm, "", "select_to_game", "tap", "applies only while SELECT gives SELECT, ignored", NULL);

    /* the log lines: '*' = not in the file (built-in), '!' = the file's value was not used (notes) */
    int o = snprintf(pm->line, sizeof pm->line, "controls: %s%s%s%smodifier=%s%s select_to_game=%s%s",
                     vpre ? "controls_preset=" : "", vpre ? preset_name[pm->preset] : "",
                     vpre ? (bad_pre ? "!" : "") : "", vpre ? " " : "", mod_name[pm->mod],
                     !vmod ? "*" : bad_mod ? "!" : "", pm->sel_tap ? "tap" : "always", !vsg ? "*" : bad_sg ? "!" : "");
    for (int i = 0; i < PI_N && o < (int)sizeof pm->line - 64; i++) {
        const PadIn *in = &pm->lay[0].in[i];
        input_text(i, in, nb, sizeof nb);
        if (i >= PI_CROSS && !val[0][i] && !strcmp(nb, def_text(PRESET_DEFAULT, 0, 0, i, db, sizeof db))) continue;
        if (i == PI_SELECT && in->n == 1 && in->c[0] == PM_SELECT && (in->fl[0] & PF_TAP) && pm->sel_tap)
            snprintf(nb, sizeof nb, "select");                 /* the tap is select_to_game's (shown there) */
        if (i == PI_RSTICK && in->mode == RS_DPAD && pm->sel_up_bit)
            strcat(nb, pm->sel_up_bit == PS2_L3 ? "(up=l3)" : "(up=r3)");
        o += snprintf(pm->line + o, sizeof pm->line - (size_t)o, " map_%s=%s%s", in_name[i], nb,
                      !val[0][i] ? "*" : pm->bad[0][i] ? "!" : "");
    }
    if (pm->cam_inv && o < (int)sizeof pm->line - 24)
        snprintf(pm->line + o, sizeof pm->line - (size_t)o, " camera_invert=%s",
                 pm->cam_inv == 1 ? "x" : pm->cam_inv == 2 ? "y" : "xy");
    if (pm->has_car) {
        o = snprintf(pm->car_line, sizeof pm->car_line, "controls in a vehicle:");
        int any = 0;
        for (int i = 0; i < PI_N && o < (int)sizeof pm->car_line - 64; i++) {
            char t0[128];
            input_text(i, &pm->lay[0].in[i], t0, sizeof t0);
            input_text(i, &pm->lay[1].in[i], nb, sizeof nb);
            if (!from_car[1][i] && !strcmp(t0, nb)) continue;
            if (i == PI_RSTICK && pm->lay[1].in[i].mode == RS_DPAD && pm->sel_up_bit)
                strcat(nb, pm->sel_up_bit == PS2_L3 ? "(up=l3)" : "(up=r3)");
            o += snprintf(pm->car_line + o, sizeof pm->car_line - (size_t)o, " car_%s=%s%s", in_name[i], nb,
                          !from_car[1][i] ? "*" : pm->bad[1][i] ? "!" : "");
            any = 1;
        }
        if (!any) snprintf(pm->car_line + o, sizeof pm->car_line - (size_t)o, " (as on foot)");
    }
}


/* b strictly inside a */
static int inside(uint32_t b, uint32_t a) { return (a & b) == b && a != b; }

uint16_t padmap_apply(const PadMap *pm, PadTap *st, uint32_t buttons, unsigned lx, unsigned ly_, uint64_t frame,
                      int ctx, uint8_t sticks[4])
{
    int li = pm->has_car && ctx ? 1 : 0;
    const PadLayer *ly = &pm->lay[li];
    if (st->ctx != li) {                                   /* layer change: no tap carries over */
        memset(st->e, 0, sizeof st->e);
        st->ctx = li;
    }
    uint32_t h = buttons & PM_PHYS;
    uint8_t ax = padmap_stick(lx), ay = padmap_stick(ly_);
    if (ax != 0x80 || ay != 0x80) h |= PM_ASTICK;
    if (ax < FAR_LO) h |= PM_ALEFT;
    if (ax > FAR_HI) h |= PM_ARIGHT;
    if (ay < FAR_LO) h |= PM_AUP;
    if (ay > FAR_HI) h |= PM_ADOWN;
    uint64_t act = 0, live = 0;
    for (int e = 0; e < ly->n_ent; e++)
        if (!(ly->ent[e].chord & ~h)) act |= 1ull << e;
    /* 1. plain combinations: silent inside a larger held plain combination */
    for (int e = 0; e < ly->n_ent; e++) {
        const PadEnt *p = &ly->ent[e];
        if (!(act & (1ull << e)) || p->flags) continue;
        int quiet = 0;
        if (!p->select_plain)                              /* never silent (select_to_game=always) */
            for (int f = 0; f < ly->n_ent && !quiet; f++)
                if ((act & (1ull << f)) && !ly->ent[f].flags && inside(p->chord, ly->ent[f].chord)) quiet = 1;
        if (!quiet) live |= 1ull << e;
    }
    /* 2. tap / hold combinations: a pulse after a lone press, or held alone long enough ("+hold") */
    uint16_t m = 0;
    uint32_t held_chords[PADMAP_MAX_ENT];
    int n_held = 0;
    for (int e = 0; e < ly->n_ent; e++) {
        const PadEnt *p = &ly->ent[e];
        if (!p->flags) continue;
        PadTapE *s = &st->e[li][e];
        if (act & (1ull << e)) {
            if (!s->down) s->down = 1, s->alone = 1, s->held = 0, s->n_held = 1, s->hframe = frame;
            else if (frame != s->hframe) { s->hframe = frame; if (s->n_held < 255) s->n_held++; }
            int other = (h & PM_PHYS & ~p->chord) || ((h & PM_ASTICK) && !(p->chord & PM_ANALOG));
            for (int f = 0; f < ly->n_ent && !other; f++)  /* a combination in effect that is not inside this one */
                if ((live & (1ull << f)) && !ly->ent[f].select_plain && (ly->ent[f].chord & p->chord & PM_PHYS) &&
                    !inside(ly->ent[f].chord, p->chord))
                    other = 1;
            if (other) s->alone = 0;                       /* something else was used: not a tap */
            if (s->alone && (p->flags & PF_HOLD) && s->n_held >= PADMAP_HOLD_FRAMES) s->held = 1, s->pulse = 0;
            if (s->held && s->alone) { m |= p->out; held_chords[n_held++] = p->chord; }
        } else if (s->down) {
            s->down = 0;
            if (s->alone && !s->held) s->pulse = 1, s->pframe = frame;
            s->held = 0;
        }
        if (s->pulse) {
            if (frame != s->pframe) s->pframe = frame, s->pulse++;
            if (s->pulse > PADMAP_TAP_FRAMES) s->pulse = 0;
            else m |= p->out;
        }
    }
    /* 3. the plain combinations' outputs (a held "+hold" combination silences the ones inside it) */
    int ls_analog = ly->in[PI_LSTICK].mode == RS_ANALOG, stick_used = 0;
    uint8_t rx = 0x80, ry = 0x80, lsx = ls_analog ? ax : 0x80, lsy = ls_analog ? ay : 0x80;
    int ldx = -1, ldy = -1;
    for (int e = 0; e < ly->n_ent; e++) {
        if (!(live & (1ull << e))) continue;
        const PadEnt *p = &ly->ent[e];
        int quiet = 0;
        for (int k = 0; k < n_held && !p->select_plain; k++)
            if (inside(p->chord, held_chords[k])) quiet = 1;
        if (quiet) continue;
        if (p->chord & PM_ANALOG) stick_used = 1;
        switch (p->dir) {
        case -1: m |= p->out; break;
        case 0: rx = 0x00; break;
        case 1: rx = 0xFF; break;
        case 2: ry = 0x00; break;
        case 3: ry = 0xFF; break;
        case 4:                                            /* <button>+analog right stick (stories: L + stick) */
            rx = pm->cam_inv & 1 ? (uint8_t)(ax ? 0x100 - ax : 0xFF) : ax;   /* camera_invert: around 0x80 */
            ry = pm->cam_inv & 2 ? (uint8_t)(ay ? 0x100 - ay : 0xFF) : ay;
            break;
        case 5: ldx = 0x00; break;
        case 6: ldx = 0xFF; break;
        case 7: ldy = 0x00; break;
        case 8: ldy = 0xFF; break;
        }
    }
    if (stick_used) lsx = lsy = 0x80;                      /* the analog stick works a combination */
    if (ldx >= 0) lsx = (uint8_t)ldx;
    if (ldy >= 0) lsy = (uint8_t)ldy;
    sticks[0] = rx;
    sticks[1] = ry;
    sticks[2] = lsx;
    sticks[3] = lsy;
    return m;
}
