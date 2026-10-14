"""VU0 micro-program translator: VU microcode -> C that runs a VCALLMS subroutine to completion on the EE runtime's
VU0 register file (R5900Ctx.vu0, ps2rc runtime header r5900_rt.h).

How (all decided at translation time, nothing interpreted at run time):
  1. Timing. Every entry point is expanded into a graph of instruction instances by an abstract run of the micro-mode
     pipeline (one instruction per cycle; FMAC-pipe results usable 4 cycles after issue, readers of those lanes
     stall; ACC bypassed; MAC / clip flags seen by FMAND / FCAND & co. are those of the newest FMAC / CLIP issued at
     least 4 cycles earlier; Q 7 cycles after DIV/SQRT, 13 after RSQRT, upper Q reads not interlocked, WAITQ and a
     second FDIV op wait; upper and lower read their operands before either writes; I is read before the LOI of the
     same instruction; branches and the E bit have one delay slot). The state of that pipeline is part of a node's
     identity, so an instruction reached with different timing becomes two nodes and every flag / Q read in the
     output names its producer statically. Data values never influence timing (only branch outcomes, which fork).
  2. Arithmetic (the EE runtime's VU0 model = PCSX2's interpreter with clamping): operands with exponent 0 -> +-0,
     exponent 255 -> +-FLT_MAX; IEEE single with the runtime's FPU in round-toward-zero + flush-to-zero; products
     rounded before MADD/MSUB/OPMSUB accumulate; DIV/RSQRT by +-0 -> +-FLT_MAX (sign of s ^ t; RSQRT 0/0 -> signed 0);
     SQRT of |t|; CLIP compares raw bit patterns; MAC = Z/S of the dest lanes (OPMULA/OPMSUB keep the w bits).
     A dataflow pass knows which lanes hold values computed by the program (always finite and normal or zero) and
     clamps only the others (caller's registers, raw copies).
  3. Output: one C function per entry with every VF/ACC/VI lane in a local (loaded at the start, stored at each exit
     only where the paths to that exit may have written it), the flag / Q producers as locals; a dispatcher
     `int SYM(R5900Ctx *c, uint32_t addr)` (byte address of the entry, as VCALLMS passes it; returns 1 for an
     unknown address) and `int SYM_is_code(const void *code, uint32_t nbytes)` (FNV-1a check of an uploaded copy).
  Not supported (refused with a message, never guessed): VU data memory (LQ/SQ/ILW/...), XGKICK/XTOP, EFU (P),
  status-flag instructions, FCSET, MAX/MINI/FTOI, the R register, a branch that reads a VI written by the integer
  op right before it, a branch in a delay slot, D/T bits, programs whose expansion does not converge.
The output is code translated from the microcode it was generated from: generate it on the user's machine.
"""
import io
from dataclasses import dataclass, field

from .micro_gen_isa import decode_program, text, XYZW

FDIV_LAT = {"DIV": 7, "SQRT": 7, "RSQRT": 13}
MAX_NODES = 20000


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------------------------------------- timing expansion
@dataclass(frozen=True)
class St:
    pc: int
    br: tuple = ()                 # (countdown, target)
    eb: int = 0                    # E-bit countdown
    fmac: tuple = ()               # entries (age, ((reg, mask), ...), MAC snapshot, own pc if a CLIP else -2, own pc,
                                   #          sets MAC)
    mac_vis: int = -1              # pc of the FMAC whose MAC flags are visible (-1 = the caller's)
    clip_vis: int = -1
    run_mac: int = -1              # newest FMAC executed / CLIP executed
    run_clip: int = -1
    fdiv: tuple = ()               # (age, latency, producer pc)
    q_vis: int = -1
    q_run: int = -1
    links: tuple = ()              # ((vi, value), ...) VI registers holding BAL links
    last_ialu: int = 0             # VI written by an integer op in the previous instruction


@dataclass
class Node:
    id: int
    st: St
    m: object                      # Micro
    mac_src: int = -1              # resolved flag / Q producers seen by this instance's readers
    clip_src: int = -1
    q_src: int = -1
    mac_prev: int = -1             # run_mac before this instruction (OPMULA/OPMSUB keep-w)
    succ: list = field(default_factory=list)   # [(kind, node id or None)] kind: "next", "taken", "fall", "exit"
    exit_st: St = None             # pipeline drained at an exit
    tpc: int = 0
    cond: str = ""                 # branch condition (C) for "taken"


def _age(fmac, d):
    return tuple((e[0] + d,) + e[1:] for e in fmac)


