"""Return-value demand (back-end lever "rlive"): which parts of the EE return registers a function's callers
can observe, so that an ABI-mode function writes back at its returns only those.

A v5 ABI-mode function writes v0, v1 (+ their 128-bit upper halves), f0 and f1 back to the context at every return
whenever its own code wrote them (backend_c translate_function, flush kind "ret"): 2-3 instructions for v1, one store per
FPR and the sign word of v0 even where no caller ever reads them (v1 / f0 / f1 are mostly scratch registers). This
analysis computes, for every function G of one program, the bits

    V0L  bits 0..31 of v0        V0H  bits 32..63 of v0       V0Q  bits 64..127 of v0
    V1   v1 (all 128 bits)       F0   f0                       F1   f1

that some reader may observe after G returns:
  * the caller's own reads after the call site, up to a redefinition (backward, instruction-level CFG of
    analysis.width._Fn): a GPR read of v0 counts V0L, plus V0H when the caller's v0 local is not narrowed (a narrowed
    local reloads only the low word, backend_c), plus V0H|V0Q for a 128-bit read; any read of v1 counts V1;
  * values that reach the caller's return or one of its tail calls without a redefinition (the context slot keeps the
    callee's value): the caller's own demand (a fixpoint over the call graph);
  * a call through a pointer / an indirect tail call: every possibly indirectly called function gets the union of the
    demand of all indirect call sites (and of the functions that tail-call through a pointer);
  * conservatively FULL: what an unlifted instruction (syscall: v1 = call number; COP0, traps ...) or a transfer onto
    an exact (exempt) function may read, a call whose continuation leaves the function, exact callers, and the
    functions the driver names (`full`: hooked functions, natives, functions the HLE or other code units call, address
    taken functions - the HLE reads v0/v1 of handlers it dispatches; see tests/phase6psp/translate_psp.py --rlive).

Assumption (the ABI hypothesis of the v4/v5 back-end, the project notes): a caller does not read a return
register its callee did not define - after a call the translation already reloads v0/v1/f0/f1 from the context, so a
caller-saved register's value across a call is the context's, not the caller's own.
"""
from ..ir import link_register

V0L, V0H, V0Q, V1, F0, F1 = 1, 2, 4, 8, 16, 32
FULL = 63
_V0 = V0L | V0H | V0Q
_REG_BITS = {2: _V0, 3: V1}


def _fpr_masks(insn):
    from ..backend_c import _fpr_def, _fpr_reads
    c, r = _fpr_reads(insn)
    return c | r, _fpr_def(insn)


def site_summaries(f, narrow, exempt=frozenset(), exact=False):
    """[(target or None, use bits, pass bits)] for every call site of the function `f` (analysis.width._Fn / backend_c
    fn_cfg with jump tables attached), plus ("tail", target or None) entries (target None = through a pointer) for its
    tail calls. `narrow`: the function's final narrowed GPRs (backend_c._plan). use = bits the function itself (or an
    unknown reader) observes, pass = bits that flow on to the function's own callers (its returns / tail calls)."""
    if exact:                                            # an exact function writes back / reads everything
        out = []
        for x in f.addrs:
            for _t, e in f.succ[x]:
                if e is not None:
                    out.append((e[1], FULL, 0))
            for e in f.exits[x]:
                if e[0] == "call":
                    out.append((e[1], FULL, 0))
                elif e[0] == "tail":
                    out.append(("tail", e[1]))
        return out
    v0use = V0L if 2 in narrow else V0L | V0H
    # backward: live[x] = (use, pass) before instruction x; pass bits are kept in the upper half (<< 6)
    live = {x: 0 for x in f.addrs}
    order = sorted(f.addrs, reverse=True)
    sites = {}
    changed = True
    while changed:
        changed = False
        for x in order:
            insn, op = f.insn[x], f.op[x]
            o = 0
            for e in f.exits[x]:
                if e[0] == "ret":
                    o |= FULL << 6
                elif e[0] == "tail":
                    o |= FULL << 6                       # the tail callee returns to our caller
                    if e[1] is not None and e[1] in exempt:
                        o |= FULL
                elif e[0] == "call":                     # a call whose continuation is outside this function
                    sites[(x, "exit")] = (e[1], FULL, 0)
            for s, e in f.succ[x]:
                v = live[s]
                if e is not None:                        # call edge: s sees the callee's results
                    sites[(x, s)] = (e[1], v & FULL, v >> 6 & FULL)
                    v = FULL if e[1] is not None and e[1] in exempt else 0
                o |= v
            m = insn.mnem
            if (op is None and m != "syscall") or m in ("break", "eret", ".word") or \
                    m.startswith(("mfc0", "mtc0", "cop0", "t", "di", "ei", "bc0")):
                o |= FULL                                # rt_fatal / HLE / unknown reader: everything observable
            # GPR defs / uses (width._uses_defs semantics; syscall: reads v1 + args, returns v0/v1)
            use = dfn = 0
            if op is not None:
                for r, b in op.srcs:
                    if r in _REG_BITS:
                        use |= (_V0 if b == 128 else v0use) if r == 2 else V1
                if op.base in _REG_BITS:
                    use |= v0use if op.base == 2 else V1
                if op.dst in _REG_BITS:
                    dfn |= _REG_BITS[op.dst]
            lr = link_register(insn)
            if lr in _REG_BITS:
                dfn |= _REG_BITS[lr]
            if insn.mnem == "syscall":
                use |= V1
                dfn |= _V0 | V1
            fr, fd = _fpr_masks(insn)
            use |= (F0 if fr & 1 else 0) | (F1 if fr & 2 else 0)
            dfn |= (F0 if fd & 1 else 0) | (F1 if fd & 2 else 0)
            # a 32-bit def of v0 replaces bits 0..63 only; the upper half stays (q128 locals): keep V0Q alive
            if dfn & V0L and not (op is not None and op.dst_bits == 128):
                dfn &= ~V0Q
            o = (o & ~(dfn | dfn << 6)) | use
            if o != live[x]:
                live[x] = o
                changed = True
    out = list(sites.values())
    for x in f.addrs:
        for e in f.exits[x]:
            if e[0] == "tail":
                out.append(("tail", e[1]))
    return out


def solve(summaries, full=frozenset(), indirect=frozenset()):
    """Fixpoint {function: demand bits} over {function: site_summaries(...)}. full: functions whose returns stay as
    they are (FULL); indirect: functions that may be called through a pointer (they get the union of the indirect
    call sites' demand). Functions outside `summaries` never appear as keys of the result (callers treat a missing
    key as FULL)."""
    dem = {a: (FULL if a in full else 0) for a in summaries}
    for a in full:
        dem.setdefault(a, FULL)
    ind = 0
    changed = True
    while changed:
        changed = False
        nind = ind
        for a, sites in summaries.items():
            da = dem.get(a, FULL)
            for s in sites:
                if s[0] == "tail":
                    t = s[1]
                    if t is None:
                        nind |= da
                    elif t in dem and dem[t] | da != dem[t]:
                        dem[t] |= da
                        changed = True
                    continue
                t, use, pas = s
                d = use | (pas & da)
                if t is None:
                    nind |= d
                elif t in dem and dem[t] | d != dem[t]:
                    dem[t] |= d
                    changed = True
        if nind != ind:
            ind = nind
            changed = True
        for t in indirect:
            if t in dem and dem[t] | ind != dem[t]:
                dem[t] |= ind
                changed = True
    return dem, ind
