/* hle_memcard.c - libmc HLE: a PS2 memory card (8 MB) in slot 1 backed by files. Without a card the "no memory card"
 * answers of hle_sce.c; the hooks are the sceMc* names the pack's hooks.def binds.
 *
 * The usual call pattern: sceMcInit, GetInfo, Sync, Open, Close, Seek, Read, Write, Mkdir, Chdir, GetDir, Delete,
 * Format, GetEntSpace, every call followed by sceMcSync(0 = wait, &cmd, &result). The HLE completes each command at
 * once ("immediate completion"). The behaviour (written for this runtime; the file layout and the command semantics
 * only, no code) follows the open-source ps2sdk memory card modules (libmc, mcman):
 *   - a command issued while another is pending returns the pending command's number (> 0), nothing happens;
 *   - sceMcSync returns 1 and clears the command when one finished, -1 when none is registered (the old stub always
 *     returned 1: harmless for a game that ignores v0 and syncs only right after a command);
 *   - result codes: 0 ok, -1 changed (formatted) card, -2 unformatted, -3 full, -4 no entry (also: mkdir of an
 *     existing name), -5 denied (bad fd, second writer), -6 directory not empty, -7 no free handle (3 handles),
 *     -10 no card;
 *   - GetInfo: type 2 (PS2 card), free = free 1 KB clusters (8,000 on an empty formatted card: mcman_format2 makes
 *     8,001 allocatable clusters of the 8,192, the root directory takes one), result -1 at the first call after the
 *     card appeared, 0 afterwards;
 *   - O_CREAT (0x200) on an existing file recreates it empty (mcman_open2 marks the old entry deleted); without O_CREAT
 *     writes overwrite in place; 0x40 = mkdir (sceMcMkdir is sceMcOpen(name, 0x40) in mcserv);
 *   - GetDir: the name's last component is a pattern ('*', '?'); a sub-directory lists "." and ".." first, the root
 *     does not; 64-byte sceMcTblGetDir entries; mode 1 continues the previous listing;
 *   - modes: directory 0x8427, file 0x8417 while open after creation, 0x8497 once closed;
 *   - space: files take ceil(size / 1 KB) clusters, a directory ceil(entries / 2) (entries incl. "." and ".."; a
 *     deleted entry's slot is reused by the next one, GetEntSpace = (entries & 1) + deleted slots).
 * The card's directory tree is indexed in RAM at the first sceMcInit (one listing of the card folder); GetInfo, GetDir,
 * GetEntSpace and Chdir never touch the memory stick, so a game that polls the card every frame costs nothing. Only
 * the game's own commands do file IO: Open/Read of a file, Write/Close of a file it writes, Mkdir, Delete, Format.
 *
 * Crash-safe writes: a file opened for writing is written to "<name>._ps2rc_tmp" (O_CREAT: empty; otherwise a copy of
 * the original); sceMcClose closes it (FAT commits the size on close), renames it to "<name>._ps2rc_new" (= complete),
 * removes the original and renames the new file in place. At the next start, a "._ps2rc_new" left behind replaces its
 * original and a "._ps2rc_tmp" (incomplete) is deleted. Names containing "_ps2rc" are never shown to the game.
 * An unformatted card: the folder holds "_ps2rc_unformatted" (tests; sceMcFormat removes everything and the marker).
 *
 * No card (host without --memcard, PSP ps2rc.cfg memcard=0): exactly the old stub behaviour - every command completes
 * with -10, GetInfo reports type/free/format 0, no virtual time charged (host baseline: main loop at vtime 40.090).
 * Card: each command charges MC_COST_CMD EE cycles (+ n/8 for data) at a safe point - deterministic, independent of
 * how long the memory stick takes.
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_memcard.h"

int hle_mc_inserted;

/* every libmc call in the log: verbose >= 1, or the environment variable HLE_MC_TRACE (host investigations) */
static int mc_trace = -1;
#define MCLOG(...) do { if (mc_trace < 0) mc_trace = getenv("HLE_MC_TRACE") != NULL; \
                        if (mc_trace || hle_verbose >= 1) hle_logf(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ libmc constants (ps2sdk libmc.h / libmc-common.h) */
enum { MCF_GET_INFO = 1, MCF_OPEN, MCF_CLOSE, MCF_SEEK, MCF_READ, MCF_WRITE, MCF_FLUSH = 0x0A, MCF_MKDIR, MCF_CHDIR,
       MCF_GETDIR, MCF_SETINFO, MCF_DELETE, MCF_FORMAT, MCF_UNFORMAT, MCF_GETENT, MCF_RENAME };
#define MC_OK          0
#define MC_CHANGED    (-1)
#define MC_NOFORMAT   (-2)
#define MC_FULL       (-3)
#define MC_NOENTRY    (-4)
#define MC_DENIED     (-5)
#define MC_NOTEMPTY   (-6)
#define MC_UPLIMIT    (-7)
#define MC_NOCARD     (-10)
#define MCA_READ      0x0001
#define MCA_WRITE     0x0002
#define MCA_EXEC      0x0004
#define MCA_FILE      0x0010
#define MCA_SUBDIR    0x0020
#define MC_CREATE_DIR 0x0040
#define MCA_CLOSED    0x0080
#define MC_CREATE     0x0200
#define MCA_0400      0x0400
#define MCA_HIDDEN    0x2000
#define MCA_EXISTS    0x8000
#define MODE_DIR      (MCA_EXISTS | MCA_0400 | MCA_SUBDIR | MCA_EXEC | MCA_WRITE | MCA_READ)   /* 0x8427 */
#define MODE_FILE_NEW (MCA_EXISTS | MCA_0400 | MCA_FILE | MCA_EXEC | MCA_WRITE | MCA_READ)     /* 0x8417 */

#define MC_ALLOC_CLUSTERS 8001u          /* mcman_format2 on an 8 MB card (8,192 clusters of 1 KB) */
#define MC_MAX_FD 3                      /* mcman MAX_FDHANDLES */
#define MC_MAX_NODES 512                  /* entries on the card (a game's saves: about 10) */
#define MC_COST_CMD 2000u                /* EE cycles per command (card present) */
#define MC_HIDDEN_TAG "_ps2rc"
#define MC_TMP_SUFFIX "._ps2rc_tmp"
#define MC_NEW_SUFFIX "._ps2rc_new"
#define MC_UNFORMATTED_MARK "_ps2rc_unformatted"

/* ------------------------------------------------------------------ the card index (RAM) */
typedef struct {
    char name[32];
    int16_t parent;                      /* node index; -1 for the root */
    uint8_t used, is_dir;
    uint16_t mode;
    uint32_t size;                       /* files: bytes */
    uint32_t len;                        /* directories: entry slots incl. "." and ".." */
    uint32_t dslots;                     /* directories: deleted slots the next entry reuses */
    uint32_t order;                      /* listing order inside the parent */
    HleMcTime ctime, mtime;
} McNode;

static McNode nodes[MC_MAX_NODES];
static int n_nodes;                      /* high-water mark of used slots */
static uint32_t next_order;
static int mc_state;                     /* 0 = not configured yet, 1 = no card, 2 = card inserted */
static int mc_formatted;
static int mc_info_seen;                 /* GetInfo already reported this card (result 0 instead of -1) */
static char mc_root[320];                /* host directory of the card, ends with '/' */
static int mc_cwd;                       /* current directory (sceMcChdir), port 0 */

static int mc_cmd, mc_result;            /* registered command and its result (sceMcSync) */

typedef struct {
    int used, node, rd, wr, dir, staged;
    uint32_t pos;
    uint64_t bytes;                      /* read / written through this handle (log) */
    unsigned calls;
} McHandle;
static McHandle fh[MC_MAX_FD];

/* GetDir cursor */
static int gd_dir = -1, gd_pos;
static uint32_t gd_last_order;
static char gd_pattern[64];

static uint8_t copybuf[16384];

/* ------------------------------------------------------------------ helpers */
static int card_present(int port) { return mc_state == 2 && port == 0; }

static void mc_charge(R5900Ctx *c, uint64_t cost)
{
    if (mc_state != 2) return;           /* no card: no virtual time, as the old stubs */
    if (hle_in_intr) hle_time_charge(cost);
    else hle_safe_point(c, cost);
}

/* the runtime's own files and PCSX2's folder-memory-card metadata (_pcsx2_superblock, _pcsx2_index,
   _pcsx2_meta_directory): a PCSX2 folder card's directory works as the card folder as it is */
static int has_hidden_tag(const char *name) { return strstr(name, MC_HIDDEN_TAG) != NULL || !strncmp(name, "_pcsx2_", 7); }

static uint32_t clusters_of(uint32_t bytes) { return (bytes + 1023u) / 1024u; }
static uint32_t dir_clusters(uint32_t len) { return (len + 1u) / 2u; }

static uint32_t used_clusters(void)
{
    uint32_t u = 0;
    for (int i = 0; i < n_nodes; i++) {
        if (!nodes[i].used) continue;
        u += nodes[i].is_dir ? dir_clusters(nodes[i].len) : clusters_of(nodes[i].size);
    }
    return u;
}

static uint32_t free_clusters(void)
{
    uint32_t u = used_clusters();
    return u >= MC_ALLOC_CLUSTERS ? 0 : MC_ALLOC_CLUSTERS - u;
}

static int node_alloc(void)
{
    for (int i = 1; i < MC_MAX_NODES; i++)
        if (!nodes[i].used) {
            memset(&nodes[i], 0, sizeof nodes[i]);
            if (i >= n_nodes) n_nodes = i + 1;
            return i;
        }
    return -1;
}

static int child_find(int dir, const char *name, size_t len)
{
    if (len >= sizeof nodes[0].name) return -1;
    for (int i = 1; i < n_nodes; i++)
        if (nodes[i].used && nodes[i].parent == dir && strlen(nodes[i].name) == len && !memcmp(nodes[i].name, name, len))
            return i;
    return -1;
}

static int child_count(int dir)
{
    int n = 0;
    for (int i = 1; i < n_nodes; i++) if (nodes[i].used && nodes[i].parent == dir) n++;
    return n;
}

/* host path of a node (+ optional suffix) */
static void host_path(int node, const char *suffix, char *out, size_t n)
{
    const char *parts[16];
    int np = 0;
    for (int i = node; i > 0 && np < 16; i = nodes[i].parent) parts[np++] = nodes[i].name;
    size_t k = (size_t)snprintf(out, n, "%s", mc_root);
    for (int j = np - 1; j >= 0 && k < n; j--) k += (size_t)snprintf(out + k, n - k, "%s%s", parts[j], j ? "/" : "");
    if (suffix && k < n) snprintf(out + k, n - k, "%s", suffix);
}

/* PS2 path of a node, for the log */
static const char *ps2_path(int node)
{
    static char b[2][160];
    static int r;
    char *o = b[r++ & 1];
    const char *parts[16];
    int np = 0;
    for (int i = node; i > 0 && np < 16; i = nodes[i].parent) parts[np++] = nodes[i].name;
    size_t k = 0;
    o[0] = '/';
    o[1] = 0;
    for (int j = np - 1; j >= 0 && k < sizeof b[0] - 34; j--) k += (size_t)snprintf(o + k, sizeof b[0] - k, "/%s", parts[j]);
    return o;
}

/* resolves a PS2 path (absolute or relative to the current directory). Returns the node, or -1 with *parent = the
   directory the last component would live in and *leaf = that component (create), or -2 when an intermediate
   component is missing / not a directory (mcman: -4) */
static int resolve(const char *path, int *parent, const char **leaf, size_t *leaf_len)
{
    int cur = path[0] == '/' ? 0 : mc_cwd;
    const char *p = path;
    if (parent) *parent = -1;
    if (leaf) *leaf = NULL;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) return cur;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        int last = !e || !e[strspn(e, "/")];
        int next;
        if (len == 1 && p[0] == '.') next = cur;
        else if (len == 2 && p[0] == '.' && p[1] == '.') next = cur > 0 ? nodes[cur].parent : 0;
        else next = child_find(cur, p, len);
        if (next < 0) {
            if (!last) return -2;
            if (parent) *parent = cur;
            if (leaf) *leaf = p;
            if (leaf_len) *leaf_len = len;
            return -1;
        }
        if (!last && !nodes[next].is_dir) return -2;
        cur = next;
        p += len;
    }
}

