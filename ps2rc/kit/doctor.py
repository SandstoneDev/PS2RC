"""python -m ps2rc doctor - what this machine has, and what is missing, for the ps2rc toolkit.

    python -m ps2rc doctor [--json] [--no-wsl] [--no-docker] [--quick]

Checks only. It reads no game data (a disc folder named in local.toml is tested for existence, never opened), downloads
nothing, installs nothing and writes nothing. Every missing item comes with an install hint; installing is the user's
decision (an agent asks first).

  --quick       skip the probes that start other programs (WSL, Docker, compilers, emulators): Python-side only
  --no-wsl      do not probe WSL distributions (Windows)
  --no-docker   do not probe Docker
  --json        machine-readable output (a list of checks plus the summary)

Exit status: 0 when the core requirements are present (analysis and translation can run), 1 otherwise.
"""
import argparse
import importlib
import json
import os
import re
import shutil
import subprocess
import sys

from ps2rc import config

MIN_PY = (3, 11)
MIN_PSP_GCC = 9                    # older community psp-gcc builds cannot compile the runtime (gnu11)
WORK_GB = 10                       # translations, caches and builds of one game (docs/PORTING_GUIDE.md)
NOWIN = 0x08000000 if os.name == "nt" else 0

HINT_PY = "install Python 3.11 or newer (https://www.python.org/downloads/)"
HINT_PIP = "python -m pip install %s"
HINT_PSPDEV = ("install the pspdev toolchain (https://pspdev.github.io/ ; sources https://github.com/pspdev/pspdev); "
               "on Windows inside WSL2 (https://learn.microsoft.com/en-us/windows/wsl/install), or use the Docker "
               "image pspdev/pspdev (downloading it needs the user's consent)")
HINT_GCC = "install gcc, make and the zlib headers in the build shell (Debian/Ubuntu: sudo apt install build-essential zlib1g-dev)"
HINT_FFMPEG = "optional: install ffmpeg (https://ffmpeg.org/download.html) and set [tools] ffmpeg in local.toml"
HINT_PPSSPP = ("optional: install PPSSPP (https://www.ppsspp.org/) and set [tools] ppsspp_dir in local.toml "
               "(the folder with the executable and a memstick/ folder)")
HINT_PCSX2 = ("optional: install PCSX2 (https://pcsx2.net/) for bit-exact reference runs; it needs a BIOS dumped from "
              "the user's own PS2 (never download one); set [tools] pcsx2_install / pcsx2_oracle / ps2_bios_dir")
HINT_LOCAL = ("create local.toml at the repository root: [paths] work = \"<work dir>\" and one "
              "[games.\"<pack>\"] disc = \"<extracted disc>\" per game (see ps2rc/config.py)")


class Check:
    def __init__(self, area, name, status, detail="", hint="", need="core"):
        self.area, self.name, self.status, self.detail, self.hint, self.need = area, name, status, detail, hint, need

    def to_dict(self):
        return dict(self.__dict__)


def _run(argv, timeout=30, stdin=None):
    """(returncode, text) of a short command; (None, reason) when it cannot start. A script goes in through stdin
    (stdin=bytes): quotes inside a command-line argument do not survive the trip through wsl.exe."""
    try:
        kw = {"creationflags": NOWIN} if os.name == "nt" else {}
        if stdin is None:
            kw["stdin"] = subprocess.DEVNULL
        else:
            kw["input"] = stdin
        r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, **kw)
        out = r.stdout
        if isinstance(out, bytes):
            out = out.decode("utf-16-le", "ignore") if out[1:2] == b"\x00" else out.decode("utf-8", "replace")
        return r.returncode, out
    except (OSError, subprocess.SubprocessError) as e:
        return None, str(e)


