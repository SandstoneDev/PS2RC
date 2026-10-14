"""R5900 decoder vs GNU objdump (binutils 2.44, -m mips:5900, -M no-aliases) - an oracle independent of ps2rc's tables.

Accepted differences (documented, not bugs):
  * NON_R5900: encodings objdump decodes for other MIPS ISAs that do not exist on the R5900 (R5900 opcode map):
    jalx, double/long FPU formats, cfc0/ctc0, generic coprocessor ops c0/c1/c2/c3 - ps2rc reports them invalid.
  * SQRT_BUG: binutils decodes R5900 SQRT.S like generic MIPS (fd, fs; ft = 0) but the R5900 takes the operand in ft
    (`SQRT.S fd, ft`, fs field 00000), as PCSX2 executes it (tests/poc1 fpu_unary against the PCSX2 oracle; objdump shows such words as `c1`). ps2rc decodes SQRT.S with the operand in ft and ignores the reserved fs field (as PCSX2 does). Consequence:
    GNU as (-march=r5900) MISENCODES `sqrt.s $fd,$fs` (operand placed in fs) - hand-written test code must use .word.
  * VU_ACC_ORDER: binutils prints VADDA and VMSUBA operands as (ft, fs); ps2rc prints them (fs, ft) like VMULA and
    VSUBA, which binutils prints that way.
  * RENAMES: objdump spells R5900 CVT.W.S as trunc.w.s (the R5900 conversion always truncates: the
    PCSX2 oracle runs of tests/poc1); ps2rc uses the name cvt.w.s. objdump prints `ori rt,zero,imm` as the macro `dli rt,imm` even with -M no-aliases.
  * EE_IGNORED_FIELDS: SPECIAL words whose "0" fields (the instruction formats) are nonzero - objdump prints .word,
    the EE executes the base operation: PCSX2 selects SPECIAL operations by the function field alone and reads only
    the operand fields, and Sony's libipu executes its inline constant 0x00ff00ff = DSRA32 with rs = 7. ps2rc decodes
    such a word exactly like the same word with those fields cleared (ps2rc.r5900.decode.SPECIAL_IGNORED /
    special_ignored_bits).
"""
import os
import re

import pytest

from ps2rc import config
from ps2rc.r5900 import decode
from ps2rc.r5900.decode import special_ignored_bits

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLES = os.path.join(ROOT, "tests", "data", "r5900_objdump_examples.tsv")
GOLDEN_DIR = os.path.join(config.work_dir(), "golden")   # tools/dev/objdump_golden.py writes them (synthetic words)
GOLDENS = ["random400k.tsv", "structured.tsv"]

NON_R5900 = re.compile(r"^(jalx|c[0-3]|cfc0|ctc0|[a-z.]*\.(d|l)|cvt\.(d|l)\.[a-z]|dli)\b")


def _vu_acc_order_divergence(ours, theirs):
    """VADDA/VMSUBA: same instruction, objdump swaps the two source operands (see module docstring)."""
    mnem, _, ops = theirs.partition("\t")
    if mnem.split(".")[0] not in ("vadda", "vmsuba") or not ours.valid:
        return False
    a = ops.split(",")
    return len(a) == 3 and ours.text == mnem + "\t" + ",".join([a[0], a[2], a[1]])


def _sqrt_divergence(word, objdump_mnem):
    """SQRT.S encodings on which binutils and Sony disagree (see module docstring)."""
    if (word >> 21) != (17 << 5 | 16) or (word & 63) != 4:
        return False
    fs, ft = (word >> 11) & 31, (word >> 16) & 31
    if objdump_mnem == "c1" and ft != 0:                  # Sony sqrt.s fd,ft; binutils rejects ft != 0
        return True
    if objdump_mnem == "sqrt.s" and fs != 0:              # binutils reads the operand from fs
        return True
    return False


def normalize_objdump(text):
    mnem, _, ops = text.partition("\t")
    if mnem == "trunc.w.s":
        mnem = "cvt.w.s"
    if mnem == "dli":
        rt, imm = ops.split(",")
        return "ori\t%s,zero,%s" % (rt, imm)
    return mnem + ("\t" + ops if ops else "")


def _ee_ignored_fields(word, ours):
    """EE_IGNORED_FIELDS (see module docstring): ours is exactly the base operation of the word with its ignored
    must-be-zero fields cleared."""
    bits = special_ignored_bits(word)
    if not bits or not ours.valid:
        return False
    base = decode(word & ~bits, ours.pc)
    return base.valid and (ours.mnem, ours.ops, ours.text) == (base.mnem, base.ops, base.text)


def compare(word, pc, objdump_text):
    """Return None when ps2rc agrees with objdump, else a description of the difference."""
    ours = decode(word, pc)
    theirs = objdump_text
    if theirs.startswith(".word"):
        if not ours.valid or _ee_ignored_fields(word, ours):
            return None
        return "objdump .word, ours %r" % ours.text
    mnem = theirs.split("\t")[0]
    if _sqrt_divergence(word, mnem) or _vu_acc_order_divergence(ours, theirs):
        return None
    if NON_R5900.match(mnem) and mnem not in ("dli",):
        return None if not ours.valid else "objdump non-R5900 %r, ours %r" % (theirs, ours.text)
    expected = normalize_objdump(theirs)
    if not ours.valid:
        return "ours invalid, objdump %r" % expected
    return None if ours.text == expected else "ours %r, objdump %r" % (ours.text, expected)


def load_tsv(path, with_index=True):
    rows = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        parts = line.rstrip("\n").split("\t", 2 if with_index else 2)
        rows.append(parts)
    return rows


def test_decoder_matches_objdump_on_curated_examples():
    bad = []
    for word, pc, text in load_tsv(EXAMPLES, with_index=False):
        diff = compare(int(word, 16), int(pc, 16), text)
        if diff:
            bad.append("%s @%s: %s" % (word, pc, diff))
    assert not bad, "%d mismatches, first:\n%s" % (len(bad), "\n".join(bad[:40]))


@pytest.mark.slow
@pytest.mark.parametrize("golden", GOLDENS)
def test_decoder_matches_objdump_on_full_golden(golden):
    path = os.path.join(GOLDEN_DIR, golden)
    if not os.path.exists(path):
        pytest.skip("golden not generated: run tools/dev/objdump_golden.py (needs the pspdev binutils)")
    bad, n = [], 0
    for idx, word, text in load_tsv(path):
        n += 1
        diff = compare(int(word, 16), (int(idx) * 4) & 0xFFFFFFFF, text)
        if diff:
            bad.append("%s @%x: %s" % (word, int(idx) * 4, diff))
    assert not bad, "%d/%d mismatches, first:\n%s" % (len(bad), n, "\n".join(bad[:60]))
