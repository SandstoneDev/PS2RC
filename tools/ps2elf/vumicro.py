"""VU micro-mode (VU0/VU1) 64-bit LIW decoder - classification level.

Tables: the upper / lower opcode fields as PCSX2 pcsx2/VUops.cpp decodes them (UpperTable / LowerOP tables).
 Microinstruction in memory:
word0 = lower, word1 = upper (little-endian 64-bit).
"""
_BC = 'xyzw'

UP = {}
for i, n in enumerate(['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']):
    for b in range(4):
        UP[i * 4 + b] = n + _BC[b]
UP.update({28: 'MULq', 29: 'MAXi', 30: 'MULi', 31: 'MINIi', 32: 'ADDq', 33: 'MADDq', 34: 'ADDi', 35: 'MADDi',
           36: 'SUBq', 37: 'MSUBq', 38: 'SUBi', 39: 'MSUBi', 40: 'ADD', 41: 'MADD', 42: 'MUL', 43: 'MAX',
           44: 'SUB', 45: 'MSUB', 46: 'OPMSUB', 47: 'MINI'})
UP2 = {}
for i, n in enumerate(['ADDA', 'SUBA', 'MADDA', 'MSUBA']):
    for b in range(4):
        UP2[i * 4 + b] = n + _BC[b]
for b in range(4):
    UP2[24 + b] = 'MULA' + _BC[b]
UP2.update({16: 'ITOF0', 17: 'ITOF4', 18: 'ITOF12', 19: 'ITOF15', 20: 'FTOI0', 21: 'FTOI4', 22: 'FTOI12',
            23: 'FTOI15', 28: 'MULAq', 29: 'ABS', 30: 'MULAi', 31: 'CLIP', 32: 'ADDAq', 33: 'MADDAq',
            34: 'ADDAi', 35: 'MADDAi', 36: 'SUBAq', 37: 'MSUBAq', 38: 'SUBAi', 39: 'MSUBAi', 40: 'ADDA',
            41: 'MADDA', 42: 'MULA', 44: 'SUBA', 45: 'MSUBA', 46: 'OPMULA', 47: 'NOP'})

LO = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ',
      0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR',
      0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR',
      0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
LOOP = {48: 'IADD', 49: 'ISUB', 50: 'IADDI', 52: 'IAND', 53: 'IOR'}
LOOP2 = {48: 'MOVE', 49: 'MR32', 52: 'LQI', 53: 'SQI', 54: 'LQD', 55: 'SQD', 56: 'DIV', 57: 'SQRT',
         58: 'RSQRT', 59: 'WAITQ', 60: 'MTIR', 61: 'MFIR', 62: 'ILWR', 63: 'ISWR', 64: 'RNEXT', 65: 'RGET',
         66: 'RINIT', 67: 'RXOR', 100: 'MFP', 104: 'XTOP', 105: 'XITOP', 108: 'XGKICK', 112: 'ESADD',
         113: 'ERSADD', 114: 'ELENG', 115: 'ERLENG', 116: 'EATANxy', 117: 'EATANxz', 118: 'ESUM',
         120: 'ESQRT', 121: 'ERSQRT', 122: 'ERCPR', 123: 'WAITP', 124: 'ESIN', 125: 'EATAN', 126: 'EEXP'}

UPPER_NOP = 0x000002FF
LOWER_NOP = 0x8000033C


def upper(w):
    f = w & 63
    if f >= 60:
        return UP2.get((((w >> 6) & 31) << 2) | (w & 3))
    return UP.get(f)


def upper_flags(w):
    return {'I': bool(w >> 31 & 1), 'E': bool(w >> 30 & 1), 'M': bool(w >> 29 & 1), 'D': bool(w >> 28 & 1),
            'T': bool(w >> 27 & 1)}


def lower(w):
    if w >> 31:
        if (w >> 25) != 0x40:
            return None
        f = w & 63
        if f >= 60:
            return LOOP2.get((((w >> 6) & 31) << 2) | (w & 3))
        return LOOP.get(f)
    return LO.get(w >> 25)


def decode_pair(lo, up):
    """Return (upper_mnem, lower_mnem_or_'LOI', flags) or None if invalid."""
    u = upper(up)
    if u is None:
        return None
    fl = upper_flags(up)
    if fl['I']:
        return u, 'LOI', fl
    l = lower(lo)
    if l is None:
        return None
    return u, l, fl
