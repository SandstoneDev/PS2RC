"""Function discovery for stripped R5900 executables (Phase 2b).

Seeds: entry point, direct call targets (jal / bal), tail-call targets (`j` - Metrowerks uses `beq zero,zero` for local
jumps and `j` only to leave the function), and data words that point at a plausible function start in the code ranges.
Each seed is explored by following control flow (delay slots, branch-likely); exploration stops at returns, tail calls,
invalid words and *strong* starts (the entry point, extra seeds, direct call targets: a branch or fall-through onto one
is a tail call into that function). Every other start (tail-call targets, data/code-address seeds, gap seeds) is weak:
an intra-function edge (branch, `beq zero,zero`, jump-table case, fall-through) that reaches it does not cut the
function - the function owns that code too. A weak start stays a function of its own (its own entry), so `addrs` sets
of different functions may overlap: a fragment entry owns a copy of the code it reaches in its parent, and a `j` into
the middle of a function (hand-written loops, e.g. crt0) gets a function that owns the code from there on.

`jr reg` is resolved as a switch when a jump table is found: backward evaluation of the load address (lui/addiu/ori +
sll index + addu) along one predecessor path (fall-through, else the only forward branch into the block, else the code
laid out above it), the bound from the guard `sltiu x, idx, N` + `beq/bne x, zero` whose in-range side leads to the jr
(the index followed through register copies and stack-slot reloads, before or after the scaling); a table without a
guard (Metrowerks omits it when the value range is known) runs over the consecutive code pointers up to the next
table, a word that is not one, or a direct call target. Otherwise the `jr` is an indirect tail call. Unowned code is re-seeded at its first non-padding word only if exploration from it is well formed
(no invalid words, ends in a return or tail call, does not run into another function's code) - code ranges also hold
VU microcode and data.
"""
import struct
from dataclasses import dataclass, field

from ..r5900 import decode
from ..r5900.flow import flow
from ..r5900.semantics import dest_gpr

NOP = 0
SP = 29
_BLOCK_END = ("branch", "jump", "jump_indirect", "return", "call", "call_indirect", "call_conditional", "eret")
_CALLS = ("call", "call_conditional", "call_indirect")
_CALLER_SAVED = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31)


@dataclass
class Function:
    start: int
    end: int = 0                  # exclusive
    addrs: set = field(default_factory=set)
    calls: set = field(default_factory=set)
    tail_calls: set = field(default_factory=set)
    jump_tables: list = field(default_factory=list)      # (jr_addr, table_addr, [targets])
    indirect_tail_calls: list = field(default_factory=list)
    well_formed: bool = True      # ended in return/tail call, met no invalid word


@dataclass
class FunctionSet:
    functions: dict
    code_ranges: list
    strong: set = field(default_factory=set)     # entry, extra seeds, direct call targets (exploration barriers)


class _Code:
    def __init__(self, img, ranges):
        self.img = img
        self.ranges = ranges
        self._cache = {}
        self._into = None

    def inside(self, a):
        return a is not None and a & 3 == 0 and any(lo <= a < hi for lo, hi in self.ranges)

    def word(self, a):
        return self.img.word(a)

    def get(self, a):
        r = self._cache.get(a)
        if r is None:
            i = decode(self.img.word(a), a)
            r = (i, flow(i))
            self._cache[a] = r
        return r

    def into(self):
        """{target: [addresses of the local branches (conditional, beq r,r) to it]} over the code ranges."""
        if self._into is None:
            m = {}
            for lo, hi in self.ranges:
                for a in range(lo, hi, 4):
                    i, fl = self.get(a)
                    if fl.kind == "branch" or (fl.kind == "jump" and i.mnem != "j"):
                        m.setdefault(fl.target, []).append(a)
            self._into = m
        return self._into


# ---------------------------------------------------------------- seeds
def _plausible_start(code, a):
    """Valid instruction right after a function end: `jr ra/j/jr` + its delay slot, then optional nop padding.
    A word that is itself a delay slot is never a start."""
    if not code.inside(a) or not code.get(a)[0].valid:
        return False
    if code.inside(a - 4) and code.get(a - 4)[1].has_delay_slot:
        return False
    b = a - 4
    while code.inside(b) and code.word(b) == NOP:
        b -= 4
    if not code.inside(b):
        return any(lo == a for lo, _ in code.ranges)
    if _ends_function(code, b) and code.inside(b + 4) and code.word(b + 4) == NOP:
        return True                     # end jump whose delay slot was a nop (skipped as padding)
    return code.inside(b - 4) and _ends_function(code, b - 4)


