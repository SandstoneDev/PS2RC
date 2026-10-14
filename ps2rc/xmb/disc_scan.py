"""The generic provider of pictures and music from the player's own disc (the disc_art / disc_music contract).

It knows no game: it reads the disc image the player picked, at the moment he asks, and writes what it finds only
into his own cache folder (the window passes %LOCALAPPDATA%/PS2RC/disc_art); nothing of it is shipped or staged.

Pictures: every texture dictionary on the disc (found by its content, ps2tex.py) is decoded; a texture whose longer
side is >= MIN_SIDE is offered, ranked by generic rules:
    loading     opaque, >= 512 x 256, in a dictionary of >= 3 such pictures (a set of full-screen pictures);
                these and the titles are ordered most colourful first (the one-click suggestion takes the first)
    title       opaque, >= 512 x 256, alone or nearly alone in its dictionary
    background  opaque, longer side >= 256
    logo        with transparency, at least twice as wide as high
    other       everything else
Full-screen pictures are stored at power-of-two sizes and shown 4:3 on the TV: they are written at 640 x 480.
When no loose dictionary holds a set of full-screen pictures (no "loading" one), the dictionaries inside "VER2"
archives are read too (only their full-screen pictures).

Music: (1) the pack's facts [xmb.music] (pack.toml) describe indexed tracks of interleaved PS-ADPCM sub-streams:
a name table, a track table and the track header layout (see games/_template/pack.toml); (2) on any disc, loose
"VAGp" files (the PS2's documented ADPCM sample file) of >= MIN_SECONDS.
"""
import array
import json
import os
import struct
import sys

from . import ps2tex

MIN_SIDE = 128
FULL_W, FULL_H = 512, 256
SET_MIN = 3
SCREEN = (640, 480)
MIN_SECONDS = 20.0
ORDER = ("loading", "title", "background", "logo", "other")
COEF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
ARCHIVE_ENTRY_MIN = 64 * 1024


def _read_range(disc, f, offset, size):
    from .disc_art import read_range
    return read_range(disc, f, offset, size)


def _head(disc, f, n=16):
    return _read_range(disc, f, 0, min(n, f.size))


def pack_facts(pack_dir):
    """[xmb] of the pack's pack.toml (+ pack.private.toml), {} when there is none"""
    if not pack_dir or not os.path.isfile(os.path.join(pack_dir, "pack.toml")):
        return {}
    try:
        from ps2rc import pack
        return dict(pack.Pack(pack_dir).get("xmb", {}) or {})
    except Exception:                  # an unreadable pack.toml: no facts (pictures still work)
        return {}


class Provider:
    """the generic provider for one pack (facts from its pack.toml)"""
    SUGGEST = {}

    def __init__(self, pack_dir):
        self.pack_dir = pack_dir
        self.facts = pack_facts(pack_dir)
        self.__file__ = os.path.abspath(__file__)
        self.facts_id = json.dumps(self.facts, sort_keys=True, default=str)

    # ------------------------------------------------------------------------------------------------ pictures
    def artwork(self, disc, out_dir, log):
        return artwork(disc, out_dir, log)

    # ------------------------------------------------------------------------------------------------ music
    def music_list(self, disc, log, progress):
        return music_list(disc, log, progress, self.facts.get("music"))

    def music_pcm(self, disc, ref, start, seconds):
        return music_pcm(disc, ref, start, seconds, self.facts.get("music"))


# ---------------------------------------------------------------------------------------------------- pictures
def _opaque(im):
    a = im.getchannel("A")
    if a.getextrema()[0] >= 250:
        return True
    return sum(a.histogram()[250:]) >= 0.99 * im.width * im.height


def _full(t):
    return t.width >= FULL_W and t.height >= FULL_H


def _candidates(disc, log, cancel):
    """[(dictionary path, [(texture, image)])] of the loose dictionaries"""
    out = []
    for f in disc.files:
        if cancel():
            break
        if f.size < 16 or not ps2tex.is_dictionary(_head(disc, f), f.size):
            continue
        try:
            texs = ps2tex.parse(disc.read(f))
        except (ValueError, IndexError, struct.error) as e:
            log("pictures: %s not readable (%s)" % (f.iso, e))
            continue
        out.append((f.iso, _decoded(f.iso, texs, log, lambda t: max(t.width, t.height) >= MIN_SIDE)))
    return out


def _decoded(where, texs, log, want):
    got = []
    for t in texs:
        if not want(t):
            continue
        try:
            got.append((t, ps2tex.decode(t)))
        except (ValueError, IndexError, struct.error) as e:
            log("pictures: %s/%s not decodable (%s)" % (where, t.name, e))
    return got


