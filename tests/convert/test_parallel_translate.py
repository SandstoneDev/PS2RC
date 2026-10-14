"""The parallel translation (ps2rc.pipeline.parallel) gives the same C text as the serial loops: a synthetic
program of the back-end tests, emitted serially and from a process pool in small chunks, plain and in ABI mode with
narrowing plans computed both ways."""
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from ps2rc import backend_c  # noqa: E402
from ps2rc.analysis.functions import discover_functions  # noqa: E402
from ps2rc.analysis.width import analyse_program, narrowable  # noqa: E402
from ps2rc.pipeline import parallel  # noqa: E402

tb = pytest.importorskip("test_backend_c")


def _emit(img, fs, **opts):
    return backend_c.emit_units(img, fs, "unit", roots=None, per_unit=4, **opts)


@pytest.mark.parametrize("abi", [False, True])
def test_parallel_emit_equals_serial(monkeypatch, abi):
    img = tb.image()
    fs = discover_functions(img, tb.CODE, extra_seeds=tb.SEEDS)
    opts = {}
    if abi:
        _res, livein = analyse_program(img, fs.functions)
        starts = set(fs.functions)
        plan = {a: narrowable(img, fn, starts, livein, abi=True) for a, fn in sorted(fs.functions.items())}
        opts = dict(cache=True, narrow=plan, fpu_fast=True, abi=True, livein=livein, v5=backend_c.V5_STEPS["v5s4y2"])
    monkeypatch.setenv("PS2RC_TRANSLATE_JOBS", "1")
    serial = _emit(img, fs, **opts)
    monkeypatch.setenv("PS2RC_TRANSLATE_JOBS", "2")
    monkeypatch.setattr(parallel, "CHUNK", 2)              # 14 functions -> 7 chunks over 2 processes
    par = _emit(img, fs, **opts)
    assert par[0] == serial[0] and par[1] == serial[1] and par[2] == serial[2]


def test_parallel_narrowing_equals_serial(monkeypatch):
    img = tb.image()
    fs = discover_functions(img, tb.CODE, extra_seeds=tb.SEEDS)
    _res, livein = analyse_program(img, fs.functions)
    starts = set(fs.functions)
    wa, wr = {0x1100: 1}, frozenset({0x1300})
    serial = {a: narrowable(img, fn, starts, livein, wa, wr, abi=a != 0x1400) for a, fn in sorted(fs.functions.items())}
    monkeypatch.setattr(parallel, "CHUNK", 3)
    par = parallel.narrow_all(img, fs.functions, starts, livein, wa, wr, {0x1400}, 2)
    assert par == serial and list(par) == list(serial)


def test_parallel_fixpoints_equal_serial(monkeypatch):
    """argument_livein and wide_sets (ps2rc.pipeline.translate): Jacobi rounds split over a team of processes give
    the same sets (and the same insertion order) as the serial rounds."""
    from ps2rc.pipeline import translate as tr
    img = tb.image()
    fs = discover_functions(img, tb.CODE, extra_seeds=tb.SEEDS)
    monkeypatch.setattr(parallel, "CHUNK", 2)
    out = {}
    for jobs in ("1", "3"):
        monkeypatch.setenv("PS2RC_TRANSLATE_JOBS", jobs)
        tr.SYSCALL_SEEN["n"] = 0
        livein = tr.argument_livein(img, fs.functions)
        wa, wr, rounds = tr.wide_sets(img, fs.functions, livein, {min(fs.functions)}, set())
        out[jobs] = (list(livein.items()), list(wa), list(wr), rounds, tr.SYSCALL_SEEN["n"])
    assert out["3"] == out["1"]