def _ends_function(code, a):
    """jr ra, j (Metrowerks tail call), jr reg, eret. A local `beq zero,zero` does not end a function."""
    i, fl = code.get(a)
    return fl.kind in ("return", "jump_indirect", "eret") or (fl.kind == "jump" and i.mnem == "j")


def _code_address_seeds(code):
    """Function addresses built in code: lui r,hi followed (within 8 instructions) by addiu/ori x,r,lo."""
    seeds = set()
    for lo, hi in code.ranges:
        for a in range(lo, hi, 4):
            i, _ = code.get(a)
            if i.mnem != "lui":
                continue
            r, hv = i.ops[0][1], i.ops[1][1]
            for b in range(a + 4, min(a + 36, hi), 4):
                j, fl = code.get(b)
                if j.mnem in ("addiu", "ori") and j.ops[1][1] == r:
                    v = ((hv << 16) + j.ops[2][1]) if j.mnem == "addiu" else ((hv << 16) | j.ops[2][1])
                    v &= 0xFFFFFFFF
                    if code.inside(v) and _plausible_start(code, v):
                        seeds.add(v)
                    break
                if j.ops and j.ops[0][0] == "r" and j.ops[0][1] == r or fl.kind in _BLOCK_END:
                    break
    return seeds


def _data_pointer_seeds(img, code, exclude=()):
    seeds = set()
    for seg in img.segments:
        data = seg.data[:seg.filesz]
        n = len(data) // 4
        for k, v in enumerate(struct.unpack("<%dI" % n, data[:n * 4])):
            if v & 3 or not code.inside(v) or code.inside(seg.vaddr + k * 4) or seg.vaddr + k * 4 in exclude:
                continue
            if _plausible_start(code, v):
                seeds.add(v)
    return seeds


# ---------------------------------------------------------------- backward paths (jump-table recognition)
def _fall_pred(code, a):
    """The instruction executed right before `a` when `a` is reached sequentially (not by a branch to it), or None:
    the branch whose delay slot `a` is, the delay slot of a branch/call that continues at `a` (the not-taken
    branch-likely itself: its slot is nullified), or the previous plain instruction."""
    p = a - 4
    if not code.inside(p):
        return None
    i, fl = code.get(p)
    if not i.valid:
        return None
    if fl.has_delay_slot:
        return p
    if code.inside(p - 4):
        _q, qfl = code.get(p - 4)
        if qfl.has_delay_slot:
            if qfl.kind in ("branch",) + _CALLS:
                return p - 4 if qfl.likely else p
            return None                                 # after j / jr / beq r,r: not reached sequentially
    if fl.kind in ("break", "eret", "invalid"):
        return None
    return p


def _path(code, a, limit):
    """Backward path from `a`: [(address, edge)] in reverse execution order, the instruction executed right before `a`
    first. Follows the sequential predecessor when there is one, else the delay slot of the only branch into the
    current address when that is a forward branch, else the word laid out above it - the old linear walk: a block
    entered only by a backward branch is a rotated loop's head (Metrowerks: `b cond; head: ...; cond: bxx head`) whose
    preheader, where loop-invariant values are set, sits right above it. edge: 'seq' (control went on to the next
    address), 'taken' (the delay slot of a taken branch: control went on to its target) or 'lin' (layout order, not a
    known control-flow edge)."""
    out, seen, cur = [], {a}, a
    for _ in range(limit):
        p, edge = _fall_pred(code, cur), "seq"
        if p is None:
            srcs = code.into().get(cur, ())
            if len(srcs) == 1 and srcs[0] < cur and code.inside(srcs[0] + 4):
                p, edge = srcs[0] + 4, "taken"
            elif code.inside(cur - 4):
                p, edge = cur - 4, "lin"
            else:
                break
        if p in seen:
            break
        seen.add(p)
        out.append((p, edge))
        cur = p
    return out


def _after_call(code, x):
    """True when x is the delay slot of a call: a callee runs between x and the next instruction on a 'seq' edge."""
    return code.inside(x - 4) and code.get(x - 4)[1].kind in _CALLS


