/* hle_mmio.c - every guest access outside the 32 MB RAM window (src/hle/rt/r5900_rt.h slow path): RAM mirrors,
 * scratchpad, VU memory, EE hardware registers (timers, DMAC, INTC, GIF/VIF status) and GS privileged registers.
 *
 * Graphics are not emulated: DMA to VIF0/VIF1/GIF/IPU completes instantly and its data is dropped; the only DMA that
 * moves data is scratchpad <-> RAM (channels 8/9), which game code can use as a memcpy engine. Completion sets
 * D_STAT.CIS and raises the DMAC interrupt; it is delivered at the next safe point (not inside the register store).
 * Every register access is a safe point (fixed virtual-time charge), so hardware polling loops always progress.
 *
 * Which accesses get here: the host v0 harness traps every non-RAM address (ps2rc/pipeline/patch_rt_header.py "host");
 * the PSP build traps only in the functions of the trap set (tests/phase6psp/trapset.py: rtx_* / rmx_* accessors) -
 * everywhere else non-RAM addresses are masked into RAM, which is right for the scratchpad (aliased at guest RAM
 * 0x0000-0x3FFF, hle_spr == hle_ram) and the uncached/KSEG mirrors. hle_plat_note_mmio records who accessed hardware
 * (host profiling run -> the dynamic part of the trap set).
 */
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "hle_prof.h"                          /* build 22: zones hle.dma, hle.memtrap */
#include "hle_fast.h"                          /* x_dma */

uint32_t hle_gs_csr;
static uint32_t *eereg;                              /* 0x10000000-0x1000FFFF register file (32-bit view), 64 KB */
static uint8_t *vumem;                               /* 0x11000000-0x1100FFFF, 64 KB */
static uint64_t *gspriv;                             /* 0x12000000-0x12001FFF, 8 KB */
static uint64_t timer_base[4];
static uint64_t dma_kicks[10], dma_qwords[10];
/* GIF PATH3 masking as a renderer's DMA queue may use it (VIF command 0x21): it writes VIF MSKPATH3(1) straight
   into the VIF1 FIFO, waits for GIF_STAT.M3P, kicks GIF DMA (data parks in the GIF FIFO: GIF_STAT.FQC != 0 is
   awaited), then kicks the VIF1 chain whose end lets PATH3 run again. Modelled with two flags. */
static int path3_masked, gif_fifo_parked;
static unsigned unknown_logged;
void hle_vsync_flag_update(void);

#define R(off) eereg[((off) & 0xFFFF) >> 2]
static const uint32_t chan_base[10] = {0x8000, 0x9000, 0xA000, 0xB000, 0xB400, 0xC000, 0xC400, 0xC800, 0xD000, 0xD400};
static const char *chan_name[10] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};

/* COP0 condition CPCOND0 (BC0F/BC0T DMA-completion polls in Sony libdma/libgraph and in renderers, translated
   by the current back-end as rt_cpcond0): every transfer is complete as soon as it starts; each poll is a safe point */
static int cpcond0(R5900Ctx *c)
{
    hle_stat_mmio++;
    if (c && !hle_in_intr) hle_safe_point(c, 50);
    return 1;
}

void hle_mmio_init(void)
{
    if (!eereg) {                                    /* big buffers: the hole on the PSP */
        eereg = hle_plat_big_alloc(0x10000);
        vumem = hle_plat_big_alloc(0x10000);
        gspriv = hle_plat_big_alloc(0x2000);
    }
    rt_cpcond0_hook = cpcond0;
    R(0xF520) = 0x1201;                              /* D_ENABLER: DMA enabled (ps2sdk default value) */
    R(0xF590) = 0x1201;
    R(0xE000) = 1;                                   /* D_CTRL.DMAE */
    hle_gs_csr = 0;
}

static int chan_of(uint32_t off)
{
    for (int i = 0; i < 10; i++) if (off >= chan_base[i] && off < chan_base[i] + 0x100) return i;
    return -1;
}

static uint32_t timer_count(int t)
{
    uint32_t mode = R(t * 0x800 + 0x10);
    static const uint32_t div[4] = {1, 16, 256, 9372};
    uint64_t bus = hle_vtime / 2;
    return (uint32_t)(((bus - timer_base[t]) / div[mode & 3]) & 0xFFFF);
}

