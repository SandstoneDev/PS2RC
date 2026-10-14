"""Conversion step: which functions (main code + the code overlays) the ABI-mode back-end may translate, and which
must stay exact (ps2rc.analysis.abi).

    python convert/non_abi.py

Reads the functions file of convert/discover.py (main code); the overlays are discovered here. Writes the pack's
[translate] non_abi file:
  {"counts": {...},
   "functions": {"<unit>:<start>": {"reasons": [...], "details": {...}, "info": {...}, "insns": n}},  # must be exact
   "noted": {"<unit>:<start>": {...}},   # ABI-safe functions with observations
   "exact_starts": {"<unit>": [start, ...]},                                   # the set for the back-end
   "j_only_targets": {...}, "address_taken_exact": [...]}"""
import collections
import json
import os
import struct
import time

from common import OVERLAY_WINDOW, PACK, decode, discover_overlays, load_functions, main_image, say
from ps2rc.analysis.abi import Unit, analyse

OUT_JSON = PACK.path("translate.non_abi")


def key_name(k):
    return "%s:%08x" % k


def references(img, code_ranges, starts):
    """{start: ['data' | 'code']}: function starts whose address appears as a data word or is built by lui+addiu/ori."""
    refs = collections.defaultdict(set)
    inside = lambda a: any(lo <= a < hi for lo, hi in code_ranges)       # noqa: E731
    for seg in img.segments:
        data = seg.data[:seg.filesz]
        n = len(data) // 4
        for k, v in enumerate(struct.unpack("<%dI" % n, data[:n * 4])):
            if v in starts and not inside(seg.vaddr + 4 * k):
                refs[v].add("data")
    for lo, hi in code_ranges:
        for a in range(lo, hi, 4):
            i = decode(img.word(a), a)
            if i.mnem != "lui":
                continue
            r, hv = i.ops[0][1], i.ops[1][1]
            for b in range(a + 4, min(a + 36, hi), 4):
                j = decode(img.word(b), b)
                if j.mnem in ("addiu", "ori") and j.ops[1][1] == r:
                    v = (((hv << 16) + j.ops[2][1]) if j.mnem == "addiu" else ((hv << 16) | j.ops[2][1])) & 0xFFFFFFFF
                    if v in starts:
                        refs[v].add("code")
                    break
    return refs


def main():
    t0 = time.time()
    img, code = main_image()
    funcs = load_functions()
    units = [Unit("main", img, funcs)]
    ranges = {"main": code}
    for name, (oimg, ocode, ofs) in discover_overlays(img).items():
        units.append(Unit(name, oimg, ofs.functions))
        ranges[name] = ocode
    say("units: %s" % ", ".join("%s %d" % (u.name, len(u.functions)) for u in units))

    def progress(stage, k):
        if stage == "cfg" or k % 5 == 0:
            say("  %s %d (%.0f s)" % (stage, k, time.time() - t0))

    res = analyse(units, window=OVERLAY_WINDOW, progress=progress)
    exact = {k: v for k, v in res.items() if v.exact}
    by_reason, insns_by_reason = collections.Counter(), collections.Counter()
    for v in exact.values():
        for r in v.reasons:
            by_reason[r] += 1
            insns_by_reason[r] += v.insns
    words = collections.defaultdict(set)                 # distinct guest instruction words (functions overlap)
    exact_words = collections.defaultdict(set)
    for u in units:
        for s, fn in u.functions.items():
            words[u.name] |= fn.addrs
            if (u.name, s) in exact:
                exact_words[u.name] |= fn.addrs
    total_insns = sum(v.insns for v in res.values())
    exact_insns = sum(v.insns for v in exact.values())
    # starts reached by `j` (tail calls) but never by a call
    calls, jumps = collections.defaultdict(set), collections.defaultdict(set)
    for u in units:
        seen = set()
        for fn in u.functions.values():
            for a in fn.addrs:
                if a in seen:
                    continue
                seen.add(a)
                i = decode(u.img.word(a), a)
                if i.mnem in ("jal", "bgezal", "bltzal", "bgezall", "bltzall") and i.ops:
                    calls[u.name].add(i.ops[-1][1])
                elif i.mnem == "j":
                    jumps[u.name].add(i.ops[0][1])
    all_calls = set().union(*calls.values())
    j_only = {}
    for u in units:
        refs = references(u.img, ranges[u.name], set(u.functions))
        owners = collections.defaultdict(set)
        for s, fn in u.functions.items():
            for a in fn.addrs:
                owners[a].add(s)
        for t in sorted(jumps[u.name]):
            if t in all_calls or t not in u.functions:
                continue
            inner = sorted(owners[t] - {t})
            kind = ("inside %s" % ", ".join("%08x" % p for p in inner)) if inner else \
                ("address taken (%s)" % "+".join(sorted(refs[t]))) if refs.get(t) else "tail-called only"
            j_only[key_name((u.name, t))] = {"kind": kind, "exact": res[(u.name, t)].exact}
    taken = []
    for u in units:
        refs = references(u.img, ranges[u.name], {s for (n, s) in exact if n == u.name})
        taken += [key_name((u.name, s)) for s in sorted(refs) if (u.name, s) in exact]
    counts = {"functions": len(res), "guest_insns": total_insns,
              "distinct_guest_words": sum(len(w) for w in words.values()),
              "exact_functions": len(exact), "exact_guest_insns": exact_insns,
              "exact_distinct_words": sum(len(w) for w in exact_words.values()),
              "abi_safe_functions": len(res) - len(exact),
              "per_reason": {r: {"functions": by_reason[r], "guest_insns": insns_by_reason[r]}
                             for r in sorted(by_reason)},
              "per_unit": {u.name: {"functions": len(u.functions), "exact": sum(1 for k in exact if k[0] == u.name),
                                    "exact_guest_insns": sum(v.insns for k, v in exact.items() if k[0] == u.name)}
                           for u in units},
              "noted": dict(collections.Counter(n for v in res.values() if not v.exact for n in v.info)),
              "j_only_targets": dict(collections.Counter(
                  (x["kind"].split(" ")[0] + (" (exact)" if x["exact"] else "")) for x in j_only.values()))}
    out = {"counts": counts,
           "functions": {key_name(k): {"reasons": sorted(v.reasons), "details": v.reasons, "info": v.info,
                                       "insns": v.insns} for k, v in sorted(exact.items())},
           "noted": {key_name(k): v.info for k, v in sorted(res.items()) if not v.exact and v.info},
           "exact_starts": {u.name: ["%08x" % s for (n, s) in sorted(exact) if n == u.name] for u in units},
           "j_only_targets": j_only, "address_taken_exact": taken}
    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    say("functions %d (guest insns %d): must be exact %d (%d insns, %.3f %%)"
        % (len(res), total_insns, len(exact), exact_insns, 100.0 * exact_insns / max(total_insns, 1)))
    for r in sorted(by_reason):
        say("  %-16s %5d functions %8d insns" % (r, by_reason[r], insns_by_reason[r]))
    say("wrote %s (%.0f s)" % (OUT_JSON, time.time() - t0))


if __name__ == "__main__":
    main()
