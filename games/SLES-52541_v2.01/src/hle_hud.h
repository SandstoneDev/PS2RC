/* hle_hud.h - the HUD on the PSP screen (hle_hud.c): the game lays out its HUD for a 4:3 picture in 640x448 guest units;
 * the PSP shows that picture stretched to 16:9 (480x272), so the radar is a wide ellipse and every HUD element is a
 * third wider than on a TV. hle_hud.c routes the calls inside the HUD draw (sites.def) through handlers that set a
 * 2D transform for the element group being drawn; the 2D back-ends (hle_ge.c on the PSP, hle_render.c on the host)
 * apply it to every 2D vertex / glyph quad in guest units before their own screen scale:
 *   x' = ax + (x - ax) * kx,   y' = ay + (y - ay) * ky     (anchor = the screen corner / edge of the group)
 * Nothing in the game's state changes: the transform only moves what is drawn.
 * Keys (ps2rc.cfg; host: the same keys via HLE_CFG / HLE_HUD_* environment):
 *   hud_aspect = fix | stretch   fix (PSP default): 4:3 proportions anchored to the corners, round radar;
 *                                stretch (host default) = the old picture
 *   hud_scale  = 50..200         % size of the anchored HUD groups (money, clock, health, weapon, radar, help box)
 *   hud_outline = on | off       black outline around the anchored HUD text (money, clock, ammo, names)
 *   hud_radar  = round | ps2     round = the radar circle is round on the PSP; ps2 = same 4:3 factor as the rest */
#ifndef HLE_HUD_H
#define HLE_HUD_H

extern int hle_hud_on;                    /* a transform is active (inside a HUD group) */
extern int hle_hud_outline;               /* the active group's text gets a black outline */
extern float hle_hud_ax, hle_hud_kx, hle_hud_ay, hle_hud_ky;

static inline float hle_hud_x(float x) { return hle_hud_on ? hle_hud_ax + (x - hle_hud_ax) * hle_hud_kx : x; }
static inline float hle_hud_y(float y) { return hle_hud_on ? hle_hud_ay + (y - hle_hud_ay) * hle_hud_ky : y; }

void hle_hud_report(void);

#endif
