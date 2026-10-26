/* hle_menu.h - the in-game settings menu (Dear ImGui through lib/psp_imgui).
 *
 * Open: L + R + Cross + D-pad Down held together in game (hle_menu_chord.h; ps2rc_dev.cfg menu_chord=<buttons joined
 * with +>, menu=0 = no menu). The
 * game stops inside its flip (all guest threads are coroutines of the game thread, so the whole game is paused), the
 * sound output pauses, and the last game picture stays under the menu. Every setting of ps2rc.cfg / ps2rc_dev.cfg is
 * listed by category (the game pack's registry, hle/core/hle_settings_reg.h); a change goes into the cfg table at once
 * (hle_cfg_set) and is applied live where the registry has a live setter, else marked "after restart". START saves
 * into the files, keeping their comments (hle/core/hle_cfg_save.c). Closing: Circle. The real time the menu was open is
 * left out of the game's pacing clock (hle_plat_pace_shift), so the game does not race to catch up.
 * Cost while closed: one compare per flip (hle_menu_req) and one in the pad read; RAM: the menu's pool
 * (menu_pool_kb, default 320) is taken from the free user memory only while the menu is open.
 *
 * Split: hle_menu.cpp = the menu (C++, ImGui, the registry, the cfg table, saving; no PSP calls: the host test drives
 * it through HleMenuHost), hle_menu_psp.c = the PSP side (combination, pool, pause, files, pacing, log).
 */
#ifndef HLE_MENU_H
#define HLE_MENU_H
#ifdef __cplusplus
extern "C" {
#endif

/* what the menu needs from its platform (all calls on the game thread) */
typedef struct {
    unsigned (*buttons)(void);                     /* PSP buttons held now (pspctrl bits) */
    int (*frame_begin)(void);                      /* 1 = a GE list is open on the next display buffer, game picture in */
    void (*frame_end)(int shot);                   /* finish + wait + present; shot > 0: also save that frame (BMP) */
    double (*seconds)(void);                       /* real time, seconds */
    int (*read_file)(int dev, char *buf, int cap); /* the player (0) / developer (1) cfg file: bytes, -1 = none */
    int (*write_file)(int dev, const char *buf, int n);   /* replace that file: 0 = ok */
    void (*log)(const char *line);                 /* one session.log line (no newline needed) */
    void (*idle)(void);                            /* once per menu frame after the present: vblank wait, watchdog */
    void *(*tmp_alloc)(unsigned bytes);            /* scratch for a save (file buffers, ~90 KB), NULL = no memory */
    void (*tmp_free)(void *p);
} HleMenuHost;

typedef struct {
    int frames, changes, saves, live, restart;     /* frames shown, values changed, saves, live / restart changes */
    unsigned pool_peak, pool_bytes, failed_allocs, max_vertices, max_list_bytes;
} HleMenuResult;

/* the modal menu: returns when it is closed. pool = memory for ImGui and the menu (given back afterwards); script =
   NULL, or an automatic key sequence (developer key menu_script, emulator checks): tokens separated by ',':
   U D L R (D-pad), X O T S (Cross Circle Triangle Square), LT RT (L / R), ST SE (START / SELECT), wN (wait N frames),
   shot (save the frame), close; the menu closes when the script ends. 0 = closed, -1 = no memory for ImGui. */
int hle_menu_ui(const HleMenuHost *h, void *pool, unsigned pool_bytes, const char *script, HleMenuResult *res);

/* PSP side (hle_menu_psp.c) */
extern volatile int hle_menu_req;                  /* hle_pad.c: the combination was pressed (or menu_auto is due) */
unsigned hle_menu_pad_filter(unsigned buttons);    /* hle_pad.c, every pad read: combination, buttons kept from the game */
typedef struct {
    int (*frame_begin)(void);
    void (*frame_end)(int shot);
} HleMenuGe;
void hle_menu_run(const HleMenuGe *ge);            /* the GE back-end's flip, when hle_menu_req is set */

#ifdef __cplusplus
}
#endif
#endif
