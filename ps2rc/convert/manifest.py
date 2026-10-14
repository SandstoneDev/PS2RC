"""The PSP folder's manifest, the folder check and the update plan.

A conversion writes ps2rc_manifest.json into the PSP folder: the converter version, the runtime / pack fingerprint,
the game (serial, version) and a disc fingerprint (hashes of the files taken from the disc, never the image), and per
file its size, SHA-256, md5 and kind:

    generated   made by the converter (EBOOT.PBP, the kernel module, the developer cfg, disc map, ...)
    game_data   copied from the disc (disc/..., the boot program)
    user        the player may edit it (the settings file): a different hash is "changed by you", not damage

Files the runtime writes into the folder (saves, logs; the pack's [stage] runtime_files) are listed by pattern: the
check never reports them and an update never touches them.

    python -m ps2rc.convert verify <PSP folder>              the folder check (no disc image needed)
    python -m ps2rc.convert update <disc image> --out DIR [--dry-run]

verify_folder() works on the memory stick itself (USB / card reader): SHA-256 of every file, read once, progress in
bytes. Missing, damaged and extra files are reported in plain words.
"""
import fnmatch
import hashlib
import json
import os
import time

MANIFEST = "ps2rc_manifest.json"
FORMAT = 1
CHUNK = 1 << 20
SKIP = ("MANIFEST.md5", MANIFEST)


class Hasher:
    """SHA-256 + md5 of one stream (both: md5 for the older MANIFEST.md5 / card tools, SHA-256 for the check)."""

    def __init__(self):
        self.s, self.m, self.n = hashlib.sha256(), hashlib.md5(), 0

    def update(self, b):
        self.s.update(b)
        self.m.update(b)
        self.n += len(b)

    def result(self):
        return self.s.hexdigest(), self.m.hexdigest()


def hash_file(path, on_bytes=None, cancel=None):
    h = Hasher()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            if on_bytes:
                on_bytes(len(b))
            if cancel:
                cancel()
    return h.result()


def rel(path, folder):
    return os.path.relpath(path, folder).replace("\\", "/")


def walk(folder):
    out = []
    for dp, _d, names in os.walk(folder):
        for n in names:
            if n.endswith(".part"):
                continue
            r = rel(os.path.join(dp, n), folder)
            if r in SKIP:
                continue
            out.append(r)
    return sorted(out)


def is_runtime_file(relpath, patterns):
    low = relpath.lower()
    return any(fnmatch.fnmatchcase(low, p.lower()) for p in patterns or ())


def load(folder):
    try:
        with open(os.path.join(folder, MANIFEST), encoding="utf-8") as f:
            m = json.load(f)
        return m if m.get("format") == FORMAT and isinstance(m.get("files"), dict) else None
    except (OSError, ValueError):
        return None


