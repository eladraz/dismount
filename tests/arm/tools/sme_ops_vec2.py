"""More multi vector instructions of the SME2 model (sme_model.py): zip, uzp, the unpacks, the
conversions with vector groups, the rounding to integral, the saturating narrows, sel and the
lookup tables (luti2, luti4) of zt0.

Author: Elad Raz <e@eladraz.com>
"""
import fpmodel as F

from sme_model import (handler, mask, sext, elem, set_elem, Za, Zr, ZList, Pred, M64, SIZES)
from sme_ops_vec import regs_of, elements, signed_max, signed_min


def ops_regs(ops):
    return [regs_of(o) for o in ops]


@handler('zip', 'uzp')
def h_zip(m, mn, ops):
    m.need_streaming()
    dst = regs_of(ops[0])
    esize = ops[0].esize
    n = len(dst)
    lanes = elements(m, esize)
    if isinstance(ops[1], ZList):
        srcs = regs_of(ops[1])
    else:
        srcs = [ops[1].n, ops[2].n]
    vals = [[elem(m.z(s), i, esize) for i in range(lanes)] for s in srcs]
    outs = []
    for d in range(n):
        out = 0
        for j in range(lanes):
            if mn == 'zip':
                v = vals[j % n][d * (lanes // n) + j // n]
            else:
                flat = n * j + d
                v = vals[flat // lanes][flat % lanes]
            out = set_elem(out, j, esize, v)
        outs.append(out)
    for d in range(n):
        m.set_z(dst[d], outs[d])


@handler('sunpk', 'uunpk')
def h_unpk(m, mn, ops):
    m.need_streaming()
    dst = regs_of(ops[0])
    srcs = regs_of(ops[1])
    wide = ops[0].esize
    narrow = ops[1].esize
    lanes = elements(m, wide)
    outs = []
    for s in srcs:
        v = m.z(s)
        for half in range(2):
            out = 0
            for i in range(lanes):
                x = elem(v, half * lanes + i, narrow)
                if mn == 'sunpk':
                    x = sext(x, narrow)
                out = set_elem(out, i, wide, x & mask(wide))
            outs.append(out)
    for d, o in zip(dst, outs):
        m.set_z(d, o)


def float_group(m, mn, ops, fn):
    dst = regs_of(ops[0])
    src = regs_of(ops[1])
    esize = ops[0].esize
    outs = []
    for s in src:
        v = m.z(s)
        out = 0
        for i in range(elements(m, esize)):
            out = set_elem(out, i, esize, fn(elem(v, i, esize), esize))
        outs.append(out)
    for d, o in zip(dst, outs):
        m.set_z(d, o)


@handler('frinta', 'frintm', 'frintn', 'frintp')
def h_frint(m, mn, ops):
    m.need_streaming()
    rmode = {'frinta': 'A', 'frintm': 'M', 'frintn': 'N', 'frintp': 'P'}[mn]
    float_group(m, mn, ops, lambda x, e: F.fp_round_int(m.fmt(e), m.ctl, m.fst, x, rmode, False))


@handler('scvtf', 'ucvtf')
def h_icvtf(m, mn, ops):
    m.need_streaming()
    float_group(m, mn, ops, lambda x, e: F.fixed_to_fp(m.fmt(e), m.ctl, m.fst, x, e, 0, mn == 'ucvtf'))


@handler('fcvtzs', 'fcvtzu')
def h_fcvtz(m, mn, ops):
    m.need_streaming()
    float_group(m, mn, ops, lambda x, e: F.fp_to_fixed(m.fmt(e), m.ctl, m.fst, x, e, 0, mn == 'fcvtzu', 'Z'))


@handler('fcvt', 'fcvtn', 'bfcvt', 'bfcvtn', 'fcvtl')
def h_fcvt_narrow(m, mn, ops):
    m.need_streaming()
    if isinstance(ops[0], ZList):
        # FEAT_SME_F16F16: half to single, the halves (fcvt) or the even and odd elements (fcvtl)
        # of the source into the two destinations
        ctl = F.Ctl(m.ctl.rmode, fz=m.ctl.fz, dn=m.ctl.dn, ahp=m.ctl.ahp, fz16=False)
        src = m.z(ops[1].n)
        lanes = elements(m, 32)
        outs = []
        for k in range(2):
            out = 0
            for i in range(lanes):
                idx = (k * lanes + i) if mn == 'fcvt' else (2 * i + k)
                out = set_elem(out, i, 32, F.fp_convert(F.HALF, F.SINGLE, ctl, m.fst, elem(src, idx, 16)))
            outs.append(out)
        for d, o in zip(ops[0].regs, outs):
            m.set_z(d, o)
        return
    dst = ops[0].n
    srcs = regs_of(ops[1])
    wide = ops[1].esize
    narrow = ops[0].esize
    if narrow == 8:
        raise NotImplementedError(mn)
    src_fmt = m.fmt(wide)
    dst_fmt = F.BFLOAT if mn.startswith('bf') else m.fmt(narrow)
    lanes = elements(m, wide)
    n = len(srcs)
    out = 0
    # (the conversions don't flush the half precision: FPRoundCV clears FZ16)
    ctl = F.Ctl(m.ctl.rmode, fz=m.ctl.fz, dn=m.ctl.dn, ahp=m.ctl.ahp, fz16=False)
    for k, s in enumerate(srcs):
        v = m.z(s)
        for i in range(lanes):
            x = F.fp_convert(src_fmt, dst_fmt, ctl, m.fst, elem(v, i, wide))
            pos = (n * i + k) if mn.endswith('n') else (k * lanes + i)
            out = set_elem(out, pos, narrow, x)
    m.set_z(dst, out)


def saturate_narrow(value, bits, signed_result):
    if signed_result:
        return max(signed_min(bits), min(signed_max(bits), value)) & mask(bits)
    return max(0, min(mask(bits), value))


@handler('sqcvt', 'sqcvtn', 'sqcvtu', 'sqcvtun', 'uqcvt', 'uqcvtn')
def h_qcvt(m, mn, ops):
    m.need_streaming()
    dst = ops[0].n
    srcs = regs_of(ops[1])
    wide = ops[1].esize
    narrow = ops[0].esize
    lanes = elements(m, wide)
    n = len(srcs)
    in_signed = mn.startswith('sq')
    out_signed = mn in ('sqcvt', 'sqcvtn')
    out = 0
    for k, s in enumerate(srcs):
        v = m.z(s)
        for i in range(lanes):
            x = elem(v, i, wide)
            if in_signed:
                x = sext(x, wide)
            pos = (n * i + k) if mn.endswith('n') else (k * lanes + i)
            out = set_elem(out, pos, narrow, saturate_narrow(x, narrow, out_signed))
    m.set_z(dst, out)


@handler('sqrshr', 'sqrshrn', 'sqrshru', 'sqrshrun', 'uqrshr', 'uqrshrn')
def h_qrshr(m, mn, ops):
    m.need_streaming()
    dst = ops[0].n
    srcs = regs_of(ops[1])
    shift = ops[2]
    wide = ops[1].esize
    narrow = ops[0].esize
    lanes = elements(m, wide)
    n = len(srcs)
    in_signed = mn.startswith('sq')
    out_signed = mn in ('sqrshr', 'sqrshrn')
    out = 0
    for k, s in enumerate(srcs):
        v = m.z(s)
        for i in range(lanes):
            x = elem(v, i, wide)
            if in_signed:
                x = sext(x, wide)
            x = (x + (1 << (shift - 1))) >> shift
            pos = (n * i + k) if mn.endswith('n') else (k * lanes + i)
            out = set_elem(out, pos, narrow, saturate_narrow(x, narrow, out_signed))
    m.set_z(dst, out)


def counter_predicate(m, pn, esize_op, nvec, r):
    """CounterToPredicate for the vector r of a group of nvec vectors: the byte granular predicate as
    the activity of the lanes of esize_op bits (a list of booleans)."""
    v = m.st['p'][pn] & 0xFFFF
    invert = (v >> 15) & 1
    k = None
    for i in range(4):
        if (v >> i) & 1:
            k = i
            break
    n = elements(m, esize_op)
    if k is None:
        return [False] * n
    ec = 1 << k
    count = (v >> (k + 1)) & (0xFF >> k)
    pl = m.svl // 8
    out = []
    for i in range(n):
        b = i * (esize_op // 8)
        if b % ec != 0:
            out.append(False)
            continue
        g = r * (pl // ec) + b // ec
        out.append((g < count) != bool(invert))
    return out


@handler('sel')
def h_sel(m, mn, ops):
    m.need_streaming()
    dst = regs_of(ops[0])
    pn = ops[1].n
    first = regs_of(ops[2])
    second = regs_of(ops[3])
    esize = ops[0].esize
    outs = []
    for r in range(len(dst)):
        active = counter_predicate(m, pn, esize, len(dst), r)
        a, b = m.z(first[r]), m.z(second[r])
        out = 0
        for i in range(elements(m, esize)):
            out = set_elem(out, i, esize, elem(a if active[i] else b, i, esize))
        outs.append(out)
    for d, o in zip(dst, outs):
        m.set_z(d, o)


@handler('luti2', 'luti4')
def h_luti(m, mn, ops):
    m.need_streaming()
    m.need_za()
    bits = 2 if mn == 'luti2' else 4
    dst = regs_of(ops[0])
    src = ops[2]
    imm = src.index
    esize = ops[0].esize
    n = elements(m, esize)
    nreg = len(dst)
    idx_vec = m.z(src.n)
    zt0 = m.st['zt0']
    outs = []
    for r in range(nreg):
        out = 0
        for i in range(n):
            e = r * n + i
            pos = nreg * n * (imm % max(1, esize // (nreg * bits))) + e
            index = (idx_vec >> (bits * pos)) & ((1 << bits) - 1)
            out = set_elem(out, i, esize, (zt0 >> (32 * index)) & mask(esize))
        outs.append(out)
    for d, o in zip(dst, outs):
        m.set_z(d, o)


@handler('famax', 'famin')
def h_famax(m, mn, ops):
    m.need_streaming()
    from sme_ops_vec import map_group

    def fn(a, b, e):
        fmt = m.fmt(e)
        return F.fp_max_min(fmt, m.ctl, m.fst, F.fp_abs(fmt, a), F.fp_abs(fmt, b), mn == 'famax', False)
    map_group(m, ops, fn)


def fp_scale(fmt, ctl, st, a, n):
    """FPScale: a * 2^n (n a signed integer), rounded."""
    num = F.unpack(fmt, a, ctl, st)
    r = F.process_nans(fmt, [num], ctl, st)
    if r is not None:
        return r
    if num.kind in ('inf', 'zero'):
        return num.bits if num.kind == 'inf' else F.flushed_bits(fmt, num)
    n = max(-4 * (fmt.maxexp + fmt.F), min(4 * (fmt.maxexp + fmt.F), n))
    from fractions import Fraction
    return F.fp_round(fmt, ctl, st, num.value * (Fraction(2) ** n))


@handler('fscale', 'bfscale')
def h_fscale(m, mn, ops):
    m.need_streaming()
    from sme_ops_vec import map_group
    bf = mn == 'bfscale'

    def fn(a, b, e):
        return fp_scale(m.fmt(e, bf), m.ctl, m.fst, a, sext(b, e))
    map_group(m, ops, fn)
