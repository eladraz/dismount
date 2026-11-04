"""The data movement instructions of the SME model (sme_model.py): mova / movaz / movt, the loads and
stores of the slices, ldr / str of za and zt0, zero.

Author: Elad Raz <e@eladraz.com>
"""
import re

from sme_model import (handler, mask, elem, set_elem, pred_active, Za, Zr, ZList, Pred, Gpr, Mem,
                       za_group, zlist_of, M64)


def multi_slice(m, za, nreg, r):
    """The r-th slice of a multi vector move: the vector select rounded down to a multiple of nreg."""
    w = m.w(za.reg)
    return (w - w % nreg + za.off + r) % m.tile_dim(za.esize)


@handler('mova', 'mov')
def h_mova(m, mn, ops):
    a, b = ops[0], ops[-1]
    if isinstance(a, tuple) or isinstance(b, tuple) or isinstance(a, Gpr) or isinstance(b, Gpr):
        return h_movt(m, mn, ops)
    m.need_streaming()
    m.need_za()
    if isinstance(a, Zr) and isinstance(ops[1], Pred):
        # tile -> vector, merging
        za = b
        esize = za.esize
        sl = m.slice_get(za, m.slice_index(za))
        res = m.z(a.n)
        p = m.p(ops[1].n)
        for e in range(m.svl // esize):
            if pred_active(p, e, esize):
                res = set_elem(res, e, esize, elem(sl, e, esize))
        m.set_z(a.n, res)
        return
    if isinstance(a, Za) and a.tile is not None and isinstance(ops[1], Pred):
        za = a
        esize = za.esize
        idx = m.slice_index(za)
        sl = m.slice_get(za, idx)
        p = m.p(ops[1].n)
        zn = m.z(b.n)
        for e in range(m.svl // esize):
            if pred_active(p, e, esize):
                sl = set_elem(sl, e, esize, elem(zn, e, esize))
        m.slice_set(za, idx, sl)
        return
    # SME2 multi vector forms
    if isinstance(a, Za):
        regs = b.regs
        if a.tile is None:
            for r, vec in enumerate(za_group(m, a, len(regs))):
                m.set_za_row(vec, m.z(regs[r]))
        else:
            for r in range(len(regs)):
                idx = multi_slice(m, a, len(regs), r)
                m.slice_set(a, idx, m.z(regs[r]))
        return
    regs = a.regs
    if b.tile is None:
        for r, vec in enumerate(za_group(m, b, len(regs))):
            m.set_z(regs[r], m.za_row(vec))
    else:
        for r in range(len(regs)):
            idx = multi_slice(m, b, len(regs), r)
            m.set_z(regs[r], m.slice_get(b, idx))


@handler('movaz')
def h_movaz(m, mn, ops):
    m.need_streaming()
    m.need_za()
    a, b = ops[0], ops[1]
    regs = zlist_of(a)
    if b.tile is None:
        for r, vec in enumerate(za_group(m, b, len(regs))):
            m.set_z(regs[r], m.za_row(vec))
            m.set_za_row(vec, 0)
    else:
        for r in range(len(regs)):
            idx = multi_slice(m, b, len(regs), r)
            m.set_z(regs[r], m.slice_get(b, idx))
            m.slice_set(b, idx, 0)


@handler('movt')
def h_movt(m, mn, ops):
    m.need_za()
    a, b = ops[0], ops[1]
    if isinstance(a, Gpr):
        off = b[1]
        v = (m.st['zt0'] >> (8 * off)) & M64
        if a.n != 31:
            m.st['x'][a.n] = v
        return
    if isinstance(b, Gpr):
        off = a[1]
        zt = m.st['zt0']
        zt &= ~(M64 << (8 * off))
        zt |= (m.x(b.n) & M64) << (8 * off)
        m.st['zt0'] = zt
        return
    raise NotImplementedError(mn)


def base_address(m, mem):
    base = m.st['sp'] if mem.base == 'sp' else m.xsp(int(mem.base[1:]))
    return base


@handler('ld1b', 'ld1h', 'ld1w', 'ld1d', 'ld1q')
def h_ld1(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pg, mem = ops[0], ops[1], ops[2]
    esize = za.esize
    idx = m.slice_index(za)
    addr = (base_address(m, mem) + ((m.x(mem.index) << mem.shift) if mem.index is not None else 0)) & M64
    p = m.p(pg.n)
    sl = 0
    nb = esize // 8
    for e in range(m.svl // esize):
        if pred_active(p, e, esize):
            sl = set_elem(sl, e, esize, m.read((addr + e * nb) & M64, nb))
    m.slice_set(za, idx, sl)


@handler('st1b', 'st1h', 'st1w', 'st1d', 'st1q')
def h_st1(m, mn, ops):
    m.need_streaming()
    m.need_za()
    za, pg, mem = ops[0], ops[1], ops[2]
    esize = za.esize
    idx = m.slice_index(za)
    addr = (base_address(m, mem) + ((m.x(mem.index) << mem.shift) if mem.index is not None else 0)) & M64
    p = m.p(pg.n)
    sl = m.slice_get(za, idx)
    nb = esize // 8
    for e in range(m.svl // esize):
        if pred_active(p, e, esize):
            m.write((addr + e * nb) & M64, nb, elem(sl, e, esize))


@handler('ldr')
def h_ldr(m, mn, ops):
    m.need_za()
    base = base_address(m, ops[1])
    if isinstance(ops[0], tuple):
        m.st['zt0'] = m.read(base, 64)
        return
    za = ops[0]
    imm = ops[1].imm
    vec = (m.w(za.reg) + imm) % m.rows
    n = m.rows
    m.set_za_row(vec, m.read((base + imm * n) & M64, n))


@handler('str')
def h_str(m, mn, ops):
    m.need_za()
    base = base_address(m, ops[1])
    if isinstance(ops[0], tuple):
        m.write(base, 64, m.st['zt0'])
        return
    za = ops[0]
    imm = ops[1].imm
    vec = (m.w(za.reg) + imm) % m.rows
    n = m.rows
    m.write((base + imm * n) & M64, n, m.za_row(vec))


@handler('zero')
def h_zero(m, mn, ops):
    m.need_za()
    a = ops[0]
    if a == 'zt0list':
        m.st['zt0'] = 0
        return
    if isinstance(a, Za):
        nreg = a.vg or 1
        stride = m.rows // nreg
        for r in range(nreg):
            for k in range(a.off, a.last + 1):
                m.set_za_row((m.w(a.reg) + k) % stride + r * stride, 0)
        return
    tiles = set()
    if a.replace(' ', '') == '{za}':
        tiles = set(range(8))
    else:
        for t in re.findall(r'za(\d)\.d', a):
            tiles.add(int(t))
    for r in range(m.rows):
        if r % 8 in tiles:
            m.set_za_row(r, 0)


def set_mode(m, bit, new):
    """smstart / smstop / msr svcr: a bit of SVCR (0: SM, 1: ZA) set to 'new'. A change of the
    streaming mode zeroes z, p, ffr (and so v) and sets fpsr to 0x0800009F, a change of the ZA
    enable zeroes za and zt0 (measured on the M4: a bit which doesn't change does nothing)."""
    key = 'sm' if bit == 0 else 'za'
    new = bool(new)
    if bool(m.st.get(key)) == new:
        return
    m.st[key] = new
    if bit == 0:
        m.st['z'] = [0] * 32
        m.st['p'] = [0] * 16
        m.st['ffr'] = 0
        m.st['v'] = [0] * 32
        m.st['fpsr'] = 0x0800009F
    else:
        m.st['zarows'] = [0] * m.rows
        m.st['zt0'] = 0


@handler('smstart', 'smstop')
def h_smstart(m, mn, ops):
    start = mn == 'smstart'
    which = ops[0] if ops else 'smza'
    if which in ('sm', 'smza'):
        set_mode(m, 0, start)
    if which in ('za', 'smza'):
        set_mode(m, 1, start)


@handler('msr')
def h_msr(m, mn, ops):
    if ops[0] != 'svcr':
        raise NotImplementedError(mn)
    v = m.x(ops[1].n)
    set_mode(m, 0, v & 1)
    set_mode(m, 1, (v >> 1) & 1)


@handler('mrs')
def h_mrs(m, mn, ops):
    if ops[1] != 'svcr':
        raise NotImplementedError(mn)
    v = (1 if m.st.get('sm') else 0) | (2 if m.st.get('za') else 0)
    if ops[0].n != 31:
        m.st['x'][ops[0].n] = v


@handler('rdsvl', 'addsvl', 'addspl')
def h_rdsvl(m, mn, ops):
    """The multiples of the streaming vector length in bytes (SVL / 8) and of the predicate length
    (SVL / 64); not streaming instructions."""
    unit = (m.svl // 64) if mn == 'addspl' else (m.svl // 8)
    offset = ops[-1] * unit
    if mn == 'rdsvl':
        value = offset
    else:
        base = m.st['sp'] if ops[1].text == 'sp' else m.x(ops[1].n)
        value = base + offset
    value &= M64
    d = ops[0]
    if d.text == 'sp':
        m.st['sp'] = value
    elif d.n != 31:
        m.st['x'][d.n] = value
