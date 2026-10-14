"""C back-end v0: translate decoded R5900 functions into C over the runtime context (src/runtime/r5900_rt.h).

Correctness first: every GPR lives in the context as 128 bits, 32-bit ops sign-extend into the low 64 bits and leave
the upper 64 bits alone (PCSX2/EE semantics), HI/LO hold both pipelines. Delay slots are emitted after the branch
condition is evaluated; branch-likely executes its slot only when taken. Control flow:
  jal/bal          link written, then the delay slot, then a direct C call
  jalr             target read, link written, delay slot, call through rc_dispatch (guest address -> host function)
  j                tail call (a `j` into the function's own code: goto);  jr ra: return
  jr reg           C switch when function discovery recovered a jump table, else an indirect tail call
  branch / fall-through out of the function onto another function's start: (conditional) tail call
  syscall/break/eret/traps/COP0: rt_fatal (HLE comes later); BC0F/BC0T: rt_cpcond0() (DMA polling, HLE hook)
COP2 = VU0 macro mode and the LQC2/SQC2/QMTC2/QMFC2/CFC2/CTC2 transfers: vu/macro.py (VU0 state in the context,
VCALLMS through the rt_vu0_callms hook).

v0 (cache=False): every register access goes through the context. v1 (cache=True): the low 64 bits of the GPRs and the
FPRs a function uses live in C locals (r<n>, f<n>); the context is written back before every call, dispatch, return
and rt_fatal and reloaded after calls; 128-bit operations (LQ/SQ/MMI) run on the context with their registers synced
around them. The upper 64 GPR bits always live in the context. Both variants are bit-exact (PoC-1/PoC-2).
v2 narrows proven-canonical GPRs to int32_t locals, v3 skips the FPU input clamp of known-clean operands, v4 is ABI
mode (translate_function). abi_exempt (the non-ABI detector's result): functions translated exact inside an ABI-mode
program, every direct transfer to them a full boundary. v5 = v4 + the size levers of V5_LEVELS (S1 clamp/aligned
copies, S2 128-bit values / HI-LO / ACC / condition bit in locals and clamp-once, S3 no callee-saved save/restore
pairs and one-word context write-backs).
"""
import re

from .ir import LiftError, lift, link_register
from .r5900 import decode
from .r5900.flow import flow
from .vu import macro as vu0
from .analysis import retdemand as _RD

_LOCAL = {"on": False, "narrow": frozenset()}   # v1: registers in C locals; v2: `narrow` ones as int32_t

# back-end v5 = v4 + size levers, cumulative steps: name -> lever set.
#   S1 (exact, no new assumption):
#   clampmin2  FPU input clamp through the branchless f_in5 / F5 (runtime) - exact, same mapping as f_in
#   ctxal      128-bit context operations (LQ/SQ/LQC2-style copies) through the function's memory window rm_lq/rm_sq
#              (16-byte aligned on both sides, RAM base/mask in locals) instead of rt_lq/rt_sq
#   S2 (ABI-mode functions; state the EE ABI does not pass between functions moves from the context to C locals):
#   q128       LQ, SQ and the 128-bit move idiom paddub/por d,s,zero on C locals (upper halves in h<n>); other
#              128-bit instructions stay on the context with their upper halves synced
#   hilo       HI/LO (both pipelines) in C locals lo_<p>/hi_<p>
#   acc        FPU accumulator in a C local acc_
#   fcc        FPU condition bit in a C local fcc_
#   fclean     FPR cleanliness (no input clamp needed) survives calls except for the reloaded f0/f1 (in ABI mode the
#              other FPR locals are not touched by a call)
#   clamp1     an LWC1/MTC1 value used only through the input clamp is clamped once where it is loaded
#   S3 (ABI mode):
#   nocs       callee-saved save/restore pairs (SQ/SD + LQ/LD of s0-s7/fp/ra through sp) dropped (cs_pairs)
#   ctx32      narrowed registers written back to the context as one word where no reader needs the upper word:
#              sp/gp everywhere (every v5 load of the sp/gp context slots takes the low word - they are canonical
#              32-bit addresses on the EE - and a 128-bit instruction reading them as data gets a full write first);
#              arguments of a direct call whose callee only reads that slot's low word (_lowonly_args)
#   S4 (ABI-mode functions; memory model: guard areas around the guest window):
#   maskbase   guest addresses as rmb_*(m_, base, offset): the window mask applies to the base register only and the
#              offset is added after it, so ram + (base & mask) is shared by the accesses through one base. Same
#              address as rm_* unless (base & mask) + offset leaves the window (an end pointer at 0x02000000 with a
#              negative offset; accesses beyond the end of RAM) - the harness keeps RMB_GUARD bytes around the window
#              and RMB_CHECK builds report every such access (r5900_rt.h)
#   speed levers on top of S4 (ABI-mode functions with maskbase):
#   ufuse      adjacent LDL/LDR, LWL/LWR, SDL/SDR, SWL/SWR pairs of one register and one base whose offsets cover
#              exactly the 8 (4) bytes at base + offset: one unaligned access (rmb_uld/ulw/usd/usw: lwl/lwr or
#              swl/swr on the Allegrex) instead of two merges with variable shifts and masks. Exact: the pair moves
#              exactly those bytes whatever the alignment, the old register value never survives the pair, and the
#              address of every byte is the maskbase one (ram + (base & mask) + offset + k)
V5_S1 = frozenset({"clampmin2", "ctxal"})
V5_S2 = V5_S1 | {"q128", "hilo", "acc", "fcc", "fclean", "clamp1"}
V5_S3A = V5_S2 | {"nocs"}
V5_S3 = V5_S3A | {"ctx32"}
V5_S4 = V5_S3 | {"maskbase"}
#   vfloc      VU0 macro mode: VF01-VF31 / ACC lanes in C locals (vu.macro.stmt_loc) in ABI-mode functions; the lanes
#              the function writes go back to c->vu0 at every boundary where the dirty set says they differ (calls,
#              returns, syscalls, fatal points, VCALLMS, jump-table exits), every used lane is reloaded after calls and
#              VCALLMS. VU0 registers are not part of the EE ABI: this keeps the context exact wherever another
#              function, the HLE or a VU0 micro subroutine can read it
#              VFLOC_SKIP: C names of functions kept on the context form (a per-function choice by measured code; the
#              lanes in locals raise the register pressure of long VU0 sequences - tests/phase6psp/y_vfloc_skip.txt)
V5_Y1 = V5_S4 | {"ufuse"}
V5_Y2 = V5_Y1 | {"vfloc"}
#   vcm        (round 4) VCALLMS / VCALLMSR write back only the dirty VU0 lanes (lever vfloc) instead of every dirty
#              local: the micro subroutine HLE reads and writes only c->vu0, and the dirty sets are not cleared at a
#              VCALLMS (_dirty), so every GPR / FPR / HI-LO local still reaches the context at the next boundary
V5_Y3 = V5_Y2 | {"vcm"}
#   rlive      a return writes back only the parts of v0 / v1 / f0 / f1 that
#              some caller can observe (analysis.retdemand: whole-program fixpoint over the call sites; v0 as one word
#              when no reader takes its upper word). Needs RLIVE["on"] set by the driver with the functions whose returns
#              must stay complete (hooks, natives, functions the HLE or another code unit calls, address-taken ones);
#              without it the lever does nothing (every function FULL)
V5_Y2R = V5_Y2 | {"rlive"}
V5_Y3R = V5_Y3 | {"rlive"}
#   argx       lever ctx32's one-word arguments also through pass-through callees: a function that hands its
#              incoming argument slot unchanged to a direct callee (no write-back, the slot is read there) keeps the
#              one-word property when that callee reads the slot as one word too (_lowonly_args per_target + a
#              fixpoint in _emit_parts) - direct callers then store such arguments as one word (no sign word)
V5_Y3X = V5_Y3 | {"argx"}
V5_Y3RX = V5_Y3R | {"argx"}
#   vwt        (lever vfloc) a VF register defined by LQC2 / QMTC2 (all four lanes from memory / a GPR) is written to
#              c->vu0 right there and counts as clean afterwards (no write-back at the next boundary): the lanes are
#              no longer kept alive up to the function's exit only to be written back, which removes most callee-saved
#              saves and spills of short VU0 helpers (a loaded matrix used once). The context sees the same final
#              values at every boundary (a later write of the register makes it dirty again, as before)
V5_Y3RXW = V5_Y3RX | {"vwt"}
#   mloc       (lever q128) the MMI word shuffles PCPYLD, PEXTLW, PROT3W (the VU0 helpers' operand packing) and the VU0
#              transfers QMTC2 / QMFC2 run on the C locals (low half r<n>, upper half h<n>; VF lanes
#              v<n>_<k> with vfloc) instead of the context: no 128-bit context round trip per instruction. The context
#              copies follow the q128 rules (written where the ABI makes them observable)
V5_Y3RXM = V5_Y3RXW | {"mloc"}
V5_LEVELS = {"v5s1": V5_S1, "v5s2": V5_S2, "v5s3": V5_S3, "v5s4": V5_S4}  # the variants of the PoC harnesses
V5_STEPS = dict(V5_LEVELS, v5s3a=V5_S3A, v5s4y1=V5_Y1, v5s4y2=V5_Y2, v5s4y3=V5_Y3,   # + size steps + speed steps
                v5s4y2r=V5_Y2R, v5s4y3r=V5_Y3R, v5s4y3x=V5_Y3X, v5s4y3rx=V5_Y3RX,        # + return / argument widths
                v5s4y3rxw=V5_Y3RXW, v5s4y3rxm=V5_Y3RXM)                                # + VF write-through, MMI locals
# lever rlive: driver settings (ps2rc/pipeline/translate.py --rlive) - on: apply to the next emit_units call(s) whose
#   unit name is in `units` (the driver sets the pack's main unit); full / indirect: guest addresses (see
#   analysis.retdemand.solve); stats: filled in
RLIVE = {"on": False, "units": (), "full": frozenset(), "indirect": frozenset(), "stats": {}}
VFLOC_SKIP = frozenset()                                 # set by the driver (ps2rc/pipeline/translate.py --vfloc-skip)
V5_EXACT = frozenset({"clampmin2", "ctxal", "ctx32"})   # levers also for exact functions (ctx32: only its sp/gp part)
_SPGP = (28, 29)
_ARG_MASK = 0xFF0                                       # a0-a3, t0-t3


def _v5(lever):
    return lever in _LOCAL.get("v5", ())


def _fin():
    """C name of the clamped-operand helper (F: raw FPR bits -> float with the R5900 input clamp)."""
    return "F5" if _v5("clampmin2") else "F"


def _acc():
    return "acc_" if _v5("acc") else "c->acc.u"


def _cmp(expr):
    """FPU compare: sets the condition bit."""
    return "fcc_ = (%s);" % expr if _v5("fcc") else "fpu_cmp(c, %s);" % expr


class Unsupported(Exception):
    pass


def R(n):
    if n == 0:
        return "0ull"
    if not _LOCAL["on"]:
        return "c->r[%d].d[0]" % n
    return "((uint64_t)(int64_t)r%d)" % n if n in _LOCAL["narrow"] else "r%d" % n


def S(n):
    return "((int64_t)%s)" % R(n)


def W(n, expr):
    """Assignment of a 64-bit value to GPR n (dropped for $zero)."""
    if n == 0:
        return ""
    if not _LOCAL["on"]:
        return "c->r[%d].d[0] = %s;" % (n, expr)
    return ("r%d = (int32_t)(%s);" if n in _LOCAL["narrow"] else "r%d = %s;") % (n, expr)


def W128(n, expr):
    return "" if n == 0 else "c->r[%d] = %s;" % (n, expr)


def F(n):
    return ("f%d" if _LOCAL["on"] else "c->fpr[%d].u") % n


def _mem_addr(op):
    base, off = op[1], op[2]
    if _LOCAL.get("on") and _v5("maskbase"):           # v5 S4: base and offset apart; translate_function turns
        return "/*B*/(uint32_t)%s, %d" % (R(base), off)  # rm_<op>(m_, /*B*/...) into rmb_<op>(m_, base, offset)
    return "(uint32_t)(%s + %d)" % (R(base), off)


_RMB_RE = re.compile(r"\brm_(lb|lh|lw|ld|sb|sh|sw|sd|lwl|lwr|ldl|ldr|swl|swr|sdl|sdr|lq|sq|uld|ulw|usd|usw)\(m_, "
                     r"/\*B\*/")
# lever ufuse: (first, second) mnemonic of an unaligned pair -> (fused op, offset of the first minus the second)
_UPAIR = {("ldl", "ldr"): ("uld", 7), ("ldr", "ldl"): ("uld", -7), ("lwl", "lwr"): ("ulw", 3),
          ("lwr", "lwl"): ("ulw", -3), ("sdl", "sdr"): ("usd", 7), ("sdr", "sdl"): ("usd", -7),
          ("swl", "swr"): ("usw", 3), ("swr", "swl"): ("usw", -3)}


