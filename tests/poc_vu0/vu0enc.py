"""Encoder for the R5900 COP2 (VU0 macro mode) instructions used by the VU0 tests.

GNU as (-march=r5900) does not encode every R5900 instruction correctly, so the test programs emit COP2 words with
.word. The opcode numbers below were written independently of ps2rc's decoder; tests/test_vu0_macro.py checks every
word against the objdump-validated decoder.

    asm("vmaddaz.xyz", "ACC", 4, 1)      -> word of vmaddaz.xyz $ACCxyz,$vf4xyz,$vf1z
Operand conventions (registers as ints): FMAC  fd|"ACC", fs, ft ; vmove/vmr32/vabs  ft, fs ; vrget/vrnext  ft ;
vrinit/vrxor  fs, field ; vcallms  byte address ; qmfc2/qmtc2/cfc2/ctc2 (".i" suffix = interlock)  rt, reg ;
lqc2/sqc2  ft, offset, base.
"""
COP2 = 0x12
FIELDS = "xyzw"

# SPECIAL1 (bits 5..0 < 60): name -> funct
SP1 = {}
for _k, _n in enumerate(("vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini", "vmul")):
    for _b, _f in enumerate(FIELDS):
        SP1[_n + _f] = 4 * _k + _b
SP1.update({"vmulq": 0x1C, "vmaxi": 0x1D, "vmuli": 0x1E, "vminii": 0x1F, "vaddq": 0x20, "vmaddq": 0x21,
            "vaddi": 0x22, "vmaddi": 0x23, "vsubq": 0x24, "vmsubq": 0x25, "vsubi": 0x26, "vmsubi": 0x27,
            "vadd": 0x28, "vmadd": 0x29, "vmul": 0x2A, "vmax": 0x2B, "vsub": 0x2C, "vmsub": 0x2D, "vopmsub": 0x2E,
            "vmini": 0x2F, "vcallms": 0x38, "vcallmsr": 0x39})
# SPECIAL2 (bits 5..2 = 1111): name -> 7-bit code = bits 10..6 : bits 1..0
SP2 = {}
for _k, _n in enumerate(("vadda", "vsuba", "vmadda", "vmsuba")):
    for _b, _f in enumerate(FIELDS):
        SP2[_n + _f] = 4 * _k + _b
for _b, _f in enumerate(FIELDS):
    SP2["vmula" + _f] = 0x18 + _b
SP2.update({"vmulaq": 0x1C, "vabs": 0x1D, "vmulai": 0x1E, "vaddaq": 0x20, "vmaddaq": 0x21, "vaddai": 0x22,
            "vmaddai": 0x23, "vsubaq": 0x24, "vmsubaq": 0x25, "vsubai": 0x26, "vmsubai": 0x27, "vadda": 0x28,
            "vmadda": 0x29, "vmula": 0x2A, "vsuba": 0x2C, "vmsuba": 0x2D, "vnop": 0x2F, "vmove": 0x30,
            "vmr32": 0x31, "vwaitq": 0x3B, "vrnext": 0x40, "vrget": 0x41, "vrinit": 0x42, "vrxor": 0x43})


def _dest(comps):
    return sum(1 << (3 - FIELDS.index(ch)) for ch in comps)


def _special1(funct, dest, ft, fs, fd):
    return (COP2 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


def _special2(code, dest, ft, fs):
    return (COP2 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((code >> 2) << 6) | 0x3C | (code & 3)


def asm(mnem, *ops):
    """32-bit word of one COP2 instruction (see the module docstring for the operand conventions)."""
    name, _, comps = mnem.partition(".")
    if name in ("qmfc2", "cfc2", "qmtc2", "ctc2"):
        rs = {"qmfc2": 1, "cfc2": 2, "qmtc2": 5, "ctc2": 6}[name]
        rt, reg = ops
        return (COP2 << 26) | (rs << 21) | (rt << 16) | (reg << 11) | (1 if comps == "i" else 0)
    if name in ("lqc2", "sqc2"):
        ft, off, base = ops
        return ((0x36 if name == "lqc2" else 0x3E) << 26) | (base << 21) | (ft << 16) | (off & 0xFFFF)
    if name == "vcallms":
        return _special1(SP1[name], 0, 0, 0, 0) | ((ops[0] >> 3) << 6)
    if name == "vcallmsr":                          # fs field = 27 (CMSAR0) by definition
        return _special1(SP1[name], 0, 0, 27, 0)
    if name in ("vrinit", "vrxor"):
        fs, field = ops
        return _special2(SP2[name], FIELDS.index(field), 0, fs)
    if name in ("vrget", "vrnext"):
        return _special2(SP2[name], _dest(comps), ops[0], 0)
    if name in ("vmove", "vmr32", "vabs"):
        ft, fs = ops
        return _special2(SP2[name], _dest(comps), ft, fs)
    if name in ("vnop", "vwaitq"):
        return _special2(SP2[name], 0, 0, 0)
    d, fs = ops[0], ops[1]
    ft = ops[2] if len(ops) > 2 else 0
    if d == "ACC":
        return _special2(SP2[name], _dest(comps), ft, fs)
    return _special1(SP1[name], _dest(comps), ft, fs, d)


def word(mnem, *ops):
    """Assembler line `.word 0x...  # text` for a guest .S file."""
    return ".word 0x%08x  # %s %s" % (asm(mnem, *ops), mnem, ",".join(str(o) for o in ops))
