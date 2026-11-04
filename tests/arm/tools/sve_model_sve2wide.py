"""The model of the SVE2 widening and narrowing instructions of Arm64SemanticLiftSve2Wide.cpp, written from
the pseudocode of the Arm ARM (see sve_model.py): the matrix multiplies and the quadword polynomial
multiplies which neither oracle runs, and the edge cases of the long, saturating and narrowing instructions
at the vector lengths the M4 does not run."""
import sve_model as M
from sve_model import Row, mask, sext

VLS = (128, 256, 512)


def machine(vl, seed):
    return M.random_machine(vl, seed)


def add_row(rows, text, seed, model, vls=VLS, setup=None):
    for vl in vls:
        m = machine(vl, text + seed)
        if setup:
            setup(m)
        rows.append(Row(text, vl, m, model))


def signed_edges(esize, count):
    """Elements at the signed boundaries, repeated."""
    base = [1 << (esize - 1), (1 << (esize - 1)) - 1, mask(esize), 0, 1, 2, (1 << (esize - 1)) + 1]
    return [base[i % len(base)] for i in range(count)]


def clmul(a, b):
    r = 0
    for i in range(64):
        if (b >> i) & 1:
            r ^= a << i
    return r


def mmla(first_signed, second_signed):
    def model(m):
        a, b, acc = m.zget(1, 8), m.zget(2, 8), m.zget(0, 32)
        out = list(acc)
        for seg in range(m.vl // 128):
            for i in range(2):
                for j in range(2):
                    total = acc[4 * seg + 2 * i + j]
                    for k in range(8):
                        x = a[16 * seg + 8 * i + k]
                        y = b[16 * seg + 8 * j + k]
                        x = sext(x, 8) if first_signed else x
                        y = sext(y, 8) if second_signed else y
                        total += x * y
                    out[4 * seg + 2 * i + j] = total & mask(32)
        m.zset(0, 32, out)
    return model


def pmull_quad(top):
    def model(m):
        a, b = m.zget(1, 64), m.zget(2, 64)
        out = []
        for seg in range(m.vl // 128):
            p = clmul(a[2 * seg + top], b[2 * seg + top])
            out += [p & mask(64), p >> 64]
        m.zset(0, 64, out)
    return model


def sqdmullb(top, esize):
    n = esize // 2

    def model(m):
        a, b = m.zget(1, esize), m.zget(2, esize)
        out = []
        for i in range(m.lanes(esize)):
            x = sext(a[i] >> n if top else a[i], n)
            y = sext(b[i] >> n if top else b[i], n)
            p = 2 * x * y
            out.append(min(max(p, -(1 << (esize - 1))), (1 << (esize - 1)) - 1) & mask(esize))
        m.zset(0, esize, out)
    return model


def sqdmlalb(subtract, esize):
    n = esize // 2

    def model(m):
        a, b, acc = m.zget(1, esize), m.zget(2, esize), m.zget(0, esize)
        out = []
        for i in range(m.lanes(esize)):
            x, y = sext(a[i], n), sext(b[i], n)
            p = min(max(2 * x * y, -(1 << (esize - 1))), (1 << (esize - 1)) - 1)
            t = sext(acc[i], esize) + (-p if subtract else p)
            out.append(min(max(t, -(1 << (esize - 1))), (1 << (esize - 1)) - 1) & mask(esize))
        m.zset(0, esize, out)
    return model


def narrow_shift(esize, shift, rounding, signed_src, saturate, top):
    """sqshrnb/sqrshrnb/uqshrnb/sqshrunb/shrnb ... saturate: None, 'signed', 'unsigned'."""
    n = esize // 2

    def model(m):
        src = m.zget(1, esize)
        dst = m.zget(0, esize)
        out = []
        for i in range(m.lanes(esize)):
            x = sext(src[i], esize) if signed_src else src[i]
            x = (x + ((1 << (shift - 1)) if rounding else 0)) >> shift
            if saturate == 'signed':
                x = min(max(x, -(1 << (n - 1))), (1 << (n - 1)) - 1)
            elif saturate == 'unsigned':
                x = min(max(x, 0), (1 << n) - 1)
            x &= mask(n)
            if top:
                x = (dst[i] & mask(n)) | (x << n)
            out.append(x)
        m.zset(0, esize, out)
    return model


def high_narrow(esize, subtract, rounding, top):
    n = esize // 2

    def model(m):
        a, b, dst = m.zget(1, esize), m.zget(2, esize), m.zget(0, esize)
        out = []
        for i in range(m.lanes(esize)):
            t = (a[i] - b[i]) if subtract else (a[i] + b[i])
            if rounding:
                t += 1 << (n - 1)
            x = (t & mask(esize)) >> n
            out.append(((dst[i] & mask(n)) | (x << n)) if top else x)
        m.zset(0, esize, out)
    return model


def sshll(top, esize, shift, signed):
    n = esize // 2

    def model(m):
        a = m.zget(1, esize)
        out = []
        for i in range(m.lanes(esize)):
            x = (a[i] >> n) if top else (a[i] & mask(n))
            x = sext(x, n) if signed else x
            out.append((x << shift) & mask(esize))
        m.zset(0, esize, out)
    return model


def dot(first_signed, second_signed, ratio, esize, index=None):
    """sdot/udot/usdot: the sum of the products of ratio source elements per destination element."""
    sb = esize // ratio

    def model(m):
        a, b, acc = m.zget(1, sb), m.zget(2, sb), m.zget(0, esize)
        out = []
        for i in range(m.lanes(esize)):
            total = acc[i]
            seg = (i * esize) // 128
            for k in range(ratio):
                x = a[i * ratio + k]
                if index is None:
                    y = b[i * ratio + k]
                else:
                    y = b[seg * (128 // sb) + index * ratio + k]
                x = sext(x, sb) if first_signed else x
                y = sext(y, sb) if second_signed else y
                total += x * y
            out.append(total & mask(esize))
        m.zset(0, esize, out)
    return model


def saddwt(esize):
    n = esize // 2

    def model(m):
        a, b = m.zget(1, esize), m.zget(2, esize)
        m.zset(0, esize, [(a[i] + sext(b[i] >> n, n)) & mask(esize) for i in range(m.lanes(esize))])
    return model


def edge_setup(esize):
    def setup(m):
        for r in (1, 2, 0):
            m.zset(r, esize, signed_edges(esize, m.lanes(esize)))
        m.zset(2, esize, signed_edges(esize, m.lanes(esize))[2:] + signed_edges(esize, 2))
    return setup


def rows():
    r = []
    add_row(r, 'smmla z0.s, z1.b, z2.b', 'a', mmla(True, True))
    add_row(r, 'ummla z0.s, z1.b, z2.b', 'b', mmla(False, False))
    add_row(r, 'usmmla z0.s, z1.b, z2.b', 'c', mmla(False, True))
    add_row(r, 'pmullb z0.q, z1.d, z2.d', 'd', pmull_quad(0))
    add_row(r, 'pmullt z0.q, z1.d, z2.d', 'e', pmull_quad(1))
    add_row(r, 'sqdmullb z0.h, z1.b, z2.b', 'f', sqdmullb(0, 16), setup=edge_setup(16))
    add_row(r, 'sqdmullt z0.s, z1.h, z2.h', 'g', sqdmullb(1, 32), setup=edge_setup(32))
    add_row(r, 'sqdmlalb z0.d, z1.s, z2.s', 'h', sqdmlalb(False, 64), setup=edge_setup(64))
    add_row(r, 'sqdmlslb z0.s, z1.h, z2.h', 'i', sqdmlalb(True, 32), setup=edge_setup(32))
    add_row(r, 'sqshrnb z0.b, z1.h, #3', 'j', narrow_shift(16, 3, False, True, 'signed', False), setup=edge_setup(16))
    add_row(r, 'sqrshrnt z0.h, z1.s, #7', 'k', narrow_shift(32, 7, True, True, 'signed', True), setup=edge_setup(32))
    add_row(r, 'uqrshrnb z0.s, z1.d, #32', 'l', narrow_shift(64, 32, True, False, 'unsigned', False), setup=edge_setup(64))
    add_row(r, 'sqshrunt z0.b, z1.h, #1', 'm', narrow_shift(16, 1, False, True, 'unsigned', True), setup=edge_setup(16))
    add_row(r, 'rshrnb z0.h, z1.s, #16', 'n', narrow_shift(32, 16, True, False, None, False), setup=edge_setup(32))
    add_row(r, 'shrnt z0.s, z1.d, #1', 'o', narrow_shift(64, 1, False, False, None, True), setup=edge_setup(64))
    add_row(r, 'raddhnb z0.b, z1.h, z2.h', 'p', high_narrow(16, False, True, False), setup=edge_setup(16))
    add_row(r, 'rsubhnt z0.h, z1.s, z2.s', 'q', high_narrow(32, True, True, True), setup=edge_setup(32))
    add_row(r, 'sshllt z0.s, z1.h, #5', 's', sshll(1, 32, 5, True))
    add_row(r, 'ushllb z0.d, z1.s, #31', 't', sshll(0, 64, 31, False))
    add_row(r, 'sdot z0.s, z1.b, z2.b', 'u', dot(True, True, 4, 32))
    add_row(r, 'usdot z0.s, z1.b, z2.b[3]', 'v', dot(False, True, 4, 32, 3))
    add_row(r, 'sdot z0.d, z1.h, z2.h[1]', 'w', dot(True, True, 4, 64, 1))
    add_row(r, 'saddwt z0.h, z1.h, z2.b', 'x', saddwt(16))
    return r
