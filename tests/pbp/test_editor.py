"""EBOOT editor (ps2rc/pbp/editor.py, eboot_edit.py): rebuilds keep DATA.PSP / DATA.PSAR byte for byte, offsets are
right, title edits, PS1 rules. Every PBP here is generated (homebrew-like and PS1-like layouts with random payloads):
no game data, no platform files."""
import hashlib
import os
import random
import struct
import zlib

import pytest

from ps2rc.pbp import png
from ps2rc.pbp.editor import Eboot, PSAR_ALIGN, ICON0_SQUARE, free_name, psp_module_size, KIND_PS1, KIND_HOMEBREW, \
    KIND_PS2RC
from ps2rc.pbp.eboot_edit import main as cli
from ps2rc.pbp.pbp import Pbp
from ps2rc.pbp.sfo import Sfo, make as make_sfo
from ps2rc.pbp.spec import PbpError, SLOTS


def rnd(n, seed):
    return random.Random(seed).randbytes(n)


def flat_png(w, h, mode='RGB', color=(40, 90, 160)):
    ch = 3 if mode == 'RGB' else 4
    px = bytes(color + ((255,) if ch == 4 else ())) * (w * h)
    return png.encode(w, h, mode, px)


def noise_png(w, h, seed=1):
    return png.encode(w, h, 'RGB', rnd(w * h * 3, seed))


def with_ihdr(data, interlace=None, depth=None):
    """the same PNG with another interlace flag / bit depth in IHDR (CRC fixed); only the header is meaningful"""
    body = bytearray(data[16:29])
    if interlace is not None:
        body[12] = interlace
    if depth is not None:
        body[8] = depth
    crc = struct.pack('>I', zlib.crc32(b'IHDR' + bytes(body)) & 0xFFFFFFFF)
    return data[:16] + bytes(body) + crc + data[33:]


def homebrew_pbp(tmp_path, name='EBOOT.PBP', psar=b'', icon0=True):
    p = Pbp()
    p['PARAM.SFO'] = make_sfo('Test Homebrew', extra=[('MEMSIZE', 1)]).build()
    if icon0:
        p['ICON0.PNG'] = flat_png(144, 80)
    p['PIC1.PNG'] = flat_png(480, 272, color=(10, 20, 30))
    p['DATA.PSP'] = b'\x7fELF' + rnd(70000, 2)
    p['DATA.PSAR'] = psar
    path = tmp_path / name
    path.write_bytes(p.build())
    return path


def psp_module(n, seed=3):
    """a "~PSP" packed-module stand-in: header with its size at 0x2C, random body"""
    m = bytearray(rnd(n, seed))
    m[0:4] = b'~PSP'
    struct.pack_into('<I', m, 0x2C, n)
    return bytes(m)


def ps1_pbp(tmp_path, name='EBOOT.PBP', icon0=None, psar_magic=b'PSISOIMG0000', psar_len=300000, pad_junk=False):
    """the PS1-eboot layout the public packers write: files back to back, DATA.PSP = packed module + zero padding,
    DATA.PSAR on a 64 KiB boundary starting with the disc-image magic"""
    s = Sfo()
    for k, v in (('BOOTABLE', 1), ('CATEGORY', 'ME'), ('DISC_ID', 'HBRW00001'), ('DISC_VERSION', '1.00'),
                 ('LICENSE', 'test'), ('PARENTAL_LEVEL', 1), ('PSP_SYSTEM_VER', '3.01'), ('REGION', 0x8000)):
        s.set(k, v)
    s.set('TITLE', 'PS1 Test Disc', reserved=128)
    sfo = s.build()
    icon0 = flat_png(80, 80) if icon0 is None else icon0
    pic0, pic1 = flat_png(310, 180, 'RGBA'), flat_png(480, 272)
    mod = psp_module(30632)
    head_len = 40 + len(sfo) + len(icon0) + len(pic0) + len(pic1)
    psar_off = (head_len + len(mod) + PSAR_ALIGN - 1) // PSAR_ALIGN * PSAR_ALIGN
    pad = b'\0' * (psar_off - head_len - len(mod))
    if pad_junk:
        pad = b'\x01' + pad[1:]
    offs = [40, 40 + len(sfo)]
    offs.append(offs[-1] + len(icon0))         # ICON1 (empty)
    offs.append(offs[-1])                      # PIC0
    offs.append(offs[-1] + len(pic0))          # PIC1
    offs.append(offs[-1] + len(pic1))          # SND0 (empty)
    offs.append(offs[-1])                      # DATA.PSP
    offs.append(psar_off)                      # DATA.PSAR
    psar = psar_magic + rnd(psar_len - len(psar_magic), 4)
    data = b'\0PBP' + struct.pack('<I', 0x10000) + struct.pack('<8I', *offs) + sfo + icon0 + pic0 + pic1 + mod + \
        pad + psar
    path = tmp_path / name
    path.write_bytes(data)
    return path, mod, psar