/* mcman_checkdirpath: '?' = one character, '*' = any run */
static int name_match(const char *name, const char *pat)
{
    if (!*pat) return !*name;
    if (*pat == '*') {
        while (*pat == '*') pat++;
        if (!*pat) return 1;
        for (; *name; name++) if (name_match(name, pat)) return 1;
        return name_match(name, pat);
    }
    if (!*name) return *pat == '?';                           /* mcman: a '?' run may pass the end of the name */
    if (*pat == '?' || *pat == *name) return name_match(name + 1, pat + 1);
    return 0;
}

static void now(HleMcTime *t) { hle_plat_mc_now(t); }

/* ------------------------------------------------------------------ index from the card folder */
#define LIST_MAX 128
typedef struct { char name[64]; int is_dir; uint32_t size; HleMcTime mt, ct; } ListEnt;
static ListEnt lst[LIST_MAX];
static int n_lst;

static void list_cb(void *ctx, const char *name, int is_dir, uint32_t size, const HleMcTime *mt, const HleMcTime *ct)
{
    (void)ctx;
    if (n_lst >= LIST_MAX || strlen(name) >= sizeof lst[0].name) return;
    ListEnt *e = &lst[n_lst++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir;
    e->size = size;
    e->mt = *mt;
    e->ct = *ct;
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static void index_dir(int dir, const char *hdir, int depth)
{
    n_lst = 0;
    if (hle_plat_mc_list(hdir, list_cb, NULL) < 0) { LOG("memcard: cannot list %s\n", hdir); return; }
    /* crash recovery: "<x>._ps2rc_new" is a complete file whose final rename did not happen, "<x>._ps2rc_tmp" an
       incomplete one */
    int recovered = 0;
    for (int i = 0; i < n_lst; i++) {
        char from[400], to[400];
        if (lst[i].is_dir) continue;
        if (ends_with(lst[i].name, MC_TMP_SUFFIX)) {
            snprintf(from, sizeof from, "%s%s", hdir, lst[i].name);
            hle_plat_mc_remove(from);
            LOG("memcard: removed an incomplete write %s\n", from);
            recovered++;
        } else if (ends_with(lst[i].name, MC_NEW_SUFFIX)) {
            snprintf(from, sizeof from, "%s%s", hdir, lst[i].name);
            snprintf(to, sizeof to, "%s%.*s", hdir, (int)(strlen(lst[i].name) - strlen(MC_NEW_SUFFIX)), lst[i].name);
            if (hle_plat_mc_exists(to)) hle_plat_mc_remove(to);
            int r = hle_plat_mc_rename(from, to);
            LOG("memcard: completed an interrupted save %s -> %s (%d)\n", from, to, r);
            recovered++;
        }
    }
    if (recovered) {                                       /* list again: names / sizes changed */
        n_lst = 0;
        if (hle_plat_mc_list(hdir, list_cb, NULL) < 0) return;
    }
    /* deterministic order: by name */
    for (int i = 1; i < n_lst; i++)
        for (int j = i; j > 0 && strcmp(lst[j - 1].name, lst[j].name) > 0; j--) {
            ListEnt t = lst[j];
            lst[j] = lst[j - 1];
            lst[j - 1] = t;
        }
    int first = n_nodes, count = 0;
    for (int i = 0; i < n_lst; i++) {
        const ListEnt *e = &lst[i];
        if (dir == 0 && !strcmp(e->name, MC_UNFORMATTED_MARK)) { mc_formatted = 0; continue; }
        if (has_hidden_tag(e->name) || e->name[0] == '.') continue;
        if (strlen(e->name) > 31) { LOG("memcard: skipped %s%s (name longer than 31 characters)\n", hdir, e->name); continue; }
        int k = node_alloc();
        if (k < 0) { LOG("memcard: index full (%d entries), %s%s skipped\n", MC_MAX_NODES, hdir, e->name); break; }
        McNode *nd = &nodes[k];
        snprintf(nd->name, sizeof nd->name, "%s", e->name);
        nd->used = 1;
        nd->parent = (int16_t)dir;
        nd->is_dir = (uint8_t)e->is_dir;
        nd->mode = e->is_dir ? MODE_DIR : (MODE_FILE_NEW | MCA_CLOSED);
        nd->size = e->is_dir ? 0 : e->size;
        nd->len = 2;
        nd->order = ++next_order;
        nd->mtime = e->mt;
        nd->ctime = e->ct;
        count++;
    }
    nodes[dir].len = 2 + (uint32_t)count;
    if (depth >= 8) return;
    int last = n_nodes;
    for (int k = first; k < last; k++) {
        if (!nodes[k].used || nodes[k].parent != dir || !nodes[k].is_dir) continue;
        char sub[400];
        host_path(k, "/", sub, sizeof sub);
        index_dir(k, sub, depth + 1);
    }
}

static void mc_configure(void)
{
    if (mc_state) return;
    mc_state = 1;
    if (!hle_plat_mc_config(mc_root, sizeof mc_root)) {
        LOG("memcard: slot 1 empty (no memory card)\n");
        return;
    }
    size_t rl = strlen(mc_root);
    if (rl && mc_root[rl - 1] != '/' && rl + 1 < sizeof mc_root) { mc_root[rl] = '/'; mc_root[rl + 1] = 0; }
    /* the folder of the card (and its parent): created when missing = a new, formatted card */
    char d[320];
    snprintf(d, sizeof d, "%s", mc_root);
    d[strlen(d) - 1] = 0;
    if (hle_plat_mc_exists(d) != 2) {
        char parent[320];
        snprintf(parent, sizeof parent, "%s", d);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) { *sl = 0; if (hle_plat_mc_exists(parent) != 2) hle_plat_mc_mkdir(parent); }
        if (hle_plat_mc_mkdir(d) < 0 || hle_plat_mc_exists(d) != 2) {
            LOG("memcard: cannot create the card folder %s - slot 1 stays empty\n", d);
            return;
        }
        LOG("memcard: new card folder %s (a formatted, empty 8 MB card)\n", d);
    }
    memset(nodes, 0, sizeof nodes);
    nodes[0].used = 1;
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].mode = MODE_DIR;
    nodes[0].len = 2;
    now(&nodes[0].ctime);
    nodes[0].mtime = nodes[0].ctime;
    n_nodes = 1;
    mc_formatted = 1;
    mc_cwd = 0;
    index_dir(0, mc_root, 1);
    mc_state = 2;
    hle_mc_inserted = 1;
    int nf = 0, nd = 0;
    for (int i = 1; i < n_nodes; i++) if (nodes[i].used) { if (nodes[i].is_dir) nd++; else nf++; }
    LOG("memcard: slot 1 = %s (%s, %d directories, %d files, %u KB free of 8,000)\n", mc_root,
        mc_formatted ? "formatted" : "UNFORMATTED", nd, nf, mc_formatted ? free_clusters() : 0u);
    for (int i = 1; i < n_nodes; i++)
        if (nodes[i].used && nodes[i].parent == 0)
            LOG("memcard:   %s%s (%d entries)\n", ps2_path(i), nodes[i].is_dir ? "/" : "",
                nodes[i].is_dir ? child_count(i) : 0);
}

