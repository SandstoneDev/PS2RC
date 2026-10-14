"""Control-flow classification of one decoded R5900 instruction.

kinds: none · branch (conditional, direct target) · jump (unconditional direct: j, beq r,r) · call (jal, bal-like) ·
call_conditional (bltzal/bgezal on a real register) · call_indirect (jalr) · jump_indirect (jr != ra) · return (jr ra) ·
syscall · break · eret · trap (conditional traps; execution continues when not taken) · invalid.
All branches/jumps have one delay slot (EE Core ISM); branch-likely nullifies it when not taken. ERET has none.
"""
from dataclasses import dataclass

_COND = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bc0f", "bc0t", "bc1f", "bc1t", "bc2f", "bc2t"}
_LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bc0fl", "bc0tl", "bc1fl", "bc1tl", "bc2fl",
           "bc2tl", "bltzall", "bgezall"}
_LINKING = {"bltzal", "bgezal", "bltzall", "bgezall"}
_TRAPS = {"tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"}


@dataclass(slots=True, frozen=True)
class Flow:
    kind: str
    target: int = None
    likely: bool = False
    has_delay_slot: bool = False
    reg: int = None        # register holding the target (jr/jalr)
    link: int = None       # register receiving the return address


def flow(insn):
    if not insn.valid:
        return Flow("invalid")
    m = insn.mnem
    ops = insn.ops
    if m in ("j", "jal"):
        return Flow("jump" if m == "j" else "call", ops[0][1], has_delay_slot=True, link=31 if m == "jal" else None)
    if m == "jr":
        rs = ops[0][1]
        return Flow("return" if rs == 31 else "jump_indirect", has_delay_slot=True, reg=rs)
    if m == "jalr":
        rd, rs = (31, ops[0][1]) if len(ops) == 1 else (ops[0][1], ops[1][1])
        return Flow("call_indirect", has_delay_slot=True, reg=rs, link=rd)
    if m in _COND or m in _LIKELY or m in _LINKING:
        target = ops[-1][1]
        likely = m in _LIKELY
        if m in _LINKING:
            rs = ops[0][1]
            kind = "call" if rs == 0 and m.startswith("bgez") else "call_conditional"
            return Flow(kind, target, likely, True, link=31)
        if m in ("beq", "beql") and ops[0][1] == ops[1][1]:
            return Flow("jump", target, likely, True)
        return Flow("branch", target, likely, True)
    if m == "syscall":
        return Flow("syscall")
    if m == "break":
        return Flow("break")
    if m == "eret":
        return Flow("eret")
    if m in _TRAPS:
        return Flow("trap")
    return Flow("none")
