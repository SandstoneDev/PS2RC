"""Command line: python -m ps2rc.pbp {pack,info,unpack,repack,placeholders} ...

    pack          take an EBOOT.PBP (or a program file) and write one with new menu assets / PARAM.SFO values
    info          show the files and the PARAM.SFO of a PBP and check them
    unpack        write the files of a PBP into a folder
    repack        build a PBP from such a folder
    placeholders  write neutral placeholder assets (ICON0/PIC0/PIC1 PNG, and ICON1.PMF when ffmpeg is there)

Run `python -m ps2rc.pbp COMMAND --help` for the options. Exit status: 0 done, 1 refused (the message says why).
"""
import argparse
import os
import sys

from . import assets, icon1, placeholder
from .pbp import Pbp
from .sfo import make as make_sfo
from .spec import PbpError, ICON1_DEFAULT_SECONDS


def _kv(text, as_int):
    if '=' not in text:
        raise PbpError('expected KEY=VALUE, got %r' % text)
    k, v = text.split('=', 1)
    if as_int:
        try:
            v = int(v, 0)
        except ValueError:
            raise PbpError('%s: %r is not an integer' % (k, v))
    return k.strip(), v


def _color(text):
    t = text.lstrip('#')
    if len(t) != 6:
        raise PbpError('colour %r: use RRGGBB' % text)
    return tuple(int(t[i:i + 2], 16) for i in (0, 2, 4))


def cmd_pack(a):
    if bool(a.eboot) == bool(a.data_psp):
        raise PbpError('give either --eboot (an existing PBP) or --data-psp (a program file)')
    if a.eboot:
        p = Pbp.read(a.eboot)
    else:
        if not a.title:
            raise PbpError('--data-psp needs --title')
        p = Pbp()
        with open(a.data_psp, 'rb') as f:
            p['DATA.PSP'] = f.read()
        if a.data_psar:
            with open(a.data_psar, 'rb') as f:
                p['DATA.PSAR'] = f.read()
        p['PARAM.SFO'] = make_sfo(a.title, extra=[('MEMSIZE', a.memsize)] if a.memsize is not None else ()).build()
    notes = assets.set_params(p, title=a.title, disc_id=a.disc_id, disc_version=a.disc_version,
                              parental=a.parental, memsize=a.memsize, app_ver=a.app_ver, category=a.category,
                              extra_str=[_kv(s, False) for s in a.set], extra_int=[_kv(s, True) for s in a.set_int])
    notes += assets.apply(p, icon0=a.icon0, icon1_src=a.icon1, pic0=a.pic0, pic1=a.pic1, snd0=a.snd0,
                          drop=a.drop, fit=a.fit, background=_color(a.background), icon0_alpha=a.icon0_alpha,
                          icon1_seconds=a.icon1_seconds, allow_off_spec=a.allow_off_spec, ffmpeg=a.ffmpeg,
                          icon1_epmap=a.icon1_epmap, log=print)
    for n in notes:
        print(n)
    size = p.write(a.out)
    print('wrote %s (%d bytes)' % (a.out, size))
    for i in assets.check(p):
        if i.level != 'info':
            print('  %s' % i)
    return 0


def cmd_info(a):
    p = Pbp.read(a.pbp)
    for line in assets.describe(p):
        print(line)
    issues = assets.check(p)
    print('checks:')
    for i in issues:
        print('  %s' % i)
    if not issues:
        print('  none')
    return 1 if a.strict and any(i.level in ('error', 'spec') for i in issues) else 0


def cmd_unpack(a):
    p = Pbp.read(a.pbp)
    for name in p.unpack(a.dir):
        print('%-10s %9d bytes' % (name, len(p[name])))
    return 0


def cmd_repack(a):
    p = Pbp.from_dir(a.dir)
    if not p['PARAM.SFO'] or not p['DATA.PSP']:
        raise PbpError('%s needs at least PARAM.SFO and DATA.PSP' % a.dir)
    print('wrote %s (%d bytes)' % (a.out, p.write(a.out)))
    return 0


def cmd_placeholders(a):
    os.makedirs(a.dir, exist_ok=True)
    out = {'ICON0.PNG': placeholder.icon0(a.text), 'PIC0.PNG': placeholder.pic0(a.text, a.sub),
           'PIC1.PNG': placeholder.pic1()}
    if a.icon1:
        pmf, rep = icon1.encode(frames=placeholder.icon1_frames(a.text, a.icon1_seconds),
                                max_seconds=a.icon1_seconds, ffmpeg=a.ffmpeg, epmap=a.icon1_epmap)
        out['ICON1.PMF'] = pmf
    for name, data in out.items():
        with open(os.path.join(a.dir, name), 'wb') as f:
            f.write(data)
        print('%-10s %7d bytes' % (name, len(data)))
    return 0