def _ufuse_pairs(insns, flows, labels):
    """Lever ufuse: {address of the first instruction: (fused op, register, base, lowest offset)} for adjacent unaligned
    pairs that run as one unit: the second is no branch target, the first is no delay slot, both name the same register
    and base, the offsets differ by exactly 7 (3), and a load pair does not overwrite its own base."""
    out = {}
    for a, i in insns.items():
        j = insns.get(a + 4)
        if j is None or (i.mnem, j.mnem) not in _UPAIR or a + 4 in labels or a - 4 in out:
            continue
        p = insns.get(a - 4)
        if p is not None and flows[a - 4].has_delay_slot:
            continue
        op, d = _UPAIR[(i.mnem, j.mnem)]
        rt, base = i.ops[0][1], i.ops[1][1]
        if j.ops[0][1] != rt or j.ops[1][1] != base or i.ops[1][2] - j.ops[1][2] != d:
            continue
        if op in ("uld", "ulw") and base == rt:
            continue
        out[a] = (op, rt, base, min(i.ops[1][2], j.ops[1][2]))
    return out


def _ufuse_stmt(pair):
    """C statement of a fused unaligned pair (lever ufuse), in stmt()'s rt_ form (the cache path rewrites it to rm_ /
    rmb_ like every other access)."""
    op, rt, base, off = pair
    a = _mem_addr(("mem", base, off))
    if op == "uld":
        return W(rt, "rt_uld(c, %s)" % a) or ";"
    if op == "ulw":
        return W(rt, "SX32(rt_ulw(c, %s))" % a) or ";"
    return "rt_%s(c, %s, %s);" % (op, a, R(rt))


_SHIFT32 = {"sll": "SX32((uint32_t){t} << {s})", "srl": "SX32((uint32_t){t} >> {s})",
            "sra": "(uint64_t)(int64_t)((int32_t){t} >> {s})"}
_SHIFT64 = {"dsll": "({t} << {s})", "dsrl": "({t} >> {s})", "dsra": "(uint64_t)((int64_t){t} >> {s})",
            "dsll32": "({t} << ({s} + 32))", "dsrl32": "({t} >> ({s} + 32))",
            "dsra32": "(uint64_t)((int64_t){t} >> ({s} + 32))"}
_ALU3 = {"addu": "SX32((uint32_t){a} + (uint32_t){b})", "add": "SX32((uint32_t){a} + (uint32_t){b})",
         "subu": "SX32((uint32_t){a} - (uint32_t){b})", "sub": "SX32((uint32_t){a} - (uint32_t){b})",
         "and": "({a} & {b})", "or": "({a} | {b})", "xor": "({a} ^ {b})", "nor": "~({a} | {b})",
         "slt": "((int64_t){a} < (int64_t){b})", "sltu": "((uint64_t){a} < (uint64_t){b})",
         "daddu": "({a} + {b})", "dadd": "({a} + {b})", "dsubu": "({a} - {b})", "dsub": "({a} - {b})"}
_MMI3 = {"paddw", "psubw", "psubb", "paddub", "padduw", "psubsw", "pand", "por", "pxor", "pnor", "pcpyld", "pcpyud",
         "pextlw", "pextuw", "pextlh", "pextlb", "ppacw", "pinteh", "pcgtw", "pceqw", "pmaxw", "pminw",
         "paddh", "pcgth", "pmaxh", "pminh", "ppacb", "pextub"}
_MMI_SHIFT = {"psllw", "psrlw", "psllh", "psrlh", "psrah"}
_MMI1 = {"ppac5", "pexew", "prot3w", "pcpyh"}
_LOADS = {"lb": "SX8", "lbu": "ZX8", "lh": "SX16", "lhu": "ZX16", "lw": "SXW", "lwu": "ZXW", "ld": "D"}


def _cop0_reload(i):
    """After rt_fatal(c, "mfc0", ...): a harness that goes on (COP0 HLE) has written the result into the context, so
    a register held in a C local is reloaded from it (Status polls and Count reads, where v1+ kept the stale local:
    65 sites in a large game)."""
    if i.mnem == "mfc0" and i.ops[0][1] and _LOCAL.get("on"):
        return " /*LOAD:%d*/" % i.ops[0][1]
    return ""


def stmt(i):
    """C statement(s) for a non-control-flow instruction."""
    m, o = i.mnem, i.ops
    if vu0.handles(i):                                   # COP2: VU0 macro mode and the EE<->VU0 transfers
        s = (vu0.stmt_loc if _LOCAL.get("vfloc") else vu0.stmt)(i, _LOCAL.get("vpc"), R, W, _mem_addr)
        if s is None:
            raise Unsupported("%08x: %s" % (i.pc, i.text))
        t = _vwt_target(i) if _LOCAL.get("vwt") else 0
        if t:                                            # lever vwt: the loaded lanes go to the context at once
            s += " " + " ".join("c->vu0.vf[%d].w[%d] = v%d_%d;" % (t, k, t, k) for k in range(4))
        return s
    if m in _ALU3:
        return W(o[0][1], _ALU3[m].format(a=R(o[1][1]), b=R(o[2][1])))
    if m in ("neg", "negu"):
        return W(o[0][1], "SX32(0u - (uint32_t)%s)" % R(o[1][1]))
    if m in ("dneg", "dnegu"):
        return W(o[0][1], "(0ull - %s)" % R(o[1][1]))
    if m in ("addiu", "addi"):
        return W(o[0][1], "SX32((uint32_t)%s + (uint32_t)%d)" % (R(o[1][1]), o[2][1]))
    if m in ("daddiu", "daddi"):
        return W(o[0][1], "(%s + (uint64_t)(int64_t)%d)" % (R(o[1][1]), o[2][1]))
    if m == "slti":
        return W(o[0][1], "((int64_t)%s < (int64_t)%d)" % (R(o[1][1]), o[2][1]))
    if m == "sltiu":
        return W(o[0][1], "((uint64_t)%s < (uint64_t)(int64_t)%d)" % (R(o[1][1]), o[2][1]))
    if m in ("andi", "ori", "xori"):
        op = {"andi": "&", "ori": "|", "xori": "^"}[m]
        return W(o[0][1], "(%s %s 0x%xull)" % (R(o[1][1]), op, o[2][1]))
    if m == "lui":
        return W(o[0][1], "SX32(0x%xu)" % ((o[1][1] << 16) & 0xFFFFFFFF))
    if m in _SHIFT32:
        return W(o[0][1], _SHIFT32[m].format(t=R(o[1][1]), s=o[2][1]))
    if m in _SHIFT64:
        return W(o[0][1], _SHIFT64[m].format(t=R(o[1][1]), s=o[2][1]))
    if m in ("sllv", "srlv", "srav"):
        s = "((uint32_t)%s & 31)" % R(o[2][1])
        return W(o[0][1], _SHIFT32[m[:-1]].format(t=R(o[1][1]), s=s))
    if m in ("dsllv", "dsrlv", "dsrav"):
        s = "((uint32_t)%s & 63)" % R(o[2][1])
        e = {"dsllv": "({t} << {s})", "dsrlv": "({t} >> {s})", "dsrav": "(uint64_t)((int64_t){t} >> {s})"}[m]
        return W(o[0][1], e.format(t=R(o[1][1]), s=s))
    if m in ("movz", "movn"):
        cmp = "==" if m == "movz" else "!="
        if o[0][1] == 0:
            return ""
        return "{ if (%s %s 0) %s }" % (R(o[2][1]), cmp, W(o[0][1], R(o[1][1])))
    # HI/LO
    hl = _v5("hilo")                                     # v5: HI/LO in C locals lo_<p>, hi_<p>
    if m in ("mult", "multu", "mult1", "multu1", "madd", "maddu", "madd1", "maddu1"):
        p = 1 if m.endswith("1") else 0
        base = m.rstrip("1") if p else m
        regs = o if len(o) == 2 else o[1:]
        if hl:
            s = "rl_%s(&lo_%d, &hi_%d, %s, %s);" % (base, p, p, R(regs[0][1]), R(regs[1][1]))
            return s + (" " + W(o[0][1], "lo_%d" % p) if len(o) == 3 else "")
        fn = {"mult": "rt_mult", "multu": "rt_multu", "madd": "rt_madd", "maddu": "rt_maddu"}[base]
        s = "%s(c, %d, %s, %s);" % (fn, p, R(regs[0][1]), R(regs[1][1]))
        if len(o) == 3:
            s += " " + W(o[0][1], "c->lo.d[%d]" % p)
        return s
    if m in ("div", "divu", "div1", "divu1"):
        p = 1 if m.endswith("1") else 0
        fn = "rt_div" if m.startswith("div") and "u" not in m else "rt_divu"
        if hl:
            return "rl_%s(&lo_%d, &hi_%d, %s, %s);" % (fn[3:], p, p, R(o[1][1]), R(o[2][1]))
        return "%s(c, %d, %s, %s);" % (fn, p, R(o[1][1]), R(o[2][1]))
    if m in ("mfhi", "mflo", "mfhi1", "mflo1"):
        p = 1 if m.endswith("1") else 0
        if hl:
            return W(o[0][1], "%s_%d" % ("hi" if "hi" in m else "lo", p))
        return W(o[0][1], "c->%s.d[%d]" % ("hi" if "hi" in m else "lo", p))
    if m in ("mthi", "mtlo", "mthi1", "mtlo1"):
        p = 1 if m.endswith("1") else 0
        if hl:
            return "%s_%d = %s;" % ("hi" if "hi" in m else "lo", p, R(o[0][1]))
        return "c->%s.d[%d] = %s;" % ("hi" if "hi" in m else "lo", p, R(o[0][1]))
    # loads / stores
    if m in _LOADS:
        a = _mem_addr(o[1])
        conv = {"SX8": "(uint64_t)(int64_t)(int8_t)rt_lb(c, {a})", "ZX8": "(uint64_t)rt_lb(c, {a})",
                "SX16": "(uint64_t)(int64_t)(int16_t)rt_lh(c, {a})", "ZX16": "(uint64_t)rt_lh(c, {a})",
                "SXW": "SX32(rt_lw(c, {a}))", "ZXW": "(uint64_t)rt_lw(c, {a})", "D": "rt_ld(c, {a})"}[_LOADS[m]]
        return W(o[0][1], conv.format(a=a))
    if m in ("lwl", "lwr", "ldl", "ldr"):
        t = o[0][1]
        return W(t, "(uint64_t)rt_%s(c, %s, %s)" % (m, _mem_addr(o[1]), R(t)))
    if m in ("sb", "sh", "sw", "sd", "swl", "swr", "sdl", "sdr"):
        return "rt_%s(c, %s, %s);" % (m, _mem_addr(o[1]), R(o[0][1]))
    if m == "lq":
        return "" if o[0][1] == 0 else "rt_lq(c, %s, &c->r[%d]);" % (_mem_addr(o[1]), o[0][1])
    if m == "sq":
        return "rt_sq(c, %s, &c->r[%d]);" % (_mem_addr(o[1]), o[0][1])
    if m == "lwc1":
        if i.pc in _LOCAL.get("clamp1", ()):            # v5 clamp1: clamped once here (only clamped uses follow)
            return "%s = %s(rt_lw(c, %s));" % (F(o[0][1]), "f_in5" if _v5("clampmin2") else "f_in", _mem_addr(o[1]))
        return "%s = rt_lw(c, %s);" % (F(o[0][1]), _mem_addr(o[1]))
    if m == "swc1":
        return "rt_sw(c, %s, %s);" % (_mem_addr(o[1]), F(o[0][1]))
    if m in ("sync", "sync.p", "pref", "cache"):
        return ";"
    if m in ("mfc0", "mtc0", "di", "ei"):                # kernel-mode state: HLE later, fatal for now
        return "{ /*FLUSH@i%08x|fatal*/ rt_fatal(c, \"%s\", 0x%08xu, 0x%08xu);%s }" % (i.pc, m, i.pc, i.word,
                                                                                     _cop0_reload(i))
    if m == "syscall":                                   # EE kernel call: number in v1, HLE table (rt_syscall)
        return "{ /*FLUSH@i%08x|sys*/ rt_syscall(c, 0x%08xu); /*RELOADSYS*/ }" % (i.pc, i.pc)
    if m in ("break", "eret"):                           # also met in delay slots: `beql v0,zero,1f; break 7`
        return "{ /*FLUSH@i%08x|fatal*/ rt_fatal(c, \"%s\", 0x%08xu, 0x%08xu); return; }" % (i.pc, m, i.pc, i.word)
    if m in _TRAP_COND or m[:-1] in _TRAP_COND:
        return _trap(i)
    # shift-amount register / MMI
    if m == "mtsab":
        return "c->sa = (((uint32_t)%s ^ (uint32_t)%d) & 15u) * 8u;" % (R(o[0][1]), o[1][1])
    if m == "mtsah":
        return "c->sa = (((uint32_t)%s ^ (uint32_t)%d) & 7u) * 16u;" % (R(o[0][1]), o[1][1])
    if m == "mtsa":
        return "c->sa = (uint32_t)%s;" % R(o[0][1])
    if m == "mfsa":
        return W(o[0][1], "(uint64_t)c->sa")
    if m in _MMI3:
        return W128(o[0][1], "mmi_%s(c->r[%d], c->r[%d])" % (m, o[1][1], o[2][1]))
    if m in _MMI1:
        return W128(o[0][1], "mmi_%s(c->r[%d])" % (m, o[1][1]))
    if m in _MMI_SHIFT:
        return W128(o[0][1], "mmi_%s(c->r[%d], %d)" % (m, o[1][1], o[2][1]))
    if m == "qfsrv":
        return W128(o[0][1], "mmi_qfsrv(c->r[%d], c->r[%d], c->sa)" % (o[1][1], o[2][1]))
    if m == "plzcw":
        return W128(o[0][1], "mmi_plzcw(c->r[%d], c->r[%d])" % (o[0][1], o[1][1]))
    # FPU
    fst = _LOCAL.get("fpc")
    if fst is not None:                                  # v3: operands of known-clean FPRs are not clamped again
        fast = _fpu_fast(m, o, fst)
        if fast is not None:
            return fast
    if m in ("add.s", "sub.s", "mul.s", "div.s"):
        return "%s = fpu_%s(%s, %s);" % (F(o[0][1]), m[:3], F(o[1][1]), F(o[2][1]))
    if m in ("max.s", "min.s"):
        return "%s = fpu_%s(%s, %s);" % (F(o[0][1]), m[:3], F(o[1][1]), F(o[2][1]))
    sfx = "5" if _v5("clampmin2") else ""                # v5: helpers that clamp through f_in5
    if m == "sqrt.s":
        return "%s = fpu_sqrt%s(%s);" % (F(o[0][1]), sfx, F(o[1][1]))
    if m == "rsqrt.s":
        return "%s = fpu_rsqrt%s(%s, %s);" % (F(o[0][1]), sfx, F(o[1][1]), F(o[2][1]))
    if m == "abs.s":
        return "%s = %s & 0x7FFFFFFFu;" % (F(o[0][1]), F(o[1][1]))
    if m == "neg.s":
        return "%s = %s ^ 0x80000000u;" % (F(o[0][1]), F(o[1][1]))
    if m == "mov.s":
        return "%s = %s;" % (F(o[0][1]), F(o[1][1]))
    if m in ("adda.s", "suba.s", "mula.s"):
        return "%s = fpu_%s(%s, %s);" % (_acc(), m[:3], F(o[0][1]), F(o[1][1]))
    if m in ("madd.s", "msub.s"):
        return "%s = fpu_%s(%s, %s, %s);" % (F(o[0][1]), m[:4], _acc(), F(o[1][1]), F(o[2][1]))
    if m in ("madda.s", "msuba.s"):
        return "%s = fpu_%s%s(%s, %s, %s);" % (_acc(), m[:5], sfx, _acc(), F(o[0][1]), F(o[1][1]))
    if m == "cvt.w.s":
        return "%s = fpu_cvt_w(%s);" % (F(o[0][1]), F(o[1][1]))
    if m == "cvt.s.w":
        return "%s = fpu_cvt_s(%s);" % (F(o[0][1]), F(o[1][1]))
    if m in ("c.eq.s", "c.lt.s", "c.le.s"):
        op = {"c.eq.s": "==", "c.lt.s": "<", "c.le.s": "<="}[m]
        return _cmp("%s(%s) %s %s(%s)" % (_fin(), F(o[0][1]), op, _fin(), F(o[1][1])))
    if m == "c.f.s":
        return "fcc_ = 0;" if _v5("fcc") else "c->fcr31 &= ~FCR31_C;"
    if m == "mfc1":
        return W(o[0][1], "SX32(%s)" % F(o[1][1]))
    if m == "mtc1":
        if i.pc in _LOCAL.get("clamp1", ()):
            return "%s = %s((uint32_t)%s);" % (F(o[1][1]), "f_in5" if _v5("clampmin2") else "f_in", R(o[0][1]))
        return "%s = (uint32_t)%s;" % (F(o[1][1]), R(o[0][1]))
    if m == "cfc1":
        if _v5("fcc") and o[1][1] == 31:                 # FCR31 with the condition bit of the local
            return W(o[0][1], "SX32((c->fcr31 & ~FCR31_C) | (fcc_ ? FCR31_C : 0u))")
        return W(o[0][1], "fpu_cfc1(c, %d)" % o[1][1])
    if m == "ctc1":
        if _v5("fcc") and o[1][1] == 31:
            return "{ c->fcr31 = (uint32_t)%s; fcc_ = (c->fcr31 & FCR31_C) != 0; }" % R(o[0][1])
        return "if (%d == 31) c->fcr31 = (uint32_t)%s;" % (o[1][1], R(o[0][1]))
    raise Unsupported("%08x: %s" % (i.pc, i.text))


