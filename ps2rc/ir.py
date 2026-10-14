"""Width-explicit IR (Phase 3): what every R5900 instruction reads and writes, at which width.

`lift(insn) -> Op` describes an instruction's effect on the GPR file:

  dst        GPR written (None if none; writes to $zero are dropped)
  dst_bits   64: the low 64 bits are written, the upper 64 kept (all non-128-bit EE ops); 128: the whole register
  srcs       ((reg, bits), ...) GPR value reads; bits = 32 (only the low word matters), 64 or 128. The old
             destination counts as a source where it is merged (LWR, LWL, MOVZ/MOVN, LDL/LDR, PLZCW).
  base       memory base register (read at 32 bits: EE addresses are 32-bit), or None
  out        canonicity of the low 64 bits of the result:
               'c32'   always a canonical sign-extended 32-bit value (ADDU, LW, SLT, LUI, MFC1, DSRA32...)
               'pass'  canonical iff every source is (AND/OR/XOR/NOR, ORI/XORI, moves, DSRA/DSRAV, LWR, MOVZ/MOVN;
                       MFHI/MFLO: iff the HI/LO half read is)
               'blob'  raw 64/128-bit data from memory (LD, LDL/LDR, LQ) - fine if it is only stored again
               'wide'  anything else (DADDU, DSLL, LWU, MMI, QMFC2...)
  store      bits of the data register a store writes to memory (0 for non-stores)
  cls        category of an explicitly 64/128-bit instruction ('' for 32-bit and other ops):
               move64 move128 cmove dalu dshift ld ldlr lwu sd sdlr lq sq mmi cop2
  hilo_w     HI/LO halves written: subset of ('hi0', 'lo0', 'hi1', 'lo1'); hilo_r: halves read;
  hilo_out   canonicity of the HI/LO halves written: 'c32' (32-bit MULT/DIV/MADD results, sign-extended), 'pass'
             (MTHI/MTLO: canonical iff the source is) or 'wide' (128-bit MMI writes)
  pure       no effect besides writing dst (a dead pure op can be dropped)

The same table drives the width analysis (analysis/width.py) and later code generation.
"""
from dataclasses import dataclass, replace


@dataclass(frozen=True, slots=True)
class Op:
    mnem: str
    dst: object = None
    dst_bits: int = 64
    srcs: tuple = ()
    base: object = None
    out: str = "c32"
    store: int = 0
    cls: str = ""
    hilo_w: tuple = ()
    hilo_r: tuple = ()
    hilo_out: str = "c32"
    pure: bool = True


class LiftError(Exception):
    pass


# compact operand-role specs: d = destination GPR, D = destination GPR (128-bit write), sNN = source GPR read at NN
# bits, x = operand without GPR effect (immediate, FPR, target...), m = memory operand (base read at 32 bits)
_ALU = {
    # 32-bit ALU: results sign-extended from 32 bits
    "addu": ("d s32 s32", "c32"), "add": ("d s32 s32", "c32"), "subu": ("d s32 s32", "c32"),
    "sub": ("d s32 s32", "c32"), "addiu": ("d s32 x", "c32"), "addi": ("d s32 x", "c32"),
    "sll": ("d s32 x", "c32"), "srl": ("d s32 x", "c32"), "sra": ("d s32 x", "c32"),
    "sllv": ("d s32 s32", "c32"), "srlv": ("d s32 s32", "c32"), "srav": ("d s32 s32", "c32"),
    "andi": ("d s32 x", "c32"), "lui": ("d x", "c32"),
    "neg": ("d s32", "c32"), "negu": ("d s32", "c32"),
    # 64-bit reads, canonical result
    "slt": ("d s64 s64", "c32"), "sltu": ("d s64 s64", "c32"), "slti": ("d s64 x", "c32"),
    "sltiu": ("d s64 x", "c32"),
    # bitwise: canonical in -> canonical out
    "and": ("d s64 s64", "pass"), "or": ("d s64 s64", "pass"), "xor": ("d s64 s64", "pass"),
    "nor": ("d s64 s64", "pass"), "ori": ("d s64 x", "pass"), "xori": ("d s64 x", "pass"),
}
BITWISE = {"and", "or", "xor", "nor", "ori", "xori"}
_DALU = {"daddu": "d s64 s64", "dadd": "d s64 s64", "dsubu": "d s64 s64", "dsub": "d s64 s64",
         "daddiu": "d s64 x", "daddi": "d s64 x", "dneg": "d s64", "dnegu": "d s64"}
