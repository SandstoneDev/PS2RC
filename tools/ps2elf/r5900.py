"""R5900 (PS2 Emotion Engine core) instruction decoder - classification level.

Opcode tables: the R5900 opcode maps, cross-checked against PCSX2 pcsx2/R5900OpcodeTables.cpp.

Purpose: instruction census / static analysis, not cycle-accurate emulation.

decode(word) -> (mnemonic, group) where group is one of
  CPU, CPU64, BRANCH, BRANCHL, JUMP, LOADSTORE, LS64, LS128, MMI, COP0, FPU, VU0M, SYS, INVALID
"""

def _bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)

PRIMARY = {
    2: ('J', 'JUMP'), 3: ('JAL', 'JUMP'),
    4: ('BEQ', 'BRANCH'), 5: ('BNE', 'BRANCH'), 6: ('BLEZ', 'BRANCH'), 7: ('BGTZ', 'BRANCH'),
    8: ('ADDI', 'CPU'), 9: ('ADDIU', 'CPU'), 10: ('SLTI', 'CPU'), 11: ('SLTIU', 'CPU'),
    12: ('ANDI', 'CPU'), 13: ('ORI', 'CPU'), 14: ('XORI', 'CPU'), 15: ('LUI', 'CPU'),
    20: ('BEQL', 'BRANCHL'), 21: ('BNEL', 'BRANCHL'), 22: ('BLEZL', 'BRANCHL'), 23: ('BGTZL', 'BRANCHL'),
    24: ('DADDI', 'CPU64'), 25: ('DADDIU', 'CPU64'), 26: ('LDL', 'LS64'), 27: ('LDR', 'LS64'),
    30: ('LQ', 'LS128'), 31: ('SQ', 'LS128'),
    32: ('LB', 'LOADSTORE'), 33: ('LH', 'LOADSTORE'), 34: ('LWL', 'LOADSTORE'), 35: ('LW', 'LOADSTORE'),
    36: ('LBU', 'LOADSTORE'), 37: ('LHU', 'LOADSTORE'), 38: ('LWR', 'LOADSTORE'), 39: ('LWU', 'LS64'),
    40: ('SB', 'LOADSTORE'), 41: ('SH', 'LOADSTORE'), 42: ('SWL', 'LOADSTORE'), 43: ('SW', 'LOADSTORE'),
    44: ('SDL', 'LS64'), 45: ('SDR', 'LS64'), 46: ('SWR', 'LOADSTORE'), 47: ('CACHE', 'SYS'),
    49: ('LWC1', 'FPU'), 51: ('PREF', 'SYS'), 54: ('LQC2', 'VU0M'), 55: ('LD', 'LS64'),
    57: ('SWC1', 'FPU'), 62: ('SQC2', 'VU0M'), 63: ('SD', 'LS64'),
}

SPECIAL = {
    0: ('SLL', 'CPU'), 2: ('SRL', 'CPU'), 3: ('SRA', 'CPU'), 4: ('SLLV', 'CPU'), 6: ('SRLV', 'CPU'),
    7: ('SRAV', 'CPU'), 8: ('JR', 'JUMP'), 9: ('JALR', 'JUMP'), 10: ('MOVZ', 'CPU'), 11: ('MOVN', 'CPU'),
    12: ('SYSCALL', 'SYS'), 13: ('BREAK', 'SYS'), 15: ('SYNC', 'SYS'),
    16: ('MFHI', 'CPU'), 17: ('MTHI', 'CPU'), 18: ('MFLO', 'CPU'), 19: ('MTLO', 'CPU'),
    20: ('DSLLV', 'CPU64'), 22: ('DSRLV', 'CPU64'), 23: ('DSRAV', 'CPU64'),
    24: ('MULT', 'CPU'), 25: ('MULTU', 'CPU'), 26: ('DIV', 'CPU'), 27: ('DIVU', 'CPU'),
    32: ('ADD', 'CPU'), 33: ('ADDU', 'CPU'), 34: ('SUB', 'CPU'), 35: ('SUBU', 'CPU'),
    36: ('AND', 'CPU'), 37: ('OR', 'CPU'), 38: ('XOR', 'CPU'), 39: ('NOR', 'CPU'),
    40: ('MFSA', 'MMI'), 41: ('MTSA', 'MMI'), 42: ('SLT', 'CPU'), 43: ('SLTU', 'CPU'),
    44: ('DADD', 'CPU64'), 45: ('DADDU', 'CPU64'), 46: ('DSUB', 'CPU64'), 47: ('DSUBU', 'CPU64'),
    48: ('TGE', 'SYS'), 49: ('TGEU', 'SYS'), 50: ('TLT', 'SYS'), 51: ('TLTU', 'SYS'), 52: ('TEQ', 'SYS'), 54: ('TNE', 'SYS'),
    56: ('DSLL', 'CPU64'), 58: ('DSRL', 'CPU64'), 59: ('DSRA', 'CPU64'),
    60: ('DSLL32', 'CPU64'), 62: ('DSRL32', 'CPU64'), 63: ('DSRA32', 'CPU64'),
}

