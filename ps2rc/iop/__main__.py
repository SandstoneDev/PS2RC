"""Command line of the IOP static recompiler (see __init__.py)."""
import argparse
import collections
import os
import sys

from .irx import load
from .analysis import analyse
from .gen_c import generate


def _info(args):
    x = load(args.module, int(args.base, 0))
    p = analyse(x)
    print("module %s version 0x%04x md5 %s" % (x.name, x.version, x.md5))
    print("entry 0x%08x gp 0x%08x text 0x%x data 0x%x bss 0x%x (base 0x%x)" % (x.base + x.entry, x.base + x.gp,
                                                                          x.text_size, x.data_size, x.bss_size, x.base))
    for imp in x.imports:
        print("import %-8s v%x: %s" % (imp.lib, imp.version, " ".join(str(f.num) for f in imp.fns)))
    for ex in x.exports:
        print("export %-8s v%x: %d functions" % (ex.lib, ex.version, len(ex.fns)))
    print("functions %d, callbacks %s" % (len(p.entries), " ".join("0x%x" % a for a in p.callbacks)))
    c = collections.Counter(s for s, _, _ in p.issues)
    print("issues: %s" % (dict(c) or "none"))
    for s, a, t in p.issues:
        if s != "info":
            print("  %s 0x%08x %s" % (s, a, t))
    return 1 if p.errors() else 0


def _list(args):
    x = load(args.module, int(args.base, 0))
    p = analyse(x)
    names = {a: "%s_%d" % (f.lib, f.num) for a, f in p.stubs.items()}
    out = open(args.output, "w") if args.output else sys.stdout
    for e in p.entries:
        f = p.functions[e]
        out.write("\nF_%08x:%s\n" % (e, "   (callback)" if e in p.callbacks else ""))
        for pc in range(f.entry, f.end, 4):
            i = p.insns[pc]
            t = i.text
            if i.op in ("jal", "j") and i.target in names:
                t += "   <%s>" % names[i.target]
            out.write("%s%08x: %08x  %s\n" % ("L" if pc in f.labels else " ", pc, i.word, t))
    if out is not sys.stdout:
        out.close()
    return 0


def _gen(args):
    x = load(args.module, int(args.base, 0))
    src, p = generate(x, args.sym, abi=not args.no_abi)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", newline="\n") as f:
        f.write(src)
    print("%s: %d functions, %d imports -> %s (%d lines)" % (x.name, len(p.entries),
                                                          sum(len(i.fns) for i in x.imports), args.output,
                                                          src.count("\n")))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m ps2rc.iop", description="IOP static recompiler (IRX -> C)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("info", "list", "gen"):
        s = sub.add_parser(name)
        s.add_argument("module")
        s.add_argument("--base", default="0x1000", help="load address of the module image (default 0x1000)")
        if name in ("list", "gen"):
            s.add_argument("-o", "--output", required=(name == "gen"))
        if name == "gen":
            s.add_argument("--sym", required=True, help="C name of the IopModule the file defines")
            s.add_argument("--no-abi", action="store_true", help="reload every register after a call (code that "
                                                                  "does not follow the MIPS calling convention)")
    args = ap.parse_args(argv)
    return {"info": _info, "list": _list, "gen": _gen}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
