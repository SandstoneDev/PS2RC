"""Find VU microcode embedded in EE images as VIF MPG packets (VIFcode: CMD[31:24] (bit 31 = interrupt), NUM[23:16]
(64-bit instructions, 0 = 256), IMMEDIATE[15:0] (load address in 64-bit units), as PCSX2's VIF code reads it).
The microcode payload follows the VIFcode and is 64-bit aligned. Engines commonly pack each program as a DMA-chain
fragment: DMAtag (qword) whose upper half carries VIF NOP + MPG, then the payload; consecutive packets continue it.
"""
import struct
from dataclasses import dataclass

from ..vu.micro import valid_pair, is_nop_pair

MPG = 0x4A


@dataclass
class MpgPacket:
    vifcode_addr: int
    num: int
    load: int          # in 64-bit instruction units
    payload_addr: int

    @property
    def end(self):
        return self.payload_addr + self.num * 8


def find_mpg_packets(img):
    packets = []
    for seg in img.segments:
        data = seg.data[:seg.filesz]
        n = len(data) // 4
        ws = struct.unpack("<%dI" % n, data[:n * 4])
        for k, w in enumerate(ws):
            if (w >> 24) & 0x7F != MPG:
                continue
            p = k + 1
            addr = seg.vaddr + p * 4
            if addr % 8:
                continue
            num = (w >> 16) & 0xFF or 256
            if p + 2 * num > n:
                continue
            nops = 0
            ok = True
            for j in range(num):
                lo, up = ws[p + 2 * j], ws[p + 2 * j + 1]
                if not valid_pair(lo, up):
                    ok = False
                    break
                nops += is_nop_pair(lo, up)
            if ok and nops < num:
                packets.append(MpgPacket(seg.vaddr + k * 4, num, w & 0xFFFF, addr))
    return packets


def find_vu_regions(img, merge_gap=64):
    """Address ranges [lo, hi) occupied by MPG packets (from the qword holding the DMA tag to the payload end),
    merging packets separated by at most `merge_gap` bytes."""
    regions = []
    for pk in sorted(find_mpg_packets(img), key=lambda p: p.vifcode_addr):
        lo, hi = pk.vifcode_addr & ~0xF, pk.end
        if regions and lo - regions[-1][1] <= merge_gap:
            regions[-1][1] = max(regions[-1][1], hi)
        else:
            regions.append([lo, hi])
    return [tuple(r) for r in regions]
