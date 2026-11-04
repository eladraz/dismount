"""The ZA array vector instructions of the SME model (sme_model.py): the accumulating add / sub / fadd /
fsub, fmla / fmls, the dot products (sdot, udot, usdot, sudot, bfdot, fdot), the vertical dot products,
and the long multiply accumulates (smlal... smlall, fmlal, bfmlal).

Author: Elad Raz <e@eladraz.com>
"""
import fpmodel as F

from sme_model import (handler, mask, sext, elem, set_elem, Za, Zr, ZList, M64)


class Srcs(object):
    """The z sources of a ZA array instruction: zn registers (a list), zm either a list (one per zn), a
    single register, or an indexed register."""

    def __init__(self, m, ops):
        self.zn = ops[1].regs if isinstance(ops[1], ZList) else [ops[1].n]
        self.nreg = len(self.zn)
        self.esize = ops[1].esize
        third = ops[2] if len(ops) > 2 else None
        self.zm = None
        self.index = None
        if isinstance(third, ZList):
            self.zm = third.regs
        elif isinstance(third, Zr):
            self.zm = [third.n] * self.nreg
            self.index = third.index

    def zn_val(self, m, r):
        return m.z(self.zn[r])

    def zm_val(self, m, r):
        return m.z(self.zm[r])


def segment_elem(value, i, esize, index, bits_per_group, svl):
    """The element of an indexed source: the group `index` (of bits_per_group bits) of the 128 bit
    segment of lane i."""
    per_seg = 128 // bits_per_group
    seg = (i * esize) // 128
    return elem(value, seg * per_seg + index, bits_per_group)


