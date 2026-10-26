"""EBOOT editor: change the menu look (ICON0, ICON1, PIC0, PIC1, SND0) and the PARAM.SFO title of an existing
EBOOT.PBP without touching its program or data.

Works with any PBP: PS2RC outputs, PSP homebrew, PS1 eboots (DATA.PSAR holds a PS1 disc image, often hundreds of
megabytes). DATA.PSP and DATA.PSAR are copied byte for byte; DATA.PSAR is streamed from the file, never loaded.

Layout of the rebuilt file (see pbp.py for the header):
  * the header keeps its version bytes; the files before DATA.PSAR follow back to back from the original first
    offset (pspsdk pack-pbp and the PS1 packers all write them this way);
  * DATA.PSP is kept as it is, with one exception: a packed module ("~PSP" header) followed by zero padding up to
    DATA.PSAR (the layout of PS1 eboots) is counted by its own size (the u32 at 0x2C of its header), so the padding
    can shrink or grow;
  * DATA.PSAR keeps its original offset whenever the new files before it fit; otherwise it moves to the next 64 KiB
    boundary (0x10000), as the public PS1 packers place it (popstation and its ports, pop-fe). Inside DATA.PSAR the
    PS1 data refers to itself relative to the start of DATA.PSAR (multi-disc tables at +0x200 are relative too: the
    ARK-5 Popcorn source adds the DATA.PSAR offset to them), so moving it keeps it valid;
  * a PBP with an empty DATA.PSAR (normal homebrew) is written exactly as pack-pbp would.
With nothing changed the rebuilt bytes equal the original file (tested).

PS1 eboots and ICON0: the PS1 emulator of the PSP reads ICON0 from the EBOOT when the game starts, not only the
menu. The Popcorn module of the PRO and ARK custom firmwares (public source, function get_icon0_status /
getIcon0Status) checks that ICON0 is a PNG of exactly 80x80 pixels and otherwise hands the emulator a stock icon
instead (the menu still shows the user's icon). On firmware without that module what a 144x80 ICON0 does inside
the emulator is Unknown (users report crashes at the start), so for PS1 eboots this editor recommends 80x80 and
says so when a 144x80 icon is chosen.
"""
import hashlib
import io
import os
import re
import struct

from . import at3, icon1, image, png
from .pbp import MAGIC, HEADER, module_kind
from .sfo import Sfo
from .spec import (PbpError, Issue, SLOTS, ImageSpec, ICON0, PIC0, PIC1, MEDIA_BUDGET, MEDIA_BUDGET_MAX, TITLE_MAX,
                   blocking)

PSAR_ALIGN = 0x10000
CHUNK = 4 << 20

ICON0_SQUARE = ImageSpec('ICON0.PNG', 80, 80, 'RGB', 'still icon, square (PS1 eboots, minis)', True)
ICON0_SHAPES = {'wide': ICON0, 'square': ICON0_SQUARE}
IMAGE_SPECS = {'ICON0.PNG': ICON0, 'PIC0.PNG': PIC0, 'PIC1.PNG': PIC1}
EDITABLE = ('ICON0.PNG', 'ICON1.PMF', 'PIC0.PNG', 'PIC1.PNG', 'SND0.AT3')
SHORT = {'icon0': 'ICON0.PNG', 'icon1': 'ICON1.PMF', 'pic0': 'PIC0.PNG', 'pic1': 'PIC1.PNG', 'snd0': 'SND0.AT3'}

KIND_PS1 = 'ps1'               # PS1 eboot (DATA.PSAR: PSISOIMG0000 / PSTITLEIMG000000, or CATEGORY "ME")
KIND_HOMEBREW = 'homebrew'     # DATA.PSP is a plain ELF (pspsdk homebrew, PS2RC outputs)
KIND_PS2RC = 'ps2rc'           # a PS2RC game folder (ps2rc_manifest.json next to the EBOOT)
KIND_PACKED = 'packed'         # DATA.PSP is a packed module and DATA.PSAR is not a PS1 image
KIND_OTHER = 'other'
KIND_TEXT = {KIND_PS1: 'PS1 eboot', KIND_HOMEBREW: 'PSP homebrew', KIND_PS2RC: 'PS2RC game',
             KIND_PACKED: 'PSP program (packed module)', KIND_OTHER: 'unknown program'}

