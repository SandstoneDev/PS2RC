"""Put user menu assets into a PBP and check a whole PBP."""
import os

from . import at3, icon1, image, sfo as sfo_mod
from .pbp import Pbp, module_kind
from .spec import (PbpError, Issue, blocking, IMAGES, MEDIA_BUDGET, MEDIA_BUDGET_MAX, ICON1_DEFAULT_SECONDS,
                   SLOT_OF, SLOTS)

PMF_EXT = ('.pmf',)


def _read(src, what):
    if isinstance(src, (bytes, bytearray)):
        return bytes(src)
    try:
        with open(src, 'rb') as f:
            return f.read()
    except OSError as e:
        raise PbpError('%s: cannot read %s (%s)' % (what, src, e.strerror or e))


def check_media_budget(icon1_bytes, snd0_bytes):
    total = len(icon1_bytes or b'') + len(snd0_bytes or b'')
    if total > MEDIA_BUDGET_MAX:
        return [Issue('spec', 'ICON1.PMF + SND0.AT3', '%d bytes together, the limit is 500 KB (%d)' % (
            total, MEDIA_BUDGET))]
    if total > MEDIA_BUDGET:
        return [Issue('warn', 'ICON1.PMF + SND0.AT3', '%d bytes together: under 500 KiB but over 500,000 bytes '
                                                       '(which one the limit means is Unknown)' % total)]
    return []


def check(p):
    """every finding for a Pbp (its PARAM.SFO and every asset present)"""
    out = []
    raw = p['PARAM.SFO']
    if not raw:
        out.append(Issue('error', 'PARAM.SFO', 'missing'))
    else:
        try:
            out += sfo_mod.Sfo.parse(raw).validate()
        except PbpError as e:
            out.append(Issue('error', 'PARAM.SFO', str(e).split(': ', 1)[-1]))
    for key, spec in IMAGES.items():
        data = p[spec.slot]
        if data:
            out += image.check(data, spec, allow_alpha=(key == 'icon0'))
        elif spec.required:
            out.append(Issue('warn', spec.slot, 'missing: documented as required (PBPs without it still appear in the menu and start)'))
    if p['ICON1.PMF']:
        out += icon1.check(p['ICON1.PMF'])
    if p['SND0.AT3']:
        out += at3.check(p['SND0.AT3'])
    out += check_media_budget(p['ICON1.PMF'], p['SND0.AT3'])
    if not p['DATA.PSP']:
        out.append(Issue('error', 'DATA.PSP', 'missing: there is no program to start'))
    return out


def apply(p, icon0=None, icon1_src=None, pic0=None, pic1=None, snd0=None, drop=(), fit='cover',
          background=(0, 0, 0), icon0_alpha=False, icon1_seconds=ICON1_DEFAULT_SECONDS, allow_off_spec=False,
          ffmpeg=None, icon1_epmap=False, log=None):
    """Put the given assets into Pbp `p` (paths or bytes; None = keep what p has). drop = short names or file names
    to remove (e.g. 'icon1'). icon1_epmap: give ICON1 an entry-point map (icon1.add_entry_points) when it has none.
    -> list of notes. Raises PbpError, and changes nothing, when an asset is unusable."""
    say = log or (lambda s: None)
    notes, new = [], {}
    for name in drop:
        slot = SLOT_OF.get(name, name)
        if slot not in SLOTS or slot in ('PARAM.SFO', 'DATA.PSP'):
            raise PbpError('cannot drop %r (droppable: icon0 icon1 pic0 pic1 snd0 data_psar)' % name)
        new[slot] = b''
        notes.append('%s: removed' % slot)
    # SND0 first: ICON1 gets the part of the shared budget it leaves
    if snd0 is not None:
        data = _read(snd0, 'SND0.AT3')
        found = at3.check(data)
        stop = blocking(found, allow_off_spec)
        if stop:
            raise PbpError('SND0.AT3: %s' % '; '.join(i.message for i in stop))
        new['SND0.AT3'] = data
        notes.append('SND0.AT3: %d bytes; %s' % (len(data), '; '.join(i.message for i in found)))
    snd0_now = new.get('SND0.AT3', p['SND0.AT3'])
    if icon1_src is not None:
        is_pmf = isinstance(icon1_src, (bytes, bytearray)) or \
            os.path.splitext(str(icon1_src))[1].lower() in PMF_EXT
        if is_pmf:
            data = _read(icon1_src, 'ICON1.PMF')
            found = icon1.check(data)
            stop = blocking(found, allow_off_spec)
            if stop:
                raise PbpError('ICON1.PMF: %s' % '; '.join(i.message for i in stop))
            if icon1_epmap and not icon1.entry_points(data):
                data = icon1.add_entry_points(data)
                found = icon1.check(data)
            new['ICON1.PMF'] = data
            notes.append('ICON1.PMF: %s used (%s)' % (
                'the given data' if isinstance(icon1_src, (bytes, bytearray)) else icon1_src,
                '; '.join(i.message for i in found)))
        else:
            budget = MEDIA_BUDGET - len(snd0_now)
            say('ICON1.PMF: encoding %s (budget %d bytes) ...' % (icon1_src, budget))
            data, rep = icon1.encode(src=str(icon1_src), budget=budget, max_seconds=icon1_seconds, fit=fit,
                                     ffmpeg=ffmpeg, epmap=icon1_epmap)
            new['ICON1.PMF'] = data
            notes.append('ICON1.PMF: %s -> %d frames, %.2f s, %d kbit/s, %d bytes, %d entry points' % (
                rep['source'], rep['frames'] or 0, rep['seconds'], rep['kbps'], rep['bytes'], rep['entry_points']))
    for key, src in (('icon0', icon0), ('pic0', pic0), ('pic1', pic1)):
        if src is None:
            continue
        spec = IMAGES[key]
        data, n = image.prepare(src, spec, fit=fit, background=background,
                                keep_alpha=(icon0_alpha and key == 'icon0'))
        new[spec.slot] = data
        notes += n
        if icon0_alpha and key == 'icon0' and image.png.info(data)['mode'] == 'RGBA':
            notes.append('ICON0.PNG: kept RGBA; the documented format is 24-bit RGB, how the menu draws its '
                         'transparency is Unknown')
    budget_issues = check_media_budget(new.get('ICON1.PMF', p['ICON1.PMF']), new.get('SND0.AT3', p['SND0.AT3']))
    stop = blocking(budget_issues, allow_off_spec)
    if stop:
        raise PbpError('%s: %s' % (stop[0].what, stop[0].message))
    notes += ['%s: %s' % (i.what, i.message) for i in budget_issues]
    for slot, data in new.items():
        p[slot] = data
    return notes


