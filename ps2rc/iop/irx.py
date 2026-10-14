"""IRX (PS2 IOP relocatable module) loader.

An IRX is an ELF32 little-endian MIPS file of type 0xFF80 with a `.iopmod` section (module info: entry point, gp,
text/data/bss sizes, version, name), one loadable segment that starts at address 0 and REL relocations
(R_MIPS_32 / R_MIPS_26 / R_MIPS_HI16 / R_MIPS_LO16) relative to that address. Calls into other IOP modules go through
import stub tables inside .text (magic word 0x41E0 in the upper half, version, 8-character library name, then one
`jr ra; addiu zero, zero, N` pair per imported function number N, terminated by two zero words); a module's own
exports use the magic 0x41C0 in the upper half. All of this is the documented IOP module format (open-source PS2 SDKs describe it).

`load(path_or_bytes, base)` returns an `Irx`: the relocated image placed at `base`, sections, relocation records,
imports and exports. Nothing here knows any particular game.
"""
import hashlib
import struct
from dataclasses import dataclass, field

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
IMPORT_MAGIC = 0x41E0 << 16
EXPORT_MAGIC = 0x41C0 << 16
JR_RA = (31 << 21) | 8


class IrxError(Exception):
    pass


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int


@dataclass
class Reloc:
    offset: int        # module-relative address of the relocated word
    type: int


@dataclass
class ImportFn:
    stub: int          # module-relative address of the stub (jr ra)
    lib: str
    num: int


@dataclass
class Import:
    lib: str
    version: int
    table: int         # module-relative address of the table
    fns: list = field(default_factory=list)


@dataclass
class Export:
    lib: str
    version: int
    table: int
    fns: list = field(default_factory=list)     # module-relative entry addresses (index = function number)


@dataclass
class Irx:
    name: str
    version: int
    entry: int          # module-relative
    gp: int             # module-relative
    text_size: int
    data_size: int
    bss_size: int
    image_size: int     # text + data + bss
    base: int
    image: bytearray    # relocated, `image_size` bytes, byte 0 = address `base`
    sections: list
    relocs: list
    imports: list
    exports: list
    md5: str

    # ---------------------------------------------------------------- helpers (absolute addresses)
    def word(self, addr):
        o = addr - self.base
        return struct.unpack_from("<I", self.image, o)[0]

    def in_text(self, addr):
        return self.base <= addr < self.base + self.text_size

    def in_image(self, addr):
        return self.base <= addr < self.base + self.image_size

    def stubs(self):
        """{absolute stub address: ImportFn}"""
        return {self.base + f.stub: f for imp in self.imports for f in imp.fns}

    def import_table_ranges(self):
        """[(start, end)) absolute ranges of the import/export tables inside .text (not code)."""
        out = []
        for imp in self.imports:
            out.append((self.base + imp.table, self.base + imp.table + 20 + 8 * len(imp.fns) + 8))
        for ex in self.exports:
            out.append((self.base + ex.table, self.base + ex.table + 20 + 4 * len(ex.fns) + 4))
        return out


def _cstr(b):
    n = b.find(b"\0")
    return (b if n < 0 else b[:n]).decode("latin-1")


