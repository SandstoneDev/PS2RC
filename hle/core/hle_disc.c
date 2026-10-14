/* hle_disc.c - the game disc as seen by the HLE runtime: the extracted disc files (host: the read-only extracted
 * directory; PSP: a copy on the memory stick) plus the ISO sector map (discmap.txt: "lsn size ISO_PATH rel/path").
 * Serves path-based opens (fileio "cdrom0:\X;1", sceCdSearchFile) and LSN-based sector reads (a streaming RPC
 * server of the pack, a game's own CD file system). Sectors 0..N-1 are the synthetic ISO9660 metadata (iso_meta.bin), read on demand.
 * Every file the run touches is recorded (hle_disc_usage_dump: the list of disc files a platform needs). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: PZ_SCOPE zones */

typedef struct { uint32_t lsn, size; char iso[96], rel[96]; int h; uint32_t reads; uint64_t bytes; } DiscFile;
static DiscFile *files;
static int nfiles;
/* the disc lock (psp/hle_psp.c; a no-op until an asynchronous reader thread of the pack exists, absent on
   the host): the reader thread and the game thread never use the file table / the platform's open files at once */
void hle_plat_disc_lock(void) __attribute__((weak));
void hle_plat_disc_unlock(void) __attribute__((weak));
#define DLOCK() do { if (hle_plat_disc_lock) hle_plat_disc_lock(); } while (0)
#define DUNLOCK() do { if (hle_plat_disc_unlock) hle_plat_disc_unlock(); } while (0)
const char *hle_disc_dir;
static uint64_t bytes_read, sector_reads;
/* replacement files (host --disc-override ISO=PATH; the PSP stage simply holds the replacement in disc/): e.g. a
   settings file with a smaller heap size for the PSP's memory; same size, so the ISO metadata stays */
static struct { char iso[96]; char path[400]; } overrides[8];
static int noverrides;

void hle_disc_add_override(const char *iso, const char *path)
{
    if (noverrides < 8) {
        snprintf(overrides[noverrides].iso, sizeof overrides[0].iso, "%s", iso);
        snprintf(overrides[noverrides].path, sizeof overrides[0].path, "%s", path);
        noverrides++;
    }
}

static int open_entry(DiscFile *f)
{
    if (f->h >= 0) return f->h;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", hle_disc_dir, f->rel);
    for (int i = 0; i < noverrides; i++)
        if (strcmp(overrides[i].iso, f->iso) == 0) {
            snprintf(path, sizeof path, "%s", overrides[i].path);
            LOG("disc: %s served from %s\n", f->iso, path);
        }
    f->h = hle_plat_open(path);
    if (f->h < 0) LOG("disc: cannot open %s\n", path);
    return f->h;
}

/* sectors 0..meta_sectors-1: synthetic ISO9660 volume descriptors / path tables / directories (iso_meta.bin) - for
   a game that parses the file system itself through sector reads */
static int meta_h = -1;
static uint32_t meta_sectors;

