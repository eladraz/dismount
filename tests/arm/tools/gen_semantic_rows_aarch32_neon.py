#!/usr/bin/env python3
"""Generate tests/arm/AArch32SemanticRowsNeon.inc, the Advanced SIMD integer / structure load and store /
cryptography rows of tests/TestArmAArch32Semantics.cpp: an instruction (or a short sequence), a start state
and the state it must leave, as the Arm ARM defines them.

The expected values are computed here by an independent model of the pseudocode of the Arm ARM (exact
Python integers, the saturation by its definition, the AES S-box from the finite field inverse, the SHA
rounds from FIPS 180-4) and never by running dismount, so the unit test checks the lifter against the
architecture also for what Unicorn (the only AArch32 processor oracle) does not implement
(FEAT_I8MM: vsmmla, vummla, vusmmla, vusdot / vsudot by element) or implements differently.

The words are assembled by clang (tests/arm/semantics/armasm.py).

    gen_semantic_rows_aarch32_neon.py

Row syntax: see tests/TestArmAArch32Semantics.cpp.
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))
import armlib
import armasm

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'AArch32SemanticRowsNeon.inc')
BASE = 0x4000

M64 = (1 << 64) - 1
M128 = (1 << 128) - 1
rng = random.Random(0x4e454f4e)
ROWS = []          # (thumb, text, setup dict, expect dict)

QC = 1 << 27


def mask(bits):
    return (1 << bits) - 1


def sx(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


def lanes(value, bits, count):
    return [(value >> (bits * i)) & mask(bits) for i in range(count)]


def join(values, bits):
    out = 0
    for i, v in enumerate(values):
        out |= (v & mask(bits)) << (bits * i)
    return out


def sat_signed(x, bits):
    low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return (min(max(x, low), high), x < low or x > high)


def sat_unsigned(x, bits):
    high = (1 << bits) - 1
    return (min(max(x, 0), high), x < 0 or x > high)


EDGES = {8: [0, 1, 0x7f, 0x80, 0x81, 0xff, 0xfe, 2, 0x40, 0xc0],
         16: [0, 1, 0x7fff, 0x8000, 0x8001, 0xffff, 0xfffe, 0x100, 0x00ff, 0x4000],
         32: [0, 1, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe, 0x10000, 0x40000000, 0xc0000000],
         64: [0, 1, mask(63), 1 << 63, (1 << 63) + 1, M64, M64 - 1, 1 << 32, 1 << 62, 0xc000000000000000]}


def draw(bits, size=128):
    """A register value of `size` bits made of lanes of `bits` bits: edge values and random ones."""
    out = []
    for _ in range(size // bits):
        r = rng.random()
        if r < 0.5:
            out.append(rng.choice(EDGES[bits]))
        elif r < 0.6:
            out.append(rng.choice(EDGES[bits]) ^ 1)
        else:
            out.append(rng.getrandbits(bits))
    return join(out, bits)


def small_shifts(bits, size=128):
    """Shift counts in the low byte of each lane: positive, negative, zero, beyond the lane."""
    out = []
    for _ in range(size // bits):
        out.append(rng.choice([0, 1, 2, 3, bits - 1, bits, bits + 1, 0xff, 0xfe, 0xff - (bits - 1), 0x80,
                               0x7f, rng.getrandbits(8), rng.getrandbits(8)]))
    return join(out, bits)


class State:
    def __init__(self, q, fpscr=0):
        self.q = list(q)
        self.fpscr = fpscr

    def d(self, n):
        return (self.q[n // 2] >> (64 * (n % 2))) & M64

    def set_d(self, n, value):
        shift = 64 * (n % 2)
        self.q[n // 2] = (self.q[n // 2] & ~(M64 << shift)) | ((value & M64) << shift)

    def reg(self, n, size):
        return self.q[n] if size == 128 else self.d(n)

    def set_reg(self, n, size, value):
        if size == 128:
            self.q[n] = value & M128
        else:
            self.set_d(n, value)


def new_state(qs):
    q = [0] * 16
    for n, v in qs.items():
        q[n] = v
    return State(q, rng.choice([0, 0, 0, QC, 0x03000000]))


def snapshot(setup):
    return {k: v for k, v in setup.items()}


def tokens(state, initial, keep=None):
    out = {}
    for n in range(16):
        if state.q[n] != initial.q[n]:
            out['q%d' % n] = state.q[n]
    if state.fpscr != initial.fpscr:
        out['fpscr'] = state.fpscr
    return out


def setup_of(state, extra=None):
    out = {}
    for n in range(16):
        if state.q[n]:
            out['q%d' % n] = state.q[n]
    if state.fpscr:
        out['fpscr'] = state.fpscr
    if extra:
        out.update(extra)
    return out


def row(text, state_before, state_after, extra_setup=None, extra_expect=None, thumb=0):
    setup = setup_of(state_before, extra_setup)
    expect = tokens(state_after, state_before)
    if extra_expect:
        expect.update(extra_expect)
    ROWS.append((thumb, text, setup, expect))


# --------------------------------------------------------------------------------------------
# Same size operations
# --------------------------------------------------------------------------------------------

def lane_binary(base, kind, e, a, b, acc):
    """(result, saturated) of one lane of a same size operation. a, b, acc are unsigned lane values."""
    sa, sb = sx(a, e), sx(b, e)
    A, B = (sa, sb) if kind == 's' else (a, b)
    m = mask(e)
    if base == 'vadd':
        return ((a + b) & m, False)
    if base == 'vsub':
        return ((a - b) & m, False)
    if base == 'vmul':
        if kind == 'p':
            r = 0
            for i in range(e):
                if (b >> i) & 1:
                    r ^= a << i
            return (r & m, False)
        return ((a * b) & m, False)
    if base == 'vmla':
        return ((acc + a * b) & m, False)
    if base == 'vmls':
        return ((acc - a * b) & m, False)
    if base == 'vabd':
        return (abs(A - B) & m, False)
    if base == 'vaba':
        return ((acc + abs(A - B)) & m, False)
    if base == 'vhadd':
        return (((A + B) >> 1) & m, False)
    if base == 'vrhadd':
        return (((A + B + 1) >> 1) & m, False)
    if base == 'vhsub':
        return (((A - B) >> 1) & m, False)
    if base == 'vmax':
        return (max(A, B) & m, False)
    if base == 'vmin':
        return (min(A, B) & m, False)
    if base in ('vqadd', 'vqsub'):
        exact = A + B if base == 'vqadd' else A - B
        value, s = sat_signed(exact, e) if kind == 's' else sat_unsigned(exact, e)
        return (value & m, s)
    if base == 'vtst':
        return (m if (a & b) else 0, False)
    if base == 'vceq':
        return (m if a == b else 0, False)
    if base == 'vcge':
        return (m if A >= B else 0, False)
    if base == 'vcgt':
        return (m if A > B else 0, False)
    if base == 'vcle':
        return (m if sa <= sb else 0, False)
    if base == 'vclt':
        return (m if sa < sb else 0, False)
    raise ValueError(base)


def binary_row(base, t, size, zero=False):
    kind, e = t[0], int(t[1:])
    d_, n_, m_ = (0, 1, 2) if size == 64 else (4, 5, 6)
    st = new_state({d_ // (1 if size == 128 else 2): 0})
    # the registers: d regs 0, 1, 2 (q0, q0 and q1), q regs 4, 5, 6
    if size == 64:
        st = new_state({0: draw(e), 1: draw(e), 2: draw(e)})
        a_reg, b_reg, d_reg = st.d(2), st.d(4), st.d(0)
        text = '%s.%s d0, d2, d4' % (base, t) if not zero else '%s.%s d0, d2, #0' % (base, t)
    else:
        st = new_state({4: draw(e), 5: draw(e), 6: draw(e)})
        a_reg, b_reg, d_reg = st.q[5], st.q[6], st.q[4]
        text = '%s.%s q4, q5, q6' % (base, t) if not zero else '%s.%s q4, q5, #0' % (base, t)
    if base in ('vtst',):
        text = text.replace('.' + t, '.' + t[1:])
    if zero:
        b_reg = 0
    n = size // e
    sat = False
    res = []
    for a, b, acc in zip(lanes(a_reg, e, n), lanes(b_reg, e, n), lanes(d_reg, e, n)):
        r, s = lane_binary(base, 's' if (zero and base in ('vcle', 'vclt')) else t[0], e, a, b, acc)
        res.append(r)
        sat = sat or s
    after = State(st.q, st.fpscr | (QC if sat else 0))
    if size == 64:
        after.set_d(0, join(res, e))
    else:
        after.q[4] = join(res, e)
    row(text, st, after)


for base, types in (('vadd', ['i8', 'i16', 'i32', 'i64']), ('vsub', ['i8', 'i16', 'i32', 'i64']),
                    ('vmul', ['i8', 'i16', 'i32', 'p8']), ('vmla', ['i8', 'i16', 'i32']),
                    ('vmls', ['i8', 'i16', 'i32']),
                    ('vabd', ['s8', 'u16', 's32']), ('vaba', ['u8', 's16', 'u32']),
                    ('vhadd', ['s8', 'u8', 's16', 'u32']), ('vrhadd', ['s8', 'u16', 's32']),
                    ('vhsub', ['s8', 'u8', 'u16', 's32']), ('vmax', ['s8', 'u8', 's16', 'u32']),
                    ('vmin', ['s16', 'u16', 's32', 'u8']),
                    ('vqadd', ['s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64']),
                    ('vqsub', ['s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64']),
                    ('vceq', ['i8', 'i16', 'i32']), ('vcge', ['s8', 'u8', 's32']), ('vcgt', ['s16', 'u16', 'u32'])):
    for t in types:
        # the integer types of the compare / add are spelled i
        tt = t
        for size in (64, 128):
            for _ in range(5):
                if base in ('vadd', 'vsub', 'vmul', 'vmla', 'vmls', 'vceq'):
                    t_model = 's' + t[1:] if t[0] == 'i' else t
                else:
                    t_model = t
                # the model works with 's' for the signed types and anything else for the unsigned ones
                kind = t_model[0]
                e = int(t_model[1:])
                if base == 'vmul' and kind == 'p':
                    pass
                binary_row(base, tt if t[0] != 'i' else tt, size)
for base in ('vtst',):
    for e in (8, 16, 32):
        for size in (64, 128):
            for _ in range(3):
                binary_row(base, 'u%d' % e, size)
for base in ('vceq', 'vcge', 'vcgt', 'vcle', 'vclt'):
    for t in ('s8', 's16', 's32') if base != 'vceq' else ('i8', 'i16', 'i32'):
        for size in (64, 128):
            for _ in range(3):
                binary_row(base, t, size, zero=True)

# --------------------------------------------------------------------------------------------
# Unary
# --------------------------------------------------------------------------------------------


def clz(value, bits):
    n = 0
    for i in range(bits - 1, -1, -1):
        if (value >> i) & 1:
            break
        n += 1
    return n


def cls(value, bits):
    s = (value >> (bits - 1)) & 1
    n = 0
    for i in range(bits - 2, -1, -1):
        if ((value >> i) & 1) != s:
            break
        n += 1
    return n


def unary_row(base, e, size):
    st = new_state({4: draw(e), 5: draw(e)} if size == 128 else {0: draw(e)})
    after = State(st.q, st.fpscr)
    src = st.q[5] if size == 128 else st.d(1)
    text = ('%s.%s q4, q5' if size == 128 else '%s.%s d0, d2') % (base, ('s%d' if base in ('vabs', 'vneg', 'vqabs', 'vqneg', 'vcls') else
                                                                     'i%d' if base == 'vclz' else '%d') % e)
    if size == 64:
        src = st.d(2)
        st = new_state({0: draw(e), 1: draw(e)})
        src = st.d(2)
        after = State(st.q, st.fpscr)
    res = []
    sat = False
    for a in lanes(src, e, size // e):
        s = sx(a, e)
        mn = -(1 << (e - 1))
        if base == 'vabs':
            r = abs(s) & mask(e)
        elif base == 'vneg':
            r = (-s) & mask(e)
        elif base == 'vqabs':
            v, ss = sat_signed(abs(s), e)
            r, sat = v & mask(e), sat or ss
        elif base == 'vqneg':
            v, ss = sat_signed(-s, e)
            r, sat = v & mask(e), sat or ss
        elif base == 'vcls':
            r = cls(a, e)
        elif base == 'vclz':
            r = clz(a, e)
        else:
            r = bin(a).count('1')
        res.append(r)
    if sat:
        after.fpscr |= QC
    value = join(res, e)
    if size == 128:
        after.q[4] = value
    else:
        after.set_d(0, value)
    row(text, st, after)


for base, es in (('vabs', (8, 16, 32)), ('vneg', (8, 16, 32)), ('vqabs', (8, 16, 32)), ('vqneg', (8, 16, 32)),
                 ('vcls', (8, 16, 32)), ('vclz', (8, 16, 32)), ('vcnt', (8,))):
    for e in es:
        for size in (64, 128):
            for _ in range(4):
                unary_row(base, e, size)


# --------------------------------------------------------------------------------------------
# Logic
# --------------------------------------------------------------------------------------------

def logic_row(base, size):
    regs = (0, 2, 4) if size == 64 else (4, 5, 6)
    st = new_state({4: rng.getrandbits(128), 5: rng.getrandbits(128), 6: rng.getrandbits(128),
                    0: rng.getrandbits(128), 1: rng.getrandbits(128)})
    m = mask(size)
    if size == 64:
        d, n, mm = st.d(0), st.d(2), st.d(4)
        text = '%s d0, d2, d4' % base
    else:
        d, n, mm = st.q[4], st.q[5], st.q[6]
        text = '%s q4, q5, q6' % base
    if base == 'vand':
        r = n & mm
    elif base == 'vbic':
        r = n & ~mm
    elif base == 'vorr':
        r = n | mm
    elif base == 'vorn':
        r = n | (~mm & m)
    elif base == 'veor':
        r = n ^ mm
    elif base == 'vbsl':
        r = (d & n) | (~d & mm)
    elif base == 'vbit':
        r = (n & mm) | (d & ~mm)
    else:
        r = (d & mm) | (n & ~mm)
    after = State(st.q, st.fpscr)
    after.set_reg(0 if size == 64 else 4, size, r & m)
    row(text, st, after)


for base in ('vand', 'vbic', 'vorr', 'vorn', 'veor', 'vbsl', 'vbit', 'vbif'):
    for size in (64, 128):
        for _ in range(3):
            logic_row(base, size)


def imm_row(base, t, value, size):
    e = int(t[1:])
    st = new_state({4: rng.getrandbits(128), 0: rng.getrandbits(128), 1: rng.getrandbits(128)})
    pattern = join([value] * (size // e), e)
    cur = st.d(0) if size == 64 else st.q[4]
    if base == 'vorr':
        r = cur | pattern
    elif base == 'vbic':
        r = cur & ~pattern
    else:
        r = ~pattern
    after = State(st.q, st.fpscr)
    after.set_reg(0 if size == 64 else 4, size, r & mask(size))
    row('%s.%s %s, #0x%x' % (base, t, 'd0' if size == 64 else 'q4', value), st, after)


for base in ('vorr', 'vbic', 'vmvn'):
    for t, v in (('i16', 0x12), ('i16', 0x3400), ('i32', 0x56), ('i32', 0x7800), ('i32', 0x90000), ('i32', 0xab000000)):
        for size in (64, 128):
            imm_row(base, t, v, size)
for size in (64, 128):
    imm_row('vmvn', 'i32', 0x12ff, size)
    imm_row('vmvn', 'i32', 0x12ffff, size)

# --------------------------------------------------------------------------------------------
# Long, wide, narrow high
# --------------------------------------------------------------------------------------------


def long_row(base, t):
    kind, e = t[0], int(t[1:])
    w = 2 * e
    n = 64 // e
    st = new_state({4: draw(w), 5: draw(w), 1: draw(e), 2: draw(e)})
    d1, d2 = st.d(2), st.d(4)
    acc = st.q[4] if base in ('vabal', 'vmlal', 'vmlsl') else 0
    big = st.q[5]
    after = State(st.q, st.fpscr)
    res = []
    for i in range(n):
        a = lanes(d1, e, n)[i]
        b = lanes(d2, e, n)[i]
        A = sx(a, e) if kind == 's' else a
        B = sx(b, e) if kind == 's' else b
        if base in ('vaddl', 'vsubl', 'vabdl', 'vabal', 'vmull', 'vmlal', 'vmlsl'):
            if base == 'vaddl':
                r = A + B
            elif base == 'vsubl':
                r = A - B
            elif base == 'vabdl':
                r = abs(A - B)
            elif base == 'vabal':
                r = lanes(st.q[4], w, n)[i] + abs(A - B)
            elif base == 'vmull':
                if kind == 'p':
                    r = 0
                    for k in range(e):
                        if (b >> k) & 1:
                            r ^= a << k
                else:
                    r = A * B
            elif base == 'vmlal':
                r = lanes(st.q[4], w, n)[i] + A * B
            else:
                r = lanes(st.q[4], w, n)[i] - A * B
        else:
            x = lanes(big, w, n)[i]
            r = x + B if base == 'vaddw' else x - B
        res.append(r & mask(w))
    after.q[4] = join(res, w)
    if base in ('vaddw', 'vsubw'):
        row('%s.%s q4, q5, d4' % (base, t), st, after)
    else:
        row('%s.%s q4, d2, d4' % (base, t), st, after)


for base in ('vaddl', 'vsubl', 'vabdl', 'vabal', 'vmull', 'vmlal', 'vmlsl', 'vaddw', 'vsubw'):
    for t in ('s8', 'u8', 's16', 'u16', 's32', 'u32'):
        for _ in range(2):
            long_row(base, t)
for _ in range(4):
    long_row('vmull', 'p8')


def narrow_high_row(base, t):
    w = int(t[1:])
    e = w // 2
    n = 128 // w
    st = new_state({5: draw(w), 6: draw(w), 0: draw(8)})
    res = []
    for a, b in zip(lanes(st.q[5], w, n), lanes(st.q[6], w, n)):
        r = a + b if base in ('vaddhn', 'vraddhn') else a - b
        if base in ('vraddhn', 'vrsubhn'):
            r += 1 << (e - 1)
        res.append((r >> e) & mask(e))
    after = State(st.q, st.fpscr)
    after.set_d(0, join(res, e))
    row('%s.%s d0, q5, q6' % (base, t), st, after)


for base in ('vaddhn', 'vraddhn', 'vsubhn', 'vrsubhn'):
    for t in ('i16', 'i32', 'i64'):
        for _ in range(3):
            narrow_high_row(base, t)

# --------------------------------------------------------------------------------------------
# Saturating doubling
# --------------------------------------------------------------------------------------------


def qdmul_row(base, t, size, by_scalar):
    e = int(t[1:])
    w = 2 * e
    long_ = base in ('vqdmull', 'vqdmlal', 'vqdmlsl')
    st = new_state({4: draw(w if long_ else e), 5: draw(e), 6: draw(e), 1: draw(e), 2: draw(e)})
    sat = False
    res = []
    scal = None
    if by_scalar:
        index = rng.randrange(4 if e == 16 else 2)
        scal = lanes(st.d(4), e, 64 // e)[index]
    n = (64 if long_ else size) // e
    if long_:
        src1 = lanes(st.d(2), e, n)
        src2 = lanes(st.d(4), e, n)
        acc = lanes(st.q[4], w, n)
    else:
        src1 = lanes(st.q[5] if size == 128 else st.d(2), e, n)
        src2 = lanes(st.q[6] if size == 128 else st.d(4), e, n)
        acc = lanes(st.q[4] if size == 128 else st.d(0), e, n)
    for i in range(n):
        a = sx(src1[i], e)
        b = sx(scal if by_scalar else src2[i], e)
        if long_:
            v, s = sat_signed(2 * a * b, w)
            sat = sat or s
            if base == 'vqdmlal':
                v, s2 = sat_signed(sx(acc[i], w) + v, w)
                sat = sat or s2
            elif base == 'vqdmlsl':
                v, s2 = sat_signed(sx(acc[i], w) - v, w)
                sat = sat or s2
            res.append(v & mask(w))
        else:
            if base == 'vqdmulh':
                v, s = sat_signed((2 * a * b) >> e, e)
            elif base == 'vqrdmulh':
                v, s = sat_signed((2 * a * b + (1 << (e - 1))) >> e, e)
            elif base == 'vqrdmlah':
                v, s = sat_signed(((sx(acc[i], e) << e) + 2 * a * b + (1 << (e - 1))) >> e, e)
            else:
                v, s = sat_signed(((sx(acc[i], e) << e) - 2 * a * b + (1 << (e - 1))) >> e, e)
            sat = sat or s
            res.append(v & mask(e))
    after = State(st.q, st.fpscr | (QC if sat else 0))
    if long_:
        after.q[4] = join(res, w)
        second = 'd%d[%d]' % (4, index) if by_scalar else 'd4'
        if by_scalar:
            # the scalar operand d4[index]: the lanes of d4 above
            pass
        row('%s.%s q4, d2, %s' % (base, t, second), st, after)
    else:
        regs = ('q4', 'q5', 'q6') if size == 128 else ('d0', 'd2', 'd4')
        second = ('d4[%d]' % index) if by_scalar else regs[2]
        if size == 128:
            after.q[4] = join(res, e)
        else:
            after.set_d(0, join(res, e))
        row('%s.%s %s, %s, %s' % (base, t, regs[0], regs[1], second), st, after)


for base in ('vqdmulh', 'vqrdmulh', 'vqrdmlah', 'vqrdmlsh'):
    for t in ('s16', 's32'):
        for size in (64, 128):
            for by in (False, True):
                for _ in range(4):
                    qdmul_row(base, t, size, by)
for base in ('vqdmull', 'vqdmlal', 'vqdmlsl'):
    for t in ('s16', 's32'):
        for by in (False, True):
            for _ in range(5):
                qdmul_row(base, t, 64, by)

# --------------------------------------------------------------------------------------------
# Shifts
# --------------------------------------------------------------------------------------------


def shift_right_exact(x, n, rounding):
    if rounding:
        return (x + (1 << (n - 1))) >> n
    return x >> n


def shift_imm_row(base, t, amount, size):
    kind, e = t[0], int(t[1:])
    st = new_state({4: draw(e), 5: draw(e), 0: draw(e), 1: draw(e)})
    reg_d = 'q4' if size == 128 else 'd0'
    reg_m = 'q5' if size == 128 else 'd2'
    src = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
    dst = lanes(st.q[4] if size == 128 else st.d(0), e, size // e)
    m = mask(e)
    sat = False
    res = []
    for a, old in zip(src, dst):
        A = sx(a, e) if kind == 's' else a
        if base in ('vshr', 'vrshr', 'vsra', 'vrsra'):
            r = shift_right_exact(A, amount, base in ('vrshr', 'vrsra'))
            if base in ('vsra', 'vrsra'):
                r += old
            res.append(r & m)
        elif base == 'vshl':
            res.append((a << amount) & m)
        elif base in ('vqshl', 'vqshlu'):
            v = A << amount
            if base == 'vqshlu':
                r, s = sat_unsigned(v, e)
            elif kind == 's':
                r, s = sat_signed(v, e)
            else:
                r, s = sat_unsigned(v, e)
            sat = sat or s
            res.append(r & m)
        elif base == 'vsli':
            keep = mask(e) & ~(mask(e) << amount)
            res.append(((old & keep) | ((a << amount) & m & ~keep)) & m)
        else:
            if amount >= e:
                shifted, top = 0, 0
            else:
                shifted = a >> amount
                top = (mask(e) >> amount)
            res.append(((old & ~top) | shifted) & m)
    after = State(st.q, st.fpscr | (QC if sat else 0))
    value = join(res, e)
    if size == 128:
        after.q[4] = value
    else:
        after.set_d(0, value)
    suffix = t[1:] if base in ('vsli', 'vsri') else t
    row('%s.%s %s, %s, #%d' % (base, suffix, reg_d, reg_m, amount), st, after)


for base, types in (('vshr', ['s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64']),
                    ('vrshr', ['s8', 'u16', 's32', 'u64']), ('vsra', ['s8', 'u16', 's32', 's64']),
                    ('vrsra', ['u8', 's16', 'u32', 'u64'])):
    for t in types:
        e = int(t[1:])
        for amount in (1, e // 2, e):
            for size in (64, 128):
                shift_imm_row(base, t, amount, size)
for t in ('i8', 'i16', 'i32', 'i64'):
    e = int(t[1:])
    for amount in (0, 1, e - 1):
        for size in (64, 128):
            shift_imm_row('vshl', t, amount, size)
for base, types in (('vqshl', ['s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64']),
                    ('vqshlu', ['s8', 's16', 's32', 's64'])):
    for t in types:
        e = int(t[1:])
        for amount in (0, 1, e // 2, e - 1):
            for size in (64, 128):
                for _ in range(2):
                    shift_imm_row(base, t, amount, size)
for base in ('vsli', 'vsri'):
    for e in (8, 16, 32, 64):
        for amount in ((0, 1, e - 1) if base == 'vsli' else (1, e // 2, e)):
            for size in (64, 128):
                shift_imm_row(base, 'u%d' % e, amount, size)


def shift_reg_row(base, t, size):
    kind, e = t[0], int(t[1:])
    st = new_state({4: draw(e), 5: draw(e), 6: small_shifts(e), 0: draw(e), 1: draw(e), 2: small_shifts(e)})
    values = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
    counts = lanes(st.q[6] if size == 128 else st.d(4), e, size // e)
    sat = False
    res = []
    for a, c in zip(values, counts):
        A = sx(a, e) if kind == 's' else a
        shift = sx(c & 0xff, 8)
        if shift >= 0:
            v = A << shift
            if base in ('vqshl', 'vqrshl'):
                v, s = sat_signed(v, e) if kind == 's' else sat_unsigned(v, e)
                sat = sat or s
            res.append(v & mask(e))
        else:
            n = -shift
            if base in ('vrshl', 'vqrshl'):
                v = (A + (1 << (n - 1))) >> n
            else:
                v = A >> n
            res.append(v & mask(e))
    after = State(st.q, st.fpscr | (QC if sat else 0))
    if size == 128:
        after.q[4] = join(res, e)
        row('%s.%s q4, q5, q6' % (base, t), st, after)
    else:
        after.set_d(0, join(res, e))
        row('%s.%s d0, d2, d4' % (base, t), st, after)


for base in ('vshl', 'vrshl', 'vqshl', 'vqrshl'):
    for t in ('s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64'):
        for size in (64, 128):
            for _ in range(3):
                shift_reg_row(base, t, size)


def narrow_shift_row(base, t, amount):
    kind, s = t[0], int(t[1:])
    r_bits = s // 2
    st = new_state({5: draw(s), 0: draw(8)})
    src = lanes(st.q[5], s, 128 // s)
    sat = False
    res = []
    for a in src:
        A = sx(a, s) if kind == 's' else a
        if base in ('vshrn', 'vrshrn'):
            v = shift_right_exact(a, amount, base == 'vrshrn')
            res.append(v & mask(r_bits))
            continue
        v = shift_right_exact(A, amount, base in ('vqrshrn', 'vqrshrun'))
        if base in ('vqshrun', 'vqrshrun') or (base == 'vqshrn' and kind == 'u') or (base == 'vqrshrn' and kind == 'u'):
            r, sa = sat_unsigned(v, r_bits)
        else:
            r, sa = sat_signed(v, r_bits)
        sat = sat or sa
        res.append(r & mask(r_bits))
    after = State(st.q, st.fpscr | (QC if sat else 0))
    after.set_d(0, join(res, r_bits))
    row('%s.%s d0, q5, #%d' % (base, t, amount), st, after)


for base, types in (('vshrn', ['i16', 'i32', 'i64']), ('vrshrn', ['i16', 'i32', 'i64']),
                    ('vqshrn', ['s16', 's32', 's64', 'u16', 'u32', 'u64']),
                    ('vqrshrn', ['s16', 's32', 's64', 'u16', 'u32', 'u64']),
                    ('vqshrun', ['s16', 's32', 's64']), ('vqrshrun', ['s16', 's32', 's64'])):
    for t in types:
        s = int(t[1:])
        for amount in (1, s // 4, s // 2):
            for _ in range(3):
                narrow_shift_row(base, t, amount)


def move_row(base, t):
    kind, s = t[0], int(t[1:])
    st = new_state({5: draw(s), 1: draw(s), 0: draw(8)})
    after = State(st.q, st.fpscr)
    if base == 'vmovl':
        src = lanes(st.d(2), s, 64 // s)
        res = [(sx(a, s) if kind == 's' else a) & mask(2 * s) for a in src]
        after.q[4] = join(res, 2 * s)
        row('vmovl.%s q4, d2' % t, st, after)
    elif base == 'vshll':
        src = lanes(st.d(2), s, 64 // s)
        res = [(a << s) & mask(2 * s) for a in src]
        after.q[4] = join(res, 2 * s)
        row('vshll.%s q4, d2, #%d' % (t, s), st, after)
    else:
        src = lanes(st.q[5], s, 128 // s)
        sat = False
        res = []
        for a in src:
            A = sx(a, s) if kind == 's' else a
            if base == 'vmovn':
                res.append(a & mask(s // 2))
            elif base == 'vqmovun':
                v, sa = sat_unsigned(A, s // 2)
                sat = sat or sa
                res.append(v)
            else:
                v, sa = sat_signed(A, s // 2) if kind == 's' else sat_unsigned(A, s // 2)
                sat = sat or sa
                res.append(v & mask(s // 2))
        after.set_d(0, join(res, s // 2))
        after.fpscr |= QC if sat else 0
        row('%s.%s d0, q5' % (base, t), st, after)


for t in ('s8', 'u8', 's16', 'u16', 's32', 'u32'):
    for _ in range(2):
        move_row('vmovl', t)
for t in ('i8', 'i16', 'i32'):
    move_row('vshll', t)
for t in ('i16', 'i32', 'i64'):
    for _ in range(2):
        move_row('vmovn', t)
for t in ('s16', 's32', 's64', 'u16', 'u32', 'u64'):
    for _ in range(3):
        move_row('vqmovn', t)
for t in ('s16', 's32', 's64'):
    for _ in range(3):
        move_row('vqmovun', t)

# --------------------------------------------------------------------------------------------
# Pairwise and permutes
# --------------------------------------------------------------------------------------------

for base in ('vpadd', 'vpmax', 'vpmin'):
    for t in (('i8', 'i16', 'i32') if base == 'vpadd' else ('s8', 'u8', 's16', 'u16', 's32', 'u32')):
        kind, e = t[0], int(t[1:])
        for _ in range(3):
            st = new_state({0: draw(e), 1: draw(e)})
            n = 64 // e
            a, b = lanes(st.d(2), e, n), lanes(st.d(4), e, n)
            res = []
            for src in (a, b):
                for j in range(n // 2):
                    x, y = src[2 * j], src[2 * j + 1]
                    if kind == 's':
                        x, y = sx(x, e), sx(y, e)
                    r = x + y if base == 'vpadd' else (max(x, y) if base == 'vpmax' else min(x, y))
                    res.append(r & mask(e))
            after = State(st.q, st.fpscr)
            after.set_d(0, join(res, e))
            row('%s.%s d0, d2, d4' % (base, t), st, after)
for base in ('vpaddl', 'vpadal'):
    for t in ('s8', 'u8', 's16', 'u16', 's32', 'u32'):
        kind, e = t[0], int(t[1:])
        for size in (64, 128):
            st = new_state({4: draw(2 * e), 5: draw(e), 0: draw(2 * e), 1: draw(e)})
            src = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
            old = lanes(st.q[4] if size == 128 else st.d(0), 2 * e, size // (2 * e))
            res = []
            for j in range(size // (2 * e)):
                x, y = src[2 * j], src[2 * j + 1]
                if kind == 's':
                    x, y = sx(x, e), sx(y, e)
                r = x + y + (old[j] if base == 'vpadal' else 0)
                res.append(r & mask(2 * e))
            after = State(st.q, st.fpscr)
            after.set_reg(4 if size == 128 else 0, size, join(res, 2 * e))
            row('%s.%s %s, %s' % (base, t, 'q4' if size == 128 else 'd0', 'q5' if size == 128 else 'd2'), st, after)

for base, e, container in (('vrev16', 8, 16), ('vrev32', 8, 32), ('vrev32', 16, 32), ('vrev64', 8, 64),
                           ('vrev64', 16, 64), ('vrev64', 32, 64)):
    for size in (64, 128):
        st = new_state({4: draw(8), 5: draw(8), 0: draw(8), 1: draw(8)})
        src = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
        g = container // e
        res = [src[(i // g) * g + (g - 1 - i % g)] for i in range(len(src))]
        after = State(st.q, st.fpscr)
        after.set_reg(4 if size == 128 else 0, size, join(res, e))
        row('%s.%d %s, %s' % (base, e, 'q4' if size == 128 else 'd0', 'q5' if size == 128 else 'd2'), st, after)
for size in (64, 128):
    st = new_state({4: rng.getrandbits(128), 5: rng.getrandbits(128), 0: rng.getrandbits(128), 1: rng.getrandbits(128)})
    after = State(st.q, st.fpscr)
    if size == 128:
        after.q[4], after.q[5] = st.q[5], st.q[4]
        row('vswp q4, q5', st, after)
    else:
        after.set_d(0, st.d(2))
        after.set_d(2, st.d(0))
        row('vswp d0, d2', st, after)
for base in ('vtrn', 'vuzp', 'vzip'):
    for e in (8, 16, 32):
        for size in (64, 128):
            if size == 128 or True:
                st = new_state({4: draw(e), 5: draw(e), 0: draw(e), 1: draw(e)})
                x = lanes(st.q[4] if size == 128 else st.d(0), e, size // e)
                y = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
                n = len(x)
                if base == 'vtrn':
                    nx = list(x)
                    ny = list(y)
                    for i in range(0, n, 2):
                        nx[i + 1], ny[i] = y[i], x[i + 1]
                elif base == 'vuzp':
                    cat = x + y
                    nx, ny = cat[0::2], cat[1::2]
                else:
                    cat = [v for pair in zip(x, y) for v in pair]
                    nx, ny = cat[:n], cat[n:]
                if base == 'vzip' and e == 32 and size == 64:
                    pass
                after = State(st.q, st.fpscr)
                if size == 128:
                    after.q[4], after.q[5] = join(nx, e), join(ny, e)
                else:
                    after.set_d(0, join(nx, e))
                    after.set_d(2, join(ny, e))
                row('%s.%d %s, %s' % (base, e, 'q4' if size == 128 else 'd0', 'q5' if size == 128 else 'd2'), st, after)
for e, idx, size in ((8, 1, 64), (8, 7, 64), (16, 3, 64), (32, 1, 64), (8, 3, 128), (8, 15, 128), (16, 5, 128), (32, 3, 128),
                     (64, 1, 128)):
    st = new_state({4: draw(8), 5: draw(8), 6: draw(8), 0: draw(8), 1: draw(8), 2: draw(8)})
    x = lanes(st.q[5] if size == 128 else st.d(2), e, size // e)
    y = lanes(st.q[6] if size == 128 else st.d(4), e, size // e)
    cat = x + y
    res = cat[idx:idx + len(x)]
    after = State(st.q, st.fpscr)
    after.set_reg(4 if size == 128 else 0, size, join(res, e))
    row('vext.%d %s, %s, %s, #%d' % (e, *(('q4', 'q5', 'q6') if size == 128 else ('d0', 'd2', 'd4')), idx), st, after)
for e in (8, 16, 32):
    for size in (64, 128):
        st = new_state({4: draw(8), 0: draw(8)})
        r1 = rng.getrandbits(32)
        after = State(st.q, st.fpscr)
        after.set_reg(4 if size == 128 else 0, size, join([r1 & mask(e)] * (size // e), e))
        row('vdup.%d %s, r1' % (e, 'q4' if size == 128 else 'd0'), st, after, extra_setup={'r1': r1})
for e, idx in ((8, 5), (16, 2), (32, 1)):
    for size in (64, 128):
        st = new_state({4: draw(8), 0: draw(8), 1: draw(8), 3: draw(e)})
        v = lanes(st.d(7), e, 64 // e)[idx]
        after = State(st.q, st.fpscr)
        after.set_reg(4 if size == 128 else 0, size, join([v] * (size // e), e))
        row('vdup.%d %s, d7[%d]' % (e, 'q4' if size == 128 else 'd0', idx), st, after)

for count in (1, 2, 3, 4):
    for ext in (False, True):
        st = new_state({0: draw(8), 1: draw(8), 2: draw(8), 3: draw(8)})
        # indexes: bytes near the table length
        table = []
        for r in range(count):
            table += lanes(st.d(4 + r), 8, 8)
        idx_bytes = [rng.choice([0, 1, 7, 8, 15, 16, 24, 31, 32, 255, rng.getrandbits(8)]) for _ in range(8)]
        st.set_d(2, join(idx_bytes, 8))
        old = lanes(st.d(0), 8, 8)
        res = [table[i] if i < len(table) else (old[k] if ext else 0) for k, i in enumerate(idx_bytes)]
        after = State(st.q, st.fpscr)
        after.set_d(0, join(res, 8))
        lst = ', '.join('d%d' % (4 + r) for r in range(count))
        row('%s.8 d0, {%s}, d2' % ('vtbx' if ext else 'vtbl', lst), st, after)

for e in (8, 16, 32):
    for size in (64, 128):
        pass


# estimates
def recip_estimate(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def rsqrt_estimate(a):
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


def urecpe(x):
    if x < 0x80000000:
        return 0xffffffff
    return (recip_estimate(x >> 23) & 0x1ff) << 23


def ursqrte(x):
    if x < 0x40000000:
        return 0xffffffff
    return (rsqrt_estimate(x >> 23) & 0x1ff) << 23


for base, fn in (('vrecpe', urecpe), ('vrsqrte', ursqrte)):
    for size in (64, 128):
        for _ in range(8):
            st = new_state({4: 0, 5: 0})
            vals = [rng.choice([0, 0x3fffffff, 0x40000000, 0x7fffffff, 0x80000000, 0xffffffff, 0x80ffffff, 0xb0000000,
                                rng.getrandbits(32), rng.getrandbits(32)]) for _ in range(size // 32)]
            src = join(vals, 32)
            if size == 128:
                st.q[5] = src
            else:
                st.set_d(2, src)
            after = State(st.q, st.fpscr)
            after.set_reg(4 if size == 128 else 0, size, join([fn(v) for v in vals], 32))
            row('%s.u32 %s, %s' % (base, 'q4' if size == 128 else 'd0', 'q5' if size == 128 else 'd2'), st, after)

# --------------------------------------------------------------------------------------------
# Dot products and matrix multiplies
# --------------------------------------------------------------------------------------------


def dot_row(base, size, by_element):
    sa = base in ('vsdot', 'vsudot')
    sb = base in ('vsdot', 'vusdot')
    st = new_state({4: rng.getrandbits(128), 5: draw(8), 6: draw(8), 0: rng.getrandbits(128), 1: draw(8), 2: draw(8)})
    n = size // 32
    acc = lanes(st.q[4] if size == 128 else st.d(0), 32, n)
    a = lanes(st.q[5] if size == 128 else st.d(2), 8, size // 8)
    if by_element:
        idx = rng.randrange(2)
        b_all = lanes(st.d(12), 8, 8)
    else:
        b_all = lanes(st.q[6] if size == 128 else st.d(4), 8, size // 8)
    res = []
    for i in range(n):
        s = acc[i]
        for j in range(4):
            x = a[4 * i + j]
            y = b_all[4 * idx + j] if by_element else b_all[4 * i + j]
            s += (sx(x, 8) if sa else x) * (sx(y, 8) if sb else y)
        res.append(s & mask(32))
    after = State(st.q, st.fpscr)
    after.set_reg(4 if size == 128 else 0, size, join(res, 32))
    t = {'vsdot': 's8', 'vudot': 'u8', 'vusdot': 's8', 'vsudot': 'u8'}[base]
    regs = ('q4', 'q5', 'd12') if size == 128 else ('d0', 'd2', 'd12')
    if by_element:
        row('%s.%s %s, %s, d12[%d]' % (base, t, regs[0], regs[1], idx), st, after)
    else:
        regs = ('q4', 'q5', 'q6') if size == 128 else ('d0', 'd2', 'd4')
        row('%s.%s %s, %s, %s' % (base, t, *regs), st, after)


for base in ('vsdot', 'vudot', 'vusdot', 'vsudot'):
    for size in (64, 128):
        for by in (False, True):
            if base == 'vsudot' and not by:
                continue
            for _ in range(3):
                dot_row(base, size, by)

for base in ('vsmmla', 'vummla', 'vusmmla'):
    sa = base == 'vsmmla'
    sb = base != 'vummla'
    for _ in range(5):
        st = new_state({4: rng.getrandbits(128), 5: draw(8), 6: draw(8)})
        acc = lanes(st.q[4], 32, 4)
        a = lanes(st.q[5], 8, 16)
        b = lanes(st.q[6], 8, 16)
        res = []
        for i in range(2):
            for j in range(2):
                s = acc[2 * i + j]
                for k in range(8):
                    x, y = a[8 * i + k], b[8 * j + k]
                    s += (sx(x, 8) if sa else x) * (sx(y, 8) if sb else y)
                res.append(s & mask(32))
        after = State(st.q, st.fpscr)
        after.q[4] = join(res, 32)
        row('%s.%s q4, q5, q6' % (base, 'u8' if base == 'vummla' else 's8'), st, after)

# --------------------------------------------------------------------------------------------
# Cryptography
# --------------------------------------------------------------------------------------------


def gf_mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11b
        b >>= 1
    return r


def gf_inv(a):
    if a == 0:
        return 0
    for b in range(1, 256):
        if gf_mul(a, b) == 1:
            return b


def make_sbox():
    sbox = []
    for x in range(256):
        b = gf_inv(x)
        s = b
        for i in range(1, 5):
            s ^= ((b << i) | (b >> (8 - i))) & 0xff
        sbox.append(s ^ 0x63)
    return sbox


SBOX = make_sbox()
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i


def aes_bytes(value):
    return [(value >> (8 * i)) & 0xff for i in range(16)]


def aes_join(b):
    return sum(v << (8 * i) for i, v in enumerate(b))


def shift_rows(b, inverse=False):
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            if inverse:
                out[4 * ((c + r) % 4) + r] = b[4 * c + r]
            else:
                out[4 * c + r] = b[4 * ((c + r) % 4) + r]
    return out


def mix_columns(b, inverse=False):
    out = [0] * 16
    mat = [[14, 11, 13, 9], [9, 14, 11, 13], [13, 9, 14, 11], [11, 13, 9, 14]] if inverse else \
        [[2, 3, 1, 1], [1, 2, 3, 1], [1, 1, 2, 3], [3, 1, 1, 2]]
    for c in range(4):
        for r in range(4):
            v = 0
            for k in range(4):
                v ^= gf_mul(mat[r][k], b[4 * c + k])
            out[4 * c + r] = v
    return out


def aese(d, m):
    x = aes_bytes(d ^ m)
    return aes_join([SBOX[v] for v in shift_rows(x)])


def aesd(d, m):
    x = aes_bytes(d ^ m)
    return aes_join([INV_SBOX[v] for v in shift_rows(x, True)])


for _ in range(4):
    st = new_state({4: rng.getrandbits(128), 5: rng.getrandbits(128)})
    for base, fn in (('aese', lambda d, m: aese(d, m)), ('aesd', lambda d, m: aesd(d, m)),
                     ('aesmc', lambda d, m: aes_join(mix_columns(aes_bytes(m)))),
                     ('aesimc', lambda d, m: aes_join(mix_columns(aes_bytes(m), True)))):
        after = State(st.q, st.fpscr)
        after.q[4] = fn(st.q[4], st.q[5])
        row('%s.8 q4, q5' % base, st, after)


def aes_key_expansion(key):
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[v] for v in t]
            t[0] ^= rcon
            rcon = gf_mul(rcon, 2)
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return [aes_join(sum(w[4 * r:4 * r + 4], [])) for r in range(11)]


# FIPS-197 appendix C.1: the whole AES-128 encryption of a block with the round keys in q1 - q11
_key = bytes(range(16))
_round_keys = aes_key_expansion(_key)
_plain = aes_join(list(bytes.fromhex('00112233445566778899aabbccddeeff')))
_state = new_state({0: _plain})
for _r in range(11):
    _state.q[1 + _r] = _round_keys[_r]
_code = []
for _r in range(9):
    _code += ['aese.8 q0, q%d' % (1 + _r), 'aesmc.8 q0, q0']
_code += ['aese.8 q0, q10', 'veor q0, q0, q11']
_after = State(_state.q, _state.fpscr)
_after.q[0] = aes_join(list(bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')))
row('; '.join(_code), _state, _after)


def rol(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xffffffff


def ror(x, n):
    return ((x >> n) | (x << (32 - n))) & 0xffffffff


def sha_choose(x, y, z):
    return (x & y) | (~x & 0xffffffff & z)


def sha_majority(x, y, z):
    return (x & y) | (x & z) | (y & z)


def sha_parity(x, y, z):
    return x ^ y ^ z


def sha1_round_ops(base, d, n, m):
    x = lanes(d, 32, 4)
    y = lanes(n, 32, 4)[0]
    w = lanes(m, 32, 4)
    f = {'sha1c': sha_choose, 'sha1p': sha_parity, 'sha1m': sha_majority}[base]
    xv = x[0] | (x[1] << 32) | (x[2] << 64) | (x[3] << 96)
    for e in range(4):
        xl = lanes(xv, 32, 4)
        t = f(xl[1], xl[2], xl[3])
        y = (y + rol(xl[0], 5) + t + w[e]) & 0xffffffff
        xl[1] = rol(xl[1], 30)
        # <Y, X> = ROL(Y : X, 32) of the 160 bit value, X in the low bits? the 160 bit value Y:X rotated
        full = (y << 128) | join(xl, 32)
        full = ((full << 32) | (full >> 128)) & mask(160)
        xv = full & M128
        y = full >> 128
        xl = lanes(xv, 32, 4)
        # the rotate moves X<127:96> to Y and Y to X<31:0>... this is the pseudocode's step
    return xv


def sha1su1(d, n):
    x = lanes(d, 32, 4)
    y = lanes(n, 32, 4)
    t = [x[0] ^ y[1], x[1] ^ y[2], x[2] ^ y[3], x[3]]
    return join([rol(t[0], 1), rol(t[1], 1), rol(t[2], 1), rol(t[3], 1) ^ rol(t[0], 2)], 32)


def sha1su0(d, n, m):
    x = d
    y = n
    z = m
    t = ((x >> 64) & M64) | ((y & M64) << 64)
    return t ^ x ^ z


def sha256_hash(x, y, w, part1):
    xl, yl, wl = lanes(x, 32, 4), lanes(y, 32, 4), lanes(w, 32, 4)
    L = xl + yl
    for e in range(4):
        chs = sha_choose(L[4], L[5], L[6])
        maj = sha_majority(L[0], L[1], L[2])
        s1 = ror(L[4], 6) ^ ror(L[4], 11) ^ ror(L[4], 25)
        s0 = ror(L[0], 2) ^ ror(L[0], 13) ^ ror(L[0], 22)
        t = (L[7] + s1 + chs + wl[e]) & 0xffffffff
        L[3] = (t + L[3]) & 0xffffffff
        L[7] = (t + s0 + maj) & 0xffffffff
        L = [L[7]] + L[:7]
    return join(L[:4] if part1 else L[4:], 32)


def sha256su0(d, m):
    x, mm = lanes(d, 32, 4), lanes(m, 32, 4)
    t = [x[1], x[2], x[3], mm[0]]
    return join([(x[i] + (ror(t[i], 7) ^ ror(t[i], 18) ^ (t[i] >> 3))) & 0xffffffff for i in range(4)], 32)


def sha256su1(d, n, m):
    x, y, z = lanes(d, 32, 4), lanes(n, 32, 4), lanes(m, 32, 4)
    t0 = [y[1], y[2], y[3], z[0]]
    t1 = [z[2], z[3]]
    r = [0] * 4
    for e in range(2):
        elt = ror(t1[e], 17) ^ ror(t1[e], 19) ^ (t1[e] >> 10)
        r[e] = (elt + x[e] + t0[e]) & 0xffffffff
    t1 = r[:2]
    for e in range(2, 4):
        elt = ror(t1[e - 2], 17) ^ ror(t1[e - 2], 19) ^ (t1[e - 2] >> 10)
        r[e] = (elt + x[e] + t0[e]) & 0xffffffff
    return join(r, 32)


for _ in range(4):
    st = new_state({4: rng.getrandbits(128), 5: rng.getrandbits(128), 6: rng.getrandbits(128)})
    d, n, m = st.q[4], st.q[5], st.q[6]
    for text, value in (('sha1c.32 q4, q5, q6', sha1_round_ops('sha1c', d, n, m)),
                        ('sha1p.32 q4, q5, q6', sha1_round_ops('sha1p', d, n, m)),
                        ('sha1m.32 q4, q5, q6', sha1_round_ops('sha1m', d, n, m)),
                        ('sha1su0.32 q4, q5, q6', sha1su0(d, n, m)),
                        ('sha1su1.32 q4, q5', sha1su1(d, n)),
                        ('sha1h.32 q4, q5', rol(lanes(n, 32, 4)[0], 30)),
                        ('sha256h.32 q4, q5, q6', sha256_hash(d, n, m, True)),
                        ('sha256h2.32 q4, q5, q6', sha256_hash(n, d, m, False)),
                        ('sha256su0.32 q4, q5', sha256su0(d, n)),
                        ('sha256su1.32 q4, q5, q6', sha256su1(d, n, m))):
        after = State(st.q, st.fpscr)
        after.q[4] = value
        row(text, st, after)


def clmul(a, b):
    r = 0
    for i in range(64):
        if (b >> i) & 1:
            r ^= a << i
    return r


for _ in range(5):
    st = new_state({1: rng.getrandbits(128), 2: rng.getrandbits(128)})
    after = State(st.q, st.fpscr)
    after.q[4] = clmul(st.d(2), st.d(4))
    row('vmull.p64 q4, d2, d4', st, after)

# --------------------------------------------------------------------------------------------
# Structure loads and stores
# --------------------------------------------------------------------------------------------


def memory_bytes(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))


def vls_rows():
    mem = memory_bytes(64, 5)
    for load in (True, False):
        # (text, kind, n, e, regs, stride)
        pass


def mem_token(address, data):
    return '%x:%s' % (address, data.hex())


def structure_row(op, n, e, mode, regs, stride, index=None, wb=None, thumb=0):
    """One vld / vst. mode: 'multi', 'lane', 'all'. regs: first d register, stride: 1 or 2."""
    load = op == 'vld'
    count = len(regs)
    bytes_ = e // 8
    data = memory_bytes(64, rng.getrandbits(16))
    st = new_state({i: rng.getrandbits(128) for i in range(8)})
    start_q = list(st.q)
    elems = 64 // e
    lst = []
    mem_after = bytearray(data)
    if mode == 'multi':
        if n == 1:
            total = 8 * count
            if load:
                for k, r in enumerate(regs):
                    st_after_d = int.from_bytes(data[8 * k:8 * k + 8], 'little')
                    lst.append((r, st_after_d))
            else:
                for k, r in enumerate(regs):
                    mem_after[8 * k:8 * k + 8] = st.d(r).to_bytes(8, 'little')
        else:
            groups = count // n if n == 2 else 1
            inc = 2 if (n == 2 and count == 4) else stride
            off = 0
            acc = {}
            for g in range(groups):
                for el in range(elems):
                    for j in range(n):
                        reg = regs[0] + g + j * inc
                        chunk = data[off:off + bytes_]
                        if load:
                            acc.setdefault(reg, [0] * elems)[el] = int.from_bytes(chunk, 'little')
                        else:
                            lane = lanes(st.d(reg), e, elems)[el]
                            mem_after[off:off + bytes_] = lane.to_bytes(bytes_, 'little')
                        off += bytes_
            total = off
            for reg, lanes_ in acc.items():
                lst.append((reg, join(lanes_, e)))
    elif mode == 'lane':
        off = 0
        for j in range(n):
            reg = regs[0] + j * stride
            if load:
                v = int.from_bytes(data[off:off + bytes_], 'little')
                cur = lanes(st.d(reg), e, elems)
                cur[index] = v
                lst.append((reg, join(cur, e)))
            else:
                lane = lanes(st.d(reg), e, elems)[index]
                mem_after[off:off + bytes_] = lane.to_bytes(bytes_, 'little')
            off += bytes_
        total = off
    else:
        off = 0
        for j in range(count):
            reg = regs[0] + j * stride
            k = 0 if n == 1 else j
            v = int.from_bytes(data[k * bytes_:(k + 1) * bytes_], 'little')
            lst.append((reg, join([v] * elems, e)))
        total = bytes_ * (1 if n == 1 else n)
    after = State(st.q, st.fpscr)
    for reg, value in lst:
        after.set_d(reg, value)
    # the text
    if mode == 'multi':
        names = ', '.join('d%d' % (regs[0] + i) for i in range(count)) if n == 1 else None
        if n == 1:
            lstt = names
        elif n == 2 and count == 4:
            lstt = ', '.join('d%d' % (regs[0] + i) for i in range(4))
        else:
            lstt = ', '.join('d%d' % (regs[0] + j * stride) for j in range(n))
    elif mode == 'lane':
        lstt = ', '.join('d%d[%d]' % (regs[0] + j * stride, index) for j in range(n))
    else:
        lstt = ', '.join('d%d[]' % (regs[0] + j * stride) for j in range(count))
    extra = {'r4': BASE, 'mem': mem_token(BASE, data)}
    text = '%s%d.%d {%s}, [r4]' % (op, n, e, lstt)
    expect = {}
    if wb == '!':
        text += '!'
        expect['r4'] = BASE + total
    elif wb == 'r':
        text += ', r2'
        extra['r2'] = 0x20
        expect['r4'] = BASE + 0x20
    if not load:
        changed = [(BASE + i, mem_after[i]) for i in range(len(mem_after)) if mem_after[i] != data[i]]
        if changed:
            runs = []
            for a, v in changed:
                if runs and runs[-1][0] + len(runs[-1][1]) == a:
                    runs[-1][1].append(v)
                else:
                    runs.append((a, [v]))
            expect['mem'] = ','.join('%x:%s' % (a, bytes(v).hex()) for a, v in runs)
    row(text, st, after, extra, expect, thumb)


for load in ('vld', 'vst'):
    for e in (8, 16, 32, 64):
        for regs in ([0], [0, 1], [0, 1, 2], [0, 1, 2, 3]):
            for wb in (None, '!', 'r'):
                structure_row(load, 1, e, 'multi', regs, 1, wb=wb)
    for e in (8, 16, 32):
        for regs, stride in (([0, 1], 1), ([0, 2], 2), ([0, 1, 2, 3], 1)):
            for wb in (None, '!', 'r'):
                structure_row(load, 2, e, 'multi', regs, stride, wb=wb)
        for stride in (1, 2):
            for wb in (None, '!', 'r'):
                structure_row(load, 3, e, 'multi', [0, 1, 2] if stride == 1 else [0, 2, 4], stride, wb=wb)
                structure_row(load, 4, e, 'multi', [0, 1, 2, 3] if stride == 1 else [0, 2, 4, 6], stride, wb=wb)
        for n in (1, 2, 3, 4):
            for stride in ((1,) if (n == 1 or e == 8) else (1, 2)):
                for index in range(min(64 // e, 4)):
                    for wb in (None, '!', 'r'):
                        regs = [1 + j * stride for j in range(n)]
                        structure_row(load, n, e, 'lane', regs, stride, index=index, wb=wb)
for e in (8, 16, 32):
    for wb in (None, '!', 'r'):
        structure_row('vld', 1, e, 'all', [0], 1, wb=wb)
        structure_row('vld', 1, e, 'all', [0, 1], 1, wb=wb)
        for n in (2, 3, 4):
            for stride in (1, 2):
                structure_row('vld', n, e, 'all', [1 + j * stride for j in range(n)], stride, wb=wb)

# a few T32 rows, to exercise the T32 -> A32 translation of the SIMD encodings
T32_TEXTS = [
    ('vadd.i32 q4, q5, q6', None), ('vqadd.s8 d0, d2, d4', None), ('vshl.u16 q4, q5, #3', None),
    ('vld1.32 {d0, d1}, [r4]!', None), ('vmull.s16 q4, d2, d4', None), ('aese.8 q4, q5', None),
]


def hex_fmt(value):
    return '%x' % value


def render():
    for i, r in enumerate(list(ROWS)):
        if i % 7 == 3 and ';' not in r[1]:
            ROWS.append((1, r[1], r[2], r[3]))
    texts = [r[1] for r in ROWS]
    a32 = armasm.assemble('a32', texts)
    t32 = armasm.assemble('t32', texts)
    a32 = [t32[i] if ROWS[i][0] else a32[i] for i in range(len(ROWS))]
    extra = ['-march=armv8.6-a+crypto+i8mm+bf16+fp16', '-mfpu=crypto-neon-fp-armv8']
    out = ['// Generated by tests/arm/tools/gen_semantic_rows_aarch32_neon.py: the Advanced SIMD integer rows of',
           '// tests/TestArmAArch32Semantics.cpp.',
           '// { isThumb, "code", "text", "setup", "expected" }',
           '#define AARCH32_SEMANTIC_ROWS_NEON \\']
    skipped = 0
    for (thumb, text, setup, expect), (hexb, err) in zip(ROWS, a32):
        if err is not None:
            alt = armasm.assemble('t32' if thumb else 'a32', [text], extra=extra)[0]
            hexb, err = alt
        if err is not None:
            skipped += 1
            sys.stderr.write('not assembled: %s: %s\n' % (text, err))
            continue

        def fmt(d):
            parts = []
            for k, v in d.items():
                if k == 'mem':
                    parts.append('mem=' + v if isinstance(v, str) else 'mem=' + v)
                else:
                    parts.append('%s=%x' % (k, v))
            return ' '.join(parts)

        out.append('    { %d, "%s", "%s", "%s", "%s" }, \\' % (thumb, hexb, text.replace('"', '\\"'), fmt(setup), fmt(expect)))
    return '\n'.join(out) + '\n', skipped


def main():
    text, skipped = render()
    with open(OUTPUT, 'w') as f:
        f.write(text)
    print('wrote %s: %d rows (%d not assembled)' % (OUTPUT, len(ROWS) - skipped, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
