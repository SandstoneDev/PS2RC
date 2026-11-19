"""The R5900 performance counters (COP0 MFPS/MTPS = PCCR, MFPC/MTPC = PCR0/PCR1; games time their frames with them):
the decoder's register writes, their IR (reads are not pure: the value changes with time), and a guest function that
uses all four going through the C back-end in every variant - each becomes an rt_fatal point the harness may continue
after (the HLE models the counters, hle_main.c). Hand-encoded words; emission only (no compiler needed)."""
import struct

import pytest

from ps2rc import ir
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from ps2rc.image import Image, Segment
from ps2rc.r5900 import decode
from ps2rc.r5900.semantics import dest_gpr

V0, V1, A0, A1, RA = 2, 3, 4, 5, 31


def mfps(rt, reg=0): return 0x4000C800 | rt << 16 | reg << 1
def mfpc(rt, reg): return 0x4000C801 | rt << 16 | reg << 1
def mtps(rt, reg=0): return 0x4080C800 | rt << 16 | reg << 1
def mtpc(rt, reg): return 0x4080C801 | rt << 16 | reg << 1
def jr(rs): return rs << 21 | 0x08


BASE, SIZE = 0x1000, 0x1000
F = 0x1000
PROGRAM = [mtps(A0), mtpc(A1, 0), mtpc(A1, 1), mfpc(V0, 0), mfpc(V1, 1), mfps(A0), jr(RA), 0]


def image():
    mem = bytearray(SIZE)
    for k, w in enumerate(PROGRAM):
        struct.pack_into("<I", mem, F - BASE + 4 * k, w)
    seg = Segment(vaddr=BASE, filesz=SIZE, memsz=SIZE, flags=7, data=bytes(mem))
    return Image(path="<pccr>", entry=F, gp=0, segments=[seg], overlay_slots=[])


@pytest.mark.parametrize("word,text,dest", [
    (mfpc(V0, 0), "mfpc\tv0,0", V0), (mfpc(V1, 1), "mfpc\tv1,1", V1), (mfps(V0), "mfps\tv0,0", V0),
    (mtpc(A1, 1), "mtpc\ta1,1", None), (mtps(A0), "mtps\ta0,0", None)])
def test_decoder_and_register_writes(word, text, dest):
    d = decode(word, F)
    assert d.text == text
    assert dest_gpr(d) == dest


def test_ir_reads_are_not_pure():
    for w, dst in ((mfpc(V0, 0), V0), (mfps(V1), V1), (mtpc(A0, 1), None), (mtps(A1), None)):
        op = ir.lift(decode(w, F))
        assert op.pure is False                            # never dropped as dead code, never moved
        assert op.dst == dst


@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4"])
def test_function_with_counters_translates(variant):
    img = image()
    fs = discover_functions(img, [(BASE, BASE + SIZE)], extra_seeds=[F])
    opts = {}
    if variant != "v0":
        opts = {"cache": True, "fpu_fast": variant in ("v3", "v4"), "abi": variant == "v4"}
    if variant in ("v2", "v3", "v4"):
        opts["narrow"] = narrowing_plan(img, fs.functions, abi=variant == "v4", roots={F})
    if variant == "v4":
        opts["livein"] = argument_livein(img, fs.functions)
    src, _header = emit_program(img, fs, roots=[F], **opts)
    assert "unsupported" not in src
    for m in ("mtps", "mtpc", "mfpc", "mfps"):
        assert '"%s"' % m in src, m                        # each one an rt_fatal point the HLE continues after
