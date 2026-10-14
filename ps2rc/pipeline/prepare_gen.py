"""PoC-6: build copy of the generated translation with the harness hooks applied (runs under WSL python3 or Windows).

    python3 prepare_gen.py <generated dir> <src/hle/hooks.def> <out dir> [<patch dir>]
                           [--trapset FILE] [--textpatch FILE] [--fix-mfc0] [--callflush fn_XXXXXXXX:REG ...]
                           [--zones src/hle/zones.def] [--sites sites.def]

Every function listed in hooks.def (HOOK / TRACE, main or overlay) has its generated definition renamed
`fn_<addr>__guest` (`ov<N>_fn_<addr>__guest`): calls and the dispatch tables keep using `fn_<addr>`, which the harness
defines. With a patch dir (build/poc6/patch from retranslate_stubs.py), its units are added and the original
definitions of the functions listed in its patched.txt are renamed `<name>__stub`, so the re-translated bodies take
over (a hook on such a function then renames the re-translated body). Every backward `goto` becomes
`{ RC_BACKEDGE(c); goto L_x; }` (a safe point every RC_BACKEDGE_PERIOD loop iterations).

Always generated: `rc_main_lookup()` appended to <unit>_tab.c, and hle_fnmap.c listing the functions that are defined
but in no dispatch table (e.g. overlay static constructors translated as extras) - hle/core/hle_main.c resolves call
targets outside the tables with them (no dlsym, so the same code works on the PSP). Also hle_zones.c: the profiler's
zone wrappers and zone table (empty without --zones; build 22, src/hle/hle_prof.h).

Options (the v4 / PSP build, tests/phase6psp/build.sh):
  --trapset FILE     functions (one name per line) whose memory accesses use the trapping accessors rtx_* / rmx_*
                     (patch_rt_header.py --mode psp|check): hardware registers reach src/hle/hle_mmio.c there; every
                     other function keeps the masked accessors (scratchpad and RAM mirrors alias through the mask).
  --textpatch FILE   lines "<function><TAB><old text><TAB><new text>": replaced inside that function's body, exactly
                     once (e.g. the RAM-end constant of a game's heap set-up -> hle_ram_end).
  --fix-mfc0         ABI-mode code keeps registers in C locals and does not reload the destination of an MFC0 after
                     rt_fatal(c, "mfc0", ...) wrote it into the context: add that reload (when the function has the
                     register as a local).
  --callflush F:R    before every direct call of F in ABI-mode code, store the local rR into the context: F reads rR as
                     a non-ABI input (e.g. Metrowerks __ptmf_scall takes the member pointer in t9 = r25).
  --native FILE      native fast paths. FILE lists generated functions
                     (fn_XXXXXXXX or the guest address, one a line, # comments) that the pack's C modules implement
                     as y_nat_XXXXXXXX(c): the definition keeps its name (and its zone rename) but its body becomes a
                     call of the native, the translated body is kept as <name>__ynguest (the native's check mode runs
                     it). Not for hooked / traced / patched functions. Callers, dispatch tables, argument liveness
                     and hooks.def are unchanged: the native reads and writes exactly what the translated body does.
  --zone-track       (host line-profiling builds only): the zone wrappers of the
                     timed (not sampled) zones also record the innermost zone per guest thread (y_lprof.h
                     y_zone_enter / y_zone_leave, zone id + 1), so y_lineprof.py's function entry counters split by
                     zone - the hardware profiler's "self" time of a zone in translated functions.
  --zones FILE       profiler zones (src/hle/zones.def: ZONE(addr, name, flags) / ZONE_OV(ov, addr, name, flags)): the
                     function's body is renamed <name>__zbody (<name>__guest__zbody with PZF_GUEST, which is required
                     exactly for HOOK/TRACE functions: the zone then times the guest original) and hle_zones.c defines
                     the wrapper <name> that times it through hle_pz_wrap (calls and dispatch tables are unchanged).
  --sites FILE       call sites routed through an HLE handler: SITE(<function>, <return address>, <handler>) lines
                     (8 hex digits each, C comments allowed). The call inside the translated function whose return
                     address (the r31 value the translation sets at the call) is <return address> calls
                     `<handler>(c, target)` instead of the target (an indirect call: the computed address; a direct
                     call: its address); the handler decides whether / when to run the target (rc_dispatch). Each
                     site must be found exactly once. Main program only.
Files are only rewritten when their content changes, so make rebuilds just the affected units. Fails when a listed
address is not a translated function or a text patch does not apply.
"""
import os
import re
import sys

