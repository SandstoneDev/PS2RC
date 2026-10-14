/* mc_unit.c - unit test of the memory card HLE (hle/core/hle_memcard.c + the host file layer hle/host/hle_mcfs_host.c)
 * outside a game: the libmc calls are made the way games make them (command, then sceMcSync(0, &cmd, &result)) on
 * a card folder in a temporary directory, and the folder is checked on the host file system.
 *
 *   mc_unit <mode> <card dir>      modes: nocard | basic | recover | unformatted | full | norename
 * Prints "PASS <mode>" (exit 0) or the first failed check (exit 1). Built and run by tests/memcard/test_memcard.py.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "hle.h"
#include "hle_memcard.h"

/* ------------------------------------------------------------------ the parts of the HLE core the module uses */
uint8_t *hle_ram;
uint32_t hle_ram_end;
uint64_t hle_vtime;
int hle_in_intr;
int hle_verbose;
static uint64_t charged;                       /* virtual time the module charged */
void hle_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void hle_time_charge(uint64_t cycles) { charged += cycles; hle_vtime += cycles; }
void hle_safe_point(R5900Ctx *c, uint64_t cost) { (void)c; hle_time_charge(cost); }
void hle_trace_call(const char *name, uint32_t addr, R5900Ctx *c) { (void)name; (void)addr; (void)c; }
double hle_host_seconds(void) { return 0.0; }
void *gptr(uint32_t a, uint32_t n) { if (a + n > hle_ram_end) { printf("FAIL gptr %08x+%u\n", a, n); exit(1); } return hle_ram + a; }
const char *gstr(uint32_t a) { return (const char *)hle_ram + a; }
void gcopy_out(uint32_t dst, const void *src, uint32_t n) { memcpy(gptr(dst, n), src, n); }
void gcopy_in(void *dst, uint32_t src, uint32_t n) { memcpy(dst, gptr(src, n), n); }

/* the hooks under test (no hle_bind.h here: every hook name is unbound, so HLE_SCE(x) defines hle_fn_x) */
void hle_fn_sceMcInit(R5900Ctx *c);
void hle_fn_sceMcGetInfo(R5900Ctx *c);
void hle_fn_sceMcSync(R5900Ctx *c);
void hle_fn_sceMcOpen(R5900Ctx *c);
void hle_fn_sceMcClose(R5900Ctx *c);
void hle_fn_sceMcSeek(R5900Ctx *c);
void hle_fn_sceMcRead(R5900Ctx *c);
void hle_fn_sceMcWrite(R5900Ctx *c);
void hle_fn_sceMcMkdir(R5900Ctx *c);
void hle_fn_sceMcChdir(R5900Ctx *c);
void hle_fn_sceMcGetDir(R5900Ctx *c);
void hle_fn_sceMcDelete(R5900Ctx *c);
void hle_fn_sceMcFormat(R5900Ctx *c);
void hle_fn_sceMcGetEntSpace(R5900Ctx *c);

/* ------------------------------------------------------------------ test helpers */
static R5900Ctx ctx;
static const char *mode;
static char card[512];
#define G_STR   0x1000u                        /* guest scratch: strings */
#define G_OUT   0x2000u                        /* guest scratch: type/free/format/cmd/result words */
#define G_TBL   0x3000u                        /* guest scratch: GetDir table (24 x 64) */
#define G_BUF   0x10000u                       /* guest scratch: data buffer (1 MB) */

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s line %d: ", mode, __LINE__); printf(__VA_ARGS__); \
                                             printf("\n"); exit(1); } } while (0)