PS1_ICON0_NOTE = ('PS1 eboot with a %dx%d ICON0: the PS1 emulator also reads ICON0 when the game starts. PRO / ARK '
                  'custom firmware give the emulator a stock icon when ICON0 is not 80x80 (the menu still shows '
                  'yours); on other firmware this is Unknown and users report crashes at the start. Safest: 80x80.')


def psp_module_size(data):
    """the size a "~PSP" packed module gives in its header (u32 at 0x2C), or None"""
    if len(data) >= 0x30 and data[:4] == b'~PSP':
        n = struct.unpack_from('<I', data, 0x2C)[0]
        if 0 < n <= len(data):
            return n
    return None


def _align(n, a):
    return (n + a - 1) // a * a


def free_name(path, suffix='.bak'):
    """path + suffix, or path + suffix + N when taken"""
    cand, n = path + suffix, 2
    while os.path.exists(cand):
        cand, n = '%s%s%d' % (path, suffix, n), n + 1
    return cand


def strip_png(data):
    """-> PNG without metadata chunks (text, colour profile, time, EXIF); the picture is unchanged"""
    if not png.is_png(data):
        return data
    out, pos = [data[:8]], 8
    while pos + 8 <= len(data):
        n, ctype = struct.unpack('>I4s', data[pos:pos + 8])
        end = pos + 12 + n
        if ctype in (b'IHDR', b'PLTE', b'tRNS', b'IDAT', b'IEND'):
            out.append(data[pos:end])
        if ctype == b'IEND':
            break
        pos = end
    return b''.join(out)


def picture(src, spec, box=None, fit='cover', background=(0, 0, 0)):
    """any picture (path or bytes) -> (PNG bytes of the slot's exact size and depth, note). box = (x0, y0, x1, y1)
    in source pixels (the caller keeps the slot's aspect; it is clamped here). Needs Pillow unless the source is
    already an exact PNG and no box is given."""
    if box is None:
        data, notes = image.prepare(src, spec, fit=fit, background=background)
        return strip_png(data), '; '.join(notes)
    try:
        from PIL import Image
    except ImportError:
        raise PbpError('%s: cropping needs Pillow (python -m pip install pillow)' % spec.slot)
    raw = src if isinstance(src, (bytes, bytearray)) else open(src, 'rb').read()
    try:
        im = Image.open(io.BytesIO(raw))
        if getattr(im, 'is_animated', False):
            im.seek(0)
        im.load()
    except Exception as e:      # Pillow raises unrelated types for unreadable files
        raise PbpError('%s: not a picture Pillow can read (%s)' % (spec.slot, e))
    im = im.convert('RGBA')
    W, H = spec.width, spec.height
    x0, y0, x1, y1 = (float(v) for v in box)
    w = max(1.0, min(x1 - x0, im.width, im.height * W / H))
    h = w * H / W
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    x0 = min(max(0.0, cx - w / 2), im.width - w)
    y0 = min(max(0.0, cy - h / 2), im.height - h)
    b = (round(x0), round(y0), round(x0 + w), round(y0 + h))
    out = im.crop(b).resize((W, H), Image.LANCZOS)
    if spec.mode == 'RGB':
        flat = Image.new('RGBA', (W, H), tuple(background) + (255,))
        flat.alpha_composite(out)
        out = flat.convert('RGB')
    buf = io.BytesIO()
    out.save(buf, 'PNG', optimize=True)
    data = strip_png(buf.getvalue())
    bad = [i for i in image.check(data, spec) if i.level in ('error', 'spec')]
    if bad:
        raise PbpError('%s: internal: the converted picture fails its own check: %s' % (spec.slot, bad[0]))
    return data, '%s: %dx%d, crop %s -> %dx%d %s' % (spec.slot, im.width, im.height, b, W, H, spec.mode)


