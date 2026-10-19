"""Check that a prebuilt pspdev install was built from the commits pinned in toolchain.lock.toml.

    python tools/toolchain/check_prebuilt_commits.py PSPDEV_DIR_OR_BUILD_TXT [--names binutils,gcc,...]

A bundle that ships files of a prebuilt pspdev release (the Linux / macOS host programs, or the target libraries
the Windows toolchain takes from the Linux release) offers the archives of fetch_sources.py as their source; that offer
is only right when the prebuilt's build.txt names the same commits. Exit status 1 on any difference (or a missing
build.txt), 0 when every named component matches.
"""
import os
import sys

try:
    import tomllib
except ImportError:          # Python < 3.11
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "toolchain.lock.toml")
DEFAULT = ("binutils", "gcc", "newlib", "pthread-embedded", "pspsdk")


def build_commits(path):
    """build.txt lines: '<component> <commit> <date> <subject>' -> {component: commit}"""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and parts[0] not in out:
                out[parts[0]] = parts[1]
    return out


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    if tomllib is None:
        sys.exit("needs Python 3.11 or newer (tomllib)")
    path = argv[0]
    if os.path.isdir(path):
        path = os.path.join(path, "build.txt")
    names = DEFAULT
    if "--names" in argv:
        names = tuple(argv[argv.index("--names") + 1].split(","))
    if not os.path.isfile(path):
        print("no %s: the prebuilt's commits cannot be checked" % path)
        return 1
    lock = tomllib.load(open(LOCK, "rb"))["sources"]
    got = build_commits(path)
    bad = 0
    for n in names:
        want, have = lock[n]["commit"], got.get(n)
        if have != want:
            print("MISMATCH %-17s prebuilt %s, toolchain.lock.toml %s" % (n, have, want))
            bad += 1
        else:
            print("ok       %-17s %s" % (n, want))
    if bad:
        print("the prebuilt is not built from the pinned commits: its source is not the pinned archives "
              "(update toolchain.lock.toml or the release tag)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
