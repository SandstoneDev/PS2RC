"""Finding and running the PSP C toolchain (psp-gcc, binutils, pspsdk) without the player installing anything.

Order: (1) --toolchain DIR / PS2RC_TOOLCHAIN, (2) the bundle's own `toolchain/` folder (a native Windows build made
by tools/toolchain/build_windows.sh), (3) PSPDEV / psp-gcc on PATH, (4) WSL: a distribution with ~/pspdev,
(5) Docker: a local pspdev/pspdev image (never pulled unless allowed).

Commands are lists; file arguments are given as HostPath so each runner can translate them (a Windows drive path ->
/mnt/<drive>/... in WSL, a mounted folder in Docker). Many commands run as one batch: a thread pool for native tools, one wsl.exe /
docker call with `xargs -P` otherwise (no start-up cost per unit).
"""
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from .steps import ConvertError

EXE = ".exe" if os.name == "nt" else ""
# psp-g++ (in-game menu) must resolve to the selected toolchain too - an unlisted name is found by
# CreateProcess through the CONVERTER's own PATH (here: an old MinPSPW psp-g++ without cc1plus -> "CreateProcess:
# No such file"; on a player's PC: none, the version probe fails and the EBOOT silently has no menu)
TOOLS = ("psp-gcc", "psp-g++", "psp-as", "psp-ld", "psp-strip", "psp-fixup-imports", "psp-prxgen", "psp-size", "psp-objcopy")
_NOWIN = 0x08000000 if os.name == "nt" else 0
# compilers run below normal priority (Windows) / nice 10 (WSL, Docker): the PC stays usable and other work on a
# shared machine keeps its share
_BELOW = 0x00004000 if os.name == "nt" else 0


def _hidden(flags=0):
    """no console window (CREATE_NO_WINDOW + SW_HIDE) on Windows"""
    if os.name != "nt":
        return {}
    from ps2rc import proc
    return proc.hidden(flags)


class HostPath(str):
    """A path on this PC that a runner must translate."""


def _run(cmd, timeout=60, **kw):
    if os.name == "nt":
        from ps2rc import proc
        for k, v in proc.hidden().items():
            kw.setdefault(k, v)
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                          timeout=timeout, **kw)


class Job:
    __slots__ = ("argv", "label", "log", "rc", "output", "seconds")

    def __init__(self, argv, label, log=None):
        self.argv, self.label, self.log = argv, label, log
        self.rc, self.output, self.seconds = None, "", 0.0


# peak memory of one psp-gcc: cc1's peak working set on the biggest generated unit of the first pack (2.5 MB of C, -O2) is 445 MB (measured
# 2026-10-13, native gcc 15.2); 512 per job leaves room for the assembler. Twelve at once in a 7 GB WSL VM shared with
# other jobs thrashed (2026-10-06: the VM stopped answering), so the job count is capped by the memory the compilers'
# machine has free.
MB_PER_JOB = 512