def sha(b):
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------------------------------ round trip
@pytest.mark.parametrize('kind', ['homebrew', 'homebrew_psar', 'ps1', 'ps1_junkpad', 'ps1_multi'])
def test_unchanged_rebuild_is_byte_identical(tmp_path, kind):
    if kind == 'homebrew':
        path = homebrew_pbp(tmp_path)
    elif kind == 'homebrew_psar':
        path = homebrew_pbp(tmp_path, psar=rnd(5000, 9))
    else:
        path = ps1_pbp(tmp_path, pad_junk=(kind == 'ps1_junkpad'),
                       psar_magic=b'PSTITLEIMG000000' if kind == 'ps1_multi' else b'PSISOIMG0000')[0]
    orig = path.read_bytes()
    e = Eboot(path)
    assert e.head_bytes() + orig[e.psar_offset:] == orig
    out = tmp_path / 'copy.pbp'
    res = e.write(out=out)
    assert out.read_bytes() == orig and not res['layout'].moved
    m = os.path.getmtime(path)
    assert e.write()['unchanged'] and os.path.getmtime(path) == m and not os.path.exists(str(path) + '.bak')


def test_kinds(tmp_path):
    assert Eboot(homebrew_pbp(tmp_path)).kind == KIND_HOMEBREW
    assert Eboot(ps1_pbp(tmp_path, 'p.pbp')[0]).kind == KIND_PS1
    assert Eboot(ps1_pbp(tmp_path, 'm.pbp', psar_magic=b'PSTITLEIMG000000')[0]).multi_disc
    d = tmp_path / 'PS2RC_X'
    d.mkdir()
    (d / 'ps2rc_manifest.json').write_text('{}')
    assert Eboot(homebrew_pbp(d)).kind == KIND_PS2RC


def test_damaged_and_foreign_files_refused(tmp_path):
    bad = tmp_path / 'elf.pbp'
    bad.write_bytes(b'\x7fELF' + b'\0' * 100)
    with pytest.raises(PbpError, match='plain ELF'):
        Eboot(bad)
    path = homebrew_pbp(tmp_path)
    data = bytearray(path.read_bytes())
    struct.pack_into('<I', data, 8 + 4 * 6, len(data) + 10)      # DATA.PSP past the end
    path.write_bytes(bytes(data))
    with pytest.raises(PbpError, match='damaged'):
        Eboot(path)


# ------------------------------------------------------------------------------------------ homebrew edits
def test_homebrew_icon0_change_offsets_and_payload(tmp_path):
    path = homebrew_pbp(tmp_path, psar=rnd(5000, 9))
    before = Pbp.read(path)
    e = Eboot(path)
    new_icon = noise_png(144, 80, seed=7)
    e.set_file('icon0', new_icon)
    e.remove('pic1')
    pic0 = flat_png(310, 180, 'RGBA')
    e.set_file('PIC0.PNG', pic0)
    res = e.write()
    after = Pbp.read(path)
    assert after['ICON0.PNG'] == new_icon and after['PIC0.PNG'] == pic0 and after['PIC1.PNG'] == b''
    for slot in ('PARAM.SFO', 'DATA.PSP', 'DATA.PSAR', 'ICON1.PMF', 'SND0.AT3'):
        assert after[slot] == before[slot], slot
    # header offsets: back to back from 40 (pack-pbp layout), DATA.PSAR right after DATA.PSP
    raw = path.read_bytes()
    offs = struct.unpack('<8I', raw[8:40])
    o = 40
    for i, slot in enumerate(SLOTS):
        assert offs[i] == o, slot
        o += len(after[slot])
    assert o == len(raw) == res['size']
    assert open(res['backup'], 'rb').read() == before.build()


