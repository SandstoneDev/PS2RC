"""PARAM.SFO: read, change and write the parameter file of a PSP application.

Layout (little-endian), as written by pspsdk's mksfoex and read by PPSSPP (Core/ELF/ParamSFO.cpp):

    header  4 bytes magic "\\0PSF" (00 50 53 46), u32 version 0x101, u32 key table offset,
            u32 data table offset, u32 entry count
    entry   (16 bytes each) u16 key offset (in the key table), u16 format, u32 used length,
            u32 reserved length, u32 data offset (in the data table)
    keys    zero-terminated ASCII names, the table padded with zeros to a multiple of 4
    data    each value in its reserved length (a multiple of 4), zero-filled

Formats: 0x0204 = UTF-8 text with a terminating zero (used length counts the zero), 0x0404 = u32,
0x0004 = UTF-8/bytes without a terminating zero (kept as bytes).

A file in this layout is rebuilt byte for byte: `Sfo.parse(b).build() == b` (see `standard`).
"""
import re
import struct

from .spec import PbpError, Issue, SFO_KEYS, TITLE_MAX, DISC_ID_MAX, PARENTAL_MAX

MAGIC = struct.unpack('<I', b'\0PSF')[0]
VERSION = 0x00000101
FMT_BYTES, FMT_STR, FMT_INT = 0x0004, 0x0204, 0x0404

# pspsdk mksfoex defaults, in its order (TITLE comes last, after any extra keys and these).
MKSFOEX_DEFAULTS = (('BOOTABLE', 1), ('CATEGORY', 'MG'), ('DISC_ID', 'UCJS10041'), ('DISC_VERSION', '1.00'),
                    ('PARENTAL_LEVEL', 1), ('PSP_SYSTEM_VER', '1.00'), ('REGION', 0x8000))


def _room(n):
    return (n + 3) & ~3


class Entry:
    __slots__ = ('key', 'fmt', 'value', 'reserved')

    def __init__(self, key, fmt, value, reserved):
        self.key, self.fmt, self.value, self.reserved = key, fmt, value, reserved

    def payload(self):
        if self.fmt == FMT_INT:
            return struct.pack('<I', self.value & 0xFFFFFFFF)
        if self.fmt == FMT_STR:
            return self.value.encode('utf-8') + b'\0'
        return bytes(self.value)

    def __repr__(self):
        return 'Entry(%r, 0x%04X, %r, %d)' % (self.key, self.fmt, self.value, self.reserved)


