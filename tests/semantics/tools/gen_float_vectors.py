#!/usr/bin/env python3
"""Generate the reference vectors of the soft float library.

Writes tests/semantics/float_vectors.inc, included by tests/TestSemanticFloat.cpp.

The expected results are computed with fractions.Fraction (exact arithmetic)
and rounded by an independent implementation of the five IEEE-754 rounding
modes plus round to odd. The environment is the x86 one: tininess is detected
after rounding, the default NaN is negative, the underflow exception is masked
(the flag is raised for a tiny inexact result), FTZ and DAZ are off.

Usage: python3 tests/semantics/tools/gen_float_vectors.py [--out FILE]
Deterministic: the seed is fixed.
"""
import argparse
import os
import random
import sys
from fractions import Fraction

# Operation codes (the same as in TestSemanticFloat.cpp)
ADD, SUB, MUL, DIV, SQRT, FMA, CVT, FROM_SIGNED, FROM_UNSIGNED, \
    TO_SIGNED, TO_UNSIGNED, ROUND_INT, ROUND_INT_EXACT = range(13)

# Float formats: code, exponent bits, fraction bits (without an integer bit),
# bias, precision, explicit integer bit
FORMATS = {
    0: ('single', 8, 23, 127, 24, False),
    1: ('double', 11, 52, 1023, 53, False),
    2: ('extended', 15, 63, 16383, 64, True),
    3: ('half', 5, 10, 15, 11, False),
    4: ('bfloat16', 8, 7, 127, 8, False),
}

# Rounding modes: nearest even, nearest away, zero, +inf, -inf, odd
MODES = [0, 1, 2, 3, 4, 5]

# Exceptions
INVALID, DENORMAL, DIVZERO, OVERFLOW, UNDERFLOW, INEXACT = 1, 2, 4, 8, 0x10, 0x20


class Num(object):
    """A decoded number: kind in zero, fin, inf, nan; value is the magnitude"""

    def __init__(self, kind, sign=0, value=Fraction(0), denormal=False):
        self.kind = kind
        self.sign = sign
        self.value = value
        self.denormal = denormal

    def signed(self):
        return -self.value if self.sign else self.value


def two(e):
    return Fraction(2) ** e


def fmt_info(f):
    return FORMATS[f]


def total_bits(f):
    name, eb, fb, bias, p, explicit = FORMATS[f]
    return 80 if explicit else 1 + eb + fb


def decode(f, lo, hi):
    name, eb, fb, bias, p, explicit = FORMATS[f]
    emin = 1 - bias
    if explicit:
        sign = (hi >> 15) & 1
        field = hi & 0x7FFF
        sig = lo
        if field == 0x7FFF:
            assert (sig >> 63) and (sig & ((1 << 63) - 1)) == 0, 'no nan inputs'
            return Num('inf', sign)
        if field == 0:
            assert (sig >> 63) == 0
            if sig == 0:
                return Num('zero', sign)
            return Num('fin', sign, Fraction(sig) * two(emin - 63), True)
        assert sig >> 63
        return Num('fin', sign, Fraction(sig) * two(field - bias - 63))
    bits = lo | (hi << 64)
    sign = (bits >> (eb + fb)) & 1
    field = (bits >> fb) & ((1 << eb) - 1)
    frac = bits & ((1 << fb) - 1)
    if field == (1 << eb) - 1:
        assert frac == 0, 'no nan inputs'
        return Num('inf', sign)
    if field == 0:
        if frac == 0:
            return Num('zero', sign)
        return Num('fin', sign, Fraction(frac) * two(emin - fb), True)
    return Num('fin', sign, Fraction((1 << fb) | frac) * two(field - bias - fb))


def pack(f, sign, field, frac, intbit=False):
    name, eb, fb, bias, p, explicit = FORMATS[f]
    if explicit:
        return ((frac | ((1 << 63) if intbit else 0)),
                (sign << 15) | field)
    bits = (sign << (eb + fb)) | (field << fb) | frac
    return (bits & ((1 << 64) - 1), bits >> 64)


def make_zero(f, sign):
    return pack(f, sign, 0, 0)


def make_inf(f, sign):
    name, eb, fb, bias, p, explicit = FORMATS[f]
    return pack(f, sign, (1 << eb) - 1, 0, True)


def make_default_nan(f):
    name, eb, fb, bias, p, explicit = FORMATS[f]
    return pack(f, 1, (1 << eb) - 1, 1 << (fb - 1), True)


def floor_log2(m):
    """floor(log2(m)) of a positive Fraction"""
    n, d = m.numerator, m.denominator
    e = n.bit_length() - d.bit_length()
    if two(e) > m:
        e -= 1
    return e


