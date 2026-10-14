"""Translate a game pack's program: the boot ELF (and its code overlays) -> C compilation units + stats.json.

    python ps2rc/pipeline/translate.py [--pack NAME] [--variant V] [--out DIR] [--functions FILE|discover]
                                       [--exempt] [--overlays] [--vfloc-skip FILE] [--no-psp-rules] [--rlive]

Defaults come from the pack's [translate] table (games/_template/pack.toml documents it):
  variant       back-end variant: v0 (exact everywhere), v4, v5s1..v5s4, v5s4y1, v5s4y2 (ps2rc.backend_c.V5_STEPS)
  unit          base name of the main program's units (<unit>.h, <unit>_NNN.c, <unit>_tab.c; default "main")
  functions     a function set computed once by function discovery (a pickle {address: Function}); omitted: the
                discovery runs now over [memory] code (+ ctor_code) from the entry point
  non_abi       json {"exact_starts": {unit: [hex addresses]}}: functions translated exact (--exempt / exempt = true)
  overlays      true: the pack's [overlays] files (Metrowerks MWo3) are translated too, one unit set each
  vfloc_skip    functions kept off the vfloc lever (one C name per line)
  rlive         true (or --rlive): lever rlive of the variants v5s4y2r / v5s4y3r / v5s4y3rx - a return writes back only
                the parts of v0 / v1 / f0 / f1 its callers read (ps2rc.analysis.retdemand); the functions whose returns
                stay complete: see rlive_sets
  rlive_lists   glob(s) of address lists (first word of a line = a hex address) whose functions keep complete returns
                (native fast paths that compare the whole context, functions the HLE observes), e.g. "lists/*.txt"
  psp_rules     the argument-liveness rules the HLE needs (default true for the ABI variants, see below)
  stats_header  a C header with the translation's coverage numbers, e.g. "main_stats.h" (prefix stats_prefix)
  out           the output dir (default: the pack's [build] translation)
The ELF is the pack's [id] boot_elf in its disc folder (local.toml [games."<pack>"] disc, or [run] disc).

Width analysis (ABI variants): argument liveness, narrowing plans and the whole-program wide_args / wide_rets closure
(main program + overlays, iterated until neither grows). The PSP liveness rules (psp_rules), needed because in ABI mode
a call site stores into the context only the argument registers the callee is known to read:
  1. a syscall reads every argument register (the EE kernel takes its arguments from a0-a3 / t0-t3);
  2. a function the HLE replaces (hooks.def HOOK / HOOK_OV) gets all argument registers (the HLE reads them in C);
  3./4. wide_args / wide_rets recomputed as a joint fixpoint over width's forward lattice, so values that are only passed
     through (an argument forwarded to a callee, a callee's 64-bit result returned) are seen;
  5. (v5 lever ctx32) hooked functions get no one-word arguments.
"""
import collections
import json
import os
import pickle
import re
import struct
import sys
import time

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from ps2rc import config, pack as _pack  # noqa: E402
from ps2rc.image import Image, Segment, load_elf  # noqa: E402
from ps2rc.overlay import load_overlay  # noqa: E402
from ps2rc.analysis import width  # noqa: E402
from ps2rc.analysis.functions import FunctionSet, discover_functions  # noqa: E402
from ps2rc.analysis.width import ARGS, analyse_program, narrowable  # noqa: E402
from ps2rc import backend_c  # noqa: E402
from ps2rc.backend_c import V5_STEPS, emit_units  # noqa: E402

_Fn, _uses_defs, CALLER_SAVED = width._Fn, width._uses_defs, width.CALLER_SAVED
SYSCALL_SEEN = {"n": 0}


def say(*a):
    print(*a, flush=True)


def progress(tag):
    t0 = time.time()

    def p(k, n):
        say("  %s %d/%d (%.0f s)" % (tag, k, n, time.time() - t0))
    return p


def mnemonics(unsupported):
    c = collections.Counter()
    for _a, msg in unsupported:
        m = re.match(r"[0-9a-f]{8}: (\S+)", msg)
        c[m.group(1) if m else msg.split(":")[-1].strip()[:30]] += 1
    return c


def variant_opts(variant, plan, livein, exempt=None):
    if variant == "v0":
        return {}
    opts = {"cache": True, "narrow": plan, "fpu_fast": True, "abi": True, "livein": livein}
    if variant in V5_STEPS:
        opts["v5"] = V5_STEPS[variant]
    if exempt:
        opts["abi_exempt"] = exempt
    return opts