def _def_of(code, reg, a, limit):
    """Most recent instruction before address a (on the unique backward path) that writes `reg`."""
    for x, edge in _path(code, a, limit):
        i, _fl = code.get(x)
        if edge == "seq" and _after_call(code, x) and reg in _CALLER_SAVED:
            return None                                  # clobbered by the callee
        if dest_gpr(i) == reg:
            return x, i
    return None


def _eval(code, reg, a, depth=0):
    """Backward-evaluate `reg` at address a into (const, index) where index is (reg, shift address) or None."""
    if reg == 0:
        return 0, None
    if depth > 8:
        return None
    d = _def_of(code, reg, a, 96)
    if d is None:
        return None
    da, i = d
    m, ops = i.mnem, i.ops
    if m == "lui":
        return (ops[1][1] << 16) & 0xFFFFFFFF, None
    if m in ("addiu", "daddiu", "ori"):
        sub = _eval(code, ops[1][1], da, depth + 1)
        if sub is None or sub[1] is not None:
            return None
        return ((sub[0] + ops[2][1]) if m != "ori" else (sub[0] | ops[2][1])) & 0xFFFFFFFF, None
    if m in ("addu", "daddu"):
        x = _eval(code, ops[1][1], da, depth + 1)
        y = _eval(code, ops[2][1], da, depth + 1)
        if x is None or y is None or (x[1] is not None and y[1] is not None):
            return None
        return (x[0] + y[0]) & 0xFFFFFFFF, x[1] or y[1]
    if m in ("sll", "dsll") and ops[2][1] == 2:
        return 0, (ops[1][1], da)
    return None


def _copy_source(i):
    """Source register when instruction i only copies/extends a register (move, sign/zero-extension)."""
    m, ops = i.mnem, i.ops
    if m in ("dsll32", "dsra32", "dsrl32", "sll") and ops[2][1] == 0:
        return ops[1][1]
    if m in ("daddu", "addu", "or") and ops[2][1] == 0:
        return ops[1][1]
    if m in ("daddu", "addu", "or") and ops[1][1] == 0:
        return ops[2][1]
    if m == "paddub" and ops[2][1] == 0:       # Metrowerks 128-bit move idiom
        return ops[1][1]
    return None


_STACK_LOADS = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "lwu": 4, "ld": 8}
_GUARD_TAKEN = {"beq": False, "beql": False, "bne": True, "bnel": True}   # in-range path = branch taken?


def _guard_bound(code, jr_addr, idx_reg, sll_addr, limit=96):
    """Switch bound N from the guard on the path into the jr: `sltiu x, i, N` where i holds the index value the table
    scaling reads (same value: register copies, reloads of the same stack slot, before or after the shift), and a
    `beq/bne x, zero` whose in-range side leads to the jr. Value numbering over the unique backward path."""
    seq = list(reversed(_path(code, jr_addr, limit)))
    if not any(x == sll_addr for x, _ in seq):
        return None
    vn, mem, fresh = {}, {}, [0]

    def new():
        fresh[0] += 1
        return ("v", fresh[0])

    def get(r):
        return 0 if r == 0 else vn.setdefault(r, ("in", r))

    vn_idx, cands, guards = None, [], []
    for k, (x, edge) in enumerate(seq):
        i, fl = code.get(x)
        m, ops = i.mnem, i.ops
        if x == sll_addr:
            vn_idx = get(idx_reg)
        if fl.kind == "branch" and m in _GUARD_TAKEN and len(ops) == 3 and ("r", 0) in ops[:2]:
            nxt = seq[k + 1] if k + 1 < len(seq) else None
            taken = nxt is not None and nxt[0] == x + 4 and nxt[1] == "taken"
            r = ops[0][1] if ops[1] == ("r", 0) else ops[1][1]
            guards.append((k, get(r), _GUARD_TAKEN[m] == taken))
        d = dest_gpr(i)
        if m == "sltiu" and d:
            v = new()
            cands.append((k, get(ops[1][1]), ops[2][1], v))
            vn[d] = v
        elif d:
            src = _copy_source(i)
            if src is not None:
                vn[d] = get(src)
            elif m in _STACK_LOADS and ops[1][1] == SP:
                key = (ops[1][2], _STACK_LOADS[m], m)
                vn[d] = mem.get(key) or mem.setdefault(key, new())
            else:
                vn[d] = new()
        if d == SP:
            mem.clear()
        elif fl.kind == "none" and d is None and ops and ops[-1][0] == "mem" and ops[-1][1] == SP:
            off = ops[-1][2]                                # a store into the frame: forget overlapping slots
            for key in [kk for kk in mem if kk[0] < off + 16 and off < kk[0] + kk[1]]:
                del mem[key]
            if m in ("sw", "sd") and ops[0][0] == "r":     # store-to-load forwarding of a spilled value
                mem[(off, 4 if m == "sw" else 8, "lw" if m == "sw" else "ld")] = get(ops[0][1])
        if edge == "seq" and _after_call(code, x):          # the callee ran: caller-saved registers unknown
            for r in _CALLER_SAVED:
                vn[r] = new()
    if vn_idx is None:
        return None
    best = None
    for k, vr, n, vx in cands:
        if vr == vn_idx and any(gk > k and gv == vx and ok for gk, gv, ok in guards):
            best = n
    return best


