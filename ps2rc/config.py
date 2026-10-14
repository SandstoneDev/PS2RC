"""Per-machine settings of a checkout: where the game-derived work files live, where a pack's extracted disc is, and
external tools. Nothing here is tracked by git: the values come from `local.toml` at the repository root (or the file
named by PS2RC_LOCAL) and from environment variables, which win.

    # local.toml (gitignored)
    [paths]
    work = "<work dir>"               # translations, analysis caches, build products, stages (default: <repo>/build)
    analysis = "<analysis dir>"       # analysis results of the game binaries (default: <repo>/analysis)

    [games."SLUS-12345_v1.00"]        # one table per game pack (games/<name>/)
    disc = "<extracted disc>"         # the extracted disc: the folder holding SYSTEM.CNF and the boot ELF

    [tools]
    ffmpeg = "<path of ffmpeg>"       # optional tools (ffmpeg, ffprobe, ppsspp_dir, pcsx2_oracle, ...)
    wsl_distro = "Debian"             # Windows: the WSL distribution that runs the shell steps

Environment: PS2RC_WORK (work dir), PS2RC_ANALYSIS (analysis dir), PS2RC_DISC (disc dir of the pack being built), PS2RC_LOCAL (another local.toml).

Paths may be written in Windows form (with a drive letter); under Linux/WSL they are returned in their WSL form (under
/mnt, the drive letter in lower case).

    python3 config.py get work [--pack NAME]     prints one value (keys: work, disc, repo, tools.<name>)
"""
import os
import re
import sys

try:
    import tomllib
except ImportError:                                    # Python < 3.11
    tomllib = None


def repo_root():
    """The checkout root: the first parent directory of this file that holds pytest.ini."""
    d = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.exists(os.path.join(d, "pytest.ini")):
            return d
        up = os.path.dirname(d)
        if up == d:
            raise RuntimeError("ps2rc.config: no pytest.ini above %s" % __file__)
        d = up


def host_path(p):
    """A path in the form this OS opens: a drive-letter path -> its /mnt/<drive>/ form on Linux, and back on Windows."""
    if p is None:
        return None
    p = str(p)
    if os.name == "posix":
        m = re.match(r"^([A-Za-z]):[\\/](.*)$", p)
        if m:
            return "/mnt/%s/%s" % (m.group(1).lower(), m.group(2).replace("\\", "/"))
    else:
        m = re.match(r"^/mnt/([a-z])/(.*)$", p)
        if m:
            return m.group(1).upper() + ":/" + m.group(2)
    return p


def posix_path(p):
    """The WSL form of a path (for shell scripts run through wsl.exe)."""
    p = os.path.abspath(p) if not re.match(r"^[A-Za-z]:", str(p)) and os.name != "posix" else str(p)
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", p)
    if m:
        return "/mnt/%s/%s" % (m.group(1).lower(), m.group(2).replace("\\", "/"))
    return p


_LOCAL = None


def local():
    """The parsed local.toml ({} when there is none)."""
    global _LOCAL
    if _LOCAL is None:
        path = os.environ.get("PS2RC_LOCAL") or os.path.join(repo_root(), "local.toml")
        _LOCAL = {}
        if os.path.exists(path):
            if tomllib is None:
                raise RuntimeError("ps2rc.config: reading %s needs Python 3.11+ (tomllib)" % path)
            with open(path, "rb") as f:
                _LOCAL = tomllib.load(f)
    return _LOCAL


def work_dir():
    """Game-derived work files (never in git): PS2RC_WORK, else local.toml [paths] work, else <repo>/build."""
    w = os.environ.get("PS2RC_WORK") or local().get("paths", {}).get("work")
    return host_path(w) if w else os.path.join(repo_root(), "build")


def analysis_dir():
    """Analysis results of the game binaries (never in git): PS2RC_ANALYSIS, else local.toml [paths] analysis, else
    <repo>/analysis."""
    a = os.environ.get("PS2RC_ANALYSIS") or local().get("paths", {}).get("analysis")
    return host_path(a) if a else os.path.join(repo_root(), "analysis")