def exempt_sets(path):
    """{unit name: set of exact starts} from a non-ABI json ("main", "<overlay file>")."""
    d = json.load(open(path, encoding="utf-8"))
    return {u: {int(s, 16) for s in v} for u, v in d["exact_starts"].items()}


def write(out, files):
    for name, text in files.items():
        open(os.path.join(out, name), "w", newline="\n", encoding="ascii").write(text)


def write_stats_header(out, stats, name, prefix):
    """<name>: the translation's coverage numbers as C macros (<prefix>_GUEST_BYTES, _FUNCTIONS, _STUBBED)."""
    ovs = stats.get("overlays", {}).values()
    guest = (stats["guest_insns"] - stats["main"]["stubbed_guest_insns"] +
             sum(o["guest_insns"] - o["stubbed_guest_insns"] for o in ovs))
    text = ["/* generated by ps2rc/pipeline/translate.py */",
            "#define %s_GUEST_BYTES %du      /* guest code translated (main + overlays), bytes */" % (prefix, 4 * guest),
            "#define %s_FUNCTIONS %du" % (prefix, stats["main"]["translated"] + stats["main"]["stubbed"] +
                                          sum(o["functions"] for o in ovs)),
            "#define %s_STUBBED %du" % (prefix, stats["main"]["stubbed"] + sum(o["stubbed"] for o in ovs)), ""]
    open(os.path.join(out, name), "w", newline="\n", encoding="ascii").write("\n".join(text))


def work_rel(path):
    """A work-dir file as recorded in stats.json: "build/<path in the work dir>"."""
    return "build/" + os.path.relpath(path, config.work_dir()).replace("\\", "/")


