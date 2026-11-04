#!/usr/bin/env python3
"""An independent model of the floating point pseudocode of the Arm ARM (FPUnpack, FPRound, FPProcessNaNs,
FPAdd / FPMul / FPMulAdd / FPDiv / FPSqrt / FPMax / FPMin / FPCompare, FPToFixed, FixedToFP, FPRoundInt,
FPConvert, FPRecipEstimate / FPRSqrtEstimate / FPRecipX, FPRecipStepFused / FPRSqrtStepFused, FPMulX, FPScale),
on exact rational numbers (fractions.Fraction) with Python integers: it shares nothing with
Source/dismount/semantics/SemanticFloat.cpp. It is the source of the expected values of the unit rows of the
floating point instructions (gen_simd_semantic_rows_float.py), and it is checked against the Apple M4 and
Unicorn (check_armfp.py) so the rows are the architecture's and not a copy of the lifter's.

The environment `Env` is the FPCR: the rounding mode (RMode 0 RN, 1 RP, 2 RM, 3 RZ), FZ, FZ16 and DN. The
functions return the result and the exception flags as a bit mask of FPSR (IOC 1, DZC 2, OFC 4, UFC 8, IXC 16,
IDC 128). The formats are the widths 16 (IEEE half), 32 and 64, and 'bf16' for BFloat16 (a single precision
number with the low 16 bits cut).

Only the default floating point behavior is modeled (FPCR.AH = 0, FIZ = 0, NEP = 0, AHP = 0, EBF = 0).
"""
from fractions import Fraction

IOC, DZC, OFC, UFC, IXC, IDC = 1, 2, 4, 8, 16, 128

RN, RP, RM, RZ, RA, RO = 0, 1, 2, 3, 4, 5      # RA: nearest away, RO: to odd (FCVTXN)


class Env:
    def __init__(self, rmode=RN, fz=0, fz16=0, dn=0, ahp=0):
        self.rmode, self.fz, self.fz16, self.dn, self.ahp = rmode, fz, fz16, dn, ahp

    @staticmethod
    def from_fpcr(fpcr):
        return Env((fpcr >> 22) & 3, (fpcr >> 24) & 1, (fpcr >> 19) & 1, (fpcr >> 25) & 1, (fpcr >> 26) & 1)

    def fpcr(self):
        return (self.rmode << 22) | (self.fz << 24) | (self.fz16 << 19) | (self.dn << 25) | (self.ahp << 26)


class Fmt:
    """A floating point format: E exponent bits, M fraction bits."""
    def __init__(self, name, total, E, M):
        self.name, self.N, self.E, self.M = name, total, E, M
        self.bias = (1 << (E - 1)) - 1
        self.emin = 1 - self.bias

    def __repr__(self):
        return self.name


H, S, D, BF = Fmt('h', 16, 5, 10), Fmt('s', 32, 8, 23), Fmt('d', 64, 11, 52), Fmt('bf16', 16, 8, 7)
FORMATS = {16: H, 32: S, 64: D}


def fmt_of(x):
    return x if isinstance(x, Fmt) else FORMATS[x]


def default_nan(f):
    return (((1 << f.E) - 1) << f.M) | (1 << (f.M - 1))


def infinity(f, sign):
    return (sign << (f.N - 1)) | (((1 << f.E) - 1) << f.M)


def zero(f, sign):
    return sign << (f.N - 1)


def max_normal(f, sign):
    return (sign << (f.N - 1)) | ((((1 << f.E) - 2)) << f.M) | ((1 << f.M) - 1)


def two(f, sign):
    return pack(f, sign, 1 + f.bias, 0)


def pack(f, sign, exp, frac):
    return (sign << (f.N - 1)) | (exp << f.M) | frac


# ---------------------------------------------------------------------------
# Unpack
# ---------------------------------------------------------------------------

class Num:
    """kind: 'zero' 'normal' 'inf' 'qnan' 'snan'; value: the magnitude as a Fraction (finite); fmt: the format
    the number was unpacked from (set by unpack)."""
    def __init__(self, kind, sign, value=Fraction(0), bits=0):
        self.kind, self.sign, self.value, self.bits = kind, sign, value, bits
        self.fmt = None


