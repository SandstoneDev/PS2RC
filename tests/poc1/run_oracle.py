"""Run the PoC-1 guest ELF in PCSX2 (EE recompiler and EE interpreter) and save the output blocks.
Writes build/poc1/pcsx2_rec.bin, build/poc1/pcsx2_interp.bin and pcsx2_runs.json (timings, modes)."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc.oracle.pcsx2 import Pcsx2Runner  # noqa: E402

OUT = os.path.join(ROOT, "build", "poc1")


def symbols():
    syms = {}
    for line in open(os.path.join(OUT, "poc1.syms")):
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def main():
    meta = json.load(open(os.path.join(OUT, "tests.json")))
    nbytes = len(meta["tests"]) * meta["tests"][0]["nvec"] * meta["out_size"]
    syms = symbols()
    runner = Pcsx2Runner()
    report = {}
    for mode, interp in (("rec", False), ("interp", True)):
        r = runner.run_elf(os.path.join(OUT, "poc1.elf"), syms["done_flag"], 0x600DF00D, timeout_s=600,
                           ee_interpreter=interp, reads={"out": (syms["outputs"], nbytes)},
                           log_path=os.path.join(OUT, "pcsx2_%s.log" % mode))
        open(os.path.join(OUT, "pcsx2_%s.bin" % mode), "wb").write(r["blocks"]["out"])
        report[mode] = {k: r[k] for k in ("cpu_mode", "timings", "shutdown", "tlb_misses", "pcsx2_version")}
        print(mode, r["cpu_mode"], {k: round(v, 2) for k, v in r["timings"].items()}, "tlb_misses", r["tlb_misses"])
    json.dump(report, open(os.path.join(OUT, "pcsx2_runs.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
