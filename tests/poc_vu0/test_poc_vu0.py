"""VU0 macro mode (COP2) bit-exact against PCSX2: R5900 test functions (GNU as -march=r5900, COP2 as .word) run in
PCSX2 v2.6.3 with the CPU settings of a GameDB eeClampMode-2 game (run_oracle.py: EE recompiler = microVU0 macro
mode, and EE interpreter) and, translated by ps2rc's C back-end (variants v0-v4), on the host (gcc, round-toward-zero
+ flush-to-zero).

Acceptance (vu0_compare.check):
  * host == PCSX2 interpreter on every vector (ordinary and exponent-255 "edge") of the arithmetic tests;
  * host == PCSX2 recompiler on every ordinary vector (the recompiler clamps only some operands of exponent 255 and
    can produce Inf/NaN there - documented deviation, counted);
  * ref 'rec' tests (control registers, VRNEXT to VF00): host == recompiler on every vector - the interpreter keeps
    VI01-VI15 as 32 bits, clears FBRST after a reset and does not advance R for VRNEXT VF00 (PCSX2 interpreter quirks).
Mutation checks: a broken runtime (fused MADD, missing operand clamp, wrong LFSR tap, sign-extending CFC2, broken
PMAXH - on a private copy of src/runtime) and a broken clean-lane analysis (LQC2 results treated as clean, v3) must
each make the comparison fail.

The PCSX2 results are cached in build/poc_vu0 and re-run only when the guest ELF changed (its SHA-1 is recorded).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.environ.get("PS2RC_VU0_OUT") or os.path.join(ROOT, "build", "poc_vu0")   # gen_vu0.py: PS2RC_VU0_EXTRA
sys.path.insert(0, HERE)
import vu0_build_rc  # noqa: E402
import vu0_compare  # noqa: E402

pytestmark = [pytest.mark.wsl, pytest.mark.oracle]


def _wsl_path(p):
    return config.posix_path(p)


def _wsl(script, *args):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    p = subprocess.run(config.bash_argv(script, *args), env=env,
                       capture_output=True)
    assert p.returncode == 0, (p.stdout + p.stderr).decode(errors="replace")[-3000:]
    return p.stdout.decode(errors="replace")


def _sha1(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest()


@pytest.fixture(scope="module")
def oracle():
    """Guest program + PCSX2 results, (re)built when the generator changed or the results are stale."""
    srcs = [os.path.join(HERE, f) for f in ("gen_vu0.py", "vu0enc.py", "guest/start.S", "guest/vu0.ld")]
    srcs += [f for f in os.environ.get("PS2RC_VU0_EXTRA", "").split(os.pathsep) if f]
    elf = os.path.join(OUT, "vu0.elf")
    if not os.path.exists(elf) or os.path.getmtime(elf) < max(os.path.getmtime(s) for s in srcs):
        subprocess.run([sys.executable, os.path.join(HERE, "gen_vu0.py")], check=True, capture_output=True)
        _wsl(os.path.join(HERE, "guest", "build.sh"), _wsl_path(OUT))
    runs = os.path.join(OUT, "pcsx2_runs.json")
    fresh = os.path.exists(runs) and json.load(open(runs, encoding="utf-8")).get("elf_sha1") == _sha1(elf)
    if not fresh or not all(os.path.exists(os.path.join(OUT, "pcsx2_%s.bin" % m)) for m in ("rec", "interp")):
        p = subprocess.run([sys.executable, os.path.join(HERE, "run_oracle.py")], capture_output=True, text=True)
        assert p.returncode == 0, p.stdout[-2000:] + p.stderr[-3000:]
    report = json.load(open(runs, encoding="utf-8"))
    assert report["rec"]["cpu_mode"] == "recompiler" and report["interp"]["cpu_mode"] == "interpreter"
    assert report["rec"]["tlb_misses"] == 0 and report["interp"]["tlb_misses"] == 0
    return report


def _host_run(variant, tag, runtime=None):
    rc = os.path.join(OUT, "rc", tag)
    vu0_build_rc.translate(variant, rc)
    res = os.path.join(OUT, "host_%s.bin" % tag)
    args = [_wsl_path(rc), _wsl_path(res)] + ([_wsl_path(runtime)] if runtime else [])
    _wsl(os.path.join(HERE, "host", "build_run.sh"), *args)
    return res


@pytest.mark.parametrize("variant", ["v0", "v1", "v2", "v3", "v4"])
def test_host_vu0_matches_pcsx2(oracle, variant):
    fails, text = vu0_compare.check(_host_run(variant, variant))
    assert not fails, "%s\n%s" % (fails, text)


MUTATIONS = [   # (name, file, old, new, tests that must turn red)
    ("fused_madd", "r5900_rt.h", "r = op == VU0_MADD ? z + p : z - p;",
     "r = op == VU0_MADD ? __builtin_fmaf(x, y, z) : z - p;", {"madd_fd"}),
    ("no_fs_clamp", "r5900_rt.h", "float x = u2f((cl & 1) ? s : vu0_clamp(s))", "float x = u2f(s)", {"mul_fd", "add_fd"}),
    ("lfsr_tap", "r5900_rt.h", "((r >> 22) & 1)) & 0x7FFFFFu)", "((r >> 21) & 1)) & 0x7FFFFFu)", {"rreg"}),
    ("cfc2_sext", "r5900_rt.h", "if (n < 16) return c->vu0.vi[n] & 0xFFFFu;",
     "if (n < 16) return (uint64_t)(int64_t)(int16_t)c->vu0.vi[n];", {"ctrl"}),
    ("pmaxh", "r5900_rt.h", "if (b.sh[i] >= a.sh[i]) a.sh[i] = b.sh[i];", "if (b.sh[i] > a.sh[i] + 1) a.sh[i] = b.sh[i];",
     {"mmi_h1"}),
]


@pytest.mark.parametrize("name,fname,old,new,red", MUTATIONS, ids=[m[0] for m in MUTATIONS])
def test_mutated_runtime_is_caught(oracle, name, fname, old, new, red):
    """A private copy of src/runtime with one semantic change (the shared header is never touched)."""
    rt = os.path.join(OUT, "mut", name)
    shutil.rmtree(rt, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "runtime"), rt)
    path = os.path.join(rt, fname)
    src = open(path, encoding="utf-8").read()
    assert src.count(old) == 1, "mutation target not unique: %r" % old
    open(path, "w", encoding="utf-8", newline="\n").write(src.replace(old, new))
    fails, text = vu0_compare.check(_host_run("v0", "mut_" + name, runtime=rt))
    assert red <= set(fails), "mutation %s survived in %s\n%s" % (name, sorted(red - set(fails)), text)


def test_mutated_clean_analysis_is_caught(oracle, monkeypatch):
    """v3 with LQC2 results wrongly treated as clean (no input clamp on loaded denormal/exponent-255 lanes)."""
    from ps2rc.vu import macro
    orig = macro.transfer

    def transfer(insn, st):
        if insn.mnem == "lqc2" and insn.ops[0][1]:
            return st | 0xF << 4 * insn.ops[0][1]
        return orig(insn, st)
    monkeypatch.setattr(macro, "transfer", transfer)
    fails, text = vu0_compare.check(_host_run("v3", "mut_clean"))
    assert {"mul_fd"} <= set(fails), text
