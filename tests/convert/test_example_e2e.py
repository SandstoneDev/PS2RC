"""The converter end to end on the example pack (a small PS2 program written for this repository, no game): the
program is assembled (pspdev binutils), put on a synthetic disc image with a SYSTEM.CNF, and the converter makes the
PSP folder from that image - identify, translate, build, EBOOT, stage, verify. Needs the pspdev toolchain (Linux, or
WSL on Windows), marker `wsl`; skipped without it."""
import os
import shutil
import subprocess
import sys

import pytest

from isobuild import build_iso

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _bash(script, env):
    if os.name == "nt":
        if not shutil.which("wsl.exe"):
            pytest.skip("needs WSL with pspdev")
        e = dict(env, WSLENV="PS2RC_WORK/p:PS2RC_LOCAL/p", MSYS_NO_PATHCONV="1")
        root = "/mnt/" + ROOT[0].lower() + ROOT[2:].replace("\\", "/")
        return subprocess.run(["wsl.exe", "-d", "Debian", "--", "bash", "-c", "cd '%s' && %s" % (root, script)],
                              env=e, capture_output=True, text=True, timeout=1800)
    return subprocess.run(["bash", "-c", script], cwd=ROOT, env=env, capture_output=True, text=True, timeout=1800)


@pytest.mark.wsl
def test_converter_on_the_example_pack(tmp_path):
    if not os.path.exists(os.path.join(ROOT, "games", "example", "build_elf.sh")):
        pytest.skip("no example pack in this checkout")
    work = tmp_path / "elfwork"
    work.mkdir()
    local = tmp_path / "local.toml"
    local.write_text("")
    r = _bash("bash games/example/build_elf.sh", dict(os.environ, PS2RC_WORK=str(work), PS2RC_LOCAL=str(local)))
    if r.returncode != 0 and ("psp-as" in r.stderr and "not found" in r.stderr):
        pytest.skip("no pspdev binutils")
    assert r.returncode == 0, r.stdout + r.stderr
    elf = (work / "example" / "disc" / "EXAMPLE.ELF").read_bytes()
    iso = tmp_path / "example.iso"
    iso.write_bytes(build_iso({"SYSTEM.CNF": b"BOOT2 = cdrom0:\\EXAMPLE.ELF;1\r\nVER = 1.00\r\nVMODE = PAL\r\n",
                               "EXAMPLE.ELF": elf}, volume_id="EXAMPLE"))
    from ps2rc.convert.pipeline import Conversion, Options
    from ps2rc.convert.steps import Progress
    from ps2rc.pbp import Pbp, Sfo
    prog = Progress()
    events = []
    prog.listeners.append(events.append)
    out = tmp_path / "out"
    res = Conversion(Options(str(iso), str(out), repo=ROOT, splash=False, audio="none", movies="skip",
                             title="Example via the converter", jobs=os.cpu_count()), prog).run()
    folder = res["folder"]
    assert os.path.basename(folder) == "PS2RC_EXAMPLE"
    for f in ("EBOOT.PBP", "EXAMPLE.ELF", "discmap.txt", "iso_meta.bin", "ps2rc.cfg", "README_PS2RC.txt",
              "MANIFEST.md5", os.path.join("disc", "EXAMPLE.ELF")):
        assert os.path.exists(os.path.join(folder, f)), f
    p = Pbp.read(os.path.join(folder, "EBOOT.PBP"))
    assert Sfo.parse(p["PARAM.SFO"]).get("TITLE") == "Example via the converter"
    assert p["DATA.PSP"][:4] == b"\x7fELF"
    assert "audio=0" in open(os.path.join(folder, "ps2rc.cfg")).read()
    assert any(ev[0] == "warn" and "elf_sha256" in ev[1] for ev in events)      # the example pack has no [verify]
