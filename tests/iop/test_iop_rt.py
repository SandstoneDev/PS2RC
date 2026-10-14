"""The IOP runtime's kernel / library HLE (ps2rc/iop/rt) seen from a module written in C (iop_kernel_test.c):
priority preemption, FIFO order, event flags, SIF RPC (reply before lower-priority threads run; time passes while the
EE waits), hardware timers, libsd register mapping + IRQ / DMA handlers, sprintf, sysmem, sleep/wakeup, semaphore
limits. Synthetic only."""
import os
import shutil

import pytest

from iop_testlib import wsl_run, gcc_line

HERE = os.path.dirname(os.path.abspath(__file__))

EXPECTED = {
    "K1": "0 H:wait L:sig H:got L:end",
    "K2": "A:wait B:wait A:got B:got",
    "K3": "after1 E:got r=0 bits=7",
    "K4": "0 105 1 R:fno5 W:ran buf=238",
    "K5": "0 102 -1 R:fno2 W:ran",
    "K6": "n=1 act=1 ns=1000000 act=0 T:77 T:77",
    "K7": "n=8 ram=abcdefgh sd=[P29.3=1eff KON1=5 KOFF0=800001 A3.1=5000 IRQ1230,0 IRQ1230,1 DMA7000+8] "
          "D:ch1 data42 I:core1 Q:woke",
    "K8": "19 [ab--7-beef-z-    q|] low=1 high=1 reuse=1 S:sleep S:awake poll=0,-419 sig=0,-420",
    "K9": "unresolved=0 unimpl=0",
}


@pytest.mark.wsl
@pytest.mark.parametrize("check", ["", "-DIOP_CHECK"])
def test_kernel_and_libraries(tmp_path, check):
    shutil.copy(os.path.join(HERE, "iop_kernel_test.c"), str(tmp_path))
    rc, out, err = wsl_run(gcc_line(["iop_kernel_test.c"], "k", check) + " && ./k", str(tmp_path))
    assert rc == 0, err[-3000:]
    got = dict(l.split(" ", 1) for l in out.splitlines() if l[:1] == "K")
    bad = {k: (got.get(k), v) for k, v in EXPECTED.items() if got.get(k) != v}
    assert not bad, bad