def test_backup_names_do_not_overwrite(tmp_path):
    path = homebrew_pbp(tmp_path)
    first = path.read_bytes()
    e = Eboot(path)
    e.set_title('One')
    assert e.write()['backup'].endswith('.bak')
    e = Eboot(path)
    e.set_title('Two')
    assert e.write()['backup'].endswith('.bak2')
    assert open(str(path) + '.bak', 'rb').read() == first
    assert free_name(str(path)).endswith('.bak3')


def test_no_backup_and_verify_failure_keeps_original(tmp_path, monkeypatch):
    path = homebrew_pbp(tmp_path, psar=rnd(3000, 5))
    orig = path.read_bytes()
    e = Eboot(path)
    e.set_title('Changed')
    monkeypatch.setattr(Eboot, '_hash_range', staticmethod(lambda *a: 'wrong'))
    with pytest.raises(PbpError, match='verification failed'):
        e.write()
    assert path.read_bytes() == orig
    assert [p.name for p in tmp_path.iterdir()] == ['EBOOT.PBP']
    monkeypatch.undo()
    assert Eboot(path).set_title('Changed') and e.write(backup=False)['backup'] is None
    assert [p.name for p in tmp_path.iterdir()] == ['EBOOT.PBP']


def test_bad_write_and_full_disk_refused(tmp_path, monkeypatch):
    import shutil
    path = homebrew_pbp(tmp_path, psar=rnd(3000, 6))
    orig = path.read_bytes()
    e = Eboot(path)
    e.set_title('Changed')
    real_fsync = os.fsync

    def corrupt(fd):                                    # a card that writes one wrong byte
        os.lseek(fd, 20, os.SEEK_SET)
        os.write(fd, b'X')
        real_fsync(fd)
    monkeypatch.setattr(os, 'fsync', corrupt)
    with pytest.raises(PbpError, match='header and menu files differ'):
        e.write()
    monkeypatch.setattr(os, 'fsync', real_fsync)
    monkeypatch.setattr(shutil, 'disk_usage', lambda d: shutil._ntuple_diskusage(10, 10, 1000))
    with pytest.raises(PbpError, match='free space'):
        e.write()
    assert path.read_bytes() == orig and [p.name for p in tmp_path.iterdir()] == ['EBOOT.PBP']


# ------------------------------------------------------------------------------------------ PARAM.SFO title
def test_title_edit_keeps_other_keys(tmp_path):
    path, _mod, _psar = ps1_pbp(tmp_path)
    old = Sfo.parse(Pbp.read(path)['PARAM.SFO'])
    e = Eboot(path)
    e.set_title(u'Mon jeu édition')
    e.write()
    new = Sfo.parse(Pbp.read(path)['PARAM.SFO'])
    assert new.get('TITLE') == u'Mon jeu édition'
    assert [(x.key, x.value) for x in new.entries if x.key != 'TITLE'] == \
        [(x.key, x.value) for x in old.entries if x.key != 'TITLE']
    assert new.find('TITLE').reserved == 128            # the room the original reserved is kept
    assert new.standard


def test_title_rules_and_restore(tmp_path):
    path = homebrew_pbp(tmp_path)
    e = Eboot(path)
    for bad in ('', 'a' * 128, 'two\nlines', 'tab\tx'):
        with pytest.raises(PbpError):
            e.set_title(bad)
    e.set_title('x' * 127)
    assert e.changed() == ['PARAM.SFO']
    e.set_title('Test Homebrew')
    assert e.changed() == [] and e.files['PARAM.SFO'] == e.orig['PARAM.SFO']


