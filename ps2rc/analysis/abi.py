"""Non-ABI detector (Phase 4b): which functions back-end v4 ("ABI mode") may translate, and which must stay exact.

v4 keeps registers in C locals and only syncs with the guest context what the EE ABI (Metrowerks EABI64) makes
observable: before a call the argument registers the callee reads (a0-a3, t0-t3), sp, gp and the FPU arguments
f12-f19; at a return v0, v1, f0, f1, sp, gp; after a call the caller reloads v0, v1, f0, f1 only and keeps s0-s7/fp
(and everything else) in its locals. A function is ABI-safe when that model loses nothing. Reasons to stay exact:

  1-input        the function reads, as its incoming value, a register other than the ABI inputs (a0-a3, t0-t3,
                 f12-f19, sp, gp). Only *definite* reads count: the register holds its entry value on every path to the
                 read (a register written on some paths only is a maybe-uninitialised compiler variable, not an input).
                 A register stored to the function's own stack frame in its entry state is a save, not a read
                 (Metrowerks prologues: `sq s0,off(sp)`), unless the slot is loaded back into another register.
                 A callee-saved register the function has already saved to its frame is its own variable: a definite
                 read of it is an uninitialised-variable read (noted as 'garbage_reads' in the details, no reason).
                 Callee-saved inputs (s0-s7, fp, f20-f31) a callee reads are inputs of a caller that passes them through
                 unchanged. A caller-saved register (at, v0, v1, t4-t9, k0/k1, f0-f11) read as an incoming value is an
                 input only when a caller (call, tail call, fragment entry) sets it for the transfer: its own write
                 reaches the transfer through straight-line code (Metrowerks: `addiu t9,sp,N; ...; jal __ptmf_scall`).
                 Otherwise compiled code read garbage (partial-register inserts of 64-bit helpers): 'garbage_reads'.
  2-output       a caller reads, after the call, a register other than v0, v1, f0, f1 (sp, gp) whose value can only come
                 from this callee (every reaching definition of the read is a call that may modify it) - v4 would give
                 the caller its stale local. Propagated down to the callees that produce the value the function passes
                 on unchanged. The two halves of an unaligned load (ldr+ldl, lwl+lwr, also interleaved) are one write.
  2-callee-saved the function writes a callee-saved register (s0-s7, fp, gp, f20-f31) itself and does not restore it
                 on some return (saves/restores are tracked through stack slots and register copies).
  3-fragment     its entry lies inside another function whose frame or registers are live there (a `j` into the middle
                 of a function, a data-referenced address inside a function body).
  4-stack        the stack pointer is not back at its entry value at a return or tail call (a fragment that pops its
                 parent's frame, stack switching, a noreturn call followed by a fall-through).
  4-ra-data      it reads its return address as data (not only to save it or to return through it).
  4-ra-return    it returns / tail-calls through a return address that is not its own (longjmp-like: no back-end
                 variant can express that as a C return).
  4-kernel       it leaves through `eret` (exception handler, kernel/user mode switch): not entered by a call.
                 (di/ei/mfc0/mtc0 in an ordinary function are only noted: 'kernel_instructions' in the details.)

Analysis: per function an instruction-level CFG (analysis.width._Fn: delay slots, branch-likely, jump tables) and a
forward pass (stack-pointer offset, also through Metrowerks' large-frame idiom `ori t4,zero,N; subu sp,sp,t4; addu
t5,t4,sp`; which registers may differ from their entry value - own writes and callee modifications -; which registers
/ stack slots hold which entry value: save/restore pairs, register copies). Summaries are interprocedural: `returns`
(the function can return: not after KExit/ExitThread/... syscalls or calls of non-returning functions, no endless
loop - code after such a call is dead), `mod` (what it may leave modified), `inputs` (its incoming non-ABI registers);
fixpoints over the call graph. Calls between units (main code and the overlays) are resolved by address; unknown /
indirect targets are assumed ABI-conforming. HI/LO, ACC, FCR31, the shift-amount register, the VU0 registers and the
upper GPR halves always
live in the context (exact in every variant) and are not tracked.
"""
import heapq
from dataclasses import dataclass, field

from ..ir import LiftError, lift, link_register
from ..r5900.semantics import dest_gpr
from .width import _Fn

NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
         "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
SP, GP, RA = 29, 28, 31


def gpr(*rs):
    m = 0
    for r in rs:
        m |= 1 << r
    return m


def fpr(*fs):
    m = 0
    for f in fs:
        m |= 1 << (32 + f)
    return m


def reg_bit(name):
    """Mask of one register by name ('s0', 'f20')."""
    return gpr(NAMES.index(name)) if name in NAMES else fpr(int(name[1:]))


