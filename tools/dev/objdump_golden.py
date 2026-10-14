"""Build objdump (binutils 2.44, -m mips:5900) reference disassembly for a list of 32-bit words.

The reference is independent of ps2rc's own opcode tables: GNU objdump (pspdev) runs on a raw binary of the words
(natively on Linux, through WSL on Windows). Output: TSV lines  <index> <word hex8> <objdump text>  (text = mnemonic
+ '\t' + operands, '.word' when undecodable). The goldens of tests/test_r5900_decode.py (in <work dir>/golden):

    python tools/dev/objdump_golden.py --random 400000 --seed 1 --out <work dir>/golden/random400k.tsv
    python tools/dev/objdump_golden.py --structured --out <work dir>/golden/structured.tsv
    python tools/dev/objdump_golden.py --words words.bin --out x.tsv
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc import config  # noqa: E402


def win_to_wsl(p):
    return config.posix_path(os.path.abspath(p))


def run_objdump(words, workdir):
    os.makedirs(workdir, exist_ok=True)
    binp = os.path.join(workdir, "words.bin")
    with open(binp, "wb") as f:
        f.write(struct.pack("<%dI" % len(words), *words))
    outp = os.path.join(workdir, "objdump.txt")
    script = os.path.join(workdir, "run.sh")
    with open(script, "w", newline="\n") as f:
        f.write("export PATH=$HOME/pspdev/bin:$PATH\n")
        f.write("psp-objdump -b binary -m mips:5900 -EL -D -z --no-show-raw-insn -M no-aliases '%s' > '%s'\n"
                % (win_to_wsl(binp), win_to_wsl(outp)))
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    subprocess.run(config.bash_argv(script), check=True, env=env)
    return parse_objdump(open(outp, encoding="utf-8", errors="replace").read(), len(words))


_LINE = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")


def parse_objdump(text, n):
    out = [None] * n
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        off = int(m.group(1), 16)
        if off % 4 or off // 4 >= n:
            continue
        out[off // 4] = m.group(2).rstrip()
    return out


FIELDS = [(21, 0x1F), (16, 0x1F), (11, 0x1F), (6, 0x1F), (0, 0x3F)]   # rs rt rd sa funct (generic MIPS layout)


def structured_words(seed, per_combo=6):
    """Encoding-space sweep that does not use ps2rc's opcode knowledge: for every primary opcode, every value of the
    'secondary' 5/6-bit fields (rs, rt, funct) and every subset of the remaining generic fields forced to zero, emit a
    few random fillings. Strict encodings (fields required to be zero) are therefore covered."""
    rnd = random.Random(seed)
    words = []
    for op in range(64):
        for sec_shift, sec_mask in ((0, 0x3F), (21, 0x1F), (16, 0x1F)):
            for sec in range(sec_mask + 1):
                for zero_set in range(32):
                    for _ in range(per_combo if zero_set in (0, 31) else 1):
                        w = (op << 26) | (rnd.getrandbits(26))
                        w = (w & ~(sec_mask << sec_shift)) | (sec << sec_shift)
                        for k, (sh, mk) in enumerate(FIELDS):
                            if zero_set >> k & 1 and sh != sec_shift:
                                w &= ~(mk << sh)
                        words.append(w & 0xFFFFFFFF)
    return sorted(set(words))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--random", type=int, default=0)
    ap.add_argument("--structured", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--words")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.random:
        rnd = random.Random(a.seed)
        words = [rnd.getrandbits(32) for _ in range(a.random)]
    elif a.structured:
        words = structured_words(a.seed)
    else:
        raw = open(a.words, "rb").read()
        words = list(struct.unpack("<%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
    texts = run_objdump(words, os.path.join(os.path.dirname(os.path.abspath(a.out)), "_work"))
    with open(a.out, "w", encoding="utf-8") as f:
        for i, (w, t) in enumerate(zip(words, texts)):
            f.write("%d\t%08x\t%s\n" % (i, w, t if t is not None else "<missing>"))
    print("wrote", len(words), "lines to", a.out)


if __name__ == "__main__":
    main()
