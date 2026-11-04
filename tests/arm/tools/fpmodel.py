#!/usr/bin/env python3
"""An independent model of the floating point pseudocode of the Arm ARM (FPUnpack, FPRound, FPAdd,
FPMul, FPDiv, FPSqrt, FPMulAdd, FPMax / FPMin (and the Num forms), FPCompare, FPRoundInt,
FPToFixed, FixedToFP, FPConvert, FPRecipEstimate, FPRSqrtEstimate, FPRecipStep, FPRSqrtStep) for the
formats half, single, double and bfloat16, with exact rational arithmetic (fractions.Fraction). It
is the expected value generator of the unit rows of the AArch32 floating point instructions
(gen_semantic_rows_aarch32_vfp.py) which Unicorn cannot run (half precision, bfloat16): it does not
use dismount. The model is checked against Unicorn on the instructions Unicorn runs (single and
double: tests/arm/tools/check_fpmodel.py), so the half precision results are the same code with
the other format parameters.

The control state (class Ctl) is the FPSCR: the rounding mode, FZ (flush to zero of the operands
and the tiny results), DN (default NaN), AHP. The exception flags accumulate in a State (the FPSCR
cumulative bits IOC DZC OFC UFC IXC IDC). Tininess is detected before rounding and an input flush
raises IDC (ARM), but the half precision one.

Author: Elad Raz <e@eladraz.com>
"""
from fractions import Fraction

IOC, DZC, OFC, UFC, IXC, IDC = 0x01, 0x02, 0x04, 0x08, 0x10, 0x80


class Fmt(object):
    def __init__(self, name, e, f):
        self.name = name
        self.E = e
        self.F = f
        self.N = 1 + e + f
        self.bias = (1 << (e - 1)) - 1
        self.minexp = 2 - (1 << (e - 1))
        self.maxexp = (1 << e) - 1


HALF = Fmt('f16', 5, 10)
SINGLE = Fmt('f32', 8, 23)
DOUBLE = Fmt('f64', 11, 52)
BFLOAT = Fmt('bf16', 8, 7)


class Ctl(object):
    """The FPSCR controls. fz is the flush to zero for the formats other than half, fz16 for half."""

    def __init__(self, rmode='N', fz=False, dn=False, ahp=False, fz16=False):
        self.rmode = rmode
        self.fz = fz
        self.dn = dn
        self.ahp = ahp
        self.fz16 = fz16

    def flush(self, fmt):
        return self.fz16 if fmt is HALF else self.fz

    @staticmethod
    def from_fpscr(value):
        return Ctl(rmode='NPMZ'[(value >> 22) & 3], fz=bool(value >> 24 & 1), dn=bool(value >> 25 & 1),
                   ahp=bool(value >> 26 & 1), fz16=bool(value >> 19 & 1))

    @staticmethod
    def standard(ahp=False):
        """StandardFPSCRValue: round to nearest, FZ, DN, FZ16 clear."""
        return Ctl('N', fz=True, dn=True, ahp=ahp, fz16=False)


class State(object):
    def __init__(self, flags=0):
        self.flags = flags

    def raise_(self, flag):
        self.flags |= flag


# --------------------------------------------------------------------------------------------
# Encodings
# --------------------------------------------------------------------------------------------

def pack(fmt, sign, exp, frac):
    return (sign << (fmt.N - 1)) | (exp << fmt.F) | frac


def zero(fmt, sign):
    return pack(fmt, sign, 0, 0)


def infinity(fmt, sign):
    return pack(fmt, sign, fmt.maxexp, 0)


def max_normal(fmt, sign):
    return pack(fmt, sign, fmt.maxexp - 1, (1 << fmt.F) - 1)


def default_nan(fmt):
    return pack(fmt, 0, fmt.maxexp, 1 << (fmt.F - 1))


class Num(object):
    """kind: zero, finite, inf, qnan, snan; value: the exact signed value of a finite number."""

    def __init__(self, kind, sign, value=Fraction(0), bits=0):
        self.kind = kind
        self.sign = sign
        self.value = value
        self.bits = bits

    @property
    def is_nan(self):
        return self.kind in ('qnan', 'snan')


