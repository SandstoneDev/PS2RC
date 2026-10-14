"""VU micro-mode instruction classification (the upper / lower opcode fields).

A microinstruction is 64 bits: lower word at the lower address, upper word above it. Upper bit 31 (I) means the
lower word is a float immediate (LOI). Used here to recognise microcode embedded in EE images.
"""
_BC = "xyzw"
UPPER = {}
for _i, _n in enumerate(["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]):
    for _b in range(4):
        UPPER[_i * 4 + _b] = _n + _BC[_b]
UPPER.update({28: "MULq", 29: "MAXi", 30: "MULi", 31: "MINIi", 32: "ADDq", 33: "MADDq", 34: "ADDi", 35: "MADDi",
              36: "SUBq", 37: "MSUBq", 38: "SUBi", 39: "MSUBi", 40: "ADD", 41: "MADD", 42: "MUL", 43: "MAX",
              44: "SUB", 45: "MSUB", 46: "OPMSUB", 47: "MINI"})
UPPER_SPECIAL = {}
for _i, _n in enumerate(["ADDA", "SUBA", "MADDA", "MSUBA"]):
    for _b in range(4):
        UPPER_SPECIAL[_i * 4 + _b] = _n + _BC[_b]
for _b in range(4):
    UPPER_SPECIAL[24 + _b] = "MULA" + _BC[_b]
UPPER_SPECIAL.update({16: "ITOF0", 17: "ITOF4", 18: "ITOF12", 19: "ITOF15", 20: "FTOI0", 21: "FTOI4", 22: "FTOI12",
                      23: "FTOI15", 28: "MULAq", 29: "ABS", 30: "MULAi", 31: "CLIP", 32: "ADDAq", 33: "MADDAq",
                      34: "ADDAi", 35: "MADDAi", 36: "SUBAq", 37: "MSUBAq", 38: "SUBAi", 39: "MSUBAi",
                      40: "ADDA", 41: "MADDA", 42: "MULA", 44: "SUBA", 45: "MSUBA", 46: "OPMULA", 47: "NOP"})
LOWER = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
         0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
         0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
         0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
LOWER_OP = {48: "IADD", 49: "ISUB", 50: "IADDI", 52: "IAND", 53: "IOR"}
LOWER_SPECIAL = {48: "MOVE", 49: "MR32", 52: "LQI", 53: "SQI", 54: "LQD", 55: "SQD", 56: "DIV", 57: "SQRT",
                 58: "RSQRT", 59: "WAITQ", 60: "MTIR", 61: "MFIR", 62: "ILWR", 63: "ISWR", 64: "RNEXT",
                 65: "RGET", 66: "RINIT", 67: "RXOR", 100: "MFP", 104: "XTOP", 105: "XITOP", 108: "XGKICK",
                 112: "ESADD", 113: "ERSADD", 114: "ELENG", 115: "ERLENG", 116: "EATANxy", 117: "EATANxz",
                 118: "ESUM", 120: "ESQRT", 121: "ERSQRT", 122: "ERCPR", 123: "WAITP", 124: "ESIN", 125: "EATAN",
                 126: "EEXP"}
UPPER_NOP = 0x000002FF
LOWER_NOP = 0x8000033C


def upper_name(w):
    f = w & 63
    if f >= 60:
        return UPPER_SPECIAL.get(((w >> 6) & 31) << 2 | (w & 3))
    return UPPER.get(f)


def lower_name(w):
    if w >> 31:
        if w >> 25 != 0x40:
            return None
        f = w & 63
        if f >= 60:
            return LOWER_SPECIAL.get(((w >> 6) & 31) << 2 | (w & 3))
        return LOWER_OP.get(f)
    return LOWER.get(w >> 25)


def valid_pair(lower, upper):
    """True when (lower, upper) is a valid microinstruction (lower is an immediate when upper has the I bit)."""
    if upper_name(upper) is None:
        return False
    return bool(upper >> 31) or lower_name(lower) is not None


def is_nop_pair(lower, upper):
    return (upper & 0x07FFFFFF) == UPPER_NOP and (lower == LOWER_NOP or upper >> 31)
