"""PARAM.SFO reader/writer: layout vectors, byte-identical round trips, pspsdk mksfoex as an oracle, validation."""
import struct
import subprocess

import pytest

from ps2rc.pbp import PbpError, Sfo, make_sfo
from ps2rc.pbp.sfo import FMT_INT, FMT_STR, FMT_BYTES
from pbp_helpers import find_tool


def test_layout_vector_written_by_hand():
    # one integer entry "A" = 5: header 20 + entry 16 = 36 (key table), "A\0" padded to 4 -> data at 40
    want = (b'\0PSF' + struct.pack('<4I', 0x101, 36, 40, 1) + struct.pack('<HHIII', 0, 0x0404, 4, 4, 0) +
            b'A\0\0\0' + struct.pack('<I', 5))
    s = Sfo()
    s.set('A', 5)
    assert s.build() == want
    back = Sfo.parse(want)
    assert back.get('A') == 5 and back.standard


def test_text_entry_vector():
    # "AB" = "xyz": used 4 (with the zero), reserved 4; keys "AB\0" padded to 4
    want = (b'\0PSF' + struct.pack('<4I', 0x101, 36, 40, 1) + struct.pack('<HHIII', 0, 0x0204, 4, 4, 0) +
            b'AB\0\0' + b'xyz\0')
    s = Sfo()
    s.set('AB', 'xyz')
    assert s.build() == want


def test_make_round_trip():
    s = make_sfo('My test title', extra=[('MEMSIZE', 1)])
    b = s.build()
    back = Sfo.parse(b)
    assert back.standard and back.build() == b
    assert back.keys() == ['MEMSIZE', 'BOOTABLE', 'CATEGORY', 'DISC_ID', 'DISC_VERSION', 'PARENTAL_LEVEL',
                           'PSP_SYSTEM_VER', 'REGION', 'TITLE']
    assert back.get('TITLE') == 'My test title' and back.get('MEMSIZE') == 1 and back.get('REGION') == 0x8000
    assert back.find('TITLE').fmt == FMT_STR and back.find('BOOTABLE').fmt == FMT_INT


@pytest.mark.parametrize('args,title,extra', [
    (['-d', 'MEMSIZE=1'], 'PS2RC test', [('MEMSIZE', 1)]),
    (['-d', 'MEMSIZE=1', '-s', 'APP_VER=01.00'], 'A', [('MEMSIZE', 1), ('APP_VER', '01.00')]),
    ([], 'Twelve chars', []),
])
def test_same_bytes_as_mksfoex(tmp_path, args, title, extra):
    tool = find_tool('mksfoex')
    if not tool:
        pytest.skip('pspsdk mksfoex not on PATH')
    out = tmp_path / 'p.sfo'
    subprocess.run([tool] + args + [title, str(out)], check=True, capture_output=True)
    assert make_sfo(title, extra=extra).build() == out.read_bytes()


def test_set_keeps_place_and_room():
    s = Sfo()
    s.set('TITLE', 'abc', reserved=128)
    s.set('DISC_ID', 'ABCD12345')
    s.set('TITLE', 'a much longer title that still fits')
    assert s.keys() == ['TITLE', 'DISC_ID'] and s.find('TITLE').reserved == 128
    b = s.build()
    assert Sfo.parse(b).build() == b and Sfo.parse(b).find('TITLE').reserved == 128
    s.set('DISC_ID', 'ABCD123456789')            # longer than its room of 12 -> grows to 16
    assert s.find('DISC_ID').reserved == 16
    s.set('NEWKEY', 7)
    assert s.keys()[-1] == 'NEWKEY'


@pytest.mark.parametrize('key,value', [('TITLE', True), ('TITLE', 1.5), ('bad key', 'x'), ('BIG', 1 << 32)])
def test_set_refuses(key, value):
    with pytest.raises(PbpError):
        Sfo().set(key, value)


def test_bytes_format_is_kept():
    s = Sfo()
    s.set('A', 1)
    b = bytearray(s.build())
    struct.pack_into('<H', b, 22, FMT_BYTES)      # entry 0 format -> 0x0004
    back = Sfo.parse(bytes(b))
    assert back.find('A').fmt == FMT_BYTES and back.get('A') == b'\x01\0\0\0' and back.build() == bytes(b)


def test_non_standard_layout_is_reported():
    b = make_sfo('x').build() + b'\0' * 8          # extra bytes at the end: values read, layout flagged
    s = Sfo.parse(b)
    assert s.get('TITLE') == 'x' and not s.standard
    assert any('non-standard' in i.message for i in s.validate())


@pytest.mark.parametrize('damage', ['magic', 'count', 'short', 'value'])
def test_damaged(damage):
    b = bytearray(make_sfo('x').build())
    if damage == 'magic':
        b[0] = 1
    elif damage == 'count':
        struct.pack_into('<I', b, 16, 1000)
    elif damage == 'short':
        b = b[:10]
    else:
        struct.pack_into('<I', b, 20 + 8, 4000)    # reserved length of entry 0 past the end
    with pytest.raises(PbpError):
        Sfo.parse(bytes(b))


def levels(s):
    return {i.level for i in s.validate()}


def test_validate_good():
    assert not levels(make_sfo('Fine title', extra=[('MEMSIZE', 1)])) & {'error', 'spec'}


@pytest.mark.parametrize('key,value,level', [
    ('TITLE', 'x' * 128, 'error'), ('TITLE', '', 'error'), ('TITLE', 'two\nlines', 'spec'),
    ('DISC_ID', 'ABCD-12345', 'spec'), ('DISC_ID', 'abcd12345', 'spec'), ('DISC_ID', 'A' * 13, 'spec'),
    ('PARENTAL_LEVEL', 12, 'spec'), ('DISC_VERSION', '1.0', 'warn'), ('MEMSIZE', 2, 'warn'),
])
def test_validate_finds(key, value, level):
    s = make_sfo('Fine title')
    s.set(key, value)
    assert level in levels(s)


def test_utf8_title_counts_bytes():
    s = make_sfo('\u00e9' * 63)                     # 126 bytes: fits
    assert 'error' not in levels(s)
    s.set('TITLE', '\u00e9' * 64)                   # 128 bytes: does not
    assert 'error' in levels(s)
    assert Sfo.parse(make_sfo('\u00e9t\u00e9').build()).get('TITLE') == '\u00e9t\u00e9'