_LOAD32 = {"lb", "lbu", "lh", "lhu", "lw"}
_STORE32 = {"sb": 8, "sh": 16, "sw": 32, "swl": 32, "swr": 32}
_MMI_MOVE = {"por", "paddub"}            # por/paddub x, y, zero: 128-bit register move (Metrowerks idiom)
_NO_GPR = {"sync", "sync.p", "nop", "syscall", "break", "eret", "ei", "di", "tlbp", "tlbr", "tlbwi", "tlbwr",
           "j", "jal", "bc0f", "bc0t", "bc0fl", "bc0tl", "bc1f", "bc1t", "bc1fl", "bc1tl", "bc2f", "bc2t",
           "bc2fl", "bc2tl"}
_TRAPS = {"tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"}
_BRANCH2 = {"beq", "bne", "beql", "bnel"}
_BRANCH1 = {"blez", "bgtz", "bltz", "bgez", "blezl", "bgtzl", "bltzl", "bgezl", "bltzal", "bgezal", "bltzall",
            "bgezall"}
_HILO_MUL = {"mult": "0", "multu": "0", "mult1": "1", "multu1": "1", "madd": "0", "maddu": "0", "madd1": "1",
             "maddu1": "1"}
ALL_HILO = ("hi0", "lo0", "hi1", "lo1")


def _apply(m, ops, spec, out, cls="", store=0, pure=True):
    """Build an Op from a role spec over the decoded operand list."""
    roles = spec.split()
    if len(roles) != len(ops):
        raise LiftError("%s: spec %r vs %d operands" % (m, spec, len(ops)))
    dst, dst_bits, srcs, base = None, 64, [], None
    for role, op in zip(roles, ops):
        if role in ("d", "D"):
            if op[0] != "r":
                raise LiftError("%s: destination is not a GPR" % m)
            dst, dst_bits = op[1], (128 if role == "D" else 64)
        elif role[0] == "s":
            if op[0] != "r":
                raise LiftError("%s: source is not a GPR" % m)
            srcs.append((op[1], int(role[1:])))
        elif role == "m":
            if op[0] != "mem":
                raise LiftError("%s: expected a memory operand" % m)
            base = op[1] or None
    srcs = tuple((r, b) for r, b in srcs if r != 0)
    if dst == 0:
        dst = None
    return Op(m, dst=dst, dst_bits=dst_bits, srcs=srcs, base=base, out=out, store=store, cls=cls, pure=pure)


def _merge_old(op, bits):
    """The destination's old value is merged into the result (partial loads, conditional moves)."""
    return replace(op, srcs=op.srcs + (((op.dst, bits),) if op.dst is not None else ()))