def _table_base(code, jr_addr, reg):
    """(table address, index register, shift address) of the jump-table load feeding `jr reg`, or None."""
    d = _def_of(code, reg, jr_addr, 12)
    if d is None:
        return None
    la, lw = d
    if lw.mnem != "lw":
        return None
    base, off = lw.ops[1][1], lw.ops[1][2]
    ev = _eval(code, base, la)
    if ev is None or ev[1] is None:
        return None
    return (ev[0] + off) & 0xFFFFFFFF, ev[1][0], ev[1][1]


def _table_words(code, table, n):
    targets = []
    for k in range(n):
        try:
            t = code.img.word(table + 4 * k)
        except Exception:
            return None
        if not code.inside(t):
            return None
        targets.append(t)
    return targets


def _table_run(code, table, jr_addr, bases, strong, limit=1024, reach=0x10000):
    """Entries of a switch table without a bound check: consecutive code pointers near the jr, up to the next table,
    a word that is not one, or a direct call target."""
    targets = []
    for k in range(limit):
        at = table + 4 * k
        if k and at in bases:
            break
        try:
            t = code.img.word(at)
        except Exception:
            break
        if not code.inside(t) or abs(t - jr_addr) > reach or t in strong or not code.get(t)[0].valid:
            break
        targets.append(t)
    return targets if len(targets) >= 2 else None


def _find_jump_tables(code, strong):
    """{jr address: (table address, [targets])} for every `jr reg` in the code ranges that loads from a table."""
    found = {}
    for lo, hi in code.ranges:
        for a in range(lo, hi, 4):
            _i, fl = code.get(a)
            if fl.kind == "jump_indirect":
                tb = _table_base(code, a, fl.reg)
                if tb:
                    found[a] = tb
    bases = {tb[0] for tb in found.values()}
    tables = {}
    for jr, (table, idx, sll_addr) in found.items():
        n = _guard_bound(code, jr, idx, sll_addr)
        if n is None:
            targets = _table_run(code, table, jr, bases, strong)
        else:
            targets = _table_words(code, table, n) if 0 < n <= 4096 else None
        if targets:
            tables[jr] = (table, targets)
    return tables