REGIMM = {
    0: ('BLTZ', 'BRANCH'), 1: ('BGEZ', 'BRANCH'), 2: ('BLTZL', 'BRANCHL'), 3: ('BGEZL', 'BRANCHL'),
    8: ('TGEI', 'SYS'), 9: ('TGEIU', 'SYS'), 10: ('TLTI', 'SYS'), 11: ('TLTIU', 'SYS'), 12: ('TEQI', 'SYS'), 14: ('TNEI', 'SYS'),
    16: ('BLTZAL', 'BRANCH'), 17: ('BGEZAL', 'BRANCH'), 18: ('BLTZALL', 'BRANCHL'), 19: ('BGEZALL', 'BRANCHL'),
    24: ('MTSAB', 'MMI'), 25: ('MTSAH', 'MMI'),
}

MMI = {
    0: 'MADD', 1: 'MADDU', 4: 'PLZCW', 16: 'MFHI1', 17: 'MTHI1', 18: 'MFLO1', 19: 'MTLO1',
    24: 'MULT1', 25: 'MULTU1', 26: 'DIV1', 27: 'DIVU1', 32: 'MADD1', 33: 'MADDU1',
    52: 'PSLLH', 54: 'PSRLH', 55: 'PSRAH', 60: 'PSLLW', 62: 'PSRLW', 63: 'PSRAW',
}
MMI0 = {0: 'PADDW', 1: 'PSUBW', 2: 'PCGTW', 3: 'PMAXW', 4: 'PADDH', 5: 'PSUBH', 6: 'PCGTH', 7: 'PMAXH',
        8: 'PADDB', 9: 'PSUBB', 10: 'PCGTB', 16: 'PADDSW', 17: 'PSUBSW', 18: 'PEXTLW', 19: 'PPACW',
        20: 'PADDSH', 21: 'PSUBSH', 22: 'PEXTLH', 23: 'PPACH', 24: 'PADDSB', 25: 'PSUBSB', 26: 'PEXTLB',
        27: 'PPACB', 30: 'PEXT5', 31: 'PPAC5'}
MMI1 = {1: 'PABSW', 2: 'PCEQW', 3: 'PMINW', 4: 'PADSBH', 5: 'PABSH', 6: 'PCEQH', 7: 'PMINH', 10: 'PCEQB',
        16: 'PADDUW', 17: 'PSUBUW', 18: 'PEXTUW', 20: 'PADDUH', 21: 'PSUBUH', 22: 'PEXTUH',
        24: 'PADDUB', 25: 'PSUBUB', 26: 'PEXTUB', 27: 'QFSRV'}
MMI2 = {0: 'PMADDW', 2: 'PSLLVW', 3: 'PSRLVW', 4: 'PMSUBW', 8: 'PMFHI', 9: 'PMFLO', 10: 'PINTH',
        12: 'PMULTW', 13: 'PDIVW', 14: 'PCPYLD', 16: 'PMADDH', 17: 'PHMADH', 18: 'PAND', 19: 'PXOR',
        20: 'PMSUBH', 21: 'PHMSBH', 26: 'PEXEH', 27: 'PREVH', 28: 'PMULTH', 29: 'PDIVBW', 30: 'PEXEW', 31: 'PROT3W'}
