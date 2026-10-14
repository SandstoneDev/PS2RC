"""Width analysis (Phase 3): which 64/128-bit R5900 operations can run as 32-bit operations.

Two dataflow analyses per function over the instruction-level CFG (delay slots in execution order, branch-likely
not-taken edges skip the slot, calls are edges with an ABI transfer):

forward  canonical (C32): the register's low 64 bits are the sign extension of its low 32 bits; blob: the register
         holds raw data loaded from memory; hi: its upper 64 bits hold a meaningful 128-bit value. sp-relative stack
         slots remember what was stored, so LQ/LD restores of saved registers keep their facts; HI/LO halves carry
         their own C32 bits.
backward demanded bits per register: L (bits 0..31), H (32..63 used as a number), U (64..127 used), C (32..63 only
         copied to memory by SD/SDL/SDR), Q (64..127 only copied to memory by SQ). Moves and bitwise ops pass the
         demand of their result through; DADDU/DSLL whose result is demanded only in L need only L of their inputs.

ABI hypothesis H, checked at every boundary (violations are reported, not assumed away): values crossing a call
boundary are canonical - arguments (live-in registers of the callee), return values v0/v1, callee-saved registers -
and no meaningful upper-64 bits are live across a call.

An explicitly wide instruction (ir.Op.cls) is *reducible* when a 32-bit register model executes it exactly:
  move64/cmove        sources canonical, or the result is demanded only in L
  move128             as move64, and the source's upper half meaningless or not demanded (U/Q)
  dalu, dsll*         result demanded only in L (the low word of the result depends on the low words of the inputs)
  dsrl*, dsrlv        canonical source and result demanded only in L
  dsra*, dsrav        canonical source (the result is canonical)
  sext pair           dsll32 x,y,0 + dsra32 x,x,0 (sign extension of the low word: the identity on 32-bit registers)
  zext pair           dsll32 x,y,0 + dsrl32 x,x,0 with the result demanded only in L
  ld, ldl/ldr, lq     result demanded only in L, C, Q (copied back to memory: split into word moves), or the loaded
                      value is a canonical value this function saved to that stack slot
  lwu                 result demanded only in L
  sd, sdl/sdr         data canonical (stored as two words) or raw data from a load (a copy)
  sq                  data raw (a copy), or canonical with a meaningless upper half
  mmi, cop2           never (128-bit SIMD / VU0 transfers need a different mechanism)
"""
import heapq
from collections import Counter
from dataclasses import dataclass, field

from ..ir import BITWISE, LiftError, lift, link_register
from ..r5900 import decode
from ..r5900.flow import flow


def _m(*regs):
    return sum(1 << r for r in regs)


SP, GP, RA = 29, 28, 31
CALLER_SAVED = _m(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 31)
CALLEE_SAVED = _m(16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30)
ARGS = _m(4, 5, 6, 7, 8, 9, 10, 11)
RETS = _m(2, 3)
ALL = 0xFFFFFFFF
L, H, U, C, Q, X = 1, 2, 4, 8, 16, 32
ZERO_DEM = (0, 0, 0, 0, 0, 0)
HL = {"hi0": 1, "lo0": 2, "hi1": 4, "lo1": 8}
WIDE_CLASSES = ("move64", "move128", "cmove", "sext", "zext", "dalu", "dshift", "ld", "ldlr", "lwu", "sd", "sdlr",
                "lq", "sq", "mmi", "cop2")


@dataclass
class WidthResult:
    """Per-instruction verdicts and ABI-hypothesis checks for one function (or a whole program, merged)."""
    total: Counter = field(default_factory=Counter)        # wide instructions per class
    reducible: Counter = field(default_factory=Counter)    # of which reducible
    why: Counter = field(default_factory=Counter)          # (class, reason) for the irreducible ones
    violations: Counter = field(default_factory=Counter)   # ABI hypothesis checks that failed, by kind
    examples: dict = field(default_factory=dict)           # (class, reason) / violation kind -> first address
    wide_args: Counter = field(default_factory=Counter)    # callee -> call sites passing a non-canonical argument
    wide_rets: Counter = field(default_factory=Counter)    # function -> returns with a non-canonical v0
    unknown_ops: int = 0
    unreached: int = 0
    verdict: dict = field(default_factory=dict)            # address -> (class, reducible, reason)
    extern_wide_args: Counter = field(default_factory=Counter)  # outside the unit -> callers passing non-canonical
    wide_arg_regs: dict = field(default_factory=dict)      # function -> argument registers arriving non-canonical
    closure: object = None                                 # analyse_program's WideClosure

    def merge(self, o, keep_verdicts=False):
        self.total.update(o.total)
        self.reducible.update(o.reducible)
        self.why.update(o.why)
        self.violations.update(o.violations)
        self.wide_args.update(o.wide_args)
        self.wide_rets.update(o.wide_rets)
        self.extern_wide_args.update(o.extern_wide_args)
        for k, v in o.examples.items():
            self.examples.setdefault(k, v)
        self.unknown_ops += o.unknown_ops
        self.unreached += o.unreached
        if keep_verdicts:
            self.verdict.update(o.verdict)


