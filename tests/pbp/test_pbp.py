"""EBOOT.PBP container: layout, byte-identical round trips, pspsdk pack-pbp as an oracle, damaged files."""
import struct
import subprocess

import pytest

from ps2rc.pbp import Pbp, PbpError, SLOTS, make_sfo, module_kind
from pbp_helpers import fake_elf, find_tool, make_png


def sample(sizes=(0, 137, 0, 55, 1000, 0, 4096, 9)):
    files = {}
    for k, (name, n) in enumerate(zip(SLOTS, sizes)):
        files[name] = bytes((k * 37 + i) & 0xFF for i in range(n))
    files['PARAM.SFO'] = make_sfo('t').build()
    return Pbp(files)


def test_layout_by_hand():
    p = sample()
    b = p.build()
    assert b[:8] == b'\0PBP\x00\x00\x01\x00'
    offs = struct.unpack('<8I', b[8:40])
    assert offs[0] == 40
    pos = 40
    for i, name in enumerate(SLOTS):
        assert offs[i] == pos
        assert b[pos:pos + len(p[name])] == p[name]
        pos += len(p[name])
    assert pos == len(b)


def test_round_trip_identical():
    b = sample().build()
    p = Pbp.parse(b)
    assert p.build() == b
    for name in SLOTS:
        assert p[name] == sample()[name]


def test_gap_and_version_kept():
    b = bytearray(sample().build())
    # move every file 8 bytes later, the gap holds non-zero bytes; version field changed
    offs = [o + 8 for o in struct.unpack('<8I', b[8:40])]
    odd = bytes(b[:4]) + b'\x01\x02\x03\x04' + struct.pack('<8I', *offs) + b'GAPBYTES' + bytes(b[40:])
    p = Pbp.parse(odd)
    assert p.first_offset == 48 and p.version == b'\x01\x02\x03\x04'
    assert p.build() == odd


@pytest.mark.parametrize('sizes', [(300, 0, 0, 0, 0, 0, 100, 0), (300, 50, 60, 70, 80, 90, 100, 110),
                                   (300, 0, 0, 0, 0, 0, 0, 0)])
def test_same_bytes_as_pack_pbp(tmp_path, sizes):
    tool = find_tool('pack-pbp')
    if not tool:
        pytest.skip('pspsdk pack-pbp not on PATH')
    p = sample(sizes)
    args = []
    for name in SLOTS:
        if p[name]:
            f = tmp_path / name
            f.write_bytes(p[name])
            args.append(str(f))
        else:
            args.append('NULL')
    out = tmp_path / 'EBOOT.PBP'
    subprocess.run([tool, str(out)] + args, check=True, capture_output=True)
    assert p.build() == out.read_bytes()


def test_unpack_and_repack(tmp_path):
    p = sample()
    names = p.unpack(str(tmp_path / 'u'))
    assert names == [n for n in SLOTS if p[n]]
    assert Pbp.from_dir(str(tmp_path / 'u')).build() == p.build()


def test_write_and_read(tmp_path):
    p = sample()
    path = tmp_path / 'x.pbp'
    assert p.write(str(path)) == len(p.build())
    assert Pbp.read(str(path)).build() == p.build()


@pytest.mark.parametrize('damage,needle', [
    ('magic', 'not a PBP'), ('elf', 'plain ELF'), ('order', 'out of order'), ('past', 'past the end'),
    ('inside', 'inside the 40-byte header'), ('short', 'not a PBP'),
])
def test_damaged(damage, needle):
    b = bytearray(sample().build())
    if damage == 'magic':
        b[1] = ord('X')
    elif damage == 'elf':
        b = bytearray(fake_elf())
    elif damage == 'order':
        struct.pack_into('<I', b, 8 + 4 * 3, 39 + 400)     # PIC0 offset beyond PIC1's
        struct.pack_into('<I', b, 8 + 4 * 4, 41)
    elif damage == 'past':
        struct.pack_into('<I', b, 8 + 4 * 7, len(b) + 1)
    elif damage == 'inside':
        struct.pack_into('<I', b, 8, 20)
    else:
        b = b[:30]
    with pytest.raises(PbpError, match=needle):
        Pbp.parse(bytes(b))


def test_slot_names():
    p = Pbp()
    p['ICON0.PNG'] = make_png(144, 80)
    with pytest.raises(PbpError):
        p['ICON2.PNG'] = b'x'


def test_module_kind():
    assert module_kind(fake_elf()).startswith('ELF')
    assert module_kind(b'~PSP' + b'\0' * 100).startswith('packed')
    assert module_kind(b'') == 'none'
