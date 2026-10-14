/* hle_mcfs_psp.c - PSP file layer of the memory card HLE (hle/core/hle_memcard.c, hle_memcard.h).
 *
 * ps2rc.cfg (next to the EBOOT):
 *   memcard=1|0        1 (default): a PS2 memory card in slot 1, 0: slot 1 empty (the old "not inserted" behaviour)
 *   memcard_dir=<dir>  the card's folder; default <data dir>memcard/slot1/ where <data dir> is disc_dir= when set
 *                      (thin A/B folders share the main folder's card) or the EBOOT's folder. A relative path is
 *                      taken relative to the EBOOT's folder.
 * Memory-stick rules (psp-developer real-hw-gotchas.md, streaming-architecture.md): nothing here runs per frame - only
 * the game's own card commands reach this file; at most ONE file is open at a time (the open-file limit: the disc
 * layer keeps up to 8 open and session.log opens per commit); a failed open that is not "file not found" asks the disc
 * layer to close its least recently used file and retries; data reaches the FAT (size) when the file is closed
 * (hle_plat_mc_release), which the core does before every rename.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_cfg.h"                 /* the shared ps2rc.cfg table */
#include "hle_memcard.h"

/* src/hle/psp/hle_psp.c: closes the disc layer's least recently used file (0 if none); weak so that this file links
   against an hle_psp.c without it */
int hle_psp_disc_close_lru(void) __attribute__((weak));

#define ERR_NOENT 0x80010002u

static uint64_t st_opens, st_bytes_r, st_bytes_w;
static unsigned st_open_retries;

static void strip_slash(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/' && p[n - 2] != ':') p[--n] = 0;
}

int hle_plat_mc_config(char *root, size_t n)
{
    char mdir[256] = "", ddir[256] = "";
    int on = 1;                                    /* default: a card (PPSSPP-verified 2026-10-05: detect, save, load) */
    {                                              /* the shared table (hle_cfg.c), whole file */
        const char *x;
        if ((x = hle_cfg_get("memcard"))) on = atoi(x);
        if ((x = hle_cfg_get("memcard_dir"))) snprintf(mdir, sizeof mdir, "%s", x);
        if ((x = hle_cfg_get("disc_dir"))) snprintf(ddir, sizeof ddir, "%s", x);
    }
    if (!on) { LOG("memcard: ps2rc.cfg memcard=0\n"); return 0; }
    if (mdir[0]) {
        int abs = strchr(mdir, ':') != NULL;
        snprintf(root, n, "%s%s%s", abs ? "" : hle_plat_disc_root(), mdir, mdir[strlen(mdir) - 1] == '/' ? "" : "/");
    } else if (ddir[0]) {
        snprintf(root, n, "%s%smemcard/slot1/", ddir, ddir[strlen(ddir) - 1] == '/' ? "" : "/");
    } else {
        snprintf(root, n, "%smemcard/slot1/", hle_plat_disc_root());
    }
    return 1;
}

/* A PS2 card stores Japan time (the PS2 RTC runs in JST; mcman stamps entries with it) and a game shows a save's date
   as JST - 9 h + the console's time zone (sceScfGetTimeZone: 0 with the HLE's GetOsdConfigParam). The FAT and the PSP
   clock keep local time, so local + 9 h goes to the game: its load list then shows the PSP's local time. */
static void from_psp(const ScePspDateTime *d, HleMcTime *t)
{
    ScePspDateTime j = *d;
    u64 tick;
    if (j.year >= 1980 && sceRtcGetTick(&j, &tick) >= 0 && sceRtcTickAddHours(&tick, &tick, 9) >= 0)
        sceRtcSetTick(&j, &tick);
    t->sec = (uint8_t)j.second;
    t->min = (uint8_t)j.minute;
    t->hour = (uint8_t)j.hour;
    t->day = (uint8_t)j.day;
    t->month = (uint8_t)j.month;
    t->year = j.year;
}

void hle_plat_mc_now(HleMcTime *t)
{
    ScePspDateTime pt;
    memset(&pt, 0, sizeof pt);
    if (sceRtcGetCurrentClockLocalTime(&pt) < 0) { pt.year = 2005; pt.month = 10; pt.day = 26; pt.hour = 12; }
    from_psp(&pt, t);
}

void hle_plat_mc_set_mtime(const char *path, const HleMcTime *t) { (void)path; (void)t; }   /* FAT stamps itself */

