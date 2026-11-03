#!/usr/bin/env python3
"""
Regenerates tests/semantics/corpus/sse_edge.asm: the MMX / SSE / SSE2 edge cases
of the semantics tests (see tests/semantics/TEST_PLAN.md, "SSE").

  python3 tests/semantics/tools/gen_sse_edge.py [--out FILE]

Each line is an instruction followed by `@state:` comments: hex tokens of the
`dismount_cli run` protocol (xmm0..7, r0..r7, mxcsr, eax.., mem=), applied on
top of a random state (run_semantics.py also draws 16 random states for every
line, from the float edge pools).

The cases: NaNs (signaling, quiet, default), infinities, zeros, denormals
with every combination of FTZ / DAZ, the four rounding modes, float to integer
conversions at and past the limits, shift counts, saturation, and the
alignment of the 128 bit operands.

The lines that need a NaN in both operands of an add / sub / mul / div use
xmm3 and xmm4: QEMU 5 picks the NaN of the larger significand where the SDM
picks the first operand (oracle_bugs.txt).
"""
import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, '..', 'corpus', 'sse_edge.asm'))

# --- float constants (bits) ---------------------------------------------------
S_ZERO, S_NZERO, S_ONE, S_NONE = 0x00000000, 0x80000000, 0x3f800000, 0xbf800000
S_INF, S_NINF, S_QNAN, S_NQNAN = 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000
S_SNAN, S_NSNAN = 0x7fa00000, 0xffa00000
S_DEN1, S_DENMAX, S_MINNORM, S_NDEN1 = 0x00000001, 0x007fffff, 0x00800000, 0x80000001
S_NMINNORM, S_MAXNORM = 0x80800000, 0x7f7fffff
S_HALF, S_ONE5, S_TWO5, S_THREE5 = 0x3f000000, 0x3fc00000, 0x40200000, 0x40600000
S_NONE5, S_NTWO5, S_TWO31, S_NTWO31 = 0xbfc00000, 0xc0200000, 0x4f000000, 0xcf000000
S_BIG = 0x60ad78ec                      # 1e20
S_NTWO31_1 = 0xcf000001                 # -2147483904

D_ZERO, D_NZERO, D_ONE, D_NONE = 0, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000
D_INF, D_NINF, D_QNAN, D_NQNAN = 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000
D_SNAN, D_NSNAN = 0x7ff4000000000000, 0xfff4000000000000
D_DEN1, D_DENMAX, D_MINNORM, D_NDEN1 = 1, 0x000fffffffffffff, 0x0010000000000000, 0x8000000000000001
D_NMINNORM, D_MAXNORM = 0x8010000000000000, 0x7fefffffffffffff
D_HALF, D_ONE5, D_TWO5, D_THREE5 = 0x3fe0000000000000, 0x3ff8000000000000, 0x4004000000000000, 0x400c000000000000
D_NONE5, D_NTWO5 = 0xbff8000000000000, 0xc004000000000000
D_TWO31, D_NTWO31 = 0x41e0000000000000, 0xc1e0000000000000
D_MAXINT, D_MAXINT_HALF = 0x41dfffffffc00000, 0x41dfffffffe00000     # 2147483647, 2147483647.5
D_NTWO31_HALF = 0xc1e0000000200000                                    # -2147483648.5
D_NTWO31_1 = 0xc1e0000000200000
D_BIG = 0x44b52d02c7e14af6                                            # 1e20

# MXCSR values: the masks stay set (0x1f80), RC (bits 13-14), FZ (15), DAZ (6)
MX_DEFAULT = 0x1f80
MX_DOWN, MX_UP, MX_ZERO = 0x3f80, 0x5f80, 0x7f80
MX_FTZ, MX_DAZ, MX_FTZ_DAZ = 0x9f80, 0x1fc0, 0x9fc0
MX_FTZ_UP, MX_DAZ_UP, MX_FTZ_DAZ_DOWN = 0xdf80, 0x5fc0, 0xbfc0
MODES = [MX_DEFAULT, MX_DOWN, MX_UP, MX_ZERO]
ENVS = [MX_DEFAULT, MX_DOWN, MX_UP, MX_ZERO, MX_FTZ, MX_DAZ, MX_FTZ_DAZ, MX_FTZ_UP,
        MX_DAZ_UP, MX_FTZ_DAZ_DOWN]