def _fx(n, st):
    """Float operand: a known-clean FPR (finite, not denormal: written by a modelled FPU operation) is used as is,
    any other one goes through the R5900 input clamp."""
    return ("u2f(%s)" if st[0] >> n & 1 else _fin() + "(%s)") % F(n)


def _fpu_fast(m, o, st):
    """v3 emission of an FPU instruction with clean-operand knowledge `st` = (clean FPR mask, ACC clean). The output
    clamp of the model is dead: with clamped (finite, normal) inputs, round-toward-zero turns overflow into FLT_MAX
    and flush-to-zero removes denormal results (host MXCSR and Allegrex FCR31 are set that way)."""
    ops = {"add.s": "+", "sub.s": "-", "mul.s": "*", "adda.s": "+", "suba.s": "-", "mula.s": "*"}
    if m in ("add.s", "sub.s", "mul.s"):
        return "%s = f2u(%s %s %s);" % (F(o[0][1]), _fx(o[1][1], st), ops[m], _fx(o[2][1], st))
    if m == "div.s":
        d, a, b = o[0][1], o[1][1], o[2][1]
        return ("%s = ((%s & 0x7F800000u) == 0) ? (((%s ^ %s) & 0x80000000u) | F_FMAX) : f2u(%s / %s);"
                % (F(d), F(b), F(a), F(b), _fx(a, st), _fx(b, st)))
    if m == "sqrt.s" and st[0] >> o[1][1] & 1:
        return "%s = fpu_sqrt_c(%s);" % (F(o[0][1]), F(o[1][1]))
    if m in ("adda.s", "suba.s", "mula.s"):
        return "%s = f2u(%s %s %s);" % (_acc(), _fx(o[0][1], st), ops[m], _fx(o[1][1], st))
    if m in ("madd.s", "msub.s"):
        acc = ("u2f(%s)" if st[1] else _fin() + "(%s)") % _acc()
        return "%s = f2u(%s %s (%s * %s));" % (F(o[0][1]), acc, "+" if m == "madd.s" else "-",
                                                _fx(o[1][1], st), _fx(o[2][1], st))
    if m in ("madda.s", "msuba.s") and st[1]:
        return "%s = f2u(u2f(%s) %s (%s * %s));" % (_acc(), _acc(), "+" if m == "madda.s" else "-",
                                                    _fx(o[0][1], st), _fx(o[1][1], st))
    if m in ("c.eq.s", "c.lt.s", "c.le.s"):
        op = {"c.eq.s": "==", "c.lt.s": "<", "c.le.s": "<="}[m]
        return _cmp("%s %s %s" % (_fx(o[0][1], st), op, _fx(o[1][1], st)))
    return None


_FPU_CLEAN_RESULT = {"add.s", "sub.s", "mul.s", "div.s", "sqrt.s", "rsqrt.s", "madd.s", "msub.s", "cvt.s.w"}
_FPU_ACC_WRITE = {"adda.s", "suba.s", "mula.s", "madda.s", "msuba.s"}


def fn_cfg(img, start, addrs, jump_tables, func_starts):
    """Instruction-level CFG of one function (analysis.width._Fn: delay slots, branch-likely, calls as edges) with its
    jump tables attached and every `j` into the function's own code turned into an edge: the back-end emits a goto
    there (_Fn models every `j` as a tail call - the only tail exits to an address of the function itself)."""
    from types import SimpleNamespace

    from .analysis.width import _Fn
    fn = SimpleNamespace(start=start, addrs=set(addrs),
                         jump_tables=[(jr, None, list(t)) for jr, t in jump_tables.items()])
    f = _Fn(img, fn, set(func_starts))
    f.attach_jump_tables(fn.jump_tables)
    for a in f.addrs:
        own = [e for e in f.exits[a] if e[0] == "tail" and e[1] in f.insn]
        if own:
            f.exits[a] = [e for e in f.exits[a] if e not in own]
            for _k, t in own:
                f.succ[a].append((t, None))
                f.pred[t].append((a, None))
    return f


def own_jumps(img, addrs):
    """{address of a `j`: target} for the `j` instructions of a function that target its own code."""
    out = {}
    for a in addrs:
        w = img.word(a)
        t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if w >> 26 == 2 and t in addrs:
            out[a] = t
    return out


CS_SAVE_REGS = frozenset(range(16, 24)) | {30, 31}       # s0-s7, fp, ra: the registers Metrowerks prologues save
_FRAME_W = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "lwu": 4, "ld": 8, "lq": 16, "lwl": 4, "lwr": 4, "ldl": 8,
            "ldr": 8, "lwc1": 4, "lqc2": 16}
_CALLER_SAVED_MASK = sum(1 << r for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 31))


