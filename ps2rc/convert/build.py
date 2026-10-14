"""Building the PSP program without make or a shell: a Python form of tests/phase6psp/build.sh (target psp), its
Makefile and pspsdk's build.mak, so a bundled native Windows toolchain needs nothing else. Same inputs, same flags,
same unit order -> the same objects and ELF as the developer build (checked by the converter's comparison runs).

Kept in step with (2026-10-06): mk/build.sh (tests/phase6psp/build.sh before the layout move) and pspsdk build.mak
(PSP_FW_VERSION 600 when unset, PSP_LARGE_MEMORY=1 -> MEMSIZE=1, BUILD_PRX rules). The flag variables (CFLAGS,
HLEFLAGS, LIBS, the PRX's OBJS / CFLAGS / LIBS / PSP_FW_VERSION) are read from the checkout's Makefiles
(mk/Makefile.psp, psp_exc/Makefile) when they exist, so a change there reaches the converter without an edit here;
the constants below are the fallback (the values of 2026-10-06).
"""
import glob
import hashlib
import json
import os
import re
import shutil

from .steps import ConvertError, run_logged, script_cmd
from .toolchain import MB_PER_JOB, Job

FPFLAGS = ["-fno-fast-math", "-fno-math-errno", "-frounding-math", "-ffp-contract=off"]
BASE_CFLAGS = ["-O2", "-G0", "-std=gnu11"] + FPFLAGS
HLEFLAGS = ["-DHLE_PSP", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-format", "-fno-strict-aliasing"]
LIBS = ["-lpspgu", "-lpsppower", "-lpsprtc", "-lpspmpeg", "-lpspaudio"]
BUILDMAK_LIBS = ["-lpspdebug", "-lpspdisplay", "-lpspge", "-lpspctrl", "-lpspnet", "-lpspnet_apctl"]
PSP_FW_VERSION = 600
TARGET = "poc6"
# the kernel module imports the CFW function it needs through its own stub (ps2rc_sctrl.S, PSPSDK import macros)
PRX_DEFAULT = {"OBJS": "exc_main.o exc_asm.o exc_me.o ps2rc_sctrl.o", "CFLAGS": "-O2 -G0 -Wall", "LIBS": "",
               "PSP_FW_VERSION": "660"}
# GPL-3.0 libraries of pspdev's psp-cfw-sdk: never linked by default (the runtime uses its own import stubs,
# hle/psp/ps2rc_kcall.S and psp_exc/ps2rc_sctrl.S); an older checkout that still names them needs allow_gpl_libs
GPL_LIBS = ("-lpspkubridge", "-lpspsystemctrl_kernel", "-lpspsystemctrl_user")


def check_gpl_libs(libs, where, allowed):
    bad = [x for x in libs if x in GPL_LIBS]
    if bad and not allowed:
        raise ConvertError("gpl_lib", "%s links %s (GPL-3.0 libraries of psp-cfw-sdk)" % (where, " ".join(bad)),
                           "use a checkout with the own import stubs (hle/psp/ps2rc_kcall.S, psp_exc/ps2rc_sctrl.S); "
                           "a developer may pass --allow-gpl-libs for an old tree (such a build must never be shared)")
    return bad

_MKVAR = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(\+=|\?=|:=|=)\s*(.*)$")


def make_vars(path):
    """Plain variable assignments of a Makefile (=, :=, +=, ?=; comments dropped; $(VAR) of variables set earlier in
    the file expanded, others - set on the make command line - left out). Conditional blocks are not evaluated."""
    vals = {}
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.split("#", 1)[0].rstrip()
        m = _MKVAR.match(line)
        if not m:
            continue
        name, op, val = m.groups()
        val = re.sub(r"\$\((\w+)\)", lambda mm: vals.get(mm.group(1), ""), val).strip()
        if op == "+=":
            vals[name] = (vals.get(name, "") + " " + val).strip()
        elif op == "?=":
            vals.setdefault(name, val)
        else:
            vals[name] = val
    return vals


def makefile_flags(repo):
    """(cflags, hleflags, libs, sfo_memsize, source) of the PSP Makefile of the checkout, else the fallback."""
    for rel in ("mk/Makefile.psp", "tests/phase6psp/Makefile"):
        p = os.path.join(repo, rel)
        if os.path.exists(p):
            v = make_vars(p)
            if v.get("CFLAGS") and v.get("LIBS"):
                return (v["CFLAGS"].split(), v.get("HLEFLAGS", " ".join(HLEFLAGS)).split(), v["LIBS"].split(),
                        memsize(v.get("PSP_LARGE_MEMORY"), int(v.get("PSP_FW_VERSION", PSP_FW_VERSION))), rel)
    return BASE_CFLAGS, HLEFLAGS, LIBS, 1, "built-in"


def memsize(large_memory, fw=PSP_FW_VERSION):
    """PARAM.SFO MEMSIZE as build.mak computes it (EXPAND_MEMORY)."""
    if fw > 390:
        return {"1": 1, "0": 0}.get(large_memory, 2)
    return {"1": 1, "2": 2}.get(large_memory, 0)


class BuildOptions:
    def __init__(self, build_num=0, splash=True, audio_model=True, alias="call", y_hdr="mb,be",
                 y_gen="-fsched-pressure", jobs=4, natives=True, layout=True, allow_gpl_libs=False, cold_opt="",
                 prebuilt=(), profile=None):
        self.build_num, self.splash, self.audio_model = build_num, splash, audio_model
        self.alias, self.y_hdr, self.y_gen, self.jobs = alias, y_hdr, y_gen, jobs
        self.natives, self.layout, self.allow_gpl_libs = natives, layout, allow_gpl_libs
        self.cold_opt = cold_opt                   # "" = off; "O1" / "Os": cold functions in <unit>_cold.c at -O1 / -Os
        self.prebuilt = list(prebuilt)             # object stores (prebuilt.py): an object with the same key is copied
        self.profile = dict(profile or {})         # the pack's build profile ([profile.<name>], see profile_sources)

    def to_dict(self):
        """What changes the build's output (the job count and the object stores do not)."""
        return {k: v for k, v in self.__dict__.items() if k not in ("jobs", "prebuilt")}


_FN_START = re.compile(r"^(__attribute__\(\(cold\)\) )?void [A-Za-z0-9_]+\(R5900Ctx \*c\)$")


_PROTO = re.compile(r"^[A-Za-z_][A-Za-z0-9_ ]*[ *]([A-Za-z_][A-Za-z0-9_]*)\([^;={}]*\);$")
_ONE_LINE_FN = re.compile(r"^void [A-Za-z0-9_]+\(R5900Ctx \*c\) \{.*\}$")


def _split_unit(text):
    """(head, prototypes, [(cold?, lines)]) of one generated unit, or None when it holds anything else at the top
    level (tables, static data, weak definitions): such a unit stays whole."""
    head, decls, chunks, cur, depth = [], [], [], None, 0
    for ln in text.split("\n"):
        if depth > 0:
            cur[1].append(ln)
            depth += ln.count("{") - ln.count("}")
            continue
        if _FN_START.match(ln):
            cur = [ln.startswith("__attribute__((cold))"), [ln]]
            chunks.append(cur)
        elif not chunks and (not ln.strip() or ln.startswith("#")):
            head.append(ln)
        elif not ln.strip():
            if cur is not None:
                cur[1].append(ln)
        elif _PROTO.match(ln):
            decls.append(ln)                    # a prototype: both halves need it
        elif _ONE_LINE_FN.match(ln) and ln.count("{") == ln.count("}"):
            cur = [False, [ln]]                 # a one-line wrapper (profiler zones): stays with the hot code
            chunks.append(cur)
        elif cur is not None and cur[1][-1] == cur[1][0] and ln.startswith("{"):
            cur[1].append(ln)                   # the body of the function just started
            depth = ln.count("{") - ln.count("}")
        else:
            return None
    if depth != 0:
        return None
    return head, decls, chunks


def split_cold(gen_dir):
    """Move the cold functions (layout_apply's __attribute__((cold))) of every generated unit into <unit>_cold.c, so
    they can be compiled at a cheaper optimisation level; hot functions stay in <unit>.c. Prototypes between the
    functions go into both halves (a cold function's own prototype for a hot caller, and the reverse); a unit with
    any other top-level text (tables, static data) is left whole. -> (units split, bytes moved). Files are rewritten
    only when their text changes."""
    split, moved = 0, 0
    for fn in sorted(os.listdir(gen_dir)):
        if not fn.endswith(".c") or fn.endswith("_cold.c"):
            continue
        path = os.path.join(gen_dir, fn)
        parts = _split_unit(open(path, encoding="utf-8", newline="").read())
        if parts is None:
            continue
        head, decls, chunks = parts
        cold = [c for c in chunks if c[0]]
        if not cold or len(cold) == len(chunks):
            continue
        hot_text = "\n".join(head + decls + [""] + [ln for c in chunks if not c[0] for ln in c[1]])
        cold_text = "\n".join(head + decls + [""] + [ln for c in cold for ln in c[1]])
        hot_text, cold_text = hot_text.rstrip("\n") + "\n", cold_text.rstrip("\n") + "\n"
        for name, text in ((fn, hot_text), (fn[:-2] + "_cold.c", cold_text)):
            q = os.path.join(gen_dir, name)
            if not (os.path.exists(q) and open(q, encoding="utf-8", newline="").read() == text):
                with open(q, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
        split += 1
        moved += len(cold_text)
    return split, moved


def _copy(src, dst):
    shutil.copy2(src, dst)


def object_key(argv, dep, source, tc):
    """Content key of one object: the command with the toolchain's own folder replaced by a placeholder (the same key
    on every PC and install folder), the compiler version, the headers' hash and the source text."""
    root = tc.pspdev() if hasattr(tc, "pspdev") else ""
    norm = " ".join(a.replace(root, "{PSPDEV}") if root else a for a in argv)
    return hashlib.sha256(("%s\0%s\0%s\0" % (norm, getattr(tc, "version", ""), dep)).encode() + source).hexdigest()


def layout_of(repo):
    """'s4': hle/core, hle/psp, runtime/, audio/, psp_exc/, the pack's [build] sources (mk/build.sh);
    's3': src/hle, src/hle/psp, src/runtime, src/audio, src/texconv, src/psp_exc (tests/phase6psp/build.sh)."""
    return "s4" if os.path.isdir(os.path.join(repo, "hle", "core")) else "s3"


def find_script(repo, *cands):
    """The first existing script of a list of locations (scripts move from tests/ to ps2rc/pipeline/)."""
    for c in cands:
        if os.path.exists(os.path.join(repo, c)):
            return c
    raise ConvertError("checkout", "none of %s in %s" % (", ".join(cands), repo), "the converter tree is incomplete")


SCRIPTS = {
    "prepare_gen": ("tests/poc6/prepare_gen.py", "ps2rc/pipeline/prepare_gen.py"),
    "layout_apply": ("tests/phase6psp/r_layout/layout_apply.py", "ps2rc/pipeline/layout_apply.py"),
    "patch_rt_header": ("tests/poc6/patch_rt_header.py", "ps2rc/pipeline/patch_rt_header.py"),
    "bind": ("src/ps2rc/bind.py", "ps2rc/bind.py"),
    "pack": ("src/ps2rc/pack.py", "ps2rc/pack.py"),
}


def _dir_files(d, target="psp"):
    """*.c *.h *.S of a folder and of its <target>/ subfolder (mk/build.sh copy_dir)."""
    out = []
    for sub in (d, os.path.join(d, target)):
        for ext in ("*.c", "*.h", "*.S"):
            out += sorted(glob.glob(os.path.join(sub, ext)))
    return [(f, os.path.basename(f)) for f in out if os.path.isfile(f)]


def profile_sources(copies, defs, pack, profile, repo):
    """A pack build profile ([profile.<name>] of pack.toml): exclude_sources (paths relative to the pack) leave the
    build, extra_sources ({repo}/... or pack-relative files / folders) join it, include_sources go on the include path
    only (hle/rt/: #included by a glue unit, not compiled on their own), defines are added; audio_extra_sources /
    audio_include_sources only when the sound model is built (the pipeline sets audio_off otherwise)."""
    if not profile:
        return copies, defs
    drop = set()
    for x in profile.get("exclude_sources", []) or []:
        drop.add(os.path.normcase(os.path.normpath(pack.expand(x) if pack else x)))
    copies = [(s, n) for s, n in copies if os.path.normcase(os.path.normpath(s)) not in drop]
    keys = [("extra_sources", ""), ("include_sources", "rt/")]       # include_sources: #included, not compiled
    if not profile.get("audio_off"):                                   # the sound model's: only with it
        keys += [("audio_extra_sources", ""), ("audio_include_sources", "rt/")]
    for key, sub in keys:
        for x in profile.get(key, []) or []:
            p = x.replace("{repo}", repo)
            p = pack.expand(p) if pack and "{repo}" not in x else p
            if os.path.isdir(p):
                copies += [(s, sub + n) for s, n in _dir_files(p)]
            else:
                copies.append((p, sub + os.path.basename(p)))
    defs = defs + ["-D%s" % d for d in profile.get("defines", []) or []]
    return copies, defs


def hle_sources(repo, audio_model, pack=None, profile=None):
    """(copies, defines): the runtime sources of the PSP build as the developer build script selects them;
    copies = [(src, name)] in copy order (a later file of the same name wins, as cp does)."""
    copies, defs = hle_sources_base(repo, audio_model, pack)
    if profile and not audio_model:
        profile = dict(profile, audio_off=True)
    return profile_sources(copies, defs, pack, profile, repo)


def hle_sources_base(repo, audio_model, pack=None):
    copies, defs = [], []
    if layout_of(repo) == "s4":
        copies += _dir_files(os.path.join(repo, "hle", "core"))
        for d in (pack.get("build.sources", []) if pack else []) or []:
            copies += _dir_files(pack.expand(d))
        copies.append((os.path.join(repo, "runtime", "r5900_rt.c"), "r5900_rt.c"))
        copies += _dir_files(os.path.join(repo, "hle", "psp"))
        for f in ("exc_user.c", "ps2rc_exc.h"):
            copies.append((os.path.join(repo, "psp_exc", f), f))
        copies = [(s, n) for s, n in copies if n != "rmb_check.c"]
        defs += ["-D%s" % d for d in ((pack.get("build.defines", []) if pack else []) or [])]
        aud = (pack.get("build.audio_sources", []) if pack else []) or []
        if audio_model and aud:
            copies += _dir_files(os.path.join(repo, "audio"))
            for d in aud:
                copies += _dir_files(pack.expand(d))
            defs.append("-DHLE_HAVE_AUDIO_MODEL")
        return copies, defs
    for f in sorted(glob.glob(os.path.join(repo, "src", "hle", "*.c")) + glob.glob(os.path.join(repo, "src", "hle", "*.h"))):
        if os.path.basename(f) == "hle_host.c":
            continue
        copies.append((f, os.path.basename(f)))
    copies.append((os.path.join(repo, "src", "runtime", "r5900_rt.c"), "r5900_rt.c"))
    for f in sorted(glob.glob(os.path.join(repo, "src", "hle", "psp", "*"))):
        if os.path.isfile(f):
            copies.append((f, os.path.basename(f)))
    for f in ("exc_user.c", "ps2rc_exc.h"):
        copies.append((os.path.join(repo, "src", "psp_exc", f), f))
    for f in ("texconv.c", "texconv.h"):
        copies.append((os.path.join(repo, "src", "texconv", f), f))
    copies = [(s, n) for s, n in copies if n != "rmb_check.c"]
    for vu0 in sorted(glob.glob(os.path.join(repo, "src", "runtime", "vu0_*.c"))):   # an old-layout VU0 program HLE
        copies += [(vu0, os.path.basename(vu0)), (vu0[:-2] + ".h", os.path.basename(vu0)[:-2] + ".h")]
        defs.append("-DHLE_HAVE_VU0_MICRO")
    aud = [f for f in sorted(glob.glob(os.path.join(repo, "src", "audio", "*.[ch]")))
           if not os.path.basename(f).startswith("audio_host")]          # the host-only data interface stays out
    if audio_model and any(f.endswith(".c") for f in aud):
        copies += [(f, os.path.basename(f)) for f in aud]
        defs.append("-DHLE_HAVE_AUDIO_MODEL")
    return copies, defs


def rt_header(repo):
    for c in ("runtime/r5900_rt.h", "src/runtime/r5900_rt.h"):
        if os.path.exists(os.path.join(repo, c)):
            return os.path.join(repo, c)
    raise ConvertError("checkout", "no runtime/r5900_rt.h in %s" % repo)


def prx_dir(repo):
    for c in ("psp_exc", "src/psp_exc"):
        if os.path.exists(os.path.join(repo, c, "exc_main.c")):
            return os.path.join(repo, c)
    raise ConvertError("checkout", "no psp_exc/exc_main.c in %s" % repo)


def translation_name(trans, alias, layout_dir, y_hdr):
    s = json.load(open(os.path.join(trans, "stats.json"), encoding="utf-8"))
    name = "%s,%s,exempt=%d" % (s.get("variant"), (s.get("function_set") or "v1").split("/")[-1],
                                len((s.get("exempt") or {}).get("main", [])))
    name += ",alias=%s,layout=%s" % (alias, os.path.basename(layout_dir) if layout_dir else "none")
    if y_hdr:
        name += ",y=%s" % y_hdr.replace(",", "+")
    return name, str(s.get("variant", ""))


class Builder:
    """One PSP build in `bdir` (gen_prep/, gen/, hle/, obj/, poc6.elf, poc6_strip.elf)."""

    def __init__(self, repo, pack, bdir, toolchain, opts, progress, env, log):
        self.repo, self.pack, self.bdir, self.tc = repo, pack, os.path.abspath(bdir), toolchain
        self.opts, self.progress, self.env, self.log = opts, progress, env, log
        self.hle_defs = []
        self.layout = pack.path("build.layout") if opts.layout else None
        if self.layout and not os.path.isdir(self.layout):
            self.layout = None
        self.natives = pack.path("build.natives") if opts.natives else None
        if "natives" in opts.profile and not opts.profile["natives"]:
            self.natives = None                            # the profile builds without the native fast paths
        self.generated = []                                # [(C file made by a generator, -DHLE_GEN_X, include only)]
        self.mk_cflags, self.mk_hleflags, self.mk_libs, self.memsize, self.mk_source = makefile_flags(repo)

    def _py(self, script, *args):
        run_logged(script_cmd(os.path.join(self.repo, script), *[str(a) for a in args]), self.repo, self.env,
                   self.log, self.progress, os.path.basename(script))

    # --- prepare (build.sh steps 1-3) -----------------------------------------------------------------------------
    def profile_hooks(self):
        """The pack's hooks.def, minus the profile's exclude_hooks (names): a filtered copy in the build dir."""
        hooks = self.pack.path("build.hooks")
        drop = set(self.opts.profile.get("exclude_hooks", []) or [])
        if not drop:
            return hooks
        import re
        out, seen = [], set()
        for ln in open(hooks, encoding="utf-8").read().splitlines(True):
            m = re.match(r"\s*(?:HOOK|TRACE)(?:_OV)?\(\s*(?:\d+\s*,\s*)?[0-9a-f]{8}\s*,\s*(\w+)", ln)
            if m and m.group(1) in drop:
                seen.add(m.group(1))
                continue
            out.append(ln)
        missing = drop - seen
        if missing:
            raise ConvertError("pack", "profile: hooks %s are not in %s" % (", ".join(sorted(missing)), hooks))
        path = os.path.join(self.bdir, "hooks_profile.def")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write("".join(out))
        return path

    def profile_zones(self):
        """The pack's zones.def; the zones of hooks the profile leaves out lose PZF_GUEST (a zone on the guest
        original needs the hook)."""
        zones = self.pack.path("build.zones") if self.pack.get("build.zones") else ""
        drop = set(self.opts.profile.get("exclude_hooks", []) or [])
        if not drop or not zones:
            return zones
        import re
        hooks = self.pack.path("build.hooks")
        addrs = set()
        for ln in open(hooks, encoding="utf-8"):
            m = re.match(r"\s*(?:HOOK|TRACE)(?:_OV)?\(\s*(?:\d+\s*,\s*)?([0-9a-f]{8})\s*,\s*(\w+)", ln)
            if m and m.group(2) in drop:
                addrs.add(m.group(1))
        out = []
        for ln in open(zones, encoding="utf-8").read().splitlines(True):
            m = re.match(r"(\s*ZONE\(\s*([0-9a-f]{8})\s*,\s*\w+\s*,\s*)([^)]*)(\).*)", ln, re.S)
            if m and m.group(2) in addrs:
                flags = [x.strip() for x in m.group(3).split("|") if x.strip() not in ("PZF_GUEST", "")]
                ln = m.group(1) + ("|".join(flags) or "0") + m.group(4)
            out.append(ln)
        path = os.path.join(self.bdir, "zones_profile.def")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write("".join(out))
        return path

    def prepare(self, trans):
        p, b = self.pack, self.bdir
        for d in ("gen", "gen_prep", "hle/rt", "obj"):
            os.makedirs(os.path.join(b, d), exist_ok=True)
        hooks = self.profile_hooks()
        args = [trans, hooks, os.path.join(b, "gen_prep")]
        if p.get("build.patch_dir"):
            args.append(p.path("build.patch_dir"))
        if p.get("build.trapset"):
            args += ["--trapset", p.path("build.trapset")]
        if p.get("build.textpatch"):
            args += ["--textpatch", p.path("build.textpatch")]
        args.append("--fix-mfc0")
        for cf in p.get("build.callflush", []) or []:
            args += ["--callflush", cf]
        args += ["--zones", self.profile_zones() or ""]           # mk/build.sh passes it even when empty
        if self.natives:
            args += ["--native", self.natives]
        if p.get("build.sites"):
            args += ["--sites", p.path("build.sites")]
        self._py(find_script(self.repo, *SCRIPTS["prepare_gen"]), *args)
        la = [os.path.join(b, "gen_prep"), os.path.join(b, "gen")]
        if self.layout:
            la += ["--cold", os.path.join(self.layout, "cold.txt")]
        gen = os.path.join(b, "gen")
        for fn in os.listdir(gen) if os.path.isdir(gen) else []:     # the cold split of an earlier run
            if fn.endswith("_cold.c"):
                os.remove(os.path.join(gen, fn))
                hot = os.path.join(gen, fn[:-7] + ".c")      # layout_apply writes the whole unit again
                if os.path.exists(hot):
                    os.remove(hot)
        self._py(find_script(self.repo, *SCRIPTS["layout_apply"]), *la)
        if self.opts.cold_opt:
            if not self.layout:
                self.progress.warn("cold code: the pack has no code layout (cold list) - nothing to split")
            else:
                n, nb = split_cold(gen)
                self.progress.log("cold code: %d units split, %.1f MB of cold functions compiled at -%s"
                                  % (n, nb / 1e6, self.opts.cold_opt))
        # runtime sources (a fresh copy: nothing stale from an older checkout)
        hle = os.path.join(b, "hle")
        for fn in os.listdir(hle):
            if os.path.isfile(os.path.join(hle, fn)):
                os.remove(os.path.join(hle, fn))
        copies, defs = hle_sources(self.repo, self.opts.audio_model, p, self.opts.profile)
        for src, name in copies:
            _copy(src, os.path.join(hle, name))
        _copy(hooks, os.path.join(hle, "hooks.def"))
        tname, variant = translation_name(trans, self.opts.alias, self.layout, self.opts.y_hdr)
        d = ["-DHLE_BUILD_NUM=%d" % self.opts.build_num]
        if self.natives:
            d.append("-DHLE_YNATIVE")
        if self.opts.splash:
            d.append("-DHLE_SPLASH")
        d += defs
        for g in self.generated:                           # game modules generated on this PC (modules.py)
            path, define = g[0], g[1]
            inc_only = len(g) > 2 and g[2]                 # #included by a glue file: on the include path only
            _copy(path, os.path.join(hle, "rt" if inc_only else "", os.path.basename(path)))
            d.append(define)
        d.append('-DHLE_TRANSLATION="%s"' % tname)
        self.hle_defs = d
        self.progress.log("translation: %s (%s)" % (trans, tname))
        self._py(find_script(self.repo, *SCRIPTS["bind"]), hooks, os.path.join(hle, "hle_bind.h"), hle)
        self._py(find_script(self.repo, *SCRIPTS["pack"]), "cheader", p.dir, os.path.join(hle, "hle_pack.h"))
        rt = [rt_header(self.repo), os.path.join(hle, "rt", "r5900_rt.h"),
              "--mode", "psp", "--alias", self.opts.alias]
        if variant.startswith("v5s4"):
            rt.append("--maskbase")
        if self.opts.y_hdr:
            rt += ["--y", self.opts.y_hdr]
        self._py(find_script(self.repo, *SCRIPTS["patch_rt_header"]), *rt)
        # code layout (order.ld): the natives first in .text
        order = os.path.join(b, "order.ld")
        if os.path.exists(order):
            os.remove(order)
        if self.layout and os.path.exists(os.path.join(self.layout, "order.ld")):
            text = open(os.path.join(self.layout, "order.ld"), encoding="utf-8", newline="").read()
            if self.natives:
                text = "".join(ln + "  *(.text.y_nat_*)\n" if ln.rstrip("\r\n") == ".text : {" else ln
                               for ln in text.splitlines(True))
            with open(order, "w", encoding="utf-8", newline="") as f:
                f.write(text)
        json.dump({"hle_defs": self.hle_defs, "translation": tname}, open(os.path.join(b, "prepare.json"), "w"))

    # --- flags ------------------------------------------------------------------------------------------------------
    def cflags(self, fw=PSP_FW_VERSION, incdir=("hle/rt", "hle", "gen"), base=None):
        dev = self.tc.pspdev()
        sep = "/" if self.tc.kind != "native" else os.sep
        inc = list(incdir) + [".", dev + sep + "psp" + sep + "include",
                              dev + sep + "psp" + sep + "sdk" + sep + "include"]
        return ["-I" + i for i in inc] + list(base if base is not None else self.mk_cflags) +             ["-D_PSP_FW_VERSION=%d" % fw]

    def _sdk(self, *parts):
        dev = self.tc.pspdev()
        sep = "/" if self.tc.kind != "native" else os.sep
        return sep.join([dev, "psp", "sdk"] + list(parts))

    def _psplib(self):
        dev = self.tc.pspdev()
        sep = "/" if self.tc.kind != "native" else os.sep
        return sep.join([dev, "psp", "lib"])

    def extras(self):
        has_order = os.path.exists(os.path.join(self.bdir, "order.ld"))
        gen_extra = (["-ffunction-sections"] if has_order else []) + (self.opts.y_gen.split() if self.opts.y_gen else [])
        hle_extra = ["-ffunction-sections"] if has_order else []
        return gen_extra, hle_extra, has_order

    # --- compile ----------------------------------------------------------------------------------------------------
    def _stamp_path(self):
        return os.path.join(self.bdir, "obj", "stamps.json")

    def _hash_files(self, paths):
        h = hashlib.sha256()
        for p in sorted(paths):
            h.update(os.path.basename(p).encode())
            h.update(open(p, "rb").read())
        return h.hexdigest()

    def compile_jobs(self):
        b = self.bdir
        if not self.hle_defs:
            self.hle_defs = json.load(open(os.path.join(b, "prepare.json")))["hle_defs"]
        gen_extra, hle_extra, _ = self.extras()
        cf = self.cflags()
        gen_c = sorted(fn for fn in os.listdir(os.path.join(b, "gen")) if fn.endswith(".c"))
        hle_c = sorted(fn for fn in os.listdir(os.path.join(b, "hle")) if fn.endswith(".c"))
        rt_h = os.path.join(b, "hle", "rt", "r5900_rt.h")
        gen_dep = self._hash_files([rt_h] + glob.glob(os.path.join(b, "gen", "*.h")))
        hle_dep = self._hash_files([rt_h] + glob.glob(os.path.join(b, "hle", "*.h")) + [os.path.join(b, "hle", "hooks.def")])
        jobs = []
        for fn in gen_c:
            o = "obj/gen/%s.o" % fn[:-2]
            lvl = ["-" + self.opts.cold_opt] if self.opts.cold_opt and fn.endswith("_cold.c") else []
            jobs.append((Job(["psp-gcc"] + cf + gen_extra + lvl + ["-w", "-c", "gen/" + fn, "-o", o], o), "gen/" + fn,
                         gen_dep))
        for fn in hle_c:
            o = "obj/hle/%s.o" % fn[:-2]
            jobs.append((Job(["psp-gcc"] + cf + self.mk_hleflags + self.hle_defs + hle_extra +
                             ["-c", "hle/" + fn, "-o", o], o),
                         "hle/" + fn, hle_dep))
        for fn in sorted(f for f in os.listdir(os.path.join(b, "hle")) if f.endswith(".S")):   # hle_ctx.S, own imports
            o = "obj/hle/%s.o" % fn[:-2]
            jobs.append((Job(["psp-gcc"] + cf + ["-c", "hle/" + fn, "-o", o], o), "hle/" + fn, hle_dep))
        return jobs, len(gen_c)

    def compile(self):
        b = self.bdir
        os.makedirs(os.path.join(b, "obj", "gen"), exist_ok=True)
        os.makedirs(os.path.join(b, "obj", "hle"), exist_ok=True)
        jobs, ngen = self.compile_jobs()
        try:
            stamps = json.load(open(self._stamp_path(), encoding="utf-8"))
        except (OSError, ValueError):
            stamps = {}
        todo, keys, reused = [], {}, 0
        stores = [d for d in self.opts.prebuilt if d and os.path.isdir(d)]
        for job, src, dep in jobs:
            key = object_key(job.argv, dep, open(os.path.join(b, src), "rb").read(), self.tc)
            keys[job.label] = key
            if stamps.get(job.label) == key and os.path.exists(os.path.join(b, job.label)):
                continue
            if os.path.exists(os.path.join(b, job.label)):
                os.remove(os.path.join(b, job.label))
            hit = next((os.path.join(d, key + ".o") for d in stores if os.path.exists(os.path.join(d, key + ".o"))),
                       None)
            if hit:                                        # a prebuilt object of exactly this source + flags
                shutil.copyfile(hit, os.path.join(b, job.label))
                stamps[job.label] = key
                reused += 1
                continue
            todo.append(job)
        if reused:
            self.progress.log("compile: %d objects taken from the prebuilt store" % reused)
        # the biggest units first: the batch ends sooner on many cores
        todo.sort(key=lambda j: -os.path.getsize(os.path.join(b, j.argv[-3])))
        nj = self.tc.jobs(self.opts.jobs) if hasattr(self.tc, "jobs") else self.opts.jobs
        if nj < self.opts.jobs:
            self.progress.warn("compile: %d parallel jobs instead of %d - the free memory where the compiler runs "
                               "allows about %d MB per job" % (nj, self.opts.jobs, MB_PER_JOB))
        self.used_jobs, self.reused = nj, reused
        self.progress.log("compile: %d of %d units to build (%d generated), %d jobs, %s"
                          % (len(todo), len(jobs), ngen, nj, self.tc.description))
        if todo:
            self.tc.run_batch(todo, b, nj, self.progress, "compile")
        failed = [j for j in todo if j.rc != 0]
        for j in todo:
            if j.rc == 0:
                stamps[j.label] = keys[j.label]
        with open(self._stamp_path(), "w", encoding="utf-8") as f:
            json.dump(stamps, f, indent=0)
        with open(os.path.join(b, "compile_times.txt"), "w", encoding="utf-8") as f:
            for j in sorted(todo, key=lambda j: -j.seconds):
                f.write("%8.1f s  %s\n" % (j.seconds, j.label))
        if failed:
            j = failed[0]
            with open(self.log, "a", encoding="utf-8") as lf:
                for f_ in failed:
                    lf.write("\n--- %s (rc %s)\n%s\n" % (f_.label, f_.rc, f_.output))
            raise ConvertError("compile", "%d unit(s) failed to compile, first: %s" % (len(failed), j.label),
                               "see the log", log=self.log, tail=j.output.strip().splitlines()[-15:])
        return todo

    # --- link (Makefile + build.mak) ---------------------------------------------------------------------------------
    def link(self):
        b = self.bdir
        gen_extra, hle_extra, has_order = self.extras()
        objs = ["obj/gen/%s.o" % fn[:-2] for fn in sorted(os.listdir(os.path.join(b, "gen"))) if fn.endswith(".c")]
        objs += ["obj/hle/%s.o" % fn[:-2] for fn in sorted(os.listdir(os.path.join(b, "hle"))) if fn.endswith(".c")]
        objs += ["obj/hle/%s.o" % fn[:-2] for fn in sorted(os.listdir(os.path.join(b, "hle"))) if fn.endswith(".S")]
        ld = ["-L.", "-L" + self._psplib(), "-L" + self._sdk("lib")]
        ld += list(self.pack.get("build.psp_ldflags", []) or [])
        if has_order:
            ld.append("-Wl,--section-ordering-file=order.ld")
        ld.append("-Wl,-zmax-page-size=128")
        ld.append("-Wl,-Map=%s.map" % TARGET)          # which library members went in (licence notices, comparisons)
        for f in (TARGET + ".elf", TARGET + "_strip.elf", TARGET + ".map"):
            if os.path.exists(os.path.join(b, f)):
                os.remove(os.path.join(b, f))
        if check_gpl_libs(self.mk_libs, self.mk_source, self.opts.allow_gpl_libs):
            self.progress.warn("linking GPL-3.0 libraries (--allow-gpl-libs): this build must never be shared")
        jobs = [Job(["psp-gcc"] + self.cflags() + ld + objs + self.mk_libs + BUILDMAK_LIBS + ["-o", TARGET + ".elf"],
                    "link")]
        self.tc.run_batch(jobs, b, 1, None, "link")
        self._check(jobs[0], "link")
        fix = [Job(["psp-fixup-imports", TARGET + ".elf"], "fixup")]
        self.tc.run_batch(fix, b, 1, None, "fixup")
        self._check(fix[0], "psp-fixup-imports")
        if "stubs out of order" in fix[0].output:
            raise ConvertError("link", "psp-fixup-imports: stubs out of order", "a library order problem in the "
                               "runtime build; report it", log=self.log, tail=fix[0].output.splitlines()[-10:])
        st = [Job(["psp-strip", TARGET + ".elf", "-o", TARGET + "_strip.elf"], "strip")]
        self.tc.run_batch(st, b, 1, None, "strip")
        self._check(st[0], "psp-strip")
        return os.path.join(b, TARGET + ".elf"), os.path.join(b, TARGET + "_strip.elf")

    def _check(self, job, what):
        with open(self.log, "a", encoding="utf-8") as lf:
            lf.write("\n$ %s\n%s\n" % (" ".join(job.argv)[:4000], job.output))
        if job.rc != 0:
            raise ConvertError("link", "%s failed (code %s)" % (what, job.rc), "see the log", log=self.log,
                               tail=job.output.strip().splitlines()[-15:])

    # --- kernel PRX (psp_exc/Makefile + build.mak BUILD_PRX=1 USE_KERNEL_LIBS=1) -------------------------------------
    def build_prx(self):
        src = prx_dir(self.repo)
        mk = os.path.join(src, "Makefile")
        v = make_vars(mk) if os.path.exists(mk) else dict(PRX_DEFAULT)
        objs = v.get("OBJS", PRX_DEFAULT["OBJS"]).split()
        d = os.path.join(self.bdir, "prx")
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        for f in os.listdir(src):
            if f.endswith((".c", ".h", ".S", ".s")):
                _copy(os.path.join(src, f), os.path.join(d, f))
        cf = self.cflags(fw=int(v.get("PSP_FW_VERSION", "660")), incdir=(".",), base=v.get("CFLAGS", "").split())
        jobs = []
        for o in objs:
            stem = o[:-2]
            srcf = next((stem + e for e in (".c", ".S", ".s") if os.path.exists(os.path.join(d, stem + e))), None)
            if srcf is None:
                raise ConvertError("prx", "kernel module: no source for %s in %s" % (o, src))
            jobs.append(Job(["psp-gcc"] + cf + ["-c", "-o", o, srcf], o))      # make's built-in .c/.S rules
        self.tc.run_batch(jobs, d, len(jobs), None, "prx_cc")
        for j in jobs:
            self._check(j, "PRX compile " + j.label)
        ld = ["-L.", "-L" + self._psplib(), "-L" + self._sdk("lib"), "-specs=" + self._sdk("lib", "prxspecs"),
              "-Wl,-q,-T" + self._sdk("lib", "linkfile.prx"), "-Wl,-zmax-page-size=128"]
        if check_gpl_libs(v.get("LIBS", "").split(), "the kernel module's Makefile", self.opts.allow_gpl_libs):
            self.progress.warn("kernel module links GPL-3.0 libraries (--allow-gpl-libs): never share this build")
        libs = ["-nostdlib"] + v.get("LIBS", "").split() + ["-lpspdebug", "-lpspdisplay_driver", "-lpspctrl_driver",
                                                            "-lpspmodinfo", "-lpspsdk", "-lpspkernel"]
        steps = [Job(["psp-gcc"] + cf + ld + objs + [self._sdk("lib", "prxexports.o")] + libs + ["-o", "ps2rc_exc.elf"],
                     "prx_link"),
                 Job(["psp-fixup-imports", "ps2rc_exc.elf"], "prx_fixup"),
                 Job(["psp-prxgen", "ps2rc_exc.elf", "ps2rc_exc.prx"], "prx_gen")]
        for j in steps:
            self.tc.run_batch([j], d, 1, None, j.label)
            self._check(j, j.label)
        return os.path.join(d, "ps2rc_exc.prx")
