"""Copy the prepared generated units (prepare_gen.py output) into the build's
gen/ dir and apply a code-layout variant to the generated C. None of these changes the program's behaviour: they are
compiler hints (GCC attributes and branch probabilities) or host-only counters.

    python3 layout_apply.py <prepared dir> <gen dir> [--cold cold.txt [--split-cold]] [--branches branches.txt]
                            [--instrument map.txt]

  --cold FILE        the functions listed (one C name per line) get __attribute__((cold)): GCC optimises them for size,
                     places them in .text.unlikely.* and predicts the paths that call them as not taken
  --split-cold       the cold functions of every unit X.c go to X_cold.c (units hold nothing but independent global
                     function definitions after their #include lines), so that r_layout/layout.mk compiles them with
                     real -Os (layout.env COLD_EXTRA) instead of the cold attribute's partial size optimisation
  --branches FILE    profile-guided block layout from layout.py (main-loop counts of the host profile): in the
                     functions listed, every conditional branch `if (t_)` with a count becomes
                     `if (__builtin_expect_with_probability(t_, 1, p))` with its measured taken ratio p, every label
                     never reached becomes `L_x: __attribute__((cold));`, and a jump-table default that only leads to
                     rt_fatal("switch") moves behind a cold label - GCC then lays the hot path out contiguously and puts
                     the never-run blocks at the end of the function
  --instrument FILE  host profiling build only (r_layout/build_host.sh PROF=1): count every conditional branch
                     (taken / not taken) and every label of every generated function (r_layout/rprof.c dumps the
                     counts); FILE maps the counter ids to (function, ordinal) / (function, label)
Files are written only when their content changes (incremental make); units that no longer exist in the prepared dir
are removed. A cold or branch name that is not defined is an error (the list was made for another translation).
"""
import os
import re
import sys

DEF = re.compile(r"^void ((?:ov\d+_)?fn_[0-9a-f]{8}(?:__guest)?(?:__zbody)?(?:__stub)?)\(R5900Ctx \*c\)$")
LABEL = re.compile(r"^(L_[0-9a-f]{8}):;")
SW_DEFAULT = re.compile(r"default: (.*rt_fatal\(c, \"switch\", 0x[0-9a-f]+u, t_\); return; )\} \}$")
INSTR_HDR = """/* layout_apply.py --instrument: host-only branch / label counters (r_layout/rprof.c) */
extern unsigned long long rpb_b[][2], rpb_l[];
extern volatile int rpb_on;
#define RPB(i, t) ((t) ? ((rpb_on ? ++rpb_b[i][1] : 0), 1) : ((rpb_on ? ++rpb_b[i][0] : 0), 0))
#define RPL(i) ((void)(rpb_on ? ++rpb_l[i] : 0))
"""


def read_names(path):
    out = set()
    for ln in open(path, encoding="ascii"):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            out.add(ln.split()[0])
    return out


def read_branches(path):
    """branches.txt (layout.py): 'B fn k taken not_taken' and 'L fn label count' lines for main-loop functions."""
    br, lab = {}, {}
    for ln in open(path, encoding="ascii"):
        p = ln.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "B":
            br.setdefault(p[1], {})[int(p[2])] = (int(p[3]), int(p[4]))
        elif p[0] == "L":
            lab.setdefault(p[1], {})[p[2]] = int(p[3])
    return br, lab


class State:
    def __init__(self):
        self.nb = self.nl = 0
        self.map = []
        self.stats = {"cold": 0, "expect": 0, "cold_labels": 0, "cold_defaults": 0, "fns_annotated": 0}


def transform(text, cold, br, lab, instrument, st, found):
    out = []
    cur = None
    k = 0
    ndef = 0
    for line in text.split("\n"):
        m = DEF.match(line)
        if m:
            cur, k = m.group(1), 0
            if cur in cold:
                found.add(cur)
                st.stats["cold"] += 1
                line = "__attribute__((cold)) " + line
            if br is not None and cur in br:
                found.add(cur)
                st.stats["fns_annotated"] += 1
            out.append(line)
            continue
        if cur is None:
            out.append(line)
            continue
        if line == "}":
            cur = None
            out.append(line)
            continue
        if instrument:
            if "if (t_)" in line:
                def rb(_m):
                    nonlocal k
                    st.map.append("B %d %s %d" % (st.nb, cur, k))
                    s = "if (RPB(%d, t_))" % st.nb
                    st.nb += 1
                    k += 1
                    return s
                line = re.sub(r"if \(t_\)", rb, line)
            lm = LABEL.match(line)
            if lm:
                st.map.append("L %d %s %s" % (st.nl, cur, lm.group(1)))
                line = line.replace(lm.group(0), "%s:; RPL(%d);" % (lm.group(1), st.nl), 1)
                st.nl += 1
        elif br is not None and cur in br:
            fb, fl = br[cur], lab.get(cur, {})
            if "if (t_)" in line:
                def ab(_m):
                    nonlocal k
                    tn = fb.get(k)
                    k += 1
                    if not tn or tn[0] + tn[1] == 0:
                        return "if (t_)"
                    st.stats["expect"] += 1
                    return "if (__builtin_expect_with_probability(t_, 1, %.4f))" % (tn[0] / float(tn[0] + tn[1]))
                line = re.sub(r"if \(t_\)", ab, line)
            lm = LABEL.match(line)
            if lm and fl.get(lm.group(1), -1) == 0:
                st.stats["cold_labels"] += 1
                line = line.replace(lm.group(0), "%s: __attribute__((cold));" % lm.group(1), 1)
            sm = SW_DEFAULT.search(line)
            if sm:
                ndef += 1
                lab_name = "LD_%s_%d" % (cur, ndef)
                st.stats["cold_defaults"] += 1
                line = line[:sm.start()] + "default: goto %s; } %s: __attribute__((cold)); %s}" % (
                    lab_name, lab_name, sm.group(1))
        out.append(line)
    return "\n".join(out)


