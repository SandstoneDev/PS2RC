/* hle_cdasync.c - disc stream reads on a reader thread (PSP only, poc6.cfg disc_async=1, from the first main-loop
 * iteration on).
 *
 * On the PS2 the game's disc streaming module reads asynchronously: Read(channel, buffer, lsn, n) only queues the
 * request, the EE goes on, the streaming loader (stream_channel) polls GetStatus (0 = done, 255 = reading) and converts
 * the models once the data is there; the synchronous loads (all requested models at once, big models) call Sync. The
 * HLE (hle_iop_servers.c) did the whole memory-stick read inside the Read call, on the game thread - every streamed
 * model stopped the game for the time of its sceIoRead (measured on a PSP: io.disc up to 23-29 ms in one iteration in
 * a static scene). Here Read queues the request for a reader thread (priority 0x1F, just above the game thread's 0x20:
 * it issues the next read at once and sleeps in sceIoRead while the game thread runs on), GetStatus answers 255 while
 * the request is open, Sync waits for it (time counted as hitch source "disc-wait"). The reader uses the disc layer
 * under its lock (hle_plat_disc_lock: the game thread's own disc / audio / memory-card file accesses wait for the
 * read in progress) and never logs. The data reaches the guest later than before, as on the console; the host keeps
 * the synchronous reads (its timeline is the gate's).
 */
#include <pspkernel.h>
#include <string.h>
#include "hle.h"
#include "hle_hitch.h"

int hle_disc_read_lsn_quiet(uint32_t lsn, uint32_t nsect, void *dst);
void hle_plat_disc_lock_create(void);

int hle_cdasync_opt = 0;                       /* poc6.cfg disc_async=1 */
#define NCH 8
static struct { volatile int busy; uint32_t lsn, n; void *dst; } job[NCH];
static volatile int ring[NCH * 2];
static volatile unsigned ring_w, ring_r;
static SceUID sem_work = -1, ev_done = -1, th = -1;
static int dead;                                /* the thread could not be created: synchronous reads */
static uint64_t st_reads, st_sectors, st_busy_polls, st_sync_waits, st_sync_us, st_reissue_waits, st_errors;
static uint32_t st_sync_max_us, st_read_max_us;
static uint64_t st_read_us;

static int reader(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelWaitSema(sem_work, 1, NULL) < 0) return 0;
        unsigned r = ring_r;
        int ch = ring[r % (NCH * 2)];
        ring_r = r + 1;
        SceUInt t0 = sceKernelGetSystemTimeLow();
        if (hle_disc_read_lsn_quiet(job[ch].lsn, job[ch].n, job[ch].dst) != 0) st_errors++;
        SceUInt dt = sceKernelGetSystemTimeLow() - t0;
        st_read_us += dt;
        if (dt > st_read_max_us) st_read_max_us = dt;
        job[ch].busy = 0;
        sceKernelSetEventFlag(ev_done, 1u << ch);
    }
    return 0;
}

static int ready(void)
{
    if (!hle_cdasync_opt || dead) return 0;
    if (!hle_mainloop_iters) return 0;            /* boot / loading screens: synchronous as before */
    if (th >= 0) return 1;
    hle_plat_disc_lock_create();
    sem_work = sceKernelCreateSema("hle_cd_work", 0, 0, NCH * 2, NULL);
    ev_done = sceKernelCreateEventFlag("hle_cd_done", 0, 0, NULL);
    th = sceKernelCreateThread("hle_discstream", reader, 0x1F, 0x4000, THREAD_ATTR_USER, NULL);
    if (sem_work < 0 || ev_done < 0 || th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        LOG("disc stream async: reader thread NOT started (sema %08x, event %08x, thread %08x) - synchronous reads\n",
            (unsigned)sem_work, (unsigned)ev_done, (unsigned)th);
        dead = 1;
        return 0;
    }
    LOG("disc stream async (disc_async=1): reader thread started at main-loop iteration %llu - Read queues, "
        "GetStatus 255 while reading, Sync waits\n", (unsigned long long)hle_mainloop_iters);
    return 1;
}

static void wait_ch(uint32_t ch, uint64_t *count)
{
    if (!job[ch].busy) return;
    (*count)++;
    SceUInt t0 = sceKernelGetSystemTimeLow();
    while (job[ch].busy) {
        SceUInt tmo = 200000;
        sceKernelWaitEventFlag(ev_done, 1u << ch, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, &tmo);
    }
    SceUInt dt = sceKernelGetSystemTimeLow() - t0;
    st_sync_us += dt;
    if (dt > st_sync_max_us) st_sync_max_us = dt;
    hle_hitch_us[HITCH_DWAIT] += dt;
}

/* 1 = queued (the caller must not touch the buffer until GetStatus says 0), 0 = do it synchronously */
int hle_cdasync_read(uint32_t ch, uint32_t lsn, uint32_t n, void *dst)
{
    if (ch >= NCH || !ready()) return 0;
    wait_ch(ch, &st_reissue_waits);                /* a channel holds one request */
    job[ch].lsn = lsn;
    job[ch].n = n;
    job[ch].dst = dst;
    job[ch].busy = 1;
    unsigned w = ring_w;
    ring[w % (NCH * 2)] = (int)ch;
    ring_w = w + 1;
    st_reads++;
    st_sectors += n;
    sceKernelSignalSema(sem_work, 1);
    return 1;
}

int hle_cdasync_status(uint32_t ch)
{
    if (ch >= NCH || th < 0) return 0;
    if (job[ch].busy) { st_busy_polls++; return 255; }
    return 0;
}

void hle_cdasync_sync(uint32_t ch)
{
    if (ch < NCH && th >= 0) wait_ch(ch, &st_sync_waits);
}

void hle_cdasync_report(void)
{
    if (!hle_cdasync_opt) return;
    LOG("disc stream async: %llu reads (%llu sectors) on the reader thread, %llu.%llu ms per read (max %u us), "
        "%llu errors (zero-filled); GetStatus answered 'reading' %llu times; the game thread waited %llu times in "
        "Sync + %llu times before re-using a channel, %llu ms in all (max %u us)%s\n", (unsigned long long)st_reads,
        (unsigned long long)st_sectors, (unsigned long long)(st_reads ? st_read_us / st_reads / 1000u : 0),
        (unsigned long long)(st_reads ? st_read_us / st_reads / 100u % 10u : 0), st_read_max_us,
        (unsigned long long)st_errors, (unsigned long long)st_busy_polls, (unsigned long long)st_sync_waits,
        (unsigned long long)st_reissue_waits, (unsigned long long)(st_sync_us / 1000u), st_sync_max_us,
        dead ? " - THE READER THREAD DID NOT START" : th < 0 ? " - never started (no main loop)" : "");
}
