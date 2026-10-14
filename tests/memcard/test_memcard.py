"""The PS2 memory card HLE (hle/core/hle_memcard.c + the host file layer hle/host/hle_mcfs_host.c).

- wsl: tests/memcard/mc_unit.c drives the libmc hooks the way games do (command + sceMcSync(0)) on a card folder:
  no card (old behaviour, no virtual time), a save + load + listing + errors + free space, crash recovery of an
  interrupted save, an unformatted card + sceMcFormat, a full card, the copy fallback of a file system without rename.
- static: every sceMc* function is defined once by name (hle_memcard.c), none left in hle_sce.c; the PSP default of
  ps2rc.cfg memcard; the card folder default.
"""
import os
import re
import shutil
import subprocess
import zlib

import pytest
from ps2rc import config  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
HLE = os.path.join(ROOT, "hle")                          # hle/core, hle/host, hle/psp


def have_wsl():
    return config.have_bash()


def read(*parts):
    return open(os.path.join(*parts), encoding="utf-8").read()


MC_HOOKS = ["sceMcInit", "sceMcEnd", "sceMcGetSlotMax", "sceMcGetInfo", "sceMcSync", "sceMcOpen", "sceMcClose",
            "sceMcSeek", "sceMcRead", "sceMcWrite", "sceMcMkdir", "sceMcChdir", "sceMcGetDir", "sceMcSetFileInfo",
            "sceMcDelete", "sceMcFormat", "sceMcUnformat", "sceMcGetEntSpace", "sceMcRename", "sceMcFlush",
            "sceMcUdCheckNewCard"]


def test_every_libmc_hook_is_defined_once_in_hle_memcard():
    """The libmc functions are bound by name (HLE_SCE(name); a game pack's hooks.def gives the addresses): each one is
    defined once, in hle_memcard.c."""
    sources = {}
    for sub in ("core", "host", "psp"):
        sources.update({sub + "/" + fn: read(HLE, sub, fn) for fn in os.listdir(os.path.join(HLE, sub)) if fn.endswith(".c")})
    for name in MC_HOOKS:
        where = [fn for fn, text in sources.items() if re.search(r"^HLE_SCE\(%s\)" % name, text, re.M)]
        assert where == ["core/hle_memcard.c"], (name, where)


def test_psp_card_defaults():
    psp = read(HLE, "psp", "hle_mcfs_psp.c")
    m = re.search(r"int on = (\d);", psp)
    assert m, "the ps2rc.cfg memcard default"
    assert m.group(1) in ("0", "1")
    assert 'memcard/slot1/' in psp                      # <data dir>memcard/slot1/
    core = read(HLE, "core", "hle_memcard.c")
    assert "MC_ALLOC_CLUSTERS 8001u" in core             # 8,000 KB free on an empty formatted 8 MB card
    assert "._ps2rc_tmp" in core and "._ps2rc_new" in core


@pytest.mark.wsl
@pytest.mark.skipif(not have_wsl(), reason="needs bash + gcc (Linux, or WSL on Windows)")
@pytest.mark.parametrize("mode", ["nocard", "basic", "recover", "unformatted", "full", "norename"])
def test_memcard_hle_unit(mode):
    r = subprocess.run(config.bash_argv(os.path.join(HERE, "mc_unit.sh"), mode,
                                        "/tmp/ps2rc_mc_unit_%08x" % (zlib.crc32(ROOT.encode()))),
                       capture_output=True, timeout=600)
    out = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
    assert r.returncode == 0 and ("PASS %s" % mode) in out, out[-3000:]
