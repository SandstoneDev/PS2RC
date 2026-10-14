"""Run a PS2 EE test ELF in a private, portable PCSX2 and read the results through PINE.

Layout (managed_settings() lists every ini key and the reason):

    <datapath>\\                  PCSX2 data root (inis\\PCSX2.ini, logs\\, cache\\, ...)
    <datapath>\\bin\\             private copy of the PCSX2 v2.6.3 program files + portable.txt ("..")
    <datapath>\\bios\\<BIOS>.BIN  NTFS *hard link* to <bios dir>\\<BIOS>.BIN (no copy);
                                 PCSX2 keeps its private <BIOS>.nvm/.mec next to it, not in the user's folder
    (<datapath>, the PCSX2 install and <bios dir>: local.toml [tools] pcsx2_oracle / pcsx2_install / ps2_bios_dir,
    the BIOS file name ps2_bios_name; or the environment variables below)

Why not `pcsx2-qt.exe -datapath <dir>` on the user's install?  v2.6.3 has no -datapath option
(added later; an unknown option pops a modal "Unknown parameter" box) and its -portable mode keeps the
data next to the executable, i.e. inside the install folder that must stay untouched.  A private copy
of the program with portable.txt = ".." makes <datapath> the data root without touching either the
install folder or %USERPROFILE%\\Documents\\PCSX2.

Typical use:

    runner = Pcsx2Runner()                       # defaults: oracle dir above, PINE slot 28911
    res = runner.run_elf(elf, done_addr, 0x600DF00D, timeout_s=60, ee_interpreter=False,
                         reads={"results": (results_addr, size)})
    res["blocks"]["results"]                     # bytes, read after the done flag appeared

or, for interactive poking, `with runner.session(elf) as s: s.wait_for_value(...); s.read_block(...)`.
"""
from __future__ import annotations

import ctypes
import os
import shutil
import socket
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

from .pine import EmuStatus, PineClient, PineCommandError, PineError, ee_ram_alias
from .. import config as _config

__all__ = [
    "Pcsx2Runner", "Pcsx2Session", "Pcsx2Error", "Pcsx2StartupError", "Pcsx2TimeoutError",
    "DEFAULT_DATAPATH", "DEFAULT_SOURCE_INSTALL", "DEFAULT_BIOS_DIR", "DEFAULT_BIOS_NAME", "DEFAULT_SLOT",
    "managed_settings",
]

def _setting(env, key, fallback):
    """The environment variable, else local.toml [tools] <key>, else the fallback (a placeholder that does not exist)."""
    v = os.environ.get(env) or _config.local().get("tools", {}).get(key)
    return v if v else fallback


DEFAULT_DATAPATH = Path(_config.host_path(_setting("PS2RC_PCSX2_ORACLE_DIR", "pcsx2_oracle",
                                                   str(Path.home() / "ps2rc-pcsx2-oracle"))))
DEFAULT_SOURCE_INSTALL = Path(_config.host_path(_setting("PS2RC_PCSX2_INSTALL", "pcsx2_install",
                                                         str(Path.home() / "pcsx2-v2.6.3"))))
DEFAULT_BIOS_DIR = Path(_config.host_path(_setting("PS2RC_PS2_BIOS_DIR", "ps2_bios_dir", str(Path.home() / "ps2-bios"))))
DEFAULT_BIOS_NAME = _setting("PS2RC_PS2_BIOS_NAME", "ps2_bios_name", "BIOS.BIN")   # the user's own BIOS dump
DEFAULT_SLOT = 28911
EXE_NAME = "pcsx2-qt.exe"

# Log line PCSX2 prints at VM start when the EE recompiler is off (WarnAboutUnsafeSettings);
# used to prove that an "interpreter" run really ran on the interpreter.
_EE_INTERP_MARKER = "EE Recompiler is not enabled"
_STARTUP_ERROR_MARKERS = ("ReportErrorAsync:",)


class Pcsx2Error(RuntimeError):
    """Base class; `log_tail` holds the end of PCSX2's log when available."""

    def __init__(self, msg: str, log_tail: str = ""):
        super().__init__(msg + (f"\n--- PCSX2 log (tail) ---\n{log_tail}" if log_tail else ""))
        self.log_tail = log_tail


class Pcsx2StartupError(Pcsx2Error):
    pass


class Pcsx2TimeoutError(Pcsx2Error):
    pass


