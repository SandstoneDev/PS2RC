"""prepare_gen --sites: a call inside a translated function, found by its return address, calls an HLE handler."""
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREP = os.path.join(REPO, "ps2rc", "pipeline", "prepare_gen.py")

UNIT = """#include "r5900_rt.h"
void fn_08800000(R5900Ctx *c)
{
    int32_t r4 = (int32_t)c->r[4].d[0], r25 = (int32_t)c->r[25].d[0], r31 = (int32_t)c->r[31].d[0];
    r31 = (int32_t)(0x08800010ull); c->r[4].d[0] = (uint64_t)(int64_t)r4; fn_08900000(c); r4 = (int32_t)c->r[2].d[0];
    { uint32_t t_ = (uint32_t)((uint64_t)(int64_t)r25); r31 = (int32_t)(0x08800020ull); rc_dispatch(c, t_); }
    r31 = (int32_t)(0x08800030ull); fn_08900000(c);
}
void fn_08900000(R5900Ctx *c)
{
    r31 = (int32_t)(0x08800010ull); fn_08a00000(c);
}
"""


def _run(tmp, sites, extra=()):
    gen = os.path.join(tmp, "gen")
    out = os.path.join(tmp, "out")
    os.makedirs(gen, exist_ok=True)
    with open(os.path.join(gen, "main_000.c"), "w", newline="\n") as f:
        f.write(UNIT)
    hooks = os.path.join(tmp, "hooks.def")
    open(hooks, "w").write("/* none */\n")
    sp = os.path.join(tmp, "sites.def")
    open(sp, "w").write(sites)
    r = subprocess.run([sys.executable, PREP, gen, hooks, out, "--sites", sp] + list(extra), capture_output=True,
                       text=True)
    text = open(os.path.join(out, "main_000.c")).read() if r.returncode == 0 else ""
    return r, text


def test_direct_and_indirect_sites(tmp_path):
    r, text = _run(str(tmp_path), "/* x */\nSITE(08800000, 08800010, h_direct)\nSITE(08800000, 08800020, h_ind)\n")
    assert r.returncode == 0, r.stderr
    assert "h_direct(c, 0x08900000u); r4 = (int32_t)c->r[2].d[0];" in text
    assert "r31 = (int32_t)(0x08800020ull); h_ind(c, t_); }" in text
    assert "r31 = (int32_t)(0x08800030ull); fn_08900000(c);" in text          # other call sites unchanged
    assert "void h_direct(R5900Ctx *c, uint32_t target);" in text
    assert "    r31 = (int32_t)(0x08800010ull); fn_08a00000(c);" in text       # same return address, other function
    assert "2 call sites" in r.stdout


def test_missing_site_fails(tmp_path):
    r, _ = _run(str(tmp_path), "SITE(08800000, 08800040, h_x)\n")
    assert r.returncode != 0
    assert "did not apply exactly once" in (r.stderr + r.stdout)