static int call(void (*fn)(R5900Ctx *), int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < 8; i++) ctx.r[4 + i].d[0] = 0;
    for (int i = 0; i < n; i++) ctx.r[4 + i].d[0] = (uint64_t)(int64_t)(int32_t)va_arg(ap, uint32_t);
    va_end(ap);
    fn(&ctx);
    return (int32_t)ctx.r[2].d[0];
}
static uint32_t str(const char *s, int slot) { uint32_t a = G_STR + 256u * (uint32_t)slot; strcpy((char *)hle_ram + a, s); return a; }
/* sceMcSync(0, &cmd, &result) -> result; checks the command number and that the sync itself returned 1 */
static int sync_res(int want_cmd)
{
    int r = call(hle_fn_sceMcSync, 3, 0u, G_OUT + 12, G_OUT + 16);
    CHECK(r == 1, "sceMcSync returned %d, want 1 (finished)", r);
    CHECK((int)gw(G_OUT + 12) == want_cmd, "sync cmd %d, want %d", (int)gw(G_OUT + 12), want_cmd);
    return (int)gw(G_OUT + 16);
}
static int info(int port, uint32_t *type, uint32_t *fr, uint32_t *fmt)
{
    int r = call(hle_fn_sceMcGetInfo, 5, (uint32_t)port, 0u, G_OUT, G_OUT + 4, G_OUT + 8);
    CHECK(r == 0, "sceMcGetInfo returned %d", r);
    int res = sync_res(1);
    *type = gw(G_OUT);
    *fr = gw(G_OUT + 4);
    *fmt = gw(G_OUT + 8);
    return res;
}
static int mc_open(const char *name, int flags) { call(hle_fn_sceMcOpen, 4, 0u, 0u, str(name, 0), (uint32_t)flags); return sync_res(2); }
static int mc_close(int fd) { call(hle_fn_sceMcClose, 1, (uint32_t)fd); return sync_res(3); }
static int mc_seek(int fd, int off, int wh) { call(hle_fn_sceMcSeek, 3, (uint32_t)fd, (uint32_t)off, (uint32_t)wh); return sync_res(4); }
static int mc_read(int fd, uint32_t buf, int n) { call(hle_fn_sceMcRead, 3, (uint32_t)fd, buf, (uint32_t)n); return sync_res(5); }
static int mc_write(int fd, uint32_t buf, int n) { call(hle_fn_sceMcWrite, 3, (uint32_t)fd, buf, (uint32_t)n); return sync_res(6); }
static int mc_mkdir(const char *name) { call(hle_fn_sceMcMkdir, 3, 0u, 0u, str(name, 0)); return sync_res(11); }
static int mc_chdir(const char *name) { call(hle_fn_sceMcChdir, 4, 0u, 0u, str(name, 0), 0u); return sync_res(12); }
static int mc_getdir(const char *name, int flags, int maxent)
{
    call(hle_fn_sceMcGetDir, 6, 0u, 0u, str(name, 0), (uint32_t)flags, (uint32_t)maxent, G_TBL);
    return sync_res(13);
}
static int mc_delete(const char *name) { call(hle_fn_sceMcDelete, 3, 0u, 0u, str(name, 0)); return sync_res(15); }
static int mc_format(void) { call(hle_fn_sceMcFormat, 2, 0u, 0u); return sync_res(16); }
static int mc_entspace(const char *name) { call(hle_fn_sceMcGetEntSpace, 3, 0u, 0u, str(name, 0)); return sync_res(18); }
static const char *ent_name(int i) { return (const char *)hle_ram + G_TBL + 64u * (uint32_t)i + 32u; }
static uint32_t ent_size(int i) { return gw(G_TBL + 64u * (uint32_t)i + 16u); }
static unsigned ent_attr(int i) { return gw(G_TBL + 64u * (uint32_t)i + 20u) & 0xFFFFu; }

static long host_size(const char *rel)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", card, rel);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static int host_has_tmp(const char *rel)                /* any "_ps2rc" file left in a host directory */
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", card, rel);
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) if (strstr(e->d_name, "_ps2rc_tmp") || strstr(e->d_name, "_ps2rc_new")) n++;
    closedir(d);
    return n;
}
static void host_write(const char *rel, const char *data, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", card, rel);
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL, "cannot create %s", p);
    fwrite(data, 1, n, f);
    fclose(f);
}
static void fill(uint32_t a, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) hle_ram[a + i] = (uint8_t)((i * 131u + seed * 7u) >> 3);
}