# --------------------------------------------------------------------------------------------------
# ini handling
# --------------------------------------------------------------------------------------------------
def managed_settings(slot: int = DEFAULT_SLOT, ee_interpreter: bool = False,
                     bios_name: str = DEFAULT_BIOS_NAME) -> list[tuple[str, str, str, str]]:
    """(section, key, value, why) for every key the oracle owns in PCSX2.ini (re-applied before each run)."""
    rec = "false" if ee_interpreter else "true"
    return [
        ("UI", "SetupWizardIncomplete", "false", "skip the first-run setup wizard (modal dialog)"),
        ("UI", "ConfirmShutdown", "false", "closing the window must not pop the 'Confirm Shutdown' box"),
        ("UI", "StartPaused", "false", "execute immediately after boot"),
        ("UI", "PauseOnFocusLoss", "false", "keep running while the window is in the background"),
        ("UI", "PauseOnControllerDisconnection", "false", "never pause because of input devices"),
        ("UI", "StartFullscreen", "false", "windowed; never grab the screen"),
        ("UI", "InhibitScreensaver", "false", "a test run must not touch the desktop session"),
        ("AutoUpdater", "CheckAtStartup", "false", "no update check / update dialog"),
        ("Folders", "Bios", "bios", "<datapath>\\bios holds a hard link to the user's BIOS image, so "
         "PCSX2's <bios>.nvm/.mec side files are written there and not into the user's BIOS folder"),
        ("Filenames", "BIOS", bios_name, "the user's own BIOS dump (local.toml [tools] ps2_bios_name)"),
        ("EmuCore", "EnablePINE", "true", "the oracle reads/writes EE memory through PINE"),
        ("EmuCore", "PINESlot", str(slot), "TCP port 127.0.0.1:<slot>; not the default 28011 so it never "
         "collides with a PCSX2 the user runs"),
        ("EmuCore", "EnableFastBoot", "true", "skip the BIOS logo/OSD, go straight to the ELF"),
        ("EmuCore", "EnableFastBootFastForward", "true", "run the BIOS part of the boot unthrottled"),
        ("EmuCore", "HostFs", "true", "let the BIOS loader open host:<elf> regardless of path spelling "
         "(the host: root is the ELF's directory)"),
        ("EmuCore", "EnableCheats", "false", "no cheats"),
        ("EmuCore", "EnableDiscordPresence", "false", "no Discord IPC"),
        ("EmuCore", "SaveStateOnShutdown", "false", "no resume state written on shutdown"),
        ("EmuCore", "WarnAboutUnsafeSettings", "true", "keeps the 'EE Recompiler is not enabled' log line "
         "that proves an interpreter run"),
        ("EmuCore", "InhibitScreensaver", "false", "see [UI]"),
        ("EmuCore/CPU/Recompiler", "EnableEE", rec, "EE recompiler (true) or EE interpreter (false); "
         "switched per run by the runner"),
        ("EmuCore/CPU/Recompiler", "EnableIOP", "true", "IOP recompiler (only the EE is under test)"),
        ("EmuCore/CPU/Recompiler", "EnableVU0", "true", "microVU0"),
        ("EmuCore/CPU/Recompiler", "EnableVU1", "true", "microVU1"),
        ("EmuCore/CPU/Recompiler", "EnableEECache", "false", "PCSX2 default: no EE cache emulation"),
        ("EmuCore/CPU/Recompiler", "PauseOnTLBMiss", "false", "a guest TLB miss must not pause the VM "
         "behind a modal error box"),
        ("EmuCore/Speedhacks", "vuThread", "false", "no MTVU thread: VU1 runs in lock-step (determinism)"),
        ("EmuCore/GS", "Renderer", "3", "Direct3D 11. PCSX2-Qt always opens a GPU device for its window; "
         "with Null(11)/Software(13) it auto-picks the API, which is Vulkan on this NVIDIA laptop and costs "
         "~1.5 s more per boot (measured). EE tests draw nothing, so this cannot change results"),
        ("EmuCore/GS", "VsyncEnable", "false", "never block on the host display"),
        ("SPU2/Output", "Backend", "Null", "no audio device"),
        ("SPU2/Output", "OutputMuted", "true", "belt and braces"),
        ("MemoryCards", "Slot1_Enable", "false", "no memory-card files; a busy card can also veto shutdown"),
        ("MemoryCards", "Slot2_Enable", "false", "see Slot1"),
        ("Logging", "EnableFileLogging", "true", "log to the -logfile given by the runner"),
        ("Logging", "EnableSystemConsole", "false", "no console window"),
        ("Logging", "EnableTimestamps", "true", "timings in the log"),
        ("Logging", "EnableEEConsole", "true", "EE kernel printf/SIO output lands in the log"),
        ("Logging", "EnableIOPConsole", "true", "IOP printf in the log (boot diagnostics)"),
        ("InputSources", "Keyboard", "false", "keys typed while the window has focus cannot pause/hotkey it"),
        ("InputSources", "Mouse", "false", "see Keyboard"),
        ("InputSources", "SDL", "false", "ignore game controllers (v2.6.3 still loads SDL's controller DB)"),
        ("InputSources", "XInput", "false", "see SDL"),
        ("InputSources", "DInput", "false", "see SDL"),
        ("Achievements", "Enabled", "false", "no RetroAchievements (hardcore mode would disable PINE)"),
    ]


