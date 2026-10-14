"""C back-end of the IOP static recompiler: IRX -> one C file that runs on the IOP runtime (ps2rc/iop/rt/iop_rt.h).

One guest function = one C function `void F_<addr>(IopCpu *c)`. Registers live in C locals inside a function and in
`c->r[]` across calls (every register the function writes is stored back before a call / return, every register it
uses is reloaded after a call), so the host compiler keeps them in machine registers. Branch delay slots are emitted in
order (the condition is computed before the slot runs). `jr ra` returns; `jal` calls; a `j` / branch to another
function's entry is a tail call; calls into other IOP modules go through the import table the runtime fills at load
time (IMP[k]). Memory is the runtime's IOP RAM image (`c->mem`, addresses masked with IOP_MEM_MASK). The module's
relocated text+data image is embedded; the runtime copies it to `base` and zeroes the bss.

The output is code translated from the module it was generated from: it is generated on the user's machine and must
not be distributed (see the header written into the file).
"""
import io

from .analysis import analyse

HI, LO = 32, 33


def _rn(n):
    if n == HI:
        return "hi"
    if n == LO:
        return "lo"
    return "r%d" % n


class _Regs:
    """Registers read / written by a function (for the local-variable cache)."""

    def __init__(self):
        self.r, self.w = set(), set()

    def use(self, i):
        for s in i.sources():
            if s:
                self.r.add(s)
        d = i.dest()
        if d:
            self.w.add(d)
        o = i.op
        if o in ("mult", "multu", "div", "divu", "mthi", "mtlo"):
            self.w.update((HI, LO) if o not in ("mthi", "mtlo") else ((HI,) if o == "mthi" else (LO,)))
        if o in ("mfhi", "mflo"):
            self.r.add(HI if o == "mfhi" else LO)
        if o == "jal" or o == "jalr" or o in ("bltzal", "bgezal"):
            self.w.add(31)
        if o in ("lwl", "lwr"):
            self.r.add(i.rt)


def _src(n):
    return "0u" if n == 0 else _rn(n)


def _imm(v):
    return "0x%xu" % (v & 0xFFFFFFFF)


CALLEE_SAVED = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31}


