"""The player's disc as the converter sees it: an ISO image (own ISO9660 reader) or an extracted disc directory plus
an LSN layout. Both give the same things: SYSTEM.CNF, the file list with sectors (LSN) and sizes, file contents,
the disc map the runtime reads (`discmap.txt`) and the ISO9660 metadata sectors before the first file
(`iso_meta.bin`: the game walks the file system itself through sector reads).

`iso_meta` modes:
    synthetic  rebuilt from the file list: volume descriptor, path tables and directories that agree with every file's
               real LSN and size (the layout the runtime has used since the first disc builds); the volume values
               (system id, volume id, volume size, date) come from the ISO's own descriptor, or from the pack for a
               directory input
    real       the image's own sectors 0..first file - 1 (ISO input only)
"""
import os
import re
import struct

from . import iso9660

SECTOR = 2048


class DiscError(Exception):
    pass


class DiscFile:
    __slots__ = ("lsn", "size", "iso", "rel")

    def __init__(self, lsn, size, iso, rel):
        self.lsn, self.size, self.iso, self.rel = lsn, size, iso, rel

    def row(self):
        return "%d %d %s %s" % (self.lsn, self.size, self.iso, self.rel)


class Disc:
    """Common interface. `files` sorted by LSN; `iso` = path on the disc without ';1'; `rel` = relative file name in
    the staged `disc/` folder (and in an extracted disc directory)."""
    kind = "?"
    label = ""
    files = ()
    volume = None          # dict(system_id, volume_id, volume_space, created) when known

    def find(self, iso_path):
        key = _key(iso_path)
        for f in self.files:
            if _key(f.iso) == key:
                return f
        return None

    def system_cnf(self):
        f = self.find("SYSTEM.CNF")
        if f is None:
            raise DiscError("%s: no SYSTEM.CNF - not a PS2 disc (or not its root folder)" % self.label)
        return iso9660.parse_system_cnf(self.read(f))

    def read(self, f):
        raise NotImplementedError

    def copy_to(self, f, dst, progress=None):
        raise NotImplementedError

    def iter_chunks(self, f, chunk=4 << 20):
        raise NotImplementedError

    def discmap_text(self):
        return "".join(f.row() + "\n" for f in self.files)

    def total_size(self):
        return sum(f.size for f in self.files)

    def close(self):
        pass


def _key(p):
    p = p.replace("\\", "/").strip("/")
    if ";" in p:
        p = p[:p.index(";")]
    return p.upper().rstrip(".")


class IsoDisc(Disc):
    kind = "iso"

    def __init__(self, path):
        self.path = path
        self.label = os.path.basename(path)
        self.iso = iso9660.IsoImage(path)
        second = self.iso.layer_info()
        if second is not None:
            self.iso.close()
            raise DiscError("%s: a second ISO9660 volume starts at sector %d (dual-layer disc with a separate file "
                            "system on layer 1) - not supported yet" % (self.label, second))
        self.files = [DiscFile(e.lsn, e.size, e.path, e.host_name) for e in self.iso.files()]
        self._entries = {f.iso: e for f, e in zip(self.files, self.iso.files())}
        self.volume = dict(system_id=self.iso.system_id, volume_id=self.iso.volume_id,
                           volume_space=self.iso.volume_space, created=self.iso.created)

    def read(self, f):
        return self.iso.read(self._entries[f.iso])

    def copy_to(self, f, dst, progress=None):
        return self.iso.copy_to(self._entries[f.iso], dst, progress)

    def iter_chunks(self, f, chunk=4 << 20):
        return self.iso.iter_chunks(self._entries[f.iso], chunk)

    def real_meta(self):
        first = min(f.lsn for f in self.files)
        return self.iso.read_sectors(0, first)

    def close(self):
        self.iso.close()


_LAYOUT_RE = re.compile(r"\s*(\d+)\s+[\d.]+MB\s+end=\s*(\d+)\s+(\S+?)(?:;1)?\s*$")