ENTRY = re.compile(r"^\s*(HOOK|TRACE)(_OV)?\(\s*(?:(\d+)\s*,\s*)?([0-9a-f]{8})\s*,", re.M)
DEF = re.compile(r"^void ((?:ov\d+_)?fn_[0-9a-f]{8})\(R5900Ctx \*c\)$")
LABEL = re.compile(r"^L_([0-9a-f]{8}):;")
GOTO = re.compile(r"goto L_([0-9a-f]{8});")
DECL = re.compile(r"^    (int32_t|uint64_t) r\d+ = ")
DECL_NAME = re.compile(r"\b(r\d+) = ")
MFC0 = re.compile(r'(rt_fatal\(c, "mfc0", 0x[0-9a-f]+u, 0x([0-9a-f]+)u\);)')
ACCESSOR = re.compile(r"\b(rt|rm|rmb)_(lb|lh|lw|ld|sb|sh|sw|sd|lq|sq)\(")   # rmb_: back-end v5s4
# an access whose base register is gp (r28) or sp (r29) is game data / stack: always RAM, stays a plain access
GPSP_BASE = re.compile(r"(?:c|m_), \(uint32_t\)\((?:\(\(uint64_t\)\(int64_t\)r(?:28|29)\)|\(uint64_t\)r(?:28|29)\b|"
                       r"r(?:28|29)\b|c->r\[(?:28|29)\]\.d\[0\])"
                       # back-end v5s4 (maskbase): rmb_<op>(m_, (uint32_t)<base>, <offset>, ...)
                       r"|m_, \(uint32_t\)(?:\(\(uint64_t\)\(int64_t\)r(?:28|29)\)|r(?:28|29)\b)")
TABENTRY = re.compile(r"\{0x([0-9a-f]{8})u, ((?:ov\d+_)?fn_[0-9a-f]{8})\}")
ZONE = re.compile(r"^\s*ZONE(_OV)?\(\s*(?:(\d+)\s*,\s*)?([0-9a-f]{8})\s*,\s*(\w+)\s*,\s*([A-Z_0-9|\s]+)\)", re.M)
SITE = re.compile(r"^\s*SITE\(\s*([0-9a-f]{8})\s*,\s*([0-9a-f]{8})\s*,\s*([A-Za-z_]\w*)\s*\)", re.M)
SITE_DIRECT = re.compile(r"\b((?:ov\d+_)?fn_([0-9a-f]{8}))\(c\);")
ZFLAGS = {"0": 0, "PZF_GUEST": 1, "PZF_HW": 2, "PZF_SAMPLE": 4}      # src/hle/hle_prof.h


def instrument_backedges(text):
    """Backward gotos (target label already emitted in this function - labels come in address order) become
    `{ RC_BACKEDGE_AT(c, 0x<x>u); goto L_x; }` (see RC_BACKEDGE in the harness runtime header; the address goes to
    hle_where, sampled by the PSP watchdog - build 15). Returns (text, count)."""
    out, seen, n = [], set(), 0
    for line in text.split("\n"):
        if line.startswith("void "):
            seen = set()
        m = LABEL.match(line)
        if m:
            seen.add(m.group(1))
        elif "goto L_" in line:
            def rep(g):
                nonlocal n
                if g.group(1) in seen:
                    n += 1
                    return "{ RC_BACKEDGE_AT(c, 0x%su); goto L_%s; }" % (g.group(1), g.group(1))
                return g.group(0)
            line = GOTO.sub(rep, line)
        out.append(line)
    return "\n".join(out), n


