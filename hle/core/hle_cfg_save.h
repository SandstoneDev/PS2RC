/* hle_cfg_save.h - writes changed settings back into the cfg files the player keeps (ps2rc.cfg and
 * ps2rc_dev.cfg) without losing their comments or layout. Pure C, no I/O (the caller reads / writes the files):
 * tests/cfg/test_cm_menu.py runs it on the host.
 *
 * One edit = one setting: the name the menu writes (the friendly name, or an internal key for a developer setting),
 * the internal key it stands for (its lines are the same setting: the reader lets the later line win), the new value.
 * Rules (hle_cfg_rewrite, one file at a time):
 *   - an edit whose setting has an active line in this file: the LAST such line (the one the reader uses) becomes
 *     "name=value"; its indentation, the spaces + " # comment" after the value and its line end stay;
 *   - insert = 1 (player file) / 2 (developer file): the edits still not done whose dev flag matches are added - after
 *     the last commented-out line of that name ("#map_l2=select+l", the template's suggestion), else at the end
 *     under one "# --- changed in the game menu" line; a new line ends like the file's lines (CRLF or LF);
 *   - value NULL = unset (back to the built-in value): every active line of the setting in this file gets a "#" in
 *     front (the line stays as a comment, nothing is inserted); done once a line was commented, or in the insert step
 *     of its file when it has no line at all;
 *   - comments, empty lines, unknown keys, the UTF-8 BOM and every other byte stay as they are.
 * Order the menu uses: player file (existing lines only), developer file (existing lines + new developer lines), player
 * file again (new player lines) - so a setting is changed where the reader takes it from.
 */
#ifndef HLE_CFG_SAVE_H
#define HLE_CFG_SAVE_H

typedef struct {
    const char *name;     /* written key: friendly name or internal key */
    const char *ikey;     /* the internal key its lines may also use (NULL = only name) */
    const char *value;    /* new value text; NULL = unset (its lines are commented out) */
    int dev;              /* 1 = a developer setting: a new line goes into the developer file */
    int done;             /* out: 1 = written into some file */
} HleCfgEdit;

#define HLE_CFG_MENU_HEADER "# --- changed in the game menu"

/* returns the new length, -1 = out (cap bytes) too small; e->done set for the edits written */
int hle_cfg_rewrite(const char *in, int n, HleCfgEdit *e, int ne, int insert, char *out, int cap);

#endif