# ---- Python side ------------------------------------------------------------------------------------------------
def check_python(out):
    v = sys.version_info
    ok = v[:2] >= MIN_PY
    out.append(Check("core", "Python", "ok" if ok else "missing", "%d.%d.%d (%s)" % (v[0], v[1], v[2], sys.executable),
                     "" if ok else HINT_PY))
    for mod, pkg, need, why in (("elftools", "pyelftools", "core", "ELF loading (ps2rc/image.py, tools/ps2elf)"),
                                ("pytest", "pytest", "core", "the test suite"),
                                ("PIL", "Pillow", "optional", "menu pictures in other formats than PNG (converter)"),
                                ("tkinter", None, "optional", "the converter's GUI")):
        try:
            m = importlib.import_module(mod)
            ver = getattr(m, "__version__", "") or getattr(m, "TkVersion", "")
            out.append(Check("core" if need == "core" else "optional", pkg or mod, "ok", "%s %s" % (why, ver)))
        except Exception:
            hint = (HINT_PIP % pkg) if pkg else "install the Tk package of your Python (python.org builds include it)"
            out.append(Check("core" if need == "core" else "optional", pkg or mod,
                             "missing" if need == "core" else "warn", why, hint, need))


def check_git(out):
    g = shutil.which("git")
    if not g:
        out.append(Check("core", "git", "missing", "", "install git from your system's package manager"))
        return
    rc, text = _run([g, "--version"], timeout=15)
    out.append(Check("core", "git", "ok" if rc == 0 else "warn", text.strip().splitlines()[0] if text.strip() else ""))


def check_checkout(out):
    root = config.repo_root()
    need = ("ps2rc", "runtime", "hle", "mk", "games")
    missing = [d for d in need if not os.path.isdir(os.path.join(root, d))]
    out.append(Check("core", "checkout", "ok" if not missing else "missing", root,
                     "" if not missing else "incomplete checkout, missing: %s (clone the repository again)"
                     % ", ".join(missing)))
    if not any(os.path.isfile(os.path.join(root, *f)) for f in (("AGENTS.md",), ("docs", "public", "AGENTS.md"))):
        return                                      # a checkout without the agent kit (e.g. the converter's repository)
    for d in ("skills", "knowledge"):
        p = os.path.join(root, d)
        alt = os.path.join(root, "docs", "public", d)
        if os.path.isdir(p) or os.path.isdir(alt):
            out.append(Check("setup", "%s/" % d, "ok", "present"))
        else:
            out.append(Check("setup", "%s/" % d, "warn", "not found", "the agent kit is incomplete", "optional"))


def check_local(out, packs_out):
    root = config.repo_root()
    path = os.environ.get("PS2RC_LOCAL") or os.path.join(root, "local.toml")
    if not os.path.exists(path):
        out.append(Check("setup", "local.toml", "warn", "not found (defaults: work = <repo>/build)", HINT_LOCAL, "setup"))
        cfg = {}
    else:
        try:
            cfg = config.local()
            out.append(Check("setup", "local.toml", "ok", "found"))
        except Exception as e:                                   # a broken file must not stop the doctor
            out.append(Check("setup", "local.toml", "missing", "cannot be read: %s" % e, "fix the TOML syntax"))
            cfg = {}
    work = config.work_dir()
    probe = work
    while probe and not os.path.exists(probe):
        up = os.path.dirname(probe)
        if up == probe:
            break
        probe = up
    try:
        free = shutil.disk_usage(probe).free / 1e9
        st = "ok" if free >= WORK_GB else "warn"
        out.append(Check("setup", "work dir", st, "%s (%.1f GB free; plan for %d GB per game, plus the disc image)"
                         % (work, free, WORK_GB), "" if st == "ok" else "free disk space or point [paths] work "
                         "elsewhere", "setup"))
    except OSError as e:
        out.append(Check("setup", "work dir", "warn", "%s: %s" % (work, e), "", "setup"))
    games = os.path.join(root, "games")
    for name in sorted(os.listdir(games)) if os.path.isdir(games) else []:
        if not os.path.exists(os.path.join(games, name, "pack.toml")):
            continue
        if name in ("_template", "example"):
            packs_out.append({"pack": name, "disc": None, "disc_exists": None})
            continue
        disc = cfg.get("games", {}).get(name, {}).get("disc")
        exists = bool(disc) and os.path.isdir(config.host_path(disc))
        packs_out.append({"pack": name, "disc": "set" if disc else None, "disc_exists": exists})
        out.append(Check("setup", "pack %s" % name, "ok" if exists else "warn",
                         "disc folder %s" % ("found" if exists else "set but not found" if disc else "not set"),
                         "" if exists else '[games."%s"] disc = "<extracted disc>" in local.toml' % name, "setup"))