def unpack(fmt, bits, ctl, st, ahp_half=False):
    """FPUnpack: flushes the denormals if FZ (raising IDC)."""
    sign = (bits >> (fmt.N - 1)) & 1
    exp = (bits >> fmt.F) & ((1 << fmt.E) - 1)
    frac = bits & ((1 << fmt.F) - 1)
    if exp == fmt.maxexp and not ahp_half:
        if frac == 0:
            return Num('inf', sign, bits=bits)
        return Num('qnan' if (frac >> (fmt.F - 1)) & 1 else 'snan', sign, bits=bits)
    if exp == 0:
        if frac == 0:
            return Num('zero', sign, bits=bits)
        if ctl.flush(fmt):
            # (the half precision flush by FZ16 doesn't raise IDC: the Arm ARM FPUnpackBase,
            # confirmed on the M4 by the A64 corpora)
            if not (fmt.N == 16 and fmt.E == 5):
                st.raise_(IDC)
            return Num('zero', sign, bits=bits)
        value = Fraction(frac) * Fraction(2) ** (fmt.minexp - fmt.F)
    else:
        value = Fraction((1 << fmt.F) + frac) * Fraction(2) ** (exp - fmt.bias - fmt.F)
    return Num('finite', sign, -value if sign else value, bits)


def process_nan(fmt, num, ctl, st):
    bits = num.bits
    if num.kind == 'snan':
        bits |= 1 << (fmt.F - 1)
        st.raise_(IOC)
    if ctl.dn:
        return default_nan(fmt)
    return bits


def process_nans(fmt, nums, ctl, st):
    """FPProcessNaNs / FPProcessNaNs3: the first signaling NaN, else the first quiet NaN."""
    for n in nums:
        if n.kind == 'snan':
            return process_nan(fmt, n, ctl, st)
    for n in nums:
        if n.kind == 'qnan':
            return process_nan(fmt, n, ctl, st)
    return None


def floor_log2(value):
    """The e with 2^e <= value < 2^(e+1) of a positive Fraction."""
    n, d = value.numerator, value.denominator
    e = n.bit_length() - d.bit_length()
    if Fraction(2) ** e > value:
        e -= 1
    return e


def round_up_p(rmode, error, int_mant, sign):
    """Whether the rounding increments the magnitude. error is a Fraction in [0, 1)."""
    if rmode == 'N':
        return error > Fraction(1, 2) or (error == Fraction(1, 2) and (int_mant & 1) == 1)
    if rmode == 'P':
        return error != 0 and not sign
    if rmode == 'M':
        return error != 0 and sign
    if rmode == 'A':
        return error >= Fraction(1, 2)
    return False


def fp_round(fmt, ctl, st, value, rmode=None, ahp_out=False):
    """FPRound of a non zero exact value (FPRoundBase)."""
    rmode = rmode or ctl.rmode
    sign = 1 if value < 0 else 0
    mag = abs(value)
    exponent = floor_log2(mag)
    minexp = fmt.minexp
    # Flush to zero before rounding (tininess before rounding)
    if ctl.flush(fmt) and exponent < minexp:
        st.raise_(UFC)
        return zero(fmt, sign)
    biased = max(exponent - minexp + 1, 0)
    if biased == 0:
        exponent = minexp
    mant = mag / Fraction(2) ** exponent
    scaled = mant * (1 << fmt.F)
    int_mant = scaled.numerator // scaled.denominator
    error = scaled - int_mant
    if biased == 0 and error != 0:
        st.raise_(UFC)
    if rmode == 'O':
        up = False
        if error != 0:
            int_mant |= 1
    else:
        up = round_up_p(rmode, error, int_mant, sign)
    if up:
        int_mant += 1
        if int_mant == (1 << fmt.F):
            biased = 1
        if int_mant == (1 << (fmt.F + 1)):
            biased += 1
            int_mant >>= 1
    if ahp_out:
        if biased >= (1 << fmt.E):
            st.raise_(IOC)
            return pack(fmt, sign, (1 << fmt.E) - 1, (1 << fmt.F) - 1)
        result = pack(fmt, sign, biased, int_mant & ((1 << fmt.F) - 1))
    elif biased >= fmt.maxexp:
        if rmode == 'N' or rmode == 'A':
            to_inf = True
        elif rmode == 'P':
            to_inf = not sign
        elif rmode == 'M':
            to_inf = bool(sign)
        else:
            to_inf = False
        st.raise_(OFC)
        st.raise_(IXC)
        return infinity(fmt, sign) if to_inf else max_normal(fmt, sign)
    else:
        result = pack(fmt, sign, biased, int_mant & ((1 << fmt.F) - 1))
    if error != 0:
        st.raise_(IXC)
    return result