def load(src, base=0):
    data = src if isinstance(src, (bytes, bytearray)) else open(src, "rb").read()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise IrxError("not a 32-bit little-endian ELF file")
    (etype, mach, _ver, _entry, phoff, shoff, _flags, _ehs, phes, phnum, shes, shnum,
     shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if etype != 0xFF80:
        raise IrxError("ELF type 0x%x is not an IRX (0xff80)" % etype)
    if mach != 8:
        raise IrxError("not a MIPS file")
    secs = []
    for i in range(shnum):
        nm, ty, fl, ad, of, sz, lk, inf, _al, _es = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shes)
        secs.append([nm, ty, fl, ad, of, sz, lk, inf])
    strtab = secs[shstrndx]
    sections = [Section(_cstr(data[strtab[4] + s[0]:]), *s[1:]) for s in secs]
    mod = next((s for s in sections if s.type == 0x70000080), None)
    if mod is None:
        raise IrxError("no .iopmod section")
    (_mi, entry, gp, tsz, dsz, bsz, ver) = struct.unpack_from("<IIIIIIH", data, mod.offset)
    name = _cstr(data[mod.offset + 26:mod.offset + mod.size])
    # the loadable segment
    seg = None
    for i in range(phnum):
        ty, of, va, _pa, fsz, msz, _fl, _al = struct.unpack_from("<IIIIIIII", data, phoff + i * phes)
        if ty == 1:
            seg = (of, va, fsz, msz)
    if seg is None or seg[1] != 0:
        raise IrxError("no loadable segment at address 0")
    of, _va, fsz, msz = seg
    if msz != tsz + dsz + bsz:
        raise IrxError("segment size 0x%x != text+data+bss 0x%x" % (msz, tsz + dsz + bsz))
    image = bytearray(msz)
    image[:fsz] = data[of:of + fsz]
    # relocations (module-relative addresses), applied for `base`
    relocs = []
    for s in sections:
        if s.type != 9:
            continue
        for k in range(s.size // 8):
            r_off, r_info = struct.unpack_from("<II", data, s.offset + 8 * k)
            relocs.append(Reloc(r_off, r_info & 0xFF))
    _apply(image, relocs, base)
    irx = Irx(name, ver, entry, gp, tsz, dsz, bsz, msz, base, image, sections, relocs, [], [],
              hashlib.md5(bytes(data)).hexdigest())
    _tables(irx)
    return irx


def _apply(image, relocs, base):
    if base == 0:
        return
    pend_hi = []
    for r in relocs:
        w = struct.unpack_from("<I", image, r.offset)[0]
        if r.type == R_MIPS_32:
            w = (w + base) & 0xFFFFFFFF
        elif r.type == R_MIPS_26:
            t = ((w & 0x3FFFFFF) << 2) + base
            w = (w & 0xFC000000) | ((t >> 2) & 0x3FFFFFF)
        elif r.type == R_MIPS_HI16:
            pend_hi.append(r.offset)
            continue
        elif r.type == R_MIPS_LO16:
            lo = w & 0xFFFF
            lo_s = lo - 0x10000 if lo & 0x8000 else lo
            for h in pend_hi:
                hw = struct.unpack_from("<I", image, h)[0]
                ahl = ((hw & 0xFFFF) << 16) + lo_s + base
                hw = (hw & 0xFFFF0000) | (((ahl + 0x8000) >> 16) & 0xFFFF)
                struct.pack_into("<I", image, h, hw)
            pend_hi = []
            w = (w & 0xFFFF0000) | ((lo + base) & 0xFFFF)
        else:
            raise IrxError("relocation type %d at 0x%x not supported" % (r.type, r.offset))
        struct.pack_into("<I", image, r.offset, w)
    if pend_hi:
        raise IrxError("R_MIPS_HI16 without a following R_MIPS_LO16")


def _tables(irx):
    t = irx.image
    n = irx.text_size
    w = lambda o: struct.unpack_from("<I", t, o)[0]
    i = 0
    while i + 20 <= n:
        m = w(i)
        if m == IMPORT_MAGIC and w(i + 4) == 0:
            imp = Import(_cstr(bytes(t[i + 12:i + 20])), w(i + 8) & 0xFFFF, i)
            j = i + 20
            while j + 8 <= n and w(j) != 0:
                if w(j) != JR_RA or (w(j + 4) >> 16) != 0x2400:
                    raise IrxError("malformed import stub at 0x%x" % j)
                imp.fns.append(ImportFn(j, imp.lib, w(j + 4) & 0xFFFF))
                j += 8
            irx.imports.append(imp)
            i = j + 8
            continue
        if m == EXPORT_MAGIC and w(i + 4) == 0:
            ex = Export(_cstr(bytes(t[i + 12:i + 20])), w(i + 8) & 0xFFFF, i)
            j = i + 20
            while j + 4 <= len(t) and w(j) != 0:
                ex.fns.append(w(j) - irx.base)
                j += 4
            irx.exports.append(ex)
            i = j + 4
            continue
        i += 4