def flush_enabled(f, env):
    # the half precision flushes by FZ16, BFloat16 and the others by FZ
    return bool(env.fz16 if f is H else env.fz)


def unpack(bits, f, env, flags, flush_override=None):
    n = _unpack(bits, f, env, flags, flush_override)
    n.fmt = f
    return n


def _unpack(bits, f, env, flags, flush_override=None):
    """FPUnpack. flags is a list [mask] updated for IDC."""
    sign = (bits >> (f.N - 1)) & 1
    exp = (bits >> f.M) & ((1 << f.E) - 1)
    frac = bits & ((1 << f.M) - 1)
    if exp == (1 << f.E) - 1:
        if frac == 0:
            return Num('inf', sign, bits=bits)
        kind = 'qnan' if (frac >> (f.M - 1)) & 1 else 'snan'
        return Num(kind, sign, bits=bits)
    if exp == 0:
        if frac == 0:
            return Num('zero', sign, bits=bits)
        flush = flush_enabled(f, env) if flush_override is None else flush_override
        if flush:
            if f is not H:
                flags[0] |= IDC
            return Num('zero', sign, bits=bits)
        return Num('normal', sign, Fraction(frac, 1 << f.M) * Fraction(2) ** f.emin, bits=bits)
    return Num('normal', sign, Fraction((1 << f.M) | frac, 1 << f.M) * Fraction(2) ** (exp - f.bias), bits=bits)


# ---------------------------------------------------------------------------
# Rounding
# ---------------------------------------------------------------------------

def round_value(value, f, env, flags, sign, rmode=None, flush_override=None):
    """FPRound of the exact non zero magnitude `value` with the sign `sign` into the format. Returns the bits."""
    rmode = env.rmode if rmode is None else rmode
    assert value > 0
    # the exponent: value = m * 2^e with 1 <= m < 2
    e = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** e > value:
        e -= 1
    elif Fraction(2) ** (e + 1) <= value:
        e += 1
    minimum_exp = f.emin
    flush = flush_enabled(f, env) if flush_override is None else flush_override
    if flush and e < minimum_exp:
        flags[0] |= UFC
        return zero(f, sign)
    # the quantum
    if e < minimum_exp:
        quantum = Fraction(2) ** (minimum_exp - f.M)
        biased = 0
    else:
        quantum = Fraction(2) ** (e - f.M)
    n = value / quantum
    q = n.numerator // n.denominator
    error = n - q
    if rmode == RN:
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and (q & 1))
    elif rmode == RP:
        up = error != 0 and not sign
    elif rmode == RM:
        up = error != 0 and sign
    elif rmode == RA:
        up = error >= Fraction(1, 2)
    elif rmode == RO:
        up = False
        if error != 0:
            q |= 1
    else:
        up = False
    if up:
        q += 1
    tiny = e < minimum_exp
    # the encoded exponent and fraction
    if tiny:
        if q >> f.M:                       # rounded up to the smallest normal number
            biased, frac = 1, q & ((1 << f.M) - 1)
        else:
            biased, frac = 0, q
    else:
        if q >> (f.M + 1):
            q >>= 1
            e += 1
        biased, frac = e + f.bias, q & ((1 << f.M) - 1)
    if biased >= (1 << f.E) - 1:
        flags[0] |= OFC | IXC
        if rmode in (RN, RA) or (rmode == RP and not sign) or (rmode == RM and sign):
            return infinity(f, sign)
        return max_normal(f, sign)
    if error != 0:
        if tiny:
            flags[0] |= UFC
        flags[0] |= IXC
    return pack(f, sign, biased, frac)


# ---------------------------------------------------------------------------
# NaNs
# ---------------------------------------------------------------------------

def process_nan(num, f, env, flags):
    """FPProcessNaN."""
    if num.kind == 'snan':
        flags[0] |= IOC
    if env.dn:
        return default_nan(f)
    source = num.fmt or f
    if source is f:
        return num.bits | (1 << (f.M - 1))
    # a NaN of a narrower format (FMLAL): the fraction moves to the top of the fraction of the result
    frac = (num.bits & ((1 << source.M) - 1)) << (f.M - source.M)
    return pack(f, num.sign, (1 << f.E) - 1, frac | (1 << (f.M - 1)))