#: ini sections whose content the oracle deletes (all hotkeys unbound)
CLEARED_SECTIONS = ("Hotkeys",)


class IniFile:
    """Tiny order/format preserving editor for PCSX2's `key = value` ini files."""

    def __init__(self, path: Path):
        self.path = Path(path)
        raw = self.path.read_bytes().decode("utf-8-sig") if self.path.exists() else ""
        self.newline = "\r\n" if "\r\n" in raw else "\n"
        self.lines = raw.splitlines()

    @staticmethod
    def _section_name(line: str):
        s = line.strip()
        return s[1:-1].strip() if s.startswith("[") and s.endswith("]") else None

    def _section_range(self, section: str):
        """(header_index, end_index) of a section's body or None."""
        start = None
        for i, line in enumerate(self.lines):
            name = self._section_name(line)
            if name is None:
                continue
            if start is not None:
                return start, i
            if name == section:
                start = i
        return (start, len(self.lines)) if start is not None else None

    @staticmethod
    def _key_of(line: str):
        s = line.strip()
        if not s or s[0] in ";#" or "=" not in s:
            return None
        return s.split("=", 1)[0].strip()

    def get(self, section: str, key: str):
        rng = self._section_range(section)
        if rng is None:
            return None
        for i in range(rng[0] + 1, rng[1]):
            if self._key_of(self.lines[i]) == key:
                return self.lines[i].split("=", 1)[1].strip()
        return None

    def set(self, section: str, key: str, value: str) -> bool:
        """Set a value; returns True when the file content changed."""
        new_line = f"{key} = {value}"
        rng = self._section_range(section)
        if rng is None:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines += [f"[{section}]", new_line, ""]
            return True
        last_entry = rng[0]
        for i in range(rng[0] + 1, rng[1]):
            if self._key_of(self.lines[i]) == key:
                if self.lines[i] == new_line:
                    return False
                self.lines[i] = new_line
                return True
            if self.lines[i].strip():
                last_entry = i
        self.lines.insert(last_entry + 1, new_line)
        return True

    def clear_section(self, section: str) -> bool:
        rng = self._section_range(section)
        if rng is None:
            return False
        body = [i for i in range(rng[0] + 1, rng[1]) if self._key_of(self.lines[i]) is not None]
        for i in reversed(body):
            del self.lines[i]
        return bool(body)

    def save(self) -> None:
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_bytes((self.newline.join(self.lines) + self.newline).encode("utf-8"))
        os.replace(tmp, self.path)


# --------------------------------------------------------------------------------------------------
# Win32 helpers (process tree, windows, job object) - ctypes only, no extra dependencies
# --------------------------------------------------------------------------------------------------
_IS_WIN = sys.platform == "win32"
if _IS_WIN:
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _u32 = ctypes.WinDLL("user32", use_last_error=True)

    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    _k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]
    _k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]

    _WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    _u32.EnumWindows.argtypes = [_WNDENUMPROC, wintypes.LPARAM]
    _u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    _u32.IsWindowVisible.argtypes = [wintypes.HWND]
    _u32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    class _IO_COUNTERS(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount",
                                                       "OtherOperationCount", "ReadTransferCount",
                                                       "WriteTransferCount", "OtherTransferCount")]

    class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", _IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    class _PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    _k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
    _k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]

_WM_CLOSE = 0x0010
_SW = {"normal": 1, "noactivate": 4, "minimized": 7}   # SW_SHOWNORMAL, SW_SHOWNOACTIVATE, SW_SHOWMINNOACTIVE


def _norm(p) -> str:
    return os.path.normcase(os.path.abspath(str(p)))


def _create_kill_on_close_job():
    """Job object that kills its processes when the last handle closes (i.e. also if Python dies)."""
    if not _IS_WIN:
        return None
    job = _k32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not _k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
        _k32.CloseHandle(job)
        return None
    return job


def _process_image(pid: int) -> str | None:
    h = _k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buf))
        if _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value
        return None
    finally:
        _k32.CloseHandle(h)


