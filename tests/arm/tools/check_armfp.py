#!/usr/bin/env python3
"""Check the Python model of the floating point pseudocode (armfp.py) against the processors: random scalar
instructions with special and random operands in every FPCR mode run on the Apple M4 (and on Unicorn) and
in the model; the result register, the cumulative flags (and NZCV for the compares) must agree. The model is the
source of the unit rows of the floating point instructions, so this is what makes the rows the architecture's.

    out/venv/bin/python tests/arm/tools/check_armfp.py [--oracle host|unicorn] [--count N] [--only REGEX]
"""
import argparse
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tests', 'arm', 'semantics'))
import armfp as M                     # noqa: E402
import armasm                         # noqa: E402
import armstate as S                  # noqa: E402

SPECIAL = {
    16: [0x0000, 0x8000, 0x3c00, 0xbc00, 0x4000, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01, 0xfc01, 0x7d00, 0x0001,
         0x8001, 0x03ff, 0x83ff, 0x0400, 0x7bff, 0xfbff, 0x6400, 0x5640, 0x3c01, 0x7fff, 0x3800, 0x4200, 0xc200],
    32: [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40000000, 0x3f000000, 0x7f800000, 0xff800000, 0x7fc00000,
         0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000,
         0x7f7fffff, 0xff7fffff, 0x4b000000, 0xcb000000, 0x4effffff, 0x4f000000, 0xcf000000, 0x5f000000, 0xdf000000,
         0x3f800001, 0x41200000, 0x3effffff, 0x7fffffff, 0x40400000, 0x40490fdb, 0x3fc00000, 0x00400000],
    64: [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x4000000000000000,
         0x3fe0000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
         0x7ff0000000000001, 0xfff0000000000001, 0x7ff4000000000000, 0x0000000000000001, 0x8000000000000001,
         0x000fffffffffffff, 0x800fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
         0x4330000000000000, 0xc330000000000000, 0x41dfffffffc00000, 0x41e0000000000000, 0xc1e0000000000000,
         0x43e0000000000000, 0xc3e0000000000000, 0x3ff0000000000001, 0x4024000000000000, 0x7fffffffffffffff,
         0x400921fb54442d18, 0x0008000000000000],
}


def draw(rng, bits):
    f = M.FORMATS[bits]
    r = rng.random()
    if r < 0.5:
        return rng.choice(SPECIAL[bits])
    if r < 0.85:
        exp = max(1, min((1 << f.E) - 2, f.bias + rng.randint(-26, 26)))
        return (rng.getrandbits(1) << (bits - 1)) | (exp << f.M) | rng.getrandbits(f.M)
    return rng.getrandbits(bits)


def draw_env(rng):
    if rng.random() < 0.5:
        return M.Env()
    return M.Env(rng.randint(0, 3), rng.getrandbits(1), rng.getrandbits(1), rng.getrandbits(1),
                 1 if rng.random() < 0.25 else 0)


REG = {16: 'h', 32: 's', 64: 'd'}


