"""Game packs: `games/<name>/pack.toml` describes one supported game version (identity, memory map, pipeline options,
the lists and modules the build uses). See games/_template/pack.toml for every key.

Path values are strings expanded here:
    {work}/x   the work dir (ps2rc.config.work_dir: game-derived files, never in git)
    {repo}/x   the checkout root
    {disc}/x   the pack's extracted disc (local.toml [games."<name>"] disc)
    x          relative to the pack directory
Which pack a command uses: --pack NAME / PS2RC_PACK, else local.toml [defaults] pack, else "example".

    python3 pack.py shell [NAME] [--posix]   shell assignments (PACK_*) for the build scripts
    python3 pack.py get [NAME] KEY            one value (dotted key, paths expanded)
    python3 pack.py cheader [NAME] OUT        hle_pack.h for the HLE build (rewritten only when it changes)
    python3 pack.py profile NAME|- PROFILE BDIR [GEN_DIR] [--posix]
                                              shell assignments (PROF_*) of a build profile for mk/build.sh
"""
import os
import shlex
import sys

try:
    import tomllib
except ImportError:                                    # Python < 3.11
    tomllib = None

if __package__ in (None, ""):                          # run as a script: python3 src/ps2rc/pack.py
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2rc import config  # noqa: E402

FORMAT = 1


def default_name():
    return os.environ.get("PS2RC_PACK") or config.local().get("defaults", {}).get("pack") or "example"


def games_dir():
    return os.path.join(config.repo_root(), "games")


PRIVATE_TOML = "pack.private.toml"


def _merge(base, over):
    """over -> base: tables are merged key by key, every other value is replaced"""
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v


class Pack:
    def __init__(self, path):
        self.dir = os.path.abspath(path)
        self.name = os.path.basename(self.dir)
        if tomllib is None:
            raise RuntimeError("ps2rc.pack needs Python 3.11+ (tomllib)")
        with open(os.path.join(self.dir, "pack.toml"), "rb") as f:
            self.data = tomllib.load(f)
        local = os.path.join(self.dir, PRIVATE_TOML)
        if os.path.exists(local):                          # a developer checkout's local additions (never published)
            with open(local, "rb") as f:
                _merge(self.data, tomllib.load(f))
        fmt = self.data.get("format", FORMAT)
        if fmt != FORMAT:
            raise SystemExit("pack %s: format %s, this ps2rc reads format %d" % (self.name, fmt, FORMAT))

    def get(self, key, default=None):
        """Dotted key lookup ("build.translation"), raw value."""
        v = self.data
        for k in key.split("."):
            if not isinstance(v, dict) or k not in v:
                return default
            v = v[k]
        return v

    def expand(self, s):
        """Expand one path string (see the module doc)."""
        if not isinstance(s, str):
            return s
        if s.startswith("{work}"):
            return os.path.normpath(config.work_dir() + s[6:])
        if s.startswith("{repo}"):
            return os.path.normpath(config.repo_root() + s[6:])
        if s.startswith("{disc}"):
            return os.path.normpath(self.disc() + s[6:])
        if s.startswith("{pack}"):
            return os.path.normpath(self.dir + s[6:])
        if os.path.isabs(s) or (len(s) > 1 and s[1] == ":"):
            return s
        return os.path.normpath(os.path.join(self.dir, s))

    def path(self, key, default=None):
        """Dotted key lookup with the path expanded (None when missing and no default)."""
        v = self.get(key)
        if v is None:
            return self.expand(default) if default is not None else None
        return self.expand(v)

    def disc(self, required=True):
        """The extracted disc folder: local.toml / PS2RC_DISC, else the pack's own [run] disc (a pack that builds its
        program itself, e.g. games/example), else an error when required."""
        d = config.disc_dir(self.name, required=False)
        if d:
            return d
        if self.get("run.disc"):
            return self.path("run.disc")
        return config.disc_dir(self.name, required)


def load(name=None):
    """A pack by name (directory under games/) or by path."""
    name = name or default_name()
    p = name if os.path.isdir(name) else os.path.join(games_dir(), name)
    if not os.path.exists(os.path.join(p, "pack.toml")):
        raise SystemExit("ps2rc: no game pack %s (games/%s/pack.toml)" % (name, name))
    return Pack(p)


