"""One conversion: disc -> identify -> extract -> analysis -> translation -> extras -> modules -> prepare -> compile ->
link -> kernel PRX -> EBOOT -> stage -> movies -> verify. Each step is skipped when its record says its inputs are
unchanged and its outputs are still there (steps.StepRecord)."""
import json
import os
import shutil
import sys
import time

from . import VERSION, repo_root
from . import cfgfile
from . import hostpaths
from . import disc as discmod
from . import manifest as mf
from . import modules as modmod
from .build import BuildOptions, Builder
from .estimate import estimate, check_disk
from .identify import IdentifyError, identify
from .recipe import Recipe, RecipeError
from .stage import README, Md5Cache, Stager, write_cfg
from .steps import (ConvertError, Progress, StepRecord, fingerprint, run_logged, script_cmd, sha_file,
                    tree_hash)
from . import toolchain as tcmod
from .prebuilt import stores as prebuilt_stores

MB_PER_TRANSLATE_JOB = 1200     # memory of one translation worker (I: image + function set + analysis results)

STEPS = [("identify", "Identify the disc"), ("extract", "Read the boot program, overlays, disc map"),
         ("analysis", "Analyse the program (functions)"), ("translate", "Translate the program to C"),
         ("extras", "Overlay constructors, trap set"), ("modules", "Game modules from generators"),
         ("prepare", "Prepare the C units"), ("compile", "Compile (psp-gcc)"), ("link", "Link"),
         ("prx", "Kernel helper module"), ("eboot", "EBOOT.PBP with your menu assets"),
         ("stage", "Copy the game data into the PSP folder"), ("movies", "Movies"), ("verify", "Verify the folder")]


def code_hash(repo):
    """The translator and the pack's analysis scripts (a change re-runs the analysis / translation steps)."""
    parts = []
    for rel in ("src/ps2rc", "tests/phase3", "tests/phase4", "tests/phase4b", "tests/poc6", "tests/phase6psp",
                "tests/refbins.py", "ps2rc/pipeline", "ps2rc/analysis", "ps2rc/r5900", "ps2rc/vu", "ps2rc/backend_c.py",
                "ps2rc/ir.py", "ps2rc/image.py", "ps2rc/overlay.py", "ps2rc/bind.py", "ps2rc/pack.py"):
        parts.append(tree_hash(os.path.join(repo, rel), exts=(".py",)))
    parts.append(VERSION)
    return fingerprint(parts)


def runtime_fingerprint(repo, pack, profile=None):
    """What the program is built from: the translator, the runtime sources, the pack (its files and recipe; not its
    tests). Stored in the folder's manifest: a different value = the update rebuilds the program."""
    return fingerprint(code_hash(repo), [tree_hash(os.path.join(repo, d)) for d in
                                         ("hle", "runtime", "audio", "psp_exc", "src/hle", "src/runtime")],
                       tree_hash(pack.dir, skip_dirs=("__pycache__", ".git", ".pytest_cache", "tests")), profile)


class Options:
    def __init__(self, source, out, **kw):
        self.source = source
        self.out = out
        self.work = kw.get("work")
        self.pack = kw.get("pack")
        self.title = kw.get("title")
        self.icon0, self.icon1, self.pic0, self.pic1, self.snd0 = (kw.get(k) for k in ("icon0", "icon1", "pic0",
                                                                                       "pic1", "snd0"))
        self.fit = kw.get("fit", "cover")
        self.movies = kw.get("movies", "auto")              # auto | skip | convert
        self.audio = kw.get("audio", "all")                 # all | min | none
        self.jobs = kw.get("jobs") or os.cpu_count() or 4
        self.toolchain = kw.get("toolchain")
        self.toolchain_prefer = kw.get("toolchain_prefer")
        self.allow_docker_pull = kw.get("allow_docker_pull", False)
        self.layout = kw.get("layout")
        self.splash = kw.get("splash", True)
        self.build_num = kw.get("build_num", 0)
        self.iso_meta = kw.get("iso_meta", "synthetic")
        self.disc_id = kw.get("disc_id")
        self.translation = kw.get("translation")            # developer: reuse a translation folder
        self.restart = kw.get("restart", False)
        self.cfg = kw.get("cfg") or []
        self.ffmpeg = kw.get("ffmpeg")
        self.manifest = kw.get("manifest", True)
        self.estimate_only = kw.get("estimate_only", False)
        self.prx = kw.get("prx", "build")                   # build | skip | <path of a ready ps2rc_exc.prx>
        self.stop_after = kw.get("stop_after")
        self.repo = kw.get("repo")                          # tests / development: another checkout
        self.allow_gpl_libs = kw.get("allow_gpl_libs", False)
        self.cold_opt = kw.get("cold_opt", "")             # "" / "O1" / "Os": cold functions at a cheaper level
        self.prebuilt = kw.get("prebuilt")                 # an extra prebuilt object store
        self.lang = kw.get("lang")                         # language of the player's settings file (None: the pack's
        #                                                    default; an update keeps the folder's)
        self.dev_cfg = kw.get("dev_cfg", True)             # also write the developer / run options file
        self.settings = dict(kw.get("settings") or {})     # player settings {name: value} put on the template's lines
        self.update = kw.get("update", False)              # update / repair an existing folder (keeps the player's
        #                                                    settings and saves, rewrites only what is missing,
        #                                                    damaged or out of date)
        self.dry_run = kw.get("dry_run", False)            # update: only list what would change
        self.verify_writes = kw.get("verify_writes")       # read every written file back (None: on for an update)
        self.force = kw.get("force", False)                # update: allow another game version than the folder's
        self.profile = kw.get("profile")                   # pack build profile (None: the pack's default)
        self.folder = kw.get("folder")                     # the PSP folder itself (default: OUT/<the pack's folder>)