def round_int(q, mode, sign):
    """Round the non negative Fraction q to an integer. Return (int, inexact)"""
    fl = q.numerator // q.denominator
    rem = q - fl
    if rem == 0:
        return fl, False
    half = Fraction(1, 2)
    if mode == 0:
        if rem > half or (rem == half and (fl & 1)):
            fl += 1
    elif mode == 1:
        if rem >= half:
            fl += 1
    elif mode == 2:
        pass
    elif mode == 3:
        if not sign:
            fl += 1
    elif mode == 4:
        if sign:
            fl += 1
    elif mode == 5:
        if not (fl & 1):
            fl += 1
    return fl, True


def round_to_format(x, f, mode, precision=None):
    """Round the non zero Fraction x. Return ((lo, hi), flags)"""
    name, eb, fb, bias, p_full, explicit = FORMATS[f]
    p = precision or p_full
    emin, emax = 1 - bias, bias
    sign = 1 if x < 0 else 0
    m = abs(x)
    e = floor_log2(m)
    flags = 0
    unit_e = max(e, emin) - (p - 1)
    M, inexact = round_int(m / two(unit_e), mode, sign)
    # Tininess after rounding: round with an unbounded exponent range
    tiny = False
    if e < emin:
        mu, _ = round_int(m / two(e - (p - 1)), mode, sign)
        tiny = mu * two(e - (p - 1)) < two(emin)
    if inexact:
        flags |= INEXACT
        if tiny:
            flags |= UNDERFLOW
    if M == (1 << p):
        M >>= 1
        unit_e += 1
    if M >= (1 << (p - 1)):
        exp = unit_e + p - 1
        if exp > emax:
            flags |= OVERFLOW | INEXACT
            to_inf = {0: True, 1: True, 2: False, 3: not sign, 4: bool(sign),
                      5: False}[mode]
            if to_inf:
                return make_inf(f, sign), flags
            maxM = (1 << p) - 1
            return pack_finite(f, sign, emax, maxM, p), flags
        return pack_finite(f, sign, exp, M, p), flags
    # Denormal or zero
    if M == 0:
        return make_zero(f, sign), flags
    return pack_denormal(f, sign, M, p), flags


def pack_finite(f, sign, exp, M, p):
    name, eb, fb, bias, p_full, explicit = FORMATS[f]
    field = exp + bias
    if explicit:
        return pack(f, sign, field, M << (64 - p), True)
    return pack(f, sign, field, M & ((1 << (p - 1)) - 1))


def pack_denormal(f, sign, M, p):
    name, eb, fb, bias, p_full, explicit = FORMATS[f]
    if explicit:
        return pack(f, sign, 0, M << (64 - p))
    return pack(f, sign, 0, M)


def pack_negated(f, bits):
    lo, hi = bits
    name, eb, fb, bias, p, explicit = FORMATS[f]
    if explicit:
        return lo, hi ^ 0x8000
    v = (lo | (hi << 64)) ^ (1 << (eb + fb))
    return v & ((1 << 64) - 1), v >> 64


def exact_zero_sign(mode):
    return 1 if mode == 4 else 0


def denormal_flag(*nums):
    return DENORMAL if any(n.denormal for n in nums) else 0


def invalid_nan(f):
    return make_default_nan(f), INVALID


