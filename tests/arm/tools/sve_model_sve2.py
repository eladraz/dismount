"""The model of the SVE2 integer instructions of Arm64SemanticLiftSve2.cpp, written from the pseudocode
of the Arm ARM (see sve_model.py). The rows cover the instructions no oracle runs (subp, bext, bdep, bgrp,
histcnt, histseg, match, nmatch, the zeroing forms of sqabs and sqneg) and the corner cases of the ones the
Apple M4 runs, at the vector lengths 128 and 256 which it cannot."""
import sve_model as M
from sve_model import Row, mask, sext, predicate_test
from sve_model_core import add_row, machine

VLS = (128, 256, 512)


def sat_signed(v, e):
    return max(-(1 << (e - 1)), min((1 << (e - 1)) - 1, v))


def sat_unsigned(v, e):
    return max(0, min((1 << e) - 1, v))


def pext(x, m, e):
    out = 0
    k = 0
    for i in range(e):
        if (m >> i) & 1:
            out |= ((x >> i) & 1) << k
            k += 1
    return out


def pdep(x, m, e):
    out = 0
    k = 0
    for i in range(e):
        if (m >> i) & 1:
            out |= ((x >> k) & 1) << i
            k += 1
    return out


def merging(esize, fn, source=2):
    """Zdn = fn(Zdn, Zm) on the active lanes (predicate p1)."""
    def model(m):
        a, b = m.zget(0, esize), m.zget(source, esize)
        m.zset(0, esize, [fn(a[i], b[i], esize) if m.active(m.p[1], i, esize) else a[i]
                          for i in range(m.lanes(esize))])
    return model


def shift_variable(a, count, e, signed, rounding, saturating):
    """The Arm ARM SRSHL / URSHL / SQSHL / UQSHL / SQRSHL / UQRSHL of one element, count signed."""
    x = sext(a, e) if signed else a
    n = sext(count, e)
    n = max(-(e + 2), min(e + 2, n))
    if n >= 0:
        r = x << n
        if saturating:
            r = sat_signed(r, e) if signed else sat_unsigned(r, e)
        return r & mask(e)
    n = -n
    if rounding:
        r = (x + (1 << (n - 1))) >> n
    else:
        r = x >> n
    return r & mask(e)


def estimates():
    def recip(a):
        a = a * 2 + 1
        b = (1 << 19) // a
        return (b + 1) // 2

    def rsqrt(a):
        if a < 256:
            a = a * 2 + 1
        else:
            a = (a >> 1) << 1
            a = (a + 1) * 2
        b = 512
        while a * (b + 1) * (b + 1) < (1 << 28):
            b += 1
        return (b + 1) // 2
    return recip, rsqrt