def x4(*lanes):
    """An xmm value from 4 dwords (lane 0 first)."""
    assert len(lanes) == 4
    return '%08x%08x%08x%08x' % (lanes[3], lanes[2], lanes[1], lanes[0])


def x2(*lanes):
    """An xmm value from 2 qwords (lane 0 first)."""
    assert len(lanes) == 2
    return '%016x%016x' % (lanes[1], lanes[0])


def x16(*lanes):
    """An xmm value from 16 bytes (byte 0 first)."""
    assert len(lanes) == 16
    return ''.join('%02x' % lanes[i] for i in range(15, -1, -1))


def x8w(*lanes):
    """An xmm value from 8 words (word 0 first)."""
    assert len(lanes) == 8
    return ''.join('%04x' % lanes[i] for i in range(7, -1, -1))


def state(**tokens):
    return ' '.join('%s=%s' % (k, ('%x' % v) if isinstance(v, int) else v)
                    for k, v in sorted(tokens.items()))


class Corpus(object):
    def __init__(self):
        self.out = []

    def section(self, title):
        self.out.append('')
        self.out.append('; --- ' + title)

    def line(self, text, *states):
        comment = ''.join(' @state: ' + s for s in states)
        self.out.append(text + (('  ;' + comment) if comment else ''))


def f32_arith(c):
    c.section('single precision arithmetic: NaNs, infinities, zeros (one NaN operand)')
    first = x4(S_SNAN, S_QNAN, S_ONE, S_NZERO)
    second = x4(S_ONE, S_ONE, S_SNAN, S_ONE)
    zeros = x4(S_ZERO, S_INF, S_NINF, S_ZERO)
    zeros2 = x4(S_ZERO, S_INF, S_INF, S_INF)
    divz = x4(S_ONE, S_NONE, S_ZERO, S_NZERO)
    zero4 = x4(S_ZERO, S_ZERO, S_ZERO, S_ZERO)
    mixed = x4(S_MAXNORM, S_MAXNORM, S_MINNORM, S_NMINNORM)
    mixed2 = x4(S_MAXNORM, S_NONE, S_HALF, S_HALF)
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('ps', 'ss'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   state(xmm1=first, xmm2=second),
                   state(xmm1=zeros, xmm2=zeros2),
                   state(xmm1=divz, xmm2=zero4),
                   state(xmm1=mixed, xmm2=mixed2),
                   state(xmm1=zeros, xmm2=zeros2, mxcsr=MX_UP),
                   state(xmm1=mixed, xmm2=mixed2, mxcsr=MX_ZERO))
    qn = x4(S_NQNAN, S_QNAN, S_NSNAN, S_ONE)
    neg = x4(S_NONE, S_NZERO, S_NINF, S_INF)
    neg2 = x4(0xc0800000, S_ZERO, S_DEN1, 0x7f7fffff)
    for suffix in ('ps', 'ss'):
        c.line('sqrt%s xmm1, xmm2' % suffix, state(xmm2=qn), state(xmm2=neg), state(xmm2=neg2),
               state(xmm2=neg2, mxcsr=MX_UP), state(xmm2=neg2, mxcsr=MX_DAZ))
    nan_a = x4(S_QNAN, S_ONE, S_SNAN, S_ONE)
    nan_b = x4(S_ONE, S_QNAN, S_ONE, S_SNAN)
    zz_a = x4(S_ZERO, S_NZERO, S_ZERO, S_NZERO)
    zz_b = x4(S_NZERO, S_ZERO, S_ZERO, S_NZERO)
    for op in ('min', 'max'):
        for suffix in ('ps', 'ss'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   state(xmm1=nan_a, xmm2=nan_b), state(xmm1=nan_b, xmm2=nan_a),
                   state(xmm1=zz_a, xmm2=zz_b), state(xmm1=zz_b, xmm2=zz_a),
                   state(xmm1=x4(S_DEN1, S_DENMAX, S_ONE, S_NONE), xmm2=x4(S_ZERO, S_NZERO, S_DEN1, S_NDEN1), mxcsr=MX_DAZ),
                   state(xmm1=x4(S_INF, S_NINF, S_ONE, S_NONE), xmm2=x4(S_NINF, S_INF, S_INF, S_NINF)))
    c.section('single precision, a NaN in both operands (QEMU 5 picks the larger significand)')
    both_a = x4(S_QNAN, S_SNAN, S_NQNAN, 0x7fc00001)
    both_b = x4(0x7fc00002, S_QNAN, S_QNAN, S_SNAN)
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('ps', 'ss'):
            c.line('%s%s xmm3, xmm4' % (op, suffix), state(xmm3=both_a, xmm4=both_b),
                   state(xmm3=both_b, xmm4=both_a))