def process_nans(nums, f, env, flags):
    """FPProcessNaNs / FPProcessNaNs3: (done, result). The first signaling NaN, then the first quiet one."""
    for n in nums:
        if n.kind == 'snan':
            return True, process_nan(n, f, env, flags)
    for n in nums:
        if n.kind == 'qnan':
            return True, process_nan(n, f, env, flags)
    return False, 0


def num_of(x, f, env, flags):
    return unpack(x, f, env, flags)


# ---------------------------------------------------------------------------
# Arithmetic
# ---------------------------------------------------------------------------

def signed_value(n):
    return -n.value if n.sign else n.value


def finish(value, f, env, flags, zero_sign_if_rm=True):
    """The result of an exact arithmetic result `value` (a signed Fraction): zero or rounded."""
    if value == 0:
        return zero(f, 1 if (env.rmode == RM and zero_sign_if_rm) else 0)
    return round_value(abs(value), f, env, flags, 1 if value < 0 else 0)


def fp_add(a, b, f, env, negate_b=False):
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    if negate_b:
        n2.sign ^= 1
        n2.bits ^= 1 << (f.N - 1)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if inf1 and inf2 and n1.sign == (n2.sign ^ 1):
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if (inf1 and not n1.sign) or (inf2 and not n2.sign):
        return infinity(f, 0), flags[0]
    if (inf1 and n1.sign) or (inf2 and n2.sign):
        return infinity(f, 1), flags[0]
    if zero1 and zero2 and n1.sign == n2.sign:
        return zero(f, n1.sign), flags[0]
    return finish(signed_value(n1) + signed_value(n2), f, env, flags), flags[0]


def fp_sub(a, b, f, env):
    """FPSub: the NaNs are processed before the negation (a NaN keeps its sign)."""
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    if inf1 and inf2 and n1.sign == n2.sign:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if (inf1 and not n1.sign) or (inf2 and n2.sign):
        return infinity(f, 0), flags[0]
    if (inf1 and n1.sign) or (inf2 and not n2.sign):
        return infinity(f, 1), flags[0]
    if zero1 and zero2 and n1.sign == (n2.sign ^ 1):
        return zero(f, n1.sign), flags[0]
    return finish(signed_value(n1) - signed_value(n2), f, env, flags), flags[0]


def fp_mul(a, b, f, env):
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    sign = n1.sign ^ n2.sign
    if (inf1 and zero2) or (zero1 and inf2):
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if inf1 or inf2:
        return infinity(f, sign), flags[0]
    if zero1 or zero2:
        return zero(f, sign), flags[0]
    return round_value(n1.value * n2.value, f, env, flags, sign), flags[0]


def fp_mulx(a, b, f, env):
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    sign = n1.sign ^ n2.sign
    if (inf1 and zero2) or (zero1 and inf2):
        return two(f, sign), flags[0]
    if inf1 or inf2:
        return infinity(f, sign), flags[0]
    if zero1 or zero2:
        return zero(f, sign), flags[0]
    return round_value(n1.value * n2.value, f, env, flags, sign), flags[0]


def fp_muladd(addend, a, b, f, env, fab=None):
    """FPMulAdd(addend, op1, op2). The NaNs are processed in the order addend, op1, op2 (the processors'; the Arm
    ARM text names op1 first). `fab` is the format of the multiplicands (FMLAL: half precision products added to a
    single precision number), the result is in `f`."""
    flags = [0]
    fab = fab or f
    na = num_of(addend, f, env, flags)
    n1, n2 = num_of(a, fab, env, flags), num_of(b, fab, env, flags)
    inf1, inf2, zero1, zero2 = n1.kind == 'inf', n2.kind == 'inf', n1.kind == 'zero', n2.kind == 'zero'
    invalid_product = (inf1 and zero2) or (zero1 and inf2)
    done, r = process_nans([na, n1, n2], f, env, flags)
    if na.kind == 'qnan' and invalid_product:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if done:
        return r, flags[0]
    if invalid_product:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    infa, zeroa = na.kind == 'inf', na.kind == 'zero'
    sign_p = n1.sign ^ n2.sign
    infp = inf1 or inf2
    if infa and infp and na.sign != sign_p:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if (infa and not na.sign) or (infp and not sign_p):
        return infinity(f, 0), flags[0]
    if (infa and na.sign) or (infp and sign_p):
        return infinity(f, 1), flags[0]
    if zeroa and (zero1 or zero2) and na.sign == sign_p:
        return zero(f, na.sign), flags[0]
    product = Fraction(0) if (zero1 or zero2) else n1.value * n2.value * (-1 if sign_p else 1)
    return finish(signed_value(na) + product, f, env, flags), flags[0]


