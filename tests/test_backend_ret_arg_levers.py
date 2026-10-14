"""The back-end levers rlive (return-value demand) and argx (one-word
arguments through pass-through callees) on hand-encoded functions: the analysis results and the generated C text.

rlive:  G writes v0, v1 and f0; F only calls G and returns (pass-through); H (a root the "HLE" calls: complete returns)
        calls F, reads v0 and redefines v1 / f0 / f1 before returning. So nobody can observe G's or F's v1 / f0 / f1:
        G's return writes v0 only; without the lever (or with F in the complete set) it writes v1 and f0 too.
argx:   K reads a0's low word; P reads a0's low word and hands the unchanged slot to K; Q calls P with a0 = 7. Lever
        ctx32 alone refuses the one-word store at Q's call (P exposes the slot); with argx K reads it as one word, so
        Q stores a0 as one word.
"""
import re

from ps2rc import backend_c
from ps2rc.analysis import retdemand as RD
from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import argument_livein, narrowing_plan
from ps2rc.backend_c import emit_program
from r5900_enc import A0, NOP, RA, SP, T0, V0, V1, ZERO, addiu, addu, image, jal, jr, lw, mtc1, sw

G, F, H, K, P, Q = 0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500


def frame(body):
    return [addiu(SP, SP, -16), sw(RA, 0, SP)] + body + [lw(RA, 0, SP), jr(RA), addiu(SP, SP, 16)]


PROGRAM = {
    G: [addiu(V1, ZERO, 5), mtc1(V1, 0), addiu(V0, ZERO, 1), jr(RA), NOP],
    F: frame([jal(G), NOP]),
    H: frame([jal(F), NOP, addu(A0, V0, ZERO), sw(A0, 4, SP), addiu(V1, ZERO, 0), mtc1(ZERO, 0), mtc1(ZERO, 1)]),
    K: [addu(V0, A0, ZERO), jr(RA), NOP],
    P: frame([addu(T0, A0, ZERO), sw(T0, 4, SP), jal(K), NOP]),
    Q: frame([addiu(A0, ZERO, 7), jal(P), NOP]),
}
SEEDS = [G, F, H, K, P, Q]
CODE = [(0x1000, 0x1600)]


def _gen(variant, full=(H, Q), units=(None,)):
    img = image(PROGRAM)
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    opts = {"cache": True, "fpu_fast": True, "abi": True,
            "narrow": narrowing_plan(img, fs.functions, abi=True, roots=set(SEEDS)),
            "livein": argument_livein(img, fs.functions), "v5": backend_c.V5_STEPS[variant]}
    saved = dict(backend_c.RLIVE)
    backend_c.RLIVE.update(on=True, units=units, full=frozenset(full), indirect=frozenset(), stats={})
    try:
        src, _hdr = emit_program(img, fs, roots=SEEDS, **opts)
    finally:
        backend_c.RLIVE.clear()
        backend_c.RLIVE.update(saved)
    return src


def _body(src, addr):
    m = re.search(r"void fn_%08x\(R5900Ctx \*c\)\n\{\n(.*?)\n\}\n" % addr, src, re.S)
    assert m, "fn_%08x missing" % addr
    return m.group(1)


def _ret_line(src, addr):
    return [ln for ln in _body(src, addr).split("\n") if "return;" in ln][-1]


def test_retdemand_analysis():
    img = image(PROGRAM)
    fs = discover_functions(img, CODE, extra_seeds=SEEDS)
    starts = set(fs.functions)
    summ = {}
    for a, fn in fs.functions.items():
        f = backend_c.fn_cfg(img, a, fn.addrs, {}, starts)
        summ[a] = RD.site_summaries(f, frozenset())
    dem, ind = RD.solve(summ, full={H, Q})
    assert dem[H] == RD.FULL and ind == 0
    assert dem[F] == RD.V0L | RD.V0H | RD.V0Q          # H reads v0 (not narrowed: both words), v0's upper half
    assert dem[G] == dem[F]                            # passes through H's return; v1 / f0 / f1 are redefined
    dem2, _ = RD.solve(summ, full={H, Q, F})
    assert dem2[G] == RD.FULL                          # F complete: everything passes through it


def test_rlive_return_writes():
    off = _gen("v5s4y3")
    on = _gen("v5s4y3r")
    assert "c->r[3]" in _ret_line(off, G) and "c->fpr[0].u = f0" in _ret_line(off, G)
    ret = _ret_line(on, G)
    assert "c->r[2]" in ret and "c->r[3]" not in ret and "c->fpr[0]" not in ret
    # the other functions are unchanged, and a complete set keeps G's returns as they were
    for a in (F, H, K, P, Q):
        assert _body(off, a) == _body(on, a)
    assert _body(_gen("v5s4y3r", full=(H, Q, G)), G) == _body(off, G)
    # the lever needs the driver's RLIVE (other unit names: no change)
    assert _body(_gen("v5s4y3r", units=("main",)), G) == _body(off, G)


def test_argx_one_word_through_pass_through():
    call = lambda src: [ln for ln in _body(src, Q).split("\n") if "fn_%08x(c)" % P in ln][0]   # noqa: E731
    off = _gen("v5s4y3")
    on = _gen("v5s4y3x")
    assert "c->r[4].d[0] =" in call(off)
    assert "c->r[4].w[0] =" in call(on) and "c->r[4].d[0]" not in call(on)
    for a in (G, F, H, K, P):
        assert _body(off, a) == _body(on, a)
