/* hle_audio_psp.h - the game's audio on the PSP.
 *
 * The sound driver's RPC server 0x65686580 (hle_iop_servers.c) runs the IOP sound driver (snd_driver.h) + the SPU2-lite
 * engine in lockstep with VIRTUAL time on the game thread, exactly as the host does (same code, same clock arithmetic
 * as audio_host.c, synchronous reads of the same disc files): every reply equals the host's. The engine's 48 kHz mix
 * is resampled to 44.1 kHz into a ring; a feeder thread (priority 0x12, above the game thread 0x20) hands it to a
 * sceAudio channel at real time through audio_stretch (WSOLA time-stretch while the game runs slower than real time).
 * Memory: one partition block (~2.7 MB: SPU RAM image 2 MB, the IOP's config tables, the ring, the stream read cache);
 * thread stack 16 KB.
 *
 * ps2rc.cfg: audio=1|0 (default 1; 0 = the silent responder), audio_vol=0..100 (100), audio_sync=stretch|gap|pitch,
 * audio_latency_ms=N (120), audio_stretch_max=N (4), audio_interp=linear|nearest, audio_voices=N (16: only the N loudest
 * sounding voices are mixed, the others keep their exact IOP-visible state - 0 = all 48), audio_out=1|0 (0: the driver
 * runs, nothing is played - CPU A/B), audio_cache_kb=N (128; stream read cache, 0 = off), audio_prio=N (0x12),
 * audio_stats=N (60: seconds between statistics lines, 0 = only at the end), audio_dump=N (emulator only: the first N
 * seconds of the 48 kHz lockstep mix -> <EBOOT dir>/audio_dump.wav, to compare with the host's HLE_AUDIO_WAV),
 * audio_log=0|1 (1 = the driver's progress lines - bank loads, track starts - in session.log, as the host's
 * HLE_AUDIO_LOG=1; the "Sound driver track:" / "Sound driver stream voices:" lines of hle_iop_servers.c are always on).
 * audio_me=0|1|2 (default 1 = the Media Engine renders, resamples and stretches once the main loop runs - the main core
 * keeps the driver + a state-only engine, same replies - and from then on the PSP cannot sleep until the game exits
 * (scePowerLock: a suspend would hang in the OS's ME driver) unless audio_me_sleep=1; 0 = everything on the main core,
 * sleep works; 2 = shadow: the main core plays, the ME renders the same blocks and the hashes are compared),
 * audio_me_stall_ms=N (500: no progress of the ME for N ms -> back to the main core), audio_me_hop_ms=N (40: the
 * feeder's deadline for one hop), audio_me_regs=0|1 (0: the reset stub writes 2 / 6, 1: ORs them in).
 * audio_me_sleep=0|1 (default 1, checked on a PSP): 1 = ps2rc_exc.prx parks the ME cleanly at suspend and restarts it
 * at resume (the OS's ME driver is bypassed for the session): no scePowerLock, the PSP may sleep with the ME taken.
 * Needs a ps2rc_exc.prx with the ME park hook; with an older module, a refused hook or 0 here: logged, and sleep is
 * held off by scePowerLock from the takeover on.
 * The emulator has no ME: audio_me is ignored there (logged) - unless audio_me_sim=1 (tests only): then a main-core
 * thread runs the ME's loop (same protocol, no cache ops), and audio_me_sim_hang=N makes it stop working after N s
 * (the hand-back test).
 * Fallbacks (logged): no AUDIO/CONFIG files on the card, or no memory -> the silent responder; no sceAudio channel or
 * feeder thread -> the driver runs silently (replies unchanged).
 */
#ifndef HLE_AUDIO_PSP_H
#define HLE_AUDIO_PSP_H

#include <stdint.h>
#include "snd_driver.h"

/* reads ps2rc.cfg, allocates, binds the driver; 1 = ready, 0 = use the silent responder (audio=0 or a failure) */
int hle_audio_psp_init(void (*log)(void *user, int level, const char *fmt, ...));
/* one SIF RPC to 0x65686580 (fno 0 init, fno 1 frame); now_us = virtual time in microseconds (as audio_host_rpc) */
int hle_audio_psp_rpc(uint32_t fno, const void *send, uint32_t ssize, void *recv, uint32_t rsize, uint64_t now_us);
SndDrv *hle_audio_psp_model(void);
/* final statistics; stops the feeder and releases the channel */
void hle_audio_psp_report(void);
int hle_audio_psp_stop(void);       /* feeder stopped + channel released (exit paths) */

#endif
