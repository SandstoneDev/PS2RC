#!/usr/bin/env python3
"""complete_licenses.py - the licence texts a shipped toolchain folder must carry beyond what build_windows.sh copies
from the binutils / gcc trees and MSYS2: taken from the pinned source archives (fetch_sources.py) themselves.

    python tools/toolchain/complete_licenses.py TOOLCHAIN_DIR SOURCES_DIR [--pyelftools DIST_INFO_DIR]

Writes TOOLCHAIN_DIR/licenses/<name>/...:
  pthread-embedded/  COPYING, COPYING.LIB, COPYING.vita (LGPL-2.1-or-later; MIT for the Vita patches) - linked into
                     every PSP build by the toolchain's default libraries
  newlib/            COPYING.NEWLIB (newlib's own copy) and COPYING.LIBGLOSS
  gmp-mpfr-mpc/      README.txt + the LGPL-3 / GPL-3 texts of the gcc tree (GMP, MPFR and MPC are linked statically
                     into cc1.exe / lto1.exe; their source: the MSYS2 source packages named in BUILDINFO.txt)
  construct/         LICENSE (MIT) when pyelftools' dist-info is given (pyelftools carries construct)
  isl/               LICENSE (MIT) + README.txt - ISL is linked statically into cc1.exe / lto1.exe; the text comes from
                     an isl-*.tar.* source archive in SOURCES_DIR, else from --isl-license, else from MSYS2's own isl
                     package (/usr/share/licenses/isl/LICENSE: the same upstream file)
A text that is missing from its archive is reported, never invented. Exit status 1 when anything is missing."""
import argparse
import glob
import os
import shutil
import sys
import tarfile


def _extract(archive, names, dest):
    """Copy the members whose base name is in names (first match each) into dest -> list of names found."""
    found = []
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            base = os.path.basename(m.name)
            if m.isfile() and base in names and base not in found and m.name.count("/") <= 2:
                os.makedirs(dest, exist_ok=True)
                with t.extractfile(m) as src, open(os.path.join(dest, base), "wb") as out:
                    shutil.copyfileobj(src, out)
                found.append(base)
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("toolchain")
    ap.add_argument("sources")
    ap.add_argument("--pyelftools", help="pyelftools' .dist-info folder (its LICENSE includes construct's MIT text)")
    ap.add_argument("--isl-license", help="ISL's LICENSE file (default: an isl source archive, else MSYS2's copy)")
    a = ap.parse_args(argv)
    lic = os.path.join(a.toolchain, "licenses")
    missing = []
    jobs = [("pthread-embedded-*.tar.gz", ["COPYING", "COPYING.LIB", "COPYING.vita"], "pthread-embedded"),
            ("newlib-*.tar.gz", ["COPYING.NEWLIB", "COPYING.LIBGLOSS"], "newlib")]
    for pattern, names, sub in jobs:
        arch = sorted(glob.glob(os.path.join(a.sources, pattern)))
        if not arch:
            missing.append("%s: no archive %s in %s" % (sub, pattern, a.sources))
            continue
        got = _extract(arch[-1], names, os.path.join(lic, sub))
        missing += ["%s: %s not in %s" % (sub, n, os.path.basename(arch[-1])) for n in names if n not in got]
        print("%s: %s" % (sub, ", ".join(got) or "nothing"))
    gcc = sorted(glob.glob(os.path.join(a.sources, "gcc-*.tar.gz")))
    if gcc:
        d = os.path.join(lic, "gmp-mpfr-mpc")
        got = _extract(gcc[-1], ["COPYING3", "COPYING3.LIB"], d)
        with open(os.path.join(d, "README.txt"), "w", encoding="ascii", newline="\n") as f:
            f.write("GMP, MPFR and MPC are linked statically into the compiler programs (cc1.exe, lto1.exe).\n"
                    "Licence: GNU LGPL version 3 or later (GMP: also GNU GPL version 2 or later) - the texts are\n"
                    "COPYING3.LIB (LGPL-3) and COPYING3 (GPL-3) in this folder. Source: the MSYS2 UCRT64 source\n"
                    "packages of the versions named in BUILDINFO.txt (offered next to the converter download).\n")
        print("gmp-mpfr-mpc: README.txt + %s" % ", ".join(got))
        missing += ["gmp-mpfr-mpc: %s not in the gcc archive" % n for n in ("COPYING3", "COPYING3.LIB") if n not in got]
    else:
        missing.append("gmp-mpfr-mpc: no gcc archive in %s" % a.sources)
    isl = os.path.join(lic, "isl")
    arch = sorted(glob.glob(os.path.join(a.sources, "isl-*.tar.*")))
    got = _extract(arch[-1], ["LICENSE"], isl) if arch else []
    if not got:
        for cand in [a.isl_license, "/usr/share/licenses/isl/LICENSE",
                     os.path.join(os.environ.get("SystemDrive", "C:") + os.sep, "msys64", "usr", "share", "licenses",
                                  "isl", "LICENSE")]:
            if cand and os.path.isfile(cand):
                os.makedirs(isl, exist_ok=True)
                shutil.copyfile(cand, os.path.join(isl, "LICENSE"))
                got = ["LICENSE"]
                break
    if got and open(os.path.join(isl, "LICENSE"), encoding="utf-8", errors="replace").read().startswith("MIT License"):
        with open(os.path.join(isl, "README.txt"), "w", encoding="ascii", newline="\n") as f:
            f.write("ISL (the integer set library, gcc's Graphite loop optimiser) is linked statically into the compiler\n"
                    "programs (cc1.exe, lto1.exe). Licence: MIT (LICENSE in this folder). Source: the isl release of\n"
                    "the version named in BUILDINFO.txt (https://libisl.sourceforge.io/) / its MSYS2 source package.\n")
        print("isl: LICENSE + README.txt")
    else:
        missing.append("isl: no MIT LICENSE found (isl-*.tar.* in %s, --isl-license, MSYS2's copy)" % a.sources)
    if a.pyelftools:
        src = os.path.join(a.pyelftools, "LICENSE")
        if os.path.isfile(src):
            os.makedirs(os.path.join(lic, "construct"), exist_ok=True)
            shutil.copyfile(src, os.path.join(lic, "construct", "LICENSE"))
            print("construct: LICENSE (from pyelftools)")
        else:
            missing.append("construct: no LICENSE in %s" % a.pyelftools)
    for m in missing:
        print("MISSING " + m, file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
