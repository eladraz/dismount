"""The model of the SVE floating point instructions of Arm64SemanticLiftSveFloat2.cpp that no processor
available to the differential tests (tests/arm/semantics) runs: the bfloat16 arithmetic, famax /
famin, the matrix multiplies, the rounding to a 32 / 64 bit integer, the conversions of the integers
to the top elements, and the zeroing forms of the SVE2.2 instructions. Written from the pseudocode of
the Arm ARM (see sve_model.py) with exact rational arithmetic: fpcr is 0 (round to nearest even, no
flush to zero, no default NaN), the exceptions are accumulated in fpsr."""
import random
import zlib
from fractions import Fraction

import sve_model as M
from sve_model import Machine, Row, mask, sext

VLS = (128, 256, 512)

IOC, DZC, OFC, UFC, IXC, IDC = 1, 2, 4, 8, 16, 0x80


class Fmt:
    def __init__(self, ebits, mbits):
        self.e = ebits
        self.m = mbits
        self.bits = 1 + ebits + mbits
        self.bias = (1 << (ebits - 1)) - 1
        self.emax = (1 << ebits) - 1


HALF, BRAIN, SINGLE, DOUBLE = Fmt(5, 10), Fmt(8, 7), Fmt(8, 23), Fmt(11, 52)


def decode(fmt, bits):
    """('nan', quiet, sign, fraction) | ('inf', sign) | ('zero', sign) | ('num', Fraction)"""
    sign = bits >> (fmt.bits - 1)
    exp = (bits >> fmt.m) & fmt.emax
    frac = bits & mask(fmt.m)
    if exp == fmt.emax:
        if frac == 0:
            return ('inf', sign)
        return ('nan', (frac >> (fmt.m - 1)) & 1, sign, frac)
    if exp == 0:
        if frac == 0:
            return ('zero', sign)
        value = Fraction(frac, 1 << fmt.m) * Fraction(2) ** (1 - fmt.bias)
    else:
        value = (1 + Fraction(frac, 1 << fmt.m)) * Fraction(2) ** (exp - fmt.bias)
    return ('num', -value if sign else value)


def pack(fmt, sign, exp, frac):
    return (sign << (fmt.bits - 1)) | (exp << fmt.m) | frac


def infinity(fmt, sign):
    return pack(fmt, sign, fmt.emax, 0)


def maximum(fmt, sign):
    return pack(fmt, sign, fmt.emax - 1, mask(fmt.m))


def default_nan(fmt):
    return pack(fmt, 0, fmt.emax, 1 << (fmt.m - 1))


def round_value(fmt, value, mode='rne'):
    """(bits, flags) of the rounding of the nonzero Fraction to the format (tininess is detected
    before rounding)."""
    sign = 1 if value < 0 else 0
    a = -value if sign else value
    emin = 1 - fmt.bias
    e = 0
    t = a
    while t >= 2:
        t /= 2
        e += 1
    while t < 1:
        t *= 2
        e -= 1
    flags = 0
    tiny = e < emin
    quantum_exp = (emin if tiny else e) - fmt.m
    q = a / (Fraction(2) ** quantum_exp)
    whole = q.numerator // q.denominator
    rem = q - whole
    if rem != 0:
        flags |= IXC
        if mode == 'rne':
            if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and whole & 1):
                whole += 1
        elif mode == 'rto':
            whole |= 1
    if tiny and rem != 0:
        flags |= UFC
    # the value of the rounded significand
    if whole >= (1 << (fmt.m + 1)) and not tiny:
        whole >>= 1
        e += 1
        quantum_exp += 1
    if tiny:
        if whole >= (1 << fmt.m):
            return pack(fmt, sign, 1, whole - (1 << fmt.m)), flags
        return pack(fmt, sign, 0, whole), flags
    if e + fmt.bias >= fmt.emax:
        flags |= OFC | IXC
        if mode == 'rz':
            return maximum(fmt, sign), flags
        return infinity(fmt, sign), flags
    return pack(fmt, sign, e + fmt.bias, whole - (1 << fmt.m)), flags


def quiet(fmt, bits):
    return bits | (1 << (fmt.m - 1))