def arith(op, f, mode, ops, pc=None):
    """Return ((lo, hi), flags) of an arithmetic operation on decoded ops"""
    flags = denormal_flag(*ops)
    if op in (ADD, SUB):
        x, y = ops
        if op == SUB:
            y = Num(y.kind, 1 - y.sign, y.value, y.denormal)
        if x.kind == 'inf' or y.kind == 'inf':
            if x.kind == 'inf' and y.kind == 'inf' and x.sign != y.sign:
                return invalid_nan(f)
            return make_inf(f, x.sign if x.kind == 'inf' else y.sign), flags
        if x.kind == 'zero' and y.kind == 'zero':
            sign = x.sign if x.sign == y.sign else exact_zero_sign(mode)
            return make_zero(f, sign), flags
        s = x.signed() + y.signed()
        if s == 0:
            return make_zero(f, exact_zero_sign(mode)), flags
        r, fl = round_to_format(s, f, mode, pc)
        return r, flags | fl
    if op == MUL:
        x, y = ops
        sign = x.sign ^ y.sign
        if x.kind == 'inf' or y.kind == 'inf':
            if x.kind == 'zero' or y.kind == 'zero':
                return invalid_nan(f)
            return make_inf(f, sign), flags
        if x.kind == 'zero' or y.kind == 'zero':
            return make_zero(f, sign), flags
        r, fl = round_to_format(x.signed() * y.signed(), f, mode, pc)
        return r, flags | fl
    if op == DIV:
        x, y = ops
        sign = x.sign ^ y.sign
        if x.kind == 'inf':
            if y.kind == 'inf':
                return invalid_nan(f)
            return make_inf(f, sign), flags
        if y.kind == 'inf':
            return make_zero(f, sign), flags
        if y.kind == 'zero':
            # Detected before the denormal operand of the dividend: only the
            # divide by zero (or invalid) flag
            if x.kind == 'zero':
                return invalid_nan(f)
            return make_inf(f, sign), DIVZERO
        if x.kind == 'zero':
            return make_zero(f, sign), flags
        r, fl = round_to_format(x.signed() / y.signed(), f, mode, pc)
        return r, flags | fl
    if op == SQRT:
        x = ops[0]
        if x.kind == 'zero':
            return make_zero(f, x.sign), flags
        if x.sign:
            return invalid_nan(f)
        if x.kind == 'inf':
            return make_inf(f, 0), flags
        r, fl = round_to_format(exact_sqrt(x.value), f, mode, pc)
        return r, flags | fl
    if op == FMA:
        x, y, z = ops
        psign = x.sign ^ y.sign
        if (x.kind == 'inf' and y.kind == 'zero') or \
                (x.kind == 'zero' and y.kind == 'inf'):
            return invalid_nan(f)
        if x.kind == 'inf' or y.kind == 'inf':
            if z.kind == 'inf' and z.sign != psign:
                return invalid_nan(f)
            return make_inf(f, psign), flags
        if z.kind == 'inf':
            return make_inf(f, z.sign), flags
        prod = x.signed() * y.signed()
        pzero = x.kind == 'zero' or y.kind == 'zero'
        if pzero and z.kind == 'zero':
            sign = psign if psign == z.sign else exact_zero_sign(mode)
            return make_zero(f, sign), flags
        s = prod + z.signed()
        if s == 0:
            return make_zero(f, exact_zero_sign(mode)), flags
        r, fl = round_to_format(s, f, mode, pc)
        return r, flags | fl
    raise ValueError(op)


def exact_sqrt(m):
    """A Fraction which rounds like sqrt(m): exact when sqrt(m) is rational,
    else slightly above the truncated root (never on a rounding boundary)"""
    import math
    K = 200 + max(0, -floor_log2(m)) // 2 + 1
    scaled = m * (1 << (2 * K))
    n = scaled.numerator // scaled.denominator
    t = math.isqrt(n)
    if scaled.denominator == 1 and t * t == scaled.numerator:
        return Fraction(t, 1 << K)
    return Fraction(t, 1 << K) + Fraction(1, 1 << (K + 1))


def convert(f_from, f_to, mode, x):
    flags = denormal_flag(x)
    if x.kind == 'zero':
        return make_zero(f_to, x.sign), flags
    if x.kind == 'inf':
        return make_inf(f_to, x.sign), flags
    r, fl = round_to_format(x.signed(), f_to, mode)
    return r, flags | fl


def from_int(f, mode, value, bits, signed):
    if signed and value >= (1 << (bits - 1)):
        value -= 1 << bits
    if value == 0:
        return make_zero(f, 0), 0
    return round_to_format(Fraction(value), f, mode)


def to_int(f, mode, x, bits, signed):
    """Return (integer in its low bits, flags)"""
    mask = (1 << bits) - 1
    maxv = (mask >> 1) if signed else mask
    minv = -(maxv + 1) if signed else 0
    indefinite = (1 << (bits - 1)) if signed else mask
    # No denormal operand exception: the SDM lists the invalid and the
    # precision exceptions only (cvtss2si, cvttps2dq, fist...)
    flags = 0
    if x.kind == 'zero':
        return 0, flags
    if x.kind == 'inf':
        return indefinite, flags | INVALID
    q, inexact = round_int(x.value, mode, x.sign)
    v = -q if x.sign else q
    if v > maxv or v < minv:
        return indefinite, flags | INVALID
    return v & mask, flags | (INEXACT if inexact else 0)


def round_integral(f, mode, x, signal_inexact):
    flags = denormal_flag(x)
    if x.kind in ('zero', 'inf'):
        return (make_zero(f, x.sign) if x.kind == 'zero'
                else make_inf(f, x.sign)), flags
    q, inexact = round_int(x.value, mode, x.sign)
    if inexact and signal_inexact:
        flags |= INEXACT
    if q == 0:
        return make_zero(f, x.sign), flags
    r, _ = round_to_format(-Fraction(q) if x.sign else Fraction(q), f, mode)
    return r, flags