def set_params(p, title=None, disc_id=None, disc_version=None, parental=None, memsize=None, app_ver=None,
               category=None, extra_str=(), extra_int=()):
    """change PARAM.SFO values in place; -> list of notes. The SFO is only rebuilt when something changes."""
    changes = [('TITLE', title), ('DISC_ID', disc_id), ('DISC_VERSION', disc_version),
               ('PARENTAL_LEVEL', parental), ('MEMSIZE', memsize), ('APP_VER', app_ver), ('CATEGORY', category)]
    changes += list(extra_str) + list(extra_int)
    changes = [(k, v) for k, v in changes if v is not None]
    if not changes:
        return []
    s = sfo_mod.Sfo.parse(p['PARAM.SFO'])
    notes = []
    for k, v in changes:
        old = s.get(k)
        if old != v:
            s.set(k, v)
            notes.append('PARAM.SFO: %s %r -> %r' % (k, old, v))
    stop = blocking(s.validate())
    if stop:
        raise PbpError('PARAM.SFO: %s' % '; '.join(i.message for i in stop))
    if notes:
        p['PARAM.SFO'] = s.build()
    return notes


def describe(p):
    """lines for `info`"""
    lines, offs = [], p.offsets()
    total = p.first_offset + sum(len(p[slot]) for slot in SLOTS)
    lines.append('PBP version %s, %d bytes' % (p.version.hex(), total))
    for i, slot in enumerate(SLOTS):
        data = p[slot]
        extra = ''
        if slot == 'DATA.PSP':
            extra = module_kind(data)
        elif data and slot.endswith('.PNG'):
            try:
                n = image.png.info(data, slot)
                extra = '%dx%d %d-bit %s' % (n['width'], n['height'], n['bit_depth'], n['mode'])
            except PbpError as e:
                extra = str(e)
        elif data and slot == 'ICON1.PMF':
            try:
                m = icon1.parse(data)
                v = [s for s in m['streams'] if s['kind'] == 'video']
                extra = 'PSMF %s %s %s' % (m['version'], '%dx%d' % (v[0]['width'], v[0]['height']) if v else 'no video',
                                           '%.2f s' % m['seconds'] if m['seconds'] else '')
            except PbpError as e:
                extra = str(e)
        elif data and slot == 'SND0.AT3':
            try:
                a = at3.parse(data)
                extra = '%s %d Hz %d ch %s' % (a['codec'], a['rate'], a['channels'],
                                               '%.2f s' % a['seconds'] if a['seconds'] else '')
            except PbpError as e:
                extra = str(e)
        lines.append('  %-10s offset %9d  size %9d  %s' % (slot, offs[i], len(data), extra if data else '-'))
    if p['PARAM.SFO']:
        try:
            s = sfo_mod.Sfo.parse(p['PARAM.SFO'])
            lines.append('PARAM.SFO (%s layout):' % ('standard' if s.standard else 'non-standard'))
            for e in s.entries:
                v = ('0x%X (%d)' % (e.value, e.value)) if e.fmt == sfo_mod.FMT_INT else repr(e.value)
                lines.append('  %-16s %s' % (e.key, v))
        except PbpError as e:
            lines.append('PARAM.SFO: %s' % e)
    return lines
