#!/usr/bin/env python3
"""Generate tests/arm/Arm64SimdSemanticRowsCrypto.inc (ARM64_SIMD_ROWS_CRYPTO), the unit rows of the A64
cryptographic instructions: AES, SHA1, SHA256, SHA512, SHA3 (eor3 bcax rax1 xar), SM3, SM4 and pmull.

The expected values come from an independent Python model of the Arm ARM pseudocode of each instruction. The
models are checked here, before the rows are written, by composing them into the real algorithms and comparing
with references that are not part of this repository:
  * AES: the FIPS-197 round of appendix C.1 (state after SubBytes / ShiftRows / MixColumns), the S-box and the
    field arithmetic are computed (GF(2^8) inverse and the affine map), not tabulated;
  * SHA-1, SHA-256, SHA-512: the compression of one block ('abc') with the instruction sequences of the
    architecture's crypto extension equals hashlib; the round constants and the initial values are computed from
    the cube / square roots of the primes;
  * SM3: the compression of 'abc' with sm3ss1 / sm3tt1* / sm3tt2* / sm3partw* equals the digest of the standard
    (66c7f0f4 62eeedd9 d1f2d46b dc10e4e2 4167c487 5cf2f7a2 297da02b 8f4ba8e0);
  * SM4: the key schedule (sm4ekey) and the 32 rounds (sm4e) encrypt like `openssl enc -sm4-ecb`.
The registers are little endian: the lane 0 / byte 0 of a vector is the lowest part of the value.

    gen_simd_semantic_rows_crypto.py
"""
import hashlib
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib
from simd_rows_common import Rows, lane, set_lane, mask, M32, M64, M128

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'Arm64SimdSemanticRowsCrypto.inc')
LIFTER = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64SemanticLiftCrypto.cpp')


def rol32(x, n):
    return ((x << n) | (x >> (32 - n))) & M32 if n else x


def ror32(x, n):
    return rol32(x, 32 - n)


def rol64(x, n):
    return ((x << n) | (x >> (64 - n))) & M64 if n else x


def ror64(x, n):
    return rol64(x, 64 - n)


def words(v, n=4, bits=32):
    return [lane(v, i, bits) for i in range(n)]


def pack(ws, bits=32):
    v = 0
    for i, w in enumerate(ws):
        v |= (w & mask(bits)) << (i * bits)
    return v


# ---------------------------------------------------------------------------------------------------
# AES (FIPS-197)
# ---------------------------------------------------------------------------------------------------
def gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11B
        b >>= 1
    return r


def ginv(a):
    if a == 0:
        return 0
    return next(b for b in range(1, 256) if gmul(a, b) == 1)


def sub(a):
    x = ginv(a)
    r = 0x63
    for i in range(5):
        r ^= ((x << i) | (x >> (8 - i))) & 0xFF
    return r


SBOX = [sub(i) for i in range(256)]
INV_SBOX = [SBOX.index(i) for i in range(256)]


def shift_rows(s, inverse=False):
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            src = (c + r) % 4 if not inverse else (c - r) % 4
            out[r + 4 * c] = s[r + 4 * src]
    return out


def mix_columns(s, inverse=False):
    matrix = [[2, 3, 1, 1], [1, 2, 3, 1], [1, 1, 2, 3], [3, 1, 1, 2]] if not inverse else \
             [[14, 11, 13, 9], [9, 14, 11, 13], [13, 9, 14, 11], [11, 13, 9, 14]]
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            v = 0
            for k in range(4):
                v ^= gmul(matrix[r][k], s[4 * c + k])
            out[4 * c + r] = v
    return out


def to_bytes(v):
    return list(v.to_bytes(16, 'little'))


def from_bytes(b):
    return int.from_bytes(bytes(b), 'little')


def aese(d, n):
    s = to_bytes(d ^ n)
    return from_bytes(shift_rows([SBOX[x] for x in s]))


def aesd(d, n):
    s = to_bytes(d ^ n)
    return from_bytes([INV_SBOX[x] for x in shift_rows(s, True)])


def aesmc(n):
    return from_bytes(mix_columns(to_bytes(n)))


