// hle_menu.cpp - the in-game settings menu (see hle_menu.h). Dear ImGui through lib/psp_imgui, the game
// pack's settings registry (hle_settings_reg.h), the cfg table (hle_cfg.h), saving with comments kept (hle_cfg_save.h).
// No PSP calls here: the platform comes in through HleMenuHost (hle_menu_psp.c on the PSP, the host test's harness).
//
// Screen (480x272): header (title, unsaved count, "game paused") / category tabs (L / R) / the setting rows of the tab
// (D-pad Up / Down, Left / Right changes the value at once) / help line (marquee) / range + default + live line /
// button hints. Popups: Square = the whole help text wrapped; Circle with unsaved changes = save / keep / back.
// Values are handled as the player writes them in ps2rc.cfg (friendly text: "40", "half", "select+l").
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "psp_imgui.h"
extern "C" {
#include "hle_cfg.h"
#include "hle_cfg_save.h"
#include "hle_settings_reg.h"
#include "hle_menu.h"
}

extern "C" __attribute__((weak)) const HleSetting *hle_settings_table(int *n) { *n = 0; return NULL; }

namespace {

const int kVal = 32;                 // longest value text kept (cfg values of the menu's keys are short)
const int kMaxItems = 160;
const int kMaxTabs = 12;
const unsigned kAccent = IM_COL32(255, 190, 64, 255);
const unsigned kGood = IM_COL32(120, 220, 140, 255);
const unsigned kWarn = IM_COL32(255, 140, 90, 255);

struct Item {
    const HleSetting *s;             // registry line (NULL: a friendly name without one / an unknown file key)
    const HleCfgAlias *a;            // the friendly name's alias (NULL: a developer key)
    const char *key;                 // the name written into the file
    const char *label, *help, *unit;
    const char *choices[24];         // the values the menu steps through (empty: numeric)
    int nchoices;
    int lo, hi, step;                // numeric range (friendly)
    int tab;
    bool ro, numeric;
    char cur[kVal], open[kVal], saved[kVal];
    signed char live;                // last apply: 1 live, 0 after restart, -1 not changed yet
};

// per session (static: survives closing the menu; the pool does not) - the values of the first opening = the values
// the game booted with, for the "after restart" marks
char g_boot[kMaxItems][kVal];
int g_boot_n = -1;

struct Menu {
    const HleMenuHost *h;
    Item *it;
    int n;
    const char *tabs[kMaxTabs];
    int ntabs, tab_menu, tab, want_tab;
    int focus[kMaxTabs];
    int idx[kMaxItems];              // items of the current tab, in order
    int nidx;
    char *strpool;                   // copies of choice lists
    int pop, pop_focus;              // popup: 0 none, 1 help, 2 close
    float help_scroll;
    char msg[160];
    double msg_until;
    bool msg_bad;
    bool quit;
    int shot_req;
    HleMenuResult *res;
    // script
    const char *script;
    int sc_wait;
    unsigned sc_btn;
    int sc_phase;
    int shots;
} M;

void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (M.h && M.h->log) M.h->log(b);
}

void msgv(bool bad, const char *fmt, va_list ap)
{
    vsnprintf(M.msg, sizeof M.msg, fmt, ap);
    M.msg_until = ImGui::GetTime() + 3.5;
    M.msg_bad = bad;
}
void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void msg(const char *fmt, ...) { va_list ap; va_start(ap, fmt); msgv(false, fmt, ap); va_end(ap); }
void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void err(const char *fmt, ...) { va_list ap; va_start(ap, fmt); msgv(true, fmt, ap); va_end(ap); }

