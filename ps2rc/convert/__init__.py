"""PS2RC converter: the player's own PS2 disc image -> a PSP game folder, entirely on the player's PC.

    python -m ps2rc.convert <disc.iso | disc folder> --out DIR [options]      (see __main__.py / README.md)

Modules: iso9660 (own ISO9660 reader), disc (disc map + metadata sectors), identify (disc -> game pack), recipe
(the pack's conversion recipe), steps (resumable steps + progress), toolchain (finding psp-gcc: bundled, PATH, WSL,
Docker), build (compiling and linking without make/sh), stage (output folder), pipeline (the whole run), estimate.
"""
import os
import sys

VERSION = "0.2.0"
FOLDER_MAX = 32


def default_folder(serial):
    """The PSP folder name a conversion gets by default (the window and the command line alike): PS2RC_ + the disc's
    serial, ASCII letters / digits only, at most 32 characters (e.g. PS2RC_SLUS12345 for SLUS-12345)."""
    s = "".join(c for c in str(serial or "") if c.isascii() and c.isalnum()).upper()
    return ("PS2RC_" + s)[:FOLDER_MAX] if s else "PS2RC_GAME"


def repo_root():
    """The checkout / bundle root: the first parent of this package that holds games/ and the core package
    (src/ps2rc today, ps2rc/ after the layout move)."""
    d = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if os.path.isdir(os.path.join(d, "games")):
        return d
    raise RuntimeError("ps2rc.convert: no games/ folder next to %s" % d)


def _bridge():
    """Make the core modules (pack, config, bind, ...) importable as ps2rc.<name> whether the core package still lives
    in src/ps2rc (today) or in ps2rc/ (after the move): the converter package sits in <root>/ps2rc/."""
    import ps2rc
    root = repo_root()
    for d in (os.path.join(root, "src", "ps2rc"), os.path.join(root, "ps2rc")):
        if os.path.isdir(d) and d not in list(ps2rc.__path__):
            try:
                ps2rc.__path__.append(d)
            except AttributeError:                    # very old namespace path objects
                ps2rc.__path__ = list(ps2rc.__path__) + [d]
    src = os.path.join(root, "src")
    if os.path.isdir(os.path.join(src, "ps2rc")) and src not in sys.path:
        sys.path.append(src)                          # scripts run in-process import "ps2rc" from src/


_bridge()


def player_cfg_template(pack=None, source=None, lang="en"):
    """The pack's player settings template (text) in `lang`; None = the pack has none (see cfgfile)."""
    from .cfgfile import player_cfg_template as f
    return f(pack, source, lang)


def verify_folder(folder, progress=None, quick=False):
    """Check a PSP folder against its ps2rc_manifest.json (see manifest.verify_folder)."""
    from .manifest import verify_folder as f
    return f(folder, progress, quick)


def plan_update(folder, source=None, progress=None, **opts):
    """What an update would change (see manifest.plan_update)."""
    from .manifest import plan_update as f
    return f(folder, source, progress, **opts)
