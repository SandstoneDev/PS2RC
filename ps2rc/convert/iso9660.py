"""A small, read-only ISO9660 reader (no external tool): the volume descriptor, the directory tree with each file's
first sector (LSN) and size, and file contents by path.

Images: plain 2048-byte sectors (.iso), and raw 2352-byte CD sectors (mode 1 or mode 2 form 1, .bin) - detected from
the volume descriptor's position. Multi-extent files are joined. Names are ISO9660 level 1/2 names as written on the
disc ("SYSTEM.CNF;1"); paths here use '/' and no version suffix ("SYSTEM/IOPRP300.IMG"). A file name that ends in '.'
(no extension, "PAD.") keeps the dot in `path` (as the disc's own directory says) and loses it in `host_name`.

    with IsoImage("game.iso") as iso:
        cnf = iso.read("SYSTEM.CNF")
        for e in iso.files():               # sorted by LSN
            print(e.lsn, e.size, e.path)
"""
import os
import struct

SECTOR = 2048
RAW_SECTOR = 2352
_SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class IsoError(Exception):
    """The image is not a readable ISO9660 volume (or a path is missing)."""


class IsoEntry:
    __slots__ = ("path", "lsn", "size", "is_dir", "extents", "flags")

    def __init__(self, path, lsn, size, is_dir, extents=None, flags=0):
        self.path = path                  # "DIR/NAME.EXT" (no ";1")
        self.lsn = lsn                    # first sector
        self.size = size                  # bytes (sum of all extents)
        self.is_dir = is_dir
        self.extents = extents or [(lsn, size)]
        self.flags = flags

    @property
    def host_name(self):
        """The path as a file name on a PC / memory stick: a trailing '.' (no extension) dropped."""
        return "/".join(p[:-1] if p.endswith(".") else p for p in self.path.split("/"))

    def __repr__(self):
        return "IsoEntry(%r, lsn=%d, size=%d%s)" % (self.path, self.lsn, self.size, ", dir" if self.is_dir else "")


def _both32(b, off):
    return struct.unpack_from("<I", b, off)[0]


