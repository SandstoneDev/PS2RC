"""Back-end v5, lever "clampmin2": the branchless FPU input clamp f_in5 of runtime/r5900_rt.h must equal
the reference clamp f_in (exponent 0 -> +-0, exponent 255 -> +-0x7F7FFFFF, anything else unchanged) for every one of
the 2^32 bit patterns - checked exhaustively in C on the host (WSL gcc), the mutants below must be caught by the same
program, and psp-gcc must compile f_in5 without a branch (the point of the lever)."""
import os
import re
import subprocess

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_v5_clamp")
HEADER = os.path.join(ROOT, "runtime", "r5900_rt.h")

PROGRAM = r"""
#include <stdio.h>
#include "r5900_rt.h"
int main(void)
{
    unsigned long long bad = 0, n = 0;
    uint32_t x = 0;
    do {
        uint32_t a = f_in(x), b = f_in5(x);
        if (a != b) { if (bad < 4) printf("x=%08x f_in=%08x f_in5=%08x\n", x, a, b); bad++; }
        n++;
    } while (++x != 0);
    printf("checked=%llu bad=%llu\n", n, bad);
    return 0;
}
"""
# mutants of f_in5's body (source text replacements) - each must give bad > 0
MUTANTS = {
    "min_le": ("a < (int32_t)F_FMAX ? a : (int32_t)F_FMAX", "a <= (int32_t)F_FMAX - 1 ? a : (int32_t)F_FMAX - 1"),
    "no_exp0": ("m = ((x >> 23) & 0xFFu) ? m : 0;", ";"),
    "exp_mask": ("((x >> 23) & 0xFFu)", "((x >> 24) & 0x7Fu)"),
    "sign_lost": ("return x + (uint32_t)(m - a);", "return (uint32_t)m;"),
}


def _wsl_path(p):
    return config.posix_path(p)


def _run(tag, header_text):
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "r5900_rt.h"), "w", newline="\n", encoding="utf-8").write(header_text)
    open(os.path.join(d, "main.c"), "w", newline="\n", encoding="ascii").write(PROGRAM)
    script = os.path.join(d, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "set -e\ncd %s\ngcc -O2 -std=gnu11 -w -I. main.c -o clamp\n./clamp\n" % _wsl_path(d))
    p = subprocess.run(config.bash_argv(script),
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-2000:]
    m = re.search(r"checked=(\d+) bad=(\d+)", p.stdout.decode())
    assert m, p.stdout.decode()
    return int(m.group(1)), int(m.group(2)), p.stdout.decode()


@pytest.mark.wsl
def test_f_in5_equals_f_in_for_all_2_32_inputs():
    checked, bad, out = _run("exhaustive", open(HEADER, encoding="utf-8").read())
    assert checked == 1 << 32 and bad == 0, out


@pytest.mark.wsl
@pytest.mark.parametrize("mutant", sorted(MUTANTS))
def test_mutated_clamp_is_caught(mutant):
    text = open(HEADER, encoding="utf-8").read()
    body = text[text.index("static inline uint32_t f_in5("):]
    body = body[:body.index("\n}\n") + 3]
    old, new = MUTANTS[mutant]
    assert old in body
    checked, bad, _out = _run("mut_" + mutant, text.replace(body, body.replace(old, new)))
    assert checked == 1 << 32 and bad > 0


@pytest.mark.wsl
def test_psp_gcc_compiles_f_in5_without_a_branch():
    d = os.path.join(OUT, "psp")
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "probe.c"), "w", newline="\n", encoding="ascii").write(
        '#include "r5900_rt.h"\nuint32_t probe(uint32_t x) { return f_in5(x); }\n'
        'float probe_mul(uint32_t a, uint32_t b) { return F5(a) * F5(b); }\n')
    script = os.path.join(d, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write(
        "set -e\nexport PATH=$HOME/pspdev/bin:$PATH\ncd %s\npsp-gcc -O2 -G0 -std=gnu11 -w -fno-fast-math "
        "-fno-math-errno -frounding-math -ffp-contract=off -I%s -c probe.c -o probe.o\npsp-objdump -d probe.o\n"
        % (_wsl_path(d), _wsl_path(os.path.join(ROOT, "runtime"))))
    p = subprocess.run(config.bash_argv(script),
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-2000:]
    dis = p.stdout.decode()
    for fn in ("probe", "probe_mul"):
        body = dis.split("<%s>:" % fn)[1].split("\n\n")[0]
        insns = [ln.split("\t")[2].split()[0] for ln in body.splitlines() if ln.count("\t") >= 2]
        branches = [m for m in insns if m.startswith("b") or m in ("j", "jal")]
        assert insns and not branches, (fn, insns)
        if fn == "probe":
            assert "min" in insns and len(insns) <= 10, insns          # ext/and/min/movz/subu/addu + jr ra
