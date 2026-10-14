"""Compare PoC-1 result files (32-byte output blocks, test-major) and print per-test mismatch counts.
usage: python compare.py <results.bin> [reference.bin=build/poc1/pcsx2_interp.bin] [--show N]"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.path.join(HERE, "..", "..", "build", "poc1"))


def compare(ours_path, ref_path, show=2):
    meta = json.load(open(os.path.join(OUT, "tests.json")))
    a = open(ours_path, "rb").read()
    b = open(ref_path, "rb").read()
    vec = open(os.path.join(OUT, "vectors.bin"), "rb").read()
    n = meta["tests"][0]["nvec"]
    bad = {}
    for k, t in enumerate(meta["tests"]):
        rows = []
        for v in range(n):
            o = (k * n + v) * 32
            if a[o:o + 32] != b[o:o + 32]:
                rows.append((v, vec[(k * n + v) * 64:(k * n + v) * 64 + 32].hex(), a[o:o + 32].hex(), b[o:o + 32].hex()))
        if rows:
            bad[t["name"]] = rows
    return meta, bad


def main():
    argv = sys.argv[1:]
    show = 2
    if "--show" in argv:
        k = argv.index("--show")
        show = int(argv[k + 1])
        del argv[k:k + 2]
    args = argv
    ours = args[0]
    ref = args[1] if len(args) > 1 else os.path.join(OUT, "pcsx2_interp.bin")
    meta, bad = compare(ours, ref)
    total = sum(len(v) for v in bad.values())
    print("%d mismatching vectors in %d of %d tests" % (total, len(bad), len(meta["tests"])))
    for name, rows in bad.items():
        print("  %-14s %5d" % (name, len(rows)))
        for v, inp, x, y in rows[:show]:
            print("      v%-5d in=%s\n             ours=%s\n             ref =%s" % (v, inp, x, y))


if __name__ == "__main__":
    main()