def exact_zero(fmt, ctl):
    return zero(fmt, 1 if ctl.rmode == 'M' else 0)


# --------------------------------------------------------------------------------------------
# Arithmetic
# --------------------------------------------------------------------------------------------

def fp_add(fmt, ctl, st, a, b, subtract=False):
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    r = process_nans(fmt, [n1, n2], ctl, st)
    if r is not None:
        return r
    s2 = n2.sign ^ (1 if subtract else 0)
    v2 = -n2.value if subtract else n2.value
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if inf1 and inf2 and n1.sign != s2:
        st.raise_(IOC)
        return default_nan(fmt)
    if (inf1 and n1.sign == 0) or (inf2 and s2 == 0):
        return infinity(fmt, 0)
    if (inf1 and n1.sign == 1) or (inf2 and s2 == 1):
        return infinity(fmt, 1)
    if zero1 and zero2 and n1.sign == s2:
        return zero(fmt, n1.sign)
    value = n1.value + v2
    if value == 0:
        return exact_zero(fmt, ctl)
    return fp_round(fmt, ctl, st, value)


def fp_sub(fmt, ctl, st, a, b):
    return fp_add(fmt, ctl, st, a, b, True)


def fp_mul(fmt, ctl, st, a, b):
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    r = process_nans(fmt, [n1, n2], ctl, st)
    if r is not None:
        return r
    sign = n1.sign ^ n2.sign
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if (inf1 and zero2) or (zero1 and inf2):
        st.raise_(IOC)
        return default_nan(fmt)
    if inf1 or inf2:
        return infinity(fmt, sign)
    if zero1 or zero2:
        return zero(fmt, sign)
    return fp_round(fmt, ctl, st, n1.value * n2.value)


def fp_div(fmt, ctl, st, a, b):
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    r = process_nans(fmt, [n1, n2], ctl, st)
    if r is not None:
        return r
    sign = n1.sign ^ n2.sign
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if (inf1 and inf2) or (zero1 and zero2):
        st.raise_(IOC)
        return default_nan(fmt)
    if inf1 or zero2:
        if not inf1:
            st.raise_(DZC)
        return infinity(fmt, sign)
    if zero1 or inf2:
        return zero(fmt, sign)
    return fp_round(fmt, ctl, st, n1.value / n2.value)


def isqrt_frac(value, bits):
    """A Fraction strictly inside the integer-spaced interval around sqrt(value), or the exact
    root. Returns (Fraction, exact)."""
    k = 2 * bits
    scaled = value * Fraction(4) ** k
    n = scaled.numerator // scaled.denominator
    import math
    r = math.isqrt(n)
    exact = (Fraction(r) ** 2 == scaled)
    if exact:
        return Fraction(r, 2 ** k), True
    return Fraction(2 * r + 1, 2 ** (k + 1)), False


def fp_sqrt(fmt, ctl, st, a):
    n = unpack(fmt, a, ctl, st)
    r = process_nans(fmt, [n], ctl, st)
    if r is not None:
        return r
    if n.kind == 'zero':
        return zero(fmt, n.sign)
    if n.kind == 'inf' and n.sign == 0:
        return infinity(fmt, 0)
    if n.sign == 1:
        st.raise_(IOC)
        return default_nan(fmt)
    root, exact = isqrt_frac(n.value, fmt.F + fmt.bias + 8)
    return fp_round(fmt, ctl, st, root)