static int timer_of(uint32_t off)
{
    if (off < 0x0800) return 0;
    if (off < 0x1000) return 1;
    if (off < 0x1800) return 2;
    return 3;
}

/* ------------------------------------------------------------------ scratchpad DMA (the only DMA that moves data)
   DMA addresses are physical, decoded like PCSX2 (pcsx2/Dmac.cpp dmaGetAddr / SPRdmaGetAddr): bit 31 (tag SPR flag)
   or 0x70000000 -> scratchpad; otherwise & 0x1FFFFFF0: RAM, 0x10000000-0x10003FFF = scratchpad without the flag,
   0x11000000-0x1100FFFF = VU memory. Anything else: logged, the quadword is skipped. */
static uint8_t *dma_qw(uint32_t addr)
{
    if ((addr & 0x80000000u) || (addr & 0x70000000u) == 0x70000000u) return hle_spr + (addr & 0x3FF0u);
    addr &= 0x1FFFFFF0u;
    if (addr < hle_ram_end) return hle_ram + addr;
    if (addr >= 0x10000000u && addr < 0x10004000u) return hle_spr + (addr & 0x3FF0u);
    if (addr >= 0x11000000u && addr < 0x11010000u) return vumem + (addr - 0x11000000u);
    static unsigned bad;
    if (bad++ < 32) LOG("DMA address %08x is not memory - quadword skipped\n", addr);
    return NULL;
}
/* x_dma=1: the same copies as runs - as many quadwords as stay contiguous on
   both sides (RAM below the end; the scratchpad up to its 16 KB wrap) go in one memcpy when source and destination do
   not overlap (the scratchpad may alias low RAM); everything else (SPR / VU / unmapped addresses, overlap, single
   quadwords) takes the per-quadword path. Build 29 on the PSP: 316,208 fromSPR kicks of ~62 quadwords, ~15-22 us each
   (profiler zone hle.dma) - one dma_qw() + memcpy call per quadword. */
static int x_dma_on(void)
{
    if (hle_x_dma < 0) hle_x_dma = hle_x_opt("dma", 1) != 0;   /* round 3a: default on */
    return hle_x_dma;
}

static uint32_t x_ram_run(uint32_t madr, uint8_t **p)    /* quadwords contiguous in RAM from madr (0: not RAM) */
{
    *p = NULL;
    if ((madr & 0x80000000u) || (madr & 0x70000000u) == 0x70000000u) return 0;
    uint32_t a = madr & 0x1FFFFFF0u;
    if (a >= hle_ram_end) return 0;
    *p = hle_ram + a;
    return (hle_ram_end - a) >> 4;
}

static void x_spr_copy(uint32_t sadr, uint32_t madr, uint32_t qwc, int to_spr)
{
    while (qwc) {
        uint8_t *ram;
        uint32_t n = x_ram_run(madr, &ram), s = sadr & 0x3FF0u, ns = (0x4000u - s) >> 4;
        if (n > ns) n = ns;
        if (n > qwc) n = qwc;
        uint8_t *spr = hle_spr + s;
        if (n > 1 && (ram + 16u * n <= spr || spr + 16u * n <= ram)) {
            if (to_spr) memcpy(spr, ram, 16u * n);
            else memcpy(ram, spr, 16u * n);
        } else {
            n = 1;
            uint8_t *q = dma_qw(madr);
            if (q) {
                if (to_spr) memcpy(spr, q, 16);
                else memcpy(q, spr, 16);
            }
        }
        sadr += 16u * n;
        madr += 16u * n;
        qwc -= n;
    }
}

static void spr_copy_to(uint32_t sadr, uint32_t madr, uint32_t qwc)        /* RAM -> SPR */
{
    if (x_dma_on()) { x_spr_copy(sadr, madr, qwc, 1); return; }
    for (uint32_t i = 0; i < qwc; i++) {
        uint8_t *src = dma_qw(madr + 16 * i);
        if (src) memcpy(hle_spr + ((sadr + 16 * i) & 0x3FF0u), src, 16);
    }
}
static void spr_copy_from(uint32_t sadr, uint32_t madr, uint32_t qwc)      /* SPR -> RAM */
{
    if (x_dma_on()) { x_spr_copy(sadr, madr, qwc, 0); return; }
    for (uint32_t i = 0; i < qwc; i++) {
        uint8_t *dst = dma_qw(madr + 16 * i);
        if (dst) memcpy(dst, hle_spr + ((sadr + 16 * i) & 0x3FF0u), 16);
    }
}