/* ------------------------------------------------------------------ command registration (libmc) */
/* returns 1 when the command may run; else sets v0 to the libmc refusal (pending command number) */
static int mc_begin(R5900Ctx *c)
{
    mc_configure();
    if (mc_cmd) { RET(mc_cmd); return 0; }
    return 1;
}

static void mc_end(R5900Ctx *c, int cmd, int result, uint64_t cost)
{
    mc_cmd = cmd;
    mc_result = result;
    RET(0);
    mc_charge(c, MC_COST_CMD + cost);
}

static int fd_valid(int fd) { return fd >= 0 && fd < MC_MAX_FD && fh[fd].used; }

static void close_handles_of(int node)
{
    for (int i = 0; i < MC_MAX_FD; i++) if (fh[i].used && fh[i].node == node) fh[i].used = 0;
}

/* ------------------------------------------------------------------ staged writes */
/* copies a whole file through the platform layer (one cached fd: open/close per chunk - rare paths) */
static int copy_file(const char *from, const char *to, uint32_t size)
{
    if (!size) return hle_plat_mc_write(to, 0, NULL, 0, 1) < 0 ? -1 : 0;
    for (uint32_t off = 0; off < size;) {
        uint32_t n = size - off < sizeof copybuf ? size - off : (uint32_t)sizeof copybuf;
        if (hle_plat_mc_read(from, off, copybuf, n) != (int64_t)n) return -1;
        if (hle_plat_mc_write(to, off, copybuf, n, off == 0) != (int64_t)n) return -1;
        off += n;
    }
    hle_plat_mc_release();
    return 0;
}

