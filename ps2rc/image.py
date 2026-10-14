"""Guest image model of a PS2 EE executable (Phase 1).

Loads an ELF32 little-endian MIPS executable: loadable segments (bss zero-filled), overlay slots (PT_LOAD entries with
no file data that share one address - the Metrowerks static overlay scheme), $gp from .reginfo, and, when the
binary is not stripped, its symbols and REL relocations.
"""
import bisect
import struct
from dataclasses import dataclass, field

from elftools.elf.elffile import ELFFile


class ImageError(Exception):
    pass


@dataclass
class Segment:
    vaddr: int
    filesz: int
    memsz: int
    flags: int
    data: bytes  # memsz bytes, bss zero-filled

    @property
    def end(self):
        return self.vaddr + self.memsz


@dataclass
class OverlaySlot:
    index: int      # program-header index (overlay id)
    vaddr: int
    memsz: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: str       # 'func', 'object', 'section', 'file', 'notype'
    bind: str       # 'global', 'local', 'weak'


@dataclass
class Relocation:
    offset: int     # guest virtual address of the relocated field
    type: int       # R_MIPS_* number
    symbol: int     # symbol index


@dataclass
class Image:
    path: str
    entry: int
    gp: int
    segments: list
    overlay_slots: list
    symbols: list = field(default_factory=list)
    relocations: list = field(default_factory=list)

    def __post_init__(self):
        self.segments.sort(key=lambda s: s.vaddr)
        self._starts = [s.vaddr for s in self.segments]

    def _segment(self, addr):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0 and addr < self.segments[i].end:
            return self.segments[i]
        return None

    def contains(self, addr):
        return self._segment(addr) is not None

    def read(self, addr, n):
        seg = self._segment(addr)
        if seg is None or addr + n > seg.end:
            raise ImageError("address range 0x%08x+%d is not mapped" % (addr, n))
        off = addr - seg.vaddr
        return seg.data[off:off + n]

    def word(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def stripped(self):
        """Copy of this image without symbols and relocations (for testing analyses against ground truth)."""
        return Image(self.path, self.entry, self.gp, list(self.segments), list(self.overlay_slots), [], [])

    def check_overlay(self, ov):
        """Raise ImageError unless the overlay was linked for this image's overlay slot."""
        if not 1 <= ov.id <= len(self.overlay_slots):
            raise ImageError("overlay %s has id %d, image has %d slots" % (ov.name, ov.id, len(self.overlay_slots)))
        slot = self.overlay_slots[ov.id - 1]
        if ov.load_addr != slot.vaddr:
            raise ImageError("overlay %s linked for 0x%08x, slot is at 0x%08x" % (ov.name, ov.load_addr, slot.vaddr))
        if ov.memsz != slot.memsz:
            raise ImageError("overlay %s needs 0x%x bytes, slot has 0x%x" % (ov.name, ov.memsz, slot.memsz))


_SYM_TYPES = {"STT_FUNC": "func", "STT_OBJECT": "object", "STT_SECTION": "section", "STT_FILE": "file",
              "STT_NOTYPE": "notype"}
_SYM_BINDS = {"STB_GLOBAL": "global", "STB_LOCAL": "local", "STB_WEAK": "weak"}


def load_elf(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf.header["e_machine"] != "EM_MIPS" or not elf.little_endian or elf.elfclass != 32:
            raise ImageError("%s: not a little-endian ELF32 MIPS file" % path)
        segments, slots = [], []
        for i, ph in enumerate(elf.iter_segments()):
            if ph["p_type"] != "PT_LOAD" or ph["p_memsz"] == 0:
                continue
            if ph["p_filesz"] == 0 and _is_overlay_slot(elf, ph):
                slots.append(OverlaySlot(index=len(slots) + 1, vaddr=ph["p_vaddr"], memsz=ph["p_memsz"]))
                continue
            data = ph.data() + bytes(ph["p_memsz"] - ph["p_filesz"])
            segments.append(Segment(ph["p_vaddr"], ph["p_filesz"], ph["p_memsz"], ph["p_flags"], data))
        gp = 0
        reginfo = elf.get_section_by_name(".reginfo")
        if reginfo is not None and reginfo["sh_size"] >= 24:
            gp = struct.unpack_from("<I", reginfo.data(), 20)[0]
        symbols, relocs = [], []
        symtab = elf.get_section_by_name(".symtab")
        if symtab is not None and symtab["sh_size"]:
            for s in symtab.iter_symbols():
                symbols.append(Symbol(s.name, s["st_value"], s["st_size"],
                                      _SYM_TYPES.get(s["st_info"]["type"], "other"),
                                      _SYM_BINDS.get(s["st_info"]["bind"], "other")))
        for sec in elf.iter_sections():
            if sec["sh_type"] == "SHT_REL":
                raw = sec.data()
                for off in range(0, len(raw) - 7, 8):
                    r_offset, r_info = struct.unpack_from("<II", raw, off)
                    relocs.append(Relocation(r_offset, r_info & 0xFF, r_info >> 8))
        return Image(path, elf.header["e_entry"], gp, segments, slots, symbols, relocs)


def _is_overlay_slot(elf, ph):
    """A PT_LOAD with no file bytes is an overlay slot when another such PT_LOAD shares its address."""
    same = [p for p in elf.iter_segments()
            if p["p_type"] == "PT_LOAD" and p["p_filesz"] == 0 and p["p_memsz"] and p["p_vaddr"] == ph["p_vaddr"]]
    return len(same) > 1
