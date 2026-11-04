"""The model of the SVE2.1 quadword, miscellaneous and cryptographic instructions of
Arm64SemanticLiftSveQuad.cpp, Arm64SemanticLiftSveMisc.cpp and Arm64SemanticLiftSveCrypto.cpp, written from
the pseudocode of the Arm ARM (see sve_model.py). The AES and SM4 primitives are computed here from their
definitions (the AES S-box from the inverse in GF(2^8), SM4 checked against the standard test vector)."""
import random
import zlib
import sve_model as M
from sve_model import Machine, Row, mask, sext, f32, f32_bits, f32_from_bits, f64_bits, f64_from_bits

VLS = (128, 256, 512)


def machine(vl, seed):
    m = M.random_machine(vl, seed)
    M.memory_pattern(m, 0x4000, 0x2000, 3)
    m.x[4] = 0x4100
    m.x[5] = 3
    return m


def add_row(rows, text, seed, model, vls=VLS, setup=None):
    for vl in vls:
        m = machine(vl, text + seed)
        if setup:
            setup(m)
        rows.append(Row(text, vl, m, model))


def segments(m):
    return m.vl // 128


def set_floats(m, regs, esize, rng_seed):
    """z registers of finite floats (no NaN, no infinity)."""
    rng = random.Random(zlib.crc32(rng_seed.encode()))
    for n in regs:
        vals = []
        for _ in range(m.lanes(esize)):
            v = float(rng.randint(-100, 100))
            vals.append(f32_bits(v) if esize == 32 else f64_bits(v))
        m.zset(n, esize, vals)


# ---------------------------------------------------------------- reductions of the segments

def qv(esize, combine, identity):
    def model(m):
        z = m.zget(2, esize)
        n = m.lanes(esize)
        sl = 128 // esize
        out = []
        for j in range(sl):
            acc = None
            for seg in range(segments(m)):
                lane = seg * sl + j
                v = z[lane] if m.active(m.p[1], lane, esize) else identity
                acc = v if acc is None else combine(acc, v)
            out.append(acc)
        m.zset(0, esize, out + [0] * (n - sl))
    return model


def fqv(esize, combine, identity):
    """Floating point reduction: a tree of adjacent pairs over the segments."""
    to = f32_from_bits if esize == 32 else f64_from_bits
    frm = f32_bits if esize == 32 else f64_bits
    rnd = f32 if esize == 32 else (lambda v: v)

    def model(m):
        z = m.zget(2, esize)
        n = m.lanes(esize)
        sl = 128 // esize
        out = []
        for j in range(sl):
            vals = [to(z[seg * sl + j]) if m.active(m.p[1], seg * sl + j, esize) else identity
                    for seg in range(segments(m))]
            while len(vals) > 1:
                vals = [rnd(combine(vals[i], vals[i + 1])) for i in range(0, len(vals), 2)]
            r = vals[0]
            out.append(frm(f32(r) if esize == 32 else r))
        m.zset(0, esize, out + [0] * (n - sl))
    return model


# ---------------------------------------------------------------- permutes

def per_segment(esize, fn):
    def model(m):
        sl = 128 // esize
        a, b, d = m.zget(1, esize), m.zget(2, esize), m.zget(0, esize)
        out = []
        for seg in range(segments(m)):
            sa, sb, sd = a[seg * sl:(seg + 1) * sl], b[seg * sl:(seg + 1) * sl], d[seg * sl:(seg + 1) * sl]
            out += fn(sa, sb, sd, sl)
        m.zset(0, esize, out)
    return model


