/* hle_menu_psp.c - the PSP side of the in-game settings menu (hle_menu.h): the button combination, the
 * pause (game thread inside the flip, sound output paused, the menu's real time left out of the pacing clock), the
 * menu's memory (a user partition block only while it is open), the cfg files, the log.
 *
 * ps2rc_dev.cfg keys (developer; the player needs none):
 *   menu=0|1             1 (default) = the menu can be opened
 *   menu_chord=a+b[+c[+d]] the combination (default l+r+cross+down = L + R + Cross + D-pad Down; names: select start
 *                        l r up down left right triangle circle cross square; rules: hle_menu_chord.h); it opens on the
 *                        last button's press, and the game gets neither that press nor the release after the menu
 *   menu_pool_kb=N       the menu's memory (default 320, 192..1024)
 *   menu_auto=T          open the menu by itself when the virtual time reaches T s (emulator checks)
 *   menu_chord_test=T    emulator check: from virtual T s the combination is pressed by itself (all but Down for
 *                        0.3 s, then Down too, released after 0.6 s) - the real opening path
 *   menu_script=...      the first opening runs this key sequence (hle_menu.h) and closes; with shot tokens the
 *                        frames are saved as menu_shot_<n>.bmp next to the EBOOT (emulator checks)
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hle.h"
#include "hle_cfg.h"
#include "hle_menu.h"
#include "hle_menu_chord.h"

_Static_assert(MC_SELECT == PSP_CTRL_SELECT && MC_START == PSP_CTRL_START && MC_DOWN == PSP_CTRL_DOWN &&
               MC_L == PSP_CTRL_LTRIGGER && MC_R == PSP_CTRL_RTRIGGER && MC_CROSS == PSP_CTRL_CROSS &&
               MC_UP == PSP_CTRL_UP && MC_LEFT == PSP_CTRL_LEFT && MC_RIGHT == PSP_CTRL_RIGHT &&
               MC_TRIANGLE == PSP_CTRL_TRIANGLE && MC_CIRCLE == PSP_CTRL_CIRCLE && MC_SQUARE == PSP_CTRL_SQUARE,
               "hle_menu_chord.h button bits = pspctrl.h");

void hle_plat_pace_shift(uint64_t us);                         /* hle_psp.c: real time left out of the pacing */
void hle_audio_pause(int on) __attribute__((weak));            /* the game pack's sound output (AUDIO=1 builds) */

static int m_init, m_on = 1, m_pool_kb = 320, m_opens;
static MenuChord m_mc = {MC_DEFAULT, 0, 0};
static double m_auto = -1.0, m_ctest = -1.0;
static const char *m_script;

static void m_cfg(void)
{
    m_init = 1;
    const char *v, *why;
    if ((v = hle_cfg_get("menu"))) m_on = atoi(v) != 0;
    if ((v = hle_cfg_get("menu_pool_kb"))) { m_pool_kb = atoi(v); m_pool_kb = m_pool_kb < 192 ? 192 : m_pool_kb > 1024 ? 1024 : m_pool_kb; }
    if ((v = hle_cfg_get("menu_auto"))) m_auto = strtod(v, NULL);
    if ((v = hle_cfg_get("menu_chord_test"))) m_ctest = strtod(v, NULL);
    m_script = hle_cfg_get("menu_script");
    if ((v = hle_cfg_get("menu_chord"))) {
        unsigned c = mc_parse(v, &why);
        if (c) m_mc.chord = c;
        else LOG("menu: menu_chord=%s: %s - l+r+cross+down kept\n", v, why);
    }
    LOG("menu: in-game settings menu %s (combination %s, pool %d KB while open)%s\n", m_on ? "on" : "OFF (menu=0)",
        m_mc.chord != MC_DEFAULT ? hle_cfg_get("menu_chord") : "l+r+cross+down", m_pool_kb,
        m_auto >= 0 ? " - opens by itself (menu_auto)" : "");
}

unsigned hle_menu_pad_filter(unsigned b)
{
    if (!m_init) m_cfg();
    if (!m_on) return b;
    int open;
    unsigned raw = b;
    if (m_ctest >= 0.0) {                                  /* emulator check: the combination pressed by itself */
        double t = (double)hle_vtime / EE_HZ - m_ctest;
        if (t >= 0.0 && t < 0.6) raw |= m_mc.chord & ~(t < 0.3 ? MC_DOWN : 0u);   /* all but Down, then Down too */
        else if (t >= 0.6) m_ctest = -1.0;
    }
    b = mc_filter(&m_mc, raw, &open);
    if (open) {
        hle_menu_req = 1;
        m_ctest = -1.0;                                    /* (the test presses once) */
        LOG("menu: combination %04x completed at vtime %.3f s; the game gets %04x on this read\n", raw,
            (double)hle_vtime / EE_HZ, b);
    }
    if (m_auto >= 0.0 && (double)hle_vtime / EE_HZ >= m_auto) { m_auto = -1.0; hle_menu_req = 1; }
    return b;
}