# ---- build shell, toolchains ------------------------------------------------------------------------------------
SHELL_PROBE = r'''
echo "python3=$(python3 -c 'import sys;print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null)"
echo "gcc=$(gcc -dumpversion 2>/dev/null)"
echo "gcc_machine=$(gcc -dumpmachine 2>/dev/null)"
echo "make=$(make --version 2>/dev/null | head -n 1)"
if echo '#include <zlib.h>' | gcc -E -x c - >/dev/null 2>&1; then echo zlib=yes; else echo zlib=no; fi
d=""
for x in "$PSPDEV" "$HOME/pspdev" /usr/local/pspdev; do
  if [ -n "$x" ] && [ -x "$x/bin/psp-gcc" ]; then d="$x"; break; fi
done
echo "pspdev=$d"
if [ -n "$d" ]; then echo "pspgcc=$("$d/bin/psp-gcc" -dumpversion 2>/dev/null)"; fi
echo "nproc=$(nproc 2>/dev/null)"
'''


def _parse_kv(text):
    kv = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    return kv


def _wsl_distros():
    if os.name != "nt" or not shutil.which("wsl.exe"):
        return None
    rc, text = _run(["wsl.exe", "-l", "-q"], timeout=30)
    if rc is None:
        return None
    return [d.strip().strip("\x00") for d in text.splitlines()
            if d.strip().strip("\x00") and not d.strip().lower().startswith("docker-desktop")]


def probe_shell(no_wsl=False):
    """-> (description, {key: value}) of the shell the build scripts run in, or (reason, None)."""
    if os.name == "nt":
        if no_wsl:
            return "WSL not probed (--no-wsl)", None
        dists = _wsl_distros()
        if dists is None:
            return "WSL is not installed", None
        want = os.environ.get("PS2RC_WSL_DISTRO") or config.local().get("tools", {}).get("wsl_distro") or "Debian"
        if want not in dists:
            return "WSL distribution %r not found (installed: %s)" % (want, ", ".join(dists) or "none"), None
        rc, text = _run(["wsl.exe", "-d", want, "--", "bash", "-l", "-s"], timeout=120, stdin=SHELL_PROBE.encode())
        if rc is None:
            return "WSL %s did not answer: %s" % (want, text), None
        return "WSL %s" % want, _parse_kv(text)
    bash = shutil.which("bash")
    if not bash:
        return "bash not found", None
    rc, text = _run([bash, "-l", "-s"], timeout=60, stdin=SHELL_PROBE.encode())
    return ("bash (%s)" % sys.platform), _parse_kv(text or "")


def _major(v):
    m = re.match(r"(\d+)", v or "")
    return int(m.group(1)) if m else 0


def check_shell(out, no_wsl):
    where, kv = probe_shell(no_wsl)
    hint_shell = ("install WSL2 with a Debian or Ubuntu distribution (https://learn.microsoft.com/en-us/windows/wsl/"
                  "install) and set [tools] wsl_distro in local.toml if it is not called Debian"
                  if os.name == "nt" else "install bash")
    if kv is None:
        st = "skip" if "not probed" in where else "missing"
        out.append(Check("build", "build shell", st, where, "" if st == "skip" else hint_shell, "build"))
        return None
    out.append(Check("build", "build shell", "ok", where))
    pv = kv.get("python3", "")
    ok = tuple(int(x) for x in pv.split(".")[:2]) >= MIN_PY if re.match(r"\d+\.\d+", pv) else False
    out.append(Check("build", "python3 in the build shell", "ok" if ok else "missing", pv or "not found",
                     "" if ok else "the build scripts run python3 3.11+ in the build shell (tomllib); install it there",
                     "build"))
    gcc_ok = bool(kv.get("gcc"))
    mach = kv.get("gcc_machine", "")
    host_ok = gcc_ok and mach.startswith("x86_64") and kv.get("zlib") == "yes" and bool(kv.get("make"))
    detail = "gcc %s (%s), make %s, zlib headers %s" % (kv.get("gcc") or "missing", mach or "?",
                                                          "found" if kv.get("make") else "missing",
                                                          kv.get("zlib", "?"))
    hint = ""
    if not host_ok:
        hint = HINT_GCC
        if gcc_ok and not mach.startswith("x86_64"):
            hint = "the host harness (mk/Makefile.host) is written for x86-64 Linux; other hosts: Unknown"
    out.append(Check("build", "host compiler (host build)", "ok" if host_ok else "missing", detail, hint, "host"))
    if kv.get("pspdev"):
        v = kv.get("pspgcc", "")
        st = "ok" if _major(v) >= MIN_PSP_GCC else "missing"
        out.append(Check("build", "pspdev in the build shell", st, "%s, psp-gcc %s" % (kv["pspdev"], v or "?"),
                         "" if st == "ok" else "psp-gcc %d or newer is needed; update pspdev" % MIN_PSP_GCC, "psp"))
    else:
        out.append(Check("build", "pspdev in the build shell", "missing", "no psp-gcc in $PSPDEV, ~/pspdev or "
                         "/usr/local/pspdev", HINT_PSPDEV, "psp"))
    return kv


