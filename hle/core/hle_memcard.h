/* hle_memcard.h - PS2 memory card (libmc) HLE backed by files: interface between the core (hle/core/hle_memcard.c)
 * and the platform file layer (host: hle/host/hle_mcfs_host.c, PSP: hle/psp/hle_mcfs_psp.c).
 *
 * The card in slot 1 (port 0) is a directory tree that mirrors mc0:/ - e.g. <root>/BESLUS-12345SAVE00/SAVE0.
 * Slot 2 (port 1) is always empty. Names the platform layer finds that contain "_ps2rc" are the runtime's own
 * (temporary files of a crash-safe write, the unformatted-card marker) and never reach the game.
 */
#ifndef HLE_MEMCARD_H
#define HLE_MEMCARD_H

#include <stddef.h>
#include <stdint.h>

/* PS2 date/time as libmc reports it (sceMcStDateTime without the reserved byte) */
typedef struct {
    uint8_t sec, min, hour, day, month;
    uint16_t year;
} HleMcTime;

/* callback of hle_plat_mc_list: one entry of a directory ("." and ".." are not passed) */
typedef void (*HleMcListFn)(void *ctx, const char *name, int is_dir, uint32_t size, const HleMcTime *mtime,
                            const HleMcTime *ctime);

/* ------------------------------------------------------------------ platform file layer */
/* configuration: returns 1 when a card is inserted in slot 1 and writes its root directory (ends with '/') to root,
   0 = no card (the host without --memcard, PSP poc6.cfg memcard=0). Called once, at the game's first sceMcInit. */
int hle_plat_mc_config(char *root, size_t n);
/* the clock that stamps entries the game creates or changes (PSP: the RTC's local time; host: a fixed date + the
   virtual time, deterministic) */
void hle_plat_mc_now(HleMcTime *t);
/* directory operations, paths absolute (root + PS2 path); 0 = ok, < 0 = error */
int hle_plat_mc_list(const char *dir, HleMcListFn fn, void *ctx);
int hle_plat_mc_exists(const char *path);                 /* 1 file, 2 directory, 0 none */
int hle_plat_mc_mkdir(const char *path);
int hle_plat_mc_rmdir(const char *path);
int hle_plat_mc_remove(const char *path);
int hle_plat_mc_rename(const char *from, const char *to); /* the target must not exist */
/* file data. The layer may keep ONE file open between calls (the PSP's open-file limit: the disc layer holds up to
   8 files, session.log opens and closes per commit); hle_plat_mc_release() closes it - after it the data and the
   size are on the stick (FAT commits a file's size only on close). create = 1: the file is created / truncated. */
int64_t hle_plat_mc_read(const char *path, uint32_t off, void *dst, uint32_t n);
int64_t hle_plat_mc_write(const char *path, uint32_t off, const void *src, uint32_t n, int create);
void hle_plat_mc_release(void);
/* host after a write: sets the file's modification time (keeps a later listing deterministic); PSP: no-op */
void hle_plat_mc_set_mtime(const char *path, const HleMcTime *t);

/* ------------------------------------------------------------------ core */
extern int hle_mc_inserted;               /* 1 once the card was found / created at sceMcInit (log + tests) */
#ifndef HLE_PSP
extern const char *hle_mc_host_dir;       /* host harness --memcard DIR (hle_host.c) */
#endif

#endif
