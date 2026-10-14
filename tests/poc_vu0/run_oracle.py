"""Run the VU0 oracle guest ELF in PCSX2 (EE recompiler = microVU0 macro mode, and EE interpreter = VUops.cpp) with
the CPU settings of a game whose PCSX2 GameIndex.yaml entry has clampModes eeClampMode 2, and save the output blocks.

Those settings: FPU "extra + preserve sign", no vuClampMode (VU0 stays "normal": vu0Overflow only), no roundModes
(FPU/VU0 round toward zero, DAZ/FTZ), no gameFixes touching VU0. A test ELF matches no GameDB entry, so those values
are set explicitly (GAME_OVERRIDES).

Writes build/poc_vu0/pcsx2_rec.bin, pcsx2_interp.bin, pcsx2_runs.json (timings, modes, overrides, input hash).
"""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc.oracle.pcsx2 import Pcsx2Runner  # noqa: E402

OUT = os.environ.get("PS2RC_VU0_OUT") or os.path.join(ROOT, "build", "poc_vu0")
GAME_OVERRIDES = {
    ("EmuCore/CPU/Recompiler", "fpuOverflow"): True,          # eeClampMode 2
    ("EmuCore/CPU/Recompiler", "fpuExtraOverflow"): True,
    ("EmuCore/CPU/Recompiler", "fpuFullMode"): False,
    ("EmuCore/CPU/Recompiler", "vu0Overflow"): True,          # VU clamp mode 1 ("normal", the default)
    ("EmuCore/CPU/Recompiler", "vu0ExtraOverflow"): False,
    ("EmuCore/CPU/Recompiler", "vu0SignOverflow"): False,
    ("EmuCore/CPU/Recompiler", "vu0Underflow"): False,
    ("EmuCore/CPU/Recompiler", "vu1Overflow"): True,
    ("EmuCore/CPU/Recompiler", "vu1ExtraOverflow"): False,
    ("EmuCore/CPU/Recompiler", "vu1SignOverflow"): False,
    ("EmuCore/CPU/Recompiler", "vu1Underflow"): False,
    ("EmuCore/CPU", "FPU.Roundmode"): 3,                      # ChopZero
    ("EmuCore/CPU", "FPU.DenormalsAreZero"): True,
    ("EmuCore/CPU", "FPUDiv.Roundmode"): 0,                   # PCSX2 default (nearest) for FPU division
    ("EmuCore/CPU", "FPUDiv.DenormalsAreZero"): True,
    ("EmuCore/CPU", "VU0.Roundmode"): 3,
    ("EmuCore/CPU", "VU0.DenormalsAreZero"): True,
    ("EmuCore/CPU", "VU1.Roundmode"): 3,
    ("EmuCore/CPU", "VU1.DenormalsAreZero"): True,
    ("EmuCore/Speedhacks", "vuFlagHack"): True,               # default
}


def symbols():
    syms = {}
    for line in open(os.path.join(OUT, "vu0.syms"), encoding="ascii"):
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def input_hash():
    h = hashlib.sha1()
    for name in ("vu0.elf",):
        h.update(open(os.path.join(OUT, name), "rb").read())
    return h.hexdigest()


def main():
    meta = json.load(open(os.path.join(OUT, "tests.json"), encoding="utf-8"))
    nbytes = len(meta["tests"]) * meta["tests"][0]["nvec"] * meta["out_size"]
    syms = symbols()
    runner = Pcsx2Runner()
    report = {"elf_sha1": input_hash(), "overrides": {"%s/%s" % k: v for k, v in GAME_OVERRIDES.items()}}
    for mode, interp in (("rec", False), ("interp", True)):
        # generous timeout: on a loaded machine PCSX2 needed up to ~380 s just to start (normally ~15 s per run)
        r = runner.run_elf(os.path.join(OUT, "vu0.elf"), syms["done_flag"], 0x600DF00D, timeout_s=1800,
                           ee_interpreter=interp, reads={"out": (syms["outputs"], nbytes)},
                           log_path=os.path.join(OUT, "pcsx2_%s.log" % mode), overrides=GAME_OVERRIDES)
        open(os.path.join(OUT, "pcsx2_%s.bin" % mode), "wb").write(r["blocks"]["out"])
        report[mode] = {k: r[k] for k in ("cpu_mode", "timings", "shutdown", "tlb_misses", "pcsx2_version")}
        print(mode, r["cpu_mode"], {k: round(v, 2) for k, v in r["timings"].items()}, "tlb_misses", r["tlb_misses"])
    json.dump(report, open(os.path.join(OUT, "pcsx2_runs.json"), "w", encoding="utf-8"), indent=1)


if __name__ == "__main__":
    main()
