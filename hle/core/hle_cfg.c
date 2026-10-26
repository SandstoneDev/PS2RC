/* hle_cfg.c - one shared reader of the run options file (the pack's HLE_PACK_CFG, e.g. ps2rc.cfg) + the player's
 * friendly setting names (aliases of the internal keys). Rules: hle_cfg.h. Every module walks this table instead of
 * opening the file itself: the file may be as long as the player likes.
 *
 * Memory: a static pool (HLE_CFG_POOL bytes) for the key / value strings of the lines that are not comments, and a
 * static index (HLE_CFG_MAX keys); the file is streamed through a 512-byte stack chunk. No malloc (the first caller
 * is the automatic overclock at the first line of main(), before the heap / log exist).
 * Standalone (tests): -DHLE_CFG_STANDALONE compiles this file without hle.h (host stdio).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hle_cfg.h"
#ifdef HLE_CFG_STANDALONE
#define LOG(...) printf(__VA_ARGS__)
#define HLE_PACK_CFG "ps2rc.cfg"
#else
#include "hle.h"
#endif
#ifdef HLE_PSP
#include <pspiofilemgr.h>
#endif

#define HLE_CFG_POOL 8192
#define HLE_CFG_MAX  256
#define HLE_CFG_LINE 1024                          /* longest accepted line (the longest in any cfg so far: 256) */

static char pool[HLE_CFG_POOL];
static int pool_n;
static struct { unsigned short k, v; } tab[HLE_CFG_MAX];
static int n_tab = -1, found, dropped;
static char notes[512];
static int notes_n;

static void note(const char *fmt, const char *a, const char *b, const char *c)
{
    if (notes_n >= (int)sizeof notes - 2) return;
    int r = snprintf(notes + notes_n, sizeof notes - (size_t)notes_n, fmt, a, b, c);
    if (r > 0) notes_n += r;
    if (notes_n >= (int)sizeof notes) notes_n = (int)sizeof notes - 1;
}

/* ------------------------------------------------------------------ the friendly names (player settings)
   The table is the game's (the game pack's hle_settings.c); a game without one gets the weak empty
   table below (no friendly names, the reader works the same). Fields: hle_cfg.h HleCfgAlias. */
typedef HleCfgAlias Alias;
__attribute__((weak)) const HleCfgAlias *hle_cfg_alias_table(int *n) { *n = 0; return NULL; }
static const Alias *aliases;
static int N_ALIAS = -1;
static void aliases_get(void) { if (N_ALIAS < 0) aliases = hle_cfg_alias_table(&N_ALIAS); }

static const Alias *alias_of(const char *k)
{
    aliases_get();
    for (int i = 0; i < N_ALIAS; i++)
        if (!strcmp(aliases[i].name, k)) return &aliases[i];
    return NULL;
}

