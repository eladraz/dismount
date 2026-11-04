#!/usr/bin/env python3
"""Generate tests/arm/ArmFloatVectors.inc, the vectors of tests/TestSemanticFloatArm.cpp: operations of the
software floating point library (SemanticFloat) in the ARM environment (the pseudocode of the Arm ARM: the flush to
zero with the denormal operand flag, the default NaN, the alternative half precision), with the expected results
and exception flags computed by the independent Python model armfp.py (checked against the Apple M4 by
check_armfp.py).

Row: { operation, format, format2, rounding, control, a, b, c, result, flags }
  control: bit 0 FZ, bit 1 FZ16, bit 2 DN, bit 3 AHP, bit 4 the operands are signed integers / the result is signed
  flags: the SemanticFloatFlags bits (invalid 1, denormal 2, divide by zero 4, overflow 8, underflow 16, inexact 32)

    gen_arm_float_vectors.py [--seed N]
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

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'ArmFloatVectors.inc')

# The operations, see TestSemanticFloatArm.cpp
(OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT, OP_FMA, OP_MULX, OP_RECIP_STEP, OP_RSQRT_STEP, OP_RECIP_ESTIMATE,
 OP_RSQRT_ESTIMATE, OP_RECIP_EXPONENT, OP_ROUND32, OP_ROUND64, OP_SCALE, OP_MAX_NUMBER, OP_MIN_NUMBER, OP_CONVERT,
 OP_FMA_WIDENING, OP_BFLOAT_DOT, OP_TO_INTEGER, OP_FROM_INTEGER, OP_COMPARE_SIGNALING, OP_ROUND_INEXACT,
 OP_MAX, OP_MIN) = range(26)

FORMAT_ID = {16: 1, 32: 2, 64: 3}      # 0 is not used; the test maps them to SemanticType::FloatFormat
FLAG_BITS = ((M.IOC, 1), (M.IDC, 2), (M.DZC, 4), (M.OFC, 8), (M.UFC, 16), (M.IXC, 32))


def flags_of(arm_flags):
    out = 0
    for arm, ir in FLAG_BITS:
        if arm_flags & arm:
            out |= ir
    return out


def control_of(env):
    return env.fz | (env.fz16 << 1) | (env.dn << 2) | (env.ahp << 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--seed', type=int, default=5)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    rows = []

    def env():
        if rng.random() < 0.3:
            return M.Env()
        return M.Env(rng.randint(0, 3), rng.getrandbits(1), rng.getrandbits(1), rng.getrandbits(1),
                     1 if rng.random() < 0.3 else 0)

    def add(op, bits, bits2, e, a, b, c, result, flags, signed=0):
        rows.append((op, FORMAT_ID[bits], FORMAT_ID.get(bits2, 0), e.rmode, control_of(e) | (signed << 4),
                     a, b, c, result, flags_of(flags)))

    for bits in (16, 32, 64):
        f = M.FORMATS[bits]
        for n in range(14):
            e = env()
            a, b, c = (C.draw(rng, bits) for _ in range(3))
            for op, fn in ((OP_ADD, lambda: M.fp_add(a, b, f, e)), (OP_SUB, lambda: M.fp_sub(a, b, f, e)),
                           (OP_MUL, lambda: M.fp_mul(a, b, f, e)), (OP_DIV, lambda: M.fp_div(a, b, f, e)),
                           (OP_FMA, lambda: M.fp_muladd(c, a, b, f, e)),
                           (OP_MULX, lambda: M.fp_mulx(a, b, f, e)),
                           (OP_RECIP_STEP, lambda: M.fp_recip_step(a, b, f, e)),
                           (OP_RSQRT_STEP, lambda: M.fp_rsqrt_step(a, b, f, e)),
                           (OP_MAX_NUMBER, lambda: M.fp_max(a, b, f, e, True, True)),
                           (OP_MIN_NUMBER, lambda: M.fp_max(a, b, f, e, False, True)),
                           (OP_MAX, lambda: M.fp_max(a, b, f, e, True, False)),
                           (OP_MIN, lambda: M.fp_max(a, b, f, e, False, False))):
                result, flags = fn()
                add(op, bits, 0, e, a, b, c, result, flags)
            for op, fn in ((OP_SQRT, lambda: M.fp_sqrt(a, f, e)),
                           (OP_RECIP_ESTIMATE, lambda: M.fp_recip_estimate(a, f, e)),
                           (OP_RSQRT_ESTIMATE, lambda: M.fp_rsqrt_estimate(a, f, e)),
                           (OP_RECIP_EXPONENT, lambda: M.fp_recipx(a, f, e))):
                result, flags = fn()
                add(op, bits, 0, e, a, 0, 0, result, flags)
            if bits != 16:
                for op, bound, rmode in ((OP_ROUND32, 32, M.RZ), (OP_ROUND64, 64, M.RZ), (OP_ROUND32, 32, None),
                                         (OP_ROUND64, 64, None)):
                    result, flags = M.fp_round_int_bounded(a, f, e, rmode if rmode is not None else e.rmode, bound)
                    # rounding None is the dynamic one: the environment's own mode
                    add(op, bits, 0, e, a, 0, 0, result, flags)
                    if rmode is not None:
                        rows[-1] = rows[-1][:3] + (M.RZ,) + rows[-1][4:]
                    else:
                        pass
            # scale by an integer
            scale = rng.choice([0, 1, -1, 5, -5, 40, -40, 300, -300, 3000, -3000, rng.randint(-100, 100)])
            result, flags = M.fp_scale(a, scale, f, e)
            add(OP_SCALE, bits, 0, e, a, scale & ((1 << 64) - 1), 0, result, flags)
            # the signaling compare: a bit pattern of the relation in the result
            rel, flags = M.fp_compare(a, b, f, e, True)
            add(OP_COMPARE_SIGNALING, bits, 0, e, a, b, 0, {'lt': 0, 'eq': 1, 'gt': 2, 'un': 3}[rel], flags)
            # float to integer and back, with fraction bits
            for width in (32, 64):
                for fbits in (0, 3, width - 1):
                    for signed in (0, 1):
                        result, flags = M.fp_to_fixed(a, f, e, width, bool(signed), fbits, M.RZ)
                        add(OP_TO_INTEGER, bits, width, e, a, fbits, 0, result, flags, signed)
                        rows[-1] = rows[-1][:3] + (M.RZ,) + rows[-1][4:]
                        v = rng.getrandbits(width)
                        result, flags = M.fixed_to_fp(v, width, bool(signed), fbits, f, e)
                        add(OP_FROM_INTEGER, bits, width, e, v, fbits, 0, result, flags, signed)
        # the conversions
        for bits2 in (16, 32, 64):
            if bits2 == bits:
                continue
            for n in range(10):
                e = env()
                a = C.draw(rng, bits)
                result, flags = M.fp_convert(a, f, M.FORMATS[bits2], e)
                add(OP_CONVERT, bits, bits2, e, a, 0, 0, result, flags)
    # the widening multiply-add: half products in single (FMLAL) and the BFloat16 dot product
    for n in range(40):
        e = env()
        a, b = C.draw(rng, 16), C.draw(rng, 16)
        c = C.draw(rng, 32)
        result, flags = M.fp_muladd(c, a, b, M.S, e, fab=M.H)
        add(OP_FMA_WIDENING, 32, 16, e, a, b, c, result, flags)

    def bf():
        if rng.random() < 0.4:
            return rng.choice([0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f80, 0xff80, 0x7fc0, 0x7f81, 0x0001, 0x0040, 0x7f7f, 0x4f00])
        return (rng.getrandbits(1) << 15) | (rng.randint(100, 150) << 7) | rng.getrandbits(7)
    for n in range(60):
        e = env()
        acc = C.draw(rng, 32) if rng.random() < 0.5 else ((rng.getrandbits(1) << 31) | (rng.randint(100, 150) << 23) | rng.getrandbits(23))
        a0, a1, b0, b1 = bf(), bf(), bf(), bf()
        result = M.bfloat_dot_add(acc, a0, a1, b0, b1)
        # a, b and c carry the pairs: a = a1:a0, b = b1:b0, c = the addend
        add(OP_BFLOAT_DOT, 32, 16, e, (a1 << 16) | a0, (b1 << 16) | b0, acc, result, 0)
    with open(OUTPUT, 'w') as f:
        f.write('// Generated by tests/arm/tools/gen_arm_float_vectors.py: the vectors of tests/TestSemanticFloatArm.cpp.\n')
        f.write('// { operation, format, format2, rounding, control, a, b, c, result, flags }\n')
        f.write('#define ARM_FLOAT_VECTORS \\\n')
        for r in rows:
            f.write('    { %d, %d, %d, %d, 0x%x, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, 0x%x }, \\\n' % r)
        f.write('\n')
    print('wrote %s: %d vectors' % (OUTPUT, len(rows)))


if __name__ == '__main__':
    main()