# ---------------------------------------------------------------- exploration
def _explore(code, start, barriers, owner, tables, commit=True):
    """Explore one function. Stops at `barriers` (other functions' strong starts: tail calls). A committed exploration
    goes on through code that other functions own (overlap); a trial exploration (commit=False: ownership only
    checked, not recorded) stops there and reports it in `foreign`."""
    fn = Function(start)
    foreign = False
    if not commit:
        owner = _Overlay(owner)
    work = [start]
    ended_cleanly = False
    while work:
        a = work.pop()
        while True:
            if a in fn.addrs or not code.inside(a):
                break
            if a != start and a in barriers:
                break
            if not commit and a in owner and owner[a] != start:
                foreign = True
                break
            i, fl = code.get(a)
            if not i.valid:
                fn.well_formed = False
                break
            fn.addrs.add(a)
            if a not in owner:
                owner[a] = start
            k = fl.kind
            if k in ("none", "trap", "syscall", "break"):
                a += 4
                continue
            if fl.has_delay_slot and code.inside(a + 4):
                ds = a + 4
                if not code.get(ds)[0].valid or (not commit and ds in owner and owner[ds] != start):
                    fn.well_formed = False          # delay slot shared with another function: dead/odd code
                else:
                    fn.addrs.add(ds)
                    if ds not in owner:
                        owner[ds] = start
            if k == "branch":
                if code.inside(fl.target):
                    work.append(fl.target)
                a += 8
                continue
            if k in _CALLS:
                if fl.target is not None and code.inside(fl.target):
                    fn.calls.add(fl.target)
                a += 8
                continue
            if k == "jump":
                if i.mnem == "j":                          # tail call
                    if code.inside(fl.target):
                        fn.tail_calls.add(fl.target)
                    ended_cleanly = True
                elif code.inside(fl.target):               # beq r,r: local unconditional branch
                    work.append(fl.target)
                break
            if k == "jump_indirect":
                jt = tables.get(a)
                if jt:
                    fn.jump_tables.append((a, jt[0], jt[1]))
                    work.extend(jt[1])
                else:
                    fn.indirect_tail_calls.append(a)
                    ended_cleanly = True
                break
            if k in ("return", "eret"):
                ended_cleanly = True
            break
    fn.well_formed = fn.well_formed and ended_cleanly
    fn.end = max(fn.addrs) + 4 if fn.addrs else start
    if not commit:
        fn.foreign = foreign                    # trial only (committed Functions keep the pickled field set)
    return fn


class _Overlay(dict):
    """Write-local, read-through view of the ownership map (for trial explorations)."""

    def __init__(self, base):
        super().__init__()
        self.base = base

    def __contains__(self, k):
        return dict.__contains__(self, k) or k in self.base

    def __getitem__(self, k):
        return dict.__getitem__(self, k) if dict.__contains__(self, k) else self.base[k]


def _gap_candidates(code, owner):
    """Unowned non-padding words that start right after owned code/padding or after a function end."""
    for lo, hi in code.ranges:
        prev_owned = True
        a = lo
        while a < hi:
            if a in owner:
                prev_owned = True
                a += 4
                continue
            w = code.word(a)
            if w == NOP:
                a += 4
                continue
            if prev_owned or _plausible_start(code, a):
                yield a
            prev_owned = False
            a += 4


def discover_functions(img, code_ranges, extra_seeds=()):
    code = _Code(img, code_ranges)
    strong = set(s for s in extra_seeds if code.inside(s))
    if code.inside(img.entry):
        strong.add(img.entry)
    for lo, hi in code_ranges:
        for a in range(lo, hi, 4):
            i, fl = code.get(a)
            if fl.kind in ("call", "call_conditional") and code.inside(fl.target):
                strong.add(fl.target)
    # jump tables are recognised locally (no exploration context needed): find them all up front so their entries
    # are never mistaken for function pointers
    tables = _find_jump_tables(code, strong)
    table_words = {t + 4 * k for t, targets in tables.values() for k in range(len(targets))}
    functions, owner = {}, {}
    starts = set(strong)

    def run(pending):
        """Explore pending seeds; tail-call targets become (weak) seeds too (until closure)."""
        while pending:
            for s in sorted(pending):
                if s not in functions:
                    functions[s] = _explore(code, s, strong, owner, tables)
            new = {t for fn in functions.values() for t in fn.tail_calls} - starts
            starts.update(new)
            pending = new

    run(set(starts))                                   # 1. strong seeds: entry, direct calls (+ tail calls)
    data_seeds = {a for a in _data_pointer_seeds(img, code, exclude=table_words) | _code_address_seeds(code)
                  if a not in owner}
    starts.update(data_seeds)
    run(data_seeds)                                    # 2. addresses taken in data/code (vtables, callbacks)
    rejected = set()
    while True:                                        # 3. unreferenced but well-formed code in gaps
        new, claimed = set(), set()
        for g in _gap_candidates(code, owner):
            if g in starts or g in rejected or g in claimed:
                continue
            fn = _explore(code, g, strong, owner, tables, commit=False)
            if fn.well_formed and not fn.foreign:       # a piece of another function's code is not a function
                new.add(g)
                claimed |= fn.addrs
            else:
                rejected.add(g)
        if not new:
            break
        starts.update(new)
        run(new)
    return FunctionSet(functions, code_ranges, strong)