def rows():
    r = []
    # the shifts by a vector: the shift amount is the whole signed element
    for name, signed, rounding, saturating in [('srshl', 1, 1, 0), ('urshl', 0, 1, 0), ('sqshl', 1, 0, 1),
                                               ('uqshl', 0, 0, 1), ('sqrshl', 1, 1, 1), ('uqrshl', 0, 1, 1)]:
        for e, suffix in ((8, 'b'), (16, 'h'), (32, 's'), (64, 'd')):
            def fn(a, b, esize, signed=signed, rounding=rounding, saturating=saturating):
                return shift_variable(a, b, esize, signed, rounding, saturating)

            def setup(m, e=e):
                # the counts around the element size, negative and positive, with garbage above the byte
                counts = [0, 1, e - 1, e, e + 1, mask(e), mask(e) - e + 1, (1 << (e - 1)), (1 << (e - 1)) + 1,
                          (1 << (e - 1)) - 1, 3, mask(e) - 2]
                m.zset(2, e, [counts[i % len(counts)] for i in range(m.lanes(e))])
                m.p[1] = M.mask(m.vl // 8)
            add_row(r, '%s z0.%s, p1/m, z0.%s, z2.%s' % (name, suffix, suffix, suffix), 'sv%s%d' % (name, e),
                    merging(e, fn), setup=setup)
            def setup_reversed(m, e=e):
                counts = [0, 1, e - 1, e, e + 1, mask(e), mask(e) - e + 1, (1 << (e - 1)), 3, mask(e) - 2]
                m.zset(0, e, [counts[i % len(counts)] for i in range(m.lanes(e))])
                m.p[1] = M.mask(m.vl // 8)

            def fn_r(a, b, esize, signed=signed, rounding=rounding, saturating=saturating):
                return shift_variable(b, a, esize, signed, rounding, saturating)
            add_row(r, '%sr z0.%s, p1/m, z0.%s, z2.%s' % (name, suffix, suffix, suffix), 'sr%s%d' % (name, e),
                    merging(e, fn_r), vls=(128, 256), setup=setup_reversed)

    # the immediate forms
    def immediate(esize, fn, amount):
        def model(m):
            a = m.zget(0, esize)
            m.zset(0, esize, [fn(a[i], amount, esize) if m.active(m.p[1], i, esize) else a[i]
                              for i in range(m.lanes(esize))])
        return model
    add_row(r, 'sqshl z0.b, p1/m, z0.b, #3', 'sqi', immediate(8, lambda a, n, e: sat_signed(sext(a, e) << n, e) & mask(e), 3),
            vls=(128, 256))
    add_row(r, 'uqshl z0.h, p1/m, z0.h, #9', 'uqi', immediate(16, lambda a, n, e: sat_unsigned(a << n, e) & mask(e), 9),
            vls=(128, 256))
    add_row(r, 'sqshlu z0.s, p1/m, z0.s, #4', 'sqsu', immediate(32, lambda a, n, e: sat_unsigned(sext(a, e) << n, e), 4),
            vls=(128, 256))
    add_row(r, 'srshr z0.d, p1/m, z0.d, #7', 'srs', immediate(64, lambda a, n, e: ((sext(a, e) + (1 << (n - 1))) >> n) & mask(e), 7),
            vls=(128, 256))
    add_row(r, 'urshr z0.b, p1/m, z0.b, #8', 'urs', immediate(8, lambda a, n, e: ((a + (1 << (n - 1))) >> n) & mask(e), 8),
            vls=(128, 256))

    def sli(m):
        d, n = m.zget(0, 16), m.zget(1, 16)
        m.zset(0, 16, [((n[i] << 5) & mask(16)) | (d[i] & mask(5)) for i in range(m.lanes(16))])
    add_row(r, 'sli z0.h, z1.h, #5', 'sli', sli, vls=(128, 256))

    def sri(m):
        d, n = m.zget(0, 8), m.zget(1, 8)
        m.zset(0, 8, [(n[i] >> 3) | (d[i] & (mask(8) & ~(mask(8) >> 3))) for i in range(m.lanes(8))])
    add_row(r, 'sri z0.b, z1.b, #3', 'sri', sri, vls=(128, 256))

    def srsra(m):
        d, n = m.zget(0, 32), m.zget(1, 32)
        m.zset(0, 32, [(d[i] + ((sext(n[i], 32) + (1 << 5)) >> 6)) & mask(32) for i in range(m.lanes(32))])
    add_row(r, 'srsra z0.s, z1.s, #6', 'srsra', srsra, vls=(128, 256))

    # halving, with the extremes
    def extremes(m):
        e = 16
        vals = [0x7fff, 0x8000, 0xffff, 0, 1, 0x8001, 0x7ffe, 0xfffe]
        m.zset(0, e, [vals[i % 8] for i in range(m.lanes(e))])
        m.zset(2, e, [vals[(i * 3 + 1) % 8] for i in range(m.lanes(e))])
        m.p[1] = M.mask(m.vl // 8)
    for name, fn, e in [('shadd', lambda a, b, e: ((sext(a, e) + sext(b, e)) >> 1) & mask(e), 16),
                        ('uhadd', lambda a, b, e: (a + b) >> 1, 16),
                        ('srhadd', lambda a, b, e: ((sext(a, e) + sext(b, e) + 1) >> 1) & mask(e), 16),
                        ('urhadd', lambda a, b, e: (a + b + 1) >> 1, 16),
                        ('shsub', lambda a, b, e: ((sext(a, e) - sext(b, e)) >> 1) & mask(e), 16),
                        ('uhsub', lambda a, b, e: ((a - b) >> 1) & mask(e), 16),
                        ('shsubr', lambda a, b, e: ((sext(b, e) - sext(a, e)) >> 1) & mask(e), 16),
                        ('uhsubr', lambda a, b, e: ((b - a) >> 1) & mask(e), 16)]:
        add_row(r, '%s z0.h, p1/m, z0.h, z2.h' % name, 'h' + name, merging(e, fn), vls=(128, 256), setup=extremes)

    # pairwise: the pairs of each source interleaved
    def pairwise(esize, fn):
        def model(m):
            a, b = m.zget(0, esize), m.zget(2, esize)
            out = []
            for i in range(m.lanes(esize)):
                if m.active(m.p[1], i, esize):
                    src = a if i % 2 == 0 else b
                    j = i - (i % 2)
                    out.append(fn(src[j], src[j + 1], esize))
                else:
                    out.append(a[i])
            m.zset(0, esize, out)
        return model
    add_row(r, 'addp z0.s, p1/m, z0.s, z2.s', 'padd', pairwise(32, lambda x, y, e: (x + y) & mask(e)), vls=(128, 256))
    add_row(r, 'smaxp z0.b, p1/m, z0.b, z2.b', 'psmax', pairwise(8, lambda x, y, e: x if sext(x, e) > sext(y, e) else y),
            vls=(128, 256))
    add_row(r, 'uminp z0.h, p1/m, z0.h, z2.h', 'pumin', pairwise(16, lambda x, y, e: min(x, y)), vls=(128, 256))
    add_row(r, 'subp z0.s, p1/m, z0.s, z2.s', 'psub', pairwise(32, lambda x, y, e: (x - y) & mask(e)))

    # sadalp, uadalp
    def adalp(signed):
        def model(m):
            d, n = m.zget(0, 32), m.zget(1, 16)
            out = []
            for i in range(m.lanes(32)):
                lo, hi = n[2 * i], n[2 * i + 1]
                if signed:
                    lo, hi = sext(lo, 16), sext(hi, 16)
                out.append((d[i] + lo + hi) & mask(32) if m.active(m.p[2], i, 32) else d[i])
            m.zset(0, 32, out)
        return model
    add_row(r, 'sadalp z0.s, p2/m, z1.h', 'sad', adalp(True), vls=(128, 256))
    add_row(r, 'uadalp z0.s, p2/m, z1.h', 'uad', adalp(False), vls=(128, 256))

    # sqabs, sqneg: merging and zeroing
    def saturating_unary(esize, is_abs, zeroing):
        def model(m):
            a = m.zget(1, esize)
            d = m.zget(0, esize)
            out = []
            for i in range(m.lanes(esize)):
                if m.active(m.p[2], i, esize):
                    v = sext(a[i], esize)
                    v = abs(v) if is_abs else -v
                    out.append(sat_signed(v, esize) & mask(esize))
                else:
                    out.append(0 if zeroing else d[i])
            m.zset(0, esize, out)
        return model

    def edge_unary(m):
        e = 8
        m.zset(1, e, [0x80, 0x7f, 0, 1, 0xff, 0x81, 0x80, 0x40][:8] * (m.lanes(e) // 8))
    for name, is_abs in (('sqabs', True), ('sqneg', False)):
        add_row(r, '%s z0.b, p2/m, z1.b' % name, 'u' + name, saturating_unary(8, is_abs, False), setup=edge_unary)
        add_row(r, '%s z0.b, p2/z, z1.b' % name, 'z' + name, saturating_unary(8, is_abs, True), setup=edge_unary)

    # the reciprocal estimates
    recip, rsqrt = estimates()

    def unsigned_estimate(fn_estimate, ones_below, zeroing=False):
        def model(m):
            a, d = m.zget(1, 32), m.zget(0, 32)
            out = []
            for i in range(m.lanes(32)):
                if m.active(m.p[2], i, 32):
                    if a[i] < ones_below:
                        out.append(0xffffffff)
                    else:
                        out.append(((fn_estimate(a[i] >> 23)) & 0x1ff) << 23)
                else:
                    out.append(0 if zeroing else d[i])
            m.zset(0, 32, out)
        return model

    def estimate_values(m):
        vals = [0, 0x7fffffff, 0x80000000, 0xffffffff, 0x40000000, 0x5fffffff, 0xa5a5a5a5, 0x90000000,
                0x3fffffff, 0xc0000000, 0x81234567, 0x7fffffff]
        m.zset(1, 32, [vals[i % len(vals)] for i in range(m.lanes(32))])
    add_row(r, 'urecpe z0.s, p2/m, z1.s', 'urecpe', unsigned_estimate(recip, 0x80000000), setup=estimate_values)
    add_row(r, 'ursqrte z0.s, p2/m, z1.s', 'ursqrte', unsigned_estimate(rsqrt, 0x40000000), setup=estimate_values)
    add_row(r, 'urecpe z0.s, p2/z, z1.s', 'urecpez', unsigned_estimate(recip, 0x80000000, True), setup=estimate_values)
    add_row(r, 'ursqrte z0.s, p2/z, z1.s', 'ursqrtez', unsigned_estimate(rsqrt, 0x40000000, True), setup=estimate_values)

    # saturating doubling multiplies, with the minimum times the minimum
    def doubling(esize, rounding, accumulate=0):
        def model(m):
            a, b, d = m.zget(1, esize), m.zget(2, esize), m.zget(0, esize)
            out = []
            for i in range(m.lanes(esize)):
                p = sext(a[i], esize) * sext(b[i], esize)
                if accumulate == 0:
                    v = (2 * p + ((1 << (esize - 1)) if rounding else 0)) >> esize
                    out.append(sat_signed(v, esize) & mask(esize))
                else:
                    acc = sext(d[i], esize)
                    sign = 1 if accumulate > 0 else -1
                    v = ((acc << esize) + sign * 2 * p + (1 << (esize - 1))) >> esize
                    out.append(sat_signed(v, esize) & mask(esize))
            m.zset(0, esize, out)
        return model

    def doubling_values(esize):
        def setup(m):
            e = esize
            vals = [1 << (e - 1), mask(e), 1, (1 << (e - 1)) - 1, 0, 3 << (e - 3), (1 << (e - 1)) + 1, 5]
            m.zset(1, e, [vals[i % 8] for i in range(m.lanes(e))])
            m.zset(2, e, [vals[(i * 5 + 3) % 8] for i in range(m.lanes(e))])
            m.zset(0, e, [vals[(i * 7 + 1) % 8] for i in range(m.lanes(e))])
            for i in range(0, m.lanes(e), 8):
                m.zget(1, e)
        return setup
    for e, suffix in ((8, 'b'), (16, 'h'), (32, 's'), (64, 'd')):
        add_row(r, 'sqdmulh z0.%s, z1.%s, z2.%s' % ((suffix,) * 3), 'dm%d' % e, doubling(e, False),
                vls=(128, 256), setup=doubling_values(e))
        add_row(r, 'sqrdmulh z0.%s, z1.%s, z2.%s' % ((suffix,) * 3), 'rdm%d' % e, doubling(e, True),
                vls=(128, 256), setup=doubling_values(e))
        add_row(r, 'sqrdmlah z0.%s, z1.%s, z2.%s' % ((suffix,) * 3), 'rda%d' % e, doubling(e, True, 1),
                vls=(128, 256), setup=doubling_values(e))
        add_row(r, 'sqrdmlsh z0.%s, z1.%s, z2.%s' % ((suffix,) * 3), 'rds%d' % e, doubling(e, True, -1),
                vls=(128, 256), setup=doubling_values(e))

    # saba, uaba, sclamp, uclamp
    def aba(signed):
        def model(m):
            d, a, b = m.zget(0, 16), m.zget(1, 16), m.zget(2, 16)
            m.zset(0, 16, [(d[i] + abs((sext(a[i], 16) - sext(b[i], 16)) if signed else (a[i] - b[i]))) & mask(16)
                           for i in range(m.lanes(16))])
        return model
    add_row(r, 'saba z0.h, z1.h, z2.h', 'saba', aba(True), vls=(128, 256))
    add_row(r, 'uaba z0.h, z1.h, z2.h', 'uaba', aba(False), vls=(128, 256))

    def clamp(signed):
        def model(m):
            d, a, b = m.zget(0, 32), m.zget(1, 32), m.zget(2, 32)
            f = (lambda v: sext(v, 32)) if signed else (lambda v: v)
            m.zset(0, 32, [min(max(f(d[i]), f(a[i])), f(b[i])) & mask(32) for i in range(m.lanes(32))])
        return model
    add_row(r, 'sclamp z0.s, z1.s, z2.s', 'scl', clamp(True), vls=(128, 256))
    add_row(r, 'uclamp z0.s, z1.s, z2.s', 'ucl', clamp(False), vls=(128, 256))

    # bitwise
    def bitwise(fn):
        def model(m):
            d, a, k = m.z[0], m.z[1], m.z[2]
            m.z[0] = fn(d, a, k) & mask(m.vl)
        return model
    full = lambda m: mask(m.vl)
    add_row(r, 'eor3 z0.d, z0.d, z1.d, z2.d', 'eor3', bitwise(lambda d, a, k: d ^ a ^ k), vls=(128, 256))
    add_row(r, 'bcax z0.d, z0.d, z1.d, z2.d', 'bcax', bitwise(lambda d, a, k: d ^ (a & ~k)), vls=(128, 256))
    add_row(r, 'bsl z0.d, z0.d, z1.d, z2.d', 'bsl', bitwise(lambda d, a, k: (d & k) | (a & ~k)), vls=(128, 256))
    add_row(r, 'bsl1n z0.d, z0.d, z1.d, z2.d', 'bsl1n', bitwise(lambda d, a, k: (~d & k) | (a & ~k)), vls=(128, 256))
    add_row(r, 'bsl2n z0.d, z0.d, z1.d, z2.d', 'bsl2n', bitwise(lambda d, a, k: (d & k) | (~a & ~k)), vls=(128, 256))
    add_row(r, 'nbsl z0.d, z0.d, z1.d, z2.d', 'nbsl', bitwise(lambda d, a, k: ~((d & k) | (a & ~k))), vls=(128, 256))

    def xar(m):
        d, a = m.zget(0, 16), m.zget(1, 16)
        out = []
        for i in range(m.lanes(16)):
            x = d[i] ^ a[i]
            out.append(((x >> 5) | (x << 11)) & mask(16))
        m.zset(0, 16, out)
    add_row(r, 'xar z0.h, z0.h, z1.h, #5', 'xar', xar, vls=(128, 256))

    def eorbt(m):
        d, n, z = m.zget(0, 16), m.zget(1, 16), m.zget(2, 16)
        m.zset(0, 16, [(n[i] ^ z[i + 1]) if i % 2 == 0 else d[i] for i in range(m.lanes(16))])
    add_row(r, 'eorbt z0.h, z1.h, z2.h', 'eorbt', eorbt, vls=(128, 256))

    def eortb(m):
        d, n, z = m.zget(0, 16), m.zget(1, 16), m.zget(2, 16)
        m.zset(0, 16, [(n[i] ^ z[i - 1]) if i % 2 == 1 else d[i] for i in range(m.lanes(16))])
    add_row(r, 'eortb z0.h, z1.h, z2.h', 'eortb', eortb, vls=(128, 256))

    def pmul(m):
        a, b = m.zget(1, 8), m.zget(2, 8)
        out = []
        for i in range(m.lanes(8)):
            v = 0
            for k in range(8):
                if (b[i] >> k) & 1:
                    v ^= a[i] << k
            out.append(v & 0xff)
        m.zset(0, 8, out)
    add_row(r, 'pmul z0.b, z1.b, z2.b', 'pmul', pmul, vls=(128, 256))

    # the bit permutes
    def bit_permute(esize, kind):
        def model(m):
            a, b = m.zget(1, esize), m.zget(2, esize)
            out = []
            for i in range(m.lanes(esize)):
                if kind == 'bext':
                    out.append(pext(a[i], b[i], esize))
                elif kind == 'bdep':
                    out.append(pdep(a[i], b[i], esize))
                else:
                    ones = bin(b[i]).count('1')
                    out.append(pext(a[i], b[i], esize) | (pext(a[i], ~b[i] & mask(esize), esize) << ones))
            m.zset(0, esize, out)
        return model
    for kind in ('bext', 'bdep', 'bgrp'):
        for e, suffix in ((8, 'b'), (16, 'h'), (32, 's'), (64, 'd')):
            add_row(r, '%s z0.%s, z1.%s, z2.%s' % (kind, suffix, suffix, suffix), kind + str(e), bit_permute(e, kind))

    # carry long
    def carry_long(top, subtract):
        def model(m):
            d, n, z = m.zget(0, 32), m.zget(1, 32), m.zget(2, 32)
            out = list(d)
            for p in range(m.lanes(32) // 2):
                x = d[2 * p]
                y = n[2 * p + (1 if top else 0)]
                if subtract:
                    y = ~y & mask(32)
                c = z[2 * p + 1] & 1
                total = x + y + c
                out[2 * p] = total & mask(32)
                out[2 * p + 1] = total >> 32
            m.zset(0, 32, out)
        return model
    add_row(r, 'adclb z0.s, z1.s, z2.s', 'adclb', carry_long(False, False))
    add_row(r, 'adclt z0.s, z1.s, z2.s', 'adclt', carry_long(True, False))
    add_row(r, 'sbclb z0.s, z1.s, z2.s', 'sbclb', carry_long(False, True))
    add_row(r, 'sbclt z0.s, z1.s, z2.s', 'sbclt', carry_long(True, True))

    # complex integer add and multiply add
    def cadd(esize, rot270, saturating):
        def model(m):
            a, b = m.zget(0, esize), m.zget(1, esize)
            out = []
            for p in range(m.lanes(esize) // 2):
                are, aim, bre, bim = a[2 * p], a[2 * p + 1], b[2 * p], b[2 * p + 1]
                s = (lambda v: sat_signed(v, esize)) if saturating else (lambda v: v)
                if not rot270:
                    re, im = s(sext(are, esize) - sext(bim, esize)), s(sext(aim, esize) + sext(bre, esize))
                else:
                    re, im = s(sext(are, esize) + sext(bim, esize)), s(sext(aim, esize) - sext(bre, esize))
                out += [re & mask(esize), im & mask(esize)]
            m.zset(0, esize, out)
        return model
    add_row(r, 'cadd z0.b, z0.b, z1.b, #90', 'cadd90', cadd(8, False, False), vls=(128, 256))
    add_row(r, 'cadd z0.s, z0.s, z1.s, #270', 'cadd270', cadd(32, True, False), vls=(128, 256))
    add_row(r, 'sqcadd z0.b, z0.b, z1.b, #90', 'sqcadd90', cadd(8, False, True), vls=(128, 256),
            setup=lambda m: (m.zset(0, 8, [0x80, 0x7f] * (m.lanes(8) // 2)), m.zset(1, 8, [0x7f, 0x80] * (m.lanes(8) // 2))))
    add_row(r, 'sqcadd z0.h, z0.h, z1.h, #270', 'sqcadd270', cadd(16, True, True), vls=(128, 256))

    def cmla(esize, rot, accumulate_fn=None):
        def model(m):
            n, mm, d = m.zget(1, esize), m.zget(2, esize), m.zget(0, esize)
            out = []
            for p in range(m.lanes(esize) // 2):
                nre, nim, mre, mim = n[2 * p], n[2 * p + 1], mm[2 * p], mm[2 * p + 1]
                if rot == 0:
                    e1, e2, e3, e4 = nre, mre, nre, mim
                elif rot == 90:
                    e1, e2, e3, e4 = nim, -sext(mim, esize), nim, mre
                elif rot == 180:
                    e1, e2, e3, e4 = nre, -sext(mre, esize), nre, -sext(mim, esize)
                else:
                    e1, e2, e3, e4 = nim, mim, nim, -sext(mre, esize)
                out.append(accumulate_fn(d[2 * p], sext(e1, esize), sext(e2, esize)))
                out.append(accumulate_fn(d[2 * p + 1], sext(e3, esize), sext(e4, esize)))
            m.zset(0, esize, out)
        return model
    for rot in (0, 90, 180, 270):
        add_row(r, 'cmla z0.s, z1.s, z2.s, #%d' % rot, 'cmla%d' % rot,
                cmla(32, rot, lambda acc, x, y: (acc + x * y) & mask(32)), vls=(128, 256))
        add_row(r, 'sqrdcmlah z0.h, z1.h, z2.h, #%d' % rot, 'rcmla%d' % rot,
                cmla(16, rot, lambda acc, x, y: sat_signed(((sext(acc, 16) << 16) + 2 * x * y + (1 << 15)) >> 16, 16) & mask(16)),
                vls=(128, 256))

    # the complex dot product (host derived formulas)
    def cdot(rot):
        def model(m):
            d, n, z = m.zget(0, 32), m.zget(1, 8), m.zget(2, 8)
            out = []
            for lane in range(m.lanes(32)):
                acc = d[lane]
                for p in range(2):
                    nr, ni = sext(n[4 * lane + 2 * p], 8), sext(n[4 * lane + 2 * p + 1], 8)
                    mr, mi = sext(z[4 * lane + 2 * p], 8), sext(z[4 * lane + 2 * p + 1], 8)
                    acc += {0: nr * mr - ni * mi, 90: nr * mi + ni * mr, 180: nr * mr + ni * mi, 270: nr * mi - ni * mr}[rot]
                out.append(acc & mask(32))
            m.zset(0, 32, out)
        return model
    for rot in (0, 90, 180, 270):
        add_row(r, 'cdot z0.s, z1.b, z2.b, #%d' % rot, 'cdot%d' % rot, cdot(rot), vls=(128, 256))

    # histograms and matches
    def histcnt(esize):
        def model(m):
            n, z = m.zget(2, esize), m.zget(3, esize)
            out = []
            for i in range(m.lanes(esize)):
                if m.active(m.p[1], i, esize):
                    out.append(sum(1 for j in range(i + 1) if m.active(m.p[1], j, esize) and n[j] == z[i]))
                else:
                    out.append(0)
            m.zset(0, esize, out)
        return model

    def few_values(esize):
        def setup(m):
            m.zset(2, esize, [i % 3 for i in range(m.lanes(esize))])
            m.zset(3, esize, [(i * 2) % 3 for i in range(m.lanes(esize))])
        return setup
    add_row(r, 'histcnt z0.s, p1/z, z2.s, z3.s', 'hc32', histcnt(32), setup=few_values(32))
    add_row(r, 'histcnt z0.d, p1/z, z2.d, z3.d', 'hc64', histcnt(64), setup=few_values(64))

    def histseg(m):
        n, z = m.zget(1, 8), m.zget(2, 8)
        out = []
        for i in range(m.lanes(8)):
            seg = (i // 16) * 16
            out.append(sum(1 for j in range(16) if n[seg + j] == z[i]))
        m.zset(0, 8, out)
    add_row(r, 'histseg z0.b, z1.b, z2.b', 'hs', histseg,
            setup=lambda m: (m.zset(1, 8, [i % 5 for i in range(m.lanes(8))]), m.zset(2, 8, [(i * 3) % 5 for i in range(m.lanes(8))])))

    def match(esize, negate):
        def model(m):
            n, z = m.zget(2, esize), m.zget(3, esize)
            per = 128 // esize
            bits = []
            for i in range(m.lanes(esize)):
                seg = (i // per) * per
                found = any(n[i] == z[seg + j] for j in range(per))
                bits.append(1 if m.active(m.p[1], i, esize) and (found != negate) else 0)
            m.pset_elems(0, esize, bits)
            m.nzcv = predicate_test(m.vl, m.p[1], m.p[0], esize)
        return model
    for kind, negate in (('match', False), ('nmatch', True)):
        add_row(r, '%s p0.b, p1/z, z2.b, z3.b' % kind, kind + 'b', match(8, negate), setup=few_values(8))
        add_row(r, '%s p0.h, p1/z, z2.h, z3.h' % kind, kind + 'h', match(16, negate), setup=few_values(16))
    return r
