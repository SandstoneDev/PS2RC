"""Start the converter window the robust way (the app's launcher and the start scripts call this).

    pythonw gui/run_gui.py

* the window runs as a child process of this Python, with Qt's own plugin folders set from the installed PySide6
  (QT_PLUGIN_PATH, QT_QPA_PLATFORM_PLUGIN_PATH) and no Qt settings inherited from elsewhere (QT_QPA_PLATFORM ...);
* its error output goes to %LOCALAPPDATA%\\PS2RC\\gui_start.log;
* when it stops at once with an error (e.g. "no Qt platform plugin could be initialized"), it is started once more
  with QT_DEBUG_PLUGINS=1 (the plugin search goes into the log) and a message box names the log and what to do.
"""
import importlib.util
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GUI = os.path.join(HERE, "ps2rc_gui.py")
QUICK = 25.0          # an exit within this many seconds with an error code = the window could not start
CLEAN = ("QT_QPA_PLATFORM", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_FONTDIR", "QT_DEBUG_PLUGINS",
         "QT_SCALE_FACTOR", "QT_STYLE_OVERRIDE")


def log_path():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    d = os.path.join(base, "PS2RC")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, "gui_start.log")


def message(text, title="PS2RC Converter"):
    if os.name == "nt":
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, text, title, 0x10)
    else:
        sys.stderr.write(text + "\n")


def qt_env(debug=False):
    """-> (environment for the window, PySide6 folder or None). No Qt plugin paths in the environment: the window
    sets them inside its own process (QCoreApplication.addLibraryPath), so no program it starts inherits our Qt
    (a PCSX2 started from the window once failed on QT_PLUGIN_PATH pointing here)"""
    env = {k: v for k, v in os.environ.items() if k.upper() not in CLEAN}
    spec = importlib.util.find_spec("PySide6")
    if spec is None or not spec.origin:
        return env, None
    pdir = os.path.dirname(spec.origin)
    env["QT_FORCE_STDERR_LOGGING"] = "1"                         # Qt's messages into the log, not a message box
    #                                                              (the window removes it again after starting)
    if debug:
        env["QT_DEBUG_PLUGINS"] = "1"
    return env, pdir


def run(args, debug, log):
    env, pdir = qt_env(debug)
    t0 = time.time()
    with open(log, "a" if debug else "w", encoding="utf-8", errors="replace") as f:
        f.write("=== %s  python %s  PySide6 %s%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), sys.executable, pdir,
                                                     "  (QT_DEBUG_PLUGINS=1)" if debug else ""))
        f.flush()
        p = subprocess.run([sys.executable, "-X", "utf8", GUI] + args,      # UTF-8 log whatever the code page
                           env=env, stdout=f, stderr=subprocess.STDOUT,
                           cwd=os.path.dirname(HERE))
    return p.returncode, time.time() - t0


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    log = log_path()
    if importlib.util.find_spec("PySide6") is None:
        message("The converter window needs Qt for Python (PySide6), which this Python does not have:\n%s\n\n"
                "Start the converter app (PS2RC Converter.exe), or install the pinned PySide6 into this Python "
                "(gui/build_requirements_qt.txt)." % sys.executable)
        return 2
    code, secs = run(args, False, log)
    if code == 0:
        return code
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            first = f.read()
    except OSError:
        first = ""
    if secs > QUICK and "platform plugin" not in first:
        return code                                       # it ran a while: not a start failure
    code, _secs = run(args, True, log)
    if code != 0:
        tail = ""
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                lines = [ln.rstrip() for ln in f if ln.strip()]
            tail = "\n".join(lines[-8:])
        except OSError:
            pass
        why = ""
        if "DLL load failed" in tail or (code & 4294967295) in (3221225781, 3221225785, 3221225595):  # DLL not found /
            # entry point not found / wrong image format
            why = ("A part of Windows the window needs is missing or too old. The converter needs 64-bit "
                   "Windows 10 version 1809 or newer, or Windows 11 (see Requirements in README.txt of the "
                   "converter folder).\n\n")
        message("The converter window could not start (exit code %s).\n\n%sLast lines of %s -\n%s\n\n"
                "Please send this log file. Keep the converter folder together (python, app) and do not move "
                "files out of python\\Lib\\site-packages\\PySide6." % (code, why, log, tail))
    return code


if __name__ == "__main__":
    sys.exit(main())