/* ------------------------------------------------------------------ modes */
static void t_nocard(void)
{
    uint32_t type, fr, fmt;
    hle_mc_host_dir = NULL;
    CHECK(call(hle_fn_sceMcSync, 3, 0u, G_OUT + 12, G_OUT + 16) == -1, "sync with nothing registered must return -1");
    CHECK(call(hle_fn_sceMcInit, 1, 0u) == 0, "sceMcInit");
    int r = info(0, &type, &fr, &fmt);
    CHECK(r == -10 && type == 0 && fr == 0 && fmt == 0, "no card: result %d type %u free %u format %u", r, type, fr, fmt);
    CHECK(mc_open("/x", 1) == -10, "open without a card");
    CHECK(mc_getdir("/*", 0, 24) == -10, "getdir without a card");
    CHECK(charged == 0, "no card must not charge virtual time (%llu)", (unsigned long long)charged);
    CHECK(!hle_mc_inserted, "no card inserted");
}

static void t_basic(void)
{
    uint32_t type, fr, fmt;
    hle_mc_host_dir = card;
    CHECK(call(hle_fn_sceMcInit, 1, 0u) == 0, "sceMcInit");
    CHECK(hle_mc_inserted, "card inserted");
    int r = info(0, &type, &fr, &fmt);
    CHECK(r == -1 && type == 2 && fr == 8000 && fmt == 1, "new card: result %d type %u free %u format %u", r, type, fr, fmt);
    r = info(0, &type, &fr, &fmt);
    CHECK(r == 0, "same card: result %d", r);
    r = info(1, &type, &fr, &fmt);
    CHECK(r == -10 && type == 0, "slot 2 is empty: %d %u", r, type);
    /* a second command while one is pending: refused with the pending command's number */
    CHECK(call(hle_fn_sceMcGetInfo, 5, 0u, 0u, G_OUT, 0u, 0u) == 0, "getinfo");
    CHECK(call(hle_fn_sceMcGetInfo, 5, 0u, 0u, G_OUT, 0u, 0u) == 1, "busy: the pending command number");
    sync_res(1);
    CHECK(call(hle_fn_sceMcSync, 3, 0u, 0u, 0u) == -1, "idle again");

    /* a game's save sequence */
    CHECK(mc_chdir("/BESLUS-12345SAVE00") == -4, "chdir to a missing folder");
    CHECK(mc_getdir("*.b", 0, 24) == 0, "no saves");
    CHECK(mc_entspace("/BESLUS-12345SAVE00") == -4, "entspace of a missing folder");
    CHECK(mc_mkdir("/BESLUS-12345SAVE00") == 0, "mkdir");
    CHECK(mc_mkdir("/BESLUS-12345SAVE00") == -4, "mkdir of an existing name");
    CHECK(mc_chdir("/BESLUS-12345SAVE00") == 0, "chdir");
    CHECK(mc_getdir("*", 0, 24) == 2, "a new folder lists . and ..");
    CHECK(!strcmp(ent_name(0), ".") && !strcmp(ent_name(1), ".."), "names %s %s", ent_name(0), ent_name(1));
    CHECK(ent_attr(0) == 0x8427, "dir attr %04x", ent_attr(0));
    const uint32_t chunks[4] = {51200, 51200, 51200, 49152};
    int fd = mc_open("1  First save.b", 0x203);
    CHECK(fd >= 0, "create the save: %d", fd);
    uint32_t total = 0;
    for (int k = 0; k < 4; k++) {
        fill(G_BUF + total, chunks[k], (uint32_t)k + 1);
        CHECK(mc_write(fd, G_BUF + total, (int)chunks[k]) == (int)chunks[k], "write chunk %d", k);
        total += chunks[k];
    }
    CHECK(host_size("BESLUS-12345SAVE00/1  First save.b") < 0, "nothing at the final name before the close");
    CHECK(mc_close(fd) == 0, "close");
    CHECK(host_size("BESLUS-12345SAVE00/1  First save.b") == 202752, "saved size %ld",
          host_size("BESLUS-12345SAVE00/1  First save.b"));
    CHECK(host_has_tmp("BESLUS-12345SAVE00") == 0, "temporary files left");
    CHECK(mc_close(fd) == -5, "a closed handle is refused");
    fd = mc_open("icon.sys", 0x203);
    CHECK(fd >= 0, "icon.sys");
    fill(G_BUF, 964, 9);
    CHECK(mc_write(fd, G_BUF, 964) == 964 && mc_close(fd) == 0, "icon.sys write");
    r = info(0, &type, &fr, &fmt);
    /* 8001 - root (2 + 1 entries: 2 clusters) - folder (2 + 2 entries: 2) - 198 - 1 */
    CHECK(fr == 8001 - 2 - 2 - 198 - 1, "free after the save: %u", fr);

    /* the load listing and the load */
    CHECK(mc_getdir("/BESLUS-12345SAVE00/*.b", 0, 24) == 1, "one save");
    CHECK(!strcmp(ent_name(0), "1  First save.b") && ent_size(0) == 202752 && ent_attr(0) == 0x8497,
          "save entry %s %u %04x", ent_name(0), ent_size(0), ent_attr(0));
    CHECK(mc_getdir("/*", 0, 24) == 1 && !strcmp(ent_name(0), "BESLUS-12345SAVE00"), "root lists no . / ..");
    CHECK(mc_getdir("/BESLUS-12345SAVE00/*", 0, 2) == 2, "first two");
    CHECK(mc_getdir("/BESLUS-12345SAVE00/*", 1, 24) == 2, "continued listing");
    CHECK(mc_getdir("/BESLUS-12345SAVE00/?con.s?s", 0, 24) == 1, "'?' pattern");
    fd = mc_open("/BESLUS-12345SAVE00/1  First save.b", 1);
    CHECK(fd >= 0, "open for reading %d", fd);
    memset(hle_ram + G_BUF, 0, 300000);
    total = 0;
    for (int k = 0; k < 4; k++) {
        CHECK(mc_read(fd, G_BUF + total, (int)chunks[k]) == (int)chunks[k], "read chunk %d", k);
        total += chunks[k];
    }
    CHECK(mc_read(fd, G_BUF, 100) == 0, "read at the end");
    CHECK(mc_seek(fd, 0, 2) == 202752 && mc_seek(fd, -100, 1) == 202652 && mc_seek(fd, 10, 0) == 10, "seek");
    CHECK(mc_write(fd, G_BUF, 4) == -5, "write on a read handle");
    CHECK(mc_close(fd) == 0, "close");
    uint32_t back = 0;
    for (int k = 0; k < 4; k++) {
        for (uint32_t i = 0; i < chunks[k]; i++)
            CHECK(hle_ram[G_BUF + back + i] == (uint8_t)((i * 131u + ((uint32_t)k + 1) * 7u) >> 3), "data at %u", back + i);
        back += chunks[k];
    }
    CHECK(mc_entspace("/BESLUS-12345SAVE00") == 0, "entspace (4 entries, no deleted)");
    /* another way games write: an existing file opened O_RDWR (3) / O_WRONLY (2), seek, overwrite */
    fd = mc_open("icon.sys", 3);
    CHECK(fd >= 0 && mc_seek(fd, 100, 0) == 100, "open O_RDWR + seek");
    memcpy(hle_ram + G_BUF, "PHOTO", 5);
    CHECK(mc_write(fd, G_BUF, 5) == 5 && mc_close(fd) == 0, "overwrite in the middle");
    CHECK(host_size("BESLUS-12345SAVE00/icon.sys") == 964, "size kept: %ld", host_size("BESLUS-12345SAVE00/icon.sys"));
    fd = mc_open("icon.sys", 1);
    memset(hle_ram + G_BUF, 0, 964);
    CHECK(fd >= 0 && mc_read(fd, G_BUF, 964) == 964 && mc_close(fd) == 0, "read back");
    CHECK(!memcmp(hle_ram + G_BUF + 100, "PHOTO", 5) && hle_ram[G_BUF + 99] == (uint8_t)((99u * 131u + 9u * 7u) >> 3),
          "bytes around the overwrite");
    CHECK(host_has_tmp("BESLUS-12345SAVE00") == 0, "temporary files left");

    /* handles, writers, errors */
    int f1 = mc_open("a", 0x203), f2 = mc_open("b", 0x203), f3 = mc_open("c", 0x203);
    CHECK(f1 >= 0 && f2 >= 0 && f3 >= 0, "three handles");
    CHECK(mc_open("d", 0x203) == -7, "a fourth handle");
    CHECK(mc_close(f3) == 0 && mc_open("a", 0x202) == -5, "a second writer");
    CHECK(mc_close(f1) == 0 && mc_close(f2) == 0, "close a b");
    CHECK(mc_open("missing", 1) == -4 && mc_open("/nodir/x", 0x203) == -4, "missing entries");
    CHECK(mc_delete("/BESLUS-12345SAVE00") == -6, "a folder with files");
    CHECK(mc_delete("a") == 0 && host_size("BESLUS-12345SAVE00/a") < 0, "delete a file");
    CHECK(mc_entspace("/BESLUS-12345SAVE00") == 2, "entspace: 7 entries (odd) + 1 deleted slot");
    CHECK(mc_delete("b") == 0 && mc_delete("c") == 0 && mc_delete("icon.sys") == 0 &&
          mc_delete("1  First save.b") == 0, "delete the rest");
    CHECK(mc_delete("/BESLUS-12345SAVE00") == 0, "delete the folder");
    CHECK(host_size("BESLUS-12345SAVE00") < 0, "folder gone on the host");
    r = info(0, &type, &fr, &fmt);
    CHECK(fr == 8001 - 2, "free after deleting everything: %u (the root keeps its slot)", fr);
    CHECK(charged > 0, "the card charges virtual time");
}