MMI3 = {0: 'PMADDUW', 3: 'PSRAVW', 8: 'PMTHI', 9: 'PMTLO', 10: 'PINTEH', 12: 'PMULTUW', 13: 'PDIVUW',
        14: 'PCPYUD', 18: 'POR', 19: 'PNOR', 26: 'PEXCH', 27: 'PCPYH', 30: 'PEXCW'}
PMFHL = {0: 'PMFHL.LW', 1: 'PMFHL.UW', 2: 'PMFHL.SLW', 3: 'PMFHL.LH', 4: 'PMFHL.SH'}

COP0_C0 = {1: 'TLBR', 2: 'TLBWI', 6: 'TLBWR', 8: 'TLBP', 24: 'ERET', 56: 'EI', 57: 'DI'}

FPU_S = {0: 'ADD.S', 1: 'SUB.S', 2: 'MUL.S', 3: 'DIV.S', 4: 'SQRT.S', 5: 'ABS.S', 6: 'MOV.S', 7: 'NEG.S',
         22: 'RSQRT.S', 24: 'ADDA.S', 25: 'SUBA.S', 26: 'MULA.S', 28: 'MADD.S', 29: 'MSUB.S',
         30: 'MADDA.S', 31: 'MSUBA.S', 36: 'CVT.W.S', 40: 'MAX.S', 41: 'MIN.S',
         48: 'C.F.S', 50: 'C.EQ.S', 52: 'C.LT.S', 54: 'C.LE.S'}

_BC = ['x', 'y', 'z', 'w']
VU_SP1 = {}
for i, n in enumerate(['VADD', 'VSUB', 'VMADD', 'VMSUB', 'VMAX', 'VMINI', 'VMUL']):
    for b in range(4):
        VU_SP1[i * 4 + b] = n + _BC[b]
VU_SP1.update({28: 'VMULq', 29: 'VMAXi', 30: 'VMULi', 31: 'VMINIi', 32: 'VADDq', 33: 'VMADDq', 34: 'VADDi',
               35: 'VMADDi', 36: 'VSUBq', 37: 'VMSUBq', 38: 'VSUBi', 39: 'VMSUBi', 40: 'VADD', 41: 'VMADD',
               42: 'VMUL', 43: 'VMAX', 44: 'VSUB', 45: 'VMSUB', 46: 'VOPMSUB', 47: 'VMINI', 48: 'VIADD',
               49: 'VISUB', 50: 'VIADDI', 52: 'VIAND', 53: 'VIOR', 56: 'VCALLMS', 57: 'VCALLMSR'})
VU_SP2 = {}
for i, n in enumerate(['VADDA', 'VSUBA', 'VMADDA', 'VMSUBA']):
    for b in range(4):
        VU_SP2[i * 4 + b] = n + _BC[b]
for b in range(4):
    VU_SP2[24 + b] = 'VMULA' + _BC[b]
VU_SP2.update({16: 'VITOF0', 17: 'VITOF4', 18: 'VITOF12', 19: 'VITOF15', 20: 'VFTOI0', 21: 'VFTOI4',
               22: 'VFTOI12', 23: 'VFTOI15', 28: 'VMULAq', 29: 'VABS', 30: 'VMULAi', 31: 'VCLIPw',
               32: 'VADDAq', 33: 'VMADDAq', 34: 'VADDAi', 35: 'VMADDAi', 36: 'VSUBAq', 37: 'VMSUBAq',
               38: 'VSUBAi', 39: 'VMSUBAi', 40: 'VADDA', 41: 'VMADDA', 42: 'VMULA', 44: 'VSUBA',
               45: 'VMSUBA', 46: 'VOPMULA', 47: 'VNOP', 48: 'VMOVE', 49: 'VMR32', 52: 'VLQI', 53: 'VSQI',
               54: 'VLQD', 55: 'VSQD', 56: 'VDIV', 57: 'VSQRT', 58: 'VRSQRT', 59: 'VWAITQ', 60: 'VMTIR',
               61: 'VMFIR', 62: 'VILWR', 63: 'VISWR', 64: 'VRNEXT', 65: 'VRGET', 66: 'VRINIT', 67: 'VRXOR'})

INVALID = ('INVALID', 'INVALID')