# build-script variables: (shell name, pack key, is a path)
SHELL_VARS = [
    ("PACK_TRANS", "build.translation", True),
    ("PACK_PATCH_DIR", "build.patch_dir", True),
    ("PACK_TRAPSET", "build.trapset", True),
    ("PACK_TEXTPATCH", "build.textpatch", True),
    ("PACK_HOOKS", "build.hooks", True),
    ("PACK_ZONES", "build.zones", True),
    ("PACK_SITES", "build.sites", True),
    ("PACK_NATIVES", "build.natives", True),
    ("PACK_LAYOUT", "build.layout", True),
    ("PACK_PKG_OUT", "build.pkg_out", True),
    ("PACK_CALLFLUSH", "build.callflush", False),
    ("PACK_ELF", "id.boot_elf", False),
    ("PACK_TITLE", "id.psp_title", False),
    ("PACK_PSP_LDFLAGS", "build.psp_ldflags", False),
    ("PACK_SOURCES", "build.sources", True),
    ("PACK_AUDIO_SOURCES", "build.audio_sources", True),
    ("PACK_DEFINES", "build.defines", False),
    ("PACK_Y_HDR", "build.y_hdr", False),
    ("PACK_Y_GEN", "build.y_gen", False),
    ("PACK_ALIAS", "build.alias", False),
    ("PACK_DISCMAP", "run.discmap", True),
    ("PACK_RUN_OUT", "run.out", True),
    ("PACK_DISC_OVERRIDE", "run.disc_override", True),
]


def shell(pack, posix=False):
    conv = config.posix_path if posix else (lambda p: p)
    lines = ["PACK_NAME=%s" % shlex.quote(pack.name), "PACK_DIR=%s" % shlex.quote(conv(pack.dir)),
             "PACK_WORK=%s" % shlex.quote(conv(config.work_dir()))]
    disc = pack.disc(required=False)
    lines.append("PACK_DISC=%s" % shlex.quote(conv(disc) if disc else ""))
    for var, key, is_path in SHELL_VARS:
        v = pack.get(key)
        if v is None:
            s = ""
        elif isinstance(v, list):
            items = []
            for x in v:
                if is_path and "=" in x:                   # NAME=path (disc overrides)
                    k, p = x.split("=", 1)
                    items.append("%s=%s" % (k, conv(pack.expand(p))))
                else:
                    items.append(conv(pack.expand(x)) if is_path else str(x))
            s = " ".join(items)
        else:
            s = conv(pack.expand(v)) if is_path else str(v)
        lines.append("%s=%s" % (var, shlex.quote(s)))
    return "\n".join(lines)


def _hook_addr(pack, label, what):
    """Guest address of a HOOK / TRACE label of the pack's hooks.def."""
    from ps2rc import bind
    for _k, ov, addr, name in bind.read_hooks(pack.path("build.hooks")):
        if name == label:
            if ov:
                raise SystemExit("pack %s: [runtime] %s = %s is an overlay function" % (pack.name, what, label))
            return addr
    raise SystemExit("pack %s: [runtime] %s = %s is not a label of hooks.def" % (pack.name, what, label))