def nan_of(fmt, operands, flags):
    """The NaN result for operand bits (a signaling NaN first, then a quiet one, in order)."""
    for want_quiet in (0, 1):
        for bits in operands:
            d = decode(fmt, bits)
            if d[0] == 'nan' and d[1] == want_quiet:
                if not want_quiet:
                    flags[0] |= IOC
                return quiet(fmt, bits)
    return None


def has_signaling(fmt, operands):
    return any(decode(fmt, b)[0] == 'nan' and decode(fmt, b)[1] == 0 for b in operands)


def fp_add(fmt, a, b, flags, negate_b=False):
    if negate_b:
        b ^= 1 << (fmt.bits - 1)
    n = nan_of(fmt, [a, b], flags)
    if n is not None:
        return n
    da, db = decode(fmt, a), decode(fmt, b)
    if da[0] == 'inf' or db[0] == 'inf':
        if da[0] == 'inf' and db[0] == 'inf' and da[1] != db[1]:
            flags[0] |= IOC
            return default_nan(fmt)
        return infinity(fmt, da[1] if da[0] == 'inf' else db[1])
    va = da[1] if da[0] == 'num' else Fraction(0)
    vb = db[1] if db[0] == 'num' else Fraction(0)
    total = va + vb
    if total == 0:
        if da[0] == 'zero' and db[0] == 'zero' and da[1] == db[1]:
            return pack(fmt, da[1], 0, 0)
        return 0
    bits, f = round_value(fmt, total)
    flags[0] |= f
    return bits


def fp_fma(fmt, a, b, c, flags):
    """a * b + c with one rounding."""
    da, db, dc = decode(fmt, a), decode(fmt, b), decode(fmt, c)
    # the addend first
    n = nan_of(fmt, [c, a, b], flags)
    inf_zero = (da[0] == 'inf' and db[0] == 'zero') or (da[0] == 'zero' and db[0] == 'inf')
    if n is not None:
        if inf_zero and dc[0] == 'nan' and dc[1] == 1 and not has_signaling(fmt, [a, b, c]):
            flags[0] |= IOC
            return default_nan(fmt)
        return n
    if inf_zero:
        flags[0] |= IOC
        return default_nan(fmt)
    sign = (da[1] if da[0] in ('inf', 'zero') else (1 if da[1] < 0 else 0)) ^ \
           (db[1] if db[0] in ('inf', 'zero') else (1 if db[1] < 0 else 0))
    if da[0] == 'inf' or db[0] == 'inf':
        if dc[0] == 'inf' and dc[1] != sign:
            flags[0] |= IOC
            return default_nan(fmt)
        return infinity(fmt, sign)
    if dc[0] == 'inf':
        return infinity(fmt, dc[1])
    va = da[1] if da[0] == 'num' else Fraction(0)
    vb = db[1] if db[0] == 'num' else Fraction(0)
    vc = dc[1] if dc[0] == 'num' else Fraction(0)
    total = va * vb + vc
    if total == 0:
        product_zero = va * vb == 0
        if product_zero and dc[0] == 'zero' and sign == dc[1]:
            return pack(fmt, sign, 0, 0)
        return 0
    bits, f = round_value(fmt, total)
    flags[0] |= f
    return bits


def _mul(fmt, a, b, flags):
    return _mul_mode(fmt, a, b, flags, 'rne')


def _add_mode(fmt, a, b, flags, mode):
    n = nan_of(fmt, [a, b], flags)
    if n is not None:
        return n
    da, db = decode(fmt, a), decode(fmt, b)
    if da[0] == 'inf' or db[0] == 'inf':
        if da[0] == 'inf' and db[0] == 'inf' and da[1] != db[1]:
            flags[0] |= IOC
            return default_nan(fmt)
        return infinity(fmt, da[1] if da[0] == 'inf' else db[1])
    va = da[1] if da[0] == 'num' else Fraction(0)
    vb = db[1] if db[0] == 'num' else Fraction(0)
    total = va + vb
    if total == 0:
        if da[0] == 'zero' and db[0] == 'zero' and da[1] == db[1]:
            return pack(fmt, da[1], 0, 0)
        return 0
    bits, f = round_value(fmt, total, mode)
    flags[0] |= f
    return bits