class Expander:
    def __init__(self, prog):
        self.p = prog
        self.nodes = []
        self.key = {}

    def node(self, st):
        n = self.key.get(st)
        if n is not None:
            return n
        if len(self.nodes) >= MAX_NODES:
            raise Unsupported("the timing expansion does not converge (more than %d instruction instances)" % MAX_NODES)
        if not 0 <= st.pc < len(self.p):
            raise Unsupported("execution leaves the microcode at 0x%03x" % st.pc)
        n = Node(len(self.nodes), st, self.p[st.pc])
        self.nodes.append(n)
        self.key[st] = n.id
        self.work.append(n.id)
        return n.id

    def expand(self, entry):
        self.work = []
        root = self.node(St(pc=entry))
        while self.work:
            self.step(self.nodes[self.work.pop()])
        return root

    @staticmethod
    def _stall(fmac, reads):
        need = 0
        for reg, mask in reads:
            if reg == 0 or not mask:
                continue
            for a, ws, *_ in fmac:
                if a >= 4:
                    continue
                for r, m in ws:
                    if r == reg and m & mask:
                        need = max(need, 4 - a)
        return need

    def step(self, n):
        s, m = n.st, n.m
        if m.unknown:
            raise Unsupported("0x%03x: unknown instruction (%s)" % (m.index, ", ".join(m.unknown)))
        if m.dbit or m.tbit:
            raise Unsupported("0x%03x: D/T bit (debug break) not supported" % m.index)
        u, lo = m.up, m.lo
        if u.base in ("MAX", "MINI", "FTOI"):
            raise Unsupported("0x%03x: %s not supported (semantics not verified)" % (m.index, u.name))
        lname = lo.name if lo is not None else "LOI"
        if lname in ("LQ", "SQ", "ILW", "ISW", "LQI", "SQI", "LQD", "SQD", "ILWR", "ISWR", "XTOP", "XITOP", "XGKICK",
                     "MFP", "WAITP", "RNEXT", "RGET", "RINIT", "RXOR", "FSEQ", "FSSET", "FSAND", "FSOR", "FCSET") \
                or lname.startswith("E"):
            raise Unsupported("0x%03x: %s not supported" % (m.index, lname))
        if s.br and s.br[0] == 1 and lo is not None and lo.is_branch:
            raise Unsupported("0x%03x: branch in a branch delay slot" % m.index)
        # 1. a new cycle; stalls
        fmac, fdiv = _age(s.fmac, 1), ((s.fdiv[0] + 1,) + s.fdiv[1:]) if s.fdiv else ()
        up_fmac = u.base != "NOP"
        reads = list(u.vf_reads()) if up_fmac else []
        lo_pipe = None
        if lo is not None:
            if lname in ("MOVE", "MR32", "MFIR", "FMAND", "FMOR", "FMEQ", "FCAND", "FCOR", "FCEQ", "FCGET"):
                lo_pipe = "fmac"
                if lname in ("MOVE", "MR32", "MFIR") and lo.ft == 0:
                    lo_pipe = None
            elif lname in FDIV_LAT or lname == "WAITQ":
                lo_pipe = "fdiv"
            reads += lo.vf_reads() if lo_pipe in ("fmac", "fdiv") or lname == "MTIR" else []
        d = self._stall(fmac, reads)
        if lo_pipe == "fdiv" and fdiv and fdiv[0] + d < fdiv[1]:
            d = fdiv[1] - fdiv[0]
        if d:
            fmac = _age(fmac, d)
            fdiv = ((fdiv[0] + d,) + fdiv[1:]) if fdiv else ()
        # 2. retire
        mac_vis, clip_vis, q_vis = s.mac_vis, s.clip_vis, s.q_vis
        keep = []
        for e in fmac:
            if e[0] >= 4:
                mac_vis = e[2]
                if e[3] != -2:
                    clip_vis = e[3]
            else:
                keep.append(e)
        fmac = tuple(keep)
        if fdiv and fdiv[0] >= fdiv[1]:
            q_vis, fdiv = fdiv[2], ()
        n.mac_src, n.clip_src, n.q_src = mac_vis, clip_vis, q_vis
        n.mac_prev = s.run_mac
        # 3. execute (static effects)
        run_mac, run_clip, q_run = s.run_mac, s.run_clip, s.q_run
        if u.sets_mac():
            run_mac = m.index
        if u.base == "CLIP":
            run_clip = m.index
        # a reader names its producer by pc: a newer instance of that pc still in flight would be ambiguous
        if mac_vis >= 0 and any(e[4] == mac_vis and e[5] for e in fmac):
            raise Unsupported("0x%03x: MAC of 0x%03x read while a newer instance is in flight" % (m.index, mac_vis))
        if clip_vis >= 0 and any(e[3] == clip_vis for e in fmac):
            raise Unsupported("0x%03x: clip of 0x%03x read while a newer instance is in flight" % (m.index, clip_vis))
        if q_vis >= 0 and fdiv and fdiv[2] == q_vis:
            raise Unsupported("0x%03x: Q of 0x%03x read while a newer instance is in flight" % (m.index, q_vis))
        links = dict(s.links)
        last_ialu = 0
        vi_written = None
        if lo is not None:
            if lname in ("IADD", "ISUB", "IADDI", "IAND", "IOR"):
                vi_written = lo.id_ if lname != "IADDI" else lo.it
            elif lname in ("IADDIU", "ISUBIU"):
                vi_written = lo.it
            elif lname in ("FMAND", "FMOR"):
                vi_written = lo.it
            elif lname in ("FMEQ", "FCAND", "FCOR", "FCEQ"):
                vi_written = 1
            elif lname == "FCGET" or lname == "MTIR":
                vi_written = lo.it
            elif lname in ("BAL", "JALR"):
                vi_written = lo.it
            if lname in ("IADD", "ISUB", "IADDI", "IAND", "IOR", "IADDIU", "ISUBIU", "MTIR") and vi_written:
                last_ialu = vi_written
            if vi_written:
                links.pop(vi_written, None)
            if lname in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ", "JR", "JALR"):
                srcs = [lo.is_] + ([lo.it] if lname in ("IBEQ", "IBNE") else [])
                if s.last_ialu and s.last_ialu in srcs:
                    raise Unsupported("0x%03x: branch reads vi%02d written by the integer op right before it (VI "
                                      "delay)" % (m.index, s.last_ialu))
        if lo_pipe == "fdiv" and lname != "WAITQ":
            fdiv = (0, FDIV_LAT[lname], m.index)
            q_run = m.index
        if up_fmac or lo_pipe == "fmac":
            ws = []
            w = u.vf_write() if up_fmac else None
            if w and w[0]:
                ws.append(w)
            if lo_pipe == "fmac":
                w2 = lo.vf_write()
                if w2 and w2[0]:
                    if w and w[0] == w2[0]:
                        raise Unsupported("0x%03x: upper and lower write vf%02d" % (m.index, w2[0]))
                    ws.append(w2)
            fmac = fmac + ((0, tuple(ws), run_mac, m.index if u.base == "CLIP" else -2, m.index, u.sets_mac()),)
        # 4. control flow
        br = s.br
        eb = s.eb
        if m.ebit:
            eb = 2
        succ_states = []
        if lo is not None and lo.is_branch:
            tgt = None
            if lname in ("B", "BAL"):
                tgt = m.index + 1 + lo.imm
            elif lname in ("JR", "JALR"):
                known = dict(s.links)
                if lo.is_ not in known:
                    raise Unsupported("0x%03x: %s through vi%02d with no known value" % (m.index, lname, lo.is_))
                tgt = known[lo.is_]
            if lname in ("BAL", "JALR") and lo.it:
                links[lo.it] = m.index + 2
            if tgt is not None:
                succ_states.append(("next", (2, tgt)))
            else:
                t = m.index + 1 + lo.imm
                succ_states.append(("taken", (2, t)))
                succ_states.append(("fall", ()))
        else:
            succ_states.append(("next", None))
        base = dict(fmac=fmac, mac_vis=mac_vis, clip_vis=clip_vis, run_mac=run_mac, run_clip=run_clip, fdiv=fdiv,
                    q_vis=q_vis, q_run=q_run, links=tuple(sorted(links.items())), last_ialu=last_ialu)
        for kind, newbr in succ_states:
            b = br if newbr is None else (newbr if newbr else br)
            npc = m.index + 1
            if b:
                cnt, t = b
                cnt -= 1
                if cnt == 0:
                    npc, b = t, ()
                else:
                    b = (cnt, t)
            e2 = eb
            if e2:
                e2 -= 1
                if e2 == 0:                                       # the E-bit delay slot was this instruction
                    n.exit_st = St(pc=npc, **base)
                    n.tpc = npc
                    n.succ.append((kind, None))
                    continue
            n.succ.append((kind, self.node(St(pc=npc, br=b, eb=e2, **base))))


