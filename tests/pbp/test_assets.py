"""Putting assets into a PBP, changing PARAM.SFO values, the whole-PBP check and the command line."""
import os
import subprocess
import sys

import pytest

from ps2rc.pbp import Pbp, PbpError, Sfo, apply, check, describe, make_sfo, placeholder, png, set_params
from ps2rc.pbp.spec import MEDIA_BUDGET
from pbp_helpers import fake_elf, make_at3, make_pmf, make_png

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def base():
    return Pbp({'PARAM.SFO': make_sfo('Base title', extra=[('MEMSIZE', 1)]).build(), 'DATA.PSP': fake_elf()})


def blocking(p):
    return [i for i in check(p) if i.level in ('error', 'spec')]


def test_apply_all_ready_files():
    p = base()
    notes = apply(p, icon0=make_png(144, 80), icon1_src=make_pmf(), pic0=make_png(310, 180, 'RGBA'),
                  pic1=make_png(480, 272), snd0=make_at3())
    assert p['ICON0.PNG'] == make_png(144, 80) and p['ICON1.PMF'] == make_pmf() and p['SND0.AT3'] == make_at3()
    assert not blocking(p) and len(notes) >= 5
    b = p.build()
    assert Pbp.parse(b).build() == b and Pbp.parse(b)['DATA.PSP'] == fake_elf()
    text = '\n'.join(describe(p))
    assert '144x80 8-bit RGB' in text and 'ATRAC3plus' in text and 'PSMF 0015 144x80' in text


def test_paths_work_too(tmp_path):
    f = tmp_path / 'icon.png'
    f.write_bytes(make_png(144, 80))
    s = tmp_path / 'music.at3'
    s.write_bytes(make_at3())
    p = base()
    apply(p, icon0=str(f), snd0=str(s))
    assert p['ICON0.PNG'] == f.read_bytes() and p['SND0.AT3'] == s.read_bytes()


def test_shared_budget():
    p = base()
    big_snd = make_at3(samples=44100 * 20, data_bytes=744 * 400)          # ~298 KB
    big_icon = make_pmf(packs=120)                                         # ~248 KB
    with pytest.raises(PbpError, match='500 KB'):
        apply(p, icon1_src=big_icon, snd0=big_snd, icon0=make_png(144, 80))   # refused after every file passed
    assert not p['SND0.AT3'] and not p['ICON1.PMF'] and not p['ICON0.PNG']  # nothing changed
    apply(p, icon1_src=big_icon, snd0=big_snd, allow_off_spec=True)
    assert any(i.level == 'spec' and '500 KB' in i.message for i in check(p))
    # the budget counts what the PBP already holds
    q = base()
    apply(q, snd0=big_snd)
    with pytest.raises(PbpError, match='500 KB'):
        apply(q, icon1_src=big_icon)


def test_between_the_two_kilobytes_is_a_warning():
    p = base()
    snd = make_at3(data_bytes=MEDIA_BUDGET + 2000 - 2048 * 3 - 0x800 - 200)
    apply(p, icon1_src=make_pmf(packs=3), snd0=snd)
    total = len(p['ICON1.PMF']) + len(p['SND0.AT3'])
    assert MEDIA_BUDGET < total <= 500 * 1024
    assert any(i.level == 'warn' and 'Unknown' in i.message for i in check(p))


def test_one_bad_asset_changes_nothing():
    p = base()
    before = p.build()
    with pytest.raises(PbpError, match='PCM'):
        apply(p, icon0=make_png(144, 80), snd0=make_at3(tag=1, align=4))
    assert p.build() == before


def test_drop():
    p = base()
    apply(p, icon1_src=make_pmf(), pic1=make_png(480, 272))
    apply(p, drop=['icon1', 'PIC1.PNG'])
    assert not p['ICON1.PMF'] and not p['PIC1.PNG']
    with pytest.raises(PbpError):
        apply(p, drop=['data_psp'])


def test_set_params():
    p = base()
    before = p['PARAM.SFO']
    assert set_params(p, title='Base title', memsize=1) == []              # nothing to change: same bytes
    assert p['PARAM.SFO'] == before
    notes = set_params(p, title='New name', disc_id='TEST00001', parental=3, extra_str=[('APP_VER', '01.00')])
    s = Sfo.parse(p['PARAM.SFO'])
    assert (s.get('TITLE'), s.get('DISC_ID'), s.get('PARENTAL_LEVEL'), s.get('APP_VER')) == (
        'New name', 'TEST00001', 3, '01.00')
    assert s.get('MEMSIZE') == 1 and len(notes) == 4
    with pytest.raises(PbpError):
        set_params(p, title='x' * 200)
    assert Sfo.parse(p['PARAM.SFO']).get('TITLE') == 'New name'