def _archives(disc, log, cancel):
    """full-screen pictures of the dictionaries inside "VER2" archives"""
    out = []
    for f in disc.files:
        if cancel():
            break
        h = _head(disc, f, 8)
        n = ps2tex.archive_count(h)
        if not n or 8 + 32 * n > f.size:
            continue
        ents = ps2tex.archive_entries(_read_range(disc, f, 0, 8 + 32 * n)) or []
        log("pictures: archive %s (%d entries)" % (f.iso, len(ents)))
        for name, off, size in sorted(ents, key=lambda e: e[1]):
            if cancel():
                break
            if size < ARCHIVE_ENTRY_MIN or off + size > f.size + ps2tex.SECTOR:
                continue
            if not ps2tex.is_dictionary(_read_range(disc, f, off, 16), size):
                continue
            try:
                texs = ps2tex.parse(_read_range(disc, f, off, size))
            except (ValueError, IndexError, struct.error):
                continue
            got = _decoded("%s/%s" % (f.iso, name), texs, log, _full)
            got = [(t, im) for t, im in got if _opaque(im)]
            if got:
                out.append(("%s/%s" % (f.iso, name), got))
    return out


def classify(groups):
    """[(dictionary, [(texture, image)])] -> [(category, dictionary, texture, image)] in gallery order"""
    rows = []
    for where, got in groups:
        full = [t for t, im in got if _full(t) and _opaque(im)]
        for t, im in got:
            opaque = _opaque(im)
            if _full(t) and opaque:
                cat = "loading" if len(full) >= SET_MIN else "title"
            elif opaque and max(t.width, t.height) >= 256:
                cat = "background"
            elif not opaque and t.width >= 2 * t.height:
                cat = "logo"
            else:
                cat = "other"
            rows.append((cat, where, t, im, colourfulness(im) if cat in ("loading", "title") else 0.0))
    # full-screen pictures: the most colourful first (the one-click suggestion takes the first); others: largest first
    rows.sort(key=lambda r: (ORDER.index(r[0]), -r[4], -r[2].width * r[2].height))
    return [r[:4] for r in rows]


def colourfulness(im):
    """a picture's colourfulness (Hasler / Suesstrunk on a 32 x 24 copy): plain text screens and grey pictures low"""
    small = im.convert("RGB").resize((32, 24))
    px = small.tobytes()
    rg = [px[i] - px[i + 1] for i in range(0, len(px), 3)]
    yb = [0.5 * (px[i] + px[i + 1]) - px[i + 2] for i in range(0, len(px), 3)]
    n = float(len(rg))
    m_rg, m_yb = sum(rg) / n, sum(yb) / n
    s_rg = (sum((v - m_rg) ** 2 for v in rg) / n) ** 0.5
    s_yb = (sum((v - m_yb) ** 2 for v in yb) / n) ** 0.5
    return (s_rg ** 2 + s_yb ** 2) ** 0.5 + 0.3 * (m_rg ** 2 + m_yb ** 2) ** 0.5


def _safe(s):
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in s)


def artwork(disc, out_dir, log, cancel=lambda: False):
    """yields the disc's pictures as {name, category, path, width, height, source} (PNG files in out_dir)"""
    from PIL import Image
    groups = _candidates(disc, log, cancel)
    rows = classify(groups)
    if not any(r[0] == "loading" for r in rows):
        rows = classify(groups + _archives(disc, log, cancel))
    seen = set()
    for cat, where, t, im in rows:
        if cat in ("loading", "title"):
            im = im.resize(SCREEN, Image.LANCZOS)
        if im.getchannel("A").getextrema()[0] == 255:
            im = im.convert("RGB")
        src = os.path.basename(where)
        name = t.name or "picture"
        if name in seen:
            name = "%s (%s)" % (name, src)
        seen.add(name)
        out = os.path.join(out_dir, "%s.png" % _safe(name))
        im.save(out, optimize=True)
        yield {"name": name, "category": cat, "path": out, "width": im.width, "height": im.height, "source": src}