static int stage_begin(int node, int copy)
{
    char tmp[400], orig[400];
    host_path(node, MC_TMP_SUFFIX, tmp, sizeof tmp);
    hle_plat_mc_release();
    if (hle_plat_mc_exists(tmp)) hle_plat_mc_remove(tmp);
    if (!copy || !nodes[node].size) return hle_plat_mc_write(tmp, 0, NULL, 0, 1) < 0 ? -1 : 0;
    host_path(node, NULL, orig, sizeof orig);
    return copy_file(orig, tmp, nodes[node].size);         /* rare: O_RDWR / O_WRONLY on an existing file */
}

static int stage_commit(int node)
{
    char tmp[400], fin[400], orig[400];
    host_path(node, MC_TMP_SUFFIX, tmp, sizeof tmp);
    host_path(node, MC_NEW_SUFFIX, fin, sizeof fin);
    host_path(node, NULL, orig, sizeof orig);
    hle_plat_mc_release();                                 /* close: the FAT entry gets its size now */
    if (hle_plat_mc_exists(fin)) hle_plat_mc_remove(fin);
    int e = hle_plat_mc_rename(tmp, fin);
    if (e >= 0) {
        if (hle_plat_mc_exists(orig)) hle_plat_mc_remove(orig);
        e = hle_plat_mc_rename(fin, orig);
        if (e < 0) LOG("memcard: rename %s -> %s failed (%08x)\n", fin, orig, (unsigned)e);
    } else {
        LOG("memcard: rename %s -> %s failed (%08x)\n", tmp, fin, (unsigned)e);
    }
    if (e < 0) {                                           /* a file system without rename: copy in place */
        const char *src = hle_plat_mc_exists(fin) ? fin : tmp;
        if (hle_plat_mc_exists(orig)) hle_plat_mc_remove(orig);
        if (copy_file(src, orig, nodes[node].size) < 0) return -1;
        hle_plat_mc_remove(src);
        LOG("memcard: %s written by copying instead\n", orig);
    }
    hle_plat_mc_set_mtime(orig, &nodes[node].mtime);
    return 0;
}

/* ================================================================== hooks */
HLE_SCE(sceMcInit)                                                 /* sceMcInit(type) */
{
    hle_trace_call("sceMcInit", HLE_ADDR(sceMcInit), c);
    mc_configure();
    RET(0);
}

HLE_SCE(sceMcEnd)                                                 /* sceMcEnd */
{
    if (mc_state == 2) for (int i = 0; i < MC_MAX_FD; i++) fh[i].used = 0;
    RET(0);
}

HLE_SCE(sceMcGetSlotMax) { RET(1); }                                     /* sceMcGetSlotMax(port) */

HLE_SCE(sceMcGetInfo)                                                 /* sceMcGetInfo(port, slot, *type, *free, *format) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) {
        if (ARG(2)) sw_(ARG(2), 0);
        if (ARG(3)) sw_(ARG(3), 0);
        if (ARG(4)) sw_(ARG(4), 0);
        MCLOG("sceMcGetInfo(%d, %d) -> no card\n", SARG(0), SARG(1));
        mc_end(c, MCF_GET_INFO, MC_NOCARD, 0);
        return;
    }
    hle_trace_call("sceMcGetInfo", HLE_ADDR(sceMcGetInfo), c);
    uint32_t fr = mc_formatted ? free_clusters() : 0;
    if (ARG(2)) sw_(ARG(2), 2);                               /* sceMcTypePS2 */
    if (ARG(3)) sw_(ARG(3), fr);
    if (ARG(4)) sw_(ARG(4), (uint32_t)mc_formatted);
    int r = !mc_formatted ? MC_NOFORMAT : mc_info_seen ? MC_OK : MC_CHANGED;
    if (mc_formatted) mc_info_seen = 1;
    MCLOG("sceMcGetInfo(%d, %d) -> PS2 card, %u KB free, %s, result %d\n", port, SARG(1), fr,
         mc_formatted ? "formatted" : "unformatted", r);
    mc_end(c, MCF_GET_INFO, r, 0);
}

HLE_SCE(sceMcSync)                                                 /* sceMcSync(mode, *cmd, *result) */
{
    if (!mc_cmd) { RET(-1); return; }                         /* sceMcExecIdle: nothing registered */
    if (ARG(1)) sw_(ARG(1), (uint32_t)mc_cmd);
    if (ARG(2)) sw_(ARG(2), (uint32_t)mc_result);
    mc_cmd = 0;
    RET(1);                                                   /* sceMcExecFinish */
}