def operations():
    """(name, text template, kind, model). kind: 'v0' (the register v0), 'x0' (w0 / x0), 'nzcv'."""
    ops = []

    def binary(name, model):
        for bits in (16, 32, 64):
            r = REG[bits]
            ops.append(('%s %s' % (name, r), '%s %s0, %s1, %s2' % (name, r, r, r), 'v0', bits,
                        lambda a, e, b=bits, m=model: m(a[0], a[1], M.FORMATS[b], e)))

    binary('fadd', M.fp_add)
    binary('fsub', M.fp_sub)
    binary('fmul', M.fp_mul)
    binary('fdiv', M.fp_div)
    binary('fmulx', M.fp_mulx)
    binary('fmax', lambda a, b, f, e: M.fp_max(a, b, f, e, True, False))
    binary('fmin', lambda a, b, f, e: M.fp_max(a, b, f, e, False, False))
    binary('fmaxnm', lambda a, b, f, e: M.fp_max(a, b, f, e, True, True))
    binary('fminnm', lambda a, b, f, e: M.fp_max(a, b, f, e, False, True))
    binary('frecps', M.fp_recip_step)
    binary('frsqrts', M.fp_rsqrt_step)

    def fnmul(a, b, f, e):
        r, fl = M.fp_mul(a, b, f, e)
        return M.fp_neg(r, f), fl
    binary('fnmul', fnmul)

    def unary(name, model, sizes=(16, 32, 64)):
        for bits in sizes:
            r = REG[bits]
            ops.append(('%s %s' % (name, r), '%s %s0, %s1' % (name, r, r), 'v0', bits,
                        lambda a, e, b=bits, m=model: m(a[0], M.FORMATS[b], e)))

    unary('fsqrt', M.fp_sqrt)
    unary('frecpe', M.fp_recip_estimate)
    unary('frsqrte', M.fp_rsqrt_estimate)
    unary('frecpx', M.fp_recipx)
    unary('fabs', lambda a, f, e: (M.fp_abs(a, f), 0))
    unary('fneg', lambda a, f, e: (M.fp_neg(a, f), 0))
    for name, mode, exact in (('frintn', M.RN, False), ('frintp', M.RP, False), ('frintm', M.RM, False),
                              ('frintz', M.RZ, False), ('frinta', M.RA, False)):
        unary(name, lambda a, f, e, m=mode: M.fp_round_int(a, f, e, m, False))
    unary('frintx', lambda a, f, e: M.fp_round_int(a, f, e, e.rmode, True))
    unary('frinti', lambda a, f, e: M.fp_round_int(a, f, e, e.rmode, False))
    for bits in (32, 64):
        unary('frint32z', lambda a, f, e: M.fp_round_int_bounded(a, f, e, M.RZ, 32), (bits,))
        unary('frint32x', lambda a, f, e: M.fp_round_int_bounded(a, f, e, e.rmode, 32), (bits,))
        unary('frint64z', lambda a, f, e: M.fp_round_int_bounded(a, f, e, M.RZ, 64), (bits,))
        unary('frint64x', lambda a, f, e: M.fp_round_int_bounded(a, f, e, e.rmode, 64), (bits,))

    # fused multiply-add: Rd = Ra + Rn * Rm; the operands are a = (n, m, a)
    for bits in (16, 32, 64):
        r = REG[bits]
        f = M.FORMATS[bits]
        for name, negate_n, negate_a in (('fmadd', 0, 0), ('fmsub', 1, 0), ('fnmadd', 1, 1), ('fnmsub', 0, 1)):
            def model(a, e, f=f, negate_n=negate_n, negate_a=negate_a):
                n, m, acc = a
                if negate_n:
                    n = M.fp_neg(n, f)
                if negate_a:
                    acc = M.fp_neg(acc, f)
                return M.fp_muladd(acc, n, m, f, e)
            ops.append(('%s %s' % (name, r), '%s %s0, %s1, %s2, %s3' % (name, r, r, r, r), 'v0', bits, model))

    # compares
    for bits in (16, 32, 64):
        r = REG[bits]
        for name, signal in (('fcmp', False), ('fcmpe', True)):
            ops.append(('%s %s' % (name, r), '%s %s1, %s2' % (name, r, r), 'nzcv', bits,
                        lambda a, e, b=bits, sg=signal: _compare(a, e, b, sg)))
    ops.append(('fjcvtzs', 'fjcvtzs w0, d1', 'jcvt', 64, lambda a, e: M.fp_jcvtzs(a[0], e)))
    # precision conversions
    for src, dst in ((16, 32), (16, 64), (32, 16), (32, 64), (64, 16), (64, 32)):
        ops.append(('fcvt %s<-%s' % (REG[dst], REG[src]), 'fcvt %s0, %s1' % (REG[dst], REG[src]), 'v0', src,
                    lambda a, e, s=src, d=dst: M.fp_convert(a[0], M.FORMATS[s], M.FORMATS[d], e)))
    ops.append(('fcvtxn', 'fcvtxn s0, d1', 'v0', 64,
                lambda a, e: M.fp_convert(a[0], M.D, M.S, e, M.RO)))
    # float to integer and back
    letters = (('n', M.RN), ('m', M.RM), ('p', M.RP), ('z', M.RZ), ('a', M.RA))
    for bits in (16, 32, 64):
        r = REG[bits]
        for letter, mode in letters:
            for sign in ('s', 'u'):
                for width in (32, 64):
                    reg = 'w' if width == 32 else 'x'
                    ops.append(('fcvt%s%s %s,%s' % (letter, sign, reg, r),
                                'fcvt%s%s %s0, %s1' % (letter, sign, reg, r), 'x0', bits,
                                lambda a, e, b=bits, m=mode, sg=sign, w=width:
                                M.fp_to_fixed(a[0], M.FORMATS[b], e, w, sg == 's', 0, m)))
        for sign in ('s', 'u'):
            for width in (32, 64):
                reg = 'w' if width == 32 else 'x'
                for fbits in (0, 1, 7, width - 1):
                    suffix = ', #%d' % fbits if fbits else ''
                    if fbits == 0:
                        suffix = ''
                    ops.append(('fcvtz%s %s,%s,#%d' % (sign, reg, r, fbits),
                                'fcvtz%s %s0, %s1%s' % (sign, reg, r, suffix if fbits else ''), 'x0', bits,
                                lambda a, e, b=bits, sg=sign, w=width, fb=fbits:
                                M.fp_to_fixed(a[0], M.FORMATS[b], e, w, sg == 's', fb, M.RZ)))
                    ops.append(('%scvtf %s,%s,#%d' % (sign, r, reg, fbits),
                                '%scvtf %s0, %s1%s' % (sign, r, reg, suffix if fbits else ''), 'int', width,
                                lambda a, e, b=bits, sg=sign, w=width, fb=fbits:
                                M.fixed_to_fp(a[0], w, sg == 's', fb, M.FORMATS[b], e)))
    return ops


