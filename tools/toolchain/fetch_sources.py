"""Fetch the pinned toolchain sources of toolchain.lock.toml (GitHub source archives of exact commits) and check
their sha256; these archives are also the "Corresponding Source" published next to a toolchain bundle (GPLv3 6(d)).

    python tools/toolchain/fetch_sources.py OUT_DIR [--only gcc,binutils] [--record] [--dry-run]

--record   write the sha256 of archives that have none yet into toolchain.lock.toml (first fetch)
--dry-run  print the URLs and target files, download nothing
Downloads only from github.com/pspdev; refuses an archive whose recorded sha256 differs.
"""
import hashlib
import os
import re
import sys
import urllib.request

try:
    import tomllib
except ImportError:
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "toolchain.lock.toml")


def url_of(src):
    """GitHub's source archive of one commit (all pinned repositories are pspdev's)."""
    owner, name = src["repo"].split("/", 1)
    if owner != "pspdev":
        raise SystemExit("toolchain.lock.toml: %s is not a pspdev repository" % src["repo"])
    return "https://github.com/pspdev/%s/archive/%s.tar.gz" % (name, src["commit"])


def file_of(name, src):
    return "%s-%s.tar.gz" % (name, src["commit"][:12])


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def record(name, digest):
    text = open(LOCK, encoding="utf-8").read()
    pat = re.compile(r'(\[sources\.%s\][^\[]*?sha256 = ")(")' % re.escape(name), re.S)
    new, n = pat.subn(lambda m: m.group(1) + digest + m.group(2), text, count=1)
    if n:
        with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)
    return bool(n)


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    out = argv[0]
    only = None
    if "--only" in argv:
        only = set(argv[argv.index("--only") + 1].split(","))
    dry, rec = "--dry-run" in argv, "--record" in argv
    lock = tomllib.load(open(LOCK, "rb"))
    if not dry:
        os.makedirs(out, exist_ok=True)
    bad = 0
    for name, src in lock["sources"].items():
        if only and name not in only:
            continue
        url, path = url_of(src), os.path.join(out, file_of(name, src))
        if dry:
            print("%-22s %s -> %s" % (name, url, path))
            continue
        if not os.path.exists(path):
            print("fetch %s ..." % url, flush=True)
            with urllib.request.urlopen(url, timeout=600) as r, open(path + ".part", "wb") as f:
                for b in iter(lambda: r.read(1 << 20), b""):
                    f.write(b)
            os.replace(path + ".part", path)
        d = sha256(path)
        want = src.get("sha256", "")
        if want and want != d:
            print("MISMATCH %s: sha256 %s, lock says %s" % (path, d, want))
            bad += 1
        elif not want:
            print("%s: sha256 %s (not recorded%s)" % (name, d, "; recorded now" if rec and record(name, d) else ""))
        else:
            print("%s: ok" % name)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