ARG_GPR = gpr(4, 5, 6, 7, 8, 9, 10, 11)
ARG_FPR = fpr(*range(12, 20))
ARGS = ARG_GPR | ARG_FPR
ABI_IN = ARGS | gpr(SP, GP)
ABI_OUT = gpr(2, 3, SP, GP) | fpr(0, 1)
CS_GPR = gpr(16, 17, 18, 19, 20, 21, 22, 23, 30)
CS_FPR = fpr(*range(20, 32))
CS = CS_GPR | CS_FPR
CALLER_SAVED = gpr(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, RA) | fpr(*range(0, 20))
SCRATCH_IN = CALLER_SAVED & ~ARGS & ~gpr(RA)            # caller-saved registers that are no ABI input
KERNEL = {"eret", "mfc0", "mtc0", "di", "ei", "tlbr", "tlbwi", "tlbwr", "tlbp"}
# EE kernel calls that do not return (analysis/syscalls_used_v201.json names): KExit, _LoadExecPS2, _ExecPS2,
# ExitThread, ExitDeleteThread, _ExecOSD
NORETURN_SYSCALLS = {4, 6, 7, 35, 36, 123}
_FPR_NODEF = {"swc1", "mfc1", "cfc1", "ctc1", "c.eq.s", "c.lt.s", "c.le.s", "c.f.s", "adda.s", "suba.s", "mula.s",
              "madda.s", "msuba.s", "bc1t", "bc1f", "bc1tl", "bc1fl", "lqc2", "sqc2"}


def reg_names(mask):
    out = [NAMES[r] for r in range(1, 32) if mask >> r & 1]
    return out + ["f%d" % f for f in range(32) if mask >> (32 + f) & 1]


def _bits(mask):
    return [b for b in range(64) if mask >> b & 1]


# ---------------------------------------------------------------- per-instruction register effects
def _fpr_def(insn):
    m = insn.mnem
    if m == "mtc1":
        return insn.ops[1][1]
    if m in _FPR_NODEF or not insn.ops or insn.ops[0][0] != "f":
        return None
    return insn.ops[0][1]


def _copy(insn, op):
    """(destination bit, source bit) when the instruction only moves a register."""
    m, o = insn.mnem, insn.ops
    if m == "mov.s":
        return 1 << (32 + o[0][1]), 1 << (32 + o[1][1])
    if op is None or op.dst is None:
        return None
    if op.cls in ("move64", "move128") and len(op.srcs) == 1 and m != "daddiu":
        return 1 << op.dst, 1 << op.srcs[0][0]
    if m == "daddiu" and o[2][1] == 0 and o[1][1]:
        return 1 << op.dst, 1 << o[1][1]
    if m == "addu" and len(o) == 3 and (o[1][1] == 0 or o[2][1] == 0) and (o[1][1] or o[2][1]):
        return 1 << op.dst, 1 << (o[1][1] or o[2][1])
    if m in ("and", "or") and o[1][1] == o[2][1] and o[1][1]:     # Sony libraries: `and v1,gp,gp`
        return 1 << op.dst, 1 << o[1][1]
    return None


_ARITH = {"addiu", "daddiu", "ori", "lui", "addu", "daddu", "subu", "dsubu", "or"}


def _info(insn, fl):
    """(use, def, memory access ('st'|'ld', data bit, base register, offset) | None, copy | None,
    arithmetic (mnemonic, rd, rs, rt, imm) | None) - the last one for symbolic constants / stack addresses."""
    m = insn.mnem
    try:
        op = lift(insn)
    except LiftError:
        op = None
    # an instruction whose only effect is a write to $zero reads nothing that matters - the decoder accepts SPECIAL
    # words with ignored must-be-zero fields, so an inline constant 0x00ff00ff executed as an instruction (Sony's
    # libipu does that) is `dsra32 zero,ra,3` and must not count as "ra read as data"
    if op is not None and op.pure and op.dst is None and op.base is None and not op.hilo_w and \
            link_register(insn) is None:
        return 0, 0, None, None, None
    use = dfn = 0
    if op is not None:
        for r, _ in op.srcs:
            use |= 1 << r
        if op.base:
            use |= 1 << op.base
        if op.dst:
            dfn |= 1 << op.dst
    else:                                               # unknown operand shape: every GPR operand but the destination read
        d = dest_gpr(insn)
        if d:
            dfn |= 1 << d
        for k, o in enumerate(insn.ops):
            if o[0] in ("r", "mem") and o[1] and not (k == 0 and d and o == ("r", d)):
                use |= 1 << o[1]
    lr = link_register(insn)
    if lr:
        dfn |= 1 << lr
    fd = _fpr_def(insn)
    if fd is not None:
        dfn |= 1 << (32 + fd)
    for o in insn.ops:
        if o[0] == "f" and o[1] != fd:
            use |= 1 << (32 + o[1])
    if fl.kind == "return":
        use &= ~(1 << RA)                               # `jr ra` is a C return: the value is not read
    if m == "syscall":
        use |= gpr(3) | ARG_GPR                         # number in v1, arguments (what the back-end writes back)
        dfn |= gpr(2, 3)
    mem = None
    base = op.base if op is not None else None
    if base is not None:
        off = insn.ops[-1][2]
        if op.store and op.srcs:
            mem = ("st", 1 << op.srcs[0][0], base, off)
        elif m == "swc1":
            mem = ("st", 1 << (32 + insn.ops[0][1]), base, off)
        elif op.dst and m not in ("lwl", "lwr", "ldl", "ldr"):
            mem = ("ld", 1 << op.dst, base, off)
        elif m == "lwc1":
            mem = ("ld", 1 << (32 + insn.ops[0][1]), base, off)
    arith = None
    if m in _ARITH and op is not None and op.dst:
        o = insn.ops
        if m == "lui":
            arith = (m, o[0][1], 0, None, o[1][1])
        elif m in ("addiu", "daddiu", "ori"):
            arith = (m, o[0][1], o[1][1], None, o[2][1])
        else:
            arith = (m, o[0][1], o[1][1], o[2][1], None)
    return use & ~1, dfn & ~1, mem, _copy(insn, op), arith