def f64_arith(c):
    c.section('double precision arithmetic: NaNs, infinities, zeros (one NaN operand)')
    first = x2(D_SNAN, D_ONE)
    second = x2(D_ONE, D_QNAN)
    zeros = x2(D_ZERO, D_INF)
    zeros2 = x2(D_ZERO, D_INF)
    other = x2(D_NINF, D_INF)
    divz = x2(D_ONE, D_NZERO)
    zero2 = x2(D_ZERO, D_ZERO)
    mixed = x2(D_MAXNORM, D_MINNORM)
    mixed2 = x2(D_MAXNORM, D_HALF)
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('pd', 'sd'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   state(xmm1=first, xmm2=second),
                   state(xmm1=zeros, xmm2=zeros2),
                   state(xmm1=other, xmm2=x2(D_INF, D_ZERO)),
                   state(xmm1=divz, xmm2=zero2),
                   state(xmm1=mixed, xmm2=mixed2),
                   state(xmm1=zeros, xmm2=zeros2, mxcsr=MX_UP),
                   state(xmm1=mixed, xmm2=mixed2, mxcsr=MX_ZERO))
    neg = x2(D_NONE, D_NINF)
    for suffix in ('pd', 'sd'):
        c.line('sqrt%s xmm1, xmm2' % suffix, state(xmm2=x2(D_NQNAN, D_SNAN)), state(xmm2=neg),
               state(xmm2=x2(D_DEN1, D_TWO5)), state(xmm2=x2(D_TWO5, D_DEN1), mxcsr=MX_UP),
               state(xmm2=x2(D_DEN1, D_ONE), mxcsr=MX_DAZ))
    nan_a = x2(D_QNAN, D_ONE)
    nan_b = x2(D_ONE, D_SNAN)
    zz_a = x2(D_ZERO, D_NZERO)
    zz_b = x2(D_NZERO, D_ZERO)
    for op in ('min', 'max'):
        for suffix in ('pd', 'sd'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   state(xmm1=nan_a, xmm2=nan_b), state(xmm1=nan_b, xmm2=nan_a),
                   state(xmm1=zz_a, xmm2=zz_b), state(xmm1=zz_b, xmm2=zz_a),
                   state(xmm1=x2(D_DEN1, D_DENMAX), xmm2=x2(D_ZERO, D_NZERO), mxcsr=MX_DAZ),
                   state(xmm1=x2(D_INF, D_NINF), xmm2=x2(D_NINF, D_INF)))
    c.section('double precision, a NaN in both operands (QEMU 5 picks the larger significand)')
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('pd', 'sd'):
            c.line('%s%s xmm3, xmm4' % (op, suffix),
                   state(xmm3=x2(D_QNAN, D_SNAN), xmm4=x2(0x7ff8000000000002, D_QNAN)),
                   state(xmm3=x2(0x7ff8000000000002, D_QNAN), xmm4=x2(D_QNAN, D_SNAN)))


