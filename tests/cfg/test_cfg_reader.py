"""The run options reader (hle/core/hle_cfg.c), compiled standalone with the host gcc: the current file names first,
the pack's older name (HLE_PACK_CFG_LEGACY) only when the current one is missing, each of the two files (the player's
<cfg> and the developer's <cfg>_dev) on its own. A folder converted by an earlier version keeps working until an
update renames its files."""
import os
import shutil
import subprocess
import zlib

import pytest
from ps2rc import config

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

MAIN_C = r"""
#include <stdio.h>
#include "hle_cfg.h"
int main(void)
{
    const char *a = hle_cfg_get("a"), *d = hle_cfg_get("d");
    printf("a=%s d=%s found=%d\n", a ? a : "-", d ? d : "-", hle_cfg_found());
    hle_cfg_report();
    return 0;
}
"""

# folder -> {file: text}
CASES = {
    "both": {"ps2rc.cfg": "a=new\n", "old.cfg": "a=old\n", "ps2rc_dev.cfg": "d=newdev\n", "old_dev.cfg": "d=olddev\n"},
    "legacy": {"old.cfg": "a=old\n", "old_dev.cfg": "d=olddev\n"},
    "mixed": {"old.cfg": "a=old\n", "ps2rc_dev.cfg": "d=newdev\n"},
    "none": {},
}


def _run(tmp, legacy):
    tag = "%08x_%d" % (zlib.crc32(ROOT.encode()), legacy)
    src = tmp / "src"
    src.mkdir(exist_ok=True)
    (src / "main.c").write_text(MAIN_C, encoding="ascii")
    for name, files in CASES.items():
        d = tmp / name
        d.mkdir(exist_ok=True)
        for fn, text in files.items():
            (d / fn).write_text(text, encoding="ascii")
    core = config.posix_path(os.path.join(ROOT, "hle", "core"))
    p = config.posix_path(str(tmp))
    leg = "-DHLE_PACK_CFG_LEGACY='\"old.cfg\"'" if legacy else ""
    exe = "/tmp/ps2rc_cfg_reader_%s" % tag
    # no shell variables: wsl.exe expands $NAME itself
    cmd = ("set -e; gcc -O1 -Wall -DHLE_CFG_STANDALONE %s -I'%s' '%s/hle_cfg.c' '%s/src/main.c' -o %s; "
           % (leg, core, core, p, exe) +
           "".join("echo '== %s'; HLE_CFG_DIR='%s/%s' %s; " % (c, p, c, exe) for c in CASES) + "rm -f %s" % exe)
    r = subprocess.run(config.bash_c_argv(cmd), capture_output=True, timeout=300)
    out = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    assert r.returncode == 0, out + r.stderr.decode("utf-8", "replace")
    res = {}
    for block in out.split("== ")[1:]:
        name, _, body = block.partition("\n")
        res[name.strip()] = body
    return res


def _line(body):
    return body.split("\n", 1)[0]


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")),
                    reason="needs bash + gcc (Linux, or WSL on Windows)")
def test_new_names_first_then_legacy(tmp_path):
    res = _run(tmp_path, legacy=True)
    assert _line(res["both"]) == "a=new d=newdev found=1"          # the old files are ignored next to the new ones
    assert "(ps2rc_dev.cfg + ps2rc.cfg," in res["both"]
    assert _line(res["legacy"]) == "a=old d=olddev found=1"        # a folder of an earlier version
    assert "(old_dev.cfg + old.cfg," in res["legacy"]
    assert _line(res["mixed"]) == "a=old d=newdev found=1"         # each file falls back on its own
    assert "(ps2rc_dev.cfg + old.cfg," in res["mixed"]
    assert _line(res["none"]) == "a=- d=- found=0" and "(no file" in res["none"]


@pytest.mark.wsl
@pytest.mark.skipif(not config.have_bash() or (os.name != "nt" and not shutil.which("gcc")),
                    reason="needs bash + gcc (Linux, or WSL on Windows)")
def test_no_legacy_name_no_fallback(tmp_path):
    res = _run(tmp_path, legacy=False)
    assert _line(res["both"]) == "a=new d=newdev found=1"
    assert _line(res["legacy"]) == "a=- d=- found=0"               # a pack without cfg_legacy reads only its names
    assert _line(res["mixed"]) == "a=- d=newdev found=1"


def test_pack_values_reach_the_runtime():
    """pack.py writes HLE_PACK_CFG_LEGACY only for a pack with [runtime] cfg_legacy; the reader names no file itself."""
    from ps2rc import pack as packmod
    src = open(os.path.join(ROOT, "hle", "core", "hle_cfg.c"), encoding="utf-8").read()
    assert "HLE_PACK_CFG_LEGACY" in src
    ex = packmod.cheader(packmod.load(os.path.join(ROOT, "games", "example")))
    assert "HLE_PACK_CFG_LEGACY" not in ex and '#define HLE_PACK_CFG "ps2rc.cfg"' in ex