# ---------------------------------------------------------------------------------------------- dataflow
def _lanes(mask):
    return [i for i in range(4) if mask & (8 >> i)]


def _defs(m):
    """VF lanes / ACC lanes / VI regs / other state an instruction writes: set of keys"""
    out = set()
    u, lo = m.up, m.lo
    w = u.vf_write() if u.base != "NOP" else None
    if w and w[0]:
        out |= {("vf", w[0], i) for i in _lanes(w[1])}
    if u.writes_acc():
        out |= {("acc", 0, i) for i in _lanes(u.dest if u.base != "OPMULA" else 0xE)}
    if lo is not None:
        w2 = lo.vf_write()
        if w2 and w2[0]:
            out |= {("vf", w2[0], i) for i in _lanes(w2[1])}
        nm = lo.name
        vi = None
        if nm in ("IADD", "ISUB", "IAND", "IOR"):
            vi = lo.id_
        elif nm in ("IADDI", "IADDIU", "ISUBIU", "FMAND", "FMOR", "FCGET", "MTIR", "BAL", "JALR"):
            vi = lo.it
        elif nm in ("FMEQ", "FCAND", "FCOR", "FCEQ"):
            vi = 1
        if vi:
            out.add(("vi", vi, 0))
    if m.ibit:
        out.add(("I", 0, 0))
    if u.base == "CLIP":
        out.add(("clip", 0, 0))
    return out


def _clean_out(m, inset):
    """must-be-clean set after the instruction: keys ("vf", r, lane) / ("acc", 0, lane) holding program results"""
    s = set(inset)
    u, lo = m.up, m.lo
    produced, raw = set(), {}
    if u.base in ("ADD", "SUB", "MUL", "MADD", "MSUB", "ITOF"):
        dst = ("acc", 0) if u.acc_dst else ("vf", u.ft if u.base == "ITOF" else u.fd)
        produced |= {dst + (i,) for i in _lanes(u.dest)}
    elif u.base == "OPMULA":
        produced |= {("acc", 0, i) for i in range(3)}
    elif u.base == "OPMSUB":
        produced |= {("vf", u.fd, i) for i in range(3)}
    elif u.base == "ABS":
        for i in _lanes(u.dest):
            raw[("vf", u.ft, i)] = ("vf", u.fs, i)
    if lo is not None and lo.name in ("MOVE", "MR32") and lo.ft:
        for i in _lanes(lo.dest):
            raw[("vf", lo.ft, i)] = ("vf", lo.fs, i if lo.name == "MOVE" else (i + 1) & 3)
    if lo is not None and lo.name == "MFIR" and lo.ft:
        for i in _lanes(lo.dest):
            raw[("vf", lo.ft, i)] = None
    new = set(s)
    for k, src in raw.items():                    # raw copies: clean iff the source lane is clean (VF00 is)
        if src is not None and (src[1] == 0 or src in s):
            new.add(k)
        else:
            new.discard(k)
    for k in produced:
        if k[0] == "vf" and k[1] == 0:
            continue
        new.add(k)
    return new


def dataflow(nodes, root):
    """per node: clean-in (must), written-in (may)"""
    preds = {n.id: [] for n in nodes}
    order = []
    seen = set()
    stack = [(root, 0)]
    while stack:                                   # reverse post-order
        v, i = stack.pop()
        if i == 0:
            if v in seen:
                continue
            seen.add(v)
        succ = [t for _, t in nodes[v].succ if t is not None]
        if i < len(succ):
            stack.append((v, i + 1))
            stack.append((succ[i], 0))
        else:
            order.append(v)
    order.reverse()
    for v in order:
        for _, t in nodes[v].succ:
            if t is not None:
                preds[t].append(v)
    ALL = None
    cin = {v: ALL for v in order}
    win = {v: set() for v in order}
    cin[root] = set()
    changed = True
    while changed:
        changed = False
        for v in order:
            if v != root:
                ps = [p for p in preds[v] if cin[p] is not None]
                c = None
                for p in ps:
                    o = _clean_out(nodes[p].m, cin[p])
                    c = set(o) if c is None else c & o
                w = set()
                for p in preds[v]:
                    w |= win[p] | _defs(nodes[p].m)
                if c != cin[v] or w != win[v]:
                    cin[v], win[v] = c, w
                    changed = True
    return order, cin, win