static void t_recover(void)
{
    /* a save interrupted after "tmp -> new" (complete) and one interrupted while writing (tmp): the card folder as a
       power cut leaves it */
    char d[1024];
    snprintf(d, sizeof d, "%s/BESLUS-12345SAVE00", card);
    mkdir(card, 0755);
    mkdir(d, 0755);
    host_write("BESLUS-12345SAVE00/1  First save.b", "OLD", 3);
    host_write("BESLUS-12345SAVE00/1  First save.b._ps2rc_new", "NEWDATA", 7);
    host_write("BESLUS-12345SAVE00/icon.sys._ps2rc_tmp", "PART", 4);
    host_write("_pcsx2_superblock", "X", 1);
    hle_mc_host_dir = card;
    call(hle_fn_sceMcInit, 1, 0u);
    CHECK(host_size("BESLUS-12345SAVE00/1  First save.b") == 7, "the complete new file replaced the old one");
    CHECK(host_has_tmp("BESLUS-12345SAVE00") == 0, "leftovers removed");
    CHECK(host_size("BESLUS-12345SAVE00/icon.sys") < 0, "an incomplete write is not a file");
    CHECK(mc_getdir("/*", 0, 24) == 1, "PCSX2 folder-card metadata stays hidden (%d)", mc_getdir("/*", 0, 24));
    CHECK(mc_getdir("/BESLUS-12345SAVE00/*", 0, 24) == 3, "., .., the save");
}

