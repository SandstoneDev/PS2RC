"""What the window asks the converter core (ps2rc.convert): which game is this disc, the game's settings template,
check / update an existing PSP folder. Thin adapters, so the window stays the same while the core grows; a missing
core feature raises NotAvailable with a plain sentence."""
import glob
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class NotAvailable(Exception):
    pass


def repo():
    try:
        from ps2rc.convert.pipeline import repo_root
        return repo_root()
    except Exception:          # older core: the tree this window runs from
        return ROOT


# ------------------------------------------------------------------------------------------ the disc
MANIFEST = "ps2rc_manifest.json"          # what a converted folder holds (ps2rc.convert.manifest)
FOLDER_MAX = 32


def default_folder(serial):
    """the PSP folder name the player gets: PS2RC_ + the disc's serial, ASCII letters / digits, at most 32
    characters (never a pack's internal build-folder name)"""
    s = "".join(c for c in str(serial or "") if c.isascii() and c.isalnum()).upper()
    return ("PS2RC_" + s)[:FOLDER_MAX] if s else "PS2RC_GAME"


def foreign_folder(path):
    """True when path is a folder the converter did not make (no manifest): it must not be overwritten"""
    return os.path.isdir(path) and not os.path.isfile(os.path.join(path, MANIFEST))


def free_folder_name(out, name):
    """name, or name_2, name_3 ... (at most 32 characters) that is not taken in out by a folder the converter did not
    make"""
    if not foreign_folder(os.path.join(out, name)):
        return name
    for n in range(2, 100):
        cand = "%s_%d" % (name[:FOLDER_MAX - 1 - len(str(n))], n)
        if not foreign_folder(os.path.join(out, cand)):
            return cand
    return name
def identify(source):
    """-> {'ok': bool, 'serial', 'version', 'pack', 'title', 'folder', 'code', 'message', 'hint'}"""
    from ps2rc.convert import disc as discmod
    from ps2rc.convert.identify import identify as ident_fn, IdentifyError
    out = {"ok": False, "serial": None, "version": None, "pack": None, "title": None, "folder": None, "code": None,
           "message": "", "hint": ""}
    if os.path.isdir(source):
        out.update(ok=True, code="folder", message="disc folder: identified when the conversion starts")
        return out
    try:
        d = discmod.open_disc(source)
    except discmod.DiscError as e:
        out.update(code="input", message=str(e))
        return out
    try:
        ident = ident_fn(d, repo())
        p = ident.pack
        # the menu title a player gets when he types none: neutral (no game name, no trademark); never the pack's
        # private build title
        out.update(ok=True, serial=ident.serial, version=ident.version, pack=p.name,
                   title=p.get("profile.public.title") or "PS2RC %s" % (ident.serial or p.name),
                   folder=p.get("profile.public.folder") or default_folder(ident.serial or p.name))
    except IdentifyError as e:
        out.update(code=e.code, message=e.message, hint=e.hint)
    finally:
        d.close()
    return out


# ------------------------------------------------------------------------------------------ settings template
def settings_template(pack_name, lang="en"):
    """the pack's player settings file (text) or None. PS2RC_CFG_TEMPLATE=<file> (developers / tests) overrides it;
    a file name with "_en" gets its "_<lang>" sibling for another language when that exists."""
    dev = os.environ.get("PS2RC_CFG_TEMPLATE")
    if dev and os.path.isfile(dev):
        loc = dev.replace("_en.", "_%s." % lang)
        with open(loc if os.path.isfile(loc) else dev, encoding="utf-8-sig") as f:
            return f.read()
    try:
        from ps2rc import convert
        fn = getattr(convert, "player_cfg_template", None)
        if fn:
            return fn(pack=pack_name, lang=lang)
    except Exception:          # core without the function / pack error: fall back to the pack folder
        pass
    if not pack_name:
        return None
    pdir = os.path.join(repo(), "games", pack_name)
    names = []
    try:
        from ps2rc import pack as packmod
        p = packmod.Pack(pdir)
        for key in ("runtime.cfg_template_%s" % lang, "runtime.cfg_template"):
            v = p.get(key)
            if v:
                names.append(v)
    except Exception:          # pack.toml unreadable here: try the file names
        pass
    names += ["cfg_template_%s.cfg" % lang, "cfg_template.%s.cfg" % lang, "cfg_template.cfg"]
    for n in names:
        for path in [os.path.join(pdir, n)] + glob.glob(os.path.join(pdir, "*", n)):
            if os.path.isfile(path):
                with open(path, encoding="utf-8-sig") as f:
                    return f.read()
    return None


# ------------------------------------------------------------------------------------------ check / update
def _core(name):
    """a check / update function of the core: ps2rc.convert.<name>, else ps2rc.convert.manifest.<name>"""
    try:
        from ps2rc import convert
        fn = getattr(convert, name, None)
        if fn is None:
            from ps2rc.convert import manifest
            fn = getattr(manifest, name, None)
    except ImportError:
        fn = None
    if fn is None:
        raise NotAvailable("this converter version cannot check or update folders yet")
    return fn


def verify(folder, progress=None):
    return _core("verify_folder")(folder, progress=progress)


def plan_update(folder, source=None, opts=None):
    return _core("plan_update")(folder, source, **(opts or {}))


def options_take(key):
    """does this core's Options know `key`? (newer keys: settings, folder, update ...)"""
    try:
        from ps2rc.convert.pipeline import Options
        return hasattr(Options("", "", **{key: None}), key)
    except Exception:          # an Options that refuses the dummy values: assume the old form
        return False


def report_lists(rep):
    """-> dict of the report's lists (missing / corrupt / extra / user_changed) and ok, tolerant of older shapes"""
    out = {}
    for k in ("missing", "corrupt", "extra", "user_changed"):
        out[k] = list(getattr(rep, k, None) or (rep.get(k) if isinstance(rep, dict) else None) or [])
    ok = getattr(rep, "ok", None)
    if ok is None and isinstance(rep, dict):
        ok = rep.get("ok")
    out["ok"] = bool(ok) if ok is not None else not (out["missing"] or out["corrupt"])
    files = getattr(rep, "files", None) or (rep.get("files") if isinstance(rep, dict) else None)
    out["files"] = files if isinstance(files, int) else (len(files) if files else None)
    return out


def plan_actions(plan):
    """-> [(path, action, reason, needs_disc)]"""
    acts = getattr(plan, "actions", None)
    if acts is None and isinstance(plan, dict):
        acts = plan.get("actions")
    out = []
    for a in acts or []:
        if isinstance(a, dict):
            path, action, reason = a.get("path"), a.get("action"), a.get("reason", "")
            needs = a.get("needs_disc", action == "rebuild")
        else:
            path, action, reason = (list(a) + ["", "", ""])[:3]
            needs = action == "rebuild" or (len(a) > 3 and bool(a[3]))
        out.append((path, action, reason, needs))
    return out