# ---------------------------------------------------------------- operands

def interesting(f, rng):
    """A random operand of the format as (lo, hi), not a NaN"""
    name, eb, fb, bias, p, explicit = FORMATS[f]
    kind = rng.randrange(12)
    sign = rng.randrange(2)
    maxfield = (1 << eb) - 1
    if kind == 0:
        return make_zero(f, sign)
    if kind == 1:
        return make_inf(f, sign)
    if explicit:
        def build(field, frac):
            return pack(f, sign, field, frac, field != 0)
    else:
        def build(field, frac):
            return pack(f, sign, field, frac)
    fracbits = fb
    frac = rng.getrandbits(fracbits)
    if kind == 2:                       # denormal
        if explicit:
            return pack(f, sign, 0, rng.getrandbits(63) | 1)
        return pack(f, sign, 0, rng.getrandbits(fracbits) | 1)
    if kind == 3:                       # tiny normal
        return build(rng.randrange(1, 4), frac)
    if kind == 4:                       # huge
        return build(maxfield - 1 - rng.randrange(3), frac)
    if kind == 5:                       # near one
        return build(bias - 2 + rng.randrange(5), frac)
    if kind == 6:                       # few bits set (exact results, ties)
        frac = rng.getrandbits(3) << (fracbits - 3)
        return build(bias - 4 + rng.randrange(9), frac)
    if kind == 7:                       # a power of two
        return build(rng.randrange(1, maxfield), 0)
    if kind == 8:                       # trailing bits (rounding ties)
        frac = (rng.getrandbits(2) << (fracbits - 2)) | rng.getrandbits(2)
        return build(bias - 3 + rng.randrange(7), frac)
    return build(rng.randrange(1, maxfield), frac)


def emit_hex(v):
    return '0x%X' % v if v else '0'


class Writer(object):
    def __init__(self):
        self.lines = []
        self.counts = {}

    def add(self, op, f, f2, mode, pc, flags, a, b, c, r):
        self.lines.append('    {%d,%d,%d,%d,%d,0x%X,%s,%s,%s,%s,%s,%s,%s,%s},' % (
            op, f, f2, mode, pc, flags,
            emit_hex(a[0]), emit_hex(a[1]), emit_hex(b[0]), emit_hex(b[1]),
            emit_hex(c[0]), emit_hex(c[1]), emit_hex(r[0]), emit_hex(r[1])))
        key = (op, f)
        self.counts[key] = self.counts.get(key, 0) + 1


