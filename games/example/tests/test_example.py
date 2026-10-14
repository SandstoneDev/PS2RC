"""The example pack end to end: assemble -> translate -> host harness -> run, and the program's output equals a Python
model of it. Needs the pspdev toolchain and gcc (Linux; on Windows through WSL), marker `wsl`."""
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, ROOT)

TABLE = [0x9E3779B9, 0x7F4A7C15, 0x85EBCA6B, 0xC2B2AE35, 0x27D4EB2F, 0x165667B1, 0xD3A2646C, 0xFD7046C5,
         0xB55A4F09, 0x68E31DA4, 0x1B873593, 0xCC9E2D51, 0xE6546B64, 0x3C6EF372, 0xA54FF53A, 0x510E527F]


def expected_state_line(frames=60):
    """What src/example.S prints after `frames` frames (frame: state = state * 0x5851F42D + table[n & 15];
    acc += {n, state, n * n, ~n})."""
    m = 0xFFFFFFFF
    state, acc = 1, [0, 0, 0, 0]
    for n in range(frames):
        state = (state * 0x5851F42D + TABLE[n & 15]) & m
        acc = [(a + b) & m for a, b in zip(acc, (n, state, (n * n) & m, ~n & m))]
    return "PS2RC example: state %08x %s" % (state, " ".join("%08x" % a for a in acc))


def test_model_numbers():
    assert expected_state_line() == "PS2RC example: state 90afa006 000006ea c069b4d7 00011242 fffff8da"


def _bash(script, env):
    """Run a bash command line natively (Linux) or through WSL (Windows; paths passed with WSLENV /p)."""
    if os.name == "nt":
        if not shutil.which("wsl.exe"):
            pytest.skip("needs WSL")
        e = dict(env, WSLENV="PS2RC_WORK/p:BDIR/p:OUT/p:PS2RC_LOCAL/p", MSYS_NO_PATHCONV="1")
        root = "/mnt/" + ROOT[0].lower() + ROOT[2:].replace("\\", "/")
        return subprocess.run(["wsl.exe", "-d", "Debian", "--", "bash", "-c", "cd '%s' && %s" % (root, script)],
                              env=e, capture_output=True, text=True, timeout=1800)
    return subprocess.run(["bash", "-c", script], cwd=ROOT, env=env, capture_output=True, text=True, timeout=1800)


@pytest.mark.wsl
def test_example_end_to_end(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    local = tmp_path / "local.toml"
    local.write_text("")                                   # no machine settings: the example needs none
    env = dict(os.environ, PS2RC_WORK=str(work), PS2RC_LOCAL=str(local), BDIR=str(tmp_path / "bdir"),
               OUT=str(tmp_path / "runs"))
    r = _bash("bash games/example/build_elf.sh", env)
    assert r.returncode == 0 and "hooks.def matches" in r.stdout, r.stdout + r.stderr
    p = subprocess.run([sys.executable, os.path.join(ROOT, "ps2rc", "pipeline", "translate.py"), "--pack", "example"],
                       env=env, capture_output=True, text=True, timeout=600)
    assert p.returncode == 0 and "9 translated" in p.stdout, p.stdout + p.stderr
    r = _bash("bash mk/build.sh --pack example host 4", env)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    r = _bash("bash mk/run_host.sh --pack example 60 0 example --mainloop-limit 1000", env)
    assert r.returncode == 0, r.stdout + r.stderr
    log = (tmp_path / "runs" / "host_example.log").read_text(encoding="utf-8", errors="replace")
    assert "TTY: PS2RC example: hello from a PS2 program translated to C" in log
    assert "TTY: " + expected_state_line() in log
    assert "guest Exit(0)" in log and "main-loop iterations (0x100E00 calls): 60" in log
    assert "alias check: 0 plain-accessor sites" in log and "code-region hole check: 0 words changed" in log