def decode(w):
    op = w >> 26
    if op == 0:
        if w == 0:
            return ('NOP', 'CPU')
        return SPECIAL.get(w & 63, INVALID)
    if op == 1:
        return REGIMM.get(_bits(w, 20, 16), INVALID)
    if op == 28:  # MMI
        f = w & 63
        sa = _bits(w, 10, 6)
        if f == 8:
            n = MMI0.get(sa)
        elif f == 40:
            n = MMI1.get(sa)
        elif f == 9:
            n = MMI2.get(sa)
        elif f == 41:
            n = MMI3.get(sa)
        elif f == 48:
            n = PMFHL.get(sa)
        elif f == 49:
            n = 'PMTHL.LW' if sa == 0 else None
        else:
            n = MMI.get(f)
        return (n, 'MMI') if n else INVALID
    if op == 16:  # COP0
        rs = _bits(w, 25, 21)
        if rs == 0:
            rd = _bits(w, 15, 11)
            if rd == 25:
                return ('MFPS/MFPC', 'COP0')
            if rd == 24:
                return ('MFBPC/MFDAB..', 'COP0')
            return ('MFC0', 'COP0')
        if rs == 4:
            rd = _bits(w, 15, 11)
            if rd == 25:
                return ('MTPS/MTPC', 'COP0')
            if rd == 24:
                return ('MTBPC/MTDAB..', 'COP0')
            return ('MTC0', 'COP0')
        if rs == 8:
            return ({0: 'BC0F', 1: 'BC0T', 2: 'BC0FL', 3: 'BC0TL'}.get(_bits(w, 20, 16), 'INVALID'), 'COP0')
        if rs == 16:
            n = COP0_C0.get(w & 63)
            return (n, 'COP0') if n else INVALID
        return INVALID
    if op == 17:  # COP1 FPU
        rs = _bits(w, 25, 21)
        if rs == 0: return ('MFC1', 'FPU')
        if rs == 2: return ('CFC1', 'FPU')
        if rs == 4: return ('MTC1', 'FPU')
        if rs == 6: return ('CTC1', 'FPU')
        if rs == 8:
            return ({0: 'BC1F', 1: 'BC1T', 2: 'BC1FL', 3: 'BC1TL'}.get(_bits(w, 20, 16), 'INVALID'), 'FPU')
        if rs == 16:
            n = FPU_S.get(w & 63)
            return (n, 'FPU') if n else INVALID
        if rs == 20:
            return ('CVT.S.W', 'FPU') if (w & 63) == 32 else INVALID
        return INVALID
    if op == 18:  # COP2 = VU0 macro mode
        rs = _bits(w, 25, 21)
        if rs == 1: return ('QMFC2', 'VU0M')
        if rs == 2: return ('CFC2', 'VU0M')
        if rs == 5: return ('QMTC2', 'VU0M')
        if rs == 6: return ('CTC2', 'VU0M')
        if rs == 8:
            return ({0: 'BC2F', 1: 'BC2T', 2: 'BC2FL', 3: 'BC2TL'}.get(_bits(w, 20, 16), 'INVALID'), 'VU0M')
        if rs >= 16:
            f = w & 63
            if f >= 60:
                n = VU_SP2.get((_bits(w, 10, 6) << 2) | (w & 3))
            else:
                n = VU_SP1.get(f)
            return (n, 'VU0M') if n else INVALID
        return INVALID
    return PRIMARY.get(op, INVALID)


def branch_target(w, pc):
    """Target of PC-relative branch (incl. likely), else None."""
    op = w >> 26
    m = decode(w)[0]
    if m in ('BEQ', 'BNE', 'BLEZ', 'BGTZ', 'BEQL', 'BNEL', 'BLEZL', 'BGTZL') or op == 1 and m.startswith('B') \
            or m in ('BC0F', 'BC0T', 'BC0FL', 'BC0TL', 'BC1F', 'BC1T', 'BC1FL', 'BC1TL', 'BC2F', 'BC2T', 'BC2FL', 'BC2TL'):
        off = w & 0xFFFF
        if off & 0x8000:
            off -= 0x10000
        return (pc + 4 + (off << 2)) & 0xFFFFFFFF
    return None


def jump_target(w, pc):
    if (w >> 26) in (2, 3):
        return ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    return None
