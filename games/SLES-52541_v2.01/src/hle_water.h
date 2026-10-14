/* hle_water.h - the water surfaces, drawn on the PSP GE.
 *
 * water_render 0x182850 (in the 3D scene, between the plants and the effects, cull none) queues one 96-byte entry per
 * water polygon in the game's water buffer (0x83D610: count +0, entries +0x10, type +0x910; at most 24 entries,
 * flushed on a type change, when full and at the end) and the hook water_batch 0x3C6950 receives each batch. On the
 * PS2 the batch becomes a VU1 packet (programs water_0..3) that the HLE drops, so without this module no water is
 * drawn. Our hook draws the entries through the 3D back-end (hle_w3d.h: view-space float triangles, near /
 * guard-band clipping, the GE's projection, fog, Z test without Z write, SRCALPHA / INVSRCALPHA, the water texture the
 * game set).
 *
 * cfg (PSP cfg file / host environment HLE_<KEY>):
 *   water=0|1        0 (default) = the guest original (nothing drawn); 1 = drawn
 *   water_guest=0|1  1 = the guest original runs as well (its VU1 packet is dropped; only for checks)
 *   water_hd=0|1     0 = the high-detail entries (types 1 / 3, near the camera) are not drawn
 *   water_waves=0|1  1 = the high-detail entries get approximate waves (the PS2's wave formula is not reproduced;
 *                    default 0: flat at the polygon's heights - the CPU cost of the grid is not worth it on the PSP)
 *   water_grid=N     high-detail rectangles with waves: at most N x N cells (default 8)
 *   water_log=N      log the first N batches in detail (render states + the first entries)
 *   water_tp=F:x:y:z[,F:x:y:z]  test aid: the player is moved there at flip F (to compare frames at water)
 * The in-run A/B (a developer module) switches `water` per window (FAM_SKY -> hle_sky_cfg -> hle_water_cfg).
 */
#ifndef HLE_WATER_H
#define HLE_WATER_H
extern int water_opt_on;
void hle_water_cfg(const char *key, const char *value);
void hle_water_init(void);
void hle_water_report(void);
#endif