def fp_div(a, b, f, env):
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2 = n1.kind == 'inf', n2.kind == 'inf'
    zero1, zero2 = n1.kind == 'zero', n2.kind == 'zero'
    sign = n1.sign ^ n2.sign
    if (inf1 and inf2) or (zero1 and zero2):
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if inf1 or zero2:
        if not inf1:
            flags[0] |= DZC
        return infinity(f, sign), flags[0]
    if zero1 or inf2:
        return zero(f, sign), flags[0]
    return round_value(n1.value / n2.value, f, env, flags, sign), flags[0]


def isqrt_frac(x, bits):
    """floor(sqrt(x) * 2^bits) and whether sqrt(x)*2^bits was exact, for a positive Fraction x."""
    scaled = x * (1 << (2 * bits))
    n = scaled.numerator // scaled.denominator
    r = _isqrt(n)
    exact = (r * r == scaled)
    return r, exact


def _isqrt(n):
    import math
    return math.isqrt(n)


def fp_sqrt(a, f, env):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    if n.kind == 'zero':
        return zero(f, n.sign), flags[0]
    if n.kind == 'inf' and not n.sign:
        return infinity(f, 0), flags[0]
    if n.sign:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    # x = m * 4^k with 1 <= m < 4: sqrt(x) = sqrt(m) * 2^k, sqrt(m) with M + 72 fractional bits and a sticky bit
    k = 0
    m = n.value
    while m >= 4:
        m /= 4
        k += 1
    while m < 1:
        m *= 4
        k -= 1
    bits = f.M + 72
    r, exact = isqrt_frac(m, bits)
    value = Fraction(r, 1 << bits)
    if not exact:
        value += Fraction(1, 1 << (bits + 10))      # a sticky bit well below the rounding position
    return round_value(value * Fraction(2) ** k, f, env, flags, 0), flags[0]


def fp_neg(a, f):
    return a ^ (1 << (f.N - 1))


def fp_abs(a, f):
    return a & ~(1 << (f.N - 1))


def fp_max(a, b, f, env, is_max=True, number=False):
    """FPMax / FPMin / FPMaxNum / FPMinNum."""
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    if number:
        # a quiet NaN against a number is replaced by -infinity (maximum) / +infinity (minimum)
        sentinel = 1 if is_max else 0
        if n1.kind == 'qnan' and n2.kind != 'qnan':
            n1 = Num('inf', sentinel, bits=infinity(f, sentinel))
        elif n2.kind == 'qnan' and n1.kind != 'qnan':
            n2 = Num('inf', sentinel, bits=infinity(f, sentinel))
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]

    def key(n):
        if n.kind == 'inf':
            return (-1 if n.sign else 1) * Fraction(10) ** 1000
        return signed_value(n)
    v1, v2 = key(n1), key(n2)
    pick_first = (v1 > v2) if is_max else (v1 < v2)
    chosen = n1 if pick_first else n2
    if chosen.kind == 'inf':
        return infinity(f, chosen.sign), flags[0]
    if chosen.kind == 'zero':
        sign = (n1.sign & n2.sign) if is_max else (n1.sign | n2.sign)
        return zero(f, sign), flags[0]
    return round_value(chosen.value, f, env, flags, chosen.sign), flags[0]


def fp_compare(a, b, f, env, signal_nans):
    """(relation, flags): relation in 'lt' 'eq' 'gt' 'un'."""
    flags = [0]
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    if n1.kind in ('qnan', 'snan') or n2.kind in ('qnan', 'snan'):
        if n1.kind == 'snan' or n2.kind == 'snan' or signal_nans:
            flags[0] |= IOC
        return 'un', flags[0]

    def key(n):
        if n.kind == 'inf':
            return (-1 if n.sign else 1) * Fraction(10) ** 1000
        return signed_value(n)
    v1, v2 = key(n1), key(n2)
    return ('eq' if v1 == v2 else ('lt' if v1 < v2 else 'gt')), flags[0]