class Emitter:
    def __init__(self, prog, sym, abi=True):
        """abi: the module is compiled code that follows the MIPS calling convention (callees preserve s0-s8, gp, sp
        and return through ra): after a call only the caller-saved registers are reloaded. abi=False reloads all."""
        self.p = prog
        self.sym = sym
        self.abi = abi
        self.irx = prog.irx
        self.stub_index = {}
        k = 0
        for imp in self.irx.imports:
            for f in imp.fns:
                self.stub_index[self.irx.base + f.stub] = k
                k += 1
        self.nimports = k

    # ------------------------------------------------------------------ one instruction (no control transfer)
    def insn(self, i):
        o, rs, rt, rd = i.op, _src(i.rs), _src(i.rt), i.rd
        D = lambda n: _rn(n)
        ea = "%s + %s" % (rs, _imm(i.imm)) if i.imm else rs
        if o == "sll":
            if i.word == 0 or rd == 0:
                return ""
            return "%s = %s << %d;" % (D(rd), rt, i.sa)
        if rd == 0 and o in ("srl", "sra", "sllv", "srlv", "srav", "addu", "add", "subu", "sub", "and", "or", "xor",
                             "nor", "slt", "sltu", "mfhi", "mflo"):
            return ""
        if i.rt == 0 and o in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui", "lb", "lbu", "lh",
                               "lhu", "lw", "lwl", "lwr"):
            return ""
        if o == "srl":
            return "%s = %s >> %d;" % (D(rd), rt, i.sa)
        if o == "sra":
            return "%s = (uint32_t)((int32_t)%s >> %d);" % (D(rd), rt, i.sa)
        if o == "sllv":
            return "%s = %s << (%s & 31u);" % (D(rd), rt, rs)
        if o == "srlv":
            return "%s = %s >> (%s & 31u);" % (D(rd), rt, rs)
        if o == "srav":
            return "%s = (uint32_t)((int32_t)%s >> (%s & 31u));" % (D(rd), rt, rs)
        if o in ("addu", "add"):
            return "%s = %s + %s;" % (D(rd), rs, rt)
        if o in ("subu", "sub"):
            return "%s = %s - %s;" % (D(rd), rs, rt)
        if o == "and":
            return "%s = %s & %s;" % (D(rd), rs, rt)
        if o == "or":
            return "%s = %s | %s;" % (D(rd), rs, rt)
        if o == "xor":
            return "%s = %s ^ %s;" % (D(rd), rs, rt)
        if o == "nor":
            return "%s = ~(%s | %s);" % (D(rd), rs, rt)
        if o == "slt":
            return "%s = (int32_t)%s < (int32_t)%s;" % (D(rd), rs, rt)
        if o == "sltu":
            return "%s = %s < %s;" % (D(rd), rs, rt)
        if o in ("addiu", "addi"):
            return "%s = %s + %s;" % (D(i.rt), rs, _imm(i.imm))
        if o == "slti":
            return "%s = (int32_t)%s < %d;" % (D(i.rt), rs, i.imm)
        if o == "sltiu":
            return "%s = %s < %s;" % (D(i.rt), rs, _imm(i.imm))
        if o == "andi":
            return "%s = %s & %s;" % (D(i.rt), rs, _imm(i.imm))
        if o == "ori":
            return "%s = %s | %s;" % (D(i.rt), rs, _imm(i.imm))
        if o == "xori":
            return "%s = %s ^ %s;" % (D(i.rt), rs, _imm(i.imm))
        if o == "lui":
            return "%s = %s;" % (D(i.rt), _imm(i.imm << 16))
        if o == "mult":
            return "{ int64_t p_ = (int64_t)(int32_t)%s * (int32_t)%s; lo = (uint32_t)p_; hi = (uint32_t)((uint64_t)p_ >> 32); }" % (rs, rt)
        if o == "multu":
            return "{ uint64_t p_ = (uint64_t)%s * %s; lo = (uint32_t)p_; hi = (uint32_t)(p_ >> 32); }" % (rs, rt)
        if o == "div":
            return "IOP_DIV(hi, lo, %s, %s);" % (rs, rt)
        if o == "divu":
            return "IOP_DIVU(hi, lo, %s, %s);" % (rs, rt)
        if o == "mfhi":
            return "%s = hi;" % D(rd)
        if o == "mflo":
            return "%s = lo;" % D(rd)
        if o == "mthi":
            return "hi = %s;" % rs
        if o == "mtlo":
            return "lo = %s;" % rs
        if o == "lb":
            return "%s = (uint32_t)(int32_t)(int8_t)IOP_LB(M, %s);" % (D(i.rt), ea)
        if o == "lbu":
            return "%s = IOP_LB(M, %s);" % (D(i.rt), ea)
        if o == "lh":
            return "%s = (uint32_t)(int32_t)(int16_t)IOP_LH(M, %s);" % (D(i.rt), ea)
        if o == "lhu":
            return "%s = IOP_LH(M, %s);" % (D(i.rt), ea)
        if o == "lw":
            return "%s = IOP_LW(M, %s);" % (D(i.rt), ea)
        if o == "lwl":
            return "%s = iop_lwl(M, %s, %s);" % (D(i.rt), ea, rt)
        if o == "lwr":
            return "%s = iop_lwr(M, %s, %s);" % (D(i.rt), ea, rt)
        if o == "sb":
            return "IOP_SB(M, %s, %s);" % (ea, rt)
        if o == "sh":
            return "IOP_SH(M, %s, %s);" % (ea, rt)
        if o == "sw":
            return "IOP_SW(M, %s, %s);" % (ea, rt)
        if o == "swl":
            return "iop_swl(M, %s, %s);" % (ea, rt)
        if o == "swr":
            return "iop_swr(M, %s, %s);" % (ea, rt)
        if o in ("syscall", "break", "mfc0", "mtc0", "rfe"):
            return "SPILL iop_rt_trap(c, 0x%08xu, 0x%08xu); RELOAD" % (i.pc, i.word)
        raise ValueError("cannot translate %s at 0x%x" % (i.text, i.pc))

    # ------------------------------------------------------------------ one function
    def function(self, f, out):
        P = self.p
        regs = _Regs()
        for pc in range(f.entry, f.end, 4):
            regs.use(P.insns[pc])
        regs.w.discard(0)
        regs.r.discard(0)
        regs.w.add(29)                     # sp is always cached (cheap) - keeps the code uniform
        regs.r.add(29)
        local = sorted(regs.r | regs.w)
        spill = " ".join("c->r[%d] = %s;" % (n, _rn(n)) if n < 32 else "c->%s = %s;" % (_rn(n), _rn(n))
                         for n in sorted(regs.w))
        reload = " ".join("%s = c->r[%d];" % (_rn(n), n) if n < 32 else "%s = c->%s;" % (_rn(n), _rn(n))
                          for n in local if not (self.abi and n in CALLEE_SAVED))
        labels = set(f.labels)
        for pc in list(labels):                      # a label on a delay slot needs one on the next instruction
            if pc - 4 >= f.entry and P.insns[pc - 4].has_delay_slot:
                labels.add(pc + 4)
        w = out.write
        w("static void F_%08x(IopCpu *c)\n{\n" % f.entry)
        w("    uint8_t *const M = c->mem;\n")
        decl = [n for n in local if n < 32]
        if decl:
            w("    uint32_t %s;\n" % ", ".join("%s = c->r[%d]" % (_rn(n), n) for n in decl))
        if HI in local or LO in local:
            w("    uint32_t hi = c->hi, lo = c->lo;\n")
        w("    (void)M;\n")

        def emit_call(target, ret_pc, tail):
            s = []
            if target in self.stub_index:
                call = "IMP_%s[%d](c);" % (self.sym, self.stub_index[target])
            elif target in P.functions:
                call = "F_%08x(c);" % target
            else:
                raise ValueError("call to 0x%x: no function" % target)
            s.append(spill)
            s.append(call)
            if tail:
                s.append("return;")
            else:
                s.append(reload)
            return " ".join(x for x in s if x)

        pc = f.entry
        while pc < f.end:
            i = P.insns[pc]
            if pc in labels:
                w("L_%08x:;\n" % pc)
            if not i.has_delay_slot:
                s = self.insn(i)
                s = s.replace("SPILL", spill).replace("RELOAD", reload)
                w("    %s /* %08x: %s */\n" % (s, pc, i.text) if s else "    /* %08x: %s */\n" % (pc, i.text))
                pc += 4
                continue
            d = P.insns[pc + 4]
            ds = self.insn(d).replace("SPILL", spill).replace("RELOAD", reload)
            o = i.op
            w("    /* %08x: %s ; %s */\n" % (pc, i.text, d.text))
            if i.is_branch:
                rs, rt = _src(i.rs), _src(i.rt)
                cond = {"beq": "%s == %s" % (rs, rt), "bne": "%s != %s" % (rs, rt),
                        "blez": "(int32_t)%s <= 0" % rs, "bgtz": "(int32_t)%s > 0" % rs,
                        "bltz": "(int32_t)%s < 0" % rs, "bgez": "(int32_t)%s >= 0" % rs,
                        "bltzal": "(int32_t)%s < 0" % rs, "bgezal": "(int32_t)%s >= 0" % rs}[o]
                link = o in ("bltzal", "bgezal")
                inside = f.entry <= i.target < f.end
                always = o == "beq" and i.rs == 0 and i.rt == 0 or (o in ("bgez", "bgezal") and i.rs == 0)
                if link:
                    w("    { int t_ = %s; r31 = 0x%08xu; %s if (t_) { %s } }\n"
                      % (cond, pc + 8, ds, emit_call(i.target, pc + 8, False)))
                elif always:
                    if inside:
                        w("    %s goto L_%08x;\n" % (ds, i.target))
                    else:
                        w("    %s %s\n" % (ds, emit_call(i.target, 0, True)))
                else:
                    tgt = ("goto L_%08x;" % i.target) if inside else ("{ %s }" % emit_call(i.target, 0, True))
                    w("    { int t_ = %s; %s if (t_) %s }\n" % (cond, ds, tgt))
            elif o == "j":
                if f.entry <= i.target < f.end:
                    w("    %s goto L_%08x;\n" % (ds, i.target))
                else:
                    w("    %s %s\n" % (ds, emit_call(i.target, 0, True)))
            elif o == "jal":
                w("    r31 = 0x%08xu; %s %s\n" % (pc + 8, ds, emit_call(i.target, pc + 8, False)))
            elif o == "jr" and i.rs != 31:          # jump table (switch) or an indirect tail call
                cases = " ".join("case 0x%08xu: goto L_%08x;" % (t, t) for t in sorted(f.jtab))
                w("    { uint32_t t_ = %s; %s switch (t_) { %s default: %s iop_rt_call(c, t_); return; } }\n"
                  % (_src(i.rs), ds, cases, spill))
            elif o == "jr":
                w("    %s %s return;\n" % (ds, spill))
            elif o == "jalr":
                link = ("%s = 0x%08xu;" % (_rn(i.rd), pc + 8)) if i.rd else ""
                w("    { uint32_t t_ = %s; %s %s %s iop_rt_call(c, t_); %s }\n"
                  % (_src(i.rs), link, ds, spill, reload))
            else:
                raise ValueError(o)
            if pc + 4 in labels:                     # something branches into the delay slot
                w("    goto L_%08x;\nL_%08x:;\n    %s\n" % (pc + 8, pc + 4, ds))
            pc += 8
        if f.falls_through:
            w("    %s\n" % emit_call(f.end, 0, True))
        w("}\n\n")

    # ------------------------------------------------------------------ the file
    def file(self):
        P, x, sym = self.p, self.irx, self.sym
        out = io.StringIO()
        w = out.write
        w("/* %s.c - GENERATED by ps2rc.iop (IOP static recompiler) from the IOP module \"%s\" (md5 %s).\n"
          " * It contains code translated from that module: it is made on the user's machine from the user's own copy\n"
          " * and must not be distributed. Do not edit - regenerate. */\n" % (sym, x.name, x.md5))
        w("#include \"iop_rt.h\"\n\n")
        w("static IopFn IMP_%s[%d];\n" % (sym, max(1, self.nimports)))
        for e in P.entries:
            w("static void F_%08x(IopCpu *c);\n" % e)
        w("\n")
        for e in P.entries:
            self.function(P.functions[e], out)
        # image (text + data), relocated for `base`
        fsz = x.text_size + x.data_size
        words = [x.word(x.base + o) for o in range(0, (fsz + 3) & ~3, 4)]
        w("static const uint32_t IMAGE_%s[%d] = {\n" % (sym, len(words)))
        for k in range(0, len(words), 8):
            w("    " + ", ".join("0x%08xu" % v for v in words[k:k + 8]) + ",\n")
        w("};\n\n")
        w("static const IopImportDesc IMPORTS_%s[%d] = {\n" % (sym, max(1, self.nimports)))
        for imp in x.imports:
            for f in imp.fns:
                w("    {\"%s\", %d, 0x%08xu},\n" % (imp.lib, f.num, x.base + f.stub))
        if not self.nimports:
            w("    {0, 0, 0},\n")
        w("};\n\n")
        w("static IopFn fn_at_%s(uint32_t a)\n{\n    switch (a) {\n" % sym)
        for e in P.entries:
            w("    case 0x%08xu: return F_%08x;\n" % (e, e))
        w("    default: return 0;\n    }\n}\n\n")
        w("const IopModule %s = {\n" % sym)
        w("    \"%s\", 0x%04x, \"%s\",\n" % (x.name, x.version, x.md5))
        w("    0x%08xu, 0x%xu, 0x%xu, 0x%08xu, 0x%08xu,\n" % (x.base, x.image_size, fsz, x.base + x.entry,
                                                              x.base + x.gp))
        w("    IMAGE_%s, %d, IMPORTS_%s, IMP_%s, fn_at_%s,\n" % (sym, self.nimports, sym, sym, sym))
        w("};\n")
        return out.getvalue()


def generate(irx, sym, abi=True):
    """IRX -> (C source, Program). Raises ValueError when the module cannot be translated. abi: see Emitter."""
    prog = analyse(irx)
    errs = prog.errors()
    if errs:
        raise ValueError("cannot translate %s: %s" % (irx.name, "; ".join("0x%x %s" % (a, t) for _, a, t in errs[:8])))
    return Emitter(prog, sym, abi).file(), prog
