"""Which DLLs does every program and module of the converter folder need, and are they there on a clean Windows?

    python gui/bundle_deps.py PS2RC_Converter [--json OUT]

Reads the import tables (normal and delay-load) of every .exe / .dll / .pyd in the folder (no Windows tool needed)
and resolves each DLL name the way Windows does for that file: the folder the file is in, the folders its loader adds
(python: the interpreter folder, DLLs/, the Qt folder, numpy-style *.libs; the toolchain: its bin/; a program in
tools/<name>/: only its own folder), then the system.

A system DLL counts as "on every Windows 10 / 11" only when it is an API set (api-ms-win-*, ext-ms-win-*) or a
Microsoft file of System32 (version resource) that is not one of the redistributables (DirectX 9 extras, XInput 1.x,
old XAudio, the C++ runtimes). The Microsoft Visual C++ runtime
(msvcp140*, vcruntime140*, concrt140 ...) is not part of a clean Windows: it must be next to the program. Media
Foundation DLLs are part of Windows except the N / KN editions without the Media Feature Pack.
"""
import ctypes
import json
import os
import re
import struct
import sys

# Microsoft files that live in System32 only after a redistributable was installed (not part of a clean Windows)
MS_REDIST = re.compile(r"^(d3dx\w*_\d+|d3dcompiler_(3\d|4[0-6])|xinput1_[123]|xaudio2_[0-7]|x3daudio1_\d|"
                       r"xactengine\d_\d|atl\d+|msvcr\d+|msvcp\d+|vcomp\d+)\.dll$", re.I)
VC_RUNTIME = re.compile(r"^(msvcp\d+|vcruntime\d+|concrt\d+|vccorlib\d+|vcomp\d+|vcamp\d+|mfc\d+|msvcr\d+|"
                        r"ucrtbased)(_\w+)?\.dll$", re.I)
MEDIA_FOUNDATION = {"mfplat.dll", "mfreadwrite.dll", "mf.dll", "mfcore.dll", "mfplay.dll", "msmpeg2vdec.dll",
                    "msmpeg2adec.dll", "wmvcore.dll", "wmasf.dll", "evr.dll"}
SINCE = {"icuuc.dll": "Windows 10 1703", "icuin.dll": "Windows 10 1703", "icu.dll": "Windows 10 1903"}
PE_EXT = (".exe", ".dll", ".pyd")