def aesimc(n):
    return from_bytes(mix_columns(to_bytes(n), True))


# FIPS-197 C.1: round 1 of the example cipher
_plain = bytes.fromhex('00112233445566778899aabbccddeeff')
_key = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
assert bytes(to_bytes(aese(from_bytes(list(_plain)), from_bytes(list(_key))))).hex() == '6353e08c0960e104cd70b751bacad0e7'
assert bytes(to_bytes(aesmc(from_bytes(list(bytes.fromhex('6353e08c0960e104cd70b751bacad0e7')))))).hex() == \
    '5f72641557f5bc92f7be3b291db9f91a'
assert aesimc(aesmc(0x0123456789abcdef0011223344556677)) == 0x0123456789abcdef0011223344556677


# ---------------------------------------------------------------------------------------------------
# SHA1
# ---------------------------------------------------------------------------------------------------
def choose(x, y, z):
    return (x & y) | (~x & M32 & z)


def majority(x, y, z):
    return (x & y) | (x & z) | (y & z)


def sha1_hash(op, d, e, w):
    x = words(d)
    y = e
    wv = words(w)
    for i in range(4):
        if op == 'c':
            t = choose(x[1], x[2], x[3])
        elif op == 'm':
            t = majority(x[1], x[2], x[3])
        else:
            t = x[1] ^ x[2] ^ x[3]
        y = (y + rol32(x[0], 5) + t + wv[i]) & M32
        x[1] = rol32(x[1], 30)
        # <Y, X> = ROL(Y:X, 32)
        top = x[3]
        x = [y, x[0], x[1], x[2]]
        y = top
    return pack(x)


def sha1h(n):
    return rol32(n & M32, 30)


def sha1su0(d, n, m):
    t = ((n & M64) << 64) | (d >> 64)
    return t ^ d ^ m


def sha1su1(d, n):
    t = d ^ (n >> 32)
    tw = words(t)
    r = [rol32(tw[0], 1), rol32(tw[1], 1), rol32(tw[2], 1), rol32(tw[3], 1) ^ rol32(tw[0], 2)]
    return pack(r)


def sha1_compress_test():
    block = bytearray(b'abc') + b'\x80' + bytes(52) + (24).to_bytes(8, 'big')
    w = [int.from_bytes(block[4 * i:4 * i + 4], 'big') for i in range(16)]
    K = [0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6]
    H = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]
    abcd = pack(H[:4])
    e0 = H[4]
    wv = [pack(w[4 * i:4 * i + 4]) for i in range(4)]
    for g in range(20):
        if g >= 4:
            wv.append(sha1su1(sha1su0(wv[g - 4], wv[g - 3], wv[g - 2]), wv[g - 1]))
        k = K[g // 5]
        wk = pack([(x + k) & M32 for x in words(wv[g])])
        e1 = sha1h(words(abcd)[0])
        abcd = sha1_hash('cpmp'[g // 5], abcd, e0, wk)
        e0 = e1
    out = [(a + b) & M32 for a, b in zip(words(abcd), H[:4])] + [(e0 + H[4]) & M32]
    digest = b''.join(x.to_bytes(4, 'big') for x in out)
    assert digest == hashlib.sha1(b'abc').digest(), 'sha1 model'


sha1_compress_test()


# ---------------------------------------------------------------------------------------------------
# SHA256
# ---------------------------------------------------------------------------------------------------
def big0(x): return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22)
def big1(x): return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25)
def small0(x): return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3)
def small1(x): return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10)


def sha256_hash(x, y, w, part1):
    xs, ys, ws = words(x), words(y), words(w)
    for e in range(4):
        chs = choose(ys[0], ys[1], ys[2])
        maj = majority(xs[0], xs[1], xs[2])
        t = (ys[3] + big1(ys[0]) + chs + ws[e]) & M32
        xs[3] = (t + xs[3]) & M32
        ys[3] = (t + big0(xs[0]) + maj) & M32
        full = xs + ys                        # X low .. X high, Y low .. Y high (8 words)
        full = [full[7]] + full[:7]           # rotate left by 32 bits: the top word comes to the bottom
        xs, ys = full[:4], full[4:]
    return pack(xs if part1 else ys)


