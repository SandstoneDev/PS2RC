"""Reference model of the VU0 macro-mode arithmetic, written independently of the C runtime (exact rationals).

Model (= PCSX2's EE-interpreter VU0 macro semantics with the "normal" VU clamp):
operands: exponent 0 -> signed zero, exponent 255 -> signed 0x7F7FFFFF; IEEE single-precision arithmetic rounded
toward zero, results below the smallest normal flushed to a signed zero; MADD/MSUB round the product first.
"""
import struct
from fractions import Fraction

FLT_MAX = 0x7F7FFFFF
SIGN = 0x80000000


def clamp_in(u):
    e = (u >> 23) & 0xFF
    if e == 0:
        return u & SIGN
    if e == 255:
        return (u & SIGN) | FLT_MAX
    return u


def value(u):
    """Exact value of a finite single (sign, Fraction); u must not have exponent 255."""
    s = -1 if u & SIGN else 1
    e = (u >> 23) & 0xFF
    m = u & 0x7FFFFF
    if e == 0:
        return s, Fraction(m, 1 << 149)
    return s, Fraction(m | 0x800000) * Fraction(2) ** (e - 150)


def to_bits(neg, mag):
    """Round the magnitude toward zero to single precision; overflow -> FLT_MAX, below 2^-126 -> zero."""
    sign = SIGN if neg else 0
    if mag == 0:
        return sign
    if mag >= Fraction(2) ** 128:
        return sign | FLT_MAX
    if mag < Fraction(2) ** -126:
        return sign
    e = mag.numerator.bit_length() - mag.denominator.bit_length()       # 2^e <= mag < 2^(e+2)
    if Fraction(2) ** e > mag:
        e -= 1
    while Fraction(2) ** (e + 1) <= mag:
        e += 1
    mant = int(mag / Fraction(2) ** (e - 23))                          # truncation = toward zero
    return sign | ((e + 127) << 23) | (mant & 0x7FFFFF)


def _sv(u):
    s, v = value(u)
    return v if s > 0 else -v


def add(a, b):
    a, b = clamp_in(a), clamp_in(b)
    r = _sv(a) + _sv(b)
    if r == 0:                                    # exact zero: -0 only for (-0) + (-0) (round toward zero)
        return SIGN if (a & SIGN and b & SIGN) else 0
    return to_bits(r < 0, abs(r))


def sub(a, b):
    return add(a, clamp_in(b) ^ SIGN)


def mul(a, b):
    a, b = clamp_in(a), clamp_in(b)
    neg = bool((a ^ b) & SIGN)
    return to_bits(neg, abs(_sv(a) * _sv(b)))


def madd(acc, a, b):
    return add(clamp_in(acc), mul(a, b))


def msub(acc, a, b):
    return sub(clamp_in(acc), mul(a, b))


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def rnext(r):
    """R register LFSR step (PCSX2 AdvanceLFSR)."""
    x, y = (r >> 4) & 1, (r >> 22) & 1
    return (((r << 1) ^ x ^ y) & 0x7FFFFF) | 0x3F800000
