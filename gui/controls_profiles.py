"""Named control profiles of the converter's Controls page (no Qt). A profile is a plain text file in exactly the
ps2rc.cfg syntax, so it can be shared, edited in Notepad and pasted into ps2rc.cfg:

    # PS2RC controls profile: My layout
    # Paste these lines into ps2rc.cfg (section CONTROLS) or import the file in the PS2RC converter.
    controls_preset=stories
    map_cross=square

Only the controls keys (controls_preset, modifier, select_to_game, select_up, camera_invert, map_*, car_*). Reading checks every
line like the game does (gui/controls_model.py = the rules of hle/psp/hle_padmap.c); any unknown key, bad value or
two keys on the same buttons -> ProfileError with the line, and nothing is applied. Saved profiles live in the
converter's per-user settings folder (<settings folder>/controls_profiles/<name>.cfg), never in a game folder."""
import os
import re

import controls_model as cm

HEADER = "# PS2RC controls profile: "
HINT = "# Paste these lines into ps2rc.cfg (section CONTROLS) or import the file in the PS2RC converter."
EXT = ".cfg"
NAME_MAX = 60
WORDS = {"controls_preset": cm.PRESETS + tuple(cm.OLD_PRESET_NAMES), "modifier": cm.MODS,
         "select_to_game": ("always", "tap"), "select_up": ("stick", "r3", "l3"), "camera_invert": cm.CAM_INV}


class ProfileError(ValueError):
    pass


def clean_name(name):
    """a profile name: one line, printable, at most NAME_MAX characters; ProfileError when empty"""
    n = re.sub(r"[\x00-\x1f\x7f]", "", str(name or "")).strip()[:NAME_MAX].strip()
    if not n:
        raise ProfileError("empty name")
    return n


def to_text(name, values):
    """the file text: header + the controls keys (only the ones that differ from the built-in layout)"""
    vals = cm.Layout(values).to_values()
    order = [k for k in cm.KEYS if k in vals]
    return "\n".join([HEADER + clean_name(name), HINT] + ["%s=%s" % (k, vals[k]) for k in order]) + "\n"


def parse_text(text, fallback_name="profile"):
    """file text -> (name, values); ProfileError("line N: ...") on anything the game would not accept"""
    name, vals = None, {}
    for n, raw in enumerate(str(text).replace("\r\n", "\n").replace("\r", "\n").split("\n"), 1):
        ln = raw.strip().lstrip("﻿")
        if ln.startswith(HEADER.strip()):
            name = name or ln[len(HEADER.strip()):].strip()
            continue
        if not ln or ln[0] in "#;":
            continue
        if "=" not in ln:
            raise ProfileError("line %d: not key=value: %s" % (n, ln[:60]))
        k, v = ln.split("=", 1)
        k = k.strip().lower()
        v = re.split(r"\s[#;]", v, maxsplit=1)[0].strip()                # " # text" after a value is a comment
        if k not in cm.KEYS:
            raise ProfileError("line %d: %s is not a controls setting" % (n, k[:40]))
        if k.startswith("car_") and v.lower() in cm.UNSET_WORDS:        # car_X=foot: not set (as on foot)
            vals.pop(k, None)
            continue
        if not v:
            raise ProfileError("line %d: %s has no value" % (n, k))
        if k in WORDS and v.lower() not in WORDS[k]:
            raise ProfileError("line %d: %s=%s: use %s" % (n, k, v, ", ".join(w for w in WORDS[k]
                                                                              if w not in cm.OLD_PRESET_NAMES)))
        vals[k] = v.lower() if k in WORDS else v
    lay = cm.Layout(vals)
    for k, v in vals.items():                                    # each value as the game parses it
        if k.startswith(("map_", "car_")):
            try:
                cm.parse_value(k.split("_", 1)[1], v, lay.mod)
            except ValueError as e:
                raise ProfileError("%s=%s: %s" % (k, v, e))
    if lay.notes:
        raise ProfileError(lay.notes[0])
    try:
        nm = clean_name(name or fallback_name)
    except ProfileError:
        nm = "profile"
    return nm, vals


def _file_of(name):
    stem = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .") or "profile"
    return stem + EXT


class Store:
    """the saved profiles in one folder"""

    def __init__(self, folder):
        self.folder = folder

    def _path(self, name):
        return os.path.join(self.folder, _file_of(name))

    def names(self):
        out = []
        try:
            files = sorted(os.listdir(self.folder))
        except OSError:
            return out
        for f in files:
            if not f.lower().endswith(EXT):
                continue
            try:
                with open(os.path.join(self.folder, f), encoding="utf-8-sig") as fh:
                    nm, _v = parse_text(fh.read(), f[:-len(EXT)])
            except (OSError, ProfileError):
                continue                                     # a broken file is not offered
            out.append(nm)
        return sorted(set(out), key=str.lower)

    def exists(self, name):
        return os.path.isfile(self._path(clean_name(name)))

    def save(self, name, values):
        name = clean_name(name)
        os.makedirs(self.folder, exist_ok=True)
        tmp = self._path(name) + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(to_text(name, values))
        os.replace(tmp, self._path(name))
        return name

    def load(self, name):
        with open(self._path(clean_name(name)), encoding="utf-8-sig") as f:
            return parse_text(f.read(), name)[1]

    def delete(self, name):
        os.remove(self._path(clean_name(name)))

    def rename(self, old, new):
        new = clean_name(new)
        if self.exists(new) and _file_of(new).lower() != _file_of(clean_name(old)).lower():
            raise ProfileError("a profile named %s exists" % new)
        vals = self.load(old)
        self.delete(old)
        return self.save(new, vals)

    def free_name(self, name):
        """name, or 'name (2)', 'name (3)' ... when taken"""
        name, n, cand = clean_name(name), 2, clean_name(name)
        while self.exists(cand):
            cand = "%s (%d)" % (name[:NAME_MAX - 6], n)
            n += 1
        return cand