class IsoImage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self.image_size = os.fstat(self.f.fileno()).st_size
            self._detect()
            self._read_pvd()
        except Exception:
            self.f.close()
            raise
        self._tree = None

    # --- sectors -----------------------------------------------------------------------------------------------
    def _detect(self):
        """Sector layout: 2048-byte user data, or raw 2352-byte sectors (sync pattern) in mode 1 / mode 2 form 1."""
        self.f.seek(16 * SECTOR)
        if self.f.read(6)[1:6] == b"CD001":
            self.raw, self.data_off = False, 0
            return
        self.f.seek(16 * RAW_SECTOR)
        head = self.f.read(32)
        if head[:12] == _SYNC:
            mode = head[15]
            off = 16 if mode == 1 else 24
            if head[off + 1:off + 6] == b"CD001":
                self.raw, self.data_off = True, off
                return
        raise IsoError("%s: not an ISO9660 image (no volume descriptor at sector 16)" % self.path)

    def read_sectors(self, lsn, count=1):
        if count <= 0:
            return b""
        if not self.raw:
            self.f.seek(lsn * SECTOR)
            data = self.f.read(count * SECTOR)
        else:
            parts = []
            for i in range(count):
                self.f.seek((lsn + i) * RAW_SECTOR + self.data_off)
                parts.append(self.f.read(SECTOR))
            data = b"".join(parts)
        if len(data) < count * SECTOR:
            raise IsoError("%s: sectors %d-%d are past the end of the image (truncated dump?)"
                           % (self.path, lsn, lsn + count - 1))
        return data

    @property
    def sector_count(self):
        return self.image_size // (RAW_SECTOR if self.raw else SECTOR)

    # --- volume descriptor -------------------------------------------------------------------------------------
    def _read_pvd(self):
        lsn = 16
        self.pvd = None
        while lsn < 16 + 64:
            d = self.read_sectors(lsn)
            if d[1:6] != b"CD001":
                break
            if d[0] == 1 and self.pvd is None:
                self.pvd = d
            if d[0] == 255:
                break
            lsn += 1
        if self.pvd is None:
            raise IsoError("%s: no primary volume descriptor" % self.path)
        d = self.pvd
        self.system_id = d[8:40].decode("ascii", "replace").strip()
        self.volume_id = d[40:72].decode("ascii", "replace").strip()
        self.volume_space = _both32(d, 80)
        self.block_size = struct.unpack_from("<H", d, 128)[0]
        self.created = d[813:829].decode("ascii", "replace")
        if self.block_size != SECTOR:
            raise IsoError("%s: logical block size %d (only 2048 is supported)" % (self.path, self.block_size))
        self.root = self._record(d, 156)

    def layer_info(self):
        """A second ISO9660 volume after the first one's end = a dual-layer DVD dump with a separate file system on
        the second layer (some PS2 DVD-9 discs). Returns the second volume's sector or None."""
        lsn = self.volume_space + 16
        if lsn + 1 > self.sector_count:
            return None
        try:
            d = self.read_sectors(lsn)
        except IsoError:
            return None
        return lsn if d[1:6] == b"CD001" else None

    @staticmethod
    def _record(buf, o):
        ln = buf[o]
        lsn = _both32(buf, o + 2)
        size = _both32(buf, o + 10)
        flags = buf[o + 25]
        nl = buf[o + 32]
        name = buf[o + 33:o + 33 + nl]
        return ln, lsn, size, flags, name

    # --- directory tree ----------------------------------------------------------------------------------------
    def _dir_records(self, lsn, size):
        data = self.read_sectors(lsn, (size + SECTOR - 1) // SECTOR)[:size]
        o = 0
        while o < len(data):
            ln = data[o]
            if ln == 0:                                   # rest of this sector is padding
                o = (o // SECTOR + 1) * SECTOR
                continue
            if ln < 34 or o + ln > len(data):
                raise IsoError("%s: damaged directory record at sector %d" % (self.path, lsn + o // SECTOR))
            yield self._record(data, o)
            o += ln

    def _walk(self):
        out = {}
        seen = set()

        def walk(lsn, size, prefix):
            if lsn in seen:
                raise IsoError("%s: directory loop at sector %d" % (self.path, lsn))
            seen.add(lsn)
            pending = None                                # multi-extent file being collected
            for _ln, elsn, esz, flags, raw in self._dir_records(lsn, size):
                if raw in (b"\x00", b"\x01"):
                    continue
                name = raw.decode("latin-1")
                if ";" in name:
                    name = name[:name.index(";")]
                path = prefix + name
                if flags & 2:
                    out[path.upper()] = IsoEntry(path, elsn, esz, True, flags=flags)
                    walk(elsn, esz, path + "/")
                    continue
                if pending is not None and pending.path == path:
                    pending.extents.append((elsn, esz))
                    pending.size += esz
                else:
                    pending = IsoEntry(path, elsn, esz, False, flags=flags)
                    out[path.upper()] = pending
                if not flags & 0x80:                      # last (or only) extent
                    pending = None

        walk(self.root[1], self.root[2], "")
        return out

    @property
    def tree(self):
        if self._tree is None:
            self._tree = self._walk()
        return self._tree

    def files(self):
        """Every file, sorted by first sector."""
        return sorted((e for e in self.tree.values() if not e.is_dir), key=lambda e: (e.lsn, e.path))

    def find(self, path):
        """The entry of a path (any case, '\\' or '/', optional ';1', optional 'cdrom0:' prefix) or None."""
        p = path.replace("\\", "/")
        if ":" in p[:8]:
            p = p.split(":", 1)[1]
        p = p.strip("/")
        if ";" in p:
            p = p[:p.index(";")]
        e = self.tree.get(p.upper())
        if e is None and not p.endswith("."):
            e = self.tree.get(p.upper() + ".")
        return e

    def entry(self, path):
        e = self.find(path)
        if e is None or e.is_dir:
            raise IsoError("%s: no file %s on the disc" % (self.path, path))
        return e

    # --- contents ----------------------------------------------------------------------------------------------
    def iter_chunks(self, path, chunk=1 << 20):
        """The file's bytes in chunks (bounded memory for multi-GB files)."""
        e = self.entry(path) if not isinstance(path, IsoEntry) else path
        per = max(1, chunk // SECTOR)
        for lsn, size in e.extents:
            left = size
            cur = lsn
            while left > 0:
                n = min(per, (left + SECTOR - 1) // SECTOR)
                data = self.read_sectors(cur, n)
                take = min(left, len(data))
                yield data[:take]
                left -= take
                cur += n

    def read(self, path):
        return b"".join(self.iter_chunks(path))

    def copy_to(self, path, dst, progress=None):
        """Copy one file out; progress(bytes_done) after each chunk. Returns the size."""
        e = self.entry(path) if not isinstance(path, IsoEntry) else path
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        tmp = dst + ".part"
        done = 0
        with open(tmp, "wb") as out:
            for c in self.iter_chunks(e):
                out.write(c)
                done += len(c)
                if progress:
                    progress(done)
        os.replace(tmp, dst)
        return done

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def parse_system_cnf(data):
    """SYSTEM.CNF -> {KEY: value} (BOOT2, VER, VMODE, ...): 'KEY = value' lines, CR/LF, spaces around '='."""
    text = data.decode("latin-1")
    out = {}
    for line in text.replace("\r", "\n").split("\n"):
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip().upper()] = v.strip()
    return out


def boot_file(boot2):
    """'cdrom0:\\SLES_123.45;1' -> 'SLES_123.45' (the boot program's path on the disc, '/' separated)."""
    p = boot2.strip()
    if ":" in p[:8]:
        p = p.split(":", 1)[1]
    p = p.replace("\\", "/").strip("/")
    if ";" in p:
        p = p[:p.index(";")]
    return p


def serial_of(boot_name):
    """'SLES_123.45' -> 'SLES-12345' (the usual PS2 serial form); None when the name has no such shape."""
    base = boot_name.rsplit("/", 1)[-1].upper()
    if len(base) == 11 and base[4] == "_" and base[8] == "." and base[:4].isalpha() and \
            (base[5:8] + base[9:]).isdigit():
        return "%s-%s%s" % (base[:4], base[5:8], base[9:])
    return None