class Conversion:
    def __init__(self, opts, progress=None):
        self.o = opts
        self.p = progress or Progress()
        self.repo = os.path.abspath(opts.repo) if getattr(opts, "repo", None) else repo_root()
        self.result = {}

    # --- environment for the checkout's scripts -------------------------------------------------------------------
    def _setup_env(self, pack_name):
        o = self.o
        self.work = os.path.abspath(o.work) if o.work else hostpaths.default_work(o.out)
        if not hostpaths.ansi_safe(self.work):
            raise ConvertError("path", "the work folder %s has letters this Windows cannot pass to the PSP toolchain"
                               % self.work, "give --work with a folder whose path has only plain letters (A-Z), "
                               "digits and spaces, e.g. a folder PS2RC_work at the top of a drive")
        self.temp_moved = hostpaths.safe_temp(self.work)    # the toolchain's LTO / assembler need an ASCII TEMP
        self.disc_dir = os.path.join(self.work, "disc")
        self.analysis = os.path.join(self.work, "analysis")
        for d in (self.work, self.disc_dir, self.analysis):
            os.makedirs(d, exist_ok=True)
        local = os.path.join(self.work, "local.toml")
        with open(local, "w", encoding="utf-8", newline="\n") as f:
            f.write("# written by the PS2RC converter: this conversion's folders\n[paths]\nwork = %s\nanalysis = %s\n"
                    "\n[defaults]\npack = %s\n\n[games.%s]\ndisc = %s\n"
                    % (json.dumps(self.work.replace("\\", "/")), json.dumps(self.analysis.replace("\\", "/")),
                       json.dumps(pack_name), json.dumps(pack_name), json.dumps(self.disc_dir.replace("\\", "/"))))
        env = {"PS2RC_WORK": self.work, "PS2RC_DISC": self.disc_dir, "PS2RC_ANALYSIS": self.analysis,
               "PS2RC_PACK": pack_name, "PS2RC_LOCAL": local, "PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1",
               "PYTHONUNBUFFERED": "1"}
        os.environ.update(env)                              # in-process pack path expansion uses the same values
        from ps2rc import config
        config._LOCAL = None
        self.env = dict(os.environ)
        self.env["PYTHONPATH"] = os.pathsep.join([os.path.join(self.repo, "src"), self.repo] +
                                                 ([os.environ["PYTHONPATH"]] if os.environ.get("PYTHONPATH") else []))
        self.log = os.path.join(self.work, "logs", "convert.log")
        self.rec = StepRecord(self.work)
        if o.restart:
            for name, _t in STEPS:
                self.rec.forget(name)

    def _ctx(self):
        return {"work": self.work, "repo": self.repo, "disc": self.disc_dir, "analysis": self.analysis,
                "translation": self.translation}

    def _cmd(self, argv):
        a = [self.recipe.expand(x, self._ctx()) for x in argv]
        if a and a[0].endswith(".py"):                 # the script's folder on sys.path, also under the app's
            a = script_cmd(a[0], *a[1:])                 # isolated Python (steps.RUNSCRIPT)
        return a

    # --- the run -----------------------------------------------------------------------------------------------
    ENV_KEYS = ("PS2RC_WORK", "PS2RC_DISC", "PS2RC_ANALYSIS", "PS2RC_PACK", "PS2RC_LOCAL", "PYTHONIOENCODING",
                "PYTHONUTF8", "PYTHONUNBUFFERED", "TEMP", "TMP")

    def run(self):
        """The whole conversion; this process's environment is restored afterwards (the GUI may run several)."""
        saved = {k: os.environ.get(k) for k in self.ENV_KEYS}
        try:
            return self._run()
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            try:
                from ps2rc import config
                config._LOCAL = None
            except Exception:
                pass
            disc = getattr(self, "disc", None)
            if disc is not None:
                disc.close()

    def _run(self):
        o, p = self.o, self.p
        t_all = time.time()
        if not hostpaths.is_ascii(self.repo):
            raise ConvertError("install_path", "the converter is in %s: its path has letters other than plain A-Z, "
                               "digits and spaces, and the PSP toolchain cannot work from there" % self.repo,
                               "move the whole converter folder to a plain path, e.g. PS2RC_Converter at the top "
                               "of a drive, and start "
                               "it from there")
        os.makedirs(o.out, exist_ok=True)
        dev_analysis = None
        try:
            from ps2rc import config
            dev_analysis = config.analysis_dir()            # the checkout's own analysis dir (before isolating)
        except Exception:
            pass
        # 1 identify (always: cheap, and it decides the pack)
        self._step_begin("identify")
        t0 = time.time()
        layout = o.layout
        self.disc = self._open_disc(layout, dev_analysis)
        try:
            ident = identify(self.disc, self.repo, o.pack)
        except IdentifyError as e:
            raise ConvertError(e.code, e.message, e.hint)
        self.ident, self.pack = ident, ident.pack
        self._setup_env(self.pack.name)
        p.open_log(self.log)
        self.recipe = Recipe(self.pack, dev_analysis)
        self.translation = os.path.abspath(o.translation) if o.translation else self.pack.path("build.translation")
        p.info("disc: %s (%s), %s version %s, %s; boot program %s, %d bytes, SHA-256 %s"
               % (self.disc.label, self.disc.kind, ident.serial, ident.version, ident.region, ident.boot,
                  ident.elf_size, ident.elf_sha256))
        p.info("game pack: %s (recipe sections: %s)" % (self.pack.name, ", ".join(
            "%s from %s" % (k, v) for k, v in self.recipe.source.items() if v)))
        if getattr(ident, "warning", None):
            p.warn(ident.warning)
        if not self.recipe.get("convert.translate"):
            raise ConvertError("pack", "pack %s has no conversion recipe ([convert] in pack.toml or convert.toml)"
                               % self.pack.name, "this pack cannot be converted by this converter version")
        self.folder = self.recipe.get("stage.folder") or self.pack.get("runtime.psp_module") or "PS2RC"
        self.folder_dir = os.path.abspath(o.folder) if getattr(o, "folder", None) else             os.path.join(os.path.abspath(o.out), self.folder)
        self.folder = os.path.basename(self.folder_dir)
        self.md5cache = Md5Cache(os.path.join(self.work, "stage_md5.json"))
        vw = o.verify_writes if o.verify_writes is not None else bool(o.update)
        self.stager = Stager(self.disc, self.recipe, self.folder_dir, p, self.md5cache, verify_writes=vw)
        self.code_hash = self._code_hash()
        self._step_end("identify", t0, False)
        if o.update or o.dry_run:
            plan = self._update_check()
            if o.dry_run:
                self.result.update(plan=plan.to_dict(), plan_text=plan.text(), report=plan.report.to_dict(),
                                   plan_obj=plan)
                p.info(plan.text())
                return self.result
        # toolchain + ffmpeg + estimates
        want = self.recipe.get("convert.build.psp_gcc")
        self.tc, tried = tcmod.detect(o.toolchain, o.toolchain_prefer, mount_root=self.work,
                                      allow_docker_pull=o.allow_docker_pull, log=p.info, want=want)
        for t in tried:
            p.log("toolchain search: " + t)
        root = getattr(self.tc, "root", None)
        if self.tc.kind == "native" and root and not hostpaths.is_ascii(root):
            raise ConvertError("install_path", "the PSP toolchain is in %s: its path has letters other than plain "
                               "A-Z, digits and spaces (its linker cannot open files there)" % root,
                               "move the converter folder (or --toolchain) to a plain path, e.g. PS2RC_Converter at "
                               "the top of a drive")
        if getattr(self, "temp_moved", None):
            p.info("TEMP for the toolchain: %s (the user's TEMP has letters the PSP linker cannot read)"
                   % self.temp_moved)
        if want and self.tc.version != want:
            p.warn("psp-gcc %s found, the pack was checked with %s: the program is built, its code differs from the "
                   "checked one" % (self.tc.version, want))
        self.ffmpeg = self._find_ffmpeg()
        est = estimate(self)
        p.info(est["text"])
        self.result["estimate"] = est
        check_disk(est, self.folder_dir, self.work)
        if o.estimate_only:
            return self.result
        # 2..
        self._extract()
        self._analysis()                                   # the extras read the function lists too
        if self._stop("analysis"):
            return self.result
        if not o.translation:
            self._translate()
        else:
            p.warn("using the given translation %s (translation step skipped)" % self.translation)
        if self._stop("translate"):
            return self.result
        self._extras()
        if self._stop("extras"):
            return self.result
        gens = self._modules()
        bopts = self._build_options()
        self.builder = Builder(self.repo, self.pack, os.path.join(self.work, "build"), self.tc, bopts, p, self.env,
                               os.path.join(self.work, "logs", "build.log"))
        self.builder.generated = gens
        self._prepare(bopts)
        if self._stop("prepare"):
            return self.result
        self._compile(bopts)
        if self._stop("compile"):
            return self.result
        elf, strip = self._link()
        if self._stop("link"):
            return self.result
        prx = self._prx()
        eboot = self._eboot(strip)
        if self._stop("eboot"):
            return self.result
        self._stage(eboot, prx)
        self._movies()
        self._verify()
        total = time.time() - t_all
        p.info("DONE in %.0f s. Copy the folder %s to PSP/GAME/ on the memory stick." % (total, self.folder_dir))
        self.result.update(folder=self.folder_dir, seconds=total)
        json.dump(self.result, open(os.path.join(self.work, "result.json"), "w"), indent=1, default=str)
        return self.result

    def _stop(self, name):
        if self.o.stop_after == name:
            self.p.info("stopped after step %s (--stop-after)" % name)
            return True
        return False

    def _open_disc(self, layout, dev_analysis):
        src = self.o.source
        if os.path.isdir(src) and not layout:
            # a folder: the layout comes from the pack whose disc this is (SYSTEM.CNF first, then [disc] layout)
            layout = self._pack_layout_for_dir(src, dev_analysis)
        try:
            return discmod.open_disc(src, layout)
        except discmod.DiscError as e:
            raise ConvertError("input", str(e), "choose the .iso image of your disc (or its root folder + a layout)")

    def _pack_layout_for_dir(self, src, dev_analysis):
        from .identify import list_packs
        from . import iso9660
        cnf_path = os.path.join(src, "SYSTEM.CNF")
        if not os.path.exists(cnf_path):
            for n in os.listdir(src):
                if n.upper() == "SYSTEM.CNF":
                    cnf_path = os.path.join(src, n)
        if not os.path.exists(cnf_path):
            raise ConvertError("not_ps2", "%s: no SYSTEM.CNF in this folder" % src, "choose the disc's root folder")
        boot = iso9660.boot_file(iso9660.parse_system_cnf(open(cnf_path, "rb").read()).get("BOOT2", ""))
        for _n, pk in list_packs(self.repo):
            if isinstance(pk, Exception):
                continue
            if iso9660.boot_file(pk.get("verify.system_cnf_boot2", "")).upper() == boot.upper():
                rel = Recipe(pk, dev_analysis).get("disc.layout")
                if rel:
                    path, dev = Recipe(pk, dev_analysis).pack_file(rel)
                    if path:
                        if dev:
                            self.p.warn("disc layout taken from the developer analysis folder: %s" % path)
                        return path
        raise ConvertError("input", "%s: a disc folder needs the disc's sector layout, and no pack has one for %s"
                           % (src, boot), "use the disc image (.iso), or give --layout FILE")

    def _runtime_fp(self):
        if getattr(self, "_rt_fp", None) is None:
            self._rt_fp = runtime_fingerprint(self.repo, self.pack, self._profile()[0])
        return self._rt_fp

    def _expected_files(self):
        """{relpath: (kind, sha256 or None)} of what this conversion writes (sha known only for text it renders)."""
        exp = {}
        boot_name = self.pack.get("id.boot_elf") or os.path.basename(self.ident.boot)
        for r in ("EBOOT.PBP", "discmap.txt", "iso_meta.bin", "disc_files.txt", "README_PS2RC.txt"):
            exp[r] = ("generated", None)
        exp[boot_name] = ("game_data", None)
        if self.o.prx != "skip":
            exp["ps2rc_exc.prx"] = ("generated", None)
        import hashlib
        for name, (text, kind) in self._cfg_files(self._movies_planned()).items():
            exp[name] = (kind, hashlib.sha256(text.encode("utf-8")).hexdigest())
        sel = self.stager.selection(self.o.audio if self.o.audio != "none" else None, self.ident.boot)
        for f in sel:
            exp["disc/" + f.rel] = ("game_data", None)
        if self._movies_planned():
            old = (getattr(self, "old_manifest", None) or {}).get("files", {})
            for r in old:
                if r.startswith("movies/"):
                    exp[r] = ("generated", None)
        return exp

    def _movies_planned(self):
        if not hasattr(self, "ffmpeg"):                  # the update check runs before the tool search
            self.ffmpeg = self._find_ffmpeg()
        ok, _why = self.movies_possible()
        return ok and self.o.movies != "skip"

    def _update_check(self):
        """Update / repair: check the existing folder against its manifest; damaged and missing files are written
        again by the stage (files that pass are trusted and not copied again). -> UpdatePlan."""
        p, fd = self.p, self.folder_dir
        self.old_manifest = mf.load(fd) if os.path.isdir(fd) else None
        old = self.old_manifest
        if old:
            g = old.get("game") or {}
            if (g.get("serial"), g.get("version")) != (self.ident.serial, self.ident.version) and not self.o.force:
                raise ConvertError("update", "the folder %s holds %s %s, this disc is %s %s" % (
                    fd, g.get("serial"), g.get("version"), self.ident.serial, self.ident.version),
                    "choose the disc the folder was made from, or another output folder (--force overwrites)")
            p.info("update: checking %d files of %s (made by converter %s) ..." % (len(old["files"]), fd,
                                                                                old.get("converter")))
        elif os.path.isdir(fd):
            p.info("update: %s has no manifest - every file is compared by size and written again where it differs"
                   % fd)
        rep = mf.verify_folder(fd, p) if os.path.isdir(fd) else mf.VerifyReport(fd, None)
        if old:
            p.info(rep.text())
        plan = mf.plan_from(rep, self._expected_files(), {"converter": VERSION, "runtime": self._runtime_fp()})
        plan.notes.append(hostpaths.tools_line(self.repo, self.o.ffmpeg))
        if self.o.dry_run:
            return plan
        if old:
            bad = set(rep.missing) | set(rep.corrupt)
            self.stager.trust = {os.path.normcase(os.path.abspath(os.path.join(fd, r.replace("/", os.sep))))
                                 for r in old["files"] if r not in bad}
            for r in rep.corrupt:                       # damaged copies go; the stage writes them again
                try:
                    os.remove(os.path.join(fd, r.replace("/", os.sep)))
                except OSError:
                    pass
        for name in ("stage", "movies"):                # the stage runs again (it keeps what is right)
            self.rec.forget(name)
        self.update_report = rep
        self.update_plan = plan
        return plan

    def _code_hash(self):
        return code_hash(self.repo)

    def _find_ffmpeg(self):
        """--ffmpeg, FFMPEG, the converter app's tools/ffmpeg/, the tree's tools/bin/ffmpeg/, PATH (hostpaths)"""
        return hostpaths.find_ffmpeg(self.repo, self.o.ffmpeg)

    # --- step helpers --------------------------------------------------------------------------------------------
    def _idx(self, name):
        return [n for n, _t in STEPS].index(name) + 1

    def _step_begin(self, name):
        self.p.check_cancel()
        self.p.emit("step", self._idx(name), len(STEPS), name, dict(STEPS)[name])

    def _step_end(self, name, t0, cached):
        self.p.emit("done", name, time.time() - t0, cached)

    def _cached(self, name, fp):
        self._step_begin(name)
        if self.rec.valid(name, fp):
            self.p.log("%s: unchanged since the last run (kept)" % name)
            self._step_end(name, time.time(), True)
            return True
        return False

    def _save(self, name, fp, outputs, t0, extra=None):
        self.rec.save(name, fp, outputs, time.time() - t0, extra)
        self._step_end(name, t0, False)

    # --- steps ---------------------------------------------------------------------------------------------------
    def _extract(self):
        d = self.disc
        boot_name = self.pack.get("id.boot_elf") or os.path.basename(self.ident.boot)
        overlays = list(self.pack.get("overlays.files", []) or [])
        listing = fingerprint([(f.lsn, f.size, f.iso) for f in d.files])
        fp = fingerprint("extract", self.ident.elf_sha256, boot_name, overlays, self.o.iso_meta, listing,
                         d.volume, VERSION)
        outs = [os.path.join(self.disc_dir, boot_name)] + [os.path.join(self.disc_dir, n) for n in overlays] + \
            [os.path.join(self.work, "discmap.txt"), os.path.join(self.work, "iso_meta.bin")]
        if self._cached("extract", fp):
            return
        t0 = time.time()
        f = d.find(self.ident.boot)
        d.copy_to(f, os.path.join(self.disc_dir, boot_name))
        for n in overlays:
            e = d.find(n)
            if e is None:
                raise ConvertError("extract", "overlay %s of the pack is not on the disc" % n)
            d.copy_to(e, os.path.join(self.disc_dir, n))          # named as the pack names it
        with open(outs[-2], "w", encoding="ascii", newline="\n") as fh:
            fh.write(d.discmap_text())
        try:
            meta = discmod.iso_meta(d, self.o.iso_meta)
        except discmod.DiscError as e:
            raise ConvertError("extract", str(e))
        with open(outs[-1], "wb") as fh:
            fh.write(meta)
        self.p.log("extract: %s + %d overlays; disc map %d files; ISO9660 metadata %s, sectors 0-%d"
                   % (boot_name, len(overlays), len(d.files), self.o.iso_meta, len(meta) // 2048 - 1))
        self._save("extract", fp, outs, t0)

    def _analysis(self):
        cmds = self.recipe.get("convert.analysis", [])
        outs = [self.recipe.expand(x, self._ctx()) for x in self.recipe.get("convert.analysis_outputs", [])]
        fp = fingerprint("analysis", self.ident.elf_sha256, self.code_hash, cmds,
                         [sha_file(os.path.join(self.disc_dir, n)) for n in self.pack.get("overlays.files", [])])
        self.fp_analysis = fp
        if self._cached("analysis", fp):
            return
        t0 = time.time()
        for c in cmds:
            run_logged(self._cmd(c), self.repo, self.env, os.path.join(self.work, "logs", "analysis.log"), self.p,
                       "analysis")
        missing = [x for x in outs if not os.path.exists(x)]
        if missing:
            raise ConvertError("analysis", "the analysis did not write %s" % ", ".join(missing))
        self._save("analysis", fp, outs, t0)

    def _pack_inputs_hash(self):
        h = []
        for key in ("build.hooks", "build.zones", "build.natives", "build.textpatch"):
            v = self.pack.path(key) if self.pack.get(key) else None
            h.append(tree_hash(v) if v else None)
        lay = self.pack.path("build.layout") if self.pack.get("build.layout") else None
        h.append(tree_hash(lay) if lay else None)
        h.append(tree_hash(os.path.join(self.pack.dir, "pack.toml")))
        h.append(tree_hash(os.path.join(self.pack.dir, "convert.toml")))
        local = os.path.join(self.pack.dir, "pack.private.toml")
        if os.path.exists(local):                          # a developer checkout's local additions
            h.append(tree_hash(local))
        return fingerprint(h)

    def _translate(self):
        cmd = self.recipe.get("convert.translate")
        fp = fingerprint("translate", getattr(self, "fp_analysis", None), self.code_hash, cmd,
                         self._pack_inputs_hash())
        if self._cached("translate", fp):
            return
        t0 = time.time()
        if os.path.isdir(self.translation):
            shutil.rmtree(self.translation)
        env = dict(self.env)
        # parallel narrowing + C emission (ps2rc.pipeline.parallel): every worker holds the program and the
        # analysis results (MB_PER_TRANSLATE_JOB each), so the count follows the free memory as well as the CPUs
        avail = tcmod._avail_mb_native()
        nj = max(1, min(int(self.o.jobs), (avail // MB_PER_TRANSLATE_JOB) if avail else int(self.o.jobs)))
        if os.environ.get("PS2RC_TRANSLATE_JOBS", "").isdigit():      # developer override (1 = the serial loops)
            nj = max(1, int(os.environ["PS2RC_TRANSLATE_JOBS"]))
        env["PS2RC_TRANSLATE_JOBS"] = str(nj)
        self.p.log("translate: %d processes" % nj)
        run_logged(self._cmd(cmd), self.repo, env, os.path.join(self.work, "logs", "translate.log"), self.p,
                   "translate")
        if not os.path.exists(os.path.join(self.translation, "stats.json")):
            raise ConvertError("translate", "the translation wrote no stats.json in %s" % self.translation)
        self._save("translate", fp, [self.translation], t0)

    def _extras(self):
        cmds = self.recipe.get("convert.extras", [])
        files = self.recipe.get("convert.analysis_files", [])
        srcs = []
        for rel in files:
            path, dev = self.recipe.pack_file(rel)
            if not path:
                raise ConvertError("pack", "pack %s: %s is missing" % (self.pack.name, rel),
                                   "the pack is incomplete")
            if dev:
                self.p.warn("%s taken from the developer analysis folder %s (not in the pack yet)" % (rel, path))
            srcs.append(path)
        fp = fingerprint("extras", getattr(self, "fp_analysis", None), self.code_hash, cmds,
                         [sha_file(s) for s in srcs], self.o.translation)
        outs = [self.recipe.expand(x, self._ctx()) for x in self.recipe.get("convert.extras_outputs", [])]
        if self._cached("extras", fp):
            return
        t0 = time.time()
        for s in srcs:
            shutil.copyfile(s, os.path.join(self.analysis, os.path.basename(s)))
        for c in cmds:
            run_logged(self._cmd(c), self.repo, self.env, os.path.join(self.work, "logs", "extras.log"), self.p,
                       "extras")
        self._save("extras", fp, outs, t0)

    def _modules(self):
        self._step_begin("modules")
        t0 = time.time()
        made = modmod.run(self.recipe, self.disc, self.work, self.repo, self.env,
                          os.path.join(self.work, "logs", "modules.log"), self.p, self._profile())
        self._step_end("modules", t0, False)
        return made

    def _build_options(self):
        r = self.recipe
        return BuildOptions(profile=self._profile()[1], build_num=int(self.o.build_num), splash=bool(self.o.splash),
                            audio_model=bool(r.get("convert.build.audio_model", True)) and self.o.audio != "none",
                            alias=self.pack.get("build.alias") or r.get("convert.build.alias", "call"),
                            y_hdr=self.pack.get("build.y_hdr", r.get("convert.build.y_hdr", "")),
                            y_gen=self.pack.get("build.y_gen", r.get("convert.build.y_gen", "")), jobs=int(self.o.jobs),
                            allow_gpl_libs=bool(getattr(self.o, "allow_gpl_libs", False)),
                            cold_opt=getattr(self.o, "cold_opt", "") or "",
                            prebuilt=prebuilt_stores(getattr(self.o, "prebuilt", None)))

    def _profile(self):
        """(name, table) of the pack build profile: Options.profile, else the pack's [convert] profile; None / "full"
        = the pack's whole build."""
        name = getattr(self.o, "profile", None) or self.recipe.get("convert.profile") or "full"
        tab = self.pack.get("profile.%s" % name) if name != "full" else {}
        if name != "full" and tab is None:
            raise ConvertError("options", "pack %s has no build profile %r" % (self.pack.name, name))
        tab = dict(tab or {})
        if self.o.audio == "none" or not self.recipe.get("convert.build.audio_model", True):
            if tab.get("generators") is not None:          # no sound model: its generator is not run either
                tab["generators"] = [g for g in tab["generators"] if not g.get("audio")]
            tab["audio_off"] = True
        return name, tab

    def _prepare(self, bopts):
        b = self.builder
        hle_hash = fingerprint([tree_hash(os.path.join(self.repo, d)) for d in
                                ("src/hle", "src/runtime", "src/audio", "src/texconv", "src/psp_exc", "hle", "runtime",
                                 "audio", "psp_exc")] + [tree_hash(self.pack.dir)])
        trans_state = tree_hash(os.path.join(self.translation, "stats.json"))
        extras = [tree_hash(self.pack.path(k)) for k in ("build.patch_dir", "build.trapset") if self.pack.get(k)]
        fp = fingerprint("prepare", trans_state, self.rec.path("translate"), extras, self.code_hash, hle_hash,
                         self._pack_inputs_hash(), bopts.to_dict(), [g[0] for g in b.generated],
                         [tree_hash(g[0]) for g in b.generated])
        self.fp_prepare = fp
        outs = [os.path.join(b.bdir, "gen"), os.path.join(b.bdir, "hle"), os.path.join(b.bdir, "prepare.json")]
        if self._cached("prepare", fp):
            return
        t0 = time.time()
        b.prepare(self.translation)
        self._save("prepare", fp, outs, t0)

    def _compile(self, bopts):
        b = self.builder
        fp = fingerprint("compile", self.fp_prepare, self.tc.kind, self.tc.version, bopts.to_dict())
        self.fp_compile = fp
        if self._cached("compile", fp):
            return
        t0 = time.time()
        built = b.compile()
        secs = time.time() - t0
        nj, reused = getattr(b, "used_jobs", bopts.jobs), getattr(b, "reused", 0)
        self.result["compile"] = {"units": len(built), "seconds": round(secs, 1), "jobs": nj, "prebuilt": reused,
                                  "toolchain": self.tc.description, "psp_gcc": self.tc.version}
        self.p.info("compile: %d units in %.0f s (-j%d, %s%s)" % (len(built), secs, nj, self.tc.kind,
                                                                   ", %d prebuilt" % reused if reused else ""))
        self._save("compile", fp, [os.path.join(b.bdir, "obj")], t0, {"compile": self.result["compile"]})

    def _link(self):
        b = self.builder
        fp = fingerprint("link", self.fp_compile, tree_hash(os.path.join(b.bdir, "obj")), self.tc.description)
        elf, strip = os.path.join(b.bdir, "poc6.elf"), os.path.join(b.bdir, "poc6_strip.elf")
        self.fp_link = fp
        if self._cached("link", fp):
            return elf, strip
        t0 = time.time()
        b.link()
        self.p.log("link: %s (%d bytes), stripped %d bytes" % (elf, os.path.getsize(elf), os.path.getsize(strip)))
        self._save("link", fp, [elf, strip], t0)
        return elf, strip

    def _prx(self):
        o = self.o
        if o.prx == "skip":
            self._step_begin("prx")
            self.p.warn("kernel helper module skipped (--prx skip): no crash handler / hardware counters on the PSP")
            self._step_end("prx", time.time(), False)
            return None
        if o.prx not in ("build", None):
            self._step_begin("prx")
            self._step_end("prx", time.time(), False)
            return o.prx
        b = self.builder
        from .build import prx_dir
        fp = fingerprint("prx", tree_hash(prx_dir(self.repo)), self.tc.kind, self.tc.version, self.tc.description)
        out = os.path.join(b.bdir, "prx", "ps2rc_exc.prx")
        if self._cached("prx", fp):
            return out
        t0 = time.time()
        try:
            b.build_prx()
        except ConvertError as e:
            self.p.warn("kernel helper module not built (%s): the game runs without the crash handler" % e.message)
            self._step_end("prx", t0, False)
            return None
        self._save("prx", fp, [out], t0)
        return out

    def _eboot(self, strip):
        o = self.o
        from ps2rc.pbp import Pbp, make_sfo, apply, check, blocking, PbpError
        title = o.title or self._profile()[1].get("title") or self.pack.get("id.psp_title") or self.folder
        digits = "".join(ch for ch in (self.ident.serial or "") if ch.isdigit())[:5]
        disc_id = o.disc_id or ("PSRC" + digits.rjust(5, "0"))       # never pspsdk's default (a real product id)
        if o.disc_id == "pspsdk":
            disc_id = None                                  # pspsdk's default id (byte-identical to build.mak)
        assets = {k: getattr(o, k) for k in ("icon0", "icon1", "pic0", "pic1", "snd0")}
        keep = self._old_assets() if o.update else {}
        if keep.get("title") and not o.title:
            title = keep["title"]
        fp = fingerprint("eboot", sha_file(strip), title, disc_id, o.fit, sorted((k, sha_file(v) if isinstance(v, str)
                                                                                  else None) for k, v in
                                                                                 keep.get("files", {}).items()),
                         {k: (sha_file(v) if v and os.path.isfile(v) else v) for k, v in assets.items()})
        out = os.path.join(self.work, "EBOOT.PBP")
        if self._cached("eboot", fp):
            return out
        t0 = time.time()
        p = Pbp()
        p["PARAM.SFO"] = make_sfo(title, extra=[("MEMSIZE", self.builder.memsize)], disc_id=disc_id).build()
        p["DATA.PSP"] = open(strip, "rb").read()
        for slot, path in keep.get("files", {}).items():   # update: the folder's own menu assets, unless new ones
            p[slot] = open(path, "rb").read()
        try:
            notes = apply(p, icon0=assets["icon0"], icon1_src=assets["icon1"], pic0=assets["pic0"],
                          pic1=assets["pic1"], snd0=assets["snd0"], fit=o.fit, ffmpeg=self.ffmpeg, log=self.p.log)
        except PbpError as e:
            raise ConvertError("assets", str(e), "pick another picture / file, or leave that asset out")
        for n in notes:
            self.p.log("eboot: " + n)
        issues = check(p)
        stop = blocking(issues)
        if stop:
            raise ConvertError("eboot", "; ".join(i.message for i in stop))
        p.write(out)
        self.p.log("eboot: %s, title %r, DISC_ID %s, %d bytes" % (out, title, disc_id or "(pspsdk default)",
                                                                  os.path.getsize(out)))
        self._save("eboot", fp, [out], t0)
        return out

    def _old_assets(self):
        """Update: the menu assets and title of the folder's EBOOT.PBP (kept unless new ones are given)."""
        path = os.path.join(self.folder_dir, "EBOOT.PBP")
        out = {"files": {}}
        try:
            from ps2rc.pbp import Pbp, Sfo
            old = Pbp.read(path)
        except Exception:
            return out
        d = os.path.join(self.work, "assets_prev")
        os.makedirs(d, exist_ok=True)
        given = {"ICON0.PNG": "icon0", "ICON1.PMF": "icon1", "PIC0.PNG": "pic0", "PIC1.PNG": "pic1",
                 "SND0.AT3": "snd0"}
        for slot, opt in given.items():
            if old[slot] and getattr(self.o, opt) is None:
                fn = os.path.join(d, slot)
                with open(fn, "wb") as f:
                    f.write(old[slot])
                out["files"][slot] = fn
        try:
            out["title"] = Sfo.parse(old["PARAM.SFO"]).get("TITLE")
        except Exception:
            pass
        return out

    def _cfg_files(self, movies_on):
        """{file name: (text, kind)} of the run options files. A pack with [cfg] templates: the player's settings
        file (kind user; the converter's decisions and the given settings on the template's own lines) + the
        developer file <cfg>_dev<.ext>; otherwise one file of [stage.cfg] keys (the older form)."""
        o = self.o
        cfg_name = self.pack.get("runtime.cfg") or "ps2rc.cfg"
        players, dev, default = cfgfile.pack_templates(self.pack)
        if not players:
            text = "# run options (written by the PS2RC converter)\n" + "".join(
                "%s=%s\n" % kv for kv in self._cfg(movies_on).items())
            return {cfg_name: (text, "generated")}
        old = getattr(self, "old_manifest", None) or {}
        lang = o.lang or (old.get("cfg") or {}).get("lang") or default
        if lang not in players:
            raise ConvertError("options", "settings language %r: the pack has %s" % (lang, ", ".join(sorted(players))))
        tpl = cfgfile.read_text(players[lang])
        aliases = self.pack.get("cfg.aliases", {}) or {}
        model = self._audio_model()
        decisions = {aliases.get("movies", "movies"): "1" if movies_on else "0",
                     aliases.get("audio", "audio"): "1" if o.audio in ("all", "min") and model else "0"}
        tvals = cfgfile.parse(tpl)
        decided = {k: v for k, v in decisions.items()            # on its own line when the template differs; not in
                   if (k.lower() in tvals and tvals[k.lower()] != v) or (k.lower() not in tvals and v == "0")}
        #                                                          the template: only "off" (the runtime's default is on)
        settings = dict(decided)
        settings.update({str(k): str(v) for k, v in o.settings.items()})
        extra = [kv.split("=", 1) for kv in o.cfg]
        text = cfgfile.render_player(tpl, settings, [(k.strip(), v.strip()) for k, v in extra])
        self.cfg_info = {"name": cfg_name, "lang": lang, "template_keys": cfgfile.keys(tpl),
                         "decisions": decided, "dev": cfgfile.dev_name(cfg_name) if dev and o.dev_cfg else None}
        files = {cfg_name: (text, "user")}
        if dev and o.dev_cfg:
            files[cfgfile.dev_name(cfg_name)] = (cfgfile.read_text(dev), "generated")
        return files

    def _audio_model(self):
        from .build import hle_sources
        return "-DHLE_HAVE_AUDIO_MODEL" in hle_sources(self.repo, self._build_options().audio_model, self.pack,
                                                       self._profile()[1])[1]

    def _player_cfg_merged(self, name, text):
        """Update: the player's existing settings file merged into the new one (his values kept)."""
        path = os.path.join(self.folder_dir, name)
        old = getattr(self, "old_manifest", None) or {}
        if not (self.o.update and os.path.isfile(path)):
            return text
        prev = open(path, "rb").read().decode("utf-8", errors="replace")
        oc = old.get("cfg") or {}
        pv = cfgfile.parse(prev)
        forget = {k.lower() for k, v in (oc.get("decisions") or {}).items()
                  if pv.get(k.lower()) == v}              # the converter's old decisions are not the player's
        forget |= {k.lower() for k in list(self.o.settings) + list(self.cfg_info["decisions"])}   # this run's win
        merged, info = cfgfile.merge_player(prev, text, oc.get("template_keys"), forget=forget)
        self.p.info("update: %s - your values kept (%s), new settings %s, dropped %s" % (
            name, ", ".join(info["kept"]) or "none changed from the defaults", ", ".join(info["added"]) or "none",
            ", ".join(info["dropped"]) or "none"))
        return merged

    def _cfg(self, movies_on):
        cfg = dict((k, str(v)) for k, v in (self.recipe.get("stage.cfg", {}) or {}).items())
        cfg["movies"] = "1" if movies_on else "0"
        from .build import hle_sources
        model = "-DHLE_HAVE_AUDIO_MODEL" in hle_sources(self.repo, self._build_options().audio_model, self.pack,
                                                        self._profile()[1])[1]
        cfg["audio"] = "1" if self.o.audio in ("all", "min") and model else "0"
        for kv in self.o.cfg:
            k, v = kv.split("=", 1)
            cfg[k.strip()] = v.strip()
        return cfg

    def _movie_tool(self):
        for t in (("ps2rc", "movies", "convert_movies_open.py"), ("tools", "movies", "convert_movies_open.py")):
            path = os.path.join(self.repo, *t)
            if os.path.exists(path):
                return path
        return os.path.join(self.repo, "ps2rc", "movies", "convert_movies_open.py")

    def movies_possible(self):
        """(bool, reason): the open movie path needs ffmpeg, the open converter of the checkout and a runtime that
        plays video-only PMF + WAV."""
        tool = self._movie_tool()
        player = next((x for x in (os.path.join(self.repo, "hle", "psp", "hle_wavsrc.c"),
                                   os.path.join(self.repo, "src", "hle", "psp", "hle_wavsrc.c")) if os.path.exists(x)),
                      os.path.join(self.repo, "hle", "psp", "hle_wavsrc.c"))
        if not self.recipe.get("stage.movies"):
            return False, "the pack lists no movies"
        if not os.path.exists(tool):
            return False, "this converter has no open movie converter (ps2rc/movies/convert_movies_open.py)"
        if not os.path.exists(player):
            return False, "this runtime cannot play the open movie format yet"
        if not self.ffmpeg:
            return False, "ffmpeg not found (install ffmpeg with libx264, or give --ffmpeg PATH)"
        return True, ""

    def _stage(self, eboot, prx):
        o, st = self.o, self.stager
        ok, why = self.movies_possible()
        if o.movies == "convert" and not ok:
            raise ConvertError("movies", "movies cannot be converted: %s" % why,
                               "use --movies skip (the game starts without its intro movies)")
        self.movies_on = ok and o.movies != "skip"
        cfgs = self._cfg_files(self.movies_on)
        boot_name = self.pack.get("id.boot_elf") or os.path.basename(self.ident.boot)
        sel = st.selection(o.audio if o.audio != "none" else None, self.ident.boot)
        fp = fingerprint("stage", [(f.iso, f.size) for f in sel], sha_file(eboot), sha_file(prx) if prx else None,
                         sorted((k, v[0]) for k, v in cfgs.items()), self.recipe.get("stage.replace"),
                         tree_hash(os.path.join(self.work, "iso_meta.bin")),
                         tree_hash(os.path.join(self.work, "discmap.txt")))
        if self._cached("stage", fp):
            return
        t0 = time.time()
        os.makedirs(self.folder_dir, exist_ok=True)
        st.copy_file(eboot, "EBOOT.PBP")
        st.copy_file(os.path.join(self.disc_dir, boot_name), boot_name)
        st.copy_file(os.path.join(self.work, "discmap.txt"), "discmap.txt")
        st.copy_file(os.path.join(self.work, "iso_meta.bin"), "iso_meta.bin")
        if prx:
            st.copy_file(prx, "ps2rc_exc.prx")
        for name, (text, kind) in cfgs.items():
            if kind == "user":
                text = self._player_cfg_merged(name, text)
            st.write_bytes(name, text.encode("utf-8"))
        files, total, lines = st.stage_disc(o.audio if o.audio != "none" else None, self.ident.boot)
        with open(os.path.join(self.folder_dir, "disc_files.txt"), "w", encoding="ascii", newline="\n") as f:
            f.write("# disc files staged under disc/ (path, size in bytes)\n")
            f.write("\n".join(lines) + "\n")
        self.p.log("stage: %d disc files, %.1f MB%s, cfg %s" % (
            len(files), total / 1e6, " (%d hard links to the disc folder)" % st.linked if st.linked else "",
            " + ".join(sorted(cfgs))))
        self._save("stage", fp, [self.folder_dir + os.sep + "EBOOT.PBP", os.path.join(self.folder_dir, "disc")], t0)

    def _movies(self):
        o = self.o
        self._step_begin("movies")
        t0 = time.time()
        mdir = os.path.join(self.folder_dir, "movies")
        if not getattr(self, "movies_on", False):
            ok, why = self.movies_possible()
            self.p.info("movies: skipped (%s); the game starts without its intro movies"
                        % ("--movies skip" if o.movies == "skip" else why))
            if os.path.isdir(mdir):
                shutil.rmtree(mdir)
            self._step_end("movies", t0, False)
            return
        names = self.recipe.get("stage.movies", [])
        src = os.path.join(self.work, "movies_src")
        outd = os.path.join(self.work, "movies_out")
        os.makedirs(src, exist_ok=True)
        fp = fingerprint("movies", names, self.ffmpeg, tree_hash(os.path.dirname(self._movie_tool())))
        if not self.rec.valid("movies", fp):
            for n in names:
                f = self.disc.find(n)
                if f is None:
                    raise ConvertError("movies", "movie %s of the pack is not on the disc" % n)
                self.disc.copy_to(f, os.path.join(src, os.path.basename(f.rel)))
            stems = [os.path.splitext(os.path.basename(n))[0] for n in names]
            run_logged(script_cmd(self._movie_tool(), "--src",
                                  src, "--out", outd, "--ffmpeg", self.ffmpeg, "--names", *stems), self.repo, self.env,
                       os.path.join(self.work, "logs", "movies.log"), self.p, "movies")
            self.rec.save("movies", fp, [outd], time.time() - t0)
        os.makedirs(mdir, exist_ok=True)
        for fn in sorted(os.listdir(outd)):
            if fn.lower().endswith((".pmf", ".wav")):
                self.stager.copy_file(os.path.join(outd, fn), os.path.join("movies", fn))
        self._step_end("movies", t0, False)

    def _kind_of(self, r):
        boot_name = self.pack.get("id.boot_elf") or os.path.basename(self.ident.boot)
        ci = getattr(self, "cfg_info", None) or {}
        if r == ci.get("name"):
            return "user", "template:%s" % ci.get("lang")
        if r.startswith("disc/") or r == boot_name:
            return "game_data", "disc"
        if r.startswith("movies/"):
            return "generated", "disc"
        return "generated", None

    def _manifest_info(self):
        import hashlib
        ov = {}
        for n in self.pack.get("overlays.files", []) or []:
            pth = os.path.join(self.disc_dir, n)
            if os.path.exists(pth):
                ov[n] = mf.hash_file(pth)[0]
        ci = dict(getattr(self, "cfg_info", None) or {})
        return {"converter": VERSION, "runtime": self._runtime_fp(),
                "pack": {"name": self.pack.name, "fingerprint": tree_hash(self.pack.dir)},
                "game": {"serial": self.ident.serial, "version": self.ident.version, "region": self.ident.region},
                "disc": {"boot": self.ident.boot, "boot_sha256": self.ident.elf_sha256, "overlays_sha256": ov,
                         "label": getattr(self.disc, "label", None)},
                "options": {"audio": self.o.audio, "movies": bool(getattr(self, "movies_on", False)),
                            "profile": self._profile()[0],
                            "title_sha256": hashlib.sha256((self.o.title or "").encode()).hexdigest()},
                "cfg": ci, "runtime_files": list(self.recipe.get("stage.runtime_files", []) or [])}

    def _verify(self):
        self._step_begin("verify")
        t0 = time.time()
        from ps2rc.pbp import Pbp, check, blocking
        fd = self.folder_dir
        p = Pbp.read(os.path.join(fd, "EBOOT.PBP"))
        stop = blocking(check(p))
        if stop:
            raise ConvertError("verify", "EBOOT.PBP: %s" % "; ".join(i.message for i in stop))
        rows = {}
        for ln in open(os.path.join(fd, "discmap.txt"), encoding="ascii"):
            lsn, size, iso, rel = ln.split()
            rows[rel] = int(size)
        bad, staged = [], 0
        for ln in open(os.path.join(fd, "disc_files.txt"), encoding="ascii"):
            if ln.startswith("#") or not ln.strip():
                continue
            staged += 1
            rel, size = ln.split()[:2]
            path = os.path.join(fd, "disc", rel.replace("/", os.sep))
            if not os.path.isfile(path) or os.path.getsize(path) != rows.get(rel, -1):
                bad.append(rel)
        if bad:
            raise ConvertError("verify", "%d staged disc files are missing or have the wrong size: %s"
                               % (len(bad), ", ".join(bad[:5])), "run the converter again (the copy is resumed)")
        size = 0
        for dp, _d, fs in os.walk(fd):
            for fn in fs:
                size += os.path.getsize(os.path.join(dp, fn))
        with open(os.path.join(fd, "README_PS2RC.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(README.format(folder=self.folder, size="%.2f GB" % (size / 1e9), version=VERSION,
                                  pack=self.pack.name, game="%s %s (%s)" % (self.ident.serial, self.ident.version,
                                                                            self.ident.region)))
        if self.o.manifest:
            data = mf.build(fd, self._manifest_info(), self._kind_of, self.md5cache, self.p)
            mf.write(fd, data)
            mf.write_md5_list(fd, data)
            self.p.log("verify: %s + MANIFEST.md5 of %d files, %.2f GB" % (
                mf.MANIFEST, len(data["files"]), sum(v["size"] for v in data["files"].values()) / 1e9))
            if getattr(self.stager, "rewritten", 0):
                self.p.warn("%d file(s) read back wrong once and were written again" % self.stager.rewritten)
        self.result["folder_bytes"] = size
        if self.o.update:
            self.result["update"] = {"written": self.stager.written, "rewritten": self.stager.rewritten,
                                     "report": getattr(self, "update_report", None) and self.update_report.to_dict()}
        self.p.info("verify: EBOOT.PBP valid; %d disc files present with the right sizes; folder %.2f GB"
                    % (staged, size / 1e9))
        self._step_end("verify", t0, False)