class _Fn:
    """Instruction-level CFG of one function, with lifted ops."""

    def __init__(self, img, fn, func_starts):
        self.start = fn.start
        self.addrs = sorted(fn.addrs)
        self.insn = {a: decode(img.word(a), a) for a in self.addrs}
        self.flow = {a: flow(i) for a, i in self.insn.items()}
        self.op = {}
        self.unknown = 0
        for a, i in self.insn.items():
            try:
                self.op[a] = lift(i)
            except LiftError:
                self.op[a] = None
                self.unknown += 1
        self.succ = {a: [] for a in self.addrs}   # (node, edge): edge None, ("call", target) or ("call", None)
        self.exits = {a: [] for a in self.addrs}  # ("ret",) or ("tail", target or None)
        slots = set()
        for a in self.addrs:
            if a in slots:
                continue
            fl = self.flow[a]
            if fl.has_delay_slot and a + 4 in self.insn:
                slots.add(a + 4)
        for a in self.addrs:
            if a in slots:
                continue
            self._edges(a, fl=self.flow[a], func_starts=func_starts, has_ds=a + 4 in slots)
        self.pred = {a: [] for a in self.addrs}
        for a, ss in self.succ.items():
            for s, e in ss:
                self.pred[s].append((a, e))

    def _to(self, frm, dst, edge=None):
        if dst in self.insn:
            self.succ[frm].append((dst, edge))
        elif edge is None:
            self.exits[frm].append(("tail", dst))
        else:                                       # a call at the very end: continues outside this function
            self.exits[frm].append(("tail", dst))

    def _edges(self, a, fl, func_starts, has_ds):
        k = fl.kind
        if not fl.has_delay_slot:
            if k in ("break", "eret", "invalid"):
                return
            self._to(a, a + 4)
            return
        ds = a + 4 if has_ds else a
        if has_ds:
            self.succ[a].append((ds, None))
        after = a + 8
        if k == "branch":
            self._to(ds, fl.target)
            self._to(a if fl.likely else ds, after)
        elif k == "jump":
            if self.insn[a].mnem == "j":
                self.exits[ds].append(("tail", fl.target))
            else:
                self._to(ds, fl.target)
        elif k == "call":
            self._call(ds, after, fl.target)
        elif k == "call_conditional":
            self._call(ds, after, fl.target)
            self._to(a if fl.likely else ds, after)
        elif k == "call_indirect":
            self._call(ds, after, None)
        elif k == "return":
            self.exits[ds].append(("ret",))
        elif k == "jump_indirect":
            self.exits[ds].append(("tail", None))   # jump tables are attached by the caller of _Fn (see below)
        else:
            self._to(ds, after)

    def _call(self, frm, after, target):
        if after in self.insn:
            self.succ[frm].append((after, ("call", target)))
        else:
            self.exits[frm].append(("tail", after))
            self.exits[frm].append(("call", target))

    def attach_jump_tables(self, tables):
        for jr, _table, targets in tables:
            ds = jr + 4 if jr + 4 in self.insn else jr
            self.exits[ds] = [e for e in self.exits[ds] if e != ("tail", None)]
            for t in targets:
                if t in self.insn:
                    self.succ[ds].append((t, None))
                    self.pred[t].append((ds, None))


# ---------------------------------------------------------------- forward: canonical / blob / hi / HI-LO / stack slots
def _mem_off(insn):
    for o in insn.ops:
        if o[0] == "mem":
            return o[2]
    return None


def _slot_get(slots, off, width):
    for s in slots:
        if s[0] == off and s[1] >= width:
            return s
    return None


def _slot_put(slots, off, width, entry):
    keep = tuple(s for s in slots if s[0] + s[1] <= off or off + width <= s[0])
    return tuple(sorted(keep + ((entry,) if entry else ())))