def test_check_finds_missing_program_and_icon():
    p = Pbp({'PARAM.SFO': make_sfo('t').build()})
    found = check(p)
    assert any(i.what == 'DATA.PSP' and i.level == 'error' for i in found)
    assert any(i.what == 'ICON0.PNG' and i.level == 'warn' for i in found)


def test_placeholders_pass_their_checks():
    from ps2rc.pbp import image, ICON0, PIC0, PIC1
    assert not image.check(placeholder.icon0('ABC 123'), ICON0)
    assert not image.check(placeholder.pic0('ABC', 'SUB LINE'), PIC0)
    assert not image.check(placeholder.pic1(), PIC1)
    w, h, mode, px = png.decode_rgb(placeholder.icon0('A'))
    assert len(set(px[i:i + 3] for i in range(0, len(px), 3))) > 2       # not a blank picture


# ------------------------------------------------------------------------------------------ command line
def cli(*args, cwd=None):
    env = dict(os.environ, PYTHONPATH=ROOT)
    return subprocess.run([sys.executable, '-m', 'ps2rc.pbp'] + list(args), cwd=cwd or ROOT, env=env,
                          capture_output=True, text=True)


def test_cli_pack_info_unpack(tmp_path):
    src = tmp_path / 'in.pbp'
    base().write(str(src))
    (tmp_path / 'i.png').write_bytes(make_png(144, 80))
    (tmp_path / 'b.png').write_bytes(make_png(480, 272))
    (tmp_path / 'm.at3').write_bytes(make_at3())
    (tmp_path / 'a.pmf').write_bytes(make_pmf())
    out = tmp_path / 'out.pbp'
    r = cli('pack', '--eboot', str(src), '--out', str(out), '--title', 'Cli title', '--icon0', str(tmp_path / 'i.png'),
            '--pic1', str(tmp_path / 'b.png'), '--snd0', str(tmp_path / 'm.at3'), '--icon1', str(tmp_path / 'a.pmf'),
            '--set-int', 'USE_USB=0')
    assert r.returncode == 0, r.stderr
    p = Pbp.read(str(out))
    assert Sfo.parse(p['PARAM.SFO']).get('TITLE') == 'Cli title' and p['DATA.PSP'] == fake_elf()
    r = cli('info', str(out), '--strict')
    assert r.returncode == 0 and 'Cli title' in r.stdout and 'ATRAC3plus' in r.stdout
    r = cli('unpack', str(out), str(tmp_path / 'u'))
    assert r.returncode == 0 and (tmp_path / 'u' / 'ICON0.PNG').read_bytes() == make_png(144, 80)
    r = cli('repack', str(tmp_path / 'u'), str(tmp_path / 're.pbp'))
    assert r.returncode == 0 and (tmp_path / 're.pbp').read_bytes() == out.read_bytes()


def test_cli_new_pbp_from_a_program(tmp_path):
    (tmp_path / 'prog.elf').write_bytes(fake_elf())
    r = cli('pack', '--data-psp', str(tmp_path / 'prog.elf'), '--out', str(tmp_path / 'n.pbp'), '--title', 'New',
            '--memsize', '1')
    assert r.returncode == 0, r.stderr
    s = Sfo.parse(Pbp.read(str(tmp_path / 'n.pbp'))['PARAM.SFO'])
    assert s.get('TITLE') == 'New' and s.get('MEMSIZE') == 1 and s.keys()[0] == 'MEMSIZE'


@pytest.mark.parametrize('args,needle', [
    (['pack', '--out', 'x.pbp'], 'either --eboot'),
    (['info', 'missing.pbp'], 'cannot read'),
])
def test_cli_errors(tmp_path, args, needle):
    r = cli(*args, cwd=str(tmp_path))
    assert r.returncode == 1 and needle in r.stderr


def test_cli_refuses_wrong_snd0(tmp_path):
    src = tmp_path / 'in.pbp'
    base().write(str(src))
    (tmp_path / 'm.at3').write_bytes(make_at3(rate=48000))
    r = cli('pack', '--eboot', str(src), '--out', str(tmp_path / 'o.pbp'), '--snd0', str(tmp_path / 'm.at3'))
    assert r.returncode == 1 and '48000 Hz' in r.stderr and not (tmp_path / 'o.pbp').exists()


def test_cli_placeholders(tmp_path):
    r = cli('placeholders', str(tmp_path / 'ph'), '--text', 'TEST')
    assert r.returncode == 0, r.stderr
    assert sorted(os.listdir(tmp_path / 'ph')) == ['ICON0.PNG', 'PIC0.PNG', 'PIC1.PNG']