def denormals(c):
    c.section('denormals and tiny results with FTZ / DAZ and the four rounding modes')
    a = x4(S_DEN1, S_DENMAX, S_MINNORM, S_NDEN1)
    b = x4(S_DEN1, S_ONE, S_NMINNORM, S_ONE)
    small = x4(0x19fcd59f, 0x3f000000, 0x00800001, 0x00fffffe)
    scale = x4(0x19fcd59f, 0x00800000, 0x3f000000, 0x3f000000)
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('ps', 'ss'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   *[state(xmm1=a, xmm2=b, mxcsr=m) for m in ENVS] +
                    [state(xmm1=small, xmm2=scale, mxcsr=m) for m in ENVS])
    da = x2(D_DEN1, D_DENMAX)
    db = x2(D_DEN1, D_ONE)
    dsmall = x2(0x0008000000000001, 0x0010000000000001)
    dscale = x2(0x3fe0000000000000, 0x3fe0000000000000)
    for op in ('add', 'sub', 'mul', 'div'):
        for suffix in ('pd', 'sd'):
            c.line('%s%s xmm1, xmm2' % (op, suffix),
                   *[state(xmm1=da, xmm2=db, mxcsr=m) for m in ENVS] +
                    [state(xmm1=dsmall, xmm2=dscale, mxcsr=m) for m in ENVS])
    c.section('comparisons with denormals (DAZ) and NaNs')
    cmp_a = x4(S_DEN1, S_ZERO, S_QNAN, S_SNAN)
    cmp_b = x4(S_ZERO, S_NDEN1, S_ONE, S_ONE)
    for pred in ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord'):
        c.line('cmp%sps xmm1, xmm2' % pred,
               *[state(xmm1=cmp_a, xmm2=cmp_b, mxcsr=m) for m in (MX_DEFAULT, MX_DAZ)])
        c.line('cmp%sss xmm1, xmm2' % pred,
               *[state(xmm1=cmp_a, xmm2=cmp_b, mxcsr=m) for m in (MX_DEFAULT, MX_DAZ)])
        c.line('cmp%spd xmm1, xmm2' % pred,
               state(xmm1=x2(D_DEN1, D_QNAN), xmm2=x2(D_ZERO, D_ONE)),
               state(xmm1=x2(D_SNAN, D_ONE), xmm2=x2(D_ONE, D_NZERO), mxcsr=MX_DAZ))
        c.line('cmp%ssd xmm1, xmm2' % pred,
               state(xmm1=x2(D_DEN1, D_QNAN), xmm2=x2(D_ZERO, D_ONE)),
               state(xmm1=x2(D_SNAN, D_ONE), xmm2=x2(D_ONE, D_NZERO), mxcsr=MX_DAZ))
    for text in ('comiss', 'ucomiss'):
        for a_, b_ in ((S_ONE, S_ONE), (S_ONE, S_NONE), (S_NONE, S_ONE), (S_QNAN, S_ONE), (S_ONE, S_SNAN),
                       (S_ZERO, S_NZERO), (S_DEN1, S_ZERO), (S_INF, S_INF), (S_NINF, S_INF)):
            c.line('%s xmm1, xmm2' % text, state(xmm1=x4(a_, 0, 0, 0), xmm2=x4(b_, 0, 0, 0)),
                   state(xmm1=x4(a_, 0, 0, 0), xmm2=x4(b_, 0, 0, 0), mxcsr=MX_DAZ),
                   state(xmm1=x4(a_, 0, 0, 0), xmm2=x4(b_, 0, 0, 0), eflags=0xcd7))
    for text in ('comisd', 'ucomisd'):
        for a_, b_ in ((D_ONE, D_ONE), (D_ONE, D_NONE), (D_NONE, D_ONE), (D_QNAN, D_ONE), (D_ONE, D_SNAN),
                       (D_ZERO, D_NZERO), (D_DEN1, D_ZERO), (D_INF, D_INF), (D_NINF, D_INF)):
            c.line('%s xmm1, xmm2' % text, state(xmm1=x2(a_, 0), xmm2=x2(b_, 0)),
                   state(xmm1=x2(a_, 0), xmm2=x2(b_, 0), mxcsr=MX_DAZ),
                   state(xmm1=x2(a_, 0), xmm2=x2(b_, 0), eflags=0xcd7))


