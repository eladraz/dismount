#!/usr/bin/env python3
"""Generate tests/arm/Arm64SimdSemanticRowsFloat.inc, the rows of tests/TestArm64SemanticsSimd.cpp for the scalar
and vector floating point instructions: an instruction, a start state (the operands, FPCR, FPSR) and the state it
must leave, computed by the independent Python model of the pseudocode of the Arm ARM (armfp.py, checked against
the Apple M4 by check_armfp.py), not by dismount.

The rows cover every instruction and arrangement of the floating point lifter, with the special values (NaNs of
both kinds and signs, infinities, zeros, denormals, the extremes) in every rounding mode and with FZ / FZ16 / DN, and
the instructions that neither Unicorn nor the M4 run (FAMAX / FAMIN, FSCALE, LUTI2 / LUTI4, FPRCVT, FRINT32 / 64
vector forms are covered by the processors) so their semantics are pinned.

    gen_simd_semantic_rows_float.py [--seed N]
"""
import argparse
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib
import armfp as M
import check_armfp as C
from simd_rows_common import Rows, mask, lane, set_lane

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'Arm64SimdSemanticRowsFloat.inc')
REG = {16: 'h', 32: 's', 64: 'd'}
ARR = {16: 'h', 32: 's', 64: 'd'}


def pack_lanes(values, bits):
    v = 0
    for i, x in enumerate(values):
        v |= (x & mask(bits)) << (i * bits)
    return v


def flags_token(flags, previous=0):
    return (previous | flags) & 0x0800009f


