"""Assemble the standalone Windows converter folder (no installation for the player):

    PS2RC_Converter/
      PS2RC Converter.exe     launcher (gui/launcher.c) -> python\\pythonw.exe app\\gui\\run_gui.py
      ps2rc-convert.cmd       the command line: python\\python.exe -m ps2rc.convert ...
      python/                 a private Python (copied from an installed CPython or an embeddable package) with
                              Qt for Python (PySide6, LGPL-3.0: its Qt DLLs stay separate, replaceable files)
      app/                    the PS2RC tree (converter, translator, runtime sources, game packs)
      toolchain/              the native Windows PSP toolchain (tools/toolchain/build_windows.sh)
      tools/ffmpeg/           optional: ffmpeg (+ ffprobe: intro movies) (clips -> animated icon, music -> PCM)
      tools/atracdenc/        optional: atracdenc (menu music -> ATRAC3plus), a separate program (LGPL-2.1), with the
                              Visual C++ runtime DLLs it needs (from the Qt for Python wheel)
      licenses/
      README.txt              what the folder needs (gui/APP_README.txt)

Nothing has to be installed on the player's PC: after copying, every .exe / .dll / .pyd of the folder is checked
(gui/bundle_deps.py) - each DLL it imports must be in the folder or part of every 64-bit Windows 10 (1809+) / 11; the
build stops otherwise (--allow-missing to only report).

    python gui/build_app.py --out DIST [--source TREE] [--toolchain DIR] [--pack NAME ...] [--private]
                            [--python-from DIR] [--with-pillow] [--cc GCC] [--no-audit]
                            [--ffmpeg DIR] [--atracdenc DIR] [--with-numpy] [--allow-missing]

--source   the tree to put into app/ (default: this checkout; for a release: the public export of
           tools/release/export_public.py - then the audit runs on app/ and must report 0 hits)
--pack     game packs to include (default: _template and example). A LOCAL pack needs --private: such a folder is
           for the developer's own PC only and is marked so (PRIVATE_BUILD_DO_NOT_SHARE.txt); no audit.
The window needs PySide6 in the Python that runs this script (the pinned wheels: pip install --require-hashes -r
build_requirements_qt.txt); only QtCore / QtGui / QtWidgets and the plugins they need are copied (QML, Quick,
design programs and tools left out). Pillow is always included (the window's pictures). --with-numpy: numpy for
game packs whose own scripts need it (the disc-artwork readers of our packs do not: 42 MB saved).
--ffmpeg / --atracdenc  a folder holding that program (+ its DLLs) and its licence file(s): copied to tools/<name>/,
           the licence files and a note where the program's source code is offered to licenses/<name>/. Refused
           without a licence file. The window finds them there (ps2rc/xmb/media.py); without them the animated
           icon from clips / the menu music are off and the window says why.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PUBLIC_PACKS = ("_template", "example")
SKIP_DIRS = {".git", "build", "analysis", "__pycache__", ".pytest_cache", ".claude", "tools/release", "docs",
             "tests/convert", "tests/pbp", "tests/gui"}
SKIP_FILES = {"local.toml"}
# parts of the toolchain a conversion never uses (measured on the 15.2 build: 270 -> ~150 MB without C++): the other
# multilibs (soft-float, big endian), the full C++ library (the in-game menu is C++ built with -fno-exceptions -fno-rtti
# and links no libstdc++; psp-g++, cc1plus, the C++ headers and libsupc++ stay), LTO / coverage tools, duplicate driver
# and linker names, the static plugin library. The LTO plugin DLL and lto1 stay: pspdev's libpthread.a is linked
# through them.
TOOLCHAIN_SKIP_DIRS = {"psp/lib/soft-float", "psp/lib/eb", "lib/gcc/psp/15.2.0/soft-float", "lib/gcc/psp/15.2.0/eb",
                       "share"}
TOOLCHAIN_SKIP_FILES = {"bin/psp-lto-dump.exe", "bin/psp-gcov.exe", "bin/psp-gcov-tool.exe", "bin/psp-gcov-dump.exe",
                        "bin/psp-gcc-15.2.0.exe", "bin/psp-c++.exe", "bin/psp-ld.bfd.exe", "psp/bin/ld.bfd.exe",
                        "psp/lib/libstdc++.a", "psp/lib/libstdc++exp.a", "psp/lib/libstdc++.la",
                        "psp/lib/libstdc++exp.la", "psp/lib/libstdc++.modules.json",
                        "psp/lib/libstdc++.a-gdb.py", "libexec/gcc/psp/15.2.0/liblto_plugin.a",
                        "libexec/gcc/psp/15.2.0/liblto_plugin.la"}
# the in-game menu needs these: a toolchain without them makes EBOOTs without the menu (ps2rc/convert/build.py warns)
TOOLCHAIN_NEED_CXX = ("bin/psp-g++.exe", "libexec/gcc/psp/15.2.0/cc1plus.exe", "psp/include/c++/15.2.0/new")
# optional external programs (each runs as a separate process; their licences travel with them)
TOOLS = {
    "ffmpeg": {"exe": ("ffmpeg.exe", "ffmpeg", "ffprobe.exe", "ffprobe"),
               "source": "https://ffmpeg.org/releases/ (the release named in "
               "README.txt next to the program, e.g. ffmpeg-8.0.1.tar.xz) and the libraries its configuration line "
               "lists; a build with libx264 is GPL"},
    "atracdenc": {"exe": ("atracdenc.exe", "atracdenc"), "source": "https://github.com/dcherednik/atracdenc "
                  "(LGPL-2.1; source of the exact release: the release's tar.xz on that page, tag in VERSION.txt)"},
}
LICENCE_NAMES = ("license", "licence", "copying", "lgpl", "gpl", "notice", "readme")
# Qt for Python: what the converter window uses (Qt Widgets); everything else of the wheel is left out
QT_KEEP_TOP = ("__init__.py", "_config.py", "_git_pyside_version.py", "py.typed", "QtCore.pyd", "QtGui.pyd",
               "QtWidgets.pyd", "pyside6.abi3.dll", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Svg.dll",
               "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_codecvt_ids.dll", "concrt140.dll",
               "vcruntime140.dll", "vcruntime140_1.dll")
QT_KEEP_PLUGINS = ("platforms/qwindows.dll", "styles/qmodernwindowsstyle.dll", "imageformats/qgif.dll",
                   "imageformats/qico.dll", "imageformats/qjpeg.dll", "imageformats/qwebp.dll")
QT_SOURCE = ("Qt for Python (PySide6) %(v)s and Qt %(qt)s are used under the GNU Lesser General Public License v3.0 "
             "(LGPL-3.0, text in LGPL-3.0.txt; it builds on GPL-3.0.txt).\n"
             "They are separate, unmodified files in python/Lib/site-packages/PySide6 and shiboken6: you may replace "
             "them with another build of the same version.\n"
             "Source code of exactly these versions:\n"
             "  PySide6 / shiboken6: https://download.qt.io/official_releases/QtForPython/pyside6/PySide6-%(v)s-src/\n"
             "  Qt: https://download.qt.io/official_releases/qt/%(qtmm)s/%(qt)s/single/\n"
             "  repository: https://code.qt.io/cgit/pyside/pyside-setup.git/ (tag v%(v)s)\n"
             "On request we also send these sources on a data carrier (see the project's NOTICE).\n")
LIB_SKIP = {"test", "idlelib", "ensurepip", "lib2to3", "pydoc_data", "turtledemo", "__pycache__", "venv",
            "site-packages", "tkinter/test", "unittest/test"}
LIB_SKIP_FILES = {"turtle.py"}
# interpreter DLLs nobody here uses: Tk (the window is Qt), CPython's own test modules
DLLS_SKIP = ("_tkinter", "tcl", "tk8", "tk9", "_test", "xxlimited", "_ctypes_test", "xxsubtype")


def copytree(src, dst, skip_rel=(), skip_names=(), filt=None):
    n, b = 0, 0
    for dirpath, dirs, files in os.walk(src):
        rel = os.path.relpath(dirpath, src).replace("\\", "/")
        rel = "" if rel == "." else rel
        dirs[:] = [d for d in dirs if d not in skip_names and (rel + "/" + d).lstrip("/") not in skip_rel]
        for fn in files:
            if fn in SKIP_FILES or fn.endswith((".pyc", ".pyo")):
                continue
            p = os.path.join(dirpath, fn)
            if filt and not filt(os.path.join(rel, fn).replace("\\", "/")):
                continue
            q = os.path.join(dst, rel, fn)
            os.makedirs(os.path.dirname(q), exist_ok=True)
            shutil.copy2(p, q)
            n += 1
            b += os.path.getsize(p)
    return n, b


def du(path):
    t = 0
    for dp, _d, fs in os.walk(path):
        for f in fs:
            t += os.path.getsize(os.path.join(dp, f))
    return t


def copy_python(base, dst, with_pillow, with_numpy=False):
    """A private CPython: the interpreter, its DLLs (incl. Tk), the standard library without tests, tcl/."""
    os.makedirs(dst)
    for fn in os.listdir(base):
        p = os.path.join(base, fn)
        if os.path.isfile(p) and (fn.lower().endswith((".exe", ".dll")) or fn.upper().startswith("LICENSE")):
            if fn.lower().startswith(("python", "pythonw", "vcruntime", "license")):
                shutil.copy2(p, os.path.join(dst, fn))
    copytree(os.path.join(base, "DLLs"), os.path.join(dst, "DLLs"),
             filt=lambda rel: not os.path.basename(rel).lower().startswith(DLLS_SKIP))
    copytree(os.path.join(base, "Lib"), os.path.join(dst, "Lib"), skip_rel=LIB_SKIP | {"tkinter", "idlelib"},
             skip_names={"__pycache__"}, filt=lambda rel: rel not in LIB_SKIP_FILES)
    site = os.path.join(dst, "Lib", "site-packages")
    os.makedirs(site, exist_ok=True)

    def package(mod, dist_prefix):
        """A pure-Python package of this interpreter (with its dist-info: version and licence) into site-packages."""
        pdir = os.path.dirname(mod.__file__)
        copytree(pdir, os.path.join(site, os.path.basename(pdir)), skip_names={"__pycache__"})
        for d in os.listdir(os.path.dirname(pdir)):
            if d.lower().startswith(dist_prefix) and d.endswith(".dist-info"):
                copytree(os.path.join(os.path.dirname(pdir), d), os.path.join(site, d))

    import elftools                     # ps2rc.image reads the boot program with it (pyelftools: public domain)
    package(elftools, "pyelftools")
    if with_pillow:
        import PIL
        package(PIL, "pillow")
    if with_numpy:
        import numpy
        package(numpy, "numpy")
        libs = os.path.join(os.path.dirname(os.path.dirname(numpy.__file__)), "numpy.libs")
        if os.path.isdir(libs):
            copytree(libs, os.path.join(site, "numpy.libs"))
    copy_qt(site)
    ver = "python%d%d" % sys.version_info[:2]
    with open(os.path.join(dst, ver + "._pth"), "w", newline="\r\n") as f:
        # isolated: no PYTHONPATH / user site; the app tree is importable
        f.write("Lib\nDLLs\nLib\\site-packages\n..\\app\n")


def copy_qt(site):
    """PySide6 + shiboken6 of this interpreter, pruned to Qt Widgets (sizes measured in the report)"""
    try:
        import PySide6
        import shiboken6
    except ImportError:
        sys.exit("PySide6 is not installed in this Python: pip install --require-hashes -r build_requirements_qt.txt")
    src = os.path.dirname(PySide6.__file__)
    dst = os.path.join(site, "PySide6")
    os.makedirs(dst)
    for fn in QT_KEEP_TOP:
        if os.path.exists(os.path.join(src, fn)):
            shutil.copy2(os.path.join(src, fn), os.path.join(dst, fn))
    for rel in QT_KEEP_PLUGINS:
        p = os.path.join(src, "plugins", rel)
        if os.path.exists(p):
            os.makedirs(os.path.join(dst, "plugins", os.path.dirname(rel)), exist_ok=True)
            shutil.copy2(p, os.path.join(dst, "plugins", rel))
    copytree(os.path.dirname(shiboken6.__file__), os.path.join(site, "shiboken6"), skip_names={"__pycache__",
                                                                                               "include", "lib"})
    parent = os.path.dirname(src)
    for d in os.listdir(parent):
        if d.lower().startswith(("pyside6", "shiboken6")) and d.endswith(".dist-info"):
            copytree(os.path.join(parent, d), os.path.join(site, d))


def package_licences(site, lic):
    """licenses/<package>/: the licence files of the Python packages shipped (Pillow, pyelftools ...), which
    otherwise sit only inside their dist-info folders; Qt for Python has its own (qt_licences)"""
    n = 0
    for d in sorted(os.listdir(site)) if os.path.isdir(site) else []:
        if not d.endswith(".dist-info") or d.lower().startswith(("pyside6", "shiboken6")):
            continue
        name = d[:-len(".dist-info")].rsplit("-", 1)[0]
        top = os.path.join(site, d)
        for dp, _ds, fs in os.walk(top):
            inside = os.path.relpath(dp, top).lower()
            for fn in fs:
                if fn.lower().startswith(LICENCE_NAMES) or "licen" in inside:
                    os.makedirs(os.path.join(lic, name), exist_ok=True)
                    shutil.copy2(os.path.join(dp, fn), os.path.join(lic, name, fn))
                    n += 1
    return n


def qt_licences(lic):
    """licenses/Qt: LGPL-3.0 + GPL-3.0 texts (gui/licenses), the versions and where their source is"""
    import PySide6
    from PySide6 import QtCore
    qt = QtCore.qVersion()
    d = os.path.join(lic, "Qt")
    os.makedirs(d)
    for fn in ("LGPL-3.0.txt", "GPL-3.0.txt"):
        shutil.copy2(os.path.join(HERE, "licenses", fn), os.path.join(d, fn))
    with open(os.path.join(d, "SOURCE.txt"), "w", newline="\r\n") as f:
        f.write(QT_SOURCE % {"v": PySide6.__version__, "qt": qt, "qtmm": ".".join(qt.split(".")[:2])})


def add_tool(name, src, out, lic):
    """copy an optional program folder into tools/<name>/ and its licence texts + source note into licenses/<name>/;
    the Visual C++ runtime DLLs the program imports and does not bring are copied next to it from the Qt for Python
    wheel (they are not part of a clean Windows)"""
    spec = TOOLS[name]
    files = sorted(os.listdir(src)) if os.path.isdir(src) else []
    if not any(f.lower() in spec["exe"] for f in files):
        sys.exit("%s: no %s in %s" % (name, " / ".join(spec["exe"]), src))
    lic_files = [f for f in files if f.lower().startswith(LICENCE_NAMES)]
    if not lic_files:
        sys.exit("%s: no licence file (LICENSE / COPYING ...) in %s - it cannot be shipped without one" % (name, src))
    dst, ldst = os.path.join(out, "tools", name), os.path.join(lic, name)
    os.makedirs(dst)
    os.makedirs(ldst)
    n = 0
    for f in files:
        p = os.path.join(src, f)
        if os.path.isfile(p) and (f.lower() in spec["exe"] or f.lower().endswith((".dll", ".txt", ".md")) or
                                  f in lic_files):
            shutil.copy2(p, os.path.join(dst, f))
            n += 1
    for f in lic_files:
        shutil.copy2(os.path.join(src, f), os.path.join(ldst, f))
    vc = vc_runtime_for(dst)
    with open(os.path.join(ldst, "SOURCE.txt"), "w", newline="\r\n") as fh:
        fh.write("%s is a separate program shipped unchanged in tools/%s. It is not part of PS2RC and keeps its own "
                 "licence (the files next to this one).\nIts source code: %s\nOn request we also send the exact "
                 "source of the shipped version (see the project's NOTICE).\n" % (name, name, spec["source"]))
        if vc:
            fh.write("Next to it: %s - the Microsoft Visual C++ runtime (redistributable files, from the Qt for Python "
                     "wheel), which the program needs and a clean Windows does not have.\n" % ", ".join(vc))
    return n + len(vc)


def vc_runtime_for(folder):
    """the Visual C++ runtime DLLs the programs of `folder` import, copied from PySide6's folder -> [names]"""
    sys.path.insert(0, HERE)
    try:
        import bundle_deps
        import PySide6
    except ImportError:
        return []
    finally:
        sys.path.remove(HERE)
    return [dll for _f, dll in bundle_deps.vc_runtime_fix(folder, os.path.dirname(PySide6.__file__), [folder])]


