/* cm_menu_main.c - host driver for tests/cfg/test_cm_menu.py.
 *   cm_menu_main rewrite <in file> <insert 0|1|2> name|ikey|value|dev ...   -> the new text on stdout, then
 *                                                                              "--done <flags>" (1 per edit)
 *   cm_menu_main set name=value ...                                         -> after each set: "ok|bad key=internal"
 *   cm_menu_main pad name=value ...                                         -> the sets, then the mapping as the pad
 *                                                                              code rebuilds it (hle_pad_reconfig):
 *                                                                              "foot: ...", "car: ...", "notes: ..."
 *   (rewrite: the value <unset> = NULL, the menu's "not set")
 *   cm_menu_main stress                                                     -> 20000 sets, table and notes intact
 *   cm_menu_main chord <value> ...                                          -> "hex why" per value (0 = refused)
 *   cm_menu_main chordseq < "BUTTONS(hex) [close]" lines                     -> "GAME(hex) [OPEN]" per read */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle_cfg.h"
#include "hle_cfg_save.h"
#include "hle_menu_chord.h"
#include "hle_padmap.h"

static int rewrite(int argc, char **argv)
{
    static char in[65536], out[80000];
    FILE *f = fopen(argv[2], "rb");
    int n = f ? (int)fread(in, 1, sizeof in, f) : 0;
    if (f) fclose(f);
    int ins = atoi(argv[3]);
    HleCfgEdit e[32];
    int ne = 0;
    for (int i = 4; i < argc && ne < 32; i++) {
        char *s = strdup(argv[i]), *p[4] = {0};
        int k = 0;
        p[k++] = s;
        for (char *q = s; *q && k < 4; q++) if (*q == '|') { *q = 0; p[k++] = q + 1; }
        e[ne].name = p[0];
        e[ne].ikey = p[1] && *p[1] ? p[1] : NULL;
        e[ne].value = p[2] ? (strcmp(p[2], "<unset>") ? p[2] : NULL) : "";
        e[ne].dev = p[3] ? atoi(p[3]) : 0;
        e[ne].done = 0;
        ne++;
    }
    int r = hle_cfg_rewrite(in, n, e, ne, ins, out, (int)sizeof out);
    if (r < 0) { printf("--overflow\n"); return 1; }
    fwrite(out, 1, (size_t)r, stdout);
    printf("--done ");
    for (int i = 0; i < ne; i++) printf("%d", e[i].done);
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "rewrite")) return rewrite(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "set")) {
        const char *notes0 = strdup(hle_cfg_notes());
        for (int i = 2; i < argc; i++) {
            char *s = strdup(argv[i]), *eq = strchr(s, '=');
            *eq = 0;
            int ok = hle_cfg_set(s, eq + 1);
            const HleCfgAlias *a = hle_cfg_alias(s);
            const char *k = a ? a->key : s, *v = hle_cfg_get(k);
            char fr[40] = "-";
            if (a) hle_cfg_friendly(s, fr, sizeof fr);
            printf("%s %s=%s friendly=%s\n", ok ? "ok" : "bad", k, v ? v : "-", fr);
        }
        printf("notes_same=%d\n", !strcmp(notes0, hle_cfg_notes()));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "pad")) {
        for (int i = 2; i < argc; i++) {
            char *s = strdup(argv[i]), *eq = strchr(s, '=');
            *eq = 0;
            if (!hle_cfg_set(s, eq + 1)) printf("bad %s\n", argv[i]);
        }
        static PadMap pm;
        padmap_config(&pm, hle_cfg_get);
        printf("foot: %s\ncar: %s\nnotes: %s\n", pm.line, pm.car_line, pm.notes);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "chord")) {
        for (int i = 2; i < argc; i++) {
            const char *why;
            unsigned m = mc_parse(argv[i], &why);
            printf("%04x %s\n", m, why);
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "chordseq")) {
        MenuChord mc = {MC_DEFAULT, 0, 0};
        char ln[64], cl[16];
        unsigned b;
        while (fgets(ln, sizeof ln, stdin)) {
            cl[0] = 0;
            if (sscanf(ln, "%x %15s", &b, cl) < 1) continue;
            if (!strcmp(cl, "close")) { mc_closed(&mc, b); continue; }
            int open;
            unsigned g = mc_filter(&mc, b, &open);
            printf("%04x%s\n", g, open ? " OPEN" : "");
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "stress")) {
        char v[32];
        int n0 = hle_cfg_count();
        for (int i = 0; i < 20000; i++) {
            snprintf(v, sizeof v, "%d", (i * 7919) % 100000);
            if (!hle_cfg_set(i & 1 ? "draw_distance" : "zz_long_test_key", i & 1 ? (i & 2 ? "55" : "100") : v)) {
                printf("set failed at %d\n", i);
                return 1;
            }
        }
        const char *a = hle_cfg_get("tune_far"), *z = hle_cfg_get("zz_long_test_key"), *d = hle_cfg_get("d");
        printf("count %d->%d tune_far=%s zz=%s d=%s\n", n0, hle_cfg_count(), a ? a : "-", z ? z : "-", d ? d : "-");
        return 0;
    }
    return 2;
}
