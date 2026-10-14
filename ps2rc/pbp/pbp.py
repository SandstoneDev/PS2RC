"""EBOOT.PBP: the PSP application container.

Layout (little-endian), as pspsdk's pack-pbp writes it and PPSSPP reads it (Core/ELF/PBPReader.cpp):

    0x00  4 bytes  magic "\\0PBP"
    0x04  4 bytes  version (pack-pbp writes 00 00 01 00)
    0x08  8 x u32  offsets of PARAM.SFO, ICON0.PNG, ICON1.PMF, PIC0.PNG, PIC1.PNG, SND0.AT3, DATA.PSP, DATA.PSAR
    0x28  the files, back to back in that order, no padding

A file's size is the next offset minus its own (the last one runs to the end of the PBP). A missing file has size 0:
its offset equals the next one. pack-pbp places the first file right after the 40-byte header.
"""
import os
import struct

from .spec import PbpError, SLOTS

MAGIC = b'\0PBP'
VERSION = b'\x00\x00\x01\x00'
HEADER = 40


class Pbp:
    def __init__(self, files=None, version=VERSION):
        self.files = {name: b'' for name in SLOTS}
        for name, data in (files or {}).items():
            self[name] = data
        self.version = bytes(version)
        self.first_offset = HEADER     # where the first file starts (pack-pbp: right after the header)
        self._gap = b''                # bytes between the header and the first file (kept when re-packing)

    def __getitem__(self, name):
        return self.files[self._slot(name)]

    def __setitem__(self, name, data):
        self.files[self._slot(name)] = bytes(data or b'')

    @staticmethod
    def _slot(name):
        if name not in SLOTS:
            raise PbpError('unknown PBP file %r (known: %s)' % (name, ', '.join(SLOTS)))
        return name

    # ------------------------------------------------------------------ reading
    @classmethod
    def parse(cls, data, what='EBOOT.PBP'):
        data = bytes(data)
        if len(data) < HEADER or data[:4] != MAGIC:
            hint = ' (this is a plain ELF; it has no PBP header)' if data[:4] == b'\x7fELF' else ''
            raise PbpError('%s: not a PBP file: it must start with 00 50 42 50%s' % (what, hint))
        offs = list(struct.unpack('<8I', data[8:HEADER])) + [len(data)]
        if offs[0] < HEADER:
            raise PbpError('%s: the first file starts at %d, inside the 40-byte header' % (what, offs[0]))
        for i in range(8):
            if not offs[i] <= offs[i + 1] <= len(data):
                raise PbpError('%s: the offset of %s (%d) and of the next file (%d) are out of order or past the '
                               'end (%d bytes): the file is damaged' % (what, SLOTS[i], offs[i], offs[i + 1],
                                                                        len(data)))
        p = cls(version=data[4:8])
        for i, name in enumerate(SLOTS):
            p.files[name] = data[offs[i]:offs[i + 1]]
        p.first_offset = offs[0]
        p._gap = data[HEADER:offs[0]]
        return p

    @classmethod
    def read(cls, path):
        try:
            with open(path, 'rb') as f:
                data = f.read()
        except OSError as e:
            raise PbpError('cannot read %s (%s)' % (path, e.strerror or e))
        return cls.parse(data, os.path.basename(str(path)))

    # ------------------------------------------------------------------ writing
    def offsets(self):
        o, out = self.first_offset, []
        for name in SLOTS:
            out.append(o)
            o += len(self.files[name])
        return out

    def build(self):
        gap = self._gap
        if len(gap) != self.first_offset - HEADER:
            gap = b'\0' * (self.first_offset - HEADER)
        head = MAGIC + self.version + struct.pack('<8I', *self.offsets())
        return head + gap + b''.join(self.files[name] for name in SLOTS)

    def write(self, path):
        data = self.build()
        tmp = str(path) + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(data)
        os.replace(tmp, path)
        return len(data)

    # ------------------------------------------------------------------ helpers
    def unpack(self, out_dir):
        """write every non-empty file under its name; -> list of written names"""
        os.makedirs(out_dir, exist_ok=True)
        written = []
        for name in SLOTS:
            if self.files[name]:
                with open(os.path.join(out_dir, name), 'wb') as f:
                    f.write(self.files[name])
                written.append(name)
        return written

    @classmethod
    def from_dir(cls, src_dir):
        """the files of `unpack` back into a PBP (missing names stay empty)"""
        p = cls()
        for name in SLOTS:
            path = os.path.join(src_dir, name)
            if os.path.isfile(path):
                with open(path, 'rb') as f:
                    p.files[name] = f.read()
        return p


def module_kind(data):
    """what DATA.PSP holds, from its first bytes"""
    if not data:
        return 'none'
    if data[:4] == b'\x7fELF':
        return 'ELF (not encrypted)'
    if data[:4] == b'~PSP':
        return 'packed module ("~PSP" header)'
    return 'unknown (%s)' % data[:4].hex()
