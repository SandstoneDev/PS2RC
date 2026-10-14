"""Conversion step: translate the code overlays' static constructors that lie outside their .text.

    python convert/overlay_ctors.py      -> <work>/<[build] patch_dir>/{extra_ov<N>.h, extra_ov<N>_000.c, ...}

Overlay function discovery covers .text only; some overlays list static constructors after .text in their
constructor table, run by the overlay loader through a register call. They are translated here as extra functions in
exact mode (all registers in the context: safe next to ABI-mode code because they are only reached through the
dispatcher with a full context); ps2rc/pipeline/prepare_gen.py maps them into the dispatcher."""
import os
import struct

from common import PACK, FunctionSet, OVERLAY_NAMES, discover_functions, load_elf, load_functions, overlay_image, say
from common import BOOT_ELF
from ps2rc import backend_c


def main():
    out = PACK.path("build.patch_dir")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    img = load_elf(BOOT_ELF).stripped()
    funcs = load_functions()
    external = {a: "fn_%08x" % a for a in funcs}
    report = []
    for name in OVERLAY_NAMES:
        ov, oimg, code = overlay_image(img, name)
        ctors = [struct.unpack_from("<I", ov.raw, ov.ctor_start - ov.load_addr + 4 * k)[0]
                 for k in range((ov.ctor_end - ov.ctor_start) // 4)]
        ofs = discover_functions(oimg, code)
        missing = [a for a in ctors if a not in ofs.functions]
        if not missing:
            continue
        cfs = discover_functions(oimg, [(min(missing) & ~15, ov.ctor_start)])
        got = [a for a in missing if a in cfs.functions]
        prefix = "ov%d_" % ov.id
        sub = FunctionSet({a: cfs.functions[a] for a in got}, [])
        files, unsupported, todo = backend_c.emit_units(oimg, sub, "extra_ov%d" % ov.id, per_unit=100000,
                                                        prefix=prefix, dispatch="rc_dispatch", external=external)
        for k, v in files.items():
            if not k.endswith("_tab.c"):
                open(os.path.join(out, k), "w", encoding="ascii", newline="\n").write(v)
        report.append("%s (ov%d): static constructors outside .text %s -> %d translated (exact), %d unsupported"
                      % (name, ov.id, " ".join("%08x" % a for a in missing),
                         len(set(todo) - {a for a, _m in unsupported}), len(unsupported)))
    open(os.path.join(out, "patched.txt"), "w", encoding="ascii", newline="\n").write("")
    open(os.path.join(out, "report.txt"), "w", encoding="ascii", newline="\n").write("\n".join(report) + "\n")
    say("\n".join(report))


if __name__ == "__main__":
    main()