/* sceMcOpen(port, slot, name, mode) - also sceMcMkdir (mode 0x40) */
static void mc_open(R5900Ctx *c, int port, const char *name, int flags, int cmd)
{
    int r;
    if (!card_present(port)) { mc_end(c, cmd, MC_NOCARD, 0); return; }
    hle_trace_call(cmd == MCF_MKDIR ? "sceMcMkdir" : "sceMcOpen", cmd == MCF_MKDIR ? HLE_ADDR(sceMcMkdir)
                                                                                 : HLE_ADDR(sceMcOpen), c);
    if (!mc_formatted) { mc_end(c, cmd, MC_NOFORMAT, 0); return; }
    if (flags & MC_CREATE) flags |= MCA_WRITE;
    int fd = -1;
    for (int i = 0; i < MC_MAX_FD; i++) if (!fh[i].used) { fd = i; break; }
    if (!name[0]) r = MC_NOENTRY;
    else if (fd < 0 && !(flags & MC_CREATE_DIR)) r = MC_UPLIMIT;
    else {
        int parent;
        const char *leaf;
        size_t leaf_len = 0;
        int nd = resolve(name, &parent, &leaf, &leaf_len);
        if (nd == -2) r = MC_NOENTRY;
        else if (nd >= 0 && (flags & MC_CREATE_DIR)) r = MC_NOENTRY;            /* mkdir of an existing name */
        else if (nd < 0 && !(flags & (MC_CREATE | MC_CREATE_DIR))) r = MC_NOENTRY;
        else if (nd >= 0 && nodes[nd].is_dir) {                               /* a directory handle (dread) */
            fh[fd] = (McHandle){.used = 1, .node = nd, .dir = 1};
            r = fd;
        } else if (nd >= 0) {                                                 /* an existing file */
            int busy = 0;
            if (flags & MCA_WRITE)
                for (int i = 0; i < MC_MAX_FD; i++) if (fh[i].used && fh[i].node == nd && fh[i].wr) busy = 1;
            if (busy) r = MC_DENIED;
            else {
                McNode *n = &nodes[nd];
                int create = (flags & MC_CREATE) != 0;
                if (create) {                                                 /* recreated empty (mcman_open2) */
                    n->size = 0;
                    n->mode = MODE_FILE_NEW;
                    now(&n->ctime);
                    n->mtime = n->ctime;
                }
                fh[fd] = (McHandle){.used = 1, .node = nd, .rd = (flags & MCA_READ) != 0, .wr = (flags & MCA_WRITE) != 0};
                r = fd;
                if (fh[fd].wr) {
                    if (stage_begin(nd, !create) < 0) {
                        LOG("memcard: cannot stage a write of %s\n", ps2_path(nd));
                        fh[fd].used = 0;
                        r = MC_FULL;
                    } else fh[fd].staged = 1;
                }
            }
        } else {                                                              /* create: file or directory */
            char nm[32];
            int grow = nodes[parent].dslots == 0 && (nodes[parent].len & 1) == 0;   /* the directory needs a cluster */
            uint32_t need = (uint32_t)grow + ((flags & MC_CREATE_DIR) ? 1u : 0u);
            if (leaf_len > 31 || leaf_len == 0) r = MC_NOENTRY;
            else if (free_clusters() < need) r = MC_FULL;
            else {
                memcpy(nm, leaf, leaf_len);
                nm[leaf_len] = 0;
                int k = node_alloc();
                if (k < 0) r = MC_FULL;
                else {
                    McNode *n = &nodes[k];
                    snprintf(n->name, sizeof n->name, "%s", nm);
                    n->used = 1;
                    n->parent = (int16_t)parent;
                    n->is_dir = (flags & MC_CREATE_DIR) != 0;
                    n->mode = n->is_dir ? MODE_DIR : MODE_FILE_NEW;
                    n->len = 2;
                    n->order = ++next_order;
                    now(&n->ctime);
                    n->mtime = n->ctime;
                    nodes[parent].mtime = n->ctime;
                    if (nodes[parent].dslots) nodes[parent].dslots--; else nodes[parent].len++;
                    if (n->is_dir) {
                        char hp[400];
                        host_path(k, NULL, hp, sizeof hp);
                        hle_plat_mc_release();
                        int e = hle_plat_mc_mkdir(hp);
                        LOG("memcard: mkdir %s (%s)\n", ps2_path(k), e < 0 ? "FAILED on the memory stick" : "ok");
                        r = MC_OK;
                        if (e < 0) {                                  /* stick full / write-protected: undo */
                            nodes[k].used = 0;
                            nodes[parent].dslots++;
                            r = MC_FULL;
                        }
                    } else {
                        fh[fd] = (McHandle){.used = 1, .node = k, .rd = (flags & MCA_READ) != 0, .wr = 1};
                        if (stage_begin(k, 0) < 0) {
                            LOG("memcard: cannot create %s on the memory stick\n", ps2_path(k));
                            fh[fd].used = 0;
                            nodes[k].used = 0;
                            nodes[parent].dslots++;
                            r = MC_FULL;
                        } else {
                            fh[fd].staged = 1;
                            r = fd;
                        }
                    }
                }
            }
        }
    }
    MCLOG("%s(%d, \"%s\", %x) = %d\n", cmd == MCF_MKDIR ? "sceMcMkdir" : "sceMcOpen", port, name, flags, r);
    mc_end(c, cmd, r, 0);
}

HLE_SCE(sceMcOpen)                                                 /* sceMcOpen(port, slot, name, mode) */
{
    if (!mc_begin(c)) return;
    mc_open(c, SARG(0), gstr(ARG(2)), SARG(3), MCF_OPEN);
}

HLE_SCE(sceMcMkdir)                                                 /* sceMcMkdir(port, slot, name) */
{
    if (!mc_begin(c)) return;
    mc_open(c, SARG(0), gstr(ARG(2)), MC_CREATE_DIR, MCF_MKDIR);
}

#ifndef HLE_PSP
/* host investigations: HLE_MC_RAMDUMP=<prefix> writes the guest RAM to <prefix>_save.bin / <prefix>_load.bin when a
   whole file was written / read (the state the save captured, the state right after a load) */