def overlay_ctors(ov):
    """Addresses in the MWo3 static constructor table (ov.ctor_start .. ov.ctor_end), in table order."""
    return [struct.unpack_from("<I", ov.raw, ov.ctor_start - ov.load_addr + 4 * k)[0]
            for k in range((ov.ctor_end - ov.ctor_start) // 4)]


def overlay_unit(img, path):
    """(ov, oimg, FunctionSet, constructors outside .text) of one overlay: functions discovered in .text plus the
    static constructors listed in the constructor table that lie outside it (code between the first such constructor
    and the table) and whatever they call or tail-call inside that range."""
    name = os.path.basename(path)
    ov = load_overlay(path)
    mem = ov.memory()
    seg = Segment(vaddr=ov.load_addr, filesz=len(ov.raw), memsz=len(mem), flags=7, data=mem)
    oimg = Image(path=ov.path, entry=ov.text_addr, gp=img.gp, segments=list(img.segments) + [seg],
                 overlay_slots=[])
    text = (ov.text_addr, ov.text_addr + ov.text_size)
    ofs = discover_functions(oimg, [text])
    outside = [a for a in overlay_ctors(ov) if not text[0] <= a < text[1]]
    if outside:
        lo, hi = min(outside), ov.ctor_start
        if not (text[1] <= lo < hi):
            raise SystemExit("%s: constructors %s not between .text and the constructor table"
                             % (name, ["%08x" % a for a in outside]))
        cfs = discover_functions(oimg, [(lo, hi)], extra_seeds=outside)
        need, add = set(outside), {}
        while need:
            a = need.pop()
            if a in add or a in ofs.functions:
                continue
            if a not in cfs.functions:
                raise SystemExit("%s: static constructor %08x not discovered" % (name, a))
            add[a] = cfs.functions[a]
            need |= {t for t in cfs.functions[a].calls | cfs.functions[a].tail_calls if t in cfs.functions}
        ofs.functions.update(add)
        ofs.code_ranges.append((lo, hi))
    return ov, oimg, ofs, sorted(outside)


def weak_ctors(files, names):
    """Define the given functions weak (static constructors outside .text: a harness that still links its own
    translation of them keeps linking)."""
    out = {}
    for f, text in files.items():
        for n in names:
            text = text.replace("\nvoid %s(R5900Ctx *c)\n{" % n,
                                "\n__attribute__((weak)) /* MWo3 static constructor outside .text */\n"
                                "void %s(R5900Ctx *c)\n{" % n)
        out[f] = text
    return out


def wide_fixpoint(res, oanal, funcs):
    """Whole-program wide_args / wide_rets of the main program and the overlays: the overlays' closures get the main
    program's wide_rets; argument registers an overlay passes non-canonical to a main function go into the main
    closure; repeated until neither grows. oanal: {overlay: WidthResult}. Returns ({main function: argument registers
    arriving non-canonical}, main wide_rets, {overlay: (wide_args, wide_rets)}, main functions whose registers grew
    through overlay calls)."""
    wc = res.closure
    main_wr = set(res.wide_rets) | (wc.wr if wc else set())
    before = dict(wc.wam) if wc else {}
    extra = set()
    while True:
        ext = {}
        for ores in oanal.values():
            oc = ores.closure
            if oc is not None:
                oc.add(extern_wide_rets=main_wr)
                for t, m in oc.extern_wam.items():
                    ext[t] = ext.get(t, 0) | m
            else:
                for t in ores.extern_wide_args:
                    ext.setdefault(t, ARGS)
        ext = {t: m for t, m in ext.items() if t in funcs}
        if wc is None:
            extra = set(ext)
            break
        if not wc.add(wide_args=ext):
            break
        main_wr |= wc.wr
    wam = dict(wc.wam) if wc else {}
    from_ov = {a for a, m in wam.items() if m & ~before.get(a, 0)} if wc else extra
    for a in set(res.wide_args) | extra:
        wam.setdefault(a, ARGS)
    osets = {}
    for name, ores in oanal.items():
        oc = ores.closure
        osets[name] = (set(ores.wide_args) | (oc.wa if oc else set()),
                       set(ores.wide_rets) | (oc.wr if oc else set()) | main_wr)
    return wam, main_wr, osets, from_ov


# ------------------------------------------------------------------ the PSP liveness rules (module docstring)
def _livein_fn(f, livein):
    """One function's argument registers live at entry under the callees' `livein` (rule 1 included)
    -> (mask, how often the syscall rule fired)."""
    nsys = 0
    live = {x: 0 for x in f.addrs}
    changed = True
    order = sorted(f.addrs, reverse=True)
    while changed:
        changed = False
        for x in order:
            o = 0
            for e in f.exits[x]:
                if e[0] in ("tail", "call"):
                    o |= livein.get(e[1], ARGS) if e[1] is not None else ARGS
            for s, e in f.succ[x]:
                v = live[s]
                if e is not None:
                    v = (v & ~CALLER_SAVED) | (livein.get(e[1], ARGS) if e[1] is not None else ARGS)
                o |= v
            op = f.op[x]
            if op is not None:
                use, dfn = _uses_defs(op, f.insn[x])
                o = (o & ~dfn) | use
            if f.insn[x].mnem == "syscall":               # the EE kernel reads its arguments from a0-a3, t0-t3
                o |= ARGS                                 # (lift() gives syscall an op with no sources)
                nsys += 1
            if o != live[x]:
                live[x] = o
                changed = True
    return (live[f.start] & ARGS if f.start in live else ARGS), nsys


def build_fns(img, functions, addrs=None, need_start=False):
    """{address: width._Fn} of `addrs` (default: all functions); need_start: only those whose start was lifted."""
    starts = set(functions)
    fns = {}
    for a in (functions if addrs is None else addrs):
        fn = functions[a]
        f = _Fn(img, fn, starts)
        f.attach_jump_tables(fn.jump_tables)
        if not need_start or fn.start in f.insn:
            fns[a] = f
    return fns


def _par_team(functions):
    from ps2rc.pipeline import parallel
    n = parallel.jobs()
    return parallel if n > 1 and len(functions) > 2 * parallel.CHUNK else None


def argument_livein(img, functions, rounds=3):
    """ps2rc.analysis.width.argument_livein + rule 1 (a syscall reads every argument register). Jacobi rounds (each
    reads only the previous round's result), so PS2RC_TRANSLATE_JOBS > 1 splits every round over processes with the
    same result."""
    par = _par_team(functions)
    team = par.Team(par.jobs(), "ps2rc.pipeline.translate", "_team_fns", (img, functions, False)) if par else None
    fns = None if team else build_fns(img, functions)
    livein = {a: ARGS for a in functions}
    try:
        for _ in range(rounds + 2):                      # two more rounds: the syscall demand climbs wrapper chains
            if team:
                part = {}
                for got, nsys in team.round("_team_livein_round", livein):
                    part.update(got)
                    SYSCALL_SEEN["n"] += nsys
                new = {a: part[a] for a in functions}
            else:
                new = {}
                for a, f in fns.items():
                    new[a], nsys = _livein_fn(f, livein)
                    SYSCALL_SEEN["n"] += nsys
            if new == livein:
                break
            livein = new
    finally:
        if team:
            team.close()
    return livein


def _team_fns(payload, k, n):
    img, functions, need_start = payload[:3]
    mine = [a for i, a in enumerate(functions) if i % n == k]
    return {"fns": build_fns(img, functions, mine, need_start), "livein": payload[3] if len(payload) > 3 else None}


def _team_livein_round(state, livein):
    got, nsys = {}, 0
    for a, f in state["fns"].items():
        got[a], c = _livein_fn(f, livein)
        nsys += c
    return got, nsys


def _team_wide_round(state, wa, wr):
    return {a: _wide_fn(a, f, wa, wr, state["livein"]) for a, f in state["fns"].items()}


def _wide_fn(a, f, wa, wr, livein):
    """Function `a` under the round's wide_args / wide_rets: (functions it passes a non-canonical argument, in visit
    order; whether it returns a non-canonical v0)."""
    args, ret = [], False
    entry = (width.ALL & ~(ARGS if a in wa else 0), 0, 0, 0xF, ())

    def call(st, target):
        c32, blob, hi, hilo, slots = width._call_fwd(st)
        if target is None or target in wr:
            c32 &= ~width.RETS
        return (c32, blob, hi, hilo, slots)
    st_in = width.forward(f, entry, call)
    for x in f.addrs:
        if st_in[x] is None:
            continue
        c32 = width._fwd(f, x, st_in[x])[0]
        for _s, e in f.succ[x]:
            if e is not None and e[1] is not None and livein.get(e[1], ARGS) & ~c32:
                args.append(e[1])
        for e in f.exits[x]:
            if e[0] == "ret" and not (c32 >> 2 & 1):
                ret = True
            elif e[0] == "tail" and e[1] is not None:
                if livein.get(e[1], ARGS) & ~c32:
                    args.append(e[1])
                if e[1] in wr:
                    ret = True
    return args, ret


def wide_sets(img, functions, livein, wa0, wr0, max_rounds=200):
    """Which functions receive a non-canonical (full 64-bit) argument (wide_args) and which return a non-canonical v0
    (wide_rets), as a joint fixpoint over width's forward lattice (rules 3 / 4). Jacobi rounds: with
    PS2RC_TRANSLATE_JOBS > 1 every round is split over processes; the sets grow in the serial loop's order."""
    par = _par_team(functions)
    team = par.Team(par.jobs(), "ps2rc.pipeline.translate", "_team_fns", (img, functions, True, livein)) \
        if par else None
    fns = None if team else build_fns(img, functions, need_start=True)
    wa, wr = set(wa0), set(wr0)
    rounds = 0
    try:
        for rounds in range(1, max_rounds + 1):
            nwa, nwr = set(wa), set(wr)
            if team:
                got = {}
                for part in team.round("_team_wide_round", wa, wr):
                    got.update(part)
                order = [a for a in functions if a in got]
            else:
                got = {a: _wide_fn(a, f, wa, wr, livein) for a, f in fns.items()}
                order = list(got)
            for a in order:
                args, ret = got[a]
                nwa.update(args)
                if ret:
                    nwr.add(a)
            if nwa == wa and nwr == wr:
                break
            wa, wr = nwa, nwr
    finally:
        if team:
            team.close()
    return wa, wr, rounds


def install_lowonly_rule(hooks, backend=None):
    """Rule 5: wrap backend_c._lowonly_args (v5 lever ctx32) so that hooked functions get no one-word arguments.
    Returns the counter dict {"n": refused functions}; a back-end without the lever is left alone."""
    if backend is None:
        backend = backend_c
    counter = {"n": 0}
    orig = getattr(backend, "_lowonly_args", None)
    if orig is None:
        return counter

    def _lowonly_args(f, jump_tables, narrow, livein, *rest):
        if f.start in hooks:
            counter["n"] += 1
            return 0
        return orig(f, jump_tables, narrow, livein, *rest)     # + per_target (lever argx)
    backend._lowonly_args = _lowonly_args               # emit_units looks it up in its module
    return counter


HLE_NAMED = re.compile(r"\b(?:0x0*|fn_0*)([1-5][0-9a-fA-F]{5})\b")


def hle_named_addresses(p):
    """Guest addresses the runtime's C sources name (hex constants / fn_ names of 0x1xxxxx-0x5xxxxx in the core HLE
    and the pack's C modules, hooks and zones): the HLE may read the results of these functions."""
    roots = [os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "hle")]
    for key in ("build.sources", "build.audio_sources"):
        roots += [os.path.join(p.dir, d) for d in (p.get(key) or [])]
    files = []
    for r in roots:
        for d, _dirs, fs in os.walk(r):
            files += [os.path.join(d, fn) for fn in sorted(fs) if fn.endswith((".c", ".h", ".def", ".S"))]
    for key in ("build.hooks", "build.zones"):
        if p.get(key):
            files.append(p.path(key))
    out = set()
    for path in files:
        text = open(path, encoding="utf-8", errors="replace").read()
        out.update(int(m.group(1), 16) for m in HLE_NAMED.finditer(text))
    return out


def list_addresses(p, globs):
    import glob
    out = set()
    for g in ([globs] if isinstance(globs, str) else globs or []):
        for path in sorted(glob.glob(os.path.join(p.dir, g))):
            for ln in open(path, encoding="utf-8"):
                m = re.match(r"\s*(?:0x)?([0-9a-fA-F]{8})\b", ln)
                if m:
                    out.add(int(m.group(1), 16))
    return out


def rlive_sets(p, img, funcs, overlay_images=()):
    """Lever rlive: (full, indirect) guest function sets for backend_c.RLIVE. full = returns stay complete: hooked
    functions (the HLE reads their results), the pack's rlive_lists (native fast paths whose check mode compares the
    whole context, functions the HLE observes), every function the runtime's sources name, everything the overlays
    call, tail-call or reference, and every address-taken function of the main program (the HLE dispatches handlers -
    threads, interrupts, alarms, custom syscalls - and reads their v0/v1). indirect = the address-taken functions (also
    called by JALR). Address-taken = a data or code word equal to the function's start, or a lui/addiu|ori constant
    pair in a function."""
    starts = set(funcs)
    full = set(hooked_addresses(p.path("build.hooks")))
    full |= list_addresses(p, p.get("translate.rlive_lists"))
    full |= hle_named_addresses(p)
    full &= starts
    main_segs = {(s.vaddr, s.memsz) for s in img.segments}

    def scan(image, code_fns):
        taken, called = set(), set()
        for seg in image.segments:
            if image is not img and (seg.vaddr, seg.memsz) in main_segs:
                continue                                    # an overlay image includes the main program's segments
            data = seg.data[:seg.filesz]
            for off in range(0, len(data) - 3, 4):
                w = int.from_bytes(data[off:off + 4], "little")
                if w in starts:
                    taken.add(w)
                if w >> 26 in (2, 3):                       # j / jal
                    a = seg.vaddr + off
                    tgt = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    if tgt in starts:
                        called.add(tgt)
        for fn in code_fns:                                 # lui rX,hi ... addiu|ori rY,rX,lo
            his, los = {}, []
            for a in fn.addrs:
                try:
                    w = image.word(a)
                except Exception:
                    continue
                opc, rs, rt, imm = w >> 26, w >> 21 & 31, w >> 16 & 31, w & 0xFFFF
                if opc == 0x0F:
                    his.setdefault(rt, set()).add(imm << 16)
                elif opc in (0x09, 0x0D):
                    los.append((rs, imm, opc))
            for rs, imm, opc in los:
                for hi in his.get(rs, ()):
                    v = (hi + (imm - 0x10000 if opc == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF \
                        if opc == 0x09 else hi | imm
                    if v in starts:
                        taken.add(v)
        return taken, called
    taken, _ = scan(img, funcs.values())
    for oimg, ofuncs in overlay_images:
        ot, oc = scan(oimg, ofuncs)
        full |= ot | oc
    full |= taken
    return frozenset(full), frozenset(taken)


def hooked_addresses(hooks_def):
    text = re.sub(r"/\*.*?\*/", "", open(hooks_def, encoding="utf-8").read(), flags=re.S)
    return {int(a, 16) for a in re.findall(r"^\s*HOOK(?:_OV\(\s*\d+\s*,|\()\s*([0-9a-f]{8})\s*,", text, re.M)}


class _PspRules:
    """Installs rules 1-5 around analyse_program for one translation."""
    def __init__(self, hooks):
        self.hooks = hooks
        self.main_wide_rets = set()
        width.argument_livein = argument_livein          # analyse_program looks it up in its module
        self.lowonly = install_lowonly_rule(hooks)

    def analyse_program(self, img, functions, **kw):
        SYSCALL_SEEN["n"] = 0
        res, livein = analyse_program(img, functions, **kw)
        print("  translate_psp: syscall rule applied %d times" % SYSCALL_SEEN["n"], flush=True)
        if functions and len(functions) > 1000 and not SYSCALL_SEEN["n"]:
            raise SystemExit("translate: the syscall liveness rule never fired - argument_livein not replaced?")
        n = 0
        for a in self.hooks:
            if a in livein and livein[a] != ARGS:
                livein[a] = ARGS
                n += 1
        print("  translate_psp: %d hooked functions given full argument liveness" % n, flush=True)
        t0 = time.time()
        wa, wr, rounds = wide_sets(img, functions, livein, set(res.wide_args), set(res.wide_rets) | self.main_wide_rets)
        add_r, add_a = sorted(wr - set(res.wide_rets)), sorted(wa - set(res.wide_args))
        for a in add_r:
            res.wide_rets[a] += 1
        for a in add_a:
            res.wide_args[a] += 1
        if len(functions) > 1000:
            self.main_wide_rets.update(wr)
        print("  translate_psp: wide fixpoint (%d rounds, %.0f s): wide_rets %d (+%d), wide_args %d (+%d); new rets: %s"
              % (rounds, time.time() - t0, len(wr), len(add_r), len(wa), len(add_a),
                 " ".join("%08x" % a for a in add_r[:30])), flush=True)
        return res, livein


# ------------------------------------------------------------------ the translation
def translate(p, variant, out, functions=None, use_exempt=False, overlays=False, vfloc_skip=None, psp_rules=True,
              rlive=False):
    if variant not in ("v0", "v4") and variant not in V5_STEPS:
        raise SystemExit("unknown variant %s" % variant)
    if use_exempt and variant == "v0":
        raise SystemExit("--exempt is for the ABI-mode variants (v0 is exact everywhere)")
    unit = p.get("translate.unit", "main")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith((".c", ".h")):
            os.remove(os.path.join(out, f))
    vskip = []
    if vfloc_skip:                                        # functions kept off lever vfloc (one C name a line)
        vskip = [ln.split()[0] for ln in open(vfloc_skip, encoding="utf-8") if ln.strip() and not ln.startswith("#")]
        backend_c.VFLOC_SKIP = frozenset(vskip)
    rules = _PspRules(hooked_addresses(p.path("build.hooks"))) if psp_rules and variant != "v0" else None
    analyse = rules.analyse_program if rules else analyse_program
    disc = p.disc()
    img = load_elf(os.path.join(disc, p.get("id.boot_elf"))).stripped()
    code = [tuple(p.get("memory.code"))] + ([tuple(p.get("memory.ctor_code"))] if p.get("memory.ctor_code") else [])
    if functions and functions != "discover":
        funcs = pickle.load(open(functions, "rb"))
        fset = work_rel(functions)
    else:
        funcs = discover_functions(img, code, extra_seeds=[img.entry]).functions
        fset = "discovered"
    stats = {"variant": variant, "functions": len(funcs), "guest_insns": sum(len(f.addrs) for f in funcs.values())}
    if fset != "discovered" or use_exempt:
        stats["function_set"] = fset
    window = tuple(p.get("overlays.window", (0, 0)))
    ex = exempt_sets(p.path("translate.non_abi")) if use_exempt else {}
    # exact starts the main code may transfer to: its own + (conservatively) those of every overlay in the window
    ex_main = set(ex.get("main", ())) | {a for u, s in ex.items() if u != "main" for a in s if window[0] <= a < window[1]}
    if use_exempt:
        missing = sorted(a for a in ex.get("main", ()) if a not in funcs)
        if missing:
            raise SystemExit("non-ABI exact starts missing from the function set: %s" % ["%08x" % a for a in missing])
        stats["exempt"] = {"source": work_rel(p.path("translate.non_abi")),
                           "main": ["%08x" % a for a in sorted(ex.get("main", ()))]}
    t = time.time()
    ovs = []                                         # (name, ov, oimg, ofs, constructors outside .text)
    if overlays:
        for name in p.get("overlays.files", []):
            ov, oimg, ofs, ctors = overlay_unit(img, os.path.join(disc, name))
            own_ex = set(ex.get(name, ()))
            if use_exempt:
                missing = sorted(a for a in own_ex if a not in ofs.functions)
                if missing:
                    raise SystemExit("non-ABI exact starts missing from %s: %s" % (name, missing))
            ovs.append((name, ov, oimg, ofs, ctors))
    plan, livein, oanal, osets = {}, None, {}, {}
    if variant != "v0":
        res, livein = analyse(img, funcs, progress=progress("width"), roots={img.entry})
        for name, _ov, oimg, ofs, _c in ovs:
            oanal[name] = analyse(oimg, ofs.functions, extern_wide_rets=set(res.wide_rets), extern_livein=livein)
        main_wa, main_wr, osets, from_ov = wide_fixpoint(res, {n: r for n, (r, _l) in oanal.items()}, funcs)
        wc = res.closure
        stats["wide"] = {"main": {"wide_args": len(main_wa), "wide_rets": len(main_wr),
                                  "wide_arg_registers": sum(bin(m).count("1") for m in main_wa.values()),
                                  "from_overlays": ["%08x" % a for a in sorted(from_ov)],
                                  "indirect_calls_noncanonical_arg": len(wc.indirect_args) if wc else None},
                         "overlays": {n: {"wide_args": len(osets[n][0]), "wide_rets": len(osets[n][1])}
                                      for n in osets}}
        say("wide_args %d, wide_rets %d (+%d from overlay calls)" % (len(main_wa), len(main_wr), len(from_ov)))
        wide_args, wide_rets = dict(main_wa), frozenset(main_wr)
        wide_args[img.entry] = ARGS
        res = wc = None
        say("analysis %.0f s; narrowing ..." % (time.time() - t))
        starts = set(funcs)
        pr = progress("narrow")
        from ps2rc.pipeline import parallel
        if parallel.jobs() > 1:                       # PS2RC_TRANSLATE_JOBS: the same plans from a process pool
            plan = parallel.narrow_all(img, funcs, starts, livein, wide_args, wide_rets, ex.get("main", ()),
                                       parallel.jobs())
            pr(len(funcs), len(funcs))
        else:
            for k, (a, fn) in enumerate(sorted(funcs.items())):
                plan[a] = narrowable(img, fn, starts, livein, wide_args, wide_rets, abi=a not in ex.get("main", ()))
                if k % 1000 == 0:
                    pr(k, len(funcs))
        stats["narrowed_registers"] = sum(len(v) for v in plan.values())
    rl_info = {}
    has_rl = "rlive" in V5_STEPS.get(variant, ())
    if rlive and not has_rl:
        raise SystemExit("--rlive needs a variant with the rlive lever (v5s4y2r, v5s4y3r, v5s4y3rx)")
    if has_rl and not rlive:
        raise SystemExit("variant %s has the rlive lever: pass --rlive (or [translate] rlive = true)" % variant)
    if rlive:
        t2 = time.time()
        full, taken = rlive_sets(p, img, funcs, [(oimg, list(ofs.functions.values())) for _n, _o, oimg, ofs, _c in ovs])
        backend_c.RLIVE.update(on=True, units=(unit,), full=full, indirect=taken)
        rl_info.update(full=len(full), address_taken=len(taken))
        print("  translate_psp: rlive %d functions with complete returns (%d address-taken), %.0f s"
              % (len(full), len(taken), time.time() - t2), flush=True)
    say("translating main code ...")
    t1 = time.time()
    files, unsupported, todo = emit_units(img, FunctionSet(funcs, []), unit, roots=None, per_unit=150,
                                          progress=progress("emit"),
                                          **variant_opts(variant, plan, livein, frozenset(ex_main)))
    write(out, files)
    bad = {a for a, _m in unsupported}
    stats["main"] = {"units": len(files) - 2, "translated": len(todo) - len(bad), "stubbed": len(bad),
                     "stubbed_guest_insns": sum(len(funcs[a].addrs) for a in bad),
                     "unsupported_mnemonics": dict(mnemonics(unsupported).most_common(40)),
                     "c_bytes": sum(len(x) for x in files.values()), "seconds": round(time.time() - t1)}
    say("main: %(translated)d translated, %(stubbed)d stubbed (%(stubbed_guest_insns)d guest insns), %(units)d units"
        % stats["main"])
    if overlays:
        external = {a: "fn_%08x" % a for a in funcs}
        stats["overlays"] = {}
        for name, ov, oimg, ofs, ctors in ovs:
            own_ex = set(ex.get(name, ()))
            oplan, olive = {}, None
            if variant != "v0":
                _ores, olive_own = oanal[name]
                olive = {**livein, **olive_own}
                owa, owr = osets[name]
                ostarts = set(ofs.functions) | set(funcs)
                for a, fn in ofs.functions.items():
                    oplan[a] = narrowable(oimg, fn, ostarts, olive, frozenset(owa) | set(ofs.functions),
                                          frozenset(owr), abi=a not in own_ex)
            tag = "ov%d" % ov.id
            ofiles, ounsup, otodo = emit_units(oimg, ofs, tag, roots=None, per_unit=150, prefix=tag + "_",
                                               dispatch="rc_dispatch", external=external, lookup=True,
                                               **variant_opts(variant, oplan, olive,
                                                              frozenset(own_ex | set(ex.get("main", ())))))
            write(out, weak_ctors(ofiles, ["%sfn_%08x" % (tag + "_", a) for a in ctors]))
            obad = {a for a, _m in ounsup}
            stats["overlays"][name] = {"id": ov.id, "functions": len(otodo), "stubbed": len(obad),
                                       "guest_insns": sum(len(f.addrs) for f in ofs.functions.values()),
                                       "stubbed_guest_insns": sum(len(ofs.functions[a].addrs) for a in obad),
                                       "unsupported_mnemonics": dict(mnemonics(ounsup).most_common(10)),
                                       "ctors_outside_text": ["%08x" % a for a in ctors]}
            if use_exempt:
                stats["overlays"][name]["exempt"] = ["%08x" % a for a in sorted(own_ex)]
            say("%s (%s): %d functions (%d static constructors outside .text), %d stubbed"
                % (name, tag, len(otodo), len(ctors), len(obad)))
            oanal.pop(name, None)
    if rules:
        stats["psp_liveness"] = {"syscall_reads_args": True, "hooks_full_args": len(rules.hooks), "wide_fixpoint": True,
                                 "hooks_no_lowonly_args": rules.lowonly["n"], "driver": "ps2rc/pipeline/translate.py"}
        print("  translate_psp: one-word arguments refused for %d hooked functions (ctx32)" % rules.lowonly["n"])
    if rl_info:
        rl_info["units"] = {u: {k: v for k, v in d.items()} for u, d in backend_c.RLIVE["stats"].items()}
        stats["rlive"] = rl_info
        backend_c.RLIVE["on"] = False
    if vskip:
        stats["vfloc_skip"] = sorted(vskip)
    json.dump(stats, open(os.path.join(out, "stats.json"), "w"), indent=1)
    if p.get("translate.stats_header"):
        write_stats_header(out, stats, p.get("translate.stats_header"), p.get("translate.stats_prefix", "TR"))
    say("total %.0f s; output %s" % (time.time() - t, out))
    return stats


def main(argv):
    def opt(flag, default=None):
        return argv[argv.index(flag) + 1] if flag in argv else default
    p = _pack.load(opt("--pack"))
    variant = opt("--variant", p.get("translate.variant", "v0"))
    out = os.path.abspath(opt("--out") or p.path("translate.out") or p.path("build.translation"))
    functions = opt("--functions") or p.path("translate.functions") or "discover"
    use_exempt = "--exempt" in argv or bool(p.get("translate.exempt", False))
    overlays = "--overlays" in argv or bool(p.get("translate.overlays", False))
    vfloc = opt("--vfloc-skip") or p.path("translate.vfloc_skip")
    rules = "--no-psp-rules" not in argv and bool(p.get("translate.psp_rules", True))
    rlive = "--rlive" in argv or bool(p.get("translate.rlive", False))
    say("translate: pack %s, variant %s, functions %s -> %s" % (p.name, variant, functions, out))
    translate(p, variant, out, functions, use_exempt, overlays, vfloc, rules, rlive)


if __name__ == "__main__":
    main(sys.argv[1:])