def fp_muladd(fmt, ctl, st, addend, a, b):
    na = unpack(fmt, addend, ctl, st)
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if na.kind == 'qnan' and ((inf1 and zero2) or (zero1 and inf2)):
        st.raise_(IOC)
        return default_nan(fmt)
    r = process_nans(fmt, [na, n1, n2], ctl, st)
    if r is not None:
        return r
    sign_p = n1.sign ^ n2.sign
    inf_p = inf1 or inf2
    zero_p = zero1 or zero2
    if (inf1 and zero2) or (zero1 and inf2) or (na.kind == 'inf' and inf_p and na.sign != sign_p):
        st.raise_(IOC)
        return default_nan(fmt)
    if (na.kind == 'inf' and na.sign == 0) or (inf_p and sign_p == 0):
        return infinity(fmt, 0)
    if (na.kind == 'inf' and na.sign == 1) or (inf_p and sign_p == 1):
        return infinity(fmt, 1)
    if na.kind == 'zero' and zero_p and na.sign == sign_p:
        return zero(fmt, na.sign)
    value = na.value + (n1.value * n2.value if not zero_p else 0)
    if value == 0:
        return exact_zero(fmt, ctl)
    return fp_round(fmt, ctl, st, value)


def fp_neg(fmt, a):
    return a ^ (1 << (fmt.N - 1))


def fp_abs(fmt, a):
    return a & ~(1 << (fmt.N - 1))


def fp_max_min(fmt, ctl, st, a, b, is_max, num=False):
    """FPMax / FPMin (num: FPMaxNum / FPMinNum, a quiet NaN against a number gives the number)."""
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    if num:
        sign = 1 if is_max else 0
        if n1.kind == 'qnan' and n2.kind != 'qnan':
            n1 = Num('inf', sign, bits=infinity(fmt, sign))
        elif n2.kind == 'qnan' and n1.kind != 'qnan':
            n2 = Num('inf', sign, bits=infinity(fmt, sign))
    r = process_nans(fmt, [n1, n2], ctl, st)
    if r is not None:
        return r
    if n1.kind == 'zero' and n2.kind == 'zero' and n1.sign != n2.sign:
        return zero(fmt, 0 if is_max else 1)
    x = total_order(n1)
    y = total_order(n2)
    first = (x > y) if is_max else (x < y)
    return flushed_bits(fmt, n1 if first else n2)


def flushed_bits(fmt, num):
    """The operand as FPUnpack saw it: a flushed denormal is a zero of its sign."""
    if num.kind == 'zero':
        return zero(fmt, num.sign)
    return num.bits


def fp_compare(fmt, ctl, st, a, b, signal_nans):
    """FPCompare: the NZCV bits (as the 4 bit number)."""
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    if n1.is_nan or n2.is_nan:
        if n1.kind == 'snan' or n2.kind == 'snan' or signal_nans:
            st.raise_(IOC)
        return 0b0011
    x = total_order(n1)
    y = total_order(n2)
    if x == y:
        return 0b0110
    return 0b1000 if x < y else 0b0010


def fp_compare_op(fmt, ctl, st, a, b, kind):
    """FPCompareEQ / GE / GT: True / False."""
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    if n1.is_nan or n2.is_nan:
        if kind != 'eq' or n1.kind == 'snan' or n2.kind == 'snan':
            st.raise_(IOC)
        return False
    x = total_order(n1)
    y = total_order(n2)
    if kind == 'eq':
        return x == y
    if kind == 'ge':
        return x >= y
    return x > y


def total_order(n):
    if n.kind == 'inf':
        return Fraction(10) ** 400000 * (-1 if n.sign else 1)
    if n.kind == 'zero':
        return Fraction(0)
    return n.value


# --------------------------------------------------------------------------------------------
# Rounding to integers and conversions
# --------------------------------------------------------------------------------------------