int hle_plat_mc_list(const char *dir, HleMcListFn fn, void *ctx)
{
    char d[320];
    snprintf(d, sizeof d, "%s", dir);
    strip_slash(d);
    SceUID fd = sceIoDopen(d);
    if (fd < 0) return -1;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(fd, &e) <= 0) break;
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
        int is_dir = FIO_S_ISDIR(e.d_stat.st_mode);
        if (!is_dir && !FIO_S_ISREG(e.d_stat.st_mode)) continue;
        HleMcTime mt, ct;
        from_psp(&e.d_stat.sce_st_mtime, &mt);
        from_psp(&e.d_stat.sce_st_ctime, &ct);
        fn(ctx, e.d_name, is_dir, is_dir ? 0 : (uint32_t)e.d_stat.st_size, &mt, &ct);
    }
    sceIoDclose(fd);
    return 0;
}

int hle_plat_mc_exists(const char *path)
{
    char p[320];
    snprintf(p, sizeof p, "%s", path);
    strip_slash(p);
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(p, &st) < 0) return 0;
    return FIO_S_ISDIR(st.st_mode) ? 2 : 1;
}

/* the one cached data fd */
static SceUID cfd = -1;
static int cfd_wr;
static int64_t cfd_pos;
static char cpath[320];

void hle_plat_mc_release(void)
{
    if (cfd >= 0) sceIoClose(cfd);
    cfd = -1;
    cpath[0] = 0;
}

static SceUID open_retry(const char *path, int flags)
{
    SceUID fd = sceIoOpen(path, flags, 0777);
    for (int tries = 0; fd < 0 && (uint32_t)fd != ERR_NOENT && tries < 4; tries++) {   /* the open-file limit */
        st_open_retries++;
        if (!(hle_psp_disc_close_lru && hle_psp_disc_close_lru())) sceKernelDelayThread(20000);   /* a log commit */
        fd = sceIoOpen(path, flags, 0777);
    }
    st_opens++;
    if (fd < 0) LOG("memcard: sceIoOpen(%s, %x) failed %08x (retries %u)\n", path, flags, (unsigned)fd, st_open_retries);
    return fd;
}

static SceUID cached(const char *path, int wr, int create)
{
    if (cfd >= 0 && !create && !strcmp(cpath, path) && (cfd_wr || !wr)) return cfd;
    hle_plat_mc_release();
    SceUID fd = open_retry(path, wr ? PSP_O_RDWR | (create ? PSP_O_CREAT | PSP_O_TRUNC : 0) : PSP_O_RDONLY);
    if (fd < 0) return -1;
    cfd = fd;
    cfd_wr = wr;
    cfd_pos = 0;
    snprintf(cpath, sizeof cpath, "%s", path);
    return fd;
}

static int seek_to(uint32_t off)
{
    if (cfd_pos == (int64_t)off) return 0;
    if (sceIoLseek(cfd, (SceOff)off, PSP_SEEK_SET) != (SceOff)off) { cfd_pos = -1; return -1; }
    cfd_pos = off;
    return 0;
}

int64_t hle_plat_mc_read(const char *path, uint32_t off, void *dst, uint32_t n)
{
    if (cached(path, 0, 0) < 0 || seek_to(off) < 0) return -1;
    int r = sceIoRead(cfd, dst, n);
    if (r < 0) { cfd_pos = -1; return -1; }
    cfd_pos += r;
    st_bytes_r += (uint64_t)r;
    return r;
}

int64_t hle_plat_mc_write(const char *path, uint32_t off, const void *src, uint32_t n, int create)
{
    if (cached(path, 1, create) < 0) return -1;
    if (!n) return 0;
    if (seek_to(off) < 0) return -1;
    int w = sceIoWrite(cfd, src, n);
    if (w < 0) { cfd_pos = -1; return -1; }
    cfd_pos += w;
    st_bytes_w += (uint64_t)w;
    return w;
}

static void stripped(const char *in, char *out, size_t n) { snprintf(out, n, "%s", in); strip_slash(out); }

int hle_plat_mc_mkdir(const char *path)
{
    char p[320];
    stripped(path, p, sizeof p);
    hle_plat_mc_release();
    int r = sceIoMkdir(p, 0777);
    return r < 0 ? r : 0;
}

int hle_plat_mc_rmdir(const char *path)
{
    char p[320];
    stripped(path, p, sizeof p);
    hle_plat_mc_release();
    int r = sceIoRmdir(p);
    return r < 0 ? r : 0;
}

int hle_plat_mc_remove(const char *path)
{
    hle_plat_mc_release();
    int r = sceIoRemove(path);
    return r < 0 ? r : 0;
}

int hle_plat_mc_rename(const char *from, const char *to)
{
    hle_plat_mc_release();
    int r = sceIoRename(from, to);
    return r < 0 ? r : 0;
}