def _mul_mode(fmt, a, b, flags, mode):
    da, db = decode(fmt, a), decode(fmt, b)
    n = nan_of(fmt, [a, b], flags)
    if n is not None:
        return n
    sign = (da[1] if da[0] in ('inf', 'zero') else (1 if da[1] < 0 else 0)) ^ \
           (db[1] if db[0] in ('inf', 'zero') else (1 if db[1] < 0 else 0))
    if (da[0] == 'inf' and db[0] == 'zero') or (da[0] == 'zero' and db[0] == 'inf'):
        flags[0] |= IOC
        return default_nan(fmt)
    if da[0] == 'inf' or db[0] == 'inf':
        return infinity(fmt, sign)
    if da[0] == 'zero' or db[0] == 'zero':
        return pack(fmt, sign, 0, 0)
    bits, f = round_value(fmt, da[1] * db[1], mode)
    flags[0] |= f
    return bits


def compare_value(fmt, bits):
    d = decode(fmt, bits)
    if d[0] == 'num':
        return d[1]
    if d[0] == 'zero':
        return Fraction(0)
    return None


def fp_minmax(fmt, a, b, flags, is_max, number):
    da, db = decode(fmt, a), decode(fmt, b)
    if number:
        if da[0] == 'nan' and da[1] == 1 and db[0] != 'nan':
            a, da = infinity(fmt, 1 if is_max else 0), ('inf', 1 if is_max else 0)
        elif db[0] == 'nan' and db[1] == 1 and da[0] != 'nan':
            b, db = infinity(fmt, 1 if is_max else 0), ('inf', 1 if is_max else 0)
    n = nan_of(fmt, [a, b], flags)
    if n is not None:
        return n
    va = float('inf') if da[0] == 'inf' and da[1] == 0 else float('-inf') if da[0] == 'inf' else None
    vb = float('inf') if db[0] == 'inf' and db[1] == 0 else float('-inf') if db[0] == 'inf' else None

    def key(d, x):
        if d[0] == 'inf':
            return (Fraction(10) ** 100) * (-1 if d[1] else 1)
        if d[0] == 'zero':
            return Fraction(0)
        return d[1]
    ka, kb = key(da, a), key(db, b)
    if ka == kb:
        if da[0] == 'zero' and db[0] == 'zero':
            sa, sb = da[1], db[1]
            if sa == sb:
                return a
            return (a if sa == 0 else b) if is_max else (a if sa == 1 else b)
        return a
    if is_max:
        return a if ka > kb else b
    return a if ka < kb else b


def vector(fmt, count, rng):
    """Floating point lane bits: special values and random normal numbers."""
    special = [0, 1 << (fmt.bits - 1), pack(fmt, 0, fmt.bias, 0), pack(fmt, 1, fmt.bias, 0),
               infinity(fmt, 0), infinity(fmt, 1), pack(fmt, 0, fmt.bias + 1, 0), pack(fmt, 0, fmt.bias - 1, 0),
               maximum(fmt, 0), pack(fmt, 0, 1, 0), 1, mask(fmt.m)]
    out = []
    for _ in range(count):
        r = rng.random()
        if r < 0.3:
            out.append(rng.choice(special))
        else:
            sign = rng.getrandbits(1)
            exp = max(1, min(fmt.emax - 1, fmt.bias + rng.randint(-6, 6)))
            out.append(pack(fmt, sign, exp, rng.getrandbits(fmt.m)))
    return out


def seeded(text, vl, which=0):
    return random.Random(zlib.crc32(('%s:%d:%d' % (text, vl, which)).encode()))


