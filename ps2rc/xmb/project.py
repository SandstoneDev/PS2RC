"""The player's menu choices -> the exact files for the EBOOT (ICON0.PNG, ICON1.PMF, PIC0.PNG, PIC1.PNG, SND0.AT3).

A Project is plain data (saved as JSON by the converter window): title + one entry per slot:
    mode  'none' (slot left empty) | 'default' (our neutral generated asset) | 'file' (the player's file)
    path  the player's file; box: crop box in source pixels or None (centred "cover")
    start / seconds: the part of a clip or a song;  loop: music loops seamlessly (SND0)
build() makes every slot (cached by content + settings, so the window can rebuild after each change cheaply) and
returns paths to hand to the converter (ps2rc.convert Options icon0..snd0) plus the shared size budget.
SND0 is made first; ICON1 gets what SND0 leaves of the 500 KB budget (its bit rate is lowered to fit).
"""
import hashlib
import json
import os

from ps2rc.pbp import placeholder, at3 as pat3, icon1 as picon1
from ps2rc.pbp.spec import PbpError, MEDIA_BUDGET, MEDIA_BUDGET_MAX

from . import picture, clip, sound
from .title import clean_title, check_title

SLOT_KEYS = ("icon0", "icon1", "pic0", "pic1", "snd0")
EXT = {"icon0": ".png", "pic0": ".png", "pic1": ".png", "icon1": ".pmf", "snd0": ".at3"}
DEFAULT_MODE = {"icon0": "default", "icon1": "none", "pic0": "none", "pic1": "none", "snd0": "none"}
HAS_DEFAULT = {"icon0", "icon1", "pic0", "pic1"}            # no default music
VERSION = 1


def _slot(mode="none", path="", box=None, start=0.0, seconds=None, loop=True, codec=None):
    return {"mode": mode, "path": path, "box": list(box) if box else None, "start": float(start or 0),
            "seconds": seconds, "loop": bool(loop), "codec": codec or sound.DEFAULT_CODEC}


