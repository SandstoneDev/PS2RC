"""Metrowerks CodeWarrior static code overlays ('MWo3' overlay files of PS2 programs).

Layout (0x40-byte header, loaded together with the payload at load_addr):
  0x00 'MWo3' | 0x04 u32 id (1-based overlay slot) | 0x08 u32 load address | 0x0C u32 text size | 0x10 u32 data size
  0x14 u32 bss size | 0x18 u32 ctor table start | 0x1C u32 ctor table end | 0x20 char name[32]
File size = 0x40 + text + data; memory size = 0x40 + text + data + bss (== ELF overlay-slot memsz).
"""
import struct
from dataclasses import dataclass

HEADER_SIZE = 0x40
MAGIC = b"MWo3"


class OverlayError(Exception):
    pass


@dataclass
class Overlay:
    path: str
    id: int
    load_addr: int
    text_size: int
    data_size: int
    bss_size: int
    ctor_start: int
    ctor_end: int
    name: str
    raw: bytes      # file bytes (header + text + data); mapped at load_addr

    @property
    def memsz(self):
        return HEADER_SIZE + self.text_size + self.data_size + self.bss_size

    @property
    def text_addr(self):
        return self.load_addr + HEADER_SIZE

    def memory(self):
        """Bytes of the overlay window after loading (bss zero-filled)."""
        return self.raw + bytes(self.bss_size)


def load_overlay(path):
    raw = open(path, "rb").read()
    if len(raw) < HEADER_SIZE or raw[:4] != MAGIC:
        raise OverlayError("%s: not an MWo3 overlay" % path)
    ov_id, load, text, data, bss, c0, c1 = struct.unpack_from("<7I", raw, 4)
    name = raw[0x20:0x40].split(b"\0", 1)[0].decode("ascii", "replace")
    if len(raw) != HEADER_SIZE + text + data:
        raise OverlayError("%s: size %d != header+text+data %d" % (path, len(raw), HEADER_SIZE + text + data))
    return Overlay(path, ov_id, load, text, data, bss, c0, c1, name, raw)