def find_processes(exe_path) -> list[int]:
    """PIDs of running processes whose image is exactly `exe_path` (our private copy only)."""
    if not _IS_WIN:
        return []
    want, name = _norm(exe_path), os.path.basename(str(exe_path)).lower()
    snap = _k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap in (None, wintypes.HANDLE(-1).value):
        return []
    pids = []
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = _k32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == name:
                image = _process_image(entry.th32ProcessID)
                if image and _norm(image) == want:
                    pids.append(entry.th32ProcessID)
            ok = _k32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        _k32.CloseHandle(snap)
    return pids


def kill_pid(pid: int, wait_s: float = 5.0) -> bool:
    if not _IS_WIN:
        return False
    h = _k32.OpenProcess(0x0001 | 0x00100000, False, pid)  # PROCESS_TERMINATE | SYNCHRONIZE
    if not h:
        return False
    try:
        _k32.TerminateProcess(h, 1)
        return _k32.WaitForSingleObject(h, int(wait_s * 1000)) == 0
    finally:
        _k32.CloseHandle(h)


def _top_windows(pid: int, visible_only: bool) -> list[int]:
    found: list[int] = []

    def cb(hwnd, _lparam):
        owner = wintypes.DWORD()
        _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and (not visible_only or _u32.IsWindowVisible(hwnd)):
            found.append(hwnd)
        return True

    _u32.EnumWindows(_WNDENUMPROC(cb), 0)
    return found


def _post_close(pid: int, visible_only: bool) -> int:
    if not _IS_WIN:
        return 0
    hwnds = _top_windows(pid, visible_only)
    for hwnd in hwnds:
        _u32.PostMessageW(hwnd, _WM_CLOSE, 0, 0)
    return len(hwnds)


# --------------------------------------------------------------------------------------------------
# session / runner
# --------------------------------------------------------------------------------------------------
def _tail(path: Path | None, nbytes: int = 6000) -> str:
    if not path:
        return ""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


