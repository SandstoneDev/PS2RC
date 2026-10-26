"""Command line EBOOT editor: python -m ps2rc.pbp.eboot_edit EBOOT.PBP [options]

Changes the menu look (ICON0, ICON1, PIC0, PIC1, SND0) and the title of any EBOOT.PBP - PS2RC outputs, PSP homebrew,
PS1 eboots - and keeps DATA.PSP / DATA.PSAR byte for byte (see editor.py). Without an option it only shows the file
and its checks. The original is kept as EBOOT.PBP.bak unless --no-backup.

    --title TEXT            new title (PARAM.SFO TITLE)
    --icon0 FILE            still icon from any picture (Pillow converts it); --icon0-shape wide (144x80, 16:9)
                            or square (80x80); default: the shape the EBOOT uses now, else square for PS1 eboots
                            and wide for the rest. --crop X0,Y0,X1,Y1 = part of the picture (source pixels)
    --pic0 FILE / --pic1 FILE   picture over the background (310x180) / background (480x272)
    --icon1 FILE            animated icon: a ready .pmf, or any clip (needs ffmpeg with libx264)
    --snd0 FILE             menu music: a ready .at3, or any audio (needs atracdenc and ffmpeg)
    --remove SLOT           icon0 icon1 pic0 pic1 snd0 (repeatable)
    --fix                   re-make pictures that are outside the documented format (PS1: ICON0 -> 80x80)
    --extract DIR           write the menu files and PARAM.SFO into DIR, change nothing
    --out FILE              write there instead of in place
    --fit cover|contain|stretch   how a picture of another shape is fitted (default cover)
    --allow-off-spec        accept files outside the documented format (what the PSP then does is Unknown)
    --no-backup             do not keep the original

Exit status: 0 done, 1 refused (the message says why).
"""
import argparse
import os
import sys

from .editor import Eboot, ICON0_SHAPES, SHORT, picture
from .spec import PbpError, IMAGES


def _box(text):
    try:
        v = [float(x) for x in text.split(',')]
    except ValueError:
        v = []
    if len(v) != 4 or v[2] <= v[0] or v[3] <= v[1]:
        raise PbpError('--crop: give X0,Y0,X1,Y1 with X1 > X0 and Y1 > Y0')
    return v


def _media(slot, src, e):
    """ready .pmf / .at3 used as is, anything else converted with the converter's tools"""
    ext = os.path.splitext(src)[1].lower()
    with open(src, 'rb') as f:
        raw = f.read()
    if slot == 'ICON1.PMF' and ext == '.pmf':
        return raw, 'ICON1.PMF: ready file %d bytes' % len(raw)
    if slot == 'SND0.AT3' and ext == '.at3':
        return raw, 'SND0.AT3: ready file %d bytes' % len(raw)
    from ps2rc.pbp.spec import MEDIA_BUDGET
    if slot == 'ICON1.PMF':
        from ps2rc.xmb import clip
        pmf, rep = clip.make_icon1(src, budget=MEDIA_BUDGET - len(e.files['SND0.AT3']))
        return pmf, 'ICON1.PMF: %.1f s, %d bytes' % (rep['seconds'], rep['bytes'])
    from ps2rc.xmb import sound
    budget = MEDIA_BUDGET - len(e.files['ICON1.PMF'])
    secs = min(sound.DEFAULT_SECONDS, sound.max_seconds(budget, sound.DEFAULT_CODEC))
    pcm = sound.shape(sound.read_pcm(src, 0.0, secs), loop=True)
    data, rep = sound.encode(pcm, loop=True, codec=sound.DEFAULT_CODEC)
    return data, 'SND0.AT3: %.1f s, %d bytes' % (rep['seconds'], rep['bytes'])


def run(a, say=print):
    e = Eboot(a.eboot)
    for line in e.describe():
        say(line)
    if a.extract:
        os.makedirs(a.extract, exist_ok=True)
        for slot in ('PARAM.SFO',) + tuple(SHORT.values()):
            if e.files[slot]:
                with open(os.path.join(a.extract, slot), 'wb') as f:
                    f.write(e.files[slot])
                say('extracted %s' % slot)
        return 0
    notes = []
    if a.title is not None:
        notes += e.set_title(a.title)
    for name in a.remove:
        e.remove(name)
        notes.append('%s: removed' % SHORT.get(name, name))
    if a.icon0:
        shape = a.icon0_shape
        if shape == 'auto':
            cur = e.icon0_shape()
            shape = cur if cur != 'other' else e.default_icon0_shape()
        data, note = picture(a.icon0, ICON0_SHAPES[shape], box=_box(a.crop) if a.crop else None, fit=a.fit)
        found = e.set_file('ICON0.PNG', data, a.allow_off_spec)
        notes.append(note)
        notes += [str(i) for i in found if i.level != 'info']
    for key in ('pic0', 'pic1'):
        src = getattr(a, key)
        if src:
            data, note = picture(src, IMAGES[key], fit=a.fit)
            e.set_file(SHORT[key], data, a.allow_off_spec)
            notes.append(note)
    for key in ('snd0', 'icon1'):         # SND0 first: ICON1 gets what it leaves of the shared budget
        src = getattr(a, key)
        if src:
            data, note = _media(SHORT[key], src, e)
            found = e.set_file(SHORT[key], data, a.allow_off_spec)
            notes.append(note)
            notes += [str(i) for i in found if i.level not in ('info',)]
    if a.fix:
        notes += e.fix(fit=a.fit)
    for n in notes:
        say(n)
    say('checks:')
    issues = [i for i in e.issues() if i.level != 'info']
    for i in issues:
        say('  %s' % i)
    if not issues:
        say('  no problems found')
    if not e.changed():
        say('nothing changed, file not written')
        return 0
    res = e.write(out=a.out, backup=not a.no_backup)
    lay = res['layout']
    where = ('DATA.PSAR at 0x%X, %s' % (lay.psar_offset, 'moved' if lay.moved else 'same place'))         if lay.psar_size else 'no DATA.PSAR'
    say('wrote %s (%d bytes); DATA.PSP and DATA.PSAR copied unchanged (%s)' % (res['path'], res['size'], where))
    if res['backup']:
        say('original kept as %s' % res['backup'])
    return 0


def parser():
    ap = argparse.ArgumentParser(prog='ps2rc-eboot-edit', description='Edit the menu look and title of an '
                                 'EBOOT.PBP (DATA.PSP / DATA.PSAR stay byte for byte).')
    ap.add_argument('eboot')
    ap.add_argument('--title')
    ap.add_argument('--icon0')
    ap.add_argument('--icon0-shape', choices=('auto', 'wide', 'square'), default='auto')
    ap.add_argument('--crop')
    ap.add_argument('--pic0')
    ap.add_argument('--pic1')
    ap.add_argument('--icon1')
    ap.add_argument('--snd0')
    ap.add_argument('--remove', action='append', default=[], choices=sorted(SHORT))
    ap.add_argument('--fix', action='store_true')
    ap.add_argument('--extract')
    ap.add_argument('--out')
    ap.add_argument('--fit', choices=('cover', 'contain', 'stretch'), default='cover')
    ap.add_argument('--allow-off-spec', action='store_true')
    ap.add_argument('--no-backup', action='store_true')
    return ap


def main(argv=None):
    a = parser().parse_args(argv)
    try:
        return run(a)
    except PbpError as e:
        print('refused: %s' % e, file=sys.stderr)
        return 1
    except OSError as e:
        print('refused: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