_PAIRS = {("ldr", "ldl"): 7, ("ldl", "ldr"): -7, ("lwr", "lwl"): 3, ("lwl", "lwr"): -3}
_FIRST_HALF = {"ldr", "ldl", "lwr", "lwl"}


def _pair_up(fi, f, k, window=8):
    """Instruction k loads one half of an unaligned word/doubleword; when the other half follows within `window`
    straight-line instructions (Metrowerks interleaves several pairs) into the same register from the same base, with
    nothing in between touching that register or redefining the base, the pair overwrites the whole register: the
    first half's merge read of the old value is not a read."""
    a = fi.addrs[k]
    i1 = f.insn[a]
    (_r1, rt), (_m1, base, off) = i1.ops
    if rt == base or rt == 0:
        return
    bit = 1 << rt
    for n in range(1, window + 1):
        b = a + 4 * n
        i2 = f.insn.get(b)
        if i2 is None:
            return
        d = _PAIRS.get((i1.mnem, i2.mnem))
        if d is not None and i2.ops[0][1] == rt and i2.ops[1][1] == base and i2.ops[1][2] - off == d:
            fi.use[k] &= ~bit
            return
        j = fi.idx[b]
        if f.flow[b].kind != "none" or (fi.use[j] | fi.dfn[j]) & bit or fi.dfn[j] >> base & 1:
            return


class FnInfo:
    """Compact instruction-level view of one function: per node (sorted addresses) the register effects and the CFG.
    succ[i]: [(j, call)] with call False for a plain edge, else the call target (None: indirect); exits[i]: ('ret',),
    ('tail', target or None), ('call', target or None)."""
    __slots__ = ("key", "addrs", "idx", "start", "use", "dfn", "mem", "copy", "arith", "ret", "kernel", "succ",
                 "exits")

    def __init__(self, key, img, fn):
        self.key = key
        f = _Fn(img, fn, ())
        f.attach_jump_tables(fn.jump_tables)
        self.addrs = f.addrs
        self.idx = {a: k for k, a in enumerate(self.addrs)}
        self.start = self.idx.get(fn.start)
        n = len(self.addrs)
        self.use, self.dfn, self.mem, self.copy, self.arith = ([None] * n for _ in range(5))
        self.ret = [False] * n
        self.kernel = set()
        for k, a in enumerate(self.addrs):
            insn, fl = f.insn[a], f.flow[a]
            (self.use[k], self.dfn[k], self.mem[k], self.copy[k], self.arith[k]) = _info(insn, fl)
            self.ret[k] = fl.kind == "return"
            if insn.mnem in KERNEL:
                self.kernel.add(insn.mnem)
        for k, a in enumerate(self.addrs):              # ldr+ldl / lwl+lwr: together a full write, no merge read
            if f.insn[a].mnem in _FIRST_HALF:
                _pair_up(self, f, k)
        self.succ = [[(self.idx[s], e[1] if e is not None else False) for s, e in f.succ[a]] for a in self.addrs]
        self.exits = [list(f.exits[a]) for a in self.addrs]
        for k, a in enumerate(self.addrs):              # `addiu v1,zero,N; syscall` with a kernel call that never returns
            if f.insn[a].mnem == "syscall" and a - 4 in f.insn:
                p = f.insn[a - 4]
                if p.mnem in ("addiu", "ori") and p.ops[0][1] == 3 and p.ops[1][1] == 0 and \
                        p.ops[2][1] in NORETURN_SYSCALLS:
                    self.succ[k], self.exits[k] = [], []
        for jr, _tab, targets in fn.jump_tables:             # cases outside the function: transfers to their starts
            ds = jr + 4 if jr + 4 in self.idx else jr
            for t in set(targets):
                if t not in self.idx and ds in self.idx:
                    self.exits[self.idx[ds]].append(("tail", t))