class Layout:
    """where every file goes in the rebuilt PBP"""

    def __init__(self, offsets, data_psp_len, psar_offset, psar_size, moved, trimmed):
        self.offsets = offsets              # 8 offsets for the header
        self.data_psp_len = data_psp_len    # bytes of DATA.PSP written (before the zero gap)
        self.psar_offset = psar_offset
        self.psar_size = psar_size
        self.moved = moved                  # DATA.PSAR is not where it was
        self.trimmed = trimmed              # DATA.PSP counted by its module size (zero padding variable)
        self.size = psar_offset + psar_size

    def __repr__(self):
        return 'Layout(%r, psar=0x%X+%d, moved=%r)' % (self.offsets, self.psar_offset, self.psar_size, self.moved)


class Eboot:
    """An EBOOT.PBP opened for editing. Everything except DATA.PSAR is held in memory."""

    def __init__(self, path):
        self.path = str(path)
        try:
            f = open(self.path, 'rb')
        except OSError as e:
            raise PbpError('cannot read %s (%s)' % (self.path, e.strerror or e))
        with f:
            size = os.fstat(f.fileno()).st_size
            head = f.read(HEADER)
            if len(head) < HEADER or head[:4] != MAGIC:
                hint = ' (a plain ELF: it has no PBP header)' if head[:4] == b'\x7fELF' else ''
                raise PbpError('%s: not an EBOOT.PBP: it must start with 00 50 42 50%s' % (
                    os.path.basename(self.path), hint))
            offs = list(struct.unpack('<8I', head[8:HEADER])) + [size]
            if offs[0] < HEADER:
                raise PbpError('%s: the first file starts at %d, inside the 40-byte header: damaged' % (
                    os.path.basename(self.path), offs[0]))
            for i in range(8):
                if not offs[i] <= offs[i + 1] <= size:
                    raise PbpError('%s: the offsets of %s (%d) and of the next file (%d) are out of order or past '
                                   'the end (%d bytes): the file is damaged' % (
                                       os.path.basename(self.path), SLOTS[i], offs[i], offs[i + 1], size))
            self.version = head[4:8]
            self.first_offset = offs[0]
            self.gap = f.read(offs[0] - HEADER)
            self.orig = {}
            for i, slot in enumerate(SLOTS[:7]):
                f.seek(offs[i])
                self.orig[slot] = f.read(offs[i + 1] - offs[i])
            self.psar_offset, self.psar_size = offs[7], size - offs[7]
            f.seek(offs[7])
            self.psar_head = f.read(min(self.psar_size, 0x40))
        self.size = size
        self.orig_offsets = offs[:8]
        self.files = dict(self.orig)
        self.notes = []
        self.kind = self._kind()

    # ------------------------------------------------------------------ facts
    def _sfo(self, which='files'):
        raw = (self.files if which == 'files' else self.orig)['PARAM.SFO']
        try:
            return Sfo.parse(raw)
        except PbpError:
            return None

    def _kind(self):
        s = self._sfo('orig')
        if self.psar_head[:12] == b'PSISOIMG0000' or self.psar_head[:16] == b'PSTITLEIMG000000' or \
                (s is not None and s.get('CATEGORY') == 'ME'):
            return KIND_PS1
        dp = self.orig['DATA.PSP']
        if dp[:4] == b'\x7fELF':
            if os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(self.path)), 'ps2rc_manifest.json')):
                return KIND_PS2RC
            return KIND_HOMEBREW
        if dp[:4] == b'~PSP':
            return KIND_PACKED
        return KIND_OTHER

    @property
    def is_ps1(self):
        return self.kind == KIND_PS1

    @property
    def multi_disc(self):
        return self.psar_head[:16] == b'PSTITLEIMG000000'

    def title(self):
        s = self._sfo()
        return s.get('TITLE', '') if s is not None else ''

    def default_icon0_shape(self):
        return 'square' if self.is_ps1 else 'wide'

    def icon0_shape(self, data=None):
        """'wide' (144x80), 'square' (80x80) or 'other' for the current (or given) ICON0"""
        data = self.files['ICON0.PNG'] if data is None else data
        try:
            i = png.info(data)
        except PbpError:
            return 'other'
        for name, spec in ICON0_SHAPES.items():
            if (i['width'], i['height']) == (spec.width, spec.height):
                return name
        return 'other'

    def changed(self):
        return [s for s in SLOTS[:7] if self.files[s] != self.orig[s]]

    # ------------------------------------------------------------------ edits
    def set_title(self, title):
        """change TITLE in PARAM.SFO (other keys untouched). Back to the original bytes when it equals the original."""
        title = title if isinstance(title, str) else str(title)
        orig = self._sfo('orig')
        if orig is None:
            raise PbpError('PARAM.SFO is damaged: the title cannot be changed')
        if orig.get('TITLE') == title:
            self.files['PARAM.SFO'] = self.orig['PARAM.SFO']
            return []
        n = len(title.encode('utf-8'))
        if n == 0 or n > TITLE_MAX:
            raise PbpError('the title is %d bytes of UTF-8, it must be 1..%d' % (n, TITLE_MAX))
        if re.search(r'[\x00-\x1f\x7f]', title):
            raise PbpError('the title may not contain line breaks or control characters')
        s = self._sfo('orig')
        s.set('TITLE', title)
        self.files['PARAM.SFO'] = s.build()
        return ['PARAM.SFO: TITLE %r -> %r' % (orig.get('TITLE'), title)]

    def set_file(self, slot, data, allow_off_spec=False):
        """replace one menu file (bytes) after checking it; -> list of Issue that were accepted"""
        slot = SHORT.get(slot, slot)
        if slot not in EDITABLE:
            raise PbpError('%s cannot be replaced here (editable: %s)' % (slot, ', '.join(EDITABLE)))
        data = bytes(data or b'')
        if not data:
            return self.remove(slot)
        found = self.check_file(slot, data)
        stop = blocking(found, allow_off_spec)
        if stop:
            raise PbpError('%s: %s' % (slot, '; '.join(i.message for i in stop)))
        if slot in ('ICON1.PMF', 'SND0.AT3'):
            other = 'SND0.AT3' if slot == 'ICON1.PMF' else 'ICON1.PMF'
            total = len(data) + len(self.files[other])
            if total > MEDIA_BUDGET_MAX and not allow_off_spec:
                raise PbpError('ICON1.PMF + SND0.AT3 would be %d bytes together, the limit is 500 KB (%d): shorten '
                               'the clip or the music' % (total, MEDIA_BUDGET))
        self.files[slot] = data
        return found

    def remove(self, slot):
        slot = SHORT.get(slot, slot)
        if slot not in EDITABLE:
            raise PbpError('%s cannot be removed (removable: %s)' % (slot, ', '.join(EDITABLE)))
        self.files[slot] = b''
        return []

    def restore(self, slot):
        slot = SHORT.get(slot, slot)
        self.files[slot] = self.orig[slot]

    # ------------------------------------------------------------------ checks
    def icon0_spec(self, data):
        shape = self.icon0_shape(data)
        return ICON0_SHAPES[shape if shape != 'other' else self.default_icon0_shape()]

    def check_file(self, slot, data):
        """issues of one menu file for this EBOOT (PS1 rules included)"""
        if slot == 'ICON0.PNG':
            shape = self.icon0_shape(data)
            out = image.check(data, self.icon0_spec(data), allow_alpha=True)
            if shape == 'other':
                return out
            if self.is_ps1 and shape == 'wide':
                out.append(Issue('warn', slot, PS1_ICON0_NOTE % (ICON0.width, ICON0.height)))
            elif not self.is_ps1 and shape == 'square':
                out.append(Issue('warn', slot, '80x80: the size of PS1 eboots and minis; PSP programs normally '
                                               'use 144x80 (the menu shows a square icon)'))
            if png.info(data)['trailing_bytes']:
                out.append(Issue('warn', slot, 'bytes after the PNG end chunk (they are kept; a fix removes them)'))
            return out
        if slot in IMAGE_SPECS:
            out = image.check(data, IMAGE_SPECS[slot])
            if not [i for i in out if i.level == 'error'] and png.info(data)['trailing_bytes']:
                out.append(Issue('warn', slot, 'bytes after the PNG end chunk (they are kept; a fix removes them)'))
            return out
        if slot == 'ICON1.PMF':
            return icon1.check(data)
        if slot == 'SND0.AT3':
            return at3.check(data)
        return []

    def issues(self):
        out = []
        s = self._sfo()
        if s is None:
            out.append(Issue('error', 'PARAM.SFO', 'damaged or missing'))
        else:
            out += [i for i in s.validate() if not (self.is_ps1 and 'CATEGORY' in i.message)]
        for slot in EDITABLE:
            if self.files[slot]:
                out += self.check_file(slot, self.files[slot])
        if not self.files['ICON0.PNG']:
            out.append(Issue('warn', 'ICON0.PNG', 'no icon' + (
                ': the PS1 emulator then gets a stock icon from PRO / ARK firmware; elsewhere Unknown'
                if self.is_ps1 else ' (the menu draws a default one)')))
        total = len(self.files['ICON1.PMF']) + len(self.files['SND0.AT3'])
        if total > MEDIA_BUDGET_MAX:
            out.append(Issue('spec', 'ICON1.PMF + SND0.AT3', '%d bytes together, the limit is 500 KB' % total))
        elif total > MEDIA_BUDGET:
            out.append(Issue('warn', 'ICON1.PMF + SND0.AT3', '%d bytes together: over 500,000 bytes' % total))
        if not self.orig['DATA.PSP']:
            out.append(Issue('error', 'DATA.PSP', 'missing: there is no program to start'))
        if self.is_ps1 and self.files['ICON1.PMF'] and not self.orig['ICON1.PMF']:
            out.append(Issue('info', 'ICON1.PMF', 'an animated icon in a PS1 eboot: whether the PS1 emulator reads '
                                                  'it is Unknown (popstation writes none by default)'))
        return out

    def fixable(self):
        """slots whose picture can be re-made to the documented format (a Pillow re-encode)"""
        out = []
        for slot in ('ICON0.PNG', 'PIC0.PNG', 'PIC1.PNG'):
            data = self.files[slot]
            if not data:
                continue
            bad = [i for i in self.check_file(slot, data) if i.level in ('error', 'spec')]
            if slot == 'ICON0.PNG' and self.is_ps1 and self.icon0_shape(data) != 'square':
                bad = bad or ['ps1']
            try:
                trailing = png.info(data)['trailing_bytes']
            except PbpError:
                trailing = 0
            if bad or trailing:
                out.append(slot)
        return out

    def fix(self, fit='cover', background=(0, 0, 0)):
        """re-make every fixable picture: exact size, 8-bit, not interlaced, no metadata; PS1 ICON0 -> 80x80.
        -> notes. Needs Pillow for anything but stripping."""
        notes = []
        for slot in self.fixable():
            data = self.files[slot]
            if slot == 'ICON0.PNG':
                spec = ICON0_SQUARE if self.is_ps1 else self.icon0_spec(data)
            else:
                spec = IMAGE_SPECS[slot]
            if png.is_png(data) and not [i for i in image.check(data, spec, allow_alpha=(slot == 'ICON0.PNG'))
                                         if i.level in ('error', 'spec')]:
                new, note = strip_png(data), '%s: data after the end chunk / metadata removed' % slot
            else:
                new, note = picture(data, spec, fit=fit, background=background)
            self.files[slot] = new
            notes.append(note)
        return notes

    # ------------------------------------------------------------------ layout
    def layout(self):
        dp = self.files['DATA.PSP']
        mod = psp_module_size(dp)
        trimmed = bool(self.psar_size and mod is not None and not dp[mod:].strip(b'\0'))
        dp_len = mod if trimmed else len(dp)
        keep = bool(self.psar_size) and (trimmed or self.is_ps1)
        offsets, o = [], self.first_offset
        for slot in SLOTS[:7]:
            offsets.append(o)
            o += dp_len if slot == 'DATA.PSP' else len(self.files[slot])
        end = o
        if keep:
            psar = self.psar_offset if self.psar_offset >= end else _align(end, PSAR_ALIGN)
        else:
            psar = end
        offsets.append(psar)
        return Layout(offsets, dp_len, psar, self.psar_size, bool(self.psar_size) and psar != self.psar_offset,
                      trimmed)

    def head_bytes(self, lay=None):
        """everything before DATA.PSAR, as it will be written"""
        lay = lay or self.layout()
        parts = [MAGIC, self.version, struct.pack('<8I', *lay.offsets), self.gap]
        for slot in SLOTS[:6]:
            parts.append(self.files[slot])
        parts.append(self.files['DATA.PSP'][:lay.data_psp_len])
        head = b''.join(parts)
        return head + b'\0' * (lay.psar_offset - len(head))

    # ------------------------------------------------------------------ writing
    def _copy_psar(self, dst, progress=None):
        h = hashlib.sha256()
        done = 0
        with open(self.path, 'rb') as src:
            src.seek(self.psar_offset)
            left = self.psar_size
            while left:
                buf = src.read(min(CHUNK, left))
                if not buf:
                    raise PbpError('%s became shorter while it was copied' % self.path)
                dst.write(buf)
                h.update(buf)
                left -= len(buf)
                done += len(buf)
                if progress:
                    progress(done, self.psar_size)
        return h.hexdigest()

    @staticmethod
    def _hash_range(path, offset, size):
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            f.seek(offset)
            left = size
            while left:
                buf = f.read(min(CHUNK, left))
                if not buf:
                    break
                h.update(buf)
                left -= len(buf)
        return h.hexdigest()

    def write(self, out=None, backup=True, verify=True, progress=None):
        """write the edited EBOOT to `out` (default: in place). The original is renamed to a free
        '<name>.bak' name first when backup is True (in place, or when `out` already exists).
        -> dict(path, backup, size, layout, unchanged, psar_sha256)."""
        target = os.path.abspath(str(out or self.path))
        same = os.path.normcase(target) == os.path.normcase(os.path.abspath(self.path))
        if same and not self.changed():
            return {'path': target, 'backup': None, 'size': self.size, 'layout': self.layout(), 'unchanged': True,
                    'psar_sha256': None}
        lay = self.layout()
        head = self.head_bytes(lay)
        d = os.path.dirname(target) or '.'
        try:
            import shutil
            free = shutil.disk_usage(d).free
        except OSError:
            free = None
        if free is not None and free < lay.size + (1 << 20):
            raise PbpError('not enough free space in %s: %d bytes needed, %d free' % (d, lay.size, free))
        tmp = target + '.tmp-edit'
        try:
            with open(tmp, 'wb') as f:
                f.write(head)
                sha = self._copy_psar(f, progress)
                f.flush()
                os.fsync(f.fileno())
            if verify:
                with open(tmp, 'rb') as f:
                    if f.read(len(head)) != head:
                        raise PbpError('verification failed: the written header and menu files differ (bad disk or '
                                       'card?) - the original is untouched')
                if os.path.getsize(tmp) != lay.size or \
                        self._hash_range(tmp, lay.psar_offset, lay.psar_size) != sha:
                    raise PbpError('verification failed: the written DATA.PSAR differs (bad disk or card?) - the '
                                   'original is untouched')
            bak = None
            if os.path.exists(target):
                if backup:
                    bak = free_name(target)
                    os.replace(target, bak)
                os.replace(tmp, target)
            else:
                os.replace(tmp, target)
        except BaseException:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
        return {'path': target, 'backup': bak, 'size': lay.size, 'layout': lay, 'unchanged': False,
                'psar_sha256': sha}

    # ------------------------------------------------------------------ report
    def describe(self):
        lay = self.layout()
        lines = ['%s: %s, %d bytes, PBP version %s' % (os.path.basename(self.path), KIND_TEXT[self.kind],
                                                       self.size, self.version.hex())]
        if self.is_ps1:
            lines.append('  DATA.PSAR: %s' % ('PS1 multi-disc image' if self.multi_disc else 'PS1 disc image'))
        for i, slot in enumerate(SLOTS):
            data = self.files[slot] if slot != 'DATA.PSAR' else None
            n = len(data) if data is not None else self.psar_size
            extra = ''
            if slot == 'DATA.PSP':
                extra = module_kind(self.files[slot])
            elif data and slot.endswith('.PNG'):
                try:
                    k = png.info(data, slot)
                    extra = '%dx%d %d-bit %s' % (k['width'], k['height'], k['bit_depth'], k['mode'])
                except PbpError as e:
                    extra = str(e)
            lines.append('  %-10s offset %10d  size %10d  %s' % (slot, lay.offsets[i], n, extra if n else '-'))
        lines.append('  TITLE %r' % self.title())
        return lines