static uint64_t tagword(uint32_t a)
{
    uint64_t v = 0x70000000ull << 0;                 /* unreadable tag: id 7 (end) */
    uint8_t *p = dma_qw(a);
    if (p) memcpy(&v, p, 8);
    return v;
}

void hle_dma_kick(R5900Ctx *c, int ch)
{
    PZ_SCOPE(dma);
    (void)c;
    uint32_t b = chan_base[ch];
    uint32_t chcr = R(b), madr = R(b + 0x10), qwc = R(b + 0x20) & 0xFFFF, tadr = R(b + 0x30), sadr = R(b + 0x80);
    int mode = (chcr >> 2) & 3;
    dma_kicks[ch]++;
    VVLOG("DMA kick %s chcr %08x madr %08x qwc %x tadr %08x sadr %08x\n", chan_name[ch], chcr, madr, qwc, tadr, sadr);
    if (ch == 9) {                                   /* toSPR */
        if (mode == 0) {
            spr_copy_to(sadr, madr, qwc);
            dma_qwords[ch] += qwc;
            madr += 16 * qwc;
            sadr += 16 * qwc;
        } else if (mode == 1) {                      /* source chain */
            int guard = 0, end = 0;
            uint32_t spr = sadr;
            uint32_t retstk[2];
            int asp = 0;
            while (!end && guard++ < 100000) {
                uint32_t tadr_cur = tadr;
                uint64_t tag = tagword(tadr);
                uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32);
                uint32_t data;
                switch (id) {
                case 0: data = addr; end = 1; break;                         /* refe */
                case 1: data = tadr + 16; tadr = data + 16 * n; break;       /* cnt */
                case 2: data = tadr + 16; tadr = addr; break;                /* next */
                case 3: case 4: data = addr; tadr += 16; break;              /* ref / refs */
                case 5: data = tadr + 16; if (asp < 2) retstk[asp++] = data + 16 * n; tadr = addr; break;   /* call */
                case 6: data = tadr + 16; if (asp) tadr = retstk[--asp]; else end = 1; break;              /* ret */
                default: data = tadr + 16; end = 1; break;                   /* end */
                }
                if (chcr & 0x40) {                                           /* TTE: the tag qword goes to SPR too */
                    uint8_t *tq = dma_qw(tadr_cur);
                    if (tq) memcpy(hle_spr + (spr & 0x3FF0u), tq, 16);
                    spr += 16;
                }
                spr_copy_to(spr, data, n);
                spr += 16 * n;
                dma_qwords[ch] += n;
                if ((tag >> 31) & 1 && (chcr & 0x80)) end = 1;               /* IRQ bit with TIE */
            }
            sadr = spr;
            R(b + 0x30) = tadr;
        } else {                                     /* interleave: TQWC qwords, then skip SQWC in memory (D_SQWC) */
            uint32_t sq = R(0xE030), tq = (sq >> 16) & 0xFF, skip = sq & 0xFF;
            if (!tq) tq = qwc;
            for (uint32_t left = qwc; left > 0;) {
                uint32_t n = left < tq ? left : tq;
                spr_copy_to(sadr, madr, n);
                sadr += 16 * n;
                madr += 16 * (n + skip);
                left -= n;
            }
            dma_qwords[ch] += qwc;
        }
    } else if (ch == 8) {                            /* fromSPR */
        if (mode == 0) {
            spr_copy_from(sadr, madr, qwc);
            dma_qwords[ch] += qwc;
            madr += 16 * qwc;
            sadr += 16 * qwc;
        } else if (mode == 2) {                      /* interleave: TQWC qwords, then skip SQWC in memory */
            uint32_t sq = R(0xE030), tq = (sq >> 16) & 0xFF, skip = sq & 0xFF;
            if (!tq) tq = qwc;
            for (uint32_t left = qwc; left > 0;) {
                uint32_t n = left < tq ? left : tq;
                spr_copy_from(sadr, madr, n);
                sadr += 16 * n;
                madr += 16 * (n + skip);
                left -= n;
            }
            dma_qwords[ch] += qwc;
        } else {                                     /* destination chain: tags come from SPR */
            int guard = 0, end = 0;
            uint32_t spr = sadr;
            while (!end && guard++ < 100000) {
                uint64_t tag;
                memcpy(&tag, hle_spr + (spr & (HLE_SPR_SIZE - 16)), 8);
                uint32_t n = (uint32_t)(tag & 0xFFFF), id = (uint32_t)(tag >> 28) & 7, addr = (uint32_t)(tag >> 32);
                spr += 16;
                spr_copy_from(spr, addr, n);
                spr += 16 * n;
                dma_qwords[ch] += n;
                if (id == 7) end = 1;
            }
            sadr = spr;
        }
    } else {
        /* VIF0/VIF1/GIF/IPU/SIF: graphics/MPEG/IOP data is dropped; the transfer completes immediately */
        dma_qwords[ch] += qwc;
        madr += 16 * qwc;
        if (ch == 2 && path3_masked) gif_fifo_parked = 1;
        if (ch == 1) path3_masked = gif_fifo_parked = 0;
    }
    R(b) = chcr & ~0x100u;                           /* STR off */
    R(b + 0x10) = madr;
    R(b + 0x20) = 0;
    R(b + 0x80) = sadr;
    hle_dmac_raise(ch);
}

