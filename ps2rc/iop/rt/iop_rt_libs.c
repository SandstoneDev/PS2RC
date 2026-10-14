/* iop_rt_libs.c - IOP libraries for translated modules: sysclib, stdio, ioman (disc files), cdvdman, libsd (sound
 * registers -> IopSdOps), vblank; and the table that maps (library, export number) to the HLE functions. The export
 * numbers are those of the documented IOP module interfaces. See iop_rt.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iop_rt_int.h"

#define A(k) iop_arg(c, k)
#define RET(v) (c->r[2] = (uint32_t)(v))

/* kernel functions (iop_rt.c) */
void hle_CreateThread(IopCpu *c); void hle_DeleteThread(IopCpu *c); void hle_StartThread(IopCpu *c);
void hle_ExitThread(IopCpu *c); void hle_GetThreadId(IopCpu *c); void hle_ChangeThreadPriority(IopCpu *c);
void hle_RotateThreadReadyQueue(IopCpu *c); void hle_SleepThread(IopCpu *c); void hle_WakeupThread(IopCpu *c);
void hle_iWakeupThread(IopCpu *c); void hle_CancelWakeupThread(IopCpu *c); void hle_DelayThread(IopCpu *c);
void hle_GetSystemTime(IopCpu *c); void hle_USec2SysClock(IopCpu *c); void hle_SysClock2USec(IopCpu *c);
void hle_SetAlarm(IopCpu *c); void hle_iSetAlarm(IopCpu *c); void hle_CancelAlarm(IopCpu *c);
void hle_CreateSema(IopCpu *c); void hle_DeleteSema(IopCpu *c); void hle_SignalSema(IopCpu *c);
void hle_iSignalSema(IopCpu *c); void hle_WaitSema(IopCpu *c); void hle_PollSema(IopCpu *c);
void hle_ReferSemaStatus(IopCpu *c);
void hle_CreateEventFlag(IopCpu *c); void hle_DeleteEventFlag(IopCpu *c); void hle_SetEventFlag(IopCpu *c);
void hle_iSetEventFlag(IopCpu *c); void hle_ClearEventFlag(IopCpu *c); void hle_iClearEventFlag(IopCpu *c);
void hle_WaitEventFlag(IopCpu *c); void hle_PollEventFlag(IopCpu *c);
void hle_CpuSuspendIntr(IopCpu *c); void hle_CpuResumeIntr(IopCpu *c); void hle_CpuEnableIntr(IopCpu *c);
void hle_CpuDisableIntr(IopCpu *c); void hle_QueryIntrContext(IopCpu *c);
void hle_AllocSysMemory(IopCpu *c); void hle_FreeSysMemory(IopCpu *c); void hle_QueryMemSize(IopCpu *c);
void hle_QueryMaxFreeMemSize(IopCpu *c); void hle_QueryTotalFreeMemSize(IopCpu *c); void hle_Kprintf(IopCpu *c);
void hle_AllocHardTimer(IopCpu *c); void hle_FreeHardTimer(IopCpu *c); void hle_SetTimerHandler(IopCpu *c);
void hle_SetupHardTimer(IopCpu *c); void hle_StartHardTimer(IopCpu *c); void hle_StopHardTimer(IopCpu *c);
void hle_sceSifSetRpcQueue(IopCpu *c); void hle_sceSifRegisterRpc(IopCpu *c); void hle_sceSifRpcLoop(IopCpu *c);
void hle_sceSifGetNextRequest(IopCpu *c); void hle_sceSifExecRequest(IopCpu *c);
void hle_RegisterLibraryEntries(IopCpu *c); void hle_ok(IopCpu *c); void hle_one(IopCpu *c);

/* ------------------------------------------------------------------ guest strings and printf */

static const char *gstr(IopCpu *c, uint32_t a)
{
    return a ? (const char *)(c->mem + (a & IOP_MEM_MASK)) : "(null)";
}