NZCV_OF = {'lt': 0b1000, 'eq': 0b0110, 'gt': 0b0010, 'un': 0b0011}


# ---------------------------------------------------------------------------
# Conversions
# ---------------------------------------------------------------------------

def round_to_integer(value, rmode, sign_known=None):
    """The integer nearest to the Fraction `value` by the rounding mode, and the error."""
    fl = value.numerator // value.denominator
    error = value - fl
    if rmode == RN:
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and (fl & 1))
    elif rmode == RP:
        up = error != 0
    elif rmode == RM:
        up = False
    elif rmode == RZ:
        up = error != 0 and fl < 0
    elif rmode == RA:
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and fl >= 0)
    else:
        up = False
    if up:
        fl += 1
    return fl, error


def fp_to_fixed(a, f, env, bits, signed, fbits, rmode):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        flags[0] |= IOC
        return 0, flags[0]
    if n.kind == 'zero':
        return 0, flags[0]
    if n.kind == 'inf':
        value = Fraction(10) ** 100 * (-1 if n.sign else 1)
    else:
        value = signed_value(n) * (1 << fbits)
    integer, error = round_to_integer(value, rmode)
    lo, hi = ((-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1))
    if integer < lo or integer > hi:
        flags[0] |= IOC
        return (lo if integer < lo else hi) & ((1 << bits) - 1), flags[0]
    if error != 0:
        flags[0] |= IXC
    return integer & ((1 << bits) - 1), flags[0]


def fixed_to_fp(value, bits, signed, fbits, f, env, rmode=None):
    flags = [0]
    if signed and (value >> (bits - 1)) & 1:
        value -= 1 << bits
    if value == 0:
        return zero(f, 0), 0
    sign = 1 if value < 0 else 0
    return round_value(Fraction(abs(value), 1 << fbits), f, env, flags, sign, rmode), flags[0]


def fp_round_int(a, f, env, rmode, exact):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    if n.kind == 'inf':
        return infinity(f, n.sign), flags[0]
    if n.kind == 'zero':
        return zero(f, n.sign), flags[0]
    integer, error = round_to_integer(signed_value(n), rmode)
    if integer == 0:
        result = zero(f, n.sign)
    else:
        result = round_value(Fraction(abs(integer)), f, env, flags, 1 if integer < 0 else 0, RN, flush_override=False)
    if exact and error != 0:
        flags[0] |= IXC
    return result, flags[0]


def fp_round_int_bounded(a, f, env, rmode, bits):
    """FRINT32Z / FRINT32X / FRINT64Z / FRINT64X."""
    flags = [0]
    n = num_of(a, f, env, flags)
    minimum = pack(f, 1, f.bias + bits - 1, 0)
    if n.kind in ('qnan', 'snan', 'inf'):
        flags[0] |= IOC
        return minimum, flags[0]
    if n.kind == 'zero':
        return zero(f, n.sign), flags[0]
    integer, error = round_to_integer(signed_value(n), rmode)
    if integer < -(1 << (bits - 1)) or integer > (1 << (bits - 1)) - 1:
        flags[0] |= IOC
        return minimum, flags[0]
    if integer == 0:
        result = zero(f, n.sign)
    else:
        result = round_value(Fraction(abs(integer)), f, env, flags, 1 if integer < 0 else 0, RN, flush_override=False)
    if error != 0:
        flags[0] |= IXC
    return result, flags[0]


def fp_convert(a, ff, ft, env, rmode=None):
    """FPConvert from the format ff to the format ft."""
    flags = [0]
    # the conversions don't use FZ16 for the half precision operand and result
    envc = Env(env.rmode, env.fz, 0, env.dn)
    if env.ahp and ff is H:
        n = _unpack_alternative_half(a, flags)
    else:
        n = unpack(a, ff, envc, flags, flush_override=bool(env.fz) if ff is not H else False)
    if env.ahp and ft is H:
        return _convert_to_alternative_half(n, env, flags, rmode)
    if n.kind in ('qnan', 'snan'):
        if n.kind == 'snan':
            flags[0] |= IOC
        if env.dn:
            return default_nan(ft), flags[0]
        frac = n.bits & ((1 << ff.M) - 1)
        if ft.M >= ff.M:
            frac <<= ft.M - ff.M
        else:
            frac >>= ff.M - ft.M
        frac |= 1 << (ft.M - 1)
        return pack(ft, n.sign, (1 << ft.E) - 1, frac), flags[0]
    if n.kind == 'inf':
        return infinity(ft, n.sign), flags[0]
    if n.kind == 'zero':
        return zero(ft, n.sign), flags[0]
    flush = bool(env.fz) if ft is not H else False
    return round_value(n.value, ft, env, flags, n.sign, rmode, flush_override=flush), flags[0]