/* ------------------------------------------------------------------ EE register file */
static uint32_t eereg_read(uint32_t off)
{
    if (off < 0x2000) {                              /* timers */
        int t = timer_of(off);
        uint32_t r = off & 0x7FF;
        if (r == 0x00) return timer_count(t);
        return R(off);
    }
    switch (off) {
    case 0x3020:                                     /* GIF_STAT: no active path; M3P, FQC per the PATH3 model */
        return (path3_masked ? 2u : 0u) | (gif_fifo_parked ? 0x10000000u : 0u);
    case 0x3800: case 0x3C00: return 0;              /* VIF0/VIF1_STAT: idle, FIFO empty */
    case 0xE010: return hle_dmac_stat;               /* D_STAT */
    case 0xF000: return hle_intc_stat;               /* INTC_STAT */
    case 0xF010: return hle_intc_mask;               /* INTC_MASK */
    case 0xF520: return R(0xF590);                   /* D_ENABLER mirrors D_ENABLEW */
    case 0xF230: return R(0xF230) | 0x70000;         /* SIF SMFLG: IOP side initialised */
    default: return R(off);
    }
}

static void eereg_write(R5900Ctx *c, uint32_t off, uint32_t v)
{
    if (off < 0x2000) {
        int t = timer_of(off);
        if ((off & 0x7FF) == 0x00) {                 /* COUNT write: rebase */
            static const uint32_t div[4] = {1, 16, 256, 9372};
            uint32_t mode = R((off & ~0x7FFu) + 0x10);
            timer_base[t] = hle_vtime / 2 - (uint64_t)(v & 0xFFFF) * div[mode & 3];
            return;
        }
        R(off) = v;
        return;
    }
    switch (off) {
    case 0xE010:                                     /* D_STAT: CIS/BEIS/SIS write-1-to-clear, CIM/BEIM/SIM toggle */
        hle_dmac_stat = (hle_dmac_stat & ~(v & 0x0000E3FFu)) ^ (v & 0xE3FF0000u);
        hle_deliver_interrupts();
        return;
    case 0xF000:                                     /* INTC_STAT: write 1 to clear */
        hle_intc_stat &= ~v;
        return;
    case 0xF010:                                     /* INTC_MASK: write 1 to toggle */
        hle_intc_mask ^= v;
        hle_deliver_interrupts();
        return;
    case 0x3000:                                     /* GIF_CTRL: RST */
        if (v & 1) path3_masked = gif_fifo_parked = 0;
        return;
    }
    int ch = chan_of(off);
    R(off) = v;
    if (ch >= 0 && off == chan_base[ch] && (v & 0x100)) hle_dma_kick(c, ch);
}

static void unknown(const char *kind, uint32_t a, uint64_t v)
{
    if (unknown_logged++ < 200) LOG("MMIO %s to unmapped address %08x (value %llx)\n", kind, a, (unsigned long long)v);
}

/* host pointer for plain-memory regions (mirrors, scratchpad, VU memory); NULL for registers / unmapped */
/* build 24: what the slow memory path serves (the PSP profiler of build 23 measured ~3,300 hle_mem_* calls per
   main-loop iteration at ~2 us each): counted by address class, reported at the end (hle_mmio_report) */