# ---------------------------------------------------------------------------------------------- C emission
def _v(r, i):
    return "vf%d%s" % (r, XYZW[i])


class Emit:
    def __init__(self, prog, sym, code, mode="memory"):
        if mode not in ("memory", "locals"):
            raise ValueError("mode is 'memory' or 'locals'")
        self.p = prog
        self.sym = sym
        self.code = code
        self.mode = mode

    def lane(self, key, clean):
        kind, r, i = key
        if kind == "vf":
            if r == 0:
                return "1.0f" if i == 3 else "0.0f"
            v = _v(r, i)
        else:
            v = "acc" + XYZW[i]
        return v if key in clean else "CLF(%s)" % v

    def entry_fn(self, entry):
        ex = Expander(self.p)
        root = ex.expand(entry)
        nodes = ex.nodes
        order, cin, win = dataflow(nodes, root)
        uses = set()
        for v in order:
            n = nodes[v]
            uses |= _defs(n.m)
            for r, mk in n.m.up.vf_reads() + (n.m.lo.vf_reads() if n.m.lo is not None else []):
                if r:
                    uses |= {("vf", r, i) for i in _lanes(mk)}
            if n.m.up.reads_acc():
                uses |= {("acc", 0, i) for i in range(4)}
            if n.m.lo is not None:
                lo = n.m.lo
                for r in (lo.is_, lo.it):
                    if r and lo.name in ("IADD", "ISUB", "IAND", "IOR", "IADDI", "IADDIU", "ISUBIU", "IBEQ", "IBNE",
                                         "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ", "JR", "JALR", "FMAND", "FMOR",
                                         "FMEQ", "MFIR"):
                        uses.add(("vi", r, 0))
        # producers referenced; for MAC also which bits (a lane's Z/S bits: x 0x88, y 0x44, z 0x22, w 0x11)
        mac_bits, clip_need, q_need = {}, set(), set()
        for v in order:
            n = nodes[v]
            lo = n.m.lo
            if lo is not None and lo.name in ("FMAND", "FMOR", "FMEQ"):
                mac_bits[n.mac_src] = 0xFF
            if lo is not None and lo.name in ("FCAND", "FCOR", "FCEQ", "FCGET"):
                clip_need.add(n.clip_src)
            if n.m.up.src2 == "q" and n.m.up.base != "NOP":
                q_need.add(n.q_src)
            if n.exit_st is not None:
                mac_bits[n.exit_st.run_mac] = 0xFF
                q_need.add(n.exit_st.q_run)
        # OPMULA/OPMSUB keep the w bits of the previous MAC: that producer is needed for its w bits only
        changed = True
        while changed:
            changed = False
            for v in order:
                n = nodes[v]
                need = mac_bits.get(n.m.index, 0)
                if need & 0x11 and n.m.up.base in ("OPMULA", "OPMSUB"):
                    old = mac_bits.get(n.mac_prev, 0)
                    if old | (need & 0x11) != old:
                        mac_bits[n.mac_prev] = old | (need & 0x11)
                        changed = True
        self.mac_bits = mac_bits
        mac_need = set(mac_bits)
        out = io.StringIO()
        w = out.write
        fn = "%s_e%03x" % (self.sym, entry)
        w("static void %s(R5900Ctx *c)\n{\n" % fn)
        vfl = sorted(k for k in uses if k[0] == "vf")
        accl = sorted(k for k in uses if k[0] == "acc")
        vil = sorted(k for k in uses if k[0] == "vi")
        undef = []
        if self.mode == "locals":                      # the register file in locals: loaded here, stored at exits
            for k in vfl:
                w("    float %s = LDF(c->vu0.vf[%d].w[%d]);\n" % (_v(k[1], k[2]), k[1], k[2]))
            for k in accl:
                w("    float acc%s = LDF(c->vu0.acc.w[%d]);\n" % (XYZW[k[2]], k[2]))
            for k in vil:
                w("    u32 vi%d = c->vu0.vi[%d] & 0xFFFFu;\n" % (k[1], k[1]))
            w("    u32 ireg = c->vu0.vi[21];\n")
        else:                                          # the register file stays in memory (names = its fields)
            for k in vfl:
                undef.append(_v(k[1], k[2]))
                w("#define %s (*(mg_f32a *)(void *)&c->vu0.vf[%d].w[%d])\n" % (undef[-1], k[1], k[2]))
            for k in accl:
                undef.append("acc" + XYZW[k[2]])
                w("#define %s (*(mg_f32a *)(void *)&c->vu0.acc.w[%d])\n" % (undef[-1], k[2]))
            for k in vil:
                undef.append("vi%d" % k[1])
                w("#define %s (c->vu0.vi[%d])\n" % (undef[-1], k[1]))
            undef.append("ireg")
            w("#define ireg (c->vu0.vi[21])\n")
        self.undef = undef
        w("    int br_ = 0;\n")
        helpers = ["ireg", "br_"]
        if -1 in mac_need:
            w("    const u32 mac_entry = c->vu0.vi[17];\n")
            helpers.append("mac_entry")
        if any(n.m.up.base == "CLIP" for n in nodes) or clip_need:
            w("    u32 clip = c->vu0.vi[18];\n")
            helpers.append("clip")
        if -1 in clip_need:
            w("    const u32 clip_entry = c->vu0.vi[18];\n")
            helpers.append("clip_entry")
        if -1 in q_need:
            w("    const u32 q_entry = c->vu0.vi[22];\n")
            helpers.append("q_entry")
        for pc in sorted(x for x in mac_need if x >= 0):
            w("    u32 mac_%03x = 0;\n" % pc)
            helpers.append("mac_%03x" % pc)
        for pc in sorted(x for x in clip_need if x >= 0):
            w("    u32 clip_%03x = 0;\n" % pc)
            helpers.append("clip_%03x" % pc)
        for pc in sorted(x for x in q_need if x >= 0):
            w("    float q_%03x = 0.0f;\n" % pc)
            helpers.append("q_%03x" % pc)
        w("    %s;\n" % "; ".join("(void)%s" % h for h in helpers))
        self.mac_need, self.clip_need, self.q_need = mac_need, clip_need, q_need
        # body: nodes in reverse post-order, fall-through where possible; labels only where something jumps
        body = []
        targets = set()
        for k, v in enumerate(order):
            n = nodes[v]
            b = io.StringIO()
            b.write("/* %s */\n" % text(n.m))
            self.node(n, cin[v], b.write)
            nxt = order[k + 1] if k + 1 < len(order) else None
            succ = n.succ
            if len(succ) == 1:
                kind, t = succ[0]
                if t is None:
                    self.exit(n, win[v] | _defs(n.m), b.write)
                elif t != nxt:
                    b.write("    goto N%d;\n" % t)
                    targets.add(t)
            else:
                (k1, t1), (k2, t2) = succ
                if t1 is None or t2 is None:
                    raise Unsupported("0x%03x: conditional branch at the end of the program" % n.m.index)
                b.write("    if (br_) goto N%d;\n" % t1)
                targets.add(t1)
                if t2 != nxt:
                    b.write("    goto N%d;\n" % t2)
                    targets.add(t2)
            body.append((v, b.getvalue()))
        for v, s in body:
            if v in targets:
                w("N%d:;" % v)
            w(s)
        w("}\n")
        for name in self.undef:
            w("#undef %s\n" % name)
        w("\n")
        return fn, out.getvalue(), len(order)

    # one instruction instance
    def node(self, n, clean, w):
        m, u, lo = n.m, n.m.up, n.m.lo
        L = lambda key: self.lane(key, clean)
        tmp = []
        mac_parts = []
        # ---------------------------------------------------------------- upper: compute into temporaries
        if u.base in ("ADD", "SUB", "MUL", "MADD", "MSUB"):
            for i in _lanes(u.dest):
                a = L(("vf", u.fs, i))
                if u.src2 == "i":
                    b = "CLU(ireg)"
                elif u.src2 == "q":
                    b = ("q_%03x" % n.q_src) if n.q_src >= 0 else "CLU(q_entry)"
                else:
                    b = L(("vf", u.ft, u.bc if u.bc >= 0 else i))
                if u.base == "ADD":
                    e = "%s + %s" % (a, b)
                elif u.base == "SUB":
                    e = "%s - %s" % (a, b)
                elif u.base == "MUL":
                    e = "%s * %s" % (a, b)
                else:
                    acc = L(("acc", 0, i))
                    e = "%s %s (%s * %s)" % (acc, "+" if u.base == "MADD" else "-", a, b)
                tmp.append(("u%d" % i, e, ("acc%s" % XYZW[i]) if u.acc_dst else (_v(u.fd, i) if u.fd else None), i))
        elif u.base in ("OPMULA", "OPMSUB"):
            rot = ((1, 2), (2, 0), (0, 1))
            for i in range(3):
                a = L(("vf", u.fs, rot[i][0]))
                b = L(("vf", u.ft, rot[i][1]))
                if u.base == "OPMULA":
                    tmp.append(("u%d" % i, "%s * %s" % (a, b), "acc%s" % XYZW[i], i))
                else:
                    tmp.append(("u%d" % i, "%s - (%s * %s)" % (L(("acc", 0, i)), a, b),
                                _v(u.fd, i) if u.fd else None, i))
        elif u.base == "ITOF":
            for i in _lanes(u.dest):
                src = ("vf", u.fs, i)
                x = "0u" if u.fs == 0 and i < 3 else ("0x3F800000u" if u.fs == 0 else "B(%s)" % _v(u.fs, i))
                e = "(float)(int32_t)%s" % x
                if u.shift:
                    e = "(%s * %s)" % (e, {4: "0.0625f", 12: "(1.0f / 4096.0f)", 15: "(1.0f / 32768.0f)"}[u.shift])
                tmp.append(("u%d" % i, e, _v(u.ft, i) if u.ft else None, i))
        elif u.base == "ABS":
            for i in _lanes(u.dest):
                x = ("B(%s)" % _v(u.fs, i)) if u.fs else ("0x3F800000u" if i == 3 else "0u")
                tmp.append(("u%d" % i, "U(%s & 0x7FFFFFFFu)" % x, _v(u.ft, i) if u.ft else None, i))
        w("    {\n")
        for name, e, dst, i in tmp:
            w("        const float %s = %s;\n" % (name, e))
        if u.base == "CLIP":
            r = lambda reg, i: ("B(%s)" % _v(reg, i)) if reg else ("0x3F800000u" if i == 3 else "0u")
            w("        const u32 cj = clipj(%s, %s, %s, %s);\n" % (r(u.fs, 0), r(u.fs, 1), r(u.fs, 2), r(u.ft, 3)))
        # ---------------------------------------------------------------- lower (reads the old registers)
        post = []
        br = None
        if lo is not None:
            nm = lo.name
            vi = lambda r: "vi%d" % r if r else "0u"
            if nm in ("MOVE", "MR32") and lo.ft:
                for i in _lanes(lo.dest):
                    j = i if nm == "MOVE" else (i + 1) & 3
                    src = _v(lo.fs, j) if lo.fs else ("1.0f" if j == 3 else "0.0f")
                    w("        const float l%d = %s;\n" % (i, src))
                    post.append("%s = l%d;" % (_v(lo.ft, i), i))
            elif nm == "MFIR" and lo.ft:
                for i in _lanes(lo.dest):
                    post.append("%s = U((u32)(int32_t)(int16_t)%s);" % (_v(lo.ft, i), vi(lo.is_)))
            elif nm == "MTIR" and lo.it:
                x = ("B(%s)" % _v(lo.fs, lo.fsf)) if lo.fs else ("0x3F800000u" if lo.fsf == 3 else "0u")
                w("        vi%d = %s & 0xFFFFu;\n" % (lo.it, x))
            elif nm in ("IADD", "ISUB", "IAND", "IOR") and lo.id_:
                op = {"IADD": "+", "ISUB": "-", "IAND": "&", "IOR": "|"}[nm]
                w("        vi%d = (%s %s %s) & 0xFFFFu;\n" % (lo.id_, vi(lo.is_), op, vi(lo.it)))
            elif nm == "IADDI" and lo.it:
                w("        vi%d = (%s + %du) & 0xFFFFu;\n" % (lo.it, vi(lo.is_), lo.imm & 0xFFFF))
            elif nm in ("IADDIU", "ISUBIU") and lo.it:
                w("        vi%d = (%s %s %du) & 0xFFFFu;\n" % (lo.it, vi(lo.is_), "+" if nm == "IADDIU" else "-",
                                                         lo.imm))
            elif nm in ("FMAND", "FMOR", "FMEQ"):
                mac = ("mac_%03x" % n.mac_src) if n.mac_src >= 0 else "mac_entry"
                if nm == "FMAND" and lo.it:
                    w("        vi%d = %s & %s & 0xFFFFu;\n" % (lo.it, vi(lo.is_), mac))
                elif nm == "FMOR" and lo.it:
                    w("        vi%d = (%s | %s) & 0xFFFFu;\n" % (lo.it, vi(lo.is_), mac))
                elif nm == "FMEQ":
                    w("        vi1 = (%s & 0xFFFFu) == %s;\n" % (mac, vi(lo.is_)))
            elif nm in ("FCAND", "FCOR", "FCEQ", "FCGET"):
                cl = ("clip_%03x" % n.clip_src) if n.clip_src >= 0 else "clip_entry"
                if nm == "FCAND":
                    w("        vi1 = (%s & 0x%xu) != 0;\n" % (cl, lo.imm))
                elif nm == "FCOR":
                    w("        vi1 = ((%s | 0x%xu) & 0xFFFFFFu) == 0xFFFFFFu;\n" % (cl, lo.imm))
                elif nm == "FCEQ":
                    w("        vi1 = (%s & 0xFFFFFFu) == 0x%xu;\n" % (cl, lo.imm))
                elif lo.it:
                    w("        vi%d = %s & 0xFFFu;\n" % (lo.it, cl))
            elif nm in ("DIV", "SQRT", "RSQRT"):
                t = self.lane(("vf", lo.ft, lo.ftf), clean)
                if nm == "SQRT":
                    e = "fsqrtq(%s)" % t
                else:
                    s_ = self.lane(("vf", lo.fs, lo.fsf), clean)
                    e = "%s(%s, %s)" % ("fdivq" if nm == "DIV" else "frsqrt", s_, t)
                if m.index in self.q_need:                    # computed only when a reader / an exit uses it
                    w("        const float qn = %s;\n" % e)
                    post.append("q_%03x = qn;" % m.index)
            elif nm in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
                a = "(int16_t)%s" % vi(lo.is_)
                br = {"IBEQ": "%s == %s" % (vi(lo.is_), vi(lo.it)), "IBNE": "%s != %s" % (vi(lo.is_), vi(lo.it)),
                      "IBLTZ": "%s < 0" % a, "IBGTZ": "%s > 0" % a, "IBLEZ": "%s <= 0" % a,
                      "IBGEZ": "%s >= 0" % a}[nm]
            elif nm in ("BAL", "JALR") and lo.it:
                post.append("vi%d = %du;" % (lo.it, m.index + 2))
        # ---------------------------------------------------------------- commit
        for name, e, dst, i in tmp:
            if dst:
                w("        %s = %s;\n" % (dst, name))
        for p in post:
            if p:
                w("        %s\n" % p)
        if u.base == "CLIP":
            w("        clip = ((clip << 6) | cj) & 0xFFFFFFu;\n")
            if m.index in self.clip_need:
                w("        clip_%03x = clip;\n" % m.index)
        if u.sets_mac() and m.index in self.mac_need:
            need = self.mac_bits[m.index]
            parts = ["macl(B(u%d), %d)" % (i, i) for _, _, _, i in tmp if (0x88 >> i) & need]
            if u.base in ("OPMULA", "OPMSUB") and need & 0x11:
                prev = ("mac_%03x" % n.mac_prev) if n.mac_prev >= 0 else "mac_entry"
                parts.append("(%s & 0x1111u)" % prev)
            w("        mac_%03x = %s;\n" % (m.index, " | ".join(parts) or "0u"))
        if m.ibit:
            w("        ireg = 0x%08xu;\n" % m.lower)
        if br is not None:
            w("        br_ = %s;\n" % br)
        w("    }\n")

    def exit(self, n, written, w):
        st = n.exit_st
        for kind, r, i in sorted(written):
            if self.mode != "locals" and kind in ("vf", "acc", "vi", "I"):
                continue                               # already in the register file
            if kind == "vf" and r:
                w("    STF(c->vu0.vf[%d].w[%d], %s);\n" % (r, i, _v(r, i)))
            elif kind == "acc":
                w("    STF(c->vu0.acc.w[%d], acc%s);\n" % (i, XYZW[i]))
            elif kind == "vi" and r:
                w("    c->vu0.vi[%d] = vi%d;\n" % (r, r))
            elif kind == "I":
                w("    c->vu0.vi[21] = ireg;\n")
            elif kind == "clip":
                w("    c->vu0.vi[18] = clip;\n")
        if st.run_mac >= 0:
            w("    c->vu0.vi[17] = mac_%03x;\n" % st.run_mac)
        if st.q_run >= 0:
            w("    c->vu0.vi[22] = B(q_%03x);\n" % st.q_run)
        w("    c->vu0.vi[26] = 0x%xu;\n    return;\n" % n.tpc)

    def file(self, entries):
        out = io.StringIO()
        w = out.write
        h = 0xCBF29CE484222325
        for b in self.code:
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        w("/* %s.c - GENERATED by ps2rc.vu.micro_gen (VU0 microcode translator) from %d bytes of VU0 microcode\n"
          " * (FNV-1a 64 %016x). It contains code translated from that microcode: it is made on the user's machine\n"
          " * from the user's own copy and must not be distributed. Do not edit - regenerate.\n"
          " * Needs the EE runtime's FPU mode (round toward zero + flush-to-zero) and -ffp-contract=off. */\n"
          % (self.sym, len(self.code), h))
        w("#include <string.h>\n#include \"r5900_rt.h\"\n\n")
        w(PRELUDE)
        fns = []
        stats = []
        for e in entries:
            name, src, n = self.entry_fn(e)
            fns.append((e, name))
            stats.append((e, n))
            w(src)
        w("int %s(R5900Ctx *c, uint32_t addr)\n{\n    switch (addr) {\n" % self.sym)
        for e, name in fns:
            w("    case 0x%x: %s(c); return 0;\n" % (8 * e, name))
        w("    default: return 1;\n    }\n}\n\n")
        w("int %s_is_code(const void *code, uint32_t nbytes)\n{\n" % self.sym)
        w("    const unsigned char *p = (const unsigned char *)code;\n    uint64_t h = 0xCBF29CE484222325ull;\n")
        w("    if (nbytes != %du) return 0;\n" % len(self.code))
        w("    for (uint32_t k = 0; k < nbytes; k++) h = (h ^ p[k]) * 0x100000001B3ull;\n")
        w("    return h == 0x%016xull;\n}\n" % h)
        return out.getvalue(), stats


