"""Conversion step: the trap set - the translated functions whose memory accesses must reach the hardware model
(hle/core/hle_mmio.c) instead of being masked into guest RAM.

    python convert/trapset.py [--profile FILE ...] [--out FILE]

On the PSP every guest load / store is `ram + (addr & mask)` (one AND); a trap check on every access would cost
megabytes of code. Masking is right for the scratchpad and the RAM mirrors, but EE hardware registers (0x1000xxxx),
VU memory (0x1100xxxx) and GS privileged registers (0x1200xxxx) must be trapped. Only the functions in this set get
the trapping accessors (ps2rc/pipeline/prepare_gen.py --trapset). Sources:
  1. static: functions that build a hardware address with `lui` (upper halves 0x1000/0x1001, 0x1100/0x1101,
     0x1200/0x1201 and their KSEG1 views 0xB000..; the +1 forms come from negative 16-bit offsets) and use it as the
     base of a load / store;
  2. Sony libraries that drive hardware through pointers kept in data (DMA, graphics, IPU libraries) - by name from
     the pack's library match list (analysis/sce_matches_v201.json);
  3. optional: function lists of host profiling runs (--profile).
Default output: the pack's [build] trapset."""
import json
import os
import struct
import sys

from common import PACK, OVERLAY_NAMES, BOOT_ELF, discover_functions, load_elf, load_functions, overlay_image
from ps2rc.config import analysis_dir as _analysis_dir

HW_LUI = set()
for base in (0x1000, 0x1100, 0x1200, 0xB000, 0xB100, 0xB200):
    HW_LUI |= {base, base + 1}
# not the BIOS (0x1FC0-0x1FFF / 0xBFC0-0xBFFF): those upper halves are mostly float constants (-1.5f = 0xBFC00000)
SCE_PREFIXES = ("sceDma", "sceGs", "sceIpu", "WaitDma", "WatchDma", "defStopDMA", "_waitIpuIdle", "_sceMpegWaitIpu",
                "_sceMpegStopIpuDma", "sceVif", "sceVu0", "sceVu1", "sceGif")

MEM_OPS = {0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D,
           0x2E, 0x31, 0x36, 0x37, 0x39, 0x3E, 0x3F}        # loads/stores incl. LQ/SQ, LWC1/SWC1, LQC2/SQC2, LD/SD
ADD_IMM = {0x09, 0x0D, 0x19}                                # addiu, ori, daddiu: rt = rs + imm
ADD_REG = {0x21, 0x25, 0x2D}                                # SPECIAL addu, or, daddu: rd = rs + rt


def scan(img, fns, prefix, why):
    """A function is marked when a register loaded with a hardware upper half (lui) - or derived from one by
    addiu/ori/daddiu/addu/or/daddu - is the base of a load or store (linear order, no kill analysis: conservative)."""
    for a, fn in fns.items():
        hw, lui_seen = set(), set()
        for pc in sorted(fn.addrs):
            try:
                w = img.word(pc)
            except Exception:
                continue
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if op == 0x0F and (w & 0xFFFF) in HW_LUI and rt:
                hw.add(rt)
                lui_seen.add(w & 0xFFFF)
            elif op in ADD_IMM and rs in hw and rt:
                hw.add(rt)
            elif op == 0 and (w & 63) in ADD_REG and (rs in hw or rt in hw) and rd:
                hw.add(rd)
            elif op in MEM_OPS and rs in hw:
                why.setdefault("%sfn_%08x" % (prefix, a), set()).add(
                    "hw base (lui %s)" % "/".join("%04x" % u for u in sorted(lui_seen)))


def main():
    args = sys.argv[1:]
    out = PACK.path("build.trapset")
    profiles = []
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]
            i += 2
        elif args[i] == "--profile":
            profiles.append(args[i + 1])
            i += 2
        else:
            i += 1
    why = {}
    img = load_elf(BOOT_ELF)
    scan(img, load_functions(), "", why)
    for name in OVERLAY_NAMES:
        ov, oimg, code = overlay_image(img, name)
        ofs = discover_functions(oimg, code)
        scan(oimg, ofs.functions, "ov%d_" % ov.id, why)
        ctors = [struct.unpack_from("<I", ov.raw, ov.ctor_start - ov.load_addr + 4 * k)[0]
                 for k in range((ov.ctor_end - ov.ctor_start) // 4)]
        missing = [a for a in ctors if a not in ofs.functions]
        if missing:
            cfs = discover_functions(oimg, [(min(missing) & ~15, ov.ctor_start)])
            scan(oimg, cfs.functions, "ov%d_" % ov.id, why)
    n_static = len(why)
    sce = json.load(open(os.path.join(_analysis_dir(), "sce_matches_v201.json"), encoding="utf-8"))
    for x in sce:
        if x.get("where", "main") == "main" and x["name"].startswith(SCE_PREFIXES):
            why.setdefault("fn_%s" % x["addr"].lower(), set()).add("sce " + x["name"])
    n_sce = len(why) - n_static
    n_dyn = 0
    for p in profiles:
        for ln in open(p, encoding="utf-8", errors="replace"):
            parts = ln.split()
            if not parts or not ("fn_" in parts[0]):
                continue
            name = parts[0].replace("__guest", "").replace("__stub", "")
            if name not in why:
                n_dyn += 1
            why.setdefault(name, set()).add("seen " + os.path.basename(p))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("# trap set for the PSP build (convert/trapset.py): %d functions - static lui %d, Sony hardware "
                "libraries +%d, host profiles +%d\n" % (len(why), n_static, n_sce, n_dyn))
        for n in sorted(why):
            f.write("%s  # %s\n" % (n, ", ".join(sorted(why[n]))))
    print("trap set: %d functions (static %d, sce +%d, profiles +%d) -> %s" % (len(why), n_static, n_sce, n_dyn, out))


if __name__ == "__main__":
    main()