def write(folder, data):
    path = os.path.join(folder, MANIFEST)
    with open(path + ".part", "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(path + ".part", path)


def build(folder, info, kinds, cache=None, progress=None):
    """The manifest of the folder as it is now. info: the conversion's facts (converter, runtime, pack, game, disc,
    cfg, runtime_files ...); kinds(relpath) -> (kind, source); cache: a stage.Md5Cache with the hashes the stage
    computed while writing (those files are not read again)."""
    files = {}
    names = [r for r in walk(folder) if not is_runtime_file(r, info.get("runtime_files"))]
    total = sum(os.path.getsize(os.path.join(folder, r)) for r in names)
    done = 0
    for r in names:
        p = os.path.join(folder, r)
        got = cache.hashes(p) if cache else None
        if got is None:
            got = hash_file(p)
            if cache:
                cache.put(p, got[1], got[0])
        kind, source = kinds(r)
        files[r] = {"size": os.path.getsize(p), "sha256": got[0], "md5": got[1], "kind": kind}
        if source:
            files[r]["source"] = source
        done += files[r]["size"]
        if progress:
            progress.pct("manifest", done, total, "bytes")
    if cache:
        cache.save()
    data = dict(info)
    data.update(format=FORMAT, written=time.strftime("%Y-%m-%d %H:%M:%S"), files=files)
    data["disc"] = dict(data.get("disc") or {})
    game = sorted((r, v["size"], v["sha256"]) for r, v in files.items() if v["kind"] == "game_data")
    data["disc"]["data_sha256"] = hashlib.sha256(json.dumps(game).encode()).hexdigest()
    return data


def write_md5_list(folder, data):
    """MANIFEST.md5 (md5 + path; card tools and md5sum -c read it) from the manifest."""
    rows = ["%s  %s" % (v["md5"], r) for r, v in sorted(data["files"].items())]
    with open(os.path.join(folder, "MANIFEST.md5"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")


# ------------------------------------------------------------------ the folder check
class VerifyReport:
    def __init__(self, folder, manifest):
        self.folder, self.manifest = folder, manifest
        self.missing, self.corrupt, self.extra, self.user_changed = [], [], [], []
        self.files = self.bytes = self.checked = 0
        self.version = (manifest or {}).get("converter")
        self.game = (manifest or {}).get("game")
        self.no_manifest = manifest is None

    @property
    def ok(self):
        return not (self.no_manifest or self.missing or self.corrupt)

    def to_dict(self):
        return {k: getattr(self, k) for k in ("missing", "corrupt", "extra", "user_changed", "files", "bytes",
                                                "version", "ok", "no_manifest")}

    def text(self):
        if self.no_manifest:
            return ("%s has no %s: it was made by an older converter or is not a PS2RC folder. Run the converter "
                    "again (update) to write one." % (self.folder, MANIFEST))
        out = []
        g = self.game or {}
        head = "Folder %s (%s %s, converter %s): " % (self.folder, g.get("serial", "?"), g.get("version", ""),
                                                      self.version or "?")
        if self.ok:
            out.append(head + "all %d files OK (%.2f GB checked)." % (self.files, self.bytes / 1e9))
        else:
            out.append(head + "%d of %d files have a problem." % (len(self.missing) + len(self.corrupt), self.files))
        if self.missing:
            out.append("Missing (%d): %s" % (len(self.missing), _short(self.missing)))
        if self.corrupt:
            out.append("Damaged - the content differs from what the converter wrote (%d): %s"
                       % (len(self.corrupt), _short(self.corrupt)))
        if self.user_changed:
            out.append("Changed by you (fine): %s" % _short(self.user_changed))
        if self.extra:
            out.append("Not made by the converter (left alone): %s" % _short(self.extra))
        if not self.ok:
            out.append("Fix: run the converter's update - it writes only these files again (game data and the "
                       "program need your disc image; the settings files do not).")
        return "\n".join(out)


def _short(names, n=8):
    return ", ".join(names[:n]) + (" and %d more" % (len(names) - n) if len(names) > n else "")


def verify_folder(folder, progress=None, quick=False):
    """Check the folder against its manifest: every listed file present with its size and SHA-256 (quick=True: size
    only). progress: a steps.Progress (pct events, unit bytes, name "check"). No disc image needed."""
    m = load(folder)
    rep = VerifyReport(folder, m)
    if m is None:
        return rep
    files = m["files"]
    rep.files = len(files)
    total = sum(v["size"] for v in files.values())
    done = [0]

    def tick(n):
        done[0] += n
        if progress:
            progress.pct("check", done[0], total, "bytes")
    cancel = progress.check_cancel if progress else None
    for r in sorted(files):
        v = files[r]
        p = os.path.join(folder, r.replace("/", os.sep))
        if not os.path.isfile(p):
            rep.missing.append(r)
            tick(v["size"])
            continue
        size = os.path.getsize(p)
        bad = size != v["size"]
        if not bad and not quick:
            sha, _md5 = hash_file(p, tick, cancel)
            bad = sha != v["sha256"]
        else:
            tick(v["size"])
        rep.bytes += size
        if bad:
            (rep.user_changed if v.get("kind") == "user" else rep.corrupt).append(r)
    known = set(files)
    for r in walk(folder):
        if r not in known and not is_runtime_file(r, m.get("runtime_files")):
            rep.extra.append(r)
    rep.checked = done[0]
    return rep


# ------------------------------------------------------------------ the update plan
class UpdatePlan:
    """What an update would do: actions (path, action, reason, needs_disc). action: copy (from the disc), rebuild
    (the program: EBOOT / kernel module - needs the disc), write (a generated file without the disc), merge_cfg
    (the player's settings: values kept, new keys added), remove (a file an older version made), rename (an old
    settings file kept as <name>.old after its values moved to the new name), keep."""

    def __init__(self):
        self.actions = []
        self.notes = []
        self.report = None

    def add(self, path, action, reason, needs_disc=False):
        self.actions.append((path, action, reason, needs_disc))

    @property
    def needs_disc(self):
        return any(a[3] for a in self.actions)

    def changes(self):
        return [a for a in self.actions if a[1] != "keep"]

    def to_dict(self):
        return {"actions": [{"path": p, "action": a, "reason": r, "needs_disc": d} for p, a, r, d in self.actions],
                "needs_disc": self.needs_disc, "notes": self.notes}

    def text(self):
        ch = self.changes()
        if not ch:
            return "Nothing to do: the folder is complete and up to date." + \
                ("".join("\n" + n for n in self.notes))
        out = ["The update would change %d file(s)%s:" % (len(ch), " (needs your disc image)" if self.needs_disc
                                                          else "")]
        for p, a, r, _d in ch[:40]:
            out.append("  %-9s %s - %s" % (a, p, r))
        if len(ch) > 40:
            out.append("  ... and %d more" % (len(ch) - 40))
        out += self.notes
        return "\n".join(out)


def plan_from(report, expected, current_info, cfg_name=None, moves=()):
    """An update plan from a folder check and what the current converter would write.
    expected: {relpath: (kind, sha256 or None)} the current converter's output (None = not known without a build);
    current_info: {"converter", "runtime"} of this converter; moves: [(old, new)] run options files still under the
    pack's older name (cfgfile.cfg_moves)."""
    plan = UpdatePlan()
    moved_to = {new: old for old, new in moves}
    moved_from = {old: new for old, new in moves}
    plan.report = report
    m = report.manifest or {}
    old = m.get("files", {})
    newer = (m.get("runtime") != current_info.get("runtime") or m.get("converter") != current_info.get("converter"))
    if report.no_manifest:
        plan.notes.append("The folder has no manifest: every file is checked against the disc and rewritten where it "
                          "differs.")
    damaged = set(report.missing) | set(report.corrupt)
    for r, (kind, sha) in sorted(expected.items()):
        o = old.get(r)
        if kind == "user":
            if r in moved_to:
                plan.add(r, "merge_cfg", "your settings in %s carried over to its new name, new settings added"
                         % moved_to[r], False)
            elif r in damaged:
                plan.add(r, "write", "missing: written from the template", False)
            elif o is None and os.path.exists(os.path.join(report.folder, r)):
                plan.add(r, "merge_cfg", "your settings kept, new settings added", False)
            elif newer or (sha and o and o.get("template_sha256") not in (None, sha)):
                plan.add(r, "merge_cfg", "your settings kept, new settings added", False)
            else:
                plan.add(r, "keep", "your settings", False)
            continue
        if r in damaged:                                # a text the converter renders itself needs no disc
            plan.add(r, "copy" if kind == "game_data" else "rebuild" if r.upper().endswith((".PBP", ".PRX"))
                     else "write", "missing" if r in report.missing else "damaged", sha is None)
        elif o is None:
            plan.add(r, "copy" if kind == "game_data" else "write", "new in this converter version",
                     kind == "game_data" or sha is None)
        elif sha is not None and o.get("sha256") != sha:
            plan.add(r, "copy" if kind == "game_data" else "write", "changed in this converter version",
                     kind == "game_data")
        elif sha is None and newer and kind != "game_data":
            why = ("made by converter %s, this is %s" % (m.get("converter"), current_info.get("converter"))
                   if m.get("converter") != current_info.get("converter") else
                   "the runtime or the game pack changed since it was made")
            plan.add(r, "rebuild", why, True)
        else:
            plan.add(r, "keep", "OK", False)
    for r in sorted(old) + sorted(set(moved_from) - set(old)):
        if r in moved_from:
            ok = (old.get(r) or {}).get("kind") == "generated" and r not in damaged
            plan.add(r, "remove" if ok else "rename", "replaced by %s" % moved_from[r] if ok else
                     "replaced by %s; kept as %s.old" % (moved_from[r], r), False)
        elif r not in expected:
            plan.add(r, "remove", "an older version made it; not used any more", False)
    return plan


def plan_update(folder, source=None, progress=None, repo=None, **opts):
    """What an update of the PSP folder would change (nothing is written). With the disc image (source) the plan is
    exact (the converter's whole recipe); without it, from the folder check and the version stamps: actions with
    needs_disc tell the GUI to ask for the image."""
    if source:
        from .pipeline import Conversion, Options
        o = Options(source, os.path.dirname(os.path.abspath(folder)), folder=folder, update=True, dry_run=True,
                    repo=repo, **opts)
        return Conversion(o, progress).run()["plan_obj"]
    rep = verify_folder(folder, progress)
    m = rep.manifest or {}
    exp = {r: (v.get("kind", "generated"), None if v.get("kind") != "user" else v.get("sha256"))
           for r, v in m.get("files", {}).items()}
    cur = {"converter": None, "runtime": None}
    moves = []
    try:
        from . import VERSION, repo_root
        from .identify import list_packs
        from .pipeline import runtime_fingerprint
        cur["converter"] = VERSION
        name = (m.get("pack") or {}).get("name")
        root = repo or repo_root()
        for n, pk in list_packs(root):
            if n == name and not isinstance(pk, Exception):
                cur["runtime"] = runtime_fingerprint(root, pk, (m.get("options") or {}).get("profile"))
                from .cfgfile import cfg_moves, pack_templates, dev_name, read_text
                moves = cfg_moves(folder, pk.get("runtime.cfg") or "ps2rc.cfg", pk.get("runtime.cfg_legacy"))
                _pl, dev, _dl = pack_templates(pk)              # the developer file: known text, no disc needed
                dn = dev_name(pk.get("runtime.cfg") or "ps2rc.cfg")
                if dev and dn in exp:
                    exp[dn] = (exp[dn][0], hashlib.sha256(read_text(dev).encode("utf-8")).hexdigest())
    except Exception:
        pass
    for o, nw in moves:                            # the current converter writes them under the new name
        kind, sha = exp.pop(o, ("generated", None))
        exp[nw] = (kind, None)
    plan = plan_from(rep, exp, cur, moves=moves)
    try:
        from . import repo_root
        from .hostpaths import tools_line
        plan.notes.append(tools_line(repo or repo_root()))
    except Exception:
        pass
    if cur["runtime"] is None and m:
        plan.notes.append("The game pack %s is not installed in this converter: the update needs it."
                          % (m.get("pack") or {}).get("name"))
    return plan
