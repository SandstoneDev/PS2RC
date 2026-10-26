/* hle_mem_psp.h - the PSP's memory mode and reserves (PSP build only).
 *   - the CFW's SE configuration (SystemControl sctrlSEGetConfig, own import stub in hle_mem_psp.c)
 *   - ps2rc.cfg mem_volatile=1: the volatile 4 MB partition (sceKernelVolatileMemTryLock + scePowerLock) as a bump
 *     arena for our own big buffers (GE lists + texture arena, MMIO models, the intro movie's buffers), so that the
 *     guest code hole can take the audio block and the user partition can give the guest window (= the game heap)
 *     more memory (ps2rc.cfg heap_extra_kb=N). Default off: nothing here runs, the memory layout is the old one.
 */
#ifndef HLE_MEM_PSP_H
#define HLE_MEM_PSP_H
#include <stddef.h>
#include <stdint.h>

/* SE config: fills buf (n bytes, pre-filled with 0xA5 here) through sctrlSEGetConfig; returns its result, or
   0x7FFFFFFF when not called (emulator: PPSSPP has no SystemControl) */
int hle_mem_seconfig(uint8_t *buf, unsigned n);
/* ARK's runtime configuration (exec_mode at +144: PSP / PS Vita) through ARK-5's ArkCtrl (which = 5) or
   ARK-4's SystemCtrlForUser (which = 4); 1 = buf (>= 160 bytes) holds ARK's magic. ARK-5's sctrlHENGetSpeed (-1 = not
   called). Read-only, both. */
int hle_mem_arkconfig(uint8_t *buf, unsigned n, int which);
int hle_mem_hen_speed(void);

/* the volatile partition (all no-ops / NULL unless hle_mem_vol_lock succeeded) */
int hle_mem_vol_lock(void);                      /* 0 = locked, size >= 1 MB, memory touched and checked */
int hle_mem_vol_on(void);
void hle_mem_vol_info(char *out, unsigned n);    /* one human-readable status line (no newline) */
/* bump allocation: NULL unless size fits and leaves at least keep bytes free afterwards; 64-byte aligned, zeroed */
void *hle_mem_vol_take(size_t size, size_t keep);
/* gives the LAST taken block back (the intro movie's transient buffers); other blocks stay (1 = given back) */
int hle_mem_vol_give_back(void *p, size_t size);
size_t hle_mem_vol_free(void);
int hle_mem_vol_unlock(void);                    /* exit: after the GE and the audio are stopped */
int hle_mem_vol_unlock2(int keep_power);        /* keep_power = 1 keeps the scePowerLock (az_exit=1) */
int hle_mem_vol_power_unlock_rc(void);           /* its scePowerUnlock result (0x7FFFFFFF = not called) */

/* hle_psp.c: the guest code hole + the reserve policy (ps2rc.cfg mem_volatile) */
int hle_psp_mem_volatile(void);                  /* 1 = mem_volatile=1 AND the partition is locked */
void *hle_psp_hole_take(size_t size, size_t align);   /* NULL when the hole is full (not zeroed) */
/* KB that must stay in the hole for allocations still to come (the world3d vertex cache), for hle_audio_psp.c */
unsigned hle_psp_hole_free_kb(void);

/* the movie's transient block size the reserve keeps free in the volatile partition (hle_movie.c's maximum:
   work 64 KB + ring 538 KB + picture/sound/ATRAC 688 KB + .wav staging 32 KB, rounded up) */
#define HLE_MEM_VOL_MOVIE_KEEP (1344u * 1024u)

#endif