class Sfo:
    def __init__(self, entries=None):
        self.entries = list(entries or [])
        self.standard = True        # False when parse() met a layout build() would not reproduce

    # ------------------------------------------------------------------ reading
    @classmethod
    def parse(cls, data):
        data = bytes(data)
        if len(data) < 20:
            raise PbpError('PARAM.SFO: %d bytes, too short to be a parameter file' % len(data))
        magic, version, keyofs, valofs, count = struct.unpack('<5I', data[:20])
        if magic != MAGIC:
            raise PbpError('PARAM.SFO: wrong magic 0x%08X (a parameter file starts with 00 50 53 46)' % magic)
        if 20 + 16 * count > len(data) or keyofs > len(data) or valofs > len(data):
            raise PbpError('PARAM.SFO: %d entries / table offsets 0x%X 0x%X do not fit %d bytes' % (
                count, keyofs, valofs, len(data)))
        out = cls()
        for i in range(count):
            nameofs, fmt, used, reserved, dataofs = struct.unpack('<HHIII', data[20 + 16 * i:36 + 16 * i])
            k0 = keyofs + nameofs
            k1 = data.find(b'\0', k0)
            if k1 < 0 or k0 >= valofs:
                raise PbpError('PARAM.SFO: entry %d: key outside the key table' % i)
            key = data[k0:k1].decode('ascii', 'replace')
            d0 = valofs + dataofs
            if used > reserved or d0 + reserved > len(data):
                raise PbpError('PARAM.SFO: %s: value (%d of %d bytes at 0x%X) outside the file' % (
                    key, used, reserved, d0))
            raw = data[d0:d0 + used]
            if fmt == FMT_INT:
                if used != 4:
                    raise PbpError('PARAM.SFO: %s: integer of %d bytes' % (key, used))
                value = struct.unpack('<I', raw)[0]
            elif fmt == FMT_STR:
                value = raw.rstrip(b'\0').decode('utf-8', 'replace')
            else:
                value = raw
            out.entries.append(Entry(key, fmt, value, reserved))
        try:
            out.standard = (version == VERSION and out.build() == data)
        except PbpError:
            out.standard = False
        return out

    # ------------------------------------------------------------------ writing
    def build(self):
        keys, data, index = bytearray(), bytearray(), bytearray()
        for e in self.entries:
            p = e.payload()
            if len(p) > e.reserved:
                raise PbpError('PARAM.SFO: %s: %d bytes do not fit the %d reserved' % (e.key, len(p), e.reserved))
            index += struct.pack('<HHIII', len(keys), e.fmt, len(p), e.reserved, len(data))
            keys += e.key.encode('ascii') + b'\0'
            data += p + b'\0' * (e.reserved - len(p))
        keys += b'\0' * (_room(len(keys)) - len(keys))
        keyofs = 20 + len(index)
        head = struct.pack('<5I', MAGIC, VERSION, keyofs, keyofs + len(keys), len(self.entries))
        return head + bytes(index) + bytes(keys) + bytes(data)

    # ------------------------------------------------------------------ access
    def find(self, key):
        for e in self.entries:
            if e.key == key:
                return e
        return None

    def get(self, key, default=None):
        e = self.find(key)
        return default if e is None else e.value

    def __contains__(self, key):
        return self.find(key) is not None

    def keys(self):
        return [e.key for e in self.entries]

    def to_dict(self):
        return {e.key: e.value for e in self.entries}

    def set(self, key, value, reserved=None):
        """set a str (format 0x0204) or int (0x0404) value; an existing key keeps its place and its reserved room
        when the new value fits, a new key is appended (as mksfoex does)."""
        if not re.fullmatch(r'[A-Z0-9_]{1,64}', key):
            raise PbpError('PARAM.SFO: key %r: use A-Z, 0-9 and _ only' % key)
        if isinstance(value, bool) or not isinstance(value, (int, str)):
            raise PbpError('PARAM.SFO: %s: value must be text or an integer, not %r' % (key, value))
        fmt = FMT_INT if isinstance(value, int) else FMT_STR
        if fmt == FMT_INT and not 0 <= value <= 0xFFFFFFFF:
            raise PbpError('PARAM.SFO: %s: %d does not fit 32 bits' % (key, value))
        need = 4 if fmt == FMT_INT else len(value.encode('utf-8')) + 1
        e = self.find(key)
        if reserved is None:
            if e is not None and e.fmt == fmt and need <= e.reserved:
                reserved = e.reserved
            else:
                reserved = _room(need)
        if need > reserved:
            raise PbpError('PARAM.SFO: %s: %d bytes do not fit the %d reserved' % (key, need, reserved))
        if e is None:
            self.entries.append(Entry(key, fmt, value, reserved))
        else:
            e.fmt, e.value, e.reserved = fmt, value, reserved

    def remove(self, key):
        self.entries = [e for e in self.entries if e.key != key]

    # ------------------------------------------------------------------ checks
    def validate(self):
        out = []
        title = self.get('TITLE')
        if title is None:
            out.append(Issue('error', 'PARAM.SFO', 'no TITLE'))
        elif isinstance(title, str):
            n = len(title.encode('utf-8'))
            if n == 0 or n > TITLE_MAX:
                out.append(Issue('error', 'PARAM.SFO', 'TITLE is %d bytes of UTF-8, must be 1..%d' % (n, TITLE_MAX)))
            if re.search(r'[\x00-\x1f\x7f]', title):
                out.append(Issue('spec', 'PARAM.SFO', 'TITLE contains line breaks or control characters'))
        did = self.get('DISC_ID')
        if did is None:
            out.append(Issue('spec', 'PARAM.SFO', 'no DISC_ID'))
        elif isinstance(did, str) and not re.fullmatch(r'[A-Z0-9]{1,%d}' % DISC_ID_MAX, did):
            out.append(Issue('spec', 'PARAM.SFO', 'DISC_ID %r: use 1..%d characters A-Z and 0-9 (no hyphen)' % (
                did, DISC_ID_MAX)))
        pl = self.get('PARENTAL_LEVEL')
        if isinstance(pl, int) and not 0 <= pl <= PARENTAL_MAX:
            out.append(Issue('spec', 'PARAM.SFO', 'PARENTAL_LEVEL %d, must be 0..%d' % (pl, PARENTAL_MAX)))
        for key, rx, form in (('DISC_VERSION', r'\d\.\d\d', 'X.XX'), ('PSP_SYSTEM_VER', r'\d\.\d\d', 'X.XX'),
                              ('APP_VER', r'\d\d\.\d\d', 'XX.XX')):
            v = self.get(key)
            if isinstance(v, str) and not re.fullmatch(rx, v):
                out.append(Issue('warn', 'PARAM.SFO', '%s %r, expected the form %s' % (key, v, form)))
        mem = self.get('MEMSIZE')
        if mem is not None and mem not in (0, 1):
            out.append(Issue('warn', 'PARAM.SFO', 'MEMSIZE %r (known values: 0, 1)' % (mem,)))
        if self.get('BOOTABLE') not in (None, 1):
            out.append(Issue('warn', 'PARAM.SFO', 'BOOTABLE %r: the menu may not start it' % (self.get('BOOTABLE'),)))
        cat = self.get('CATEGORY')
        if cat is not None and cat != 'MG':
            out.append(Issue('info', 'PARAM.SFO', 'CATEGORY %r (pspsdk homebrew uses "MG")' % (cat,)))
        for e in self.entries:
            known = SFO_KEYS.get(e.key)
            if known and (known[0] == 'int') != (e.fmt == FMT_INT):
                out.append(Issue('warn', 'PARAM.SFO', '%s stored as format 0x%04X, usually %s' % (
                    e.key, e.fmt, 'an integer' if known[0] == 'int' else 'text')))
        if not self.standard:
            out.append(Issue('info', 'PARAM.SFO', 'non-standard layout: a change rewrites it in the standard one'))
        return out


def make(title, extra=(), disc_id=None):
    """a new PARAM.SFO the way pspsdk's `mksfoex [-d/-s extra...] TITLE` writes it: the extra keys first, then the
    defaults not given, then TITLE (text values reserve their length rounded up to 4)."""
    s = Sfo()
    given = set()
    for key, value in extra:
        s.set(key, value)
        given.add(key)
    for key, value in MKSFOEX_DEFAULTS:
        if key not in given:
            s.set(key, disc_id if (key == 'DISC_ID' and disc_id) else value)
    if 'TITLE' not in given:
        s.set('TITLE', title)
    return s