def sha256h(d, n, m): return sha256_hash(d, n, m, True)
def sha256h2(d, n, m): return sha256_hash(n, d, m, False)


def sha256su0(d, n):
    dw = words(d)
    t = dw[1:] + [words(n)[0]]
    return pack([(small0(t[i]) + dw[i]) & M32 for i in range(4)])


def sha256su1(d, n, m):
    dw, nw, mw = words(d), words(n), words(m)
    t0 = nw[1:] + [mw[0]]
    r0 = (small1(mw[2]) + dw[0] + t0[0]) & M32
    r1 = (small1(mw[3]) + dw[1] + t0[1]) & M32
    r2 = (small1(r0) + dw[2] + t0[2]) & M32
    r3 = (small1(r1) + dw[3] + t0[3]) & M32
    return pack([r0, r1, r2, r3])


def primes(count):
    out = []
    n = 2
    while len(out) < count:
        if all(n % p for p in out):
            out.append(n)
        n += 1
    return out


def iroot(x, k):
    lo, hi = 0, 1 << ((x.bit_length() // k) + 2)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if mid ** k <= x:
            lo = mid
        else:
            hi = mid - 1
    return lo


def frac_root(p, k, bits):
    return iroot(p << (k * bits), k) & mask(bits)


K256 = [frac_root(p, 3, 32) for p in primes(64)]
H256 = [frac_root(p, 2, 32) for p in primes(8)]
K512 = [frac_root(p, 3, 64) for p in primes(80)]
H512 = [frac_root(p, 2, 64) for p in primes(8)]


def sha256_compress_test():
    block = bytearray(b'abc') + b'\x80' + bytes(52) + (24).to_bytes(8, 'big')
    w = [int.from_bytes(block[4 * i:4 * i + 4], 'big') for i in range(16)]
    abcd, efgh = pack(H256[:4]), pack(H256[4:])
    wv = [pack(w[4 * i:4 * i + 4]) for i in range(4)]
    for g in range(16):
        if g >= 4:
            wv.append(sha256su1(sha256su0(wv[g - 4], wv[g - 3]), wv[g - 2], wv[g - 1]))
        wk = pack([(x + k) & M32 for x, k in zip(words(wv[g]), K256[4 * g:4 * g + 4])])
        tmp = abcd
        abcd = sha256h(abcd, efgh, wk)
        efgh = sha256h2(efgh, tmp, wk)
    out = [(a + b) & M32 for a, b in zip(words(abcd) + words(efgh), H256)]
    assert b''.join(x.to_bytes(4, 'big') for x in out) == hashlib.sha256(b'abc').digest(), 'sha256 model'


sha256_compress_test()


# ---------------------------------------------------------------------------------------------------
# SHA512
# ---------------------------------------------------------------------------------------------------
def S0_512(x): return ror64(x, 28) ^ ror64(x, 34) ^ ror64(x, 39)
def S1_512(x): return ror64(x, 14) ^ ror64(x, 18) ^ ror64(x, 41)
def s0_512(x): return ror64(x, 1) ^ ror64(x, 8) ^ (x >> 7)
def s1_512(x): return ror64(x, 19) ^ ror64(x, 61) ^ (x >> 6)


def ch64(x, y, z): return (x & y) | (~x & M64 & z)
def maj64(x, y, z): return (x & y) | (x & z) | (y & z)


def sha512h(d, n, m):
    d0, d1 = d & M64, d >> 64
    n0, n1 = n & M64, n >> 64
    m0, m1 = m & M64, m >> 64
    d1 = (d1 + S1_512(m1) + ch64(m1, n0, n1)) & M64
    d0 = (d0 + S1_512((d1 + m0) & M64) + ch64((d1 + m0) & M64, m1, n0)) & M64
    return (d1 << 64) | d0


def sha512h2(d, n, m):
    d0, d1 = d & M64, d >> 64
    n0 = n & M64
    m0, m1 = m & M64, m >> 64
    d1 = (d1 + S0_512(m0) + maj64(n0, m1, m0)) & M64
    d0 = (d0 + S0_512(d1) + maj64(d1, m0, m1)) & M64
    return (d1 << 64) | d0


def sha512su0(d, n):
    d0, d1 = d & M64, d >> 64
    return (((d1 + s0_512(n & M64)) & M64) << 64) | ((d0 + s0_512(d1)) & M64)


def sha512su1(d, n, m):
    d0, d1 = d & M64, d >> 64
    n0, n1 = n & M64, n >> 64
    m0, m1 = m & M64, m >> 64
    return (((d1 + s1_512(n1) + m1) & M64) << 64) | ((d0 + s1_512(n0) + m0) & M64)


def sha512_compress_test():
    block = bytearray(b'abc') + b'\x80' + bytes(112) + (24).to_bytes(8, 'big')
    w = [int.from_bytes(block[8 * i:8 * i + 8], 'big') for i in range(16)]
    for t in range(16, 80):
        w.append((s1_512(w[t - 2]) + w[t - 7] + s0_512(w[t - 15]) + w[t - 16]) & M64)
    # the schedule two words at a time with sha512su0 / sha512su1: W[2i+1]:W[2i]
    ws = [(w[2 * i + 1] << 64) | w[2 * i] for i in range(8)]
    for i in range(8, 40):
        t = sha512su0(ws[i - 8], ws[i - 7])
        # n = W[2i-2], W[2i-1]; m = W[2i-7], W[2i-6]
        m = ((ws[i - 3] & M64) << 64) | (ws[i - 4] >> 64)
        ws.append(sha512su1(t, ws[i - 1], m))
    for i in range(80):
        assert (ws[i // 2] >> (64 * (i & 1))) & M64 == w[i], 'sha512 schedule'
    # two rounds at a time: sha512h gives T1 of both rounds (the high word is round t), sha512h2 adds T2
    a, b, c, d, e, f, g, h = H512
    for p in range(40):
        kw0 = (w[2 * p] + K512[2 * p]) & M64
        kw1 = (w[2 * p + 1] + K512[2 * p + 1]) & M64
        dd = (((h + kw0) & M64) << 64) | ((g + kw1) & M64)
        t1 = sha512h(dd, (g << 64) | f, (e << 64) | d)
        full = sha512h2(t1, c, (b << 64) | a)
        e1 = (d + (t1 >> 64)) & M64
        e2 = (c + (t1 & M64)) & M64
        a, b, c, d, e, f, g, h = full & M64, full >> 64, a, b, e2, e1, e, f
    out = [(x + y) & M64 for x, y in zip([a, b, c, d, e, f, g, h], H512)]
    assert b''.join(x.to_bytes(8, 'big') for x in out) == hashlib.sha512(b'abc').digest(), 'sha512 model'


sha512_compress_test()


# ---------------------------------------------------------------------------------------------------
# SM3
# ---------------------------------------------------------------------------------------------------
def p1(x): return x ^ rol32(x, 15) ^ rol32(x, 23)
def p0(x): return x ^ rol32(x, 9) ^ rol32(x, 17)


def sm3ss1(n, m, a):
    return pack([0, 0, 0, rol32((rol32(words(n)[3], 12) + words(m)[3] + words(a)[3]) & M32, 7)])


def sm3tt(kind, d, n, m, i):
    d0, d1, d2, d3 = words(d)
    wj = words(m)[i]
    if kind[0] == '1':
        ss2 = words(n)[3] ^ rol32(d3, 12)
        t = (d1 ^ d2 ^ d3) if kind[1] == 'a' else majority(d3, d2, d1)
        tt1 = (t + d0 + ss2 + wj) & M32
        return pack([d1, rol32(d2, 9), d3, tt1])
    t = (d1 ^ d2 ^ d3) if kind[1] == 'a' else choose(d3, d2, d1)
    tt2 = (t + d0 + words(n)[3] + wj) & M32
    return pack([d1, rol32(d2, 19), d3, p0(tt2)])


def sm3partw1(d, n, m):
    dw, nw, mw = words(d), words(n), words(m)
    r = []
    for i in range(3):
        r.append(p1(dw[i] ^ nw[i] ^ rol32(mw[i + 1], 15)))
    r.append(p1(dw[3] ^ nw[3] ^ rol32(r[0], 15)))
    return pack(r)


def sm3partw2(d, n, m):
    dw, nw, mw = words(d), words(n), words(m)
    tmp = [nw[i] ^ rol32(mw[i], 7) for i in range(4)]
    r = [dw[i] ^ tmp[i] for i in range(3)]
    r.append(dw[3] ^ tmp[3] ^ p1(rol32(tmp[0], 15)))
    return pack(r)


def sm3_compress_test():
    block = b'abc' + b'\x80' + bytes(52) + (24).to_bytes(8, 'big')
    iv = [0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600, 0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e]
    w = [int.from_bytes(block[4 * i:4 * i + 4], 'big') for i in range(16)]
    # the message expansion with sm3partw1 / sm3partw2
    for j in range(16, 68, 4):
        t = sm3partw1(pack(w[j - 16:j - 12]), pack(w[j - 9:j - 5]), pack(w[j - 4:j]))
        n = sm3partw2(t, pack(w[j - 6:j - 2]), pack(w[j - 13:j - 9]))
        w += words(n)
    # the reference expansion
    ref = list(w[:16])
    for j in range(16, 68):
        ref.append(p1(ref[j - 16] ^ ref[j - 9] ^ rol32(ref[j - 3], 15)) ^ rol32(ref[j - 13], 7) ^ ref[j - 6])
    assert w == ref, 'sm3 expansion'
    wp = [w[j] ^ w[j + 4] for j in range(64)]
    # ABCD: word 3 is A; EFGH: word 3 is E
    abcd = pack([iv[3], iv[2], iv[1], iv[0]])
    efgh = pack([iv[7], iv[6], iv[5], iv[4]])
    for j in range(64):
        tj = 0x79cc4519 if j < 16 else 0x7a879d8a
        a = pack([0, 0, 0, rol32(tj, j % 32)])
        ss1 = sm3ss1(abcd, efgh, a)
        k = 'a' if j < 16 else 'b'
        wpv = pack(wp[j - j % 4:j - j % 4 + 4])
        wv = pack(w[j - j % 4:j - j % 4 + 4])
        new_abcd = sm3tt('1' + k, abcd, ss1, wpv, j % 4)
        new_efgh = sm3tt('2' + k, efgh, ss1, wv, j % 4)
        abcd, efgh = new_abcd, new_efgh
    final = words(abcd)[::-1] + words(efgh)[::-1]
    out = [a ^ b for a, b in zip(final, iv)]
    digest = b''.join(x.to_bytes(4, 'big') for x in out).hex()
    assert digest == '66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0', 'sm3 model'


sm3_compress_test()


# ---------------------------------------------------------------------------------------------------
# SM4
# ---------------------------------------------------------------------------------------------------
def load_sm4_sbox():
    text = open(LIFTER).read()
    body = text[text.index('gSm4Sbox[256]'):]
    body = body[:body.index('};')]
    values = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', body)]
    assert len(values) == 256
    return values


SM4_SBOX = load_sm4_sbox()


def sm4_t(x, key):
    b = [SM4_SBOX[(x >> (8 * i)) & 0xFF] for i in range(4)]
    s = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)
    if key:
        return s ^ rol32(s, 13) ^ rol32(s, 23)
    return s ^ rol32(s, 2) ^ rol32(s, 10) ^ rol32(s, 18) ^ rol32(s, 24)


def sm4e(d, n):
    x = words(d)
    rk = words(n)
    for i in range(4):
        nxt = x[0] ^ sm4_t(x[1] ^ x[2] ^ x[3] ^ rk[i], False)
        x = x[1:] + [nxt]
    return pack(x)


def sm4ekey(n, m):
    x = words(n)
    ck = words(m)
    out = []
    for i in range(4):
        nxt = x[0] ^ sm4_t(x[1] ^ x[2] ^ x[3] ^ ck[i], True)
        out.append(nxt)
        x = x[1:] + [nxt]
    return pack(out)


def sm4_test():
    key = bytes.fromhex('0123456789abcdeffedcba9876543210')
    plain = key
    FK = [0xa3b1bac6, 0x56aa3350, 0x677d9197, 0xb27022dc]
    mk = [int.from_bytes(key[4 * i:4 * i + 4], 'big') for i in range(4)]
    k = pack([a ^ b for a, b in zip(mk, FK)])
    rks = []
    for g in range(8):
        ck = pack([sum((((4 * (4 * g + j) + b) * 7) & 0xFF) << (24 - 8 * b) for b in range(4)) for j in range(4)])
        k = sm4ekey(k, ck)
        rks.append(k)
    state = pack([int.from_bytes(plain[4 * i:4 * i + 4], 'big') for i in range(4)])
    for g in range(8):
        state = sm4e(state, rks[g])
    out = words(state)[::-1]
    cipher = b''.join(x.to_bytes(4, 'big') for x in out)
    reference = subprocess.run(['openssl', 'enc', '-sm4-ecb', '-K', key.hex(), '-nopad'], input=plain,
                               capture_output=True).stdout
    assert cipher == reference, 'sm4 model %s %s' % (cipher.hex(), reference.hex())
    assert cipher.hex() == '681edf34d206965e86b3e94f536e4246'


sm4_test()


# ---------------------------------------------------------------------------------------------------
# Polynomial multiply
# ---------------------------------------------------------------------------------------------------
def clmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        b >>= 1
    return r


# ---------------------------------------------------------------------------------------------------
# The rows
# ---------------------------------------------------------------------------------------------------
rows = Rows()
rng = random.Random(0xC0FFEE)


def r128():
    return rng.getrandbits(128)


STATES = [0, M128, 0x0123456789abcdef0011223344556677, 1 << 127]


def draws(count):
    return STATES + [r128() for _ in range(count)]


for d in draws(4):
    n = r128()
    rows.add('aese v0.16b, v1.16b', {'v0': d, 'v1': n}, {'v0': aese(d, n)})
    rows.add('aesd v0.16b, v1.16b', {'v0': d, 'v1': n}, {'v0': aesd(d, n)})
for n in draws(4):
    rows.add('aesmc v0.16b, v1.16b', {'v1': n}, {'v0': aesmc(n)})
    rows.add('aesimc v0.16b, v1.16b', {'v1': n}, {'v0': aesimc(n)})
rows.add('aese v3.16b, v3.16b', {'v3': 0x00112233445566778899aabbccddeeff}, {'v3': aese(0x00112233445566778899aabbccddeeff, 0x00112233445566778899aabbccddeeff)})
# FIPS-197 C.1, the first round: state ^ key, SubBytes, ShiftRows
rows.add('aese v0.16b, v1.16b', {'v0': from_bytes(list(_plain)), 'v1': from_bytes(list(_key))},
         {'v0': int.from_bytes(bytes.fromhex('6353e08c0960e104cd70b751bacad0e7'), 'little')})
rows.add('aesmc v0.16b, v1.16b', {'v1': int.from_bytes(bytes.fromhex('6353e08c0960e104cd70b751bacad0e7'), 'little')},
         {'v0': int.from_bytes(bytes.fromhex('5f72641557f5bc92f7be3b291db9f91a'), 'little')})

for _ in range(6):
    d, n, m = r128(), r128(), r128()
    e = rng.getrandbits(32)
    rows.add('sha1c q0, s1, v2.4s', {'v0': d, 'v1': e, 'v2': m}, {'v0': sha1_hash('c', d, e, m)})
    rows.add('sha1m q0, s1, v2.4s', {'v0': d, 'v1': e, 'v2': m}, {'v0': sha1_hash('m', d, e, m)})
    rows.add('sha1p q0, s1, v2.4s', {'v0': d, 'v1': e, 'v2': m}, {'v0': sha1_hash('p', d, e, m)})
    rows.add('sha1h s0, s1', {'v1': n}, {'v0': sha1h(n)})
    rows.add('sha1su0 v0.4s, v1.4s, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha1su0(d, n, m)})
    rows.add('sha1su1 v0.4s, v1.4s', {'v0': d, 'v1': n}, {'v0': sha1su1(d, n)})
    rows.add('sha256h q0, q1, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha256h(d, n, m)})
    rows.add('sha256h2 q0, q1, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha256h2(d, n, m)})
    rows.add('sha256su0 v0.4s, v1.4s', {'v0': d, 'v1': n}, {'v0': sha256su0(d, n)})
    rows.add('sha256su1 v0.4s, v1.4s, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha256su1(d, n, m)})
    rows.add('sha512h q0, q1, v2.2d', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha512h(d, n, m)})
    rows.add('sha512h2 q0, q1, v2.2d', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha512h2(d, n, m)})
    rows.add('sha512su0 v0.2d, v1.2d', {'v0': d, 'v1': n}, {'v0': sha512su0(d, n)})
    rows.add('sha512su1 v0.2d, v1.2d, v2.2d', {'v0': d, 'v1': n, 'v2': m}, {'v0': sha512su1(d, n, m)})
    a = r128()
    rows.add('eor3 v0.16b, v1.16b, v2.16b, v3.16b', {'v1': d, 'v2': n, 'v3': m}, {'v0': d ^ n ^ m})
    rows.add('bcax v0.16b, v1.16b, v2.16b, v3.16b', {'v1': d, 'v2': n, 'v3': m}, {'v0': d ^ (n & ~m & M128)})
    rows.add('rax1 v0.2d, v1.2d, v2.2d', {'v1': d, 'v2': n},
             {'v0': pack([lane(d, i, 64) ^ rol64(lane(n, i, 64), 1) for i in range(2)], 64)})
    for amount in (0, 1, 17, 63):
        rows.add('xar v0.2d, v1.2d, v2.2d, #%d' % amount, {'v1': d, 'v2': n},
                 {'v0': pack([ror64(lane(d, i, 64) ^ lane(n, i, 64), amount) for i in range(2)], 64)})
    rows.add('sm3ss1 v0.4s, v1.4s, v2.4s, v3.4s', {'v1': d, 'v2': n, 'v3': m}, {'v0': sm3ss1(d, n, m)})
    for kind in ('1a', '1b', '2a', '2b'):
        for i in (0, 3):
            rows.add('sm3tt%s v0.4s, v1.4s, v2.s[%d]' % (kind, i), {'v0': d, 'v1': n, 'v2': m},
                     {'v0': sm3tt(kind, d, n, m, i)})
    rows.add('sm3partw1 v0.4s, v1.4s, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sm3partw1(d, n, m)})
    rows.add('sm3partw2 v0.4s, v1.4s, v2.4s', {'v0': d, 'v1': n, 'v2': m}, {'v0': sm3partw2(d, n, m)})
    rows.add('sm4e v0.4s, v1.4s', {'v0': d, 'v1': n}, {'v0': sm4e(d, n)})
    rows.add('sm4ekey v0.4s, v1.4s, v2.4s', {'v1': n, 'v2': m}, {'v0': sm4ekey(n, m)})
    # pmull
    rows.add('pmull v0.8h, v1.8b, v2.8b', {'v1': d, 'v2': n},
             {'v0': pack([clmul(lane(d, i, 8), lane(n, i, 8)) for i in range(8)], 16)})
    rows.add('pmull2 v0.8h, v1.16b, v2.16b', {'v1': d, 'v2': n},
             {'v0': pack([clmul(lane(d, 8 + i, 8), lane(n, 8 + i, 8)) for i in range(8)], 16)})
    rows.add('pmull v0.1q, v1.1d, v2.1d', {'v1': d, 'v2': n}, {'v0': clmul(d & M64, n & M64)})
    rows.add('pmull2 v0.1q, v1.2d, v2.2d', {'v1': d, 'v2': n}, {'v0': clmul(d >> 64, n >> 64)})

rows.write(OUTPUT, 'ARM64_SIMD_ROWS_CRYPTO', 'gen_simd_semantic_rows_crypto.py')