class Project:
    def __init__(self, title="", slots=None, default_text="PS2RC"):
        self.title = title or ""
        self.default_text = default_text
        self.allow_off_spec = False          # other off-spec assets (ATRAC3 music is a warning only since the HW test)
        self.slots = {k: _slot(DEFAULT_MODE[k]) for k in SLOT_KEYS}
        for k, v in (slots or {}).items():
            if k in self.slots:
                self.slots[k].update(v)

    # ----------------------------------------------------------------------------------------------- data
    def to_dict(self):
        return {"version": VERSION, "title": self.title, "slots": self.slots}

    @classmethod
    def from_dict(cls, d):
        d = d or {}
        return cls(d.get("title", ""), d.get("slots"))

    def save(self, path):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, indent=1)

    @classmethod
    def load(cls, path):
        with open(path, encoding="utf-8") as f:
            return cls.from_dict(json.load(f))

    def set_file(self, key, path, **kw):
        kw.setdefault("codec", self.slots[key].get("codec"))      # a new file keeps the chosen music quality
        self.slots[key] = _slot("file", path, **kw)

    def set_mode(self, key, mode):
        if mode == "default" and key not in HAS_DEFAULT:
            raise PbpError("%s has no default" % key)
        self.slots[key]["mode"] = mode

    def title_checks(self):
        return check_title(clean_title(self.title)) if self.title.strip() else []

    # ----------------------------------------------------------------------------------------------- making
    def fingerprint(self, key, extra=""):
        s = self.slots[key]
        h = hashlib.sha256(json.dumps([key, s["mode"], s["box"], s["start"], s["seconds"], s["loop"], s.get("codec"),
                                       self.default_text, extra], sort_keys=True).encode())
        if s["mode"] == "file" and s["path"]:
            try:
                st = os.stat(s["path"])
                h.update(("%s|%d|%d" % (os.path.abspath(s["path"]), st.st_size, int(st.st_mtime))).encode())
            except OSError:
                h.update(s["path"].encode("utf-8", "replace"))
        return h.hexdigest()[:20]

    def make_slot(self, key, budget=MEDIA_BUDGET, ffmpeg=None, encoder=None, delay=None):
        """-> (bytes or None, note). Raises PbpError."""
        s = self.slots[key]
        mode = s["mode"]
        if mode == "none" or (mode == "file" and not s["path"]):
            return None, ""
        if key in ("icon0", "pic0", "pic1"):
            if mode == "default":
                data = {"icon0": lambda: placeholder.icon0(self.default_text),
                        "pic0": lambda: placeholder.pic0(self.default_text, clean_title(self.title)[:24].upper()),
                        "pic1": placeholder.pic1}[key]()
                return data, "%s: default" % key
            data, notes = picture.render(s["path"], key, box=s["box"])
            return data, "; ".join(notes)
        if key == "icon1":
            secs = s["seconds"] or clip.DEFAULT_SECONDS
            if mode == "default":
                fr = placeholder.icon1_frames(self.default_text, seconds=min(secs, 4.0))
                pmf, rep = clip.make_icon1(None, budget=budget, ffmpeg=ffmpeg, frames_given=fr)
            elif s["path"].lower().endswith(".pmf"):
                with open(s["path"], "rb") as f:
                    pmf = f.read()
                bad = [i for i in picon1.check(pmf) if i.level in ("error", "spec")]
                if bad:
                    raise PbpError("ICON1: %s" % bad[0].message)
                if not picon1.entry_points(pmf):
                    pmf = picon1.add_entry_points(pmf)
                return pmf, "ICON1: ready file %d bytes" % len(pmf)
            else:
                pmf, rep = clip.make_icon1(s["path"], start=s["start"], seconds=secs, box=s["box"], budget=budget,
                                           ffmpeg=ffmpeg)
            return pmf, "ICON1: %.1f s, %d kbit/s, %d bytes" % (rep["seconds"], rep["kbps"], rep["bytes"])
        if key == "snd0":
            if s["path"].lower().endswith(".at3"):
                with open(s["path"], "rb") as f:
                    data = f.read()
                bad = [i for i in pat3.check(data) if i.level in ("error", "spec")]
                if bad:
                    raise PbpError("SND0: %s" % bad[0].message)
                return data, "SND0: ready file %d bytes" % len(data)
            secs = s["seconds"] or sound.DEFAULT_SECONDS
            codec = s.get("codec") or sound.DEFAULT_CODEC
            longest = sound.max_seconds(budget, codec)
            if secs > longest:
                raise PbpError("SND0: %.1f s of music do not fit the %d KB left of the budget (at most %.1f s)" % (
                    secs, budget // 1000, longest))
            pcm = sound.shape(sound.read_pcm(s["path"], s["start"], secs, ffmpeg), loop=s["loop"])
            data, rep = sound.encode(pcm, encoder=encoder, delay=delay, loop=s["loop"], codec=codec,
                                     allow_off_spec=self.allow_off_spec)
            return data, "SND0: trimmed to %.1f s, %s, %d bytes" % (rep["seconds"], sound.CODECS[codec].label,
                                                                  rep["bytes"])
        raise PbpError("unknown slot %r" % key)

    def build(self, work_dir, ffmpeg=None, encoder=None, delay=None, only=None, log=None):
        """make every slot (or `only` these keys) into work_dir; -> Built. Never raises for one bad slot: its error is
        in Built.errors and the slot stays empty."""
        say = log or (lambda t: None)
        cache = os.path.join(work_dir, "xmb_cache")
        os.makedirs(cache, exist_ok=True)
        b = Built()
        order = ["snd0", "icon1", "icon0", "pic0", "pic1"]
        for key in order:
            if only and key not in only:
                continue
            budget = MEDIA_BUDGET - (b.sizes.get("snd0") or 0) if key == "icon1" else MEDIA_BUDGET
            fp = self.fingerprint(key, extra=str(budget) if key == "icon1" else "")
            path = os.path.join(cache, "%s-%s%s" % (key, fp, EXT[key]))
            try:
                if os.path.isfile(path):
                    data, note = open(path, "rb").read(), "%s: cached" % key
                else:
                    data, note = self.make_slot(key, budget=budget, ffmpeg=ffmpeg, encoder=encoder, delay=delay)
                    if data is not None:
                        with open(path + ".tmp", "wb") as f:
                            f.write(data)
                        os.replace(path + ".tmp", path)
                if data is not None:
                    b.files[key], b.sizes[key] = path, len(data)
                if note:
                    b.notes.append(note)
                    say(note)
            except PbpError as e:
                b.errors[key] = str(e)
                say("%s: %s" % (key, e))
        return b


class Built:
    def __init__(self):
        self.files, self.sizes, self.errors, self.notes = {}, {}, {}, []

    @property
    def media_bytes(self):
        return (self.sizes.get("icon1") or 0) + (self.sizes.get("snd0") or 0)

    def budget(self):
        """-> (used, limit, state) state 'ok' | 'warn' (over 500,000 but under 512,000) | 'over'"""
        used = self.media_bytes
        return used, MEDIA_BUDGET, ("ok" if used <= MEDIA_BUDGET else "warn" if used <= MEDIA_BUDGET_MAX else "over")

    def options(self):
        """keyword arguments for ps2rc.convert.pipeline.Options"""
        return {k: self.files.get(k) for k in SLOT_KEYS}