def hook_names(defs_path):
    text = open(defs_path, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    names = set()
    for _kind, ov, ovid, addr in ENTRY.findall(text):
        names.add(("ov%s_" % ovid if ov else "") + "fn_" + addr)
    return names


def zone_list(path):
    """zones.def -> [(c name, zone name, overlay, address, flags)] in file order (= zone ids 0..n-1)."""
    if not path:
        return []
    text = re.sub(r"/\*.*?\*/", "", open(path, encoding="utf-8").read(), flags=re.S)
    zones, seen_c, seen_n = [], set(), set()
    for ov, ovid, addr, name, flags in ZONE.findall(text):
        f = 0
        for tok in flags.split("|"):
            tok = tok.strip()
            if tok not in ZFLAGS:
                sys.exit("zones.def: unknown flag %r for %s" % (tok, name))
            f |= ZFLAGS[tok]
        cname = ("ov%s_" % ovid if ov else "") + "fn_" + addr
        if cname in seen_c or name in seen_n:
            sys.exit("zones.def: %s / %s listed twice" % (cname, name))
        seen_c.add(cname)
        seen_n.add(name)
        zones.append((cname, name, int(ovid or 0), int(addr, 16), f))
    if len(zones) > 255:
        sys.exit("zones.def: %d zones, at most 255 (src/hle/hle_prof.h PZ_GUEST_MAX)" % len(zones))
    return zones


def zones_unit(zones, hooked, track=False):
    """gen/hle_zones.c: one wrapper per zone (the generated body was renamed <name>[__guest]__zbody) + the zone table.
    track (--zone-track): timed zones also record the innermost zone (y_lprof.h)."""
    lines = ["/* generated by tests/poc6/prepare_gen.py from src/hle/zones.def: profiler zone wrappers and table",
             "   (src/hle/hle_prof.h, src/hle/hle_prof.c) */", '#include "r5900_rt.h"', "",
             "extern volatile int hle_pz_on;",
             "void hle_pz_wrap(int z, void (*body)(R5900Ctx *), R5900Ctx *c);", ""]
    for i, (cname, _name, _ov, _addr, _f) in enumerate(zones):
        n = cname + ("__guest" if cname in hooked else "")
        lines.append("void %s__zbody(R5900Ctx *c);" % n)
        if track and not (_f & 4):                     # PZF_SAMPLE zones stay out of the tree (as on the PSP)
            lines.append("void %s(R5900Ctx *c) { unsigned char yz_ = y_zone_enter(%d); if (hle_pz_on) "
                         "hle_pz_wrap(%d, %s__zbody, c); else %s__zbody(c); y_zone_leave(yz_); }" % (n, i + 1, i, n, n))
            continue
        lines.append("void %s(R5900Ctx *c) { if (hle_pz_on) hle_pz_wrap(%d, %s__zbody, c); else %s__zbody(c); }"
                     % (n, i, n, n))
    lines.append("")
    lines.append("const int hle_pz_nguest = %d;" % len(zones))
    lines.append("const char *const hle_pz_gname[] = {%s};" % (", ".join('"%s"' % z[1] for z in zones) or "0"))
    lines.append("const uint32_t hle_pz_gaddr[] = {%s};" % (", ".join("0x%08xu" % z[3] for z in zones) or "0"))
    lines.append("const unsigned char hle_pz_gov[] = {%s};" % (", ".join(str(z[2]) for z in zones) or "0"))
    lines.append("const unsigned char hle_pz_gflags[] = {%s};" % (", ".join(str(z[4]) for z in zones) or "0"))
    return "\n".join(lines) + "\n"


def site_list(path):
    """sites.def -> {"fn_<function>": {return address: handler}}."""
    text = re.sub(r"/\*.*?\*/", "", open(path, encoding="utf-8").read(), flags=re.S)
    sites = {}
    for fn, ret, handler in SITE.findall(text):
        d = sites.setdefault("fn_" + fn, {})
        if int(ret, 16) in d:
            sys.exit("sites: %s %s listed twice" % (fn, ret))
        d[int(ret, 16)] = handler
    return sites


def apply_site(line, ret, handler):
    """The call of `line` that follows the translation's `r31 = ... (0x<ret>ull)` -> handler(c, target); None when the
    line has no such call."""
    key = "(0x%08xull)" % ret
    i = line.find(key)
    if i < 0 or "r31" not in line[max(0, i - 24):i]:
        return None
    head, tail = line[:i], line[i:]
    if "rc_dispatch(c, t_);" in tail:
        return head + tail.replace("rc_dispatch(c, t_);", "%s(c, t_);" % handler, 1)
    m = SITE_DIRECT.search(tail)
    if m:
        return head + tail[:m.start()] + "%s(c, 0x%su);" % (handler, m.group(2)) + tail[m.end():]
    return None


def read_list(path):
    out = set()
    for ln in open(path, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            out.add(ln.split()[0])
    return out


def read_textpatches(path):
    rules = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.rstrip("\n")
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        fn, old, new = ln.split("\t")
        rules.append((fn, old, new))
    return rules


class Ctx:
    def __init__(self, names, patched, trapset, patches, fix_mfc0, callflush):
        self.names, self.patched, self.trapset = names, patched, trapset
        self.patches = patches                     # fn -> [(old, new)]
        self.patch_hits = {}                       # (fn, old) -> count
        self.fix_mfc0, self.callflush = fix_mfc0, callflush
        self.native, self.native_found = set(), set()     # --native
        self.found, self.defined = set(), set()
        self.zones, self.zfound, self.zone_errors = {}, set(), []   # zones.def: c name -> flags
        self.sites, self.site_hits = {}, {}        # --sites: fn -> {ret: handler}; (fn, ret) -> count
        self.stats = {"backedges": 0, "trapped_fns": 0, "trapped_accesses": 0, "gpsp_plain": 0, "mfc0_reloads": 0,
                      "callflush": 0}


def process_c(text, is_patch, ctx):
    out = []
    cur = None
    seen = set()
    local_type = {}
    for line in text.split("\n"):
        m = DEF.match(line)
        if m:
            n = m.group(1)
            if n in ctx.patched and not is_patch:
                line = "void %s__stub(R5900Ctx *c)" % n
            else:
                hooked = n in ctx.names
                if hooked:
                    ctx.found.add(n)
                    line = "void %s__guest(R5900Ctx *c)" % n
                else:
                    ctx.defined.add(n)
                if n in ctx.zones:                 # profiler zone: the body becomes <name>[__guest]__zbody
                    if bool(ctx.zones[n] & 1) != hooked:
                        ctx.zone_errors.append("%s: PZF_GUEST %s" % (n, "missing (HOOK/TRACE function)" if hooked
                                                                     else "set but the function is not hooked/traced"))
                    ctx.zfound.add(n)
                    line = "void %s%s__zbody(R5900Ctx *c)" % (n, "__guest" if hooked else "")
                if n in ctx.native and not hooked and not is_patch:   # --native: the body calls the native
                    ctx.native_found.add(n)
                    a = n[-8:]
                    out += ["void y_nat_%s(R5900Ctx *c);" % a, line, "{", "    y_nat_%s(c);" % a, "}", ""]
                    line = "void %s__ynguest(R5900Ctx *c)" % n
            cur, seen, local_type = n, set(), {}
            if n in ctx.sites and not is_patch:      # --sites: the handlers' prototypes before the definition
                out += ["void %s(R5900Ctx *c, uint32_t target);" % h for h in sorted(set(ctx.sites[n].values()))]
            if n in ctx.trapset:
                ctx.stats["trapped_fns"] += 1
            out.append(line)
            continue
        if line.startswith("void ") or line.startswith("rc_fn ") or line.startswith("static "):
            cur = None
        if cur is None:
            out.append(line)
            continue
        if DECL.match(line):
            typ = line.split()[0]
            for d in DECL_NAME.finditer(line):
                local_type[d.group(1)] = typ
        lab = LABEL.match(line)
        if lab:
            seen.add(lab.group(1))
        elif "goto L_" in line:
            def rep(g):
                if g.group(1) in seen:
                    ctx.stats["backedges"] += 1
                    return "{ RC_BACKEDGE_AT(c, 0x%su); goto L_%s; }" % (g.group(1), g.group(1))
                return g.group(0)
            line = GOTO.sub(rep, line)
        if cur in ctx.trapset:
            def acc(g, line=line):
                if GPSP_BASE.match(line, g.end()):
                    ctx.stats["gpsp_plain"] += 1
                    return g.group(0)
                ctx.stats["trapped_accesses"] += 1
                return "%sx_%s(%s" % (g.group(1), g.group(2), "c, " if g.group(1) in ("rm", "rmb") else "")
            line = ACCESSOR.sub(acc, line)
        if ctx.fix_mfc0 and 'rt_fatal(c, "mfc0"' in line:
            def reload(g):
                rt = (int(g.group(2), 16) >> 16) & 31
                name = "r%d" % rt
                if rt == 0 or name not in local_type:
                    return g.group(1)
                ctx.stats["mfc0_reloads"] += 1
                cast = "(int32_t)" if local_type[name] == "int32_t" else ""
                return "%s %s = %sc->r[%d].d[0];" % (g.group(1), name, cast, rt)
            line = MFC0.sub(reload, line)
        for fn, reg in ctx.callflush:
            call = "%s(c);" % fn
            name = "r%d" % reg
            if call in line and name in local_type and ("c->r[%d].d[0] = " % reg) not in line.split(call)[0]:
                line = line.replace(call, "c->r[%d].d[0] = (uint64_t)(int64_t)%s; %s" % (reg, name, call))
                ctx.stats["callflush"] += 1
        if not is_patch:
            for ret, handler in ctx.sites.get(cur, {}).items():
                new = apply_site(line, ret, handler)
                if new is not None:
                    ctx.site_hits[(cur, ret)] = ctx.site_hits.get((cur, ret), 0) + 1
                    line = new
        for old, new in ctx.patches.get(cur, ()):
            if old in line:
                ctx.patch_hits[(cur, old)] = ctx.patch_hits.get((cur, old), 0) + line.count(old)
                line = line.replace(old, new)
        out.append(line)
    return "\n".join(out)


def main_lookup(tab_text):
    m = re.search(r"rc_funcs\[(\d+)\]", tab_text)
    if not m or "rc_main_lookup" in tab_text:
        return tab_text
    n = int(m.group(1))
    # build 21: the HLE defines rc_dispatch (a direct-mapped cache, src/hle/hle_main.c) in front of this table search
    tab_text = tab_text.replace("void rc_dispatch(R5900Ctx *c, uint32_t a)\n",
                                "void rc_dispatch_table(R5900Ctx *c, uint32_t a)\n")
    return tab_text.rstrip("\n") + """
/* appended by tests/poc6/prepare_gen.py: table lookup without the fatal fallback (src/hle/hle_main.c) */
rc_fn rc_main_lookup(uint32_t a)
{
    unsigned lo = 0, hi = %d;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rc_funcs[m].a < a) lo = m + 1; else hi = m;
    }
    return lo < %d && rc_funcs[lo].a == a ? rc_funcs[lo].f : 0;
}
""" % (n, n)


def is_main_tab(name):
    """The dispatch-table unit of the main program: <unit>_tab.c that is not an overlay's (ov<N>_tab.c) or an extra
    unit's (extra_ov<N>_tab.c)."""
    return name.endswith("_tab.c") and not re.match(r"(extra_)?ov\d+_tab\.c$", name)


def fnmap(extras):
    lines = ["/* generated by tests/poc6/prepare_gen.py: translated functions that are in no dispatch table */",
             '#include "r5900_rt.h"', "",
             "typedef struct { int ov; uint32_t addr; rc_fn fn; } HleExtraFn;"]
    lines += ["void %s(R5900Ctx *c);" % n for n in extras]
    lines.append("const HleExtraFn hle_extra_fns[] = {")
    for n in extras:
        m = re.match(r"(?:ov(\d+)_)?fn_([0-9a-f]{8})$", n)
        lines.append("    {%d, 0x%su, %s}," % (int(m.group(1) or 0), m.group(2), n))
    lines.append("    {0, 0, 0}")
    lines.append("};")
    lines.append("const int hle_n_extra_fns = %d;" % len(extras))
    return "\n".join(lines) + "\n"


def write_if_changed(dst, text):
    old = open(dst, encoding="ascii").read() if os.path.exists(dst) else None
    if old != text:
        open(dst, "w", encoding="ascii", newline="\n").write(text)
        return 1
    return 0


def main():
    argv = sys.argv[1:]
    opts = {"--trapset": None, "--textpatch": None, "--zones": None, "--native": None, "--sites": None}
    callflush = []
    fix_mfc0 = False
    zone_track = False
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in opts:
            opts[a] = argv[i + 1]
            i += 2
        elif a == "--callflush":
            fn, reg = argv[i + 1].split(":")
            callflush.append((fn, int(reg)))
            i += 2
        elif a == "--fix-mfc0":
            fix_mfc0 = True
            i += 1
        elif a == "--zone-track":
            zone_track = True
            i += 1
        else:
            pos.append(a)
            i += 1
    gen, defs, out = pos[:3]
    patch = pos[3] if len(pos) > 3 and os.path.isdir(pos[3]) else None
    os.makedirs(out, exist_ok=True)
    names = hook_names(defs)
    patched = set()
    if patch and os.path.exists(os.path.join(patch, "patched.txt")):
        patched = set(open(os.path.join(patch, "patched.txt"), encoding="ascii").read().split())
    trapset = read_list(opts["--trapset"]) if opts["--trapset"] else set()
    rules = read_textpatches(opts["--textpatch"]) if opts["--textpatch"] else []
    patches = {}
    for fn, old, new in rules:
        patches.setdefault(fn, []).append((old, new))
    ctx = Ctx(names, patched, trapset, patches, fix_mfc0, callflush)
    zones = zone_list(opts["--zones"])
    ctx.zones = {z[0]: z[4] for z in zones}
    if opts["--sites"]:
        ctx.sites = site_list(opts["--sites"])
    if opts["--native"]:
        for ln in open(opts["--native"], encoding="ascii"):
            ln = ln.split("#", 1)[0].strip()
            if ln:
                w = ln.split()[0]
                ctx.native.add(w if w.startswith("fn_") else "fn_%08x" % int(w, 16))
        bad = sorted(ctx.native & (names | patched))
        if bad:
            sys.exit("--native: hooked / traced / patched functions cannot be native: %s" % ", ".join(bad))
    changed = 0
    sources = [(gen, f, False) for f in sorted(os.listdir(gen)) if f.endswith((".c", ".h")) and f != "r5900_rt.h"
               and f not in ("hle_fnmap.c", "hle_zones.c")]
    if patch:
        sources += [(patch, f, True) for f in sorted(os.listdir(patch)) if f.endswith((".c", ".h"))]
    tabled = set()
    for d, f, is_patch in sources:
        text = open(os.path.join(d, f), encoding="ascii").read()
        if f.endswith(".c"):
            text = process_c(text, is_patch, ctx)
            tabled |= {n for _a, n in TABENTRY.findall(text)} if f.endswith("_tab.c") else set()
            if is_main_tab(f):                    # the main program's table (not an overlay's / extra unit's)
                text = main_lookup(text)
        changed += write_if_changed(os.path.join(out, f), text)
    extras = sorted(ctx.defined - tabled)
    changed += write_if_changed(os.path.join(out, "hle_fnmap.c"), fnmap(extras))
    changed += write_if_changed(os.path.join(out, "hle_zones.c"), zones_unit(zones, ctx.found, zone_track))
    keep = {f for _d, f, _p in sources} | {"hle_fnmap.c", "hle_zones.c"}
    for f in os.listdir(out):                         # units of an older patch set
        if f.endswith((".c", ".h")) and f not in keep:
            os.remove(os.path.join(out, f))
            changed += 1
    nmissing = sorted(ctx.native - ctx.native_found)
    if nmissing:
        sys.exit("--native lists functions that are not in the translation: %s" % ", ".join(nmissing))
    missing = sorted(names - ctx.found)
    if missing:
        sys.exit("hooks.def lists functions that are not in the translation: %s" % ", ".join(missing))
    zmissing = sorted(set(ctx.zones) - ctx.zfound)
    if zmissing or ctx.zone_errors:
        sys.exit("zones.def: not in the translation: %s; %s" % (", ".join(zmissing) or "-",
                                                                "; ".join(ctx.zone_errors) or "flags ok"))
    bad = ["%s: %r found %d times" % (fn, old, ctx.patch_hits.get((fn, old), 0)) for fn, old, _new in rules
           if ctx.patch_hits.get((fn, old), 0) != 1]
    if bad:
        sys.exit("text patches that did not apply exactly once: %s" % "; ".join(bad))
    sbad = ["%s at return address %08x: found %d times" % (fn, ret, ctx.site_hits.get((fn, ret), 0))
            for fn, d in sorted(ctx.sites.items()) for ret in sorted(d) if ctx.site_hits.get((fn, ret), 0) != 1]
    if sbad:
        sys.exit("call sites (--sites) that did not apply exactly once: %s" % "; ".join(sbad))
    s = ctx.stats
    print("prepare_gen: %d hooks applied, %d stubs replaced by re-translated bodies, %d loop back-edges counted, "
          "%d extra functions mapped, %d text patches, trap set %d functions (%d accesses trap, %d gp/sp-based stay "
          "plain), %d MFC0 reloads, %d call flushes, %d profiler zones, %d native fast paths, %d call sites; %d units "
          "+ 2 generated files, %d written or removed"
          % (len(ctx.found), len(patched), s["backedges"], len(extras), len(rules), s["trapped_fns"],
             s["trapped_accesses"], s["gpsp_plain"], s["mfc0_reloads"], s["callflush"], len(zones), len(ctx.native_found), len(ctx.site_hits),
             len(sources), changed))


if __name__ == "__main__":
    main()