def split_cold(text, cold):
    """-> (hot text, cold text or None): the lines before the first definition go to both units."""
    hot, cld, prologue, cur_cold, seen_def = [], [], [], False, False
    for line in text.split("\n"):
        m = DEF.match(line[len("__attribute__((cold)) "):] if line.startswith("__attribute__((cold)) ") else line)
        if m:
            seen_def, cur_cold = True, m.group(1) in cold
        if not seen_def:
            prologue.append(line)
            continue
        (cld if cur_cold else hot).append(line)
        if line == "}":
            cur_cold = False
    if not cld:
        return text, None
    return ("\n".join(prologue + hot),
            "\n".join(prologue + ["/* cold functions (layout_apply.py --split-cold) */"] + cld) + "\n")


def main():
    args = sys.argv[1:]
    opts = {}
    split = "--split-cold" in args
    if split:
        args.remove("--split-cold")
    for key in ("--cold", "--branches", "--instrument"):
        if key in args:
            i = args.index(key)
            opts[key] = args[i + 1]
            del args[i:i + 2]
    src, dst = args[:2]
    cold = read_names(opts["--cold"]) if "--cold" in opts else set()
    br, lab = read_branches(opts["--branches"]) if "--branches" in opts else (None, None)
    instrument = "--instrument" in opts
    os.makedirs(dst, exist_ok=True)
    st, found = State(), set()
    extra = []
    written, names = 0, sorted(f for f in os.listdir(src) if f.endswith((".c", ".h")))
    for f in names:
        text = open(os.path.join(src, f), encoding="ascii").read()
        if f.endswith(".c") and (cold or br is not None or instrument):
            nb0 = st.nb
            text = transform(text, cold, br, lab, instrument, st, found)
            if instrument and (st.nb > nb0 or "RPL(" in text):
                text = INSTR_HDR + text
        outs = [(f, text)]
        if split and f.endswith(".c") and not f.endswith("_tab.c"):
            hot, cld = split_cold(text, cold)
            outs = [(f, hot)] + ([(f[:-2] + "_cold.c", cld)] if cld else [])
        for f2, t2 in outs:
            p = os.path.join(dst, f2)
            old = open(p, encoding="ascii").read() if os.path.exists(p) else None
            if old != t2:
                open(p, "w", encoding="ascii", newline="\n").write(t2)
                written += 1
            if f2 != f:
                extra.append(f2)
    if instrument:
        tab = ("/* layout_apply.py --instrument: counter tables */\nunsigned long long rpb_b[%d][2], rpb_l[%d];\n"
               "const int rpb_nb = %d, rpb_nl = %d;\n" % (max(st.nb, 1), max(st.nl, 1), st.nb, st.nl))
        p = os.path.join(dst, "lay_counters.c")
        if not os.path.exists(p) or open(p, encoding="ascii").read() != tab:
            open(p, "w", encoding="ascii", newline="\n").write(tab)
            written += 1
        extra.append("lay_counters.c")
        open(opts["--instrument"], "w", encoding="ascii", newline="\n").write("\n".join(st.map) + "\n")
    for f in os.listdir(dst):
        if f.endswith((".c", ".h")) and f not in names and f not in extra:
            os.remove(os.path.join(dst, f))
            written += 1
    missing = sorted(cold - found)
    if br is not None:
        missing += sorted(set(br) - found)
    if missing:
        sys.exit("layout_apply: %d names not defined in %s (first: %s)" % (len(missing), src, ", ".join(missing[:5])))
    s = st.stats
    print("layout_apply: %d units -> %d files (%d written or removed); %d functions cold; %d functions with a branch "
          "profile: %d branches annotated, %d cold labels, %d cold switch defaults%s" % (
              len(names), len(names) + len(extra), written, s["cold"], s["fns_annotated"], s["expect"], s["cold_labels"], s["cold_defaults"],
              "; instrumented %d branches, %d labels" % (st.nb, st.nl) if instrument else ""))


if __name__ == "__main__":
    main()
