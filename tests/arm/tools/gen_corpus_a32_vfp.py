#!/usr/bin/env python3
"""Write tests/arm/semantics/corpus/a32_vfp.s and t32_vfp.s, the corpus of the AArch32 floating
point instructions (VFP and the floating point Advanced SIMD ones, see
tests/arm/semantics/TEST_PLAN.md): every mnemonic and operand shape of the entries which
gen_a32_vfp_semantic_table.py owns, with directed states (zeros of both signs, infinities, quiet and
signaling NaNs, denormals, the extremes, the rounding modes, FZ, DN, the cumulative flags).

The states are given as `@state:` tokens: `q<n>=` carries the registers (s<4n+i> is the lane i of
q<n>, d<2n+i> the half i), `fpscr=` the control register, `apsr=` the flags. The helper State below
builds them from the s / d registers.

    gen_corpus_a32_vfp.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

OUT_A32 = os.path.join(armlib.REPO, 'tests', 'arm', 'semantics', 'corpus', 'a32_vfp.s')
OUT_T32 = os.path.join(armlib.REPO, 'tests', 'arm', 'semantics', 'corpus', 't32_vfp.s')

F32 = {'0': 0x00000000, '-0': 0x80000000, '1': 0x3f800000, '-1': 0xbf800000, '2': 0x40000000,
       '0.5': 0x3f000000, '3.5': 0x40600000, '0.1': 0x3dcccccd, 'inf': 0x7f800000, '-inf': 0xff800000,
       'qnan': 0x7fc00000, 'snan': 0x7fa00001, '-qnan': 0xffc00123, 'den': 0x00000001, '-den': 0x80000001,
       'maxden': 0x007fffff, 'min': 0x00800000, 'max': 0x7f7fffff, '-max': 0xff7fffff, '1e10': 0x501502f9,
       'big': 0x4f000000, '-big': 0xcf000000, 'int24': 0x4b800001, '1.5': 0x3fc00000, '-1.5': 0xbfc00000,
       '2.5': 0x40200000, '-2.5': 0xc0200000}
F64 = {'0': 0, '-0': 1 << 63, '1': 0x3ff0000000000000, '-1': 0xbff0000000000000, '2': 0x4000000000000000,
       '3.5': 0x400c000000000000, '0.1': 0x3fb999999999999a, 'inf': 0x7ff0000000000000,
       '-inf': 0xfff0000000000000, 'qnan': 0x7ff8000000000000, 'snan': 0x7ff4000000000001,
       'den': 1, '-den': (1 << 63) | 1, 'maxden': 0x000fffffffffffff, 'min': 0x0010000000000000,
       'max': 0x7fefffffffffffff, '1e300': 0x7e37e43c8800759c, 'big': 0x41e0000000000000,
       '-big': 0xc1e0000000000000, '2^63': 0x43e0000000000000, '-2^63': 0xc3e0000000000000,
       '1.5': 0x3ff8000000000000, '-2.5': 0xc004000000000000, '2^52': 0x4330000000000000}
F16 = {'0': 0, '-0': 0x8000, '1': 0x3c00, '-1': 0xbc00, '2': 0x4000, '3.5': 0x4300, 'inf': 0x7c00,
       '-inf': 0xfc00, 'qnan': 0x7e00, 'snan': 0x7d01, 'den': 1, 'maxden': 0x03ff, 'min': 0x0400,
       'max': 0x7bff, '-max': 0xfbff, '1.5': 0x3e00, '-2.5': 0xc100, '0.1': 0x2e66}
# FPSCR control values: default, FZ, DN, the rounding modes, combinations, cumulative flags set
FPSCRS = [0, 0x01000000, 0x02000000, 0x00400000, 0x00800000, 0x00c00000, 0x03c00000, 0x01400000,
          0x0000009f, 0x04000000, 0xf0000000, 0x08000000]
# (FZ16, bit 19, only for the instructions Unicorn doesn't run: it has no FEAT_FP16)
FPSCRS_FZ16 = [0x00080000, 0x01080000, 0x05c80000]


def hexq(lanes):
    """The hex of a 128 bit register from four 32 bit lanes (lane 0 first)."""
    return '%08x%08x%08x%08x' % (lanes[3], lanes[2], lanes[1], lanes[0])


class State(object):
    """The registers a directed state sets: s<n> (32 bit), d<n> (64 bit), fpscr, apsr, r<n>."""

    def __init__(self):
        self.s = {}
        self.fpscr = None
        self.apsr = None
        self.r = {}

    def set_s(self, n, value):
        self.s[n] = value & 0xffffffff
        return self

    def set_d(self, n, value):
        self.s[2 * n] = value & 0xffffffff
        self.s[2 * n + 1] = (value >> 32) & 0xffffffff
        return self

    def set_h(self, n, half, value):
        """Half precision value `value` in the half `half` (0, 1) of s<n>."""
        old = self.s.get(n, 0)
        old &= ~(0xffff << (16 * half)) & 0xffffffff
        self.s[n] = old | ((value & 0xffff) << (16 * half))
        return self

    def set_r(self, n, value):
        self.r[n] = value
        return self

    def tokens(self):
        out = []
        for q in range(16):
            lanes = [self.s.get(4 * q + i) for i in range(4)]
            if any(v is not None for v in lanes):
                out.append('q%d=%s' % (q, hexq([v or 0 for v in lanes])))
        if self.fpscr is not None:
            out.append('fpscr=%x' % self.fpscr)
        if self.apsr is not None:
            out.append('apsr=%x' % self.apsr)
        for n, v in sorted(self.r.items()):
            out.append('r%d=%x' % (n, v))
        return ' '.join(out)


class Corpus(object):
    def __init__(self, title):
        self.lines = ['// %s' % title]

    def comment(self, text):
        self.lines.append('// --- %s' % text)

    def add(self, text, states=()):
        tail = ' '.join('@state: %s' % s.tokens() for s in states if s.tokens())
        self.lines.append(text + ('   // ' + tail if tail else ''))

    def write(self, path):
        with open(path, 'w') as f:
            f.write('\n'.join(self.lines) + '\n')


def pairs(table, names):
    return [(table[a], table[b]) for a, b in names]


F32_PAIRS = pairs(F32, [('1', '2'), ('inf', '-inf'), ('0', 'inf'), ('snan', '1'), ('1', 'qnan'), ('den', '1'),
                        ('max', 'max'), ('min', '0.5'), ('-0', '0'), ('3.5', '0.1'), ('-qnan', 'snan'),
                        ('maxden', 'min'), ('1', '0'), ('0', '0'), ('-max', '2'), ('-1', '-0')])
F64_PAIRS = pairs(F64, [('1', '2'), ('inf', '-inf'), ('0', 'inf'), ('snan', '1'), ('1', 'qnan'), ('den', '1'),
                        ('max', 'max'), ('min', '0.1'), ('-0', '0'), ('3.5', '0.1'), ('maxden', 'min'),
                        ('1', '0'), ('0', '0'), ('-1', '1e300')])
F16_PAIRS = pairs(F16, [('1', '2'), ('inf', '-inf'), ('0', 'inf'), ('snan', '1'), ('1', 'qnan'), ('den', '1'),
                        ('max', 'max'), ('min', '0.1'), ('-0', '0'), ('3.5', '0.1'), ('maxden', 'min')])


def pick_fpscr(fmt, i, fz16):
    table = FPSCRS
    if fz16 is None:
        fz16 = fmt == 'f16'
    if fz16:
        table = FPSCRS[:5] + FPSCRS_FZ16 + FPSCRS[5:]
    return table[i % len(table)]


def scalar_states(fmt, regs, with_accumulator=False, count=None, fz16=None):
    """Directed states for a scalar instruction `regs` = (d, n, m) register numbers (of s or d)."""
    table = {'f32': F32_PAIRS, 'f64': F64_PAIRS, 'f16': F16_PAIRS}[fmt]
    out = []
    for i, (a, b) in enumerate(table[:count]):
        st = State()
        d, n, m = regs
        if fmt == 'f32':
            st.set_s(n, a).set_s(m, b)
            if with_accumulator:
                st.set_s(d, table[(i + 3) % len(table)][0])
        elif fmt == 'f64':
            st.set_d(n, a).set_d(m, b)
            if with_accumulator:
                st.set_d(d, table[(i + 3) % len(table)][0])
        else:
            st.set_h(n, 0, a).set_h(m, 0, b)
            if with_accumulator:
                st.set_h(d, 0, table[(i + 3) % len(table)][0])
        st.fpscr = pick_fpscr(fmt, i, fz16)
        out.append(st)
    return out


def vector_states(fmt, count=None, fz16=None):
    """Directed states for a NEON instruction on d1, d2 (and q1, q2 filled alike)."""
    table = {'f32': F32_PAIRS, 'f16': F16_PAIRS}[fmt]
    out = []
    for i, (a, b) in enumerate(table[:count]):
        st = State()
        c = table[(i + 5) % len(table)]
        d = table[(i + 7) % len(table)]
        if fmt == 'f32':
            # d1 = q0.hi (s2 s3), d2 = q1.lo (s4 s5); q1 = (s4 s5 s6 s7), q2 = (s8..s11)
            st.set_s(2, a).set_s(3, c[0]).set_s(4, b).set_s(5, d[1])
            st.set_s(6, c[1]).set_s(7, a).set_s(8, d[0]).set_s(9, b).set_s(10, a).set_s(11, c[0])
            st.set_s(0, d[1]).set_s(1, c[1])
        else:
            for n in range(12):
                st.s[n] = ((table[(i + n + 1) % len(table)][1] << 16) | table[(i + n) % len(table)][0]) & 0xffffffff
            st.s[2] = ((c[0] << 16) | a) & 0xffffffff
            st.s[4] = ((d[0] << 16) | b) & 0xffffffff
        st.fpscr = pick_fpscr(fmt, i, fz16)
        out.append(st)
    return out


def gen_a32():
    c = Corpus('A32 floating point instructions (VFP and Advanced SIMD floating point), assembled for armv8.6-a')

    # ---- VFP arithmetic
    c.comment('VFP arithmetic: f32 on s registers, f64 on d registers, f16 (FEAT_FP16)')
    for fmt, regs, rn in (('f32', (0, 1, 2), 's'), ('f64', (0, 1, 2), 'd'), ('f16', (0, 1, 2), 's')):
        d, n, m = regs
        for op in ('vadd', 'vsub', 'vmul', 'vdiv', 'vnmul'):
            if fmt == 'f16' and op == 'vdiv':
                pass
            c.add('%s.%s %s0, %s1, %s2' % (op, fmt, rn, rn, rn), scalar_states(fmt, regs))
        for op in ('vmla', 'vmls', 'vnmla', 'vnmls', 'vfma', 'vfms', 'vfnma', 'vfnms'):
            c.add('%s.%s %s0, %s1, %s2' % (op, fmt, rn, rn, rn), scalar_states(fmt, regs, True))
        # the destination is a source
        c.add('vadd.%s %s1, %s1, %s2' % (fmt, rn, rn, rn), scalar_states(fmt, (1, 1, 2), count=6))
        c.add('vmul.%s %s3, %s2, %s2' % (fmt, rn, rn, rn), scalar_states(fmt, (3, 2, 2), count=6))
        c.add('vfma.%s %s2, %s2, %s1' % (fmt, rn, rn, rn), scalar_states(fmt, (2, 2, 1), True, count=6))
        for op in ('vabs', 'vneg', 'vsqrt'):
            c.add('%s.%s %s0, %s1' % (op, fmt, rn, rn), scalar_states(fmt, regs))
    c.add('vadd.f64 d16, d17, d18', scalar_states('f64', (16, 17, 18), count=6))
    c.add('vmul.f64 d31, d30, d29')
    c.add('vsqrt.f64 d20, d21')
    c.add('vfma.f64 d19, d18, d17', scalar_states('f64', (19, 18, 17), True, count=6))
    c.add('vadd.f32 s31, s30, s29')
    c.add('vadd.f32 s1, s3, s5')
    c.add('vaddeq.f32 s0, s1, s2', [scalar_states('f32', (0, 1, 2), count=1)[0]])
    c.add('vsubne.f64 d0, d1, d2')
    c.add('vmulgt.f32 s0, s1, s2')
    c.add('vdivlt.f64 d0, d1, d2')
    c.add('vmlscs.f32 s0, s1, s2')

    # ---- compares
    c.comment('vcmp / vcmpe, then the flags to the APSR')
    for fmt, rn in (('f32', 's'), ('f64', 'd'), ('f16', 's')):
        for op in ('vcmp', 'vcmpe'):
            c.add('%s.%s %s0, %s1' % (op, fmt, rn, rn), scalar_states(fmt, (0, 0, 1)))
            c.add('%s.%s %s0, #0' % (op, fmt, rn), scalar_states(fmt, (0, 0, 1)))
            c.add('%s.%s %s0, %s1 ; vmrs APSR_nzcv, fpscr' % (op, fmt, rn, rn), scalar_states(fmt, (0, 0, 1), count=8))
    c.add('vmrs APSR_nzcv, fpscr')
    c.add('vmrs r0, fpscr')
    c.add('vmrs r5, fpscr')
    c.add('vcmp.f32 s0, s1 ; vmrs r1, fpscr', scalar_states('f32', (0, 0, 1)))
    c.add('vcmp.f32 s0, s1 ; vmrs APSR_nzcv, fpscr ; vmoveq.f32 s2, s3 ; vmovne.f32 s4, s5',
          scalar_states('f32', (0, 0, 1), count=6))
    c.add('vcmp.f64 d0, d1 ; vmrs APSR_nzcv, fpscr ; vseleq.f64 d3, d4, d5',
          scalar_states('f64', (0, 0, 1), count=6))

    # ---- select, maxnm, rint
    c.comment('vsel, vmaxnm, vminnm, vrint')
    flags = [State(), State(), State(), State()]
    for st, value in zip(flags, (0x40000000, 0x80000000, 0x50000000, 0x10000000)):
        st.apsr = value
    for cond in ('eq', 'ge', 'gt', 'vs'):
        c.add('vsel%s.f32 s0, s1, s2' % cond, [])
        c.add('vsel%s.f64 d0, d1, d2' % cond, [])
        c.add('vsel%s.f16 s0, s1, s2' % cond, [])
    for fmt, rn in (('f32', 's'), ('f64', 'd'), ('f16', 's')):
        for op in ('vmaxnm', 'vminnm'):
            c.add('%s.%s %s0, %s1, %s2' % (op, fmt, rn, rn, rn), scalar_states(fmt, (0, 1, 2)))
        for op in ('vrinta', 'vrintn', 'vrintp', 'vrintm', 'vrintx', 'vrintz', 'vrintr'):
            if fmt == 'f16' and op in ('vrintx',):
                pass
            c.add('%s.%s %s0, %s1' % (op, fmt, rn, rn), scalar_states(fmt, (0, 0, 1)))

    # ---- conversions
    c.comment('vcvt: VFP')
    int_f32 = [State().set_s(1, v) for v in (0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0x01000001, 0xfeffffff, 12345678)]
    for i, st in enumerate(int_f32):
        st.fpscr = FPSCRS[i % len(FPSCRS)]
    for ty in ('s32', 'u32'):
        c.add('vcvt.f32.%s s0, s1' % ty, int_f32)
        c.add('vcvt.f64.%s d0, s1' % ty, int_f32)
        c.add('vcvt.f16.%s s0, s1' % ty, int_f32)
        c.add('vcvt.%s.f32 s0, s1' % ty, scalar_states('f32', (0, 0, 1)))
        c.add('vcvt.%s.f64 s0, d1' % ty, scalar_states('f64', (0, 0, 1)))
        c.add('vcvt.%s.f16 s0, s1' % ty, scalar_states('f16', (0, 0, 1)))
        c.add('vcvtr.%s.f32 s0, s1' % ty, scalar_states('f32', (0, 0, 1)))
        c.add('vcvtr.%s.f64 s0, d1' % ty, scalar_states('f64', (0, 0, 1)))
        c.add('vcvtr.%s.f16 s0, s1' % ty, scalar_states('f16', (0, 0, 1)))
        for mode in ('a', 'n', 'p', 'm'):
            c.add('vcvt%s.%s.f32 s0, s1' % (mode, ty), scalar_states('f32', (0, 0, 1)))
            c.add('vcvt%s.%s.f64 s0, d1' % (mode, ty), scalar_states('f64', (0, 0, 1)))
            c.add('vcvt%s.%s.f16 s0, s1' % (mode, ty), scalar_states('f16', (0, 0, 1)))
        for bits in (16, 32):
            t = ty[0] + str(bits)
            for fbits in (1, 8, bits - 1, bits):
                if fbits > bits:
                    continue
                c.add('vcvt.f32.%s s0, s0, #%d' % (t, fbits), [State().set_s(0, v) for v in (0, 1, 0xffffffff, 0x7fff, 0x8000, 0x12345678)])
                c.add('vcvt.f64.%s d0, d0, #%d' % (t, fbits), [State().set_d(0, v) for v in (0, 1, 0xffffffff, 0x7fff, 0x8000, 0x12345678)])
                c.add('vcvt.f16.%s s0, s0, #%d' % (t, fbits), [State().set_s(0, v) for v in (0, 1, 0xffffffff, 0x7fff, 0x8000)])
                c.add('vcvt.%s.f32 s0, s0, #%d' % (t, fbits), scalar_states('f32', (0, 0, 0)))
                c.add('vcvt.%s.f64 d0, d0, #%d' % (t, fbits), scalar_states('f64', (0, 0, 0)))
                c.add('vcvt.%s.f16 s0, s0, #%d' % (t, fbits), scalar_states('f16', (0, 0, 0)))
    c.add('vcvt.f64.f32 d0, s1', scalar_states('f32', (0, 0, 1)))
    c.add('vcvt.f32.f64 s0, d1', scalar_states('f64', (0, 0, 1)))
    for half in ('b', 't'):
        c.add('vcvt%s.f32.f16 s0, s1' % half, scalar_states('f16', (0, 0, 1), fz16=False) + [State().set_s(1, 0x7c017e01)])
        c.add('vcvt%s.f64.f16 d0, s1' % half, scalar_states('f16', (0, 0, 1), fz16=False) + [State().set_s(1, 0x7c017e01)])
        c.add('vcvt%s.f16.f32 s0, s1' % half, scalar_states('f32', (0, 0, 1)))
        c.add('vcvt%s.f16.f64 s0, d1' % half, scalar_states('f64', (0, 0, 1)))
        c.add('vcvt%s.bf16.f32 s0, s1' % half, scalar_states('f32', (0, 0, 1)))
    c.add('vjcvt.s32.f64 s0, d1', scalar_states('f64', (0, 0, 1)) + [State().set_d(1, v) for v in (
        0x41e0000000000000, 0xc1e0000000000000, 0x41dfffffffc00000, 0x4330000000000001, 0x7e37e43c8800759c, 0xbff8000000000000)])

    # ---- moves
    c.comment('vmov')
    c.add('vmov.f32 s0, #1.0')
    c.add('vmov.f32 s0, #-2.5')
    c.add('vmov.f32 s1, #31.0')
    c.add('vmov.f64 d0, #1.0')
    c.add('vmov.f64 d17, #0.5')
    c.add('vmov.f64 d1, #-0.1875')
    c.add('vmov.f16 s0, #1.0')
    c.add('vmov.f32 d0, #1.0')
    c.add('vmov.f32 q1, #-4.0')
    c.add('vmov.f32 s0, s1')
    c.add('vmov.f64 d0, d1')
    c.add('vmov.f64 d16, d31')
    c.add('vmov r0, s1')
    c.add('vmov s1, r0')
    c.add('vmov.f16 r0, s1')
    c.add('vmov.f16 s1, r0')
    c.add('vmov r0, r1, d2')
    c.add('vmov d2, r0, r1')
    c.add('vmov r0, r1, s2, s3')
    c.add('vmov s2, s3, r0, r1')
    c.add('vmov r2, r3, d17')
    c.add('vmov d17, r2, r3')
    for imm in ('0x12', '0x1200', '0x120000', '0x12000000', '0x12ff', '0x12ffff'):
        c.add('vmov.i32 d0, #%s' % imm)
        c.add('vmov.i32 q1, #%s' % imm)
    for imm in ('0x12', '0x1200'):
        c.add('vmov.i16 d0, #%s' % imm)
        c.add('vmov.i16 q2, #%s' % imm)
    c.add('vmov.i8 d0, #0x5a')
    c.add('vmov.i8 q3, #0xff')
    c.add('vmov.i64 d0, #0xff00ff0000ffff00')
    c.add('vmov.i64 q4, #0x00ff00ffff0000ff')
    for lane in range(2):
        c.add('vmov.32 d1[%d], r2' % lane)
        c.add('vmov.32 r2, d1[%d]' % lane)
    for lane in range(4):
        c.add('vmov.16 d2[%d], r3' % lane)
        c.add('vmov.s16 r3, d2[%d]' % lane)
        c.add('vmov.u16 r3, d2[%d]' % lane)
    for lane in range(8):
        c.add('vmov.8 d3[%d], r4' % lane)
        c.add('vmov.s8 r4, d3[%d]' % lane)
        c.add('vmov.u8 r4, d3[%d]' % lane)
    c.add('vmov.32 d17[1], r2')
    c.add('vmov.32 r2, d30[0]')
    c.add('vins.f16 s0, s1')
    c.add('vmovx.f16 s0, s1')
    c.add('vmoveq.f32 s0, s1')
    c.add('vmovne r0, s1')

    # ---- status registers
    c.comment('vmrs / vmsr')
    # (the bits 21:16 of the FPSCR are RES0 in the Arm ARM, Unicorn keeps the Stride and Len ones,
    # and has no FZ16: the line clears them)
    c.add('bic r0, r0, #0x3f0000 ; vmsr fpscr, r0')
    c.add('bic r1, r1, #0x3f0000 ; vmsr fpscr, r1 ; vmrs r2, fpscr')
    c.add('bic r0, r0, #0x3f0000 ; vmsr fpscr, r0 ; vadd.f32 s0, s1, s2', scalar_states('f32', (0, 1, 2), count=6))
    c.add('vmsr fpscr, r0 ; vmrs r2, fpscr', [State().set_r(0, v) for v in (0, 0xffffffff, 0x03c00000, 0x00080000, 0x00370000)])
    c.add('vmrs r0, mvfr0')
    c.add('vmrs r0, mvfr1')
    c.add('vmrs r0, mvfr2')
    c.add('vmrs r0, fpsid')
    c.add('vmrs r0, fpexc')
    c.add('vmsr fpexc, r0')

    # ---- memory
    c.comment('vldr / vstr / vldm / vstm / vpush / vpop')
    c.add('vldr s0, [r4]')
    c.add('vldr s1, [r4, #8]')
    c.add('vldr s2, [r4, #-8]')
    c.add('vldr d0, [r4, #16]')
    c.add('vldr d20, [r4, #-1020]')
    c.add('vldr s3, [pc, #8]')
    c.add('vldr d3, [pc, #-16]')
    c.add('vldr.16 s0, [r4, #2]')
    c.add('vldr.16 s5, [r4, #-30]')
    c.add('vstr s0, [r4]')
    c.add('vstr s1, [r4, #8]')
    c.add('vstr d0, [r4, #16]')
    c.add('vstr d20, [r4, #-8]')
    c.add('vstr.16 s0, [r4, #2]')
    c.add('vldmia r4, {s0-s3}')
    c.add('vldmia r4!, {s0-s3}')
    c.add('vldmia r4!, {d0-d3}')
    c.add('vldmia r4, {d16-d19}')
    c.add('vldmdb r4!, {s2-s6}')
    c.add('vldmdb r4!, {d1-d5}')
    c.add('vstmia r4, {s0-s3}')
    c.add('vstmia r4!, {s4-s9}')
    c.add('vstmia r4!, {d0-d3}')
    c.add('vstmdb r4!, {s0-s2}')
    c.add('vstmdb r4!, {d8-d10}')
    c.add('vstmia r4, {d31}')
    c.add('vldmia r4, {s31}')
    c.add('vpush {s0-s3}')
    c.add('vpush {d0-d7}')
    c.add('vpush {d8}')
    c.add('vpush {d16-d31}')
    c.add('vpop {s0-s3}')
    c.add('vpop {d0-d7}')
    c.add('vpop {d16-d19}')
    c.add('vldmeq r4!, {d0-d1}')
    c.add('fldmiax r4!, {d0-d2}')
    c.add('fldmdbx r4!, {d0-d2}')
    c.add('fstmiax r4!, {d0-d2}')
    c.add('fstmdbx r4!, {d0-d2}')

    # ---- Advanced SIMD floating point
    c.comment('Advanced SIMD floating point: f32 and f16 on d and q registers')
    for fmt in ('f32', 'f16'):
        for op in ('vadd', 'vsub', 'vmul', 'vabd', 'vmax', 'vmin', 'vmla', 'vmls', 'vfma', 'vfms', 'vrecps', 'vrsqrts',
                   'vceq', 'vcge', 'vcgt', 'vacge', 'vacgt', 'vmaxnm', 'vminnm'):
            if op in ('vmaxnm', 'vminnm'):
                c.add('%s.%s d0, d1, d2' % (op, fmt), vector_states(fmt))
                c.add('%s.%s q0, q1, q2' % (op, fmt), vector_states(fmt))
                continue
            c.add('%s.%s d0, d1, d2' % (op, fmt), vector_states(fmt))
            c.add('%s.%s q0, q1, q2' % (op, fmt), vector_states(fmt))
        for op in ('vpadd', 'vpmax', 'vpmin'):
            c.add('%s.%s d0, d1, d2' % (op, fmt), vector_states(fmt))
        for op in ('vceq', 'vcge', 'vcgt', 'vcle', 'vclt'):
            c.add('%s.%s d0, d1, #0' % (op, fmt), vector_states(fmt))
            c.add('%s.%s q0, q1, #0' % (op, fmt), vector_states(fmt))
        for op in ('vabs', 'vneg', 'vrecpe', 'vrsqrte', 'vrinta', 'vrintn', 'vrintp', 'vrintm', 'vrintx', 'vrintz'):
            c.add('%s.%s d0, d1' % (op, fmt), vector_states(fmt))
            c.add('%s.%s q0, q1' % (op, fmt), vector_states(fmt))
        for op in ('vmul', 'vmla', 'vmls'):
            idx = 3 if fmt == 'f16' else 1
            c.add('%s.%s d0, d1, d2[%d]' % (op, fmt, idx), vector_states(fmt))
            c.add('%s.%s q0, q1, d2[0]' % (op, fmt), vector_states(fmt))
    for ty in ('s32', 'u32'):
        c.add('vcvt.f32.%s d0, d1' % ty, vector_states('f32'))
        c.add('vcvt.f32.%s q0, q1' % ty, vector_states('f32'))
        c.add('vcvt.%s.f32 d0, d1' % ty, vector_states('f32'))
        c.add('vcvt.%s.f32 q0, q1' % ty, vector_states('f32'))
        for fbits in (1, 16, 32):
            c.add('vcvt.f32.%s d0, d1, #%d' % (ty, fbits), vector_states('f32'))
            c.add('vcvt.f32.%s q0, q1, #%d' % (ty, fbits), vector_states('f32'))
            c.add('vcvt.%s.f32 d0, d1, #%d' % (ty, fbits), vector_states('f32'))
            c.add('vcvt.%s.f32 q0, q1, #%d' % (ty, fbits), vector_states('f32'))
        for mode in ('a', 'n', 'p', 'm'):
            c.add('vcvt%s.%s.f32 d0, d1' % (mode, ty), vector_states('f32'))
            c.add('vcvt%s.%s.f32 q0, q1' % (mode, ty), vector_states('f32'))
    for ty in ('s16', 'u16'):
        c.add('vcvt.f16.%s d0, d1' % ty, vector_states('f16'))
        c.add('vcvt.f16.%s q0, q1' % ty, vector_states('f16'))
        c.add('vcvt.%s.f16 d0, d1' % ty, vector_states('f16'))
        c.add('vcvt.%s.f16 q0, q1' % ty, vector_states('f16'))
        c.add('vcvt.f16.%s d0, d1, #3' % ty, vector_states('f16'))
        c.add('vcvt.%s.f16 q0, q1, #5' % ty, vector_states('f16'))
        for mode in ('a', 'n', 'p', 'm'):
            c.add('vcvt%s.%s.f16 d0, d1' % (mode, ty), vector_states('f16'))
            c.add('vcvt%s.%s.f16 q0, q1' % (mode, ty), vector_states('f16'))
    c.add('vcvt.f16.f32 d0, q1', vector_states('f32'))
    c.add('vcvt.f32.f16 q0, d1', vector_states('f16', fz16=False))
    c.add('vcvt.bf16.f32 d0, q1', vector_states('f32'))

    # ---- FEAT_FCMA, FEAT_FHM, FEAT_BF16
    c.comment('vcadd, vcmla, vfmal, vfmsl, bfloat16')
    for fmt in ('f32', 'f16'):
        for rot in (90, 270):
            c.add('vcadd.%s d0, d1, d2, #%d' % (fmt, rot), vector_states(fmt))
            c.add('vcadd.%s q0, q1, q2, #%d' % (fmt, rot), vector_states(fmt))
        for rot in (0, 90, 180, 270):
            c.add('vcmla.%s d0, d1, d2, #%d' % (fmt, rot), vector_states(fmt))
            c.add('vcmla.%s q0, q1, q2, #%d' % (fmt, rot), vector_states(fmt))
            c.add('vcmla.%s d0, d1, d2[0], #%d' % (fmt, rot), vector_states(fmt))
            c.add('vcmla.%s q0, q1, d2[0], #%d' % (fmt, rot), vector_states(fmt))
    for op in ('vfmal', 'vfmsl'):
        c.add('%s.f16 d0, s2, s4' % op, vector_states('f16', fz16=False))
        c.add('%s.f16 q0, d2, d3' % op, vector_states('f16', fz16=False))
        c.add('%s.f16 d0, s2, s3[1]' % op, vector_states('f16', fz16=False))
        c.add('%s.f16 q0, d2, d3[2]' % op, vector_states('f16', fz16=False))
    c.add('vdot.bf16 d0, d1, d2')
    c.add('vdot.bf16 q0, q1, q2')
    c.add('vdot.bf16 d0, d1, d2[1]')
    c.add('vdot.bf16 q0, q1, d2[0]')
    c.add('vmmla.bf16 q0, q1, q2')
    c.add('vfmab.bf16 q0, q1, q2')
    c.add('vfmat.bf16 q0, q1, q2')
    c.add('vfmab.bf16 q0, q1, d2[3]')
    c.add('vfmat.bf16 q0, q1, d2[1]')
    c.write(OUT_A32)

    # ---- T32
    t = Corpus('T32 floating point instructions: the Thumb spellings of the VFP and Advanced SIMD instructions, '
               'and the conditional VFP instructions in IT blocks (one line is one unit)')
    t.add('vadd.f32 s0, s1, s2', scalar_states('f32', (0, 1, 2)))
    t.add('vadd.f64 d0, d1, d2', scalar_states('f64', (0, 1, 2)))
    t.add('vsub.f32 d0, d1, d2', vector_states('f32'))
    t.add('vmul.f32 q0, q1, q2', vector_states('f32'))
    t.add('vfma.f32 q0, q1, q2', vector_states('f32'))
    t.add('vmla.f32 d0, d1, d2[1]', vector_states('f32'))
    t.add('vcmp.f32 s0, s1 ; vmrs APSR_nzcv, fpscr', scalar_states('f32', (0, 0, 1)))
    t.add('vcmp.f64 d0, #0 ; vmrs APSR_nzcv, fpscr', scalar_states('f64', (0, 0, 1)))
    t.add('vcvt.s32.f32 s0, s1', scalar_states('f32', (0, 0, 1)))
    t.add('vcvt.f32.s32 d0, d1', vector_states('f32'))
    t.add('vcvt.f32.u32 q0, q1, #8', vector_states('f32'))
    t.add('vcvtb.f16.f32 s0, s1', scalar_states('f32', (0, 0, 1)))
    t.add('vrintx.f32 s0, s1', scalar_states('f32', (0, 0, 1)))
    t.add('vrintn.f32 q0, q1', vector_states('f32'))
    t.add('vmaxnm.f64 d0, d1, d2', scalar_states('f64', (0, 1, 2)))
    t.add('vseleq.f32 s0, s1, s2')
    t.add('vmov.f32 s0, #1.5')
    t.add('vmov.f32 q0, #-1.0')
    t.add('vmov.i32 q0, #0xff00')
    t.add('vmov r0, s1')
    t.add('vmov r0, r1, d2')
    t.add('vmov.32 d1[1], r2')
    t.add('vmov.s8 r4, d3[3]')
    t.add('vmrs APSR_nzcv, fpscr')
    t.add('vmsr fpscr, r0 ; vmrs r2, fpscr')
    t.add('vldr s0, [r4, #8]')
    t.add('vstr d0, [r4, #-16]')
    t.add('vldr s3, [pc, #8]')
    t.add('vldmia r4!, {s0-s3}')
    t.add('vstmdb r4!, {d0-d3}')
    t.add('vpush {d8-d9}')
    t.add('vpop {s0-s1}')
    for cond, apsr in (('eq', 0x40000000), ('ne', 0x40000000), ('eq', 0), ('ne', 0), ('ge', 0x90000000), ('lt', 0x90000000)):
        st = State()
        st.apsr = apsr
        t.add('it %s ; vadd%s.f32 s0, s1, s2' % (cond, cond), [st] + scalar_states('f32', (0, 1, 2), count=3))
    for pair, apsr in (('ite eq ; vaddeq.f32 s0, s1, s2 ; vsubne.f32 s0, s1, s2', 0x40000000),
                       ('ite eq ; vaddeq.f32 s0, s1, s2 ; vsubne.f32 s0, s1, s2', 0),
                       ('ite ne ; vmulne.f64 d0, d1, d2 ; vdiveq.f64 d0, d1, d2', 0),
                       ('itt gt ; vmovgt.f32 s0, s1 ; vcvtgt.s32.f32 s2, s3', 0x00000000),
                       ('itt hi ; vldrhi s0, [r4] ; vstrhi s1, [r4, #4]', 0x20000000),
                       ('ittt eq ; vmoveq r0, s1 ; vmoveq s2, r3 ; vabseq.f32 s4, s5', 0x40000000),
                       ('itete lt ; vaddlt.f32 s0, s1, s2 ; vsubge.f32 s0, s1, s2 ; vmullt.f32 s3, s1, s2 ; '
                        'vdivge.f32 s3, s1, s2', 0x80000000)):
        st = State()
        st.apsr = apsr
        t.add(pair, [st] + scalar_states('f32', (0, 1, 2), count=3))
    t.write(OUT_T32)


if __name__ == '__main__':
    gen_a32()
    print('wrote %s and %s' % (OUT_A32, OUT_T32))