def pe_imports(path):
    """-> (imports, delay imports): DLL names in lower case; ([], []) for a file that is not a PE image"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ" or len(data) < 0x40:
        return [], []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return [], []
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    if magic == 0x20B:
        image_base, = struct.unpack_from("<Q", data, opt + 24)
        dd = opt + 112
    else:
        image_base, = struct.unpack_from("<I", data, opt + 28)
        dd = opt + 96
    secs = []
    for k in range(nsec):
        s = opt + opt_size + 40 * k
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        secs.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return raw + rva - va
        return None

    def cstr(rva):
        o = off(rva)
        if o is None:
            return None
        e = data.find(b"\0", o)
        return data[o:e].decode("ascii", "replace").lower()

    def table(index, entry, name_at, is_delay):
        rva, size = struct.unpack_from("<II", data, dd + 8 * index)
        out = []
        o = off(rva) if rva else None
        while o is not None and o + entry <= len(data):
            raw_entry = data[o:o + entry]
            if not raw_entry.strip(b"\0"):
                break
            name_rva, = struct.unpack_from("<I", data, o + name_at)
            if is_delay and not (struct.unpack_from("<I", data, o)[0] & 1) and name_rva >= image_base:
                name_rva -= image_base           # old-style delay descriptor: virtual addresses
            n = cstr(name_rva)
            if n:
                out.append(n)
            o += entry
        return out
    return table(1, 20, 12, False), table(13, 32, 4, True)


def product_name(path, key="ProductName"):
    """one string of a file's version resource (ProductName, CompanyName ...; '' when there is none or not on
    Windows)"""
    if os.name != "nt":
        return ""
    ver = ctypes.windll.version
    n = ver.GetFileVersionInfoSizeW(path, None)
    if not n:
        return ""
    buf = ctypes.create_string_buffer(n)
    if not ver.GetFileVersionInfoW(path, 0, n, buf):
        return ""
    p, ln = ctypes.c_void_p(), ctypes.c_uint()
    langs = []
    if ver.VerQueryValueW(buf, "\\VarFileInfo\\Translation", ctypes.byref(p), ctypes.byref(ln)) and ln.value >= 4:
        raw = ctypes.string_at(p.value, ln.value)
        langs = ["%04x%04x" % struct.unpack_from("<HH", raw, 4 * i) for i in range(ln.value // 4)]
    for lang in langs + ["040904b0", "040904e4", "000004b0"]:
        if ver.VerQueryValueW(buf, "\\StringFileInfo\\%s\\%s" % (lang, key), ctypes.byref(p), ctypes.byref(ln)) \
                and ln.value:
            return ctypes.wstring_at(p.value, ln.value).rstrip("\0")
    return ""


def system_dir():
    return os.path.join(os.environ.get("SystemRoot") or os.environ.get("SystemDrive", "C:") + "\\Windows",
                        "System32")


def classify_system(name, sysdir=None):
    """-> 'apiset' | 'os' | 'media' | 'vcruntime' | 'other' (in System32 but not a Windows file) | 'missing'"""
    n = name.lower()
    if n.startswith(("api-ms-win-", "ext-ms-win-")):
        return "apiset"
    if VC_RUNTIME.match(n):
        return "vcruntime"
    p = os.path.join(sysdir or system_dir(), n)
    if not os.path.isfile(p):
        return "media" if n in MEDIA_FOUNDATION else "missing"
    if n in MEDIA_FOUNDATION:
        return "media"
    if MS_REDIST.match(n):
        return "other"
    if n in SINCE:              # ICU: part of Windows (Microsoft Learn, "ICU in Windows"), version info says ICU
        return "os"
    return "os" if "Microsoft" in product_name(p, "CompanyName") else "other"


def search_dirs(root, path):
    """the folders Windows (and the loader of that file) searches for the DLLs of `path` inside the converter folder"""
    rel = os.path.relpath(path, root).replace("\\", "/")
    top = rel.split("/")[0]
    here = os.path.dirname(path)
    dirs = [here]
    if top == "python":
        py = os.path.join(root, "python")
        site = os.path.join(py, "Lib", "site-packages")
        dirs += [py, os.path.join(py, "DLLs"), os.path.join(site, "PySide6"), os.path.join(site, "shiboken6")]
        if os.path.isdir(site):
            dirs += [os.path.join(site, d) for d in os.listdir(site) if d.endswith(".libs")]
    elif top == "toolchain":
        dirs.append(os.path.join(root, "toolchain", "bin"))
    return dirs


def check(root, sysdir=None):
    """-> {'files': n, 'problems': [(file, dll, kind)], 'notes': [(file, dll, kind)], 'system': {dll: kind}}
    problems: the DLL is neither in the folder nor part of every Windows (vcruntime / other / missing);
    notes: Windows parts with a condition (Media Foundation on N editions, ICU since 1703)."""
    cache, problems, notes, n = {}, [], [], 0
    for dp, _ds, fs in os.walk(root):
        for fn in fs:
            if not fn.lower().endswith(PE_EXT):
                continue
            path = os.path.join(dp, fn)
            try:
                imp, delay = pe_imports(path)
            except (OSError, struct.error):
                continue
            n += 1
            rel = os.path.relpath(path, root).replace("\\", "/")
            dirs = search_dirs(root, path)
            for dll, is_delay in [(d, False) for d in imp] + [(d, True) for d in delay]:
                if any(os.path.isfile(os.path.join(d, dll)) for d in dirs):
                    continue
                kind = cache.get(dll)
                if kind is None:
                    kind = cache[dll] = classify_system(dll, sysdir)
                if kind in ("apiset", "os"):
                    if dll in SINCE:
                        notes.append((rel, dll, "since " + SINCE[dll]))
                    continue
                if kind == "media":
                    notes.append((rel, dll, "Media Foundation%s: not on Windows N / KN without the Media Feature "
                                            "Pack" % (" (delay-loaded)" if is_delay else "")))
                    continue
                problems.append((rel, dll, kind + (" (delay-loaded)" if is_delay else "")))
    return {"files": n, "problems": problems, "notes": notes, "system": cache}


def vc_runtime_fix(root, vc_dir, folders):
    """copy the Visual C++ runtime DLLs a program in one of `folders` imports and does not find next to it from
    vc_dir (the Qt for Python wheel ships the redistributable runtime) -> [(folder, dll)] copied"""
    import shutil
    done = []
    for folder in folders:
        again = True
        while again:                    # the runtime DLLs import each other (msvcp140 -> vcruntime140_1)
            again = False
            for fn in sorted(os.listdir(folder)):
                if not fn.lower().endswith(PE_EXT):
                    continue
                imp, delay = pe_imports(os.path.join(folder, fn))
                for dll in imp + delay:
                    if VC_RUNTIME.match(dll) and not os.path.isfile(os.path.join(folder, dll)):
                        src = next((os.path.join(vc_dir, f) for f in os.listdir(vc_dir) if f.lower() == dll), None)
                        if src:
                            shutil.copy2(src, os.path.join(folder, os.path.basename(src)))
                            done.append((os.path.relpath(folder, root), os.path.basename(src)))
                            again = True
    return done


def main(argv=None):
    a = list(sys.argv[1:] if argv is None else argv)
    if not a:
        print(__doc__)
        return 2
    r = check(a[0])
    print("%d program / module files checked" % r["files"])
    for f, d, k in r["notes"]:
        print("note     %-60s %-22s %s" % (f, d, k))
    for f, d, k in r["problems"]:
        print("MISSING  %-60s %-22s %s" % (f, d, k))
    if "--json" in a:
        with open(a[a.index("--json") + 1], "w") as fh:
            json.dump(r, fh, indent=1)
    print("%d problem(s)" % len(r["problems"]))
    return 1 if r["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
