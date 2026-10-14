"""ISO9660 reader, disc map and metadata sectors (synthetic images only)."""
import os

import pytest

from isobuild import build_iso, ps2_files
from ps2rc.convert import disc as discmod
from ps2rc.convert import iso9660


def _write(tmp_path, data, name="game.iso"):
    p = tmp_path / name
    p.write_bytes(data)
    return str(p)


@pytest.mark.parametrize("raw", [None, "mode1", "mode2"])
def test_reader_lists_and_reads(tmp_path, raw):
    files = ps2_files()
    path = _write(tmp_path, build_iso(files, raw=raw), "g.bin" if raw else "g.iso")
    with iso9660.IsoImage(path) as iso:
        assert iso.raw == (raw is not None)
        assert iso.volume_id == "TESTVOL" and iso.system_id == "TESTSYS"
        got = {e.path: e for e in iso.files()}
        assert set(got) == set(files)
        lsns = [e.lsn for e in iso.files()]
        assert lsns == sorted(lsns)
        for p, data in files.items():
            assert got[p].size == len(data)
            assert iso.read(p) == data
        assert got["AUDIO/PAD."].host_name == "AUDIO/PAD"
        assert iso.find("cdrom0:\\data\\sub\\b.dat;1").path == "DATA/SUB/B.DAT"
        assert iso.find("audio/pad").path == "AUDIO/PAD."
        assert iso.find("NOPE.BIN") is None
        assert iso.find("DATA").is_dir


def test_multi_extent_and_chunks(tmp_path):
    big = bytes(range(256)) * 100          # 25,600 bytes in 3 extents of 10,240
    files = {"SYSTEM.CNF": b"BOOT2 = cdrom0:\\SLES_123.45;1\r\n", "SLES_123.45": b"E" * 10, "BIG.BIN": big}
    path = _write(tmp_path, build_iso(files, multi_extent={"BIG.BIN": 5 * 2048}))
    with iso9660.IsoImage(path) as iso:
        e = iso.entry("BIG.BIN")
        assert len(e.extents) == 3 and e.size == len(big)
        assert iso.read("BIG.BIN") == big
        assert b"".join(iso.iter_chunks("BIG.BIN", chunk=4096)) == big
        out = str(tmp_path / "x" / "big.bin")
        seen = []
        assert iso.copy_to("BIG.BIN", out, seen.append) == len(big)
        assert open(out, "rb").read() == big and seen[-1] == len(big)
        assert not os.path.exists(out + ".part")


def test_not_iso_and_truncated(tmp_path):
    p = _write(tmp_path, b"\0" * 40000)
    with pytest.raises(iso9660.IsoError):
        iso9660.IsoImage(p)
    img = build_iso(ps2_files())
    p2 = _write(tmp_path, img[:-6000], "cut.iso")        # last file cut off
    with iso9660.IsoImage(p2) as iso:
        last = iso.files()[-1]
        with pytest.raises(iso9660.IsoError):
            iso.read(last.path)


def test_system_cnf_helpers():
    cnf = iso9660.parse_system_cnf(b"BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.10\r\nVMODE = NTSC\r\n")
    assert cnf == {"BOOT2": "cdrom0:\\SLUS_123.45;1", "VER": "1.10", "VMODE": "NTSC"}
    assert iso9660.boot_file(cnf["BOOT2"]) == "SLUS_123.45"
    assert iso9660.boot_file("cdrom0:\\DIR\\GAME.ELF;1") == "DIR/GAME.ELF"
    assert iso9660.serial_of("SLUS_123.45") == "SLUS-12345"
    assert iso9660.serial_of("GAME.ELF") is None


def test_discmap_and_dual_layer_refusal(tmp_path):
    path = _write(tmp_path, build_iso(ps2_files(), pad_sectors=40))
    d = discmod.open_disc(path)
    rows = [ln.split() for ln in d.discmap_text().splitlines()]
    assert [int(r[0]) for r in rows] == sorted(int(r[0]) for r in rows)
    assert ["AUDIO/PAD.", "AUDIO/PAD"] in [r[2:] for r in rows]
    assert d.system_cnf()["BOOT2"].endswith("SLES_123.45;1")
    d.close()
    # a second volume descriptor right after the first volume = dual layer with its own file system
    img = bytearray(build_iso(ps2_files(), pad_sectors=40))
    vs = int.from_bytes(img[16 * 2048 + 80:16 * 2048 + 84], "little")
    img += bytes(20 * 2048)
    img[(vs + 16) * 2048:(vs + 16) * 2048 + 6] = b"\x01CD001"
    with pytest.raises(discmod.DiscError, match="dual-layer"):
        discmod.open_disc(_write(tmp_path, bytes(img), "dl.iso"))


def test_synthetic_meta_is_a_valid_file_system(tmp_path):
    """The rebuilt metadata + the files at their real sectors must read back as the same disc."""
    files = ps2_files()
    src = _write(tmp_path, build_iso(files, first_lsn=60))
    d = discmod.open_disc(src)
    meta = discmod.iso_meta(d, "synthetic")
    assert len(meta) == 60 * 2048
    img = bytearray(open(src, "rb").read())
    img[:len(meta)] = meta
    p2 = _write(tmp_path, bytes(img), "rebuilt.iso")
    with iso9660.IsoImage(p2) as iso:
        assert [(e.lsn, e.size, e.path) for e in iso.files()] == [(f.lsn, f.size, f.iso) for f in d.files]
        assert iso.volume_id == "TESTVOL" and iso.created == "2001020304050600"
        for p, data in files.items():
            assert iso.read(p) == data
    real = discmod.iso_meta(d, "real")
    assert real == open(src, "rb").read()[:60 * 2048]
    d.close()


def test_dir_disc_with_layout(tmp_path):
    files = ps2_files()
    img_path = _write(tmp_path, build_iso(files, first_lsn=60))
    d_iso = discmod.open_disc(img_path)
    root = tmp_path / "disc"
    for p, data in files.items():
        host = root / p.rstrip(".").lower()                 # extracted with other letter case, no trailing dot
        host.parent.mkdir(parents=True, exist_ok=True)
        host.write_bytes(data)
    (root / "notes.txt").write_bytes(b"not on the disc")
    layout = tmp_path / "layout.txt"
    with open(layout, "w") as f:
        f.write("system_id='TESTSYS' volume_id='TESTVOL' volume_space_blocks=%d (0 bytes) created=2001020304050600$\n"
                % d_iso.volume["volume_space"])
        for e in d_iso.files:
            f.write("%8d  %9.1fMB  end=%8d  %s;1\n" % (e.lsn, e.size / 1048576, e.lsn + 1, e.iso))
    d = discmod.open_disc(str(root), layout=str(layout))
    assert [(f.lsn, f.size, f.iso) for f in d.files] == [(f.lsn, f.size, f.iso) for f in d_iso.files]
    assert discmod.iso_meta(d) == discmod.iso_meta(d_iso)
    assert d.find("data/sub/b.dat").rel == "data/sub/b.dat"
    with pytest.raises(discmod.DiscError, match="real"):
        discmod.iso_meta(d, "real")
    with pytest.raises(discmod.DiscError, match="layout"):
        discmod.open_disc(str(root))
    (root / "data" / "a.dat").unlink()
    with pytest.raises(discmod.DiscError, match="missing"):
        discmod.open_disc(str(root), layout=str(layout))
    d_iso.close()