def parser():
    ap = argparse.ArgumentParser(prog='python -m ps2rc.pbp', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    pk = sub.add_parser('pack', help='write a PBP with new assets / parameters')
    pk.add_argument('--eboot', help='the EBOOT.PBP to start from (its program and PARAM.SFO are kept)')
    pk.add_argument('--data-psp', help='or: the program file (ELF/PRX) for a new PBP')
    pk.add_argument('--data-psar', help='optional DATA.PSAR for a new PBP')
    pk.add_argument('--out', required=True, help='output PBP (may be the same file as --eboot)')
    g = pk.add_argument_group('PARAM.SFO')
    g.add_argument('--title', help='menu title (UTF-8, one line, up to 127 bytes)')
    g.add_argument('--disc-id', help='product id, A-Z and 0-9')
    g.add_argument('--disc-version', help='X.XX')
    g.add_argument('--app-ver', help='XX.XX')
    g.add_argument('--parental', type=int, help='parental level 0..11')
    g.add_argument('--memsize', type=int, choices=(0, 1), help='1 = large user memory (PSP-2000 and later)')
    g.add_argument('--category', help='content category (pspsdk homebrew: MG)')
    g.add_argument('--set', action='append', default=[], metavar='KEY=TEXT', help='any text value')
    g.add_argument('--set-int', action='append', default=[], metavar='KEY=N', help='any integer value')
    g = pk.add_argument_group('menu assets (a path; omitted = keep what the EBOOT has)')
    g.add_argument('--icon0', help='still icon, any picture (made 144x80 RGB)')
    g.add_argument('--icon1', help='animated icon: a ready 144x80 .pmf, or a video / animated GIF to convert')
    g.add_argument('--pic0', help='information picture over the background (made 310x180 RGBA)')
    g.add_argument('--pic1', help='background picture (made 480x272 RGB)')
    g.add_argument('--snd0', help='menu music: a ready ATRAC3plus .at3 file (44.1 kHz stereo)')
    g.add_argument('--drop', action='append', default=[], metavar='NAME',
                   help='remove an asset: icon0, icon1, pic0, pic1, snd0, data_psar')
    g.add_argument('--fit', default='cover', choices=('cover', 'contain', 'stretch'),
                   help='how pictures and videos of another shape are fitted (default cover)')
    g.add_argument('--background', default='000000', metavar='RRGGBB', help='fill colour for RGB pictures')
    g.add_argument('--icon0-alpha', action='store_true', help='keep transparency in ICON0 (documented as RGB)')
    g.add_argument('--icon1-seconds', type=float, default=ICON1_DEFAULT_SECONDS,
                   help='longest ICON1 taken from a video (default %(default)s)')
    g.add_argument('--ffmpeg', help='ffmpeg with libx264 (default: FFMPEG or PATH)')
    g.add_argument('--icon1-epmap', action='store_true',
                   help='give ICON1 an entry-point map and PSMF version 0012 (when it has no map)')
    g.add_argument('--allow-off-spec', action='store_true',
                   help='pack files outside the documented formats/sizes (the menu may then not show them)')
    pk.set_defaults(func=cmd_pack)
    i = sub.add_parser('info', help='show and check a PBP')
    i.add_argument('pbp')
    i.add_argument('--strict', action='store_true', help='exit 1 when a check fails')
    i.set_defaults(func=cmd_info)
    u = sub.add_parser('unpack', help='write the files of a PBP into a folder')
    u.add_argument('pbp')
    u.add_argument('dir')
    u.set_defaults(func=cmd_unpack)
    r = sub.add_parser('repack', help='build a PBP from an unpacked folder')
    r.add_argument('dir')
    r.add_argument('out')
    r.set_defaults(func=cmd_repack)
    ph = sub.add_parser('placeholders', help='write neutral placeholder assets')
    ph.add_argument('dir')
    ph.add_argument('--text', default='PS2RC')
    ph.add_argument('--sub', default='', help='second line of PIC0')
    ph.add_argument('--icon1', action='store_true', help='also ICON1.PMF (needs ffmpeg)')
    ph.add_argument('--icon1-seconds', type=float, default=4.0)
    ph.add_argument('--ffmpeg')
    ph.add_argument('--icon1-epmap', action='store_true', help='ICON1 with an entry-point map, version 0012')
    ph.set_defaults(func=cmd_placeholders)
    return ap


def main(argv=None):
    a = parser().parse_args(argv)
    try:
        return a.func(a)
    except PbpError as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    except OSError as e:
        print('error: %s: %s' % (e.filename or '', e.strerror or e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
