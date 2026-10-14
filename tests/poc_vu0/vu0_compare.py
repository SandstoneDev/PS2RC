"""Compare VU0 oracle result files (128-byte output blocks, test-major) per test and vector class.

    python tests/poc_vu0/vu0_compare.py <host results.bin>
    python tests/poc_vu0/vu0_compare.py --show <a.bin> <b.bin> <test> [count]
    python tests/poc_vu0/vu0_compare.py --expected <out.bin>     (reference blocks for the PSP harness)

Prints, per test, the mismatching vectors of host vs PCSX2 interpreter and host vs PCSX2 recompiler, split into
ordinary vectors (no exponent-255 lane) and edge vectors, and rec vs interp. check() applies the acceptance rules of
test_poc_vu0.py:
  host == interpreter on every vector of every test with ref 'both';
  host == recompiler on every ordinary vector (edge vectors: counted, documented deviation);
  tests with ref 'rec' (a documented interpreter quirk): host == recompiler on every vector.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(os.environ.get("PS2RC_VU0_OUT") or os.path.join(HERE, "..", "..", "build", "poc_vu0"))


def load(path):
    return open(path, "rb").read()


def diff(meta, a, b):
    """{test: (ordinary mismatches [vector], edge mismatches [vector])}."""
    sz = meta["out_size"]
    res = {}
    for k, t in enumerate(meta["tests"]):
        n, nord = t["nvec"], t["nord"]
        ords, edges = [], []
        for v in range(n):
            o = (k * n + v) * sz
            if a[o:o + sz] != b[o:o + sz]:
                (ords if v < nord or t["kind"] == "raw" else edges).append(v)
        res[t["name"]] = (ords, edges)
    return res


def check(host_path, out=OUT):
    """(failures {test: reason}, report text)."""
    meta = json.load(open(os.path.join(out, "tests.json"), encoding="utf-8"))
    host = load(host_path)
    interp = load(os.path.join(out, "pcsx2_interp.bin"))
    rec = load(os.path.join(out, "pcsx2_rec.bin"))
    hi, hr, ri = diff(meta, host, interp), diff(meta, host, rec), diff(meta, rec, interp)
    fails, lines = {}, []
    for t in meta["tests"]:
        name = t["name"]
        lines.append("%-15s host/interp %4d+%-4d host/rec %4d+%-4d rec/interp %4d+%-4d  (ordinary+edge)" % (
            name, len(hi[name][0]), len(hi[name][1]), len(hr[name][0]), len(hr[name][1]), len(ri[name][0]),
            len(ri[name][1])))
        if t["ref"] == "both":
            if hi[name][0] or hi[name][1]:
                fails[name] = "host != interpreter on %d vectors" % (len(hi[name][0]) + len(hi[name][1]))
            elif hr[name][0]:
                fails[name] = "host != recompiler on %d ordinary vectors" % len(hr[name][0])
        elif hr[name][0] or hr[name][1]:
            fails[name] = "host != recompiler on %d vectors" % (len(hr[name][0]) + len(hr[name][1]))
    return fails, "\n".join(lines)


def write_expected(path, out=OUT):
    """The reference blocks the PSP harness compares with: PCSX2 interpreter for ref 'both' tests, PCSX2 recompiler
    for ref 'rec' tests (= what the host results equal, see check())."""
    meta = json.load(open(os.path.join(out, "tests.json"), encoding="utf-8"))
    interp = load(os.path.join(out, "pcsx2_interp.bin"))
    rec = load(os.path.join(out, "pcsx2_rec.bin"))
    sz, blob = meta["out_size"], bytearray()
    for k, t in enumerate(meta["tests"]):
        n = t["nvec"]
        src = rec if t["ref"] == "rec" else interp
        blob += src[k * n * sz:(k + 1) * n * sz]
    open(path, "wb").write(blob)


def show(path_a, path_b, name, count):
    meta = json.load(open(os.path.join(OUT, "tests.json"), encoding="utf-8"))
    vec = load(os.path.join(OUT, "vectors.bin"))
    a, b = load(path_a), load(path_b)
    sz, isz = meta["out_size"], meta["in_size"]
    for k, t in enumerate(meta["tests"]):
        if t["name"] != name:
            continue
        n = t["nvec"]
        shown = 0
        for v in range(n):
            o = (k * n + v) * sz
            if a[o:o + sz] != b[o:o + sz] and shown < count:
                shown += 1
                i = t["in_off"] + v * isz
                print("v%d in:" % v)
                for q in range(isz // 16):
                    print("   %3d %s" % (16 * q, vec[i + 16 * q:i + 16 * q + 16].hex()))
                for q in range(sz // 16):
                    x, y = a[o + 16 * q:o + 16 * q + 16], b[o + 16 * q:o + 16 * q + 16]
                    print("   out %3d %s %s %s" % (16 * q, x.hex(), "==" if x == y else "!=", y.hex()))


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--show":                     # --show <a.bin> <b.bin> <test> [count]
        show(argv[1], argv[2], argv[3], int(argv[4]) if len(argv) > 4 else 2)
        return
    if argv and argv[0] == "--expected":                 # --expected <out.bin>: reference blocks for the PSP run
        write_expected(argv[1])
        return
    fails, text = check(argv[0])
    print(text)
    print("FAIL: %s" % fails if fails else "PASS")


if __name__ == "__main__":
    main()
