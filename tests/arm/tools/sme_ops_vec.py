"""The multi vector instructions of the SME2 model (sme_model.py) which don't use za: the destructive
add / min / max / shifts / saturating doubling multiply with one vector or a vector group, clamps, sel,
zip, uzp, unpack, the conversions with vector groups and the rounding to integral.

Author: Elad Raz <e@eladraz.com>
"""
import fpmodel as F

from sme_model import (handler, mask, sext, elem, set_elem, Za, Zr, ZList, Pred, M64, SIZES)


def regs_of(op):
    return op.regs if isinstance(op, ZList) else [op.n]


def elements(m, esize):
    return m.svl // esize


def signed_max(bits):
    return (1 << (bits - 1)) - 1


def signed_min(bits):
    return -(1 << (bits - 1))


def map_group(m, ops, fn):
    """Destructive / non destructive group operation: ops = [dst list, src list, src2 (list or single)];
    fn(esize, d_elem, zm_elem) -> the new element (bits). Operates on every register of the group."""
    dst = regs_of(ops[0])
    first = regs_of(ops[1])
    third = ops[2]
    esize = ops[0].esize
    zms = regs_of(third)
    single = not isinstance(third, ZList)
    results = []
    for r in range(len(dst)):
        a = m.z(first[r])
        b = m.z(zms[0 if single else r])
        out = 0
        for i in range(elements(m, esize)):
            out = set_elem(out, i, esize, fn(elem(a, i, esize), elem(b, i, esize), esize))
        results.append(out)
    for r in range(len(dst)):
        m.set_z(dst[r], results[r])


@handler('smax', 'umax', 'smin', 'umin')
def h_minmax(m, mn, ops):
    m.need_streaming()
    signed = mn[0] == 's'
    is_max = mn.endswith('max')

    def fn(a, b, e):
        if signed:
            a, b = sext(a, e), sext(b, e)
        return max(a, b) if is_max else min(a, b)
    map_group(m, ops, fn)


@handler('add')
def h_add_group(m, mn, ops):
    if isinstance(ops[0], Za):
        import sme_ops_array
        return sme_ops_array.h_add(m, mn, ops)
    m.need_streaming()
    map_group(m, ops, lambda a, b, e: (a + b) & mask(e))


@handler('sqdmulh')
def h_sqdmulh(m, mn, ops):
    m.need_streaming()

    def fn(a, b, e):
        r = (2 * sext(a, e) * sext(b, e)) >> e
        return max(signed_min(e), min(signed_max(e), r)) & mask(e)
    map_group(m, ops, fn)


@handler('srshl', 'urshl')
def h_rshl(m, mn, ops):
    m.need_streaming()
    signed = mn[0] == 's'

    def fn(a, b, e):
        sh = sext(b, e)
        x = sext(a, e) if signed else a
        if sh >= 0:
            r = (x << sh) if sh < e else 0
        else:
            n = -sh
            r = ((x + (1 << (n - 1))) >> n) if n <= e else 0
        return r & mask(e)
    map_group(m, ops, fn)


@handler('fmax', 'fmin', 'fmaxnm', 'fminnm', 'bfmax', 'bfmin', 'bfmaxnm', 'bfminnm')
def h_fminmax(m, mn, ops):
    m.need_streaming()
    bf = mn.startswith('bf')
    is_max = mn.lstrip('b').startswith('fmax')
    num = mn.endswith('nm')

    def fn(a, b, e):
        return F.fp_max_min(m.fmt(e, bf), m.ctl, m.fst, a, b, is_max, num)
    map_group(m, ops, fn)


@handler('fmul', 'bfmul')
def h_fmul(m, mn, ops):
    m.need_streaming()
    bf = mn.startswith('bf')

    def fn(a, b, e):
        return F.fp_mul(m.fmt(e, bf), m.ctl, m.fst, a, b)
    map_group(m, ops, fn)


@handler('sclamp', 'uclamp', 'fclamp', 'bfclamp')
def h_clamp(m, mn, ops):
    m.need_streaming()
    dst = regs_of(ops[0])
    lo = m.z(ops[1].n)
    hi = m.z(ops[2].n)
    esize = ops[0].esize
    for d in dst:
        a = m.z(d)
        out = 0
        for i in range(elements(m, esize)):
            x, l, h = elem(a, i, esize), elem(lo, i, esize), elem(hi, i, esize)
            if mn in ('fclamp', 'bfclamp'):
                fmt = m.fmt(esize, mn == 'bfclamp')
                r = F.fp_max_min(fmt, m.ctl, m.fst, F.fp_max_min(fmt, m.ctl, m.fst, l, x, True, True), h, False, True)
            elif mn == 'sclamp':
                r = min(max(sext(x, esize), sext(l, esize)), sext(h, esize)) & mask(esize)
            else:
                r = min(max(x, l), h)
            out = set_elem(out, i, esize, r)
        m.set_z(d, out)