/* the word's number in "w=n,w=n" (1 = found); a leading '!' is skipped */
static int word_num(const char *words, const char *v, int *out)
{
    size_t lv = strlen(v);
    for (const char *p = words + (words[0] == '!'); *p;) {
        const char *eq = strchr(p, '='), *end = strchr(p, ',');
        if (!eq) break;
        if ((size_t)(eq - p) == lv && !strncmp(p, v, lv)) { *out = atoi(eq + 1); return 1; }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* friendly value text of an internal value (for the log line) */
static void friendly(const Alias *a, const char *iv, char *buf, size_t n)
{
    char *end;
    long x = strtol(iv, &end, 10);
    if (end == iv || *end || (a->lo == 0 && a->hi == 0)) { snprintf(buf, n, "%s", iv); return; }
    if (a->inv) {
        if (x < a->lo || x > a->hi) { snprintf(buf, n, "internal:%s", iv); return; }
        x = a->lo + a->hi - x;
    }
    snprintf(buf, n, "%ld", x);
    if (!a->words) return;
    for (const char *p = a->words + (a->words[0] == '!'); *p;) {
        const char *eq = strchr(p, '='), *cm = strchr(p, ',');
        if (!eq) break;
        if (atoi(eq + 1) == x) { snprintf(buf, n, "%.*s", (int)(eq - p), p); return; }
        if (!cm) break;
        p = cm + 1;
    }
}

/* ------------------------------------------------------------------ the table */
static int find(const char *k)
{
    for (int i = 0; i < n_tab; i++)
        if (!strcmp(pool + tab[i].k, k)) return i;
    return -1;
}

static int put_str(const char *s)                  /* copy into the pool: offset, -1 = full */
{
    size_t n = strlen(s) + 1;
    if (pool_n + (int)n > HLE_CFG_POOL) return -1;
    memcpy(pool + pool_n, s, n);
    pool_n += (int)n;
    return pool_n - (int)n;
}

/* the pool can fill up when the in-game menu changes values many times: the strings the table still
   uses are moved down (in address order) and the space of replaced values is reused */
static void pool_compact(void)
{
    static unsigned short *ref[2 * HLE_CFG_MAX];
    int n = 0;
    for (int i = 0; i < n_tab; i++) { ref[n++] = &tab[i].k; ref[n++] = &tab[i].v; }
    for (int i = 1; i < n; i++)                    /* insertion sort by offset */
        for (int j = i; j > 0 && *ref[j - 1] > *ref[j]; j--) { unsigned short *t = ref[j]; ref[j] = ref[j - 1]; ref[j - 1] = t; }
    int w = 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(pool + *ref[i]) + 1;
        if (*ref[i] != w) memmove(pool + w, pool + *ref[i], (size_t)len);
        *ref[i] = (unsigned short)w;
        w += len;
    }
    pool_n = w;
}

static int rt_set;                                 /* inside hle_cfg_set (the menu), not the file load */
static int set_kv(const char *k, const char *v)    /* 1 = stored */
{
    int i = find(k);
    if (i >= 0) {                                  /* the later line wins */
        if (!strcmp(pool + tab[i].v, v)) return 1;
        if (rt_set && strlen(v) <= strlen(pool + tab[i].v)) { strcpy(pool + tab[i].v, v); return 1; }
        int vo = put_str(v);
        if (vo < 0 && rt_set) { pool_compact(); vo = put_str(v); }
        if (vo < 0) { dropped++; return 0; }
        tab[i].v = (unsigned short)vo;
        return 1;
    }
    if (n_tab >= HLE_CFG_MAX) { dropped++; return 0; }
    if (rt_set && pool_n + (int)strlen(k) + (int)strlen(v) + 2 > HLE_CFG_POOL) pool_compact();
    int ko = put_str(k), vo = ko < 0 ? -1 : put_str(v);
    if (vo < 0) { if (ko >= 0) pool_n = ko; dropped++; return 0; }
    tab[n_tab].k = (unsigned short)ko;
    tab[n_tab].v = (unsigned short)vo;
    n_tab++;
    return 1;
}

static int line_in(char *s)                        /* 1 = stored */
{
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#' || *s == ';') return 0;
    char *eq = strchr(s, '=');
    if (!eq || eq == s) return 0;
    char *ke = eq;
    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    *ke = 0;
    for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    for (char *p = v; *p; p++)                     /* " # text" after the value is a comment */
        if ((*p == '#' || *p == ';') && p > v && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = 0;

    const Alias *a = alias_of(s);
    if (!a) return set_kv(s, v);
    if (a->lo == 0 && a->hi == 0) {                                  /* word key, stored as given */
        if (a->words && a->words[0] == '!') {                        /* ... but only one of its words */
            int x;
            for (char *p = v; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
            if (!word_num(a->words, v, &x)) { note("%s=%s is not a valid value (ignored)%s; ", s, v, ""); return 0; }
        }
        return set_kv(a->key, v);
    }
    for (char *p = v; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    int x;
    char num[16];
    if (!(a->words && word_num(a->words, v, &x))) {
        char *end;
        long l = strtol(v, &end, 10);
        if (end == v || *end || (a->words && a->words[0] == '!')) {
            note("%s=%s is not a valid value (ignored)%s; ", s, v, "");
            return 0;
        }
        x = (int)l;
        if (x < a->lo || x > a->hi) {
            int c = x < a->lo ? a->lo : a->hi;
            snprintf(num, sizeof num, "%d", c);
            note("%s=%s clamped to %s; ", s, v, num);
            x = c;
        }
    }
    if (a->inv) x = a->lo + a->hi - x;
    snprintf(num, sizeof num, "%d", x);
    return set_kv(a->key, num);
}

#ifndef HLE_PSP
static void host_export(void)
{
    /* the key prefixes whose host readers take environment variables: the core's x_ + the pack's ([runtime]
       cfg_env_prefixes -> hle_pack.h) */
#ifdef HLE_PACK_CFG_ENV_PREFIXES
    static const char *const pre[] = {"x_", HLE_PACK_CFG_ENV_PREFIXES};
#else
    static const char *const pre[] = {"x_"};
#endif
    for (int i = 0; i < n_tab; i++) {
        const char *k = pool + tab[i].k;
        int ok = 0;
        for (unsigned j = 0; j < sizeof pre / sizeof pre[0]; j++)
            if (!strncmp(k, pre[j], strlen(pre[j]))) ok = 1;
        if (!ok) continue;
        char env[64] = "HLE_";
        size_t e = 4;
        for (const char *p = k; *p && e < sizeof env - 1; p++) env[e++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        env[e] = 0;
        setenv(env, pool + tab[i].v, 0);          /* an explicit environment variable wins */
    }
}
#endif

/* one file into the table; 1 = it was there */
static int load_file(const char *path)
{
#ifdef HLE_PSP
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
#endif
    static char line[HLE_CFG_LINE];
    int ln = 0, too_long = 0, first = 1;
    for (;;) {
        char chunk[512];
#ifdef HLE_PSP
        int n = sceIoRead(fd, chunk, sizeof chunk);
#else
        int n = (int)fread(chunk, 1, sizeof chunk, fp);
#endif
        if (n <= 0) break;
        int i = 0;
        if (first && n >= 3 && (unsigned char)chunk[0] == 0xEF && (unsigned char)chunk[1] == 0xBB &&
            (unsigned char)chunk[2] == 0xBF) i = 3;                     /* UTF-8 BOM (Windows Notepad) */
        first = 0;
        for (; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (ln) { line[ln] = 0; if (!too_long) line_in(line); }
                ln = 0;
                too_long = 0;
            } else if (ln < HLE_CFG_LINE - 1) line[ln++] = c;
            else too_long = 1;
        }
    }
    if (ln && !too_long) { line[ln] = 0; line_in(line); }
#ifdef HLE_PSP
    sceIoClose(fd);
#else
    fclose(fp);
#endif
    return 1;
}

/* the player's file holds only player settings: the developer / run options live in a second file
   next to it, read FIRST - <cfg>_dev<.ext>, then the cfg (a key in both: the cfg wins, the later line).
   Older file names (the pack's HLE_PACK_CFG_LEGACY, e.g. the name a folder converted by an earlier version still
   has): each of the two files falls back to its older name only when the current name is not there.
   PSP: the EBOOT's folder. Host: HLE_CFG_DIR (a folder, the same names), then HLE_CFG_DEV, then HLE_CFG. */
static int found_dev, found_main;
static char dev_name[64] = "dev cfg", main_name[64] = HLE_PACK_CFG;   /* the names that were read (log line) */
static void dev_of(const char *cfg, char *out, size_t n)          /* <name>_dev<.ext> */
{
    const char *dot = strrchr(cfg, '.');
    snprintf(out, n, "%.*s_dev%s", (int)(dot ? dot - cfg : (int)strlen(cfg)), cfg, dot ? dot : "");
}
static int load_named(const char *root, const char *name, char *got, size_t n)
{
    char path[320];
    snprintf(path, sizeof path, "%s%s", root, name);
    if (!load_file(path)) return 0;
    snprintf(got, n, "%s", name);
    return 1;
}
static void load_dir(const char *root)
{
    char nm[64];
    dev_of(HLE_PACK_CFG, nm, sizeof nm);
    found_dev = load_named(root, nm, dev_name, sizeof dev_name);
#ifdef HLE_PACK_CFG_LEGACY
    if (!found_dev) {
        dev_of(HLE_PACK_CFG_LEGACY, nm, sizeof nm);
        found_dev = load_named(root, nm, dev_name, sizeof dev_name);
    }
#endif
    found_main = load_named(root, HLE_PACK_CFG, main_name, sizeof main_name);
#ifdef HLE_PACK_CFG_LEGACY
    if (!found_main) found_main = load_named(root, HLE_PACK_CFG_LEGACY, main_name, sizeof main_name);
#endif
}
static void load(void)
{
    if (n_tab >= 0) return;
    n_tab = 0;
#ifdef HLE_PSP
    load_dir(hle_plat_disc_root());
#else
    const char *e = getenv("HLE_CFG_DIR");
    if (e && *e) {
        char root[256];
        size_t l = strlen(e);
        snprintf(root, sizeof root, "%s%s", e, l && e[l - 1] != '/' && e[l - 1] != '\\' ? "/" : "");
        load_dir(root);
    }
    e = getenv("HLE_CFG_DEV");
    if (e && *e && load_file(e)) { found_dev = 1; snprintf(dev_name, sizeof dev_name, "%s", e); }
    e = getenv("HLE_CFG");
    if (e && *e && load_file(e)) { found_main = 1; snprintf(main_name, sizeof main_name, "%s", e); }
    host_export();
#endif
    found = found_dev || found_main;
}

#ifndef HLE_PSP
__attribute__((constructor)) static void host_load_early(void) { load(); }   /* before any module's getenv */
#endif

int hle_cfg_count(void) { load(); return n_tab; }
const char *hle_cfg_key(int i) { load(); return i >= 0 && i < n_tab ? pool + tab[i].k : ""; }
const char *hle_cfg_val(int i) { load(); return i >= 0 && i < n_tab ? pool + tab[i].v : ""; }
int hle_cfg_found(void) { load(); return found; }
const char *hle_cfg_get(const char *key)
{
    load();
    int i = find(key);
    return i >= 0 ? pool + tab[i].v : NULL;
}

const char *hle_cfg_notes(void)
{
    load();
    static char out[640];
    snprintf(out, sizeof out, "%s%s", notes, dropped ? "TABLE FULL: some keys were dropped; " : "");
    return out;
}

const char *hle_cfg_settings_line(void)
{
    load();
    static char out[1024];
    int o = snprintf(out, sizeof out, "settings:");
    aliases_get();
    for (int i = 0; i < N_ALIAS && o < (int)sizeof out - 48; i++) {
        const Alias *a = &aliases[i];
        if (a->inv < 0) continue;                  /* logged by its own module */
        const char *v = hle_cfg_get(a->key);
        char buf[40], dv[16];
        if (v) friendly(a, v, buf, sizeof buf);
        else if (a->lo == 0 && a->hi == 0) snprintf(buf, sizeof buf, "-*");
        else {
            snprintf(dv, sizeof dv, "%d", a->def);
            friendly(a, dv, buf, sizeof buf - 1);
            strcat(buf, "*");
        }
        o += snprintf(out + o, sizeof out - (size_t)o, " %s=%s", a->name, buf);
    }
    return out;
}

void hle_cfg_report(void)
{
    load();
    LOG("%s (* = not in the cfg file, code default)\n", hle_cfg_settings_line());
    const char *nt = hle_cfg_notes();
    if (*nt) LOG("settings notes: %s\n", nt);
    /* the internal key table, sorted (two runs whose cfg means the same thing print the same lines) */
    int idx[HLE_CFG_MAX];
    for (int i = 0; i < n_tab; i++) idx[i] = i;
    for (int i = 1; i < n_tab; i++)
        for (int j = i; j > 0 && strcmp(pool + tab[idx[j - 1]].k, pool + tab[idx[j]].k) > 0; j--) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    char out[1100];
    int o = 0, part = 1;
    for (int i = 0; i <= n_tab; i++) {
        const char *k = i < n_tab ? pool + tab[idx[i]].k : NULL, *v = k ? pool + tab[idx[i]].v : NULL;
        if (i == n_tab || o + (int)strlen(k) + (int)strlen(v) + 3 > 1000) {
            if (o || i == n_tab)
                LOG("cfg keys %d (%s%s%s, %d keys, pool %d of %d B): %s\n", part++,
                    found_dev ? dev_name : found_main ? "" : "no file", found_dev && found_main ? " + " : "",
                    found_main ? main_name : found_dev ? " only" : "", n_tab, pool_n, HLE_CFG_POOL, o ? out : "-");
            o = 0;
            if (i == n_tab) break;
        }
        o += snprintf(out + o, sizeof out - (size_t)o, "%s%s=%.200s", o ? " " : "", k, v);
    }
}

/* ------------------------------------------------------------------ run-time changes (the in-game menu)
   hle_cfg_set: one "name = value" as if it were the file's last line (friendly names clamped / converted to their
   internal key, words checked); the boot notes are not touched. 1 = stored, 0 = not a valid value / table full. */
int hle_cfg_set(const char *name, const char *value)
{
    load();
    char line[HLE_CFG_LINE];
    if (snprintf(line, sizeof line, "%s=%s", name, value) >= (int)sizeof line) return 0;
    int nn = notes_n, dr = dropped;
    rt_set = 1;
    int ok = line_in(line);
    rt_set = 0;
    notes_n = nn;                                  /* the boot notes stay as they were */
    notes[nn] = 0;
    dropped = dr;
    return ok;
}

const HleCfgAlias *hle_cfg_alias(const char *name) { return alias_of(name); }

int hle_cfg_friendly(const char *name, char *buf, int n)
{
    load();
    const Alias *a = alias_of(name);
    if (!a || n <= 0) { if (n > 0) buf[0] = 0; return -1; }
    const char *v = hle_cfg_get(a->key);
    if (v) { friendly(a, v, buf, (size_t)n); return 1; }
    if (a->lo == 0 && a->hi == 0) { buf[0] = 0; return 0; }
    char dv[16];
    snprintf(dv, sizeof dv, "%d", a->def);
    friendly(a, dv, buf, (size_t)n);
    return 0;
}

const char *hle_cfg_file(int dev)
{
    load();
    static char def_dev[64];
    if (dev) {
        if (found_dev) return dev_name;
        dev_of(HLE_PACK_CFG, def_dev, sizeof def_dev);
        return def_dev;
    }
    return found_main ? main_name : HLE_PACK_CFG;
}
int hle_cfg_file_found(int dev) { load(); return dev ? found_dev : found_main; }
