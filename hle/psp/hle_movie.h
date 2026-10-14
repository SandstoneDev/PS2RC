/* hle_movie.h - a game's movies on the PSP.
 *
 * A pack hooks the game's movie function (path, skippable, unskippable_bytes) and calls hle_movie_play. The PS2
 * decodes PSS movies with the IPU + SPU2; the PSP plays install-time conversions (AVC 480x272 29.97 fps + ATRAC3plus
 * 44.1 kHz in PSMF, or video-only PSMF + a .wav: ps2rc/movies) from <EBOOT dir>/movies/<NAME>.pmf with the
 * firmware's sceMpeg (Media Engine) - own player, the pictures through the pack's GE display buffers
 * (hle_ge_movie_*), the sound on a sceAudio channel.
 * PSP run options: movies=1|0 (default 1), movie_psm=8888|5650 (decoder output; default 8888, drawn by the GE into the 5650
 * display buffer), movie_mem=partition|hole (where the picture / sound buffers live; default partition), movie_skip=1|0
 * (X skips; 0 for unattended runs - PPSSPP passes the PC's keys / pads through), and for
 * PPSSPP checks only movie_dump=T,... (BMP of the displayed buffer) and movie_test_skip=T,... (a simulated X press);
 * T = movie number (in playing order, from 1) x 10000 + picture number. PSP only; the host skips the movies
 * (the pack's hook). */
#ifndef HLE_MOVIE_H
#define HLE_MOVIE_H
#include <stdint.h>

/* plays the movie for the guest path (a1 != 0: X skips at once; a1 == 0: X skips once `unskip_bytes` of the original
   PSS's bytes are behind - the PS2 rule, scaled to the PMF stream); blocks until its end. Returns 1 when pictures were
   shown, 0 when it was not played (movies=0, render=text, no PMF, the decoder refused): the caller skips as before.
   Virtual time stands still meanwhile (the host skips the movies: the same virtual timeline on both); the real time
   it took moves the pacing anchor (hle_plat_pace_shift). */
int hle_movie_play(const char *guest_path, uint32_t skippable, uint32_t unskip_bytes);

/* psp/hle_psp.c: the idle pacing ignores `us` more microseconds of real time */
void hle_plat_pace_shift(uint64_t us);

#endif