def conversions(c):
    c.section('float to integer: rounding modes, ties, overflow, NaN')
    ties = x4(S_HALF, S_ONE5, S_TWO5, S_NONE5)
    ties2 = x4(S_NTWO5, S_THREE5, 0x3e800000, 0xbe800000)
    limits = x4(S_TWO31, S_NTWO31, S_NTWO31_1, S_BIG)
    special = x4(S_QNAN, S_SNAN, S_INF, S_NINF)
    for op in ('cvtps2dq', 'cvttps2dq'):
        c.line('%s xmm1, xmm2' % op,
               *[state(xmm2=v, mxcsr=m) for v in (ties, ties2, limits, special) for m in MODES])
    for op in ('cvtss2si', 'cvttss2si'):
        c.line('%s eax, xmm2' % op,
               *[state(xmm2=x4(v, 0, 0, 0), mxcsr=m) for v in
                 (S_HALF, S_ONE5, S_TWO5, S_NONE5, S_NTWO5, S_TWO31, S_NTWO31, S_NTWO31_1, S_QNAN, S_INF, S_NINF)
                 for m in MODES])
    dties = x2(D_HALF, D_ONE5)
    dties2 = x2(D_TWO5, D_NONE5)
    dlimits = x2(D_TWO31, D_NTWO31)
    dlimits2 = x2(D_MAXINT_HALF, D_NTWO31_HALF)
    dspecial = x2(D_QNAN, D_INF)
    dspecial2 = x2(D_NINF, D_BIG)
    for op in ('cvtpd2dq', 'cvttpd2dq'):
        c.line('%s xmm1, xmm2' % op,
               *[state(xmm2=v, mxcsr=m) for v in (dties, dties2, dlimits, dlimits2, dspecial, dspecial2) for m in MODES])
    for op in ('cvtsd2si', 'cvttsd2si'):
        c.line('%s eax, xmm2' % op,
               *[state(xmm2=x2(v, 0), mxcsr=m) for v in
                 (D_HALF, D_ONE5, D_TWO5, D_NONE5, D_NTWO5, D_TWO31, D_NTWO31, D_MAXINT, D_MAXINT_HALF,
                  D_NTWO31_HALF, D_QNAN, D_INF, D_NINF, D_BIG)
                 for m in MODES])
    for op in ('cvtps2pi', 'cvttps2pi'):
        c.line('%s mm1, xmm2' % op,
               *[state(xmm2=v, mxcsr=m) for v in (ties, ties2, limits, special) for m in MODES])
    for op in ('cvtpd2pi', 'cvttpd2pi'):
        c.line('%s mm1, xmm2' % op,
               *[state(xmm2=v, mxcsr=m) for v in (dties, dties2, dlimits, dlimits2, dspecial) for m in MODES])
    c.section('integer to float: rounding of the large values')
    big_ints = x4(0x01000001, 0x7fffffff, 0x80000000, 0xfeffffff)
    for op in ('cvtdq2ps',):
        c.line('%s xmm1, xmm2' % op, *[state(xmm2=big_ints, mxcsr=m) for m in MODES])
    c.line('cvtdq2pd xmm1, xmm2', state(xmm2=big_ints), state(xmm2=x4(0x80000000, 0x7fffffff, 0, 0)))
    c.line('cvtpi2ps xmm1, mm2', *[state(r2='ffff%016x' % v, mxcsr=m) for v in
                                    (0x7fffffff01000001, 0x80000000feffffff) for m in MODES])
    c.line('cvtpi2pd xmm1, mm2', state(r2='ffff7fffffff80000000'))
    for reg in ('eax', 'ecx'):
        c.line('cvtsi2ss xmm1, %s' % reg, *[state(eax=v, ecx=v, mxcsr=m) for v in
                                              (0x01000001, 0x7fffffff, 0x80000000, 0xfeffffff, 0xffffffff)
                                              for m in MODES])
        c.line('cvtsi2sd xmm1, %s' % reg, state(eax=0x7fffffff, ecx=0x7fffffff),
               state(eax=0x80000000, ecx=0x80000000))
    c.section('float to float: rounding, overflow, denormals, NaNs')
    dd = x2(0x47efffffe0000001, 0x3810000000000000)            # just above the largest float; a tiny double
    dd2 = x2(0x36a0000000000000, 0xc7efffffefffffff)           # the smallest denormal float / 2; the limits
    dd3 = x2(D_SNAN, D_NQNAN)
    dd4 = x2(D_INF, D_NINF)
    for op in ('cvtpd2ps xmm1, xmm2', 'cvtsd2ss xmm1, xmm2'):
        c.line(op, *[state(xmm2=v, mxcsr=m) for v in (dd, dd2, dd3, dd4) for m in ENVS])
    sd = x4(S_SNAN, S_DEN1, S_DENMAX, S_NINF)
    for op in ('cvtps2pd xmm1, xmm2', 'cvtss2sd xmm1, xmm2'):
        c.line(op, *[state(xmm2=sd, mxcsr=m) for m in (MX_DEFAULT, MX_DAZ)],
               *[state(xmm2=x4(S_QNAN, S_ONE5, S_MAXNORM, S_MINNORM), mxcsr=MX_DEFAULT)])


