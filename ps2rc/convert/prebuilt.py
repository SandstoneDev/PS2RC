"""The prebuilt object store: compiled objects named by their content key (build.object_key: the compile command with
the toolchain folder as a placeholder, the compiler version, the headers' hash, the source text). A converter build
copies an object from a store instead of compiling it when the key matches - same source, same flags, same compiler
-> the same object, so nothing changes in the result; only the time.

A release ships the store of the runtime / HLE units (they are the same on every player's PC for a given pack and
converter version), so only the game-derived generated units compile on the player's PC.

    python -m ps2rc.convert.prebuilt export WORK/build STORE [--all]     objects of a finished build into a store
    python -m ps2rc.convert.prebuilt info STORE

export takes the runtime units only (obj/hle/*.o) unless --all: generated units are derived from the game's code
and never go into a store that leaves the player's PC. A store made with a LOCAL pack's headers is LOCAL too.
"""
import json
import os
import shutil
import sys

STORE_ENV = "PS2RC_PREBUILT"


def bundled_store():
    """<bundle>/prebuilt next to the app's python/ and toolchain/ folders."""
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (os.path.join(here, "..", "..", "prebuilt"), os.path.join(here, "..", "..", "..", "prebuilt")):
        if os.path.isdir(up):
            return os.path.abspath(up)
    return None


def stores(explicit=None):
    out = [explicit, os.environ.get(STORE_ENV), bundled_store()]
    return [d for d in out if d and os.path.isdir(d)]


def export(build_dir, store, include_generated=False):
    stamps = json.load(open(os.path.join(build_dir, "obj", "stamps.json"), encoding="utf-8"))
    os.makedirs(store, exist_ok=True)
    idx_path = os.path.join(store, "index.json")
    try:
        idx = json.load(open(idx_path, encoding="utf-8"))
    except (OSError, ValueError):
        idx = {}
    n = 0
    for label, key in sorted(stamps.items()):
        if not include_generated and not label.startswith("obj/hle/"):
            continue
        src = os.path.join(build_dir, label)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(store, key + ".o"))
            idx[key] = label
            n += 1
    with open(idx_path, "w", encoding="utf-8") as f:
        json.dump(idx, f, indent=0, sort_keys=True)
    return n


def main(argv):
    if len(argv) >= 3 and argv[0] == "export":
        n = export(argv[1], argv[2], "--all" in argv)
        print("prebuilt store %s: %d objects exported" % (argv[2], n))
        return 0
    if len(argv) >= 2 and argv[0] == "info":
        objs = [f for f in os.listdir(argv[1]) if f.endswith(".o")]
        size = sum(os.path.getsize(os.path.join(argv[1], f)) for f in objs)
        print("%s: %d objects, %.1f MB" % (argv[1], len(objs), size / 1e6))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