def lift(insn):
    """Op for one decoded instruction (raises LiftError for an instruction outside the table)."""
    m, ops = insn.mnem, insn.ops
    if m.endswith((".i", ".ni")) and m.split(".")[0] in ("cfc2", "ctc2", "qmfc2", "qmtc2"):
        m = m.split(".")[0]                          # VU0 interlock variants move the same bits
    if m in _ALU:
        spec, out = _ALU[m]
        op = _apply(m, ops, spec, out)
        if m == "or" and (ops[1][1] == 0 or ops[2][1] == 0):               # or x, y, zero: 64-bit move
            return replace(op, out="pass", cls="move64")
        return op
    if m in _DALU:
        op = _apply(m, ops, _DALU[m], "wide", "dalu")
        if m in ("daddu", "dadd") and (ops[1][1] == 0 or ops[2][1] == 0):  # daddu x, y, zero: 64-bit move
            return replace(op, out="pass", cls="move64")
        if m in ("daddiu", "daddi") and ops[2][1] == 0:                    # daddiu x, y, 0: move
            return replace(op, out="pass", cls="move64")
        if m in ("daddiu", "daddi") and ops[1][1] == 0:                    # daddiu x, zero, imm: Metrowerks `li`
            return replace(op, out="c32", cls="move64")
        return op
    if m in ("dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
        sa = ops[2][1]
        if m == "dsra" or (m in ("dsll", "dsrl") and sa == 0):
            out = "pass"
        elif m == "dsra32" or (m == "dsrl32" and sa > 0):
            out = "c32"
        else:
            out = "wide"
        bits = 32 if m == "dsll32" else 64                                 # dsll32 only sees the low word
        return _apply(m, ops, "d s%d x" % bits, out, "dshift")
    if m in ("dsllv", "dsrlv", "dsrav"):
        return _apply(m, ops, "d s64 s32", "pass" if m == "dsrav" else "wide", "dshift")
    if m in ("movz", "movn"):
        return _merge_old(_apply(m, ops, "d s64 s64", "pass", "cmove"), 64)
    # ---- memory
    if m in _LOAD32:
        return _apply(m, ops, "d m", "c32")
    if m == "lwu":
        return _apply(m, ops, "d m", "wide", "lwu")
    if m == "lwl":
        return _merge_old(_apply(m, ops, "d m", "c32"), 32)
    if m == "lwr":                                   # a partial LWR keeps bits 63..32 and the high byte(s)
        return _merge_old(_apply(m, ops, "d m", "pass"), 64)
    if m == "ld":
        return _apply(m, ops, "d m", "blob", "ld")
    if m in ("ldl", "ldr"):
        return _merge_old(_apply(m, ops, "d m", "blob", "ldlr"), 64)
    if m == "lq":
        return _apply(m, ops, "D m", "blob", "lq")
    if m in _STORE32:
        return _apply(m, ops, "s32 m", "c32", store=_STORE32[m], pure=False)
    if m == "sd":
        return _apply(m, ops, "s64 m", "c32", "sd", store=64, pure=False)
    if m in ("sdl", "sdr"):
        return _apply(m, ops, "s64 m", "c32", "sdlr", store=64, pure=False)
    if m == "sq":
        return _apply(m, ops, "s128 m", "c32", "sq", store=128, pure=False)
    if m in ("lwc1", "swc1", "lqc2", "sqc2", "cache", "pref"):
        return _apply(m, ops, "x m", "c32", pure=False)
    # ---- HI/LO
    if m in _HILO_MUL:
        p = _HILO_MUL[m]
        acc = ("hi" + p, "lo" + p) if m.startswith("madd") else ()
        spec = "s32 s32" if len(ops) == 2 else "d s32 s32"
        op = _apply(m, ops, spec, "c32", pure=False)
        return replace(op, hilo_w=("hi" + p, "lo" + p), hilo_r=acc, hilo_out="c32")
    if m in ("div", "divu", "div1", "divu1"):
        p = "1" if m.endswith("1") else "0"
        srcs = tuple((o[1], 32) for o in ops[-2:] if o[0] == "r" and o[1] != 0)
        return Op(m, srcs=srcs, hilo_w=("hi" + p, "lo" + p), hilo_out="c32", pure=False)
    if m in ("mfhi", "mflo", "mfhi1", "mflo1"):
        h = ("hi" if "hi" in m else "lo") + ("1" if m.endswith("1") else "0")
        return replace(_apply(m, ops, "d", "pass"), hilo_r=(h,))
    if m in ("mthi", "mtlo", "mthi1", "mtlo1"):
        h = ("hi" if "hi" in m else "lo") + ("1" if m.endswith("1") else "0")
        return replace(_apply(m, ops, "s64", "c32", pure=False), hilo_w=(h,), hilo_out="pass")
    # ---- MMI / shift-amount register
    if m in _MMI_MOVE and len(ops) == 3 and (ops[1][1] == 0 or ops[2][1] == 0):
        return _apply(m, ops, "D s128 s128", "pass", "move128")
    if m in ("pmfhi", "pmflo") or m.startswith("pmfhl"):
        return replace(_apply(m, ops, "D", "wide", "mmi"), hilo_r=ALL_HILO)
    if m in ("pmthi", "pmtlo") or m.startswith("pmthl"):
        w = {"pmthi": ("hi0", "hi1"), "pmtlo": ("lo0", "lo1")}.get(m, ALL_HILO)
        return replace(_apply(m, ops, "s128", "c32", "mmi", pure=False), hilo_w=w, hilo_out="wide")
    if m == "plzcw":                                 # writes the two low words, keeps the upper 64 bits
        return _merge_old(_apply(m, ops, "d s64", "wide", "mmi"), 128)
    if m == "mfsa":
        return _apply(m, ops, "d", "c32")
    if m == "mtsa":
        return _apply(m, ops, "s32", "c32", pure=False)
    if m in ("mtsab", "mtsah"):
        return _apply(m, ops, "s32 x", "c32", pure=False)
    if m.startswith("p") or m == "qfsrv":
        if not ops or ops[0][0] != "r" or any(o[0] not in ("r", "imm") for o in ops):
            raise LiftError("%s: unexpected MMI operands" % m)
        roles = ["D"] + ["s128" if o[0] == "r" else "x" for o in ops[1:]]
        hilo_w = ALL_HILO if m.startswith(("pmult", "pmadd", "pmsub", "pdiv", "phm")) else ()
        hilo_r = ALL_HILO if m.startswith(("pmadd", "pmsub")) else ()
        op = _apply(m, ops, " ".join(roles), "wide", "mmi", pure=not hilo_w)
        return replace(op, hilo_w=hilo_w, hilo_r=hilo_r, hilo_out="wide")
    # ---- coprocessor moves
    if m in ("mfc1", "cfc1", "mfc0", "cfc2"):
        return _apply(m, ops, "d x", "c32")
    if m in ("mtc1", "ctc1", "mtc0", "ctc2"):
        return _apply(m, ops, "s32 x", "c32", pure=False)
    if m == "qmfc2":
        return _apply(m, ops, "D x", "wide", "cop2")
    if m == "qmtc2":
        return _apply(m, ops, "s128 x", "c32", "cop2", pure=False)
    # ---- control flow
    if m in _BRANCH2:
        return _apply(m, ops, "s64 s64 x", "c32", pure=False)
    if m in _BRANCH1:
        return _apply(m, ops, "s64 x", "c32", pure=False)
    if m == "jr":
        return _apply(m, ops, "s32", "c32", pure=False)
    if m == "jalr":
        return _apply(m, ops, "s32" if len(ops) == 1 else "d s32", "c32", pure=False)
    if m in _TRAPS:
        spec = "s64 s64" if m in ("tge", "tgeu", "tlt", "tltu", "teq", "tne") else "s64 x"
        return _apply(m, ops, spec + " x" * (len(ops) - len(spec.split())), "c32", pure=False)
    if m in _NO_GPR or all(o[0] not in ("r", "mem") for o in ops):
        return Op(m, pure=False)                     # no GPR effect (FPU arithmetic, VU0 macro ops, j/jal...)
    raise LiftError("no IR rule for %s (%s)" % (m, insn.text))


LINKS = {"jal": 31, "bgezal": 31, "bltzal": 31, "bgezall": 31, "bltzall": 31}


def link_register(insn):
    """GPR written with a return address by a call instruction (jal/bal-like: ra; jalr: rd), else None."""
    if insn.mnem in LINKS:
        return 31
    if insn.mnem == "jalr":
        return 31 if len(insn.ops) == 1 else (insn.ops[0][1] or None)
    return None