# ---------------------------------------------------------------- forward: stack offset, may-differ sets, entry values
# state: (sp offset from its entry value or None, may-differ mask, own may-differ mask (without callee effects),
# holders, values) where holders is a sorted tuple of (location, entry register bit): ('r', bit) a register holding
# another register's entry value, ('s', offset) a stack slot (offset from the entry sp) holding one; values: sorted
# tuple of (register, 'c' | 'sp', n): registers known to hold a constant or entry sp + n (Metrowerks' large frames:
# `ori t4,zero,N; subu sp,sp,t4; addu t5,t4,sp; sq s0,-16(t5)`). A register outside the may-differ mask holds its own
# entry value.
def _entry_of(state, bit):
    """Bit of the register whose entry value register `bit` holds now, or None."""
    if not state[1] & bit:
        return bit
    for loc, e in state[3]:
        if loc == ("r", bit):
            return e
    return None


def _set_holds(holds, loc, e):
    h = tuple(x for x in holds if x[0] != loc)
    return tuple(sorted(h + ((loc, e),))) if e is not None else h


def _val(state, r):
    """('c', n) / ('sp', n) value of register r, or None."""
    if r == 0:
        return ("c", 0)
    if r == SP:
        return ("sp", state[0]) if state[0] is not None else None
    for x in state[4]:
        if x[0] == r:
            return x[1], x[2]
    return None


def _arith(a, state):
    """Symbolic value an arithmetic instruction writes, or None."""
    m, _rd, rs, rt, imm = a
    if m == "lui":
        return "c", (imm << 16) & 0xFFFFFFFF
    x = _val(state, rs)
    if x is None:
        return None
    if m in ("addiu", "daddiu"):
        return x[0], x[1] + imm
    if m == "ori":
        return ("c", x[1] | imm) if x[0] == "c" else None
    y = _val(state, rt)
    if y is None:
        return None
    if m in ("subu", "dsubu"):
        return (x[0], x[1] - y[1]) if y[0] == "c" else None
    if m == "or":
        if x[0] == y[0] == "c":
            return "c", x[1] | y[1]
        return x if y == ("c", 0) else (y if x == ("c", 0) else None)
    if x[0] == "c" or y[0] == "c":                      # addu / daddu
        return ("sp" if "sp" in (x[0], y[0]) else "c"), x[1] + y[1]
    return None


def _slot(state, mem):
    """Stack slot (offset from the entry sp) a memory access touches, or None."""
    v = _val(state, mem[2])
    return v[1] + mem[3] if v is not None and v[0] == "sp" else None


def _step(f, i, state):
    delta, m, mo, holds, vals = state
    mem, cp = f.mem[i], f.copy[i]
    slot = _slot(state, mem) if mem is not None else None
    if slot is not None and mem[0] == "st":
        holds = _set_holds(holds, ("s", slot), _entry_of(state, mem[1]))
    d = f.dfn[i]
    if (slot is not None and mem[0] == "ld") or cp is not None:
        if cp is None:
            bit, e = mem[1], None
            for loc, x in holds:
                if loc == ("s", slot):
                    e = x
        else:
            bit, e = cp[0], _entry_of(state, cp[1])
        d &= ~bit
        if e == bit:                                    # restored: holds its own entry value again
            m &= ~bit
            mo &= ~bit
            holds = _set_holds(holds, ("r", bit), None)
        else:
            m |= bit
            mo |= bit
            holds = _set_holds(holds, ("r", bit), e)
    if d:
        m |= d
        mo |= d
        if holds:
            holds = tuple(x for x in holds if not (x[0][0] == "r" and d & x[0][1]))
    full, a = f.dfn[i], f.arith[i]
    if full & 0xFFFFFFFF & ~(1 << SP) and (vals or a is not None):
        v = _arith(a, state) if a is not None else None
        vals = tuple(x for x in vals if not full >> x[0] & 1)
        if v is not None and a[1] != SP:
            vals = tuple(sorted(vals + ((a[1],) + v,)))
    if full >> SP & 1:
        v = _arith(a, state) if a is not None else None
        delta = v[1] if v is not None and v[0] == "sp" else None
        if delta == 0:                                  # frame popped: sp holds its entry value again
            m &= ~(1 << SP)
            mo &= ~(1 << SP)
    return delta, m, mo, holds, vals


def _join(x, y):
    if x is None:
        return y
    if y is None:
        return x
    delta = x[0] if x[0] == y[0] else None
    holds = tuple(h for h in x[3] if h in y[3])
    vals = tuple(v for v in x[4] if v in y[4])
    return delta, x[1] | y[1], x[2] | y[2], holds, vals


def _after_call(state, kill):
    delta, m, mo, holds, vals = state
    return (delta, m | kill, mo, tuple(h for h in holds if not (h[0][0] == "r" and kill & h[0][1])),
            tuple(v for v in vals if not kill >> v[0] & 1))