def read_layout(path):
    """An LSN layout file (`scripts/ps2_files/iso_dir.py` output: 'LSN  N.NMB  end=  E  PATH;1' lines, optional
    first line with the volume values) -> ([(lsn, iso_path)], volume dict or None)."""
    rows, volume = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = _LAYOUT_RE.match(line)
        if m:
            rows.append((int(m.group(1)), m.group(3)))
            continue
        if line.startswith("system_id="):
            vals = dict(re.findall(r"(\w+)=('[^']*'|\S+)", line))
            try:
                volume = dict(system_id=vals["system_id"].strip("'"), volume_id=vals["volume_id"].strip("'"),
                              volume_space=int(vals["volume_space_blocks"]), created=vals["created"][:16])
            except (KeyError, ValueError):
                volume = None
    return rows, volume


class DirDisc(Disc):
    """An extracted disc directory. A directory holds no sector numbers: they come from a layout file made from the
    ISO once (pack `[disc] layout`, or --layout)."""
    kind = "dir"

    def __init__(self, path, layout=None, volume=None, skip=None):
        self.path = os.path.abspath(path)
        self.label = os.path.basename(self.path.rstrip("\\/"))
        host = {}
        for dirpath, _dirs, names in os.walk(self.path):
            for n in names:
                rel = os.path.relpath(os.path.join(dirpath, n), self.path).replace("\\", "/")
                host[rel.upper()] = rel
        self.host = host
        if layout is None:
            raise DiscError("%s: a disc folder has no sector numbers; the game pack has no [disc] layout file and "
                            "no --layout was given (use the disc image instead)" % self.label)
        rows, lvol = read_layout(layout)
        if not rows:
            raise DiscError("%s: no file rows in the layout file %s" % (self.label, layout))
        self.volume = volume or lvol
        files = []
        for lsn, iso in rows:
            rel = host.get(iso.upper().rstrip("."))
            if rel is None:
                raise DiscError("%s: the disc file %s (layout) is missing in the folder" % (self.label, iso))
            files.append(DiscFile(lsn, os.path.getsize(os.path.join(self.path, rel)), iso, rel))
        files.sort(key=lambda f: (f.lsn, f.iso))
        self.files = files

    def _host(self, f):
        return os.path.join(self.path, f.rel.replace("/", os.sep))

    def read(self, f):
        with open(self._host(f), "rb") as h:
            return h.read()

    def copy_to(self, f, dst, progress=None):
        import shutil
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        shutil.copyfile(self._host(f), dst + ".part")
        os.replace(dst + ".part", dst)
        if progress:
            progress(f.size)
        return f.size

    def iter_chunks(self, f, chunk=4 << 20):
        with open(self._host(f), "rb") as h:
            while True:
                b = h.read(chunk)
                if not b:
                    break
                yield b

    def host_path(self, f):
        return self._host(f)


def open_disc(path, layout=None, volume=None):
    if os.path.isdir(path):
        return DirDisc(path, layout, volume)
    if not os.path.isfile(path):
        raise DiscError("%s: no such file or folder" % path)
    try:
        return IsoDisc(path)
    except iso9660.IsoError as e:
        raise DiscError(str(e))


# --- synthetic ISO9660 metadata (generic form of the disc map tool's metadata builder) -----------------------------

def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _date7(created):
    """'YYYYMMDDhhmmss..' -> the 7-byte directory record date (years since 1900, GMT offset 0)."""
    try:
        y, mo, d, h, mi, s = (int(created[0:4]), int(created[4:6]), int(created[6:8]), int(created[8:10]),
                              int(created[10:12]), int(created[12:14]))
        return bytes([y - 1900, mo, d, h, mi, s, 0])
    except (ValueError, IndexError):
        return bytes(7)