uint64_t hle_memtrap_class[7];          /* 0 RAM above the end, 1 uncached mirror, 2 KSEG, 3 scratchpad, 4 VU mem,
                                           5 hardware register / other, 6 128-bit accesses */
static uint8_t *plain(uint32_t a, int size)
{
    uint32_t top = a >> 28;
    hle_memtrap_class[a < hle_ram_end ? 0 : (top == 2 || top == 3) ? 1 : (top >= 8 && top <= 0xB) ? 2
                      : (a >= 0x70000000u && a < 0x70000000u + HLE_SPR_SIZE) ? 3
                      : (a >= 0x11000000u && a < 0x11010000u) ? 4 : 5]++;
    if (size == 16) hle_memtrap_class[6]++;
    if (a < hle_ram_end && a + size <= hle_ram_end) return hle_ram + a;   /* (trap-set accessors: RAM above the end) */
    if (top == 2 || top == 3) {                      /* uncached / uncached-accelerated RAM mirrors */
        uint32_t p = a & 0x0FFFFFFFu;
        if (p + size <= hle_ram_end) return hle_ram + p;
        return NULL;
    }
    if (top == 8 || top == 9 || top == 0xA || top == 0xB) {   /* KSEG0/KSEG1 */
        uint32_t p = a & 0x1FFFFFFFu;
        if (p + size <= hle_ram_end) return hle_ram + p;
        return NULL;
    }
    if (a >= 0x70000000u && a + size <= 0x70000000u + HLE_SPR_SIZE) return hle_spr + (a - 0x70000000u);
    if (a >= 0x11000000u && a + size <= 0x11010000u) return vumem + (a - 0x11000000u);
    return NULL;
}

static uint64_t mem_read(R5900Ctx *c, uint32_t a, int size)
{
    uint8_t *p = plain(a, size);
    if (p) {
        uint64_t v = 0;
        memcpy(&v, p, size);
        return v;
    }
    hle_stat_mmio++;
    uint32_t pa = (a >> 28) >= 0xA ? a & 0x1FFFFFFFu : a;    /* KSEG1 hardware window */
    if (pa >= 0x10000000u && pa < 0x10010000u) {
        if (c && !hle_in_intr) hle_safe_point(c, 50);
        uint32_t off = pa & 0xFFFF;
        uint64_t v = eereg_read(off & ~3u);
        if (size == 8) v |= (uint64_t)eereg_read((off & ~3u) + 4) << 32;
        else v >>= 8 * (off & 3);
        VVLOG("MMIO read%d %08x -> %llx\n", size * 8, a, (unsigned long long)v);
        return size == 8 ? v : v & ((1ull << (size * 8)) - 1);
    }
    if (pa >= 0x12000000u && pa < 0x12002000u) {
        if (c && !hle_in_intr) hle_safe_point(c, 50);
        uint32_t off = pa & 0x1FFF;
        if ((off & ~7u) == 0x1000) {                 /* CSR: FIFO empty, events, field; ID/REV in the top bits */
            uint64_t v = (hle_gs_csr & 0x3FFFu) | 0x4000u | 0x551B0000u;
            return v >> (8 * (off & 7));
        }
        return gspriv[off >> 3] >> (8 * (off & 7));
    }
    if (pa >= 0x1FC00000u && pa < 0x20000000u) return 0;      /* BIOS ROM (not provided) */
    unknown("read", a, 0);
    return 0;
}