def _avail_mb_native():
    if os.name == "nt":
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return int(m.ullAvailPhys // (1 << 20))
        return None
    try:
        for ln in open("/proc/meminfo"):
            if ln.startswith("MemAvailable:"):
                return int(ln.split()[1]) // 1024
    except OSError:
        pass
    return None


def cap_jobs(requested, avail_mb):
    """Parallel compiles that fit the free memory (at least 1); None = unknown memory: as requested."""
    if not avail_mb:
        return max(1, requested)
    return max(1, min(requested, avail_mb // MB_PER_JOB))


class Toolchain:
    kind = "?"
    description = ""
    version = None                    # psp-gcc -dumpversion
    avail_mb = None                   # free memory where the compilers run (MB), when known

    def jobs(self, requested):
        return cap_jobs(requested, self.avail_mb() if callable(self.avail_mb) else self.avail_mb)

    def tool_path(self, p):           # HostPath -> the path the tools see
        return str(p)

    def argv(self, argv):
        return [self.tool_path(a) if isinstance(a, HostPath) else a for a in argv]

    def pspdev(self):                 # tool-space root holding psp/ (newlib) and psp/sdk (pspsdk)
        raise NotImplementedError

    def run_batch(self, jobs, cwd, nproc, progress=None, name="batch"):
        raise NotImplementedError

    def run_one(self, argv, cwd, label="tool"):
        j = Job(argv, label)
        self.run_batch([j], cwd, 1)
        return j


# --- native -----------------------------------------------------------------------------------------------------

class NativeToolchain(Toolchain):
    kind = "native"

    def __init__(self, root, how):
        self.root = os.path.abspath(root)              # holds bin/psp-gcc and psp/
        self.bin = os.path.join(self.root, "bin")
        self.description = "%s (%s)" % (self.root, how)
        self.env = dict(os.environ)
        self.env["PATH"] = self.bin + os.pathsep + self.env.get("PATH", "")
        self.env["PSPDEV"] = self.root
        self.version = self._version()
        self.avail_mb = _avail_mb_native

    def _version(self):
        try:
            r = _run([os.path.join(self.bin, "psp-gcc" + EXE), "-dumpversion"], env=self.env)
            return r.stdout.decode(errors="replace").strip() if r.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    def pspdev(self):
        return self.root

    def _exe(self, argv):
        a = list(argv)
        if a and a[0] in TOOLS:
            a[0] = os.path.join(self.bin, a[0] + EXE)
        return a

    def run_batch(self, jobs, cwd, nproc, progress=None, name="batch"):
        done = [0]
        lock = threading.Lock()

        def one(j):
            if progress is not None and progress.cancelled:
                j.rc, j.output = -1, "cancelled"
                return j
            t0 = time.time()
            try:
                r = subprocess.run(self._exe(self.argv(j.argv)), cwd=cwd, env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **_hidden(_BELOW))
                j.rc, j.output = r.returncode, r.stdout.decode("utf-8", "replace")
            except OSError as e:
                j.rc, j.output = -1, str(e)
            j.seconds = time.time() - t0
            with lock:
                done[0] += 1
                if progress is not None:
                    progress.pct(name, done[0], len(jobs), "units")
            return j
        with ThreadPoolExecutor(max_workers=max(1, nproc)) as ex:
            list(ex.map(one, jobs))
        return jobs


# --- WSL / Docker: one call per batch ---------------------------------------------------------------------------

def _win_to_wsl(p):
    p = os.path.abspath(str(p))
    m = re.match(r"^([A-Za-z]):[\\/]?(.*)$", p)
    if not m:
        raise ConvertError("toolchain", "%s: not a drive path WSL can open" % p)
    return "/mnt/%s/%s" % (m.group(1).lower(), m.group(2).replace("\\", "/"))


class _ScriptBatch(Toolchain):
    """Runs a batch as one shell script inside WSL or a container; per job a .rc and a .log file."""

    def _shell(self, script_tool_path):
        raise NotImplementedError

    def run_batch(self, jobs, cwd, nproc, progress=None, name="batch"):
        bdir = os.path.join(cwd, "_batch", name)
        if os.path.isdir(bdir):
            shutil.rmtree(bdir)
        os.makedirs(bdir)
        tcwd = self.tool_path(HostPath(cwd))
        tb = self.tool_path(HostPath(bdir))
        lines = []
        for i, j in enumerate(jobs):
            a = self.argv(j.argv)
            if a and a[0] in TOOLS:
                a[0] = self.pspdev() + "/bin/" + a[0]
            cmd = " ".join(shlex.quote(x) for x in a)
            lines.append("cd %s && t0=$(date +%%s%%N); %s > %s/%d.log 2>&1; rc=$?; t1=$(date +%%s%%N); "
                         "echo \"$rc $(( (t1 - t0) / 1000000 ))\" > %s/%d.rc"
                         % (shlex.quote(tcwd), cmd, tb, i, tb, i))
        with open(os.path.join(bdir, "jobs.txt"), "w", encoding="utf-8", newline="\n") as f:
            for ln in lines:                              # xargs -d '\n': taken literally (no quote processing)
                f.write(ln + "\n")
        script = os.path.join(bdir, "run.sh")
        with open(script, "w", encoding="utf-8", newline="\n") as f:
            f.write("#!/bin/bash\nexport PATH=%s/bin:$PATH\nexport PSPDEV=%s\n" % (self.pspdev(), self.pspdev()))
            f.write("nice -n 10 xargs -P %d -d '\\n' -I CMD bash -c CMD < %s/jobs.txt\n" % (max(1, nproc), tb))
        p = subprocess.Popen(self._shell(self.tool_path(HostPath(script))), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **_hidden())
        out = []
        reader = threading.Thread(target=lambda: out.append(p.stdout.read()), daemon=True)
        reader.start()
        while p.poll() is None:
            time.sleep(0.5)
            if progress is not None:
                n = sum(1 for fn in os.listdir(bdir) if fn.endswith(".rc"))
                progress.pct(name, n, len(jobs), "units")
                if progress.cancelled:
                    p.kill()
        reader.join(5)
        for i, j in enumerate(jobs):
            rcf, logf = os.path.join(bdir, "%d.rc" % i), os.path.join(bdir, "%d.log" % i)
            if os.path.exists(rcf):
                parts = open(rcf).read().split()
                j.rc = int(parts[0])
                j.seconds = int(parts[1]) / 1000.0 if len(parts) > 1 else 0.0
            else:
                j.rc = -1
            j.output = open(logf, encoding="utf-8", errors="replace").read() if os.path.exists(logf) else \
                (out[0].decode("utf-8", "replace") if out and out[0] else "")
        if progress is not None:
            progress.pct(name, len(jobs), len(jobs), "units")
        return jobs


class WslToolchain(_ScriptBatch):
    kind = "wsl"

    def __init__(self, distro, pspdev_dir, version, avail_mb=None):
        self.distro, self._pspdev, self.version = distro, pspdev_dir, version
        self.description = "WSL %s: %s" % (distro, pspdev_dir)
        self._avail = avail_mb

    def avail_mb(self):
        try:
            r = _run(["wsl.exe", "-d", self.distro, "-e", "bash", "-c",
                      "awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo"], timeout=30)
            return int(r.stdout.decode().strip()) if r.returncode == 0 else self._avail
        except (OSError, ValueError, subprocess.SubprocessError):
            return self._avail

    def tool_path(self, p):
        return _win_to_wsl(p)

    def pspdev(self):
        return self._pspdev

    def _shell(self, script):
        return ["wsl.exe", "-d", self.distro, "-e", "bash", script]


class DockerToolchain(_ScriptBatch):
    kind = "docker"

    def __init__(self, image, mount_root, version):
        self.image, self.mount_root, self.version = image, os.path.abspath(mount_root), version
        self.description = "Docker image %s (folder %s mounted as /w)" % (image, mount_root)

    def tool_path(self, p):
        p = os.path.abspath(str(p))
        rel = os.path.relpath(p, self.mount_root)
        if rel.startswith(".."):
            raise ConvertError("toolchain", "%s is outside the folder given to Docker (%s)" % (p, self.mount_root))
        return "/w/" + rel.replace("\\", "/") if rel != "." else "/w"

    def pspdev(self):
        return "/usr/local/pspdev"

    def _shell(self, script):
        return ["docker", "run", "--rm", "-v", "%s:%s" % (self.mount_root, "/w"), self.image, "bash", script]


# --- detection ---------------------------------------------------------------------------------------------------

def _native_at(root, how):
    if root and os.path.isfile(os.path.join(root, "bin", "psp-gcc" + EXE)):
        t = NativeToolchain(root, how)
        return t if t.version else None
    return None


def _wsl_distros():
    if os.name != "nt" or not shutil.which("wsl.exe"):
        return []
    try:
        r = _run(["wsl.exe", "-l", "-q"], timeout=20)
    except (OSError, subprocess.SubprocessError):
        return []
    raw = r.stdout
    text = raw.decode("utf-16-le", "ignore") if raw[1:2] == b"\x00" else raw.decode("utf-8", "ignore")
    return [d.strip().strip("\x00") for d in text.splitlines() if d.strip().strip("\x00")
            and not d.strip().lower().startswith("docker-desktop")]


def _wsl_find(distro):
    probe = ('for d in "$PSPDEV" "$HOME/pspdev" /usr/local/pspdev; do '
             'if [ -n "$d" ] && [ -x "$d/bin/psp-gcc" ]; then echo "$d"; "$d/bin/psp-gcc" -dumpversion; exit 0; fi; '
             'done; exit 1')
    try:
        r = _run(["wsl.exe", "-d", distro, "-e", "bash", "-lc", probe], timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    lines = r.stdout.decode("utf-8", "replace").split()
    if r.returncode == 0 and len(lines) >= 2:
        return WslToolchain(distro, lines[-2], lines[-1])
    return None


def _docker_find(mount_root, allow_pull=False, image="pspdev/pspdev:latest"):
    if not shutil.which("docker"):
        return None
    try:
        r = _run(["docker", "image", "inspect", image], timeout=30)
        if r.returncode != 0:
            if not allow_pull:
                return None
            _run(["docker", "pull", image], timeout=3600)
        r = _run(["docker", "run", "--rm", image, "psp-gcc", "-dumpversion"], timeout=300)
        if r.returncode == 0:
            return DockerToolchain(image, mount_root, r.stdout.decode().strip())
    except (OSError, subprocess.SubprocessError):
        return None
    return None


def bundled_dir():
    """<bundle>/toolchain: the folder next to the app's python/ and the repository tree."""
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (os.path.join(here, "..", "..", "toolchain"), os.path.join(here, "..", "..", "..", "toolchain")):
        if os.path.isdir(up):
            return os.path.abspath(up)
    return None


MIN_MAJOR = 9                     # older psp-gcc (e.g. 4.x community builds) cannot compile the runtime (gnu11)


def _major(v):
    try:
        return int(str(v).split(".")[0])
    except (TypeError, ValueError):
        return 0


def detect(explicit=None, prefer=None, mount_root=None, allow_docker_pull=False, log=None, want=None):
    """-> (Toolchain, tried list). prefer: None/'auto', 'native', 'wsl', 'docker'. want: the pack's psp-gcc
    version; a toolchain of another major version is used only when nothing matching is found."""
    say = log or (lambda s: None)
    tried = []
    fallback = None
    order = ["explicit", "bundled", "path", "wsl", "docker"]
    if prefer in ("native", "wsl", "docker"):
        order = {"native": ["explicit", "bundled", "path"], "wsl": ["wsl"], "docker": ["docker"]}[prefer]
    for how in order:
        t = None
        if how == "explicit":
            d = explicit or os.environ.get("PS2RC_TOOLCHAIN")
            if not d:
                continue
            t = _native_at(d, "--toolchain")
            tried.append("--toolchain %s: %s" % (d, "found" if t else "no working bin/psp-gcc there"))
            if explicit and not t:
                raise ConvertError("no_toolchain", "--toolchain %s: no working bin/psp-gcc%s" % (d, EXE))
            if t and _major(t.version) >= MIN_MAJOR:          # the player's explicit choice wins over the pin
                if want and _major(t.version) != _major(want):
                    tried.append("  psp-gcc %s, the pack wants %s - used because it was asked for" % (t.version, want))
                say("toolchain: %s, psp-gcc %s" % (t.description, t.version))
                return t, tried
        elif how == "bundled":
            d = bundled_dir()
            t = _native_at(d, "bundled") if d else None
            tried.append("bundled toolchain folder: %s" % (d if t else "none"))
        elif how == "path":
            d = os.environ.get("PSPDEV")
            t = _native_at(d, "PSPDEV") if d else None
            if not t:
                g = shutil.which("psp-gcc")
                if g:
                    t = _native_at(os.path.dirname(os.path.dirname(os.path.abspath(g))), "PATH")
            tried.append("PSPDEV / PATH: %s" % ("found" if t else "none"))
        elif how == "wsl":
            for dist in _wsl_distros():
                t = _wsl_find(dist)
                tried.append("WSL %s: %s" % (dist, "pspdev found" if t else "no pspdev"))
                if t:
                    break
            if not tried or not tried[-1].startswith("WSL"):
                tried.append("WSL: not available")
        elif how == "docker":
            t = _docker_find(mount_root or os.getcwd(), allow_docker_pull)
            tried.append("Docker: %s" % ("pspdev/pspdev image" if t else "no docker or no local pspdev/pspdev image"))
        if t and _major(t.version) < MIN_MAJOR:
            tried.append("  %s: psp-gcc %s is too old (need %d or newer) - ignored" % (t.description, t.version,
                                                                                      MIN_MAJOR))
            t = None
        if t and want and _major(t.version) != _major(want):
            tried.append("  %s: psp-gcc %s, the pack wants %s - kept as a fallback" % (t.description, t.version, want))
            fallback = fallback or t
            t = None
        if t:
            say("toolchain: %s, psp-gcc %s" % (t.description, t.version))
            return t, tried
    if fallback:
        say("toolchain: %s, psp-gcc %s (not the pack's %s)" % (fallback.description, fallback.version, want))
        return fallback, tried
    raise ConvertError("no_toolchain", "no PSP toolchain found",
                       "use the converter release that includes the toolchain folder; or install WSL with pspdev "
                       "(pspdev.github.io); tried: " + "; ".join(tried))
