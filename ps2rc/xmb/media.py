"""External programs for clips and music, and probing a clip.

Search order for each program: an explicit path, an environment variable (FFMPEG, PS2RC_ATRACDENC), the converter
app's `tools/<name>/` folder (next to `app/`), then PATH. They always run as separate programs (child processes).
"""
import os
import re
import shutil
import subprocess

from ps2rc import proc
from ps2rc.pbp.spec import PbpError

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""
NO_WINDOW = 0x08000000 if os.name == "nt" else 0       # CREATE_NO_WINDOW: no console flashing from the GUI


def tool_dirs(name):
    """the app bundle's tools/<name>/ (PS2RC_Converter/tools next to PS2RC_Converter/app), then the tree's own"""
    return [os.path.join(os.path.dirname(REPO), "tools", name), os.path.join(REPO, "tools", "bin", name)]


def find_tool(name, given=None, env=None):
    """-> path or None"""
    for cand in (given, os.environ.get(env) if env else None):
        if cand:
            return cand if os.path.isfile(cand) else None
    for d in tool_dirs(name):
        p = os.path.join(d, name + EXE)
        if os.path.isfile(p):
            return p
    return shutil.which(name)


def find_ffmpeg(given=None):
    return find_tool("ffmpeg", given, "FFMPEG")


def run(cmd, stdin=None, what="ffmpeg", timeout=600, cwd=None):
    try:
        r = subprocess.run(cmd, input=stdin, capture_output=True, timeout=timeout, cwd=cwd, **proc.hidden())
    except OSError as e:
        raise PbpError("%s could not start (%s)" % (what, e))
    except subprocess.TimeoutExpired:
        raise PbpError("%s took longer than %d s and was stopped" % (what, timeout))
    if r.returncode:
        part = windows_start_failure(r.returncode)
        if part:
            raise PbpError("%s could not start: %s" % (what, part))
        tail = (r.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-4:]
        raise PbpError("%s failed (exit %d): %s" % (what, r.returncode, " | ".join(tail)))
    return r


# Windows status codes of a program that could not be loaded (exit code of the child process)
# (decimal: STATUS_DLL_NOT_FOUND, STATUS_ENTRYPOINT_NOT_FOUND, STATUS_INVALID_IMAGE_FORMAT, STATUS_DLL_INIT_FAILED)
START_FAILURES = {3221225781: "a DLL it needs is missing", 3221225785: "a DLL it needs is too old (entry point not "
                  "found)", 3221225595: "a DLL it needs is for another processor type (32 / 64 bit)",
                  3221225794: "a DLL it needs failed to start"}


def windows_start_failure(code):
    """-> plain words when the exit code says the program never started (a missing Windows part), else None"""
    msg = START_FAILURES.get(code & 0xFFFFFFFF) if os.name == "nt" and code else None
    if msg:
        return ("%s. Keep the converter folder complete (tools/<program> with its DLLs); see Requirements in "
                "README.txt of the converter folder" % msg)
    return None


def ascii_path(path):
    """True when the absolute path is plain ASCII. The PSP toolchain needs it for its own folder and for TEMP: its
    driver passes paths on as UTF-8 while the linker / LTO plugin read them in the ANSI code page (measured
    2026-10-14: a Cyrillic converter folder -> crt0.o not found, a Cyrillic TEMP -> LTO resolution file not found)"""
    try:
        os.path.abspath(path).encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def ansi_safe(path):
    """True when every character of `path` exists in this Windows' ANSI code page: programs that open files through
    the narrow C runtime (the PSP assembler / linker) can only use such paths. Always True elsewhere."""
    if os.name != "nt":
        return True
    try:
        os.path.abspath(path).encode("mbcs", "strict")
        return True
    except UnicodeEncodeError:
        return False


DUR = re.compile(rb"Duration: (\d+):(\d\d):(\d\d(?:\.\d+)?)")
VID = re.compile(rb"Stream #\S+.*?: Video: .*?(\d{2,5})x(\d{2,5})")
AUD = re.compile(rb"Stream #\S+.*?: Audio: .*?(\d{4,6}) Hz")
FPS = re.compile(rb"(\d+(?:\.\d+)?) fps")


def probe(src, ffmpeg=None):
    """-> {'seconds', 'width', 'height', 'video', 'audio', 'rate', 'fps'} (None where unknown). ffmpeg -i (no ffprobe
    needed); without ffmpeg an animated picture is probed with Pillow."""
    ff = find_ffmpeg(ffmpeg)
    if ff:
        r = subprocess.run([ff, "-hide_banner", "-i", src], capture_output=True, **proc.hidden())
        err = r.stderr or b""
        if b"Invalid data found" in err or b"No such file" in err:
            raise PbpError("%s: not a clip or sound ffmpeg can read" % src)
        info = {"seconds": None, "width": None, "height": None, "video": False, "audio": False, "rate": None,
                "fps": None}
        m = DUR.search(err)
        if m:
            info["seconds"] = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
        m = VID.search(err)
        if m:
            info.update(video=True, width=int(m.group(1)), height=int(m.group(2)))
            f = FPS.search(err[m.start():])
            info["fps"] = float(f.group(1)) if f else None
        m = AUD.search(err)
        if m:
            info.update(audio=True, rate=int(m.group(1)))
        return info
    from .picture import pillow
    Image = pillow()
    if Image is None:
        raise PbpError("%s: ffmpeg is needed to read clips and music (or Pillow for an animated GIF)" % src)
    try:
        im = Image.open(src)
        n = getattr(im, "n_frames", 1)
        total = 0
        for i in range(n):
            im.seek(i)
            total += im.info.get("duration", 100) or 100
        return {"seconds": total / 1000.0, "width": im.width, "height": im.height, "video": n > 1, "audio": False,
                "rate": None, "fps": n * 1000.0 / total if total else None}
    except Exception as e:      # Pillow raises unrelated types for unreadable files
        raise PbpError("%s: cannot read it without ffmpeg (%s)" % (src, e))