def check_native_toolchain(out, no_docker):
    """The converter can also use a native toolchain folder (bundled, --toolchain, PSPDEV / PATH) or Docker."""
    found = None
    try:
        from ps2rc.convert import toolchain as tc
        t, tried = tc.detect(prefer="native")
        found = "%s, psp-gcc %s" % (t.description, t.version)
    except Exception:
        found = None
    out.append(Check("build", "native PSP toolchain (converter)", "ok" if found else "info",
                     found or "none (the converter then uses WSL or Docker)", "", "optional"))
    if no_docker:
        return
    if not shutil.which("docker"):
        out.append(Check("build", "Docker pspdev image", "info", "docker not installed", "", "optional"))
        return
    rc, _ = _run(["docker", "image", "inspect", "pspdev/pspdev:latest"], timeout=30)
    out.append(Check("build", "Docker pspdev image", "ok" if rc == 0 else "info",
                     "present locally" if rc == 0 else "not present locally (never pulled without consent)",
                     "", "optional"))


# ---- optional tools ---------------------------------------------------------------------------------------------
def _first_line(argv):
    rc, text = _run(argv, timeout=30)
    return text.strip().splitlines()[0] if rc == 0 and text.strip() else None


def check_ffmpeg(out, quick):
    exe = config.tool("ffmpeg") or shutil.which("ffmpeg")
    if not exe or not os.path.exists(exe):
        out.append(Check("optional", "ffmpeg", "warn", "not found (movies, animated menu icon)", HINT_FFMPEG,
                         "optional"))
        return
    ver = None if quick else _first_line([exe, "-version"])
    out.append(Check("optional", "ffmpeg", "ok", ver or exe))


def check_ppsspp(out):
    d = os.environ.get("PS2RC_PPSSPP_DIR") or config.tool("ppsspp_dir")
    names = ("PPSSPPWindows64.exe", "PPSSPPSDL", "PPSSPPQt", "ppsspp")
    exe = None
    if d and os.path.isdir(d):
        exe = next((os.path.join(d, n) for n in names if os.path.exists(os.path.join(d, n))), None)
    exe = exe or next((shutil.which(n) for n in names if shutil.which(n)), None)
    if not exe:
        out.append(Check("optional", "PPSSPP", "warn", "not found (boolean checks: boots / renders / plays)",
                         HINT_PPSSPP, "optional"))
        return
    note = "" if os.name == "nt" else " - tools/dev/ppsspp_run.py is Windows-only today: start PPSSPP by hand"
    memstick = d and os.path.isdir(os.path.join(d, "memstick"))
    out.append(Check("optional", "PPSSPP", "ok", exe + ("" if memstick or not d else " (no memstick/ folder next to "
                                                         "it)") + note))


def check_pcsx2(out):
    keys = ("pcsx2_install", "pcsx2_oracle", "ps2_bios_dir")
    vals = {k: config.tool(k) for k in keys}
    exe = shutil.which("pcsx2-qt") or shutil.which("pcsx2") or shutil.which("pcsx2-qtx64.exe")
    if not any(vals.values()) and not exe:
        out.append(Check("optional", "PCSX2 (reference oracle)", "warn", "not configured", HINT_PCSX2, "optional"))
        return
    parts = []
    for k in keys:
        v = vals[k]
        parts.append("%s %s" % (k, "found" if v and os.path.exists(v) else "set, not found" if v else "not set"))
    if exe:
        parts.append("executable on PATH")
    good = all(vals[k] and os.path.exists(vals[k]) for k in keys)
    out.append(Check("optional", "PCSX2 (reference oracle)", "ok" if good else "warn", "; ".join(parts),
                     "" if good else HINT_PCSX2, "optional"))


