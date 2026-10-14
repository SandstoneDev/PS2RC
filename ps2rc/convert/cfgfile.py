"""The run options files next to the EBOOT: the player's settings file ([runtime] cfg, a commented template per
language from the pack's [cfg] player table) and the developer / run options file <cfg>_dev<.ext> ([cfg] dev, read
FIRST by the runtime; a key in both files: the player's file wins).

Syntax (the runtime's reader, hle/core/hle_cfg.h): `key=value` lines, `#` / `;` comment lines, a `# text` after a
value is a comment, keys case-insensitive, a key given twice: the later line wins.

    render_player(template, settings)        the template with the given values put on the keys' own lines
    merge_player(old, template, old_keys)    an update: the player's values kept, new keys with their defaults,
                                             keys the new template dropped commented out
    player_cfg_template(pack, source, lang)  the template text (the GUI's settings page)
"""
import os
import re

KEY_LINE = re.compile(r"^(\s*)(#\s*)?([A-Za-z_][A-Za-z0-9_]*)(\s*=\s*)([^#\r\n]*?)(\s*(?:#.*)?)$")
OLD_SECTION = "# --- settings of an older version, not used any more (kept for reference)"
OWN_SECTION = "# --- your own additions (kept by the update)"
ADDED_SECTION = "# --- added by the converter"


def _lines(text):
    return text.replace("\r\n", "\n").split("\n")


def _nl(text):
    return "\r\n" if "\r\n" in text else "\n"


def parse(text):
    """{key (lower case): value} of the active lines (the later line wins)."""
    out = {}
    for ln in _lines(text):
        m = KEY_LINE.match(ln)
        if m and not m.group(2):
            out[m.group(3).lower()] = m.group(5).strip()
    return out


def keys(text, commented=True):
    """Keys of the template in file order (active lines, and `#key=value` lines when commented)."""
    seen, out = set(), []
    for ln in _lines(text):
        m = KEY_LINE.match(ln)
        if m and (commented or not m.group(2)):
            k = m.group(3).lower()
            if k not in seen:
                seen.add(k)
                out.append(k)
    return out


def _is_example(ln):
    """A commented `#key=value` line of a template (an optional setting), not prose that happens to contain '='."""
    m = KEY_LINE.match(ln)
    return bool(m and m.group(2) and not m.group(2).strip("#").strip() and " " not in m.group(5).strip())


def render_player(template, settings=None, extra=None):
    """The template with settings {key: value} replacing the values on the keys' own lines (the last active line of
    a key; a commented `#key=` line is uncommented when no active one exists). Keys the template lacks and the
    (key, value) pairs of `extra` are appended under one separator at the end (later line wins)."""
    nl = _nl(template)
    lines = _lines(template)
    settings = {str(k).lower(): str(v) for k, v in (settings or {}).items()}
    last_active, last_comment = {}, {}
    for i, ln in enumerate(lines):
        m = KEY_LINE.match(ln)
        if not m:
            continue
        k = m.group(3).lower()
        if not m.group(2):
            last_active[k] = i
        elif _is_example(ln):
            last_comment[k] = i
    rest = []
    for k, v in settings.items():
        i = last_active.get(k, last_comment.get(k))
        if i is None:
            rest.append((k, v))
            continue
        m = KEY_LINE.match(lines[i])
        lines[i] = "%s%s%s%s%s" % (m.group(1), m.group(3), m.group(4), v, m.group(6))
    rest += [(str(k), str(v)) for k, v in (extra or [])]
    if rest:
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", ADDED_SECTION] + ["%s=%s" % kv for kv in rest] + [""]
    return nl.join(lines)


def merge_player(old, template, old_template_keys=None, forget=()):
    """An update of the player's file: the new template (already rendered with this run's values) with the player's
    values on every key both know (active in the player's file); keys of the old template the new one dropped are
    kept commented out at the end; keys the player added himself (in no template) stay active at the end. forget:
    keys whose old value is not the player's (the converter's own earlier decisions, values given for this run).
    Returns (text, report dict)."""
    new_keys = set(keys(template))
    forget = {k.lower() for k in forget}
    old_vals = {k: v for k, v in parse(old).items() if k not in forget}
    olds = set(k.lower() for k in (old_template_keys or keys(old)))
    keep = {k: v for k, v in old_vals.items() if k in new_keys}
    text = render_player(template, keep)
    dropped = [(k, v) for k, v in old_vals.items() if k not in new_keys and k in olds]
    own = [(k, v) for k, v in old_vals.items() if k not in new_keys and k not in olds]
    nl = _nl(text)
    lines = _lines(text)
    while lines and lines[-1] == "":
        lines.pop()
    if dropped:
        lines += ["", OLD_SECTION] + ["#%s=%s" % kv for kv in dropped]
    if own:
        lines += ["", OWN_SECTION] + ["%s=%s" % kv for kv in own]
    lines.append("")
    tpl_vals = parse(template)
    changed = {k: v for k, v in keep.items() if tpl_vals.get(k) != v}
    return nl.join(lines), {"kept": sorted(changed), "added": sorted(new_keys - olds),
                            "dropped": [k for k, _v in dropped], "own": [k for k, _v in own]}


# ------------------------------------------------------------------ the pack's templates
def dev_name(cfg_name):
    stem, ext = os.path.splitext(cfg_name)
    return stem + "_dev" + ext


def pack_templates(pack):
    """({lang: path}, dev path or None, default language) of a pack's [cfg] table (empty when the pack has none)."""
    tab = pack.get("cfg", {}) or {}
    players = {k: os.path.join(pack.dir, v) for k, v in (tab.get("player") or {}).items()}
    dev = os.path.join(pack.dir, tab["dev"]) if tab.get("dev") else None
    return players, dev, tab.get("default_lang") or ("en" if "en" in players else next(iter(players), None))


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8")


def player_cfg_template(pack=None, source=None, lang="en"):
    """The player's settings template of a pack (a pack name, a loaded pack, or found from a disc image / folder),
    in `lang` (falls back to the pack's default language). None = the pack has no template."""
    from . import repo_root
    from .identify import list_packs
    pk = pack
    if pk is None and source is not None:
        from . import disc as discmod
        from .identify import identify
        d = discmod.open_disc(source, None)
        try:
            pk = identify(d, repo_root(), None).pack
        finally:
            d.close()
    if isinstance(pk, str) or pk is None:
        want = pk
        pk = None
        for name, p in list_packs(repo_root()):
            if isinstance(p, Exception):
                continue
            if want is None or name == want:
                pk = p
                if want is not None or (p.get("cfg") or {}).get("player"):
                    break
        if pk is None:
            return None
    players, _dev, default = pack_templates(pk)
    path = players.get(lang) or players.get(default)
    return read_text(path) if path else None


def languages(pack):
    return sorted(pack_templates(pack)[0])