PRELUDE = r"""typedef uint32_t u32;
typedef float mg_f32a __attribute__((may_alias));
#define MG_INLINE static inline __attribute__((always_inline))
MG_INLINE float U(u32 x) { float f; memcpy(&f, &x, 4); return f; }
MG_INLINE u32 B(float f) { u32 u; memcpy(&u, &f, 4); return u; }
MG_INLINE float LDF(u32 x) { return U(x); }
#define STF(dst, f) ((dst) = B(f))
MG_INLINE float CLF(float x) { return U(vu0_clamp(B(x))); }      /* operand clamp of a value not computed here */
MG_INLINE float CLU(u32 x) { return U(vu0_clamp(x)); }           /* the same for a raw register (I, the caller's Q) */
MG_INLINE float fdivq(float s, float t)
{
    if ((B(t) & 0x7FFFFFFFu) == 0) return U(((B(s) ^ B(t)) & 0x80000000u) | 0x7F7FFFFFu);
    return s / t;
}
MG_INLINE float fsqrtq(float t) { return __builtin_sqrtf(U(B(t) & 0x7FFFFFFFu)); }
MG_INLINE float frsqrt(float s, float t)
{
    if ((B(t) & 0x7FFFFFFFu) == 0)
        return U((B(s) & 0x7FFFFFFFu) ? (((B(s) ^ B(t)) & 0x80000000u) | 0x7F7FFFFFu) : ((B(s) ^ B(t)) & 0x80000000u));
    return s / fsqrtq(t);
}
MG_INLINE u32 clipj(u32 x, u32 y, u32 z, u32 w)
{
    u32 a = (w & 0x7F800000u) ? (w & 0x7FFFFFFFu) : 0x007FFFFFu, r = 0;
    if ((x & 0x7FFFFFFFu) > a) r |= (x >> 31) ? 2u : 1u;
    if ((y & 0x7FFFFFFFu) > a) r |= (y >> 31) ? 8u : 4u;
    if ((z & 0x7FFFFFFFu) > a) r |= (z >> 31) ? 32u : 16u;
    return r;
}
MG_INLINE u32 macl(u32 r, int lane)
{
    u32 sh = 3u - (u32)lane;
    return (((r & 0x7FFFFFFFu) == 0) << sh) | ((r >> 31) << (4 + sh));
}

"""


