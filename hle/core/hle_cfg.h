/* hle_cfg.h - ONE shared reader of the run options file (HLE_PACK_CFG).
 *
 * The file is read once, whole (any length, comments allowed), into a key table:
 *   - lines starting with '#' or ';' (after spaces) are comments; empty lines are skipped; a UTF-8 BOM is skipped;
 *   - "key = value" (spaces around the key and the value are trimmed); " # text" after a value is a comment;
 *   - keys are case-insensitive (stored in lower case; every internal key is lower case);
 *   - a key given twice: the LATER line wins (one value per key in the table);
 *   - player-facing friendly names (draw_distance, peds, traffic, ...; the alias table in hle_cfg.c) are clamped to
 *     their range and stored as the internal keys they stand for (tune_far, tune_peds, ...). Friendly name and internal
 *     key are the same setting: whichever line comes LATER in the file wins.
 * PSP: <EBOOT dir><cfg>_dev<.ext> (developer / run options, optional) and then <EBOOT dir> + HLE_PACK_CFG (the
 * player's settings; a key in both: the cfg wins). Each falls back to the older name of the pack's HLE_PACK_CFG_LEGACY
 * (if it has one) when the current name is not there (a folder converted by an earlier version). Host: the folder
 * HLE_CFG_DIR (the same names and fallback), then the files named by HLE_CFG_DEV and HLE_CFG (none = empty table);
 * on the host every table key is also exported as the environment variable HLE_<KEY> (upper case) unless
 * that variable is already set, so the host's existing environment readers (HLE_TUNE_PEDS, HLE_X_AGCAM, ...) see the
 * cfg without code changes.
 */
#ifndef HLE_CFG_H
#define HLE_CFG_H

/* a friendly name (the game's table hle_cfg_aliases[], hle_settings.c):
 * name     friendly key (what the shipped cfg template uses)
 * key      the internal key it is stored as (the modules read only internal keys)
 * lo / hi  numeric range of the FRIENDLY value: outside = clamped (noted in the log); lo = hi = 0: a word key, stored
 *          as given (pad_sel_up, cpu_auto_reset); a word key with "!word=n,..." takes only those words (lower case)
 * def      the code default of the INTERNAL key (applies when the file sets neither name; shown with '*')
 * inv      1 = internal = lo + hi - friendly (shadows=1 is x_fxskip=0)
 * words    "word=number,..." friendly words (textures=half); "!" first = ONLY these values (overclock, game_fps);
 *          the log line shows the first word of an internal value */
typedef struct { const char *name, *key; int lo, hi, def, inv; const char *words; } HleCfgAlias;
const HleCfgAlias *hle_cfg_alias_table(int *n);   /* the game's table (hle_settings.c); weak default: none */

int hle_cfg_count(void);                    /* keys in the table (loads the file on the first call) */
const char *hle_cfg_key(int i);             /* i < hle_cfg_count() */
const char *hle_cfg_val(int i);
const char *hle_cfg_get(const char *key);   /* the value of an internal key, NULL = not in the file */
int hle_cfg_found(void);                    /* 1 = the cfg and/or the dev cfg was read, 0 = none (defaults) */
/* "settings: draw_distance=40 fog=250 ..." - every friendly setting with its effective value (from the file, or the
   code default marked '*'); notes = clamped / unknown values and overflow, "" if none */
const char *hle_cfg_settings_line(void);
const char *hle_cfg_notes(void);
void hle_cfg_report(void);                  /* LOG the two lines above + the sorted internal key table (session.log) */

#endif