static void mem_write(R5900Ctx *c, uint32_t a, int size, uint64_t v)
{
    uint8_t *p = plain(a, size);
    if (p) { memcpy(p, &v, size); return; }
    hle_stat_mmio++;
    uint32_t pa = (a >> 28) >= 0xA ? a & 0x1FFFFFFFu : a;
    if (pa >= 0x10000000u && pa < 0x10010000u) {
        uint32_t off = pa & 0xFFFF;
        VVLOG("MMIO write%d %08x <- %llx\n", size * 8, a, (unsigned long long)v);
        if (size == 8) {
            eereg_write(c, off, (uint32_t)v);
            eereg_write(c, off + 4, (uint32_t)(v >> 32));
        } else if (size == 4) {
            eereg_write(c, off, (uint32_t)v);
        } else {                                     /* byte/halfword register store: merge into the word */
            uint32_t w = R(off & ~3u), sh = 8 * (off & 3), m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
            eereg_write(c, off & ~3u, (w & ~m) | (((uint32_t)v << sh) & m));
        }
        if (c && !hle_in_intr) hle_safe_point(c, 50);
        return;
    }
    if (pa >= 0x12000000u && pa < 0x12002000u) {
        uint32_t off = pa & 0x1FFF;
        if ((off & ~7u) == 0x1000) {                 /* CSR: write 1 clears SIGNAL/FINISH/HSINT/VSINT/EDWINT */
            hle_gs_csr &= ~((uint32_t)v & 0x1Fu);
            if (v & 0x200) LOG("GS CSR reset\n");
            return;
        }
        uint64_t old = gspriv[off >> 3];
        if (size == 8) gspriv[off >> 3] = v;
        else {
            uint64_t m = (size == 4 ? 0xFFFFFFFFull : size == 2 ? 0xFFFFull : 0xFFull) << (8 * (off & 7));
            gspriv[off >> 3] = (old & ~m) | ((v << (8 * (off & 7))) & m);
        }
        return;
    }
    if (pa >= 0x10000000u && pa < 0x11000000u) return;
    unknown("write", a, v);
}

/* the plain-memory cases a trap-set accessor sends here most often -
   the scratchpad (0x70000000-0x70003FFF) and the uncached RAM mirrors (0x2xxxxxxx / 0x3xxxxxxx) - are served first,
   before the profiler scope and the class counters (build 23 on the PSP-3000: ~2,700 such calls per main-loop
   iteration at ~0.7 us each through PZ_SCOPE + plain()). Same addresses and the same order as plain() (RAM below the
   end, mirrors, scratchpad); everything else - KSEG, VU memory, registers - takes the unchanged slow path below.
   Build 24 inlined this check into all 5,036 trap-set accessors instead (ps2rc/pipeline/patch_rt_header.py --alias
   inline): +284 KB of code and every zone slower; `--alias call` keeps one out-of-line call per access and lands here. */
uint32_t hle_memtrap_fast;              /* accesses served by fast_plain (reported with the classes) */
static inline uint8_t *fast_plain(uint32_t a, uint32_t size)
{
    if (a < hle_ram_end && a + size <= hle_ram_end) return hle_ram + a;
    if ((a >> 29) == 1u) {                               /* 0x2xxxxxxx / 0x3xxxxxxx: uncached RAM mirrors */
        uint32_t p = a & 0x0FFFFFFFu;
        return p + size <= hle_ram_end ? hle_ram + p : NULL;
    }
    uint32_t s = a - 0x70000000u;
    if (s < HLE_SPR_SIZE && s + size <= HLE_SPR_SIZE) return hle_spr + s;
    return NULL;
}

/* public entry points (the runtime header's slow path): plain memory first, else the register model; a hardware
   access is reported to the platform with the generated function that made it (trap-set profiling on the host) */
static __attribute__((noinline)) uint64_t mem_read_slow(R5900Ctx *c, uint32_t a, int size, void *ra)
{
    PZ_SCOPE(memtrap);
    uint8_t *p = plain(a, size);
    if (p) {
        uint64_t v = 0;
        memcpy(&v, p, size);
        return v;
    }
    hle_plat_note_mmio(ra, a);
    return mem_read(c, a, size);
}

uint64_t hle_mem_read(R5900Ctx *c, uint32_t a, int size)
{
    uint8_t *p = fast_plain(a, (uint32_t)size);
    if (__builtin_expect(p != NULL, 1)) {
        hle_memtrap_fast++;
        switch (size) {
        case 1: return *p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        case 8: { uint64_t v; memcpy(&v, p, 8); return v; }
        default: { uint64_t v = 0; memcpy(&v, p, size); return v; }
        }
    }
    return mem_read_slow(c, a, size, __builtin_return_address(0));
}

static __attribute__((noinline)) void mem_write_slow(R5900Ctx *c, uint32_t a, int size, uint64_t v, void *ra)
{
    PZ_SCOPE(memtrap);
    uint8_t *p = plain(a, size);
    if (p) { memcpy(p, &v, size); return; }
    hle_plat_note_mmio(ra, a);
    mem_write(c, a, size, v);
}