def zm_group(m, src, r, i, bits, group_bits):
    """The group of `group_bits` bits (an element or a 32 bit chunk of a dot product) of zm for
    lane i."""
    v = m.z(src.zm[r])
    if src.index is not None:
        return segment_elem(v, i * group_bits // bits if False else i, bits, src.index, group_bits, m.svl)
    return elem(v, i, group_bits)


@handler('add', 'sub')
def h_add(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za = ops[0]
    if not isinstance(za, Za) or za.tile is not None or not za.has_index:
        raise NotImplementedError(mn)
    src = Srcs(m, ops)
    esize = za.esize
    vecs = group(m, za, src.nreg)
    for r, vec in enumerate(vecs):
        row = m.za_row(vec)
        for i in range(m.svl // esize):
            x = elem(m.z(src.zn[r]), i, esize)
            if src.zm is not None:
                # the array results forms write zn +/- zm (no accumulation)
                y = elem(m.z(src.zm[r]), i, esize)
                a = x - y if mn == 'sub' else x + y
            else:
                a = elem(row, i, esize)
                a = a - x if mn == 'sub' else a + x
            row = set_elem(row, i, esize, a)
        m.set_za_row(vec, row)


def group(m, za, nreg):
    stride = m.rows // nreg
    base = (m.w(za.reg) + za.off) % stride
    return [base + r * stride for r in range(nreg)]


@handler('fadd', 'fsub', 'bfadd', 'bfsub')
def h_fadd(m, mn, ops):
    m.need_streaming()
    m.need_za()
    m.za_float()
    za = ops[0]
    src = Srcs(m, ops)
    esize = za.esize
    fmt = m.fmt(esize, mn.startswith('bf'))
    for r, vec in enumerate(group(m, za, src.nreg)):
        row = m.za_row(vec)
        for i in range(m.svl // esize):
            x = elem(m.z(src.zn[r]), i, esize)
            a = elem(row, i, esize)
            a = (F.fp_sub if mn.endswith('sub') else F.fp_add)(fmt, m.ctl, m.fst, a, x)
            row = set_elem(row, i, esize, a)
        m.set_za_row(vec, row)


@handler('fmla', 'fmls', 'bfmla', 'bfmls')
def h_fmla(m, mn, ops):
    m.need_streaming()
    m.need_za()
    m.za_float()
    za = ops[0]
    src = Srcs(m, ops)
    esize = za.esize
    fmt = m.fmt(esize, mn.startswith('bf'))
    for r, vec in enumerate(group(m, za, src.nreg)):
        row = m.za_row(vec)
        for i in range(m.svl // esize):
            x = elem(m.z(src.zn[r]), i, esize)
            if mn.endswith('mls'):
                x = F.fp_neg(fmt, x)
            y = zm_group(m, src, r, i, esize, esize)
            row = set_elem(row, i, esize, F.fp_muladd(fmt, m.ctl, m.fst, elem(row, i, esize), x, y))
        m.set_za_row(vec, row)


def dot2(m, acc, a0, a1, b0, b1, is_bf):
    """The sum of two half (bfloat16) products added to a single precision accumulator (as the
    widening fmopa)."""
    src = F.BFLOAT if is_bf else F.HALF
    ctl = F.Ctl('N', fz=True, dn=True) if is_bf else m.ctl
    ps = []
    for a, b in ((a0, b0), (a1, b1)):
        x = F.fp_convert(src, F.SINGLE, ctl, m.fst, a)
        y = F.fp_convert(src, F.SINGLE, ctl, m.fst, b)
        ps.append(F.fp_mul(F.SINGLE, ctl, m.fst, x, y))
    if is_bf:
        from sme_ops_outer import add_odd
        return add_odd(m, 32, acc, add_odd(m, 32, ps[0], ps[1]))
    return F.fp_add(F.SINGLE, ctl, m.fst, acc, F.fp_add(F.SINGLE, ctl, m.fst, ps[0], ps[1]))


@handler('sdot', 'udot', 'usdot', 'sudot', 'bfdot', 'fdot')
def h_dot(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za = ops[0]
    src = Srcs(m, ops)
    esize = za.esize
    ssize = src.esize
    ways = esize // ssize
    s1 = mn in ('sdot', 'sudot')
    s2 = mn in ('sdot', 'usdot')
    is_float = mn in ('bfdot', 'fdot')
    if is_float:
        m.za_float()
    for r, vec in enumerate(group(m, za, src.nreg)):
        row = m.za_row(vec)
        zn = m.z(src.zn[r])
        for i in range(m.svl // esize):
            acc = elem(row, i, esize)
            if is_float:
                b = [zm_dot_elem(m, src, r, i, ways, k, ssize, esize) for k in range(2)]
                acc = dot2(m, acc, elem(zn, 2 * i, 16), elem(zn, 2 * i + 1, 16), b[0], b[1], mn == 'bfdot')
            else:
                for k in range(ways):
                    x = elem(zn, ways * i + k, ssize)
                    y = zm_dot_elem(m, src, r, i, ways, k, ssize, esize)
                    if s1:
                        x = sext(x, ssize)
                    if s2:
                        y = sext(y, ssize)
                    acc += x * y
                acc &= mask(esize)
            row = set_elem(row, i, esize, acc)
        m.set_za_row(vec, row)


def zm_dot_elem(m, src, r, i, ways, k, ssize, esize):
    """Element k of the group of lane i of zm (indexed: the group `index` of the 128 bit segment)."""
    v = m.z(src.zm[r])
    if src.index is not None:
        per_seg = 128 // esize
        seg = (i * esize) // 128
        g = seg * per_seg + src.index
    else:
        g = i
    return elem(v, ways * g + k, ssize)


@handler('smlal', 'umlal', 'smlsl', 'umlsl', 'smlall', 'umlall', 'smlsll', 'umlsll', 'sumlall', 'usmlall')
def h_mlal(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za = ops[0]
    src = Srcs(m, ops)
    esize = za.esize
    ssize = src.esize
    ways = esize // ssize
    sub = 'sl' in mn[:-1] and mn.endswith(('sl', 'sll')) and mn[1:3] != 'ml'
    sub = mn in ('smlsl', 'umlsl', 'smlsll', 'umlsll')
    s1 = mn in ('smlal', 'smlsl', 'smlall', 'smlsll', 'sumlall')
    s2 = mn in ('smlal', 'smlsl', 'smlall', 'smlsll', 'usmlall')
    nreg = src.nreg
    stride = m.rows // nreg
    w = m.w(za.reg)
    base = ((w - w % ways) + za.off) % stride
    for r in range(nreg):
        for j in range(ways):
            vec = base + r * stride + j
            row = m.za_row(vec)
            zn = m.z(src.zn[r])
            for i in range(m.svl // esize):
                x = elem(zn, ways * i + j, ssize)
                y = zm_long_elem(m, src, r, i, ways, j, ssize, esize)
                if s1:
                    x = sext(x, ssize)
                if s2:
                    y = sext(y, ssize)
                a = elem(row, i, esize)
                a = a - x * y if sub else a + x * y
                row = set_elem(row, i, esize, a)
            m.set_za_row(vec, row)


def zm_long_elem(m, src, r, i, ways, j, ssize, esize):
    v = m.z(src.zm[r])
    if src.index is not None:
        # the element `index` of the 128 bit segment of the source lane (ways * i + j)
        per_seg = 128 // ssize
        seg = ((ways * i + j) * ssize) // 128
        return elem(v, seg * per_seg + src.index, ssize)
    return elem(v, ways * i + j, ssize)


@handler('fmlal', 'fmlsl', 'bfmlal', 'bfmlsl')
def h_fmlal(m, mn, ops):
    m.need_streaming()
    m.need_za()
    m.za_float()
    za = ops[0]
    src = Srcs(m, ops)
    sub = mn.endswith('sl')
    is_bf = mn.startswith('bf')
    sf = F.BFLOAT if is_bf else F.HALF
    esize = 32
    ssize = 16
    ways = 2
    nreg = src.nreg
    stride = m.rows // nreg
    w = m.w(za.reg)
    base = ((w - w % ways) + za.off) % stride
    for r in range(nreg):
        for j in range(ways):
            vec = base + r * stride + j
            row = m.za_row(vec)
            zn = m.z(src.zn[r])
            for i in range(m.svl // esize):
                x = F.fp_convert(sf, F.SINGLE, m.ctl, m.fst, elem(zn, ways * i + j, ssize))
                if sub:
                    x = F.fp_neg(F.SINGLE, x)
                y = F.fp_convert(sf, F.SINGLE, m.ctl, m.fst, zm_long_elem(m, src, r, i, ways, j, ssize, esize))
                acc = elem(row, i, esize)
                res = F.fp_muladd(F.SINGLE, m.ctl, m.fst, acc, x, y)
                row = set_elem(row, i, esize, res)
            m.set_za_row(vec, row)


@handler('svdot', 'uvdot', 'suvdot', 'usvdot', 'fvdot', 'bfvdot')
def h_vdot(m, mn, ops):
    """The vertical dot products: the destination vector r takes element (ways * i + r) of every
    source register, multiplied by the group of ways elements of the indexed zm."""
    m.need_streaming()
    m.need_za()
    za = ops[0]
    src = Srcs(m, ops)
    esize = za.esize
    ssize = src.esize
    ways = src.nreg
    is_float = mn in ('fvdot', 'bfvdot')
    if is_float:
        m.za_float()
    s1 = mn in ('svdot', 'suvdot')
    s2 = mn in ('svdot', 'usvdot')
    stride = m.rows // ways
    base = (m.w(za.reg) + za.off) % stride
    per_seg = 128 // esize
    zmv = m.z(src.zm[0])
    for r in range(ways):
        vec = base + r * stride
        row = m.za_row(vec)
        for i in range(m.svl // esize):
            seg = i // per_seg
            acc = elem(row, i, esize)
            xs = [elem(m.z(src.zn[k]), ways * i + r, ssize) for k in range(ways)]
            ys = [elem(zmv, seg * (128 // ssize) + ways * src.index + k, ssize) for k in range(ways)]
            if is_float:
                acc = dot2(m, acc, xs[0], xs[1], ys[0], ys[1], mn == 'bfvdot')
            else:
                for k in range(ways):
                    x = sext(xs[k], ssize) if s1 else xs[k]
                    y = sext(ys[k], ssize) if s2 else ys[k]
                    acc += x * y
                acc &= mask(esize)
            row = set_elem(row, i, esize, acc)
        m.set_za_row(vec, row)