def structure_load(members, esize=128):
    def model(m):
        n = m.lanes(128)
        base = m.x[4] - members * (m.vl // 8)
        regs = [[0] * n for _ in range(members)]
        for i in range(n):
            if m.active(m.p[1], i, 128):
                for k in range(members):
                    regs[k][i] = m.read(base + (i * members + k) * 16, 16)
        for k in range(members):
            m.zset(k, 128, regs[k])
    return model


def structure_store(members):
    def model(m):
        n = m.lanes(128)
        base = m.x[4] + (m.x[5] << 4)
        for i in range(n):
            if m.active(m.p[1], i, 128):
                for k in range(members):
                    m.write(base + (i * members + k) * 16, 16, m.zget(k, 128)[i])
    return model


# ---------------------------------------------------------------- AES

def gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11b
        b >>= 1
    return r


def _sbox():
    inverse = [0] * 256
    for x in range(1, 256):
        for y in range(1, 256):
            if gmul(x, y) == 1:
                inverse[x] = y
                break
    box = []
    for x in range(256):
        b = inverse[x]
        r = b
        for i in range(1, 5):
            r ^= ((b << i) | (b >> (8 - i))) & 0xff
        box.append(r ^ 0x63)
    return box


AES_SBOX = _sbox()
AES_INV_SBOX = [0] * 256
for _i, _v in enumerate(AES_SBOX):
    AES_INV_SBOX[_v] = _i
assert AES_SBOX[0] == 0x63 and AES_SBOX[0x53] == 0xed


def aes_sub_shift(block, inverse=False):
    s = [(block >> (8 * i)) & 0xff for i in range(16)]
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            src = (c + r) % 4 if not inverse else (c - r) % 4
            v = s[r + 4 * src]
            out[r + 4 * c] = AES_INV_SBOX[v] if inverse else AES_SBOX[v]
    return sum(v << (8 * i) for i, v in enumerate(out))


def aes_mix(block, inverse=False):
    s = [(block >> (8 * i)) & 0xff for i in range(16)]
    f = (14, 11, 13, 9) if inverse else (2, 3, 1, 1)
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            v = 0
            for k in range(4):
                v ^= gmul(s[k + 4 * c], f[(k - r) % 4])
            out[r + 4 * c] = v
    return sum(v << (8 * i) for i, v in enumerate(out))


def aes_round(kind, state, key):
    x = state ^ key
    if kind == 'e':
        return aes_sub_shift(x)
    if kind == 'd':
        return aes_sub_shift(x, True)
    if kind == 'em':
        return aes_mix(aes_sub_shift(x))
    return aes_mix(aes_sub_shift(x, True), True)


def seg128(value, i):
    return (value >> (128 * i)) & mask(128)


def aes_vec(kind, mixonly=None):
    def model(m):
        out = 0
        for i in range(segments(m)):
            if mixonly is not None:
                r = aes_mix(seg128(m.z[0], i), mixonly) if False else aes_mix(seg128(m.z[1], i), mixonly)
            else:
                r = aes_round(kind, seg128(m.z[0], i), seg128(m.z[2], i))
            out |= r << (128 * i)
        m.z[0] = out
    return model


def aes_multi(kind, count, key_reg, index):
    def model(m):
        key = seg128(m.z[key_reg], index) if index < segments(m) else 0
        regs = [m.z[r] for r in range(count)]
        for r in range(count):
            out = 0
            for i in range(segments(m)):
                out |= aes_round(kind, seg128(regs[r], i), key) << (128 * i)
            m.z[r] = out
    return model


# ---------------------------------------------------------------- SM4

SM4_SBOX = bytes.fromhex(
    'd690e9fecce13db716b614c228fb2c05' '2b679a762abe04c3aa44132649860699'
    '9c4250f491ef987a33540b43edcfac62' 'e4b31ca9c908e89580df94fa758f3fa6'
    '4707a7fcf37317ba83593c19e6854fa8' '686b81b27164da8bf8eb0f4b70569d35'
    '1e240e5e6358d1a225227c3b01217887' 'd40046579fd327524c3602e7a0c4c89e'
    'eabf8ad240c738b5a3f7f2cef96115a1' 'e0ae5da49b341a55ad933230f58cb1e3'
    '1df6e22e8266ca60c02923ab0d534e6f' 'd5db3745defd8e2f03ff6a726d6c5b51'
    '8d1baf92bbddbc7f11d95c411f105ad8' '0ac131 88a5cd7bbd2d74d012b8e5b4b0'.replace(' ', '') +
    '8969974a0c96777e65b9f109c56ec684' '18f07dec3adc4d2079ee5f3ed7cb3948')


def rol32(x, n):
    x &= 0xffffffff
    return ((x << n) | (x >> (32 - n))) & 0xffffffff


def sm4_tau(x):
    return sum(SM4_SBOX[(x >> (8 * i)) & 0xff] << (8 * i) for i in range(4))


def sm4_round_words(w, rk, key_schedule):
    """4 rounds of SM4E (or SM4EKEY) on the four words w, with the words rk."""
    w = list(w)
    for i in range(4):
        t = sm4_tau(w[1] ^ w[2] ^ w[3] ^ rk[i])
        if key_schedule:
            t = t ^ rol32(t, 13) ^ rol32(t, 23)
        else:
            t = t ^ rol32(t, 2) ^ rol32(t, 10) ^ rol32(t, 18) ^ rol32(t, 24)
        w = [w[1], w[2], w[3], w[0] ^ t]
    return w


def _check_sm4():
    """The standard test vector of SM4: the key and the plaintext 0123456789abcdeffedcba9876543210,
    the ciphertext 681edf34d206965e86b3e94f536e4246, through the model of SM4EKEY and SM4E."""
    key = [0x01234567, 0x89abcdef, 0xfedcba98, 0x76543210]
    fk = [0xa3b1bac6, 0x56aa3350, 0x677d9197, 0xb27022dc]
    ck = [sum((((4 * i + j) * 7) % 256) << (8 * (3 - j)) for j in range(4)) for i in range(32)]
    state = [key[i] ^ fk[i] for i in range(4)]
    rks = []
    for g in range(8):
        state = sm4_round_words(state, ck[4 * g:4 * g + 4], True)
        rks.append(list(state))
    x = list(key)
    for g in range(8):
        x = sm4_round_words(x, rks[g], False)
    cipher = ''.join('%08x' % v for v in reversed(x))
    assert cipher == '681edf34d206965e86b3e94f536e4246', cipher


_check_sm4()


def sm4_vec(key_schedule):
    def model(m):
        out = 0
        for i in range(segments(m)):
            if key_schedule:
                st = seg128(m.z[1], i)
            else:
                st = seg128(m.z[0], i)
            kk = seg128(m.z[2], i)
            w = [(st >> (32 * j)) & 0xffffffff for j in range(4)]
            rk = [(kk >> (32 * j)) & 0xffffffff for j in range(4)]
            w = sm4_round_words(w, rk, key_schedule)
            out |= sum(w[j] << (32 * j) for j in range(4)) << (128 * i)
        m.z[0] = out
    return model


# ---------------------------------------------------------------- the rows

def rows():
    r = []
    # reductions
    add_row(r, 'addqv v0.16b, p1, z2.b', 'q1', qv(8, lambda a, b: (a + b) & 0xff, 0))
    add_row(r, 'addqv v0.2d, p1, z2.d', 'q2', qv(64, lambda a, b: (a + b) & mask(64), 0))
    add_row(r, 'smaxqv v0.4s, p1, z2.s', 'q3',
            qv(32, lambda a, b: a if sext(a, 32) > sext(b, 32) else b, 1 << 31))
    add_row(r, 'uminqv v0.8h, p1, z2.h', 'q4', qv(16, min, 0xffff))
    add_row(r, 'sminqv v0.16b, p1, z2.b', 'q4b', qv(8, lambda a, b: a if sext(a, 8) < sext(b, 8) else b, 0x7f))
    add_row(r, 'umaxqv v0.4s, p1, z2.s', 'q4c', qv(32, max, 0))
    add_row(r, 'orqv v0.2d, p1, z2.d', 'q5', qv(64, lambda a, b: a | b, 0))
    add_row(r, 'eorqv v0.16b, p1, z2.b', 'q6', qv(8, lambda a, b: a ^ b, 0))
    add_row(r, 'andqv v0.4s, p1, z2.s', 'q7', qv(32, lambda a, b: a & b, mask(32)))

    def fsetup(esize):
        return lambda m: set_floats(m, [2], esize, 'fq')
    add_row(r, 'faddqv v0.4s, p1, z2.s', 'f1', fqv(32, lambda a, b: a + b, 0.0), setup=fsetup(32))
    add_row(r, 'fmaxqv v0.2d, p1, z2.d', 'f2', fqv(64, max, float('-inf')), setup=fsetup(64))
    def minnum(a, b):
        if a != a:
            return b
        if b != b:
            return a
        return min(a, b)
    add_row(r, 'fminnmqv v0.4s, p1, z2.s', 'f3', fqv(32, minnum, float('nan')), setup=fsetup(32))

    # permutes
    add_row(r, 'dupq z0.s, z1.s[2]', 'dq', per_segment(32, lambda a, b, d, n: [a[2]] * n))
    add_row(r, 'dupq z0.b, z1.b[9]', 'dq2', per_segment(8, lambda a, b, d, n: [a[9]] * n))

    def extq(m):
        a, b = m.zget(0, 8), m.zget(2, 8)
        out = []
        for seg in range(segments(m)):
            cat = a[seg * 16:seg * 16 + 16] + b[seg * 16:seg * 16 + 16]
            out += cat[5:5 + 16]
        m.zset(0, 8, out)
    add_row(r, 'extq z0.b, z0.b, z2.b, #5', 'eq', extq)

    def tblq_fn(a, b, d, n):
        return [a[i] if i < n else 0 for i in b]
    def tblq(m):
        t, idx = m.zget(1, 16), m.zget(2, 16)
        out = []
        for seg in range(segments(m)):
            st, si = t[seg * 8:seg * 8 + 8], idx[seg * 8:seg * 8 + 8]
            out += [st[i] if i < 8 else 0 for i in si]
        m.zset(0, 16, out)
    add_row(r, 'tblq z0.h, {z1.h}, z2.h', 'tq', tblq,
            setup=lambda m: m.zset(2, 16, [(i * 3) % 11 for i in range(m.lanes(16))]))

    def tbxq(m):
        t, idx, d = m.zget(1, 32), m.zget(2, 32), m.zget(0, 32)
        out = []
        for seg in range(segments(m)):
            st, si, sd = t[seg * 4:seg * 4 + 4], idx[seg * 4:seg * 4 + 4], d[seg * 4:seg * 4 + 4]
            out += [st[i] if i < 4 else sd[k] for k, i in enumerate(si)]
        m.zset(0, 32, out)
    add_row(r, 'tbxq z0.s, z1.s, z2.s', 'xq', tbxq,
            setup=lambda m: m.zset(2, 32, [(i * 5) % 7 for i in range(m.lanes(32))]))

    add_row(r, 'zipq1 z0.s, z1.s, z2.s', 'zq1',
            per_segment(32, lambda a, b, d, n: [x for i in range(n // 2) for x in (a[i], b[i])]))
    add_row(r, 'zipq2 z0.h, z1.h, z2.h', 'zq2',
            per_segment(16, lambda a, b, d, n: [x for i in range(n // 2, n) for x in (a[i], b[i])]))
    add_row(r, 'uzpq1 z0.b, z1.b, z2.b', 'uq1',
            per_segment(8, lambda a, b, d, n: (a + b)[0::2]))
    add_row(r, 'uzpq2 z0.d, z1.d, z2.d', 'uq2',
            per_segment(64, lambda a, b, d, n: (a + b)[1::2]))

    # structures of quadwords
    add_row(r, 'ld2q {z0.q, z1.q}, p1/z, [x4, #-2, mul vl]', 'l2q', structure_load(2),
            setup=lambda m: m.x.__setitem__(4, 0x4100 + 2 * (m.vl // 8)))
    add_row(r, 'ld3q {z0.q - z2.q}, p1/z, [x4, #-3, mul vl]', 'l3q', structure_load(3),
            setup=lambda m: m.x.__setitem__(4, 0x4100 + 3 * (m.vl // 8)))
    add_row(r, 'ld4q {z0.q - z3.q}, p1/z, [x4, #-4, mul vl]', 'l4q', structure_load(4),
            setup=lambda m: m.x.__setitem__(4, 0x4100 + 4 * (m.vl // 8)))
    def store_setup(m):
        # every stored byte differs from the memory (the test counts a write of the same value as stray)
        for a in range(0x4000, 0x6000):
            m.mem[a] = 0xaa
        for n in range(5):
            v = 0
            for i in range(m.vl // 8):
                b = (m.z[n] >> (8 * i)) & 0xff
                v |= (b if b != 0xaa else 0x55) << (8 * i)
            m.z[n] = v
    add_row(r, 'st2q {z0.q, z1.q}, p1, [x4, x5, lsl #4]', 's2q', structure_store(2), setup=store_setup)
    add_row(r, 'st3q {z0.q - z2.q}, p1, [x4, x5, lsl #4]', 's3q', structure_store(3), setup=store_setup)
    add_row(r, 'st4q {z0.q - z3.q}, p1, [x4, x5, lsl #4]', 's4q', structure_store(4), setup=store_setup)

    # misc
    def pmov_to(m):
        esize, idx = 16, 1
        elements = m.lanes(esize)
        bits = (m.z[28] >> (idx * elements)) & mask(elements)
        m.pset_elems(3, esize, [(bits >> i) & 1 for i in range(elements)])
    add_row(r, 'pmov p3.h, z28[1]', 'pm1', pmov_to)

    def pmov_to_b(m):
        m.p[3] = m.z[28] & mask(m.vl // 8)
    add_row(r, 'pmov p3.b, z28', 'pm2', pmov_to_b)

    def pmov_from(m):
        esize, idx = 32, 2
        elements = m.lanes(esize)
        bits = sum(m.active(m.p[3], i, esize) << i for i in range(elements))
        m.z[28] = (m.z[28] & ~(mask(elements) << (idx * elements))) | (bits << (idx * elements))
    add_row(r, 'pmov z28[2], p3.s', 'pm3', pmov_from)

    def expand(m):
        a = m.zget(1, 16)
        out, k = [], 0
        for i in range(m.lanes(16)):
            if m.active(m.p[2], i, 16):
                out.append(a[k])
                k += 1
            else:
                out.append(0)
        m.zset(0, 16, out)
    add_row(r, 'expand z0.h, p2, z1.h', 'ex', expand)

    def revd(zeroing):
        def model(m):
            a, d = m.zget(1, 128), m.zget(0, 128)
            out = []
            for i in range(m.lanes(128)):
                if m.active(m.p[2], i, 128):
                    out.append(((a[i] & mask(64)) << 64) | (a[i] >> 64))
                else:
                    out.append(0 if zeroing else d[i])
            m.zset(0, 128, out)
        return model
    add_row(r, 'revd z0.q, p2/z, z1.q', 'rd1', revd(True))

    add_row(r, 'addpt z0.d, p1/m, z0.d, z2.d', 'cp1', binary64(lambda a, b: (a + b) & mask(64)))
    add_row(r, 'subpt z0.d, p1/m, z0.d, z2.d', 'cp2', binary64(lambda a, b: (a - b) & mask(64)))

    def addpt_unpredicated(m):
        a, b = m.zget(1, 64), m.zget(2, 64)
        m.zset(0, 64, [(x + y) & mask(64) for x, y in zip(a, b)])
    add_row(r, 'addpt z0.d, z1.d, z2.d', 'cp3', addpt_unpredicated)

    def madpt(m):
        d, a, b = m.zget(0, 64), m.zget(2, 64), m.zget(1, 64)
        m.zset(0, 64, [(c + x * y) & mask(64) for x, y, c in zip(d, b, a)])
    add_row(r, 'madpt z0.d, z1.d, z2.d', 'cp4', madpt)

    def mlapt(m):
        d, a, b = m.zget(0, 64), m.zget(1, 64), m.zget(2, 64)
        m.zset(0, 64, [(c + x * y) & mask(64) for x, y, c in zip(a, b, d)])
    add_row(r, 'mlapt z0.d, z1.d, z2.d', 'cp5', mlapt)

    # crypto
    add_row(r, 'aese z0.b, z0.b, z2.b', 'ae', aes_vec('e'))
    add_row(r, 'aesd z0.b, z0.b, z2.b', 'ad', aes_vec('d'))

    def mixonly(inverse):
        def model(m):
            out = 0
            for i in range(segments(m)):
                out |= aes_mix(seg128(m.z[0], i), inverse) << (128 * i)
            m.z[0] = out
        return model
    add_row(r, 'aesmc z0.b, z0.b', 'am', mixonly(False))
    add_row(r, 'aesimc z0.b, z0.b', 'ai', mixonly(True))
    add_row(r, 'aese {z0.b, z1.b}, {z0.b, z1.b}, z2.q[0]', 'a2e', aes_multi('e', 2, 2, 0))
    add_row(r, 'aesd {z0.b - z3.b}, {z0.b - z3.b}, z4.q[1]', 'a2d', aes_multi('d', 4, 4, 1), vls=(256, 512))
    add_row(r, 'aesemc {z0.b, z1.b}, {z0.b, z1.b}, z2.q[0]', 'a2em', aes_multi('em', 2, 2, 0))
    add_row(r, 'aesdimc {z0.b, z1.b}, {z0.b, z1.b}, z2.q[0]', 'a2di', aes_multi('di', 2, 2, 0))

    def rax1(m):
        a, b = m.zget(1, 64), m.zget(2, 64)
        m.zset(0, 64, [x ^ (((y << 1) | (y >> 63)) & mask(64)) for x, y in zip(a, b)])
    add_row(r, 'rax1 z0.d, z1.d, z2.d', 'rx', rax1)
    add_row(r, 'sm4e z0.s, z0.s, z2.s', 's4', sm4_vec(False))
    add_row(r, 'sm4ekey z0.s, z1.s, z2.s', 's4k', sm4_vec(True))

    # the standard test vector through the instructions
    return r


def binary64(fn):
    def model(m):
        a, b = m.zget(0, 64), m.zget(2, 64)
        m.zset(0, 64, [fn(a[i], b[i]) if m.active(m.p[1], i, 64) else a[i] for i in range(m.lanes(64))])
    return model