void hle_mem_write(R5900Ctx *c, uint32_t a, int size, uint64_t v)
{
    uint8_t *p = fast_plain(a, (uint32_t)size);
    if (__builtin_expect(p != NULL, 1)) {
        hle_memtrap_fast++;
        switch (size) {
        case 1: *p = (uint8_t)v; return;
        case 2: { uint16_t w = (uint16_t)v; memcpy(p, &w, 2); return; }
        case 4: { uint32_t w = (uint32_t)v; memcpy(p, &w, 4); return; }
        case 8: memcpy(p, &v, 8); return;
        default: memcpy(p, &v, size); return;
        }
    }
    mem_write_slow(c, a, size, v, __builtin_return_address(0));
}

static __attribute__((noinline)) void mem_read128_slow(R5900Ctx *c, uint32_t a, R5900Reg128 *d, void *ra)
{
    PZ_SCOPE(memtrap);
    uint8_t *p = plain(a, 16);
    if (p) { memcpy(d, p, 16); return; }
    hle_plat_note_mmio(ra, a);
    d->d[0] = mem_read(c, a, 8);
    d->d[1] = mem_read(c, a + 8, 8);
}

void hle_mem_read128(R5900Ctx *c, uint32_t a, R5900Reg128 *d)
{
    uint8_t *p = fast_plain(a, 16);
    if (__builtin_expect(p != NULL, 1)) { hle_memtrap_fast++; memcpy(d, p, 16); return; }
    mem_read128_slow(c, a, d, __builtin_return_address(0));
}

static __attribute__((noinline)) void mem_write128_slow(R5900Ctx *c, uint32_t a, const R5900Reg128 *s, void *ra);

void hle_mem_write128(R5900Ctx *c, uint32_t a, const R5900Reg128 *s)
{
    uint8_t *p = fast_plain(a, 16);
    if (__builtin_expect(p != NULL, 1)) { hle_memtrap_fast++; memcpy(p, s, 16); return; }
    mem_write128_slow(c, a, s, __builtin_return_address(0));
}

static __attribute__((noinline)) void mem_write128_slow(R5900Ctx *c, uint32_t a, const R5900Reg128 *s, void *ra)
{
    PZ_SCOPE(memtrap);
    uint8_t *p = plain(a, 16);
    if (p) { memcpy(p, s, 16); return; }
    hle_plat_note_mmio(ra, a);
    hle_stat_mmio++;
    uint32_t pa = (a >> 28) >= 0xA ? a & 0x1FFFFFFFu : a;
    if (pa >= 0x10005000u && pa < 0x10006000u) {        /* VIF1 FIFO: only MSKPATH3 codes matter (PATH3 model) */
        for (int i = 0; i < 4; i++)
            if (((s->w[i] >> 24) & 0x7F) == 0x06) path3_masked = (s->w[i] >> 15) & 1;
        if (!path3_masked) gif_fifo_parked = 0;
        return;
    }
    if (pa >= 0x10004000u && pa < 0x10008000u) return;   /* VIF0/GIF/IPU FIFOs: graphics data dropped */
    mem_write(c, a, 8, s->d[0]);
    mem_write(c, a + 8, 8, s->d[1]);
}

void hle_mmio_report(void)
{
    LOG("slow memory path by class: RAM-above-end %llu, uncached mirror %llu, KSEG %llu, scratchpad %llu, VU mem %llu, "
        "register/other %llu (128-bit %llu)\n", (unsigned long long)hle_memtrap_class[0],
        (unsigned long long)hle_memtrap_class[1], (unsigned long long)hle_memtrap_class[2],
        (unsigned long long)hle_memtrap_class[3], (unsigned long long)hle_memtrap_class[4],
        (unsigned long long)hle_memtrap_class[5], (unsigned long long)hle_memtrap_class[6]);
    LOG("slow memory path: %u accesses served before the profiler scope (RAM above the end / mirrors / scratchpad, "
        "build 25)\n", (unsigned)hle_memtrap_fast);
    LOG("DMA kicks:");
    for (int i = 0; i < 10; i++)
        if (dma_kicks[i]) LOG(" %s %llu (%llu qw)", chan_name[i], (unsigned long long)dma_kicks[i],
                              (unsigned long long)dma_qwords[i]);
    LOG("\n");
}