def forward(f, kill_of):
    """State before every node (None: unreachable). kill_of(call target) -> registers the callee may modify, or None
    when the callee never returns (the continuation is dead)."""
    n = len(f.addrs)
    st = [None] * n
    if f.start is None:
        return st
    st[f.start] = (0, 0, 0, (), ())
    work, pending = [f.start], {f.start}
    while work:
        i = heapq.heappop(work)
        pending.discard(i)
        out = _step(f, i, st[i])
        for j, call in f.succ[i]:
            if call is False:
                v = out
            else:
                kill = kill_of(call)
                if kill is None:
                    continue
                v = _after_call(out, kill)
            nv = _join(st[j], v)
            if nv != st[j]:
                st[j] = nv
                if j not in pending:
                    pending.add(j)
                    heapq.heappush(work, j)
    return st


def saves(f, st):
    """Per node: the register bit a store only saves (its entry value to a stack slot nothing else is loaded from)."""
    loaded, slots = {}, [None] * len(f.addrs)
    for i in range(len(f.addrs)):
        if st[i] is not None and f.mem[i] is not None:
            slots[i] = _slot(st[i], f.mem[i])
            if slots[i] is not None and f.mem[i][0] == "ld":
                loaded.setdefault(slots[i], set()).add(f.mem[i][1])
    out = [0] * len(f.addrs)
    for i in range(len(f.addrs)):
        s = f.mem[i]
        if s is None or s[0] != "st" or slots[i] is None:
            continue
        if _entry_of(st[i], s[1]) == s[1] and loaded.get(slots[i], set()) <= {s[1]}:
            out[i] = s[1]
    return out


def direct_reads(f, st, sv):
    """(reads, owned): registers read while they hold their entry value on every path to the read (saves excluded);
    of those, callee-saved ones the function has saved to its frame on every path to the read - it owns the register
    as a variable, so the read is of an uninitialised variable (garbage), not of an input."""
    m = owned = 0
    for i in range(len(f.addrs)):
        if st[i] is None:
            continue
        r = f.use[i] & ~sv[i] & ~st[i][1]
        here = 0
        if r & CS:
            for loc, e in st[i][3]:
                if loc[0] == "s":
                    here |= e
        here &= r & CS
        owned |= here
        m |= r & ~here
    return m, owned


def producers(f, st, bit, kill_of):
    """Per node, the producers of register `bit`'s value before it: ('entry',), ('def', i) or ('call', i, target) - a
    call whose callee may modify the register replaces them (whenever the callee writes it, a read after the call sees
    the callee's value)."""
    n = len(f.addrs)
    pin = [None] * n
    if f.start is None:
        return pin, [None] * n
    pout = [None] * n
    pin[f.start] = frozenset([("entry",)])
    work, pending = [f.start], {f.start}
    while work:
        i = heapq.heappop(work)
        pending.discard(i)
        p = pin[i]
        if f.dfn[i] & bit and st[i] is not None:
            p = frozenset([("entry",)]) if not _step(f, i, st[i])[1] & bit else frozenset([("def", i)])
        pout[i] = p
        for j, call in f.succ[i]:
            kill = 0 if call is False else kill_of(call)
            if kill is None:
                continue
            v = frozenset([("call", i, call)]) if kill & bit else p
            nv = v if pin[j] is None else pin[j] | v
            if nv != pin[j]:
                pin[j] = nv
                if j not in pending:
                    pending.add(j)
                    heapq.heappush(work, j)
    return pin, pout


def backward_live(f, sv, gen_of, kill_of):
    """Live-in per node: registers read before written; a call reads what gen_of(target) says and kills what the callee
    may modify; a tail call reads gen_of(target)."""
    n = len(f.addrs)
    exit_dem = [0] * n
    for i in range(n):
        for e in f.exits[i]:
            if e[0] in ("tail", "call"):
                exit_dem[i] |= gen_of(e[1])
    use = [f.use[i] & ~sv[i] for i in range(n)]
    kills = [[(j, None if call is False else kill_of(call), call) for j, call in f.succ[i]] for i in range(n)]
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            o = exit_dem[i]
            for j, kill, call in kills[i]:
                if call is False:
                    o |= live[j]
                elif kill is not None:
                    o |= (live[j] & ~kill) | gen_of(call)
            v = (o & ~f.dfn[i]) | use[i]
            if v != live[i]:
                live[i] = v
                changed = True
    return live


# ---------------------------------------------------------------- whole program
@dataclass
class Unit:
    name: str
    img: object
    functions: dict


@dataclass
class Verdict:
    unit: str
    start: int
    insns: int
    reasons: dict = field(default_factory=dict)     # reason code -> details
    info: dict = field(default_factory=dict)        # observations that are no reason to stay exact
    inputs: int = 0
    mod: int = 0

    @property
    def exact(self):
        return bool(self.reasons)


