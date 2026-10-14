"""Pictures from the player's own disc for the menu look (loading screens, title / splash screens, menu backgrounds).

Model A: nothing game-derived is shipped. At conversion time a provider decodes pictures from the player's disc image
into PNG files in the player's own cache folder; the window shows them as a gallery and puts the chosen one into the
crop box of a slot. The provider is the game pack's own `games/<pack>/xmb_artwork.py` (function
`artwork(disc, out_dir, log)`) when a developer checkout has one, else the generic disc_scan.Provider (it finds the
pictures by their content and knows no game). This module is generic: it knows no game, no file or texture name.

Provider contract: `artwork(disc, out_dir, log)` yields dicts {name, category, path, width, height, source} with
category one of CATEGORIES; optional attribute `SUGGEST = {slot: name or (name, crop box)}` for the one-click
"use game artwork".
"""
import hashlib
import importlib.util
import json
import os

from ps2rc.pbp.spec import PbpError

CATEGORIES = ("loading", "title", "background", "logo", "other")
_PROVIDERS = {}
INDEX = "index.json"
VERSION = 1


class Art:
    def __init__(self, name, category, path, width, height, source=""):
        self.name, self.category, self.path, self.width, self.height, self.source = (
            name, category, path, int(width), int(height), source)

    def to_dict(self):
        return dict(self.__dict__)

    def __repr__(self):
        return "Art(%s, %s, %dx%d)" % (self.name, self.category, self.width, self.height)


def find_provider(pack_dir, generic=True):
    """-> the pack's own provider module (loaded once), else the generic provider (disc_scan.Provider, one per pack
    and pack.toml version); None without a pack, or with generic=False when the pack has no provider of its own"""
    if not pack_dir:
        return None
    p = os.path.join(pack_dir, "xmb_artwork.py")
    if os.path.isfile(p):
        key = (os.path.abspath(p), os.path.getmtime(p))
        if key not in _PROVIDERS:
            _PROVIDERS[key] = _load(p)
        if _PROVIDERS[key] is not None or not generic:
            return _PROVIDERS[key]
    if not generic:
        return None
    toml = os.path.join(pack_dir, "pack.toml")
    key = (os.path.abspath(pack_dir), os.path.getmtime(toml) if os.path.isfile(toml) else 0)
    if key not in _PROVIDERS:
        from .disc_scan import Provider
        _PROVIDERS[key] = Provider(pack_dir)
    return _PROVIDERS[key]


def _load(p):
    name = "ps2rc_pack_artwork_" + hashlib.sha1(os.path.abspath(p).encode()).hexdigest()[:10]
    spec = importlib.util.spec_from_file_location(name, p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod if hasattr(mod, "artwork") else None


def _key(source, provider_file, facts_id=""):
    st = os.stat(source)
    h = hashlib.sha256(("%s|%d|%d|%d" % (os.path.abspath(source), st.st_size, int(st.st_mtime), VERSION)).encode())
    with open(provider_file, "rb") as f:
        h.update(f.read())
    h.update(facts_id.encode())
    return h.hexdigest()[:16]


def read_range(disc, f, offset, size):
    """`size` bytes at `offset` of disc file f without reading the whole file (an image: by sectors; a folder: by
    seeking the host file) - for providers that read parts of big archives"""
    if hasattr(disc, "iso") and hasattr(disc.iso, "read_sectors"):
        first = f.lsn + offset // 2048
        count = (offset % 2048 + size + 2047) // 2048
        data = disc.iso.read_sectors(first, count)
        return data[offset % 2048: offset % 2048 + size]
    if hasattr(disc, "host_path"):
        with open(disc.host_path(f), "rb") as h:
            h.seek(offset)
            return h.read(size)
    return disc.read(f)[offset:offset + size]


def disc_key(source, provider_file, facts_id=""):
    return _key(source, provider_file, facts_id)


def provider_key(source, prov):
    """the cache folder name of a disc read by a provider (its code + the pack facts it uses)"""
    return _key(source, prov.__file__, getattr(prov, "facts_id", ""))


def extract(source, out_dir, pack_dir, log=None, on_item=None, cancel=None):
    """decode the pack's artwork from the disc image / folder `source` into out_dir/<key>/ (cached) -> [Art].
    on_item(art) is called for each picture as soon as it is ready (the gallery fills in while the disc is read);
    cancel() -> True stops early (nothing is cached then)."""
    say = log or (lambda t: None)
    prov = find_provider(pack_dir)
    if prov is None:
        return []
    d = os.path.join(out_dir, provider_key(source, prov))
    idx = os.path.join(d, INDEX)
    if os.path.isfile(idx):
        try:
            with open(idx, encoding="utf-8") as f:
                rows = json.load(f)
            arts = [Art(**r) for r in rows]
            if all(os.path.isfile(a.path) for a in arts):
                for a in arts:
                    if on_item:
                        on_item(a)
                return arts
        except (OSError, ValueError, TypeError):
            pass
    from ps2rc.convert import disc as discmod
    try:
        disc = discmod.open_disc(source)
    except discmod.DiscError as e:
        raise PbpError("cannot read the disc image: %s" % e)
    os.makedirs(d, exist_ok=True)
    arts = []
    try:
        for r in prov.artwork(disc, d, say):
            a = Art(**r)
            arts.append(a)
            if on_item:
                on_item(a)
            if cancel and cancel():
                return arts
    finally:
        disc.close()
    with open(idx + ".tmp", "w", encoding="utf-8") as f:
        json.dump([a.to_dict() for a in arts], f, indent=1)
    os.replace(idx + ".tmp", idx)
    return arts


def suggest(arts, pack_dir):
    """the pack's suggested pictures per slot -> {slot: (Art, crop box or None)}"""
    prov = find_provider(pack_dir)
    want = getattr(prov, "SUGGEST", {}) if prov else {}
    by_name = {a.name: a for a in arts}
    out = {}
    for slot, v in want.items():
        name, box = (v, None) if isinstance(v, str) else v
        if name in by_name:
            out[slot] = (by_name[name], list(box) if box else None)
    if "pic1" not in out:                                  # any full-screen picture makes a background
        full = [a for a in arts if a.category in ("loading", "title")]
        if full:
            out["pic1"] = (full[0], None)
    if "icon0" not in out and "pic1" in out:               # the icon: the background's centre
        out["icon0"] = (out["pic1"][0], None)
    return out
