"""Control-flow analysis of an IRX for the static recompiler.

Finds the code (all of .text except the import/export tables), the function entry points (module entry, `jal`
targets, code addresses built with lui/addiu pairs = callbacks handed to the kernel, exports), the function extents,
branch labels and the cases the C back-end cannot translate. Pure analysis: no game knowledge.
"""
from dataclasses import dataclass, field

from . import irx as irxmod
from .r3000 import decode, LOADS


@dataclass
class Function:
    entry: int
    end: int                      # exclusive
    labels: set = field(default_factory=set)
    calls: set = field(default_factory=set)         # direct callee entries (incl. tail calls)
    imports: set = field(default_factory=set)       # import stub addresses called
    falls_through: bool = False   # runs into the next function without a jump
    jtab: set = field(default_factory=set)          # jump-table targets (R_MIPS_32 words) for its `jr reg`


@dataclass
class Program:
    irx: object
    insns: dict                   # address -> Insn (code only)
    code_ranges: list             # [(start, end))
    entries: list                 # sorted function entries
    callbacks: list               # entries whose address is taken (lui/addiu pairs)
    functions: dict               # entry -> Function
    stubs: dict                   # stub address -> ImportFn
    issues: list = field(default_factory=list)    # (severity, address, text)
    load_delay_uses: list = field(default_factory=list)

    def errors(self):
        return [i for i in self.issues if i[0] == "error"]