class Builder(object):
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.rows = Rows()

    def env(self):
        r = self.rng.random()
        if r < 0.4:
            return M.Env()
        return M.Env(self.rng.randint(0, 3), self.rng.getrandbits(1), self.rng.getrandbits(1),
                     self.rng.getrandbits(1), 1 if self.rng.random() < 0.2 else 0)

    def value(self, bits):
        return C.draw(self.rng, bits)

    # -- scalar rows from the checker's operation list --------------------------------------------------
    def scalar_rows(self, per_operation):
        for name, text, kind, bits, model in C.operations():
            if kind == 'jcvt':
                continue                       # the rows of other_forms()
            arity = 3 if text.count(',') == 3 else (2 if text.count(',') == 2 else 1)
            if kind == 'nzcv':
                arity = 2
            for n in range(per_operation):
                env = self.env()
                previous = self.rng.choice([0, 0, 0x10, 0x8000000 | 0x80])
                if kind == 'int':
                    width = bits
                    inputs = [self.rng.choice([0, 1, 2, 0x7f, 0x80, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
                                               (1 << 63) - 1, 1 << 63, (1 << 64) - 1, self.rng.getrandbits(64)])
                              & mask(width)]
                    setup = {'x1': inputs[0], 'fpcr': env.fpcr(), 'fpsr': previous}
                else:
                    inputs = [self.value(bits) for _ in range(3)]
                    setup = {'fpcr': env.fpcr(), 'fpsr': previous}
                    for i in range(arity):
                        setup['v%d' % (i + 1)] = inputs[i]
                want, flags = model(inputs[:arity] if kind != 'int' else inputs, env)
                if kind == 'x0':
                    expect = {'x0': want, 'fpsr': flags_token(flags, previous)}
                elif kind == 'nzcv':
                    expect = {'nzcv': want, 'fpsr': flags_token(flags, previous)}
                else:
                    expect = {'v0': want, 'fpsr': flags_token(flags, previous)}
                self.rows.add(text, setup, expect)

    # -- vector rows -------------------------------------------------------------------------------------
    def vector(self, text, bits, lanes, arity, lane_model, per=3, accumulate=False, zero=False, result_bits=None):
        """lane_model(operands_list_of_lane_values, env) -> (result lane, flags)."""
        result_bits = result_bits or bits
        for _ in range(per):
            env = self.env()
            previous = self.rng.choice([0, 0, 0x10])
            operands = [[self.value(bits) for _ in range(lanes)] for _ in range(arity)]
            acc = [self.value(result_bits) for _ in range(lanes)]
            setup = {'fpcr': env.fpcr(), 'fpsr': previous}
            for i in range(arity):
                setup['v%d' % (i + 1)] = pack_lanes(operands[i], bits)
            if accumulate:
                setup['v0'] = pack_lanes(acc, result_bits)
            results = []
            flags = 0
            for l in range(lanes):
                ops = [operands[i][l] for i in range(arity)]
                if accumulate:
                    ops = ops + [acc[l]]
                r, fl = lane_model(ops, env)
                results.append(r)
                flags |= fl
            self.rows.add(text, setup, {'v0': pack_lanes(results, result_bits), 'fpsr': flags_token(flags, previous)})

    def vector_forms(self):
        f = M.FORMATS
        shapes = [(32, 2, '2s'), (32, 4, '4s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')]
        binary = [
            ('fadd', M.fp_add), ('fsub', M.fp_sub), ('fmul', M.fp_mul), ('fdiv', M.fp_div), ('fmulx', M.fp_mulx),
            ('fmax', lambda a, b, fm, e: M.fp_max(a, b, fm, e, True, False)),
            ('fmin', lambda a, b, fm, e: M.fp_max(a, b, fm, e, False, False)),
            ('fmaxnm', lambda a, b, fm, e: M.fp_max(a, b, fm, e, True, True)),
            ('fminnm', lambda a, b, fm, e: M.fp_max(a, b, fm, e, False, True)),
            ('frecps', M.fp_recip_step), ('frsqrts', M.fp_rsqrt_step),
            ('fabd', lambda a, b, fm, e: _abd(a, b, fm, e)),
        ]
        for bits, lanes, arr in shapes:
            fm = f[bits]
            for name, model in binary:
                self.vector('%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr), bits, lanes, 2,
                            lambda o, e, m=model, fm=fm: m(o[0], o[1], fm, e))
            for name, negate in (('fmla', False), ('fmls', True)):
                def model(o, e, fm=fm, negate=negate):
                    n = M.fp_neg(o[0], fm) if negate else o[0]
                    return M.fp_muladd(o[2], n, o[1], fm, e)
                self.vector('%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr), bits, lanes, 2, model, accumulate=True)
            unary = [('fabs', lambda a, fm, e: (M.fp_abs(a, fm), 0)), ('fneg', lambda a, fm, e: (M.fp_neg(a, fm), 0)),
                     ('fsqrt', M.fp_sqrt), ('frecpe', M.fp_recip_estimate), ('frsqrte', M.fp_rsqrt_estimate),
                     ('frintn', lambda a, fm, e: M.fp_round_int(a, fm, e, M.RN, False)),
                     ('frintp', lambda a, fm, e: M.fp_round_int(a, fm, e, M.RP, False)),
                     ('frintm', lambda a, fm, e: M.fp_round_int(a, fm, e, M.RM, False)),
                     ('frintz', lambda a, fm, e: M.fp_round_int(a, fm, e, M.RZ, False)),
                     ('frinta', lambda a, fm, e: M.fp_round_int(a, fm, e, M.RA, False)),
                     ('frintx', lambda a, fm, e: M.fp_round_int(a, fm, e, e.rmode, True)),
                     ('frinti', lambda a, fm, e: M.fp_round_int(a, fm, e, e.rmode, False))]
            if bits != 16:
                unary += [('frint32z', lambda a, fm, e: M.fp_round_int_bounded(a, fm, e, M.RZ, 32)),
                          ('frint32x', lambda a, fm, e: M.fp_round_int_bounded(a, fm, e, e.rmode, 32)),
                          ('frint64z', lambda a, fm, e: M.fp_round_int_bounded(a, fm, e, M.RZ, 64)),
                          ('frint64x', lambda a, fm, e: M.fp_round_int_bounded(a, fm, e, e.rmode, 64))]
            for name, model in unary:
                if bits == 64 and lanes == 2 or bits != 64:
                    self.vector('%s v0.%s, v1.%s' % (name, arr, arr), bits, lanes, 1,
                                lambda o, e, m=model, fm=fm: m(o[0], fm, e))
            # compares: a mask of all ones lanes, the zero forms
            for name, rel in (('fcmeq', 'eq'), ('fcmge', 'ge'), ('fcmgt', 'gt'), ('facge', 'ge'), ('facgt', 'gt')):
                absolute = name.startswith('fac')

                def cmp_model(o, e, fm=fm, rel=rel, absolute=absolute, bits=bits):
                    a, b = o[0], o[1]
                    if absolute:
                        a, b = M.fp_abs(a, fm), M.fp_abs(b, fm)
                    return _compare_mask(a, b, fm, e, rel, bits)
                self.vector('%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr), bits, lanes, 2, cmp_model)
            for name, rel in (('fcmeq', 'eq'), ('fcmge', 'ge'), ('fcmgt', 'gt'), ('fcmle', 'le'), ('fcmlt', 'lt')):
                def zero_model(o, e, fm=fm, rel=rel, bits=bits):
                    return _compare_mask(o[0], 0, fm, e, rel, bits)
                self.vector('%s v0.%s, v1.%s, #0.0' % (name, arr, arr), bits, lanes, 1, zero_model)

    # -- by element, pairwise, across lanes -----------------------------------------------------------------
    def element_forms(self):
        f = M.FORMATS
        for bits, lanes, arr in ((32, 2, '2s'), (32, 4, '4s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')):
            fm = f[bits]
            for index in sorted(set([0, 1, lanes - 1, (bits == 16) * 5])):
                if index >= (128 // bits):
                    continue
                reg = 'v2' if bits != 16 else 'v2'
                for name, model in (('fmul', M.fp_mul), ('fmulx', M.fp_mulx)):
                    self.element(name, arr, bits, lanes, index, lambda a, b, e, m=model, fm=fm: m(a, b, fm, e), False)
                for name, negate in (('fmla', False), ('fmls', True)):
                    self.element(name, arr, bits, lanes, index,
                                 lambda a, b, e, acc, fm=fm, negate=negate:
                                 M.fp_muladd(acc, M.fp_neg(a, fm) if negate else a, b, fm, e), True)

    def element(self, name, arr, bits, lanes, index, model, accumulate):
        text = '%s v0.%s, v1.%s, v2.%s[%d]' % (name, arr, arr, ARR[bits], index)
        for _ in range(3):
            env = self.env()
            n = [self.value(bits) for _ in range(lanes)]
            m = [self.value(bits) for _ in range(128 // bits)]
            acc = [self.value(bits) for _ in range(lanes)]
            setup = {'fpcr': env.fpcr(), 'v1': pack_lanes(n, bits), 'v2': pack_lanes(m, bits)}
            if accumulate:
                setup['v0'] = pack_lanes(acc, bits)
            out = []
            flags = 0
            for l in range(lanes):
                r, fl = model(n[l], m[index], env, acc[l]) if accumulate else model(n[l], m[index], env)
                out.append(r)
                flags |= fl
            self.rows.add(text, setup, {'v0': pack_lanes(out, bits), 'fpsr': flags_token(flags)})

    def pairwise_forms(self):
        f = M.FORMATS
        pair_ops = [('faddp', M.fp_add), ('fmaxp', lambda a, b, fm, e: M.fp_max(a, b, fm, e, True, False)),
                    ('fminp', lambda a, b, fm, e: M.fp_max(a, b, fm, e, False, False)),
                    ('fmaxnmp', lambda a, b, fm, e: M.fp_max(a, b, fm, e, True, True)),
                    ('fminnmp', lambda a, b, fm, e: M.fp_max(a, b, fm, e, False, True))]
        for bits, lanes, arr in ((32, 2, '2s'), (32, 4, '4s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')):
            fm = f[bits]
            for name, model in pair_ops:
                text = '%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr)
                for _ in range(3):
                    env = self.env()
                    n = [self.value(bits) for _ in range(lanes)]
                    m = [self.value(bits) for _ in range(lanes)]
                    cat = n + m
                    out, flags = [], 0
                    for i in range(lanes):
                        r, fl = model(cat[2 * i], cat[2 * i + 1], fm, env)
                        out.append(r)
                        flags |= fl
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v1': pack_lanes(n, bits), 'v2': pack_lanes(m, bits)},
                                  {'v0': pack_lanes(out, bits), 'fpsr': flags_token(flags)})
            # the scalar pairwise: faddp d0, v1.2d
            if lanes == 2 and bits in (32, 64) or (bits == 16 and lanes == 8):
                n_lanes = 2 if bits != 16 else 2
                reg = REG[bits]
                src_arr = {32: '2s', 64: '2d', 16: '2h'}[bits]
                for name, model in pair_ops:
                    text = '%s %s0, v1.%s' % (name, reg, src_arr)
                    for _ in range(3):
                        env = self.env()
                        n = [self.value(bits) for _ in range(2)]
                        r, fl = model(n[0], n[1], fm, env)
                        self.rows.add(text, {'fpcr': env.fpcr(), 'v1': pack_lanes(n, bits)},
                                      {'v0': r, 'fpsr': flags_token(fl)})
        # across lanes: the tree of the Reduce() of the pseudocode
        across = [('fmaxv', True, False), ('fminv', False, False), ('fmaxnmv', True, True), ('fminnmv', False, True)]
        for bits, lanes, arr in ((32, 4, '4s'), (16, 4, '4h'), (16, 8, '8h')):
            fm = f[bits]
            for name, is_max, number in across:
                text = '%s %s0, v1.%s' % (name, REG[bits], arr)
                for _ in range(4):
                    env = self.env()
                    n = [self.value(bits) for _ in range(lanes)]
                    items = list(n)
                    flags = 0
                    while len(items) > 1:
                        nxt = []
                        for i in range(0, len(items), 2):
                            r, fl = M.fp_max(items[i], items[i + 1], fm, env, is_max, number)
                            nxt.append(r)
                            flags |= fl
                        items = nxt
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v1': pack_lanes(n, bits)},
                                  {'v0': items[0], 'fpsr': flags_token(flags)})

    # -- conversions -------------------------------------------------------------------------------------
    def conversion_forms(self):
        f = M.FORMATS
        for bits, lanes, arr in ((32, 2, '2s'), (32, 4, '4s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')):
            fm = f[bits]
            for letter, rmode in (('n', M.RN), ('m', M.RM), ('p', M.RP), ('z', M.RZ), ('a', M.RA)):
                for sign in ('s', 'u'):
                    self.vector('fcvt%s%s v0.%s, v1.%s' % (letter, sign, arr, arr), bits, lanes, 1,
                                lambda o, e, fm=fm, rmode=rmode, sg=sign, bits=bits:
                                M.fp_to_fixed(o[0], fm, e, bits, sg == 's', 0, rmode))
            for fbits in (1, bits - 1):
                for sign in ('s', 'u'):
                    self.vector('fcvtz%s v0.%s, v1.%s, #%d' % (sign, arr, arr, fbits), bits, lanes, 1,
                                lambda o, e, fm=fm, sg=sign, bits=bits, fb=fbits:
                                M.fp_to_fixed(o[0], fm, e, bits, sg == 's', fb, M.RZ))
            for sign in ('s', 'u'):
                for fbits in (0, 3):
                    suffix = ', #%d' % fbits if fbits else ''
                    self.vector('%scvtf v0.%s, v1.%s%s' % (sign, arr, arr, suffix), bits, lanes, 1,
                                lambda o, e, fm=fm, sg=sign, bits=bits, fb=fbits:
                                M.fixed_to_fp(o[0], bits, sg == 's', fb, fm, e))
        # fcvtn / fcvtl / fcvtxn
        for src, dst, a_src, a_dst, lanes in ((32, 16, '4s', '4h', 4), (64, 32, '2d', '2s', 2)):
            self.convert_narrow('fcvtn v0.%s, v1.%s' % (a_dst, a_src), src, dst, lanes, False, False)
            self.convert_narrow('fcvtn2 v0.%s, v1.%s' % ({'4h': '8h', '2s': '4s'}[a_dst], a_src), src, dst, lanes, True, False)
        self.convert_narrow('fcvtxn v0.2s, v1.2d', 64, 32, 2, False, True)
        self.convert_narrow('fcvtxn2 v0.4s, v1.2d', 64, 32, 2, True, True)
        for src, dst, a_src, a_dst, lanes in ((16, 32, '4h', '4s', 4), (32, 64, '2s', '2d', 2)):
            for upper in (False, True):
                text = ('fcvtl2 v0.%s, v1.%s' % (a_dst, {'4h': '8h', '2s': '4s'}[a_src])) if upper else \
                       ('fcvtl v0.%s, v1.%s' % (a_dst, a_src))
                for _ in range(3):
                    env = self.env()
                    n = [self.value(src) for _ in range(2 * lanes)]
                    sel = n[lanes:] if upper else n[:lanes]
                    out, flags = [], 0
                    for x in sel:
                        r, fl = M.fp_convert(x, M.FORMATS[src], M.FORMATS[dst], env)
                        out.append(r)
                        flags |= fl
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v1': pack_lanes(n, src)},
                                  {'v0': pack_lanes(out, dst), 'fpsr': flags_token(flags)})

    def convert_narrow(self, text, src, dst, lanes, upper, odd):
        for _ in range(3):
            env = self.env()
            n = [self.value(src) for _ in range(lanes)]
            old = self.rng.getrandbits(128)
            out, flags = [], 0
            for x in n:
                r, fl = M.fp_convert(x, M.FORMATS[src], M.FORMATS[dst], env, M.RO if odd else None)
                out.append(r)
                flags |= fl
            narrowed = pack_lanes(out, dst)
            setup = {'fpcr': env.fpcr(), 'v1': pack_lanes(n, src)}
            if upper:
                setup['v0'] = old
                result = (old & mask(64)) | (narrowed << 64)
            else:
                result = narrowed
            self.rows.add(text, setup, {'v0': result, 'fpsr': flags_token(flags)})

    # -- the rest ----------------------------------------------------------------------------------------
    def complex_forms(self):
        for bits, lanes, arr in ((32, 4, '4s'), (32, 2, '2s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')):
            fm = M.FORMATS[bits]
            for rot in (90, 270):
                text = 'fcadd v0.%s, v1.%s, v2.%s, #%d' % (arr, arr, arr, rot)
                for _ in range(3):
                    env = self.env()
                    n = [self.value(bits) for _ in range(lanes)]
                    m = [self.value(bits) for _ in range(lanes)]
                    out, flags = [0] * lanes, 0
                    for i in range(0, lanes, 2):
                        if rot == 90:
                            e1, e3 = M.fp_neg(m[i + 1], fm), m[i]
                        else:
                            e1, e3 = m[i + 1], M.fp_neg(m[i], fm)
                        r0, f0 = M.fp_add(n[i], e1, fm, env)
                        r1, f1 = M.fp_add(n[i + 1], e3, fm, env)
                        out[i], out[i + 1] = r0, r1
                        flags |= f0 | f1
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v1': pack_lanes(n, bits), 'v2': pack_lanes(m, bits)},
                                  {'v0': pack_lanes(out, bits), 'fpsr': flags_token(flags)})
            for rot in (0, 90, 180, 270):
                for indexed in (False, True):
                    if indexed and (bits == 64 or lanes == 2):
                        continue
                    idx = lanes // 2 - 1
                    if indexed:
                        text = 'fcmla v0.%s, v1.%s, v2.%s[%d], #%d' % (arr, arr, 's' if bits == 32 else 'h', idx, rot)
                    else:
                        text = 'fcmla v0.%s, v1.%s, v2.%s, #%d' % (arr, arr, arr, rot)
                    for _ in range(3):
                        env = self.env()
                        n = [self.value(bits) for _ in range(lanes)]
                        m = [self.value(bits) for _ in range(128 // bits)]
                        acc = [self.value(bits) for _ in range(lanes)]
                        out, flags = [0] * lanes, 0
                        for i in range(0, lanes, 2):
                            p = 2 * idx if indexed else i
                            m0, m1 = m[p], m[p + 1]
                            n0, n1 = n[i], n[i + 1]
                            if rot == 0:
                                e1, e2, e3, e4 = m0, n0, m1, n0
                            elif rot == 90:
                                e1, e2, e3, e4 = M.fp_neg(m1, fm), n1, m0, n1
                            elif rot == 180:
                                e1, e2, e3, e4 = M.fp_neg(m0, fm), n0, M.fp_neg(m1, fm), n0
                            else:
                                e1, e2, e3, e4 = m1, n1, M.fp_neg(m0, fm), n1
                            r0, f0 = M.fp_muladd(acc[i], e2, e1, fm, env)
                            r1, f1 = M.fp_muladd(acc[i + 1], e4, e3, fm, env)
                            out[i], out[i + 1] = r0, r1
                            flags |= f0 | f1
                        self.rows.add(text, {'fpcr': env.fpcr(), 'v0': pack_lanes(acc, bits), 'v1': pack_lanes(n, bits),
                                             'v2': pack_lanes(m, bits)},
                                      {'v0': pack_lanes(out, bits), 'fpsr': flags_token(flags)})

    def fhm_forms(self):
        # FMLAL / FMLSL: half precision products added to single precision lanes
        for name, sub, high in (('fmlal', False, False), ('fmlal2', False, True), ('fmlsl', True, False), ('fmlsl2', True, True)):
            for q, lanes, ta, tb in ((0, 2, '2s', '2h'), (1, 4, '4s', '4h')):
                text = '%s v0.%s, v1.%s, v2.%s' % (name, ta, tb, tb)
                for _ in range(4):
                    env = self.env()
                    n = [self.value(16) for _ in range(8)]
                    m = [self.value(16) for _ in range(8)]
                    acc = [self.value(32) for _ in range(lanes)]
                    out, flags = [], 0
                    for e in range(lanes):
                        i = e + (lanes if high else 0)
                        a = M.fp_neg(n[i], M.H) if sub else n[i]
                        r, fl = M.fp_muladd(acc[e], a, m[i], M.S, env, fab=M.H)
                        out.append(r)
                        flags |= fl
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v0': pack_lanes(acc, 32), 'v1': pack_lanes(n, 16),
                                         'v2': pack_lanes(m, 16)}, {'v0': pack_lanes(out, 32), 'fpsr': flags_token(flags)})
                itext = '%s v0.%s, v1.%s, v2.h[5]' % (name, ta, tb)
                for _ in range(3):
                    env = self.env()
                    n = [self.value(16) for _ in range(8)]
                    m = [self.value(16) for _ in range(8)]
                    acc = [self.value(32) for _ in range(lanes)]
                    out, flags = [], 0
                    for e in range(lanes):
                        i = e + (lanes if high else 0)
                        a = M.fp_neg(n[i], M.H) if sub else n[i]
                        r, fl = M.fp_muladd(acc[e], a, m[5], M.S, env, fab=M.H)
                        out.append(r)
                        flags |= fl
                    self.rows.add(itext, {'fpcr': env.fpcr(), 'v0': pack_lanes(acc, 32), 'v1': pack_lanes(n, 16),
                                          'v2': pack_lanes(m, 16)}, {'v0': pack_lanes(out, 32), 'fpsr': flags_token(flags)})

    def bfloat_forms(self):
        def bf_value():
            r = self.rng.random()
            if r < 0.4:
                return self.rng.choice([0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f80, 0xff80, 0x7fc0, 0x7f81, 0x0001, 0x007f,
                                        0x0080, 0x7f7f, 0xff7f, 0x4f00, 0x7fff, 0x3f81, 0x0040, 0x1f00])
            exp = self.rng.randint(100, 150)
            return (self.rng.getrandbits(1) << 15) | (exp << 7) | self.rng.getrandbits(7)

        def f32_value():
            r = self.rng.random()
            if r < 0.3:
                return self.rng.choice(C.SPECIAL[32])
            exp = self.rng.randint(90, 160)
            return (self.rng.getrandbits(1) << 31) | (exp << 23) | self.rng.getrandbits(23)

        for text, lanes in (('bfdot v0.2s, v1.4h, v2.4h', 2), ('bfdot v0.4s, v1.8h, v2.8h', 4)):
            for _ in range(8):
                n = [bf_value() for _ in range(2 * lanes)]
                m = [bf_value() for _ in range(2 * lanes)]
                acc = [f32_value() for _ in range(lanes)]
                out = [M.bfloat_dot_add(acc[l], n[2 * l], n[2 * l + 1], m[2 * l], m[2 * l + 1]) for l in range(lanes)]
                env = self.env()
                self.rows.add(text, {'fpcr': env.fpcr(), 'v0': pack_lanes(acc, 32), 'v1': pack_lanes(n, 16),
                                     'v2': pack_lanes(m, 16)}, {'v0': pack_lanes(out, 32)})
        for text, lanes, index in (('bfdot v0.2s, v1.4h, v2.2h[1]', 2, 1), ('bfdot v0.4s, v1.8h, v2.2h[3]', 4, 3)):
            for _ in range(5):
                n = [bf_value() for _ in range(2 * lanes)]
                m = [bf_value() for _ in range(8)]
                acc = [f32_value() for _ in range(lanes)]
                out = [M.bfloat_dot_add(acc[l], n[2 * l], n[2 * l + 1], m[2 * index], m[2 * index + 1]) for l in range(lanes)]
                self.rows.add(text, {'v0': pack_lanes(acc, 32), 'v1': pack_lanes(n, 16), 'v2': pack_lanes(m, 16)},
                              {'v0': pack_lanes(out, 32)})
        for _ in range(8):
            n = [bf_value() for _ in range(8)]
            m = [bf_value() for _ in range(8)]
            acc = [f32_value() for _ in range(4)]
            out = []
            for i in range(2):
                for j in range(2):
                    t = M.bfloat_dot_add(acc[2 * i + j], n[4 * i], n[4 * i + 1], m[4 * j], m[4 * j + 1])
                    out.append(M.bfloat_dot_add(t, n[4 * i + 2], n[4 * i + 3], m[4 * j + 2], m[4 * j + 3]))
            self.rows.add('bfmmla v0.4s, v1.8h, v2.8h', {'v0': pack_lanes(acc, 32), 'v1': pack_lanes(n, 16),
                                                        'v2': pack_lanes(m, 16)}, {'v0': pack_lanes(out, 32)})
        for name, sel in (('bfmlalb', 0), ('bfmlalt', 1)):
            for _ in range(6):
                env = self.env()
                n = [bf_value() for _ in range(8)]
                m = [bf_value() for _ in range(8)]
                acc = [f32_value() for _ in range(4)]
                out, flags = [], 0
                for e in range(4):
                    r, fl = _bf_muladd(acc[e], n[2 * e + sel], m[2 * e + sel], env)
                    out.append(r)
                    flags |= fl
                self.rows.add('%s v0.4s, v1.8h, v2.8h' % name, {'fpcr': env.fpcr(), 'v0': pack_lanes(acc, 32),
                              'v1': pack_lanes(n, 16), 'v2': pack_lanes(m, 16)},
                              {'v0': pack_lanes(out, 32), 'fpsr': flags_token(flags)})
        for _ in range(8):
            env = self.env()
            x = f32_value()
            r, fl = M.fp_convert(x, M.S, M.BF, env)
            self.rows.add('bfcvt h0, s1', {'fpcr': env.fpcr(), 'v1': x}, {'v0': r, 'fpsr': flags_token(fl)})
            xs = [f32_value() for _ in range(4)]
            outs, flags = [], 0
            for v in xs:
                r, fl = M.fp_convert(v, M.S, M.BF, env)
                outs.append(r)
                flags |= fl
            self.rows.add('bfcvtn v0.4h, v1.4s', {'fpcr': env.fpcr(), 'v1': pack_lanes(xs, 32)},
                          {'v0': pack_lanes(outs, 16), 'fpsr': flags_token(flags)})

    def other_forms(self):
        # FJCVTZS
        for _ in range(40):
            env = self.env()
            x = self.value(64)
            want, nzcv, flags = _fjcvtzs(x, env)
            self.rows.add('fjcvtzs w0, d1', {'fpcr': env.fpcr(), 'v1': x},
                          {'x0': want, 'nzcv': nzcv, 'fpsr': flags_token(flags)})
        for x in (0x4140000000000000, 0xc140000000000000, 0x41dfffffffc00000, 0x41e0000000000000, 0x7ff0000000000000,
                  0x7ff8000000000000, 0x8000000000000000, 0x3fe0000000000000, 0x43e0000000000000, 0x4f00000000000000):
            want, nzcv, flags = _fjcvtzs(x, M.Env())
            self.rows.add('fjcvtzs w0, d1', {'v1': x}, {'x0': want, 'nzcv': nzcv, 'fpsr': flags_token(flags)})
        # FAMAX / FAMIN (FEAT_FAMINMAX): the maximum / minimum of the absolute values
        for bits, lanes, arr in ((32, 2, '2s'), (32, 4, '4s'), (64, 2, '2d'), (16, 4, '4h'), (16, 8, '8h')):
            fm = M.FORMATS[bits]
            for name, is_max in (('famax', True), ('famin', False)):
                def model(o, e, fm=fm, is_max=is_max):
                    return M.fp_max(M.fp_abs(o[0], fm), M.fp_abs(o[1], fm), fm, e, is_max, False)
                self.vector('%s v0.%s, v1.%s, v2.%s' % (name, arr, arr, arr), bits, lanes, 2, model, per=4)
        # FSCALE: a * 2^b for the signed integer lanes b
        for bits, lanes, arr in ((32, 4, '4s'), (64, 2, '2d'), (16, 8, '8h')):
            fm = M.FORMATS[bits]
            for _ in range(6):
                env = self.env()
                a = [self.value(bits) for _ in range(lanes)]
                b = [self.rng.choice([0, 1, 2, -1, -2, 127, -127, 200, -200, 1 << (bits - 2), -(1 << (bits - 2)),
                                      self.rng.randint(-300, 300)]) & mask(bits) for _ in range(lanes)]
                out, flags = [], 0
                for x, y in zip(a, b):
                    y = y - (1 << bits) if y >> (bits - 1) else y
                    r, fl = M.fp_scale(x, y, fm, env)
                    out.append(r)
                    flags |= fl
                self.rows.add('fscale v0.%s, v1.%s, v2.%s' % (arr, arr, arr),
                              {'fpcr': env.fpcr(), 'v1': pack_lanes(a, bits), 'v2': pack_lanes(b, bits)},
                              {'v0': pack_lanes(out, bits), 'fpsr': flags_token(flags)})
        # LUTI2 / LUTI4 (FEAT_LUT)
        for esize, isize, count, indexes in ((8, 2, 1, range(4)), (16, 2, 1, range(8)), (8, 4, 1, range(2)), (16, 4, 2, range(4))):
            for index in indexes:
                table_regs = [self.rng.getrandbits(128) for _ in range(count)]
                vm = self.rng.getrandbits(128)
                elements = 128 // esize
                table = []
                data = b''.join(r.to_bytes(16, 'little') for r in table_regs)
                for t in range(len(data) * 8 // esize):
                    table.append(int.from_bytes(data, 'little') >> (t * esize) & mask(esize))
                out = 0
                for e in range(elements):
                    position = index * elements * isize + e * isize
                    ix = (vm >> position) & mask(isize)
                    out |= table[ix] << (e * esize)
                arr = '16b' if esize == 8 else '8h'
                mnemonic = 'luti2' if isize == 2 else 'luti4'
                if count == 2:
                    text = '%s v0.%s, {v1.%s, v2.%s}, v3[%d]' % (mnemonic, arr, arr, arr, index)
                    setup = {'v1': table_regs[0], 'v2': table_regs[1], 'v3': vm}
                else:
                    text = '%s v0.%s, {v1.%s}, v3[%d]' % (mnemonic, arr, arr, index)
                    setup = {'v1': table_regs[0], 'v3': vm}
                self.rows.add(text, setup, {'v0': out})
        # FPRCVT: conversions between the sizes in the SIMD registers
        for src, dst in ((64, 32), (16, 32), (32, 64), (16, 64)):
            for letter, rmode in (('n', M.RN), ('z', M.RZ), ('a', M.RA)):
                for sign in ('s', 'u'):
                    text = 'fcvt%s%s %s0, %s1' % (letter, sign, REG[dst], REG[src])
                    for _ in range(3):
                        env = self.env()
                        x = self.value(src)
                        r, fl = M.fp_to_fixed(x, M.FORMATS[src], env, dst, sign == 's', 0, rmode)
                        self.rows.add(text, {'fpcr': env.fpcr(), 'v1': x}, {'v0': r, 'fpsr': flags_token(fl)})
        for src, dst in ((32, 64), (32, 16), (64, 32), (64, 16)):
            for sign in ('s', 'u'):
                text = '%scvtf %s0, %s1' % (sign, REG[dst], REG[src])
                for _ in range(3):
                    env = self.env()
                    x = self.rng.getrandbits(src)
                    r, fl = M.fixed_to_fp(x, src, sign == 's', 0, M.FORMATS[dst], env)
                    self.rows.add(text, {'fpcr': env.fpcr(), 'v1': x}, {'v0': r, 'fpsr': flags_token(fl)})
        # FMOV
        for text, expect in (('fmov v0.4s, #1.0', pack_lanes([0x3f800000] * 4, 32)),
                             ('fmov v0.2d, #-2.5', pack_lanes([0xc004000000000000] * 2, 64)),
                             ('fmov v0.8h, #0.5', pack_lanes([0x3800] * 8, 16)),
                             ('fmov s0, #31.0', 0x41f80000), ('fmov d0, #-0.125', 0xbfc0000000000000)):
            self.rows.add(text, {}, {'v0': expect})
        self.rows.add('fmov x0, d1', {'v1': 0x1122334455667788}, {'x0': 0x1122334455667788})
        self.rows.add('fmov w0, s1', {'v1': 0x55667788}, {'x0': 0x55667788})
        self.rows.add('fmov d0, x1', {'x1': 0x1122334455667788, 'v0': mask(128)}, {'v0': 0x1122334455667788})
        self.rows.add('fmov x0, v1.d[1]', {'v1': (0xaabbccddeeff0011 << 64) | 5}, {'x0': 0xaabbccddeeff0011})
        self.rows.add('fmov v0.d[1], x1', {'x1': 0xaabbccddeeff0011, 'v0': 7}, {'v0': (0xaabbccddeeff0011 << 64) | 7})
        self.rows.add('fmov w0, h1', {'v1': 0xabcd}, {'x0': 0xabcd})
        self.rows.add('fmov h0, w1', {'x1': 0xdeadbeef, 'v0': mask(128)}, {'v0': 0xbeef})


def _bf_muladd(acc, a, b, env):
    """BFMLALB / BFMLALT: a single precision fused multiply-add of the BFloat16 numbers a * b + acc: FZ flushes
    the BFloat16 denormals (FZ16 does not apply), IDC is raised."""
    return M.fp_muladd(acc, a, b, M.S, env, fab=M.BF)


def _abd(a, b, fm, env):
    r, fl = M.fp_sub(a, b, fm, env)
    return M.fp_abs(r, fm), fl


def _compare_mask(a, b, fm, env, rel, bits):
    """A vector compare lane: all ones when true. The ge / gt (and le / lt) signal for every NaN, eq only for the
    signaling ones."""
    if rel == 'le':
        a, b, rel = b, a, 'ge'
    elif rel == 'lt':
        a, b, rel = b, a, 'gt'
    relation, fl = M.fp_compare(a, b, fm, env, rel != 'eq')
    holds = {'eq': relation == 'eq', 'ge': relation in ('eq', 'gt'), 'gt': relation == 'gt'}[rel]
    return (mask(bits) if holds else 0), fl


def _fjcvtzs(bits, env):
    return M.fp_jcvtzs(bits, env)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--seed', type=int, default=20240607)
    args = parser.parse_args()
    b = Builder(args.seed)
    b.scalar_rows(4)
    b.vector_forms()
    b.element_forms()
    b.pairwise_forms()
    b.conversion_forms()
    b.complex_forms()
    b.fhm_forms()
    b.bfloat_forms()
    b.other_forms()
    b.rows.write(OUTPUT, 'ARM64_SIMD_ROWS_FLOAT', 'gen_simd_semantic_rows_float.py')


if __name__ == '__main__':
    main()