static void t_unformatted(void)
{
    uint32_t type, fr, fmt;
    mkdir(card, 0755);
    host_write("_ps2rc_unformatted", "", 0);
    host_write("junk", "J", 1);
    hle_mc_host_dir = card;
    call(hle_fn_sceMcInit, 1, 0u);
    int r = info(0, &type, &fr, &fmt);
    CHECK(r == -2 && type == 2 && fr == 0 && fmt == 0, "unformatted: %d %u %u %u", r, type, fr, fmt);
    CHECK(mc_open("/x", 0x203) == -2 && mc_mkdir("/d") == -2, "commands on an unformatted card");
    CHECK(mc_format() == 0, "format");
    r = info(0, &type, &fr, &fmt);
    CHECK((r == 0 || r == -1) && fmt == 1 && fr == 8000, "formatted: %d %u %u", r, fmt, fr);
    CHECK(host_size("_ps2rc_unformatted") < 0 && host_size("junk") < 0, "format empties the folder");
}

static void t_norename(void)
{
    /* a file system that cannot rename: the save is copied in place, nothing is left behind */
    setenv("HLE_MC_NO_RENAME", "1", 1);
    hle_mc_host_dir = card;
    call(hle_fn_sceMcInit, 1, 0u);
    CHECK(mc_mkdir("/BESLUS-12345SAVE00") == 0, "mkdir");
    fill(G_BUF, 70000, 5);
    int fd = mc_open("/BESLUS-12345SAVE00/save.b", 0x203);
    CHECK(fd >= 0 && mc_write(fd, G_BUF, 70000) == 70000 && mc_close(fd) == 0, "save");
    CHECK(host_size("BESLUS-12345SAVE00/save.b") == 70000, "copied in place: %ld", host_size("BESLUS-12345SAVE00/save.b"));
    CHECK(host_has_tmp("BESLUS-12345SAVE00") == 0, "temporary files left");
    fd = mc_open("/BESLUS-12345SAVE00/save.b", 1);
    memset(hle_ram + G_BUF, 0, 70000);
    CHECK(fd >= 0 && mc_read(fd, G_BUF, 70000) == 70000 && mc_close(fd) == 0, "read back");
    for (uint32_t i = 0; i < 70000; i++) CHECK(hle_ram[G_BUF + i] == (uint8_t)((i * 131u + 5u * 7u) >> 3), "data at %u", i);
    /* an existing file opened for writing without O_CREAT is staged as a copy of itself */
    fd = mc_open("/BESLUS-12345SAVE00/save.b", 2);
    CHECK(fd >= 0 && mc_seek(fd, 69990, 0) == 69990 && mc_write(fd, G_BUF, 20) == 20 && mc_close(fd) == 0, "rewrite tail");
    CHECK(host_size("BESLUS-12345SAVE00/save.b") == 70010, "grown to %ld", host_size("BESLUS-12345SAVE00/save.b"));
}

