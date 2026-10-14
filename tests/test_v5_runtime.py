"""Runtime additions of back-end v5 (runtime/r5900_rt.h / r5900_rt.c), checked on the host (gcc):

  * rl_mult/rl_multu/rl_div/rl_divu/rl_madd/rl_maddu (back-end v5 lever "hilo": HI/LO in C locals) give exactly the
    results of the context versions rt_* for both pipelines, on edge and random operands;
  * the v5 FPU helpers fpu_sqrt5/fpu_rsqrt5/fpu_madda5/fpu_msuba5 equal fpu_sqrt/fpu_rsqrt/fpu_madda/fpu_msuba (they
    only clamp through f_in5, which equals f_in - tests/test_v5_clamp.py);
  * rt_cs_save/rt_cs_restore keep exactly s0-s7, fp (128 bits) and f20-f31;
  * rt_vu0_callms is a weak symbol: a strong definition (a pack's VU0 micro HLE) replaces it at link time, without
    one the default ends in rt_fatal("vcallms").
"""
import os
import subprocess

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "build", "unit_v5_runtime")
RT = os.path.join(ROOT, "runtime")

EQUIV = r"""
#include <stdio.h>
#include <fenv.h>
#include "r5900_rt.h"
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value) { (void)c; (void)what; (void)pc; (void)value; }
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static const uint64_t edge[] = {0, 1, 2, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFF, 0xFFFFFFFF80000000ull, 0x80000000u,
                                0xFFFFFFFFull, 0x123456789ABCDEF0ull, 0xFFFFFFFF00000000ull, 7, 0xFFFFFFFFFFFFFFF9ull};
static const uint32_t fedge[] = {0, 0x80000000u, 1, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x3F800000u, 0xBF800000u,
                                 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FFFFFFFu,
                                 0x40490FDBu, 0x1E3CE508u, 0x5F000000u};
int main(void)
{
    fesetround(FE_TOWARDZERO);
    unsigned long long n = 0, bad = 0;
    static R5900Ctx c;
    for (int k = 0; k < 400000; k++) {
        uint64_t a = k < 144 ? edge[k % 12] : rnd(), b = k < 144 ? edge[k / 12] : rnd();
        if (k >= 144 && (k & 3) == 0) b = (uint64_t)(int64_t)(int32_t)(rnd() & 0xFF) - 128;   /* small divisors */
        uint64_t lo0 = k < 144 ? edge[(k * 7) % 12] : rnd(), hi0 = k < 144 ? edge[(k * 5) % 12] : rnd();
        for (int op = 0; op < 6; op++) {
            for (int p = 0; p < 2; p++) {
                c.lo.d[p] = lo0; c.hi.d[p] = hi0;
                uint64_t lo = lo0, hi = hi0;
                switch (op) {
                case 0: rt_mult(&c, p, (int64_t)a, (int64_t)b); rl_mult(&lo, &hi, (int64_t)a, (int64_t)b); break;
                case 1: rt_multu(&c, p, a, b); rl_multu(&lo, &hi, a, b); break;
                case 2: rt_div(&c, p, a, b); rl_div(&lo, &hi, a, b); break;
                case 3: rt_divu(&c, p, a, b); rl_divu(&lo, &hi, a, b); break;
                case 4: rt_madd(&c, p, a, b); rl_madd(&lo, &hi, a, b); break;
                case 5: rt_maddu(&c, p, a, b); rl_maddu(&lo, &hi, a, b); break;
                }
                n++;
                if (lo != c.lo.d[p] || hi != c.hi.d[p]) {
                    if (bad < 5) printf("op %d p %d a %llx b %llx\n", op, p, (unsigned long long)a, (unsigned long long)b);
                    bad++;
                }
            }
        }
    }
    printf("hilo checked=%llu bad=%llu\n", n, bad);
    n = bad = 0;
    for (int k = 0; k < 600000; k++) {
        uint32_t x = k < 289 ? fedge[k % 17] : (uint32_t)rnd(), y = k < 289 ? fedge[k / 17] : (uint32_t)rnd();
        uint32_t z = (uint32_t)rnd() & 0x7FFFFFFF;
        n++;
        if (fpu_sqrt5(x) != fpu_sqrt(x) || fpu_rsqrt5(x, y) != fpu_rsqrt(x, y) || fpu_madda5(z, x, y) != fpu_madda(z, x, y)
            || fpu_msuba5(z, x, y) != fpu_msuba(z, x, y)) {
            if (bad < 5) printf("fpu x %08x y %08x\n", x, y);
            bad++;
        }
    }
    printf("fpu checked=%llu bad=%llu\n", n, bad);
    for (int r = 0; r < 32; r++) { c.r[r].d[0] = 0x1111111100000000ull + r; c.r[r].d[1] = 0x2222222200000000ull + r; c.fpr[r].u = 0x3F000000u + r; }
    R5900CSave sv;
    rt_cs_save(&c, &sv);
    for (int r = 0; r < 32; r++) { c.r[r].d[0] = ~c.r[r].d[0]; c.r[r].d[1] = ~c.r[r].d[1]; c.fpr[r].u = ~c.fpr[r].u; }
    rt_cs_restore(&c, &sv);
    int kept = 0, other = 0;
    for (int r = 0; r < 32; r++) {
        int cs = (r >= 16 && r <= 23) || r == 30;
        int same = c.r[r].d[0] == 0x1111111100000000ull + r && c.r[r].d[1] == 0x2222222200000000ull + r;
        if (cs && same) kept++;
        if (!cs && same) other++;
        int fcs = r >= 20;
        int fsame = c.fpr[r].u == 0x3F000000u + r;
        if (fcs && fsame) kept++;
        if (!fcs && fsame) other++;
    }
    printf("cs kept=%d other=%d\n", kept, other);
    return 0;
}
"""