def _dirrec(name, lba, size, is_dir, date7):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    rec = bytearray(ln)
    rec[0] = ln
    rec[2:10] = _both32(lba)
    rec[10:18] = _both32(size)
    rec[18:25] = date7
    rec[25] = 2 if is_dir else 0
    rec[28:32] = _both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def synthetic_meta(files, volume):
    """Volume descriptor, path tables (sectors 18/19) and directory extents (from sector 20) for the sectors before the
    first file, consistent with every file's LSN and size. `volume`: system_id, volume_id, volume_space, created."""
    rows = [(f.lsn, f.size, f.iso) for f in files]
    first = min(r[0] for r in rows)
    date7 = _date7(volume.get("created", ""))
    dirs = {"": []}
    for lsn, size, iso in rows:
        parts = iso.split("/")
        for k in range(1, len(parts)):
            d = "/".join(parts[:k])
            if d not in dirs:
                dirs[d] = []
                dirs["/".join(parts[:k - 1])].append((parts[k - 1].encode("ascii"), d))
        dirs["/".join(parts[:-1])].append(((parts[-1] + ";1").encode("ascii"), (lsn, size)))
    order = sorted(dirs, key=lambda d: (d.count("/") if d else -1, d))

    def sectors_for(entries):
        used, nsec = 2 * 34, 1
        for name, _t in sorted(entries):
            ln = 33 + len(name) + (1 - len(name) % 2)
            if used + ln > SECTOR:
                nsec += 1
                used = 0
            used += ln
        return nsec
    lba, nxt = {}, 20
    for d in order:
        lba[d] = nxt
        nxt += sectors_for(dirs[d])
    if nxt > first:
        raise DiscError("the ISO9660 metadata does not fit before sector %d" % first)
    meta = bytearray(first * SECTOR)
    for d in order:
        parent = d.rsplit("/", 1)[0] if "/" in d else ""
        size = sectors_for(dirs[d]) * SECTOR
        psize = sectors_for(dirs[parent]) * SECTOR
        recs = [_dirrec(b"\x00", lba[d], size, True, date7), _dirrec(b"\x01", lba[parent], psize, True, date7)]
        for name, t in sorted(dirs[d]):
            if isinstance(t, str):
                recs.append(_dirrec(name, lba[t], sectors_for(dirs[t]) * SECTOR, True, date7))
            else:
                recs.append(_dirrec(name, t[0], t[1], False, date7))
        pos = lba[d] * SECTOR
        sec_end = pos + SECTOR
        for r in recs:
            if pos + len(r) > sec_end:
                pos = sec_end
                sec_end += SECTOR
            meta[pos:pos + len(r)] = r
            pos += len(r)
    num = {d: i + 1 for i, d in enumerate(order)}
    lpt, mpt = bytearray(), bytearray()
    for d in order:
        name = d.rsplit("/", 1)[-1].encode("ascii") if d else b"\x00"
        parent = num[d.rsplit("/", 1)[0] if "/" in d else ""]
        for buf, endian in ((lpt, "<"), (mpt, ">")):
            buf += bytes([len(name), 0]) + struct.pack(endian + "I", lba[d]) + struct.pack(endian + "H", parent) + name
            if len(name) % 2:
                buf += b"\x00"
    meta[18 * SECTOR:18 * SECTOR + len(lpt)] = lpt
    meta[19 * SECTOR:19 * SECTOR + len(mpt)] = mpt
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = volume.get("system_id", "").encode("ascii").ljust(32)[:32]
    pvd[40:72] = volume.get("volume_id", "").encode("ascii").ljust(32)[:32]
    pvd[80:88] = _both32(int(volume.get("volume_space", 0)))
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[132:140] = _both32(len(lpt))
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    pvd[156:190] = _dirrec(b"\x00", lba[""], sectors_for(dirs[""]) * SECTOR, True, date7)
    for off, ln in ((190, 128), (318, 128), (446, 128), (574, 128), (702, 37), (739, 37), (776, 37)):
        pvd[off:off + ln] = b" " * ln
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = b"0" * 16 + b"\x00"
    created = volume.get("created", "")
    if len(created) >= 16 and created[:16].isdigit():
        pvd[813:829] = created[:16].encode("ascii")
    pvd[881] = 1
    meta[16 * SECTOR:17 * SECTOR] = pvd
    meta[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    return bytes(meta)


def iso_meta(disc, mode="synthetic"):
    if mode == "real":
        if disc.kind != "iso":
            raise DiscError("iso_meta=real needs a disc image (a folder has no file-system sectors)")
        return disc.real_meta()
    if not disc.volume:
        raise DiscError("%s: the volume values (system id, volume id, size, date) are unknown: the layout file has "
                        "no 'system_id=' line and the pack has no [disc] volume" % disc.label)
    return synthetic_meta(disc.files, disc.volume)