def make_icon(out_dir, variant=None):
    """ps2rc.ico (gui/app_icon.py, drawn with Qt) into out_dir -> path or None"""
    sys.path.insert(0, HERE)
    try:
        import app_icon
        return app_icon.write_ico(os.path.join(out_dir, "ps2rc.ico"), variant or app_icon.DEFAULT)
    except Exception as e:      # no PySide6 / Pillow here: a launcher without its own icon
        print("icon: not made (%s)" % e)
        return None
    finally:
        sys.path.remove(HERE)


def build_launcher(cc, out_exe, ico=None):
    src = os.path.join(HERE, "launcher.c")
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(cc) + os.pathsep + env.get("PATH", "")       # the compiler's own DLLs (MSYS2)
    extra = []
    windres = os.path.join(os.path.dirname(cc), "windres.exe")
    if ico and os.path.isfile(windres):
        work = os.path.dirname(ico)
        rc = os.path.join(work, "launcher.rc")
        with open(rc, "w") as f:
            f.write('1 ICON "%s"\n' % os.path.basename(ico))          # the exe's icon in Explorer / the taskbar
        res = os.path.join(work, "launcher_res.o")
        r = subprocess.run([windres, rc, "-O", "coff", "-o", res], cwd=work, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        if r.returncode == 0:
            extra = [res]
        else:
            print("launcher: windres failed (%s): no icon" % r.stdout.decode(errors="replace").strip())
    r = subprocess.run([cc, "-O2", "-municode", "-mwindows", "-static", "-s", "-o", out_exe, src] + extra,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    for tmp in extra:
        os.remove(tmp)
    if r.returncode != 0:
        print("launcher: %s failed (code %s): %s" % (cc, r.returncode, r.stdout.decode(errors="replace")))
        return False
    return True


def find_cc(given):
    msys = os.path.join(os.environ.get("SystemDrive", "C:") + os.sep, "msys64")      # MSYS2's default place
    for c in (given, shutil.which("gcc"), os.path.join(msys, "ucrt64", "bin", "gcc.exe"),
              os.path.join(msys, "mingw64", "bin", "gcc.exe")):
        if c and os.path.isfile(c):
            return c
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--source", default=ROOT)
    ap.add_argument("--toolchain")
    ap.add_argument("--prebuilt", help="a prebuilt object store (python -m ps2rc.convert.prebuilt export ...)")
    ap.add_argument("--pack", action="append")
    ap.add_argument("--private", action="store_true")
    ap.add_argument("--python-from", default=sys.base_prefix)
    ap.add_argument("--with-pillow", action="store_true")
    ap.add_argument("--with-numpy", action="store_true")
    ap.add_argument("--cc")
    ap.add_argument("--no-audit", action="store_true")
    ap.add_argument("--ffmpeg", help="folder with ffmpeg(.exe) + its licence file(s)")
    ap.add_argument("--atracdenc", help="folder with atracdenc(.exe) + its licence file(s)")
    ap.add_argument("--allow-missing", action="store_true", help="only report DLLs a clean Windows lacks")
    ap.add_argument("--allow-no-cxx", action="store_true",
                    help="accept a C-only toolchain (EBOOTs built with it have no in-game menu)")
    a = ap.parse_args(argv)
    packs = a.pack or list(PUBLIC_PACKS)
    # from this checkout only _template / example are public; a public export (--source, made by
    # tools/release/export_public.py + ps2rc_profile.py) holds only the public files of its packs, and app/ is
    # audited below
    # a public export has no release tooling (tools/release is never exported), so this script may also run from
    # inside the export itself
    from_export = (os.path.abspath(a.source) != os.path.abspath(ROOT) or
                   not os.path.isdir(os.path.join(a.source, "tools", "release")))
    local = [p for p in packs if p not in PUBLIC_PACKS and not from_export]
    if local and not a.private:
        sys.exit("refused: %s %s LOCAL game pack(s); a shared bundle holds only %s (use --private for a folder that "
                 "never leaves this PC)" % (", ".join(local), "is a" if len(local) == 1 else "are",
                                            " and ".join(PUBLIC_PACKS)))
    out = os.path.abspath(os.path.join(a.out, "PS2RC_Converter"))
    if os.path.exists(out):
        sys.exit("%s exists; remove it first" % out)
    os.makedirs(out)
    sizes = {}
    # app tree: everything but work dirs, docs, release tooling, tests of the tools, other packs
    games = os.path.join(a.source, "games")
    skip_games = {"games/" + g for g in (os.listdir(games) if os.path.isdir(games) else []) if g not in packs}
    # docs/ of a public export holds only the public documents the README links to; a checkout's docs/ is private
    skip = SKIP_DIRS - {"docs"} if from_export else SKIP_DIRS
    n, b = copytree(a.source, os.path.join(out, "app"), skip_rel=skip | skip_games, skip_names={"__pycache__"})
    sizes["app"] = b
    print("app: %d files, %.1f MB (packs: %s)" % (n, b / 1e6, ", ".join(packs)))
    readme = os.path.join(a.source, "docs", "public_ps2rc", "README.md")
    if os.path.isfile(readme):        # a checkout: its root README is the workshop's, the player gets the public one
        shutil.copy2(readme, os.path.join(out, "app", "README.md"))
        print("app/README.md: %s" % readme)
    copy_python(a.python_from, os.path.join(out, "python"), True, a.with_numpy)
    sizes["python"] = du(os.path.join(out, "python"))
    print("python: %.1f MB (from %s, with Pillow%s)" % (sizes["python"] / 1e6, a.python_from,
                                                         ", numpy" if a.with_numpy else ""))
    if a.toolchain:
        miss = [f for f in TOOLCHAIN_NEED_CXX if not os.path.isfile(os.path.join(a.toolchain, f))]
        if miss and not a.allow_no_cxx:
            sys.exit("refused: the toolchain %s has no %s - its EBOOTs would have no in-game menu (build it with "
                     "tools/toolchain/build_windows.sh, or pass --allow-no-cxx)" % (a.toolchain, ", ".join(miss)))
        copytree(a.toolchain, os.path.join(out, "toolchain"), skip_rel=TOOLCHAIN_SKIP_DIRS,
                 filt=lambda rel: rel not in TOOLCHAIN_SKIP_FILES)
        sizes["toolchain"] = du(os.path.join(out, "toolchain"))
        print("toolchain: %.1f MB (from %s)" % (sizes["toolchain"] / 1e6, a.toolchain))
    if a.prebuilt:
        # runtime objects compiled once by us: the player's PC compiles only the game's own units. A store made with
        # a LOCAL pack's headers belongs to a --private bundle only.
        copytree(a.prebuilt, os.path.join(out, "prebuilt"))
        sizes["prebuilt"] = du(os.path.join(out, "prebuilt"))
        print("prebuilt objects: %.1f MB (from %s)" % (sizes["prebuilt"] / 1e6, a.prebuilt))
    lic = os.path.join(out, "licenses")
    os.makedirs(lic)
    for name in TOOLS:
        src = getattr(a, name)
        if src:
            n = add_tool(name, src, out, lic)
            sizes[name] = du(os.path.join(out, "tools", name))
            print("%s: %d files, %.1f MB (from %s)" % (name, n, sizes[name] / 1e6, src))
    if a.ffmpeg and not os.path.isfile(os.path.join(out, "tools", "ffmpeg", "ffprobe.exe")):
        print("note: no ffprobe.exe next to ffmpeg: the intro movies step needs it (it is skipped / fails without)")
    readme = os.path.join(HERE, "APP_README.txt")
    if os.path.isfile(readme):
        with open(readme, encoding="utf-8") as f, open(os.path.join(out, "README.txt"), "w", encoding="utf-8",
                                                       newline="\r\n") as g:
            g.write(f.read())
    for src, name in ((os.path.join(a.python_from, "LICENSE.txt"), "Python-LICENSE.txt"),
                      (os.path.join(a.source, "docs", "public", "LICENSE"), "PS2RC-LICENSE.txt"),
                      (os.path.join(a.source, "LICENSE"), "PS2RC-LICENSE.txt")):
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(lic, name))
    qt_licences(lic)
    cc = find_cc(a.cc)
    for src in (os.path.join(a.source, "NOTICE.md"), os.path.join(a.source, "docs", "public", "NOTICE.md")):
        if os.path.isfile(src):       # third-party notices and the source offer, next to the licence texts
            shutil.copy2(src, os.path.join(lic, "NOTICE.md"))
            break
    package_licences(os.path.join(out, "python", "Lib", "site-packages"), lic)
    exe = os.path.join(out, "PS2RC Converter.exe")
    ico = make_icon(out)
    if ico:
        rc = os.path.join(out, "launcher.rc")
        print("icon: %s" % ico)
    if cc and build_launcher(cc, exe, ico):
        print("launcher: %s (%d bytes, built with %s)" % (exe, os.path.getsize(exe), cc))
        if ico and os.path.exists(rc):
            os.remove(rc)
    else:
        with open(os.path.join(out, "PS2RC Converter.cmd"), "w", newline="\r\n") as f:
            f.write('@echo off\nstart "" "%~dp0python\\pythonw.exe" "%~dp0app\\gui\\run_gui.py"\n')
        print("launcher: no working C compiler - wrote PS2RC Converter.cmd instead")
    with open(os.path.join(out, "ps2rc-convert.cmd"), "w", newline="\r\n") as f:
        f.write('@echo off\n"%~dp0python\\python.exe" -X utf8 -m ps2rc.convert %*\n')
    if a.private:
        with open(os.path.join(out, "PRIVATE_BUILD_DO_NOT_SHARE.txt"), "w") as f:
            f.write("This converter folder holds LOCAL game packs (%s). It is for this PC only: never share it.\n"
                    % ", ".join(local))
    elif not a.no_audit:
        audit = os.path.join(a.source, "tools", "release", "audit_public.py")
        if not os.path.exists(audit):                     # a public export has no release tooling: this checkout's
            audit = os.path.join(ROOT, "tools", "release", "audit_public.py")
        if os.path.exists(audit):
            r = subprocess.run([sys.executable, audit, os.path.join(out, "app"), "--all-files"])
            if r.returncode != 0:
                sys.exit("audit of app/ reported blocking hits: this folder must not be published")
        else:
            print("note: no tools/release/audit_public.py in the source tree - audit the folder before publishing")
    sys.path.insert(0, HERE)
    import bundle_deps
    sys.path.remove(HERE)
    dep = bundle_deps.check(out)
    for f, d, k in dep["notes"]:
        print("windows note: %s needs %s (%s)" % (f, d, k))
    for f, d, k in dep["problems"]:
        print("MISSING on a clean Windows: %s needs %s (%s)" % (f, d, k))
    print("dependency check: %d files, %d problem(s)" % (dep["files"], len(dep["problems"])))
    if dep["problems"] and not a.allow_missing:
        sys.exit("the folder would not run on a clean Windows (see MISSING above)")
    total = du(out)
    print("bundle: %s, %.1f MB (%s)" % (out, total / 1e6, ", ".join("%s %.1f MB" % (k, v / 1e6)
                                                                     for k, v in sizes.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