def round_to_integer(value, rmode):
    """(the integer, error != 0) of a Fraction, by the rounding mode."""
    fl = value.numerator // value.denominator
    error = value - fl
    up = False
    if rmode == 'N':
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and (fl & 1) == 1)
    elif rmode == 'P':
        up = error != 0
    elif rmode == 'M':
        up = False
    elif rmode == 'Z':
        up = error != 0 and value < 0
    elif rmode == 'A':
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and value > 0)
    return fl + (1 if up else 0), error != 0


def fp_round_int(fmt, ctl, st, a, rmode, exact):
    n = unpack(fmt, a, ctl, st)
    r = process_nans(fmt, [n], ctl, st)
    if r is not None:
        return r
    if n.kind in ('inf', 'zero'):
        return flushed_bits(fmt, n) if n.kind == 'zero' else n.bits
    integer, inexact = round_to_integer(n.value, rmode)
    if integer == 0:
        result = zero(fmt, n.sign)
    else:
        result = fp_round(fmt, Ctl('N'), State(), Fraction(integer))
    if exact and inexact:
        st.raise_(IXC)
    return result


def fp_to_fixed(fmt, ctl, st, a, bits, fbits, unsigned, rmode):
    """FPToFixed: the integer (two's complement in `bits` bits)."""
    n = unpack(fmt, a, ctl, st)
    if n.is_nan:
        st.raise_(IOC)
        return 0
    if n.kind == 'zero':
        return 0
    if n.kind == 'inf':
        overflow = True
        negative = n.sign == 1
        integer = None
    else:
        integer, inexact = round_to_integer(n.value * (1 << fbits), rmode)
        if unsigned:
            lo, hi = 0, (1 << bits) - 1
        else:
            lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        overflow = integer < lo or integer > hi
        negative = integer < 0
    if overflow:
        st.raise_(IOC)
        if unsigned:
            return 0 if negative else (1 << bits) - 1
        return (1 << (bits - 1)) if negative else (1 << (bits - 1)) - 1
    if inexact:
        st.raise_(IXC)
    return integer & ((1 << bits) - 1)


def fixed_to_fp(fmt, ctl, st, integer, bits, fbits, unsigned, rmode=None):
    """FixedToFP: `integer` is the raw `bits` bit value."""
    integer &= (1 << bits) - 1
    if not unsigned and (integer >> (bits - 1)) & 1:
        integer -= 1 << bits
    if integer == 0:
        return zero(fmt, 0)
    return fp_round(fmt, ctl, st, Fraction(integer, 1 << fbits), rmode)


def fp_convert(src, dst, ctl, st, a, ahp_in=False, ahp_out=False, rmode=None):
    """FPConvert between the formats (the alternative half precision when asked)."""
    n = unpack(src, a, ctl, st, ahp_half=ahp_in)
    if n.is_nan or (ahp_out and n.kind == 'inf'):
        if ahp_out:
            st.raise_(IOC)
            if n.kind == 'inf':
                return pack(dst, n.sign, (1 << dst.E) - 1, (1 << dst.F) - 1)
            return zero(dst, n.sign)
        return process_nan_convert(src, dst, n, ctl, st)
    if n.kind == 'inf':
        return infinity(dst, n.sign)
    if n.kind == 'zero':
        return zero(dst, n.sign)
    return fp_round(dst, ctl, st, n.value, rmode, ahp_out=ahp_out)


def process_nan_convert(src, dst, n, ctl, st):
    if n.kind == 'snan':
        st.raise_(IOC)
    if ctl.dn:
        return default_nan(dst)
    # the fraction is truncated or extended, quieted
    frac = n.bits & ((1 << src.F) - 1)
    frac |= 1 << (src.F - 1)
    if dst.F >= src.F:
        frac <<= dst.F - src.F
    else:
        frac >>= src.F - dst.F
    return pack(dst, n.sign, dst.maxexp, frac)


# --------------------------------------------------------------------------------------------
# Reciprocal estimates
# --------------------------------------------------------------------------------------------