def integer_edges(c):
    c.section('shift counts: immediate, register, memory')
    value = x8w(0x8000, 0x7fff, 0xffff, 0x0001, 0x8001, 0x00ff, 0xff00, 0x1234)
    for op in ('psllw', 'psrlw', 'psraw', 'pslld', 'psrld', 'psrad', 'psllq', 'psrlq'):
        width = {'w': 16, 'd': 32, 'q': 64}[op[-1]]
        counts = [0, 1, width - 1, width, width + 1, 63, 64, 255]
        c.line('%s xmm1, %d' % (op, 1), state(xmm1=value))
        for count in counts:
            c.line('%s xmm1, %d' % (op, count), state(xmm1=value))
            c.line('%s mm1, %d' % (op, count), state(r1='ffff8000ffff7fff0001'))
        c.line('%s xmm1, xmm2' % op,
               *[state(xmm1=value, xmm2=x2(n, 0)) for n in (0, 1, width - 1, width, 255, 256, 0x10000, 0xffffffff)] +
                [state(xmm1=value, xmm2=x2(n, 1)) for n in (0, 1, 1 << 32, 1 << 63)] +
                [state(xmm1=value, xmm2=x2(0xffffffffffffffff, 0))])
        c.line('%s mm1, mm2' % op,
               *[state(r1='ffff8000ffff7fff0001', r2='ffff%016x' % n) for n in
                 (0, 1, width - 1, width, 255, 256, 1 << 32, 0xffffffffffffffff)])
    c.section('byte shifts of the whole register')
    for op in ('pslldq', 'psrldq'):
        for count in (0, 1, 7, 8, 15, 16, 17, 255):
            c.line('%s xmm1, %d' % (op, count), state(xmm1=x16(*range(1, 17))))
    c.section('saturation: add, subtract, pack')
    b1 = x16(0x7f, 0x80, 0x00, 0xff, 0x7f, 0x80, 0x01, 0xfe, 0x7e, 0x81, 0x00, 0x01, 0xff, 0x80, 0x7f, 0x00)
    b2 = x16(0x01, 0xff, 0x00, 0x01, 0x7f, 0x80, 0xff, 0x02, 0x01, 0xff, 0x80, 0x7f, 0xff, 0x80, 0x01, 0x00)
    w1 = x8w(0x7fff, 0x8000, 0x0000, 0xffff, 0x7fff, 0x8000, 0x0001, 0xfffe)
    w2 = x8w(0x0001, 0xffff, 0x0000, 0x0001, 0x7fff, 0x8000, 0xffff, 0x0002)
    for op in ('paddsb', 'paddusb', 'psubsb', 'psubusb', 'pavgb', 'pminub', 'pmaxub', 'pcmpgtb', 'pcmpeqb'):
        c.line('%s xmm1, xmm2' % op, state(xmm1=b1, xmm2=b2), state(xmm1=b2, xmm2=b1))
    for op in ('paddsw', 'paddusw', 'psubsw', 'psubusw', 'pavgw', 'pminsw', 'pmaxsw', 'pcmpgtw',
               'pmulhw', 'pmulhuw', 'pmullw', 'pmaddwd'):
        c.line('%s xmm1, xmm2' % op, state(xmm1=w1, xmm2=w2), state(xmm1=w2, xmm2=w1),
               state(xmm1=x8w(*[0x8000] * 8), xmm2=x8w(*[0x8000] * 8)))
        c.line('%s mm1, mm2' % op, state(r1='ffff80007fffffff0001', r2='ffff8000800080007fff'))
    for op in ('packsswb', 'packuswb'):
        c.line('%s xmm1, xmm2' % op, state(xmm1=w1, xmm2=w2), state(xmm1=w2, xmm2=w1),
               state(xmm1=x8w(0x0080, 0xff80, 0x007f, 0xff7f, 0x0100, 0xffff, 0x00ff, 0x8000),
                     xmm2=x8w(0x7fff, 0x0000, 0x0001, 0xff00, 0x00fe, 0xfeff, 0x1234, 0xedcb)))
    c.line('packssdw xmm1, xmm2',
           state(xmm1=x4(0x7fffffff, 0x80000000, 0x00008000, 0xffff7fff), xmm2=x4(0x00007fff, 0xffff8000, 0x00018000, 0xfffe7fff)))
    c.line('packssdw mm1, mm2', state(r1='ffff7fffffff80000000', r2='ffff00007fffffff8000'))
    c.line('packsswb mm1, mm2', state(r1='ffff80007fff00800001', r2='ffff01ffff80007f0100'))
    c.line('packuswb mm1, mm2', state(r1='ffff80007fff00800001', r2='ffff01ffff80007f0100'))
    c.section('moves of the masks, extracts, inserts, shuffles')
    signs = x4(0x80000000, 0x7fffffff, 0xffffffff, 0x00000000)
    c.line('movmskps eax, xmm1', state(xmm1=signs), state(xmm1=x4(S_NQNAN, S_NZERO, S_NINF, S_ONE)))
    c.line('movmskpd eax, xmm1', state(xmm1=x2(D_NZERO, D_ONE)), state(xmm1=x2(D_ONE, D_NINF)))
    c.line('pmovmskb eax, xmm1', state(xmm1=b1), state(xmm1=b2))
    c.line('pmovmskb eax, mm1', state(r1='ffff80ff7f00ff800100'))
    for lane in (0, 1, 3, 7, 8, 15):
        c.line('pextrw eax, xmm1, %d' % lane, state(xmm1=w1))
        c.line('pinsrw xmm1, eax, %d' % lane, state(xmm1=w1, eax=0x12345678), state(xmm1=w2, eax=0xffffffff))
        c.line('pinsrw xmm1, word [0x3000], %d' % lane, state(xmm1=w1, mem='00003000:3412'))
        c.line('pextrw eax, mm1, %d' % lane, state(r1='ffff800000ff7fff0001'))
        c.line('pinsrw mm1, eax, %d' % lane, state(r1='ffff800000ff7fff0001', eax=0xabcd1234))
    for imm in (0x00, 0x1b, 0xe4, 0xff, 0x55, 0xaa, 0xc6):
        c.line('pshufd xmm1, xmm2, 0x%02x' % imm, state(xmm2=x4(0x11111111, 0x22222222, 0x33333333, 0x44444444)))
        c.line('pshuflw xmm1, xmm2, 0x%02x' % imm, state(xmm2=x8w(1, 2, 3, 4, 5, 6, 7, 8)))
        c.line('pshufhw xmm1, xmm2, 0x%02x' % imm, state(xmm2=x8w(1, 2, 3, 4, 5, 6, 7, 8)))
        c.line('pshufw mm1, mm2, 0x%02x' % imm, state(r2='ffff0004000300020001'))
        c.line('shufps xmm1, xmm2, 0x%02x' % imm, state(xmm1=x4(1, 2, 3, 4), xmm2=x4(5, 6, 7, 8)))
        c.line('shufpd xmm1, xmm2, 0x%02x' % imm, state(xmm1=x2(1, 2), xmm2=x2(3, 4)))
    c.section('sums of absolute differences, unsigned multiply')
    c.line('psadbw xmm1, xmm2', state(xmm1=b1, xmm2=b2), state(xmm1=x16(*[0xff] * 16), xmm2=x16(*[0] * 16)))
    c.line('psadbw mm1, mm2', state(r1='ffff80ff7f00ff800100', r2='ffff00000000ffffffff'))
    c.line('pmuludq xmm1, xmm2', state(xmm1=x4(0xffffffff, 0xffffffff, 0x80000000, 2), xmm2=x4(0xffffffff, 0, 2, 0x80000000)))
    c.line('pmuludq mm1, mm2', state(r1='ffffffffffffffffffff', r2='ffff00000000ffffffff'))


