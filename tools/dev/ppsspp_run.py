"""Shared PPSSPP runner: one PPSSPP at a time (file lock), boolean checks only. Windows; the PPSSPP folder (with a
memstick/ folder next to PPSSPPWindows64.exe) comes from local.toml [tools] ppsspp_dir or PS2RC_PPSSPP_DIR.

    python tools/dev/ppsspp_run.py STAGE_DIR --wait-file NAME [--pattern REGEX] [--timeout S] [--app NAME]

Copies STAGE_DIR (an EBOOT.PBP plus its data files) to the PPSSPP memory stick as PSP/GAME/<app> (default: the stage
dir's name), starts PPSSPPWindows64.exe on it, waits until <app>/NAME exists and (with --pattern) contains the
regex, prints that file, then closes PPSSPP. The lock file <PPSSPP folder>\\ppsspp_run.lock serialises all
callers (waits up to 2 h), so parallel jobs never kill each other's PPSSPP. Exit code 0 = file appeared (and
matched), 2 = timeout.
PPSSPP answers "does it run / pass", never numbers - timings and memory come from the PSP hardware.
"""
import msvcrt
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from ps2rc import config  # noqa: E402

PPSSPP_DIR = os.environ.get("PS2RC_PPSSPP_DIR") or config.tool("ppsspp_dir") or os.path.expanduser("~/PPSSPP")
EXE = os.path.join(PPSSPP_DIR, "PPSSPPWindows64.exe")
GAME = os.path.join(PPSSPP_DIR, "memstick", "PSP", "GAME")
LOCK = os.path.join(PPSSPP_DIR, "ppsspp_run.lock")


# Never kill by image name - other programs run their own PPSSPP copies (other folders, own locks).
# Only the process this runner started (by PID) or a leftover started from EXE (this exact path) is closed.
def kill_ppsspp(proc=None):
    if proc is not None and proc.poll() is None:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            pass
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='PPSSPPWindows64.exe'\" | Where-Object { $_.ExecutablePath "
          "-and $_.ExecutablePath -ieq '%s' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % os.path.normpath(EXE))
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True)


# A stage may hold GBs of read-only disc data (disc/, movies/, the boot ELF, the ISO metadata); copying it takes
# minutes per run. Those files are hard-linked into the memstick (same drive); everything else (EBOOT, the run
# options, the prx, logs, memory-card files - anything the EBOOT may write) is still copied, so a write by the
# emulator can never reach the stage. Falls back to a copy when a link is impossible.
READ_ONLY_TOP = ("disc", "movies", "iso_meta.bin", "discmap.txt")


def _read_only(src, rel):
    """a read-only top-level entry: the names above, or the boot ELF (an ELF file at the top that is not a PRX)"""
    if rel.split("/", 1)[0] in READ_ONLY_TOP:
        return True
    if "/" in rel or rel.lower().endswith(".prx"):
        return False
    try:
        with open(src, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def stage_copy(stage, dst):
    def copy_one(src, d):
        rel = os.path.relpath(src, stage).replace("\\", "/")
        if _read_only(src, rel):
            try:
                os.link(src, d)
                return d
            except OSError:
                pass
        return shutil.copy2(src, d)
    shutil.copytree(stage, dst, copy_function=copy_one)


def main():
    args = sys.argv[1:]
    stage = os.path.abspath(args[0])
    want = args[args.index("--wait-file") + 1]
    pattern = args[args.index("--pattern") + 1] if "--pattern" in args else None
    timeout = float(args[args.index("--timeout") + 1]) if "--timeout" in args else 900.0
    app = args[args.index("--app") + 1] if "--app" in args else os.path.basename(stage.rstrip("\\/"))
    fd = os.open(LOCK, os.O_RDWR | os.O_CREAT)
    deadline = time.time() + 7200
    while True:
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            break
        except OSError:
            if time.time() > deadline:
                print("lock timeout")
                return 3
            time.sleep(2)
    proc = None
    try:
        kill_ppsspp()
        dst = os.path.join(GAME, app)
        shutil.rmtree(dst, ignore_errors=True)
        stage_copy(stage, dst)
        target = os.path.join(dst, want)
        proc = subprocess.Popen([EXE, os.path.join(dst, "EBOOT.PBP")], cwd=PPSSPP_DIR,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        ok = False
        while time.time() - t0 < timeout:
            time.sleep(2)
            if os.path.exists(target):
                text = open(target, encoding="utf-8", errors="replace").read()
                if pattern is None or re.search(pattern, text):
                    time.sleep(1)
                    ok = True
                    break
            if proc.poll() is not None:
                break
        if os.path.exists(target):
            print(open(target, encoding="utf-8", errors="replace").read())
        print("ppsspp_run: %s after %.0f s" % ("OK" if ok else "TIMEOUT/EXIT", time.time() - t0))
        return 0 if ok else 2
    finally:
        kill_ppsspp(proc)
        os.lseek(fd, 0, os.SEEK_SET)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