class Pcsx2Session:
    """A running PCSX2 instance with a connected PINE client.  Created by Pcsx2Runner.session()."""

    def __init__(self, runner: "Pcsx2Runner", elf: Path, ee_interpreter: bool, log_path: Path, window: str):
        self.runner = runner
        self.elf = elf
        self.ee_interpreter = ee_interpreter
        self.log_path = log_path
        self.window = window
        self.proc: subprocess.Popen | None = None
        self.pine = PineClient(runner.slot, timeout=5.0)
        self.t0 = 0.0
        self.timings: dict[str, float] = {}
        self.shutdown_mode: str | None = None
        self.pcsx2_version: str | None = None
        self.command: list[str] = []
        self._job = None
        self._stderr = None
        self._last_log_check = 0.0

    # - lifecycle -------------------------------------------------------------------------------
    def start(self, timeout_s: float) -> "Pcsx2Session":
        r = self.runner
        self.command = [str(r.exe), "-batch", "-nogui", "-fastboot", "-logfile", str(self.log_path)]
        if str(self.elf).lower().endswith((".iso", ".chd", ".cso")):
            self.command.append(str(self.elf))          # a disc image boots through the BIOS like a real disc
        else:
            self.command += ["-elf", str(self.elf)]
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.log_path.unlink()
        except FileNotFoundError:
            pass
        si = None
        flags = 0
        if _IS_WIN:
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = _SW[self.window]
            flags = subprocess.CREATE_NEW_PROCESS_GROUP
        self._stderr = open(self.log_path.with_suffix(".stdio.txt"), "wb")
        self._job = _create_kill_on_close_job()
        self.t0 = time.monotonic()
        self.proc = subprocess.Popen(self.command, cwd=str(r.exe.parent), stdin=subprocess.DEVNULL,
                                     stdout=self._stderr, stderr=subprocess.STDOUT, startupinfo=si,
                                     creationflags=flags, close_fds=True)
        if self._job:
            _k32.AssignProcessToJobObject(self._job, int(self.proc._handle))
        deadline = self.t0 + timeout_s
        self._wait_pine(deadline)
        self._wait_running(deadline)
        return self

    def _check_alive(self, what: str) -> None:
        if self.proc.poll() is not None:
            raise Pcsx2StartupError(f"PCSX2 exited (code {self.proc.returncode}) while {what}",
                                    _tail(self.log_path))
        now = time.monotonic()
        if now - self._last_log_check < 0.25:   # scanning the log every poll would be wasteful
            return
        self._last_log_check = now
        log = _tail(self.log_path, 20000)
        for marker in _STARTUP_ERROR_MARKERS:
            if marker in log:
                line = next(ln for ln in log.splitlines() if marker in ln)
                raise Pcsx2StartupError(f"PCSX2 reported an error while {what}: {line.strip()}", _tail(self.log_path))

    def _wait_pine(self, deadline: float) -> None:
        """The PINE server starts with PCSX2's CPU thread, long before the VM exists."""
        while True:
            self._check_alive("waiting for PINE")
            try:
                self.pine.connect(retry_timeout=0.0)
                self.timings["pine_connect"] = time.monotonic() - self.t0
                return
            except PineError:
                pass
            if time.monotonic() > deadline:
                raise Pcsx2TimeoutError(f"PINE slot {self.runner.slot} never came up", _tail(self.log_path))
            time.sleep(0.05)

    def _wait_running(self, deadline: float) -> None:
        """Poll MsgStatus (the only command v2.6.3 answers without a VM) until the VM runs."""
        while True:
            self._check_alive("booting")
            try:
                if self.pine.status() == EmuStatus.RUNNING:
                    self.timings["vm_running"] = time.monotonic() - self.t0
                    try:
                        self.pcsx2_version = self.pine.version()
                    except PineCommandError:
                        pass
                    return
            except PineCommandError:
                pass
            except PineError:
                self._reconnect()
            if time.monotonic() > deadline:
                raise Pcsx2TimeoutError("VM never reached the Running state", _tail(self.log_path))
            time.sleep(0.02)

    def _reconnect(self) -> None:
        try:
            self.pine.connect(retry_timeout=1.0)
        except PineError:
            pass

    def wait_for_value(self, addr: int, value: int, timeout_s: float, poll_s: float = 0.01,
                       size: int = 4) -> float:
        """Poll the `size`-byte word at EE-RAM address `addr` (read through its kseg0 alias, see
        ee_ram_alias) until it equals `value`; returns seconds since launch."""
        deadline = time.monotonic() + timeout_s
        reader = {4: self.pine.read32, 8: self.pine.read64}[size]
        alias = ee_ram_alias(addr, size)
        last = None
        while True:
            try:
                last = reader(alias)
                if last == value:
                    t = time.monotonic() - self.t0
                    self.timings.setdefault("done", t)
                    return t
            except PineCommandError:
                pass                       # VM not valid (yet) - keep polling
            except PineError:
                self._reconnect()
            self._check_alive(f"waiting for 0x{value:X} at 0x{addr:08X}")
            if time.monotonic() > deadline:
                last_s = "unreadable" if last is None else f"0x{last:0{size * 2}X}"
                raise Pcsx2TimeoutError(f"0x{addr:08X} never became 0x{value:X} "
                                        f"({time.monotonic() - self.t0:.1f}s after launch; last value {last_s}; "
                                        f"ELF {self.elf})", _tail(self.log_path))
            time.sleep(poll_s)

    def verify_cpu_mode(self) -> str:
        """Check PCSX2's own log for the EE core actually used; returns 'interpreter' or 'recompiler'."""
        log = _tail(self.log_path, 1 << 22)
        mode = "interpreter" if _EE_INTERP_MARKER in log else "recompiler"
        want = "interpreter" if self.ee_interpreter else "recompiler"
        if mode != want:
            raise Pcsx2Error(f"PCSX2 ran the EE {mode}, but the {want} was requested", _tail(self.log_path))
        return mode

    # - memory (EE main RAM, accessed through kseg0 aliases; use self.pine for raw addresses) ---------
    def read_block(self, addr: int, nbytes: int) -> bytes:
        return self.pine.read_block(ee_ram_alias(addr, nbytes), nbytes)

    def read32(self, addr: int) -> int:
        return self.pine.read32(ee_ram_alias(addr, 4))

    def read64(self, addr: int) -> int:
        return self.pine.read64(ee_ram_alias(addr, 8))

    def write_block(self, addr: int, data: bytes) -> None:
        self.pine.write_block(ee_ram_alias(addr, len(data)), data)

    # - shutdown --------------------------------------------------------------------------------
    def close(self, graceful_timeout: float = 10.0) -> str:
        """Close PINE, ask PCSX2 to exit (WM_CLOSE -> VM shutdown -> batch-mode exit), kill if needed.
        Always leaves no process behind.  Returns 'graceful', 'terminated' or 'not-started'."""
        if self.shutdown_mode is not None:
            return self.shutdown_mode
        mode = "not-started"
        t_close = time.monotonic()
        try:
            self.pine.close()   # client closes first -> no TIME_WAIT on the server's port
            p = self.proc
            if p is not None:
                mode = "graceful"
                if p.poll() is None and graceful_timeout > 0:
                    self._graceful_stop(p, graceful_timeout)
                if p.poll() is None:
                    mode = "terminated"
                    p.kill()
                    try:
                        p.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        kill_pid(p.pid)
        finally:
            if self._job:
                _k32.CloseHandle(self._job)   # KILL_ON_JOB_CLOSE: nothing of this session survives
                self._job = None
            if self._stderr:
                self._stderr.close()
                self._stderr = None
            if self.proc is not None:
                self.timings["shutdown"] = time.monotonic() - t_close
            self.shutdown_mode = mode
        return mode

    @staticmethod
    def _graceful_stop(p: subprocess.Popen, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        sent_all = False
        _post_close(p.pid, visible_only=True)
        while p.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
            # the display window may not exist yet (or only hidden windows): broaden after 2 s
            if not sent_all and time.monotonic() > deadline - timeout + 2.0:
                _post_close(p.pid, visible_only=False)
                sent_all = True

    def __enter__(self) -> "Pcsx2Session":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class Pcsx2Runner:
    """Launch a private, portable PCSX2 on a test ELF and talk to it via PINE.

    `datapath` is the PCSX2 data root (holds inis\\PCSX2.ini); `exe` defaults to <datapath>\\bin\\pcsx2-qt.exe,
    a private copy of `source_install` made by setup().  One PCSX2 per data root at a time (file lock;
    concurrent callers queue for up to `lock_timeout` s).  `window`: how the game window is first shown --
    "minimized" (default, iconic), "noactivate" (normal size, not activated) or "normal".  PCSX2 cannot run
    without that window (-nogui only hides the main window).
    """

    def __init__(self, exe=None, datapath=DEFAULT_DATAPATH, slot: int = DEFAULT_SLOT,
                 source_install=DEFAULT_SOURCE_INSTALL, bios_dir=DEFAULT_BIOS_DIR, bios_name: str = DEFAULT_BIOS_NAME,
                 window: str = "minimized", lock_timeout: float = 600.0):
        self.datapath = Path(datapath)
        self.exe = Path(exe) if exe else self.datapath / "bin" / EXE_NAME
        self.slot = int(slot)
        self.source_install = Path(source_install)
        self.bios_dir = Path(bios_dir)
        self.bios_name = bios_name
        if window not in _SW:
            raise ValueError(f"window must be one of {sorted(_SW)}")
        self.window = window
        self.lock_timeout = lock_timeout

    # - paths -----------------------------------------------------------------------------------
    @property
    def ini_path(self) -> Path:
        return self.datapath / "inis" / "PCSX2.ini"

    @property
    def defaults_ini_path(self) -> Path:
        """Pristine `-testconfig` output of this PCSX2 build; PCSX2.ini is rebuilt from it for every run."""
        return self.datapath / "inis" / "PCSX2.defaults.ini"

    @property
    def bios_link(self) -> Path:
        return self.datapath / "bios" / self.bios_name

    # - setup -----------------------------------------------------------------------------------
    def check_safe(self) -> None:
        """Refuse configurations that would make PCSX2 touch the user's install or Documents\\PCSX2."""
        bindir = self.exe.parent
        if _norm(bindir) == _norm(self.source_install):
            raise Pcsx2Error(f"refusing to run the user's install {bindir}; use the private copy (setup())")
        portable = bindir / "portable.txt"
        if not portable.is_file():
            raise Pcsx2Error(f"{portable} missing: PCSX2 would fall back to Documents\\PCSX2. Run setup().")
        root = (bindir / portable.read_text(encoding="utf-8").strip()).resolve()
        if _norm(root) != _norm(self.datapath.resolve()):
            raise Pcsx2Error(f"{portable} points at {root}, expected the data root {self.datapath}")

    def setup(self) -> None:
        """Idempotently create the data root: program copy, portable.txt, default ini, BIOS hard link.
        Call with the lock held (session() does)."""
        bindir = self.exe.parent
        if not self.exe.is_file():
            if not (self.source_install / EXE_NAME).is_file():
                raise Pcsx2Error(f"PCSX2 not found: neither {self.exe} nor {self.source_install / EXE_NAME}")
            shutil.copytree(self.source_install, bindir, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("portable.txt", "portable.ini", "inis"))
        portable = bindir / "portable.txt"
        rel = os.path.relpath(self.datapath.resolve(), bindir.resolve())
        if not portable.is_file() or portable.read_text(encoding="utf-8").strip() != rel:
            portable.write_text(rel, encoding="utf-8")
        self.check_safe()
        if not self.defaults_ini_path.is_file():
            self._generate_defaults()
        self._ensure_bios_link()

    def _generate_defaults(self) -> None:
        """Let this exact PCSX2 build write its default ini (right SettingsVersion, no reset prompt)."""
        ini = self.ini_path
        backup = ini.with_suffix(".ini.bak")
        if ini.is_file():
            os.replace(ini, backup)  # -testconfig only writes defaults when PCSX2.ini is absent
        subprocess.run([str(self.exe), "-testconfig"], cwd=str(self.exe.parent), timeout=120, check=False,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not ini.is_file():
            if backup.is_file():
                os.replace(backup, ini)
            raise Pcsx2Error(f"PCSX2 -testconfig did not create {ini}")
        shutil.copyfile(ini, self.defaults_ini_path)
        backup.unlink(missing_ok=True)  # PCSX2.ini is rebuilt from the defaults before every run anyway

    def _ensure_bios_link(self) -> None:
        src = self.bios_dir / self.bios_name
        if not src.is_file():
            raise Pcsx2Error(f"BIOS image not found: {src}")
        link = self.bios_link
        link.parent.mkdir(parents=True, exist_ok=True)
        if link.exists():
            if os.path.samefile(link, src):
                return
            link.unlink()  # a stale name in *our* folder; the user's file is untouched
        try:
            os.link(src, link)  # NTFS hard link: same file, no copy; PCSX2 only ever reads it
        except OSError as e:
            raise Pcsx2Error(f"cannot hard-link {src} -> {link} ({e}); the BIOS must not be copied, and pointing "
                             f"PCSX2 at {self.bios_dir} directly would make it write <bios>.mec/.nvm there") from e

    def configure(self, ee_interpreter: bool = False, overrides: dict | None = None) -> Path:
        """Write PCSX2.ini = PCSX2.defaults.ini + oracle-managed keys + per-run `overrides`
        ({(section, key): value}).  Rebuilt before every run, so nothing PCSX2 or an earlier run
        wrote can leak into the next one."""
        ini = IniFile(self.defaults_ini_path)
        ini.path = self.ini_path
        for section, key, value, _why in managed_settings(self.slot, ee_interpreter, self.bios_name):
            ini.set(section, key, value)
        for section in CLEARED_SECTIONS:
            ini.clear_section(section)
        for (section, key), value in (overrides or {}).items():
            if isinstance(value, bool):
                value = "true" if value else "false"
            ini.set(section, key, str(value))
        ini.save()
        return self.ini_path

    # - locking & stale processes ---------------------------------------------------------------
    @contextmanager
    def _lock(self):
        import msvcrt
        self.datapath.mkdir(parents=True, exist_ok=True)
        path = self.datapath / "oracle.lock"
        fd = os.open(path, os.O_RDWR | os.O_CREAT)
        deadline = time.monotonic() + self.lock_timeout
        try:
            while True:
                try:
                    os.lseek(fd, 0, os.SEEK_SET)
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise Pcsx2Error(f"timed out waiting for {path} (another oracle run in progress?)")
                    time.sleep(0.2)
            try:
                yield
            finally:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        finally:
            os.close(fd)

    def kill_stale(self) -> list[int]:
        """Kill leftover instances of *our* private PCSX2 copy (never the user's own PCSX2)."""
        pids = find_processes(self.exe)
        for pid in pids:
            kill_pid(pid)
        return pids

    def _ensure_port_free(self) -> None:
        """PCSX2 binds 127.0.0.1:<slot> plainly; if we can do the same right now, so can it."""
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", self.slot))
        except OSError as e:
            raise Pcsx2Error(f"PINE slot {self.slot} (127.0.0.1:{self.slot}) is in use by another program: {e}") from e
        finally:
            probe.close()

    # - running ---------------------------------------------------------------------------------
    @contextmanager
    def session(self, elf_path, ee_interpreter: bool = False, log_path=None, timeout_s: float = 60.0,
                overrides: dict | None = None, shutdown: str = "graceful"):
        """Context manager: boot `elf_path` and yield a Pcsx2Session with PINE connected and the VM running.
        On exit PCSX2 is shut down - `shutdown="graceful"` (WM_CLOSE, VM shutdown, kill as fallback)
        or `"kill"` (TerminateProcess right away) - and no process of ours is left, whatever happens."""
        if not _IS_WIN:
            raise Pcsx2Error("the PCSX2 oracle runner currently supports Windows only")
        if shutdown not in ("graceful", "kill"):
            raise ValueError("shutdown must be 'graceful' or 'kill'")
        elf = Path(os.path.realpath(elf_path))
        if not elf.is_file():
            raise FileNotFoundError(elf)
        log = Path(log_path) if log_path else elf.with_name(
            f"{elf.stem}.pcsx2-{'int' if ee_interpreter else 'rec'}.log")
        with self._lock():
            self.setup()
            self.kill_stale()
            self._ensure_port_free()
            self.configure(ee_interpreter, overrides)
            sess = Pcsx2Session(self, elf, ee_interpreter, Path(os.path.abspath(log)), self.window)
            body_ok = False
            try:
                sess.start(timeout_s)
                yield sess
                body_ok = True
            finally:
                sess.close(graceful_timeout=0.0 if shutdown == "kill" else 10.0)
                leftovers = self.kill_stale()
                if leftovers and body_ok:  # should never happen; make it loud (but never mask another error)
                    raise Pcsx2Error(f"stray PCSX2 processes had to be killed: {leftovers}")

    def run_elf(self, elf_path, done_addr: int, done_value: int, timeout_s: float = 60.0,
                ee_interpreter: bool = False, reads: dict | None = None, log_path=None,
                overrides: dict | None = None, shutdown: str = "graceful") -> dict:
        """Boot `elf_path`, wait until the u32 at EE-RAM address `done_addr` == `done_value`, read `reads`
        ({name: (addr, nbytes)}, EE-RAM addresses), shut PCSX2 down.  Returns a dict:

            blocks          {name: bytes}
            cpu_mode        'interpreter' | 'recompiler' (verified from PCSX2's log)
            timings         seconds since launch: pine_connect, vm_running, done; shutdown (duration);
                            total (from the call until the process is gone)
            shutdown        'graceful' | 'terminated'
            tlb_misses      number of "TLB Miss" lines in PCSX2's log (should be 0 for a sane test)
            pcsx2_version, log_path, command, elf
        """
        t_start = time.monotonic()
        with self.session(elf_path, ee_interpreter, log_path, timeout_s, overrides, shutdown) as s:
            remaining = max(1.0, timeout_s - (time.monotonic() - s.t0))
            s.wait_for_value(done_addr, done_value, remaining)
            blocks = {name: s.read_block(addr, n) for name, (addr, n) in (reads or {}).items()}
            mode = s.verify_cpu_mode()
        timings = dict(s.timings)
        timings["total"] = time.monotonic() - t_start
        return {
            "blocks": blocks,
            "cpu_mode": mode,
            "timings": timings,
            "shutdown": s.shutdown_mode,
            "tlb_misses": _tail(s.log_path, 1 << 22).count("TLB Miss"),
            "pcsx2_version": s.pcsx2_version,
            "log_path": str(s.log_path),
            "command": s.command,
            "elf": str(s.elf),
        }


def _main(argv=None) -> int:
    import argparse
    import json

    ap = argparse.ArgumentParser(prog="python -m ps2rc.oracle", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup", help="create/refresh the oracle data root and print the managed ini keys")
    run = sub.add_parser("run", help="run an ELF until a done word appears; dump a memory block")
    run.add_argument("elf")
    run.add_argument("--done-addr", type=lambda s: int(s, 0), required=True)
    run.add_argument("--done-value", type=lambda s: int(s, 0), default=0x600DF00D)
    run.add_argument("--read", nargs=2, metavar=("ADDR", "NBYTES"), type=lambda s: int(s, 0))
    run.add_argument("--interp", action="store_true", help="EE interpreter instead of the recompiler")
    run.add_argument("--timeout", type=float, default=60.0)
    run.add_argument("--set", nargs=3, action="append", default=[], metavar=("SECTION", "KEY", "VALUE"),
                     help="extra PCSX2.ini override for this run (repeatable)")
    run.add_argument("--kill", action="store_true", help="TerminateProcess instead of a graceful shutdown")
    run.add_argument("--window", choices=sorted(_SW), default="minimized")
    args = ap.parse_args(argv)
    runner = Pcsx2Runner(window=getattr(args, "window", "minimized"))
    if args.cmd == "setup":
        with runner._lock():
            runner.setup()
            runner.configure(False)
        for section, key, value, why in managed_settings(runner.slot, False, runner.bios_name):
            print(f"[{section}] {key} = {value}    ; {why}")
        return 0
    reads = {"block": tuple(args.read)} if args.read else None
    overrides = {(s, k): v for s, k, v in args.set}
    res = runner.run_elf(args.elf, args.done_addr, args.done_value, args.timeout, args.interp, reads,
                         overrides=overrides, shutdown="kill" if args.kill else "graceful")
    if "block" in res["blocks"]:
        res["blocks"]["block"] = res["blocks"]["block"].hex()
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
