"""Disk and time estimates before a conversion starts, and the free-space check.

Sizes: the output folder is computed exactly (the pack's staging recipe applied to the disc's file list) plus the
EBOOT / boot program / metadata; the work folder from a measured full run. Times: measured on a 12-thread PC
(2026-10) and scaled by the job count; slower PCs: Unknown, shown as a range.
"""
import os
import shutil

from .steps import ConvertError

WORK_BYTES = 0.45e9         # measured: 319 MB work folder after a full run (+ margin)
EBOOT_BYTES = 14e6
MOVIES_BYTES = 21e6         # measured: two intro movies (PMF + WAV) 19.66 MiB
# seconds on the reference PC
T_ANALYSIS = 200             # measured 192 s
T_TRANSLATE = 330           # measured 320 s
T_EXTRAS = 30               # measured 21 s
T_PREPARE = 30              # measured 16 s
COMPILE_CPU_SECONDS = 800   # sum over all units, psp-gcc 15.2 (b39: 722 CPU-s for the generated units at -j6)
T_LINK = 35                 # measured 35 s
COPY_BYTES_PER_S = 60e6     # copy out of the image on the same hard disk (measured 10-60 MB/s under load)
VERIFY_BYTES_PER_S = 100e6  # md5 manifest
T_MOVIES = 40


def _fmt_t(s):
    return "%d min" % max(1, round(s / 60)) if s >= 60 else "%d s" % s


def estimate(conv):
    o = conv.o
    audio = o.audio if o.audio != "none" else None
    stage_bytes = conv.stager.plan_bytes(audio, conv.ident.boot)
    boot_size = conv.ident.elf_size
    meta = 0
    try:
        meta = min(f.lsn for f in conv.disc.files) * 2048
    except ValueError:
        pass
    movies_ok, _why = conv.movies_possible()
    movies = MOVIES_BYTES if movies_ok and o.movies != "skip" else 0
    out_bytes = stage_bytes + EBOOT_BYTES + boot_size + meta + movies
    jobs = max(1, int(o.jobs))
    eff = min(jobs, max(1, (os.cpu_count() or jobs)))
    t_compile = COMPILE_CPU_SECONDS / max(1.0, eff * 0.75) + T_LINK
    t_copy = stage_bytes / COPY_BYTES_PER_S
    steps = {"analysis": T_ANALYSIS, "translate": 0 if o.translation else T_TRANSLATE,
             "extras": T_EXTRAS, "prepare": T_PREPARE, "compile": t_compile, "stage": t_copy,
             "movies": T_MOVIES if movies else 0, "verify": out_bytes / VERIFY_BYTES_PER_S if o.manifest else 5}
    total = sum(steps.values())
    text = ("estimate: output folder %.2f GB (disc files %.2f GB, audio %s, movies %s), work folder about %.1f GB; "
            "time about %s on a 12-thread PC (compile with %d jobs about %s; slower PCs: up to 3x, Unknown)"
            % (out_bytes / 1e9, stage_bytes / 1e9, o.audio, "yes" if movies else "no", WORK_BYTES / 1e9,
               _fmt_t(total), jobs, _fmt_t(t_compile)))
    return {"out_bytes": int(out_bytes), "stage_bytes": int(stage_bytes), "work_bytes": int(WORK_BYTES),
            "seconds": int(total), "steps": {k: int(v) for k, v in steps.items()}, "text": text}


def _existing(path):
    p = os.path.abspath(path)
    while not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    return p


def _present_bytes(path):
    n = 0
    if os.path.isdir(path):
        for dp, _d, fs in os.walk(path):
            for f in fs:
                try:
                    n += os.path.getsize(os.path.join(dp, f))
                except OSError:
                    pass
    return n


def check_disk(est, out, work):
    """Raise when the free space of the drive(s) is below what is still to be written (+5 %)."""
    need = {}
    for path, want in ((out, est["out_bytes"]), (work, est["work_bytes"])):
        root = os.path.splitdrive(os.path.abspath(path))[0] or "/"
        left = max(0, want - _present_bytes(path))
        need.setdefault(root, [0, _existing(path)])
        need[root][0] += left
    for root, (left, probe) in need.items():
        free = shutil.disk_usage(probe).free
        if free < left * 1.05:
            raise ConvertError("disk", "not enough free space on %s: %.2f GB needed, %.2f GB free"
                               % (root, left / 1e9, free / 1e9),
                               "free some space or choose an output folder on another drive")