def machine(vl, text, formats):
    """A machine whose z registers 0..7 hold floating point lanes of the formats (a format per
    register index: dict reg -> Fmt), the others random."""
    m = M.random_machine(vl, text)
    rng = seeded(text, vl)
    for reg, fmt in formats.items():
        m.zset(reg, fmt.bits, vector(fmt, vl // fmt.bits, rng))
    m.fpsr = 0
    return m


def add(rows, text, formats, model, vls=VLS, setup=None):
    for vl in vls:
        m = machine(vl, text, formats)
        if setup:
            setup(m)
        rows.append(Row(text, vl, m, model))


def lanes(m, reg, fmt):
    return m.zget(reg, fmt.bits)


def put(m, reg, fmt, values):
    m.zset(reg, fmt.bits, values)


def fold(m, flags):
    m.fpsr |= flags[0]


def merge(m, p, esize, dest, new, zeroing=False):
    old = m.zget(dest, esize)
    return [new[i] if m.active(m.p[p], i, esize) else (0 if zeroing else old[i]) for i in range(m.lanes(esize))]


def rows():
    r = []
    fmt = BRAIN
    # bfloat16 arithmetic: z0 = dn, z2 = m, p1
    for name, fn in (('bfadd', lambda a, b, f: fp_add(BRAIN, a, b, f)),
                     ('bfsub', lambda a, b, f: fp_add(BRAIN, a, b, f, True)),
                     ('bfmul', lambda a, b, f: _mul(BRAIN, a, b, f)),
                     ('bfmax', lambda a, b, f: fp_minmax(BRAIN, a, b, f, True, False)),
                     ('bfmin', lambda a, b, f: fp_minmax(BRAIN, a, b, f, False, False)),
                     ('bfmaxnm', lambda a, b, f: fp_minmax(BRAIN, a, b, f, True, True)),
                     ('bfminnm', lambda a, b, f: fp_minmax(BRAIN, a, b, f, False, True))):
        def predicated(m, fn=fn):
            f = [0]
            a, b = lanes(m, 0, BRAIN), lanes(m, 2, BRAIN)
            new = [fn(a[i], b[i], f) if m.active(m.p[1], i, 16) else a[i] for i in range(len(a))]
            put(m, 0, BRAIN, new)
            # the flags of the inactive lanes are not raised: the model computes the active ones only
            fold(m, f)
        add(r, '%s z0.h, p1/m, z0.h, z2.h' % name, {0: BRAIN, 2: BRAIN}, predicated)
    for name, fn in (('bfadd', lambda a, b, f: fp_add(BRAIN, a, b, f)),
                     ('bfsub', lambda a, b, f: fp_add(BRAIN, a, b, f, True)),
                     ('bfmul', lambda a, b, f: _mul(BRAIN, a, b, f))):
        def unpredicated(m, fn=fn):
            f = [0]
            a, b = lanes(m, 1, BRAIN), lanes(m, 2, BRAIN)
            put(m, 0, BRAIN, [fn(a[i], b[i], f) for i in range(len(a))])
            fold(m, f)
        add(r, '%s z0.h, z1.h, z2.h' % name, {1: BRAIN, 2: BRAIN}, unpredicated)

    def bfmul_indexed(m):
        f = [0]
        a, b = lanes(m, 1, BRAIN), lanes(m, 2, BRAIN)
        out = []
        for i in range(len(a)):
            seg = i // 8
            out.append(_mul(BRAIN, a[i], b[seg * 8 + 3], f))
        put(m, 0, BRAIN, out)
        fold(m, f)
    add(r, 'bfmul z0.h, z1.h, z2.h[3]', {1: BRAIN, 2: BRAIN}, bfmul_indexed)

    def bfscale(m):
        f = [0]
        a, b = lanes(m, 0, BRAIN), m.zget(2, 16)
        out = []
        for i in range(len(a)):
            if not m.active(m.p[1], i, 16):
                out.append(a[i])
                continue
            n = sext(b[i], 16)
            d = decode(BRAIN, a[i])
            if d[0] == 'nan':
                out.append(nan_of(BRAIN, [a[i]], f))
            elif d[0] in ('inf', 'zero'):
                out.append(a[i])
            else:
                bits, fl = round_value(BRAIN, d[1] * Fraction(2) ** max(-400, min(400, n)))
                f[0] |= fl
                out.append(bits)
        put(m, 0, BRAIN, out)
        fold(m, f)

    def bfscale_setup(m):
        rng = seeded('bfscale', m.vl)
        m.zset(2, 16, [rng.choice([0, 1, 2, 5, -1, -3, -20, 130, -140, 200, -300, 0x7fff, 0x8000]) for _ in range(m.lanes(16))])
    add(r, 'bfscale z0.h, p1/m, z0.h, z2.h', {0: BRAIN}, bfscale, setup=bfscale_setup)

    for name, negate in (('bfmla', False), ('bfmls', True)):
        def fused(m, negate=negate):
            f = [0]
            a, b, c = lanes(m, 1, BRAIN), lanes(m, 2, BRAIN), lanes(m, 0, BRAIN)
            out = []
            for i in range(len(a)):
                if m.active(m.p[3], i, 16):
                    x = a[i] ^ (1 << 15) if negate else a[i]
                    out.append(fp_fma(BRAIN, x, b[i], c[i], f))
                else:
                    out.append(c[i])
            put(m, 0, BRAIN, out)
            fold(m, f)
        add(r, '%s z0.h, p3/m, z1.h, z2.h' % name, {0: BRAIN, 1: BRAIN, 2: BRAIN}, fused)

        def indexed(m, negate=negate):
            f = [0]
            a, b, c = lanes(m, 1, BRAIN), lanes(m, 2, BRAIN), lanes(m, 0, BRAIN)
            out = []
            for i in range(len(a)):
                seg = i // 8
                x = a[i] ^ (1 << 15) if negate else a[i]
                out.append(fp_fma(BRAIN, x, b[seg * 8 + 5], c[i], f))
            put(m, 0, BRAIN, out)
            fold(m, f)
        add(r, '%s z0.h, z1.h, z2.h[5]' % name, {0: BRAIN, 1: BRAIN, 2: BRAIN}, indexed)

    def bfclamp(m):
        f = [0]
        x, lo, hi = lanes(m, 0, BRAIN), lanes(m, 1, BRAIN), lanes(m, 2, BRAIN)
        out = []
        for i in range(len(x)):
            t = fp_minmax(BRAIN, lo[i], x[i], f, True, True)
            out.append(fp_minmax(BRAIN, t, hi[i], f, False, True))
        put(m, 0, BRAIN, out)
        fold(m, f)
    add(r, 'bfclamp z0.h, z1.h, z2.h', {0: BRAIN, 1: BRAIN, 2: BRAIN}, bfclamp)

    # famax / famin: the magnitudes
    for name, is_max in (('famax', True), ('famin', False)):
        for fmt2, sfx in ((HALF, 'h'), (SINGLE, 's'), (DOUBLE, 'd')):
            def absminmax(m, is_max=is_max, fmt2=fmt2):
                f = [0]
                a, b = lanes(m, 0, fmt2), lanes(m, 2, fmt2)
                sign = 1 << (fmt2.bits - 1)
                out = []
                for i in range(len(a)):
                    if m.active(m.p[1], i, fmt2.bits):
                        out.append(fp_minmax(fmt2, a[i] & ~sign, b[i] & ~sign, f, is_max, False))
                    else:
                        out.append(a[i])
                put(m, 0, fmt2, out)
                fold(m, f)
            add(r, '%s z0.%s, p1/m, z0.%s, z2.%s' % (name, sfx, sfx, sfx), {0: fmt2, 2: fmt2}, absminmax)

    # fmmla: a 2 x 2 matrix of every 128 / 256 bit segment, chained fused multiply adds
    for fmt2, sfx, seg in ((SINGLE, 's', 4), (DOUBLE, 'd', 4)):
        def mmla(m, fmt2=fmt2, seg=seg):
            f = [0]
            a, b, c = lanes(m, 1, fmt2), lanes(m, 2, fmt2), lanes(m, 0, fmt2)
            out = list(c)
            for base in range(0, len(a), seg):
                for i in range(2):
                    for j in range(2):
                        s = c[base + 2 * i + j]
                        for k in range(2):
                            s = fp_fma(fmt2, a[base + 2 * i + k], b[base + 2 * j + k], s, f)
                        out[base + 2 * i + j] = s
            put(m, 0, fmt2, out)
            fold(m, f)
        add(r, 'fmmla z0.%s, z1.%s, z2.%s' % (sfx, sfx, sfx), {0: fmt2, 1: fmt2, 2: fmt2}, mmla,
            vls=(128, 256, 512) if fmt2 is SINGLE else (256, 512))

    def mmla_undefined(m):
        m.exit = 'trap:undef'
    add(r, 'fmmla z0.d, z1.d, z2.d', {}, mmla_undefined, vls=(128,))

    # frint32z / frint32x / frint64z / frint64x
    for name, bits_n, mode in (('frint32z', 32, 'rz'), ('frint32x', 32, 'rne'), ('frint64z', 64, 'rz'), ('frint64x', 64, 'rne')):
        for fmt2, sfx in ((SINGLE, 's'), (DOUBLE, 'd')):
            def rint(m, bits_n=bits_n, mode=mode, fmt2=fmt2, name=name):
                f = [0]
                a = lanes(m, 1, fmt2)
                out = []
                limit = Fraction(1 << (bits_n - 1))
                minus = round_value(fmt2, -limit)[0]
                for i in range(len(a)):
                    if not m.active(m.p[1], i, fmt2.bits):
                        out.append(0 if False else m.zget(0, fmt2.bits)[i])
                        continue
                    d = decode(fmt2, a[i])
                    if d[0] in ('nan', 'inf'):
                        f[0] |= IOC
                        out.append(minus)
                    elif d[0] == 'zero':
                        out.append(a[i])
                    else:
                        v = d[1]
                        fl = int(v) if v >= 0 else -int(-v)
                        if mode == 'rz':
                            n = fl
                        else:
                            diff = v - (v.numerator // v.denominator)
                            n = v.numerator // v.denominator
                            if diff > Fraction(1, 2) or (diff == Fraction(1, 2) and n & 1):
                                n += 1
                        if n >= limit or n < -limit:
                            f[0] |= IOC
                            out.append(minus)
                        else:
                            if n == 0:
                                out.append(pack(fmt2, 1 if v < 0 else 0, 0, 0))
                            else:
                                out.append(round_value(fmt2, Fraction(n))[0])
                            if name.endswith('x') and n != v:
                                f[0] |= IXC
                put(m, 0, fmt2, out)
                fold(m, f)

            def setup(m, fmt2=fmt2, bits_n=bits_n):
                rng = seeded('rint', m.vl)
                vals = [0.5, -0.5, 1.5, 2.5, -2.5, 3.0, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
                        9.3e18, -9.3e18, 123456.789, -0.2, 1e30, float('inf'), float('nan')]
                import struct
                out = []
                for _ in range(m.lanes(fmt2.bits)):
                    v = rng.choice(vals)
                    if fmt2 is SINGLE:
                        try:
                            out.append(struct.unpack('<I', struct.pack('<f', v))[0])
                        except OverflowError:
                            out.append(infinity(SINGLE, 0))
                    else:
                        out.append(struct.unpack('<Q', struct.pack('<d', v))[0])
                put(m, 1, fmt2, out)
                m.p[1] = M.mask(m.vl // 8)
            add(r, '%s z0.%s, p1/m, z1.%s' % (name, sfx, sfx), {}, rint, setup=setup)

    # scvtflt / ucvtflt: the top integer element of every container
    for name, signed in (('scvtflt', True), ('ucvtflt', False)):
        for fmt2, sfx, src in ((HALF, 'h', 'b'), (SINGLE, 's', 'h'), (DOUBLE, 'd', 's')):
            def cvtlt(m, signed=signed, fmt2=fmt2):
                f = [0]
                half_bits = fmt2.bits // 2
                n = m.zget(1, fmt2.bits)
                out = []
                for i in range(len(n)):
                    top = n[i] >> half_bits
                    v = sext(top, half_bits) if signed else top
                    out.append(round_value(fmt2, Fraction(v))[0] if v else 0)
                    f[0] |= round_value(fmt2, Fraction(v))[1] if v else 0
                put(m, 0, fmt2, out)
                fold(m, f)
            add(r, '%s z0.%s, z1.%s' % (name, sfx, src), {}, cvtlt)

    # the conversions of the SVE2.2 zeroing forms (inactive containers become zero) and bfcvt
    def convert_bits(src, dst, bits, mode, flags):
        d = decode(src, bits)
        if d[0] == 'nan':
            if d[1] == 0:
                flags[0] |= IOC
            frac = d[3]
            shift = dst.m - src.m
            frac = (frac << shift) if shift >= 0 else (frac >> -shift)
            return pack(dst, d[2], dst.emax, quiet(dst, frac) & mask(dst.m))
        if d[0] == 'inf':
            return infinity(dst, d[1])
        if d[0] == 'zero':
            return pack(dst, d[1], 0, 0)
        out, fl = round_value(dst, d[1], mode)
        flags[0] |= fl
        return out

    for name, text_fmt, src, dst, mode, top, brain in (
            ('fcvtnt', 'fcvtnt z0.h, p1/z, z2.s', SINGLE, HALF, 'rne', True, False),
            ('fcvtxnt', 'fcvtxnt z0.s, p1/z, z2.d', DOUBLE, SINGLE, 'rto', True, False),
            ('bfcvtnt', 'bfcvtnt z0.h, p1/z, z2.s', SINGLE, BRAIN, 'rne', True, True),
            ('bfcvt', 'bfcvt z0.h, p1/z, z2.s', SINGLE, BRAIN, 'rne', False, True)):
        def narrow(m, src=src, dst=dst, mode=mode, top=top):
            f = [0]
            cont = src.bits
            a = m.zget(2, cont)
            old = m.zget(0, cont)
            out = []
            for i in range(len(a)):
                if m.active(m.p[1], i, cont):
                    v = convert_bits(src, dst, a[i], mode, f)
                    out.append((old[i] & mask(cont // 2)) | (v << (cont // 2)) if top else v)
                else:
                    out.append(0)
            m.zset(0, cont, out)
            fold(m, f)
        add(r, text_fmt, {2: src}, narrow)

    def fcvtlt(m):
        f = [0]
        a = m.zget(2, 32)
        out = []
        for i in range(len(a)):
            if m.active(m.p[1], i, 32):
                out.append(convert_bits(HALF, SINGLE, a[i] >> 16, 'rne', f))
            else:
                out.append(0)
        m.zset(0, 32, out)
        fold(m, f)
    add(r, 'fcvtlt z0.s, p1/z, z2.h', {}, fcvtlt)

    def flogb(m):
        f = [0]
        a = m.zget(2, 32)
        out = []
        for i in range(len(a)):
            if not m.active(m.p[1], i, 32):
                out.append(0)
                continue
            d = decode(SINGLE, a[i])
            if d[0] in ('nan', 'zero'):
                f[0] |= IOC
                out.append(1 << 31)
            elif d[0] == 'inf':
                out.append(mask(31))
            else:
                v = abs(d[1])
                e = 0
                while v >= 2:
                    v /= 2
                    e += 1
                while v < 1:
                    v *= 2
                    e -= 1
                out.append(e & mask(32))
        m.zset(0, 32, out)
        fold(m, f)
    add(r, 'flogb z0.s, p1/z, z2.s', {2: SINGLE}, flogb)

    def frecpx(m):
        a = m.zget(2, 32)
        f = [0]
        out = []
        for i in range(len(a)):
            if not m.active(m.p[1], i, 32):
                out.append(0)
                continue
            d = decode(SINGLE, a[i])
            if d[0] == 'nan':
                out.append(nan_of(SINGLE, [a[i]], f))
                continue
            exp = (a[i] >> 23) & 255
            new = 254 if exp == 0 else (~exp) & 255
            out.append((a[i] & (1 << 31)) | (new << 23))
        m.zset(0, 32, out)
        fold(m, f)
    add(r, 'frecpx z0.s, p1/z, z2.s', {2: SINGLE}, frecpx)

    # fcvtzsn / fcvtzun: two vectors converted toward zero, saturated, interleaved
    for name, signed in (('fcvtzsn', True), ('fcvtzun', False)):
        for src, dst, sfx, dfx in ((HALF, 8, 'h', 'b'), (SINGLE, 16, 's', 'h'), (DOUBLE, 32, 'd', 's')):
            def narrow_int(m, src=src, dst=dst, signed=signed):
                f = [0]
                first, second = m.zget(10, src.bits), m.zget(11, src.bits)
                out = []
                for i in range(len(first)):
                    for v in (first[i], second[i]):
                        d = decode(src, v)
                        low = -(1 << (dst - 1)) if signed else 0
                        high = (1 << (dst - 1)) - 1 if signed else (1 << dst) - 1
                        if d[0] == 'nan':
                            f[0] |= IOC
                            out.append(0)
                        elif d[0] == 'inf':
                            f[0] |= IOC
                            out.append((low if d[1] else high) & mask(dst))
                        elif d[0] == 'zero':
                            out.append(0)
                        else:
                            n = d[1].numerator // d[1].denominator if d[1] >= 0 else -((-d[1]).numerator // (-d[1]).denominator)
                            if n < low or n > high:
                                f[0] |= IOC
                                n = max(low, min(high, n))
                            elif n != d[1]:
                                f[0] |= IXC
                            out.append(n & mask(dst))
                m.zset(0, dst, out)
                fold(m, f)
            add(r, '%s z0.%s, { z10.%s, z11.%s }' % (name, dfx, sfx, sfx), {10: src, 11: src}, narrow_int)

    # bfdot / bfmmla: the standard bfloat16 behavior: round to odd (an overflow gives the
    # infinity), denormals flushed, default NaN, no exception flags
    def flush(fmt, bits):
        if (bits >> fmt.m) & fmt.emax == 0 and bits & mask(fmt.m):
            return bits & (1 << (fmt.bits - 1))
        return bits

    def odd_op(operation, a, b):
        a, b = flush(SINGLE, a), flush(SINGLE, b)
        flags = [0]
        if operation == 'mul':
            bits = _mul_mode(SINGLE, a, b, flags, 'rto')
        else:
            bits = _add_mode(SINGLE, a, b, flags, 'rto')
        bits = flush(SINGLE, bits)
        d = decode(SINGLE, bits)
        if d[0] == 'nan':
            return default_nan(SINGLE)
        return bits

    def bf_dot(acc, a0, a1, b0, b1):
        p0 = odd_op('mul', a0 << 16, b0 << 16)
        p1 = odd_op('mul', a1 << 16, b1 << 16)
        prod = odd_op('add', p0, p1)
        return odd_op('add', acc, prod)

    def bfdot(m):
        a, b, c = m.zget(1, 16), m.zget(2, 16), m.zget(0, 32)
        out = [bf_dot(c[i], a[2 * i], a[2 * i + 1], b[2 * i], b[2 * i + 1]) for i in range(len(c))]
        m.zset(0, 32, out)
    add(r, 'bfdot z0.s, z1.h, z2.h', {0: SINGLE, 1: BRAIN, 2: BRAIN}, bfdot)

    def bfdot_indexed(m):
        a, b, c = m.zget(1, 16), m.zget(2, 16), m.zget(0, 32)
        out = []
        for i in range(len(c)):
            seg = i // 4
            out.append(bf_dot(c[i], a[2 * i], a[2 * i + 1], b[seg * 8 + 4], b[seg * 8 + 5]))
        m.zset(0, 32, out)
    add(r, 'bfdot z0.s, z1.h, z2.h[2]', {0: SINGLE, 1: BRAIN, 2: BRAIN}, bfdot_indexed)

    def bfmmla(m):
        a, b, c = m.zget(1, 16), m.zget(2, 16), m.zget(0, 32)
        out = list(c)
        for seg in range(len(c) // 4):
            for i in range(2):
                for j in range(2):
                    acc = c[seg * 4 + 2 * i + j]
                    for k in range(2):
                        base_a = seg * 8 + 4 * i + 2 * k
                        base_b = seg * 8 + 4 * j + 2 * k
                        acc = bf_dot(acc, a[base_a], a[base_a + 1], b[base_b], b[base_b + 1])
                    out[seg * 4 + 2 * i + j] = acc
        m.zset(0, 32, out)
    add(r, 'bfmmla z0.s, z1.h, z2.h', {0: SINGLE, 1: BRAIN, 2: BRAIN}, bfmmla)
    return r

