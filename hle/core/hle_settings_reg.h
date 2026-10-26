/* hle_settings_reg.h - the settings registry of the in-game menu (hle/psp/hle_menu.cpp).
 *
 * ONE line per setting, in the game pack (hle_settings_table(), e.g. src/hle_settings_menu.c), next to the friendly-name
 * table of hle_cfg (hle_cfg_alias_table(), hle_settings.c). A new player setting = its alias line (hle_settings.c) +
 * one line here; a friendly name without a line here still shows up (category "Other", its name as label, "applies
 * after restart"), so nothing is ever unreachable - tests/cfg/test_cm_menu.py lists the missing lines.
 *
 * key      a friendly name (an alias: range, words, code default and inversion come from the alias table) or an
 *          internal key of the developer file (then lo / hi / step / def / choices below describe it)
 * cat      the menu tab ("Graphics", "World", "Performance", "Controls", "Audio", "Developer", ...; tabs appear in the
 *          order of their first line)
 * label    the title shown (short; a long one scrolls as a marquee - never shrunk)
 * help     one line of help (help bar marquee; Square shows it wrapped)
 * rec      the recommended value (the converter's template value, friendly text) - Triangle and "Recommended values";
 *          NULL = the code default
 * choices  "a,b,c": the values the menu steps through (word keys, e.g. map_l2's combinations); NULL = numeric (the
 *          alias range, or lo..hi), or the alias's "!" words
 * lo hi    numeric range of a developer key (alias keys: 0, 0 = the alias range)
 * step     Left / Right step of a numeric value (0 = 1)
 * def      code default of a developer key (text; NULL = "")
 * unit     shown after a number (" %", " m", ...; NULL = none)
 * apply    live apply: called with the INTERNAL key and value after the cfg table has the new value; returns 1 = in
 *          effect now, 0 = not possible now (the menu marks it "after restart"); NULL = applies after a restart
 * flags    HS_DEV  a developer setting: a new line goes into the developer file (ps2rc_dev.cfg)
 *          HS_RO   shown, not editable (e.g. memory layout keys that only make sense in the file)
 *          HS_HIDE not shown
 *          HS_UNSET the first choice means "not set" (e.g. car_l2=foot: as on foot): Save comments the key's
 *                  lines out instead of writing that word
 */
#ifndef HLE_SETTINGS_REG_H
#define HLE_SETTINGS_REG_H

typedef int (*HleSetApply)(const char *ikey, const char *ival);

enum { HS_DEV = 1, HS_RO = 2, HS_HIDE = 4, HS_UNSET = 8 };

typedef struct {
    const char *key, *cat, *label, *help, *rec, *choices;
    int lo, hi, step;
    const char *def, *unit;
    HleSetApply apply;
    unsigned flags;
} HleSetting;

const HleSetting *hle_settings_table(int *n);   /* the game pack's registry; weak default in hle_menu.cpp: none */

#endif