def main():
    parser = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument('--out', default=os.path.join(here, '..',
                                                      'float_vectors.inc'))
    parser.add_argument('--count', type=int, default=16,
                        help='random cases per operation, format and mode')
    args = parser.parse_args()
    rng = random.Random(0x5EEDF10A7)
    w = Writer()
    zero = (0, 0)
    N = args.count

    # Arithmetic
    for f in (0, 1, 2):
        for op in (ADD, SUB, MUL, DIV, SQRT, FMA):
            for mode in MODES:
                for _ in range(N):
                    a = interesting(f, rng)
                    b = interesting(f, rng)
                    c = interesting(f, rng)
                    if op == FMA and rng.randrange(2):
                        # Make the cancellation likely: c = -(a * b) rounded
                        x, y = decode(f, *a), decode(f, *b)
                        if x.kind == 'fin' and y.kind == 'fin':
                            r, _ = round_to_format(-(x.signed() * y.signed()),
                                                   f, 0)
                            c = r
                    ops = [decode(f, *a), decode(f, *b), decode(f, *c)]
                    n = {SQRT: 1, FMA: 3}.get(op, 2)
                    r, fl = arith(op, f, mode, ops[:n])
                    w.add(op, f, 0, mode, 0, fl, a, b if n > 1 else zero,
                          c if n > 2 else zero, r)

    # Results around the smallest normal number and the overflow threshold:
    # a * b and a / b with b chosen so the exact result is a few units below
    # 2^emin (tininess after rounding) or 2^(emax + 1)
    for f in (0, 1, 2):
        name, eb, fb, bias, p, explicit = FORMATS[f]
        for mode in MODES:
            for _ in range(N):
                a = interesting(f, rng)
                x = decode(f, *a)
                if x.kind != 'fin':
                    continue
                if rng.randrange(2):
                    top = two(1 - bias)
                else:
                    top = two(bias + 1)
                target = top * (1 - Fraction(rng.randrange(1, 17), 1 << (p + 3)))
                if rng.randrange(3) == 0:
                    target = top * (1 + Fraction(rng.randrange(0, 17),
                                                 1 << (p + 3)))
                op = rng.choice((MUL, DIV))
                want = target / x.value if op == MUL else target * x.value
                if want >= two(bias + 1):
                    continue
                b, _ = round_to_format(want, f, 0)
                y = decode(f, *b)
                if y.kind != 'fin':
                    continue
                sign = rng.randrange(2)
                if sign:
                    b = pack_negated(f, b)
                    y = decode(f, *b)
                r, fl = arith(op, f, mode, [x, y])
                w.add(op, f, 0, mode, 0, fl, a, b, zero, r)

    # The x87 precision control on the extended format
    for pc in (24, 53):
        for op in (ADD, SUB, MUL, DIV, SQRT, FMA):
            for mode in MODES:
                for _ in range(N // 2):
                    a = interesting(2, rng)
                    b = interesting(2, rng)
                    c = interesting(2, rng)
                    ops = [decode(2, *a), decode(2, *b), decode(2, *c)]
                    n = {SQRT: 1, FMA: 3}.get(op, 2)
                    r, fl = arith(op, 2, mode, ops[:n], pc)
                    w.add(op, 2, 0, mode, pc, fl, a, b if n > 1 else zero,
                          c if n > 2 else zero, r)

    # Conversions between the formats
    for f_from in (0, 1, 2, 3, 4):
        for f_to in (0, 1, 2, 3, 4):
            if f_from == f_to:
                continue
            for mode in MODES:
                for _ in range(max(2, N // 2)):
                    a = interesting(f_from, rng)
                    r, fl = convert(f_from, f_to, mode, decode(f_from, *a))
                    w.add(CVT, f_from, f_to, mode, 0, fl, a, zero, zero, r)

    # Integer to float and float to integer
    for f in (0, 1, 2):
        for bits in (8, 16, 32, 64):
            for signed in (True, False):
                for mode in MODES:
                    for _ in range(max(2, N // 4)):
                        kind = rng.randrange(4)
                        if kind == 0:
                            v = rng.getrandbits(bits)
                        elif kind == 1:
                            v = rng.getrandbits(max(1, bits - 11)) << 11 \
                                if bits > 11 else rng.getrandbits(bits)
                            v &= (1 << bits) - 1
                        elif kind == 2:
                            v = (1 << bits) - 1 - rng.getrandbits(3)
                        else:
                            v = rng.getrandbits(bits // 2 or 1)
                        r, fl = from_int(f, mode, v, bits, signed)
                        w.add(FROM_SIGNED if signed else FROM_UNSIGNED, f,
                              bits, mode, 0, fl, (v, 0), zero, zero, r)
                    for _ in range(max(3, N // 2)):
                        # Values around the range of the integer
                        a = interesting(f, rng)
                        if rng.randrange(3) == 0:
                            e = rng.randrange(-3, bits + 2)
                            x = Fraction(rng.getrandbits(8) + 1, 64) * two(e)
                            if rng.randrange(2):
                                x = -x
                            a, _ = round_to_format(x, f, 0)
                        x = decode(f, *a)
                        v, fl = to_int(f, mode, x, bits, signed)
                        w.add(TO_SIGNED if signed else TO_UNSIGNED, f, bits,
                              mode, 0, fl, a, zero, zero, (v, 0))

    # Round to an integral value
    for f in (0, 1, 2):
        for mode in MODES:
            for exact in (0, 1):
                for _ in range(N):
                    a = interesting(f, rng)
                    if rng.randrange(2):
                        x = Fraction(rng.getrandbits(10) - 512, 4) / (
                            1 << rng.randrange(0, 4))
                        if x != 0:
                            a, _ = round_to_format(x, f, 0)
                    r, fl = round_integral(f, mode, decode(f, *a), exact)
                    w.add(ROUND_INT_EXACT if exact else ROUND_INT, f, 0, mode,
                          0, fl, a, zero, zero, r)

    with open(args.out, 'w') as out:
        out.write('// Generated by tests/semantics/tools/gen_float_vectors.py: '
                  'do not edit.\n')
        out.write('// {operation, format, format2 or integer bits, rounding '
                  'mode, x87 precision, flags,\n')
        out.write('//  a low, a high, b low, b high, c low, c high, result '
                  'low, result high}\n')
        out.write('\n'.join(w.lines))
        out.write('\n')
    print('%d vectors -> %s' % (len(w.lines), args.out))


if __name__ == '__main__':
    sys.exit(main())
