"""The model of the SVE instructions of Arm64SemanticLiftSve.cpp, Arm64SemanticLiftSvePredicate.cpp and
Arm64SemanticLiftSveMemory.cpp, written from the pseudocode of the Arm ARM (see sve_model.py)."""
import sve_model as M
from sve_model import Machine, Row, mask, sext, predicate_test, decode_pred_count

VLS = (128, 256, 512)


def machine(vl, seed):
    m = M.random_machine(vl, seed)
    M.memory_pattern(m, 0x4000, 0x2000, 1)
    # the base registers of the memory rows
    m.x[4] = 0x4100
    m.x[5] = 3
    return m


def add_row(rows, text, seed, model, vls=VLS, setup=None):
    for vl in vls:
        m = machine(vl, text + seed)
        if setup:
            setup(m)
        rows.append(Row(text, vl, m, model))


def binary(esize, fn, predicated=True):
    """Zdn = op(Zdn, Zm) on the active lanes: merging."""
    def model(m):
        a, b = m.zget(0, esize), m.zget(2, esize)
        out = []
        for i in range(m.lanes(esize)):
            if not predicated or m.active(m.p[1], i, esize):
                out.append(fn(a[i], b[i], esize))
            else:
                out.append(a[i])
        m.zset(0, esize, out)
    return model