class Program:
    """All functions of all units, call targets resolved across units (main code <-> overlay window)."""

    def __init__(self, units, window=None, progress=None):
        self.units = {u.name: u for u in units}
        self.main = units[0].name
        self.window = window
        self.fns = {}
        k = 0
        for u in units:
            for s, fn in u.functions.items():
                self.fns[(u.name, s)] = FnInfo((u.name, s), u.img, fn)
                k += 1
                if progress and k % 2000 == 0:
                    progress("cfg", k)
        self.callers = {key: set() for key in self.fns}
        for key, f in self.fns.items():
            for t in self.targets_of(f):
                for c in t:
                    self.callers[c].add(key)

    def resolve(self, unit, addr):
        """Function keys a direct transfer from `unit` to `addr` may reach, or () when unknown (indirect / no function)."""
        if addr is None or addr is False:
            return ()
        if unit != self.main and addr in self.units[unit].functions:
            return ((unit, addr),)
        if addr in self.units[self.main].functions:
            return ((self.main, addr),)
        if unit == self.main and self.window and self.window[0] <= addr < self.window[1]:
            return tuple((u, addr) for u in self.units if u != self.main and addr in self.units[u].functions)
        return ()

    def transfers(self, f):
        """[(node, target address or None, kind 'call'|'tail')] of one function."""
        out = []
        for i in range(len(f.addrs)):
            for _j, call in f.succ[i]:
                if call is not False:
                    out.append((i, call, "call"))
            for e in f.exits[i]:
                if e[0] in ("tail", "call"):
                    out.append((i, e[1], e[0]))
        return out

    def targets_of(self, f):
        return [self.resolve(f.key[0], t) for _i, t, _k in self.transfers(f)]


def _order(prog):
    """Callees before callers (DFS post-order over the call graph), as a starting worklist order."""
    seen, order = set(), []
    callees = {k: sorted({c for t in prog.targets_of(f) for c in t}) for k, f in prog.fns.items()}
    for root in sorted(prog.fns):
        if root in seen:
            continue
        stack = [(root, iter(callees[root]))]
        seen.add(root)
        while stack:
            node, it = stack[-1]
            nxt = next((c for c in it if c not in seen), None)
            if nxt is None:
                order.append(node)
                stack.pop()
            else:
                seen.add(nxt)
                stack.append((nxt, iter(callees[nxt])))
    return order


def _fixpoint(order, callers, update, progress=None, tag=""):
    """Worklist over functions: update(key) -> True when its summary changed (then its callers are redone)."""
    work, queued, rounds = list(order), set(order), 0
    while work:
        nxt = []
        for k in work:
            queued.discard(k)
            if update(k):
                for c in callers[k]:
                    if c not in queued:
                        queued.add(c)
                        nxt.append(c)
        work = nxt
        rounds += 1
        if progress:
            progress(tag, rounds)


@dataclass
class Analysis:
    prog: object
    returns: dict        # key -> False when the function never returns (Exit/ExitThread, endless loops)
    mod: dict            # key -> registers the function may leave modified (interprocedural)
    inputs: dict         # key -> incoming non-ABI registers it reads (callee-saved ones passed through included)
    direct: dict         # key -> registers read as their entry value by its own instructions (ABI ones included)
    owned: dict          # key -> callee-saved registers read uninitialised after the function saved them
    states: dict         # key -> forward states per node
    saves: dict          # key -> save bit per node
    verdicts: dict
    verdicts_info: dict = field(default_factory=dict)


def _returning_exits(f, i, kill_of):
    """Exits of node i through which the function returns to its caller: ('ret',) and transfers to targets that
    return (a call at the very end continues past the function only when its callee returns)."""
    if any(e[0] == "call" and kill_of(e[1]) is None for e in f.exits[i]):
        return []
    return [e for e in f.exits[i] if e[0] == "ret" or kill_of(e[1]) is not None]