def _c_str(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def cheader(pack):
    """hle_pack.h: the pack's values the core HLE reads (memory map, boot program, main loop, ...)."""
    g = pack.get
    lines = ["/* hle_pack.h - generated by ps2rc pack.py from games/%s/pack.toml. Do not edit. */" % pack.name,
             "#ifndef HLE_PACK_H", "#define HLE_PACK_H", "",
             "#define HLE_PACK_NAME %s" % _c_str(pack.name)]
    elf = g("id.boot_elf")
    lines.append("#define HLE_PACK_BOOT_ELF %s" % _c_str(elf))
    lines.append("#define HLE_PACK_BOOT_ARG0 %s" % _c_str(g("runtime.boot_arg0", "cdrom0:\\%s;1" % elf)))
    code = g("memory.code")
    for name, v in (("CODE_LO", code[0]), ("CODE_HI", code[1]), ("DATA_END", g("memory.data_end")),
                    ("BSS_END", g("memory.bss_end")), ("RAM_END_PSP", g("memory.ram_end_psp", 0x02000000))):
        lines.append("#define HLE_PACK_%s 0x%08xu" % (name, v))
    ml = g("runtime.main_loop")
    if ml:
        lines.append("#define HLE_PACK_MAIN_LOOP_ADDR 0x%08xu" % _hook_addr(pack, ml, "main_loop"))
        lines.append("#define HLE_PACK_MAIN_LOOP_NAME %s" % _c_str(ml))
    else:                                              # no main-loop label: iterations are never counted
        lines.append("#define HLE_PACK_MAIN_LOOP_ADDR 0xffffffffu")
        lines.append('#define HLE_PACK_MAIN_LOOP_NAME "(no main loop label)"')
    st = g("runtime.selftest_at")
    lines.append("#define HLE_PACK_SELFTEST_ADDR 0x%08xu" % (_hook_addr(pack, st, "selftest_at") if st else 0xffffffff))
    loops = g("runtime.wait_loops", [])
    lines.append("#define HLE_PACK_IS_WAIT_LOOP(a) (%s)" % (" || ".join("(a) == 0x%08xu" % a for a in loops) or "0"))
    fnv = g("verify.data_fnv64")
    lines.append("#define HLE_PACK_HAVE_DATA_FNV %d" % (1 if fnv else 0))
    lines.append("#define HLE_PACK_DATA_FNV 0x%016xull" % (int(fnv, 16) if fnv else 0))
    lines.append("#define HLE_PACK_CFG %s" % _c_str(g("runtime.cfg", "ps2rc.cfg")))
    if g("runtime.cfg_legacy"):                        # an older name of the cfg (hle/core/hle_cfg.c falls back to it)
        lines.append("#define HLE_PACK_CFG_LEGACY %s" % _c_str(g("runtime.cfg_legacy")))
    envp = g("runtime.cfg_env_prefixes", [])
    if envp:                                           # host: cfg keys exported as HLE_<KEY> (hle/core/hle_cfg.c)
        lines.append("#define HLE_PACK_CFG_ENV_PREFIXES %s" % ", ".join(_c_str(x) for x in envp))
    crt0 = g("boot.crt0")                              # the start-up facts (hle/core/hle_crt0.c, HLE_CRT0_DATA)
    if crt0:
        for key, name in (("entry", "ENTRY"), ("gp", "GP"), ("stack", "STACK"), ("stack_size", "STACK_SIZE"),
                          ("args", "ARGS"), ("thread_exit", "THREAD_EXIT"), ("heap", "HEAP"),
                          ("heap_size", "HEAP_SIZE"), ("exit", "EXIT")):
            lines.append("#define HLE_PACK_CRT0_%s 0x%08xu" % (name, crt0[key] & 0xFFFFFFFF))
        lines.append("#define HLE_PACK_CRT0_BSS_LO 0x%08xu" % crt0["bss"][0])
        lines.append("#define HLE_PACK_CRT0_BSS_HI 0x%08xu" % crt0["bss"][1])
        for key, name in (("init", "INIT"), ("flush", "FLUSH"), ("main", "MAIN")):
            fn, ra = crt0[key]
            lines.append("#define HLE_PACK_CRT0_%s 0x%08xu" % (name, fn))
            lines.append("#define HLE_PACK_CRT0_%s_RA 0x%08xu" % (name, ra))
    lines.append("#define HLE_PACK_PSP_MODULE %s" % _c_str(g("runtime.psp_module", "PS2RC")))
    zones = g("runtime.zones", [])
    lines.append("#define HLE_PACK_ZONE_LIST(X)%s" % "".join(" X(%s, %s, %s, %d)" % (z[0], _c_str(z[1]), z[2] or 0, z[3])
                                                             for z in zones))
    for name, v in sorted((g("symbols") or {}).items()):
        lines.append("#define HLE_SYM_%s 0x%08xu" % (name, v))
        lines.append("#define HLE_HAVE_SYM_%s 1" % name)
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def _profile_paths(pack, items):
    """A profile's file / folder entries ({repo}/... or pack-relative) -> the files they name (*.c *.h *.S of a
    folder and of its psp/ or host/ subfolder are listed by the build script itself: folders are returned as they are)."""
    out = []
    for x in items or []:
        out.append(os.path.normpath(x.replace("{repo}", config.repo_root())) if "{repo}" in x else pack.expand(x))
    return out


def profile(pack, name, bdir, gen_dir=None, posix=False):
    """Shell assignments (PROF_*) of the pack's build profile [profile.<name>] for mk/build.sh (the converter's
    ps2rc/convert/build.py applies the same keys): the hooks.def / zones.def copies without the profile's
    exclude_hooks (written into bdir), the source basenames it excludes, its natives / defines / extra sources, and the
    generated modules found in gen_dir (a converter work dir's modules/)."""
    import re
    prof = pack.get("profile.%s" % name)
    if prof is None:
        raise SystemExit("pack %s: no [profile.%s]" % (pack.name, name))
    conv = config.posix_path if posix else (lambda p: p)
    os.makedirs(bdir, exist_ok=True)
    drop = set(prof.get("exclude_hooks", []) or [])
    hooks = pack.path("build.hooks")
    out, addrs = [], set()
    for ln in open(hooks, encoding="utf-8").read().splitlines(True):
        m = re.match(r"\s*(?:HOOK|TRACE)(?:_OV)?\(\s*(?:\d+\s*,\s*)?([0-9a-f]{8})\s*,\s*(\w+)", ln)
        if m and m.group(2) in drop:
            addrs.add(m.group(1))
            continue
        out.append(ln)
    hp = os.path.join(bdir, "hooks_profile.def")
    write_if_changed(hp, "".join(out))
    zp = ""
    if pack.get("build.zones"):
        zout = []
        for ln in open(pack.path("build.zones"), encoding="utf-8").read().splitlines(True):
            m = re.match(r"(\s*ZONE\(\s*([0-9a-f]{8})\s*,\s*\w+\s*,\s*)([^)]*)(\).*)", ln, re.S)
            if m and m.group(2) in addrs:               # a zone on the guest original needs the hook
                flags = [x.strip() for x in m.group(3).split("|") if x.strip() not in ("PZF_GUEST", "")]
                ln = m.group(1) + ("|".join(flags) or "0") + m.group(4)
            zout.append(ln)
        zp = os.path.join(bdir, "zones_profile.def")
        write_if_changed(zp, "".join(zout))
    gens = []
    for g in prof.get("generators", []) or []:
        p = os.path.join(gen_dir, g["out"]) if gen_dir else ""
        if not p or not os.path.exists(p):
            raise SystemExit("profile %s: generator %s: no %s (GEN_DIR = a converter work dir's modules/)"
                             % (name, g.get("name"), p or g["out"]))
        gens.append("%s:%s:%d:%d" % (conv(p), g.get("define", "HLE_GEN_%s" % g["name"].upper()),
                                     1 if g.get("include_only") else 0, 1 if g.get("audio") else 0))
    q = lambda xs: shlex.quote(" ".join(xs))
    lines = ["PROF_HOOKS=%s" % shlex.quote(conv(hp)), "PROF_ZONES=%s" % shlex.quote(conv(zp) if zp else ""),
             "PROF_EXCLUDE=%s" % q(os.path.basename(x) for x in _profile_paths(pack, prof.get("exclude_sources"))),
             "PROF_NATIVES_OFF=%d" % (1 if "natives" in prof and not prof["natives"] else 0),
             "PROF_DEFINES=%s" % q(prof.get("defines", []) or []),
             "PROF_EXTRA=%s" % q(conv(x) for x in _profile_paths(pack, prof.get("extra_sources"))),
             "PROF_INCLUDE=%s" % q(conv(x) for x in _profile_paths(pack, prof.get("include_sources"))),
             "PROF_AUDIO_EXTRA=%s" % q(conv(x) for x in _profile_paths(pack, prof.get("audio_extra_sources"))),
             "PROF_AUDIO_INCLUDE=%s" % q(conv(x) for x in _profile_paths(pack, prof.get("audio_include_sources"))),
             "PROF_GEN=%s" % q(gens)]
    return "\n".join(lines)


def write_if_changed(path, text):
    old = open(path, encoding="ascii").read() if os.path.exists(path) else None
    if old != text:
        with open(path, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    return old != text


def main(argv):
    posix = "--posix" in argv
    argv = [a for a in argv if a != "--posix"]
    if not argv:
        sys.exit(__doc__)
    if argv[0] == "shell":
        print(shell(load(argv[1] if len(argv) > 1 else None), posix))
    elif argv[0] == "cheader":                             # cheader [NAME] OUT
        name, out = (argv[1], argv[2]) if len(argv) > 2 else (None, argv[1])
        changed = write_if_changed(out, cheader(load(name)))
        print("pack: %s%s" % (out, "" if changed else " (unchanged)"))
    elif argv[0] == "profile":                             # profile NAME|- PROFILE BDIR [GEN_DIR]
        name = None if argv[1] == "-" else argv[1]
        print(profile(load(name), argv[2], argv[3], argv[4] if len(argv) > 4 else None, posix))
    elif argv[0] == "get":
        name, key = (argv[1], argv[2]) if len(argv) > 2 else (None, argv[1])
        p = load(name)
        v = p.path(key) if isinstance(p.get(key), str) else p.get(key)
        print(config.posix_path(v) if posix and isinstance(v, str) else v)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
