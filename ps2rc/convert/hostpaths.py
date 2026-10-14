"""Host paths and helper programs of a conversion: the PSP toolchain's path rules and where ffmpeg / ffprobe are.

The PSP toolchain on Windows reads part of its paths in the ANSI code page (measured 2026-10-14, a clean Windows
copy of the converter app): psp-gcc / cc1 understand any path, but psp-as / ld / collect2 and the LTO plugin do not -
a converter folder with Cyrillic letters -> "cannot find crt0.o", with an accented letter -> "cannot find 'ld'"; a
TEMP folder with Cyrillic -> LTO "could not open symbol resolution file"; a work folder with letters outside this
Windows' code page -> the assembler cannot read its .s file. So:
  * the converter (and its toolchain) folder must be plain ASCII - refused with a plain message otherwise;
  * TEMP / TMP are moved to an ASCII folder for the conversion when they are not ASCII;
  * the work folder must fit the code page; the default one moves to an ASCII place when it does not.
ffmpeg: --ffmpeg, FFMPEG, the converter app's tools/ffmpeg/ (next to app/), the tree's tools/bin/ffmpeg/, PATH.
ffprobe: optional (the movie converter reads the same facts from ffmpeg -i); next to ffmpeg, FFPROBE, PATH.
"""
import os
import shutil

EXE = ".exe" if os.name == "nt" else ""


def is_ascii(path):
    try:
        os.path.abspath(path).encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def ansi_safe(path):
    """Every character of the path exists in this Windows' ANSI code page (always True elsewhere)."""
    if os.name != "nt":
        return True
    try:
        os.path.abspath(path).encode("mbcs", "strict")
        return True
    except (UnicodeEncodeError, LookupError):
        return False


def tool_dirs(repo, name):
    """the converter app's tools/<name>/ (PS2RC_Converter/tools next to PS2RC_Converter/app), the tree's tools/bin/"""
    return [os.path.join(os.path.dirname(os.path.abspath(repo)), "tools", name),
            os.path.join(os.path.abspath(repo), "tools", "bin", name)]


def find_ffmpeg(repo, given=None, env=None):
    env = os.environ if env is None else env
    for c in (given, env.get("FFMPEG")):
        if c:
            return c if os.path.isfile(c) else None
    for d in tool_dirs(repo, "ffmpeg"):
        p = os.path.join(d, "ffmpeg" + EXE)
        if os.path.isfile(p):
            return p
    return shutil.which("ffmpeg")


def find_ffprobe(ffmpeg, env=None):
    env = os.environ if env is None else env
    if ffmpeg:
        p = os.path.join(os.path.dirname(ffmpeg), "ffprobe" + os.path.splitext(ffmpeg)[1])
        if os.path.isfile(p):
            return p
    c = env.get("FFPROBE")
    if c and os.path.isfile(c):
        return c
    return shutil.which("ffprobe")


def tools_line(repo, given=None):
    """one plain line about ffmpeg / ffprobe for plans and estimates"""
    ff = find_ffmpeg(repo, given)
    if not ff:
        return ("ffmpeg: not found - intro movies would be skipped (put ffmpeg in the converter's tools/ffmpeg/ "
                "folder, on PATH, or give --ffmpeg)")
    fp = find_ffprobe(ff)
    return "ffmpeg: %s; ffprobe: %s" % (ff, fp or "not found (not needed: ffmpeg gives the same facts)")


def safe_temp(work, env=None):
    """TEMP / TMP for the toolchain: kept when ASCII, else <work>/tmp when that is ASCII, else
    %ProgramData%/PS2RC/tmp. Changes `env` (default os.environ); -> the new folder or None (unchanged)."""
    env = os.environ if env is None else env
    cur = [env.get(k) for k in ("TEMP", "TMP") if env.get(k)]
    if all(is_ascii(c) for c in cur):
        return None
    t = os.path.join(work, "tmp")
    if not is_ascii(t):
        base = env.get("ProgramData") or env.get("PUBLIC") or (env.get("SystemDrive") or "C:") + "\\ProgramData"
        t = os.path.join(base, "PS2RC", "tmp")
    os.makedirs(t, exist_ok=True)
    env["TEMP"] = env["TMP"] = t
    return t


def default_work(out, env=None):
    """the work folder when none is given (the window's too): %LOCALAPPDATA%/PS2RC/work - never inside the output
    (which may be the memory stick) - when its path fits the code page, else %ProgramData%/PS2RC/work/<n> (an ASCII
    stand-in name), else <out>/_ps2rc_work (no Windows user folders: other systems)"""
    env = os.environ if env is None else env
    for base in (env.get("LOCALAPPDATA"),):
        if base:
            c = os.path.join(base, "PS2RC", "work")
            if ansi_safe(c):
                return c
    w = os.path.join(os.path.abspath(out), "_ps2rc_work")
    if not env.get("LOCALAPPDATA") and not env.get("ProgramData") and ansi_safe(w):
        return w
    name = os.path.basename(os.path.abspath(out)) or "out"
    if not is_ascii(name):
        import hashlib
        name = "out_" + hashlib.sha1(os.path.abspath(out).encode("utf-8")).hexdigest()[:10]
    for base in (env.get("ProgramData"), env.get("PUBLIC")):
        if base:
            c = os.path.join(base, "PS2RC", "work", name)
            if ansi_safe(c):
                return c
    return w
