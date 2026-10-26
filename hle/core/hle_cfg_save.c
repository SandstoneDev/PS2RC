/* hle_cfg_save.c - settings written back into the cfg text, comments and layout kept. Rules: hle_cfg_save.h. */
#include <string.h>
#include "hle_cfg_save.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* key of an active line [s, e) (reader rules: spaces, '#'/';' comments, "key = value"): 1 + its span */
static int line_key(const char *s, const char *e, const char **k0, const char **k1)
{
    const char *p = s;
    if (e - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if (p >= e || *p == '#' || *p == ';') return 0;
    const char *eq = memchr(p, '=', (size_t)(e - p));
    if (!eq || eq == p) return 0;
    const char *ke = eq;
    while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    *k0 = p;
    *k1 = ke;
    return 1;
}

static int key_is(const char *k0, const char *k1, const char *name)
{
    if (!name) return 0;
    size_t n = strlen(name);
    if ((size_t)(k1 - k0) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)k0[i]) != lower((unsigned char)name[i])) return 0;
    return 1;
}

/* a commented-out line of this name: "#name=..." / "# name = ..." */
static int commented_is(const char *s, const char *e, const char *name)
{
    const char *p = s;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if (p >= e || (*p != '#' && *p != ';')) return 0;
    p++;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    size_t n = strlen(name);
    if ((size_t)(e - p) < n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)p[i]) != lower((unsigned char)name[i])) return 0;
    p += n;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    return p < e && *p == '=';
}

typedef struct { char *o; int n, cap, bad; } Out;
static void put(Out *w, const char *s, int n)
{
    if (n <= 0) return;
    if (w->n + n > w->cap) { w->bad = 1; return; }
    memcpy(w->o + w->n, s, (size_t)n);
    w->n += n;
}
static void puts_(Out *w, const char *s) { put(w, s, (int)strlen(s)); }