char *pool_strdup(const char *s, int n)
{
    char *d = (char *)ImGui::MemAlloc((size_t)n + 1);
    if (!d) return NULL;
    memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

// "a,b,c" -> choices (deduplicated); for an alias's "!w=n,..." words: one word per number
void add_choices(Item &x, const char *list, bool alias_words)
{
    if (!list) return;
    const char *p = list + (alias_words && list[0] == '!');
    int nums[24];
    while (*p && x.nchoices < 24) {
        const char *e = strchr(p, ',');
        int len = e ? (int)(e - p) : (int)strlen(p);
        int wl = len, num = 0;
        if (alias_words) {
            const char *eq = (const char *)memchr(p, '=', (size_t)len);
            if (eq) { wl = (int)(eq - p); num = atoi(eq + 1); }
        }
        bool dup = false;
        for (int i = 0; i < x.nchoices && alias_words; i++) if (nums[i] == num) dup = true;
        if (!dup && wl > 0) {
            nums[x.nchoices] = num;
            x.choices[x.nchoices++] = pool_strdup(p, wl);
        }
        if (!e) break;
        p = e + 1;
    }
}

// the number of a friendly text ("off" -> 0 through the alias words); 1 = it is a number
bool to_num(const Item &x, const char *v, int *out)
{
    char *end;
    long l = strtol(v, &end, 10);
    if (end != v && !*end) { *out = (int)l; return true; }
    if (x.a && x.a->words) {
        size_t lv = strlen(v);
        for (const char *p = x.a->words + (x.a->words[0] == '!'); *p;) {
            const char *eq = strchr(p, '='), *cm = strchr(p, ',');
            if (!eq) break;
            if ((size_t)(eq - p) == lv && !strncmp(p, v, lv)) { *out = atoi(eq + 1); return true; }
            if (!cm) break;
            p = cm + 1;
        }
    }
    return false;
}

// a number as the player writes it: the alias word for it if there is one ("0" -> "off")
void num_text(const Item &x, int v, char *buf, int n)
{
    snprintf(buf, (size_t)n, "%d", v);
    if (!x.a || !x.a->words) return;
    for (const char *p = x.a->words + (x.a->words[0] == '!'); *p;) {
        const char *eq = strchr(p, '='), *cm = strchr(p, ',');
        if (!eq) break;
        if (atoi(eq + 1) == v && eq > p && !(eq - p == 1 && p[0] >= '0' && p[0] <= '9')) {
            snprintf(buf, (size_t)n, "%.*s", (int)(eq - p), p);
            return;
        }
        if (!cm) break;
        p = cm + 1;
    }
}

// the effective value now (the cfg table, else the code default) as friendly text
void value_now(const Item &x, char *buf, int n)
{
    if (x.a) {
        int r = hle_cfg_friendly(x.key, buf, n);
        if (r >= 0 && buf[0]) return;
        snprintf(buf, (size_t)n, "%s", x.nchoices ? x.choices[0] : "");   // a word key not in the file: built-in
        return;
    }
    const char *v = hle_cfg_get(x.key);
    snprintf(buf, (size_t)n, "%s", v ? v : x.s && x.s->def ? x.s->def : "");
}

// the code default as friendly text (alias def, inverted / as a word where the alias says so)
void default_text(const Item &x, char *buf, int n)
{
    if (x.a && !(x.a->lo == 0 && x.a->hi == 0)) {
        int v = x.a->def;
        if (x.a->inv) v = x.a->lo + x.a->hi - v;
        num_text(x, v, buf, n);
        if (x.nchoices) {                         // a choice key: the default must be one of its choices
            for (int i = 0; i < x.nchoices; i++) if (!strcmp(x.choices[i], buf)) return;
            int dv;
            for (int i = 0; i < x.nchoices; i++)
                if (to_num(x, x.choices[i], &dv) && dv == v) { snprintf(buf, (size_t)n, "%s", x.choices[i]); return; }
        }
        return;
    }
    if (x.s && x.s->def) { snprintf(buf, (size_t)n, "%s", x.s->def); return; }
    snprintf(buf, (size_t)n, "%s", x.nchoices ? x.choices[0] : "");
}

int tab_of(const char *cat)
{
    for (int i = 0; i < M.ntabs; i++) if (!strcmp(M.tabs[i], cat)) return i;
    if (M.ntabs >= kMaxTabs - 1) return M.ntabs - 1;
    M.tabs[M.ntabs] = cat;
    return M.ntabs++;
}

bool is_alias_ikey(const char *k)
{
    int na = 0;
    const HleCfgAlias *at = hle_cfg_alias_table(&na);
    for (int i = 0; i < na; i++) if (!strcmp(at[i].key, k)) return true;
    return false;
}

void build_items(void)
{
    int nr = 0, na = 0;
    const HleSetting *reg = hle_settings_table(&nr);
    const HleCfgAlias *at = hle_cfg_alias_table(&na);
    M.it = (Item *)ImGui::MemAlloc(sizeof(Item) * kMaxItems);
    memset(M.it, 0, sizeof(Item) * kMaxItems);
    M.n = 0;
    M.ntabs = 0;
    for (int pass = 0; pass < 3; pass++) {
        int cnt = pass == 0 ? nr : pass == 1 ? na : hle_cfg_count();
        for (int i = 0; i < cnt && M.n < kMaxItems; i++) {
            Item &x = M.it[M.n];
            memset(&x, 0, sizeof x);
            if (pass == 0) {                                   // the registry
                if (reg[i].flags & HS_HIDE) continue;
                x.s = &reg[i];
                x.key = reg[i].key;
                x.a = hle_cfg_alias(x.key);
                if (!x.a && !(reg[i].flags & HS_DEV)) continue;     // a player key this build does not have
                x.label = reg[i].label ? reg[i].label : x.key;
                x.help = reg[i].help ? reg[i].help : "";
                x.unit = reg[i].unit;
                x.tab = tab_of(reg[i].cat ? reg[i].cat : "Other");
                x.ro = (reg[i].flags & HS_RO) != 0;
            } else if (pass == 1) {                            // friendly names without a registry line
                bool have = false;
                for (int j = 0; j < nr && !have; j++) have = !strcmp(reg[j].key, at[i].name);
                if (have) continue;
                x.key = at[i].name;
                x.a = &at[i];
                x.label = x.key;
                x.help = "A setting of this game (no menu description yet). Applies after a restart.";
                x.tab = tab_of("Other");
            } else {                                           // file keys nobody describes: shown, not edited
                const char *k = hle_cfg_key(i);
                bool have = is_alias_ikey(k);
                for (int j = 0; j < nr && !have; j++) have = !strcmp(reg[j].key, k);
                if (have) continue;
                x.key = k;
                x.label = k;
                x.help = "A key of the cfg files that the menu does not describe: change it in the file.";
                x.tab = tab_of("Developer");
                x.ro = true;
            }
            // value model: choices (registry list, else the alias's "!" words), else numeric (alias range / lo..hi)
            if (x.s && x.s->choices) add_choices(x, x.s->choices, false);
            else if (x.a && x.a->words && x.a->words[0] == '!') add_choices(x, x.a->words, true);
            x.step = x.s && x.s->step > 0 ? x.s->step : 1;
            if (x.a && !(x.a->lo == 0 && x.a->hi == 0)) { x.lo = x.a->lo; x.hi = x.a->hi; }
            else if (x.s) { x.lo = x.s->lo; x.hi = x.s->hi; }
            x.numeric = !x.nchoices && x.hi > x.lo;
            if (!x.numeric && !x.nchoices && !x.ro) x.ro = true;   // a word key without choices: file only
            value_now(x, x.cur, kVal);
            memcpy(x.open, x.cur, kVal);
            memcpy(x.saved, x.cur, kVal);
            x.live = -1;
            M.n++;
        }
    }
    M.tab_menu = tab_of("Menu");
    if (g_boot_n < 0) {                                        // first opening of the session
        g_boot_n = M.n;
        for (int i = 0; i < M.n; i++) memcpy(g_boot[i], M.it[i].cur, kVal);
    }
}

bool restart_mark(const Item &x, int i)
{
    if (i >= g_boot_n || x.ro) return false;
    if (!strcmp(x.cur, g_boot[i])) return false;
    return !(x.s && x.s->apply) || x.live == 0;
}

int unsaved_count(void)
{
    int c = 0;
    for (int i = 0; i < M.n; i++) c += strcmp(M.it[i].cur, M.it[i].saved) != 0;
    return c;
}

// store + apply one value; false = the cfg reader refused it (the old value stays)
bool set_value(int i, const char *v)
{
    Item &x = M.it[i];
    if (!strcmp(x.cur, v)) return true;
    char old[kVal];
    memcpy(old, x.cur, kVal);
    if (!hle_cfg_set(x.key, v)) { err("%s: %s is not accepted", x.label, v); return false; }
    value_now(x, x.cur, kVal);                                 // as the reader sees it ("0" -> "off")
    if (!strcmp(old, x.cur)) return true;                      // the same value written another way
    const char *ikey = x.a ? x.a->key : x.key, *ival = hle_cfg_get(ikey);
    x.live = 0;
    if (x.s && x.s->apply && ival) x.live = (signed char)(x.s->apply(ikey, ival) ? 1 : 0);
    if (M.res) { M.res->changes++; if (x.live) M.res->live++; else M.res->restart++; }
    logf("menu: %s %s -> %s (%s=%s, %s)", x.key, old, x.cur, ikey, ival ? ival : "-", x.live ? "live" : "after restart");
    return true;
}

void step_value(int i, int dir)
{
    Item &x = M.it[i];
    if (x.ro) return;
    char buf[kVal];
    if (x.nchoices) {
        int c = -1;
        for (int k = 0; k < x.nchoices; k++) if (!strcmp(x.choices[k], x.cur)) c = k;
        if (c < 0) {                                           // a file value outside the list: by number if possible
            int v, cv;
            if (to_num(x, x.cur, &v))
                for (int k = 0; k < x.nchoices; k++) if (to_num(x, x.choices[k], &cv) && cv == v) c = k;
        }
        c = c < 0 ? (dir > 0 ? 0 : x.nchoices - 1) : (c + dir + x.nchoices) % x.nchoices;
        set_value(i, x.choices[c]);
        return;
    }
    if (!x.numeric) return;
    int v;
    if (!to_num(x, x.cur, &v)) v = x.lo;
    int nv = v + dir * x.step;
    if (x.step > 1 && dir != 0 && (v - x.lo) % x.step) nv = dir > 0 ? v + (x.step - (v - x.lo) % x.step) : v - (v - x.lo) % x.step;
    if (nv < x.lo) nv = x.lo;
    if (nv > x.hi) nv = x.hi;
    if (x.hi - x.lo == 1 && dir != 0) nv = v == x.lo ? x.hi : x.lo;   // an on / off key: Left / Right toggles
    num_text(x, nv, buf, kVal);
    set_value(i, buf);
}

void reset_item(int i)
{
    Item &x = M.it[i];
    if (x.ro) return;
    char buf[kVal];
    if (x.s && x.s->rec) snprintf(buf, kVal, "%s", x.s->rec);
    else default_text(x, buf, kVal);
    if (set_value(i, buf)) msg("%s: %s (%s)", x.label, x.cur, x.s && x.s->rec ? "recommended" : "built-in default");
}

void revert_all(void)
{
    int c = 0;
    for (int i = 0; i < M.n; i++)
        if (!M.it[i].ro && strcmp(M.it[i].cur, M.it[i].open)) { char v[kVal]; memcpy(v, M.it[i].open, kVal); c += set_value(i, v); }
    msg(c ? "Reverted %d change(s) made since the menu opened" : "Nothing to revert%.0d", c);
}

void preset_all(bool recommended)
{
    int c = 0;
    for (int i = 0; i < M.n; i++) {
        Item &x = M.it[i];
        if (x.ro || !x.a || (x.s && (x.s->flags & HS_DEV))) continue;   // player settings only
        char buf[kVal];
        if (recommended) { if (!x.s || !x.s->rec) continue; snprintf(buf, kVal, "%s", x.s->rec); }
        else default_text(x, buf, kVal);
        if (strcmp(buf, x.cur)) c += set_value(i, buf);
    }
    msg("%s: %d setting(s) changed", recommended ? "Recommended values" : "Original console values", c);
}

// save: player file (existing lines), developer file (existing + new developer lines), player file (new lines)
bool save_all(void)
{
    HleCfgEdit e[kMaxItems];
    int ne = 0;
    for (int i = 0; i < M.n && ne < kMaxItems; i++) {
        Item &x = M.it[i];
        if (x.ro || !strcmp(x.cur, x.saved)) continue;
        e[ne].name = x.key;
        e[ne].ikey = x.a ? x.a->key : NULL;
        e[ne].value = x.cur;
        if (x.s && (x.s->flags & HS_UNSET) && x.nchoices && !strcmp(x.cur, x.choices[0]))
            e[ne].value = NULL;                    // "not set": its lines are commented out
        e[ne].dev = x.s && (x.s->flags & HS_DEV) ? 1 : 0;
        e[ne].done = 0;
        ne++;
    }
    if (!ne) { msg("No unsaved changes"); return true; }
    if (!M.h->read_file || !M.h->write_file) { err("Saving is not available here"); return false; }
    // the buffers follow the file size: 32 KB first, doubled up to 128 KB when a file does not fit
    // (a fixed 24 KB refused the 0.2.0 Russian template, 24.3 KB, as "too long")
    bool ok = false, too_long = false, no_mem = false;
    int wrote[2] = {0, 0};
    int nplayer = 0, ndev = 0;
    for (int cap = 32 * 1024; cap <= 128 * 1024; cap *= 2) {
        char *mem = M.h->tmp_alloc ? (char *)M.h->tmp_alloc(3u * cap + 16384u) : NULL;   /* outside ImGui's pool */
        char *in = mem, *out = mem ? mem + cap : NULL, *out2 = mem ? mem + 2 * cap + 8192 : NULL;
        ok = mem != NULL;
        no_mem = !ok;
        too_long = false;
        nplayer = ndev = 0;
        for (int i = 0; i < ne; i++) e[i].done = 0;
        if (ok) {
            // 1. player file, existing lines only (kept in out2 for step 3)
            int n0 = M.h->read_file(0, in, cap);
            if (n0 < 0) n0 = 0;
            int p1 = n0 < cap ? hle_cfg_rewrite(in, n0, e, ne, 0, out2, cap + 8192) : -1;
            if (p1 < 0) ok = false, too_long = true;   /* does not fit: retried with bigger buffers */
            // 2. developer file: existing lines + new developer lines
            bool dev_need = false;
            for (int i = 0; i < ne; i++) if (!e[i].done) dev_need = true;
            if (ok && dev_need) {
                int n1 = M.h->read_file(1, in, cap);
                if (n1 < 0) n1 = 0;
                int before = 0;
                for (int i = 0; i < ne; i++) before += e[i].done;
                int p2 = n1 < cap ? hle_cfg_rewrite(in, n1, e, ne, 2, out, cap + 8192) : -1;
                int after = 0;
                for (int i = 0; i < ne; i++) after += e[i].done;
                ndev = after - before;
                if (p2 < 0) ok = false, too_long = true;
                else if (ndev) { ok = M.h->write_file(1, out, p2) == 0; wrote[1] = ok; }
            }
            // 3. player file: new player lines
            if (ok) {
                int p3 = hle_cfg_rewrite(out2, p1, e, ne, 1, out, cap + 8192);
                int after = 0;
                for (int i = 0; i < ne; i++) after += e[i].done;
                nplayer = after - ndev;
                if (p3 < 0) ok = false, too_long = true;
                else if (nplayer) { ok = M.h->write_file(0, out, p3) == 0; wrote[0] = ok; }
            }
        }
        if (mem) M.h->tmp_free(mem);
        if (ok || !too_long || wrote[1]) break;      /* done, a write failed, or the developer file is already written */
    }
    if (!ok) {
        if (no_mem) err("Could not save: not enough free memory for the file buffers");
        else if (too_long) err("Could not save: the settings file is too long (over 128 KB)");
        else err("Could not save: the settings file could not be written (memory stick full or locked?)");
        logf("menu: save FAILED (player written %d, dev written %d, too long %d, no memory %d)", wrote[0], wrote[1],
             too_long, no_mem);
        return false;
    }
    int restart = 0;
    for (int i = 0; i < M.n; i++) {
        Item &x = M.it[i];
        if (x.ro || !strcmp(x.cur, x.saved)) continue;
        memcpy(x.saved, x.cur, kVal);
        restart += restart_mark(x, i);
    }
    if (M.res) M.res->saves++;
    logf("menu: saved %d setting(s): %s %d, %s %d", ne, hle_cfg_file(0), nplayer, hle_cfg_file(1), ndev);
    if (restart) msg("Saved %d change(s). %d apply after a restart.", ne, restart);
    else msg("Saved %d change(s) to %s", ne, hle_cfg_file(0));
    return true;
}

// ------------------------------------------------------------------ input: the pad, or the automatic script
unsigned script_buttons(void)
{
    if (M.sc_wait > 0) { M.sc_wait--; return M.sc_phase == 1 ? M.sc_btn : 0; }
    if (M.sc_phase == 1) { M.sc_phase = 2; M.sc_wait = 3; return 0; }     // release for 4 frames
    M.sc_phase = 0;
    const char *p = M.script;
    while (*p == ',' || *p == ' ') p++;
    if (!*p) { M.quit = true; M.script = p; return 0; }
    const char *e = p;
    while (*e && *e != ',') e++;
    char t[16];
    snprintf(t, sizeof t, "%.*s", (int)(e - p), p);
    M.script = e;
    static const struct { const char *n; unsigned b; } names[] = {
        {"U", PIMGUI_UP}, {"D", PIMGUI_DOWN}, {"L", PIMGUI_LEFT}, {"R", PIMGUI_RIGHT}, {"X", PIMGUI_CROSS},
        {"O", PIMGUI_CIRCLE}, {"T", PIMGUI_TRIANGLE}, {"S", PIMGUI_SQUARE}, {"LT", PIMGUI_L}, {"RT", PIMGUI_R},
        {"ST", PIMGUI_START}, {"SE", PIMGUI_SELECT}};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(t, names[i].n)) { M.sc_btn = names[i].b; M.sc_phase = 1; M.sc_wait = 2; return M.sc_btn; }
    if (t[0] == 'w') { M.sc_wait = atoi(t + 1); M.sc_phase = 2; return 0; }
    if (!strcmp(t, "shot")) { M.shot_req = ++M.shots; M.sc_wait = 2; M.sc_phase = 2; return 0; }
    if (!strcmp(t, "close")) { M.quit = true; return 0; }
    logf("menu script: unknown token '%s'", t);
    return 0;
}