# ---------------------------------------------------------------------------------------------------- music
def adpcm(data):
    """PS-ADPCM (the PS2 sound processor's documented 16-byte frames) -> list of samples"""
    out, h1, h2 = [], 0, 0
    for o in range(0, len(data) - 15, 16):
        shift, filt = data[o] & 15, data[o] >> 4
        if filt > 4:
            filt = 0
        if shift > 12:
            shift = 9
        f0, f1 = COEF[filt]
        for i in range(28):
            nib = (data[o + 2 + i // 2] >> (4 * (i & 1))) & 15
            t = nib - 16 if nib >= 8 else nib
            s = ((t << 12) >> shift) + ((h1 * f0 + h2 * f1 + 32) >> 6)
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out.append(s)
            h2, h1 = h1, s
    return out


def _pcm(planes):
    n = min(len(pl) for pl in planes)
    out = array.array("h", bytes(2 * n * len(planes)))
    for c, pl in enumerate(planes):
        out[c::len(planes)] = array.array("h", pl[:n])
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


class _Tracks:
    """indexed tracks of interleaved PS-ADPCM sub-streams, described by the pack's [xmb.music] facts"""

    def __init__(self, disc, m):
        self.disc, self.m = disc, m
        nf = disc.find(m["names"])
        xf = disc.find(m["index"])
        if nf is None or xf is None:
            raise ValueError("the music tables are not on this disc")
        sp, rec, ln = disc.read(nf), int(m.get("names_record", 16)), int(m.get("names_len", 12))
        self.names = [sp[rec * i:rec * i + ln].split(b"\0")[0].decode("latin1") for i in range(len(sp) // rec)]
        tl = disc.read(xf)
        fmt = m.get("index_record", "<B3xII")
        size = struct.calcsize(fmt)
        self.tracks = [struct.unpack_from(fmt, tl, size * i) for i in range(len(tl) // size)]
        self._files = {}

    def file(self, k):
        if k not in self._files:
            self._files[k] = self.disc.find(self.m["container"].format(name=self.names[k]))
        return self._files[k]

    def header(self, tid):
        """(disc file, track offset, [(bytes, rate)])"""
        m = self.m
        k, off = self.tracks[tid][0], self.tracks[tid][1]
        f = self.file(k)
        if f is None:
            raise ValueError("container %s not on this disc" % self.names[k])
        h = _read_range(self.disc, f, off, int(m["header"]))
        n = struct.unpack_from("<H", h, int(m["stream_count"]))[0]
        desc = [struct.unpack_from("<II", h, int(m["streams"]) + 8 * j)
                for j in range(min(n, int(m.get("streams_max", 8))))]
        return f, off, desc

    def block(self, rate):
        m = self.m
        return rate * int(m["period"]) // int(m["period_rate"])


def music_list(disc, log, progress, m=None):
    out = []
    if m:
        try:
            out += _indexed_list(disc, log, progress, m)
        except (ValueError, KeyError, IndexError, struct.error) as e:
            log("music: %s" % e)
    out += _vag_list(disc)
    progress(1, 1)
    return out


def _indexed_list(disc, log, progress, m):
    from .disc_music import CATEGORIES
    tr = _Tracks(disc, m)
    out, count = [], {}
    default = m.get("default_category", "other")
    for tid in range(len(tr.tracks)):
        if tid % 25 == 0:
            progress(tid, len(tr.tracks))
        try:
            _f, _off, desc = tr.header(tid)
        except (ValueError, IndexError, struct.error):
            continue
        audio = [(b, r) for b, r in desc if r >= 8000]
        if not audio:
            continue
        b, r = audio[0]
        seconds = b / 16.0 * 28 / r
        if seconds < MIN_SECONDS:
            continue
        name = tr.names[tr.tracks[tid][0]]
        count[name] = count.get(name, 0) + 1
        cat = name.lower() if name.lower() in CATEGORIES else default
        out.append({"name": "%s %d" % (name, count[name]), "category": cat, "seconds": round(seconds, 1),
                    "ref": tid})
    return out


def _vag(disc, f):
    """(data offset, data bytes, rate) of a "VAGp" file, None when it is not one"""
    h = _head(disc, f, 48)
    if len(h) < 48 or h[:4] != b"VAGp":
        return None
    size, rate = struct.unpack_from(">II", h, 12)
    if not 8000 <= rate <= 48000:
        return None
    return 48, min(size, f.size - 48), rate


def _vag_list(disc):
    out = []
    for f in disc.files:
        if f.size < 48 + 16 * 1000:
            continue
        v = _vag(disc, f)
        if v is None:
            continue
        seconds = v[1] / 16.0 * 28 / v[2]
        if seconds >= MIN_SECONDS:
            out.append({"name": os.path.basename(f.iso), "category": "other", "seconds": round(seconds, 1),
                        "ref": "vag:" + f.iso})
    return out


def music_pcm(disc, ref, start, seconds, m=None):
    """-> (s16le interleaved bytes, rate, channels) of `seconds` from `start`"""
    if isinstance(ref, str) and ref.startswith("vag:"):
        f = disc.find(ref[4:])
        off, size, rate = _vag(disc, f)
        first = int(start * rate / 28) * 16
        want = min(size - first, int(seconds * rate / 28 + 1) * 16)
        return _pcm([adpcm(_read_range(disc, f, off + first, max(0, want)))]), rate, 1
    if not m:
        raise ValueError("this pack describes no music tracks")
    tr = _Tracks(disc, m)
    f, off, desc = tr.header(int(ref))
    blk = [tr.block(r) for _b, r in desc]
    chans = [k for k, (_b, r) in enumerate(desc) if r >= 8000][:2]
    rate = desc[chans[0]][1]
    first = int(start * rate / 28) * 16                       # byte offset of the first ADPCM frame
    want = int(seconds * rate / 28 + 1) * 16
    hdr, period = int(m["header"]), int(m["period"])
    planes = []
    for k in chans:
        total = min(desc[k][0], first + want)
        data, b = bytearray(), first
        while b < total:
            p, inner = divmod(b, blk[k])
            n = min(blk[k] - inner, total - b)
            data += _read_range(disc, f, off + hdr + p * period + sum(blk[:k]) + inner, n)
            b += n
        planes.append(adpcm(bytes(data)))
    return _pcm(planes), rate, len(planes)
