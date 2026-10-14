"""Group decoder-vs-objdump mismatches by (objdump mnemonic, ps2rc mnemonic) to drive decoder iterations.
usage: python tools/dev/decode_diff.py [golden ...]   (default: all goldens in <work dir>/golden)"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_r5900_decode import compare, GOLDEN_DIR, GOLDENS  # noqa: E402
from ps2rc.r5900 import decode  # noqa: E402


def main():
    files = sys.argv[1:] or [os.path.join(GOLDEN_DIR, g) for g in GOLDENS]
    groups = collections.defaultdict(list)
    total = 0
    for path in files:
        for line in open(path, encoding="utf-8"):
            idx, word, text = line.rstrip("\n").split("\t", 2)
            pc = (int(idx) * 4) & 0xFFFFFFFF
            w = int(word, 16)
            total += 1
            d = compare(w, pc, text)
            if d:
                ours = decode(w, pc)
                key = (text.split("\t")[0], ours.mnem if ours.valid else "<invalid>")
                groups[key].append("%s %s | ours: %s" % (word, text.replace("\t", " "), ours.text.replace("\t", " ")))
    n = sum(len(v) for v in groups.values())
    print("%d mismatches / %d words in %d groups" % (n, total, len(groups)))
    for key, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:80]:
        print("%6d  objdump=%-14s ours=%-14s  e.g. %s" % (len(v), key[0], key[1], v[0]))


if __name__ == "__main__":
    main()