def alignment(c):
    c.section('alignment of the 128 bit memory operands')
    aligned = 'mem=00003000:' + '00112233445566778899aabbccddeeff' * 4
    for text in ('movaps xmm1, [esi]', 'movapd xmm1, [esi]', 'movdqa xmm1, [esi]',
                 'movaps [esi], xmm1', 'movapd [esi], xmm1', 'movdqa [esi], xmm1',
                 'movntps [esi], xmm1', 'movntpd [esi], xmm1', 'movntdq [esi], xmm1',
                 'addps xmm1, [esi]', 'subpd xmm1, [esi]', 'paddb xmm1, [esi]', 'pxor xmm1, [esi]',
                 'punpcklbw xmm1, [esi]', 'pshufd xmm1, [esi], 0x1b', 'cvtdq2ps xmm1, [esi]',
                 'cvtpd2ps xmm1, [esi]', 'cvttps2dq xmm1, [esi]', 'andps xmm1, [esi]',
                 'unpcklps xmm1, [esi]', 'shufps xmm1, [esi], 0x1b', 'cmpltps xmm1, [esi]',
                 'psllw xmm1, [esi]', 'pmaddwd xmm1, [esi]', 'cvtpd2pi mm1, [esi]', 'sqrtps xmm1, [esi]'):
        c.line(text, state(esi=0x3000, **{'mem': aligned[4:]}), state(esi=0x3001), state(esi=0x3008),
               state(esi=0x300f), state(esi=0x3010))
    for text in ('movups xmm1, [esi]', 'movupd xmm1, [esi]', 'movdqu xmm1, [esi]', 'movups [esi], xmm1',
                 'movupd [esi], xmm1', 'movdqu [esi], xmm1', 'movss xmm1, [esi]', 'movsd xmm1, [esi]',
                 'movss [esi], xmm1', 'movsd [esi], xmm1', 'movd xmm1, [esi]', 'movd [esi], xmm1',
                 'movq xmm1, [esi]', 'movq [esi], xmm1', 'movlps xmm1, [esi]', 'movlps [esi], xmm1',
                 'movhps xmm1, [esi]', 'movhps [esi], xmm1', 'movlpd xmm1, [esi]', 'movhpd [esi], xmm1',
                 'addss xmm1, [esi]', 'addsd xmm1, [esi]', 'cvtps2pd xmm1, [esi]', 'cvtdq2pd xmm1, [esi]',
                 'comiss xmm1, [esi]', 'ucomisd xmm1, [esi]', 'movq mm1, [esi]', 'paddb mm1, [esi]',
                 'movntq [esi], mm1', 'punpcklbw mm1, [esi]', 'cvtpi2ps xmm1, [esi]', 'cvtps2pi mm1, [esi]'):
        c.line(text, state(esi=0x3001), state(esi=0x3003), state(esi=0x2fff), state(esi=0x3000))
    c.section('ldmxcsr / stmxcsr / emms and the fences')
    for value in (0x1f80, 0x1fc0, 0x9fc0, 0x0000, 0x1f81, 0xffff, 0x10000, 0x20001f80, 0xffff1f80):
        c.line('ldmxcsr [0x3000]', state(mem='00003000:%02x%02x%02x%02x' % (
            value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, value >> 24)))
    c.line('stmxcsr [0x3000]', state(mxcsr=0x1fa5), state(mxcsr=MX_FTZ_DAZ))
    c.line('emms', state(ftw=0xff, fsw=0x3800), state(ftw=0x00))
    for text in ('lfence', 'sfence', 'mfence', 'prefetcht0 [esi]', 'prefetcht1 [esi]', 'prefetcht2 [esi]',
                 'prefetchnta [esi]', 'clflush [esi]'):
        c.line(text)
    c.section('maskmovq / maskmovdqu')
    c.line('maskmovdqu xmm1, xmm2', state(xmm1=x16(*range(1, 17)), xmm2=x16(*[0x80, 0, 0xff, 0x7f] * 4), edi=0x3000),
           state(xmm1=x16(*range(1, 17)), xmm2=x16(*[0] * 16), edi=0x3001),
           state(xmm1=x16(*range(1, 17)), xmm2=x16(*[0xff] * 16), edi=0x3001))
    c.line('maskmovq mm1, mm2', state(r1='ffff0807060504030201', r2='ffff80007fff00ff8000', edi=0x3000),
           state(r1='ffff0807060504030201', r2='ffffffffffffffffffff', edi=0x3003))
    c.section('mmx state')
    for text in ('movq mm1, mm2', 'movd mm1, eax', 'movd eax, mm1', 'paddb mm1, mm2', 'pxor mm1, mm1',
                 'pmovmskb eax, mm1', 'movq2dq xmm1, mm2', 'movdq2q mm1, xmm2', 'cvtpi2ps xmm1, mm2',
                 'cvtps2pi mm1, xmm2', 'movq [0x3000], mm1', 'movq mm1, [0x3000]', 'emms'):
        c.line(text, state(fsw=0x3800, ftw=0x3c, fcw=0x37f), state(fsw=0x2000, ftw=0xff, fcw=0x37f),
               state(fsw=0x0000, ftw=0x00))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()
    c = Corpus()
    f32_arith(c)
    f64_arith(c)
    denormals(c)
    conversions(c)
    integer_edges(c)
    alignment(c)
    header = ['; MMX / SSE / SSE2 edge cases for the semantics tests (dismount IR vs. Unicorn), bits 32',
              '; Generated by tests/semantics/tools/gen_sse_edge.py. Do not edit.',
              '; Same format as tests/ia32/corpus, plus `@state:` comments: the tokens of a state',
              '; that are applied over a random one (see tests/semantics/TEST_PLAN.md).',
              '; @file-feature: sse2',
              'bits 32']
    with open(args.out, 'w') as f:
        f.write('\n'.join(header + c.out) + '\n')
    print('wrote %s: %d lines' % (args.out, len([l for l in c.out if l and not l.startswith(';')])))


if __name__ == '__main__':
    main()