WEAK_MAIN = r"""
#include <stdio.h>
#include "r5900_rt.h"
void rt_fatal(R5900Ctx *c, const char *what, uint32_t pc, uint32_t value) { (void)c; printf("fatal %s %x %x\n", what, pc, value); }
int main(void) { static R5900Ctx c; rt_vu0_callms(&c, 0x1000, 0x20); return 0; }
"""
STRONG = r"""
#include <stdio.h>
#include "r5900_rt.h"
void rt_vu0_callms(R5900Ctx *c, uint32_t pc, uint32_t addr) { (void)c; printf("strong %x %x\n", pc, addr); }
"""


def _wsl_path(p):
    return config.posix_path(p)


def _run(tag, files, cmd):
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    for name, text in files.items():
        open(os.path.join(d, name), "w", newline="\n", encoding="ascii").write(text)
    script = os.path.join(d, "run.sh")
    open(script, "w", newline="\n", encoding="ascii").write("set -e\ncd %s\n%s\n" % (_wsl_path(d), cmd))
    p = subprocess.run(config.bash_argv(script),
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"), capture_output=True)
    assert p.returncode == 0, p.stderr.decode(errors="replace")[-2000:]
    return p.stdout.decode()


@pytest.mark.wsl
def test_local_hilo_fpu5_and_cs_save_helpers_equal_the_context_versions():
    out = _run("equiv", {"main.c": EQUIV},
               "gcc -O2 -std=gnu11 -w -frounding-math -ffp-contract=off -fno-fast-math -I%s main.c -lm -o equiv && "
               "./equiv" % _wsl_path(RT))
    assert "hilo checked=4800000 bad=0" in out, out
    assert "fpu checked=600000 bad=0" in out, out
    assert "cs kept=21 other=0" in out, out


@pytest.mark.wsl
def test_rt_vu0_callms_default_is_weak():
    rtc = _wsl_path(os.path.join(RT, "r5900_rt.c"))
    out = _run("weak", {"main.c": WEAK_MAIN, "strong.c": STRONG},
               "gcc -O2 -std=gnu11 -w -I%s main.c %s -o default && ./default && "
               "gcc -O2 -std=gnu11 -w -I%s main.c strong.c %s -o strong && ./strong" % (
                   _wsl_path(RT), rtc, _wsl_path(RT), rtc))
    assert out.splitlines() == ["fatal vcallms 1000 20", "strong 1000 20"], out
