"""PCSX2 execution oracle - smoke test.

Builds tests/oracle/smoke/smoke.S (R5900 assembly, WSL pspdev binutils) unless build/oracle/smoke/smoke.elf
is up to date, boots it in the private portable PCSX2 (ps2rc.oracle.Pcsx2Runner) once with the EE
recompiler and once with the EE interpreter, reads the result block back through PINE and compares it
with values modelled *here* from the R5900 semantics (never taken from the ELF or from PCSX2).

    python -m pytest -m oracle tests/oracle -v
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

import pytest
from ps2rc import config  # noqa: E402

from ps2rc.oracle import Pcsx2Runner
from ps2rc.oracle.pcsx2 import find_processes

pytestmark = pytest.mark.oracle

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SMOKE_SRC = HERE / "smoke"
OUT_DIR = REPO / "build" / "oracle" / "smoke"
ELF = OUT_DIR / "smoke.elf"
SYMS = OUT_DIR / "smoke.json"

DONE_MAGIC = 0x600DF00D
SENTINEL = 0xA5A5A5A5A5A5A5A5
M32 = (1 << 32) - 1
M64 = (1 << 64) - 1


# ------------------------------------------------------------------------------------------------
# independent model of the few R5900 operations smoke.S uses (64-bit GPRs held as unsigned ints)
# ------------------------------------------------------------------------------------------------
def sext32(x: int) -> int:
    """32-bit value -> sign-extended 64-bit register value."""
    x &= M32
    return (x | ~M32) & M64 if x & 0x80000000 else x


def s64(x: int) -> int:
    return x - (1 << 64) if x & (1 << 63) else x


def lui(imm16: int) -> int:
    return sext32(imm16 << 16)


def ori(rs: int, imm16: int) -> int:
    return rs | (imm16 & 0xFFFF)


def addiu(rs: int, imm16: int) -> int:
    simm = imm16 - 0x10000 if imm16 & 0x8000 else imm16
    return sext32(rs + simm)


def addu(rs: int, rt: int) -> int:
    return sext32(rs + rt)


def daddu(rs: int, rt: int) -> int:
    return (rs + rt) & M64


def dsll32(rt: int, sa: int) -> int:
    return (rt << (sa + 32)) & M64


def dsra32(rt: int, sa: int) -> int:
    return (s64(rt) >> (sa + 32)) & M64


def mult(rs: int, rt: int):
    """R5900 3-operand MULT: returns (rd, lo, hi), each a sign-extended 32-bit half of the product."""
    a = rs & M32
    b = rt & M32
    a = a - (1 << 32) if a & 0x80000000 else a
    b = b - (1 << 32) if b & 0x80000000 else b
    p = (a * b) & M64
    lo, hi = sext32(p), sext32(p >> 32)
    return lo, lo, hi


def multu(rs: int, rt: int):
    p = (rs & M32) * (rt & M32)
    lo, hi = sext32(p), sext32(p >> 32)
    return lo, lo, hi


def paddw(a_words, b_words):
    return [(x + y) & M32 for x, y in zip(a_words, b_words)]


def q(v: int) -> bytes:
    return struct.pack("<Q", v & M64)


def expected_results() -> dict[str, bytes]:
    """The `results` slots of smoke.S, recomputed from its inputs with the model above."""
    exp: dict[str, bytes] = {}
    # 1. ADDU 0x7FFFFFFF + 1
    exp["res_addu"] = q(addu(ori(lui(0x7FFF), 0xFFFF), addiu(0, 1)))
    # 2. DSLL32 + DADDU with a LUI-sign-extended operand
    a = dsll32(ori(lui(0x1234), 0x5678), 0)
    b = ori(lui(0x9ABC), 0xDEF0)
    t2 = daddu(a, b)
    exp["res_daddu"] = q(t2)
    # 3. B = 0xFEDCBA9876543210, DSRA32 by 4
    big = daddu(dsll32(ori(lui(0xFEDC), 0xBA98), 0), ori(lui(0x7654), 0x3210))
    exp["res_dsra32"] = q(dsra32(big, 4))
    # 4. MULT 0x10000 * 0x8000
    rd, lo, hi = mult(lui(0x0001), ori(0, 0x8000))
    exp["res_mult_rd"], exp["res_mult_lo"], exp["res_mult_hi"] = q(rd), q(lo), q(hi)
    # 5. MULT -7 * 0x12345678
    rd, _lo, hi = mult(addiu(0, 0xFFF9), ori(lui(0x1234), 0x5678))
    exp["res_mult2_rd"], exp["res_mult2_hi"] = q(rd), q(hi)
    # 6. MULTU 0xFFFFFFFF * 0xFFFFFFFF
    rd, _lo, hi = multu(addiu(0, 0xFFFF), addiu(0, 0xFFFF))
    exp["res_multu_rd"], exp["res_multu_hi"] = q(rd), q(hi)
    # 7. PCPYLD rd, t2, B: hi64 = t2, lo64 = B  (memory order: lo64 first)
    exp["res_pcpyld"] = q(big) + q(t2)
    # 8. LQ/PADDW/SQ on the two input vectors
    vec_a = [0x00000001, 0x00000002, 0x00000003, 0x7FFFFFFF]
    vec_b = [0x00000010, 0x00000020, 0x00000030, 0x00000001]
    exp["res_paddw"] = struct.pack("<4I", *paddw(vec_a, vec_b))
    return exp


def test_model_sanity():
    """A few hand-checked values, so a broken model cannot hide behind a matching broken emulator."""
    exp = expected_results()
    assert exp["res_addu"] == q(0xFFFFFFFF80000000)
    assert exp["res_daddu"] == q(0x123456779ABCDEF0)
    assert exp["res_mult_hi"] == q(0)
    assert exp["res_multu_hi"] == q(0xFFFFFFFFFFFFFFFE)
    assert exp["res_paddw"][12:16] == struct.pack("<I", 0x80000000)


# ------------------------------------------------------------------------------------------------
# fixtures
# ------------------------------------------------------------------------------------------------
def _wsl_path(p: Path) -> str:
    return config.posix_path(str(Path(p).resolve()))


def _build_is_stale() -> bool:
    if not (ELF.is_file() and SYMS.is_file()):
        return True
    built = min(ELF.stat().st_mtime, SYMS.stat().st_mtime)
    return any(f.stat().st_mtime > built for f in SMOKE_SRC.iterdir() if f.is_file())


@pytest.fixture(scope="module")
def smoke_syms() -> dict[str, int]:
    if _build_is_stale():
        if not config.have_bash():
            pytest.skip("smoke.elf missing/stale and no bash (WSL on Windows) to build it")
        proc = subprocess.run(config.bash_argv(str(SMOKE_SRC / "build.sh"), _wsl_path(OUT_DIR)),
                              capture_output=True, timeout=300)
        if proc.returncode != 0 or not SYMS.is_file():
            pytest.fail("smoke build failed:\n" + proc.stdout.decode("utf-8", "replace")
                        + proc.stderr.decode("utf-8", "replace"))
    symbols = json.loads(SYMS.read_text())["symbols"]
    return {name: int(info["addr"], 16) for name, info in symbols.items()}


@pytest.fixture(scope="module")
def runner() -> Pcsx2Runner:
    r = Pcsx2Runner()
    if not (r.exe.is_file() or (r.source_install / "pcsx2-qt.exe").is_file()):
        pytest.skip(f"PCSX2 not available ({r.exe})")
    if not (r.bios_dir / r.bios_name).is_file():
        pytest.skip(f"BIOS not available ({r.bios_dir / r.bios_name})")
    return r


def _dir_fingerprint(path: Path):
    out = []
    for f in sorted(path.iterdir()):
        st = f.stat()
        digest = hashlib.sha1(f.read_bytes()).hexdigest() if f.is_file() else "<dir>"
        out.append((f.name, st.st_size, st.st_mtime_ns, digest))
    return out


@pytest.fixture(scope="module")
def bios_dir_untouched(runner):
    """The user's BIOS folder is read-only for us: it must be bit-for-bit unchanged afterwards."""
    before = _dir_fingerprint(runner.bios_dir)
    yield
    assert _dir_fingerprint(runner.bios_dir) == before, f"{runner.bios_dir} was modified!"