static void t_full(void)
{
    uint32_t type, fr, fmt;
    hle_mc_host_dir = card;
    call(hle_fn_sceMcInit, 1, 0u);
    info(0, &type, &fr, &fmt);
    int fd = mc_open("/big", 0x203);
    CHECK(fd >= 0, "big");
    CHECK(mc_write(fd, G_BUF, 7900 * 1024) == 7900 * 1024, "7,900 KB fit");
    CHECK(mc_write(fd, G_BUF, 200 * 1024) == -3, "200 KB more do not (full device)");
    CHECK(mc_close(fd) == 0, "close");
    CHECK(host_size("big") == 7900 * 1024, "size %ld", host_size("big"));
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: mc_unit nocard|basic|recover|unformatted|full DIR\n"); return 2; }
    mode = argv[1];
    snprintf(card, sizeof card, "%s", argv[2]);
    hle_ram_end = 16u << 20;
    hle_ram = calloc(1, hle_ram_end);
    if (!strcmp(mode, "nocard")) t_nocard();
    else if (!strcmp(mode, "basic")) t_basic();
    else if (!strcmp(mode, "recover")) t_recover();
    else if (!strcmp(mode, "unformatted")) t_unformatted();
    else if (!strcmp(mode, "full")) t_full();
    else if (!strcmp(mode, "norename")) t_norename();
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    printf("PASS %s\n", mode);
    return 0;
}