def translate(code, entries, sym, mode="memory"):
    """microcode bytes, entry instruction indices, C name -> (C source, [(entry, instances)]).
    mode "memory": the VU0 register file stays in R5900Ctx (each lane read / written there, like the EE runtime's
    macro mode - cheap on the Allegrex's D-cache); "locals": every lane in a C local (loaded at the start, stored at
    the exits) - fewer memory operations, more live values."""
    prog = decode_program(code)
    return Emit(prog, sym, code, mode).file(entries)


# ---------------------------------------------------------------------------------------------- VIF0 chain
def extract_mpg(read, chain, limit=4096):
    """Follow a DMA source chain at `chain` (read(addr, n) -> bytes) and collect the MPG uploads of its VIF codes:
    returns micro memory bytes (as written). Supports CNT/NEXT/REF/REFE/END tags with the VIF codes in the tag
    (TTE) and in the data; NOP/STCYCL/OFFSET/BASE/ITOP/STMOD/MSKPATH3/MARK/FLUSH*/MSCNT/STMASK/STROW/STCOL/MPG/UNPACK."""
    mem = bytearray()
    addr = chain
    stream = bytearray()
    for _ in range(limit):
        tag = read(addr, 16)
        t0 = int.from_bytes(tag[0:8], "little")
        qwc, tid, nxt = t0 & 0xFFFF, (t0 >> 28) & 7, (t0 >> 32) & 0x7FFFFFF0
        stream += tag[8:16]                         # TTE: the upper 64 bits are VIF codes
        if tid == 1:                                # CNT: data follows the tag
            stream += read(addr + 16, 16 * qwc)
            addr = addr + 16 + 16 * qwc
        elif tid == 2:                              # NEXT
            stream += read(addr + 16, 16 * qwc)
            addr = nxt
        elif tid in (3, 4):                         # REF / REFS
            stream += read(nxt, 16 * qwc)
            addr += 16
        elif tid == 0:                              # REFE
            stream += read(nxt, 16 * qwc)
            break
        elif tid == 7:                              # END
            stream += read(addr + 16, 16 * qwc)
            break
        else:
            raise Unsupported("DMA tag id %d (CALL/RET) not supported" % tid)
    k = 0
    cl, wl = 1, 1
    while k + 4 <= len(stream):
        code = int.from_bytes(stream[k:k + 4], "little")
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        k += 4
        if cmd == 0x4A:                             # MPG: num (256 if 0) instructions at imm * 8
            n = (num or 256) * 8
            data = stream[k:k + n]
            if len(data) < n:
                raise Unsupported("MPG runs past the chain")
            end = imm * 8 + n
            if len(mem) < end:
                mem += bytes(end - len(mem))
            mem[imm * 8:end] = data
            k += n
        elif cmd in (0x30, 0x31):                   # STROW / STCOL
            k += 16
        elif cmd == 0x20:                           # STMASK
            k += 4
        elif cmd == 0x01:                           # STCYCL
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd >= 0x60:                           # UNPACK: skip its data
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bits = (32 >> vl) * (vn + 1)
            if cl < wl:
                n = cl * (n // wl) + min(n % wl, cl)
            k += ((bits * n + 31) // 32) * 4
        elif cmd in (0x50, 0x51):                   # DIRECT / DIRECTHL
            k += 16 * (imm or 65536)
    return bytes(mem)


# ---------------------------------------------------------------------------------------------- converter entry point
def generate(input_path, out_path, log=print, chain=None, entries=None, sym="vu0p", mode="memory", code_fnv64=None):
    """ps2rc.convert "python" generator: input = the game's EE ELF (with `chain`, the VIF0 DMA chain address) or a raw
    microcode file (no chain); entries = VCALLMS byte addresses; writes the C file to out_path. code_fnv64 (hex
    string): refuse microcode with another FNV-1a 64 (another game version)."""
    if chain is not None:
        from ..image import load_elf
        code = extract_mpg(load_elf(input_path).read, int(chain, 0) if isinstance(chain, str) else chain)
    else:
        code = open(input_path, "rb").read()
    if code_fnv64 is not None:
        h = 0xCBF29CE484222325
        for b in code:
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        if h != int(str(code_fnv64), 16):
            raise Unsupported("microcode FNV-1a 64 %016X, expected %s" % (h, code_fnv64))
    ents = []
    for e in entries or []:
        v = int(e, 0) if isinstance(e, str) else int(e)
        if v % 8:
            raise Unsupported("entry 0x%x is not a multiple of 8" % v)
        ents.append(v // 8)
    if not ents:
        raise Unsupported("no entries given")
    src, stats = translate(code, ents, sym, mode)
    with open(out_path, "w", newline=chr(10)) as f:
        f.write(src)
    log("micro_gen: %d bytes of microcode, %s -> %s" % (len(code), ", ".join("entry 0x%x: %d instances" % (8 * e, n)
                                                                         for e, n in stats), out_path))


# ---------------------------------------------------------------------------------------------- command line
def main(argv=None):
    """python -m ps2rc.vu.micro_gen (--elf FILE --chain ADDR | --code FILE) --entries A,B,... --sym NAME -o OUT.c
    [--mode memory|locals] [--listing FILE] [--save-code FILE]. Entries are VCALLMS byte addresses (imm15 * 8)."""
    import argparse
    ap = argparse.ArgumentParser(prog="python -m ps2rc.vu.micro_gen", description="VU0 microcode -> C translator")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--elf", help="EE ELF that holds the VIF0 DMA chain uploading the microcode")
    src.add_argument("--code", help="raw microcode bytes (micro memory image from address 0)")
    ap.add_argument("--chain", help="address of the DMA chain in the ELF")
    ap.add_argument("--entries", required=True, help="comma-separated VCALLMS byte addresses, e.g. 0x20,0x68")
    ap.add_argument("--sym", required=True, help="C name of the dispatcher (int SYM(R5900Ctx *, uint32_t))")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--mode", default="memory", choices=("memory", "locals"))
    ap.add_argument("--listing")
    ap.add_argument("--save-code")
    a = ap.parse_args(argv)
    if a.elf:
        if not a.chain:
            ap.error("--elf needs --chain")
        from ..image import load_elf
        img = load_elf(a.elf)
        code = extract_mpg(img.read, int(a.chain, 0))
    else:
        code = open(a.code, "rb").read()
    entries = []
    for e in a.entries.split(","):
        v = int(e, 0)
        if v % 8:
            ap.error("entry 0x%x is not a multiple of 8" % v)
        entries.append(v // 8)
    if a.save_code:
        open(a.save_code, "wb").write(code)
    if a.listing:
        with open(a.listing, "w") as f:
            for m in decode_program(code):
                f.write(text(m) + "\n")
    try:
        c_src, stats = translate(code, entries, a.sym, a.mode)
    except Unsupported as ex:
        print("cannot translate: %s" % ex)
        return 1
    with open(a.output, "w", newline="\n") as f:
        f.write(c_src)
    print("%d bytes of microcode, entries %s -> %s (%s)" % (len(code), ", ".join("0x%x: %d instances" % (8 * e, n)
                                                                                 for e, n in stats), a.output, a.mode))
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
