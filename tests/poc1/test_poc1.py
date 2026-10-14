"""PoC-1 end-to-end: R5900 test functions (built for the PS2 with GNU as -march=r5900) -> ps2rc decode + function
discovery + C back-end -> host gcc / PSP gcc -> results must equal PCSX2's (EE interpreter) bit for bit, except the
documented deviation below.

Documented deviation from PCSX2: the shift-amount register. ps2rc keeps a bit count (MTSAB: byte count * 8); PCSX2
stores the byte count and multiplies inside QFSRV. QFSRV results agree; MFSA returns 8x PCSX2's value in ps2rc.
"""
import json
import os
import struct
import subprocess

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "build", "poc1")


def _wsl(script):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    subprocess.run(config.bash_argv(script), check=True, env=env, capture_output=True)


def _rows(path):
    return open(path, "rb").read()


def qfsrv_rule(ours, ref):
    """mmi_qfsrv output: bytes 0..15 = QFSRV result (must match); 16..31 = pcpyld(mfsa, mfsa)."""
    if ours[:16] != ref[:16]:
        return False
    a = struct.unpack("<2Q", ours[16:])
    b = struct.unpack("<2Q", ref[16:])
    return a[0] == a[1] and b[0] == b[1] and a[0] == b[0] * 8


def check(results_path, reference_path):
    meta = json.load(open(os.path.join(OUT, "tests.json")))
    a, b = _rows(results_path), _rows(reference_path)
    n = meta["tests"][0]["nvec"]
    bad = {}
    for k, t in enumerate(meta["tests"]):
        cnt = 0
        for v in range(n):
            o = (k * n + v) * 32
            x, y = a[o:o + 32], b[o:o + 32]
            if x == y:
                continue
            if t["name"] == "mmi_qfsrv" and qfsrv_rule(x, y):
                continue
            cnt += 1
        if cnt:
            bad[t["name"]] = cnt
    return bad


@pytest.mark.wsl
@pytest.mark.oracle
@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4", "v5s1", "v5s2", "v5s3", "v5s4"])
def test_host_recompiled_code_matches_pcsx2_interpreter(variant):
    ref = os.path.join(OUT, "pcsx2_interp.bin")
    if not os.path.exists(ref):
        pytest.skip("run tests/poc1/run_oracle.py first")
    subprocess.run(["python", os.path.join(HERE, "build_rc.py")] + ([] if variant == "v0" else ["--" + variant]),
                   check=True, capture_output=True)
    _wsl(os.path.join(HERE, "host", "build_run.sh"))
    bad = check(os.path.join(OUT, "host_results.bin"), ref)
    assert not bad, "mismatching vectors per test: %s" % bad