static void ram_dump(const char *what)
{
    const char *p = getenv("HLE_MC_RAMDUMP");
    if (!p) return;
    char fn[512];
    snprintf(fn, sizeof fn, "%s_%s.bin", p, what);
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    fwrite(hle_ram, 1, hle_ram_end, f);
    fclose(f);
    LOG("memcard: guest RAM dumped to %s (vtime %.3f s)\n", fn, (double)hle_vtime / EE_HZ);
}
#else
static void ram_dump(const char *what) { (void)what; }
#endif

HLE_SCE(sceMcClose)                                                 /* sceMcClose(fd) */
{
    if (!mc_begin(c)) return;
    int fd = SARG(0);
    if (mc_state != 2) { mc_end(c, MCF_CLOSE, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcClose", HLE_ADDR(sceMcClose), c);
    if (!fd_valid(fd)) { mc_end(c, MCF_CLOSE, MC_DENIED, 0); return; }
    McHandle *h = &fh[fd];
    McNode *n = &nodes[h->node];
    int r = MC_OK;
    if (h->staged) {
        n->mode |= MCA_CLOSED;
        now(&n->mtime);
        if (n->parent >= 0) nodes[n->parent].mtime = n->mtime;
        double t0 = hle_host_seconds();
        int e = stage_commit(h->node);
        LOG("memcard: saved %s (%u bytes, %u writes) %s, %.0f ms\n", ps2_path(h->node), n->size, h->calls,
            e ? "FAILED" : "ok", (hle_host_seconds() - t0) * 1000.0);
        if (e) r = MC_FULL;
        if (n->size > 65536) ram_dump("save");
    } else if (!h->dir) {
        hle_plat_mc_release();
        if (h->calls) LOG("memcard: loaded %s (%llu of %u bytes, %u reads)\n", ps2_path(h->node),
                          (unsigned long long)h->bytes, n->size, h->calls);
        if (h->bytes == n->size && n->size > 65536) ram_dump("load");
    }
    h->used = 0;
    MCLOG("sceMcClose(%d) = %d\n", fd, r);
    mc_end(c, MCF_CLOSE, r, 0);
}

HLE_SCE(sceMcSeek)                                                 /* sceMcSeek(fd, offset, origin) */
{
    if (!mc_begin(c)) return;
    int fd = SARG(0), off = SARG(1), wh = SARG(2);
    if (mc_state != 2) { mc_end(c, MCF_SEEK, MC_NOCARD, 0); return; }
    if (!fd_valid(fd)) { mc_end(c, MCF_SEEK, MC_DENIED, 0); return; }
    McHandle *h = &fh[fd];
    int64_t base = wh == 0 ? 0 : wh == 2 ? (int64_t)nodes[h->node].size : (int64_t)h->pos;
    int64_t p = base + off;
    if (p < 0) p = 0;
    h->pos = (uint32_t)p;
    MCLOG("sceMcSeek(%d, %d, %d) = %u\n", fd, off, wh, h->pos);
    mc_end(c, MCF_SEEK, (int)h->pos, 0);
}

HLE_SCE(sceMcRead)                                                 /* sceMcRead(fd, buf, size) */
{
    if (!mc_begin(c)) return;
    int fd = SARG(0);
    uint32_t buf = ARG(1);
    int size = SARG(2);
    if (mc_state != 2) { mc_end(c, MCF_READ, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcRead", HLE_ADDR(sceMcRead), c);
    if (!fd_valid(fd) || !fh[fd].rd || size < 0) { mc_end(c, MCF_READ, MC_DENIED, 0); return; }
    McHandle *h = &fh[fd];
    McNode *n = &nodes[h->node];
    uint32_t want = (uint32_t)size, avail = h->pos < n->size ? n->size - h->pos : 0;
    if (want > avail) want = avail;
    int r = 0;
    if (want) {
        char hp[400];
        host_path(h->node, h->staged ? MC_TMP_SUFFIX : NULL, hp, sizeof hp);
        int64_t got = hle_plat_mc_read(hp, h->pos, gptr(buf, want), want);
        if (got < 0) { LOG("memcard: read error on %s\n", ps2_path(h->node)); r = MC_DENIED; h->used = 0; }
        else { r = (int)got; h->pos += (uint32_t)got; h->bytes += (uint64_t)got; }
    }
    h->calls++;
    MCLOG("sceMcRead(%d, %08x, %d) = %d\n", fd, buf, size, r);
    mc_end(c, MCF_READ, r, r > 0 ? (uint64_t)r / 8 : 0);
}

HLE_SCE(sceMcWrite)                                                 /* sceMcWrite(fd, buf, size) */
{
    if (!mc_begin(c)) return;
    int fd = SARG(0);
    uint32_t buf = ARG(1);
    int size = SARG(2);
    if (mc_state != 2) { mc_end(c, MCF_WRITE, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcWrite", HLE_ADDR(sceMcWrite), c);
    if (!fd_valid(fd) || !fh[fd].wr || !fh[fd].staged || size < 0) { mc_end(c, MCF_WRITE, MC_DENIED, 0); return; }
    McHandle *h = &fh[fd];
    McNode *n = &nodes[h->node];
    uint32_t end = h->pos + (uint32_t)size;
    uint32_t newsize = end > n->size ? end : n->size;
    int r;
    if (clusters_of(newsize) - clusters_of(n->size) > free_clusters()) r = MC_FULL;
    else if (!size) r = 0;
    else {
        char hp[400];
        host_path(h->node, MC_TMP_SUFFIX, hp, sizeof hp);
        int64_t w = hle_plat_mc_write(hp, h->pos, gptr(buf, (uint32_t)size), (uint32_t)size, 0);
        if (w != size) { LOG("memcard: write error on %s (%lld)\n", ps2_path(h->node), (long long)w); r = MC_FULL; }
        else { r = size; h->pos = end; n->size = newsize; h->bytes += (uint64_t)size; }
    }
    h->calls++;
    MCLOG("sceMcWrite(%d, %08x, %d) = %d\n", fd, buf, size, r);
    mc_end(c, MCF_WRITE, r, r > 0 ? (uint64_t)r / 8 : 0);
}

HLE_SCE(sceMcChdir)                                                 /* sceMcChdir(port, slot, newdir, currentdir) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_CHDIR, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcChdir", HLE_ADDR(sceMcChdir), c);
    if (!mc_formatted) { mc_end(c, MCF_CHDIR, MC_NOFORMAT, 0); return; }
    const char *path = gstr(ARG(2));
    int nd = resolve(path, NULL, NULL, NULL);
    int r = MC_NOENTRY;
    if (nd >= 0 && nodes[nd].is_dir) {
        mc_cwd = nd;
        r = MC_OK;
        if (ARG(3)) {
            const char *cur = ps2_path(nd);
            gcopy_out(ARG(3), cur, (uint32_t)strlen(cur) + 1);
        }
    }
    MCLOG("sceMcChdir(%d, \"%s\") = %d\n", port, path, r);
    mc_end(c, MCF_CHDIR, r, 0);
}

/* one 64-byte sceMcTblGetDir entry */
static void put_time(uint32_t a, const HleMcTime *t)
{
    sb_(a + 0, 0);
    sb_(a + 1, t->sec);
    sb_(a + 2, t->min);
    sb_(a + 3, t->hour);
    sb_(a + 4, t->day);
    sb_(a + 5, t->month);
    sh_(a + 6, t->year);
}
static void put_entry(uint32_t a, const char *name, const McNode *n)
{
    memset(gptr(a, 64), 0, 64);
    put_time(a + 0, &n->ctime);
    put_time(a + 8, &n->mtime);
    sw_(a + 16, n->is_dir ? 0 : n->size);
    sh_(a + 20, n->mode);
    size_t l = strlen(name);
    gcopy_out(a + 32, name, (uint32_t)(l < 31 ? l : 31));
}

HLE_SCE(sceMcGetDir)                                    /* sceMcGetDir(port, slot, name, mode, maxent, table) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0), mode = SARG(3), maxent = SARG(4);
    uint32_t tbl = ARG(5);
    if (!card_present(port)) { mc_end(c, MCF_GETDIR, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcGetDir", HLE_ADDR(sceMcGetDir), c);
    if (!mc_formatted) { mc_end(c, MCF_GETDIR, MC_NOFORMAT, 0); return; }
    const char *name = gstr(ARG(2));
    if ((mode & 0xFFFF) == 0) {
        char dirp[160];
        const char *sl = strrchr(name, '/');
        if (!sl) { dirp[0] = 0; snprintf(gd_pattern, sizeof gd_pattern, "%s", name); }
        else {
            size_t dl = (size_t)(sl - name);
            if (dl >= sizeof dirp) dl = sizeof dirp - 1;
            memcpy(dirp, name, dl);
            dirp[dl] = 0;
            if (dl == 0) snprintf(dirp, sizeof dirp, "/");
            snprintf(gd_pattern, sizeof gd_pattern, "%s", sl + 1);
        }
        int nd = resolve(dirp, NULL, NULL, NULL);
        if (nd < 0 || !nodes[nd].is_dir) {
            gd_dir = -1;
            MCLOG("sceMcGetDir(%d, \"%s\", %d) = -4\n", port, name, mode);
            mc_end(c, MCF_GETDIR, MC_NOENTRY, 0);
            return;
        }
        gd_dir = nd;
        gd_pos = nd == 0 ? 2 : 0;                             /* the root lists no "." / ".." */
        gd_last_order = 0;
    } else if (gd_dir < 0) {
        mc_end(c, MCF_GETDIR, MC_NOENTRY, 0);
        return;
    }
    int count = 0;
    while (maxent > 0) {
        const char *ename;
        const McNode *src;
        if (gd_pos == 0) { ename = "."; src = &nodes[gd_dir]; }
        else if (gd_pos == 1) { ename = ".."; src = &nodes[nodes[gd_dir].parent >= 0 ? nodes[gd_dir].parent : 0]; }
        else {
            int best = -1;                                    /* next child in listing order */
            for (int i = 1; i < n_nodes; i++)
                if (nodes[i].used && nodes[i].parent == gd_dir && nodes[i].order > gd_last_order &&
                    (best < 0 || nodes[i].order < nodes[best].order))
                    best = i;
            if (best < 0) break;
            gd_last_order = nodes[best].order;
            ename = nodes[best].name;
            src = &nodes[best];
        }
        gd_pos++;
        if (!name_match(ename, gd_pattern)) continue;
        put_entry(tbl + 64u * (uint32_t)count, ename, src);
        count++;
        maxent--;
    }
    MCLOG("sceMcGetDir(%d, \"%s\", %d, %d) = %d\n", port, name, mode, SARG(4), count);
    mc_end(c, MCF_GETDIR, count, (uint64_t)count * 64);
}

HLE_SCE(sceMcSetFileInfo)                                    /* sceMcSetFileInfo(port, slot, name, info, flags) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0), flags = SARG(4);
    if (!card_present(port)) { mc_end(c, MCF_SETINFO, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcSetFileInfo", HLE_ADDR(sceMcSetFileInfo), c);
    if (!mc_formatted) { mc_end(c, MCF_SETINFO, MC_NOFORMAT, 0); return; }
    const char *name = gstr(ARG(2));
    uint32_t info = ARG(3);
    int nd = resolve(name, NULL, NULL, NULL), r = MC_OK;
    if (nd <= 0) r = MC_NOENTRY;
    else {
        McNode *n = &nodes[nd];
        if (flags & 0x01) { n->ctime.sec = gb(info + 1); n->ctime.min = gb(info + 2); n->ctime.hour = gb(info + 3);
                            n->ctime.day = gb(info + 4); n->ctime.month = gb(info + 5); n->ctime.year = gh(info + 6); }
        if (flags & 0x02) { n->mtime.sec = gb(info + 9); n->mtime.min = gb(info + 10); n->mtime.hour = gb(info + 11);
                            n->mtime.day = gb(info + 12); n->mtime.month = gb(info + 13); n->mtime.year = gh(info + 14); }
        if (flags & 0x04) n->mode = (uint16_t)((n->mode & ~0x180Fu) | (gh(info + 20) & 0x180Fu));
        if (flags & 0x10) {                                   /* new name (sceMcFileInfoName) */
            char nn[32], from[400], to[400];
            gcopy_in(nn, info + 32, 31);
            nn[31] = 0;
            if (!nn[0] || strchr(nn, '/') || !strcmp(nn, ".") || !strcmp(nn, "..")) r = MC_NOENTRY;
            else if (child_find(n->parent, nn, strlen(nn)) >= 0) r = MC_DENIED;
            else {
                host_path(nd, NULL, from, sizeof from);
                snprintf(n->name, sizeof n->name, "%s", nn);
                host_path(nd, NULL, to, sizeof to);
                hle_plat_mc_release();
                int e = hle_plat_mc_rename(from, to);
                LOG("memcard: rename %s -> %s (%d)\n", from, to, e);
            }
        }
    }
    MCLOG("sceMcSetFileInfo(%d, \"%s\", %x) = %d\n", port, name, flags, r);
    mc_end(c, MCF_SETINFO, r, 0);
}

static void delete_from_stick(int nd)
{
    char hp[400];
    host_path(nd, NULL, hp, sizeof hp);
    hle_plat_mc_release();
    int e = nodes[nd].is_dir ? hle_plat_mc_rmdir(hp) : hle_plat_mc_remove(hp);
    if (e < 0) LOG("memcard: could not delete %s from the memory stick (%d)\n", hp, e);
}

HLE_SCE(sceMcDelete)                                                 /* sceMcDelete(port, slot, name) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_DELETE, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcDelete", HLE_ADDR(sceMcDelete), c);
    if (!mc_formatted) { mc_end(c, MCF_DELETE, MC_NOFORMAT, 0); return; }
    const char *name = gstr(ARG(2));
    int nd = resolve(name, NULL, NULL, NULL), r;
    if (nd <= 0) r = MC_NOENTRY;
    else if (nodes[nd].is_dir && child_count(nd)) r = MC_NOTEMPTY;
    else {
        close_handles_of(nd);
        delete_from_stick(nd);
        int p = nodes[nd].parent;
        nodes[p].dslots++;
        now(&nodes[p].mtime);
        if (mc_cwd == nd) mc_cwd = p;
        if (gd_dir == nd) gd_dir = -1;
        LOG("memcard: deleted %s\n", ps2_path(nd));
        nodes[nd].used = 0;
        r = MC_OK;
    }
    MCLOG("sceMcDelete(%d, \"%s\") = %d\n", port, name, r);
    mc_end(c, MCF_DELETE, r, 0);
}

static void wipe_card(void)
{
    hle_plat_mc_release();
    for (int pass = 0; pass < 16; pass++) {                   /* leaves first */
        int left = 0;
        for (int i = n_nodes - 1; i > 0; i--) {
            if (!nodes[i].used) continue;
            if (nodes[i].is_dir && child_count(i)) { left++; continue; }
            delete_from_stick(i);
            nodes[i].used = 0;
        }
        if (!left) break;
    }
    for (int i = 0; i < MC_MAX_FD; i++) fh[i].used = 0;
    n_nodes = 1;
    nodes[0].len = 2;
    nodes[0].dslots = 0;
    now(&nodes[0].ctime);
    nodes[0].mtime = nodes[0].ctime;
    mc_cwd = 0;
    gd_dir = -1;
}

