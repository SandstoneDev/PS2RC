/* hle_fast.c - the x_* speed switches (see hle_fast.h). */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared run-options table */
#include "hle_fast.h"
#ifdef HLE_PSP
#include <pspiofilemgr.h>
#endif

int hle_x_dma = -1;

#define XMAX 64
static struct { char name[24]; int val; } xv[XMAX];
static int nx = -1;

static void x_load(void)
{
    nx = 0;
#ifdef HLE_PSP
    for (int ci = 0; ci < hle_cfg_count(); ci++) {   /* the shared table (hle_cfg.c), whole file */
        const char *k = hle_cfg_key(ci);
        if (k[0] == 'x' && k[1] == '_' && strlen(k + 2) < sizeof xv[0].name && nx < XMAX) {
            strcpy(xv[nx].name, k + 2);
            xv[nx].val = (int)strtol(hle_cfg_val(ci), NULL, 0);
            nx++;
        }
    }
#endif
}

/* run-time overrides (an in-run interleaved A/B); checked before the cfg / environment value; empty unless
   hle_x_set was called */
int hle_x_gen;
#define XOV 24                                   /* 8 -> 24 (the menu's live x_ keys + an A/B) */
static struct { char name[24]; int val; } xo[XOV];
static int nxo;

int hle_x_set(const char *name, int val)
{
    for (int i = 0; i < nxo; i++)
        if (!strcmp(xo[i].name, name)) {
            if (xo[i].val != val) { xo[i].val = val; hle_x_gen++; }
            return 1;
        }
    if (nxo >= XOV || strlen(name) >= sizeof xo[0].name) return 0;
    strcpy(xo[nxo].name, name);
    xo[nxo].val = val;
    nxo++;
    hle_x_gen++;
    return 1;
}

int hle_x_opt(const char *name, int def)
{
    for (int i = 0; i < nxo; i++)
        if (!strcmp(xo[i].name, name)) return xo[i].val;
    if (nx < 0) x_load();
#ifdef HLE_PSP
    for (int i = 0; i < nx; i++)
        if (!strcmp(xv[i].name, name)) return xv[i].val;
    return def;
#else
    char env[40] = "HLE_X_";
    size_t k = 6;
    for (const char *p = name; *p && k < sizeof env - 1; p++) env[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    env[k] = 0;
    const char *e = getenv(env);
    return e && *e ? (int)strtol(e, NULL, 0) : def;
#endif
}