// ------------------------------------------------------------------ drawing
void tab_items(int t)
{
    M.nidx = 0;
    for (int i = 0; i < M.n; i++) if (M.it[i].tab == t) M.idx[M.nidx++] = i;
}

void range_text(const Item &x, char *b, int n)
{
    char d[kVal];
    default_text(x, d, kVal);
    int o = 0;
    if (x.ro) o = snprintf(b, (size_t)n, "Shown only - change it in %s.", x.s && (x.s->flags & HS_DEV) ? hle_cfg_file(1) : "the cfg file");
    else if (x.numeric) {
        char lo[kVal], hi[kVal];
        num_text(x, x.lo, lo, kVal);
        num_text(x, x.hi, hi, kVal);
        o = snprintf(b, (size_t)n, "%s..%s%s  default %s", lo, hi, x.unit ? x.unit : "", d);
    } else o = snprintf(b, (size_t)n, "%d choices  default %s", x.nchoices, d);
    if (!x.ro && x.s && x.s->rec && o < n) o += snprintf(b + o, (size_t)(n - o), "  recommended %s", x.s->rec);
    if (!x.ro && o < n) snprintf(b + o, (size_t)(n - o), "  |  %s", x.s && x.s->apply ? "LIVE" : "applies after a restart");
}

void draw_footer(const Item *x, double ft)
{
    ImGui::Separator();
    double now = ImGui::GetTime();
    if (M.msg[0] && now < M.msg_until) pimgui_marquee(M.msg, 0, 1, now - (M.msg_until - 3.5), M.msg_bad ? kWarn : kGood);
    else pimgui_marquee(x ? x->help : "", 0, 1, ft, 0);
    char info[200] = "";
    if (x) range_text(*x, info, sizeof info);
    else if (M.tab == M.tab_menu) snprintf(info, sizeof info, "Settings: %s   developer: %s", hle_cfg_file(0), hle_cfg_file(1));
    pimgui_marquee(info, 0, 1, ft, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    pimgui_hint(PIMGUI_LEFT | PIMGUI_RIGHT, "change");
    pimgui_hint(PIMGUI_CIRCLE, "back");
    pimgui_hint(PIMGUI_TRIANGLE, "reset");
    pimgui_hint(PIMGUI_SQUARE, "help");
    pimgui_icon(PIMGUI_L);
    ImGui::SameLine(0, 2);
    pimgui_hint(PIMGUI_R, "tab");
    pimgui_hint(PIMGUI_START, "save");
    ImGui::NewLine();
}

void draw_menu_tab(float h)
{
    static const char *const act[] = {"Save to the settings file", "Revert changes (since the menu opened)",
                                      "Recommended values (all player settings)", "Original console values",
                                      "Close the menu"};
    pimgui_rows_begin("menu_rows", &M.focus[M.tab], h);
    char v[64];
    int uns = unsaved_count();
    snprintf(v, sizeof v, uns ? "%d unsaved" : "all saved", uns);
    if (pimgui_row(act[0], v, -1, 0, uns ? "*" : NULL, 0) == PIMGUI_ROW_ACTIVATE) save_all();
    if (pimgui_row(act[1], NULL, -1, 0, NULL, 0) == PIMGUI_ROW_ACTIVATE) revert_all();
    if (pimgui_row(act[2], NULL, -1, 0, NULL, 0) == PIMGUI_ROW_ACTIVATE) preset_all(true);
    if (pimgui_row(act[3], NULL, -1, 0, NULL, 0) == PIMGUI_ROW_ACTIVATE) preset_all(false);
    if (pimgui_row(act[4], NULL, -1, 0, NULL, 0) == PIMGUI_ROW_ACTIVATE) {
        if (unsaved_count()) { M.pop = 2; M.pop_focus = 0; }
        else M.quit = true;
    }
    pimgui_row("Settings file", hle_cfg_file(0), -1, 0, NULL, 1);
    pimgui_row("Developer file", hle_cfg_file(1), -1, 0, NULL, 1);
    PimguiStats st;
    pimgui_stats(&st);
    snprintf(v, sizeof v, "%u / %u KB", (st.pool_peak + 1023) / 1024, st.pool_bytes / 1024);
    pimgui_row("Menu memory (peak / pool)", v, -1, 0, NULL, 1);
    pimgui_row("Open this menu in game", "L + R + Cross + D-pad Down", -1, 0, NULL, 1);
    pimgui_row("Marks", "* unsaved   R after restart", -1, 0, NULL, 1);
    pimgui_rows_end();
}

void draw_popups(void)
{
    if (!M.pop) return;
    pimgui_block_input(0);                                     // the popup takes the keys the main window did not see
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.11f, 1.0f));
    if (M.pop == 1) {                                          // the whole help text, wrapped (never shrunk)
        const Item *x = M.nidx ? &M.it[M.idx[M.focus[M.tab] < M.nidx ? M.focus[M.tab] : 0]] : NULL;
        ImGui::SetNextWindowPos(ImVec2(24, 30));
        ImGui::SetNextWindowSize(ImVec2(432, 212));
        ImGui::Begin("##help", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", x ? x->label : "Help");
        ImGui::Separator();
        ImGui::BeginChild("##helptext", ImVec2(0, 160), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        if (x) {
            ImGui::TextWrapped("%s", x->help);
            ImGui::Spacing();
            char info[200];
            range_text(*x, info, sizeof info);
            ImGui::TextWrapped("%s", info);
            ImGui::TextDisabled("cfg key: %s%s%s", x->key, x->a ? "  (internal " : "", x->a ? x->a->key : "");
            if (x->a) { ImGui::SameLine(0, 0); ImGui::TextDisabled(")"); }
        } else {
            ImGui::TextWrapped("D-pad Up / Down: choose a setting. Left / Right: change it (applied at once where it "
                               "says LIVE). L / R: categories. Triangle: recommended value. START: save to the file. "
                               "Circle: close. Values marked R apply after a restart of the game.");
        }
        if (pimgui_repeat(PIMGUI_DOWN)) M.help_scroll += 15;
        if (pimgui_repeat(PIMGUI_UP)) M.help_scroll -= 15;
        if (M.help_scroll < 0) M.help_scroll = 0;
        if (M.help_scroll > ImGui::GetScrollMaxY()) M.help_scroll = ImGui::GetScrollMaxY();
        ImGui::SetScrollY(M.help_scroll);
        ImGui::EndChild();
        pimgui_hint(PIMGUI_CIRCLE, "close");
        pimgui_hint(PIMGUI_UP | PIMGUI_DOWN, "scroll");
        ImGui::End();
        if (pimgui_pressed(PIMGUI_CIRCLE | PIMGUI_SQUARE | PIMGUI_CROSS)) { M.pop = 0; pimgui_consume(0xFFFF); }
        pimgui_consume(0xFFFF);
        ImGui::PopStyleColor();
        return;
    }
    // close with unsaved changes
    ImGui::SetNextWindowPos(ImVec2(60, 70));
    ImGui::SetNextWindowSize(ImVec2(360, 112));
    ImGui::Begin("##close", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%d unsaved change(s)", unsaved_count());
    ImGui::Separator();
    pimgui_rows_begin("close_rows", &M.pop_focus, 58);
    int a0 = pimgui_row("Save to the settings file and close", NULL, -1, 0, NULL, 0);
    int a1 = pimgui_row("Close - keep the changes for this session only", NULL, -1, 0, NULL, 0);
    int a2 = pimgui_row("Back to the menu", NULL, -1, 0, NULL, 0);
    pimgui_rows_end();
    pimgui_hint(PIMGUI_CROSS, "choose");
    pimgui_hint(PIMGUI_CIRCLE, "back");
    ImGui::End();
    if (a0 == PIMGUI_ROW_ACTIVATE) { if (save_all()) M.quit = true; M.pop = 0; }
    else if (a1 == PIMGUI_ROW_ACTIVATE) { M.quit = true; M.pop = 0; logf("menu: closed with %d unsaved change(s) kept for this session", unsaved_count()); }
    else if (a2 == PIMGUI_ROW_ACTIVATE || pimgui_pressed(PIMGUI_CIRCLE)) M.pop = 0;
    pimgui_consume(0xFFFF);
    ImGui::PopStyleColor();
}

void frame(void)
{
    ImGuiIO &io = ImGui::GetIO();
    int pop_at_start = M.pop;
    if (pop_at_start) pimgui_block_input(1);                   // an open popup: the main window gets no keys
    // global keys first (a popup takes all keys itself)
    if (!M.pop) {
        if (pimgui_repeat(PIMGUI_L)) M.want_tab = (M.tab - 1 + M.ntabs) % M.ntabs;
        if (pimgui_repeat(PIMGUI_R)) M.want_tab = (M.tab + 1) % M.ntabs;
        if (M.want_tab >= 0) M.tab = M.want_tab;
    }
    tab_items(M.tab);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##ps2rc_menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "PS2RC settings");
    int uns = unsaved_count();
    char hdr[64];
    snprintf(hdr, sizeof hdr, uns ? "%d unsaved  |  game paused" : "game paused", uns);
    float hw = ImGui::CalcTextSize(hdr).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - hw - 8);
    ImGui::TextColored(uns ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.6f, 0.65f, 0.7f, 1.0f), "%s", hdr);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip)) {
        for (int t = 0; t < M.ntabs; t++) {
            ImGuiTabItemFlags f = M.want_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(M.tabs[t], NULL, f)) ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    M.want_tab = -1;
    float footer = 3 * ImGui::GetTextLineHeightWithSpacing() + 8;
    float h = ImGui::GetContentRegionAvail().y - footer;
    const Item *fx = NULL;
    double ft = 0;
    if (M.tab == M.tab_menu) {
        draw_menu_tab(h);
        ft = pimgui_rows_focus_time();
    } else {
        int *focus = &M.focus[M.tab];
        if (*focus >= M.nidx) *focus = M.nidx ? M.nidx - 1 : 0;
        pimgui_rows_begin("rows", focus, h);
        for (int r = 0; r < M.nidx; r++) {
            int i = M.idx[r];
            Item &x = M.it[i];
            char val[kVal + 8];
            float frac = -1.0f;
            int v;
            if (x.numeric && to_num(x, x.cur, &v)) {
                frac = x.hi > x.lo ? (float)(v - x.lo) / (float)(x.hi - x.lo) : 0.0f;
                if (x.hi - x.lo == 1) frac = -1.0f;
                bool word = !(x.cur[0] == '-' || (x.cur[0] >= '0' && x.cur[0] <= '9'));
                snprintf(val, sizeof val, "%s%s", x.cur, !word && x.unit ? x.unit : "");
            } else if (x.nchoices && x.unit) {
                bool word = !(x.cur[0] >= '0' && x.cur[0] <= '9');
                snprintf(val, sizeof val, "%s%s", x.cur, word ? "" : x.unit);
            } else snprintf(val, sizeof val, "%s", x.cur[0] ? x.cur : "-");
            char badge[4] = "";
            int bn = 0;
            if (strcmp(x.cur, x.saved)) badge[bn++] = '*';
            if (restart_mark(x, i)) badge[bn++] = 'R';
            badge[bn] = 0;
            int act = pimgui_row(x.label, val, frac, !x.ro, badge, x.ro);
            if (act == PIMGUI_ROW_LEFT) step_value(i, -1);
            else if (act == PIMGUI_ROW_RIGHT || act == PIMGUI_ROW_ACTIVATE) step_value(i, +1);
            if (r == *focus) fx = &x;
        }
        ft = pimgui_rows_focus_time();
        pimgui_rows_end();
        if (fx && !M.pop) {
            if (pimgui_pressed(PIMGUI_TRIANGLE)) reset_item((int)(fx - M.it));
        }
    }
    if (!M.pop) {
        if (pimgui_pressed(PIMGUI_SQUARE)) { M.pop = 1; M.help_scroll = 0; pimgui_consume(PIMGUI_SQUARE); }
        else if (pimgui_pressed(PIMGUI_START)) save_all();
        else if (pimgui_pressed(PIMGUI_SELECT)) revert_all();
        else if (pimgui_pressed(PIMGUI_CIRCLE)) {
            if (unsaved_count()) { M.pop = 2; M.pop_focus = 0; pimgui_consume(PIMGUI_CIRCLE); }
            else M.quit = true;
        }
    }
    draw_footer(fx, ft);
    if (M.pop) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));
    ImGui::End();
    if (pop_at_start) draw_popups();                           // a popup opened this frame starts next frame
}

}   // namespace

