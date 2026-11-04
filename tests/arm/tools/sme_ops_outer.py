"""The outer products of the SME model (sme_model.py): fmopa / fmops (single, double, half and bfloat16
widening), smopa / umopa / sumopa / usmopa and the s forms (8, 16 bit to 32 bit, 16 bit to 64 bit),
bmopa / bmops, addha / addva.

Author: Elad Raz <e@eladraz.com>
"""
import fpmodel as F

from sme_model import (handler, mask, sext, elem, set_elem, pred_active, Za, Zr, Pred, M64)

# The element of the sources of a widening outer product, and the number of elements per result
# element ("ways"): (source esize, ways) from the mnemonic and the tile size
INT_WAYS = {('b', 's'): (8, 4), ('h', 's'): (16, 2), ('h', 'd'): (16, 4)}


def fmuladd(m, esize, acc, a, b, bf=False):
    return F.fp_muladd(m.fmt(esize, bf), m.ctl, m.fst, acc, a, b)


def fp_in(m, src_fmt, dst_fmt, bits):
    """Convert a narrower float to the wider format (exact)."""
    return F.fp_convert(src_fmt, dst_fmt, m.ctl, m.fst, bits)


def add_odd(m, esize, a, b):
    """FPAdd of the standard BFloat16 behaviours: rounded to odd (an overflow still gives infinity)."""
    fmt = m.fmt(esize)
    st = F.State()
    r = F.fp_add(fmt, F.Ctl('O', fz=True, dn=True), st, a, b)
    if st.flags & F.OFC:
        r = F.fp_add(fmt, F.Ctl('N', fz=True, dn=True), F.State(), a, b)
    m.fst.flags |= st.flags
    return r


@handler('fmopa', 'fmops', 'bfmopa', 'bfmops')
def h_fmopa(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pn, pm, zn, zm = ops
    m.za_float()
    sub = mn.endswith('s')
    esize = za.esize
    dim = m.svl // esize
    p1, p2 = m.p(pn.n), m.p(pm.n)
    a, b = m.z(zn.n), m.z(zm.n)
    if zn.esize == esize:
        # non widening
        fmt = m.fmt(esize, mn.startswith('bf'))
        for r in range(dim):
            for c in range(dim):
                if pred_active(p1, r, esize) and pred_active(p2, c, esize):
                    e1 = elem(a, r, esize)
                    if sub:
                        e1 = F.fp_neg(fmt, e1)
                    acc = m.tile_elem(za.tile, esize, r, c)
                    m.tile_set_elem(za.tile, esize, r, c, fmuladd(m, esize, acc, e1, elem(b, c, esize), mn.startswith('bf')))
        return
    # widening: half (or bfloat16) to single. The two products of an element (exact in single
    # precision) are added and rounded once, then added to the accumulator
    is_bf = mn.startswith('bf')
    src = F.BFLOAT if is_bf else F.HALF
    ctl = m.ctl
    if is_bf:
        # the standard BFloat16 behaviours: round to nearest even, flush to zero, default NaN
        ctl = F.Ctl('N', fz=True, dn=True)
    for r in range(dim):
        for c in range(dim):
            acc = m.tile_elem(za.tile, esize, r, c)
            act1 = [pred_active(p1, 2 * r + k, 16) for k in range(2)]
            act2 = [pred_active(p2, 2 * c + k, 16) for k in range(2)]
            if not any(act1[k] and act2[k] for k in range(2)):
                continue
            prods = []
            for k in range(2):
                # an inactive element is zero (so a NaN against an inactive element is a NaN)
                e1 = fp_in(m, src, F.SINGLE, elem(a, 2 * r + k, 16))
                if sub:
                    e1 = F.fp_neg(F.SINGLE, e1)
                if not act1[k]:
                    e1 = 0
                e2 = fp_in(m, src, F.SINGLE, elem(b, 2 * c + k, 16)) if act2[k] else 0
                prods.append(F.fp_mul(F.SINGLE, ctl, m.fst, e1, e2))
            if is_bf:
                total = add_odd(m, 32, prods[0], prods[1])
                m.tile_set_elem(za.tile, esize, r, c, add_odd(m, 32, acc, total))
            else:
                total = F.fp_add(F.SINGLE, ctl, m.fst, prods[0], prods[1])
                m.tile_set_elem(za.tile, esize, r, c, F.fp_add(F.SINGLE, ctl, m.fst, acc, total))


@handler('smopa', 'smops', 'umopa', 'umops', 'sumopa', 'sumops', 'usmopa', 'usmops')
def h_intmopa(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pn, pm, zn, zm = ops
    sub = mn.endswith('s')
    kind = mn[:-4]
    s1 = kind in ('s', 'su')
    s2 = kind in ('s', 'us')
    esize = za.esize
    ssize, ways = INT_WAYS[(zn.size, za.size)]
    dim = m.svl // esize
    p1, p2 = m.p(pn.n), m.p(pm.n)
    a, b = m.z(zn.n), m.z(zm.n)
    for r in range(dim):
        for c in range(dim):
            acc = m.tile_elem(za.tile, esize, r, c)
            for k in range(ways):
                if pred_active(p1, ways * r + k, ssize) and pred_active(p2, ways * c + k, ssize):
                    e1 = elem(a, ways * r + k, ssize)
                    e2 = elem(b, ways * c + k, ssize)
                    if s1:
                        e1 = sext(e1, ssize)
                    if s2:
                        e2 = sext(e2, ssize)
                    prod = e1 * e2
                    acc = (acc - prod) if sub else (acc + prod)
            m.tile_set_elem(za.tile, esize, r, c, acc & mask(esize))


@handler('bmopa', 'bmops')
def h_bmopa(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pn, pm, zn, zm = ops
    sub = mn.endswith('s')
    dim = m.svl // 32
    p1, p2 = m.p(pn.n), m.p(pm.n)
    a, b = m.z(zn.n), m.z(zm.n)
    for r in range(dim):
        for c in range(dim):
            if pred_active(p1, r, 32) and pred_active(p2, c, 32):
                acc = m.tile_elem(za.tile, 32, r, c)
                x = bin(~(elem(a, r, 32) ^ elem(b, c, 32)) & mask(32)).count('1')
                acc = (acc - x) if sub else (acc + x)
                m.tile_set_elem(za.tile, 32, r, c, acc & mask(32))


@handler('addha', 'addva')
def h_addha(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pn, pm, zn = ops
    esize = za.esize
    dim = m.svl // esize
    p1, p2 = m.p(pn.n), m.p(pm.n)
    a = m.z(zn.n)
    for r in range(dim):
        for c in range(dim):
            if pred_active(p1, r, esize) and pred_active(p2, c, esize):
                v = elem(a, c if mn == 'addha' else r, esize)
                acc = m.tile_elem(za.tile, esize, r, c)
                m.tile_set_elem(za.tile, esize, r, c, (acc + v) & mask(esize))