# ------------------------------------------------------------------------------------------------
# the oracle runs
# ------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("ee_interpreter", [False, True], ids=["ee-rec", "ee-int"])
def test_smoke_in_pcsx2(runner, smoke_syms, bios_dir_untouched, ee_interpreter):
    mode = "int" if ee_interpreter else "rec"
    base = smoke_syms["results"]
    # read results + vec_a/vec_b + done + stage in one block
    span = smoke_syms["stage"] + 4 - base
    res = runner.run_elf(ELF, smoke_syms["done"], DONE_MAGIC, timeout_s=90, ee_interpreter=ee_interpreter,
                         reads={"all": (base, span)}, log_path=OUT_DIR / f"smoke.pcsx2-{mode}.log")
    t = res["timings"]
    summary = {k: round(v, 3) for k, v in t.items()} | {"cpu_mode": res["cpu_mode"],
                                                       "shutdown_mode": res["shutdown"],
                                                       "pcsx2": res["pcsx2_version"]}
    (OUT_DIR / f"smoke.timings-{mode}.json").write_text(json.dumps(summary, indent=2))
    print(f"\n[{mode}] " + ", ".join(f"{k}={v}" for k, v in summary.items()))

    assert res["cpu_mode"] == ("interpreter" if ee_interpreter else "recompiler")
    assert res["tlb_misses"] == 0, f"TLB misses in {res['log_path']}"
    assert not find_processes(runner.exe), "a PCSX2 process survived the run"

    blob = res["blocks"]["all"]

    def at(sym: str, n: int) -> bytes:
        off = smoke_syms[sym] - base
        return blob[off:off + n]

    assert struct.unpack("<I", at("done", 4))[0] == DONE_MAGIC
    assert struct.unpack("<I", at("stage", 4))[0] == 2

    mismatches = []
    for name, want in expected_results().items():
        got = at(name, len(want))
        if got != want:
            sentinel = got == struct.pack("<Q", SENTINEL) * (len(want) // 8)
            mismatches.append(f"{name}: got {got.hex()} want {want.hex()}" + (" (never stored)" if sentinel else ""))
    assert not mismatches, f"EE {res['cpu_mode']} mismatches (log: {res['log_path']}):\n" + "\n".join(mismatches)
