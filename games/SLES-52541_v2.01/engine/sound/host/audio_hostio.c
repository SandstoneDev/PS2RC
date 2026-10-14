/* audio_hostio.c - see audio_hostio.h */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include "audio_hostio.h"

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

/* dst = dir + "/" + name; 0 ok, -1 too long */
static int join(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b + 1 > cap) return -1;
    memcpy(dst, dir, a);
    dst[a] = '/';
    memcpy(dst + a + 1, name, b + 1);
    return 0;
}

int audio_hostio_resolve(const AudioHostIo *h, const char *iop_path, char *out, size_t cap)
{
    char cur[2304], probe[2304];
    size_t rl = strlen(h->root);
    if (rl + 1 > sizeof cur) return -1;
    memcpy(cur, h->root, rl + 1);
    const char *p = iop_path;
    while (*p == '\\' || *p == '/') p++;
    while (*p) {
        char comp[256];
        size_t n = 0;
        while (*p && *p != '\\' && *p != '/' && n + 1 < sizeof comp) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '\\' || *p == '/') p++;
        if (!n) continue;
        struct stat st;
        if (join(probe, sizeof probe, cur, comp) != 0) return -1;
        if (stat(probe, &st) != 0) {
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != 0) {
                    if (ieq(e->d_name, comp) && join(probe, sizeof probe, cur, e->d_name) == 0) {
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) return -1;
        }
        memcpy(cur, probe, strlen(probe) + 1);
    }
    size_t cl = strlen(cur);
    if (cl + 1 > cap) return -1;
    memcpy(out, cur, cl + 1);
    return 0;
}

static FILE *get_file(AudioHostIo *h, const char *path, uint32_t *size)
{
    for (int i = 0; i < h->nfiles; i++)
        if (!strcmp(h->files[i].key, path)) { *size = h->files[i].size; return h->files[i].f; }
    char host[2304];
    if (audio_hostio_resolve(h, path, host, sizeof host) != 0) return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    int slot = h->nfiles < AUDIO_HOSTIO_FILES ? h->nfiles++ : 0;
    if (h->files[slot].f) fclose(h->files[slot].f);
    size_t kl = strlen(path);
    if (kl >= sizeof h->files[slot].key) kl = sizeof h->files[slot].key - 1;
    memcpy(h->files[slot].key, path, kl);
    h->files[slot].key[kl] = 0;
    h->files[slot].f = f;
    h->files[slot].size = (uint32_t)n;
    *size = (uint32_t)n;
    return f;
}

static int hio_size(void *user, const char *path, uint32_t *size_out)
{
    AudioHostIo *h = (AudioHostIo *)user;
    return get_file(h, path, size_out) ? SNDD_IO_OK : SNDD_IO_ERROR;
}

static uint32_t req_hash(const char *path, uint32_t off, uint32_t n, const void *dst)
{
    uint32_t x = 2166136261u;
    for (const char *s = path; *s; s++) x = (x ^ (uint8_t)*s) * 16777619u;
    x = (x ^ off) * 16777619u;
    x = (x ^ n) * 16777619u;
    x = (x ^ (uint32_t)(uintptr_t)dst) * 16777619u;
    return x ? x : 1;
}

static int hio_read(void *user, const char *path, uint32_t off, void *dst, uint32_t n)
{
    AudioHostIo *h = (AudioHostIo *)user;
    if (h->simulate_pending) {
        uint32_t k = req_hash(path, off, n, dst);
        int i, free_i = -1;
        for (i = 0; i < AUDIO_HOSTIO_PENDING_SLOTS; i++) {
            if (h->pend[i].used && h->pend[i].hash == k) break;
            if (!h->pend[i].used && free_i < 0) free_i = i;
        }
        if (i == AUDIO_HOSTIO_PENDING_SLOTS) {               /* first sight: answer "not yet" */
            if (free_i < 0) free_i = (int)(k % AUDIO_HOSTIO_PENDING_SLOTS);
            h->pend[free_i].hash = k;
            h->pend[free_i].used = 1;
            h->pendings++;
            return SNDD_IO_PENDING;
        }
        h->pend[i].used = 0;
    }
    uint32_t size;
    FILE *f = get_file(h, path, &size);
    if (!f || off > size || n > size - off) { h->errors++; return SNDD_IO_ERROR; }
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(dst, 1, n, f) != n) { h->errors++; return SNDD_IO_ERROR; }
    h->reads++;
    h->bytes_read += n;
    return SNDD_IO_OK;
}

int audio_hostio_init(AudioHostIo *h, const char *disc_root)
{
    memset(h, 0, sizeof *h);
    size_t n = strlen(disc_root);
    if (n + 1 > sizeof h->root) return -1;
    memcpy(h->root, disc_root, n + 1);
    struct stat st;
    return stat(h->root, &st) == 0 ? 0 : -1;
}

void audio_hostio_bind(AudioHostIo *h, SnddIo *io)
{
    io->user = h;
    io->size = hio_size;
    io->read = hio_read;
}

void audio_hostio_close(AudioHostIo *h)
{
    for (int i = 0; i < h->nfiles; i++)
        if (h->files[i].f) fclose(h->files[i].f);
    h->nfiles = 0;
}