def check_signature_db(out):
    db = config.tool("sce_signature_db")
    if db and os.path.exists(db):
        out.append(Check("optional", "library signature database", "ok", "set in local.toml [tools] sce_signature_db"))
    else:
        out.append(Check("optional", "library signature database", "info",
                         "not set: naming the Sony library functions needs one the user supplies "
                         "(tools/ps2elf/sce_sigscan.py; check its licence)", "", "optional"))


# ---- summary ------------------------------------------------------------------------------------------------------
def summary(checks):
    st = {c.name: c.status for c in checks}

    def ok(*names):
        return all(st.get(n) == "ok" for n in names)

    psp_build = ok("python3 in the build shell", "pspdev in the build shell") or \
        st.get("native PSP toolchain (converter)") == "ok" or st.get("Docker pspdev image") == "ok"
    caps = {
        "analysis and translation": ok("Python", "pyelftools", "checkout"),
        "tests (data-free)": ok("Python", "pyelftools", "pytest"),
        "host build and run": ok("python3 in the build shell", "host compiler (host build)"),
        "PSP build": bool(psp_build),
        "PPSSPP checks": ok("PPSSPP"),
        "PCSX2 reference runs": ok("PCSX2 (reference oracle)"),
        "movies": ok("ffmpeg"),
    }
    core_missing = [c.name for c in checks if c.need == "core" and c.status == "missing"]
    return caps, core_missing


def run(quick=False, no_wsl=False, no_docker=False):
    checks, packs = [], []
    check_python(checks)
    check_git(checks)
    check_checkout(checks)
    check_local(checks, packs)
    if quick:
        checks.append(Check("build", "build shell", "skip", "not probed (--quick)", "", "build"))
    else:
        check_shell(checks, no_wsl)
        check_native_toolchain(checks, no_docker)
    check_ffmpeg(checks, quick)
    check_ppsspp(checks)
    check_pcsx2(checks)
    check_signature_db(checks)
    caps, core_missing = summary(checks)
    return checks, packs, caps, core_missing


MARK = {"ok": "[ok]  ", "missing": "[MISS]", "warn": "[warn]", "info": "[info]", "skip": "[skip]"}
AREAS = (("core", "Core (needed for everything)"), ("setup", "This checkout"), ("build", "Builds"),
         ("optional", "Optional tools"))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m ps2rc doctor", description=__doc__.split("\n\n")[0])
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--no-wsl", action="store_true")
    ap.add_argument("--no-docker", action="store_true")
    a = ap.parse_args(argv)
    checks, packs, caps, core_missing = run(a.quick, a.no_wsl, a.no_docker)
    if a.json:
        print(json.dumps({"checks": [c.to_dict() for c in checks], "packs": packs, "ready": caps,
                          "core_missing": core_missing, "platform": sys.platform}, indent=1))
        return 1 if core_missing else 0
    print("ps2rc doctor: checks only - no game data read, nothing downloaded")
    for area, title in AREAS:
        rows = [c for c in checks if c.area == area]
        if not rows:
            continue
        print("\n%s" % title)
        for c in rows:
            print("  %s %-34s %s" % (MARK.get(c.status, "[?]   "), c.name, c.detail))
            if c.hint and c.status in ("missing", "warn"):
                print("         -> %s" % c.hint)
    print("\nReady for:")
    for k, v in caps.items():
        print("  %s %s" % ("[yes]" if v else "[no] ", k))
    if core_missing:
        print("\nMissing core requirements: %s" % ", ".join(core_missing))
    print("\nThe real PSP is not checked here: ask the user for the model, firmware and memory stick "
          "(docs/AGENT_ONBOARDING.md).")
    return 1 if core_missing else 0


if __name__ == "__main__":
    sys.exit(main())