def _compare(a, e, bits, signal):
    rel, fl = M.fp_compare(a[0], a[1], M.FORMATS[bits], e, signal)
    return M.NZCV_OF[rel], fl


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--oracle', default='host')
    parser.add_argument('--count', type=int, default=200)
    parser.add_argument('--only', default='')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('-v', action='store_true')
    args = parser.parse_args()
    import oracle_host
    import oracle_arm64
    rng = random.Random(args.seed)
    bad = 0
    total = 0
    for name, text, kind, bits, model in operations():
        if args.only and not re.search(args.only, name):
            continue
        hexb, err = armasm.assemble('a64', [text])[0]
        if err:
            print('ASM_ERR', text, err)
            continue
        code = bytes.fromhex(hexb)
        arity = 3 if text.count(',') == 3 else (2 if text.count(',') == 2 else 1)
        mismatches = 0
        for _ in range(args.count):
            env = draw_env(rng)
            if kind == 'int':
                width = bits
                values = [rng.choice([0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
                                      0xffffffff, (1 << 63) - 1, 1 << 63, (1 << 64) - 1, 0x123456789abcdef])
                          if rng.random() < 0.5 else rng.getrandbits(64)]
                values = [values[0] & ((1 << width) - 1)]
                opbits = width
            else:
                opbits = bits
                values = [draw(rng, bits) for _ in range(4)]
            st = S.default_state('a64')
            st['x'] = [0] * 31
            st['fpcr'] = env.fpcr()
            if kind == 'int':
                st['x'][1] = values[0]
            else:
                for i, v in enumerate(values[:3]):
                    st['v'][i + 1] = v
            S.complete_vectors(st)
            if args.oracle == 'host':
                res = oracle_host.get_host().run(code, st)
            else:
                res = oracle_arm64.get_oracle(128).run(code, st)
            inputs = values
            if kind == 'int':
                want, flags = model(inputs, env)
            elif kind == 'jcvt':
                value, nzcv, flags = model(inputs, env)
                want = (value, nzcv)
            elif kind == 'nzcv':
                want, flags = model(inputs, env)
            else:
                # the 3 operand forms: fmadd(n, m, a) = v1 v2 v3: the registers are v1, v2, v3
                if text.count(',') == 3:
                    st_values = inputs[:3]
                elif text.count(',') == 2:
                    st_values = inputs[:2]
                else:
                    st_values = inputs[:1]
                want, flags = model(st_values, env)
            got_flags = res['fpsr'] & 0x9f
            if kind == 'x0':
                got = res['x'][0]
                mask = (1 << 32) - 1 if text.split()[1].startswith('w') else (1 << 64) - 1
                ok = (got & mask) == want and got_flags == flags
            elif kind == 'nzcv':
                got = res['nzcv']
                ok = got == want and got_flags == flags
            elif kind == 'jcvt':
                got = (res['x'][0] & 0xffffffff, res['nzcv'])
                ok = got == want and got_flags == flags
            else:
                got = res['v'][0]
                if kind == 'int':
                    dst_bits = {'h': 16, 's': 32, 'd': 64}[text.split()[1][0]]
                else:
                    dst_bits = {'h': 16, 's': 32, 'd': 64}[text.split()[1][0]]
                ok = (got == want) and got_flags == flags
            total += 1
            if not ok:
                mismatches += 1
                bad += 1
                if args.v or mismatches <= 2:
                    print('MISMATCH %-28s in=%s fpcr=%x want=%s/%02x got=%s/%02x' % (
                        text, ' '.join('%x' % v for v in inputs[:arity]), env.fpcr(), want if isinstance(want, tuple) else '%x' % want, flags, got if isinstance(got, tuple) else '%x' % got, got_flags))
        if mismatches:
            print('  %s: %d/%d mismatches' % (name, mismatches, args.count))
    print('%d checks, %d mismatches' % (total, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