/* printf over the guest's o32 varargs: %[-+ 0#][width][.prec][hh|h|l|ll]{d i u x X o c s p %} */
int iop_rt_format(IopCpu *c, uint32_t fmt, int argk, char *out, size_t cap)
{
    const char *f = gstr(c, fmt);
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n] = (char)(ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = '%';
        f++;
        while (*f && strchr("-+ 0#", *f) && k < 20) spec[k++] = *f++;
        if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)A(argk++)); f++; }
        while (*f >= '0' && *f <= '9' && k < 24) spec[k++] = *f++;
        if (*f == '.') {
            spec[k++] = *f++;
            if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)A(argk++)); f++; }
            while (*f >= '0' && *f <= '9' && k < 28) spec[k++] = *f++;
        }
        while (*f == 'h' || *f == 'l' || *f == 'z' || *f == 'j' || *f == 't') f++;   /* 32-bit IOP: ignore */
        char t = *f;
        if (!t) break;
        char tmp[256];
        int m = 0;
        if (t == '%') { PUT('%'); continue; }
        if (t == 'd' || t == 'i') {
            spec[k++] = 'd'; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, (int)(int32_t)A(argk++));
        } else if (t == 'u' || t == 'x' || t == 'X' || t == 'o') {
            spec[k++] = t; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, (unsigned)A(argk++));
        } else if (t == 'p') {
            spec[k++] = 'x'; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, (unsigned)A(argk++));
        } else if (t == 'c') {
            spec[k++] = 'c'; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, (int)(A(argk++) & 0xFF));
        } else if (t == 's') {
            spec[k++] = 's'; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, gstr(c, A(argk++)));
        } else {
            PUT('%'); PUT(t);
            continue;
        }
        if (m < 0) m = 0;
        if (m > (int)sizeof tmp - 1) m = (int)sizeof tmp - 1;
        for (int i = 0; i < m; i++) PUT(tmp[i]);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return (int)n;
}

/* ------------------------------------------------------------------ sysclib */