def rows():
    r = []
    # integer arithmetic, merging
    add_row(r, 'add z0.s, p1/m, z0.s, z2.s', 'a', binary(32, lambda a, b, e: (a + b) & mask(e)))
    add_row(r, 'sub z0.b, p1/m, z0.b, z2.b', 'b', binary(8, lambda a, b, e: (a - b) & mask(e)))
    add_row(r, 'mul z0.h, p1/m, z0.h, z2.h', 'c', binary(16, lambda a, b, e: (a * b) & mask(e)))
    add_row(r, 'smax z0.d, p1/m, z0.d, z2.d', 'd',
            binary(64, lambda a, b, e: a if sext(a, e) > sext(b, e) else b))
    add_row(r, 'umin z0.s, p1/m, z0.s, z2.s', 'e', binary(32, lambda a, b, e: min(a, b)))
    add_row(r, 'sabd z0.h, p1/m, z0.h, z2.h', 'f',
            binary(16, lambda a, b, e: abs(sext(a, e) - sext(b, e)) & mask(e)))
    add_row(r, 'smulh z0.s, p1/m, z0.s, z2.s', 'g',
            binary(32, lambda a, b, e: ((sext(a, e) * sext(b, e)) >> e) & mask(e)))
    add_row(r, 'udiv z0.s, p1/m, z0.s, z2.s', 'h',
            binary(32, lambda a, b, e: 0 if b == 0 else a // b))

    def sdiv(a, b, e):
        if b == 0:
            return 0
        x, y = sext(a, e), sext(b, e)
        q = abs(x) // abs(y)
        return (q if (x < 0) == (y < 0) else -q) & mask(e)
    add_row(r, 'sdiv z0.d, p1/m, z0.d, z2.d', 'i', binary(64, sdiv))

    # shifts by immediate and the arithmetic shift for division
    def asr_imm(m):
        a = m.zget(0, 16)
        m.zset(0, 16, [(sext(a[i], 16) >> 3) & mask(16) if m.active(m.p[1], i, 16) else a[i]
                       for i in range(m.lanes(16))])
    add_row(r, 'asr z0.h, p1/m, z0.h, #3', 'j', asr_imm)

    def asrd(m):
        a = m.zget(0, 32)
        out = []
        for i in range(m.lanes(32)):
            if m.active(m.p[1], i, 32):
                x = sext(a[i], 32)
                out.append((-((-x) >> 5) if x < 0 else x >> 5) & mask(32))
            else:
                out.append(a[i])
        m.zset(0, 32, out)
    add_row(r, 'asrd z0.s, p1/m, z0.s, #5', 'k', asrd)

    # compare, with the flags
    def cmpgt(m):
        a, b = m.zget(1, 32), m.zget(2, 32)
        bits = [1 if m.active(m.p[3], i, 32) and sext(a[i], 32) > sext(b[i], 32) else 0
                for i in range(m.lanes(32))]
        m.pset_elems(0, 32, bits)
        m.nzcv = predicate_test(m.vl, m.p[3], m.p[0], 32)
    add_row(r, 'cmpgt p0.s, p3/z, z1.s, z2.s', 'l', cmpgt)

    # while: the values wrap around at the width of the register (the M4 and Unicorn agree)
    def whilelt(m):
        x1, x2 = sext(m.x[1], 64), sext(m.x[2], 64)
        n = m.lanes(8)
        bits = []
        last = 1
        for i in range(n):
            last = last and (sext(x1 + i, 64) < x2)
            bits.append(1 if last else 0)
        m.pset_elems(0, 8, bits)
        m.nzcv = predicate_test(m.vl, (1 << (m.vl // 8)) - 1, m.p[0], 8)

    def whilelt_setup(values):
        def setup(m):
            m.x[1], m.x[2] = values
        return setup
    for k, values in enumerate([(5, 9), (0x7ffffffffffffffe, 0x7fffffffffffffff), (0x8000000000000000, 0x7fffffffffffffff),
                                (3, 3), (0, 1000)]):
        add_row(r, 'whilelt p0.b, x1, x2', 'wl%d' % k, whilelt, setup=whilelt_setup(values))

    def whilele_model(m):
        x1, x2 = sext(m.x[1], 64), sext(m.x[2], 64)
        bits = []
        last = 1
        for i in range(m.lanes(32)):
            last = last and (sext(x1 + i, 64) <= x2)
            bits.append(1 if last else 0)
        m.pset_elems(0, 32, bits)
        m.nzcv = predicate_test(m.vl, M.mask(m.vl // 8), m.p[0], 32)
    add_row(r, 'whilele p0.s, x1, x2', 'wle', whilele_model, setup=whilelt_setup((0x7fffffffffffffff, 0x7fffffffffffffff)))

    # ptrue with patterns, and the flags of ptrues
    def ptrue(pattern, esize, flags):
        def model(m):
            count = decode_pred_count(pattern, m.lanes(esize))
            m.pset_elems(0, esize, [1 if i < count else 0 for i in range(m.lanes(esize))])
            if flags:
                m.nzcv = predicate_test(m.vl, m.p[0], m.p[0], esize)
        return model
    add_row(r, 'ptrue p0.s, vl3', 'pt1', ptrue(3, 32, False))
    add_row(r, 'ptrue p0.d, pow2', 'pt2', ptrue(0, 64, False))
    add_row(r, 'ptrues p0.b, mul3', 'pt3', ptrue(30, 8, True))
    add_row(r, 'ptrues p0.h, vl16', 'pt4', ptrue(9, 16, True))
    add_row(r, 'ptrue p0.b, vl64', 'pt5', ptrue(11, 8, False))

    # the counts
    def cntp(m):
        n = 0
        for i in range(m.lanes(16)):
            n += m.active(m.p[1], i, 16) & m.active(m.p[2], i, 16)
        m.x[0] = n
    add_row(r, 'cntp x0, p1, p2.h', 'cp', cntp)

    def cntw_mul(m):
        m.x[0] = decode_pred_count(5, m.lanes(32)) * 3
    add_row(r, 'cntw x0, vl5, mul #3', 'cw', cntw_mul)

    def sqdecp32(m):
        n = sum(m.active(m.p[1], i, 8) for i in range(m.lanes(8)))
        value = max(-(1 << 31), min((1 << 31) - 1, sext(m.x[0], 32) - n))
        m.x[0] = value & mask(64)
    add_row(r, 'sqdecp x0, p1.b, w0', 'sd', sqdecp32,
            setup=lambda m: m.x.__setitem__(0, 0x80000000 + 7))

    def incd_scalar(m):
        m.x[0] = (m.x[0] + decode_pred_count(31, m.lanes(64)) * 4) & mask(64)
    add_row(r, 'incd x0, all, mul #4', 'id', incd_scalar)

    def rdvl(m):
        m.x[0] = (-3 * (m.vl // 8)) & mask(64)
    add_row(r, 'rdvl x0, #-3', 'rv', rdvl)

    # index, sel, permutes
    def index_model(m):
        start, step = sext(m.x[1], 32), -3
        m.zset(0, 32, [(start + i * step) for i in range(m.lanes(32))])
    add_row(r, 'index z0.s, w1, #-3', 'ix', index_model)

    def sel_model(m):
        a, b = m.zget(1, 16), m.zget(2, 16)
        m.zset(0, 16, [a[i] if m.active(m.p[3], i, 16) else b[i] for i in range(m.lanes(16))])
    add_row(r, 'sel z0.h, p3, z1.h, z2.h', 'se', sel_model)

    def zip1(m):
        a, b = m.zget(1, 32), m.zget(2, 32)
        n = m.lanes(32)
        m.zset(0, 32, [a[i // 2] if i % 2 == 0 else b[i // 2] for i in range(n)])
    add_row(r, 'zip1 z0.s, z1.s, z2.s', 'z1', zip1)

    def uzp2(m):
        a, b = m.zget(1, 16), m.zget(2, 16)
        both = a + b
        m.zset(0, 16, [both[2 * i + 1] for i in range(m.lanes(16))])
    add_row(r, 'uzp2 z0.h, z1.h, z2.h', 'u2', uzp2)

    def trn1(m):
        a, b = m.zget(1, 8), m.zget(2, 8)
        m.zset(0, 8, [a[i & ~1] if i % 2 == 0 else b[i & ~1] for i in range(m.lanes(8))])
    add_row(r, 'trn1 z0.b, z1.b, z2.b', 't1', trn1)

    def zip1_predicate(m):
        # the predicate elements are moved whole (two bits for .h)
        step = 2
        n = m.lanes(16)
        a, b = m.p[1], m.p[2]

        def elem(p, i):
            return (p >> (i * step)) & 3
        value = 0
        for i in range(n):
            e = elem(a, i // 2) if i % 2 == 0 else elem(b, i // 2)
            value |= e << (i * step)
        m.p[0] = value
    add_row(r, 'zip1 p0.h, p1.h, p2.h', 'zp', zip1_predicate)

    def tbl(m):
        table, index = m.zget(1, 32), m.zget(2, 32)
        n = m.lanes(32)
        m.zset(0, 32, [table[index[i]] if index[i] < n else 0 for i in range(n)])
    add_row(r, 'tbl z0.s, {z1.s}, z2.s', 'tb', tbl,
            setup=lambda m: m.zset(2, 32, [(i * 5 + 1) % (m.lanes(32) + 3) for i in range(m.lanes(32))]))

    def rev(m):
        a = m.zget(1, 16)
        m.zset(0, 16, a[::-1])
    add_row(r, 'rev z0.h, z1.h', 'rv2', rev)

    def splice(m):
        n = m.lanes(32)
        a, b = m.zget(0, 32), m.zget(2, 32)
        active = [i for i in range(n) if m.active(m.p[1], i, 32)]
        out = []
        if active:
            out = a[active[0]:active[-1] + 1]
        out = out + b[:n - len(out)]
        m.zset(0, 32, out)
    add_row(r, 'splice z0.s, p1, z0.s, z2.s', 'sp', splice)

    def compact(m):
        a = m.zget(1, 64)
        out = [a[i] for i in range(m.lanes(64)) if m.active(m.p[2], i, 64)]
        m.zset(0, 64, out + [0] * (m.lanes(64) - len(out)))
    add_row(r, 'compact z0.d, p2, z1.d', 'cm', compact)

    def lastb(m):
        n = m.lanes(32)
        active = [i for i in range(n) if m.active(m.p[1], i, 32)]
        idx = active[-1] if active else n - 1
        m.x[0] = m.zget(2, 32)[idx]
    add_row(r, 'lastb w0, p1, z2.s', 'lb', lastb)

    def lasta(m):
        n = m.lanes(16)
        active = [i for i in range(n) if m.active(m.p[1], i, 16)]
        idx = (active[-1] + 1) % n if active else 0
        m.x[0] = m.zget(2, 16)[idx]
    add_row(r, 'lasta w0, p1, z2.h', 'la', lasta)

    def insr(m):
        a = m.zget(0, 32)
        m.zset(0, 32, [m.x[1] & mask(32)] + a[:-1])
    add_row(r, 'insr z0.s, w1', 'in', insr)

    # the predicate instructions
    def brkpa(m):
        n = m.lanes(8)
        g, a, b = m.p[1], m.p[2], m.p[3]
        last_active = None
        for i in range(n):
            if m.active(g, i, 8):
                last_active = m.active(a, i, 8)
        out = []
        if last_active:
            last = 1
            for i in range(n):
                out.append(last & m.active(g, i, 8))
                if m.active(g, i, 8):
                    last = last and not m.active(b, i, 8)
        else:
            out = [0] * n
        m.pset_elems(0, 8, out)
    add_row(r, 'brkpa p0.b, p1/z, p2.b, p3.b', 'bp', brkpa)

    def pnext(m):
        n = m.lanes(32)
        cur = [i for i in range(n) if m.active(m.p[0], i, 32)]
        start = cur[-1] + 1 if cur else 0
        nxt = [i for i in range(start, n) if m.active(m.p[1], i, 32)]
        m.pset_elems(0, 32, [1 if nxt and i == nxt[0] else 0 for i in range(n)])
        m.nzcv = predicate_test(m.vl, m.p[1], m.p[0], 32)
    add_row(r, 'pnext p0.s, p1, p0.s', 'pn', pnext)

    # the floating point environment of ARM (the soft float): flush to zero raises IDC for the single
    # precision operands, the half precision flushes by FZ16, a signaling NaN of the fused multiply add is
    # chosen with the addend first, and the default NaN of FPCR.DN
    def fp_setup(fpcr, **registers):
        def setup(m):
            m.fpcr = fpcr
            m.p[1] = M.mask(m.vl // 8)
            m.fpsr = 0
            for name, lanes in registers.items():
                n, size = int(name[1:]), 32 if name[0] == 's' else 16
                full = [0] * m.lanes(size)
                for i, v in enumerate(lanes):
                    full[i] = v
                m.zset(n, size, full)
        return setup

    def fadd_flush(m):
        m.zset(0, 32, [0x3f800000] * m.lanes(32))
        m.fpsr = 0x80
    add_row(r, 'fadd z0.s, p1/m, z0.s, z2.s', 'fz', fadd_flush,
            setup=lambda m: fp_setup(0x1000000, s0=[0x00400000], s2=[0x3f800000] * m.lanes(32))(m))

    def fadd_half(m):
        m.zset(0, 16, [0x3c00] * m.lanes(16))
        m.fpsr = 0x10
    add_row(r, 'fadd z0.h, p1/m, z0.h, z2.h', 'fh', fadd_half,
            setup=lambda m: fp_setup(0x1000000, h0=[0x0200], h2=[0x3c00] * m.lanes(16))(m))

    def fmla_order(expected, flags):
        def model(m):
            m.zset(0, 32, [expected] + [0] * (m.lanes(32) - 1))
            m.fpsr = flags
        return model
    add_row(r, 'fmla z0.s, p1/m, z1.s, z2.s', 'fo1', fmla_order(0x7fc00002, 1),
            setup=lambda m: fp_setup(0, s0=[0x7fc00001], s1=[0x7f800002], s2=[0x3f800000] * m.lanes(32))(m))
    add_row(r, 'fmla z0.s, p1/m, z1.s, z2.s', 'fo2', fmla_order(0x7fe00000, 1),
            setup=lambda m: fp_setup(0, s0=[0x7fa00000], s1=[0x7f800002], s2=[0x3f800000] * m.lanes(32))(m))
    def fadd_default_nan(m):
        m.zset(0, 32, [0x7fc00000] + [0x3f800000] * (m.lanes(32) - 1))
    add_row(r, 'fadd z0.s, p1/m, z0.s, z2.s', 'dn', fadd_default_nan,
            setup=lambda m: fp_setup(0x2000000, s0=[0x7fc00003], s2=[0x3f800000] * m.lanes(32))(m))

    # memory: contiguous, extending, gather, first fault
    def ld1sh(m):
        n = m.lanes(32)
        base = m.x[4]
        out = []
        for i in range(n):
            if m.active(m.p[1], i, 32):
                out.append(sext(m.read(base + 2 * i, 2), 16))
            else:
                out.append(0)
        m.zset(0, 32, out)
    add_row(r, 'ld1sh {z0.s}, p1/z, [x4]', 'l1', ld1sh)

    def st1b_trunc(m):
        n = m.lanes(32)
        base = m.x[4] + m.x[5]
        for i in range(n):
            if m.active(m.p[1], i, 32):
                m.write(base + i, 1, m.zget(0, 32)[i])
    add_row(r, 'st1b {z0.s}, p1, [x4, x5]', 'st', st1b_trunc)

    def ld2w(m):
        n = m.lanes(32)
        base = m.x[4]
        z0, z1 = [0] * n, [0] * n
        for i in range(n):
            if m.active(m.p[1], i, 32):
                z0[i] = m.read(base + 8 * i, 4)
                z1[i] = m.read(base + 8 * i + 4, 4)
        m.zset(0, 32, z0)
        m.zset(1, 32, z1)
    add_row(r, 'ld2w {z0.s, z1.s}, p1/z, [x4]', 'l2', ld2w)

    def st3d(m):
        n = m.lanes(64)
        base = m.x[4]
        for i in range(n):
            if m.active(m.p[1], i, 64):
                for k in range(3):
                    m.write(base + 24 * i + 8 * k, 8, m.zget(2 + k, 64)[i])
    add_row(r, 'st3d {z2.d, z3.d, z4.d}, p1, [x4]', 's3', st3d)

    def ld1w_quad(m):
        # SVE2.1: a quadword for each active 128 bit element, consecutive in memory
        out = []
        for i in range(m.lanes(128)):
            out.append(m.read(m.x[4] + (m.x[5] << 2) + 16 * i, 16) if m.active(m.p[1], i, 128) else 0)
        m.zset(0, 128, out)
    add_row(r, 'ld1w {z0.q}, p1/z, [x4, x5, lsl #2]', 'lq', ld1w_quad)

    def st1d_quad(m):
        for i in range(m.lanes(128)):
            if m.active(m.p[1], i, 128):
                m.write(m.x[4] + (m.x[5] << 3) + 16 * i, 16, m.zget(0, 128)[i])
    add_row(r, 'st1d {z0.q}, p1, [x4, x5, lsl #3]', 'sq', st1d_quad)

    def gather(m):
        n = m.lanes(32)
        idx = m.zget(2, 32)
        out = []
        for i in range(n):
            if m.active(m.p[1], i, 32):
                out.append(m.read(m.x[4] + (idx[i] << 2), 4))
            else:
                out.append(0)
        m.zset(0, 32, out)
    add_row(r, 'ld1w {z0.s}, p1/z, [x4, z2.s, uxtw #2]', 'ga', gather,
            setup=lambda m: m.zset(2, 32, [(i * 7) & 0xff for i in range(m.lanes(32))]))

    def scatter(m):
        n = m.lanes(64)
        idx = m.zget(2, 64)
        for i in range(n):
            if m.active(m.p[1], i, 64):
                m.write(m.x[4] + idx[i] * 8, 8, m.zget(0, 64)[i])
    add_row(r, 'st1d {z0.d}, p1, [x4, z2.d, lsl #3]', 'sc', scatter,
            setup=lambda m: m.zset(2, 64, [(i * 5) & 0x3f for i in range(m.lanes(64))]))

    def ldff1_edge(m):
        # the memory ends at 0x20000: the elements from the first one which faults are not
        # loaded (FFR), the first active element would trap
        n = m.lanes(32)
        out = []
        faulted = False
        ffr = m.ffr
        first = True
        for i in range(n):
            if m.active(m.p[1], i, 32):
                v = m.read(m.x[4] + 4 * i, 4)
                if v is None:
                    if first:
                        m.exit = 'fault'
                        return
                    faulted = True
            if faulted:
                ffr &= ~(0xf << (i * 4))
                out.append(0)
            else:
                if m.active(m.p[1], i, 32):
                    first = False
                    out.append(v)
                else:
                    out.append(0)
        m.zset(0, 32, out)
        m.ffr = ffr
    add_row(r, 'ldff1w {z0.s}, p1/z, [x4]', 'ff', ldff1_edge,
            setup=lambda m: (m.x.__setitem__(4, 0x20000 - 8), m.p.__setitem__(1, M.mask(m.vl // 8))))

    def ldnf1_edge(m):
        n = m.lanes(8)
        out = []
        faulted = False
        ffr = m.ffr
        for i in range(n):
            v = None
            if m.active(m.p[1], i, 8):
                v = m.read(m.x[4] + i, 1)
                if v is None:
                    faulted = True
            if faulted:
                ffr &= ~(1 << i)
                out.append(0)
            else:
                out.append(v if v is not None else 0)
        m.zset(0, 8, out)
        m.ffr = ffr
    add_row(r, 'ldnf1b {z0.b}, p1/z, [x4]', 'nf', ldnf1_edge,
            setup=lambda m: (m.x.__setitem__(4, 0x20000 - 5), m.p.__setitem__(1, M.mask(m.vl // 8))))
    return r