# ---------------------------------------------------------------------------
# Estimates and steps
# ---------------------------------------------------------------------------

def recip_estimate9(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def recip_sqrt_estimate9(a):
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


def fp_recip_estimate(a, f, env):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    if n.kind == 'inf':
        return zero(f, n.sign), flags[0]
    if n.kind == 'zero':
        flags[0] |= DZC
        return infinity(f, n.sign), flags[0]
    limit = {16: Fraction(1, 1 << 16), 32: Fraction(2) ** -128, 64: Fraction(2) ** -1024}[f.N]
    if n.value < limit:
        rm = env.rmode
        to_inf = (rm == RN) or (rm == RP and not n.sign) or (rm == RM and n.sign)
        flags[0] |= OFC | IXC
        return (infinity(f, n.sign) if to_inf else max_normal(f, n.sign)), flags[0]
    flush = env.fz16 if f is H else env.fz
    big = {16: Fraction(1 << 14), 32: Fraction(2) ** 126, 64: Fraction(2) ** 1022}[f.N]
    if flush and n.value >= big:
        flags[0] |= UFC
        return zero(f, n.sign), flags[0]
    exp = (a >> f.M) & ((1 << f.E) - 1)
    frac = (a & ((1 << f.M) - 1)) << (52 - f.M)
    if exp == 0:
        if (frac >> 51) & 1 == 0:
            exp = -1
            frac = (frac << 2) & ((1 << 52) - 1)
        else:
            frac = (frac << 1) & ((1 << 52) - 1)
    scaled = 256 | ((frac >> 44) & 0xff)
    result_exp = 2 * f.bias - 1 - exp
    estimate = recip_estimate9(scaled)
    frac = (estimate & 0xff) << 44
    if result_exp == 0:
        frac = (1 << 51) | (frac >> 1)
    elif result_exp == -1:
        frac = (1 << 50) | (frac >> 2)
        result_exp = 0
    return pack(f, n.sign, result_exp, frac >> (52 - f.M)), flags[0]


def fp_rsqrt_estimate(a, f, env):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    if n.kind == 'zero':
        flags[0] |= DZC
        return infinity(f, n.sign), flags[0]
    if n.sign:
        flags[0] |= IOC
        return default_nan(f), flags[0]
    if n.kind == 'inf':
        return zero(f, 0), flags[0]
    exp = (a >> f.M) & ((1 << f.E) - 1)
    frac = (a & ((1 << f.M) - 1)) << (52 - f.M)
    if exp == 0:
        while (frac >> 51) & 1 == 0:
            frac = (frac << 1) & ((1 << 52) - 1)
            exp -= 1
        frac = (frac << 1) & ((1 << 52) - 1)
    if exp & 1 == 0:
        scaled = 256 | ((frac >> 44) & 0xff)
    else:
        scaled = 128 | ((frac >> 45) & 0x7f)
    result_exp = (3 * f.bias - 1 - exp) // 2
    estimate = recip_sqrt_estimate9(scaled)
    frac = (estimate & 0xff) << 44
    return pack(f, 0, result_exp, frac >> (52 - f.M)), flags[0]


def fp_recipx(a, f, env):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    exp = (a >> f.M) & ((1 << f.E) - 1)
    full = (1 << f.E) - 1
    result_exp = (full - 1) if exp == 0 else (~exp & full)
    return pack(f, (a >> (f.N - 1)) & 1, result_exp, 0), flags[0]


def fp_recip_step(a, b, f, env):
    """FRECPS: 2 - a * b, fused."""
    flags = [0]
    a = fp_neg(a, f)
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2, zero1, zero2 = n1.kind == 'inf', n2.kind == 'inf', n1.kind == 'zero', n2.kind == 'zero'
    if (inf1 and zero2) or (zero1 and inf2):
        return two(f, 0), flags[0]
    if inf1 or inf2:
        return infinity(f, n1.sign ^ n2.sign), flags[0]
    product = Fraction(0) if (zero1 or zero2) else n1.value * n2.value * (-1 if n1.sign ^ n2.sign else 1)
    return finish(Fraction(2) + product, f, env, flags), flags[0]


def fp_rsqrt_step(a, b, f, env):
    """FRSQRTS: (3 - a * b) / 2, fused."""
    flags = [0]
    a = fp_neg(a, f)
    n1, n2 = num_of(a, f, env, flags), num_of(b, f, env, flags)
    done, r = process_nans([n1, n2], f, env, flags)
    if done:
        return r, flags[0]
    inf1, inf2, zero1, zero2 = n1.kind == 'inf', n2.kind == 'inf', n1.kind == 'zero', n2.kind == 'zero'
    if (inf1 and zero2) or (zero1 and inf2):
        return pack(f, 0, f.bias, 1 << (f.M - 1)), flags[0]       # 1.5
    if inf1 or inf2:
        return infinity(f, n1.sign ^ n2.sign), flags[0]
    product = Fraction(0) if (zero1 or zero2) else n1.value * n2.value * (-1 if n1.sign ^ n2.sign else 1)
    return finish((Fraction(3) + product) / 2, f, env, flags), flags[0]


def fp_scale(a, scale, f, env):
    flags = [0]
    n = num_of(a, f, env, flags)
    if n.kind in ('qnan', 'snan'):
        return process_nan(n, f, env, flags), flags[0]
    if n.kind == 'zero':
        return zero(f, n.sign), flags[0]
    if n.kind == 'inf':
        return infinity(f, n.sign), flags[0]
    # beyond 2^-5000 and 2^5000 every format overflows or underflows alike
    scale = max(-5000, min(5000, scale))
    return round_value(n.value * Fraction(2) ** scale, f, env, flags, n.sign), flags[0]


# ---------------------------------------------------------------------------
# BFloat16 (FEAT_BF16, FPCR.EBF = 0)
# ---------------------------------------------------------------------------

def _odd_single(kind, sign, value):
    """The single precision bits of an exact result: rounded to odd, tiny results flushed to zero, an overflow
    is an infinity. kind is 'zero', 'inf' or 'normal'."""
    if kind == 'inf':
        return infinity(S, sign)
    if kind == 'zero' or value == 0:
        return zero(S, sign)
    flags = [0]
    r = round_value(abs(value), S, Env(RO, 1, 0, 0), flags, 1 if value < 0 else 0, RO)
    if flags[0] & OFC:
        return infinity(S, 1 if value < 0 else 0)
    return r


def _extended(bits):
    """(kind, sign, signed value) of a single precision number with the denormals flushed, NaNs excluded."""
    n = unpack(bits, S, Env(RN, 1, 0, 0), [0])
    return n.kind, n.sign, (-n.value if n.sign else n.value)


def _single_add(x, y):
    kx, sx, vx = _extended(x)
    ky, sy, vy = _extended(y)
    if kx == 'inf' and ky == 'inf':
        return default_nan(S) if sx != sy else infinity(S, sx)
    if kx == 'inf':
        return infinity(S, sx)
    if ky == 'inf':
        return infinity(S, sy)
    if kx == 'zero' and ky == 'zero':
        return zero(S, sx & sy)
    total = vx + vy
    if total == 0:
        return zero(S, 0)
    return _odd_single('normal', 1 if total < 0 else 0, total)


def _single_mul(x, y):
    kx, sx, vx = _extended(x)
    ky, sy, vy = _extended(y)
    sign = sx ^ sy
    if (kx == 'inf' and ky == 'zero') or (kx == 'zero' and ky == 'inf'):
        return default_nan(S)
    if kx == 'inf' or ky == 'inf':
        return infinity(S, sign)
    if kx == 'zero' or ky == 'zero':
        return zero(S, sign)
    return _odd_single('normal', sign, vx * vy)


def bfloat_dot_add(acc, a0, a1, b0, b1):
    """BFDotAdd with the standard behaviors: flushed denormals, a NaN is the default NaN, no flags, the sum of the
    two products and the sum with the accumulator are rounded to odd."""
    env = Env(RN, 1, 0, 0)
    ops = [unpack(x, BF, env, [0]) for x in (a0, a1, b0, b1)]
    if any(n.kind in ('qnan', 'snan') for n in ops) or unpack(acc, S, env, [0]).kind in ('qnan', 'snan'):
        return default_nan(S)
    w = [zero(S, n.sign) if n.kind == 'zero' else x << 16 for n, x in zip(ops, (a0, a1, b0, b1))]
    p0 = _single_mul(w[0], w[2])
    p1 = _single_mul(w[1], w[3])
    for p in (p0, p1):
        if p == default_nan(S):
            return p
    s = _single_add(p0, p1)
    if s == default_nan(S):
        return s
    return _single_add(acc, s)


# ---------------------------------------------------------------------------
# The alternative half precision (FPCR.AHP): no infinities and NaNs, the exponent 31 is a number
# ---------------------------------------------------------------------------

def _unpack_alternative_half(bits, flags):
    sign = (bits >> 15) & 1
    exp = (bits >> 10) & 31
    frac = bits & 0x3ff
    if exp == 0:
        if frac == 0:
            return Num('zero', sign, bits=bits)
        n = Num('normal', sign, Fraction(frac, 1 << 10) * Fraction(2) ** -14, bits=bits)
    else:
        n = Num('normal', sign, Fraction((1 << 10) | frac, 1 << 10) * Fraction(2) ** (exp - 15), bits=bits)
    n.fmt = H
    return n


def _convert_to_alternative_half(n, env, flags, rmode):
    largest = (n.sign << 15) | 0x7fff
    if n.kind in ('qnan', 'snan'):
        flags[0] |= IOC
        return zero(H, n.sign), flags[0]
    if n.kind == 'inf':
        flags[0] |= IOC
        return largest, flags[0]
    if n.kind == 'zero':
        return zero(H, n.sign), flags[0]
    rmode = env.rmode if rmode is None else rmode
    value = n.value
    # the rounding with the exponent range of the alternative format
    e = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** e > value:
        e -= 1
    elif Fraction(2) ** (e + 1) <= value:
        e += 1
    quantum = Fraction(2) ** (max(e, -14) - 10)
    q_exact = value / quantum
    q = q_exact.numerator // q_exact.denominator
    error = q_exact - q
    sign = n.sign
    if rmode == RN:
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and (q & 1))
    elif rmode == RP:
        up = error != 0 and not sign
    elif rmode == RM:
        up = error != 0 and sign
    else:
        up = False
    if up:
        q += 1
    if e < -14:
        biased, frac = (1, q & 0x3ff) if q >> 10 else (0, q)
    else:
        if q >> 11:
            q >>= 1
            e += 1
        biased, frac = e + 15, q & 0x3ff
    if biased > 31:
        flags[0] |= IOC
        return largest, flags[0]
    if error != 0:
        if e < -14 and biased == 0:
            flags[0] |= UFC
        flags[0] |= IXC
    return pack(H, sign, biased, frac), flags[0]


def fp_jcvtzs(bits, env):
    """FJCVTZS (FPToFixedJS): (the w0 value, NZCV, flags): the double converted toward zero modulo 2^32, Z set
    when the conversion was exact."""
    flags = [0]
    n = unpack(bits, D, env, flags)
    z = 1
    if n.kind in ('qnan', 'snan', 'inf'):
        flags[0] |= IOC
        return 0, 0, flags[0]
    value = (-n.value if n.sign else n.value) if n.kind == 'normal' else Fraction(0)
    integer = int(value)                       # toward zero
    error = value - integer
    result = integer & 0xffffffff
    if integer < -(1 << 31) or integer > (1 << 31) - 1:
        flags[0] |= IOC
        z = 0
    elif error != 0:
        flags[0] |= IXC
        z = 0
    elif n.sign and value == 0:
        z = 0
    if flags[0] & IDC:
        z = 0                                   # a denormal flushed to zero is not exact
    return result, z << 2, flags[0]