/* whole small text file into a malloc'ed, NUL-terminated buffer */
static char *slurp(const char *path, size_t *len)
{
    int h = hle_plat_open(path);
    if (h < 0) return NULL;
    int64_t n = hle_plat_size(h);
    if (n < 0 || n > (1 << 22)) return NULL;
    char *b = malloc((size_t)n + 1);
    if (!b || hle_plat_pread(h, 0, b, (uint32_t)n) != n) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

int hle_disc_init(const char *dir, const char *mapfile, const char *metafile)
{
    hle_disc_dir = dir;
    meta_h = hle_plat_open(metafile);
    int64_t msz = meta_h >= 0 ? hle_plat_size(meta_h) : -1;
    if (msz > 0) {
        meta_sectors = (uint32_t)(msz / 2048);
        LOG("disc: ISO9660 metadata sectors 0-%u from %s\n", meta_sectors - 1, metafile);
    } else {
        meta_h = -1;
        LOG("disc: no %s - file-system sectors will read as zeros\n", metafile);
    }
    char *text = slurp(mapfile, NULL);
    if (!text) { LOG("disc: cannot read the disc map %s\n", mapfile); return 0; }
    int cap = 512;
    files = calloc(cap, sizeof *files);
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        DiscFile f;
        memset(&f, 0, sizeof f);
        f.h = -1;
        if (sscanf(line, "%u %u %95s %95s", &f.lsn, &f.size, f.iso, f.rel) == 4) {
            if (nfiles == cap) files = realloc(files, (cap *= 2) * sizeof *files);
            files[nfiles++] = f;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    LOG("disc: %d files mapped from %s, files in %s\n", nfiles, mapfile, dir);
    return nfiles > 0;
}

/* "cdrom0:\DATA\FILE.DAT;1" -> "DATA/FILE.DAT" */
static void normalise(const char *in, char *out, size_t n)
{
    const char *p = in;
    const char *colon = strchr(p, ':');
    if (colon && colon - p < 8) p = colon + 1;
    while (*p == '\\' || *p == '/') p++;
    size_t k = 0;
    for (; *p && k + 1 < n; p++) {
        if (*p == ';') break;
        out[k++] = *p == '\\' ? '/' : (char)toupper((unsigned char)*p);
    }
    out[k] = 0;
}

static DiscFile *find_path(const char *guest_path)
{
    char key[256];
    normalise(guest_path, key, sizeof key);
    for (int i = 0; i < nfiles; i++) {
        char iso[96];
        snprintf(iso, sizeof iso, "%s", files[i].iso);
        size_t l = strlen(iso);
        if (l && iso[l - 1] == '.') iso[l - 1] = 0;
        if (strcmp(iso, key) == 0) return &files[i];
    }
    return NULL;
}

int hle_disc_lookup(const char *guest_path, uint32_t *lsn, uint32_t *size)
{
    DiscFile *f = find_path(guest_path);
    if (!f) return 0;
    *lsn = f->lsn;
    *size = f->size;
    return 1;
}

/* path-based handles: index into files[] */
static int hle_disc_open_unlocked(const char *guest_path, uint32_t *size_out)
{
    DiscFile *f = find_path(guest_path);
    if (!f || open_entry(f) < 0) return -1;
    if (size_out) *size_out = f->size;
    return (int)(f - files);
}

int hle_disc_open(const char *guest_path, uint32_t *size_out)
{
    DLOCK();
    int r = hle_disc_open_unlocked(guest_path, size_out);
    DUNLOCK();
    return r;
}

static int64_t hle_disc_read_fd_unlocked(int idx, uint64_t off, void *dst, uint32_t n)
{
    PZ_SCOPE(disc);
    if (idx < 0 || idx >= nfiles) return -1;
    DiscFile *f = &files[idx];
    if (open_entry(f) < 0) return -1;
    if (off >= f->size) return 0;
    if (off + n > f->size) n = (uint32_t)(f->size - off);
    int64_t r = hle_plat_pread(f->h, off, dst, n);
    if (r > 0) {
        bytes_read += (uint64_t)r;
        f->bytes += (uint64_t)r;
    }
    f->reads++;
    return r;
}

int64_t hle_disc_read_fd(int idx, uint64_t off, void *dst, uint32_t n)
{
    DLOCK();
    int64_t r = hle_disc_read_fd_unlocked(idx, off, dst, n);
    DUNLOCK();
    return r;
}

void hle_disc_close(int idx) { (void)idx; }

static DiscFile *file_of_lsn(uint32_t lsn)
{
    int lo = 0, hi = nfiles - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (files[mid].lsn <= lsn) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return NULL;
    DiscFile *f = &files[best];
    uint32_t nsect = (f->size + 2047) / 2048;
    if (lsn >= f->lsn + (nsect ? nsect : 1)) return NULL;
    return f;
}

const char *hle_disc_name_of_lsn(uint32_t lsn)
{
    if (lsn < meta_sectors) return "<ISO9660 metadata>";
    DiscFile *f = file_of_lsn(lsn);
    return f ? f->iso : "<no file>";
}

int hle_disc_read_lsn(uint32_t lsn, uint32_t nsect, void *dst)
{
    DLOCK();
    uint8_t *out = dst;
    int ok = 0;
    sector_reads++;
    for (uint32_t s = 0; s < nsect;) {
        if (lsn + s < meta_sectors) {
            uint32_t cnt = meta_sectors - (lsn + s);
            if (cnt > nsect - s) cnt = nsect - s;
            if (hle_plat_pread(meta_h, (uint64_t)(lsn + s) * 2048, out + (size_t)s * 2048, cnt * 2048) != (int64_t)cnt * 2048) {
                memset(out + (size_t)s * 2048, 0, (size_t)cnt * 2048);
                ok = -1;
            }
            s += cnt;
            continue;
        }
        DiscFile *f = file_of_lsn(lsn + s);
        if (!f) {
            memset(out + (size_t)s * 2048, 0, 2048);
            ok = -1;
            s++;
            continue;
        }
        uint32_t fsect = (f->size + 2047) / 2048;
        uint32_t first = lsn + s - f->lsn, cnt = fsect - first;
        if (cnt > nsect - s) cnt = nsect - s;
        /* bytes past the file end (the last sector's tail) or past a short read are zero-filled */
        uint64_t foff = (uint64_t)first * 2048, want = (uint64_t)cnt * 2048;
        int64_t r = hle_disc_read_fd_unlocked((int)(f - files), foff, out + (size_t)s * 2048, cnt * 2048);
        if (r < 0) { ok = -1; r = 0; }
        if ((uint64_t)r < want) memset(out + (size_t)s * 2048 + r, 0, (size_t)(want - (uint64_t)r));
        s += cnt;
    }
    DUNLOCK();
    return ok;
}

/* hle_disc_read_lsn for an asynchronous reader thread of the pack - no profiler zone and
   no log line (the profiler and the log buffer belong to the game thread), the same files, zero fill and result */
int hle_disc_read_lsn_quiet(uint32_t lsn, uint32_t nsect, void *dst)
{
    DLOCK();
    uint8_t *out = dst;
    int ok = 0;
    sector_reads++;
    for (uint32_t s = 0; s < nsect;) {
        if (lsn + s < meta_sectors) {
            uint32_t cnt = meta_sectors - (lsn + s);
            if (cnt > nsect - s) cnt = nsect - s;
            if (hle_plat_pread(meta_h, (uint64_t)(lsn + s) * 2048, out + (size_t)s * 2048, cnt * 2048) != (int64_t)cnt * 2048) {
                memset(out + (size_t)s * 2048, 0, (size_t)cnt * 2048);
                ok = -1;
            }
            s += cnt;
            continue;
        }
        DiscFile *f = file_of_lsn(lsn + s);
        if (!f) {
            memset(out + (size_t)s * 2048, 0, 2048);
            ok = -1;
            s++;
            continue;
        }
        uint32_t fsect = (f->size + 2047) / 2048;
        uint32_t first = lsn + s - f->lsn, cnt = fsect - first;
        if (cnt > nsect - s) cnt = nsect - s;
        uint64_t foff = (uint64_t)first * 2048, want = (uint64_t)cnt * 2048;
        int64_t r = -1;
        if (f->h < 0) {                                    /* open_entry without its log lines */
            char path[512];
            snprintf(path, sizeof path, "%s/%s", hle_disc_dir, f->rel);
            for (int i = 0; i < noverrides; i++)
                if (strcmp(overrides[i].iso, f->iso) == 0) snprintf(path, sizeof path, "%s", overrides[i].path);
            f->h = hle_plat_open(path);
        }
        if (f->h >= 0 && foff < f->size) {
            uint32_t n = cnt * 2048;
            if (foff + n > f->size) n = (uint32_t)(f->size - foff);
            r = hle_plat_pread(f->h, foff, out + (size_t)s * 2048, n);
            if (r > 0) { f->bytes += (uint64_t)r; bytes_read += (uint64_t)r; }
            f->reads++;
        } else if (f->h >= 0) r = 0;
        if (r < 0) { ok = -1; r = 0; }
        if ((uint64_t)r < want) memset(out + (size_t)s * 2048 + r, 0, (size_t)(want - (uint64_t)r));
        s += cnt;
    }
    DUNLOCK();
    return ok;
}

void hle_disc_report(void)
{
    int used = 0;
    uint64_t used_size = 0;
    for (int i = 0; i < nfiles; i++)
        if (files[i].reads) { used++; used_size += files[i].size; }
    LOG("disc: %llu bytes read, %llu sector-read requests, %d files touched (%llu bytes of files)\n",
        (unsigned long long)bytes_read, (unsigned long long)sector_reads, used, (unsigned long long)used_size);
}

/* "ISO_PATH size bytes_read reads" for every file the run read: the disc files a platform must have */
void hle_disc_usage_dump(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < nfiles; i++)
        if (files[i].reads)
            fprintf(f, "%s %u %llu %u\n", files[i].iso, files[i].size, (unsigned long long)files[i].bytes,
                    files[i].reads);
    fclose(f);
}