HLE_SCE(sceMcFormat)                                                 /* sceMcFormat(port, slot) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_FORMAT, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcFormat", HLE_ADDR(sceMcFormat), c);
    wipe_card();
    char mark[400];
    snprintf(mark, sizeof mark, "%s%s", mc_root, MC_UNFORMATTED_MARK);
    if (hle_plat_mc_exists(mark)) hle_plat_mc_remove(mark);
    mc_formatted = 1;
    LOG("memcard: formatted (%u KB free)\n", free_clusters());
    mc_end(c, MCF_FORMAT, MC_OK, 0);
}

HLE_SCE(sceMcUnformat)                                                 /* sceMcUnformat(port, slot) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_UNFORMAT, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcUnformat", HLE_ADDR(sceMcUnformat), c);
    wipe_card();
    char mark[400];
    snprintf(mark, sizeof mark, "%s%s", mc_root, MC_UNFORMATTED_MARK);
    hle_plat_mc_write(mark, 0, NULL, 0, 1);
    hle_plat_mc_release();
    mc_formatted = 0;
    LOG("memcard: unformatted\n");
    mc_end(c, MCF_UNFORMAT, MC_OK, 0);
}

HLE_SCE(sceMcGetEntSpace)                                                 /* sceMcGetEntSpace(port, slot, path) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_GETENT, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcGetEntSpace", HLE_ADDR(sceMcGetEntSpace), c);
    if (!mc_formatted) { mc_end(c, MCF_GETENT, MC_NOFORMAT, 0); return; }
    const char *path = gstr(ARG(2));
    int nd = resolve(path, NULL, NULL, NULL), r;
    if (nd < 0 || !nodes[nd].is_dir) r = MC_NOENTRY;
    else r = (int)((nodes[nd].len & 1u) + nodes[nd].dslots);
    MCLOG("sceMcGetEntSpace(%d, \"%s\") = %d\n", port, path, r);
    mc_end(c, MCF_GETENT, r, 0);
}

HLE_SCE(sceMcRename)                                                 /* sceMcRename(port, slot, old, new) */
{
    if (!mc_begin(c)) return;
    int port = SARG(0);
    if (!card_present(port)) { mc_end(c, MCF_RENAME, MC_NOCARD, 0); return; }
    hle_trace_call("sceMcRename", HLE_ADDR(sceMcRename), c);
    if (!mc_formatted) { mc_end(c, MCF_RENAME, MC_NOFORMAT, 0); return; }
    const char *from_name = gstr(ARG(2));
    char nn[64];
    snprintf(nn, sizeof nn, "%s", gstr(ARG(3)));
    int nd = resolve(from_name, NULL, NULL, NULL), r = MC_OK;
    if (nd <= 0) r = MC_NOENTRY;
    else if (!nn[0] || strlen(nn) > 31 || strchr(nn, '/')) r = MC_NOENTRY;
    else if (child_find(nodes[nd].parent, nn, strlen(nn)) >= 0) r = MC_DENIED;
    else {
        char from[400], to[400];
        host_path(nd, NULL, from, sizeof from);
        snprintf(nodes[nd].name, sizeof nodes[nd].name, "%s", nn);
        host_path(nd, NULL, to, sizeof to);
        hle_plat_mc_release();
        int e = hle_plat_mc_rename(from, to);
        LOG("memcard: rename %s -> %s (%d)\n", from, to, e);
    }
    mc_end(c, MCF_RENAME, r, 0);
}

HLE_SCE(sceMcFlush)                                                 /* sceMcFlush(fd) */
{
    if (!mc_begin(c)) return;
    if (mc_state != 2) { mc_end(c, MCF_FLUSH, MC_NOCARD, 0); return; }
    mc_end(c, MCF_FLUSH, fd_valid(SARG(0)) ? MC_OK : MC_DENIED, 0);
}

HLE_SCE(sceMcUdCheckNewCard) { RET(0); }                                     /* sceMcUdCheckNewCard */
