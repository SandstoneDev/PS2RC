"""Steps, progress events, resumability and errors of a conversion.

A step records `work/steps/<name>.json` when it finishes: its input fingerprint and its outputs (size of each file,
file count + bytes of each folder). On the next run a step whose fingerprint is unchanged and whose outputs are all
still there is skipped ("cached"); anything else runs again (and every later step whose fingerprint depends on its
outputs). `--restart` ignores the records.
"""
import hashlib
import json
import os
import subprocess
import sys
import threading
import time


class ConvertError(Exception):
    """A failure the user can act on: code (short id), message (one line), hint (what to do), log (path)."""

    def __init__(self, code, message, hint="", log=None, tail=None):
        super().__init__(message)
        self.code, self.message, self.hint, self.log, self.tail = code, message, hint, log, tail or []

    def lines(self):
        out = ["ERROR (%s): %s" % (self.code, self.message)]
        if self.hint:
            out.append("  what to do: %s" % self.hint)
        if self.log:
            out.append("  log: %s" % self.log)
        if self.tail:
            out.append("  last lines:")
            out += ["    " + t for t in self.tail]
        return out


class Progress:
    """Events: ('step', index, total, name, title) ('done', name, seconds, cached) ('pct', name, done, total, unit)
    ('log', text) ('warn', text) ('info', text). Listeners are called in the worker thread."""

    def __init__(self, logfile=None):
        self.listeners = []
        self.logfile = logfile
        self._lock = threading.Lock()
        self._fh = None
        self.cancelled = False

    def open_log(self, path):
        self.logfile = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self._fh = open(path, "a", encoding="utf-8", errors="replace")
        self._fh.write("\n===== %s =====\n" % time.strftime("%Y-%m-%d %H:%M:%S"))

    def emit(self, *ev):
        with self._lock:
            if self._fh and ev[0] in ("log", "warn", "info", "step", "done"):
                text = ev[1] if ev[0] in ("log", "warn", "info") else " ".join(str(x) for x in ev[1:])
                self._fh.write("%s %s: %s\n" % (time.strftime("%H:%M:%S"), ev[0], text))
                self._fh.flush()
            for fn in list(self.listeners):
                fn(ev)

    def log(self, text):
        self.emit("log", text)

    def info(self, text):
        self.emit("info", text)

    def warn(self, text):
        self.emit("warn", text)

    def pct(self, name, done, total, unit=""):
        self.emit("pct", name, done, total, unit)

    def check_cancel(self):
        if self.cancelled:
            raise ConvertError("cancelled", "stopped by the user", "run again: finished steps are kept")

    def close(self):
        if self._fh:
            self._fh.close()
            self._fh = None


# --- fingerprints --------------------------------------------------------------------------------------------------

def sha_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def tree_hash(root, exts=None, skip_dirs=("__pycache__", ".git", ".pytest_cache")):
    """SHA-256 over (relative path, content) of the files under root (or of one file)."""
    h = hashlib.sha256()
    if os.path.isfile(root):
        h.update(open(root, "rb").read())
        return h.hexdigest()
    if not os.path.isdir(root):
        return "missing"
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in skip_dirs)
        for fn in sorted(files):
            if exts and not fn.endswith(exts):
                continue
            p = os.path.join(dirpath, fn)
            h.update(os.path.relpath(p, root).replace("\\", "/").encode())
            h.update(b"\0")
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def fingerprint(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(json.dumps(p, sort_keys=True, default=str).encode())
        h.update(b"\x1e")
    return h.hexdigest()


def outputs_state(paths):
    st = {}
    for p in paths:
        if os.path.isdir(p):
            n, b = 0, 0
            for dp, _d, fs in os.walk(p):
                for fn in fs:
                    n += 1
                    b += os.path.getsize(os.path.join(dp, fn))
            st[p] = ["dir", n, b]
        elif os.path.isfile(p):
            st[p] = ["file", os.path.getsize(p)]
        else:
            st[p] = None
    return st


class StepRecord:
    def __init__(self, work):
        self.dir = os.path.join(work, "steps")
        os.makedirs(self.dir, exist_ok=True)

    def path(self, name):
        return os.path.join(self.dir, name + ".json")

    def valid(self, name, fp):
        p = self.path(name)
        if not os.path.exists(p):
            return False
        try:
            rec = json.load(open(p, encoding="utf-8"))
        except ValueError:
            return False
        if rec.get("fingerprint") != fp:
            return False
        saved = rec.get("outputs", {})
        now = outputs_state(list(saved))
        return all(saved[k] is not None and now.get(k) == saved[k] for k in saved)

    def save(self, name, fp, outputs, seconds, extra=None):
        rec = {"fingerprint": fp, "outputs": outputs_state(outputs), "seconds": round(seconds, 1),
               "time": time.strftime("%Y-%m-%d %H:%M:%S")}
        if extra:
            rec.update(extra)
        tmp = self.path(name) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1)
        os.replace(tmp, self.path(name))

    def forget(self, name):
        try:
            os.remove(self.path(name))
        except OSError:
            pass

    def seconds(self, name):
        try:
            return json.load(open(self.path(name), encoding="utf-8")).get("seconds")
        except (OSError, ValueError):
            return None


# --- running Python scripts of the checkout ------------------------------------------------------------------------

def run_logged(cmd, cwd, env, log_path, progress, name, timeout=None, tail_lines=25):
    """Run a command, append its output to log_path, forward lines as 'log' events, raise ConvertError on failure."""
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    tail = []
    with open(log_path, "a", encoding="utf-8", errors="replace") as lf:
        lf.write("\n$ %s\n" % " ".join(cmd))
        lf.flush()
        from ps2rc import proc
        kw = proc.hidden()                               # no console window (the GUI has none to share)
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, **kw)
        t0 = time.time()
        for raw in iter(p.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip()
            lf.write(line + "\n")
            tail.append(line)
            if len(tail) > tail_lines:
                tail.pop(0)
            progress.emit("log", "  " + line)
            if progress.cancelled:
                p.kill()
                break
            if timeout and time.time() - t0 > timeout:
                p.kill()
                break
        rc = p.wait()
    progress.check_cancel()
    if rc != 0:
        prog = [c for c in cmd[1:2] + cmd[2:3] if c != RUNSCRIPT and not c.startswith("-")][:1] or cmd[:1]
        raise ConvertError("step_failed", "%s: %s ended with code %d" % (name, os.path.basename(prog[0]), rc),
                           "see the log; a failure here is a bug of the converter or the pack - please report it",
                           log=log_path, tail=tail)
    return tail


def python_exe():
    return sys.executable


# runs a script like `python SCRIPT` with the script's folder on sys.path - also under the app folder's isolated
# Python (its ._pth file leaves the script folder off sys.path); called by path, so it needs no importable ps2rc
RUNSCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runscript.py")


def script_cmd(script, *args):
    return [python_exe(), RUNSCRIPT, script] + list(args)