/* ------------------------------------------------------------------ HleMenuHost on the PSP */
static const HleMenuGe *m_ge;
static unsigned h_buttons(void)
{
    SceCtrlData p;
    if (sceCtrlPeekBufferPositive(&p, 1) <= 0) return 0;
    return p.Buttons & 0xFFFFu;
}
static int h_begin(void) { return m_ge->frame_begin(); }
static void h_end(int shot) { m_ge->frame_end(shot); }
static double h_seconds(void) { return (double)sceKernelGetSystemTimeWide() / 1e6; }
static void h_path(int dev, char *p, size_t n) { snprintf(p, n, "%s%s", hle_plat_disc_root(), hle_cfg_file(dev)); }
static int h_read(int dev, char *buf, int cap)
{
    char path[320];
    h_path(dev, path, sizeof path);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    return n < 0 ? -1 : n;
}
static int write_all(const char *path, const char *buf, int n)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int w = sceIoWrite(fd, buf, n);
    sceIoClose(fd);
    if (w != n) return -1;
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);                 /* read back the size */
    if (fd < 0) return -1;
    SceOff sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return sz == (SceOff)n ? 0 : -1;
}
/* <file>.new written and checked, the old file kept as <file>.bak, then renamed; renames refused -> written in place */
static int h_write(int dev, const char *buf, int n)
{
    char path[320], tmp[340], bak[340];
    h_path(dev, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    snprintf(bak, sizeof bak, "%s.bak", path);
    if (write_all(tmp, buf, n) < 0) { sceIoRemove(tmp); return write_all(path, buf, n); }
    SceIoStat st;
    int had = sceIoGetstat(path, &st) >= 0;
    sceIoRemove(bak);
    if (had && sceIoRename(path, bak) < 0) { sceIoRemove(tmp); return write_all(path, buf, n); }
    if (sceIoRename(tmp, path) < 0) {
        int r = write_all(path, buf, n);
        sceIoRemove(tmp);
        return r;
    }
    return 0;
}
static void h_log(const char *s) { LOG("%s\n", s); }
static SceUID tmp_uid = -1;
static void *h_tmp_alloc(unsigned n)
{
    tmp_uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "ps2rc_menu_tmp", PSP_SMEM_High, n, NULL);
    return tmp_uid < 0 ? NULL : sceKernelGetBlockHeadAddr(tmp_uid);
}
static void h_tmp_free(void *p)
{
    (void)p;
    if (tmp_uid >= 0) sceKernelFreePartitionMemory(tmp_uid);
    tmp_uid = -1;
}
static void h_idle(void)
{
    hle_progress++;                                        /* the watchdog sees progress while the menu is open */
    sceDisplayWaitVblankStart();
}

void hle_menu_run(const HleMenuGe *ge)
{
    hle_menu_req = 0;
    if (!m_init) m_cfg();
    if (!m_on) return;
    unsigned bytes = (unsigned)m_pool_kb << 10;
    SceUID uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "ps2rc_menu", PSP_SMEM_High, bytes, NULL);
    if (uid < 0) {
        LOG("menu: NOT opened - no %u KB of free memory (%08x; largest free block %u KB)\n", bytes >> 10, (unsigned)uid,
            (unsigned)(sceKernelMaxFreeMemSize() >> 10));
        return;
    }
    void *pool = sceKernelGetBlockHeadAddr(uid);
    m_opens++;
    LOG("menu: opened #%d at vtime %.3f s, frame %llu (pool %u KB, free after it %u KB)\n", m_opens,
        (double)hle_vtime / EE_HZ, (unsigned long long)hle_frame, bytes >> 10, (unsigned)(sceKernelTotalFreeMemSize() >> 10));
    if (hle_audio_pause) hle_audio_pause(1);
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    m_ge = ge;
    static const HleMenuHost host = {h_buttons, h_begin, h_end, h_seconds, h_read, h_write, h_log, h_idle, h_tmp_alloc,
                                     h_tmp_free};
    HleMenuResult r;
    memset(&r, 0, sizeof r);
    const char *script = m_opens == 1 ? m_script : NULL;
    int rc = hle_menu_ui(&host, pool, bytes, script, &r);
    SceInt64 dt = sceKernelGetSystemTimeWide() - t0;
    hle_plat_pace_shift((uint64_t)dt);                     /* the game does not race to catch up the menu's time */
    if (hle_audio_pause) hle_audio_pause(0);
    sceKernelFreePartitionMemory(uid);
    mc_closed(&m_mc, h_buttons());                         /* e.g. Circle: not for the game until released */
    if (rc < 0) LOG("menu: ImGui could not start in %u KB\n", bytes >> 10);
    LOG("menu: closed after %.1f s, %d frames: %d change(s) (%d live, %d after restart), %d save(s); pool peak %u of %u "
        "KB, %u failed allocations; max %u vertices, %u KB GE list per frame\n", (double)dt / 1e6, r.frames, r.changes,
        r.live, r.restart, r.saves, (r.pool_peak + 1023) >> 10, r.pool_bytes >> 10, r.failed_allocs, r.max_vertices,
        (r.max_list_bytes + 1023) >> 10);
    if (script) LOG("menu script: done\n");
}