def run(prog, progress=None):
    """The interprocedural fixpoints (which functions return, what they may modify, what they read) and verdicts."""
    order = _order(prog)
    returns = {k: False for k in prog.fns}
    mod = {k: 0 for k in prog.fns}
    inp = {k: 0 for k in prog.fns}
    states = {}

    def kill_for(unit):
        def kill_of(target):
            """Registers the callee may modify; None when it never returns."""
            ks = prog.resolve(unit, target)
            if not ks:
                return CALLER_SAVED
            m, ret = 0, False
            for k in ks:
                if returns[k]:
                    ret = True
                    m |= mod[k]
            return m if ret else None
        return kill_of

    def inputs_of(unit, target):
        m = 0
        for k in prog.resolve(unit, target):
            m |= inp[k]
        return m

    def update_returns(k):
        f, kill_of = prog.fns[k], kill_for(k[0])
        r = False
        if f.start is not None:
            seen, work = {f.start}, [f.start]
            while work and not r:
                i = work.pop()
                r = bool(_returning_exits(f, i, kill_of))
                for j, call in f.succ[i]:
                    if j not in seen and (call is False or kill_of(call) is not None):
                        seen.add(j)
                        work.append(j)
        changed = r != returns[k]
        returns[k] = r
        return changed

    def update_mod(k):
        f = prog.fns[k]
        kill_of = kill_for(k[0])
        st = forward(f, kill_of)
        states[k] = st
        m = 0
        for i in range(len(f.addrs)):
            if st[i] is None or not f.exits[i]:
                continue
            out = _step(f, i, st[i])
            for e in _returning_exits(f, i, kill_of):
                m |= out[1]
                if e[0] in ("tail", "call"):
                    m |= kill_of(e[1])
        changed = m != mod[k]
        mod[k] = m
        return changed

    _fixpoint(order, prog.callers, update_returns, progress, "returns")

    _fixpoint(order, prog.callers, update_mod, progress, "mod")
    sv = {k: saves(f, states[k]) for k, f in prog.fns.items()}
    direct, owned = {}, {}
    for k, f in prog.fns.items():
        direct[k], owned[k] = direct_reads(f, states[k], sv[k])
    transfers = {k: prog.transfers(f) for k, f in prog.fns.items()}

    def update_inputs(k):
        f, st = prog.fns[k], states[k]
        m = direct[k] & ~ABI_IN
        for i, t, kind in transfers[k]:             # callee-saved inputs passed through unchanged
            if st[i] is None:
                continue
            need = inputs_of(k[0], t) & CS
            if need:
                m |= need & ~_step(f, i, st[i])[1]
        changed = m != inp[k]
        inp[k] = m
        return changed

    _fixpoint(order, prog.callers, update_inputs, progress, "inputs")
    an = Analysis(prog, returns, mod, inp, direct, owned, states, sv, {})
    an.verdicts = _verdicts(an, kill_for, inputs_of, transfers)
    return an


def analyse(units, window=None, progress=None):
    """{(unit name, start): Verdict} for every function of every unit (units[0] is the main code)."""
    return run(Program(units, window, progress), progress).verdicts


def _verdicts(an, kill_for, inputs_of, transfers):
    prog, mod, inp, states = an.prog, an.mod, an.inputs, an.states
    res = {k: Verdict(k[0], k[1], len(f.addrs), inputs=inp[k], mod=mod[k]) for k, f in prog.fns.items()}
    into = {k: [] for k in prog.fns}                 # transfers into a function: (source key, node, 'out' | 'in')
    for k, f in prog.fns.items():
        st = states[k]
        for i, t, _kind in transfers[k]:
            if st[i] is not None:
                for c in prog.resolve(k[0], t):
                    into[c].append((k, i, "out"))
    for k, f in prog.fns.items():                   # fragment entries: the enclosing function's state at the entry
        unit = prog.units[k[0]].functions
        for a, i in f.idx.items():
            if a != k[1] and a in unit and states[k][i] is not None:
                into[(k[0], a)].append((k, i, "in"))
                delta, m = states[k][i][0], states[k][i][1]
                live_state = m & ~ARGS
                if delta != 0 or live_state:
                    res[(k[0], a)].reasons.setdefault("3-fragment", []).append(
                        {"parent": "%s:%08x" % k, "sp_offset": "?" if delta is None else delta,
                         "registers_set": reg_names(live_state)[:12]})
    prod_cache = {}

    def straight(f, j, i, limit=48):
        """Node j reaches node i through straight-line code only (no branch, no call in between)."""
        for _ in range(limit):
            if j == i:
                return True
            if len(f.succ[j]) != 1 or f.succ[j][0][1] is not False:
                return False
            j = f.succ[j][0][0]
        return False

    def provider(src, i, where, b):
        """The source function sets register b for the transfer: its own write reaches the transfer through
        straight-line code (Metrowerks' `addiu t9,sp,N; ...; jal __ptmf_scall`). A write further away that merely
        survives (a compiler temporary) does not count: compiled code passes nothing in scratch registers."""
        if (src, b) not in prod_cache:
            prod_cache[(src, b)] = producers(prog.fns[src], states[src], b, kill_for(src[0]))
        ps = prod_cache[(src, b)][0 if where == "in" else 1][i]
        return bool(ps) and any(p[0] == "def" and straight(prog.fns[src], p[1], i) for p in ps)

    for k, f in prog.fns.items():
        v, st = res[k], states[k]
        bad = inp[k] & CS
        scratch = an.direct[k] & SCRATCH_IN
        provided = 0
        for bit in _bits(scratch):
            if any(provider(src, i, where, 1 << bit) for src, i, where in into[k]):
                provided |= 1 << bit
        bad |= provided
        if bad:
            v.reasons["1-input"] = reg_names(bad)
        if scratch & ~provided or an.owned[k]:
            v.info["garbage_reads"] = reg_names(scratch & ~provided | an.owned[k])
        if an.direct[k] >> RA & 1:
            v.reasons["4-ra-data"] = "reads its return address as data"
        if not an.returns[k]:
            v.info["noreturn"] = True
        own_cs, deltas, ra_bad = 0, set(), []
        kill_of = kill_for(k[0])
        for i in range(len(f.addrs)):
            if st[i] is None:
                continue
            if f.ret[i] and _entry_of(st[i], 1 << RA) != 1 << RA:
                ra_bad.append("%08x" % f.addrs[i])
            if not f.exits[i]:
                continue
            out = _step(f, i, st[i])
            for e in _returning_exits(f, i, kill_of):
                own_cs |= out[2]
                deltas.add(out[0])
                if e[0] == "tail" and _entry_of(out, 1 << RA) != 1 << RA:
                    ra_bad.append("%08x" % f.addrs[i])
        if own_cs & (CS | gpr(GP)):
            v.reasons["2-callee-saved"] = reg_names(own_cs & (CS | gpr(GP)))
        if deltas - {0}:
            v.reasons["4-stack"] = sorted("?" if d is None else "%+d" % d for d in deltas - {0})
        if ra_bad:
            v.reasons["4-ra-return"] = sorted(set(ra_bad))[:8]
        if "eret" in f.kernel:                      # exception handler / mode switch: not entered by a call
            v.reasons["4-kernel"] = sorted(f.kernel)
        elif f.kernel:                              # di/ei/mfc0/mtc0 in an ordinary function (rt_fatal until HLE)
            v.info["kernel_instructions"] = sorted(f.kernel)
    an.verdicts_info = {}
    for (t, bit), sites in _observed_outputs(an, kill_for, inputs_of).items():
        d = res[t].reasons.setdefault("2-output", {})
        d[reg_names(bit)[0]] = sites[:6]
    for k, info in an.verdicts_info.items():
        res[k].info.update(info)
    return res


