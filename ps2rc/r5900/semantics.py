"""Instruction semantics helpers (register reads/writes) - grows into the IR lifter's source of truth (Phase 3)."""

# Mnemonics whose first GPR operand is a SOURCE (or that write no GPR at all).
_NO_GPR_WRITE = {
    # stores
    "sb", "sh", "sw", "sd", "sq", "swl", "swr", "sdl", "sdr", "swc1", "sqc2",
    # branches / jumps (except linking forms handled below)
    "beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl", "bltz", "bgez", "bltzl", "bgezl",
    "bc0f", "bc0t", "bc0fl", "bc0tl", "bc1f", "bc1t", "bc1fl", "bc1tl", "bc2f", "bc2t", "bc2fl", "bc2tl",
    "j", "jr",
    # moves to special registers / coprocessors
    "mthi", "mtlo", "mthi1", "mtlo1", "mtsa", "mtsab", "mtsah", "pmthi", "pmtlo", "pmthl.lw",
    "mtc0", "mtc1", "ctc1", "qmtc2", "qmtc2.i", "ctc2", "ctc2.i", "mtps", "mtpc",
    "mtbpc", "mtiab", "mtiabm", "mtdab", "mtdabm", "mtdvb", "mtdvbm",
    # divides (HI/LO only)
    "div", "divu", "div1", "divu1", "pdivw", "pdivuw", "pdivbw",
    # traps, system
    "tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei",
    "syscall", "break", "sync", "sync.p", "cache", "pref", "eret", "ei", "di", "tlbr", "tlbwi", "tlbwr", "tlbp",
    # FPU loads/arith write FPRs, not GPRs
    "lwc1", "lqc2",
}
_TWO_OP_HILO = {"mult", "multu", "mult1", "multu1", "madd", "maddu", "madd1", "maddu1"}
_LINK = {"jal", "bltzal", "bgezal", "bltzall", "bgezall"}


def dest_gpr(insn):
    """GPR index written by `insn`, or None (also None for writes to $zero)."""
    if not insn.valid:
        return None
    m, ops = insn.mnem, insn.ops
    if m in _LINK:
        return 31
    if m == "jalr":
        return 31 if len(ops) == 1 else (ops[0][1] or None)
    if m in _NO_GPR_WRITE or m.startswith("v") or m.endswith(".s") or m.startswith("c."):
        return None
    if m in _TWO_OP_HILO and len(ops) == 2:
        return None
    if not ops or ops[0][0] != "r":
        return None
    return ops[0][1] or None
