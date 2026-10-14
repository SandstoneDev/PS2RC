"""Child processes without console windows and without the converter app's own environment.

The converter window runs under pythonw.exe (no console). A console program it starts (ffmpeg, the compiler, a
helper script's python.exe) would get a console window of its own, and every program it starts inherits the
window's environment. Use:

    subprocess.run(cmd, ..., **proc.hidden())          # no console window (Windows), output still captured
    subprocess.run(cmd, ..., env=proc.clean_env(env))  # no Qt / Python variables of the app for other programs
"""
import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000
# variables of the app's own Python / Qt that must not reach other programs (a Qt program started from here would
# look for its plugins in our Qt; PCSX2 failed exactly so)
APP_VARS_PREFIX = ("QT_", "QML", "QTWEBENGINE", "PYSIDE", "SHIBOKEN")
APP_VARS = {"PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONUSERBASE"}


def hidden(flags=0):
    """keyword arguments for subprocess: no console window on Windows (also hidden if one is made)"""
    if os.name != "nt":
        return {}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0                                  # SW_HIDE
    return {"creationflags": CREATE_NO_WINDOW | flags, "startupinfo": si}


def _own_dirs():
    """folders of this Python (and the Qt inside it): PATH entries pointing there are the app's"""
    out = []
    for p in (sys.prefix, sys.base_prefix, os.path.dirname(sys.executable)):
        if p:
            out.append(os.path.normcase(os.path.abspath(p)))
    return out


def clean_env(env=None):
    """a copy of env (default os.environ) without the app's Qt / Python variables and without PATH entries inside
    the app's own Python folder"""
    src = os.environ if env is None else env
    own = _own_dirs()
    out = {}
    for k, v in src.items():
        ku = k.upper()
        if ku in APP_VARS or ku.startswith(APP_VARS_PREFIX):
            continue
        if ku == "PATH":
            keep = [p for p in v.split(os.pathsep)
                    if p and not any(os.path.normcase(os.path.abspath(p)).startswith(o) for o in own)]
            v = os.pathsep.join(keep)
        out[k] = v
    return out


def scrub_environment():
    """remove the app's Qt / Python variables and PATH entries from this process itself (after Qt has started):
    everything started later - also by Windows from a file dialog or an Explorer window opened here - gets a
    clean environment. -> names removed"""
    clean = clean_env()
    gone = [k for k in list(os.environ) if k not in clean]
    for k in gone:
        del os.environ[k]
    if os.environ.get("PATH", "") != clean.get("PATH", ""):
        os.environ["PATH"] = clean.get("PATH", "")
    return gone


def python_for_children():
    """python.exe instead of pythonw.exe for helper scripts: a console program gets a hidden console (CREATE_NO_
    WINDOW) that the programs it starts share; under pythonw each of those would open its own console window"""
    exe = sys.executable or ""
    if os.name == "nt" and os.path.basename(exe).lower() == "pythonw.exe":
        cand = os.path.join(os.path.dirname(exe), "python.exe")
        if os.path.isfile(cand):
            return cand
    return exe