CORE_SOURCE_DIRS = ("hle/core", "hle/host", "hle/psp", "runtime", "audio", "psp_exc", "mk")
_SOURCES = None


def source_index():
    """{file name: path} of the C sources / headers / build files: the core dirs, then every game pack's tree (a name
    found in the core wins; within the packs the first one in sorted path order)."""
    global _SOURCES
    if _SOURCES is None:
        root = repo_root()
        idx = {}
        for d in CORE_SOURCE_DIRS:
            p = os.path.join(root, *d.split("/"))
            if os.path.isdir(p):
                for f in sorted(os.listdir(p)):
                    idx.setdefault(f, os.path.join(p, f))
        games = os.path.join(root, "games")
        if os.path.isdir(games):
            for dirpath, dirs, files in sorted(os.walk(games)):
                dirs.sort()
                if os.sep + "tests" in dirpath or os.sep + "tools" in dirpath:
                    continue
                for f in sorted(files):
                    if f.endswith((".c", ".h", ".S")):
                        idx.setdefault(f, os.path.join(dirpath, f))
        _SOURCES = idx
    return _SOURCES


def source_file(name):
    """Path of a source file by its file name (source_index); raises KeyError when there is none."""
    return source_index()[name]


def source_files(suffix=".c", pack=None):
    """Every indexed source file with that suffix (core + packs; with pack=NAME only the core + that pack)."""
    out = []
    for f, p in sorted(source_index().items()):
        if f.endswith(suffix) and (pack is None or os.sep + "games" + os.sep not in p or
                                   os.sep + "games" + os.sep + pack + os.sep in p):
            out.append(p)
    return out


def game_setting(pack, key, default=None):
    """local.toml [games."<pack>"] <key>."""
    return local().get("games", {}).get(pack, {}).get(key, default)


def disc_dir(pack, required=True):
    """The extracted disc of a pack: PS2RC_DISC, else local.toml [games."<pack>"] disc."""
    d = os.environ.get("PS2RC_DISC") or game_setting(pack, "disc")
    if not d and required:
        raise SystemExit('ps2rc: no disc folder for pack %s - set [games."%s"] disc = "..." in %s (or PS2RC_DISC)'
                         % (pack, pack, os.path.join(repo_root(), "local.toml")))
    return host_path(d) if d else None


def tool(name, default=None):
    """local.toml [tools] <name>."""
    v = local().get("tools", {}).get(name)
    return host_path(v) if v else default


def _wsl_distro():
    return os.environ.get("PS2RC_WSL_DISTRO") or local().get("tools", {}).get("wsl_distro") or "Debian"


def bash_argv(script, *args):
    """The argv that runs a bash script: natively on Linux, through WSL on Windows (paths in args are passed as they
    are - convert them with posix_path first)."""
    if os.name == "nt":
        return ["wsl.exe", "-d", _wsl_distro(), "--", "bash", posix_path(script)] + list(args)
    return ["bash", script] + list(args)


def bash_c_argv(command):
    """The argv that runs a bash command line: natively on Linux, through WSL on Windows."""
    if os.name == "nt":
        return ["wsl.exe", "-d", _wsl_distro(), "--", "bash", "-c", command]
    return ["bash", "-c", command]


def have_bash():
    """A shell for the build / test steps: bash on Linux, wsl.exe on Windows."""
    import shutil
    return shutil.which("wsl.exe" if os.name == "nt" else "bash") is not None


def main(argv):
    if len(argv) < 2 or argv[0] != "get":
        sys.exit(__doc__)
    key = argv[1]
    pack = argv[argv.index("--pack") + 1] if "--pack" in argv else None
    if key == "work":
        v = work_dir()
    elif key == "analysis":
        v = analysis_dir()
    elif key == "repo":
        v = repo_root()
    elif key == "disc":
        v = disc_dir(pack)
    elif key.startswith("tools."):
        v = tool(key[6:], "")
    else:
        sys.exit("unknown key %s" % key)
    print(posix_path(v) if "--posix" in argv else v)


if __name__ == "__main__":
    main(sys.argv[1:])
