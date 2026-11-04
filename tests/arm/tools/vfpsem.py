#!/usr/bin/env python3
"""The semantics of the AArch32 floating point instructions on a register state, written from the
Arm ARM pseudocode with the model of fpmodel.py (independent of dismount). run(text, state)
returns the state after the instruction. The state is a dict:
    q       list of 16 ints (128 bit); the d and s registers are views of them
    r       list of 13 ints            fpscr   int            apsr  int (N Z C V bits 31-28)
    mem     {address: byte}
The instructions are the ones of the owned part of the A32 SIMD table (gen_a32_vfp_semantic_table.py)
with the register operands as the corpus writes them. Used by check_fpmodel.py (the model against
Unicorn for single and double precision) and by gen_semantic_rows_aarch32_vfp.py (the unit rows of
half precision, bfloat16 and the other instructions Unicorn doesn't run).

Author: Elad Raz <e@eladraz.com>
"""
import copy
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpmodel as M
from fractions import Fraction

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
FORMATS = {'f16': M.HALF, 'f32': M.SINGLE, 'f64': M.DOUBLE, 'bf16': M.BFLOAT}
FPSCR_WRITABLE = 0xFFC8009F


def get_reg(state, name):
    kind, n = name[0], int(name[1:])
    if kind == 'q':
        return state['q'][n]
    if kind == 'd':
        return (state['q'][n // 2] >> (64 * (n % 2))) & M64
    return (state['q'][n // 4] >> (32 * (n % 4))) & M32


def set_reg(state, name, value):
    kind, n = name[0], int(name[1:])
    if kind == 'q':
        state['q'][n] = value & ((1 << 128) - 1)
        return
    bits = 64 if kind == 'd' else 32
    index = n % (128 // bits)
    q = n // (128 // bits)
    mask = ((1 << bits) - 1) << (bits * index)
    state['q'][q] = (state['q'][q] & ~mask) | ((value & ((1 << bits) - 1)) << (bits * index))


def reg_bits(name):
    return {'q': 128, 'd': 64, 's': 32}[name[0]]


def split_operands(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Op(object):
    """A parsed operand: a register (name, index), an immediate or a core register."""

    def __init__(self, text):
        self.text = text
        m = re.match(r'^([sdq]\d+)(?:\[(\d+)\])?$', text)
        self.reg = m.group(1) if m else None
        self.index = int(m.group(2)) if m and m.group(2) is not None else None
        self.imm = None
        if text.startswith('#'):
            try:
                self.imm = float(text[1:]) if ('.' in text) else int(text[1:], 0)
            except ValueError:
                self.imm = text[1:]
        m = re.match(r'^r(\d+)$', text)
        self.core = int(m.group(1)) if m else None


def lanes_of(value, bits, count):
    return [(value >> (bits * i)) & ((1 << bits) - 1) for i in range(count)]


def join_lanes(lanes, bits):
    out = 0
    for i, v in enumerate(lanes):
        out |= (v & ((1 << bits) - 1)) << (bits * i)
    return out


class Machine(object):
    def __init__(self, state):
        self.s = copy.deepcopy(state)
        self.st = M.State()
        self.fpscr0 = state['fpscr']

    def finish(self):
        self.s['fpscr'] = (self.s['fpscr'] | self.st.flags) & 0xFFFFFFFF
        return self.s

    def ctl(self, standard):
        c = M.Ctl.from_fpscr(self.fpscr0)
        if standard:
            return M.Ctl.standard(ahp=c.ahp)
        return c


def is_vector_shape(dest, fmt):
    return dest[0] == 'q' or (dest[0] == 'd' and fmt is not M.DOUBLE)


def read_lanes(machine, op, fmt, count):
    """The lanes of a register operand (a scalar operand d2[1] is replicated)."""
    bits = fmt.N
    if op.index is not None:
        value = get_reg(machine.s, op.reg)
        lane = (value >> (bits * op.index)) & ((1 << bits) - 1)
        return [lane] * count
    value = get_reg(machine.s, op.reg)
    return lanes_of(value, bits, count)


def write_lanes(machine, op, fmt, lanes):
    set_reg(machine.s, op.reg, join_lanes(lanes, fmt.N))


def exec_arith(m, base, fmt, ops):
    dest = ops[0]
    vector = is_vector_shape(dest.reg, fmt)
    ctl = m.ctl(vector)
    st = m.st
    count = (reg_bits(dest.reg) // fmt.N) if vector else 1
    a = read_lanes(m, ops[1], fmt, count)
    b = read_lanes(m, ops[2], fmt, count)
    d = read_lanes(m, dest, fmt, count)
    out = []
    for i in range(count):
        x, y, acc = a[i], b[i], d[i]
        if base == 'vadd':
            r = M.fp_add(fmt, ctl, st, x, y)
        elif base == 'vsub':
            r = M.fp_sub(fmt, ctl, st, x, y)
        elif base == 'vmul':
            r = M.fp_mul(fmt, ctl, st, x, y)
        elif base == 'vdiv':
            r = M.fp_div(fmt, ctl, st, x, y)
        elif base == 'vnmul':
            r = M.fp_neg(fmt, M.fp_mul(fmt, ctl, st, x, y))
        elif base == 'vabd':
            r = M.fp_abs(fmt, M.fp_sub(fmt, ctl, st, x, y))
        elif base in ('vmax', 'vmin'):
            r = M.fp_max_min(fmt, ctl, st, x, y, base == 'vmax')
        elif base in ('vmaxnm', 'vminnm'):
            r = M.fp_max_min(fmt, ctl, st, x, y, base == 'vmaxnm', num=True)
        elif base == 'vmla':
            r = M.fp_add(fmt, ctl, st, acc, M.fp_mul(fmt, ctl, st, x, y))
        elif base == 'vmls':
            p = M.fp_mul(fmt, ctl, st, x, y)
            r = M.fp_sub(fmt, ctl, st, acc, p) if vector else M.fp_add(fmt, ctl, st, acc, M.fp_neg(fmt, p))
        elif base == 'vnmla':
            p = M.fp_mul(fmt, ctl, st, x, y)
            r = M.fp_add(fmt, ctl, st, M.fp_neg(fmt, acc), M.fp_neg(fmt, p))
        elif base == 'vnmls':
            p = M.fp_mul(fmt, ctl, st, x, y)
            r = M.fp_add(fmt, ctl, st, M.fp_neg(fmt, acc), p)
        elif base == 'vfma':
            r = M.fp_muladd(fmt, ctl, st, acc, x, y)
        elif base == 'vfms':
            r = M.fp_muladd(fmt, ctl, st, acc, M.fp_neg(fmt, x), y)
        elif base == 'vfnma':
            r = M.fp_muladd(fmt, ctl, st, M.fp_neg(fmt, acc), M.fp_neg(fmt, x), y)
        elif base == 'vfnms':
            r = M.fp_muladd(fmt, ctl, st, M.fp_neg(fmt, acc), x, y)
        elif base == 'vrecps':
            r = M.fp_recip_step(fmt, ctl, st, x, y)
        elif base == 'vrsqrts':
            r = M.fp_recip_step(fmt, ctl, st, x, y, sqrt=True)
        else:
            raise ValueError(base)
        out.append(r)
    write_lanes(m, dest, fmt, out)


def exec_pairwise(m, base, fmt, ops):
    ctl = m.ctl(True)
    count = 64 // fmt.N
    a = read_lanes(m, ops[1], fmt, count)
    b = read_lanes(m, ops[2], fmt, count)
    out = [0] * count
    for half, src in enumerate((a, b)):
        for i in range(count // 2):
            x, y = src[2 * i], src[2 * i + 1]
            if base == 'vpadd':
                r = M.fp_add(fmt, ctl, m.st, x, y)
            else:
                r = M.fp_max_min(fmt, ctl, m.st, x, y, base == 'vpmax')
            out[half * (count // 2) + i] = r
    write_lanes(m, ops[0], fmt, out)


def exec_unary(m, base, fmt, ops):
    dest = ops[0]
    vector = is_vector_shape(dest.reg, fmt)
    ctl = m.ctl(vector)
    count = (reg_bits(dest.reg) // fmt.N) if vector else 1
    a = read_lanes(m, ops[1], fmt, count)
    out = []
    for x in a:
        if base == 'vabs':
            r = M.fp_abs(fmt, x)
        elif base == 'vneg':
            r = M.fp_neg(fmt, x)
        elif base == 'vsqrt':
            r = M.fp_sqrt(fmt, ctl, m.st, x)
        elif base == 'vrecpe':
            r = M.fp_recip_estimate(fmt, ctl, m.st, x)
        elif base == 'vrsqrte':
            r = M.fp_rsqrt_estimate(fmt, ctl, m.st, x)
        else:
            mode = {'vrintz': 'Z', 'vrinta': 'A', 'vrintn': 'N', 'vrintp': 'P', 'vrintm': 'M',
                    'vrintx': ctl.rmode, 'vrintr': ctl.rmode}[base]
            r = M.fp_round_int(fmt, ctl, m.st, x, mode, base == 'vrintx')
        out.append(r)
    write_lanes(m, dest, fmt, out)


def exec_compare(m, base, fmt, ops):
    ctl = m.ctl(False)
    a = get_reg(m.s, ops[0].reg) & ((1 << fmt.N) - 1)
    if ops[1].imm is not None:
        b = 0
    else:
        b = get_reg(m.s, ops[1].reg) & ((1 << fmt.N) - 1)
    nzcv = M.fp_compare(fmt, ctl, m.st, a, b, base == 'vcmpe')
    m.s['fpscr'] = (m.s['fpscr'] & 0x0FFFFFFF) | (nzcv << 28)
    m.fpscr_flags_written = True


def exec_vector_compare(m, base, fmt, ops):
    dest = ops[0]
    ctl = m.ctl(True)
    count = reg_bits(dest.reg) // fmt.N
    a = read_lanes(m, ops[1], fmt, count)
    if ops[2].imm is not None:
        b = [0] * count
    else:
        b = read_lanes(m, ops[2], fmt, count)
    kind = {'vceq': 'eq', 'vcge': 'ge', 'vcgt': 'gt', 'vacge': 'ge', 'vacgt': 'gt', 'vcle': 'le', 'vclt': 'lt'}[base]
    out = []
    for x, y in zip(a, b):
        if base.startswith('vac'):
            x, y = M.fp_abs(fmt, x), M.fp_abs(fmt, y)
        if kind == 'le':
            r = M.fp_compare_op(fmt, ctl, m.st, y, x, 'ge')
        elif kind == 'lt':
            r = M.fp_compare_op(fmt, ctl, m.st, y, x, 'gt')
        else:
            r = M.fp_compare_op(fmt, ctl, m.st, x, y, kind)
        out.append(((1 << fmt.N) - 1) if r else 0)
    write_lanes(m, dest, fmt, out)


def exec_select(m, base, fmt, ops):
    n = (m.s['apsr'] >> 31) & 1
    z = (m.s['apsr'] >> 30) & 1
    v = (m.s['apsr'] >> 28) & 1
    cond = {'vseleq': z == 1, 'vselge': n == v, 'vselgt': z == 0 and n == v, 'vselvs': v == 1}[base]
    mask = (1 << fmt.N) - 1
    pick = ops[1] if cond else ops[2]
    set_reg(m.s, ops[0].reg, get_reg(m.s, pick.reg) & mask)


def mode_of(base):
    return {'vcvt': 'Z', 'vcvtr': None, 'vcvta': 'A', 'vcvtn': 'N', 'vcvtp': 'P', 'vcvtm': 'M'}[base]


def exec_convert(m, base, parts, ops):
    dest = ops[0]
    p0, p1 = parts[0], parts[1]
    fbits = ops[2].imm if len(ops) > 2 else 0
    if base in ('vcvtb', 'vcvtt'):
        top = base == 'vcvtt'
        ctl = m.ctl(False)
        dst, src = FORMATS[p0], FORMATS[p1]
        srcv = get_reg(m.s, ops[1].reg)
        if src.N == 16 or src is M.BFLOAT:
            half = (srcv >> (16 if top else 0)) & 0xFFFF
            r = M.fp_convert(src, dst, ctl, m.st, half, ahp_in=ctl.ahp and src is M.HALF)
            set_reg(m.s, dest.reg, r)
        else:
            srcv &= (1 << src.N) - 1
            rmode = ctl.rmode if dst is not M.BFLOAT else 'N'
            r = M.fp_convert(src, dst, ctl, m.st, srcv, ahp_out=ctl.ahp and dst is M.HALF, rmode=rmode)
            old = get_reg(m.s, dest.reg)
            shift = 16 if top else 0
            set_reg(m.s, dest.reg, (old & ~(0xFFFF << shift)) | (r << shift))
        return
    im = re.match(r'^([su])(\d+)$', p0)
    fm = re.match(r'^([su])(\d+)$', p1)
    if im and p1 in FORMATS:
        # float to integer
        src = FORMATS[p1]
        unsigned = im.group(1) == 'u'
        ibits = int(im.group(2))
        vector = is_vector_shape(ops[1].reg, src)
        ctl = m.ctl(vector)
        rmode = mode_of(base) or ctl.rmode
        count = (reg_bits(dest.reg) // ibits) if vector else 1
        a = read_lanes(m, ops[1], src, count)
        out = []
        for x in a:
            r = M.fp_to_fixed(src, ctl, m.st, x, ibits, fbits, unsigned, rmode)
            out.append(r)
        if vector:
            set_reg(m.s, dest.reg, join_lanes(out, ibits))
        else:
            r = out[0]
            if not unsigned and (r >> (ibits - 1)) & 1:
                r -= 1 << ibits
            set_reg(m.s, dest.reg, r & ((1 << reg_bits(dest.reg)) - 1))
        return
    if p0 in FORMATS and fm:
        dst = FORMATS[p0]
        unsigned = fm.group(1) == 'u'
        ibits = int(fm.group(2))
        vector = is_vector_shape(dest.reg, dst)
        ctl = m.ctl(vector)
        count = (reg_bits(dest.reg) // dst.N) if vector else 1
        srcv = get_reg(m.s, ops[1].reg)
        out = []
        for i in range(count):
            raw = (srcv >> (ibits * i)) & ((1 << ibits) - 1)
            out.append(M.fixed_to_fp(dst, ctl, m.st, raw, ibits, fbits, unsigned))
        if vector:
            set_reg(m.s, dest.reg, join_lanes(out, dst.N))
        else:
            set_reg(m.s, dest.reg, out[0])
        return
    if p0 in FORMATS and p1 in FORMATS:
        dst, src = FORMATS[p0], FORMATS[p1]
        vector = reg_bits(dest.reg) > 32 and not (dest.reg[0] == 'd' and dst is M.DOUBLE) and \
            not (ops[1].reg[0] == 'd' and src is M.DOUBLE)
        ctl = m.ctl(vector)
        if vector:
            count = reg_bits(dest.reg) // dst.N
            a = lanes_of(get_reg(m.s, ops[1].reg), src.N, count)
            rm = 'N' if dst is M.BFLOAT else None
            out = [M.fp_convert(src, dst, ctl, m.st, x, ahp_in=ctl.ahp and src is M.HALF,
                                ahp_out=ctl.ahp and dst is M.HALF, rmode=rm) for x in a]
            set_reg(m.s, dest.reg, join_lanes(out, dst.N))
        else:
            x = get_reg(m.s, ops[1].reg) & ((1 << src.N) - 1)
            r = M.fp_convert(src, dst, ctl, m.st, x, ahp_in=ctl.ahp and src is M.HALF,
                             ahp_out=ctl.ahp and dst is M.HALF)
            set_reg(m.s, dest.reg, r)
        return
    raise ValueError('vcvt %r' % (parts,))


def fp_imm_bits(fmt, value):
    """VFPExpandImm of the 8 bit immediate that encodes the float `value`."""
    for imm8 in range(256):
        sign = (imm8 >> 7) & 1
        b6 = (imm8 >> 6) & 1
        low = (imm8 >> 4) & 3
        frac = imm8 & 15
        e_bits = fmt.E
        exp = ((b6 ^ 1) << (e_bits - 1)) | ((((1 << (e_bits - 3)) - 1) if b6 else 0) << 2) | low
        bits = (sign << (fmt.N - 1)) | (exp << fmt.F) | (frac << (fmt.F - 4))
        n = M.unpack(fmt, bits, M.Ctl(), M.State())
        if n.kind == 'finite' and n.value == Fraction(value):
            return bits
    raise ValueError(value)


def exec_move(m, base, parts, ops, text):
    dest = ops[0]
    p0 = parts[0] if parts else ''
    if dest.reg is not None and dest.index is not None:
        # vmov.32 / .16 / .8 Dd[x], Rt
        bits = int(p0)
        value = m.s['r'][ops[1].core] & ((1 << bits) - 1)
        old = get_reg(m.s, dest.reg)
        mask = ((1 << bits) - 1) << (bits * dest.index)
        set_reg(m.s, dest.reg, (old & ~mask) | (value << (bits * dest.index)))
        return
    if len(ops) == 2 and ops[1].reg is not None and ops[1].index is not None:
        # vmov.32 Rt, Dn[x]; vmov.s8 / .u8 / .s16 / .u16 Rt, Dn[x]
        signed = p0[0] == 's'
        bits = int(p0.lstrip('su'))
        value = (get_reg(m.s, ops[1].reg) >> (bits * ops[1].index)) & ((1 << bits) - 1)
        if signed and (value >> (bits - 1)) & 1:
            value -= 1 << bits
        m.s['r'][dest.core] = value & M32
        return
    if len(ops) == 2 and ops[1].imm is not None:
        if p0 in FORMATS:
            fmt = FORMATS[p0]
            bits = fp_imm_bits(fmt, ops[1].imm)
            vector = is_vector_shape(dest.reg, fmt) and reg_bits(dest.reg) > fmt.N
            count = reg_bits(dest.reg) // fmt.N if vector else 1
            set_reg(m.s, dest.reg, join_lanes([bits] * count, fmt.N))
        else:
            width = int(p0[1:])
            count = max(1, reg_bits(dest.reg) // width)
            set_reg(m.s, dest.reg, join_lanes([ops[1].imm & ((1 << width) - 1)] * count, width))
        return
    if len(ops) == 2 and dest.core is not None:
        value = get_reg(m.s, ops[1].reg)
        if p0 == 'f16':
            value &= 0xFFFF
        m.s['r'][dest.core] = value & M32
        return
    if len(ops) == 2 and ops[1].core is not None:
        value = m.s['r'][ops[1].core]
        if p0 == 'f16':
            value &= 0xFFFF
        set_reg(m.s, dest.reg, value)
        return
    if len(ops) == 2:
        set_reg(m.s, dest.reg, get_reg(m.s, ops[1].reg))
        return
    raise ValueError(text)


def exec_complex(m, base, fmt, ops):
    """vcadd, vcmla: the complex numbers are pairs of lanes."""
    dest = ops[0]
    ctl = m.ctl(True)
    st = m.st
    count = reg_bits(dest.reg) // fmt.N
    rotation = ops[-1].imm
    n = lanes_of(get_reg(m.s, ops[1].reg), fmt.N, count)
    if ops[2].index is not None:
        # the pair at the index of the register
        pair_bits = 2 * fmt.N
        pair = (get_reg(m.s, ops[2].reg) >> (pair_bits * ops[2].index)) & ((1 << pair_bits) - 1)
        mm = lanes_of(pair, fmt.N, 2) * (count // 2)
    else:
        mm = lanes_of(get_reg(m.s, ops[2].reg), fmt.N, count)
    d = lanes_of(get_reg(m.s, dest.reg), fmt.N, count)
    out = [0] * count
    for e in range(count // 2):
        re_, im = 2 * e, 2 * e + 1
        if base == 'vcadd':
            if rotation == 90:
                out[re_] = M.fp_sub(fmt, ctl, st, n[re_], mm[im])
                out[im] = M.fp_add(fmt, ctl, st, n[im], mm[re_])
            else:
                out[re_] = M.fp_add(fmt, ctl, st, n[re_], mm[im])
                out[im] = M.fp_sub(fmt, ctl, st, n[im], mm[re_])
            continue
        neg = lambda x: M.fp_neg(fmt, x)
        if rotation == 0:
            e1, e2, e3, e4 = mm[re_], n[re_], mm[im], n[re_]
        elif rotation == 90:
            e1, e2, e3, e4 = neg(mm[im]), n[im], mm[re_], n[im]
        elif rotation == 180:
            e1, e2, e3, e4 = neg(mm[re_]), n[re_], neg(mm[im]), n[re_]
        else:
            e1, e2, e3, e4 = mm[im], n[im], neg(mm[re_]), n[im]
        out[re_] = M.fp_muladd(fmt, ctl, st, d[re_], e2, e1)
        out[im] = M.fp_muladd(fmt, ctl, st, d[im], e4, e3)
    set_reg(m.s, dest.reg, join_lanes(out, fmt.N))


def widen_bf16(x):
    return (x & 0xFFFF) << 16


def bf16_dot_add(m, ctl, addend, a1, a2, b1, b2):
    """BFDotAdd: addend + (a1 * b1 + a2 * b2), the sum of the products rounded to odd."""
    st = m.st
    p1 = M.fp_mul(M.SINGLE, ctl, st, widen_bf16(a1), widen_bf16(b1))
    p2 = M.fp_mul(M.SINGLE, ctl, st, widen_bf16(a2), widen_bf16(b2))
    odd = M.Ctl('O', fz=ctl.fz, dn=ctl.dn)
    total = M.fp_add(M.SINGLE, odd, st, p1, p2)
    return M.fp_add(M.SINGLE, ctl, st, addend, total)


def exec_bf16(m, base, ops):
    dest = ops[0]
    ctl = m.ctl(True)
    st = m.st
    count = reg_bits(dest.reg) // 32
    d = lanes_of(get_reg(m.s, dest.reg), 32, count)
    n = lanes_of(get_reg(m.s, ops[1].reg), 16, 2 * count)
    if base == 'vmmla':
        mm = lanes_of(get_reg(m.s, ops[2].reg), 16, 8)
        out = [0] * 4
        for i in range(2):
            for j in range(2):
                acc = d[2 * i + j]
                for k in range(2):
                    acc = bf16_dot_add(m, ctl, acc, n[4 * i + 2 * k], n[4 * i + 2 * k + 1],
                                       mm[4 * j + 2 * k], mm[4 * j + 2 * k + 1])
                out[2 * i + j] = acc
        set_reg(m.s, dest.reg, join_lanes(out, 32))
        return
    if base == 'vdot':
        if ops[2].index is not None:
            pair = (get_reg(m.s, ops[2].reg) >> (32 * ops[2].index)) & M32
            mm = lanes_of(pair, 16, 2) * count
        else:
            mm = lanes_of(get_reg(m.s, ops[2].reg), 16, 2 * count)
        out = [bf16_dot_add(m, ctl, d[e], n[2 * e], n[2 * e + 1], mm[2 * e], mm[2 * e + 1]) for e in range(count)]
        set_reg(m.s, dest.reg, join_lanes(out, 32))
        return
    select = 1 if base == 'vfmat' else 0
    if ops[2].index is not None:
        value = get_reg(m.s, ops[2].reg)
        if ops[2].index >= 4:
            number = int(ops[2].reg[1:])
            value = get_reg(m.s, 'q%d' % (number // 2))
            lane = (value >> (16 * ops[2].index + 64 * (number % 2))) & 0xFFFF
        else:
            lane = (value >> (16 * ops[2].index)) & 0xFFFF
        mm = [lane] * 8
    else:
        mm = lanes_of(get_reg(m.s, ops[2].reg), 16, 8)
    out = []
    for e in range(count):
        b = mm[0] if ops[2].index is not None else mm[2 * e + select]
        out.append(M.fp_muladd(M.SINGLE, ctl, st, d[e], widen_bf16(n[2 * e + select]), widen_bf16(b)))
    set_reg(m.s, dest.reg, join_lanes(out, 32))


def exec_instruction(m, text):
    mt = re.match(r'^([a-z0-9]+)((?:\.[a-z0-9]+)*)\s*(.*)$', text.strip())
    base = mt.group(1)
    parts = [p for p in mt.group(2).split('.') if p]
    ops = [Op(o) for o in split_operands(mt.group(3))]
    if base in ('vadd', 'vsub', 'vmul', 'vdiv', 'vnmul', 'vabd', 'vmax', 'vmin', 'vmaxnm', 'vminnm', 'vmla',
                'vmls', 'vnmla', 'vnmls', 'vfma', 'vfms', 'vfnma', 'vfnms', 'vrecps', 'vrsqrts'):
        exec_arith(m, base, FORMATS[parts[0]], ops)
    elif base in ('vpadd', 'vpmax', 'vpmin'):
        exec_pairwise(m, base, FORMATS[parts[0]], ops)
    elif base in ('vabs', 'vneg', 'vsqrt', 'vrecpe', 'vrsqrte', 'vrinta', 'vrintn', 'vrintp', 'vrintm', 'vrintx',
                  'vrintz', 'vrintr'):
        exec_unary(m, base, FORMATS[parts[0]], ops)
    elif base in ('vcmp', 'vcmpe'):
        exec_compare(m, base, FORMATS[parts[0]], ops)
    elif base in ('vceq', 'vcge', 'vcgt', 'vcle', 'vclt', 'vacge', 'vacgt'):
        exec_vector_compare(m, base, FORMATS[parts[0]], ops)
    elif base.startswith('vsel'):
        exec_select(m, base, FORMATS[parts[0]], ops)
    elif base.startswith('vcvt'):
        exec_convert(m, base, parts, ops)
    elif base in ('vcadd', 'vcmla'):
        exec_complex(m, base, FORMATS[parts[0]], ops)
    elif base in ('vdot', 'vmmla', 'vfmab', 'vfmat'):
        exec_bf16(m, base, ops)
    elif base == 'vmov':
        exec_move(m, base, parts, ops, text)
    elif base == 'vmrs':
        if ops[0].text.lower() == 'apsr_nzcv':
            m.s['apsr'] = (m.s['apsr'] & 0x0FFFFFFF) | (m.s['fpscr'] & 0xF0000000)
        else:
            m.s['r'][ops[0].core] = m.s['fpscr']
    elif base == 'vmsr':
        m.s['fpscr'] = m.s['r'][ops[1].core] & FPSCR_WRITABLE
    elif base == 'vins':
        old = get_reg(m.s, ops[0].reg)
        set_reg(m.s, ops[0].reg, (old & 0xFFFF) | ((get_reg(m.s, ops[1].reg) & 0xFFFF) << 16))
    elif base == 'vmovx':
        set_reg(m.s, ops[0].reg, (get_reg(m.s, ops[1].reg) >> 16) & 0xFFFF)
    else:
        raise ValueError('unsupported %s' % text)


def run(text, state):
    """The state after the instruction `text` (several instructions joined by ';')."""
    m = Machine(state)
    for part in text.split(';'):
        exec_instruction(m, part)
    return m.finish()