# ------------------------------------------------------------------------------------------ PS1 eboots
def test_ps1_small_change_keeps_psar_offset(tmp_path):
    path, mod, psar = ps1_pbp(tmp_path)
    e = Eboot(path)
    psar_off = e.psar_offset
    e.set_file('ICON0.PNG', flat_png(80, 80, color=(200, 0, 0)))
    e.set_title('Renamed')
    res = e.write()
    raw = path.read_bytes()
    offs = struct.unpack('<8I', raw[8:40])
    assert offs[7] == psar_off and not res['layout'].moved
    assert raw[offs[7]:] == psar
    assert raw[offs[6]:offs[6] + len(mod)] == mod
    assert not raw[offs[6] + len(mod):offs[7]].strip(b'\0')


def test_ps1_big_change_moves_psar_to_64k_boundary(tmp_path):
    path, mod, psar = ps1_pbp(tmp_path)
    e = Eboot(path)
    old_off = e.psar_offset
    big = noise_png(480, 272, seed=11)                  # ~390 KB: no longer fits before the old DATA.PSAR
    e.set_file('PIC1.PNG', big)
    res = e.write()
    raw = path.read_bytes()
    offs = struct.unpack('<8I', raw[8:40])
    assert res['layout'].moved and offs[7] > old_off and offs[7] % PSAR_ALIGN == 0
    assert res['layout'].trimmed and res['layout'].data_psp_len == len(mod)
    assert raw[offs[4]:offs[5]] == big
    assert raw[offs[6]:offs[6] + len(mod)] == mod and psp_module_size(raw[offs[6]:offs[7]]) == len(mod)
    assert not raw[offs[6] + len(mod):offs[7]].strip(b'\0')
    assert offs[7] == (offs[6] + len(mod) + PSAR_ALIGN - 1) // PSAR_ALIGN * PSAR_ALIGN     # padding re-made
    assert raw[offs[7]:] == psar and sha(raw[offs[7]:]) == res['psar_sha256']
    # and back: a smaller PIC1 again -> the new offset is kept, the payload still identical
    e = Eboot(path)
    e.set_file('PIC1.PNG', flat_png(480, 272))
    e.write(backup=False)
    raw2 = path.read_bytes()
    assert struct.unpack('<8I', raw2[8:40])[7] == offs[7] and raw2[offs[7]:] == psar


def test_ps1_junk_padding_is_kept_and_psar_stays_aligned(tmp_path):
    path, mod, psar = ps1_pbp(tmp_path, pad_junk=True)
    e = Eboot(path)
    e.set_file('PIC1.PNG', noise_png(480, 272, seed=12))
    e.write(backup=False)
    raw = path.read_bytes()
    offs = struct.unpack('<8I', raw[8:40])
    assert offs[7] % PSAR_ALIGN == 0 and raw[offs[7]:] == psar
    assert raw[offs[6]:offs[6] + len(mod)] == mod


def test_ps1_wide_icon0_warns_square_is_clean(tmp_path):
    path = ps1_pbp(tmp_path)[0]
    e = Eboot(path)
    assert e.default_icon0_shape() == 'square' and e.icon0_shape() == 'square'
    found = e.set_file('ICON0.PNG', flat_png(144, 80))
    assert any(i.level == 'warn' and 'PS1 emulator' in i.message for i in found)
    assert 'ICON0.PNG' in e.fixable()
    assert not [i for i in e.check_file('ICON0.PNG', flat_png(80, 80)) if i.level != 'info']
    with pytest.raises(PbpError, match='80x80'):
        e.set_file('ICON0.PNG', flat_png(100, 100))     # neither shape


def test_homebrew_square_icon_warns(tmp_path):
    e = Eboot(homebrew_pbp(tmp_path))
    assert e.default_icon0_shape() == 'wide'
    found = e.set_file('ICON0.PNG', flat_png(80, 80))
    assert any(i.level == 'warn' for i in found)
    with pytest.raises(PbpError, match='144x80'):
        e.set_file('ICON0.PNG', flat_png(160, 90))