int hle_cfg_rewrite(const char *in, int n, HleCfgEdit *e, int ne, int insert, char *out, int cap)
{
    enum { MAXE = 128 };
    int last[MAXE], after[MAXE];                   /* line index of the edit's last active / commented line */
    if (ne > MAXE) ne = MAXE;
    for (int i = 0; i < ne; i++) last[i] = after[i] = -1;
    int crlf = 0, nl = 0;
    for (int i = 0; i + 1 < n; i++) if (in[i] == '\r' && in[i + 1] == '\n') { crlf = 1; break; }
    /* pass 1: which line each edit owns */
    for (int p = 0, li = 0; p < n; li++) {
        const char *s = in + p, *le = memchr(s, '\n', (size_t)(n - p));
        int len = le ? (int)(le - s) + 1 : n - p;
        const char *e1 = s + len;
        while (e1 > s && (e1[-1] == '\n' || e1[-1] == '\r')) e1--;
        const char *k0, *k1;
        int act = line_key(s, e1, &k0, &k1);
        for (int i = 0; i < ne; i++) {
            if (e[i].done || !e[i].value) continue;           /* an unset edit owns no line (pass 2 comments) */
            if (act && (key_is(k0, k1, e[i].name) || key_is(k0, k1, e[i].ikey))) last[i] = li;
            else if (!act && commented_is(s, e1, e[i].name)) after[i] = li;
        }
        p += len;
        nl = li + 1;
    }
    (void)nl;
    Out w = {out, 0, cap, 0};
    int pending_tail = 0;                          /* edits inserted at the end */
    for (int i = 0; i < ne; i++) {
        if (e[i].done || last[i] >= 0) continue;
        int mine = insert == 1 ? !e[i].dev : insert == 2 ? e[i].dev : 0;
        if (!mine || !e[i].value) { after[i] = -1; continue; }
        if (after[i] < 0) pending_tail++;
    }
    int unset_hit[MAXE];                           /* an unset edit commented a line in this file */
    for (int i = 0; i < ne; i++) unset_hit[i] = 0;
    const char *eol = crlf ? "\r\n" : "\n";
    int ended_nl = n == 0 || in[n - 1] == '\n';
    /* pass 2: copy, replacing owned lines, inserting after commented ones */
    for (int p = 0, li = 0; p < n; li++) {
        const char *s = in + p, *le = memchr(s, '\n', (size_t)(n - p));
        int len = le ? (int)(le - s) + 1 : n - p;
        const char *e1 = s + len;
        while (e1 > s && (e1[-1] == '\n' || e1[-1] == '\r')) e1--;
        int replaced = 0;
        {                                          /* unset: every active line of the setting is commented out */
            const char *k0, *k1;
            if (line_key(s, e1, &k0, &k1))
                for (int i = 0; i < ne && !replaced; i++) {
                    if (e[i].value || (e[i].done && !unset_hit[i])) continue;
                    if (!key_is(k0, k1, e[i].name) && !key_is(k0, k1, e[i].ikey)) continue;
                    const char *b = s;
                    if (e1 - b >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
                        (unsigned char)b[2] == 0xBF) b += 3;
                    put(&w, s, (int)(b - s));      /* the BOM stays first */
                    puts_(&w, "#");
                    put(&w, b, (int)((s + len) - b));
                    unset_hit[i] = 1;
                    e[i].done = 1;
                    replaced = 1;
                }
        }
        for (int i = 0; i < ne && !replaced; i++) {
            if (e[i].done || last[i] != li) continue;
            const char *k0, *k1;
            line_key(s, e1, &k0, &k1);
            const char *eq = memchr(k0, '=', (size_t)(e1 - k0));
            const char *v = eq + 1;
            while (v < e1 && (*v == ' ' || *v == '\t')) v++;
            const char *ve = v;                    /* the value ends at " #" / " ;" or the trailing spaces */
            const char *cm = e1;
            for (const char *q = v; q < e1; q++)
                if ((*q == '#' || *q == ';') && q > v && (q[-1] == ' ' || q[-1] == '\t')) { cm = q; break; }
            ve = cm;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            put(&w, s, (int)(k0 - s));             /* BOM / indentation */
            puts_(&w, e[i].name);
            put(&w, k1, (int)(v - k1));            /* " = " as it was */
            puts_(&w, e[i].value);
            put(&w, ve, (int)((s + len) - ve));    /* spaces + comment + line end */
            e[i].done = 1;
            replaced = 1;
        }
        if (!replaced) put(&w, s, len);
        for (int i = 0; i < ne; i++) {
            if (e[i].done || after[i] != li) continue;
            if (len && s[len - 1] != '\n') puts_(&w, eol);   /* the last line had no line end */
            puts_(&w, e[i].name);
            puts_(&w, "=");
            puts_(&w, e[i].value);
            puts_(&w, eol);
            e[i].done = 1;
        }
        p += len;
    }
    for (int i = 0; i < ne; i++) {                 /* an unset setting without a line: nothing to write */
        if (e[i].value || e[i].done) continue;
        if ((insert == 1 && !e[i].dev) || (insert == 2 && e[i].dev)) e[i].done = 1;
    }
    if (pending_tail) {
        int last_nl = w.n == 0 || w.o[w.n - 1] == '\n';
        if (!last_nl) puts_(&w, eol);
        (void)ended_nl;
        int have_hdr = 0;                          /* the header from an earlier save */
        {
            const char *h = HLE_CFG_MENU_HEADER;
            size_t hl = strlen(h);
            for (int p = 0; p + (int)hl <= n; p++)
                if ((p == 0 || in[p - 1] == '\n') && !memcmp(in + p, h, hl)) { have_hdr = 1; break; }
        }
        if (!have_hdr) {
            puts_(&w, HLE_CFG_MENU_HEADER);
            puts_(&w, eol);
        }
        for (int i = 0; i < ne; i++) {
            if (e[i].done || last[i] >= 0) continue;
            int mine = insert == 1 ? !e[i].dev : insert == 2 ? e[i].dev : 0;
            if (!mine) continue;
            puts_(&w, e[i].name);
            puts_(&w, "=");
            puts_(&w, e[i].value);
            puts_(&w, eol);
            e[i].done = 1;
        }
    }
    return w.bad ? -1 : w.n;
}
