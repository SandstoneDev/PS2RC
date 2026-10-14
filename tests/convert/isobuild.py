"""A tiny ISO9660 image writer for the tests (independent of ps2rc.convert): a primary volume descriptor, path tables,
one sector per directory, files laid out from a given first sector, optional multi-extent files and raw 2352-byte
sectors (CD mode 1 / mode 2 form 1)."""
import struct

S = 2048


def b16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def b32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def rec(name, lsn, size, is_dir, flags=0):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    r = bytearray(ln)
    r[0] = ln
    r[2:10] = b32(lsn)
    r[10:18] = b32(size)
    r[18:25] = bytes([120, 1, 2, 3, 4, 5, 0])
    r[25] = (2 if is_dir else 0) | flags
    r[28:32] = b16(1)
    r[32] = n
    r[33:33 + n] = name
    return bytes(r)


def build_iso(files, volume_id="TESTVOL", system_id="TESTSYS", first_lsn=None, multi_extent=None, raw=None,
              created="2001020304050600", pad_sectors=0):
    """files: {"DIR/NAME.EXT": bytes} (upper case; a name ending in '.' has no extension). Returns the image bytes.
    multi_extent: {path: extent bytes (multiple of 2048)}. raw: None, 'mode1' or 'mode2'."""
    multi_extent = multi_extent or {}
    dirs = {"": []}
    for p in files:
        parts = p.split("/")
        for k in range(1, len(parts)):
            d = "/".join(parts[:k])
            if d not in dirs:
                dirs[d] = []
                dirs["/".join(parts[:k - 1])].append(("dir", parts[k - 1], d))
        dirs["/".join(parts[:-1])].append(("file", parts[-1], p))
    order = sorted(dirs, key=lambda d: (d.count("/") if d else -1, d))
    dlsn = {d: 20 + i for i, d in enumerate(order)}
    lsn = first_lsn or (20 + len(order) + 2)
    flsn = {}
    for p in sorted(files):
        flsn[p] = lsn
        lsn += max(1, (len(files[p]) + S - 1) // S)
    total = lsn + pad_sectors
    img = bytearray(total * S)
    for d in order:
        parent = d.rsplit("/", 1)[0] if "/" in d else ""
        recs = [rec(b"\x00", dlsn[d], S, True), rec(b"\x01", dlsn[parent], S, True)]
        for kind, name, ref in sorted(dirs[d], key=lambda t: t[1]):
            if kind == "dir":
                recs.append(rec(name.encode(), dlsn[ref], S, True))
            else:
                data = files[ref]
                ext = multi_extent.get(ref)
                nm = (name + ";1").encode()
                if ext:
                    off, start = 0, flsn[ref]
                    while off < len(data):
                        part = min(ext, len(data) - off)
                        last = off + part >= len(data)
                        recs.append(rec(nm, start + off // S, part, False, 0 if last else 0x80))
                        off += part
                else:
                    recs.append(rec(nm, flsn[ref], len(data), False))
        blob = b"".join(recs)
        assert len(blob) <= S, "test directory too big for one sector"
        img[dlsn[d] * S:dlsn[d] * S + len(blob)] = blob
    for p, data in files.items():
        img[flsn[p] * S:flsn[p] * S + len(data)] = data
    pvd = bytearray(S)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = system_id.encode().ljust(32)
    pvd[40:72] = volume_id.encode().ljust(32)
    pvd[80:88] = b32(total)
    pvd[120:124] = b16(1)
    pvd[124:128] = b16(1)
    pvd[128:132] = b16(S)
    pvd[156:190] = rec(b"\x00", dlsn[""], S, True)
    pvd[813:829] = created.encode()
    pvd[881] = 1
    img[16 * S:17 * S] = pvd
    img[17 * S:17 * S + 7] = b"\xffCD001\x01"
    if raw is None:
        return bytes(img)
    out = bytearray()
    for i in range(total):
        sync = b"\x00" + b"\xff" * 10 + b"\x00"
        m, s, f = (i + 150) // 4500, ((i + 150) // 75) % 60, (i + 150) % 75
        bcd = lambda v: ((v // 10) << 4) | (v % 10)  # noqa: E731
        head = bytes([bcd(m), bcd(s), bcd(f), 1 if raw == "mode1" else 2])
        body = img[i * S:(i + 1) * S]
        if raw == "mode1":
            sec = sync + head + body + bytes(288)
        else:
            sec = sync + head + bytes(8) + body + bytes(280)
        assert len(sec) == 2352
        out += sec
    return bytes(out)


def ps2_files(boot="SLES_123.45", elf=b"\x7fELF" + bytes(4000), extra=None):
    """A minimal PS2-like disc: SYSTEM.CNF + boot program + a few data files."""
    files = {"SYSTEM.CNF": ("BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = PAL\r\n" % boot).encode(),
             boot: elf,
             "DATA/A.DAT": b"a" * 5000,
             "DATA/SUB/B.DAT": b"b" * 10,
             "MODELS/X.IMG": b"x" * 9000,
             "MODELS/Y.TEX": b"y" * 100,
             "AUDIO/PAD.": b"\0" * 2048,
             "AUDIO/SFX/GEN01.PAK": b"g" * 3000,
             "AUDIO/SFX/GEN02.PAK": b"h" * 3000,
             "CONFIG.INI": b"memory\t\t13550\r\nother 1\r\n"}
    files.update(extra or {})
    return files