extern "C" int hle_menu_ui(const HleMenuHost *h, void *pool, unsigned pool_bytes, const char *script, HleMenuResult *res)
{
    if (!pimgui_create(pool, pool_bytes, PIMGUI_NAV_ROWS)) return -1;
    memset(&M, 0, sizeof M);
    M.h = h;
    M.res = res;
    M.want_tab = -1;
    M.script = script && *script ? script : NULL;
    if (res) memset(res, 0, sizeof *res);
    build_items();
    if (!M.script && h->buttons) pimgui_held_at_start(h->buttons());   // the opening L+R+Cross+Down is no press
    logf("menu: %d settings in %d categories (%s / %s)%s", M.n, M.ntabs, hle_cfg_file(0), hle_cfg_file(1),
         M.script ? " - automatic script" : "");
    double t_prev = h->seconds ? h->seconds() : 0;
    while (!M.quit) {
        unsigned b = M.script ? script_buttons() : h->buttons ? h->buttons() : 0;
        double t = h->seconds ? h->seconds() : t_prev + 1.0 / 60.0;
        float dt = (float)(t - t_prev);
        if (dt > 0.25f) dt = 0.25f;                            // a long stall: no jump of the marquee / repeat
        t_prev = t;
        if (h->frame_begin && !h->frame_begin()) break;
        pimgui_new_frame(b, dt);
        frame();
        pimgui_render();
        PimguiStats st;
        pimgui_stats(&st);
        if (res) {
            res->frames++;
            if (st.vertices > res->max_vertices) res->max_vertices = st.vertices;
            if (st.list_bytes > res->max_list_bytes) res->max_list_bytes = st.list_bytes;
        }
        int shot = M.shot_req;
        M.shot_req = 0;
        if (h->frame_end) h->frame_end(shot);
        if (h->idle) h->idle();
    }
    PimguiStats st;
    pimgui_stats(&st);
    if (res) {
        res->pool_peak = st.pool_peak;
        res->pool_bytes = st.pool_bytes;
        res->failed_allocs = st.failed;
    }
    pimgui_destroy();
    M.it = NULL;
    return 0;
}
