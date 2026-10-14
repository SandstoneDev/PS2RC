/* audio_hostio.h - host implementation of the sound driver's data interface (SnddIo) over an extracted disc folder
 * (tests, tools and the host boot harness). Resolves the IOP's "AUDIO\\SFX\\GENRL01.PAK" style paths case-
 * insensitively against the folder (the v2.01 extraction mixes "audio/CONFIG/BankLkup.dat" and "GENRL01.pak"), keeps
 * a few files open, and can simulate asynchronous completion (every distinct request first answers PENDING) so tests
 * exercise the driver's retry paths. Host-only: stdio + POSIX dirent; the PSP platform provides its own SnddIo. */
#ifndef AUDIO_HOSTIO_H
#define AUDIO_HOSTIO_H

#include <stdint.h>
#include <stdio.h>
#include "snd_driver.h"

#define AUDIO_HOSTIO_FILES 24
#define AUDIO_HOSTIO_PENDING_SLOTS 64

typedef struct AudioHostIo {
    char root[1024];
    struct { char key[64]; FILE *f; uint32_t size; } files[AUDIO_HOSTIO_FILES];
    int nfiles;
    int simulate_pending;               /* 1: each new (path, offset, size, dst) answers PENDING once */
    struct { uint32_t hash; uint8_t used; } pend[AUDIO_HOSTIO_PENDING_SLOTS];
    uint64_t bytes_read;
    uint32_t reads, pendings, errors;
} AudioHostIo;

int audio_hostio_init(AudioHostIo *h, const char *disc_root);    /* 0 ok */
void audio_hostio_bind(AudioHostIo *h, SnddIo *io);
void audio_hostio_close(AudioHostIo *h);
/* resolve an IOP path to a host path (case-insensitive); 0 ok */
int audio_hostio_resolve(const AudioHostIo *h, const char *iop_path, char *out, size_t cap);

#endif
