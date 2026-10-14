/* hle_mcfs_host.c - host (Linux/WSL) file layer of the memory card HLE (hle/core/hle_memcard.c, hle_memcard.h).
 * The card is the directory of the harness option --memcard DIR (no option: slot 1 empty, the "no memory card"
 * behaviour). Timestamps are deterministic: the clock is 2005-10-26 12:00:00 (the HLE's sceCdReadClock date) plus
 * the virtual time, and every file the game writes gets that time as its modification time, so a later run lists the
 * same dates. Compiled into the PSP build too (every C file of src/hle is), where it is empty.
 */
#ifndef HLE_PSP
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "hle.h"
#include "hle_memcard.h"

const char *hle_mc_host_dir;              /* --memcard DIR */

int hle_plat_mc_config(char *root, size_t n)
{
    if (!hle_mc_host_dir || !hle_mc_host_dir[0]) return 0;
    snprintf(root, n, "%s%s", hle_mc_host_dir, hle_mc_host_dir[strlen(hle_mc_host_dir) - 1] == '/' ? "" : "/");
    return 1;
}

static void from_tm(const struct tm *tm, HleMcTime *t)
{
    t->sec = (uint8_t)tm->tm_sec;
    t->min = (uint8_t)tm->tm_min;
    t->hour = (uint8_t)tm->tm_hour;
    t->day = (uint8_t)tm->tm_mday;
    t->month = (uint8_t)(tm->tm_mon + 1);
    t->year = (uint16_t)(tm->tm_year + 1900);
}

static time_t base_time(void)
{
    struct tm b = {0};
    b.tm_year = 2005 - 1900;
    b.tm_mon = 9;
    b.tm_mday = 26;
    b.tm_hour = 12;
    return timegm(&b);
}

void hle_plat_mc_now(HleMcTime *t)
{
    time_t s = base_time() + (time_t)(hle_vtime / EE_HZ);
    struct tm tm;
    gmtime_r(&s, &tm);
    from_tm(&tm, t);
}

void hle_plat_mc_set_mtime(const char *path, const HleMcTime *t)
{
    struct tm tm = {0};
    tm.tm_year = t->year - 1900;
    tm.tm_mon = t->month - 1;
    tm.tm_mday = t->day;
    tm.tm_hour = t->hour;
    tm.tm_min = t->min;
    tm.tm_sec = t->sec;
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = timegm(&tm);
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(path, tv);
}

int hle_plat_mc_list(const char *dir, HleMcListFn fn, void *ctx)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        if (stat(p, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        struct tm tm;
        HleMcTime mt, ct;
        gmtime_r(&st.st_mtime, &tm);
        from_tm(&tm, &mt);
        ct = mt;                                           /* Linux has no creation time: the modification time */
        fn(ctx, e->d_name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size, &mt, &ct);
    }
    closedir(d);
    return 0;
}

int hle_plat_mc_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

/* the one cached data fd */
static int cfd = -1, cfd_wr;
static char cpath[1024];

void hle_plat_mc_release(void)
{
    if (cfd >= 0) close(cfd);
    cfd = -1;
    cpath[0] = 0;
}

static int cached(const char *path, int wr, int create)
{
    if (cfd >= 0 && !create && !strcmp(cpath, path) && (cfd_wr || !wr)) return cfd;
    hle_plat_mc_release();
    int fd = wr ? open(path, O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644) : open(path, O_RDONLY);
    if (fd < 0) return -1;
    cfd = fd;
    cfd_wr = wr;
    snprintf(cpath, sizeof cpath, "%s", path);
    return fd;
}

int hle_plat_mc_mkdir(const char *path) { hle_plat_mc_release(); return mkdir(path, 0755) == 0 ? 0 : -errno; }
int hle_plat_mc_rmdir(const char *path) { hle_plat_mc_release(); return rmdir(path) == 0 ? 0 : -errno; }
int hle_plat_mc_remove(const char *path) { hle_plat_mc_release(); return unlink(path) == 0 ? 0 : -errno; }
int hle_plat_mc_rename(const char *from, const char *to)
{
    hle_plat_mc_release();
    if (getenv("HLE_MC_NO_RENAME")) return -EPERM;        /* tests: the core's copy fallback */
    if (hle_plat_mc_exists(to)) return -EEXIST;           /* the PSP's FAT refuses it too */
    return rename(from, to) == 0 ? 0 : -errno;
}

int64_t hle_plat_mc_read(const char *path, uint32_t off, void *dst, uint32_t n)
{
    int fd = cached(path, 0, 0);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, dst, n, (off_t)off);
    return r < 0 ? -1 : (int64_t)r;
}

int64_t hle_plat_mc_write(const char *path, uint32_t off, const void *src, uint32_t n, int create)
{
    int fd = cached(path, 1, create);
    if (fd < 0) return -1;
    if (!n) return 0;
    ssize_t w = pwrite(fd, src, n, (off_t)off);
    return w < 0 ? -1 : (int64_t)w;
}
#endif