def recip_estimate_int(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def rsqrt_estimate_int(a):
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


def fp_recip_estimate(fmt, ctl, st, a):
    n = unpack(fmt, a, ctl, st)
    r = process_nans(fmt, [n], ctl, st)
    if r is not None:
        return r
    if n.kind == 'inf':
        return zero(fmt, n.sign)
    if n.kind == 'zero':
        st.raise_(DZC)
        return infinity(fmt, n.sign)
    if abs(n.value) < Fraction(2) ** (-(fmt.bias + 1)):
        rm = ctl.rmode
        to_inf = rm == 'N' or (rm == 'P' and n.sign == 0) or (rm == 'M' and n.sign == 1)
        st.raise_(OFC)
        st.raise_(IXC)
        return infinity(fmt, n.sign) if to_inf else max_normal(fmt, n.sign)
    if ctl.flush(fmt) and abs(n.value) >= Fraction(2) ** (fmt.bias - 1):
        st.raise_(UFC)
        return zero(fmt, n.sign)
    frac = (a & ((1 << fmt.F) - 1)) << (52 - fmt.F)
    exp = (a >> fmt.F) & ((1 << fmt.E) - 1)
    if exp == 0:
        if (frac >> 51) & 1 == 0:
            exp = -1
            frac = (frac << 2) & ((1 << 52) - 1)
        else:
            frac = (frac << 1) & ((1 << 52) - 1)
    scaled = (1 << 8) | (frac >> 44)
    base = {16: 29, 32: 253, 64: 2045}[fmt.N]
    result_exp = base - exp
    estimate = recip_estimate_int(scaled)
    frac = (estimate & 0xff) << 44
    if result_exp == 0:
        frac = (1 << 51) | (frac >> 1)
    elif result_exp == -1:
        frac = (1 << 50) | (frac >> 2)
        result_exp = 0
    return pack(fmt, n.sign, result_exp, frac >> (52 - fmt.F))


def fp_rsqrt_estimate(fmt, ctl, st, a):
    n = unpack(fmt, a, ctl, st)
    r = process_nans(fmt, [n], ctl, st)
    if r is not None:
        return r
    if n.kind == 'zero':
        st.raise_(DZC)
        return infinity(fmt, n.sign)
    if n.sign == 1:
        st.raise_(IOC)
        return default_nan(fmt)
    if n.kind == 'inf':
        return zero(fmt, 0)
    frac = (a & ((1 << fmt.F) - 1)) << (52 - fmt.F)
    exp = (a >> fmt.F) & ((1 << fmt.E) - 1)
    if exp == 0:
        while (frac >> 51) & 1 == 0:
            frac = (frac << 1) & ((1 << 52) - 1)
            exp -= 1
        frac = (frac << 1) & ((1 << 52) - 1)
    if exp & 1 == 0:
        scaled = (1 << 8) | (frac >> 44)
    else:
        scaled = (1 << 7) | (frac >> 45)
    base = {16: 44, 32: 380, 64: 3068}[fmt.N]
    result_exp = (base - exp) // 2
    estimate = rsqrt_estimate_int(scaled)
    frac = (estimate & 0xff) << 44
    return pack(fmt, 0, result_exp, frac >> (52 - fmt.F))


def fp_recip_step(fmt, ctl, st, a, b, sqrt=False):
    """FPRecipStep (AArch32, not fused): 2 - a * b; FPRSqrtStep: (3 - a * b) / 2."""
    n1 = unpack(fmt, a, ctl, st)
    n2 = unpack(fmt, b, ctl, st)
    r = process_nans(fmt, [n1, n2], ctl, st)
    if r is not None:
        return r
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    two = fp_from_float(fmt, 2.0)
    three = fp_from_float(fmt, 3.0)
    if (inf1 and zero2) or (zero1 and inf2):
        return fp_from_float(fmt, 1.5 if sqrt else 2.0)
    product = fp_mul(fmt, ctl, st, a, b)
    diff = fp_sub(fmt, ctl, st, three if sqrt else two, product)
    if sqrt:
        return fp_mul(fmt, ctl, st, diff, fp_from_float(fmt, 0.5))
    return diff


def fp_from_float(fmt, value):
    """The bits of an exactly representable Python float."""
    return fp_round(fmt, Ctl('N'), State(), Fraction(value)) if value != 0 else 0