def test_fix_makes_ps1_icon_square(tmp_path):
    pytest.importorskip('PIL.Image')
    path = ps1_pbp(tmp_path, icon0=noise_png(144, 80, seed=5))[0]
    e = Eboot(path)
    assert e.icon0_shape() == 'wide'
    notes = e.fix()
    assert notes and e.icon0_shape() == 'square'
    assert png.info(e.files['ICON0.PNG'])['mode'] == 'RGB'
    assert not e.fixable()


# ------------------------------------------------------------------------------------------ validation
def test_picture_validation(tmp_path):
    e = Eboot(homebrew_pbp(tmp_path))
    good = flat_png(144, 80)
    with pytest.raises(PbpError, match='interlaced'):
        e.set_file('ICON0.PNG', with_ihdr(good, interlace=1))
    with pytest.raises(PbpError, match='16-bit'):
        e.set_file('ICON0.PNG', with_ihdr(good, depth=16))
    with pytest.raises(PbpError, match='CRC|damaged'):
        e.set_file('ICON0.PNG', good[:40] + b'\xff' + good[41:])
    with pytest.raises(PbpError, match='480x272'):
        e.set_file('PIC1.PNG', flat_png(400, 240))
    e.set_file('ICON0.PNG', with_ihdr(good, interlace=1), allow_off_spec=True)   # the user insists
    assert 'ICON0.PNG' in e.fixable()
    with pytest.raises(PbpError, match='cannot be replaced'):
        e.set_file('DATA.PSP', b'x')


def test_trailing_bytes_are_stripped_by_fix(tmp_path):
    e = Eboot(homebrew_pbp(tmp_path))
    good = flat_png(144, 80)
    e.set_file('ICON0.PNG', good + b'JUNKJUNK')
    assert 'ICON0.PNG' in e.fixable()
    e.fix()
    assert e.files['ICON0.PNG'] == good


def test_media_budget(tmp_path):
    from pbp_helpers import make_at3, make_pmf
    e = Eboot(homebrew_pbp(tmp_path))
    snd = make_at3(samples=44100 * 20)
    e.set_file('SND0.AT3', snd)
    pmf = make_pmf(packs=(512000 - len(snd)) // 2048 + 2)
    with pytest.raises(PbpError, match='500 KB'):
        e.set_file('ICON1.PMF', pmf)


# ------------------------------------------------------------------------------------------ command line
def test_cli_title_icon_and_extract(tmp_path, capsys):
    path, mod, psar = ps1_pbp(tmp_path)
    icon = tmp_path / 'wide.png'
    icon.write_bytes(flat_png(144, 80, color=(1, 2, 3)))
    assert cli([str(path), '--title', 'CLI title', '--icon0', str(icon), '--icon0-shape', 'wide']) == 0
    out = capsys.readouterr().out
    assert 'PS1 emulator' in out and 'original kept as' in out
    p = Pbp.read(path)
    assert Sfo.parse(p['PARAM.SFO']).get('TITLE') == 'CLI title'
    assert png.info(p['ICON0.PNG'])['width'] == 144 and p['DATA.PSAR'] == psar
    ex = tmp_path / 'ex'
    assert cli([str(path), '--extract', str(ex)]) == 0
    assert sorted(os.listdir(ex)) == ['ICON0.PNG', 'PARAM.SFO', 'PIC0.PNG', 'PIC1.PNG']
    assert cli([str(tmp_path / 'missing.pbp')]) == 1


def test_cli_fix_and_info_only(tmp_path, capsys):
    pytest.importorskip('PIL.Image')
    path = ps1_pbp(tmp_path, icon0=flat_png(144, 80))[0]
    before = path.read_bytes()
    assert cli([str(path)]) == 0 and path.read_bytes() == before
    assert 'nothing changed' in capsys.readouterr().out
    assert cli([str(path), '--fix', '--no-backup']) == 0
    assert png.info(Pbp.read(path)['ICON0.PNG'])['width'] == ICON0_SQUARE.width
    assert not os.path.exists(str(path) + '.bak')