def _frame_refs(f):
    """Before every instruction: which registers hold the entry sp + a known constant ({reg: delta}; sp itself is
    register 29) and which hold some other value derived from sp (a set, "tainted"). (None, None): unreachable.
    Copies (daddu/addu/or x,y,zero), addiu/daddiu x,y,k of a tracked register keep the offset; anything else computed
    from a tracked or tainted register taints its destination; a call clobbers the caller-saved ones."""
    import heapq
    din = {a: None for a in f.addrs}
    din[f.start] = ((29, 0),), frozenset()
    work, pending = [f.start], {f.start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        exact, taint = din[a]
        ex = dict(exact)
        tn = set(taint)
        op, insn = f.op[a], f.insn[a]
        if op is not None and op.dst is not None:
            d, m, o = op.dst, insn.mnem, insn.ops
            srcs = [r for r, _b in op.srcs]
            val, taint = None, False
            if m in ("addiu", "daddiu") and o[1][1] in ex:
                val = ex[o[1][1]] + o[2][1]
            elif m in ("addu", "daddu", "or") and len(o) == 3 and 0 in (o[1][1], o[2][1]) and \
                    (o[1][1] or o[2][1]) in ex:
                val = ex[o[1][1] or o[2][1]]
            elif any(r in ex or r in tn for r in srcs):
                taint = True
            ex.pop(d, None)
            tn.discard(d)
            if val is not None:
                ex[d] = val
            elif taint:
                tn.add(d)
        lr = link_register(insn)
        if lr:
            ex.pop(lr, None)
            tn.discard(lr)
        for t, e in f.succ[a]:
            if e is not None:                                    # call: caller-saved registers are clobbered
                ce = {r: v for r, v in ex.items() if not _CALLER_SAVED_MASK >> r & 1}
                ct = {r for r in tn if not _CALLER_SAVED_MASK >> r & 1}
            else:
                ce, ct = ex, tn
            cur = din[t]
            if cur is None:
                new = (tuple(sorted(ce.items())), frozenset(ct))
            else:
                cex = dict(cur[0])
                keep = {r: v for r, v in ce.items() if cex.get(r) == v}
                lost = {r for r in set(ce) | set(cex) if r not in keep}
                new = (tuple(sorted(keep.items())), frozenset(set(cur[1]) | ct | lost))
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din


def cs_pairs(f, jump_tables):
    """Lever "nocs" (ABI mode): guest addresses of the callee-saved save/restore pairs that can be dropped.

    A pair is, per frame slot (address relative to the entry sp), the SQ/SD of one register of s0-s7/fp/ra through
    sp while the register still holds its entry value on every path (prologue saves) and the LQ/LD of the same
    register from that slot through sp (restores). It is dropped (no code for either) when
      - no other load of the function may read a byte of the slot: loads through sp or a register known to hold
        entry sp + k are checked by address; a load through any other sp-derived value cancels every pair,
      - every restored value is dead: on no path from a restore is the register read before it is written again or
        the function leaves (ABI mode writes s0-s7/fp/ra back neither at returns nor at (tail) calls, and `jr ra`
        is a C return),
      - no jump-table case leaves the function (such a case flushes everything).
    The caller's values of these registers live in the caller's C locals in ABI mode; only the frame slot contents
    differ from the guest (not ABI-observable). Functions B's detector requires exact never get here (abi=False).
    Not covered: a callee reading this frame through a pointer it was given (beyond the object it points to)."""
    import heapq
    start = f.start
    if start not in f.insn or f.unknown:
        return frozenset()
    for targets in jump_tables.values():
        if any(t not in f.insn for t in targets):
            return frozenset()
    refs = _frame_refs(f)
    # registers possibly written since entry, before every instruction (calls keep s0-s7/fp by the ABI; the call
    # instruction itself writes ra)
    defs = {}
    for a in f.addrs:
        op, insn = f.op[a], f.insn[a]
        g = 1 << op.dst if op is not None and op.dst is not None else 0
        lr = link_register(insn)
        if lr:
            g |= 1 << lr
        defs[a] = g
    wb = {a: None for a in f.addrs}
    wb[start] = 0
    work, pending = [start], {start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = wb[a] | defs[a]
        for t, _e in f.succ[a]:
            new = out if wb[t] is None else wb[t] | out
            if new != wb[t]:
                wb[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    saves, restores, loads = {}, {}, []
    for a in f.addrs:
        if refs[a] is None:                                     # unreachable
            continue
        insn = f.insn[a]
        m = insn.mnem
        if m not in _FRAME_W and m not in ("sq", "sd"):
            continue
        if not insn.ops or insn.ops[-1][0] != "mem":
            continue
        base, off = insn.ops[-1][1], insn.ops[-1][2]
        exact, taint = dict(refs[a][0]), refs[a][1]
        if m in _FRAME_W:
            if base in exact:
                loads.append((a, exact[base] + off, _FRAME_W[m]))
            elif base in taint:
                return frozenset()                              # a load at an unknown place of the frame
        if base != 29 or 29 not in exact or insn.ops[0][0] != "r" or insn.ops[0][1] not in CS_SAVE_REGS:
            continue
        slot, r = exact[29] + off, insn.ops[0][1]
        if m in ("lq", "ld"):
            restores.setdefault(slot, []).append((a, r))
        elif m in ("sq", "sd"):
            saves.setdefault(slot, []).append((a, r, 16 if m == "sq" else 8, not wb[a] >> r & 1))
    # liveness of the registers (bit masks), backward: an instruction's reads (a return does not read ra), its writes
    # kill; calls keep s0-s7/fp/ra liveness flowing (the callee preserves them)
    uses = {}
    for a in f.addrs:
        op = f.op[a]
        u = 0
        if op is not None:
            for r, _b in op.srcs:
                u |= 1 << r
            if op.base:
                u |= 1 << op.base
        if f.flow[a].kind == "return":
            u &= ~(1 << 31)
        uses[a] = u
    live = {a: 0 for a in f.addrs}
    order = sorted(f.addrs, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            out = 0
            for t, _e in f.succ[a]:
                out |= live[t]
            v = (out & ~defs[a]) | uses[a]
            if v != live[a]:
                live[a] = v
                changed = True
    drop = set()
    for slot, ss in saves.items():
        rs = restores.get(slot)
        if not rs:
            continue
        regs = {r for _a, r, _w, _ok in ss} | {r for _a, r in rs}
        if len(regs) != 1 or not all(ok for _a, _r, _w, ok in ss):
            continue
        r = next(iter(regs))
        width = max(w for _a, _r, w, _ok in ss)
        mine = {a for a, _r in rs}
        if any(a not in mine and lo < slot + width and slot < lo + w for a, lo, w in loads):
            continue
        if any(any(live[t] >> r & 1 for t, _e in f.succ[a]) for a in mine):
            continue
        drop.update(a for a, _r, _w, _ok in ss)
        drop.update(mine)
    return frozenset(drop)


_MLOC_MMI = frozenset({"pcpyld", "pextlw", "prot3w"})        # lever mloc: the word shuffles of the VU0 helpers
# lever mlocx (experimental, in no variant): also these. Host 2026-10-15: guest-identical except 9 fewer loop
# back-edges in the boot (an MMI string scan + the render driver together; outputs equal) - cause Unknown
_MLOCX_MMI = frozenset({"pcpyud", "pextuw", "pand", "por", "pxor", "pnor"})


def _mloc_op(op, ext=False):
    """Lever "mloc": an MMI word shuffle (lever mlocx: also the bitwise ops / upper-half shuffles) or a QMTC2 / QMFC2
    that runs on the C locals."""
    m = op.mnem.split(".")[0]
    return (op.cls == "mmi" and (m in _MLOC_MMI or (ext and m in _MLOCX_MMI))) or         (op.cls == "cop2" and m in ("qmtc2", "qmfc2"))


def _q128_ops(f, drop=frozenset(), mloc=False, mlocx=False):
    """Lever "q128": {address: Op} of the 128-bit instructions that run on C locals (LQ, SQ and the 128-bit move
    idiom paddub/por d,s,zero; lever mloc: also the MMI word shuffles / bitwise ops and QMTC2 / QMFC2 of _mloc_op) and
    the registers whose upper halves then live in h<n> locals."""
    conv, regs = {}, set()
    for a in f.addrs:
        op = f.op[a]
        if a in drop or op is None:
            continue
        if op.cls not in ("lq", "sq", "move128") and not (mloc and _mloc_op(op, mlocx)):
            continue
        conv[a] = op
        if op.dst is not None:
            regs.add(op.dst)
        regs.update(r for r, b in op.srcs if b == 128)
    regs.discard(0)
    return conv, frozenset(regs)


def _q128_unnarrow(f, conv, narrow):
    """Narrowed registers that would lose bits 32..63 once their 128-bit values live in locals: the narrowing plan
    (analysis.width.narrowable) lets 128-bit instructions read and write the exact context value, a narrowed local
    holds only canonical (sign-extended) low halves. A narrowed register stays narrowed only if it is canonical at
    every converted instruction that reads it and after every one that writes it (forward canonicity of the width
    analysis; arguments and call results conservatively unknown)."""
    from .analysis.width import ALL, ARGS, RA, RETS, _fwd, forward

    def call(st, _target):
        c32, blob, hi, _hilo, slots = st
        return ((c32 & ~RETS) | 1 << RA, blob & ~RETS, hi & ~RETS, 0, slots)

    st_in = forward(f, (ALL & ~ARGS, 0, 0, 0xF, ()), call)
    out = set()
    for a, op in conv.items():
        st = st_in.get(a)
        if st is None:
            continue
        for r, b in op.srcs:
            if b == 128 and r in narrow and not st[0] >> r & 1:
                out.add(r)
        if op.dst is not None and op.dst in narrow and not _fwd(f, a, st)[0] >> op.dst & 1:
            out.add(op.dst)
    return out


def _q128_stmt(i, op):
    """Local form of a converted 128-bit instruction: low half in r<n>, upper half in h<n>."""
    m, o = i.mnem, i.ops
    if op.cls == "lq":
        n = o[0][1]
        if n == 0:
            return ";"
        return "{ R5900Reg128 q_; rm_lq(m_, %s, &q_); %s h%d = q_.d[1]; }" % (_mem_addr(o[1]), W(n, "q_.d[0]"), n)
    if op.cls == "sq":
        n = o[0][1]
        if n == 0:
            return "{ R5900Reg128 q_; q_.d[0] = 0; q_.d[1] = 0; rm_sq(m_, %s, &q_); }" % _mem_addr(o[1])
        return "{ R5900Reg128 q_; q_.d[0] = %s; q_.d[1] = h%d; rm_sq(m_, %s, &q_); }" % (R(n), n, _mem_addr(o[1]))
    if op.cls in ("mmi", "cop2"):                        # lever mloc
        return _mloc_stmt(i, op)
    d, s = o[0][1], o[1][1] or o[2][1]                   # move128: paddub/por d, s, zero
    if d == 0:
        return ";"
    if s == 0:
        return "%s h%d = 0;" % (W(d, "0ull"), d)
    return "%s h%d = h%d;" % (W(d, R(s)), d, s)


def _mloc_stmt(i, op):
    """Lever mloc: the local form of an MMI word shuffle / bitwise op (low half r<n>, upper half h<n>) and of the
    VU0 transfers QMTC2 / QMFC2 (the VF side: lanes v<n>_<k> with lever vfloc, else c->vu0). Sources are read into
    temporaries first (the destination may be a source)."""
    m, o = i.mnem.split(".")[0], i.ops

    def lo(n):
        return R(n)

    def hi(n):
        return "h%d" % n if n else "0ull"

    vloc = _LOCAL.get("vfloc")
    if m == "qmtc2":                                     # qmtc2 rt, vf: the four words of rt to VF[vf]
        rt, vf = o[0][1], o[1][1]
        if vf == 0:
            return ";"
        dst = ["v%d_%d" % (vf, k) if vloc else "c->vu0.vf[%d].w[%d]" % (vf, k) for k in range(4)]
        s = "{ uint64_t l_ = %s, u_ = %s; %s = (uint32_t)l_; %s = (uint32_t)(l_ >> 32); %s = (uint32_t)u_; " \
            "%s = (uint32_t)(u_ >> 32); }" % (lo(rt), hi(rt), dst[0], dst[1], dst[2], dst[3])
        if vloc and _LOCAL.get("vwt"):                   # lever vwt: written through
            s += " " + " ".join("c->vu0.vf[%d].w[%d] = v%d_%d;" % (vf, k, vf, k) for k in range(4))
        return s
    if m == "qmfc2":                                     # qmfc2 rt, vf: VF[vf] to rt
        rt, vf = o[0][1], o[1][1]
        if rt == 0:
            return ";"
        src = [vu0._lv(vf, k) if vloc else "c->vu0.vf[%d].w[%d]" % (vf, k) for k in range(4)]
        return "{ uint64_t l_ = (uint64_t)%s | (uint64_t)%s << 32, u_ = (uint64_t)%s | (uint64_t)%s << 32; %s " \
               "h%d = u_; }" % (src[0], src[1], src[2], src[3], W(rt, "l_"), rt)
    d = o[0][1]
    if d == 0:
        return ";"
    if m == "prot3w":                                    # (w0, w1, w2, w3) -> (w1, w2, w0, w3)
        t = o[1][1]
        return "{ uint64_t l_ = %s, u_ = %s; %s h%d = (u_ & 0xFFFFFFFF00000000ull) | (uint32_t)l_; }" % (
            lo(t), hi(t), W(d, "(l_ >> 32) | (u_ << 32)"), d)
    s, t = o[1][1], o[2][1]
    head = "uint64_t sl_ = %s, su_ = %s, tl_ = %s, tu_ = %s;" % (lo(s), hi(s), lo(t), hi(t))
    if m == "pcpyld":                                    # lo = t.lo, hi = s.lo
        nl, nu = "tl_", "sl_"
    elif m == "pcpyud":                                  # lo = s.hi, hi = t.hi
        nl, nu = "su_", "tu_"
    elif m == "pextlw":                                  # (t.w0, s.w0, t.w1, s.w1)
        nl, nu = "(uint32_t)tl_ | (uint64_t)(uint32_t)sl_ << 32", "(tl_ >> 32) | (sl_ >> 32) << 32"
    elif m == "pextuw":                                  # (t.w2, s.w2, t.w3, s.w3)
        nl, nu = "(uint32_t)tu_ | (uint64_t)(uint32_t)su_ << 32", "(tu_ >> 32) | (su_ >> 32) << 32"
    else:                                                # pand / por / pxor / pnor
        opx = {"pand": "&", "por": "|", "pxor": "^", "pnor": "|"}[m]
        neg = "~" if m == "pnor" else ""
        nl, nu = "%s(sl_ %s tl_)" % (neg, opx), "%s(su_ %s tu_)" % (neg, opx)
    return "{ %s uint64_t nl_ = %s, nu_ = %s; %s h%d = nu_; }" % (head, nl, nu, W(d, "nl_"), d)


_FPU_CLAMPED_OPS = {"add.s": (1, 2), "sub.s": (1, 2), "mul.s": (1, 2), "div.s": (1, 2), "adda.s": (0, 1),
                    "suba.s": (0, 1), "mula.s": (0, 1), "madd.s": (1, 2), "msub.s": (1, 2), "madda.s": (0, 1),
                    "msuba.s": (0, 1), "c.eq.s": (0, 1), "c.lt.s": (0, 1), "c.le.s": (0, 1), "sqrt.s": (1,),
                    "rsqrt.s": (1, 2)}
_FATAL_GO_ON = {"mfc0", "mtc0", "di", "ei"}          # rt_fatal points after which a harness may continue (COP0 HLE)


def _fpr_reads(insn):
    """(clamped, raw) FPR read masks of one instruction: operands the R5900 model reads through the input clamp, and
    those whose raw bits it uses (stores, moves, conversions, MAX/MIN ...)."""
    ops = insn.ops
    dest = None
    if _fpr_def(insn):
        dest = 1 if insn.mnem == "mtc1" else 0
    clamped, raw = 0, 0
    cl = _FPU_CLAMPED_OPS.get(insn.mnem, ())
    for k, o in enumerate(ops):
        if o[0] != "f" or k == dest:
            continue
        if k in cl:
            clamped |= 1 << o[1]
        else:
            raw |= 1 << o[1]
    return clamped, raw


def _clamp1_defs(f):
    """Lever "clamp1" (ABI mode): LWC1/MTC1 definitions whose value is only ever used through the input clamp - every
    use reachable before the FPR is written again is an arithmetic/compare operand, no raw use (SWC1, MFC1, MOV.S,
    ABS.S, NEG.S, CVT.W.S, MAX.S/MIN.S) and no write-back of the register to the context (call arguments f12-f19,
    returned f0/f1, the full flushes before exempt functions and COP0 rt_fatal points). Such a value is clamped once
    where it is defined and counts as clean afterwards."""
    fd = {a: _fpr_def(f.insn[a]) for a in f.addrs}
    rd = {a: _fpr_reads(f.insn[a]) for a in f.addrs}
    flush = {}
    for a in f.addrs:
        x = 0
        insn = f.insn[a]
        if insn.mnem in _FATAL_GO_ON:
            x = -1
        for _t, e in f.succ[a]:
            if e is not None:
                x |= -1 if e[1] is not None and _exempt(e[1]) else 0xFF000
        for e in f.exits[a]:
            if e[0] == "ret":
                x |= 0x3
            elif e[0] in ("tail", "call"):
                x |= -1 if e[1] is not None and _exempt(e[1]) else 0xFF000
        if f.flow[a].kind == "jump_indirect":            # jump table cases outside the function: full flush
            x = -1
        flush[a] = x & 0xFFFFFFFF
    raw_in = {a: 0 for a in f.addrs}
    any_in = {a: 0 for a in f.addrs}
    order = sorted(f.addrs, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            ro = ao = 0
            for t, e in f.succ[a]:
                k = ~0x3 if e is not None else -1          # f0/f1 are reloaded after an (ABI) call
                ro |= raw_in[t] & k
                ao |= any_in[t] & k
            ro |= flush[a]
            ao |= flush[a]
            c, r = rd[a]
            rv = (ro & ~fd[a]) | r
            av = (ao & ~fd[a]) | r | c
            if (rv, av) != (raw_in[a], any_in[a]):
                raw_in[a], any_in[a] = rv, av
                changed = True
    out = set()
    for a in f.addrs:
        if f.insn[a].mnem not in ("lwc1", "mtc1"):
            continue
        bit = fd[a]
        ro = ao = 0
        for t, e in f.succ[a]:
            k = ~0x3 if e is not None else -1
            ro |= raw_in[t] & k
            ao |= any_in[t] & k
        ro |= flush[a]
        ao |= flush[a]
        if ao & bit and not ro & bit:
            out.add(a)
    return frozenset(out)


def _levers(v5, cache, fpu_fast, abi):
    """The v5 levers a function gets: none without the v3+ basis; exact functions only those in V5_EXACT."""
    levers = frozenset(v5 or ()) if cache and fpu_fast else frozenset()
    return levers if abi else levers & V5_EXACT


def _plan(img, start, addrs, jump_tables, func_starts, narrow, levers, cache):
    """Per-function analyses that decide the representation: (CFG, final narrowed GPRs, dropped callee-saved pairs
    (nocs), q128 instructions {addr: Op}, q128 registers). Shared by translate_function and the ctx32 pre-pass of
    _emit_parts, which must see exactly the narrowing the translation uses."""
    narrow = frozenset(narrow) if cache else frozenset()
    if narrow and any(_j_goto(t, addrs) for t in own_jumps(img, addrs).values()):
        narrow = frozenset()     # the width analysis models `j` as a tail call: no narrowing across its goto loops
    f = fn_cfg(img, start, addrs, jump_tables, func_starts) if cache else None
    drop = cs_pairs(f, jump_tables) if "nocs" in levers else frozenset()
    conv, qloc = _q128_ops(f, drop, "mloc" in levers, "mlocx" in levers) if "q128" in levers else ({}, frozenset())
    if conv and narrow:
        narrow = narrow - _q128_unnarrow(f, conv, narrow)
    return f, narrow, drop, conv, qloc


def _lowonly_args(f, jump_tables, narrow, livein, per_target=None):
    """Lever "ctx32", argument part (ABI mode): bit mask of the argument registers (a0-a3, t0-t3) whose incoming
    context slot only this function reads, and only its low word. The register is narrowed here (its local is loaded as
    (int32_t)) and, while it still holds the incoming value and no call has happened since entry, no point lets anyone
    else read the slot: no (tail, indirect) call whose callee reads it, no transfer onto an exempt function, no
    syscall, no 128-bit instruction reading it, no jump-table case leaving the function. A direct caller may then write
    that argument back as one word. After a call, an argument register this function has not written is a
    caller-saved register the callee may have changed: reading it there is reading garbage on the EE too, so the stale
    upper word of the slot is not observable.
    per_target (lever argx): a dict - exposures to a direct, non-exempt callee are recorded there
    ({callee: mask}) instead of refusing the register; the caller (_emit_parts) keeps such a register one-word only if
    the callee reads that slot as one word too (a fixpoint over the program)."""
    import heapq
    if f.start not in f.insn or any(t not in f.insn for ts in jump_tables.values() for t in ts):
        return 0

    def direct(t):
        return per_target is not None and t is not None and not _exempt(t)

    def readers(t):
        if t is None or _exempt(t):
            return _ARG_MASK
        if direct(t):                                    # lever argx: recorded per callee below
            return 0
        return (livein.get(t, _ARG_MASK) if livein else _ARG_MASK) & _ARG_MASK

    exposed = 0
    din = {a: None for a in f.addrs}
    din[f.start] = _ARG_MASK
    work, pending = [f.start], {f.start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        st = din[a]
        op, insn = f.op[a], f.insn[a]
        if insn.mnem == "syscall":                       # the HLE reads the arguments
            exposed |= st
        if op is not None and (op.dst_bits == 128 or any(b == 128 for _, b in op.srcs)):
            for r, _b in op.srcs:
                exposed |= st & 1 << r
        d = 1 << op.dst if op is not None and op.dst is not None else 0
        lr = link_register(insn)
        if lr:
            d |= 1 << lr
        out = st & ~d
        for _t, e in f.succ[a]:                          # calls: flushed after this instruction (its delay slot)
            if e is not None:
                exposed |= out & readers(e[1])
        for e in f.exits[a]:
            if e[0] in ("tail", "call"):
                exposed |= out & readers(e[1])
        if per_target is not None:                       # lever argx: exposures to direct callees, per callee
            for t in [e[1] for _t, e in f.succ[a] if e is not None] +                      [e[1] for e in f.exits[a] if e[0] in ("tail", "call")]:
                if direct(t):
                    m = out & (livein.get(t, _ARG_MASK) if livein else _ARG_MASK) & _ARG_MASK
                    if m:
                        per_target[t] = per_target.get(t, 0) | m
        for t, e in f.succ[a]:
            v = 0 if e is not None else out
            new = v if din[t] is None else din[t] | v
            if new != din[t]:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return sum(1 << n for n in narrow) & _ARG_MASK & ~exposed


def _fpr_clean(img, start, addrs, jump_tables, func_starts, keep_calls=False, keep_acc=False, clean_defs=frozenset(),
               f=None):
    """Clean FPRs (and ACC) before every instruction: written by a modelled FPU operation on every path since entry or
    the last call. LWC1/MTC1/CVT.W.S write unknown bit patterns; calls make everything unknown.
    v5: keep_calls (lever fclean, ABI mode) - a call leaves the FPR locals untouched except the reloaded f0/f1, so
    their cleanliness survives it (keep_acc: also the ACC local, lever acc); clean_defs (lever clamp1) - LWC1/MTC1
    whose value is clamped where it is loaded."""
    import heapq

    f = f or fn_cfg(img, start, addrs, jump_tables, func_starts)

    def transfer(a, insn, st):
        mask, acc = st
        m, o = insn.mnem, insn.ops
        if m in _FPU_CLEAN_RESULT:
            mask |= 1 << o[0][1]
        elif m in ("max.s", "min.s"):
            ok = mask >> o[1][1] & 1 and mask >> o[2][1] & 1
            mask = mask | 1 << o[0][1] if ok else mask & ~(1 << o[0][1])
        elif m in ("abs.s", "neg.s", "mov.s"):
            mask = mask | 1 << o[0][1] if mask >> o[1][1] & 1 else mask & ~(1 << o[0][1])
        elif m in ("lwc1", "cvt.w.s"):
            mask = mask | 1 << o[0][1] if a in clean_defs else mask & ~(1 << o[0][1])
        elif m == "mtc1":
            mask = mask | 1 << o[1][1] if a in clean_defs else mask & ~(1 << o[1][1])
        elif m in _FPU_ACC_WRITE:
            acc = True
        return (mask, acc)

    din = {a: None for a in f.addrs}
    if start not in din:
        return din
    din[start] = (0, False)
    work, pending = [start], {start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = transfer(a, f.insn[a], din[a])
        for t, e in f.succ[a]:
            if e is None:
                v = out
            elif keep_calls and not (e[1] is not None and _exempt(e[1])):   # an exempt callee: RELOADALL
                v = (out[0] & ~0x3, out[1] and keep_acc)
            else:
                v = (0, False)
            cur = din[t]
            new = v if cur is None else (cur[0] & v[0], cur[1] and v[1])
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din
    din[start] = (0, False)
    work, pending = [start], {start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = transfer(f.insn[a], din[a])
        for t, e in f.succ[a]:
            v = (0, False) if e is not None else out
            cur = din[t]
            new = v if cur is None else (cur[0] & v[0], cur[1] and v[1])
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din


_BRANCH_COND = {
    "beq": "{a} == {b}", "bne": "{a} != {b}", "beql": "{a} == {b}", "bnel": "{a} != {b}",
    "blez": "(int64_t){a} <= 0", "bgtz": "(int64_t){a} > 0", "blezl": "(int64_t){a} <= 0", "bgtzl": "(int64_t){a} > 0",
    "bltz": "(int64_t){a} < 0", "bgez": "(int64_t){a} >= 0", "bltzl": "(int64_t){a} < 0", "bgezl": "(int64_t){a} >= 0",
    "bltzal": "(int64_t){a} < 0", "bgezal": "(int64_t){a} >= 0", "bltzall": "(int64_t){a} < 0",
    "bgezall": "(int64_t){a} >= 0",
    "bc1t": "(c->fcr31 & FCR31_C) != 0", "bc1f": "(c->fcr31 & FCR31_C) == 0",
    "bc1tl": "(c->fcr31 & FCR31_C) != 0", "bc1fl": "(c->fcr31 & FCR31_C) == 0",
    # COP0 condition (Sony DMA-completion polling): runtime hook, see rt_cpcond0 in r5900_rt.h
    "bc0t": "rt_cpcond0(c)", "bc0f": "!rt_cpcond0(c)", "bc0tl": "rt_cpcond0(c)", "bc0fl": "!rt_cpcond0(c)",
}
_TRAP_COND = {"teq": "{a} == {b}", "tne": "{a} != {b}", "tge": "(int64_t){a} >= (int64_t){b}",
              "tgeu": "{a} >= {b}", "tlt": "(int64_t){a} < (int64_t){b}", "tltu": "{a} < {b}"}
# kinds after which execution can continue at the next instruction (after the delay slot for branches/calls)
_FALLS_THROUGH = ("none", "trap", "branch", "call", "call_conditional", "call_indirect")


def fname(addr):
    return "%sfn_%08x" % (_LOCAL.get("prefix", ""), addr)


def _dispatch():
    return _LOCAL.get("dispatch", "rc_dispatch")


def _callee(target):
    """C function for a direct call target: a function of this translation unit, an external one (e.g. main code
    called from an overlay), or None - then the call goes through the dispatcher (overlay window, unknown code)."""
    if target in _LOCAL.get("starts", ()):
        return fname(target)
    return (_LOCAL.get("external") or {}).get(target)


def _call(target):
    f = _callee(target)
    return "%s(c)" % f if f else "%s(c, 0x%08xu)" % (_dispatch(), target)


def _exempt(target):
    """Target is a function that must run exact (the non-ABI detector: abi_exempt): every transfer to it is a
    full boundary."""
    return target in _LOCAL.get("exempt", ())


def _xk(kind, target):
    """Flush kind of a transfer to `target`: "x" + kind for an exempt target (full flush, see translate_function)."""
    return "x" + kind if _exempt(target) else kind


def _reload_after(target):
    """Marker after a direct call: an exempt callee may change any register (RELOADALL)."""
    return "/*RELOADALL*/" if _exempt(target) else "/*RELOAD*/"


def _j_goto(target, insns):
    """A `j` into the function's own code (crt0's bss-clear loops) becomes a goto - not a tail call: as a call every
    loop iteration would nest a C call (and a target that is no function start would end in the dispatcher)."""
    return target in insns


def _pcall(expr, target=None):
    """Call statement. In an exact function of an ABI-mode program (`protect`), a callee that is not exempt runs in ABI
    mode: it keeps callee-saved registers only in its C locals and may leave other values in their context copies, so
    the caller keeps the context copies of s0-s7/fp (128 bits) and f20-f31 across the call (rt_cs_save/restore)."""
    if _LOCAL.get("protect") and (target is None or not _exempt(target)):
        return "{ R5900CSave cs_; rt_cs_save(c, &cs_); %s; rt_cs_restore(c, &cs_); }" % expr
    return expr + ";"


def _cond(i):
    ops = i.ops
    if i.mnem not in _BRANCH_COND:                   # bc2* (VU0 condition)
        raise Unsupported("%08x: %s" % (i.pc, i.text))
    if i.mnem.startswith("bc1") and _v5("fcc"):      # v5: the condition bit lives in the local fcc_
        return "fcc_" if i.mnem in ("bc1t", "bc1tl") else "!fcc_"
    return _BRANCH_COND[i.mnem].format(a=R(ops[0][1]) if ops[0][0] == "r" else "",
                                       b=R(ops[1][1]) if len(ops) > 2 and ops[1][0] == "r" else "")


def _trap(i):
    m, o = i.mnem, i.ops
    if m in _TRAP_COND:
        cond = _TRAP_COND[m].format(a=R(o[0][1]), b=R(o[1][1]))
    else:                                          # immediate forms: teqi tnei tgei tgeiu tlti tltiu
        cond = _TRAP_COND[m[:-1]].format(a=R(o[0][1]), b="(uint64_t)(int64_t)%d" % o[1][1])
    return "if (%s) { /*FLUSH@i%08x|fatal*/ rt_fatal(c, \"trap\", 0x%08xu, 0x%08xu); }" % (cond, i.pc, i.pc, i.word)


def _exits(img, fn, func_starts):
    """Function starts this function reaches by leaving its own code: branches, jumps and fall-through."""
    out, slots = set(), set()
    for a in sorted(fn.addrs):
        if a in slots:                                  # delay slots: their control transfer is the branch's
            continue
        fl = flow(decode(img.word(a), a))
        if fl.has_delay_slot:
            slots.add(a + 4)
        if fl.kind in ("branch", "jump") and fl.target not in fn.addrs and fl.target in func_starts:
            out.add(fl.target)
        if fl.kind in _FALLS_THROUGH:
            nxt = a + (8 if fl.has_delay_slot else 4)
            if nxt not in fn.addrs and nxt in func_starts:
                out.add(nxt)
    return out


_MEM_RE = re.compile(r"\brt_(lb|lh|lw|ld|sb|sh|sw|sd|lwl|lwr|ldl|ldr|swl|swr|sdl|sdr|lq|sq|uld|ulw|usd|usw)\(c, ")  # lq/sq: LQC2/SQC2 (LQ/SQ are 128-bit ops); u*: lever ufuse


def _stmt(i, cache):
    """stmt() for one instruction; with the v1 cache, 128-bit instructions run on the context with their registers
    synced from/to the locals around them."""
    fstates = _LOCAL.get("fstates")
    _LOCAL["fpc"] = fstates.get(i.pc) if fstates else None
    vstates = _LOCAL.get("vstates")
    _LOCAL["vpc"] = vstates.get(i.pc) if vstates else None
    if not cache:
        return stmt(i)
    if i.pc in _LOCAL.get("drop", ()):                  # v5 nocs: callee-saved save/restore without code
        return ";"
    try:
        op = lift(i)
    except LiftError:
        op = None
    if op is None or not (op.dst_bits == 128 or any(b == 128 for _, b in op.srcs)):
        return _MEM_RE.sub(r"rm_\1(m_, ", stmt(i))
    conv = _LOCAL.get("conv")
    if conv and i.pc in conv:                            # v5 q128: LQ/SQ/128-bit move on the C locals
        return _q128_stmt(i, conv[i.pc])
    _LOCAL["on"] = False
    try:
        body = stmt(i)
    finally:
        _LOCAL["on"] = True
    if _v5("ctxal"):                                     # v5: through the local window, aligned context side
        body = _MEM_RE.sub(r"rm_\1(m_, ", body)
    regs = sorted({r for r, _ in op.srcs} | ({op.base} if op.base else set()) | ({op.dst} if op.dst else set()))
    pre = "/*SYNC@%08x:%s*/" % (i.pc, ",".join(str(r) for r in regs)) if regs else ""
    post = "/*LOAD:%d*/" % op.dst if op.dst else ""
    qloc = _LOCAL.get("qloc") or frozenset()
    if qloc:                                             # v5 q128: upper halves of h<n> registers to/from the context
        hs = sorted({r for r, b in op.srcs if b == 128} & qloc)
        pre += "".join(" c->r[%d].d[1] = h%d;" % (r, r) for r in hs)
        if op.dst in qloc:
            post += " h%d = c->r[%d].d[1];" % (op.dst, op.dst)
    if _v5("ctx32"):                                     # sp/gp read as data: their slots' upper words first
        for r in sorted({r for r, _b in op.srcs} & set(_SPGP)):
            pre += " c->r[%d].w[1] = (uint32_t)((int32_t)c->r[%d].w[0] >> 31);" % (r, r)
    return "{ %s %s %s }" % (pre, body, post)


_FPR_NODEF = {"swc1", "mfc1", "cfc1", "ctc1", "c.eq.s", "c.lt.s", "c.le.s", "c.f.s", "adda.s", "suba.s", "mula.s",
              "madda.s", "msuba.s", "bc1t", "bc1f", "bc1tl", "bc1fl", "lqc2", "sqc2"}
CALLER_SAVED_GPRS = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 31)


def _fpr_def(insn):
    m = insn.mnem
    if m == "mtc1":
        return 1 << insn.ops[1][1]
    if m in _FPR_NODEF or not insn.ops or insn.ops[0][0] != "f":
        return 0
    return 1 << insn.ops[0][1]


_FN_STATE = ("exempt", "protect", "v5", "drop", "conv", "qloc", "clamp1", "vfloc", "vwt")   # per-function _LOCAL keys


def _vwt_target(insn):
    """Lever vwt: the VF register an LQC2 / QMTC2 defines (all four lanes), else 0."""
    base = vu0.base_mnem(insn.mnem)
    if base == "lqc2":
        return insn.ops[0][1]
    if base == "qmtc2":
        return insn.ops[1][1]
    return 0


def _vf_dirty(f, vwt=False):
    """Lever vfloc: VF registers (bit n; ACC = bit 32) whose locals may differ from c->vu0, before and after every
    instruction. Calls and VCALLMS leave everything synced (flushed before, reloaded after). vwt: an LQC2 / QMTC2
    writes its register through (clean afterwards)."""
    import heapq

    from .vu.macro import vf_writes
    wr, clr = {}, {}
    for a in f.addrs:
        w = vf_writes(f.insn[a])
        wr[a] = -1 if w == -1 else sum(1 << n for n in w)
        t = _vwt_target(f.insn[a]) if vwt else 0
        clr[a] = 1 << t if t else 0
        if t:
            wr[a] = 0
    din = {a: None for a in f.addrs}
    dout = {}
    if f.start not in din:
        return din, dout
    din[f.start] = 0
    work, pending = [f.start], {f.start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = 0 if wr[a] == -1 else (din[a] | wr[a]) & ~clr[a]
        dout[a] = out
        for t, e in f.succ[a]:
            v = 0 if e is not None else out
            new = v if din[t] is None else din[t] | v
            if new != din[t]:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din, dout
ABI_CS_GPRS = 0xFF << 16 | 1 << 30                   # s0-s7, fp: a callee may leave other values in the context


def _dirty(img, start, addrs, jump_tables, func_starts, abi=False, drop=frozenset(), conv=None, f=None):
    """Registers written since the last context sync, per program point: ({addr: (gpr, fpr, upper)} before the
    instruction, {addr: (gpr, fpr, upper)} after it). Calls sync everything (flush before, reload after); 128-bit
    instructions sync the registers they touch. v5: `drop` (lever nocs) instructions have no code; `conv` (q128)
    instructions write C locals (no sync); upper = registers whose h<n> local may differ from the context's upper
    half (q128)."""
    import heapq

    f = f or fn_cfg(img, start, addrs, jump_tables, func_starts)
    conv = conv or {}
    gdef, gclean, fdef, hdef = {}, {}, {}, {}
    for a in f.addrs:
        op, insn = f.op[a], f.insn[a]
        if a in drop:
            gdef[a] = gclean[a] = fdef[a] = hdef[a] = 0
            continue
        g = 0
        if op is not None and op.dst is not None:
            g |= 1 << op.dst
        lr = link_register(insn)
        if lr:
            g |= 1 << lr
        gdef[a] = g
        clean = hd = 0
        if a in conv:
            hd = 1 << op.dst if op.dst is not None else 0
        elif op is not None and (op.dst_bits == 128 or any(b == 128 for _, b in op.srcs)):
            for r, _ in op.srcs:
                clean |= 1 << r
            if op.base:
                clean |= 1 << op.base
            if op.dst:
                clean |= 1 << op.dst
        gclean[a] = clean
        hdef[a] = hd
        fdef[a] = _fpr_def(insn)
    din = {a: None for a in f.addrs}
    if start not in din:
        return din, {}
    din[start] = (0, 0, 0)
    work, pending = [start], {start}
    dout = {}
    after_call = ((ABI_CS_GPRS if abi else 0), 0, (ABI_CS_GPRS if abi else 0))
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        g, fl, h = din[a]
        g = (g | gdef[a]) & ~gclean[a]
        fl |= fdef[a]
        h = (h | hdef[a]) & ~gclean[a]
        dout[a] = (g, fl, h)
        for t, e in f.succ[a]:
            # after a call everything is synced; in ABI mode the callee does not write back s0-s7/fp, so the
            # context copies of the caller's locals (low and, q128, upper halves) are stale until written again
            v = after_call if e is not None else (g, fl, h)
            cur = din[t]
            new = v if cur is None else (cur[0] | v[0], cur[1] | v[1], cur[2] | v[2])
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din, dout


def translate_function(img, start, addrs, jump_tables=None, func_starts=(), cache=False, narrow=frozenset(),
                       fpu_fast=False, abi=False, livein=None, exempt=None, program_abi=None, v5=None):
    """C source for the guest function `start` whose instructions are at `addrs` (set of addresses).

    jump_tables: {jr address: [case targets]} recovered by function discovery (the jr becomes a C switch);
    func_starts: addresses of all known functions - a branch or a fall-through that leaves this function and lands on
    one of them becomes a (conditional) tail call; cache: back-end v1 (registers in C locals); narrow: GPRs the
    width analysis proved to hold canonical values here (v2: int32_t locals, sign-extended when read); fpu_fast: v3
    FPU emission (operands clamped only when not known clean); abi: v4 - only what the ABI makes observable goes
    through the context (arguments a callee reads per `livein`, sp/gp and FPU arguments before calls; v0/v1/f0/f1 at
    returns and after calls; everything dirty before rt_fatal and syscalls); callee-saved and scratch registers stay
    in C locals across calls.

    exempt: addresses of functions that must run exact in an ABI-mode program (the non-ABI detector; emit_program
    passes its abi_exempt): a direct call, `j`, branch, fall-through or jump-table case onto one is a full boundary -
    flush kinds xcall=/xtail=/xcase= write back every written local in an ABI-mode function (every dirty one in an
    exact function) and /*RELOADALL*/ reloads every used local after the call. program_abi (default: abi): the
    program runs in ABI mode; an exact function (abi=False) in such a program keeps the context copies of its
    callee-saved registers across calls of non-exempt (ABI-mode) callees (rt_cs_save/rt_cs_restore)."""
    jump_tables = jump_tables or {}
    program_abi = abi if program_abi is None else program_abi
    saved = {k: _LOCAL[k] for k in _FN_STATE if k in _LOCAL}
    levers = _levers(v5, cache, fpu_fast, abi)
    if exempt is not None:
        _LOCAL["exempt"] = frozenset(exempt)
    _LOCAL["v5"] = levers
    _LOCAL["protect"] = bool(cache and program_abi and not abi)
    f, narrow, drop, conv, qloc = _plan(img, start, addrs, jump_tables, func_starts, narrow, levers, cache)
    outside = any(t not in addrs for ts in jump_tables.values() for t in ts)
    c1 = _clamp1_defs(f) if "clamp1" in levers and not outside else frozenset()
    _LOCAL.update(drop=drop, conv=conv, qloc=qloc, clamp1=c1)
    _LOCAL["on"], _LOCAL["narrow"] = bool(cache), narrow
    _LOCAL["fstates"] = _fpr_clean(img, start, addrs, jump_tables, func_starts, keep_calls="fclean" in levers,
                                   keep_acc="acc" in levers, clean_defs=c1, f=f) if fpu_fast else None
    # v3+: VU0 lanes known clean skip the input clamp (vu.macro.clean_states; None for functions without VU0 code)
    _LOCAL["vstates"] = vu0.clean_states(img, start, addrs, jump_tables, func_starts) if fpu_fast else None
    # lever vfloc: VF / ACC lanes in C locals (only functions with VU0 code; ABI mode - levers of exact
    # functions are V5_EXACT only)
    vfloc_on = bool(cache and "vfloc" in levers and _LOCAL["vstates"] is not None and f is not None and
                    fname(start) not in VFLOC_SKIP)
    _LOCAL["vfloc"] = vfloc_on
    vwt_on = vfloc_on and "vwt" in levers                    # lever vwt (needs vfloc)
    _LOCAL["vwt"] = vwt_on
    own_starts = "starts" not in _LOCAL                  # called directly (not from emit_program)
    if own_starts:
        _LOCAL["starts"] = set(func_starts)
    try:
        lines = _translate_body(img, start, addrs, jump_tables, func_starts, cache)
    finally:
        _LOCAL["on"], _LOCAL["narrow"], _LOCAL["fstates"], _LOCAL["fpc"] = False, frozenset(), None, None
        _LOCAL["vstates"], _LOCAL["vpc"] = None, None
        if own_starts:
            del _LOCAL["starts"]
        for k in _FN_STATE:
            _LOCAL.pop(k, None)
        _LOCAL.update(saved)
    body = "\n".join(lines)
    if "/*B*/" in body:                                  # v5 maskbase: base and offset apart (r5900_rt.h rmb_*)
        body = _RMB_RE.sub(r"rmb_\1(m_, ", body)
        if "/*B*/" in body:
            raise AssertionError("maskbase address outside an rm_* accessor in %s" % fname(start))
    head = ["void %s(R5900Ctx *c)" % fname(start), "{"]
    if not cache:
        body = re.sub(r"/\*FLUSH@[io][0-9a-f]{8}\|[^*]*\*/ ?", "", body).replace(" /*RELOAD*/", "")
        body = body.replace(" /*RELOADSYS*/", "").replace(" /*RELOADALL*/", "")
        return "\n".join(head) + "\n" + body + "\n}\n"
    ctx32 = "ctx32" in levers                              # v5: one-word context copies (sp/gp; some arguments)

    def store(n):                                          # local -> context (low 64 bits)
        if ctx32 and n in _SPGP:                           # readers take the low word (canonical addresses)
            return "c->r[%d].w[0] = (uint32_t)r%d;" % (n, n)
        return "c->r[%d].d[0] = (uint64_t)(int64_t)r%d;" % (n, n) if n in narrow else "c->r[%d].d[0] = r%d;" % (n, n)

    def load(n):                                           # context -> local
        if ctx32 and n in _SPGP:
            return "(int32_t)c->r[%d].w[0]" % n if n in narrow else "SX32(c->r[%d].w[0])" % n
        return "(int32_t)c->r[%d].d[0]" % n if n in narrow else "c->r[%d].d[0]" % n

    # results of 128-bit instructions come back from the context (they also count as uses/writes of the local)
    body = re.sub(r"/\*LOAD:(\d+)\*/", lambda m: "r%s = %s;" % (m.group(1), load(int(m.group(1)))), body)
    used_r = sorted({int(x) for x in re.findall(r"\br(\d+)\b", body)})
    used_f = sorted({int(x) for x in re.findall(r"\bf(\d+)\b", body)})
    used_h = sorted({int(x) for x in re.findall(r"\bh(\d+)\b", body)})           # v5 q128 upper halves
    wr_r = {int(x) for x in re.findall(r"\br(\d+) = ", body)}
    wr_f = {int(x) for x in re.findall(r"\bf(\d+) = ", body)}
    # v5: HI/LO, ACC, condition bit in locals (hilo, acc, fcc)
    hilo = [n for n in ("lo_0", "hi_0", "lo_1", "hi_1") if re.search(r"\b%s\b" % n, body)]
    acc = bool(re.search(r"\bacc_\b", body))
    fcc = bool(re.search(r"\bfcc_\b", body))
    xstore = ["c->%s.d[%s] = %s;" % (n[:2], n[3], n) for n in hilo] + (["c->acc.u = acc_;"] if acc else []) + \
        (["c->fcr31 = (c->fcr31 & ~FCR31_C) | (fcc_ ? FCR31_C : 0u);"] if fcc else [])
    xload = ["%s = c->%s.d[%s];" % (n, n[:2], n[3]) for n in hilo] + (["acc_ = c->acc.u;"] if acc else []) + \
        (["fcc_ = (c->fcr31 & FCR31_C) != 0;"] if fcc else [])
    din, dout = _dirty(img, start, addrs, jump_tables, func_starts, abi, drop, conv, f)
    # lever vfloc: lanes used / written, dirty VF registers per point
    used_v = sorted({(int(n), int(i)) for n, i in re.findall(r"\bv(\d+)_(\d)\b", body)} |
                    {(32, int(i)) for i in re.findall(r"\bva_(\d)\b", body)}) if vfloc_on else []
    wr_v = sorted({(int(n), int(i)) for n, i in re.findall(r"\bv(\d+)_(\d) = ", body)} |
                  {(32, int(i)) for i in re.findall(r"\bva_(\d) = ", body)}) if vfloc_on else []
    vdin, vdout = _vf_dirty(f, vwt_on) if vfloc_on else ({}, {})

    def vname(n, i):
        return "va_%d" % i if n == 32 else "v%d_%d" % (n, i)

    def vslot(n, i):
        return "c->vu0.acc.w[%d]" % i if n == 32 else "c->vu0.vf[%d].w[%d]" % (n, i)
    vload = " ".join("%s = %s;" % (vname(n, i), vslot(n, i)) for n, i in used_v)

    args_all = 0xFF0                                       # a0-a3, t0-t3
    fargs = 0xFF000                                        # f12-f19

    def flush(m):
        st = (din if m.group(1) == "i" else dout).get(int(m.group(2), 16))
        g, fl, h = st if st is not None else (-1, -1, -1)   # unknown point: everything written may be dirty
        kind = m.group(3)
        if kind == "vcm":                                  # VCALLMS / VCALLMSR (vu.macro): a full flush, except with
            if abi and "vcm" in levers:                    # lever vcm - the VU0 micro subroutine (the
                g = fl = h = 0                             # rt_vu0_callms hook) reads and writes only c->vu0: the
            else:                                          # dirty VU0 lanes only (the GPR / FPR locals stay dirty
                kind = "fatal"                             # and are written at the next boundary as before)
        low = 0                                            # ctx32: arguments the callee reads only as a low word
        if abi:
            if kind.startswith(("call=", "tail=")):
                tgt = int(kind[5:], 16)
                am = livein.get(tgt, args_all) if livein else args_all
                g &= am | 1 << 28 | 1 << 29
                fl &= fargs
                h &= am
                if ctx32:
                    low = (_LOCAL.get("lowonly") or {}).get(tgt, 0)
            elif kind in ("icall", "itail"):
                g &= args_all | 1 << 28 | 1 << 29
                fl &= fargs
                h &= args_all
            elif kind == "ret":
                g &= 0xC | 1 << 28 | 1 << 29               # v0, v1 + sp, gp (kept valid in the context)
                fl &= 0x3                                  # f0, f1
                h &= 0xC
                rd = _LOCAL.get("retdem")                  # lever rlive: only what a caller can observe
                if rd is not None:
                    d = rd.get(start, _RD.FULL)
                    if not d & (_RD.V0L | _RD.V0H):
                        g &= ~(1 << 2)
                    elif not d & _RD.V0H:
                        low |= 1 << 2                      # v0 as one word (below: c->r[2].w[0])
                    if not d & _RD.V0Q:
                        h &= ~(1 << 2)
                    if not d & _RD.V1:
                        g &= ~(1 << 3)
                        h &= ~(1 << 3)
                    if not d & _RD.F0:
                        fl &= ~1
                    if not d & _RD.F1:
                        fl &= ~2
            elif kind == "sys":
                g &= args_all | 1 << 3 | 1 << 28 | 1 << 29  # arguments, v1 (syscall number), sp, gp
                fl = 0
                h &= args_all | 1 << 3
            elif kind.startswith("x"):                     # transfer to an exempt (exact) function: every written
                g = fl = h = -1                            # local (after ABI calls the dirty sets are not exact)
        out = [("c->r[%d].w[0] = (uint32_t)r%d;" % (n, n) if low >> n & 1 and (n in narrow or kind == "ret")
                else store(n))
               for n in sorted(wr_r) if g >> n & 1] + \
            ["c->fpr[%d].u = f%d;" % (n, n) for n in sorted(wr_f) if fl >> n & 1] + \
            ["c->r[%d].d[1] = h%d;" % (n, n) for n in used_h if h >> n & 1]
        if kind in ("fatal", "sys") or kind.startswith(("x", "case=")):
            out += xstore                                  # the v5 locals of state no ABI boundary passes
        if wr_v:                                           # lever vfloc: VU0 state is no ABI state - every boundary
            vst = (vdin if m.group(1) == "i" else vdout).get(int(m.group(2), 16))
            vm = -1 if vst is None else vst
            out += ["%s = %s;" % (vslot(n, i), vname(n, i)) for n, i in wr_v if vm >> n & 1]
        return " ".join(out)

    def sync(m):                                           # before a 128-bit op: only dirty locals differ
        st = din.get(int(m.group(1), 16))
        g = st[0] if st is not None else -1
        return " ".join(store(int(n)) for n in m.group(2).split(",") if int(n) in wr_r and g >> int(n) & 1)

    reload = " ".join(["r%d = %s;" % (n, load(n)) for n in used_r
                       if n in (CALLER_SAVED_GPRS if not abi else (2, 3))] +
                      ["f%d = c->fpr[%d].u;" % (n, n) for n in used_f if not abi or n in (0, 1)] +
                      ["h%d = c->r[%d].d[1];" % (n, n) for n in used_h if n in (2, 3)])
    if vload:                                              # lever vfloc: VF / ACC lanes after a call
        reload = (reload + " " + vload).strip()
    wide_r = [n for n in used_r if n not in narrow]
    narrow_r = [n for n in used_r if n in narrow]
    if wide_r:
        head.append("    uint64_t %s;" % ", ".join("r%d = %s" % (n, load(n)) for n in wide_r))
    if narrow_r:
        head.append("    int32_t %s;" % ", ".join("r%d = %s" % (n, load(n)) for n in narrow_r))
    if used_f:
        head.append("    uint32_t %s;" % ", ".join("f%d = c->fpr[%d].u" % (n, n) for n in used_f))
    if used_h or hilo:
        head.append("    uint64_t %s;" % ", ".join(["h%d = c->r[%d].d[1]" % (n, n) for n in used_h] +
                                                  ["%s = c->%s.d[%s]" % (n, n[:2], n[3]) for n in hilo]))
    if used_v:
        head.append("    uint32_t %s;" % ", ".join("%s = %s" % (vname(n, i), vslot(n, i)) for n, i in used_v))
    if acc:
        head.append("    uint32_t acc_ = c->acc.u;")
    if fcc:
        head.append("    int fcc_ = (c->fcr31 & FCR31_C) != 0;")
    if "m_," in body:
        head.append("    const R5900Mem m_ = {c->ram, c->ram_mask};")
    reload_all = " ".join(["r%d = %s;" % (n, load(n)) for n in used_r] +
                          ["f%d = c->fpr[%d].u;" % (n, n) for n in used_f] +
                          ["h%d = c->r[%d].d[1];" % (n, n) for n in used_h] + xload + ([vload] if vload else []))
    body = body.replace("/*VRELOAD*/", vload)
    body = re.sub(r"/\*FLUSH@([io])([0-9a-f]{8})\|([^*]*)\*/", flush, body).replace("/*RELOAD*/", reload)
    body = body.replace("/*RELOADALL*/", reload_all)
    body = body.replace("/*RELOADSYS*/", " ".join("r%d = %s;" % (n, load(n)) for n in used_r if n in (2, 3)))
    body = re.sub(r"/\*SYNC@([0-9a-f]{8}):([0-9,]+)\*/", sync, body)
    return "\n".join(head) + "\n" + body + "\n}\n"


def _translate_body(img, start, addrs, jump_tables, func_starts, cache):
    insns = {a: decode(img.word(a), a) for a in sorted(addrs)}
    flows = {a: flow(i) for a, i in insns.items()}
    labels = {fl.target for fl in flows.values() if fl.kind in ("branch", "jump") and fl.target in insns}
    for targets in jump_tables.values():
        labels.update(t for t in targets if t in insns)
    fused = _ufuse_pairs(insns, flows, labels) if cache and _v5("ufuse") and _v5("maskbase") else {}
    lines = []
    skip = set()

    def leave(target, at):
        """Control transfer from `at` to `target`, outside this function (flush point: after `at`)."""
        return "{ /*FLUSH|%s=%08x*/ %s; return; }" % (_xk("tail", target), target, _call(target))

    for a in sorted(insns):
        if a in skip:
            continue
        i, fl = insns[a], flows[a]
        if a in labels:
            lines.append("L_%08x:;" % a)
        k = fl.kind
        ds = a + 4
        mark = len(lines)
        point = a
        if a in fused:                                     # lever ufuse: the pair as one unaligned access
            lines.append("    " + _MEM_RE.sub(r"rm_\1(m_, ", _ufuse_stmt(fused[a])))
            lines.append("    ;")                          # the second instruction of the pair
            skip.add(a + 4)
            nxt = a + 8
        elif not fl.has_delay_slot:
            lines.append("    " + _stmt(i, cache))
            nxt = a + 4
        else:
            point = ds if ds in insns else a
            ds_s = _stmt(insns[ds], cache) if ds in insns else ";"
            skip.add(ds)
            nxt = a + 8
            if k == "branch" or (k == "jump" and i.mnem != "j"):
                go = "goto L_%08x;" % fl.target if fl.target in insns else leave(fl.target, a)
                if k == "jump":
                    lines.append("    { %s %s }" % (ds_s, go))
                elif fl.likely:
                    lines.append("    if (%s) { %s %s }" % (_cond(i), ds_s, go))
                else:
                    lines.append("    { int t_ = (%s); %s if (t_) %s }" % (_cond(i), ds_s, go))
            elif k == "return":
                lines.append("    %s /*FLUSH|ret*/ return;" % ds_s)
            elif k == "call":                              # jal / bal: link written before the delay slot
                t = fl.target
                lines.append("    %s %s /*FLUSH|%s=%08x*/ %s %s" % (W(31, "0x%08xull" % (a + 8)), ds_s, _xk("call", t),
                                                                  t, _pcall(_call(t), t), _reload_after(t)))
            elif k == "call_conditional":                  # bltzal/bgezal(l): link written whether taken or not
                t = fl.target
                if fl.likely:
                    lines.append("    { int t_ = (%s); %s if (t_) { %s /*FLUSH|%s=%08x*/ %s %s } }"
                                 % (_cond(i), W(31, "0x%08xull" % (a + 8)), ds_s, _xk("call", t), t,
                                    _pcall(_call(t), t), _reload_after(t)))
                else:
                    lines.append("    { int t_ = (%s); %s %s if (t_) { /*FLUSH|%s=%08x*/ %s %s } }"
                                 % (_cond(i), W(31, "0x%08xull" % (a + 8)), ds_s, _xk("call", t), t,
                                    _pcall(_call(t), t), _reload_after(t)))
            elif k == "call_indirect":                     # jalr: target read, link written, delay slot, call
                link = "" if fl.link == 0 else W(fl.link, "0x%08xull" % (a + 8)) + " "
                lines.append("    { uint32_t t_ = (uint32_t)%s; %s%s /*FLUSH|icall*/ %s /*RELOAD*/ }"
                             % (R(fl.reg), link, ds_s, _pcall("%s(c, t_)" % _dispatch())))
            elif k == "jump" and _j_goto(fl.target, insns):  # j into this function's own code (crt0 loops): goto
                lines.append("    { %s goto L_%08x; }" % (ds_s, fl.target))
            elif k == "jump":                              # j: tail call
                t = fl.target
                lines.append("    %s /*FLUSH|%s=%08x*/ %s; return;" % (ds_s, _xk("tail", t), t, _call(t)))
            elif k == "jump_indirect" and a in jump_tables:
                # a case outside this function (discovery made it another function's start) is a tail call into it;
                # that code reads whatever this function left in any register, so everything is written back
                # (flush kind "case": not an ABI boundary)
                cases = " ".join("case 0x%08xu: %s" % (t, "goto L_%08x;" % t if t in insns else
                                                       "{ /*FLUSH|%s=%08x*/ %s; return; }" % (_xk("case", t), t,
                                                                                             _call(t)))
                                 for t in sorted(set(jump_tables[a])))
                lines.append("    { uint32_t t_ = (uint32_t)%s; %s switch (t_) { %s default: /*FLUSH|fatal*/ "
                             "rt_fatal(c, \"switch\", 0x%08xu, t_); return; } }" % (R(fl.reg), ds_s, cases, a))
            elif k == "jump_indirect":                     # jr reg without a table: indirect tail call
                lines.append("    { uint32_t t_ = (uint32_t)%s; %s /*FLUSH|itail*/ %s(c, t_); return; }"
                             % (R(fl.reg), ds_s, _dispatch()))
            else:
                raise Unsupported("%08x: %s (%s)" % (a, i.text, k))
            if ds in labels:                               # something jumps into the delay slot
                lines.append("    goto L_%08x;" % (ds + 4))
                lines.append("L_%08x:;" % ds)
                lines.append("    " + ds_s)
                labels.discard(ds + 4)
                lines.append("L_%08x:;" % (ds + 4))
        if k in _FALLS_THROUGH and nxt not in insns:      # execution continues past this function's code
            lines.append("    " + leave(nxt, a))
        for n in range(mark, len(lines)):                  # control-flow flushes happen after the delay slot
            lines[n] = lines[n].replace("/*FLUSH|", "/*FLUSH@o%08x|" % point)
    return lines


def reachable(img, fs, roots):
    """Functions reachable from `roots` through direct calls, tail calls, branches and fall-through into other
    functions. Indirect (JALR / jr) targets are not followed: pass them as roots."""
    funcs = fs.functions
    starts = set(funcs)
    seen, work = set(), [r for r in roots if r in funcs]
    while work:
        a = work.pop()
        if a in seen:
            continue
        seen.add(a)
        fn = funcs[a]
        nxt = set(fn.calls) | set(fn.tail_calls) | _exits(img, fn, starts)
        work.extend(t for t in nxt if t in funcs and t not in seen)
    return seen


def _emit_parts(img, fs, roots, cache, prefix, narrow, fpu_fast, abi, livein, external, dispatch, lookup,
                stub_unsupported=False, progress=None, abi_exempt=None, v5=None, unit=None):
    """Shared core of emit_program / emit_units: (todo, head lines, external decls, decls, [(addr, body)],
    table+dispatcher text, exports, unsupported [(addr, message)])."""
    funcs = fs.functions
    todo = sorted(reachable(img, fs, roots) if roots is not None else funcs)
    starts = set(funcs)
    dispatch = dispatch or prefix + "rc_dispatch"
    # exempt targets: in ABI mode translated exact, full boundaries; in the exact cached variants (v1-v3) the calls of
    # such a function still reload every register afterwards (it may change callee-saved ones, e.g. gp)
    exempt = frozenset(abi_exempt or ()) if cache else frozenset()
    saved = dict(_LOCAL)
    _LOCAL.update(prefix=prefix, dispatch=dispatch, starts=starts, external=external or {}, exempt=exempt)
    unsupported = []
    try:
        # v5 ctx32: which argument slots each ABI-mode function of this unit reads only as a low word (its callers
        # may then write those arguments as one word); computed on the same final narrowing the translation uses
        lowonly = {}
        do_low = "ctx32" in _levers(v5, cache, fpu_fast, abi) and abi
        # lever rlive: call-site summaries of every function, then the program-wide return demand
        do_rl = bool("rlive" in _levers(v5, cache, fpu_fast, abi) and abi and RLIVE["on"] and unit in RLIVE["units"])
        rsum = {}
        do_ax = do_low and "argx" in _levers(v5, cache, fpu_fast, abi)    # lever argx
        expo = {}
        if do_low or do_rl:
            for a in todo:
                fn = funcs[a]
                jt = {jr: targets for jr, _table, targets in fn.jump_tables}
                if a in exempt:
                    if do_rl:
                        rsum[a] = _RD.site_summaries(fn_cfg(img, a, fn.addrs, jt, starts), frozenset(), exempt,
                                                     exact=True)
                    continue
                nr = narrow.get(a, frozenset()) if narrow else frozenset()
                f, nrf, _d, _c, _q = _plan(img, a, fn.addrs, jt, starts, nr, _levers(v5, cache, fpu_fast, True), cache)
                if do_low:
                    if do_ax:
                        pt = expo[a] = {}
                        lowonly[a] = _lowonly_args(f, jt, nrf, livein, pt)
                    else:
                        lowonly[a] = _lowonly_args(f, jt, nrf, livein)
                if do_rl:
                    rsum[a] = _RD.site_summaries(f, nrf, exempt)
        if do_ax:                                        # one-word slots through pass-through callees: a register
            n0 = sum(bin(m).count("1") for m in lowonly.values())     # stays one-word if every callee it is
            changed = True                                            # exposed to reads that slot as one word
            while changed:
                changed = False
                for a, pt in expo.items():
                    bad = 0
                    for t, m in pt.items():
                        bad |= m & ~lowonly.get(t, 0)
                    if lowonly[a] & bad:
                        lowonly[a] &= ~bad
                        changed = True
            RLIVE["stats"]["argx_%s" % unit] = {"candidates": n0,
                                                "one_word": sum(bin(m).count("1") for m in lowonly.values())}
        _LOCAL["lowonly"] = lowonly
        if do_rl:
            dem, ind = _RD.solve(rsum, full=RLIVE["full"] | exempt, indirect=RLIVE["indirect"])
            _LOCAL["retdem"] = dem
            from collections import Counter
            RLIVE["stats"][unit] = {"functions": len(dem), "indirect_demand": ind,
                                    "demand_histogram": {"%02x" % k: v for k, v in
                                                         sorted(Counter(dem[a] for a in todo if a in dem).items())}}
        decls = ["void %s(R5900Ctx *c);" % fname(a) for a in todo]
        bodies = []
        from ps2rc.pipeline import parallel as _par       # PS2RC_TRANSLATE_JOBS > 1: the same text from a pool
        njobs = _par.jobs()
        if njobs > 1 and len(todo) > 2 * _par.CHUNK:
            bodies, unsup = _par.emit_bodies(img, funcs, todo, starts, cache, narrow, fpu_fast, abi, exempt, livein,
                                             v5, _LOCAL, VFLOC_SKIP, stub_unsupported, njobs, progress)
            unsupported.extend(unsup)
            todo_serial = []
        else:
            todo_serial = todo
        for k, a in enumerate(todo_serial):
            fn = funcs[a]
            jt = {jr: targets for jr, _table, targets in fn.jump_tables}
            nr = narrow.get(a, frozenset()) if narrow else frozenset()
            try:
                bodies.append((a, translate_function(img, a, fn.addrs, jt, starts, cache, nr, fpu_fast,
                                                     abi and a not in exempt, livein, program_abi=abi, v5=v5)))
            except Unsupported as e:
                if not stub_unsupported:
                    raise
                unsupported.append((a, str(e)))
                _LOCAL.update(prefix=prefix, dispatch=dispatch, starts=starts, external=external or {}, exempt=exempt)
                bodies.append((a, "void %s(R5900Ctx *c)\n{\n    rt_fatal(c, \"unsupported\", 0x%08xu, 0);\n}\n"
                               % (fname(a), a)))
            if progress and k % 500 == 0:
                progress(k, len(todo))
        names = [(a, fname(a)) for a in todo]
    finally:
        _LOCAL.clear()
        _LOCAL.update(saved)
    ext = ["void %s(R5900Ctx *c);" % n for n in sorted(set((external or {}).values()))]
    tab = prefix + "rc_funcs"
    table = ["static const struct { uint32_t a; void (*f)(R5900Ctx *); } %s[%d] = {" % (tab, len(todo))]
    table += ["    {0x%08xu, %s}," % (a, n) for a, n in names]
    table.append("};")
    search = """    unsigned lo = 0, hi = %d;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (%s[m].a < a) lo = m + 1; else hi = m;
    }""" % (len(todo), tab)
    if lookup:
        disp = """rc_fn %src_lookup(uint32_t a)
{
%s
    return lo < %d && %s[lo].a == a ? %s[lo].f : 0;
}
""" % (prefix, search, len(todo), tab, tab)
        exports = "rc_fn %src_lookup(uint32_t guest_addr);\n" % prefix
    else:
        disp = """void %s(R5900Ctx *c, uint32_t a)
{
    if (a - rc_overlay_lo < rc_overlay_hi - rc_overlay_lo) {      /* overlay window: the loaded overlay's code */
        rc_lookup_fn l = rc_overlay_lookup[rc_overlay_current & (RC_MAX_OVERLAYS - 1)];
        rc_fn f = l ? l(a) : 0;
        if (f) { f(c); return; }
        rt_fatal(c, "overlay", (uint32_t)rc_overlay_current, a);
        return;
    }
%s
    if (lo < %d && %s[lo].a == a) { %s[lo].f(c); return; }
    rt_fatal(c, "dispatch", 0, a);
}
""" % (dispatch, search, len(todo), tab, tab)
        exports = "void %s(R5900Ctx *c, uint32_t guest_addr);\n" % dispatch
    head = ["/* generated by ps2rc.backend_c - do not edit */", '#include "r5900_rt.h"', exports.rstrip()]
    if lookup:                                       # an overlay's JALRs go to the main dispatcher
        head.append("void %s(R5900Ctx *c, uint32_t guest_addr);" % dispatch)
    return todo, head, ext, decls, bodies, "\n".join(table) + "\n" + disp, exports, unsupported


def emit_program(img, fs, roots=None, cache=False, prefix="", narrow=None, fpu_fast=False, abi=False,
                 livein=None, external=None, dispatch=None, lookup=False, abi_exempt=None, v5=None):
    """C translation unit for the functions reachable from `roots` (all discovered functions by default).

    prefix: symbol prefix of the generated functions (several translations in one binary: back-end variants, overlay
    units); external: {guest address: C name} of functions translated in another unit (an overlay calls main code
    directly); calls to any other address go through the dispatcher. The unit defines the JALR dispatcher
    `dispatch` (default <prefix>rc_dispatch: binary search over this unit's functions, guest addresses inside the
    overlay window go to the lookup of the loaded overlay - rc_overlay_*), or with lookup=True only
    <prefix>rc_lookup(guest address) -> function or NULL for the main dispatcher (an overlay unit).
    abi_exempt (cached variants v1+): guest addresses of functions that must stay exact (the non-ABI detector,
    the pack's [translate] non_abi "exact_starts"; for an overlay unit also the main-code ones it calls). In ABI mode those
    functions are translated with abi=False (their narrowing plan must be computed with abi=False too) and every
    direct transfer to one of them is a full boundary (translate_function: exempt); in v1-v3 every call of one
    reloads all registers afterwards.
    v5: back-end v5 lever set (V5_LEVELS; with cache + fpu_fast, i.e. on top of v3/v4).
    Returns (source, header)."""
    _todo, head, ext, decls, bodies, tail, exports, _u = _emit_parts(img, fs, roots, cache, prefix, narrow, fpu_fast,
                                                                     abi, livein, external, dispatch, lookup,
                                                                     abi_exempt=abi_exempt, v5=v5)
    src = head + [""] + ext + decls + [""] + [b for _a, b in bodies] + [tail]
    header = "/* generated by ps2rc.backend_c */\n" + "\n".join(decls) + "\n" + exports
    return "\n".join(src), header


def emit_units(img, fs, name, roots=None, per_unit=150, progress=None, **opts):
    """A whole program as several C compilation units (big programs: parallel builds, bounded compiler memory):
    <name>.h (declarations), <name>_NNN.c (per_unit functions each), <name>_tab.c (table + dispatcher/lookup).
    Functions with an instruction the back-end does not support become stubs calling rt_fatal("unsupported").
    Returns ({file name: text}, [(address, message)] of the stubbed functions, [translated function addresses])."""
    todo, head, ext, decls, bodies, tail, exports, unsupported = _emit_parts(
        img, fs, roots, opts.get("cache", False), opts.get("prefix", ""), opts.get("narrow"),
        opts.get("fpu_fast", False), opts.get("abi", False), opts.get("livein"), opts.get("external"),
        opts.get("dispatch"), opts.get("lookup", False), stub_unsupported=True, progress=progress,
        abi_exempt=opts.get("abi_exempt"), v5=opts.get("v5"), unit=name)
    guard = "PS2RC_%s_H" % name.upper()
    files = {name + ".h": "\n".join(["#ifndef " + guard, "#define " + guard] + head + ext + decls +
                                     ["#endif", ""])}
    for k in range(0, len(bodies), per_unit):
        part = bodies[k:k + per_unit]
        files["%s_%03d.c" % (name, k // per_unit)] = "\n".join(['#include "%s.h"' % name, ""] + [b for _a, b in part])
    files[name + "_tab.c"] = '#include "%s.h"\n\n%s' % (name, tail)
    return files, unsupported, todo