static void l_memcpy(IopCpu *c) { if (A(2)) memmove(c->mem + (A(0) & IOP_MEM_MASK), c->mem + (A(1) & IOP_MEM_MASK), A(2)); RET(A(0)); }
static void l_memmove(IopCpu *c) { l_memcpy(c); }
static void l_memset(IopCpu *c) { if (A(2)) memset(c->mem + (A(0) & IOP_MEM_MASK), (int)(A(1) & 0xFF), A(2)); RET(A(0)); }
static void l_bzero(IopCpu *c) { if (A(1)) memset(c->mem + (A(0) & IOP_MEM_MASK), 0, A(1)); RET(0); }
static void l_bcopy(IopCpu *c) { if (A(2)) memmove(c->mem + (A(1) & IOP_MEM_MASK), c->mem + (A(0) & IOP_MEM_MASK), A(2)); RET(0); }
static void l_memcmp(IopCpu *c) { RET(A(2) ? memcmp(c->mem + (A(0) & IOP_MEM_MASK), c->mem + (A(1) & IOP_MEM_MASK), A(2)) : 0); }
static void l_memchr(IopCpu *c)
{
    const uint8_t *p = c->mem + (A(0) & IOP_MEM_MASK);
    for (uint32_t i = 0; i < A(2); i++) if (p[i] == (uint8_t)A(1)) { RET(A(0) + i); return; }
    RET(0);
}
static void l_strlen(IopCpu *c) { RET(strlen(gstr(c, A(0)))); }
static void l_strcpy(IopCpu *c) { char *d = (char *)c->mem + (A(0) & IOP_MEM_MASK); memmove(d, gstr(c, A(1)), strlen(gstr(c, A(1))) + 1); RET(A(0)); }
static void l_strncpy(IopCpu *c)
{
    char *d = (char *)c->mem + (A(0) & IOP_MEM_MASK);
    const char *s = gstr(c, A(1));
    uint32_t n = A(2), i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    RET(A(0));
}
static void l_strcat(IopCpu *c) { char *d = (char *)c->mem + (A(0) & IOP_MEM_MASK); strcat(d, gstr(c, A(1))); RET(A(0)); }
static void l_strcmp(IopCpu *c) { RET(strcmp(gstr(c, A(0)), gstr(c, A(1)))); }
static void l_strncmp(IopCpu *c) { RET(strncmp(gstr(c, A(0)), gstr(c, A(1)), A(2))); }
static void l_strchr(IopCpu *c) { const char *s = gstr(c, A(0)), *p = strchr(s, (int)(A(1) & 0xFF)); RET(p ? A(0) + (uint32_t)(p - s) : 0); }
static void l_strrchr(IopCpu *c) { const char *s = gstr(c, A(0)), *p = strrchr(s, (int)(A(1) & 0xFF)); RET(p ? A(0) + (uint32_t)(p - s) : 0); }
static void l_toupper(IopCpu *c) { uint32_t ch = A(0) & 0xFF; RET(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); }
static void l_tolower(IopCpu *c) { uint32_t ch = A(0) & 0xFF; RET(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
static void l_strtol(IopCpu *c)
{
    const char *s = gstr(c, A(0));
    char *e = 0;
    long v = strtol(s, &e, (int)A(2));
    if (A(1)) { uint32_t a = A(1); uint32_t ep = A(0) + (uint32_t)(e - s); memcpy(c->mem + (a & IOP_MEM_MASK), &ep, 4); }
    RET((uint32_t)v);
}
static void l_sprintf(IopCpu *c)
{
    char b[1024];
    int n = iop_rt_format(c, A(1), 2, b, sizeof b);
    size_t m = strlen(b);
    memcpy(c->mem + (A(0) & IOP_MEM_MASK), b, m + 1);
    RET(n);
}
static void l_printf(IopCpu *c)
{
    char b[512];
    int n = iop_rt_format(c, A(0), 1, b, sizeof b);
    iop_rt_log(c->rt, 1, "%s", b);
    RET(n);
}

/* ------------------------------------------------------------------ disc paths */

/* "cdrom0:\\DIR\\FILE.EXT;1" / "host0:dir/file" / "\\DIR\\FILE;1" -> "\\DIR\\FILE;1" form for the disc back-end */
static void disc_path(const char *in, char *out, size_t cap)
{
    const char *p = strchr(in, ':');
    if (p && p - in < 10) in = p + 1;
    size_t n = 0;
    if (*in != '\\' && *in != '/' && n + 1 < cap) out[n++] = '\\';
    for (; *in && n + 1 < cap; in++) out[n++] = *in == '/' ? '\\' : *in;
    out[n] = 0;
}

static int disc_lookup(IopRt *rt, const char *path, uint32_t *lsn, uint32_t *size)
{
    if (!rt->cfg.disc.lookup) return -1;
    char p[256];
    disc_path(path, p, sizeof p);
    return rt->cfg.disc.lookup(rt->cfg.disc.user, p, lsn, size);
}

int iop_rt_disc_read(IopRt *rt, uint32_t lsn, uint32_t nsect, void *dst)
{
    if (!rt->cfg.disc.read) return -1;
    return rt->cfg.disc.read(rt->cfg.disc.user, lsn, nsect, dst);
}

static int disc_read_bytes(IopRt *rt, uint32_t lsn, uint32_t off, uint8_t *dst, uint32_t n)
{
    if (!rt->cfg.disc.read) return -1;
    uint8_t sec[2048];
    while (n) {
        uint32_t s = lsn + off / 2048u, o = off % 2048u, k = 2048u - o;
        if (k > n) k = n;
        if (o == 0 && k == 2048u) {
            uint32_t whole = n / 2048u;
            if (rt->cfg.disc.read(rt->cfg.disc.user, s, whole, dst) != 0) return -1;
            k = whole * 2048u;
        } else {
            if (rt->cfg.disc.read(rt->cfg.disc.user, s, 1, sec) != 0) return -1;
            memcpy(dst, sec + o, k);
        }
        dst += k;
        off += k;
        n -= k;
    }
    return 0;
}

/* ------------------------------------------------------------------ ioman (read-only files of the disc) */

static void io_open(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t lsn, size;
    if (disc_lookup(rt, gstr(c, A(0)), &lsn, &size) != 0) {
        iop_rt_log(rt, 2, "IOP: open(%s) - not found\n", gstr(c, A(0)));
        RET(-2);
        return;
    }
    for (int i = 0; i < IOP_MAX_FDS; i++) {
        IopFd *f = &rt->fd[i];
        if (f->used) continue;
        f->used = 1; f->lsn = lsn; f->size = size; f->pos = 0;
        RET(i + 3);
        return;
    }
    RET(-24);
}
static IopFd *fdp(IopCpu *c, uint32_t fd)
{
    int i = (int)fd - 3;
    if (i < 0 || i >= IOP_MAX_FDS || !c->rt->fd[i].used) return 0;
    return &c->rt->fd[i];
}
static void io_close(IopCpu *c) { IopFd *f = fdp(c, A(0)); if (f) f->used = 0; RET(f ? 0 : -9); }
static void io_read(IopCpu *c)
{
    IopFd *f = fdp(c, A(0));
    if (!f) { RET(-9); return; }
    uint32_t n = A(2);
    if (f->pos >= f->size) n = 0;
    else if (n > f->size - f->pos) n = f->size - f->pos;
    if (n && disc_read_bytes(c->rt, f->lsn, f->pos, c->mem + (A(1) & IOP_MEM_MASK), n) != 0) { RET(-5); return; }
    f->pos += n;
    RET(n);
}
static void io_lseek(IopCpu *c)
{
    IopFd *f = fdp(c, A(0));
    if (!f) { RET(-9); return; }
    int32_t off = (int32_t)A(1);
    int64_t p = A(2) == 0 ? off : A(2) == 1 ? (int64_t)f->pos + off : (int64_t)f->size + off;
    if (p < 0) { RET(-22); return; }
    f->pos = (uint32_t)p;
    RET(f->pos);
}
static void io_write(IopCpu *c) { RET(-30); }   /* read-only medium */

/* ------------------------------------------------------------------ cdvdman */

/* sceCdlFILE: u32 lsn, u32 size, char name[16], u8 date[8] */
static void cd_search_file(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t lsn, size, fp = A(0);
    const char *name = gstr(c, A(1));
    if (disc_lookup(rt, name, &lsn, &size) != 0) {
        iop_rt_log(rt, 2, "IOP: sceCdSearchFile(%s) - not found\n", name);
        RET(0);
        return;
    }
    wr32(rt, fp, lsn);
    wr32(rt, fp + 4, size);
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    char *dn = (char *)rt->mem + ((fp + 8) & IOP_MEM_MASK);
    memset(dn, 0, 16);
    strncpy(dn, b, 15);
    memset(rt->mem + ((fp + 24) & IOP_MEM_MASK), 0, 8);
    RET(1);
}
static void cd_layer_search_file(IopCpu *c) { cd_search_file(c); }
static void cd_read_dual_info(IopCpu *c)
{
    if (A(0)) wr32(c->rt, A(0), 0);                       /* single layer as far as the module can tell */
    if (A(1)) wr32(c->rt, A(1), 0);
    RET(1);
}
static void cd_read(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t lsn = A(0), n = A(1);
    if (!rt->cfg.disc.read || rt->cfg.disc.read(rt->cfg.disc.user, lsn, n, c->mem + (A(2) & IOP_MEM_MASK)) != 0) {
        RET(0);
        return;
    }
    RET(1);
}
static void cd_zero(IopCpu *c) { RET(0); }
static void cd_disk_type(IopCpu *c) { RET(0x14); }      /* PS2 DVD */
static void cd_disk_ready(IopCpu *c) { RET(2); }        /* ready */

/* ------------------------------------------------------------------ libsd */

#define SD_SPU(rt) (&(rt)->cfg.sd)

static void sd_set_irq(IopRt *rt)
{
    if (SD_SPU(rt)->set_irq) SD_SPU(rt)->set_irq(SD_SPU(rt)->user, rt->sd.irqa[0] & 0x1FFFFFu, rt->sd.irq_enable);
}

static void sd_init(IopCpu *c)
{
    IopRt *rt = c->rt;
    /* the sound processor itself is the back-end's (its reset state); only the library's view is reset here */
    memset(rt->sd.vparam, 0, sizeof rt->sd.vparam);
    RET(0);
}

static void sd_set_param(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, v = (e >> 1) & 31u, id = (e >> 8) & 0xFF;
    uint16_t val = (uint16_t)A(1);
    if (e & 0x80) { rt->sd.cparam[core][id & 31u] = val; RET(0); return; }   /* core parameter (volumes, mix) */
    if (v < 24) rt->sd.vparam[core][v][id & 7u] = val;
    if (id <= 4 && v < 24 && SD_SPU(rt)->set_param) SD_SPU(rt)->set_param(SD_SPU(rt)->user, core * 24u + v, id, val);
    RET(0);
}

static void sd_get_param(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, v = (e >> 1) & 31u, id = (e >> 8) & 0xFF;
    if (e & 0x80) { RET(rt->sd.cparam[core][id & 31u]); return; }
    if (v >= 24) { RET(0); return; }
    if (id == 5 && SD_SPU(rt)->get_envx) { RET(SD_SPU(rt)->get_envx(SD_SPU(rt)->user, core * 24u + v)); return; }
    RET(rt->sd.vparam[core][v][id & 7u]);
}

static void sd_set_switch(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, id = (e >> 8) & 0xFF, val = A(1) & 0xFFFFFFu;
    rt->sd.sw[core][id & 31u] = val;
    if (id == 0x15 && SD_SPU(rt)->key_on) SD_SPU(rt)->key_on(SD_SPU(rt)->user, core, val);
    else if (id == 0x16 && SD_SPU(rt)->key_off) SD_SPU(rt)->key_off(SD_SPU(rt)->user, core, val);
    RET(0);
}

static void sd_get_switch(IopCpu *c)
{
    uint32_t e = A(0) & 0xFFFF;
    RET(c->rt->sd.sw[e & 1u][(e >> 8) & 31u]);
}

static void sd_set_addr(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, v = (e >> 1) & 31u, id = (e >> 8) & 0xFF, val = A(1);
    rt->sd.addr[core][id & 63u] = val;
    if ((id == 0x20 || id == 0x21) && v < 24) {
        if (SD_SPU(rt)->set_addr)
            SD_SPU(rt)->set_addr(SD_SPU(rt)->user, core * 24u + v, id - 0x20, val & (SD_SPU(rt)->ram_size - 1u));
    } else if (id == 0x1F) {
        rt->sd.irqa[core] = val;
        rt->sd.irqa[0] = val;                                  /* the back-end has one IRQ address for both cores */
        sd_set_irq(rt);
    }
    RET(0);
}

static void sd_get_addr(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, v = (e >> 1) & 31u, id = (e >> 8) & 0xFF;
    if (id == 0x22 && v < 24 && SD_SPU(rt)->get_nax) { RET(SD_SPU(rt)->get_nax(SD_SPU(rt)->user, core * 24u + v)); return; }
    RET(rt->sd.addr[core][id & 63u]);
}

static void sd_set_core_attr(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t e = A(0) & 0xFFFF, core = e & 1u, id = e & ~1u;
    rt->sd.cattr[core][(id >> 1) & 15u] = A(1);
    if (id == 4 && core == 0) {                                /* SD_CORE_IRQ_ENABLE */
        rt->sd.irq_enable = A(1) ? 1 : 0;
        sd_set_irq(rt);
    }
    RET(0);
}

static void sd_get_core_attr(IopCpu *c)
{
    uint32_t e = A(0) & 0xFFFF;
    RET(c->rt->sd.cattr[e & 1u][((e & ~1u) >> 1) & 15u]);
}

static void sd_dma_to_spu(IopRt *rt, uint32_t iop, uint32_t spu, uint32_t size)
{
    IopSdOps *s = SD_SPU(rt);
    if (!s->ram || spu >= s->ram_size) return;
    if (rt->cfg.sd_dma_round64) size = (size + 63u) & ~63u;
    if (size > s->ram_size - spu) size = s->ram_size - spu;
    if (!size) return;
    if (s->dma_begin) s->dma_begin(s->user, spu, size);
    memcpy(s->ram + spu, rt->mem + (iop & IOP_MEM_MASK), size);
    if (s->dma_done) s->dma_done(s->user, spu, size);
    rt->st.sd_dma_bytes += size;
    rt->st.sd_dmas++;
}

static void sd_trans_done(IopCpu *c, uint32_t ch)
{
    IopRt *rt = c->rt;
    ch &= 1u;
    if (rt->sd.trans_func[ch]) {
        iop_rt_call_handler(rt, rt->sd.trans_func[ch], rt->sd.trans_gp[ch], ch, rt->sd.trans_data[ch], 0);
        iop_rt_preempt_check(rt);
    }
}

/* sceSdVoiceTrans(channel, mode, iop_addr, spu_addr, size): the transfer completes at once */
static void sd_voice_trans(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t ch = A(0), mode = A(1), iop = A(2), spu = A(3), size = A(4);
    if ((mode & 3u) == 0) sd_dma_to_spu(rt, iop, spu, size);
    else if (SD_SPU(rt)->ram && spu < SD_SPU(rt)->ram_size) {  /* read back */
        uint32_t n = size > SD_SPU(rt)->ram_size - spu ? SD_SPU(rt)->ram_size - spu : size;
        memcpy(rt->mem + (iop & IOP_MEM_MASK), SD_SPU(rt)->ram + spu, n);
    }
    RET(size);
    sd_trans_done(c, ch);
}

/* sceSdBlockTrans(channel, mode, iop_addr, size, ...): a one-shot write is a plain transfer to the core's block */
static void sd_block_trans(IopCpu *c)
{
    RET(0);
    sd_trans_done(c, A(0));
}

static void sd_trans_status(IopCpu *c) { RET(1); }          /* finished */

static void sd_set_trans_intr_handler(IopCpu *c)
{
    IopRt *rt = c->rt;
    uint32_t ch = A(0) & 1u;
    RET(rt->sd.trans_func[ch]);
    rt->sd.trans_func[ch] = A(1);
    rt->sd.trans_data[ch] = A(2);
    rt->sd.trans_gp[ch] = c->r[28];
}

static void sd_set_spu2_intr_handler(IopCpu *c)
{
    IopRt *rt = c->rt;
    RET(rt->sd.spu2_func);
    rt->sd.spu2_func = A(0);
    rt->sd.spu2_data = A(1);
    rt->sd.spu2_gp = c->r[28];
}

static void sd_set_irq_callback(IopCpu *c)
{
    IopRt *rt = c->rt;
    RET(rt->sd.irq_cb);
    rt->sd.irq_cb = A(0);
    rt->sd.irq_cb_gp = c->r[28];
}

static void sd_note2pitch(IopCpu *c) { RET(0x1000); }

void iop_rt_spu_irq(IopRt *rt, unsigned core_bits)
{
    rt->sd.irq_enable = 0;                                    /* the IRQ is acknowledged by disabling it */
    if (rt->sd.spu2_func) iop_rt_call_handler(rt, rt->sd.spu2_func, rt->sd.spu2_gp, core_bits, rt->sd.spu2_data, 0);
    else if (rt->sd.irq_cb) iop_rt_call_handler(rt, rt->sd.irq_cb, rt->sd.irq_cb_gp, 0, 0, 0);
}

/* ------------------------------------------------------------------ vblank */

static void vb_wait(IopCpu *c)
{
    IopRt *rt = c->rt;
    if (rt->cur < 0 || rt->intr_level) { RET(0); return; }
    iop_rt_wait(rt, W_VBLANK);
    RET(0);
}

void iop_rt_vblank(IopRt *rt)
{
    for (int t = 0; t < IOP_MAX_THREADS; t++) {
        IopThread *T = &rt->th[t];
        if (T->state == TS_WAIT && (T->wait == W_VBLANK || T->wait == W_VBLANK_END)) iop_rt_ready(rt, t, 0);
    }
}

/* ------------------------------------------------------------------ the table */

typedef struct { const char *lib; unsigned num; IopFn fn; const char *name; } Ent;

static const Ent table[] = {
    {"sysmem", 4, hle_AllocSysMemory, "AllocSysMemory"}, {"sysmem", 5, hle_FreeSysMemory, "FreeSysMemory"},
    {"sysmem", 6, hle_QueryMemSize, "QueryMemSize"}, {"sysmem", 7, hle_QueryMaxFreeMemSize, "QueryMaxFreeMemSize"},
    {"sysmem", 8, hle_QueryTotalFreeMemSize, "QueryTotalFreeMemSize"}, {"sysmem", 14, hle_Kprintf, "Kprintf"},
    {"loadcore", 6, hle_RegisterLibraryEntries, "RegisterLibraryEntries"},
    {"intrman", 17, hle_CpuSuspendIntr, "CpuSuspendIntr"}, {"intrman", 18, hle_CpuResumeIntr, "CpuResumeIntr"},
    {"intrman", 9, hle_CpuDisableIntr, "CpuDisableIntr"}, {"intrman", 10, hle_CpuEnableIntr, "CpuEnableIntr"},
    {"intrman", 23, hle_QueryIntrContext, "QueryIntrContext"},
    {"thbase", 4, hle_CreateThread, "CreateThread"}, {"thbase", 5, hle_DeleteThread, "DeleteThread"},
    {"thbase", 6, hle_StartThread, "StartThread"}, {"thbase", 8, hle_ExitThread, "ExitThread"},
    {"thbase", 14, hle_ChangeThreadPriority, "ChangeThreadPriority"},
    {"thbase", 16, hle_RotateThreadReadyQueue, "RotateThreadReadyQueue"},
    {"thbase", 20, hle_GetThreadId, "GetThreadId"}, {"thbase", 24, hle_SleepThread, "SleepThread"},
    {"thbase", 25, hle_WakeupThread, "WakeupThread"}, {"thbase", 26, hle_iWakeupThread, "iWakeupThread"},
    {"thbase", 27, hle_CancelWakeupThread, "CancelWakeupThread"}, {"thbase", 33, hle_DelayThread, "DelayThread"},
    {"thbase", 34, hle_GetSystemTime, "GetSystemTime"}, {"thbase", 35, hle_SetAlarm, "SetAlarm"},
    {"thbase", 36, hle_iSetAlarm, "iSetAlarm"}, {"thbase", 37, hle_CancelAlarm, "CancelAlarm"},
    {"thbase", 38, hle_CancelAlarm, "iCancelAlarm"}, {"thbase", 39, hle_USec2SysClock, "USec2SysClock"},
    {"thbase", 40, hle_SysClock2USec, "SysClock2USec"},
    {"thsemap", 4, hle_CreateSema, "CreateSema"}, {"thsemap", 5, hle_DeleteSema, "DeleteSema"},
    {"thsemap", 6, hle_SignalSema, "SignalSema"}, {"thsemap", 7, hle_iSignalSema, "iSignalSema"},
    {"thsemap", 8, hle_WaitSema, "WaitSema"}, {"thsemap", 9, hle_PollSema, "PollSema"},
    {"thsemap", 11, hle_ReferSemaStatus, "ReferSemaStatus"},
    {"thevent", 4, hle_CreateEventFlag, "CreateEventFlag"}, {"thevent", 5, hle_DeleteEventFlag, "DeleteEventFlag"},
    {"thevent", 6, hle_SetEventFlag, "SetEventFlag"}, {"thevent", 7, hle_iSetEventFlag, "iSetEventFlag"},
    {"thevent", 8, hle_ClearEventFlag, "ClearEventFlag"}, {"thevent", 9, hle_iClearEventFlag, "iClearEventFlag"},
    {"thevent", 10, hle_WaitEventFlag, "WaitEventFlag"}, {"thevent", 11, hle_PollEventFlag, "PollEventFlag"},
    {"timrman", 4, hle_AllocHardTimer, "AllocHardTimer"}, {"timrman", 6, hle_FreeHardTimer, "FreeHardTimer"},
    {"timrman", 20, hle_SetTimerHandler, "SetTimerHandler"}, {"timrman", 22, hle_SetupHardTimer, "SetupHardTimer"},
    {"timrman", 23, hle_StartHardTimer, "StartHardTimer"}, {"timrman", 24, hle_StopHardTimer, "StopHardTimer"},
    {"sifman", 5, hle_ok, "sceSifInit"}, {"sifman", 29, hle_one, "sceSifCheckInit"},
    {"sifcmd", 14, hle_ok, "sceSifInitRpc"}, {"sifcmd", 17, hle_sceSifRegisterRpc, "sceSifRegisterRpc"},
    {"sifcmd", 19, hle_sceSifSetRpcQueue, "sceSifSetRpcQueue"},
    {"sifcmd", 20, hle_sceSifGetNextRequest, "sceSifGetNextRequest"},
    {"sifcmd", 21, hle_sceSifExecRequest, "sceSifExecRequest"}, {"sifcmd", 22, hle_sceSifRpcLoop, "sceSifRpcLoop"},
    {"stdio", 4, l_printf, "printf"},
    {"sysclib", 6, l_toupper, "toupper"}, {"sysclib", 7, l_tolower, "tolower"}, {"sysclib", 10, l_memchr, "memchr"},
    {"sysclib", 11, l_memcmp, "memcmp"}, {"sysclib", 12, l_memcpy, "memcpy"}, {"sysclib", 13, l_memmove, "memmove"},
    {"sysclib", 14, l_memset, "memset"}, {"sysclib", 15, l_memcmp, "bcmp"}, {"sysclib", 16, l_bcopy, "bcopy"},
    {"sysclib", 17, l_bzero, "bzero"}, {"sysclib", 19, l_sprintf, "sprintf"}, {"sysclib", 20, l_strcat, "strcat"},
    {"sysclib", 21, l_strchr, "strchr"}, {"sysclib", 22, l_strcmp, "strcmp"}, {"sysclib", 23, l_strcpy, "strcpy"},
    {"sysclib", 27, l_strlen, "strlen"}, {"sysclib", 29, l_strncmp, "strncmp"},
    {"sysclib", 30, l_strncpy, "strncpy"}, {"sysclib", 32, l_strrchr, "strrchr"}, {"sysclib", 36, l_strtol, "strtol"},
    {"ioman", 4, io_open, "open"}, {"ioman", 5, io_close, "close"}, {"ioman", 6, io_read, "read"},
    {"ioman", 7, io_write, "write"}, {"ioman", 8, io_lseek, "lseek"},
    {"cdvdman", 4, hle_one, "sceCdInit"}, {"cdvdman", 6, cd_read, "sceCdRead"}, {"cdvdman", 8, cd_zero, "sceCdGetError"},
    {"cdvdman", 10, cd_search_file, "sceCdSearchFile"}, {"cdvdman", 11, cd_zero, "sceCdSync"},
    {"cdvdman", 12, cd_disk_type, "sceCdGetDiskType"}, {"cdvdman", 13, cd_disk_ready, "sceCdDiskReady"},
    {"cdvdman", 83, cd_read_dual_info, "sceCdReadDvdDualInfo"},
    {"cdvdman", 84, cd_layer_search_file, "sceCdLayerSearchFile"},
    {"libsd", 4, sd_init, "sceSdInit"}, {"libsd", 5, sd_set_param, "sceSdSetParam"},
    {"libsd", 6, sd_get_param, "sceSdGetParam"}, {"libsd", 7, sd_set_switch, "sceSdSetSwitch"},
    {"libsd", 8, sd_get_switch, "sceSdGetSwitch"}, {"libsd", 9, sd_set_addr, "sceSdSetAddr"},
    {"libsd", 10, sd_get_addr, "sceSdGetAddr"}, {"libsd", 11, sd_set_core_attr, "sceSdSetCoreAttr"},
    {"libsd", 12, sd_get_core_attr, "sceSdGetCoreAttr"}, {"libsd", 13, sd_note2pitch, "sceSdNote2Pitch"},
    {"libsd", 17, sd_voice_trans, "sceSdVoiceTrans"}, {"libsd", 18, sd_block_trans, "sceSdBlockTrans"},
    {"libsd", 19, sd_trans_status, "sceSdVoiceTransStatus"}, {"libsd", 20, sd_trans_status, "sceSdBlockTransStatus"},
    {"libsd", 22, sd_set_irq_callback, "sceSdSetIRQCallback"}, {"libsd", 23, hle_ok, "sceSdSetEffectAttr"},
    {"libsd", 25, hle_ok, "sceSdClearEffectWorkArea"},
    {"libsd", 26, sd_set_trans_intr_handler, "sceSdSetTransIntrHandler"},
    {"libsd", 27, sd_set_spu2_intr_handler, "sceSdSetSpu2IntrHandler"},
    {"vblank", 4, vb_wait, "WaitVblankStart"}, {"vblank", 5, vb_wait, "WaitVblankEnd"},
    {"vblank", 6, vb_wait, "WaitVblank"}, {"vblank", 7, vb_wait, "WaitNonVblank"},
};

IopFn iop_rt_builtin(const char *lib, unsigned num, const char **name)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].num == num && !strncmp(table[i].lib, lib, 8)) {
            if (name) *name = table[i].name;
            return table[i].fn;
        }
    return 0;
}

void iop_rt_libs_init(IopRt *rt)
{
    rt->sd.irq_enable = 0;
}
