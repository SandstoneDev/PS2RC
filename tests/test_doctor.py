"""python -m ps2rc doctor (data-free; runs with or without the agent kit in the checkout)."""
import json
import subprocess
import sys

from ps2rc import config
from ps2rc.kit import doctor

ROOT = config.repo_root()


def test_doctor_quick_json():
    r = subprocess.run([sys.executable, "-m", "ps2rc", "doctor", "--quick", "--json"], cwd=ROOT,
                       capture_output=True, text=True, timeout=120)
    d = json.loads(r.stdout)
    assert {"checks", "ready", "core_missing", "packs"} <= set(d)
    assert any(c["name"] == "Python" for c in d["checks"])
    assert r.returncode == (1 if d["core_missing"] else 0)


def test_doctor_summary_logic():
    C = doctor.Check
    checks = [C("core", "Python", "ok"), C("core", "pyelftools", "ok"), C("core", "checkout", "ok"),
              C("core", "pytest", "missing"), C("build", "Docker pspdev image", "ok", need="optional")]
    caps, missing = doctor.summary(checks)
    assert caps["analysis and translation"] and not caps["tests (data-free)"] and caps["PSP build"]
    assert missing == ["pytest"]
