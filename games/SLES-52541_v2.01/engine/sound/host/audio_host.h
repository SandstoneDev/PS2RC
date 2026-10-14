/* audio_host.h - ready-made host setup of the IOP sound driver for the host boot harness (hle_iop_servers.c): the
 * driver (snd_driver.h) + the SPU2-lite engine in lockstep with the caller's (virtual) clock, disc files from the
 * extracted disc folder, and an optional WAV capture of everything the engine renders. Two calls replace the silent
 * responder:
 *
 *     static AudioHost *aud;
 *     aud = audio_host_create(hle_disc_dir, getenv("HLE_AUDIO_WAV"), log_fn, 0);        // once, at IOP init
 *     audio_host_rpc(aud, fno, gptr(send, ssize), ssize, gptr(recv, rsize), rsize,
 *                    hle_vtime * 1000000 / EE_HZ);                                     // in the 0x65686580 hook
 *
 * Host only (stdio, stdlib). Not thread-safe (the harness is single-threaded). */
#ifndef AUDIO_HOST_H
#define AUDIO_HOST_H

#include <stdint.h>
#include "snd_driver.h"

typedef struct AudioHost AudioHost;

/* disc_root: the extracted disc folder; wav_path: 0 or a file that receives the 48 kHz stereo mix;
 * log: 0 or a printf-like sink for the IOP's messages (level 0 errors, 1 progress, 2 chatty) */
AudioHost *audio_host_create(const char *disc_root, const char *wav_path,
                             void (*log)(void *user, int level, const char *fmt, ...), void *log_user);

/* one SIF RPC to 0x65686580 (fno 0 init, fno 1 frame); now_us = the caller's clock (the elapsed time since the
 * previous fno 1 is rendered first, sub-millisecond remainders carried). send and recv may alias. Returns 0. */
int audio_host_rpc(AudioHost *h, uint32_t fno, const void *send, uint32_t ssize, void *recv, uint32_t rsize,
                   uint64_t now_us);

SndDrv *audio_host_model(AudioHost *h);
void audio_host_destroy(AudioHost *h);   /* closes the WAV (writes its header) */

#endif