def _fwd(f, a, st):
    op, insn = f.op[a], f.insn[a]
    c32, blob, hi, hilo, slots = st
    if op is None:
        return st
    off = _mem_off(insn) if op.base == SP else None
    d = op.dst
    if d is not None:
        bit = 1 << d
        res_c = res_blob = False
        res_hi = bool(hi & bit) if op.dst_bits == 64 else False
        if op.hilo_r and op.mnem.startswith("mf"):
            res_c = all(hilo & HL[h] for h in op.hilo_r)
        elif op.out == "c32":
            res_c = True
        elif op.out == "pass":
            res_c = all(c32 >> r & 1 for r, _ in op.srcs)
            if op.cls in ("move64", "move128", "cmove"):
                res_blob = all(blob >> r & 1 or c32 >> r & 1 for r, _ in op.srcs) and \
                    any(blob >> r & 1 for r, _ in op.srcs)
            if op.cls == "move128":
                res_hi = any(hi >> r & 1 for r, _ in op.srcs)
        elif op.out == "blob":
            s = _slot_get(slots, off, op.dst_bits // 8 if op.cls == "lq" else 8) if off is not None else None
            if s is not None and op.cls in ("ld", "lq") and s[2]:
                res_c = True
                res_hi = bool(s[3]) if op.cls == "lq" else res_hi
            else:
                res_blob = True
                if op.cls == "lq":
                    res_hi = True
        if op.cls in ("mmi", "cop2") and op.dst_bits == 128:
            res_hi = True
        c32 = c32 | bit if res_c else c32 & ~bit
        blob = blob | bit if res_blob else blob & ~bit
        hi = hi | bit if res_hi else hi & ~bit
    lr = link_register(insn)
    if lr is not None and d is None:
        c32 |= 1 << lr
        blob &= ~(1 << lr)
    if op.hilo_w:
        v = op.hilo_out == "c32" or (op.hilo_out == "pass" and all(c32 >> r & 1 for r, _ in op.srcs))
        for h in op.hilo_w:
            hilo = hilo | HL[h] if v else hilo & ~HL[h]
    if op.store and off is not None:
        width = max(op.store // 8, 1)
        entry = None
        if op.mnem in ("sw", "sd", "sq"):
            r = op.srcs[0][0] if op.srcs else 0
            entry = (off, width, 1 if (r == 0 or c32 >> r & 1) else 0, hi >> r & 1 if r else 0,
                     blob >> r & 1 if r else 0)
        slots = _slot_put(slots, off, width, entry)
    if d == SP:
        if op.mnem in ("addiu", "daddiu") and insn.ops[1][1] == SP:
            k = insn.ops[2][1]
            slots = tuple((s[0] - k,) + s[1:] for s in slots)
        else:
            slots = ()
    return (c32 | 1, blob & ~1, hi & ~1, hilo, slots)


def _call_fwd(st):
    c32, blob, hi, hilo, slots = st
    return (c32 | CALLER_SAVED, blob & ~CALLER_SAVED, hi & ~CALLER_SAVED, 0xF, slots)


def _join(x, y):
    if x is None:
        return y
    if y is None:
        return x
    slots = tuple(s for s in x[4] if s in y[4])
    return (x[0] & y[0], x[1] & y[1], x[2] | y[2], x[3] & y[3], slots)


ENTRY_STATE = (ALL, 0, 0, 0xF, ())


def forward(f, entry=ENTRY_STATE, call=None):
    """Forward facts before every instruction. `call(state, target)` is the transfer over a call edge (default: the
    ABI hypothesis - caller-saved registers canonical, callee-saved kept)."""
    st_in = {a: None for a in f.addrs}
    st_in[f.start] = entry
    work = [f.start]
    pending = {f.start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = _fwd(f, a, st_in[a])
        for s, e in f.succ[a]:
            v = out if e is None else (_call_fwd(out) if call is None else call(out, e[1]))
            j = _join(st_in[s], v)
            if j != st_in[s]:
                st_in[s] = j
                if s not in pending:
                    pending.add(s)
                    heapq.heappush(work, s)
    return st_in


# ---------------------------------------------------------------- backward: demanded bits
_FLAGS = (L, H, U, C, Q, X)


def _flags(dem, r):
    b = 1 << r
    out = 0
    for k, m in enumerate(dem):
        if m & b:
            out |= _FLAGS[k]
    return out


def _src_demand(op, idx, r, bits, dd):
    m, cls = op.mnem, op.cls
    low = L | H | C | X
    if op.store and idx == 0:
        return {64: L | C, 128: L | C | Q}.get(op.store, L)
    if cls == "cmove":
        return L | H if idx == 1 else (dd & low) or L
    if cls == "move64" or m in BITWISE or (cls == "dshift" and op.out == "pass" and m in ("dsll", "dsrl")):
        return dd & low
    if cls == "move128":
        return dd
    if cls == "dalu":
        return L if not dd & (H | C | X) else L | H
    if cls == "dshift":
        if m in ("dsllv", "dsrlv", "dsrav") and idx == 1:
            return L
        if m in ("dsll", "dsllv"):
            return L if not dd & (H | C | X) else L | H
        if m == "dsll32":
            return L
        if m in ("dsrl32", "dsra32"):
            return X                                 # reads the high word only, as a word
        return L | H
    if m in ("lwr", "ldl", "ldr") and r == op.dst:
        return (dd & low) or L
    return {32: L, 64: L | H, 128: L | H | U}[bits]


def _bwd(f, a, dem):
    op, insn = f.op[a], f.insn[a]
    if op is None:
        return dem
    dem = list(dem)
    d = op.dst
    dd = 0
    if d is not None:
        b = ~(1 << d)
        dd = _flags(dem, d)
        for k in (0, 1, 3, 5):                       # low 64 bits written: L H C X
            dem[k] &= b
        if op.dst_bits == 128:
            dem[2] &= b
            dem[4] &= b
        else:
            dd &= L | H | C | X
    lr = link_register(insn)
    if lr is not None and d is None:
        b = ~(1 << lr)
        for k in (0, 1, 3, 5):
            dem[k] &= b
    if not (op.pure and d is not None and dd == 0):
        for idx, (r, bits) in enumerate(op.srcs):
            fl = _src_demand(op, idx, r, bits, dd)
            b = 1 << r
            for k, flag in enumerate(_FLAGS):
                if fl & flag:
                    dem[k] |= b
        if op.base is not None:
            dem[0] |= 1 << op.base
    if insn.mnem == "syscall":                       # the kernel returns in v0/v1, reads v1 and the arguments whole
        dem = [x & ~RETS for x in dem]
        dem[0] |= ARGS | V1
        dem[1] |= ARGS | V1
    return tuple(dem)


def _call_bwd(dem, args):
    keep = ~CALLER_SAVED
    dL, dH, dU, dC, dQ, dX = (x & keep for x in dem)
    return (dL | args | 1 << GP, dH | args, dU, dC, dQ, dX)


def _exit_demand(exits, livein):
    dL = dH = 0
    for e in exits:
        if e[0] == "ret":
            dL |= RETS | CALLEE_SAVED
            dH |= RETS | CALLEE_SAVED
        elif e[0] == "tail":
            args = livein.get(e[1], ARGS) if e[1] is not None else ARGS
            dL |= args | CALLEE_SAVED | 1 << RA
            dH |= args | CALLEE_SAVED
        elif e[0] == "call":
            args = livein.get(e[1], ARGS) if e[1] is not None else ARGS
            dL |= args
            dH |= args
    return (dL, dH, 0, 0, 0, 0)


def backward(f, livein):
    zero = ZERO_DEM
    exit_dem = {a: _exit_demand(f.exits[a], livein) if f.exits[a] else zero for a in f.addrs}
    out = dict(exit_dem)
    inn = {a: zero for a in f.addrs}
    order = sorted(f.addrs, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            o = exit_dem[a]
            for s, e in f.succ[a]:
                v = inn[s]
                if e is not None:
                    v = _call_bwd(v, livein.get(e[1], ARGS) if e[1] is not None else ARGS)
                o = tuple(x | y for x, y in zip(o, v))
            out[a] = o
            i = _bwd(f, a, o)
            if i != inn[a]:
                inn[a] = i
                changed = True
    return inn, out


# ---------------------------------------------------------------- classification
def _classify(f, a, st, dem, pair):
    op, insn = f.op[a], f.insn[a]
    cls = op.cls
    c32, blob, hi = st[0], st[1], st[2]
    d = op.dst
    dd = _flags(dem, d) if d is not None else 0
    if op.dst_bits == 64:
        dd &= L | H | C | X
    low_only = not dd & (H | C | X)
    vals = [r for r, _ in op.srcs if not (r == d and cls not in ("cmove",))] or [r for r, _ in op.srcs]
    canon = all(c32 >> r & 1 for r in vals)
    if pair in ("sext", "mask"):                     # sign-extension / mask of a word field: canonical result
        return ("sext" if pair == "sext" else "zext"), True, ""
    if pair == "zext":
        return "zext", low_only, "zext result used as 64-bit"
    if cls in ("move64", "cmove"):
        return cls, canon or low_only, "non-canonical value moved and used as 64-bit"
    if cls == "move128":
        src_hi = any(hi >> r & 1 for r in vals)
        ok = (canon or low_only) and (not src_hi or not dd & (U | Q))
        return cls, ok, "128-bit value moved" if src_hi else "non-canonical value moved and used as 64-bit"
    if cls == "dalu":
        return cls, low_only, "64-bit arithmetic result used as 64-bit"
    if cls == "dshift":
        m = op.mnem
        if op.out == "pass" and m in ("dsll", "dsrl"):
            return cls, canon or low_only, "shift by 0 of a non-canonical value"
        if m in ("dsll", "dsllv", "dsll32"):
            return cls, low_only, "64-bit left shift used as 64-bit"
        if m == "dsra32" or (m == "dsrl32" and insn.ops[2][1] > 0):
            return cls, True, ""                    # high-word extraction: a word move on a split value
        if m == "dsrl32":
            return cls, low_only, "zero-extended high word used as 64-bit"
        if m in ("dsra", "dsrav"):
            return cls, canon, "arithmetic shift of a non-canonical value"
        return cls, canon and low_only, "logical right shift of a 64-bit value"
    if cls in ("ld", "lq", "ldlr"):
        return cls, None, ""                         # decided with the state after the load (analyse_function)
    if cls == "lwu":
        return cls, low_only, "zero-extended word used as 64-bit"
    if cls in ("sd", "sdlr"):
        r = op.srcs[0][0] if op.srcs else 0
        ok = r == 0 or bool(c32 >> r & 1) or bool(blob >> r & 1)
        return cls, ok, "64-bit computed value stored"
    if cls == "sq":
        r = op.srcs[0][0] if op.srcs else 0
        ok = r == 0 or bool(blob >> r & 1) or (bool(c32 >> r & 1) and not hi >> r & 1)
        return cls, ok, "128-bit value stored" if (r and hi >> r & 1) else "64-bit computed value stored"
    return cls, False, "SIMD/VU0"


def _pairs(f, out, window=6):
    """dsll32 x,y,s ... dsra32/dsrl32 z,x,s2: a bit field of y's low word, sign-extended ('sext') or zero-extended
    ('mask' when s2 > 0, the result is canonical; 'zext' when s2 = 0). Unrelated straight-line instructions may sit in
    between (scheduling), the second half may sit in the delay slot of a (non-likely) branch, and it may write
    another register z when the intermediate x is dead afterwards.
    Returns {address of either instruction: (kind, address of the second)}."""
    res = {}
    for a in f.addrs:
        i = f.insn[a]
        if i.mnem != "dsll32" or not i.ops[0][1]:
            continue
        x = i.ops[0][1]
        b = a + 4
        for _ in range(window):
            if b not in f.insn or len(f.pred.get(b, ())) != 1:
                break
            j, op, fl = f.insn[b], f.op[b], f.flow[b]
            if j.mnem in ("dsra32", "dsrl32") and j.ops[1][1] == x and \
                    (j.ops[0][1] == x or not out[b][0] >> x & 1 and not out[b][1] >> x & 1 and
                     not out[b][3] >> x & 1 and not out[b][5] >> x & 1):
                s2 = j.ops[2][1]                     # bits (s2-s) .. (31-s) of y's low word, any s2
                kind = "sext" if j.mnem == "dsra32" else ("mask" if s2 else "zext")
                res[a] = res[b] = (kind, b)
                break
            if op is None or op.dst == x or op.base == x or any(r == x for r, _ in op.srcs):
                break
            if fl.kind != "none":
                if not fl.has_delay_slot or fl.likely or fl.kind in ("call", "call_indirect", "call_conditional"):
                    break
                b += 4                               # the delay slot executes before the branch takes effect
                continue
            b += 4
    return res


def analyse_function(img, fn, func_starts, livein, keep_verdicts=False):
    f = _Fn(img, fn, func_starts)
    f.attach_jump_tables(fn.jump_tables)
    return _analyse_fn(f, livein, keep_verdicts)


def _analyse_fn(f, livein, keep_verdicts=False):
    res = WidthResult(unknown_ops=f.unknown)
    if f.start not in f.insn:                       # nothing decodable at the start: nothing to analyse
        return res
    st_in = forward(f)
    st_after = {a: _fwd(f, a, st_in[a]) for a in f.addrs if st_in[a] is not None}
    _inn, out = backward(f, livein)
    pairs = _pairs(f, out)
    for a in f.addrs:
        op = f.op[a]
        if op is None or not op.cls:
            continue
        if st_in[a] is None:
            res.unreached += 1
            continue
        kind, second = pairs.get(a, (None, a))        # a pair is judged by the demand on its final result
        cls, ok, why = _classify(f, second, st_in[second], out[second], kind)
        if cls in ("ld", "lq", "ldlr"):
            d = op.dst
            dd = _flags(out[a], d) if d is not None else 0
            restored = d is not None and bool(st_after[a][0] >> d & 1)
            src_hi = d is not None and bool(st_after[a][2] >> d & 1)
            if cls == "lq":
                ok = (not dd & H or restored) and (not dd & U or not src_hi)
                why = "128-bit data used as a value" if dd & U else "loaded doubleword used as 64-bit value"
            else:
                ok = not dd & H or restored             # X (high word as a word) and C (copy): split loads
                why = "loaded doubleword used as 64-bit value"
        res.total[cls] += 1
        if ok:
            res.reducible[cls] += 1
        else:
            res.why[(cls, why)] += 1
            res.examples.setdefault((cls, why), a)
        if keep_verdicts:
            res.verdict[a] = (cls, ok, "" if ok else why)
    _check_abi(f, st_in, st_after, out, livein, res)
    return res


def _check_abi(f, st_in, st_after, out, livein, res):
    """Hypothesis H at this function's boundaries: canonical args at calls, canonical v0/callee-saved at returns,
    callee-saved values and upper halves that live across calls."""
    for a in f.addrs:
        st = st_after.get(a)
        if st is None:
            continue
        c32, hi = st[0], st[2]
        for s, e in f.succ[a]:
            if e is None:
                continue
            args = livein.get(e[1], ARGS) if e[1] is not None else ARGS
            bad = args & ~c32
            if bad:
                kind = "call: non-canonical argument" + (" (indirect)" if e[1] is None else "")
                res.violations[kind] += bin(bad).count("1")
                res.examples.setdefault(kind, a)
                if e[1] is not None:
                    res.wide_args[e[1]] += 1
            # demand at the continuation: callee-saved registers that are live across the call
            din = _bwd(f, s, out[s]) if s in out else ZERO_DEM
            live_cs = (din[0] | din[1]) & CALLEE_SAVED
            if live_cs & ~c32:
                res.violations["call: non-canonical callee-saved live across"] += 1
                res.examples.setdefault("call: non-canonical callee-saved live across", a)
            if din[2] & CALLEE_SAVED & hi:
                res.violations["call: 128-bit callee-saved live across"] += 1
                res.examples.setdefault("call: 128-bit callee-saved live across", a)
        for e in f.exits[a]:
            if e[0] == "ret":
                if not c32 >> 2 & 1:
                    res.violations["return: non-canonical v0"] += 1
                    res.examples.setdefault("return: non-canonical v0", a)
                    res.wide_rets[f.start] += 1
                if CALLEE_SAVED & ~c32:
                    res.violations["return: non-canonical callee-saved"] += 1
                    res.examples.setdefault("return: non-canonical callee-saved", a)
            elif e[0] in ("tail", "call") and e[1] is not None and e[1] in livein:
                bad = livein[e[1]] & ~c32
                if bad:
                    res.violations["tail call: non-canonical argument"] += bin(bad).count("1")
                    res.examples.setdefault("tail call: non-canonical argument", a)
                    res.wide_args[e[1]] += 1


# ---------------------------------------------------------------- argument liveness (interprocedural)
V1 = 1 << 3


def _uses_defs(op, insn):
    """(registers read, registers written) of one instruction, for argument liveness. A `syscall` reads its call number
    in v1 and its arguments in a0-a3/t0-t3 (the EE kernel - the HLE - takes them from the context) and returns in
    v0/v1; lift() gives it no operands (without this rule every `li v1,N; syscall; jr ra` wrapper had an empty
    livein, and ABI-mode callers kept its arguments in C locals - a CreateThread wrapper then started a thread at
    string bytes)."""
    use = dfn = 0
    if op is not None:
        for r, _ in op.srcs:
            use |= 1 << r
        if op.base is not None:
            use |= 1 << op.base
        if op.dst is not None:
            dfn |= 1 << op.dst
    lr = link_register(insn)
    if lr is not None:
        dfn |= 1 << lr
    if insn.mnem == "syscall":
        use |= ARGS | V1
        dfn |= RETS
    return use, dfn


def _callers(fns):
    """{target: starts of the functions in `fns` (_Fn) with a direct call, tail call or branch edge onto it}."""
    rev = {}
    for a, f in fns.items():
        for x in f.addrs:
            for _s, e in f.succ[x]:
                if e is not None and e[1] is not None:
                    rev.setdefault(e[1], set()).add(a)
            for e in f.exits[x]:
                if e[0] in ("tail", "call") and e[1] is not None:
                    rev.setdefault(e[1], set()).add(a)
    return rev


def _fn_livein(f, livein):
    """Argument registers (a0-a3, t0-t3) `f` reads before writing them, given its callees' `livein` (calls through a
    pointer and calls of functions not in `livein` read all of them)."""
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
            use, dfn = _uses_defs(f.op[x], f.insn[x])
            o = (o & ~dfn) | use
            if o != live[x]:
                live[x] = o
                changed = True
    return live[f.start] & ARGS if f.start in live else ARGS


def argument_livein(img, functions, rounds=None):
    """{function start: argument registers (a0-a3, t0-t3) read before written - also by a syscall (_uses_defs)}, over
    the call graph so that wrappers passing their arguments on count them as live-in.

    A greatest fixpoint: every function starts as reading all of them and only the callers of a function whose set
    shrank are recomputed. Every step is an over-approximation of what is read (sound for the call write-backs of the
    ABI mode); `rounds` caps the number of steps (None: until nothing changes - before 2026-10-03 the default was 3
    rounds over all functions).
    v1 is not part of the result although a syscall reads it: in a large game 54 functions read a v1 they did not
    write, the 17 sources being garbage reads on default paths (`addiu v0,v1,69` after a switch that does not set v1)
    and crt0 (exact); a function that really takes an input in v1 is non-ABI, translated exact, and transfers
    to it write back every register (backend_c exempt)."""
    starts = set(functions)
    fns = {}
    for a, fn in functions.items():
        f = _Fn(img, fn, starts)
        f.attach_jump_tables(fn.jump_tables)
        fns[a] = f
    callers = _callers(fns)
    livein = {a: ARGS for a in functions}
    work, k = set(functions), 0
    while work and (rounds is None or k < rounds):
        k += 1
        new = {a: _fn_livein(fns[a], livein) for a in sorted(work)}      # one step from the previous values
        work = set()
        for a, v in new.items():
            if v != livein[a]:
                livein[a] = v
                work |= callers.get(a, set())
    return livein


# ---------------------------------------------------------------- 64-bit values passed through (whole program)
class WideClosure:
    """wide_args / wide_rets of one program unit as a joint least fixpoint over the forward lattice (the
    per-function check assumes canonical arguments at every entry and a canonical v0/v1 after every call, so a 64-bit
    value that is only passed through - an argument forwarded to a callee, a callee's result returned or tail-called -
    never marked anything: atof -> strtod -> _strtod_r of a C library return a double in v0, _f_ulltof forwards its
    64-bit a0 to __floatdisf).

    `wam` {function: argument registers that may arrive non-canonical} (`wa` = its keys): a function starts with those
    registers non-canonical, and after a call v0/v1 are non-canonical when the callee is in wide_rets, in
    `extern_wide_rets` (functions of another unit: the main program's for an overlay) or unknown (a call through a
    pointer - narrowable's own rule). Then a call or tail call passing a non-canonical register the target reads
    (`livein`) adds that register to the target's mask (to `extern_wam` when the target is not a function of this
    unit); a return with a non-canonical v0, and a tail call to a function of wide_rets (or through a pointer), put the
    function into wide_rets. Every function is analysed once, then only those whose inputs changed. Seeds:
    `wide_args` (an iterable: all argument registers, or {function: mask}), `wide_rets`.

    Calls through a pointer with a non-canonical argument cannot mark their unknown targets; they are collected in
    `indirect_args` (call sites) - the ABI hypothesis for them stays a checked-and-reported assumption (in a large
    game: 37 such sites under the per-function hypothesis, mostly scratch t0/t2 left over before virtual calls).
    Returns are judged by v0 only: there 999 functions leave a non-canonical v1, and no caller reads v1 after a call
    (the 18 call sites that seem to are `ldl v1 / ldr v1` pairs, which overwrite it whole)."""

    def __init__(self, fns, livein, wide_args=(), wide_rets=(), extern_wide_rets=()):
        self.fns, self.livein = fns, livein
        self.wam = {}
        for a, m in _masks(wide_args).items():
            self.wam[a] = self.wam.get(a, 0) | m
        self.wr, self.xwr = set(wide_rets), set(extern_wide_rets)
        self.extern_wa = {}              # target outside fns -> set of functions passing it a non-canonical argument
        self.extern_wam = {}             # target outside fns -> those argument registers
        self.indirect_args = set()       # (function, address) of calls through a pointer with a non-canonical argument
        self.visits = 0
        self.callers = _callers(fns)
        self._run(set(fns))

    @property
    def wa(self):
        return set(self.wam)

    def add(self, wide_args=(), extern_wide_rets=()):
        """New seeds (overlay -> main wide arguments, a grown extern_wide_rets); returns True when a set grew."""
        before = (dict(self.wam), len(self.wr), dict(self.extern_wam))
        work = set()
        for a, m in _masks(wide_args).items():
            if a in self.fns and m & ~self.wam.get(a, 0):
                self.wam[a] = self.wam.get(a, 0) | m
                work.add(a)
        new_x = set(extern_wide_rets) - self.xwr
        self.xwr |= new_x
        for t in new_x:
            work |= self.callers.get(t, set())
        self._run(work)
        return before != (self.wam, len(self.wr), self.extern_wam)

    def _visit(self, a):
        f = self.fns[a]
        livein, wr, xwr = self.livein, self.wr, self.xwr
        if f.start not in f.insn:
            return [], False

        def call(st, target):
            c32, blob, hi, hilo, slots = _call_fwd(st)
            if target is None or target in wr or target in xwr:
                c32 &= ~RETS
            return (c32, blob, hi, hilo, slots)
        st_in = forward(f, (ALL & ~self.wam.get(a, 0), 0, 0, 0xF, ()), call)
        marks, ret_wide = [], False
        for x in f.addrs:
            if st_in[x] is None:
                continue
            c32 = _fwd(f, x, st_in[x])[0]
            edges = [e for _s, e in f.succ[x] if e is not None] + [e for e in f.exits[x] if e[0] != "ret"]
            for e in edges:
                t = e[1]
                if t is None:
                    if ARGS & ~c32:
                        self.indirect_args.add((a, x))
                    if e[0] == "tail":
                        ret_wide = True                  # returns what an unknown function returns
                    continue
                bad = livein.get(t, ARGS) & ARGS & ~c32
                if bad:
                    marks.append((t, bad))
                if e[0] == "tail" and (t in wr or t in xwr):
                    ret_wide = True
            if any(e[0] == "ret" for e in f.exits[x]) and not c32 >> 2 & 1:
                ret_wide = True
        return marks, ret_wide

    def _run(self, work):
        while work:
            nxt = set()
            for a in sorted(work):
                self.visits += 1
                marks, ret_wide = self._visit(a)
                for t, bad in marks:
                    if t not in self.fns:
                        self.extern_wa.setdefault(t, set()).add(a)
                        self.extern_wam[t] = self.extern_wam.get(t, 0) | bad
                    elif bad & ~self.wam.get(t, 0):
                        self.wam[t] = self.wam.get(t, 0) | bad
                        nxt.add(t)
                if ret_wide and a not in self.wr:
                    self.wr.add(a)
                    nxt |= self.callers.get(a, set())
            work = nxt & set(self.fns)


def wide_arg_masks(res, roots=()):
    """narrowable's wide_args as {function: argument registers}: the closure's registers (res.wide_arg_regs), all of
    them for every other member of res.wide_args (e.g. added by a driver's own rule) and for every root."""
    m = {a: res.wide_arg_regs.get(a, ARGS) for a in res.wide_args}
    for r in roots:
        m[r] = ARGS
    return m


def _masks(wide_args):
    """{function: argument registers} from {function: mask} or an iterable of functions (all argument registers)."""
    if isinstance(wide_args, dict):
        return {a: m & ARGS for a, m in wide_args.items()}
    return {a: ARGS for a in wide_args}


def analyse_program(img, functions, keep_verdicts=False, progress=None, extern_wide_rets=(), roots=(),
                    closure=True, extern_livein=None):
    """Width analysis of every function; returns (merged WidthResult, livein).

    closure (default): res.wide_args / res.wide_rets also hold what passing values through adds (WideClosure: one
    count for each function it added; res.wide_arg_regs: which argument registers may arrive non-canonical), seeded
    with `roots` (entry points called with unknown arguments: all argument registers); `extern_wide_rets` are the
    wide_rets of the code this unit calls into (an overlay: the main program's), `extern_livein` that code's argument
    liveness (which registers a call there passes). Calls of functions outside the unit return canonical values
    unless listed in extern_wide_rets.
    res.extern_wide_args: functions outside `functions` called with a non-canonical argument (an overlay passing a
    64-bit value to main code); res.closure: the WideClosure (its sets and statistics). The returned livein covers
    `functions` only."""
    livein = argument_livein(img, functions)
    full = {**extern_livein, **livein} if extern_livein else livein
    starts = set(functions)
    total = WidthResult()
    fns = {} if closure else None
    for k, (a, fn) in enumerate(sorted(functions.items())):
        f = _Fn(img, fn, starts)
        f.attach_jump_tables(fn.jump_tables)
        total.merge(_analyse_fn(f, full, keep_verdicts), keep_verdicts)
        if closure:
            fns[a] = f
        if progress and k % 1000 == 0:
            progress(k, len(functions))
    if closure:
        wc = WideClosure(fns, full, set(roots), set(total.wide_rets), extern_wide_rets)
        # the closure's first pass includes the per-function check (which also names targets outside the unit)
        assert all(a in wc.wam or a in wc.extern_wam for a in total.wide_args)
        for a in sorted(wc.wa - set(total.wide_args)):
            total.wide_args[a] += 1
        total.wide_arg_regs = dict(wc.wam)
        for a in sorted(wc.wr - set(total.wide_rets)):
            total.wide_rets[a] += 1
        for t, frm in wc.extern_wa.items():
            total.extern_wide_args[t] += len(frm)
        total.closure = wc
    return total, livein


ENTRY_UNKNOWN = (1 | 1 << SP | 1 << GP | 1 << RA, 0, 0, 0, ())   # only $zero, sp, gp, ra known canonical


def _is_ctx_op(op):
    return op.dst_bits == 128 or any(b == 128 for _, b in op.srcs)


def narrowable(img, fn, func_starts, livein, wide_args=frozenset(), wide_rets=frozenset(), abi=False):
    """GPRs that `fn` can keep as 32-bit values in back-end v2 (int32_t locals, sign-extended when read, the context
    keeping the exact 64/128-bit value of everything the function has not written).

    Sound without the ABI hypothesis at entry: entry values are unknown; a register qualifies when every value the
    function itself writes into it (outside 128-bit instructions, which run on the context) is canonical and every
    read of more than its low word sees a canonical value - except reads by 128-bit instructions of a register the
    function has not written since the last context sync (those read the exact context value). After a direct call to
    a function not known to return a non-canonical v0 (`wide_rets`, from the whole-program check), v0/v1 are
    canonical; after calls through a pointer they are unknown. `livein`/`wide_args` are accepted for symmetry.

    abi=True (back-end v4, ABI-observable equivalence): entry values follow the ABI - arguments canonical unless the
    function receives a non-canonical argument somewhere (`wide_args`: a set - all argument registers - or {function:
    argument registers}, analyse_program's wide_arg_regs), callee-saved and scratch registers hold values
    the function may not interpret beyond saving them; after a call, caller-saved registers other than v0/v1 keep
    their (not reloaded) local values."""
    import heapq
    f = _Fn(img, fn, func_starts)
    f.attach_jump_tables(fn.jump_tables)
    if fn.start not in f.insn or f.unknown:
        return frozenset()

    def call(st, target):
        c32, blob, hi, hilo, slots = st
        keep = ~CALLER_SAVED if not abi else ~RETS
        c32 = (c32 & keep) | 1 << RA
        if target is not None and target not in wide_rets:
            c32 |= RETS
        return (c32, blob & keep, hi & keep, 0, slots)

    entry = ENTRY_UNKNOWN
    if abi:
        m = wide_args.get(fn.start, 0) & ARGS if isinstance(wide_args, dict) else (ARGS if fn.start in wide_args else 0)
        c32 = ALL & ~m
        entry = (c32, 0, 0, 0, ())
    st_in = forward(f, entry, call)
    # registers written since the last context sync (same rules as the code generator)
    din = {a: None for a in f.addrs}
    din[f.start] = 0
    work, pending = [f.start], {f.start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        op, insn = f.op[a], f.insn[a]
        d = din[a]
        g = 0 if op is None or op.dst is None else 1 << op.dst
        lr = link_register(insn)
        if lr:
            g |= 1 << lr
        clean = 0
        if op is not None and _is_ctx_op(op):
            for r, _ in op.srcs:
                clean |= 1 << r
            if op.dst:
                clean |= 1 << op.dst
        out = (d | g) & ~clean
        for t, e in f.succ[a]:
            v = ((0xFF << 16 | 1 << 30) if abi else 0) if e is not None else out   # ABI mode: s0-s7/fp stale after calls
            new = v if din[t] is None else din[t] | v
            if new != din[t]:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    cand = set(range(1, 32)) - {26, 27}
    fatal = {"break", "syscall", "eret", "trap"}
    for a in f.addrs:
        st = st_in[a]
        if st is None:
            continue
        op, insn = f.op[a], f.insn[a]
        c32 = st[0]
        ctx = _is_ctx_op(op)
        dirty = din[a] or 0
        after = _fwd(f, a, st)
        for r, bits in op.srcs:
            if bits > 32 and not c32 >> r & 1 and (not ctx or dirty >> r & 1):
                cand.discard(r)
        if op.dst is not None and not ctx and not after[0] >> op.dst & 1:
            cand.discard(op.dst)
        # a (may-)dirty narrowed register is written back to the context sign-extended: at every write-back point
        # its value must be canonical on every path, also on paths where the function did not write it
        if ctx:                                            # sync of the registers a 128-bit instruction touches
            touched = {r for r, _ in op.srcs} | ({op.base} if op.base else set()) | ({op.dst} if op.dst else set())
            cand -= {r for r in touched if dirty >> r & 1 and not c32 >> r & 1}
        if f.flow[a].kind in fatal or insn.mnem in ("mfc0", "mtc0", "di", "ei"):
            cand -= {r for r in range(32) if dirty >> r & 1 and not c32 >> r & 1}
        leaves = bool(f.exits[a]) or any(e is not None for _t, e in f.succ[a])
        if leaves:                                         # call / return / tail call: flush after this instruction
            g = 0 if op.dst is None else 1 << op.dst
            lr = link_register(insn)
            if lr:
                g |= 1 << lr
            out_dirty = dirty | g
            cand -= {r for r in range(32) if out_dirty >> r & 1 and not after[0] >> r & 1}
    return frozenset(cand)


def narrowing_plan(img, functions, starts=None, abi=False, roots=()):
    """{function start: narrowable GPRs} for a set of functions (back-end v2; abi=True: v4), with the ABI exceptions
    computed over the same set (analyse_program: with the closure over values passed through). `roots` (entry points
    / JALR targets called from outside the analysed code) count as receiving non-canonical arguments in ABI mode, and
    so do the functions they pass those arguments on to."""
    starts = set(starts) if starts is not None else set(functions)
    res, livein = analyse_program(img, functions, roots=roots)
    wide_args, wide_rets = wide_arg_masks(res, roots), frozenset(res.wide_rets)
    return {a: narrowable(img, fn, starts, livein, wide_args, wide_rets, abi) for a, fn in functions.items()}


def argument_plan(img, functions):
    """{function start: argument registers it reads} (interprocedural), for ABI-mode call write-backs."""
    return argument_livein(img, functions)
