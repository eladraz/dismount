#!/usr/bin/env python3
"""Generate tests/arm/Arm64SimdSemanticRowsInteger.inc, the integer rows of tests/TestArm64SemanticsSimd.cpp:
an Advanced SIMD integer instruction, a start state, and the state it must leave, as the Arm ARM defines them.
The expected values are computed here by an independent model of the pseudocode (arbitrary precision integers,
SignedSatQ / UnsignedSatQ, the shifts with the rounding constant of the Arm ARM...) and not by running dismount.
The model was validated against the M4 (tests/arm/semantics --oracle host); the rows pin the edge cases
(saturation: FPSR.QC = 0x08000000, rounding, the shift counts) and cover every mnemonic.

The words are assembled by clang (tests/arm/semantics/armasm.py).

    gen_simd_semantic_rows_integer.py

Row syntax of the generated file, see tests/TestArm64SemanticsSimd.cpp:
    { {word, ...}, count, "text", "setup", "expected" }
Registers which the expectation does not name must be unchanged.
"""
import os
import random
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))
import armlib
import armasm

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'Arm64SimdSemanticRowsInteger.inc')
QC = 0x08000000
ROWS = []


def mask(bits):
    return (1 << bits) - 1


def sx(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


def lanes(value, e, n):
    return [(value >> (i * e)) & mask(e) for i in range(n)]


def pack(values, e):
    v = 0
    for i, x in enumerate(values):
        v |= (x & mask(e)) << (i * e)
    return v


def sat_signed(x, e):
    lo, hi = -(1 << (e - 1)), (1 << (e - 1)) - 1
    if x > hi:
        return hi, True
    if x < lo:
        return lo, True
    return x, False


def sat_unsigned(x, e):
    hi = (1 << e) - 1
    if x > hi:
        return hi, True
    if x < 0:
        return 0, True
    return x, False


def rshift(x, n, rounding):
    """x >> n with the optional rounding (+ 2^(n-1)) of the Arm ARM, on arbitrary integers (floor)."""
    if rounding and n > 0:
        x += 1 << (n - 1)
    return x >> n


def edge(e):
    m = mask(e)
    smax = (1 << (e - 1)) - 1
    return [0, 1, m, smax, smax + 1, smax + 2, smax - 1, 2, 3, e - 1, e, (-e) & m, 0x55 if e == 8 else int('55' * (e // 8), 16),
            int('aa' * (e // 8), 16), 0x7f, 0x80 & m]


class Rng(object):
    def __init__(self, text):
        self.r = random.Random(zlib.crc32(text.encode()))

    def vec(self, e, n=None):
        n = n or 128 // e
        values = edge(e)
        return pack([self.r.choice(values) if self.r.random() < 0.7 else self.r.getrandbits(e) for _ in range(n)], e)


def row(text, setup, expect):
    ROWS.append((text, setup, expect))


def vtoken(d):
    return d


# ----------------------------------------------------------------------------------------------------------
# The elementwise operations: f(a, b, acc, e) -> (result, saturated)
# ----------------------------------------------------------------------------------------------------------
def f_add(a, b, acc, e): return (a + b) & mask(e), False
def f_sub(a, b, acc, e): return (a - b) & mask(e), False
def f_mul(a, b, acc, e): return (a * b) & mask(e), False
def f_mla(a, b, acc, e): return (acc + a * b) & mask(e), False
def f_mls(a, b, acc, e): return (acc - a * b) & mask(e), False
def f_cmeq(a, b, acc, e): return (mask(e) if a == b else 0), False
def f_cmge(a, b, acc, e): return (mask(e) if sx(a, e) >= sx(b, e) else 0), False
def f_cmgt(a, b, acc, e): return (mask(e) if sx(a, e) > sx(b, e) else 0), False
def f_cmhi(a, b, acc, e): return (mask(e) if a > b else 0), False
def f_cmhs(a, b, acc, e): return (mask(e) if a >= b else 0), False
def f_cmtst(a, b, acc, e): return (mask(e) if a & b else 0), False
def f_smax(a, b, acc, e): return max(sx(a, e), sx(b, e)) & mask(e), False
def f_smin(a, b, acc, e): return min(sx(a, e), sx(b, e)) & mask(e), False
def f_umax(a, b, acc, e): return max(a, b), False
def f_umin(a, b, acc, e): return min(a, b), False
def f_sabd(a, b, acc, e): return abs(sx(a, e) - sx(b, e)) & mask(e), False
def f_uabd(a, b, acc, e): return abs(a - b) & mask(e), False
def f_saba(a, b, acc, e): return (acc + abs(sx(a, e) - sx(b, e))) & mask(e), False
def f_uaba(a, b, acc, e): return (acc + abs(a - b)) & mask(e), False
def f_shadd(a, b, acc, e): return ((sx(a, e) + sx(b, e)) >> 1) & mask(e), False
def f_uhadd(a, b, acc, e): return ((a + b) >> 1) & mask(e), False
def f_srhadd(a, b, acc, e): return ((sx(a, e) + sx(b, e) + 1) >> 1) & mask(e), False
def f_urhadd(a, b, acc, e): return ((a + b + 1) >> 1) & mask(e), False
def f_shsub(a, b, acc, e): return ((sx(a, e) - sx(b, e)) >> 1) & mask(e), False
def f_uhsub(a, b, acc, e): return ((a - b) >> 1) & mask(e), False


def f_sqadd(a, b, acc, e):
    r, s = sat_signed(sx(a, e) + sx(b, e), e)
    return r & mask(e), s


def f_uqadd(a, b, acc, e):
    r, s = sat_unsigned(a + b, e)
    return r, s


def f_sqsub(a, b, acc, e):
    r, s = sat_signed(sx(a, e) - sx(b, e), e)
    return r & mask(e), s


def f_uqsub(a, b, acc, e):
    r, s = sat_unsigned(a - b, e)
    return r, s


def shift_reg(a, b, e, signed, rounding, saturating):
    sh = sx(b & 0xff, 8)
    value = sx(a, e) if signed else a
    if sh >= 0:
        r = value << sh
        if saturating:
            r, s = sat_signed(r, e) if signed else sat_unsigned(r, e)
            return r & mask(e), s
        return r & mask(e), False
    return rshift(value, -sh, rounding) & mask(e), False


def f_sshl(a, b, acc, e): return shift_reg(a, b, e, True, False, False)
def f_ushl(a, b, acc, e): return shift_reg(a, b, e, False, False, False)
def f_srshl(a, b, acc, e): return shift_reg(a, b, e, True, True, False)
def f_urshl(a, b, acc, e): return shift_reg(a, b, e, False, True, False)
def f_sqshl(a, b, acc, e): return shift_reg(a, b, e, True, False, True)
def f_uqshl(a, b, acc, e): return shift_reg(a, b, e, False, False, True)
def f_sqrshl(a, b, acc, e): return shift_reg(a, b, e, True, True, True)
def f_uqrshl(a, b, acc, e): return shift_reg(a, b, e, False, True, True)


def f_sqdmulh(a, b, acc, e):
    r, s = sat_signed((2 * sx(a, e) * sx(b, e)) >> e, e)
    return r & mask(e), s


def f_sqrdmulh(a, b, acc, e):
    r, s = sat_signed((2 * sx(a, e) * sx(b, e) + (1 << (e - 1))) >> e, e)
    return r & mask(e), s


def f_sqrdmlah(a, b, acc, e):
    r, s = sat_signed(((sx(acc, e) << e) + 2 * sx(a, e) * sx(b, e) + (1 << (e - 1))) >> e, e)
    return r & mask(e), s


def f_sqrdmlsh(a, b, acc, e):
    r, s = sat_signed(((sx(acc, e) << e) - 2 * sx(a, e) * sx(b, e) + (1 << (e - 1))) >> e, e)
    return r & mask(e), s


def f_pmul(a, b, acc, e):
    r = 0
    for i in range(e):
        if (b >> i) & 1:
            r ^= a << i
    return r & mask(e), False


SAME = [
    ('add', f_add, [8, 16, 32, 64]), ('sub', f_sub, [8, 16, 32, 64]), ('mul', f_mul, [8, 16, 32]),
    ('mla', f_mla, [8, 16, 32]), ('mls', f_mls, [8, 16, 32]), ('cmeq', f_cmeq, [8, 16, 32, 64]),
    ('cmge', f_cmge, [8, 16, 32, 64]), ('cmgt', f_cmgt, [8, 16, 32, 64]), ('cmhi', f_cmhi, [8, 16, 32, 64]),
    ('cmhs', f_cmhs, [8, 16, 32, 64]), ('cmtst', f_cmtst, [8, 16, 32, 64]), ('smax', f_smax, [8, 16, 32]),
    ('smin', f_smin, [8, 16, 32]), ('umax', f_umax, [8, 16, 32]), ('umin', f_umin, [8, 16, 32]),
    ('sabd', f_sabd, [8, 16, 32]), ('uabd', f_uabd, [8, 16, 32]), ('saba', f_saba, [8, 16, 32]),
    ('uaba', f_uaba, [8, 16, 32]), ('shadd', f_shadd, [8, 16, 32]), ('uhadd', f_uhadd, [8, 16, 32]),
    ('srhadd', f_srhadd, [8, 16, 32]), ('urhadd', f_urhadd, [8, 16, 32]), ('shsub', f_shsub, [8, 16, 32]),
    ('uhsub', f_uhsub, [8, 16, 32]), ('sqadd', f_sqadd, [8, 16, 32, 64]), ('uqadd', f_uqadd, [8, 16, 32, 64]),
    ('sqsub', f_sqsub, [8, 16, 32, 64]), ('uqsub', f_uqsub, [8, 16, 32, 64]), ('sshl', f_sshl, [8, 16, 32, 64]),
    ('ushl', f_ushl, [8, 16, 32, 64]), ('srshl', f_srshl, [8, 16, 32, 64]), ('urshl', f_urshl, [8, 16, 32, 64]),
    ('sqshl', f_sqshl, [8, 16, 32, 64]), ('uqshl', f_uqshl, [8, 16, 32, 64]), ('sqrshl', f_sqrshl, [8, 16, 32, 64]),
    ('uqrshl', f_uqrshl, [8, 16, 32, 64]), ('sqdmulh', f_sqdmulh, [16, 32]), ('sqrdmulh', f_sqrdmulh, [16, 32]),
    ('sqrdmlah', f_sqrdmlah, [16, 32]), ('sqrdmlsh', f_sqrdmlsh, [16, 32]), ('pmul', f_pmul, [8]),
]
LETTER = {8: 'b', 16: 'h', 32: 's', 64: 'd'}


def arrangement(e, q):
    n = (128 if q else 64) // e
    if e == 64 and not q:
        return '1d'
    return '%d%s' % (n, LETTER[e])


def elementwise_rows():
    for name, f, sizes in SAME:
        for e in sizes:
            for q in (1, 0):
                if e == 64 and not q:
                    continue
                if name == 'pmul':
                    pass
                n = (128 if q else 64) // e
                arr = arrangement(e, q)
                text = '%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr)
                if name in ('sqdmulh', 'sqrdmulh', 'sqrdmlah', 'sqrdmlsh') and e == 64:
                    continue
                rng = Rng(text)
                for k in range(2 if q else 1):
                    a, b, acc = rng.vec(e, n), rng.vec(e, n), rng.vec(e, n)
                    # a corner lane: both minimum
                    if k == 1:
                        a = pack([1 << (e - 1)] * n, e)
                        b = pack([1 << (e - 1)] * n, e)
                    out, qc = [], False
                    for x, y, z in zip(lanes(a, e, n), lanes(b, e, n), lanes(acc, e, n)):
                        r, s = f(x, y, z, e)
                        out.append(r)
                        qc = qc or s
                    expect = {'v0': pack(out, e)}
                    if qc:
                        expect['fpsr'] = QC
                    row(text, {'v0': acc, 'v1': a, 'v2': b}, expect)
    # the scalar forms
    for name, f, sizes in SAME:
        if name in ('mla', 'mls', 'smax', 'smin', 'umax', 'umin', 'sabd', 'uabd', 'saba', 'uaba', 'shadd', 'uhadd', 'srhadd',
                    'urhadd', 'shsub', 'uhsub', 'mul', 'pmul', 'sqrdmlah', 'sqrdmlsh') and name not in ('sqrdmlah', 'sqrdmlsh'):
            continue
        for e in sizes:
            if name in ('add', 'sub', 'cmeq', 'cmge', 'cmgt', 'cmhi', 'cmhs', 'cmtst', 'sshl', 'ushl', 'srshl', 'urshl') and e != 64:
                continue
            if name in ('sqdmulh', 'sqrdmulh', 'sqrdmlah', 'sqrdmlsh') and e not in (16, 32):
                continue
            reg = LETTER[e]
            text = '%s %s0, %s1, %s2' % (name, reg, reg, reg)
            rng = Rng(text)
            for k in range(2):
                a, b, acc = rng.vec(e, 1), rng.vec(e, 1), rng.vec(e, 1)
                if k == 1:
                    a = b = 1 << (e - 1)
                r, s = f(a, b, acc, e)
                expect = {'v0': r}
                if s:
                    expect['fpsr'] = QC
                row(text, {'v0': acc, 'v1': a, 'v2': b}, expect)


# By element
def by_element_rows():
    for name, f, sizes in (('mul', f_mul, [16, 32]), ('mla', f_mla, [16, 32]), ('mls', f_mls, [16, 32]),
                           ('sqdmulh', f_sqdmulh, [16, 32]), ('sqrdmulh', f_sqrdmulh, [16, 32]),
                           ('sqrdmlah', f_sqrdmlah, [16, 32]), ('sqrdmlsh', f_sqrdmlsh, [16, 32])):
        for e in sizes:
            n = 128 // e
            arr = arrangement(e, 1)
            for index in (0, 3 if e == 16 else 1):
                text = '%s v0.%s, v1.%s, v2.%s[%d]' % (name, arr, arr, LETTER[e], index)
                rng = Rng(text)
                a, m, acc = rng.vec(e), rng.vec(e), rng.vec(e)
                b = lanes(m, e, n)[index]
                out, qc = [], False
                for x, z in zip(lanes(a, e, n), lanes(acc, e, n)):
                    r, s = f(x, b, z, e)
                    out.append(r)
                    qc = qc or s
                expect = {'v0': pack(out, e)}
                if qc:
                    expect['fpsr'] = QC
                row(text, {'v0': acc, 'v1': a, 'v2': m}, expect)
            # scalar by element
            if name.startswith('sq'):
                reg = LETTER[e]
                text = '%s %s0, %s1, v2.%s[1]' % (name, reg, reg, LETTER[e])
                rng = Rng(text)
                a, m, acc = rng.vec(e, 1), rng.vec(e), rng.vec(e, 1)
                b = lanes(m, e, 128 // e)[1]
                r, s = f(a, b, acc, e)
                expect = {'v0': r}
                if s:
                    expect['fpsr'] = QC
                row(text, {'v0': acc, 'v1': a, 'v2': m}, expect)


# ----------------------------------------------------------------------------------------------------------
# Unary
# ----------------------------------------------------------------------------------------------------------
def clz(a, e):
    return e - a.bit_length()


def cls(a, e):
    s = sx(a, e)
    t = s if s >= 0 else ~s
    return e - 1 - t.bit_length()


def u_abs(a, acc, e): return abs(sx(a, e)) & mask(e), False
def u_neg(a, acc, e): return (-a) & mask(e), False


def u_sqabs(a, acc, e):
    r, s = sat_signed(abs(sx(a, e)), e)
    return r & mask(e), s


def u_sqneg(a, acc, e):
    r, s = sat_signed(-sx(a, e), e)
    return r & mask(e), s


def u_suqadd(a, acc, e):
    r, s = sat_signed(sx(acc, e) + a, e)
    return r & mask(e), s


def u_usqadd(a, acc, e):
    r, s = sat_unsigned(acc + sx(a, e), e)
    return r, s


UNARY = [
    ('abs', u_abs, [8, 16, 32, 64]), ('neg', u_neg, [8, 16, 32, 64]), ('sqabs', u_sqabs, [8, 16, 32, 64]),
    ('sqneg', u_sqneg, [8, 16, 32, 64]), ('cls', lambda a, c, e: (cls(a, e), False), [8, 16, 32]),
    ('clz', lambda a, c, e: (clz(a, e), False), [8, 16, 32]), ('suqadd', u_suqadd, [8, 16, 32, 64]),
    ('usqadd', u_usqadd, [8, 16, 32, 64]),
]


def unary_rows():
    for name, f, sizes in UNARY:
        for e in sizes:
            for q in (1, 0):
                if e == 64 and not q:
                    continue
                n = (128 if q else 64) // e
                arr = arrangement(e, q)
                text = '%s v0.%s, v1.%s' % (name, arr, arr)
                rng = Rng(text)
                for k in range(2):
                    a, acc = rng.vec(e, n), rng.vec(e, n)
                    if k == 1:
                        a = pack([1 << (e - 1)] * n, e)
                    out, qc = [], False
                    for x, z in zip(lanes(a, e, n), lanes(acc, e, n)):
                        r, s = f(x, z, e)
                        out.append(r)
                        qc = qc or s
                    expect = {'v0': pack(out, e)}
                    if qc:
                        expect['fpsr'] = QC
                    row(text, {'v0': acc, 'v1': a}, expect)
            if name in ('abs', 'neg', 'sqabs', 'sqneg', 'suqadd', 'usqadd') and e in (8, 16, 32, 64) and \
                    (e == 64 or name.startswith('sq') or name.endswith('qadd')):
                reg = LETTER[e]
                text = '%s %s0, %s1' % (name, reg, reg)
                rng = Rng(text)
                a, acc = rng.vec(e, 1), rng.vec(e, 1)
                if e == 64 or name.startswith('sq') or name.endswith('qadd'):
                    a = 1 << (e - 1)
                r, s = f(a, acc, e)
                expect = {'v0': r}
                if s:
                    expect['fpsr'] = QC
                row(text, {'v0': acc, 'v1': a}, expect)
    # cnt, rbit, not, rev
    for q in (1, 0):
        arr = '16b' if q else '8b'
        n = 16 if q else 8
        for name, g in (('cnt', lambda x: bin(x).count('1')), ('rbit', lambda x: int('{:08b}'.format(x)[::-1], 2)),
                        ('not', lambda x: x ^ 0xff)):
            text = '%s v0.%s, v1.%s' % (name, arr, arr)
            rng = Rng(text)
            a = rng.vec(8, n)
            row(text, {'v1': a}, {'v0': pack([g(x) for x in lanes(a, 8, n)], 8)})
    for name, e, container in (('rev16', 8, 16), ('rev32', 8, 32), ('rev32', 16, 32), ('rev64', 8, 64), ('rev64', 16, 64), ('rev64', 32, 64)):
        for q in (1, 0):
            n = (128 if q else 64) // e
            arr = arrangement(e, q)
            text = '%s v0.%s, v1.%s' % (name, arr, arr)
            rng = Rng(text)
            a = rng.vec(e, n)
            per = container // e
            src = lanes(a, e, n)
            out = [src[(i // per) * per + (per - 1 - i % per)] for i in range(n)]
            row(text, {'v1': a}, {'v0': pack(out, e)})
    # compares with zero
    for name, pred in (('cmeq', lambda a, e: a == 0), ('cmge', lambda a, e: sx(a, e) >= 0), ('cmgt', lambda a, e: sx(a, e) > 0),
                       ('cmle', lambda a, e: sx(a, e) <= 0), ('cmlt', lambda a, e: sx(a, e) < 0)):
        for e in (8, 16, 32, 64):
            n = 128 // e
            arr = arrangement(e, 1)
            text = '%s v0.%s, v1.%s, #0' % (name, arr, arr)
            rng = Rng(text)
            a = rng.vec(e, n)
            row(text, {'v1': a}, {'v0': pack([mask(e) if pred(x, e) else 0 for x in lanes(a, e, n)], e)})
        text = '%s d0, d1, #0' % name
        a = Rng(text).vec(64, 1)
        row(text, {'v1': a}, {'v0': mask(64) if pred(a, 64) else 0})
    # urecpe, ursqrte (the estimate tables of the Arm ARM)
    def recip(a):
        if not (a >> 31):
            return 0xffffffff
        x = (a >> 23) & 0x1ff
        r = ((1 << 19) // (x * 2 + 1) + 1) // 2
        return (r & 0x1ff) << 23

    def rsqrt(a):
        if (a >> 30) == 0:
            return 0xffffffff
        x = (a >> 23) & 0x1ff
        if x < 256:
            x = x * 2 + 1
        else:
            x = ((x >> 1) << 1 + 0) if False else (x >> 1) << 1
            x = (x + 1) * 2
        b = 512
        while x * (b + 1) * (b + 1) < (1 << 28):
            b += 1
        return (((b + 1) // 2) & 0x1ff) << 23
    for name, g in (('urecpe', recip), ('ursqrte', rsqrt)):
        text = '%s v0.4s, v1.4s' % name
        a = pack([0x80000000, 0xc0000000, 0xffffffff, 0x40000000], 32)
        row(text, {'v1': a}, {'v0': pack([g(x) for x in lanes(a, 32, 4)], 32)})
        a = pack([0x00000001, 0x7fffffff, 0xa5a5a5a5, 0x3fffffff], 32)
        row(text, {'v1': a}, {'v0': pack([g(x) for x in lanes(a, 32, 4)], 32)})


# ----------------------------------------------------------------------------------------------------------
# Long, wide and narrow
# ----------------------------------------------------------------------------------------------------------
def long_rows():
    ops = [
        ('saddl', lambda a, b, acc, e: sx(a, e) + sx(b, e), True), ('uaddl', lambda a, b, acc, e: a + b, False),
        ('ssubl', lambda a, b, acc, e: sx(a, e) - sx(b, e), True), ('usubl', lambda a, b, acc, e: a - b, False),
        ('sabdl', lambda a, b, acc, e: abs(sx(a, e) - sx(b, e)), True), ('uabdl', lambda a, b, acc, e: abs(a - b), False),
        ('sabal', lambda a, b, acc, e: acc + abs(sx(a, e) - sx(b, e)), True),
        ('uabal', lambda a, b, acc, e: acc + abs(a - b), False),
        ('smull', lambda a, b, acc, e: sx(a, e) * sx(b, e), True), ('umull', lambda a, b, acc, e: a * b, False),
        ('smlal', lambda a, b, acc, e: acc + sx(a, e) * sx(b, e), True), ('umlal', lambda a, b, acc, e: acc + a * b, False),
        ('smlsl', lambda a, b, acc, e: acc - sx(a, e) * sx(b, e), True), ('umlsl', lambda a, b, acc, e: acc - a * b, False),
    ]
    for name, f, signed in ops:
        for e in (8, 16, 32):
            w = 2 * e
            for upper in (0, 1):
                n = 64 // e
                narrow = arrangement(e, upper)
                wide = arrangement(w, 1)
                text = '%s%s v0.%s, v1.%s, v2.%s' % (name, '2' if upper else '', wide, narrow, narrow)
                rng = Rng(text)
                a, b, acc = rng.vec(e), rng.vec(e), rng.vec(w)
                al, bl, cl = lanes(a, e, 128 // e), lanes(b, e, 128 // e), lanes(acc, w, n)
                base = n if upper else 0
                out = [f(al[base + i], bl[base + i], sx(cl[i], w) if signed else cl[i], e) & mask(w) for i in range(n)]
                row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': pack(out, w)})
    for name, f in (('saddw', lambda a, b, e: sx(a, 2 * e) + sx(b, e)), ('uaddw', lambda a, b, e: a + b),
                    ('ssubw', lambda a, b, e: sx(a, 2 * e) - sx(b, e)), ('usubw', lambda a, b, e: a - b)):
        for e in (8, 16, 32):
            w = 2 * e
            for upper in (0, 1):
                n = 64 // e
                narrow = arrangement(e, upper)
                text = '%s%s v0.%s, v1.%s, v2.%s' % (name, '2' if upper else '', arrangement(w, 1), arrangement(w, 1), narrow)
                rng = Rng(text)
                a, b = rng.vec(w), rng.vec(e)
                al, bl = lanes(a, w, n), lanes(b, e, 128 // e)
                base = n if upper else 0
                out = [f(al[i], bl[base + i], e) & mask(w) for i in range(n)]
                row(text, {'v1': a, 'v2': b}, {'v0': pack(out, w)})
    # sqdmull, sqdmlal, sqdmlsl: the saturation of the doubling and of the accumulation
    for name in ('sqdmull', 'sqdmlal', 'sqdmlsl'):
        for e in (16, 32):
            w = 2 * e
            for upper in (0, 1):
                n = 64 // e
                narrow = arrangement(e, upper)
                text = '%s%s v0.%s, v1.%s, v2.%s' % (name, '2' if upper else '', arrangement(w, 1), narrow, narrow)
                rng = Rng(text)
                for k in range(2):
                    a, b, acc = rng.vec(e), rng.vec(e), rng.vec(w)
                    if k == 1:
                        a = b = pack([1 << (e - 1)] * (128 // e), e)
                        acc = pack([(1 << (w - 1)) - 5] * (128 // w), w)
                    al, bl, cl = lanes(a, e, 128 // e), lanes(b, e, 128 // e), lanes(acc, w, n)
                    base = n if upper else 0
                    out, qc = [], False
                    for i in range(n):
                        p, s1 = sat_signed(2 * sx(al[base + i], e) * sx(bl[base + i], e), w)
                        if name == 'sqdmull':
                            r, s2 = p, False
                        elif name == 'sqdmlal':
                            r, s2 = sat_signed(sx(cl[i], w) + p, w)
                        else:
                            r, s2 = sat_signed(sx(cl[i], w) - p, w)
                        out.append(r & mask(w))
                        qc = qc or s1 or s2
                    expect = {'v0': pack(out, w)}
                    if qc:
                        expect['fpsr'] = QC
                    row(text, {'v0': acc, 'v1': a, 'v2': b}, expect)
        # scalar and by element
        text = '%s s0, h1, h2' % name
        rng = Rng(text)
        a, b, acc = rng.vec(16, 1), rng.vec(16, 1), rng.vec(32, 1)
        a = b = 0x8000
        p, s1 = sat_signed(2 * sx(a, 16) * sx(b, 16), 32)
        acc = 0x7ffffff0
        if name == 'sqdmull':
            r, s2 = p, False
        elif name == 'sqdmlal':
            r, s2 = sat_signed(sx(acc, 32) + p, 32)
        else:
            r, s2 = sat_signed(sx(acc, 32) - p, 32)
        row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': r & mask(32), 'fpsr': QC})
        text = '%s v0.4s, v1.4h, v2.h[2]' % name
        rng = Rng(text)
        a, m, acc = rng.vec(16), rng.vec(16), rng.vec(32)
        b = lanes(m, 16, 8)[2]
        out, qc = [], False
        for i in range(4):
            p, s1 = sat_signed(2 * sx(lanes(a, 16, 8)[i], 16) * sx(b, 16), 32)
            if name == 'sqdmull':
                r, s2 = p, False
            elif name == 'sqdmlal':
                r, s2 = sat_signed(sx(lanes(acc, 32, 4)[i], 32) + p, 32)
            else:
                r, s2 = sat_signed(sx(lanes(acc, 32, 4)[i], 32) - p, 32)
            out.append(r & mask(32))
            qc = qc or s1 or s2
        expect = {'v0': pack(out, 32)}
        if qc:
            expect['fpsr'] = QC
        row(text, {'v0': acc, 'v1': a, 'v2': m}, expect)
    # long by element
    for name, f, signed in (('smull', lambda a, b, acc: a * b, True), ('umlal', lambda a, b, acc: acc + a * b, False),
                            ('smlsl', lambda a, b, acc: acc - a * b, True)):
        text = '%s v0.4s, v1.4h, v2.h[5]' % name
        rng = Rng(text)
        a, m, acc = rng.vec(16), rng.vec(16), rng.vec(32)
        b = lanes(m, 16, 8)[5]
        al = lanes(a, 16, 8)
        cl = lanes(acc, 32, 4)
        conv = (lambda v, e: sx(v, e)) if signed else (lambda v, e: v)
        out = [f(conv(al[i], 16), conv(b, 16), conv(cl[i], 32)) & mask(32) for i in range(4)]
        row(text, {'v0': acc, 'v1': a, 'v2': m}, {'v0': pack(out, 32)})


def hn_rows():
    for name, sub, rnd in (('addhn', False, False), ('raddhn', False, True), ('subhn', True, False), ('rsubhn', True, True)):
        for w in (16, 32, 64):
            e = w // 2
            n = 128 // w
            for upper in (0, 1):
                text = '%s%s v0.%s, v1.%s, v2.%s' % (name, '2' if upper else '', arrangement(e, upper), arrangement(w, 1), arrangement(w, 1))
                rng = Rng(text)
                a, b, old = rng.vec(w), rng.vec(w), rng.vec(e)
                out = []
                for x, y in zip(lanes(a, w, n), lanes(b, w, n)):
                    v = (x - y) if sub else (x + y)
                    if rnd:
                        v += 1 << (e - 1)
                    out.append((v & mask(w)) >> e)
                if upper:
                    result = (old & mask(64)) | (pack(out, e) << 64)
                else:
                    result = pack(out, e)
                row(text, {'v0': old, 'v1': a, 'v2': b}, {'v0': result})


def narrow_rows():
    # xtn, sqxtn, uqxtn, sqxtun
    for name, g in (('xtn', lambda x, e: (x & mask(e), False)),
                    ('sqxtn', lambda x, e: tuple(sat_signed(x, e))),
                    ('uqxtn', None), ('sqxtun', lambda x, e: tuple(sat_unsigned(x, e)))):
        for w in (16, 32, 64):
            e = w // 2
            n = 128 // w
            for upper in (0, 1):
                text = '%s%s v0.%s, v1.%s' % (name, '2' if upper else '', arrangement(e, upper), arrangement(w, 1))
                rng = Rng(text)
                a, old = rng.vec(w), rng.vec(e)
                out, qc = [], False
                for x in lanes(a, w, n):
                    if name == 'xtn':
                        r, s = g(x, e)
                    elif name == 'sqxtn':
                        r, s = g(sx(x, w), e)
                    elif name == 'uqxtn':
                        r, s = sat_unsigned(x, e)
                    else:
                        r, s = g(sx(x, w), e)
                    out.append(r & mask(e))
                    qc = qc or s
                result = ((old & mask(64)) | (pack(out, e) << 64)) if upper else pack(out, e)
                expect = {'v0': result}
                if qc:
                    expect['fpsr'] = QC
                row(text, {'v0': old, 'v1': a}, expect)
    # scalar narrow
    for name, w in (('sqxtn', 16), ('uqxtn', 32), ('sqxtun', 64)):
        e = w // 2
        text = '%s %s0, %s1' % (name, LETTER[e], LETTER[w])
        a = mask(w) - 1
        if name == 'sqxtn':
            r, s = sat_signed(sx(a, w), e)
        elif name == 'uqxtn':
            r, s = sat_unsigned(a, e)
        else:
            r, s = sat_unsigned(sx(a, w), e)
        row(text, {'v1': a}, {'v0': r & mask(e), 'fpsr': QC} if s else {'v0': r & mask(e)})
    # the shifts with a narrowing
    specs = [('shrn', False, False, 'plain'), ('rshrn', True, False, 'plain'), ('sqshrn', False, True, 'signed'),
             ('sqrshrn', True, True, 'signed'), ('uqshrn', False, True, 'unsigned'), ('uqrshrn', True, True, 'unsigned'),
             ('sqshrun', False, True, 'sun'), ('sqrshrun', True, True, 'sun')]
    for name, rnd, sat, kind in specs:
        for w in (16, 32, 64):
            e = w // 2
            n = 128 // w
            for upper in (0, 1):
                for shift in (1, e):
                    text = '%s%s v0.%s, v1.%s, #%d' % (name, '2' if upper else '', arrangement(e, upper), arrangement(w, 1), shift)
                    rng = Rng(text)
                    a, old = rng.vec(w), rng.vec(e)
                    out, qc = [], False
                    for x in lanes(a, w, n):
                        v = sx(x, w) if kind in ('signed', 'sun') else x
                        v = rshift(v, shift, rnd)
                        if kind == 'plain':
                            r, s = v & mask(e), False
                        elif kind == 'signed':
                            r, s = sat_signed(v, e)
                        elif kind == 'unsigned':
                            r, s = sat_unsigned(v, e)
                        else:
                            r, s = sat_unsigned(v, e)
                        out.append(r & mask(e))
                        qc = qc or s
                    result = ((old & mask(64)) | (pack(out, e) << 64)) if upper else pack(out, e)
                    expect = {'v0': result}
                    if qc:
                        expect['fpsr'] = QC
                    row(text, {'v0': old, 'v1': a}, expect)
    # sqshrn scalar
    text = 'sqshrn h0, s1, #3'
    row(text, {'v1': 0x7fffffff}, {'v0': 0x7fff, 'fpsr': QC})


# ----------------------------------------------------------------------------------------------------------
# Shifts by an immediate
# ----------------------------------------------------------------------------------------------------------
def shift_imm_rows():
    for e in (8, 16, 32, 64):
        n = 128 // e
        arr = arrangement(e, 1)
        for shift in sorted(set([1, e // 2, e])):
            def rows_for(name, f, limit_left=False):
                text = '%s v0.%s, v1.%s, #%d' % (name, arr, arr, shift)
                rng = Rng(text)
                a, acc = rng.vec(e), rng.vec(e)
                out, qc = [], False
                for x, z in zip(lanes(a, e, n), lanes(acc, e, n)):
                    r, s = f(x, z)
                    out.append(r & mask(e))
                    qc = qc or s
                expect = {'v0': pack(out, e)}
                if qc:
                    expect['fpsr'] = QC
                row(text, {'v0': acc, 'v1': a}, expect)
            rows_for('sshr', lambda x, z: (sx(x, e) >> shift, False))
            rows_for('ushr', lambda x, z: (x >> shift, False))
            rows_for('srshr', lambda x, z: (rshift(sx(x, e), shift, True), False))
            rows_for('urshr', lambda x, z: (rshift(x, shift, True), False))
            rows_for('ssra', lambda x, z: (z + (sx(x, e) >> shift), False))
            rows_for('usra', lambda x, z: (z + (x >> shift), False))
            rows_for('srsra', lambda x, z: (z + rshift(sx(x, e), shift, True), False))
            rows_for('ursra', lambda x, z: (z + rshift(x, shift, True), False))
            rows_for('sri', lambda x, z: (((x >> shift) | (z & ~(mask(e) >> shift))) & mask(e), False))
            if shift < e:
                rows_for('shl', lambda x, z: ((x << shift) & mask(e), False))
                rows_for('sli', lambda x, z: (((x << shift) | (z & mask(shift))) & mask(e), False))
                rows_for('sqshl', lambda x, z: (lambda t: (t[0] & mask(e), t[1]))(sat_signed(sx(x, e) << shift, e)))
                rows_for('uqshl', lambda x, z: sat_unsigned(x << shift, e))
                rows_for('sqshlu', lambda x, z: sat_unsigned(sx(x, e) << shift, e))
        # scalar d
    for name, f in (('shl', lambda x, z: (x << 3) & mask(64)), ('ushr', lambda x, z: x >> 3), ('sshr', lambda x, z: (sx(x, 64) >> 3) & mask(64)),
                    ('ssra', lambda x, z: (z + (sx(x, 64) >> 3)) & mask(64)), ('srshr', lambda x, z: rshift(sx(x, 64), 3, True) & mask(64)),
                    ('sri', lambda x, z: ((x >> 3) | (z & ~(mask(64) >> 3))) & mask(64))):
        text = '%s d0, d1, #3' % name
        rng = Rng(text)
        a, acc = rng.vec(64, 1), rng.vec(64, 1)
        row(text, {'v0': acc, 'v1': a}, {'v0': f(a, acc)})
    # sqshl scalar b
    text = 'sqshl b0, b1, #1'
    row(text, {'v1': 0x70}, {'v0': 0x7f, 'fpsr': QC})
    text = 'uqshl h0, h1, #4'
    row(text, {'v1': 0x1000}, {'v0': 0xffff, 'fpsr': QC})
    text = 'sqshlu s0, s1, #2'
    row(text, {'v1': 0x80000000}, {'v0': 0, 'fpsr': QC})
    # widening shifts
    for name, signed in (('sshll', True), ('ushll', False)):
        for e in (8, 16, 32):
            w = 2 * e
            n = 64 // e
            for upper in (0, 1):
                for shift in (0 if False else 1, e - 1):
                    text = '%s%s v0.%s, v1.%s, #%d' % (name, '2' if upper else '', arrangement(w, 1), arrangement(e, upper), shift)
                    rng = Rng(text)
                    a = rng.vec(e)
                    base = n if upper else 0
                    src = lanes(a, e, 128 // e)
                    out = [((sx(src[base + i], e) if signed else src[base + i]) << shift) & mask(w) for i in range(n)]
                    row(text, {'v1': a}, {'v0': pack(out, w)})
    for name, signed in (('sxtl', True), ('uxtl', False)):
        for e in (8, 16, 32):
            w = 2 * e
            n = 64 // e
            for upper in (0, 1):
                text = '%s%s v0.%s, v1.%s' % (name, '2' if upper else '', arrangement(w, 1), arrangement(e, upper))
                rng = Rng(text)
                a = rng.vec(e)
                base = n if upper else 0
                src = lanes(a, e, 128 // e)
                out = [(sx(src[base + i], e) if signed else src[base + i]) & mask(w) for i in range(n)]
                row(text, {'v1': a}, {'v0': pack(out, w)})
    for e in (8, 16, 32):
        w = 2 * e
        n = 64 // e
        for upper in (0, 1):
            text = 'shll%s v0.%s, v1.%s, #%d' % ('2' if upper else '', arrangement(w, 1), arrangement(e, upper), e)
            rng = Rng(text)
            a = rng.vec(e)
            base = n if upper else 0
            src = lanes(a, e, 128 // e)
            row(text, {'v1': a}, {'v0': pack([(src[base + i] << e) & mask(w) for i in range(n)], w)})


# ----------------------------------------------------------------------------------------------------------
# Pairwise, across lanes, permutes, moves, dot products
# ----------------------------------------------------------------------------------------------------------
def pairwise_rows():
    ops = [('addp', lambda a, b, e: (a + b) & mask(e)), ('smaxp', lambda a, b, e: max(sx(a, e), sx(b, e)) & mask(e)),
           ('sminp', lambda a, b, e: min(sx(a, e), sx(b, e)) & mask(e)), ('umaxp', lambda a, b, e: max(a, b)),
           ('uminp', lambda a, b, e: min(a, b))]
    for name, f in ops:
        for e in (8, 16, 32, 64):
            if e == 64 and name != 'addp':
                continue
            for q in (1, 0):
                if e == 64 and not q:
                    continue
                n = (128 if q else 64) // e
                arr = arrangement(e, q)
                text = '%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr)
                rng = Rng(text)
                a, b = rng.vec(e, n), rng.vec(e, n)
                cat = lanes(a, e, n) + lanes(b, e, n)
                # the pairs of Vn:Vm: the low half of the result from Vn
                out = [f(cat[2 * i], cat[2 * i + 1], e) for i in range(n)]
                # Vn pairs first, then Vm pairs
                out = [f(lanes(a, e, n)[2 * i], lanes(a, e, n)[2 * i + 1], e) for i in range(n // 2)] + \
                      [f(lanes(b, e, n)[2 * i], lanes(b, e, n)[2 * i + 1], e) for i in range(n // 2)]
                row(text, {'v1': a, 'v2': b}, {'v0': pack(out, e)})
    text = 'addp d0, v1.2d'
    a = pack([0xffffffffffffffff, 2], 64)
    row(text, {'v1': a}, {'v0': 1})
    # long pairs
    for name, signed, acc_form in (('saddlp', True, False), ('uaddlp', False, False), ('sadalp', True, True), ('uadalp', False, True)):
        for e in (8, 16, 32):
            w = 2 * e
            for q in (1, 0):
                n = (128 if q else 64) // e
                text = '%s v0.%s, v1.%s' % (name, arrangement(w, q) if q else arrangement(w, 0) if w != 64 else '1d', arrangement(e, q))
                rng = Rng(text)
                a, acc = rng.vec(e, n), rng.vec(w, n // 2)
                src = lanes(a, e, n)
                old = lanes(acc, w, n // 2)
                out = []
                for i in range(n // 2):
                    s = (sx(src[2 * i], e) + sx(src[2 * i + 1], e)) if signed else (src[2 * i] + src[2 * i + 1])
                    if acc_form:
                        s += old[i]
                    out.append(s & mask(w))
                row(text, {'v0': acc, 'v1': a}, {'v0': pack(out, w)})
    # across lanes
    for name, f, long_form, signed in (('addv', lambda v, e: sum(v) & mask(e), False, False),
                                       ('saddlv', lambda v, e: sum(sx(x, e) for x in v) & mask(2 * e), True, True),
                                       ('uaddlv', lambda v, e: sum(v) & mask(2 * e), True, False),
                                       ('smaxv', lambda v, e: max(sx(x, e) for x in v) & mask(e), False, True),
                                       ('sminv', lambda v, e: min(sx(x, e) for x in v) & mask(e), False, True),
                                       ('umaxv', lambda v, e: max(v), False, False), ('uminv', lambda v, e: min(v), False, False)):
        for e in (8, 16, 32):
            for q in (1, 0):
                n = (128 if q else 64) // e
                if n < 4 and not (e == 32 and n == 4):
                    if n < 4:
                        continue
                if e == 32 and not q:
                    continue
                if e == 32 and long_form:
                    out_reg = 'd'
                else:
                    out_reg = LETTER[2 * e] if long_form else LETTER[e]
                text = '%s %s0, v1.%s' % (name, out_reg, arrangement(e, q))
                rng = Rng(text)
                a = rng.vec(e, n)
                row(text, {'v1': a}, {'v0': f(lanes(a, e, n), e)})


def permute_rows():
    for e in (8, 16, 32, 64):
        for q in (1, 0):
            if e == 64 and not q:
                continue
            n = (128 if q else 64) // e
            arr = arrangement(e, q)
            for name in ('zip1', 'zip2', 'uzp1', 'uzp2', 'trn1', 'trn2'):
                text = '%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr)
                rng = Rng(text)
                a, b = rng.vec(e, n), rng.vec(e, n)
                A, B = lanes(a, e, n), lanes(b, e, n)
                part = int(name[-1]) - 1
                if name.startswith('zip'):
                    base = part * (n // 2)
                    out = []
                    for i in range(n // 2):
                        out += [A[base + i], B[base + i]]
                elif name.startswith('uzp'):
                    cat = A + B
                    out = [cat[2 * i + part] for i in range(n)]
                else:
                    out = []
                    for i in range(n // 2):
                        out += [A[2 * i + part], B[2 * i + part]]
                row(text, {'v1': a, 'v2': b}, {'v0': pack(out, e)})
    for q in (1, 0):
        n = 16 if q else 8
        arr = '16b' if q else '8b'
        for index in (0, 3, n - 1):
            if index >= n:
                continue
            text = 'ext v0.%s, v1.%s, v2.%s, #%d' % (arr, arr, arr, index)
            rng = Rng(text)
            a, b = rng.vec(8, n), rng.vec(8, n)
            cat = lanes(a, 8, n) + lanes(b, 8, n)
            row(text, {'v1': a, 'v2': b}, {'v0': pack(cat[index:index + n], 8)})
    # dup, ins, umov, smov
    for e in (8, 16, 32, 64):
        n = 128 // e
        arr = arrangement(e, 1)
        reg = 'x1' if e == 64 else 'w1'
        text = 'dup v0.%s, %s' % (arr, reg)
        row(text, {'x1': 0x1122334455667788}, {'v0': pack([0x1122334455667788 & mask(e)] * n, e)})
        text = 'dup v0.%s, v1.%s[%d]' % (arr, LETTER[e], n - 1)
        rng = Rng(text)
        a = rng.vec(e)
        row(text, {'v1': a}, {'v0': pack([lanes(a, e, n)[n - 1]] * n, e)})
        text = 'ins v0.%s[%d], %s' % (LETTER[e], 1, reg)
        rng = Rng(text)
        old = rng.vec(e)
        L = lanes(old, e, n)
        L[1] = 0x1122334455667788 & mask(e)
        row(text, {'v0': old, 'x1': 0x1122334455667788}, {'v0': pack(L, e)})
        text = 'ins v0.%s[%d], v1.%s[%d]' % (LETTER[e], 0, LETTER[e], n - 1)
        rng = Rng(text)
        old, a = rng.vec(e), rng.vec(e)
        L = lanes(old, e, n)
        L[0] = lanes(a, e, n)[n - 1]
        row(text, {'v0': old, 'v1': a}, {'v0': pack(L, e)})
        if e < 64:
            text = 'umov w0, v1.%s[%d]' % (LETTER[e], n - 1)
            rng = Rng(text)
            a = rng.vec(e)
            row(text, {'v1': a}, {'x0': lanes(a, e, n)[n - 1]})
            if e < 64:
                text = 'smov %s0, v1.%s[%d]' % ('x' if e < 64 else 'w', LETTER[e], 1)
                a = rng.vec(e)
                row(text, {'v1': a}, {'x0': sx(lanes(a, e, n)[1], e) & mask(64)})
                if e < 32:
                    text = 'smov w0, v1.%s[%d]' % (LETTER[e], 1)
                    row(text, {'v1': a}, {'x0': sx(lanes(a, e, n)[1], e) & mask(32)})
        else:
            text = 'umov x0, v1.d[1]'
            a = Rng(text).vec(64)
            row(text, {'v1': a}, {'x0': lanes(a, 64, 2)[1]})
    # tbl, tbx
    for count in (1, 2, 3, 4):
        regs = ', '.join('v%d.16b' % (1 + i) for i in range(count))
        for name in ('tbl', 'tbx'):
            text = '%s v0.16b, { %s }, v8.16b' % (name, regs)
            rng = Rng(text)
            tabs = [rng.vec(8, 16) for _ in range(count)]
            idx = pack([0, 1, 15, 16, 31, 32, 47, 48, 63, 64, 255, 5, 20, 40, 60, 2], 8)
            old = rng.vec(8, 16)
            table = []
            for t in tabs:
                table += lanes(t, 8, 16)
            out = []
            for i, x in enumerate(lanes(idx, 8, 16)):
                if x < 16 * count:
                    out.append(table[x])
                else:
                    out.append(lanes(old, 8, 16)[i] if name == 'tbx' else 0)
            setup = {'v0': old, 'v8': idx}
            for i, t in enumerate(tabs):
                setup['v%d' % (1 + i)] = t
            row(text, setup, {'v0': pack(out, 8)})
    text = 'tbl v0.8b, { v1.16b }, v8.8b'
    rng = Rng(text)
    t = rng.vec(8, 16)
    idx = pack([0, 15, 16, 3, 255, 7, 8, 9], 8)
    row(text, {'v1': t, 'v8': idx}, {'v0': pack([lanes(t, 8, 16)[x] if x < 16 else 0 for x in lanes(idx, 8, 8)], 8)})


def logic_rows():
    for q in (1, 0):
        n = 16 if q else 8
        arr = '16b' if q else '8b'
        m = mask(8 * n)
        rng = Rng('logic%d' % q)
        a, b, old = rng.vec(8, n), rng.vec(8, n), rng.vec(8, n)
        for name, g in (('and', lambda: a & b), ('orr', lambda: a | b), ('eor', lambda: a ^ b), ('bic', lambda: a & ~b & m),
                        ('orn', lambda: a | (~b & m)), ('bsl', lambda: (old & a) | (~old & b & m)),
                        ('bit', lambda: (old & ~b & m) | (a & b)), ('bif', lambda: (old & b) | (a & ~b & m))):
            text = '%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr)
            row(text, {'v0': old, 'v1': a, 'v2': b}, {'v0': g()})
        text = 'mov v0.%s, v1.%s' % (arr, arr)
        row(text, {'v1': a}, {'v0': a})
    # modified immediates
    for text, g in (
        ('movi v0.4s, #0x12', lambda old: pack([0x12] * 4, 32)), ('movi v0.4s, #0x12, lsl #16', lambda old: pack([0x120000] * 4, 32)),
        ('movi v0.2s, #0x12, msl #8', lambda old: pack([0x12ff] * 2, 32)), ('movi v0.8h, #0x34, lsl #8', lambda old: pack([0x3400] * 8, 16)),
        ('movi v0.16b, #0xa5', lambda old: pack([0xa5] * 16, 8)), ('movi v0.2d, #0xff00ff00ff00ff00', lambda old: pack([0xff00ff00ff00ff00] * 2, 64)),
        ('movi d0, #0x00ff00ff00ff00ff', lambda old: 0x00ff00ff00ff00ff), ('mvni v0.4s, #0x12, lsl #8', lambda old: pack([(~0x1200) & mask(32)] * 4, 32)),
        ('mvni v0.4h, #0x12', lambda old: pack([(~0x12) & mask(16)] * 4, 16)), ('mvni v0.4s, #0x12, msl #16', lambda old: pack([(~0x12ffff) & mask(32)] * 4, 32)),
        ('orr v0.4s, #0x12, lsl #8', lambda old: pack([x | 0x1200 for x in lanes(old, 32, 4)], 32)),
        ('bic v0.8h, #0x12', lambda old: pack([x & ~0x12 for x in lanes(old, 16, 8)], 16)),
        ('bic v0.2s, #0x12, lsl #24', lambda old: pack([x & ~0x12000000 for x in lanes(old & mask(64), 32, 2)], 32))):
        old = Rng(text).vec(32)
        row(text, {'v0': old}, {'v0': g(old)})


def dot_rows():
    for name, sa, sb in (('sdot', True, True), ('udot', False, False), ('usdot', False, True)):
        for q in (1, 0):
            n = 4 if q else 2
            text = '%s v0.%ds, v1.%db, v2.%db' % (name, n, n * 4, n * 4)
            rng = Rng(text)
            a, b, acc = rng.vec(8, 4 * n), rng.vec(8, 4 * n), rng.vec(32, n)
            A, B, C = lanes(a, 8, 4 * n), lanes(b, 8, 4 * n), lanes(acc, 32, n)
            out = []
            for i in range(n):
                s = C[i]
                for j in range(4):
                    s += (sx(A[4 * i + j], 8) if sa else A[4 * i + j]) * (sx(B[4 * i + j], 8) if sb else B[4 * i + j])
                out.append(s & mask(32))
            row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': pack(out, 32)})
        text = '%s v0.4s, v1.16b, v2.4b[%d]' % (name, 2)
        rng = Rng(text)
        a, b, acc = rng.vec(8), rng.vec(8), rng.vec(32)
        A, B, C = lanes(a, 8, 16), lanes(b, 8, 16), lanes(acc, 32, 4)
        out = []
        for i in range(4):
            s = C[i]
            for j in range(4):
                s += (sx(A[4 * i + j], 8) if sa else A[4 * i + j]) * (sx(B[8 + j], 8) if sb else B[8 + j])
            out.append(s & mask(32))
        row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': pack(out, 32)})
    text = 'sudot v0.4s, v1.16b, v2.4b[1]'
    rng = Rng(text)
    a, b, acc = rng.vec(8), rng.vec(8), rng.vec(32)
    A, B, C = lanes(a, 8, 16), lanes(b, 8, 16), lanes(acc, 32, 4)
    out = []
    for i in range(4):
        s = C[i]
        for j in range(4):
            s += sx(A[4 * i + j], 8) * B[4 + j]
        out.append(s & mask(32))
    row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': pack(out, 32)})
    for name, sa, sb in (('smmla', True, True), ('ummla', False, False), ('usmmla', False, True)):
        text = '%s v0.4s, v1.16b, v2.16b' % name
        rng = Rng(text)
        a, b, acc = rng.vec(8), rng.vec(8), rng.vec(32)
        A, B, C = lanes(a, 8, 16), lanes(b, 8, 16), lanes(acc, 32, 4)
        out = []
        for i in range(2):
            for j in range(2):
                s = C[2 * i + j]
                for k in range(8):
                    s += (sx(A[8 * i + k], 8) if sa else A[8 * i + k]) * (sx(B[8 * j + k], 8) if sb else B[8 * j + k])
                out.append(s & mask(32))
        row(text, {'v0': acc, 'v1': a, 'v2': b}, {'v0': pack(out, 32)})


def emit():
    words = armasm.assemble('a64', [r[0] for r in ROWS])
    lines = ['// Generated by tests/arm/tools/gen_simd_semantic_rows_integer.py: the integer rows of tests/TestArm64SemanticsSimd.cpp.',
             '// { {words}, count, "text", "setup", "expected" }',
             '#define ARM64_SIMD_ROWS_INTEGER \\']
    for (text, setup, expect), (hexb, err) in zip(ROWS, words):
        if err:
            raise SystemExit('%s: %s' % (text, err))
        word = int.from_bytes(bytes.fromhex(hexb), 'little')

        def tokens(d):
            parts = []
            for key, value in d.items():
                parts.append('%s=%x' % (key, value))
            return ' '.join(parts)
        lines.append('    { {0x%08x}, 1, "%s", "%s", "%s" }, \\' % (word, text, tokens(setup), tokens(expect)))
    lines.append('')
    open(OUTPUT, 'w').write('\n'.join(lines) + '\n')
    print('%d rows' % len(ROWS))


def main():
    elementwise_rows()
    by_element_rows()
    unary_rows()
    long_rows()
    hn_rows()
    narrow_rows()
    shift_imm_rows()
    pairwise_rows()
    permute_rows()
    logic_rows()
    dot_rows()
    emit()


if __name__ == '__main__':
    main()
