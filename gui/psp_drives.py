"""Find a PSP (or its memory card in a reader) connected to this PC, and the converted game folders on it.

A drive counts as a PSP when it has a PSP/GAME folder (a PSP in USB mode shows its memory stick as a removable
drive). Only reads: nothing here writes to a drive.
"""
import json
import os
import shutil
import string

MANIFEST = "ps2rc_manifest.json"


def _windows_drives():
    import ctypes
    k32 = ctypes.windll.kernel32
    mask = k32.GetLogicalDrives()
    out = []
    for i, letter in enumerate(string.ascii_uppercase):
        if not mask & (1 << i):
            continue
        root = letter + ":" + os.sep
        kind = k32.GetDriveTypeW(ctypes.c_wchar_p(root))
        if kind not in (2, 3):                       # 2 removable, 3 fixed (some card readers report fixed)
            continue
        if letter in "AB":
            continue
        label = ctypes.create_unicode_buffer(261)
        ok = k32.GetVolumeInformationW(ctypes.c_wchar_p(root), label, 261, None, None, None, None, 0)
        out.append((root, label.value if ok else "", kind == 2))
    return out


def _posix_drives():
    out = []
    for base in ("/media", "/run/media", "/Volumes", "/mnt"):
        if not os.path.isdir(base):
            continue
        for a in sorted(os.listdir(base)):
            p = os.path.join(base, a)
            subs = [p] + ([os.path.join(p, b) for b in sorted(os.listdir(p))] if base in ("/media", "/run/media")
                          and os.path.isdir(p) else [])
            for s in subs:
                if os.path.isdir(s):
                    out.append((s, os.path.basename(s), True))
    return out


def drives(roots=None):
    """-> [{'root', 'label', 'removable', 'is_psp', 'game_dir', 'free', 'total'}] - PSP drives first.
    roots: [(root, label, removable)] for tests; default = this PC's drives."""
    try:
        raw = roots if roots is not None else (_windows_drives() if os.name == "nt" else _posix_drives())
    except Exception:          # no ctypes / odd platform: nothing detected, the player browses by hand
        raw = []
    out = []
    for root, label, removable in raw:
        game = os.path.join(root, "PSP", "GAME")
        is_psp = os.path.isdir(game)
        if not is_psp and not removable:
            continue
        try:
            du = shutil.disk_usage(root)
            free, total = du.free, du.total
        except OSError:
            continue                                   # empty card reader slot
        out.append({"root": root, "label": label, "removable": removable, "is_psp": is_psp, "game_dir": game,
                    "free": free, "total": total})
    out.sort(key=lambda d: (not d["is_psp"], d["root"]))
    return out


def read_manifest(folder):
    try:
        with open(os.path.join(folder, MANIFEST), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def manifest_version(m):
    if not isinstance(m, dict):
        return None
    for k in ("converter_version", "converter", "version"):
        v = m.get(k)
        if isinstance(v, str):
            return v
        if isinstance(v, dict) and isinstance(v.get("version"), str):
            return v["version"]
    return None


def folder_title(folder):
    """the menu title from EBOOT.PBP's PARAM.SFO - only the header and the SFO are read (a PSP over USB is slow)"""
    try:
        import struct
        from ps2rc.pbp import Sfo
        with open(os.path.join(folder, "EBOOT.PBP"), "rb") as f:
            head = f.read(40)
            if head[:4] != b"\0PBP":
                return None
            sfo_off, icon_off = struct.unpack_from("<II", head, 8)
            f.seek(sfo_off)
            sfo = f.read(max(0, min(icon_off - sfo_off, 64 * 1024)))
        return Sfo.parse(sfo).get("TITLE")
    except Exception:          # damaged / foreign EBOOT: shown without a title
        return None


def is_ps2rc_folder(folder):
    if os.path.isfile(os.path.join(folder, MANIFEST)):
        return True
    return os.path.isfile(os.path.join(folder, "EBOOT.PBP")) and any(
        os.path.isfile(os.path.join(folder, n)) for n in ("ps2rc.cfg", "iso_meta.bin"))


def game_folders(drive_list):
    """converted folders on the given drives -> [{'path', 'drive', 'title', 'version', 'manifest'}]"""
    out = []
    for d in drive_list:
        g = d["game_dir"]
        if not os.path.isdir(g):
            continue
        for name in sorted(os.listdir(g)):
            p = os.path.join(g, name)
            if os.path.isdir(p) and is_ps2rc_folder(p):
                m = read_manifest(p)
                out.append({"path": p, "drive": d["root"], "name": name, "title": folder_title(p),
                            "version": manifest_version(m), "manifest": m is not None})
    return out


def version_tuple(v):
    try:
        return tuple(int(x) for x in str(v).split("+")[0].split("-")[0].split("."))
    except ValueError:
        return None


def update_available(folder_version, app_version):
    a, b = version_tuple(folder_version), version_tuple(app_version)
    return bool(a and b and a < b)