def _observed_outputs(an, kill_for, inputs_of):
    """{(callee key, register bit): [where]}: reads after a call of a value only a callee can have produced."""
    prog, mod, states, sv = an.prog, an.mod, an.states, an.saves
    found = {}

    def note(key, bit, where):
        lst = found.setdefault((key, bit), [])
        if where not in lst:
            lst.append(where)
            return True
        return False

    for k, f in prog.fns.items():
        kill_of = kill_for(k[0])

        def gen_of(t, unit=k[0]):
            return inputs_of(unit, t) & CS
        cand = 0
        for i in range(len(f.addrs)):
            for _j, call in f.succ[i]:
                if call is not False:
                    cand |= kill_of(call) or 0
        cand &= ~ABI_OUT
        if not cand:
            continue
        live = backward_live(f, sv[k], gen_of, kill_of)
        after = 0
        for i in range(len(f.addrs)):
            for j, call in f.succ[i]:
                if call is not False:
                    after |= live[j] & (kill_of(call) or 0)
        after &= ~ABI_OUT
        st = states[k]
        for bit in _bits(after):
            b = 1 << bit
            pin, pout = producers(f, st, b, kill_of)
            reads = [(i, pin[i]) for i in range(len(f.addrs)) if pin[i] is not None and f.use[i] & ~sv[k][i] & b]
            reads += [(i, pout[i]) for i in range(len(f.addrs)) for _j, call in f.succ[i]
                      if call is not False and pout[i] is not None and gen_of(call) & b]
            for i, ps in reads:
                if ps and all(p[0] == "call" for p in ps):
                    for p in ps:
                        ks = prog.resolve(k[0], p[2])
                        for c in ks:
                            note(c, b, "%s:%08x read at %08x" % (k[0], k[1], f.addrs[i]))
                        if not ks:                   # an unknown (indirect) callee's value: nobody to blame
                            an.verdicts_info.setdefault(k, {}).setdefault("read_after_indirect_call", []).append(
                                "%s@%08x" % (reg_names(b)[0], f.addrs[i]))
    # the value a function passes on unchanged comes from its own callees: they produce the output too
    work = list(found)
    while work:
        key, b = work.pop()
        f, st = prog.fns[key], states[key]
        kill_of = kill_for(key[0])
        _pin, pout = producers(f, st, b, kill_of)
        for i in range(len(f.addrs)):
            if pout[i] is None or not f.exits[i]:
                continue
            ps = set(pout[i])
            exits = _returning_exits(f, i, kill_of)
            if not exits:
                continue
            for e in exits:
                if e[0] in ("tail", "call") and kill_of(e[1]) & b:
                    ps.add(("call", i, e[1]))
            for p in ps:
                if p[0] == "call":
                    for c in prog.resolve(key[0], p[2]):
                        if note(c, b, "via %s:%08x" % key) and (c, b) not in work:
                            work.append((c, b))
    return found