def code_pointers(irx, words=False):
    """Absolute addresses into .text built by R_MIPS_HI16/R_MIPS_LO16 pairs (words=False) or held by R_MIPS_32 data
    words (words=True: function pointers or jump-table entries)."""
    out = {}
    hi = []
    for r in irx.relocs:
        if r.type == irxmod.R_MIPS_HI16:
            hi.append(r.offset)
        elif r.type == irxmod.R_MIPS_LO16:
            lo = irx.word(irx.base + r.offset) & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            for h in hi:
                v = (((irx.word(irx.base + h) & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                if irx.in_text(v) and not words:
                    out.setdefault(v, []).append(irx.base + h)
            hi = []
        elif r.type == irxmod.R_MIPS_32 and words:
            v = irx.word(irx.base + r.offset)
            if irx.in_text(v):
                out.setdefault(v, []).append(irx.base + r.offset)
    return out


def analyse(irx):
    tables = irx.import_table_ranges()
    base, tend = irx.base, irx.base + irx.text_size
    code_ranges = []
    cur = base
    for a, b in sorted(tables):
        if a > cur:
            code_ranges.append((cur, a))
        cur = max(cur, b)
    if cur < tend:
        code_ranges.append((cur, tend))
    insns = {}
    for a, b in code_ranges:
        for pc in range(a, b, 4):
            insns[pc] = decode(irx.word(pc), pc)
    stubs = irx.stubs()
    prog = Program(irx, insns, code_ranges, [], [], {}, stubs)

    def in_code(a):
        return a in insns

    entries = {base + irx.entry}
    for pc, i in insns.items():
        if i.op == "jal":
            if i.target in stubs:
                continue
            if not in_code(i.target):
                prog.issues.append(("error", pc, "jal to 0x%x outside the code" % i.target))
                continue
            entries.add(i.target)
    cbs = code_pointers(irx)
    for a in cbs:
        if a in stubs:
            prog.issues.append(("warn", a, "address of an import stub taken (called through a pointer)"))
            continue
        if in_code(a):
            entries.add(a)
    for ex in irx.exports:
        for f in ex.fns:
            if in_code(base + f):
                entries.add(base + f)
    prog.callbacks = [a for a in cbs if a in entries]

    # function extents; a branch out of its function into the middle of another one adds an entry (split); an
    # R_MIPS_32 code pointer is a jump-table entry when it points into a function that has a `jr reg`, else a
    # function pointer (a new entry). Recomputed until stable.
    words = code_pointers(irx, words=True)
    jt = {}
    while True:
        ents = sorted(entries)
        ends = {}
        for k, e in enumerate(ents):
            nxt = ents[k + 1] if k + 1 < len(ents) else None
            rng_end = next(b for a, b in code_ranges if a <= e < b)
            ends[e] = min(nxt, rng_end) if nxt is not None else rng_end
        added = False
        for e in ents:
            for pc in range(e, ends[e], 4):
                i = insns[pc]
                if (i.is_branch or i.op == "j") and not (e <= i.target < ends[e]):
                    if i.target in stubs:
                        continue
                    if in_code(i.target) and i.target not in entries:
                        entries.add(i.target)
                        added = True
        owners = [e for e in ents if any(insns[pc].op == "jr" and insns[pc].rs != 31 for pc in range(e, ends[e], 4))]
        jt = {}
        for a in words:
            if not in_code(a) or a in stubs:
                continue
            own = next((e for e in owners if e < a < ends[e]), None)
            if own is not None:
                jt.setdefault(own, set()).add(a)
            elif a not in entries:
                entries.add(a)
                prog.callbacks.append(a)
                added = True
        if not added:
            break
    prog.entries = ents
    prog.callbacks = sorted(set(prog.callbacks))

    for e in ents:
        f = Function(e, ends[e])
        f.jtab = set(jt.get(e, ()))
        f.labels |= f.jtab
        pc = e
        while pc < f.end:
            i = insns[pc]
            if not i.valid:
                prog.issues.append(("error", pc, "invalid instruction 0x%08x" % i.word))
            if i.op in ("syscall", "break", "mfc0", "mtc0", "rfe"):
                prog.issues.append(("warn", pc, "%s (runtime trap)" % i.op))
            if i.op in ("add", "addi", "sub"):
                prog.issues.append(("info", pc, "%s: overflow trap not modelled" % i.op))
            if i.has_delay_slot:
                if pc + 4 >= f.end:
                    prog.issues.append(("error", pc, "delay slot outside the function"))
                else:
                    d = insns[pc + 4]
                    if d.has_delay_slot:
                        prog.issues.append(("error", pc, "jump in a delay slot"))
                if i.is_branch:
                    if f.entry <= i.target < f.end:
                        f.labels.add(i.target)
                    elif i.target in entries:
                        f.calls.add(i.target)              # conditional tail call
                    elif i.target in stubs:
                        f.imports.add(i.target)
                    else:
                        prog.issues.append(("error", pc, "branch to 0x%x outside the code" % i.target))
                    if i.op in ("bltzal", "bgezal"):
                        f.calls.add(i.target)
                elif i.op == "j":
                    if f.entry <= i.target < f.end:
                        f.labels.add(i.target)                 # a loop (also back to the entry)
                    elif i.target in entries:
                        f.calls.add(i.target)                  # tail call
                    elif i.target in stubs:
                        f.imports.add(i.target)                # tail call into another module
                    else:
                        prog.issues.append(("error", pc, "j to 0x%x outside the code" % i.target))
                elif i.op == "jal":
                    (f.imports if i.target in stubs else f.calls).add(i.target)
                elif i.op == "jr" and i.rs != 31 and not f.jtab:
                    prog.issues.append(("warn", pc, "jr through a register with no jump table: an indirect tail call "
                                                    "at run time"))
                elif i.op == "jalr":
                    pass                                    # indirect call: resolved at run time
            if i.is_load and pc + 4 < f.end:
                n = insns[pc + 4]
                if i.rt != 0 and i.rt in n.sources() and not (i.op in ("lwl", "lwr") and n.op in ("lwl", "lwr")):
                    prog.load_delay_uses.append(pc)
            pc += 4
        last = insns[f.end - 8] if f.end - 8 >= f.entry else None
        tail = insns[f.end - 4] if f.end - 4 >= f.entry else None
        uncond = lambda x: x is not None and (x.op in ("j", "jr") or (x.op == "beq" and x.rs == 0 and x.rt == 0))
        f.falls_through = not uncond(last) and f.end in entries
        if f.falls_through:
            f.calls.add(f.end)
        prog.functions[e] = f
    for pc in prog.load_delay_uses:
        prog.issues.append(("error", pc, "load-delay slot reads the loaded register (old value semantics)"))
    return prog
